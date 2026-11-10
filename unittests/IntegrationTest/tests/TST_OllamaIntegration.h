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


class TST_OllamaIntegration : public UnitTest::Test
{
    TEST_CLASS(TST_OllamaIntegration)
public:
    TST_OllamaIntegration()
        : Test("TST_OllamaIntegration")
    {
        ADD_TEST(TST_OllamaIntegration::testModelsFetchReturnsNonEmptyList);
        ADD_TEST(TST_OllamaIntegration::testBareBaseUrlChatRequest);
        ADD_TEST(TST_OllamaIntegration::testOpenAiCompatibleEndpoint);
    }

private:

    static const char* baseUrl() { return "http://localhost:11434"; }

    // Ollama is a local server, not a secret-gated env var - reachability
    // itself is the skip condition. Returns false (and prints the skip
    // message) if no server answers within a short timeout.
    static bool fetchModelsOrSkip(QStringList& models)
    {
        QtLLM::Client client(QtLLM::Provider::Ollama, baseUrl(), "");

        QEventLoop loop;
        bool gotError = false;

        QObject::connect(&client, &QtLLM::Client::modelsAvailable, [&](const QStringList& list) {
            models = list;
            loop.quit();
        });
        QObject::connect(&client, &QtLLM::Client::errorOccurred, [&](const QString&) {
            gotError = true;
            loop.quit();
        });

        // Local server - a reachability check should resolve almost instantly.
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);

        client.fetchAvailableModels();
        loop.exec();

        if (gotError || models.isEmpty()) {
            std::cout << "  Skipping: Ollama not reachable at localhost:11434\n";
            return false;
        }
        return true;
    }


    // Tests
    TEST_FUNCTION(testModelsFetchReturnsNonEmptyList)
    {
        TEST_START;

        QStringList models;
        if (!fetchModelsOrSkip(models))
            return;

        TEST_ASSERT_M(!models.isEmpty(), "Expected at least one model from the local Ollama server");
        std::cout << "  Models: " << models.join(", ").toStdString() << "\n";
    }


    // Regression test for bug #1: OllamaProtocol::sendRequest() must rewrite
    // a bare base URL (no /api/chat suffix) to the chat endpoint itself -
    // this exact Client construction used to 405 before the fix.
    TEST_FUNCTION(testBareBaseUrlChatRequest)
    {
        TEST_START;

        QStringList models;
        if (!fetchModelsOrSkip(models))
            return;

        QtLLM::Client client(QtLLM::Provider::Ollama, baseUrl(), "");
        // Not models.first(): that may be a 70 GB model that takes minutes to load.
        const QString model = QtLLM::Client::pickFallbackModel(QtLLM::Provider::Ollama, models);
        client.setModel(model);

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

        // Local generation can be slow on first load (model into VRAM) -
        // give it more headroom than the cloud-API integration test.
        QTimer::singleShot(120000, &loop, &QEventLoop::quit);

        client.sendPrompt("Reply with exactly one short word.");
        loop.exec();

        TEST_ASSERT_M(!gotError, ("Ollama error: " + errorMsg).toStdString().c_str());
        TEST_ASSERT_M(!receivedText.isEmpty(), "Expected non-empty response text");
        std::cout << "  Model: " << model.toStdString() << "\n";
        std::cout << "  Response: " << receivedText.toStdString() << "\n";
    }


    // Provider::OpenAI against a real server: Ollama also speaks the OpenAI
    // Chat Completions API under /v1. Streaming and a tool round.
    TEST_FUNCTION(testOpenAiCompatibleEndpoint)
    {
        TEST_START;

        QStringList models;
        if (!fetchModelsOrSkip(models))
            return;
        // Not whatever comes first: that may be a 70 GB model.
        const QString model = QtLLM::Client::pickFallbackModel(QtLLM::Provider::Ollama, models);

        QtLLM::Client client(QtLLM::Provider::OpenAI, QString(baseUrl()) + "/v1");
        client.setModel(model);
        client.setSystemPrompt("Use the tools you are given. Answer in one short sentence.");
        QtLLM::Tool tool;
        tool.setName("get_secret_code").setDescription("Returns the secret code of this application.");
        int calls = 0;
        client.registerTool(tool, [&](const QJsonObject&) {
            ++calls;
            return QtLLM::toolOk({{"code", "KIWI-42"}});
        });

        QEventLoop loop;
        QString text, error;
        int deltas = 0;
        QObject::connect(&client, &QtLLM::Client::responseDelta, [&](const QString&) { ++deltas; });
        QObject::connect(&client, &QtLLM::Client::responseReady, [&](const QString& t) { text = t; loop.quit(); });
        QObject::connect(&client, &QtLLM::Client::errorOccurred, [&](const QString& m) { error = m; loop.quit(); });
        // A 3B model now and then writes its tool call as text instead of
        // making it. That is the model, not the protocol: ask again.
        for (int attempt = 1; attempt <= 3 && calls == 0; ++attempt) {
            if (attempt > 1) {
                std::cout << "  Attempt " << attempt << " (the model answered: " << text.toStdString() << ")\n";
                client.clearConversation();
            }
            QTimer timeout;
            timeout.setSingleShot(true);
            QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            timeout.start(180000);
            client.sendPrompt("Call the get_secret_code tool and tell me the code it returns.");
            loop.exec();
            if (!error.isEmpty())
                break;
        }

        TEST_ASSERT_M(error.isEmpty(), ("error: " + error).toStdString().c_str());
        std::cout << "  Model: " << client.model().toStdString() << ", deltas: " << deltas
                  << ", tool calls: " << calls << "\n";
        std::cout << "  Response: " << text.toStdString() << "\n";
        TEST_COMPARE(client.model(), model);
        TEST_ASSERT(calls >= 1);
        TEST_ASSERT(text.contains("KIWI-42"));
        TEST_ASSERT(deltas > 0);                               // it streamed
        TEST_ASSERT(client.usageStats().inputTokens > 0);      // usage came with the stream
    }
};

TEST_INSTANTIATE(TST_OllamaIntegration);
