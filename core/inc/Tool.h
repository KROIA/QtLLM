#pragma once
#include "QtLLM_base.h"
#include <QString>
#include <QStringList>
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

    // UI-only metadata. Never sent to the model — carrier fields for tool
    // settings dialogs, grouping, and progress display in the host app.
    Tool& setTitle(const QString& title);          // display name
    Tool& setGroup(const QString& group);          // grouping for UI lists
    Tool& setStatusText(const QString& statusText); // shown while executing

    // type: JSON Schema primitive — "string", "integer", "boolean", "number", "array", "object"
    // enumValues non-empty adds "enum": [...] to the parameter schema.
    Tool& addParameter(const QString& name,
                       const QString& type,
                       const QString& description,
                       bool required = false,
                       const QStringList& enumValues = {});

    // Convenience: string parameter restricted to a fixed set of values.
    Tool& addEnumParameter(const QString& name,
                           const QStringList& allowedValues,
                           const QString& description,
                           bool required = false);

    QString name() const;
    QString description() const;
    QString title() const;
    QString group() const;
    QString statusText() const;

    // Validate args against this tool's parameter definitions.
    // Returns an empty object when valid, otherwise a ready-to-send error
    // result: {"status":"error","message":...,"expected":{...}} for missing/
    // mistyped parameters or {"status":"error","message":...,
    // "allowed_values":[...]} for enum violations (compared case-insensitively).
    QJsonObject validate(const QJsonObject& args) const;

    // Same validation driven by a raw JSON-Schema object (the format produced
    // by parameterSchema()/registerTool raw overload: {"type":"object",
    // "properties":{...},"required":[...]}). Used by Client for tools that
    // were registered with a hand-built schema.
    static QJsonObject validateAgainstSchema(const QJsonObject& parameterSchema,
                                             const QJsonObject& args);

    // Produces the object placed in the Claude API "tools" array; uses "input_schema" key (Claude format).
    QJsonObject toApiObject() const;

    // Produces the tool object for OpenAI-compatible APIs (Ollama, etc.); wraps parameters under "parameters" key inside a "function" object.
    QJsonObject toOpenAiApiObject() const;

private:
    struct Parameter {
        QString     name;
        QString     type;
        QString     description;
        bool        required;
        QStringList enumValues;
    };

    QJsonObject buildParameterSchema() const;

    QString          m_name;
    QString          m_description;
    QString          m_title;
    QString          m_group;
    QString          m_statusText;
    QList<Parameter> m_parameters;
};

} // namespace QtLLM
