#pragma once

#include "UnitTest.h"
#include "QtLLM.h"
#include <QJsonObject>
#include <QJsonArray>


class TST_Client : public UnitTest::Test
{
    TEST_CLASS(TST_Client)
public:
    TST_Client()
        : Test("TST_Client")
    {
        ADD_TEST(TST_Client::testDefaultConstruction);
        ADD_TEST(TST_Client::testRegisterAndUnregister);
        ADD_TEST(TST_Client::testRegisterRawSchema);
        ADD_TEST(TST_Client::testClearPreservesTools);
        ADD_TEST(TST_Client::testMultipleToolRegistration);
    }

private:

    // Tests
    TEST_FUNCTION(testDefaultConstruction)
    {
        TEST_START;

        QtLLM::Client client("test_key");
        TEST_ASSERT(client.conversationHistory().isEmpty());
    }


    TEST_FUNCTION(testRegisterAndUnregister)
    {
        TEST_START;

        QtLLM::Client client("test_key");

        QtLLM::Tool tool;
        tool.setName("circle").setDescription("Handles circle geometry");
        client.registerTool(tool, [](const QJsonObject&) { return QJsonObject{}; });

        client.unregisterTool("circle");

        TEST_ASSERT(client.conversationHistory().isEmpty());
    }


    TEST_FUNCTION(testRegisterRawSchema)
    {
        TEST_START;

        QtLLM::Client client("test_key");

        QJsonObject paramSchema;
        paramSchema["type"] = "object";
        paramSchema["properties"] = QJsonObject{};

        client.registerTool(
            "add",
            "Adds two numbers",
            paramSchema,
            [](const QJsonObject&) { return QJsonObject{{"result", 42}}; }
        );

        TEST_ASSERT(client.conversationHistory().isEmpty());
    }


    TEST_FUNCTION(testClearPreservesTools)
    {
        TEST_START;

        QtLLM::Client client("test_key");

        QtLLM::Tool tool;
        tool.setName("persist").setDescription("Persists across clear");
        client.registerTool(tool, [](const QJsonObject&) { return QJsonObject{}; });

        client.clearConversation();

        TEST_ASSERT(client.conversationHistory().isEmpty());

        // Re-registering after clear should not crash
        QtLLM::Tool tool2;
        tool2.setName("persist2").setDescription("Another tool after clear");
        client.registerTool(tool2, [](const QJsonObject&) { return QJsonObject{}; });

        TEST_ASSERT(client.conversationHistory().isEmpty());
    }


    TEST_FUNCTION(testMultipleToolRegistration)
    {
        TEST_START;

        QtLLM::Client client("test_key");

        QtLLM::Tool toolA;
        toolA.setName("toolA").setDescription("First tool");
        client.registerTool(toolA, [](const QJsonObject&) { return QJsonObject{}; });

        QtLLM::Tool toolB;
        toolB.setName("toolB").setDescription("Second tool");
        client.registerTool(toolB, [](const QJsonObject&) { return QJsonObject{}; });

        QtLLM::Tool toolC;
        toolC.setName("toolC").setDescription("Third tool");
        client.registerTool(toolC, [](const QJsonObject&) { return QJsonObject{}; });

        client.unregisterTool("toolB");

        TEST_ASSERT(client.conversationHistory().isEmpty());
    }

};

TEST_INSTANTIATE(TST_Client);
