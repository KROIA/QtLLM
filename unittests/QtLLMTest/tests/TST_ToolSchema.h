#pragma once

#include "UnitTest.h"
#include "QtLLM.h"
#include <QJsonObject>
#include <QJsonArray>


// Tests for schema building (arrays, nested objects, ranges, defaults,
// examples, strict mode), Tool::prepareInput() and Tool::isValidName().
class TST_ToolSchema : public UnitTest::Test
{
    TEST_CLASS(TST_ToolSchema)
public:
    TST_ToolSchema()
        : Test("TST_ToolSchema")
    {
        ADD_TEST(TST_ToolSchema::testArraySchema);
        ADD_TEST(TST_ToolSchema::testObjectSchema);
        ADD_TEST(TST_ToolSchema::testRawSchema);
        ADD_TEST(TST_ToolSchema::testRangeAndDefaultSchema);
        ADD_TEST(TST_ToolSchema::testExamples);
        ADD_TEST(TST_ToolSchema::testStrictSchema);
        ADD_TEST(TST_ToolSchema::testStrictSchemaAcceptsNullForOptional);
        ADD_TEST(TST_ToolSchema::testCoercion);
        ADD_TEST(TST_ToolSchema::testIntegerGivenAsDouble);
        ADD_TEST(TST_ToolSchema::testDefaultsApplied);
        ADD_TEST(TST_ToolSchema::testEnumSpelling);
        ADD_TEST(TST_ToolSchema::testUnknownParameter);
        ADD_TEST(TST_ToolSchema::testUnknownParameterAllowed);
        ADD_TEST(TST_ToolSchema::testNestedViolation);
        ADD_TEST(TST_ToolSchema::testArrayItemViolation);
        ADD_TEST(TST_ToolSchema::testRangeViolation);
        ADD_TEST(TST_ToolSchema::testNullValues);
        ADD_TEST(TST_ToolSchema::testEmptySchema);
        ADD_TEST(TST_ToolSchema::testExtraNesting);
        ADD_TEST(TST_ToolSchema::testValidateAgainstSchemaRecursive);
        ADD_TEST(TST_ToolSchema::testIsValidName);
    }

private:

    static QtLLM::Tool makeFields()
    {
        QtLLM::Tool fields;
        fields.addEnumParameter("mode", {"fast", "safe"}, "Mode", true)
              .addParameter("level", "integer", "Level", false)
              .setRange("level", 1, 5);
        return fields;
    }

    static QtLLM::Tool makeTool()
    {
        QtLLM::Tool tool;
        tool.setName("configure")
            .setDescription("Configures things.")
            .addParameter("count", "integer", "How many", true)
            .addParameter("ratio", "number", "Ratio", false)
            .addParameter("enabled", "boolean", "On/off", false)
            .addParameter("label", "string", "Label", false)
            .addParameter("color", "string", "Colour", false)
            .addEnumParameter("kind", {"SwOption", "Property"}, "Kind", false)
            .addArrayParameter("ids", "integer", "Ids", false)
            .addObjectParameter("options", makeFields(), "Options", false)
            .setRange("count", 0, 10)
            .setDefault("color", "red");
        return tool;
    }

    static QJsonObject properties(const QJsonObject& schema)
    {
        return schema["properties"].toObject();
    }


    TEST_FUNCTION(testArraySchema)
    {
        TEST_START;

        QtLLM::Tool tool;
        tool.addArrayParameter("tags", "string", "Tags", true)
            .addArrayParameter("points", QJsonObject{{"type", "object"},
                                                     {"properties", QJsonObject{
                                                         {"x", QJsonObject{{"type", "number"}}}}}},
                               "Points");

        QJsonObject props = properties(tool.toApiObject()["input_schema"].toObject());
        QJsonObject tags = props["tags"].toObject();
        TEST_COMPARE(tags["type"].toString(), QString("array"));
        TEST_COMPARE(tags["description"].toString(), QString("Tags"));
        TEST_COMPARE(tags["items"].toObject()["type"].toString(), QString("string"));

        QJsonObject points = props["points"].toObject();
        TEST_COMPARE(points["items"].toObject()["properties"].toObject()
                     ["x"].toObject()["type"].toString(), QString("number"));
    }


