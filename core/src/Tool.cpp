#include "Tool.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>

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
    m_parameters.append(Parameter{name, type, description, required, enumValues});
    return *this;
}

Tool& Tool::addEnumParameter(const QString& name,
                             const QStringList& allowedValues,
                             const QString& description,
                             bool required)
{
    return addParameter(name, QStringLiteral("string"), description, required, allowedValues);
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

QJsonObject Tool::buildParameterSchema() const
{
    QJsonObject properties;
    QJsonArray requiredArray;

    for (const Parameter& param : m_parameters) {
        QJsonObject paramObj;
        paramObj["type"] = param.type;
        paramObj["description"] = param.description;
        if (!param.enumValues.isEmpty())
            paramObj["enum"] = QJsonArray::fromStringList(param.enumValues);
        properties[param.name] = paramObj;

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
    if (type == QLatin1String("number"))  return value.isDouble();
    if (type == QLatin1String("integer")) {
        if (!value.isDouble())
            return false;
        const double d = value.toDouble();
        return d == static_cast<double>(static_cast<qint64>(d));
    }
    return true;
}

QJsonObject Tool::validateAgainstSchema(const QJsonObject& parameterSchema,
                                        const QJsonObject& args)
{
    const QJsonObject properties = parameterSchema["properties"].toObject();

    // "expected" map (name -> type) reused by all error results so the model
    // can self-correct on the next attempt.
    QJsonObject expected;
    for (auto it = properties.begin(); it != properties.end(); ++it)
        expected[it.key()] = it.value().toObject()["type"].toString();

    for (const QJsonValue& req : parameterSchema["required"].toArray()) {
        const QString name = req.toString();
        if (!args.contains(name)) {
            return QJsonObject{
                {"status",   "error"},
                {"message",  QString("missing required parameter: %1").arg(name)},
                {"expected", expected}};
        }
    }

    for (auto it = properties.begin(); it != properties.end(); ++it) {
        const QString name = it.key();
        if (!args.contains(name))
            continue;

        const QJsonObject propSchema = it.value().toObject();
        const QJsonValue value = args[name];

        const QString type = propSchema["type"].toString();
        if (!type.isEmpty() && !matchesJsonType(type, value)) {
            return QJsonObject{
                {"status",   "error"},
                {"message",  QString("invalid type for '%1': expected %2").arg(name, type)},
                {"expected", expected}};
        }

        const QJsonArray enumArray = propSchema["enum"].toArray();
        if (!enumArray.isEmpty() && value.isString()) {
            // Case-insensitive — models often return values in a different case
            bool found = false;
            for (const QJsonValue& allowed : enumArray) {
                if (allowed.toString().compare(value.toString(), Qt::CaseInsensitive) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return QJsonObject{
                    {"status",         "error"},
                    {"message",        QString("invalid value for '%1'").arg(name)},
                    {"allowed_values", enumArray}};
            }
        }
    }

    return QJsonObject();
}

QJsonObject Tool::toApiObject() const
{
    QJsonObject obj;
    obj["name"] = m_name;
    obj["description"] = m_description;
    obj["input_schema"] = buildParameterSchema();

    return obj;
}

QJsonObject Tool::toOpenAiApiObject() const
{
    QJsonObject function;
    function["name"]        = m_name;
    function["description"] = m_description;
    function["parameters"]  = buildParameterSchema();

    QJsonObject obj;
    obj["type"]     = QStringLiteral("function");
    obj["function"] = function;

    return obj;
}

} // namespace QtLLM
