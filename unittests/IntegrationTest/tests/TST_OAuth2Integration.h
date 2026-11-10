#pragma once

#include "UnitTest.h"
#include "QtLLM.h"
#include <QEventLoop>
#include <QTimer>
#include <iostream>

// Signs in at a real identity provider in the system browser, then - when an
// API URL is given - sends one prompt through the OAuth2 proxy with the
// user's token. Opt-in: skipped unless the variables below are set, so no
// provider URL or client ID has to live in the code.
//
//   QTLLM_OAUTH_ISSUER        issuer URL (discovery at <issuer>/.well-known/openid-configuration)
//   QTLLM_OAUTH_CLIENT_ID     client ID registered at the provider
//   QTLLM_OAUTH_REDIRECT_URI  registered loopback redirect, e.g. http://localhost:12000/auth/callback
//   QTLLM_OAUTH_SCOPES        optional, space separated (default "openid")
//   QTLLM_OAUTH_API_URL       optional: Anthropic Messages endpoint behind the proxy
//                             (".../v1/messages"); without it only the sign-in is tested
//   QTLLM_OAUTH_MODEL         optional model for that prompt
//   QTLLM_OAUTH_TOKEN_STORE   optional file to keep the tokens in between runs
class TST_OAuth2Integration : public UnitTest::Test
{
    TEST_CLASS(TST_OAuth2Integration)
public:
    TST_OAuth2Integration()
        : Test("TST_OAuth2Integration")
    {
        ADD_TEST(TST_OAuth2Integration::testSignInAndPrompt);
    }

private:
    static QString env(const char* name)
    {
        return QString::fromUtf8(qgetenv(name)).trimmed();
    }

    TEST_FUNCTION(testSignInAndPrompt)
    {
        TEST_START;

        QtLLM::OAuth2Config config;
        config.issuer      = QUrl(env("QTLLM_OAUTH_ISSUER"));
        config.clientId    = env("QTLLM_OAUTH_CLIENT_ID");
        config.redirectUri = QUrl(env("QTLLM_OAUTH_REDIRECT_URI"));
        const QString scopes = env("QTLLM_OAUTH_SCOPES");
        if (!scopes.isEmpty())
            config.scopes = scopes.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (config.issuer.isEmpty() || config.clientId.isEmpty() || config.redirectUri.isEmpty()) {
            std::cout << "  Skipping: QTLLM_OAUTH_ISSUER / QTLLM_OAUTH_CLIENT_ID / QTLLM_OAUTH_REDIRECT_URI not set\n";
            return;
        }

        QtLLM::OAuth2Session session(config);
        session.setTokenStorePath(env("QTLLM_OAUTH_TOKEN_STORE"));
        QEventLoop loop;
        QString failure;
        QObject::connect(&session, &QtLLM::OAuth2Session::signedIn, &loop, &QEventLoop::quit);
        QObject::connect(&session, &QtLLM::OAuth2Session::signInFailed, [&](const QString& e) {
            failure = e;
            loop.quit();
        });
        std::cout << "  Complete the sign-in in the browser...\n";
        session.signIn();
        loop.exec();
        TEST_ASSERT_M(failure.isEmpty(), failure.toStdString());
        TEST_ASSERT(session.isSignedIn());
        std::cout << "  Signed in as: " << session.displayName().toStdString()
                  << " <" << session.email().toStdString() << ">\n"
                  << "  Groups: " << session.groups().join(", ").toStdString() << "\n"
                  << "  Token expires: " << session.expiresAt().toLocalTime().toString().toStdString() << "\n";

        const QString apiUrl = env("QTLLM_OAUTH_API_URL");
        if (apiUrl.isEmpty()) {
            std::cout << "  QTLLM_OAUTH_API_URL not set: prompt through the proxy skipped\n";
            return;
        }
        QtLLM::Client client(QtLLM::Provider::Claude, apiUrl, QString());
        client.setAccessTokenProvider(session.accessTokenProvider());
        const QString model = env("QTLLM_OAUTH_MODEL");
        if (!model.isEmpty())
            client.setModel(model);

        QString answer, error;
        QEventLoop turn;
        QObject::connect(&client, &QtLLM::Client::responseReady, [&](const QString& t) { answer = t; turn.quit(); });
        QObject::connect(&client, &QtLLM::Client::errorOccurred, [&](const QString& e) { error = e; turn.quit(); });
        QTimer::singleShot(120000, &turn, &QEventLoop::quit);
        client.sendPrompt("Reply with the single word: pong");
        turn.exec();
        TEST_ASSERT_M(error.isEmpty(), error.toStdString());
        TEST_ASSERT_M(answer.contains("pong", Qt::CaseInsensitive), answer.toStdString());
        std::cout << "  Answer through the proxy: " << answer.toStdString() << "\n";
    }
};

TEST_INSTANTIATE(TST_OAuth2Integration);
