#pragma once
#include "QtLLM_base.h"

// Scripting is built on the ScriptHost library, an optional dependency
// (CMake option QTLLM_SCRIPTING, see dependencies/ScriptHost.cmake). Without
// it this header declares nothing and QTLLM_HAS_SCRIPTING is 0:
//
//   #if QTLLM_HAS_SCRIPTING
//       auto* scripting = new QtLLM::Scripting(&client, this);
//   #endif
#if SH_LIBRARY_AVAILABLE == 1
#define QTLLM_HAS_SCRIPTING 1

#include "Agent.h"
#include "ScriptApprovalDialog.h"
#include "ScriptEditorDialog.h"
#include "ScriptEngine.h"
#include "ScriptLibrary.h"
#include "ScriptMethod.h"
#include "ScriptsWidget.h"
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>

namespace QtLLM
{

class Client;

// The classes of ScriptHost under the names an application of this library
// expects them.
using ScriptMethod             = ScriptHost::ScriptMethod;
using ScriptError              = ScriptHost::ScriptError;
using ScriptCompletion         = ScriptHost::ScriptCompletion;
using ScriptMethodHandler      = ScriptHost::ScriptMethodHandler;
using AsyncScriptMethodHandler = ScriptHost::AsyncScriptMethodHandler;
using ScriptLibrary            = ScriptHost::ScriptLibrary;
using ScriptsWidget            = ScriptHost::ScriptsWidget;
using ScriptEditorDialog       = ScriptHost::ScriptEditorDialog;
// Installs the approval dialog for the model's scripts over `parent`.
// Scripting installs it without a parent; call it again to give it one.
using ScriptHost::useApprovalDialog;

// Lets the model run JavaScript inside the application.
//
// What for: "do this for every object". Without scripts the model makes one
// tool call per object and every call, with its input and its result, passes
// through the conversation. With a script it writes the loop once; the calls
// happen here and only what the script returns goes back.
//
// The engine is ScriptHost's: everything about methods, checks, limits,
// approval, saved scripts and the editor is ScriptHost::ScriptEngine, which
// this class is. What it adds is the model's side:
//   - the tools run_script and script_api, and with a library list_scripts,
//     read_script and save_script
//   - tools of the client as script methods (exposeTool)
//   - llm.ask / llm.choose: a question to a model from inside a script
//   - a script of the model ends when its turn is cancelled - unless the
//     model started it in the background (run_script with background: true):
//     that one goes on, under its run_id, until it ends, reaches the
//     engine's Limits or the model ends it; script_status and stop_script
//     are the model's tools for it
//
//   auto* scripting = new QtLLM::Scripting(&client, this);
//   QtLLM::ScriptMethod read("db.readObject");
//   read.setDescription("Reads one object.").addParameter("id", "string", "Object id")
//       .setReturns("object", "The object", exampleObject).setReadOnly(true);
//   scripting->registerMethod(read, [this](const QJsonObject& args) -> QJsonValue {
//       return m_db.object(args["id"].toString());
//   });
//
// setEnabled(false) takes the tools from the model; the application and the
// user can still run scripts.
class QT_LLM_API Scripting : public ScriptHost::ScriptEngine
{
    Q_OBJECT
public:
    explicit Scripting(Client* client, QObject* parent = nullptr);
    ~Scripting() override;

    Client* client() const;

    // ---- Tools as methods -------------------------------------------------
    // Tools of the client as methods: tools.<name>({...}). Nothing is exposed
    // until the application says so. By name, so it may be said before the
    // tool is registered. A '-' in a tool name becomes '_'. The scripting
    // tools themselves, the agent tools and find_tools / call_tool are never
    // exposed. The consent handler of the client is asked once per tool and
    // script, not once per call.
    void exposeTool(const QString& toolName, bool expose = true);
    void exposeToolGroup(const QString& group, bool expose = true);
    void exposeAllTools(bool expose = true);

    // ---- Asking a model from inside a script -------------------------------
    // Gives scripts two more methods:
    //   llm.ask(prompt, data)              the model's answer as text
    //   llm.choose(prompt, options, data)  exactly one of the options
    // for the decisions a loop cannot make by itself ("is this description
    // outdated?"). Every call is a conversation of its own: the model sees the
    // prompt and the data, nothing of the chat, and has no tools - so what is
    // in the data cannot make it do anything. The provider returns the
    // configuration of the helper (endpoint, key, model): typically a small,
    // fast model. nullptr (default): the methods do not exist.
    // The calls cost tokens; they run in a dry run as well, since the script's
    // decisions depend on them.
    void setLlmConfigProvider(std::function<AgentConfig()> provider);
    bool hasLlm() const { return bool(m_llmConfig); }
    // How often a script may call each of the two; the run is stopped at the
    // next call. Default 200.
    void setLlmCallLimit(int calls);
    int  llmCallLimit() const { return m_llmCallLimit; }

    // ---- How the model gets the definition of the methods -------------------
    // OnDemand (default): through the tool script_api, costing nothing until
    // a script is written. InToolDescription: as part of run_script's
    // description - always there, but the tool list changes whenever the
    // methods do, which invalidates the provider's prompt cache.
    enum class ApiDelivery { OnDemand, InToolDescription };
    void setApiDelivery(ApiDelivery delivery);
    ApiDelivery apiDelivery() const { return m_apiDelivery; }

    // With a library (setLibrary) the model gets list_scripts, read_script
    // and, unless this is switched off, save_script. Default true.
    void setModelMaySaveScripts(bool allowed);
    bool modelMaySaveScripts() const { return m_modelMaySave; }

private:
    void syncTools();
    void syncExposedTools();
    bool isExposed(const QString& toolName, const QString& group) const;
    void registerLlmMethods();
    void askLlm(const QJsonObject& args, bool choose, ScriptCompletion done);
    QJsonObject runStatus(RunId id, bool details) const;

    QPointer<Client>     m_client;
    QStringList          m_registeredTools;     // names of the tools this object registered
    QString              m_toolsState;          // what they were registered for
    bool                 m_syncing = false;

    QSet<QString>        m_exposedGroups;
    QHash<QString, bool> m_exposeOverride;      // tool name -> explicitly (not) exposed
    bool                 m_exposeAll = false;
    QStringList          m_exposedMethods;      // the methods made from tools
    QString              m_exposedState;
    QSet<QString>        m_consentAsked, m_consentDeclined;     // per run

    std::function<AgentConfig()> m_llmConfig;
    int                  m_llmCallLimit = 200;

    ApiDelivery          m_apiDelivery = ApiDelivery::OnDemand;
    bool                 m_modelMaySave = true;

    // The model's runs. requestRun() does not say which run it started; the
    // one run_script waits for is m_pending until runStarted names it.
    struct PendingRun;
    struct LiveRun
    {
        QStringList log;                        // the latest lines
        int         logLines = 0;               // all of them
        QJsonObject progress;
    };
    QSharedPointer<PendingRun> m_pending;
    QList<RunId>         m_modelRuns;           // started through run_script, oldest first
    QHash<RunId, LiveRun> m_live;               // those still running
    QHash<RunId, std::function<void(const QString&)>> m_waited;  // run_script waits: its status line
};

} // namespace QtLLM

#else
#define QTLLM_HAS_SCRIPTING 0
#endif
