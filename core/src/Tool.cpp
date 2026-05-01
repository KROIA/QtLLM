#include "Tool.h"
#include <QJsonObject>
#include <QJsonArray>

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

Tool& Tool::addParameter(const QString& name,
                         const QString& type,
                         const QString& description,
                         bool required)
{
    m_parameters.append(Parameter{name, type, description, required});
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

QJsonObject Tool::toApiObject() const
{
    QJsonObject properties;
    QJsonArray requiredArray;

    for (const Parameter& param : m_parameters) {
        QJsonObject paramObj;
        paramObj["type"] = param.type;
        paramObj["description"] = param.description;
        properties[param.name] = paramObj;

        if (param.required) {
            requiredArray.append(param.name);
        }
    }

    QJsonObject inputSchema;
    inputSchema["type"] = QStringLiteral("object");
    inputSchema["properties"] = properties;
    if (!requiredArray.isEmpty()) {
        inputSchema["required"] = requiredArray;
    }

    QJsonObject obj;
    obj["name"] = m_name;
    obj["description"] = m_description;
    obj["input_schema"] = inputSchema;

    return obj;
}

} // namespace QtLLM
