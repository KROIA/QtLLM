#pragma once

#include "UnitTest.h"
#include "QtLLM.h"
#include <QJsonObject>
#include <QJsonArray>


// Tests for the generic tool infrastructure (FeatureRequest QtLLM-Tool-API):
// enum parameters, validation, enable/disable, metadata, result helpers,
// call cap and consent-hook API surface.
class TST_ToolInfra : public UnitTest::Test
{
    TEST_CLASS(TST_ToolInfra)
public:
    TST_ToolInfra()
        : Test("TST_ToolInfra")
    {
        ADD_TEST(TST_ToolInfra::testEnumParameterInBothSchemas);
        ADD_TEST(TST_ToolInfra::testAddEnumParameter);
        ADD_TEST(TST_ToolInfra::testValidateOk);
        ADD_TEST(TST_ToolInfra::testValidateMissingRequired);
        ADD_TEST(TST_ToolInfra::testValidateWrongType);
        ADD_TEST(TST_ToolInfra::testValidateIntegerAcceptsWholeNumber);
        ADD_TEST(TST_ToolInfra::testValidateEnumCaseInsensitive);
        ADD_TEST(TST_ToolInfra::testValidateEnumRejects);
        ADD_TEST(TST_ToolInfra::testValidateAgainstRawSchema);
        ADD_TEST(TST_ToolInfra::testToolResultHelpers);
        ADD_TEST(TST_ToolInfra::testMetadataFields);
        ADD_TEST(TST_ToolInfra::testClientEnableDisable);
        ADD_TEST(TST_ToolInfra::testClientRegisteredTools);
        ADD_TEST(TST_ToolInfra::testClientSettingsSurface);
    }

private:

    static QtLLM::Tool makeTool()
    {
        QtLLM::Tool tool;
        tool.setName("createObject")
            .setDescription("Creates an object.")
            .addParameter("id", "integer", "Object id", true)
            .addParameter("fields", "object", "Extra fields", false)
            .addEnumParameter("type", {"SwOption", "Property", "AssignmentTable"},
                              "Object type", true);
        return tool;
    }


    TEST_FUNCTION(testEnumParameterInBothSchemas)
    {
        TEST_START;

        QtLLM::Tool tool = makeTool();

        QJsonObject claude = tool.toApiObject()["input_schema"].toObject();
        QJsonArray claudeEnum = claude["properties"].toObject()
                                      ["type"].toObject()["enum"].toArray();
        TEST_COMPARE(claudeEnum.size(), 3);
        TEST_COMPARE(claudeEnum[0].toString(), QString("SwOption"));

        QJsonObject openAi = tool.toOpenAiApiObject()["function"].toObject()
                                  ["parameters"].toObject();
        QJsonArray openAiEnum = openAi["properties"].toObject()
                                      ["type"].toObject()["enum"].toArray();
        TEST_COMPARE(openAiEnum.size(), 3);

        // Plain parameters must not get an enum key
        TEST_ASSERT(!claude["properties"].toObject()["id"].toObject().contains("enum"));
    }


    TEST_FUNCTION(testAddEnumParameter)
    {
        TEST_START;

        QtLLM::Tool tool;
        tool.setName("t").addEnumParameter("mode", {"fast", "safe"}, "Mode", true);

        QJsonObject prop = tool.toApiObject()["input_schema"].toObject()
                               ["properties"].toObject()["mode"].toObject();
        TEST_COMPARE(prop["type"].toString(), QString("string"));
        TEST_COMPARE(prop["enum"].toArray().size(), 2);
    }


    TEST_FUNCTION(testValidateOk)
    {
        TEST_START;

        QJsonObject err = makeTool().validate(
            QJsonObject{{"id", 5}, {"type", "SwOption"}});
        TEST_ASSERT(err.isEmpty());
    }


    TEST_FUNCTION(testValidateMissingRequired)
    {
        TEST_START;

        QJsonObject err = makeTool().validate(QJsonObject{{"type", "SwOption"}});
        TEST_COMPARE(err["status"].toString(), QString("error"));
        TEST_COMPARE(err["message"].toString(), QString("missing required parameter: id"));
        TEST_COMPARE(err["expected"].toObject()["id"].toString(), QString("integer"));
        TEST_COMPARE(err["expected"].toObject()["fields"].toString(), QString("object"));
    }


    TEST_FUNCTION(testValidateWrongType)
    {
        TEST_START;

        QJsonObject err = makeTool().validate(
            QJsonObject{{"id", "five"}, {"type", "SwOption"}});
        TEST_COMPARE(err["status"].toString(), QString("error"));
        TEST_ASSERT(err["message"].toString().contains("invalid type for 'id'"));
        TEST_ASSERT(err.contains("expected"));
    }


    TEST_FUNCTION(testValidateIntegerAcceptsWholeNumber)
    {
        TEST_START;

        // JSON has no integer type — 5.0 must pass, 5.5 must fail
        TEST_ASSERT(makeTool().validate(
            QJsonObject{{"id", 5.0}, {"type", "SwOption"}}).isEmpty());
        QJsonObject err = makeTool().validate(
            QJsonObject{{"id", 5.5}, {"type", "SwOption"}});
        TEST_COMPARE(err["status"].toString(), QString("error"));
    }


    TEST_FUNCTION(testValidateEnumCaseInsensitive)
    {
        TEST_START;

        QJsonObject err = makeTool().validate(
            QJsonObject{{"id", 1}, {"type", "swoption"}});
        TEST_ASSERT(err.isEmpty());
    }


