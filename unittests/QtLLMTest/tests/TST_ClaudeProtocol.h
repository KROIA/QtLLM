#pragma once

#include "UnitTest.h"
#include "QtLLM.h"
#include <QJsonObject>
#include <QJsonArray>


class TST_ClaudeProtocol : public UnitTest::Test
{
    TEST_CLASS(TST_ClaudeProtocol)
public:
    TST_ClaudeProtocol()
        : Test("TST_ClaudeProtocol")
    {
        ADD_TEST(TST_ClaudeProtocol::testClientBuildsTool);
        ADD_TEST(TST_ClaudeProtocol::testToolRegistration);
        ADD_TEST(TST_ClaudeProtocol::testClearConversation);
        ADD_TEST(TST_ClaudeProtocol::testToolApiObjectStructure);
    }

private:

    // Tests
    TEST_FUNCTION(testClientBuildsTool)
    {
        TEST_START;

        QtLLM::Client client("fake_key");
        TEST_ASSERT(client.conversationHistory().isEmpty());
    }


    TEST_FUNCTION(testToolRegistration)
    {
        TEST_START;

        QtLLM::Client client("fake_key");

        QtLLM::Tool tool;
        tool.setName("myTool").setDescription("A test tool");
        client.registerTool(tool, [](const QJsonObject&) { return QJsonObject{}; });

        TEST_ASSERT(client.conversationHistory().isEmpty());

        client.unregisterTool("myTool");

        TEST_ASSERT(client.conversationHistory().isEmpty());
    }


    TEST_FUNCTION(testClearConversation)
    {
        TEST_START;

        QtLLM::Client client("fake_key");
        client.clearConversation();

        TEST_ASSERT(client.conversationHistory().isEmpty());
    }


    TEST_FUNCTION(testToolApiObjectStructure)
    {
        TEST_START;

        QtLLM::Tool tool;
        tool.setName("structuredTool")
            .setDescription("Tool with two params")
            .addParameter("requiredParam", "string", "A required string", true)
            .addParameter("optionalParam", "integer", "An optional int", false);

        QJsonObject obj = tool.toApiObject();

        TEST_ASSERT(obj.contains("name"));
        TEST_ASSERT(obj.contains("description"));
        TEST_ASSERT(obj.contains("input_schema"));

        QJsonObject inputSchema = obj["input_schema"].toObject();
        TEST_COMPARE(inputSchema["type"].toString(), QString("object"));

        QJsonObject properties = inputSchema["properties"].toObject();
        TEST_ASSERT(properties.contains("requiredParam"));
        TEST_ASSERT(properties.contains("optionalParam"));

        QJsonArray required = inputSchema["required"].toArray();
        TEST_COMPARE(required.size(), 1);
        TEST_COMPARE(required[0].toString(), QString("requiredParam"));
    }

};

TEST_INSTANTIATE(TST_ClaudeProtocol);
