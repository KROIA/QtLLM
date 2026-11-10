#pragma once

#include "UnitTest.h"
#include "QtLLM.h"
#include "FakeApi.h"
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>
#include <functional>

// A minimal OpenID Connect provider on a loopback port: discovery document,
// token endpoint (authorization_code with PKCE check, refresh_token) and
// userinfo. The "browser" is played by the test: it reads the authorization
// URL and calls the redirect URI itself.
struct FakeIdp
{
    QString clientId = QStringLiteral("test-client");
    QString code     = QStringLiteral("code-123");
    QString challenge;              // code_challenge from the authorization URL
    QString nonce;
    QString redirectUri;
    QList<QUrlQuery> tokenRequests;
    bool    rejectRefresh = false;
    int     accessCounter = 0;
    int     expiresIn = 3600;
    QHash<QTcpSocket*, QByteArray> buffers;
    QTcpServer server;              // last: destroyed first

    FakeIdp()
    {
        server.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&server, &QTcpServer::newConnection, [this]() {
            while (QTcpSocket* socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, [this, socket]() { onData(socket); });
                QObject::connect(socket, &QTcpSocket::disconnected, [this, socket]() {
                    buffers.remove(socket);
                    socket->deleteLater();
                });
            }
        });
    }

    QString issuer() const { return QStringLiteral("http://127.0.0.1:%1/oauth").arg(server.serverPort()); }

    static QByteArray b64(const QByteArray& data)
    {
        return data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    }

    QByteArray idToken() const
    {
        const QJsonObject claims{{"sub", "u1"}, {"nonce", nonce},
                                 {"preferred_username", "jdoe@example.com"},
                                 {"given_name", "Jane"}, {"family_name", "Doe"},
                                 {"groups", QJsonArray{"llm-users", "staff"}}};
        return b64("{\"alg\":\"none\"}") + '.' + b64(QJsonDocument(claims).toJson(QJsonDocument::Compact)) + ".sig";
    }

    QByteArray tokenAnswer(const QUrlQuery& form, int* status)
    {
        *status = 200;
        const QString grant = form.queryItemValue("grant_type", QUrl::FullyDecoded);
        if (form.queryItemValue("client_id", QUrl::FullyDecoded) != clientId) {
            *status = 401;
            return R"({"error":"invalid_client"})";
        }
        if (grant == "authorization_code") {
            const QByteArray verifier = form.queryItemValue("code_verifier", QUrl::FullyDecoded).toLatin1();
            const QString expected = QString::fromLatin1(b64(QCryptographicHash::hash(verifier, QCryptographicHash::Sha256)));
            if (form.queryItemValue("code", QUrl::FullyDecoded) != code || expected != challenge
                || form.queryItemValue("redirect_uri", QUrl::FullyDecoded) != redirectUri) {
                *status = 400;
                return R"({"error":"invalid_grant","error_description":"bad code or verifier"})";
            }
        } else if (grant == "refresh_token") {
            if (rejectRefresh || form.queryItemValue("refresh_token", QUrl::FullyDecoded) != "refresh-1") {
                *status = 400;
                return R"({"error":"invalid_grant","error_description":"refresh token expired"})";
            }
        } else {
            *status = 400;
            return R"({"error":"unsupported_grant_type"})";
        }
        QJsonObject answer{{"access_token", QStringLiteral("access-%1").arg(++accessCounter)},
                           {"token_type", "Bearer"}, {"expires_in", expiresIn}};
        if (grant == "authorization_code") {
            answer["refresh_token"] = "refresh-1";
            answer["id_token"] = QString::fromLatin1(idToken());
        }
        return QJsonDocument(answer).toJson(QJsonDocument::Compact);
    }

    void onData(QTcpSocket* socket)
    {
        QByteArray& buffer = buffers[socket];
        buffer += socket->readAll();
        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;
        const QByteArray head = buffer.left(headerEnd);
        int length = 0;
        const int pos = head.toLower().indexOf("content-length:");
        if (pos >= 0)
            length = head.mid(pos + 15, head.indexOf('\r', pos) - pos - 15).trimmed().toInt();
        if (buffer.size() < headerEnd + 4 + length)
            return;
        const QList<QByteArray> requestLine = head.left(head.indexOf('\r')).split(' ');
        const QByteArray body = buffer.mid(headerEnd + 4, length);
        buffer.clear();

        const QByteArray path = requestLine.value(1);
        int status = 200;
        QByteArray answer;
        if (path == "/oauth/.well-known/openid-configuration") {
            const QString base = QStringLiteral("http://127.0.0.1:%1/oauth").arg(server.serverPort());
            answer = QJsonDocument(QJsonObject{{"issuer", base},
                                               {"authorization_endpoint", base + "/authorize"},
                                               {"token_endpoint", base + "/token"},
                                               {"userinfo_endpoint", base + "/userinfo"}})
                         .toJson(QJsonDocument::Compact);
        } else if (path == "/oauth/token") {
            const QUrlQuery form(QString::fromLatin1(body));
            tokenRequests.append(form);
            answer = tokenAnswer(form, &status);
        } else if (path == "/oauth/userinfo") {
            if (head.contains("Authorization: Bearer access-"))
                answer = R"({"email":"jane.doe@example.com","name":"Jane Doe"})";
            else
                status = 401;
        } else {
            status = 404;
        }
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " X\r\nContent-Type: application/json\r\n"
                      "Connection: close\r\nContent-Length: " + QByteArray::number(answer.size()) + "\r\n\r\n" + answer);
        socket->disconnectFromHost();
    }
};

