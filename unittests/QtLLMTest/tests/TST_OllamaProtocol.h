#pragma once

#include "UnitTest.h"
#include "QtLLM.h"
#include <QJsonObject>
#include <QJsonArray>


// Regression coverage for the Ollama chat-URL / provider-construction fixes.
// No network calls here - constructing a Client only stores the URL and never
// posts anything, so these run instantly. The actual "/api/chat" path
// normalization can only be observed against a live server; see
// IntegrationTest/tests/TST_OllamaIntegration.h for that.
class TST_OllamaProtocol : public UnitTest::Test
{
    TEST_CLASS(TST_OllamaProtocol)
public:
    TST_OllamaProtocol()
        : Test("TST_OllamaProtocol")
    {
        ADD_TEST(TST_OllamaProtocol::testConstructionWithBareBaseUrl);
        ADD_TEST(TST_OllamaProtocol::testConstructionWithTrailingSlashUrl);
        ADD_TEST(TST_OllamaProtocol::testConstructionWithExplicitApiChatUrl);
        ADD_TEST(TST_OllamaProtocol::testSettersDoNotTouchHistory);
        ADD_TEST(TST_OllamaProtocol::testToolEnableDisableSync);
        ADD_TEST(TST_OllamaProtocol::testProviderSwitchPreservesTools);
        ADD_TEST(TST_OllamaProtocol::testOpenAiSchemaMultipleParamsAndEnum);
        ADD_TEST(TST_OllamaProtocol::testRawSchemaToolMissingRequiredParam);
    }

private:

    // Tests

    // Bug #1 regression: sendRequest() must normalize m_url to end in
    // /api/chat itself - a bare base URL (the exact shape that used to 405)
    // must still construct cleanly.
    TEST_FUNCTION(testConstructionWithBareBaseUrl)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://localhost:11434", "");

        TEST_ASSERT(client.conversationHistory().isEmpty());
    }


    TEST_FUNCTION(testConstructionWithTrailingSlashUrl)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://localhost:11434/", "");

        TEST_ASSERT(client.conversationHistory().isEmpty());
    }


    TEST_FUNCTION(testConstructionWithExplicitApiChatUrl)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://localhost:11434/api/chat", "");

        TEST_ASSERT(client.conversationHistory().isEmpty());
    }


    TEST_FUNCTION(testSettersDoNotTouchHistory)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://localhost:11434", "");
        client.setModel("qwen2.5-coder:32b");
        client.setMaxTokens(1024);
        client.setSystemPrompt("You are a helpful assistant.");

        TEST_ASSERT(client.conversationHistory().isEmpty());
    }


    TEST_FUNCTION(testToolEnableDisableSync)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://localhost:11434", "");

        QtLLM::Tool toolA;
        toolA.setName("toolA").setDescription("First tool");
        client.registerTool(toolA, [](const QJsonObject&) { return QJsonObject{}; });

        QtLLM::Tool toolB;
        toolB.setName("toolB").setDescription("Second tool");
        client.registerTool(toolB, [](const QJsonObject&) { return QJsonObject{}; });

        TEST_COMPARE(client.toolNames().size(), 2);
        TEST_COMPARE(client.enabledToolNames().size(), 2);

        client.setToolEnabled("toolA", false);

        TEST_ASSERT(!client.isToolEnabled("toolA"));
        TEST_ASSERT(client.isToolEnabled("toolB"));
        TEST_COMPARE(client.toolNames().size(), 2);
        TEST_COMPARE(client.enabledToolNames().size(), 1);
        TEST_COMPARE(client.enabledToolNames().first(), QString("toolB"));
    }


    TEST_FUNCTION(testProviderSwitchPreservesTools)
    {
        TEST_START;

        QtLLM::Client client("fake_key");

        QtLLM::Tool tool;
        tool.setName("shared").setDescription("Survives a provider switch");
        client.registerTool(tool, [](const QJsonObject&) { return QJsonObject{}; });

        client.setProvider(QtLLM::Provider::Ollama, "http://localhost:11434", "");

        TEST_ASSERT(client.toolNames().contains("shared"));
        TEST_ASSERT(client.conversationHistory().isEmpty());

        client.setProvider(QtLLM::Provider::Claude, "https://api.anthropic.com/v1/messages", "fake_key");

        TEST_ASSERT(client.toolNames().contains("shared"));
    }


    // Extends TST_Provider::testToolOpenAiSchemaFormat with multiple
    // parameters, only one of which is required, plus an enum parameter -
    // the shape Ollama's tool-calling models actually receive.
    TEST_FUNCTION(testOpenAiSchemaMultipleParamsAndEnum)
    {
        TEST_START;

        QtLLM::Tool tool;
        tool.setName("book")
            .setDescription("Books a room")
            .addParameter("room", "string", "Room name", true)
            .addEnumParameter("size", {"small", "large"}, "Room size", false);

        QJsonObject obj = tool.toOpenAiApiObject();
        QJsonObject parameters = obj["function"].toObject()["parameters"].toObject();
        QJsonObject properties = parameters["properties"].toObject();

        TEST_ASSERT(properties.contains("room"));
        TEST_ASSERT(properties.contains("size"));

        QJsonArray required = parameters["required"].toArray();
        TEST_COMPARE(required.size(), 1);
        TEST_COMPARE(required[0].toString(), QString("room"));

        QJsonArray enumValues = properties["size"].toObject()["enum"].toArray();
        TEST_COMPARE(enumValues.size(), 2);
    }


    // Error path: a tool call missing a required parameter must be rejected
    // before the handler ever runs - this is the schema Client stores for
    // tools registered via the raw-schema overload (used by Ollama the same
    // as Claude).
    TEST_FUNCTION(testRawSchemaToolMissingRequiredParam)
    {
        TEST_START;

        QJsonObject paramSchema;
        paramSchema["type"] = "object";
        paramSchema["properties"] = QJsonObject{
            {"city", QJsonObject{{"type", "string"}}}
        };
        paramSchema["required"] = QJsonArray{"city"};

        QJsonObject result = QtLLM::Tool::validateAgainstSchema(paramSchema, QJsonObject{});

        TEST_COMPARE(result["status"].toString(), QString("error"));
        TEST_ASSERT(result.contains("expected"));
    }

};

TEST_INSTANTIATE(TST_OllamaProtocol);
