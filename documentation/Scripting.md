# Scripting

Lets the model, and the user, run small JavaScript programs inside the application.

## What it is for

"Do this for every object." Without scripts the model makes one tool call per object. Every call, with its input and its result, passes through the conversation, and for a thousand objects the model has to write a thousand tool calls.

With a script the model writes the loop once:

```js
const changed = [];
for (const id of db.listObjects()) {
    const object = db.readObject(id);
    if (object.year < args.before) {
        db.setProperty(id, "status", "archived");
        changed.push(object.name);
    }
}
return { archived: changed.length, first: changed.slice(0, 5) };
```

The calls happen inside the application. Only the script and what it returns go through the model.

## Where it comes from

The engine is a library of its own, [ScriptHost](https://github.com/KROIA/ScriptHost): methods, checks, limits, dry run, approval, saved scripts, the Scripts tab and the editor with its debugger. It knows nothing about language models and can be used without QtLLM.

`QtLLM::Scripting` is a `ScriptHost::ScriptEngine` with the model's side added: the tools `run_script` and `script_api`, tools as methods, `llm.ask`, and ending a script when its turn is cancelled. The ScriptHost classes are available under QtLLM names (`QtLLM::ScriptMethod`, `ScriptError`, `ScriptLibrary`, `ScriptsWidget`, `ScriptEditorDialog`).

## Building with or without it

ScriptHost is an optional dependency, declared in `dependencies/ScriptHost.cmake`. It uses `QJSEngine` and so brings the Qt module **Qml**.

```
cmake --preset x64-Debug -DQTLLM_SCRIPTING=OFF
```

| `QTLLM_SCRIPTING` | Effect |
|---|---|
| `ON` (default) | ScriptHost is fetched and linked; `QtLLM::Scripting` exists; `QTLLM_HAS_SCRIPTING` is 1 |
| `OFF` | Nothing is fetched, no Qml. `Scripting.h` declares nothing; `QTLLM_HAS_SCRIPTING` is 0; the Settings dialog has no Scripts tab |

Code that should build both ways guards its scripting part:

```cpp
#if QTLLM_HAS_SCRIPTING
    auto* scripting = new QtLLM::Scripting(&client, this);
#endif
```

To build against a local checkout of ScriptHost: `-DFETCHCONTENT_SOURCE_DIR_SCRIPTHOST="C:/path/to/ScriptHost"`.

## Setting it up

```cpp
auto* scripting = new QtLLM::Scripting(&client, this);

QtLLM::ScriptMethod read("db.readObject");
read.setDescription("Reads one object of the project.")
    .addParameter("id", "string", "Object id")
    .setReturns("object", "The object", QJsonObject{{"id", "a1"}, {"name", "Pump"}, {"year", 2019}})
    .setReadOnly(true)
    .setGroup("Database");
scripting->registerMethod(read, [this](const QJsonObject& args) -> QJsonValue {
    const QString id = args["id"].toString();
    if (!m_db.contains(id))
        throw QtLLM::ScriptError("no object '" + id + "'");   // the script gets an exception
    return m_db.object(id);
});
```

From then on the model has the tools `run_script` and `script_api`.

## The methods a script can call

A script can call exactly what the application registers, nothing else. The engine has no file, network or process access of its own.

### Defining a method

| Call | Meaning |
|---|---|
| `ScriptMethod("db.readObject")` | `space.name` is a function of the object `space`; a plain `name` is a global function |
| `setDescription(text)` | What it does |
| `addParameter(name, type, description, required = true, enumValues = {})` | Types: `string`, `integer`, `number`, `boolean`, `array`, `object`. Optional parameters last |
| `setRange(name, min, max)`, `setDefault(name, value)` | Checked and applied before the handler runs |
| `setReturns(type, description, example)` | The example gives the return type its shape, and is what a dry run answers |
| `setReadOnly(true)` | The method only reads: it runs for real in a dry run, and a script that calls only read-only methods counts as a reading script. Whether reading scripts need approval is set per kind (see [Approval](#approval)) |
| `setGroup(name)` | For settings lists and for switching whole groups |

In a script, arguments are positional in the order the parameters were added: `db.readObject("a1")`. A single object is accepted as named arguments: `db.setProperty({id: "a1", name: "status", value: "archived"})`.

Every call is checked against the definition first. `"5"` for a number is repaired, an enum's spelling is normalised, defaults are applied. A call that does not fit never reaches the handler; the script gets an exception that names the signature.

### Handlers

```cpp
scripting->registerMethod(method, [](const QJsonObject& args) -> QJsonValue { ... });
scripting->registerAsyncMethod(method, [](const QJsonObject& args, QtLLM::ScriptCompletion done) {
    startSomething(args, [done](const QJsonValue& result) { done.finish(result); });   // or done.fail("why")
});
```

Handlers run on the GUI thread and may touch widgets and application data. The script runs on a thread of its own and waits; the application stays responsive. To fail a call, throw `QtLLM::ScriptError` (or call `done.fail`).

### Tools as methods

A tool of the client can be made a method. None is until the application says so:

```cpp
scripting->exposeTool("ui_get_items");     // tools.ui_get_items({...})
scripting->exposeToolGroup("App UI");
scripting->exposeAllTools();
```

An exposed tool takes its input as one object and returns the tool's result; a tool error is an exception. A `-` in a tool name becomes `_`. The scripting tools, the agent tools and `find_tools` / `call_tool` are never exposed. The consent handler is asked once per tool and script, not once per call.

### Built-in methods

| Method | |
|---|---|
| `log(...values)` | Line in the script log: shown to the user, the last 20 lines go back with the result |
| `progress(done, total, text)` | Status line of the chat |
| `sleep(ms)` | Waits; the application stays responsive |
| `pause(label)` | Stops in the debugger; does nothing otherwise |
| `args` | The arguments of the run |

### Asking a model from inside a script

```cpp
scripting->setLlmConfigProvider([this]() {
    QtLLM::AgentConfig config;          // endpoint, key, model of the helper
    config.model = "claude-haiku-5-5";
    return config;
});
```

adds `llm.ask(prompt, data)` (answer as text) and `llm.choose(prompt, options, data)` (exactly one of the options). Every call is a conversation of its own: the model sees the prompt and the data, nothing of the chat, and has no tools. That is what makes it safe to hand it application data.

```js
for (const id of db.listObjects()) {
    const object = db.readObject(id);
    if (llm.choose("Is this description outdated?", ["yes", "no"], object.description) === "yes")
        db.setProperty(id, "status", "review");
}
```

Each call costs time and tokens. `setLlmCallLimit(n)` (default 200) limits how often a script may call each of the two. Calls are sequential.

### Switching methods on and off

```cpp
scripting->setMethodEnabled("db.deleteObject", false);
scripting->setMethodGroupEnabled("Database", false);          // a method's own setting wins
scripting->setMethodUserConfigurable("db.deleteObject", false);   // the user cannot switch it either
```

By name and durable, like the tool policy of `Client`: may be set before the method is registered. A disabled method is not in the definition, and a script that uses it is rejected before it runs. The built-in methods and exposed tools are switched the same way (`"log"`, `"llm.ask"`, `"tools.ui_click"`).

## The definition

`Scripting::apiDefinition(query)` renders the enabled methods as TypeScript declarations:

```ts
// Methods a script can call. Calls are synchronous; a failing call throws.
declare namespace db {
  /** Reads one object of the project. Read-only.
   *  @param id Object id
   *  @returns The object */
  function readObject(id: string): { id: string, name: string, year: number };
}
```

The same text serves two readers:

- **The model** gets it from the tool `script_api(query)`. Without a query it returns everything, or an index with one line per method when the text is larger than `setApiBudgetChars()` (default 6000). With a query (a method name, a namespace, keywords) it returns the matching declarations.
- **The user** sees it in the reference pane of the script editor.

`setApiDelivery(ApiDelivery::InToolDescription)` puts the definition into the description of `run_script` instead. It is then always in front of the model, but the tool list changes whenever the methods do, which invalidates the provider's prompt cache.

## What the model gets back

`run_script(code | name, args, dry_run)` passes three stages. The first two execute nothing.

| Stage | Checks | Error |
|---|---|---|
| Syntax | The code compiles | `{"status":"error","phase":"syntax","line":7,"message":"Expected token `)'"}` |
| Check | Every `space.method` in the source exists and is enabled | `{"status":"error","phase":"check","line":3,"message":"no method 'db.readObjekt'","did_you_mean":"db.readObject"}` |
| Run | Arguments per call; whatever the script throws | `{"status":"error","phase":"runtime","line":12,"message":"db.setProperty: ... Signature: db.setProperty(...)","calls":{...}}` |

Other outcomes:

```json
{"status":"ok","result":{"archived":312},"calls":{"db.readObject":1000,"db.setProperty":312},"duration_ms":4210,"log":["..."]}
{"status":"error","phase":"limit","message":"stopped after 5000 method calls (the limit)","calls":{...}}
{"status":"error","phase":"declined","message":"the user declined the script","reason":"leave archived ones alone"}
{"status":"error","phase":"cancelled","message":"the script was stopped"}
```

A script that fails half way leaves its changes; `calls` says how many calls of which method were made. There is no rollback.

### The language

ECMAScript 2016 as implemented by Qt: `let` / `const`, arrow functions, `for...of`, template strings, destructuring, `Map` / `Set`. Not available: `async` / `await`, object spread; optional chaining (`?.`) and `??` only in a build with Qt 6. The script is the body of a function: `return` hands back the result. It runs in strict mode.

### Dry run

With `dry_run: true`, read-only methods run for real and every other call is recorded and answered with the method's return example:

```json
{"status":"ok","dry_run":true,"result":4,
 "would_call":{"db.setProperty":{"count":312,"first_arguments":[["a1","status","archived"], ...]}}}
```

## Limits

```cpp
QtLLM::Scripting::Limits limits;
limits.timeoutMs   = 300000;   // whole run; not applied in the debugger
limits.maxCalls    = 5000;     // method calls
limits.maxLogLines = 200;
scripting->setLimits(limits);
```

`ScriptMethod::setMaxCallsPerRun(n)` limits one method, for methods that are expensive.

One script runs at a time. A script that waits, sleeps or loops without calling anything is stopped by the time limit; `try` / `catch` in the script cannot swallow that.

## Approval

Whether the user sees a script of the model, and agrees, before it runs, is decided per kind of script. Every run the model asks for is of exactly one kind:

| `ScriptKind` | A run is of this kind when |
|---|---|
| `NewReading` | the code came with the request and only calls read-only methods |
| `NewChanging` | the code came with the request and calls a method that is not read-only, or picks methods by computed names |
| `SavedReading`, `SavedChanging` | the same two, for a script of the library run by name that the user saved or the application ships (origin `user` or `application`). A script the model saved itself counts as new |
| `DryRun` | it is a dry run, of whatever script: nothing is changed |

`setApprovalRequired(kind, bool)` switches one kind; by default none needs approval. `setApproval(...)` sets all of them at once:

| `setApproval(...)` | Needs approval |
|---|---|
| `Approval::Never` | no kind |
| `Approval::ChangingScripts` | `NewChanging` and `SavedChanging` |
| `Approval::Always` | every kind |

A script that runs unasked as a reading one is held to that while changing scripts of the same origin need approval: its first call to a changing method stops it (phase `approval`).

The user decides the same in Settings > Scripts, with one check box per kind under "Ask before running". `setApprovalUserConfigurable(kind, false)` takes one kind out of the user's hands, `setApprovalUserConfigurable(false)` all of them.

The built-in dialog shows the code, the arguments and the methods the script uses, marked as reading or changing. The user can run it, test-run it (a dry run, with the result shown in the dialog) or decline with a reason that goes back to the model. "Do not ask again for this script in this session" remembers the exact code.

`setApprovalHandler(handler)` replaces the dialog with the application's own; the handler may decide later.

## Saved scripts

```cpp
auto* library = new QtLLM::ScriptLibrary(this);
library->setDirectory(appData + "/scripts");          // writable
library->addReadOnlyDirectory(":/scripts");           // shipped with the application
scripting->setLibrary(library);
```

One `.js` file per script, with a header of comment lines:

```js
// @name Archive outdated objects
// @description Sets the status of every object older than `before` to archived.
// @param before number Year
// @origin user

for (const id of db.listObjects()) { ... }
```

With a library the model also has:

| Tool | |
|---|---|
| `list_scripts` | Names, descriptions, parameters |
| `read_script(name)` | The code |
| `save_script(name, description, code, parameters)` | Checked first; a script with a mistake is not saved. Replaces only scripts the model saved itself |
| `run_script(name, args)` | Runs a saved script |

The model cannot delete scripts. `setModelMaySaveScripts(false)` takes `save_script` away.

## Switches

| What | By code | Locked against the user |
|---|---|---|
| Scripting for the model | `setEnabled(bool)` | `setEnabledUserConfigurable(bool)` |
| What needs approval | `setApprovalRequired(kind, bool)`, `setApproval(preset)` | `setApprovalUserConfigurable(kind, bool)`, or `(bool)` for every kind |
| Each method | `setMethodEnabled`, `setMethodGroupEnabled` | `setMethodUserConfigurable`, `setMethodGroupUserConfigurable` |
| Tools as methods | `exposeTool`, `exposeToolGroup`, `exposeAllTools` | as methods, above |
| `llm.ask` / `llm.choose` | `setLlmConfigProvider`, `setLlmCallLimit` | as methods, above |
| Saved scripts | `setLibrary`, `setModelMaySaveScripts` | |
| Editing in the Scripts tab | `setUserMayEditScripts(bool)` | |
| Runner and debugger in the editor | `setDebuggerEnabled(bool)` | |
| Each scripting tool | `Client::setToolEnabled("run_script", ...)`, group `"Scripting"` | `Client::setToolUserConfigurable` |

A switch that is not user configurable is shown disabled in the Scripts tab, and the `...ByUser` functions refuse to change it.

`userSettings()` returns the user's choices as JSON for the application to store; `applyUserSettings()` puts them back and skips what is locked.

## The Scripts tab and the editor

`SettingsDialog::setScripting(scripting)` adds a **Scripts** tab. The same widget, `QtLLM::ScriptsWidget`, can be placed in a settings window of the application's own.

- **Switches**: "Allow the assistant to run scripts" and, under "Ask before running", one check box per kind of script.
- **Scripts**: the saved scripts. New, Edit, Duplicate, Delete, Open folder.
- **Methods**: every method with a check box; locked ones are greyed out.
- **Runs**: the scripts run in this session, with code, arguments, result and log.

The editor (`QtLLM::ScriptEditorDialog`) is a window of its own: name, description, parameters, the code, and beside it the reference of the methods a script can call, with search. A double click inserts a call.

### Running and stepping

Check, Run and Dry run work on the code as it stands in the editor, saved or not.

Debug starts the script and stops before its first statement. The current line is marked, and the Variables table shows what the script's variables hold there; a value that changed since the stop before is bold.

| Button | |
|---|---|
| Step | Runs the statement and stops before the next one, also inside functions of the script |
| Next call | Runs on to the next method call and shows it with its arguments; the Calls table gets the value it returned |
| Continue | Runs on to the next breakpoint (set by a click in the line-number margin) or to `pause("label")` in the script |

The engine has no debugger interface, so for a debug run the script is rewritten: a hidden call in front of every statement that starts a line. Stepping is therefore by line - a second statement on a line is not stopped at - and a variable appears from the line after its declaration on. A script whose statements the rewriting cannot tell apart is stepped call by call instead. Normal runs are not rewritten.

## Running scripts from code

```cpp
QtLLM::Scripting::RunOptions options;
options.args = QJsonObject{{"before", 2015}};
options.dryRun = true;
scripting->run(code, options, [](const QJsonObject& result) { ... });

const QtLLM::Scripting::Check check = scripting->check(code);   // compiles? uses only what exists?
```

`run()` works with scripting disabled for the model as well: the switch is about the model's tools.

`requestRun(request, callback)` is what `run_script` uses: the script is checked, approved by the user where the kind of script needs it, then run. An application can route other requesters through it as well.

Signals: `runStarted`, `runLog`, `runProgress`, `runFinished`; in the debugger `runPaused` and `runCallFinished`. `runs()` is the history of the session.
