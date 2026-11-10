#include "Scripting.h"
#if SH_LIBRARY_AVAILABLE == 1

#include "Client.h"
#include "ToolResult.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <utility>

namespace QtLLM
{

namespace
{

const char* const kGroup = "Scripting";       // group of the model's scripting tools
const char* const kRunScript = "run_script";
const char* const kScriptApi = "script_api";
const char* const kListScripts = "list_scripts";
const char* const kReadScript = "read_script";
const char* const kSaveScript = "save_script";

QString compactJson(const QJsonValue& value)
{
    // QJsonDocument cannot hold a scalar in Qt 5, so serialise via an array.
    const QByteArray json = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}

QJsonArray parametersToJson(const QList<ScriptLibrary::Parameter>& parameters)
{
    QJsonArray array;
    for (const ScriptLibrary::Parameter& p : parameters)
        array.append(QJsonObject{{"name", p.name}, {"type", p.type}, {"description", p.description}});
    return array;
}

} // namespace

// ------------------------------------------------------------ construction

Scripting::Scripting(Client* client, QObject* parent)
    : ScriptHost::ScriptEngine(parent)
    , m_client(client)
{
    setRequesterName(QStringLiteral("the assistant"));

    // The engine's switches decide which tools the model has.
    connect(this, &ScriptEngine::enabledChanged, this, [this]() { syncTools(); });
    connect(this, &ScriptEngine::settingsChanged, this, [this]() { syncTools(); });
    connect(this, &ScriptEngine::methodsChanged, this, [this]() {
        if (m_apiDelivery == ApiDelivery::InToolDescription)
            syncTools();
    });

    // One script, one question per tool - the answers hold for that run.
    connect(this, &ScriptEngine::runStarted, this, [this]() {
        m_consentAsked.clear();
        m_consentDeclined.clear();
    });
    connect(this, &ScriptEngine::runProgress, this,
            [this](double doneCount, double total, const QString& text) {
        if (!m_runProgress || !isExternalRun())
            return;
        QString status = QStringLiteral("Script: %1 / %2").arg(doneCount).arg(total);
        if (!text.isEmpty())
            status += QStringLiteral(" - ") + text;
        m_runProgress(status);
    });
    connect(this, &ScriptEngine::runFinished, this, [this]() {
        m_runProgress = nullptr;
        if (m_llmAgent)
            m_llmAgent->deleteLater();
    });

    if (m_client) {
        // Exposed tools are methods: when the tools change, so do they.
        connect(m_client, &Client::toolsChanged, this, [this]() {
            if (!m_syncing)
                syncExposedTools();
        });
        // The turn ended while the model's script was still running: it was
        // cancelled, and nobody is waiting for the result.
        connect(m_client, &Client::requestFinished, this, [this]() {
            if (isExternalRun())
                stop();
        });
    }
    syncTools();
}

Scripting::~Scripting()
{
    if (m_client) {
        for (const QString& name : std::as_const(m_registeredTools))
            m_client->unregisterTool(name);
    }
}

Client* Scripting::client() const
{
    return m_client;
}

void Scripting::setApiDelivery(ApiDelivery delivery)
{
    if (m_apiDelivery == delivery)
        return;
    m_apiDelivery = delivery;
    syncTools();
}

void Scripting::setModelMaySaveScripts(bool allowed)
{
    if (m_modelMaySave == allowed)
        return;
    m_modelMaySave = allowed;
    syncTools();
}

// ------------------------------------------------------------ tools as methods

void Scripting::exposeTool(const QString& toolName, bool expose)
{
    m_exposeOverride.insert(toolName, expose);
    syncExposedTools();
}

void Scripting::exposeToolGroup(const QString& group, bool expose)
{
    if (expose)
        m_exposedGroups.insert(group);
    else
        m_exposedGroups.remove(group);
    syncExposedTools();
}

void Scripting::exposeAllTools(bool expose)
{
    m_exposeAll = expose;
    syncExposedTools();
}

bool Scripting::isExposed(const QString& toolName, const QString& group) const
{
    const auto it = m_exposeOverride.constFind(toolName);
    if (it != m_exposeOverride.cend())
        return it.value();
    return m_exposeAll || m_exposedGroups.contains(group);
}

void Scripting::syncExposedTools()
{
    if (!m_client)
        return;

    // A script running scripts, starting agents or going through the tool
    // search has no use and plenty of ways to go wrong.
    static const QSet<QString> never{
        QStringLiteral("run_subagent"), QStringLiteral("list_agents"), QStringLiteral("message_agent"),
        QStringLiteral("find_tools"), QStringLiteral("call_tool")};

    QList<Tool> wanted;
    QString state;
    if (m_exposeAll || !m_exposedGroups.isEmpty() || !m_exposeOverride.isEmpty()) {
        const QList<Tool> tools = m_client->registeredTools();
        for (const Tool& tool : tools) {
            const QString name = tool.name();
            if (never.contains(name) || tool.group() == QLatin1String(kGroup)
                || !m_client->isToolEnabled(name) || !isExposed(name, tool.group()))
                continue;
            wanted.append(tool);
            state += name + QLatin1Char('\x1f') + tool.group() + QLatin1Char('\x1f') + tool.description()
                     + QLatin1Char('\x1f') + compactJson(tool.parameterSchema())
                     + (tool.isReadOnly() ? QStringLiteral("\x1fro\n") : QStringLiteral("\x1frw\n"));
        }
    }
    if (state == m_exposedState)
        return;     // the same tools as before: nothing to do
    m_exposedState = state;

    // Registering fires methodsChanged each time; the model's tools are
    // brought up to date once, at the end.
    const bool wasSyncing = m_syncing;
    m_syncing = true;
    QStringList names;
    const QPointer<Scripting> self(this);
    for (const Tool& tool : std::as_const(wanted)) {
        // Tool names may hold '-', which a JavaScript name cannot.
        QString function = tool.name();
        function.replace(QLatin1Char('-'), QLatin1Char('_'));
        const QString methodName = QStringLiteral("tools.") + function;
        // A method the application registered under that name is the application's.
        if (hasMethod(methodName) && !m_exposedMethods.contains(methodName))
            continue;

        ScriptMethod method = ScriptMethod::fromSchema(methodName, tool.parameterSchema());
        method.setDescription(tool.description())
              .setGroup(tool.group().isEmpty() ? QStringLiteral("Tools") : tool.group())
              .setReadOnly(tool.isReadOnly())
              .setReturns(QStringLiteral("object"), QStringLiteral("The tool's result"));
        const QString toolName = tool.name();
        registerAsyncMethod(method, [self, toolName](const QJsonObject& args, ScriptCompletion done) {
            if (!self || !self->m_client) {
                done.fail(QStringLiteral("the tool is gone"));
                return;
            }
            if (self->m_consentDeclined.contains(toolName)) {
                done.fail(QStringLiteral("user declined"));
                return;
            }
            // A script makes its calls on its own account: no chat card per
            // call, no turn limit, and the whole result - a truncated one
            // would be broken JSON to the script.
            ToolCallOptions options;
            options.announce = false;
            options.countInTurn = false;
            options.capResult = false;
            options.askConsent = !self->m_consentAsked.contains(toolName);
            self->m_consentAsked.insert(toolName);
            self->m_client->callTool(toolName, args, [self, done, toolName](const QJsonObject& toolResult) {
                QJsonObject result = toolResult;
                toolTakeImages(result);     // a script cannot look at pictures
                if (result.value("status").toString() == QLatin1String("error")) {
                    const QString message = result.value("message").toString();
                    if (self && message == QLatin1String("user declined"))
                        self->m_consentDeclined.insert(toolName);
                    done.fail(message);
                    return;
                }
                done.finish(result);
            }, options);
        });
        names << methodName;
    }
    for (const QString& old : std::as_const(m_exposedMethods)) {
        if (!names.contains(old))
            unregisterMethod(old);
    }
    m_exposedMethods = names;
    m_syncing = wasSyncing;
    if (m_apiDelivery == ApiDelivery::InToolDescription)
        syncTools();
}

// ------------------------------------------------------------ llm.ask / llm.choose

void Scripting::setLlmConfigProvider(std::function<AgentConfig()> provider)
{
    m_llmConfig = std::move(provider);
    if (!m_llmConfig) {
        unregisterMethod(QStringLiteral("llm.ask"));
        unregisterMethod(QStringLiteral("llm.choose"));
        return;
    }
    if (!hasMethod(QStringLiteral("llm.ask")))
        registerLlmMethods();
}

void Scripting::setLlmCallLimit(int calls)
{
    m_llmCallLimit = calls;
    if (m_llmConfig)
        registerLlmMethods();
}

void Scripting::registerLlmMethods()
{
    const QString group = QStringLiteral("Language model");
    const QJsonObject anything{{"description", "What the question is about: an object, a list, a text"}};
    const QPointer<Scripting> self(this);

    ScriptMethod ask(QStringLiteral("llm.ask"));
    ask.setDescription(QStringLiteral(
            "Asks a language model and returns its answer as text. Each call starts fresh: the "
            "model sees this prompt and the data, nothing else. Say in the prompt what form the "
            "answer should have. Costs time and tokens - not for what plain code can decide."))
       .setGroup(group).setReadOnly(true).setMaxCallsPerRun(m_llmCallLimit)
       .addParameter(QStringLiteral("prompt"), QStringLiteral("string"), QStringLiteral("The question or instruction"))
       .addRawParameter(QStringLiteral("data"), anything, false)
       .setReturns(QStringLiteral("string"), QStringLiteral("The answer"), QStringLiteral("yes"));
    registerAsyncMethod(ask, [self](const QJsonObject& args, ScriptCompletion done) {
        if (self)
            self->askLlm(args, false, done);
    });

    ScriptMethod choose(QStringLiteral("llm.choose"));
    choose.setDescription(QStringLiteral(
            "Like llm.ask, but the answer is exactly one of the options - for decisions the "
            "script branches on."))
          .setGroup(group).setReadOnly(true).setMaxCallsPerRun(m_llmCallLimit)
          .addParameter(QStringLiteral("prompt"), QStringLiteral("string"), QStringLiteral("The question"))
          .addRawParameter(QStringLiteral("options"),
                           QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "string"}}},
                                       {"description", "The possible answers"}})
          .addRawParameter(QStringLiteral("data"), anything, false)
          .setReturns(QStringLiteral("string"), QStringLiteral("One of the options"), QStringLiteral("yes"));
    registerAsyncMethod(choose, [self](const QJsonObject& args, ScriptCompletion done) {
        if (self)
            self->askLlm(args, true, done);
    });
}

