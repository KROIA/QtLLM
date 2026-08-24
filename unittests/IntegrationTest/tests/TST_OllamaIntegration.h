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
        client.setModel(models.first());

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
        std::cout << "  Model: " << models.first().toStdString() << "\n";
        std::cout << "  Response: " << receivedText.toStdString() << "\n";
    }

};

TEST_INSTANTIATE(TST_OllamaIntegration);
