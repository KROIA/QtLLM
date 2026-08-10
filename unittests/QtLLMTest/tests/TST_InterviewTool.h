#pragma once

#include "UnitTest.h"
#include "QtLLM.h"
#include <QJsonObject>
#include <QJsonArray>


class TST_InterviewTool : public UnitTest::Test
{
    TEST_CLASS(TST_InterviewTool)
public:
    TST_InterviewTool()
        : Test("TST_InterviewTool")
    {
        ADD_TEST(TST_InterviewTool::testNameAndDescription);
        ADD_TEST(TST_InterviewTool::testSchemaTopLevel);
        ADD_TEST(TST_InterviewTool::testSchemaQuestionItem);
        ADD_TEST(TST_InterviewTool::testSchemaOptionItem);
        ADD_TEST(TST_InterviewTool::testRegisterOnClient);
    }

private:

    TEST_FUNCTION(testNameAndDescription)
    {
        TEST_START;

        TEST_COMPARE(QtLLM::InterviewTool::name(), QString("ask_user_question"));
        TEST_ASSERT(!QtLLM::InterviewTool::description().isEmpty());
    }


    TEST_FUNCTION(testSchemaTopLevel)
    {
        TEST_START;

        QJsonObject schema = QtLLM::InterviewTool::parameterSchema();
        TEST_COMPARE(schema["type"].toString(), QString("object"));

        QJsonArray required = schema["required"].toArray();
        TEST_COMPARE(required.size(), 1);
        TEST_COMPARE(required[0].toString(), QString("questions"));

        QJsonObject questions = schema["properties"].toObject()["questions"].toObject();
        TEST_COMPARE(questions["type"].toString(), QString("array"));
        TEST_COMPARE(questions["minItems"].toInt(), 1);
        TEST_COMPARE(questions["maxItems"].toInt(), 4);
    }


    TEST_FUNCTION(testSchemaQuestionItem)
    {
        TEST_START;

        QJsonObject schema = QtLLM::InterviewTool::parameterSchema();
        QJsonObject item = schema["properties"].toObject()["questions"].toObject()
                                 ["items"].toObject();
        TEST_COMPARE(item["type"].toString(), QString("object"));

        QJsonObject props = item["properties"].toObject();
        TEST_ASSERT(props.contains("question"));
        TEST_ASSERT(props.contains("header"));
        TEST_ASSERT(props.contains("multiSelect"));
        TEST_ASSERT(props.contains("allowCustom"));
        TEST_ASSERT(props.contains("options"));
        TEST_COMPARE(props["multiSelect"].toObject()["type"].toString(), QString("boolean"));

        QJsonArray required = item["required"].toArray();
        TEST_ASSERT(required.contains(QJsonValue("question")));
        TEST_ASSERT(required.contains(QJsonValue("options")));
    }


    TEST_FUNCTION(testSchemaOptionItem)
    {
        TEST_START;

        QJsonObject schema = QtLLM::InterviewTool::parameterSchema();
        QJsonObject options = schema["properties"].toObject()["questions"].toObject()
                                    ["items"].toObject()["properties"].toObject()
                                    ["options"].toObject();
        TEST_COMPARE(options["type"].toString(), QString("array"));
        TEST_COMPARE(options["minItems"].toInt(), 2);
        TEST_COMPARE(options["maxItems"].toInt(), 4);

        QJsonObject optionItem = options["items"].toObject();
        QJsonObject optionProps = optionItem["properties"].toObject();
        TEST_ASSERT(optionProps.contains("label"));
        TEST_ASSERT(optionProps.contains("description"));

        QJsonArray required = optionItem["required"].toArray();
        TEST_ASSERT(required.contains(QJsonValue("label")));
        TEST_ASSERT(!required.contains(QJsonValue("description")));
    }


    TEST_FUNCTION(testRegisterOnClient)
    {
        TEST_START;

        // Registering with a null chat widget must not crash; the handler
        // guards with QPointer and returns an error result if invoked.
        QtLLM::Client client("dummy-key");
        QtLLM::InterviewTool::registerOn(&client, nullptr);
    }
};

TEST_INSTANTIATE(TST_InterviewTool);