void Scripting::askLlm(const QJsonObject& args, bool choose, ScriptCompletion done)
{
    if (!m_llmConfig) {
        done.fail(QStringLiteral("no language model is configured for scripts"));
        return;
    }
    QStringList options;
    for (const QJsonValue& v : args.value("options").toArray())
        options << v.toString();
    if (choose && options.size() < 2) {
        done.fail(QStringLiteral("give at least two options"));
        return;
    }

    // One helper per run, with an empty conversation for every question.
    if (!m_llmAgent) {
        AgentConfig config = m_llmConfig();
        if (config.name.isEmpty())
            config.name = QStringLiteral("script-llm");
        if (config.systemPrompt.isEmpty()) {
            config.systemPrompt = QStringLiteral(
                "A program asks you a question and uses your reply as it is. Reply with the "
                "answer only: no introduction, no explanation unless the question asks for one.");
        }
        m_llmAgent = new Agent(config, this);
        if (m_client)
            m_llmAgent->setModelFilter(m_client->allowedModels(), m_client->blockedModels());
    }
    m_llmAgent->clearConversation();

    QString prompt = args.value("prompt").toString();
    if (choose) {
        prompt += QStringLiteral("\n\nAnswer with exactly one of these, and nothing else:");
        for (const QString& option : std::as_const(options))
            prompt += QStringLiteral("\n- ") + option;
    }
    const QJsonValue data = args.value("data");
    if (!data.isUndefined() && !data.isNull()) {
        prompt += QStringLiteral("\n\nData:\n")
                  + (data.isString() ? data.toString() : compactJson(data));
    }

    m_llmAgent->ask(prompt, [done, options, choose](const QString& reply, bool ok) {
        if (!ok) {
            done.fail(reply);
            return;
        }
        const QString answer = reply.trimmed();
        if (!choose) {
            done.finish(answer);
            return;
        }
        // The option itself, or - for a model that wraps it in a sentence -
        // the one option the answer names.
        QStringList named;
        for (const QString& option : options) {
            if (answer.compare(option, Qt::CaseInsensitive) == 0) {
                done.finish(option);
                return;
            }
            if (answer.contains(option, Qt::CaseInsensitive))
                named << option;
        }
        if (named.size() == 1)
            done.finish(named.first());
        else
            done.fail(QStringLiteral("the model answered \"%1\", which is not one of the options").arg(answer.left(120)));
    });
}