    TEST_FUNCTION(testObjectSchema)
    {
        TEST_START;

        QJsonObject schema = makeTool().toApiObject()["input_schema"].toObject();
        QJsonObject options = properties(schema)["options"].toObject();
        TEST_COMPARE(options["type"].toString(), QString("object"));
        TEST_COMPARE(options["description"].toString(), QString("Options"));
        TEST_COMPARE(properties(options)["mode"].toObject()["enum"].toArray().size(), 2);
        TEST_COMPARE(options["required"].toArray().size(), 1);
        TEST_COMPARE(options["required"].toArray()[0].toString(), QString("mode"));

        // Required at the outer level is only "count"
        TEST_COMPARE(schema["required"].toArray().size(), 1);
    }


    TEST_FUNCTION(testRawSchema)
    {
        TEST_START;

        QJsonObject raw{{"type", "string"}, {"format", "date"}, {"description", "Day"}};
        QtLLM::Tool tool;
        tool.addRawParameter("day", raw, true);

        QJsonObject schema = tool.toApiObject()["input_schema"].toObject();
        TEST_COMPARE(properties(schema)["day"].toObject(), raw);
        TEST_COMPARE(schema["required"].toArray()[0].toString(), QString("day"));
    }


    TEST_FUNCTION(testRangeAndDefaultSchema)
    {
        TEST_START;

        QJsonObject props = properties(makeTool().toApiObject()["input_schema"].toObject());
        TEST_COMPARE(props["count"].toObject()["minimum"].toDouble(), 0.0);
        TEST_COMPARE(props["count"].toObject()["maximum"].toDouble(), 10.0);
        TEST_COMPARE(props["color"].toObject()["default"].toString(), QString("red"));
        TEST_ASSERT(!props["label"].toObject().contains("default"));

        // Unknown names are ignored rather than creating a parameter
        QtLLM::Tool tool;
        tool.setRange("nope", 1, 2).setDefault("nope", 3);
        TEST_ASSERT(properties(tool.toApiObject()["input_schema"].toObject()).isEmpty());
    }


    TEST_FUNCTION(testExamples)
    {
        TEST_START;

        QtLLM::Tool tool;
        tool.setName("t").setDescription("Does it.")
            .addExample(QJsonObject{{"a", 1}})
            .addExample(QJsonObject{{"b", "x"}});

        TEST_COMPARE(tool.description(), QString("Does it."));
        TEST_COMPARE(tool.toApiObject()["description"].toString(),
                     QString("Does it.\nExample input: {\"a\":1}\nExample input: {\"b\":\"x\"}"));
        TEST_ASSERT(tool.toOpenAiApiObject()["function"].toObject()["description"]
                        .toString().contains("Example input: {\"a\":1}"));

        QtLLM::Tool bare;
        bare.addExample(QJsonObject{{"a", 1}});
        TEST_COMPARE(bare.toApiObject()["description"].toString(),
                     QString("Example input: {\"a\":1}"));
    }


