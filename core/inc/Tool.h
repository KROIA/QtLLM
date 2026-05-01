#pragma once
#include "QtLLM_base.h"
#include <QString>
#include <QList>
#include <QJsonObject>
#include <QJsonArray>

namespace QtLLM
{

class QT_LLM_API Tool
{
public:
    Tool();

    // Builder setters — return *this for chaining
    Tool& setName(const QString& name);
    Tool& setDescription(const QString& description);

    // type: JSON Schema primitive — "string", "integer", "boolean", "number", "array", "object"
    Tool& addParameter(const QString& name,
                       const QString& type,
                       const QString& description,
                       bool required = false);

    QString name() const;
    QString description() const;

    // Produces the object placed in the Claude API "tools" array; uses "input_schema" key (Claude format).
    QJsonObject toApiObject() const;

    // Produces the tool object for OpenAI-compatible APIs (Ollama, etc.); wraps parameters under "parameters" key inside a "function" object.
    QJsonObject toOpenAiApiObject() const;

private:
    struct Parameter {
        QString name;
        QString type;
        QString description;
        bool    required;
    };

    QString          m_name;
    QString          m_description;
    QList<Parameter> m_parameters;
};

} // namespace QtLLM