class TST_OAuth2Session : public UnitTest::Test
{
    TEST_CLASS(TST_OAuth2Session)
public:
    TST_OAuth2Session()
        : Test("TST_OAuth2Session")
    {
        ADD_TEST(TST_OAuth2Session::testSignIn);
        ADD_TEST(TST_OAuth2Session::testClientSendsBearerToken);
        ADD_TEST(TST_OAuth2Session::testNotSignedInFailsRequest);
        ADD_TEST(TST_OAuth2Session::testProviderErrorFailsSignIn);
        ADD_TEST(TST_OAuth2Session::testForeignStateIsIgnored);
        ADD_TEST(TST_OAuth2Session::testRefresh);
        ADD_TEST(TST_OAuth2Session::testRejectedRefreshRequiresSignIn);
        ADD_TEST(TST_OAuth2Session::testStoredTokenSignsInWithoutBrowser);
        ADD_TEST(TST_OAuth2Session::testExpiredStoredTokenIsRefreshed);
        ADD_TEST(TST_OAuth2Session::testRejectedStoredTokenFallsBackToBrowser);
        ADD_TEST(TST_OAuth2Session::testStoreOfOtherClientIsIgnored);
        ADD_TEST(TST_OAuth2Session::testSignOutDeletesStore);
    }

private:
    static bool spinUntil(const std::function<bool()>& condition, int timeoutMs = 5000)
    {
        QEventLoop loop;
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, [&]() {
            if (condition())
                loop.quit();
        });
        poll.start(10);
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
        if (!condition())
            loop.exec();
        return condition();
    }

    static int freePort()
    {
        QTcpServer probe;
        probe.listen(QHostAddress::LocalHost, 0);
        return probe.serverPort();
    }

    static QtLLM::OAuth2Config configFor(const FakeIdp& idp)
    {
        QtLLM::OAuth2Config config;
        config.issuer      = QUrl(idp.issuer());
        config.clientId    = idp.clientId;
        config.scopes      = QStringList{"openid", "groups"};
        config.redirectUri = QUrl(QStringLiteral("http://127.0.0.1:%1/auth/callback").arg(freePort()));
        return config;
    }

    // Plays the browser: takes the parameters from the authorization URL and
    // calls the redirect URI with `callbackQuery(state)`.
    static void installBrowser(QtLLM::OAuth2Session& session, FakeIdp& idp, QNetworkAccessManager& nam,
                               std::function<QString(const QString& state)> callbackQuery)
    {
        session.setBrowserOpener([&idp, &nam, callbackQuery](const QUrl& url) {
            const QUrlQuery query(url);
            idp.challenge   = query.queryItemValue("code_challenge", QUrl::FullyDecoded);
            idp.nonce       = query.queryItemValue("nonce", QUrl::FullyDecoded);
            idp.redirectUri = query.queryItemValue("redirect_uri", QUrl::FullyDecoded);
            QUrl redirect(idp.redirectUri);
            redirect.setQuery(callbackQuery(query.queryItemValue("state", QUrl::FullyDecoded)));
            QTimer::singleShot(0, [&nam, redirect]() {
                QNetworkReply* reply = nam.get(QNetworkRequest(redirect));
                QObject::connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
            });
        });
    }

    static QString okCallback(const FakeIdp& idp, const QString& state)
    {
        return QStringLiteral("code=%1&state=%2").arg(idp.code, QString::fromLatin1(QUrl::toPercentEncoding(state)));
    }

    // Signs in against `idp`; true when signedIn() arrived.
    static bool signIn(QtLLM::OAuth2Session& session, FakeIdp& idp, QNetworkAccessManager& nam)
    {
        installBrowser(session, idp, nam, [&idp](const QString& state) { return okCallback(idp, state); });
        bool done = false;
        QObject::connect(&session, &QtLLM::OAuth2Session::signedIn, [&]() { done = true; });
        session.signIn();
        return spinUntil([&]() { return done; });
    }

    // Signs in with a store at `path`; counts how often the browser opened.
    static bool signInWithStore(QtLLM::OAuth2Session& session, FakeIdp& idp, QNetworkAccessManager& nam,
                                const QString& path, int* browserOpens)
    {
        session.setTokenStorePath(path);
        installBrowser(session, idp, nam, [&idp, browserOpens](const QString& state) {
            ++*browserOpens;
            return okCallback(idp, state);
        });
        bool done = false;
        QObject* guard = new QObject;
        QObject::connect(&session, &QtLLM::OAuth2Session::signedIn, guard, [&]() { done = true; });
        session.signIn();
        const bool ok = spinUntil([&]() { return done; });
        delete guard;
        return ok;
    }

    TEST_FUNCTION(testStoredTokenSignsInWithoutBrowser)
    {
        TEST_START;
        FakeIdp idp;
        QNetworkAccessManager nam;
        QTemporaryDir dir;
        const QString path = dir.filePath("sub/session.dat");
        int opens = 0;
        {
            QtLLM::OAuth2Session first(configFor(idp));
            TEST_ASSERT(signInWithStore(first, idp, nam, path, &opens));
        }
        TEST_ASSERT_EQUAL(opens, 1);
        TEST_ASSERT(QFile::exists(path));
        QFile file(path);
        file.open(QIODevice::ReadOnly);
        const QByteArray content = file.readAll();
#ifdef Q_OS_WIN
        TEST_ASSERT_M(!content.contains("refresh-1"), "token store must be encrypted");
#endif

        QtLLM::OAuth2Session second(configFor(idp));
        TEST_ASSERT(signInWithStore(second, idp, nam, path, &opens));
        TEST_ASSERT_EQUAL(opens, 1);                       // no browser
        TEST_ASSERT_EQUAL(idp.tokenRequests.size(), 1);   // no token request either
        TEST_ASSERT_EQUAL(second.accessToken(), QString("access-1"));
        TEST_ASSERT_EQUAL(second.email(), QString("jane.doe@example.com"));
    }

    TEST_FUNCTION(testExpiredStoredTokenIsRefreshed)
    {
        TEST_START;
        FakeIdp idp;
        idp.expiresIn = 1;
        QNetworkAccessManager nam;
        QTemporaryDir dir;
        const QString path = dir.filePath("session.dat");
        int opens = 0;
        {
            QtLLM::OAuth2Session first(configFor(idp));
            TEST_ASSERT(signInWithStore(first, idp, nam, path, &opens));
        }
        QThread::msleep(1200);
        QtLLM::OAuth2Session second(configFor(idp));
        TEST_ASSERT(signInWithStore(second, idp, nam, path, &opens));
        TEST_ASSERT_EQUAL(opens, 1);
        TEST_ASSERT_EQUAL(idp.tokenRequests.last().queryItemValue("grant_type"), QString("refresh_token"));
        TEST_ASSERT_EQUAL(second.accessToken(), QString("access-2"));
        TEST_ASSERT_EQUAL(second.displayName(), QString("Jane Doe"));
    }

    TEST_FUNCTION(testRejectedStoredTokenFallsBackToBrowser)
    {
        TEST_START;
        FakeIdp idp;
        idp.expiresIn = 1;
        QNetworkAccessManager nam;
        QTemporaryDir dir;
        const QString path = dir.filePath("session.dat");
        int opens = 0;
        {
            QtLLM::OAuth2Session first(configFor(idp));
            TEST_ASSERT(signInWithStore(first, idp, nam, path, &opens));
        }
        QThread::msleep(1200);
        idp.rejectRefresh = true;
        QtLLM::OAuth2Session second(configFor(idp));
        TEST_ASSERT(signInWithStore(second, idp, nam, path, &opens));
        TEST_ASSERT_EQUAL(opens, 2);
        TEST_ASSERT_EQUAL(second.accessToken(), QString("access-2"));
    }

    TEST_FUNCTION(testStoreOfOtherClientIsIgnored)
    {
        TEST_START;
        FakeIdp idp;
        QNetworkAccessManager nam;
        QTemporaryDir dir;
        const QString path = dir.filePath("session.dat");
        int opens = 0;
        {
            QtLLM::OAuth2Session first(configFor(idp));
            TEST_ASSERT(signInWithStore(first, idp, nam, path, &opens));
        }
        QtLLM::OAuth2Config other = configFor(idp);
        other.clientId = "other-client";
        idp.clientId = "other-client";
        QtLLM::OAuth2Session second(other);
        TEST_ASSERT(signInWithStore(second, idp, nam, path, &opens));
        TEST_ASSERT_EQUAL(opens, 2);
    }

    TEST_FUNCTION(testSignOutDeletesStore)
    {
        TEST_START;
        FakeIdp idp;
        QNetworkAccessManager nam;
        QTemporaryDir dir;
        const QString path = dir.filePath("session.dat");
        int opens = 0;
        QtLLM::OAuth2Session session(configFor(idp));
        TEST_ASSERT(signInWithStore(session, idp, nam, path, &opens));
        TEST_ASSERT(QFile::exists(path));
        session.signOut();
        TEST_ASSERT(!QFile::exists(path));
    }

    TEST_FUNCTION(testSignIn)
    {
        TEST_START;
        FakeIdp idp;
        QNetworkAccessManager nam;
        QtLLM::OAuth2Session session(configFor(idp));
        QStringList failures;
        QObject::connect(&session, &QtLLM::OAuth2Session::signInFailed, [&](const QString& e) { failures << e; });

        TEST_ASSERT(signIn(session, idp, nam));
        TEST_ASSERT(failures.isEmpty());
        TEST_ASSERT(session.state() == QtLLM::OAuth2Session::State::SignedIn);
        TEST_ASSERT(session.isSignedIn());
        TEST_ASSERT_EQUAL(session.accessToken(), QString("access-1"));
        TEST_ASSERT(session.expiresAt().isValid());
        TEST_ASSERT(!idp.challenge.isEmpty());
        TEST_ASSERT(!idp.nonce.isEmpty());
        // ID token claims, completed by userinfo.
        TEST_ASSERT_EQUAL(session.displayName(), QString("Jane Doe"));
        TEST_ASSERT_EQUAL(session.email(), QString("jane.doe@example.com"));
        TEST_ASSERT_EQUAL(session.groups(), QStringList({"llm-users", "staff"}));
        TEST_ASSERT_EQUAL(idp.tokenRequests.size(), 1);
    }

    TEST_FUNCTION(testClientSendsBearerToken)
    {
        TEST_START;
        FakeIdp idp;
        FakeApi api;
        QNetworkAccessManager nam;
        QtLLM::OAuth2Session session(configFor(idp));
        TEST_ASSERT(signIn(session, idp, nam));

        // No API key: the token is the only credential.
        QtLLM::Client client(QtLLM::Provider::Claude, api.url(), QString());
        client.setStreamingEnabled(false);
        client.setAccessTokenProvider(session.accessTokenProvider());
        api.replies.enqueue(FakeApi::textReply("hello"));
        QString answer;
        QObject::connect(&client, &QtLLM::Client::responseReady, [&](const QString& t) { answer = t; });
        client.sendPrompt("hi");
        TEST_ASSERT(spinUntil([&]() { return !answer.isEmpty(); }));
        TEST_ASSERT_EQUAL(answer, QString("hello"));
        TEST_ASSERT_EQUAL(api.messageHeads.size(), 1);
        TEST_ASSERT(api.messageHeads[0].contains("authorization: bearer access-1"));
        TEST_ASSERT(!api.messageHeads[0].contains("x-api-key"));

        // Survives a provider switch.
        client.setProvider(QtLLM::Provider::Claude, api.url(), QString());
        client.setStreamingEnabled(false);
        api.replies.enqueue(FakeApi::textReply("again"));
        answer.clear();
        client.sendPrompt("hi");
        TEST_ASSERT(spinUntil([&]() { return !answer.isEmpty(); }));
        TEST_ASSERT(api.messageHeads.last().contains("authorization: bearer access-1"));
    }

    TEST_FUNCTION(testNotSignedInFailsRequest)
    {
        TEST_START;
        FakeApi api;
        QtLLM::Client client(QtLLM::Provider::Claude, api.url(), QString());
        client.setAccessTokenProvider([]() { return QString(); });
        QString error;
        QObject::connect(&client, &QtLLM::Client::errorOccurred, [&](const QString& e) { error = e; });
        client.sendPrompt("hi");
        TEST_ASSERT(spinUntil([&]() { return !error.isEmpty(); }));
        TEST_ASSERT(error.contains("Not signed in"));
        TEST_ASSERT_EQUAL(api.messageRequests, 0);
    }

    TEST_FUNCTION(testProviderErrorFailsSignIn)
    {
        TEST_START;
        FakeIdp idp;
        QNetworkAccessManager nam;
        QtLLM::OAuth2Session session(configFor(idp));
        installBrowser(session, idp, nam, [](const QString& state) {
            return QStringLiteral("error=access_denied&error_description=no%20access&state=")
                 + QString::fromLatin1(QUrl::toPercentEncoding(state));
        });
        QString failure;
        QObject::connect(&session, &QtLLM::OAuth2Session::signInFailed, [&](const QString& e) { failure = e; });
        session.signIn();
        TEST_ASSERT(spinUntil([&]() { return !failure.isEmpty(); }));
        TEST_ASSERT(failure.contains("access_denied"));
        TEST_ASSERT(failure.contains("no access"));
        TEST_ASSERT(session.state() == QtLLM::OAuth2Session::State::SignedOut);
        TEST_ASSERT(idp.tokenRequests.isEmpty());
    }

    TEST_FUNCTION(testForeignStateIsIgnored)
    {
        TEST_START;
        FakeIdp idp;
        QNetworkAccessManager nam;
        QtLLM::OAuth2Session session(configFor(idp));
        installBrowser(session, idp, nam, [&idp](const QString&) { return okCallback(idp, "forged"); });
        bool signedIn = false;
        QObject::connect(&session, &QtLLM::OAuth2Session::signedIn, [&]() { signedIn = true; });
        session.signIn();
        spinUntil([]() { return false; }, 500);
        TEST_ASSERT(!signedIn);
        TEST_ASSERT(idp.tokenRequests.isEmpty());
        TEST_ASSERT(session.state() == QtLLM::OAuth2Session::State::SigningIn);
        session.cancelSignIn();
        TEST_ASSERT(session.state() == QtLLM::OAuth2Session::State::SignedOut);
    }

    TEST_FUNCTION(testRefresh)
    {
        TEST_START;
        FakeIdp idp;
        QNetworkAccessManager nam;
        QtLLM::OAuth2Session session(configFor(idp));
        TEST_ASSERT(signIn(session, idp, nam));
        bool refreshed = false;
        QObject::connect(&session, &QtLLM::OAuth2Session::tokenRefreshed, [&]() { refreshed = true; });
        session.refresh();
        TEST_ASSERT(spinUntil([&]() { return refreshed; }));
        TEST_ASSERT_EQUAL(session.accessToken(), QString("access-2"));
        // The refresh answer carried no ID token: the claims stay.
        TEST_ASSERT_EQUAL(session.email(), QString("jane.doe@example.com"));
        TEST_ASSERT(session.state() == QtLLM::OAuth2Session::State::SignedIn);
    }

    TEST_FUNCTION(testRejectedRefreshRequiresSignIn)
    {
        TEST_START;
        FakeIdp idp;
        QNetworkAccessManager nam;
        QtLLM::OAuth2Session session(configFor(idp));
        TEST_ASSERT(signIn(session, idp, nam));
        idp.rejectRefresh = true;
        QString reason;
        QObject::connect(&session, &QtLLM::OAuth2Session::signInRequired, [&](const QString& r) { reason = r; });
        session.refresh();
        TEST_ASSERT(spinUntil([&]() { return !reason.isEmpty(); }));
        TEST_ASSERT(reason.contains("refresh token expired"));
        TEST_ASSERT(!session.isSignedIn());
        TEST_ASSERT(session.state() == QtLLM::OAuth2Session::State::SignedOut);
    }
};

TEST_INSTANTIATE(TST_OAuth2Session);