    TEST_FUNCTION(testStrictSchema)
    {
        TEST_START;

        QtLLM::Tool tool = makeTool();
        TEST_ASSERT(!tool.isStrict());
        TEST_ASSERT(!tool.toApiObject().contains("strict"));
        tool.setStrict(true);
        TEST_ASSERT(tool.isStrict());

        QJsonObject api = tool.toApiObject();
        TEST_ASSERT(api["strict"].toBool());
        QJsonObject schema = api["input_schema"].toObject();
        TEST_ASSERT(schema.contains("additionalProperties"));
        TEST_ASSERT(!schema["additionalProperties"].toBool());
        TEST_COMPARE(schema["required"].toArray().size(), properties(schema).size());

        QJsonObject props = properties(schema);
        TEST_COMPARE(props["count"].toObject()["type"].toString(), QString("integer"));
        TEST_COMPARE(props["ratio"].toObject()["type"].toArray(),
                     (QJsonArray{"number", "null"}));

        // Nullable enum must list null too, or null is still rejected
        TEST_ASSERT(props["kind"].toObject()["enum"].toArray().contains(QJsonValue()));

        // Constraints strict mode rejects move into the description
        QJsonObject count = props["count"].toObject();
        TEST_ASSERT(!count.contains("minimum"));
        TEST_ASSERT(count["description"].toString().contains("minimum: 0"));
        TEST_ASSERT(!props["color"].toObject().contains("default"));
        TEST_ASSERT(props["color"].toObject()["description"].toString().contains("default: \"red\""));

        QJsonObject options = props["options"].toObject();
        TEST_ASSERT(options.contains("additionalProperties"));
        TEST_COMPARE(options["required"].toArray().size(), 2);
        TEST_COMPARE(properties(options)["level"].toObject()["type"].toArray(),
                     (QJsonArray{"integer", "null"}));

        // The full schema keeps ranges and defaults for client-side checks
        TEST_ASSERT(properties(tool.parameterSchema())["count"].toObject().contains("minimum"));

        // OpenAI format is unaffected by strict
        QJsonObject function = tool.toOpenAiApiObject()["function"].toObject();
        TEST_ASSERT(!function.contains("strict"));
        QJsonObject parameters = function["parameters"].toObject();
        TEST_ASSERT(!parameters.contains("additionalProperties"));
        TEST_COMPARE(parameters["required"].toArray().size(), 1);
        TEST_COMPARE(properties(parameters)["ratio"].toObject()["type"].toString(), QString("number"));
    }