    TEST_FUNCTION(testValidateEnumRejects)
    {
        TEST_START;

        QJsonObject err = makeTool().validate(
            QJsonObject{{"id", 1}, {"type", "Banana"}});
        TEST_COMPARE(err["status"].toString(), QString("error"));
        TEST_COMPARE(err["message"].toString(), QString("invalid value for 'type'"));
        TEST_COMPARE(err["allowed_values"].toArray().size(), 3);
    }


    TEST_FUNCTION(testValidateAgainstRawSchema)
    {
        TEST_START;

        QJsonObject schema{
            {"type", "object"},
            {"properties", QJsonObject{
                {"city", QJsonObject{{"type", "string"}}}}},
            {"required", QJsonArray{"city"}}};

        TEST_ASSERT(QtLLM::Tool::validateAgainstSchema(
            schema, QJsonObject{{"city", "Bern"}}).isEmpty());
        QJsonObject err = QtLLM::Tool::validateAgainstSchema(schema, QJsonObject{});
        TEST_COMPARE(err["status"].toString(), QString("error"));
    }


    TEST_FUNCTION(testToolResultHelpers)
    {
        TEST_START;

        QJsonObject ok = QtLLM::toolOk(QJsonObject{{"id", 42}});
        TEST_COMPARE(ok["status"].toString(), QString("ok"));
        TEST_COMPARE(ok["id"].toInt(), 42);

        QJsonObject err = QtLLM::toolError("not found", QJsonObject{{"id", 7}});
        TEST_COMPARE(err["status"].toString(), QString("error"));
        TEST_COMPARE(err["message"].toString(), QString("not found"));
        TEST_COMPARE(err["id"].toInt(), 7);
    }


    TEST_FUNCTION(testMetadataFields)
    {
        TEST_START;

        QtLLM::Tool tool;
        tool.setName("deleteObject")
            .setTitle("Delete object")
            .setGroup("Mutation")
            .setStatusText("Deleting…");

        TEST_COMPARE(tool.title(), QString("Delete object"));
        TEST_COMPARE(tool.group(), QString("Mutation"));
        TEST_COMPARE(tool.statusText(), QString("Deleting…"));

        // Metadata must never leak into the model-facing schemas
        QJsonObject api = tool.toApiObject();
        TEST_ASSERT(!api.contains("title"));
        TEST_ASSERT(!api.contains("group"));
        TEST_ASSERT(!api.contains("statusText"));
    }


    TEST_FUNCTION(testClientEnableDisable)
    {
        TEST_START;

        QtLLM::Client client("fake_key");
        client.registerTool(makeTool(), [](const QJsonObject&) { return QJsonObject{}; });
        QtLLM::Tool other;
        other.setName("otherTool").setDescription("x");
        client.registerTool(other, [](const QJsonObject&) { return QJsonObject{}; });

        TEST_COMPARE(client.toolNames().size(), 2);
        TEST_COMPARE(client.enabledToolNames().size(), 2);
        TEST_ASSERT(client.isToolEnabled("createObject"));

        client.setToolEnabled("createObject", false);
        TEST_ASSERT(!client.isToolEnabled("createObject"));
        TEST_COMPARE(client.toolNames().size(), 2);           // still registered
        TEST_COMPARE(client.enabledToolNames().size(), 1);
        TEST_COMPARE(client.enabledToolNames()[0], QString("otherTool"));

        client.setToolEnabled("createObject", true);
        TEST_COMPARE(client.enabledToolNames().size(), 2);

        // Unknown tools: not enabled, no crash
        TEST_ASSERT(!client.isToolEnabled("doesNotExist"));
        client.setToolEnabled("doesNotExist", false);
    }


    TEST_FUNCTION(testClientRegisteredTools)
    {
        TEST_START;

        QtLLM::Client client("fake_key");
        QtLLM::Tool tool = makeTool();
        tool.setTitle("Create object").setGroup("Mutation");
        client.registerTool(tool, [](const QJsonObject&) { return QJsonObject{}; });
        client.registerTool("rawTool", "raw description", QJsonObject{{"type", "object"}},
                            [](const QJsonObject&) { return QJsonObject{}; });

        QList<QtLLM::Tool> tools = client.registeredTools();
        TEST_COMPARE(tools.size(), 2);
        for (const QtLLM::Tool& t : tools) {
            if (t.name() == "createObject") {
                TEST_COMPARE(t.title(), QString("Create object"));
                TEST_COMPARE(t.group(), QString("Mutation"));
            } else {
                TEST_COMPARE(t.name(), QString("rawTool"));
                TEST_COMPARE(t.description(), QString("raw description"));
            }
        }
    }


    TEST_FUNCTION(testClientSettingsSurface)
    {
        TEST_START;

        QtLLM::Client client("fake_key");

        TEST_ASSERT(!client.validateToolInput());     // defaults: everything off
        TEST_COMPARE(client.maxToolCallsPerTurn(), 0);

        client.setValidateToolInput(true);
        TEST_ASSERT(client.validateToolInput());

        client.setMaxToolCallsPerTurn(3);
        TEST_COMPARE(client.maxToolCallsPerTurn(), 3);

        client.setToolConsentHandler(
            [](const QString&, const QJsonObject&) { return false; });
        client.setToolConsentHandler(nullptr);        // reset must not crash
    }
};

TEST_INSTANTIATE(TST_ToolInfra);
