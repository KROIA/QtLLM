#pragma once

#include "UnitTest.h"
#include "QtLLM.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <iostream>


class TST_Integration : public UnitTest::Test
{
    TEST_CLASS(TST_Integration)
public:
    TST_Integration()
        : Test("TST_Integration")
    {
        ADD_TEST(TST_Integration::testEnvVarsPresent);
        ADD_TEST(TST_Integration::testSimpleTextResponse);
        ADD_TEST(TST_Integration::testToolUse);
    }

private:

    // Tests
    TEST_FUNCTION(testEnvVarsPresent)
    {
        TEST_START;

        QString apiKey  = QString::fromUtf8(qgetenv("ANTHROPIC_FOUNDRY_API_KEY"));
        QString baseUrl = QString::fromUtf8(qgetenv("ANTHROPIC_FOUNDRY_BASE_URL"));

        TEST_ASSERT_M(!apiKey.isEmpty(),  "ANTHROPIC_FOUNDRY_API_KEY env var must be set");
        TEST_ASSERT_M(!baseUrl.isEmpty(), "ANTHROPIC_FOUNDRY_BASE_URL env var must be set");
    }


    TEST_FUNCTION(testSimpleTextResponse)
    {
        TEST_START;

        QString apiKey  = QString::fromUtf8(qgetenv("ANTHROPIC_FOUNDRY_API_KEY"));
        QString baseUrl = QString::fromUtf8(qgetenv("ANTHROPIC_FOUNDRY_BASE_URL"));

        if (apiKey.isEmpty() || baseUrl.isEmpty())
        {
            std::cout << "  Skipping: env var not set\n";
            return;
        }

        // ANTHROPIC_FOUNDRY_BASE_URL is a base URL — append the messages endpoint path
        QString endpointUrl = baseUrl + "/v1/messages";

        QtLLM::Client client(apiKey, endpointUrl);
        client.setModel("claude-sonnet-4-5");
        client.setMaxTokens(512);
        client.setSystemPrompt("Reply with exactly one short sentence.");

        QEventLoop loop;
        QString receivedText;
        bool gotError = false;
        QString errorMsg;

        QObject::connect(&client, &QtLLM::Client::responseReady, [&](const QString& text) {
            receivedText = text;
            loop.quit();
        });
        QObject::connect(&client, &QtLLM::Client::errorOccurred, [&](const QString& msg) {
            gotError = true;
            errorMsg = msg;
            loop.quit();
        });

        // Timeout after 30 seconds to avoid hanging indefinitely
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);

        client.sendPrompt("Say hello.");
        loop.exec();

        TEST_ASSERT_M(!gotError, ("API error: " + errorMsg).toStdString().c_str());
        TEST_ASSERT_M(!receivedText.isEmpty(), "Expected non-empty response text");
        std::cout << "  Response: " << receivedText.toStdString() << "\n";
    }


    TEST_FUNCTION(testToolUse)
    {
        TEST_START;

        QString apiKey  = QString::fromUtf8(qgetenv("ANTHROPIC_FOUNDRY_API_KEY"));
        QString baseUrl = QString::fromUtf8(qgetenv("ANTHROPIC_FOUNDRY_BASE_URL"));

        if (apiKey.isEmpty() || baseUrl.isEmpty())
        {
            std::cout << "  Skipping: env var not set\n";
            return;
        }

        // ANTHROPIC_FOUNDRY_BASE_URL is a base URL — append the messages endpoint path
        QString endpointUrl = baseUrl + "/v1/messages";

        QtLLM::Client client(apiKey, endpointUrl);
        client.setModel("claude-sonnet-4-5");
        client.setMaxTokens(512);

        QJsonObject paramSchema;
        paramSchema["type"] = "object";
        paramSchema["properties"] = QJsonObject{
            {"a", QJsonObject{{"type", "number"}}},
            {"b", QJsonObject{{"type", "number"}}}
        };
        paramSchema["required"] = QJsonArray{"a", "b"};

        bool toolWasCalled = false;

        client.registerTool(
            "add",
            "Adds two numbers.",
            paramSchema,
            [](const QJsonObject& args) {
                return QJsonObject{{"result", args["a"].toDouble() + args["b"].toDouble()}};
            }
        );

        QObject::connect(&client, &QtLLM::Client::toolInvoked, [&](const QString&, const QJsonObject&) {
            toolWasCalled = true;
        });

        QEventLoop loop;
        QString receivedText;
        bool gotError = false;
        QString errorMsg;

        QObject::connect(&client, &QtLLM::Client::responseReady, [&](const QString& text) {
            receivedText = text;
            loop.quit();
        });
        QObject::connect(&client, &QtLLM::Client::errorOccurred, [&](const QString& msg) {
            gotError = true;
            errorMsg = msg;
            loop.quit();
        });

        // Timeout after 30 seconds to avoid hanging indefinitely
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);

        client.sendPrompt("What is 7 plus 8? Use the add tool.");
        loop.exec();

        TEST_ASSERT_M(!gotError, ("API error: " + errorMsg).toStdString().c_str());
        TEST_ASSERT_M(toolWasCalled, "Expected the add tool to be called");
        TEST_ASSERT_M(!receivedText.isEmpty(), "Expected non-empty final response");
        std::cout << "  Tool was invoked: " << (toolWasCalled ? "yes" : "no") << "\n";
        std::cout << "  Response: " << receivedText.toStdString() << "\n";
    }

};

TEST_INSTANTIATE(TST_Integration);