    TEST_FUNCTION(testStrictSchemaAcceptsNullForOptional)
    {
        TEST_START;

        QtLLM::Tool tool = makeTool();
        tool.setStrict(true);
        QJsonObject schema = tool.toApiObject()["input_schema"].toObject();

        QJsonObject args{{"count", 3}, {"ratio", QJsonValue()}, {"kind", QJsonValue()}};
        QJsonObject error;
        TEST_ASSERT(QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_ASSERT(!args.contains("ratio"));
        TEST_ASSERT(!args.contains("kind"));
    }


    TEST_FUNCTION(testCoercion)
    {
        TEST_START;

        const QJsonObject schema = makeTool().parameterSchema();
        QJsonObject args{{"count", " 7 "}, {"ratio", "2.5"}, {"enabled", "TRUE"},
                         {"ids", QJsonArray{"1", 2}}};
        QJsonObject error;
        TEST_ASSERT(QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_ASSERT(args["count"].isDouble());
        TEST_COMPARE(args["count"].toInt(), 7);
        TEST_COMPARE(args["ratio"].toDouble(), 2.5);
        TEST_ASSERT(args["enabled"].isBool());
        TEST_ASSERT(args["enabled"].toBool());
        TEST_COMPARE(args["ids"].toArray()[0].toInt(), 1);

        args = QJsonObject{{"count", 1}, {"enabled", "false"}};
        TEST_ASSERT(QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_ASSERT(args["enabled"].isBool() && !args["enabled"].toBool());

        // A number is not turned into a string
        args = QJsonObject{{"count", 1}, {"label", 5}};
        TEST_ASSERT(!QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_ASSERT(error["message"].toString().contains("invalid type for 'label'"));

        // "5.5" is no integer, "yes" no boolean, "abc" no number
        args = QJsonObject{{"count", "5.5"}};
        TEST_ASSERT(!QtLLM::Tool::prepareInput(schema, args, &error));
        args = QJsonObject{{"count", 1}, {"enabled", "yes"}};
        TEST_ASSERT(!QtLLM::Tool::prepareInput(schema, args, &error));
        args = QJsonObject{{"count", "abc"}};
        TEST_ASSERT(!QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_COMPARE(error["status"].toString(), QString("error"));
        TEST_ASSERT(error["expected"].toObject().contains("count"));
    }


    TEST_FUNCTION(testIntegerGivenAsDouble)
    {
        TEST_START;

        const QJsonObject schema = makeTool().parameterSchema();
        QJsonObject args{{"count", 5.0}};
        QJsonObject error;
        TEST_ASSERT(QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_COMPARE(args["count"].toInt(), 5);

        args = QJsonObject{{"count", "5.0"}};
        TEST_ASSERT(QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_COMPARE(args["count"].toInt(), 5);

        args = QJsonObject{{"count", 5.5}};
        TEST_ASSERT(!QtLLM::Tool::prepareInput(schema, args, &error));
    }


    TEST_FUNCTION(testDefaultsApplied)
    {
        TEST_START;

        const QJsonObject schema = makeTool().parameterSchema();
        QJsonObject args{{"count", 1}};
        QJsonObject error;
        TEST_ASSERT(QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_COMPARE(args["color"].toString(), QString("red"));
        TEST_ASSERT(!args.contains("label"));

        // An explicit value wins over the default
        args = QJsonObject{{"count", 1}, {"color", "blue"}};
        TEST_ASSERT(QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_COMPARE(args["color"].toString(), QString("blue"));
    }


    TEST_FUNCTION(testEnumSpelling)
    {
        TEST_START;

        const QJsonObject schema = makeTool().parameterSchema();
        QJsonObject args{{"count", 1}, {"kind", "swoption"},
                         {"options", QJsonObject{{"mode", "SAFE"}}}};
        QJsonObject error;
        TEST_ASSERT(QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_COMPARE(args["kind"].toString(), QString("SwOption"));
        TEST_COMPARE(args["options"].toObject()["mode"].toString(), QString("safe"));

        args = QJsonObject{{"count", 1}, {"kind", "Banana"}};
        TEST_ASSERT(!QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_COMPARE(error["message"].toString(), QString("invalid value for 'kind'"));
        TEST_COMPARE(error["allowed_values"].toArray().size(), 2);
    }


    TEST_FUNCTION(testUnknownParameter)
    {
        TEST_START;

        const QJsonObject schema = makeTool().parameterSchema();
        QJsonObject args{{"count", 1}, {"colr", "blue"}};
        const QJsonObject original = args;
        QJsonObject error;
        TEST_ASSERT(!QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_COMPARE(args, original);
        TEST_COMPARE(error["status"].toString(), QString("error"));
        TEST_COMPARE(error["message"].toString(), QString("unknown parameter: colr"));
        TEST_COMPARE(error["did_you_mean"].toString(), QString("color"));
        TEST_ASSERT(error["valid_parameters"].toArray().contains(QJsonValue("count")));
        TEST_ASSERT(error["expected"].toObject().contains("color"));

        // Case-only difference is suggested; the error is still raised
        args = QJsonObject{{"Count", 1}};
        TEST_ASSERT(!QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_COMPARE(error["did_you_mean"].toString(), QString("count"));

        // Nothing close: no suggestion, and a short name never suggests another
        args = QJsonObject{{"count", 1}, {"zzzzzz", 1}};
        TEST_ASSERT(!QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_ASSERT(!error.contains("did_you_mean"));
        args = QJsonObject{{"count", 1}, {"x", 1}};
        TEST_ASSERT(!QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_ASSERT(!error.contains("did_you_mean"));

        // A null error pointer is allowed
        TEST_ASSERT(!QtLLM::Tool::prepareInput(schema, args, nullptr));
    }


    TEST_FUNCTION(testUnknownParameterAllowed)
    {
        TEST_START;

        const QJsonObject schema = makeTool().parameterSchema();
        QJsonObject args{{"count", 1}, {"extra", 1}};
        QJsonObject error;
        TEST_ASSERT(QtLLM::Tool::prepareInput(schema, args, &error, false));
        TEST_COMPARE(args["extra"].toInt(), 1);

        // additionalProperties: true opts a schema out of the check
        QJsonObject open{{"type", "object"},
                         {"properties", QJsonObject{{"a", QJsonObject{{"type", "string"}}}}},
                         {"additionalProperties", true}};
        args = QJsonObject{{"b", 1}};
        TEST_ASSERT(QtLLM::Tool::prepareInput(open, args, &error));
    }


    TEST_FUNCTION(testNestedViolation)
    {
        TEST_START;

        const QJsonObject schema = makeTool().parameterSchema();
        QJsonObject error;

        QJsonObject args{{"count", 1}, {"options", QJsonObject{{"level", 2}}}};
        TEST_ASSERT(!QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_COMPARE(error["message"].toString(), QString("missing required parameter: options.mode"));
        TEST_ASSERT(error["expected"].toObject().contains("mode"));

        args = QJsonObject{{"count", 1}, {"options", QJsonObject{{"mode", "fast"}, {"levl", 2}}}};
        TEST_ASSERT(!QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_COMPARE(error["message"].toString(), QString("unknown parameter: options.levl"));
        TEST_COMPARE(error["did_you_mean"].toString(), QString("level"));

        args = QJsonObject{{"count", 1}, {"options", QJsonObject{{"mode", "fast"}, {"level", 9}}}};
        TEST_ASSERT(!QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_ASSERT(error["message"].toString().contains("options.level"));

        // Coercion reaches nested values
        args = QJsonObject{{"count", 1}, {"options", QJsonObject{{"mode", "fast"}, {"level", "3"}}}};
        TEST_ASSERT(QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_COMPARE(args["options"].toObject()["level"].toInt(), 3);
    }


    TEST_FUNCTION(testArrayItemViolation)
    {
        TEST_START;

        const QJsonObject schema = makeTool().parameterSchema();
        QJsonObject args{{"count", 1}, {"ids", QJsonArray{1, "two", 3}}};
        QJsonObject error;
        TEST_ASSERT(!QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_COMPARE(error["message"].toString(), QString("invalid type for 'ids[1]': expected integer"));

        // Items that are objects are checked recursively
        QtLLM::Tool tool;
        tool.addArrayParameter("points", QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{{"x", QJsonObject{{"type", "number"}}}}},
            {"required", QJsonArray{"x"}}}, "Points", true);
        args = QJsonObject{{"points", QJsonArray{QJsonObject{{"x", 1}}, QJsonObject{}}}};
        TEST_ASSERT(!QtLLM::Tool::prepareInput(tool.parameterSchema(), args, &error));
        TEST_COMPARE(error["message"].toString(), QString("missing required parameter: points[1].x"));

        args = QJsonObject{{"points", QJsonArray{}}};
        TEST_ASSERT(QtLLM::Tool::prepareInput(tool.parameterSchema(), args, &error));
    }


    TEST_FUNCTION(testRangeViolation)
    {
        TEST_START;

        const QJsonObject schema = makeTool().parameterSchema();
        QJsonObject error;
        QJsonObject args{{"count", 11}};
        TEST_ASSERT(!QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_COMPARE(error["message"].toString(), QString("value for 'count' must be at most 10"));
        TEST_ASSERT(error.contains("expected"));

        args = QJsonObject{{"count", -1}};
        TEST_ASSERT(!QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_COMPARE(error["message"].toString(), QString("value for 'count' must be at least 0"));

        // Bounds are inclusive; a coerced string is range-checked too
        args = QJsonObject{{"count", 10}};
        TEST_ASSERT(QtLLM::Tool::prepareInput(schema, args, &error));
        args = QJsonObject{{"count", "12"}};
        TEST_ASSERT(!QtLLM::Tool::prepareInput(schema, args, &error));
    }


    TEST_FUNCTION(testNullValues)
    {
        TEST_START;

        const QJsonObject schema = makeTool().parameterSchema();
        QJsonObject error;

        // Null for an optional parameter is "omitted", so the default applies
        QJsonObject args{{"count", 1}, {"label", QJsonValue()}, {"color", QJsonValue()}};
        TEST_ASSERT(QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_ASSERT(!args.contains("label"));
        TEST_COMPARE(args["color"].toString(), QString("red"));

        // Null for a required parameter is not, it is a wrong value
        args = QJsonObject{{"count", QJsonValue()}};
        TEST_ASSERT(!QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_COMPARE(error["message"].toString(), QString("invalid type for 'count': expected integer"));
    }


    TEST_FUNCTION(testEmptySchema)
    {
        TEST_START;

        QJsonObject error;

        // No schema at all: nothing to check against
        QJsonObject args{{"anything", 1}};
        TEST_ASSERT(QtLLM::Tool::prepareInput(QJsonObject(), args, &error));
        TEST_COMPARE(args["anything"].toInt(), 1);

        // A tool without parameters declares an empty property set
        QtLLM::Tool tool;
        tool.setName("ping");
        args = QJsonObject();
        TEST_ASSERT(QtLLM::Tool::prepareInput(tool.parameterSchema(), args, &error));
        args = QJsonObject{{"x", 1}};
        TEST_ASSERT(!QtLLM::Tool::prepareInput(tool.parameterSchema(), args, &error));
        TEST_ASSERT(error["valid_parameters"].toArray().isEmpty());

        // A free-form object parameter accepts any content
        QtLLM::Tool freeForm;
        freeForm.addParameter("fields", "object", "Anything", true);
        args = QJsonObject{{"fields", QJsonObject{{"a", QJsonObject{{"b", 1}}}}}};
        TEST_ASSERT(QtLLM::Tool::prepareInput(freeForm.parameterSchema(), args, &error));
    }


    TEST_FUNCTION(testExtraNesting)
    {
        TEST_START;

        // Models sometimes wrap the arguments in one more object
        QJsonObject schema{
            {"type", "object"},
            {"properties", QJsonObject{{"city", QJsonObject{{"type", "string"}}}}},
            {"required", QJsonArray{"city"}}};
        QJsonObject args{{"input", QJsonObject{{"city", "Bern"}}}};
        QJsonObject error;
        TEST_ASSERT(!QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_COMPARE(error["message"].toString(), QString("unknown parameter: input"));

        // An object where a string is expected is a type error, not coerced
        args = QJsonObject{{"city", QJsonObject{{"name", "Bern"}}}};
        TEST_ASSERT(!QtLLM::Tool::prepareInput(schema, args, &error));
        TEST_ASSERT(error["message"].toString().contains("invalid type for 'city'"));
    }


    TEST_FUNCTION(testValidateAgainstSchemaRecursive)
    {
        TEST_START;

        const QJsonObject schema = makeTool().parameterSchema();

        // Now recursive: nested objects, array items and ranges
        TEST_ASSERT(!QtLLM::Tool::validateAgainstSchema(
            schema, QJsonObject{{"count", 1}, {"options", QJsonObject{}}}).isEmpty());
        TEST_ASSERT(!QtLLM::Tool::validateAgainstSchema(
            schema, QJsonObject{{"count", 1}, {"ids", QJsonArray{"x"}}}).isEmpty());
        TEST_ASSERT(!QtLLM::Tool::validateAgainstSchema(
            schema, QJsonObject{{"count", 20}}).isEmpty());

        // Still lenient towards unknown names, and does not coerce
        TEST_ASSERT(QtLLM::Tool::validateAgainstSchema(
            schema, QJsonObject{{"count", 1}, {"extra", 1}}).isEmpty());
        TEST_ASSERT(!QtLLM::Tool::validateAgainstSchema(
            schema, QJsonObject{{"count", "1"}}).isEmpty());
    }


    TEST_FUNCTION(testIsValidName)
    {
        TEST_START;

        TEST_ASSERT(QtLLM::Tool::isValidName("get_weather"));
        TEST_ASSERT(QtLLM::Tool::isValidName("a-B_9"));
        TEST_ASSERT(QtLLM::Tool::isValidName(QString(64, QLatin1Char('a'))));
        TEST_ASSERT(!QtLLM::Tool::isValidName(QString(65, QLatin1Char('a'))));
        TEST_ASSERT(!QtLLM::Tool::isValidName(""));
        TEST_ASSERT(!QtLLM::Tool::isValidName("get weather"));
        TEST_ASSERT(!QtLLM::Tool::isValidName("tool.name"));
        TEST_ASSERT(!QtLLM::Tool::isValidName("name\n"));
        TEST_ASSERT(!QtLLM::Tool::isValidName(QString::fromUtf8("w\xC3\xA4rme")));
    }

};

TEST_INSTANTIATE(TST_ToolSchema);
