#include "Tool.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QVector>
#include <algorithm>
#include <cmath>

namespace QtLLM
{

Tool::Tool()
{
}

Tool& Tool::setName(const QString& name)
{
    m_name = name;
    return *this;
}

Tool& Tool::setDescription(const QString& description)
{
    m_description = description;
    return *this;
}

Tool& Tool::setTitle(const QString& title)
{
    m_title = title;
    return *this;
}

Tool& Tool::setGroup(const QString& group)
{
    m_group = group;
    return *this;
}

Tool& Tool::setStatusText(const QString& statusText)
{
    m_statusText = statusText;
    return *this;
}

Tool& Tool::addParameter(const QString& name,
                         const QString& type,
                         const QString& description,
                         bool required,
                         const QStringList& enumValues)
{
    QJsonObject schema;
    schema["type"] = type;
    schema["description"] = description;
    if (!enumValues.isEmpty())
        schema["enum"] = QJsonArray::fromStringList(enumValues);
    return addParameterSchema(name, schema, required);
}

Tool& Tool::addEnumParameter(const QString& name,
                             const QStringList& allowedValues,
                             const QString& description,
                             bool required)
{
    return addParameter(name, QStringLiteral("string"), description, required, allowedValues);
}

Tool& Tool::addArrayParameter(const QString& name,
                              const QString& itemType,
                              const QString& description,
                              bool required)
{
    return addArrayParameter(name, QJsonObject{{"type", itemType}}, description, required);
}

Tool& Tool::addArrayParameter(const QString& name,
                              const QJsonObject& itemSchema,
                              const QString& description,
                              bool required)
{
    QJsonObject schema;
    schema["type"] = QStringLiteral("array");
    schema["description"] = description;
    schema["items"] = itemSchema;
    return addParameterSchema(name, schema, required);
}

Tool& Tool::addObjectParameter(const QString& name,
                               const Tool& fields,
                               const QString& description,
                               bool required)
{
    QJsonObject schema = fields.buildParameterSchema();
    schema["description"] = description;
    return addParameterSchema(name, schema, required);
}

Tool& Tool::addRawParameter(const QString& name,
                            const QJsonObject& schema,
                            bool required)
{
    return addParameterSchema(name, schema, required);
}

Tool& Tool::setRange(const QString& name, double minimum, double maximum)
{
    if (Parameter* param = findParameter(name)) {
        param->schema["minimum"] = minimum;
        param->schema["maximum"] = maximum;
    }
    return *this;
}

Tool& Tool::setDefault(const QString& name, const QJsonValue& value)
{
    if (Parameter* param = findParameter(name))
        param->schema["default"] = value;
    return *this;
}

Tool& Tool::addExample(const QJsonObject& input)
{
    m_examples.append(input);
    return *this;
}

Tool& Tool::setStrict(bool strict)
{
    m_strict = strict;
    return *this;
}

bool Tool::isValidName(const QString& name)
{
    // \A..\z rather than ^..$: '$' would also match before a trailing newline
    static const QRegularExpression re(QStringLiteral("\\A[a-zA-Z0-9_-]{1,64}\\z"));
    return re.match(name).hasMatch();
}

Tool& Tool::addParameterSchema(const QString& name, const QJsonObject& schema, bool required)
{
    m_parameters.append(Parameter{name, required, schema});
    return *this;
}

Tool::Parameter* Tool::findParameter(const QString& name)
{
    for (Parameter& param : m_parameters) {
        if (param.name == name)
            return &param;
    }
    return nullptr;
}

Tool& Tool::setReadOnly(bool readOnly)
{
    m_readOnly = readOnly;
    return *this;
}

Tool& Tool::setMaxResultChars(int chars)
{
    m_maxResultChars = chars;
    return *this;
}

Tool& Tool::setTimeoutMs(int ms)
{
    m_timeoutMs = ms;
    return *this;
}

QString Tool::name() const
{
    return m_name;
}

QString Tool::description() const
{
    return m_description;
}

QString Tool::title() const
{
    return m_title;
}

QString Tool::group() const
{
    return m_group;
}

QString Tool::statusText() const
{
    return m_statusText;
}

QJsonObject Tool::parameterSchema() const
{
    return buildParameterSchema();
}

QJsonObject Tool::buildParameterSchema() const
{
    QJsonObject properties;
    QJsonArray requiredArray;

    for (const Parameter& param : m_parameters) {
        properties[param.name] = param.schema;

        if (param.required) {
            requiredArray.append(param.name);
        }
    }

    QJsonObject schema;
    schema["type"] = QStringLiteral("object");
    schema["properties"] = properties;
    if (!requiredArray.isEmpty()) {
        schema["required"] = requiredArray;
    }
    return schema;
}

QJsonObject Tool::validate(const QJsonObject& args) const
{
    return validateAgainstSchema(buildParameterSchema(), args);
}

// Type check for a single JSON-Schema primitive name. Unknown type names
// pass — forward compatibility beats false rejections here.
static bool matchesJsonType(const QString& type, const QJsonValue& value)
{
    if (type == QLatin1String("string"))  return value.isString();
    if (type == QLatin1String("boolean")) return value.isBool();
    if (type == QLatin1String("array"))   return value.isArray();
    if (type == QLatin1String("object"))  return value.isObject();
    if (type == QLatin1String("null"))    return value.isNull();
    if (type == QLatin1String("number"))  return value.isDouble();
    if (type == QLatin1String("integer")) {
        if (!value.isDouble())
            return false;
        const double d = value.toDouble();
        return d == static_cast<double>(static_cast<qint64>(d));
    }
    return true;
}

// "type" is a single name or, in nullable/strict schemas, a list of names.
static QStringList schemaTypes(const QJsonObject& schema)
{
    const QJsonValue type = schema["type"];
    if (type.isString())
        return {type.toString()};
    QStringList types;
    for (const QJsonValue& t : type.toArray())
        types.append(t.toString());
    return types;
}

static bool matchesAnyType(const QStringList& types, const QJsonValue& value)
{
    if (types.isEmpty())
        return true;
    for (const QString& type : types) {
        if (matchesJsonType(type, value))
            return true;
    }
    return false;
}

static QString jsonText(const QJsonValue& value)
{
    // QJsonDocument cannot hold a scalar in Qt 5, so serialise via an array
    const QByteArray json = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}

static int editDistance(const QString& a, const QString& b)
{
    QVector<int> row(b.size() + 1);
    for (int j = 0; j <= b.size(); ++j)
        row[j] = j;
    for (int i = 1; i <= a.size(); ++i) {
        int diagonal = row[0];
        row[0] = i;
        for (int j = 1; j <= b.size(); ++j) {
            const int above = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1,
                               diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

// Closest declared name, or empty when nothing is near enough to be a typo.
// The distance must stay below the shorter length so "x" never suggests "id".
static QString closestName(const QString& name, const QStringList& candidates)
{
    QString best;
    int bestDistance = 3;
    for (const QString& candidate : candidates) {
        const int d = editDistance(name.toLower(), candidate.toLower());
        if (d < bestDistance && d < std::min(name.size(), candidate.size())) {
            best = candidate;
            bestDistance = d;
        }
    }
    return best;
}

namespace
{
struct InputPass
{
    bool repair;         // coerce, apply defaults, treat optional null as omitted
    bool rejectUnknown;
    QJsonObject error;
};
}

static QString joinPath(const QString& path, const QString& key)
{
    return path.isEmpty() ? key : path + '.' + key;
}

static bool failWith(InputPass& pass, const QString& message, const QJsonObject& expected)
{
    pass.error = QJsonObject{
        {"status",   "error"},
        {"message",  message},
        {"expected", expected}};
    return false;
}

static bool prepareObject(const QJsonObject& schema, QJsonObject& obj,
                          const QString& path, InputPass& pass);

static bool prepareValue(const QJsonObject& schema, QJsonValue& value, const QString& path,
                         const QJsonObject& expected, InputPass& pass)
{
    const QStringList types = schemaTypes(schema);

    // Local models in particular send "5" for 5 and "true" for true
    if (pass.repair && value.isString() && !types.contains(QStringLiteral("string"))) {
        const QString text = value.toString().trimmed();
        bool ok = false;
        const double d = text.toDouble(&ok);
        const bool isTrue = text.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
        const bool isFalse = text.compare(QLatin1String("false"), Qt::CaseInsensitive) == 0;
        if (ok && std::isfinite(d)
            && (types.contains(QStringLiteral("number"))
                || (types.contains(QStringLiteral("integer"))
                    && matchesJsonType(QStringLiteral("integer"), d))))
            value = d;
        else if (types.contains(QStringLiteral("boolean")) && (isTrue || isFalse))
            value = isTrue;
    }

    const QJsonArray enumArray = schema["enum"].toArray();
    if (value.isString()) {
        // Case-insensitive — models often return values in a different case
        for (const QJsonValue& allowed : enumArray) {
            if (allowed.isString()
                && allowed.toString().compare(value.toString(), Qt::CaseInsensitive) == 0) {
                value = allowed;
                break;
            }
        }
    }

    if (!matchesAnyType(types, value)) {
        return failWith(pass, QString("invalid type for '%1': expected %2")
                                  .arg(path, types.join('|')), expected);
    }

    if (!enumArray.isEmpty() && !enumArray.contains(value)) {
        pass.error = QJsonObject{
            {"status",         "error"},
            {"message",        QString("invalid value for '%1'").arg(path)},
            {"allowed_values", enumArray}};
        return false;
    }

    if (value.isDouble()) {
        const double d = value.toDouble();
        if (schema.contains("minimum") && d < schema["minimum"].toDouble()) {
            return failWith(pass, QString("value for '%1' must be at least %2")
                                      .arg(path).arg(schema["minimum"].toDouble()), expected);
        }
        if (schema.contains("maximum") && d > schema["maximum"].toDouble()) {
            return failWith(pass, QString("value for '%1' must be at most %2")
                                      .arg(path).arg(schema["maximum"].toDouble()), expected);
        }
    }

    if (value.isObject() && schema.contains("properties")) {
        QJsonObject nested = value.toObject();
        if (!prepareObject(schema, nested, path, pass))
            return false;
        value = nested;
    }

    if (value.isArray() && schema["items"].isObject()) {
        const QJsonObject itemSchema = schema["items"].toObject();
        QJsonArray items = value.toArray();
        for (int i = 0; i < items.size(); ++i) {
            QJsonValue item = items[i];
            if (!prepareValue(itemSchema, item, QString("%1[%2]").arg(path).arg(i), expected, pass))
                return false;
            items[i] = item;
        }
        value = items;
    }
    return true;
}

static bool prepareObject(const QJsonObject& schema, QJsonObject& obj,
                          const QString& path, InputPass& pass)
{
    const QJsonObject properties = schema["properties"].toObject();
    const QJsonArray required = schema["required"].toArray();

    // "expected" map (name -> type) reused by all error results so the model
    // can self-correct on the next attempt.
    QJsonObject expected;
    for (auto it = properties.begin(); it != properties.end(); ++it)
        expected[it.key()] = schemaTypes(it.value().toObject()).join('|');

    // Strict schemas express "optional" as required-but-nullable
    auto nullable = [&](const QString& name) {
        return schemaTypes(properties[name].toObject()).contains(QStringLiteral("null"));
    };

    if (pass.repair) {
        for (const QString& key : obj.keys()) {
            if (obj[key].isNull() && properties.contains(key)
                && (!required.contains(key) || nullable(key)))
                obj.remove(key);
        }
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            const QJsonObject prop = it.value().toObject();
            if (!obj.contains(it.key()) && prop.contains("default"))
                obj[it.key()] = prop["default"];
        }
    }

    // Only a schema that declares its properties can call a name unknown
    const QJsonValue additional = schema["additionalProperties"];
    if (pass.rejectUnknown && schema.contains("properties")
        && !additional.isObject() && additional != QJsonValue(true)) {
        for (const QString& key : obj.keys()) {
            if (properties.contains(key))
                continue;
            failWith(pass, QString("unknown parameter: %1").arg(joinPath(path, key)), expected);
            pass.error["valid_parameters"] = QJsonArray::fromStringList(properties.keys());
            const QString suggestion = closestName(key, properties.keys());
            if (!suggestion.isEmpty())
                pass.error["did_you_mean"] = suggestion;
            return false;
        }
    }

    for (const QJsonValue& req : required) {
        const QString name = req.toString();
        if (!obj.contains(name) && !nullable(name)) {
            return failWith(pass, QString("missing required parameter: %1")
                                      .arg(joinPath(path, name)), expected);
        }
    }

    for (auto it = properties.begin(); it != properties.end(); ++it) {
        if (!obj.contains(it.key()))
            continue;
        QJsonValue value = obj[it.key()];
        if (!prepareValue(it.value().toObject(), value, joinPath(path, it.key()), expected, pass))
            return false;
        obj[it.key()] = value;
    }
    return true;
}

QJsonObject Tool::validateAgainstSchema(const QJsonObject& parameterSchema,
                                        const QJsonObject& args)
{
    InputPass pass{false, false, QJsonObject()};
    QJsonObject copy = args;
    return prepareObject(parameterSchema, copy, QString(), pass) ? QJsonObject() : pass.error;
}

bool Tool::prepareInput(const QJsonObject& parameterSchema,
                        QJsonObject& args,
                        QJsonObject* error,
                        bool rejectUnknown)
{
    InputPass pass{true, rejectUnknown, QJsonObject()};
    QJsonObject repaired = args;
    if (!prepareObject(parameterSchema, repaired, QString(), pass)) {
        if (error)
            *error = pass.error;
        return false;
    }
    args = repaired;
    return true;
}

QJsonObject Tool::normalizeEnumCase(const QJsonObject& parameterSchema,
                                    const QJsonObject& args)
{
    QJsonObject normalized = args;
    const QJsonObject properties = parameterSchema["properties"].toObject();
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        const QJsonValue value = args.value(it.key());
        if (!value.isString())
            continue;
        for (const QJsonValue& allowed : it.value().toObject()["enum"].toArray()) {
            if (allowed.toString().compare(value.toString(), Qt::CaseInsensitive) == 0) {
                normalized[it.key()] = allowed;
                break;
            }
        }
    }
    return normalized;
}

// Rewrites a schema into the form Claude's strict mode accepts: closed
// objects, every property required, optional ones nullable, and constraints
// strict mode rejects moved into the description so the model still sees them.
static QJsonObject strictSchema(QJsonObject schema)
{
    QStringList notes;
    for (const char* key : {"minimum", "maximum", "default"}) {
        const QString name = QLatin1String(key);
        if (schema.contains(name)) {
            notes.append(QString("%1: %2").arg(name, jsonText(schema[name])));
            schema.remove(name);
        }
    }
    if (!notes.isEmpty()) {
        const QString description = schema["description"].toString();
        schema["description"] = (description.isEmpty() ? QString() : description + ' ')
                                + '(' + notes.join(QStringLiteral(", ")) + ')';
    }

    if (schema["items"].isObject())
        schema["items"] = strictSchema(schema["items"].toObject());

    if (!schemaTypes(schema).contains(QStringLiteral("object")) && !schema.contains("properties"))
        return schema;

    const QJsonObject properties = schema["properties"].toObject();
    const QJsonArray required = schema["required"].toArray();
    QJsonObject strictProperties;
    QJsonArray allRequired;
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        QJsonObject prop = strictSchema(it.value().toObject());
        if (!required.contains(it.key())) {
            const QJsonValue type = prop["type"];
            if (type.isString()) {
                prop["type"] = QJsonArray{type, "null"};
            } else if (type.isArray()) {
                QJsonArray types = type.toArray();
                if (!types.contains(QJsonValue("null")))
                    types.append("null");
                prop["type"] = types;
            } else {
                prop = QJsonObject{{"anyOf", QJsonArray{prop, QJsonObject{{"type", "null"}}}}};
            }
            // The enum would otherwise still rule the null out
            if (prop.contains("enum")) {
                QJsonArray values = prop["enum"].toArray();
                if (!values.contains(QJsonValue()))
                    values.append(QJsonValue());
                prop["enum"] = values;
            }
        }
        strictProperties[it.key()] = prop;
        allRequired.append(it.key());
    }
    schema["properties"] = strictProperties;
    if (!allRequired.isEmpty())
        schema["required"] = allRequired;
    schema["additionalProperties"] = false;
    return schema;
}

QString Tool::modelDescription() const
{
    QString description = m_description;
    for (const QJsonObject& example : m_examples) {
        if (!description.isEmpty())
            description += '\n';
        description += QStringLiteral("Example input: ") + jsonText(example);
    }
    return description;
}

QJsonObject Tool::toApiObject() const
{
    QJsonObject obj;
    obj["name"] = m_name;
    obj["description"] = modelDescription();
    if (m_strict) {
        obj["strict"] = true;
        obj["input_schema"] = strictSchema(buildParameterSchema());
    } else {
        obj["input_schema"] = buildParameterSchema();
    }

    return obj;
}

QJsonObject Tool::toOpenAiApiObject() const
{
    QJsonObject function;
    function["name"]        = m_name;
    function["description"] = modelDescription();
    function["parameters"]  = buildParameterSchema();

    QJsonObject obj;
    obj["type"]     = QStringLiteral("function");
    obj["function"] = function;

    return obj;
}

} // namespace QtLLM
