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

    // Array parameter whose elements are of itemType / match itemSchema.
    Tool& addArrayParameter(const QString& name,
                            const QString& itemType,
                            const QString& description,
                            bool required = false);
    Tool& addArrayParameter(const QString& name,
                            const QJsonObject& itemSchema,
                            const QString& description,
                            bool required = false);

    // Nested object parameter; its properties are the parameters of fields.
    Tool& addObjectParameter(const QString& name,
                             const Tool& fields,
                             const QString& description,
                             bool required = false);

    // Escape hatch: schema is used verbatim as the property schema.
    Tool& addRawParameter(const QString& name,
                          const QJsonObject& schema,
                          bool required = false);

    // Refine an already added parameter; unknown names are ignored.
    // setDefault is stated in the schema and applied by prepareInput().
    Tool& setRange(const QString& name, double minimum, double maximum);
    Tool& setDefault(const QString& name, const QJsonValue& value);

    // Appended to the description sent to the model as "Example input: {...}".
    // description() keeps returning the plain text.
    Tool& addExample(const QJsonObject& input);

    // Claude strict tool use: toApiObject() emits "strict": true, closes every
    // object schema with "additionalProperties": false and lists all properties
    // as required, optional ones becoming nullable. Constraints strict mode
    // does not accept (minimum/maximum/default) move into the description.
    // toOpenAiApiObject() is unaffected.
    Tool& setStrict(bool strict);
    bool isStrict() const { return m_strict; }

    // True when name matches ^[a-zA-Z0-9_-]{1,64}$, the rule the APIs enforce.
    static bool isValidName(const QString& name);

    // The candidate closest to name, or empty when none is near enough to be
    // a typo of it. What "did_you_mean" hints are made of.
    static QString closestName(const QString& name, const QStringList& candidates);

    // Behaviour hints, never sent to the model.
    // readOnly: the tool only inspects state. Consent dialogs can let such
    //   calls through and ask only for the ones that change something.
    // maxResultChars: per-tool override of Client::setMaxToolResultChars();
    //   -1 (default) = use the client's value, 0 = unlimited.
    // timeoutMs: how long an asynchronous handler may take before the model is
    //   told the call timed out; 0 (default) = the client's default.
    Tool& setReadOnly(bool readOnly);
    Tool& setMaxResultChars(int chars);
    Tool& setTimeoutMs(int ms);
    bool isReadOnly() const { return m_readOnly; }
    int  maxResultChars() const { return m_maxResultChars; }
    int  timeoutMs() const { return m_timeoutMs; }

    QString name() const;
    QString description() const;
    QString title() const;
    QString group() const;
    QString statusText() const;

    // Full parameter schema with ranges and defaults, independent of strict
    // mode. This is the schema to hand to prepareInput()/validateAgainstSchema().
    QJsonObject parameterSchema() const;

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

    // Returns args with every string value of an enum parameter rewritten to
    // the spelling declared in the schema ("OpenFile" -> "openFile"). Values
    // that match no entry are left alone - rejecting them is validation's job.
    static QJsonObject normalizeEnumCase(const QJsonObject& parameterSchema,
                                         const QJsonObject& args);

    // Repairs and checks a tool call's input before the handler runs, in one
    // recursive pass: "5" -> 5 / "true" -> true where the schema wants a
    // number/boolean, enum spelling normalised, null for an optional parameter
    // treated as omitted, defaults applied, then type/enum/range/required
    // checks through nested objects and array items. With rejectUnknown, a
    // parameter name the schema does not declare is an error listing the valid
    // names plus "did_you_mean" when one is close.
    // Returns true with the repaired input in args; otherwise false, args
    // untouched and *error (if given) set to a ready-to-send error result.
    static bool prepareInput(const QJsonObject& parameterSchema,
                             QJsonObject& args,
                             QJsonObject* error,
                             bool rejectUnknown = true);

    // Produces the object placed in the Claude API "tools" array; uses "input_schema" key (Claude format).
    QJsonObject toApiObject() const;

    // Produces the tool object for OpenAI-compatible APIs (Ollama, etc.); wraps parameters under "parameters" key inside a "function" object.
    QJsonObject toOpenAiApiObject() const;

private:
    struct Parameter {
        QString     name;
        bool        required;
        QJsonObject schema;
    };

    Tool& addParameterSchema(const QString& name, const QJsonObject& schema, bool required);
    Parameter* findParameter(const QString& name);
    QJsonObject buildParameterSchema() const;
    QString modelDescription() const;

    QString          m_name;
    QString          m_description;
    QString          m_title;
    QString          m_group;
    QString          m_statusText;
    QList<Parameter> m_parameters;
    QList<QJsonObject> m_examples;
    bool             m_strict = false;
    bool             m_readOnly = false;
    int              m_maxResultChars = -1;
    int              m_timeoutMs = 0;
};

} // namespace QtLLM