// ------------------------------------------------------------ the model's tools

void Scripting::syncTools()
{
    if (!m_client || m_syncing)
        return;

    const bool withLibrary = library() != nullptr;
    QString description = QStringLiteral(
        "Run a JavaScript program inside the application. Use it when a task needs many "
        "calls or a loop - \"do this for every object\" - instead of making the calls one by "
        "one: the script calls the application's methods directly, and only what it returns "
        "comes back to you. Rules: the code is the body of a function - `return` hands back "
        "the result (keep it small: counts, a short list, not all the data); `args` holds the "
        "arguments you pass; method calls are synchronous and a failing one throws; the "
        "language is ECMAScript 2016 - no async/await, no optional chaining (?.), no ??, no "
        "object spread. The script is checked before it runs; a mistake comes back with its "
        "line. With dry_run, methods that change something are only recorded and you get "
        "back what the script would do.");
    if (m_apiDelivery == ApiDelivery::InToolDescription)
        description += QStringLiteral("\n\nThe methods a script can call:\n") + apiDefinition();
    else
        description += QStringLiteral(" Get the methods and their signatures from script_api first.");

    // Registered again only when what it depends on has changed: the tool
    // list is part of every request, and the provider's cache hangs on it.
    const QString state = QStringLiteral("%1|%2|%3|%4|%5|").arg(isEnabled()).arg(withLibrary)
                              .arg(m_modelMaySave).arg(int(m_apiDelivery)).arg(limits().timeoutMs)
                          + description;
    if (state == m_toolsState)
        return;
    m_toolsState = state;

    m_syncing = true;
    for (const QString& name : std::as_const(m_registeredTools))
        m_client->unregisterTool(name);
    m_registeredTools.clear();

    if (isEnabled()) {
        const QPointer<Scripting> self(this);
        const QString group = QLatin1String(kGroup);

        // ---- run_script
        Tool runTool;
        runTool.setName(QLatin1String(kRunScript))
               .setTitle(QStringLiteral("Run script"))
               .setGroup(group)
               .setStatusText(QString::fromUtf8("Running a script…"))
               .setDescription(description)
               // The script's own limit ends it; this one is for a script
               // that waits for the user's approval.
               .setTimeoutMs(limits().timeoutMs + 600000)
               .addParameter(QStringLiteral("code"), QStringLiteral("string"),
                             QStringLiteral("The script: the body of a function."), !withLibrary);
        if (withLibrary) {
            runTool.addParameter(QStringLiteral("name"), QStringLiteral("string"),
                                 QStringLiteral("Instead of code: the name of a saved script (list_scripts)."));
        }
        runTool.addParameter(QStringLiteral("args"), QStringLiteral("object"),
                             QStringLiteral("Becomes `args` in the script."))
               .addParameter(QStringLiteral("dry_run"), QStringLiteral("boolean"),
                             QStringLiteral("Only record the calls that would change something."));
        m_client->registerAsyncTool(runTool, [self](const QJsonObject& input, ToolCompletion done) {
            if (!self) {
                done.finish(toolError(QStringLiteral("scripting is gone")));
                return;
            }
            Request request;
            request.code = input.value("code").toString();
            request.name = input.value("name").toString().trimmed();
            request.args = input.value("args").toObject();
            request.dryRun = input.value("dry_run").toBool();
            if (request.code.trimmed().isEmpty()) {
                if (request.name.isEmpty()) {
                    done.finish(toolError(QStringLiteral("pass the script as `code`")));
                    return;
                }
                if (!self->library()) {
                    done.finish(toolError(QStringLiteral("there are no saved scripts; pass `code`")));
                    return;
                }
                if (!self->library()->contains(request.name)) {
                    done.finish(toolError(QStringLiteral("no script '%1'; list_scripts shows what there is")
                                              .arg(request.name)));
                    return;
                }
            } else {
                request.name.clear();
            }
            self->m_runProgress = [done](const QString& text) { done.progress(text); };
            self->requestRun(request,
                             [done](const QJsonObject& result) { done.finish(result); },
                             [done]() { return done.isFinished(); });
        });
        m_registeredTools << runTool.name();

        // ---- script_api
        if (m_apiDelivery == ApiDelivery::OnDemand) {
            Tool apiTool;
            apiTool.setName(QLatin1String(kScriptApi))
                   .setTitle(QStringLiteral("Script interface"))
                   .setGroup(group)
                   .setReadOnly(true)
                   .setDescription(QStringLiteral(
                       "The methods a script (run_script) can call, as TypeScript declarations with "
                       "what each does, takes and returns. Without a query: all of them, or an index "
                       "when there are many. With a query (a method name, a namespace, keywords): "
                       "the matching ones."))
                   .addParameter(QStringLiteral("query"), QStringLiteral("string"),
                                 QStringLiteral("What to look for; leave out for everything."));
            m_client->registerTool(apiTool, [self](const QJsonObject& input) -> QJsonObject {
                if (!self)
                    return toolError(QStringLiteral("scripting is gone"));
                const QString definition = self->apiDefinition(input.value("query").toString());
                QJsonObject result{{"definition", definition}};
                // Too many methods for one answer: the engine sent an index.
                if (!definition.contains(QLatin1String("declare ")))
                    result["hint"] = "call script_api again with a query (a method name, a namespace, "
                                     "keywords) to get the declarations";
                return toolOk(result);
            });
            m_registeredTools << apiTool.name();
        }

        // ---- saved scripts
        if (withLibrary) {
            Tool listTool;
            listTool.setName(QLatin1String(kListScripts))
                    .setTitle(QStringLiteral("List scripts"))
                    .setGroup(group)
                    .setReadOnly(true)
                    .setDescription(QStringLiteral(
                        "The saved scripts: name, what each does and the arguments it expects. Run "
                        "one with run_script(name, args) instead of writing it again."));
            m_client->registerTool(listTool, [self](const QJsonObject&) -> QJsonObject {
                if (!self || !self->library())
                    return toolError(QStringLiteral("there is no script library"));
                QJsonArray list;
                const QList<ScriptLibrary::Script> scripts = self->library()->scripts();
                for (const ScriptLibrary::Script& script : scripts) {
                    list.append(QJsonObject{{"name", script.name},
                                            {"description", script.description},
                                            {"parameters", parametersToJson(script.parameters)}});
                }
                return toolOk({{"scripts", list}});
            });
            m_registeredTools << listTool.name();

            Tool readTool;
            readTool.setName(QLatin1String(kReadScript))
                    .setTitle(QStringLiteral("Read script"))
                    .setGroup(group)
                    .setReadOnly(true)
                    .setDescription(QStringLiteral("The code of a saved script, to see how it works or to adapt it."))
                    .addParameter(QStringLiteral("name"), QStringLiteral("string"),
                                  QStringLiteral("Name as list_scripts reports it."), true);
            m_client->registerTool(readTool, [self](const QJsonObject& input) -> QJsonObject {
                if (!self || !self->library())
                    return toolError(QStringLiteral("there is no script library"));
                const ScriptLibrary::Script script = self->library()->script(input.value("name").toString());
                if (!script.isValid())
                    return toolError(QStringLiteral("no script '%1'; list_scripts shows what there is")
                                         .arg(input.value("name").toString()));
                return toolOk({{"name", script.name},
                               {"description", script.description},
                               {"parameters", parametersToJson(script.parameters)},
                               {"code", script.code}});
            });
            m_registeredTools << readTool.name();

            if (m_modelMaySave) {
                Tool parameter;
                parameter.addParameter(QStringLiteral("name"), QStringLiteral("string"),
                                       QStringLiteral("Key in `args`"), true)
                         .addParameter(QStringLiteral("type"), QStringLiteral("string"),
                                       QStringLiteral("string, number, boolean, array or object"))
                         .addParameter(QStringLiteral("description"), QStringLiteral("string"),
                                       QStringLiteral("What it is for"));
                Tool saveTool;
                saveTool.setName(QLatin1String(kSaveScript))
                        .setTitle(QStringLiteral("Save script"))
                        .setGroup(group)
                        .setDescription(QStringLiteral(
                            "Save a script for later use, by you or by the user. Worth it for a script "
                            "that worked and will be needed again; make what varies an argument "
                            "(`args`). The script is checked first and not saved if it has a mistake. "
                            "Saving under the name of a script you saved before replaces it."))
                        .addParameter(QStringLiteral("name"), QStringLiteral("string"),
                                      QStringLiteral("Short name, e.g. \"Archive outdated objects\"."), true)
                        .addParameter(QStringLiteral("description"), QStringLiteral("string"),
                                      QStringLiteral("One sentence: what it does."), true)
                        .addParameter(QStringLiteral("code"), QStringLiteral("string"),
                                      QStringLiteral("The script: the body of a function."), true)
                        .addArrayParameter(QStringLiteral("parameters"), parameter.parameterSchema(),
                                           QStringLiteral("The arguments the script expects in `args`."));
                m_client->registerTool(saveTool, [self](const QJsonObject& input) -> QJsonObject {
                    if (!self || !self->library())
                        return toolError(QStringLiteral("there is no script library"));
                    ScriptLibrary::Script script;
                    script.name = input.value("name").toString().simplified();
                    script.description = input.value("description").toString();
                    script.code = input.value("code").toString();
                    script.origin = QStringLiteral("assistant");
                    for (const QJsonValue& v : input.value("parameters").toArray()) {
                        const QJsonObject p = v.toObject();
                        script.parameters.append(ScriptLibrary::Parameter{
                            p.value("name").toString(), p.value("type").toString(),
                            p.value("description").toString()});
                    }
                    const Check checked = self->check(script.code);
                    if (!checked.ok)
                        return checked.toJson();
                    // The model replaces its own scripts, not the user's or the application's.
                    const ScriptLibrary::Script existing = self->library()->script(script.name);
                    if (existing.isValid() && (existing.readOnly || existing.origin != QLatin1String("assistant")))
                        return toolError(QStringLiteral("a script named '%1' exists and is not yours to replace; "
                                                        "choose another name").arg(existing.name));
                    QString problem;
                    if (!self->library()->save(script, &problem))
                        return toolError(problem);
                    return toolOk({{"saved", script.name}});
                });
                m_registeredTools << saveTool.name();
            }
        }
    }
    m_syncing = false;
}

} // namespace QtLLM

#endif // SH_LIBRARY_AVAILABLE
