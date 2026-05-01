#pragma once

#include "UnitTest.h"
#include "QtLLM.h"
#include <QJsonObject>
#include <QJsonArray>


class TST_Tool : public UnitTest::Test
{
    TEST_CLASS(TST_Tool)
public:
    TST_Tool()
        : Test("TST_Tool")
    {
        ADD_TEST(TST_Tool::testEmptyTool);
        ADD_TEST(TST_Tool::testSetNameAndDescription);
        ADD_TEST(TST_Tool::testSingleRequiredParam);
        ADD_TEST(TST_Tool::testSingleOptionalParam);
        ADD_TEST(TST_Tool::testMultipleParams);
        ADD_TEST(TST_Tool::testBuilderChaining);
    }

private:

    // Tests
    TEST_FUNCTION(testEmptyTool)
    {
        TEST_START;

        QtLLM::Tool tool;
        QJsonObject obj = tool.toApiObject();

        TEST_COMPARE(obj["name"].toString(), QString(""));
        TEST_COMPARE(obj["description"].toString(), QString(""));

        QJsonObject inputSchema = obj["input_schema"].toObject();
        TEST_COMPARE(inputSchema["type"].toString(), QString("object"));
        TEST_ASSERT(inputSchema["properties"].toObject().isEmpty());
    }


    TEST_FUNCTION(testSetNameAndDescription)
    {
        TEST_START;

        QtLLM::Tool tool;
        tool.setName("myTool").setDescription("does stuff");

        TEST_COMPARE(tool.name(), QString("myTool"));
        TEST_COMPARE(tool.description(), QString("does stuff"));
        TEST_COMPARE(tool.toApiObject()["name"].toString(), QString("myTool"));
    }


    TEST_FUNCTION(testSingleRequiredParam)
    {
        TEST_START;

        QtLLM::Tool tool;
        tool.setName("calculator");
        tool.addParameter("paramName", "string", "A string parameter", true);

        QJsonObject obj = tool.toApiObject();
        QJsonObject inputSchema = obj["input_schema"].toObject();
        QJsonObject properties = inputSchema["properties"].toObject();

        TEST_ASSERT(properties.contains("paramName"));
        TEST_COMPARE(properties["paramName"].toObject()["type"].toString(), QString("string"));

        QJsonArray required = inputSchema["required"].toArray();
        TEST_COMPARE(required.size(), 1);
        TEST_COMPARE(required[0].toString(), QString("paramName"));
    }


    TEST_FUNCTION(testSingleOptionalParam)
    {
        TEST_START;

        QtLLM::Tool tool;
        tool.setName("formatter");
        tool.addParameter("optParam", "integer", "An optional integer", false);

        QJsonObject obj = tool.toApiObject();
        QJsonObject inputSchema = obj["input_schema"].toObject();
        QJsonObject properties = inputSchema["properties"].toObject();

        TEST_ASSERT(properties.contains("optParam"));

        bool noRequired = !inputSchema.contains("required") || inputSchema["required"].toArray().isEmpty();
        TEST_ASSERT(noRequired);
    }


    TEST_FUNCTION(testMultipleParams)
    {
        TEST_START;

        QtLLM::Tool tool;
        tool.setName("multiTool");
        tool.addParameter("alpha", "string", "First param", true);
        tool.addParameter("beta",  "number", "Second param", true);
        tool.addParameter("gamma", "boolean", "Third param", false);

        QJsonObject obj = tool.toApiObject();
        QJsonObject inputSchema = obj["input_schema"].toObject();
        QJsonObject properties = inputSchema["properties"].toObject();

        TEST_COMPARE(properties.size(), 3);

        QJsonArray required = inputSchema["required"].toArray();
        TEST_COMPARE(required.size(), 2);

        bool gammaNotRequired = true;
        for (int i = 0; i < required.size(); ++i)
        {
            if (required[i].toString() == "gamma")
            {
                gammaNotRequired = false;
            }
        }
        TEST_ASSERT(gammaNotRequired);
    }


    TEST_FUNCTION(testBuilderChaining)
    {
        TEST_START;

        QtLLM::Tool tool;
        tool.setName("x").setDescription("y").addParameter("p1", "string", "desc", false);

        TEST_COMPARE(tool.name(), QString("x"));
    }

};

TEST_INSTANTIATE(TST_Tool);
