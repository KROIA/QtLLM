#pragma once

#include "UnitTest.h"
#include "QtLLM.h"
#include <QJsonObject>
#include <QJsonArray>


class TST_Provider : public UnitTest::Test
{
    TEST_CLASS(TST_Provider)
public:
    TST_Provider()
        : Test("TST_Provider")
    {
        ADD_TEST(TST_Provider::testProviderEnumValues);
        ADD_TEST(TST_Provider::testOllamaClientConstruction);
        ADD_TEST(TST_Provider::testToolOpenAiSchemaFormat);
        ADD_TEST(TST_Provider::testOllamaToolRegistration);
        ADD_TEST(TST_Provider::testClearConversationOllama);
        ADD_TEST(TST_Provider::testClaudeSchemaFormat);
    }

private:

    // Tests
    TEST_FUNCTION(testProviderEnumValues)
    {
        TEST_START;

        QtLLM::Client claudeClient(QtLLM::Provider::Claude, "http://localhost:11434/api/chat", "");
        QtLLM::Client ollamaClient(QtLLM::Provider::Ollama, "http://localhost:11434/api/chat", "");

        TEST_ASSERT(claudeClient.conversationHistory().isEmpty());
        TEST_ASSERT(ollamaClient.conversationHistory().isEmpty());
    }


    TEST_FUNCTION(testOllamaClientConstruction)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://localhost:11434/api/chat");

        TEST_ASSERT(client.conversationHistory().isEmpty());
    }


    TEST_FUNCTION(testToolOpenAiSchemaFormat)
    {
        TEST_START;

        QtLLM::Tool tool;
        tool.setName("weather").setDescription("Gets the current weather");
        tool.addParameter("location", "string", "The city name", true);

        QJsonObject obj = tool.toOpenAiApiObject();

        TEST_COMPARE(obj["type"].toString(), QString("function"));
        TEST_ASSERT(obj.contains("function"));

        QJsonObject functionObj = obj["function"].toObject();
        TEST_ASSERT(functionObj.contains("name"));
        TEST_ASSERT(functionObj.contains("description"));
        TEST_ASSERT(functionObj.contains("parameters"));

        QJsonObject parameters = functionObj["parameters"].toObject();
        TEST_COMPARE(parameters["type"].toString(), QString("object"));
        TEST_ASSERT(parameters.contains("properties"));
    }


    TEST_FUNCTION(testOllamaToolRegistration)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://localhost:11434/api/chat");

        QtLLM::Tool tool;
        tool.setName("calc").setDescription("Performs calculations");
        client.registerTool(tool, [](const QJsonObject&) { return QJsonObject{}; });

        TEST_ASSERT(client.conversationHistory().isEmpty());

        client.unregisterTool("calc");
    }


    TEST_FUNCTION(testClearConversationOllama)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://localhost:11434/api/chat");
        client.clearConversation();

        TEST_ASSERT(client.conversationHistory().isEmpty());
    }


    TEST_FUNCTION(testClaudeSchemaFormat)
    {
        TEST_START;

        QtLLM::Tool tool;
        tool.setName("weather").setDescription("Gets the current weather");
        tool.addParameter("location", "string", "The city name", true);

        QJsonObject obj = tool.toApiObject();

        TEST_ASSERT(obj.contains("name"));
        TEST_ASSERT(obj.contains("description"));
        TEST_ASSERT(obj.contains("input_schema"));
        TEST_ASSERT(!obj.contains("parameters"));

        QJsonObject inputSchema = obj["input_schema"].toObject();
        TEST_COMPARE(inputSchema["type"].toString(), QString("object"));
    }

};

TEST_INSTANTIATE(TST_Provider);
