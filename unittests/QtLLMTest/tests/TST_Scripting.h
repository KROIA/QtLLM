#pragma once

// Scripts: the methods an application registers, what a script may call, the
// feedback a broken script gets, limits, dry run, approval, the debugger's
// call stepping, saved scripts and the model's tools for all of it.

#include "UnitTest.h"
#include "QtLLM.h"
#include "FakeApi.h"
#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>

class TST_Scripting : public UnitTest::Test
{
    TEST_CLASS(TST_Scripting)
public:
    TST_Scripting()
        : Test("TST_Scripting")
    {
        ADD_TEST(TST_Scripting::testToolsFollowTheSwitches);
        ADD_TEST(TST_Scripting::testLoopOverObjects);
        ADD_TEST(TST_Scripting::testSyntaxErrorHasTheLine);
        ADD_TEST(TST_Scripting::testUnknownMethodIsRejectedBeforeRunning);
        ADD_TEST(TST_Scripting::testScannerIgnoresTextAndOwnNames);
        ADD_TEST(TST_Scripting::testDisabledMethodsDoNotExist);
        ADD_TEST(TST_Scripting::testArgumentsAreChecked);
        ADD_TEST(TST_Scripting::testFailingMethodThrows);
        ADD_TEST(TST_Scripting::testReturnValueLogAndThrow);
        ADD_TEST(TST_Scripting::testCallLimit);
        ADD_TEST(TST_Scripting::testEndlessLoopHitsTheTimeLimit);
        ADD_TEST(TST_Scripting::testStop);
        ADD_TEST(TST_Scripting::testAsynchronousMethod);
        ADD_TEST(TST_Scripting::testDryRun);
        ADD_TEST(TST_Scripting::testExposedTools);
        ADD_TEST(TST_Scripting::testApiDefinition);
        ADD_TEST(TST_Scripting::testModelRunsScripts);
        ADD_TEST(TST_Scripting::testApprovalAlways);
        ADD_TEST(TST_Scripting::testApprovalForChangingScripts);
        ADD_TEST(TST_Scripting::testApprovalDialog);
        ADD_TEST(TST_Scripting::testUserSettingsAndLocks);
        ADD_TEST(TST_Scripting::testCallStepping);
        ADD_TEST(TST_Scripting::testScriptFiles);
        ADD_TEST(TST_Scripting::testModelUsesSavedScripts);
        ADD_TEST(TST_Scripting::testAskingAModelFromAScript);
    }

private:
    static bool spin(const std::function<bool()>& condition, int timeoutMs = 10000)
    {
        QEventLoop loop;
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, [&]() {
            if (condition())
                loop.quit();
        });
        poll.start(5);
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
        if (!condition())
            loop.exec();
        return condition();
    }

    using Options = QtLLM::Scripting::RunOptions;

    // Runs a script and waits for its result.
    static QJsonObject runScript(QtLLM::Scripting& scripting, const QString& code,
                                 const Options& options = Options())
    {
        QJsonObject result;
        bool done = false;
        scripting.run(code, options, [&](const QJsonObject& r) {
            result = r;
            done = true;
        });
        spin([&]() { return done; });
        return result;
    }

    // A small "database of objects", the kind of thing scripts are for.
    struct Db
    {
        QMap<QString, QJsonObject> objects;
        int reads = 0;
        int writes = 0;

        Db()
        {
            for (int i = 1; i <= 6; ++i) {
                const QString id = QString("o%1").arg(i);
                objects.insert(id, QJsonObject{{"id", id}, {"name", QString("Object %1").arg(i)},
                                               {"age", i * 10}, {"status", "active"}});
            }
        }
        int archived() const
        {
            int n = 0;
            for (const QJsonObject& o : objects)
                n += o["status"].toString() == "archived" ? 1 : 0;
            return n;
        }
    };

    static void registerDb(QtLLM::Scripting& scripting, Db& db)
    {
        QtLLM::ScriptMethod list("db.list");
        list.setDescription("Lists the ids of all objects.").setGroup("Database").setReadOnly(true)
            .setReturns("array", "The ids", QJsonArray{"o1", "o2"});
        scripting.registerMethod(list, [&db](const QJsonObject&) -> QJsonValue {
            return QJsonArray::fromStringList(db.objects.keys());
        });

        QtLLM::ScriptMethod read("db.read");
        read.setDescription("Reads one object.").setGroup("Database").setReadOnly(true)
            .addParameter("id", "string", "Object id")
            .setReturns("object", "The object",
                        QJsonObject{{"id", "o1"}, {"name", "Pump"}, {"age", 3}, {"status", "active"}});
        scripting.registerMethod(read, [&db](const QJsonObject& args) -> QJsonValue {
            ++db.reads;
            const QString id = args["id"].toString();
            if (!db.objects.contains(id))
                throw QtLLM::ScriptError(QString("no object '%1'").arg(id));
            return db.objects.value(id);
        });

        QtLLM::ScriptMethod setStatus("db.setStatus");
        setStatus.setDescription("Sets the status of an object.").setGroup("Database")
            .addParameter("id", "string", "Object id")
            .addParameter("status", "string", "New status", true, {"active", "archived"})
            .setReturns("object", "", QJsonObject{{"changed", true}});
        scripting.registerMethod(setStatus, [&db](const QJsonObject& args) -> QJsonValue {
            ++db.writes;
            const QString id = args["id"].toString();
            if (!db.objects.contains(id))
                throw QtLLM::ScriptError(QString("no object '%1'").arg(id));
            db.objects[id]["status"] = args["status"].toString();
            return QJsonObject{{"changed", true}};
        });
    }

    struct Outcome
    {
        QString            response;
        QStringList        errors;
        QList<QJsonObject> toolResults;     // what the model got back, in order
    };

    // One prompt, until the answer or an error is there.
    static Outcome turn(QtLLM::Client& client, const QString& prompt)
    {
        Outcome outcome;
        bool done = false;
        auto c1 = QObject::connect(&client, &QtLLM::Client::responseReady, [&](const QString& text) {
            outcome.response = text;
            done = true;
        });
        auto c2 = QObject::connect(&client, &QtLLM::Client::errorOccurred, [&](const QString& message) {
            outcome.errors.append(message);
            done = true;
        });
        auto c3 = QObject::connect(&client, &QtLLM::Client::toolCompleted,
                                   [&](const QString&, const QJsonObject& result) {
            outcome.toolResults.append(result);
        });
        client.sendPrompt(prompt);
        spin([&]() { return done; });
        QObject::disconnect(c1);
        QObject::disconnect(c2);
        QObject::disconnect(c3);
        return outcome;
    }

    static QByteArray scriptCall(const QString& code, const QJsonObject& more = QJsonObject(),
                                 const QString& id = "t1")
    {
        QJsonObject input = more;
        input["code"] = code;
        return FakeApi::toolUseReply("run_script", input, id);
    }

    // The scripting tools exist while scripting is on; the library adds its own.
    TEST_FUNCTION(testToolsFollowTheSwitches)
    {
        TEST_START;

        FakeApi api;
        QtLLM::Client client("key", api.url());
        QtLLM::Scripting scripting(&client);
        TEST_ASSERT(scripting.isEnabled());
        TEST_ASSERT(client.toolNames().contains("run_script"));
        TEST_ASSERT(client.toolNames().contains("script_api"));
        TEST_ASSERT(!client.toolNames().contains("list_scripts"));

        scripting.setEnabled(false);
        TEST_ASSERT(!client.toolNames().contains("run_script"));
        TEST_ASSERT(!client.toolNames().contains("script_api"));
        // The application itself can still run a script.
        TEST_COMPARE(runScript(scripting, "return 1 + 1;")["result"].toInt(), 2);
        scripting.setEnabled(true);

        QtLLM::ScriptLibrary library;
        scripting.setLibrary(&library);
        TEST_ASSERT(client.toolNames().contains("list_scripts"));
        TEST_ASSERT(client.toolNames().contains("read_script"));
        TEST_ASSERT(client.toolNames().contains("save_script"));
        scripting.setModelMaySaveScripts(false);
        TEST_ASSERT(!client.toolNames().contains("save_script"));
        TEST_ASSERT(client.toolNames().contains("list_scripts"));

        // The definition inside the tool: no script_api then.
        scripting.setApiDelivery(QtLLM::Scripting::ApiDelivery::InToolDescription);
        TEST_ASSERT(!client.toolNames().contains("script_api"));
        QString description;
        for (const QtLLM::Tool& tool : client.registeredTools()) {
            if (tool.name() == "run_script")
                description = tool.description();
        }
        TEST_ASSERT(description.contains("declare function log("));

        // The tools are an ordinary group for the tool policy.
        client.setToolGroupEnabled("Scripting", false);
        TEST_ASSERT(!client.isToolEnabled("run_script"));
    }

    // What it is for: one script, many calls, a small result.
    TEST_FUNCTION(testLoopOverObjects)
    {
        TEST_START;

        FakeApi api;
        QtLLM::Client client("key", api.url());
        QtLLM::Scripting scripting(&client);
        Db db;
        registerDb(scripting, db);

        Options options;
        options.args = QJsonObject{{"minAge", 40}};
        const QJsonObject result = runScript(scripting,
            "const changed = [];\n"
            "for (const id of db.list()) {\n"
            "    const o = db.read(id);\n"
            "    if (o.age >= args.minAge) {\n"
            "        db.setStatus(id, 'archived');\n"
            "        changed.push(o.name);\n"
            "    }\n"
            "}\n"
            "return {count: changed.length, first: changed[0]};\n", options);

        TEST_COMPARE(result["status"].toString(), QString("ok"));
        TEST_COMPARE(result["result"].toObject()["count"].toInt(), 3);
        TEST_COMPARE(result["result"].toObject()["first"].toString(), QString("Object 4"));
        TEST_COMPARE(db.archived(), 3);
        const QJsonObject calls = result["calls"].toObject();
        TEST_COMPARE(calls["db.list"].toInt(), 1);
        TEST_COMPARE(calls["db.read"].toInt(), 6);
        TEST_COMPARE(calls["db.setStatus"].toInt(), 3);
        TEST_ASSERT(result.contains("duration_ms"));

        // It is on record.
        TEST_COMPARE(scripting.runs().size(), 1);
        TEST_COMPARE(scripting.runs().first().result["status"].toString(), QString("ok"));
        TEST_ASSERT(!scripting.isRunning());
    }

    TEST_FUNCTION(testSyntaxErrorHasTheLine)
    {
        TEST_START;

        FakeApi api;
        QtLLM::Client client("key", api.url());
        QtLLM::Scripting scripting(&client);
        Db db;
        registerDb(scripting, db);

        const QString broken = "const ids = db.list();\nfor (const id of ids {\n    db.read(id);\n}\n";
        const QtLLM::Scripting::Check check = scripting.check(broken);
        TEST_ASSERT(!check.ok);
        TEST_COMPARE(check.phase, QString("syntax"));
        TEST_COMPARE(check.line, 2);
        TEST_ASSERT(!check.message.isEmpty());
        TEST_COMPARE(check.toJson()["status"].toString(), QString("error"));
        TEST_COMPARE(check.toJson()["line"].toInt(), 2);

        // Running it reports the same, and nothing was called.
        const QJsonObject result = runScript(scripting, broken);
        TEST_COMPARE(result["phase"].toString(), QString("syntax"));
        TEST_COMPARE(result["line"].toInt(), 2);
        TEST_COMPARE(db.reads, 0);

        // Code that closes the function early to run while it is "read" finds
        // nothing to call, and an endless loop there is stopped.
        QElapsedTimer timer;
        timer.start();
        const QtLLM::Scripting::Check escape = scripting.check("}); while (true) {} (function(){");
        TEST_ASSERT(!escape.ok);
        TEST_COMPARE(escape.phase, QString("syntax"));
        TEST_ASSERT(timer.elapsed() < 8000);

        TEST_ASSERT(scripting.check("return db.list().length;").ok);
    }

    TEST_FUNCTION(testUnknownMethodIsRejectedBeforeRunning)
    {
        TEST_START;

        FakeApi api;
        QtLLM::Client client("key", api.url());
        QtLLM::Scripting scripting(&client);
        Db db;
        registerDb(scripting, db);

        const QString code = "const ids = db.list();\nfor (const id of ids)\n    db.reed(id);\nreturn ids.length;\n";
        const QtLLM::Scripting::Check check = scripting.check(code);
        TEST_ASSERT(!check.ok);
        TEST_COMPARE(check.phase, QString("check"));
        TEST_COMPARE(check.line, 3);
        TEST_ASSERT(check.message.contains("db.reed"));
        TEST_COMPARE(check.didYouMean, QString("db.read"));
        TEST_COMPARE(check.toJson()["did_you_mean"].toString(), QString("db.read"));

        // Nothing close: the namespace's methods are named.
        const QtLLM::Scripting::Check noMatch = scripting.check("db.archiveEverything();");
        TEST_ASSERT(!noMatch.ok);
        TEST_ASSERT(noMatch.message.contains("db.setStatus"));

        // What a script uses, and whether it changes anything.
        const QtLLM::Scripting::Check reading = scripting.check("return db.read(db.list()[0]).name;");
        TEST_ASSERT(reading.ok);
        TEST_COMPARE(reading.methods.size(), 2);
        TEST_ASSERT(reading.changingMethods.isEmpty());
        TEST_ASSERT(!reading.dynamic);
        const QtLLM::Scripting::Check changing = scripting.check("db.setStatus('o1', 'archived');");
        TEST_COMPARE(changing.changingMethods, QStringList{"db.setStatus"});
        // By a computed name: the source cannot tell.
        TEST_ASSERT(scripting.check("const m = 'set' + 'Status'; db[m]('o1', 'archived');").dynamic);
        TEST_ASSERT(scripting.check("return eval('1 + 1');").dynamic);

        // An object the application does not have fails when the script gets there.
        const QJsonObject result = runScript(scripting, "const a = 1;\nreturn database.list();\n");
        TEST_COMPARE(result["phase"].toString(), QString("runtime"));
        TEST_COMPARE(result["line"].toInt(), 2);
        TEST_ASSERT(result["message"].toString().contains("database"));
        TEST_ASSERT(result.contains("hint"));
    }

    // Method names inside strings and comments are text, not calls; a name
    // the script declares itself is the script's.
    TEST_FUNCTION(testScannerIgnoresTextAndOwnNames)
    {
        TEST_START;

        FakeApi api;
        QtLLM::Client client("key", api.url());
        QtLLM::Scripting scripting(&client);
        Db db;
        registerDb(scripting, db);

        TEST_ASSERT(scripting.check("// db.nothing() would be wrong\nreturn 'db.nothing()';").ok);
        TEST_ASSERT(scripting.check("/* db.nothing()\n db.other() */ return \"it's db.gone()\";").ok);
        TEST_ASSERT(scripting.check("return `plain db.nothing() text`;").ok);
        TEST_ASSERT(scripting.check("return /db.nothing\\(/.test('x');").ok);
        // ...but code inside a template is code.
        const QtLLM::Scripting::Check inTemplate = scripting.check("return `name: ${db.nothing('o1')}`;");
        TEST_ASSERT(!inTemplate.ok);
        TEST_COMPARE(inTemplate.phase, QString("check"));
        TEST_ASSERT(scripting.check("return `name: ${db.read('o1').name}`;").ok);
        TEST_COMPARE(runScript(scripting, "return `name: ${db.read('o1').name}`;")["result"].toString(),
                     QString("name: Object 1"));
        // A division is not the start of a regular expression.
        TEST_ASSERT(!scripting.check("const a = 4 / 2; db.nothing(); const b = a / 2;").ok);

        // The script's own `db`.
        TEST_ASSERT(scripting.check("const db = {size: () => 3};\nreturn db.size();").ok);
        TEST_COMPARE(runScript(scripting, "const db = {size: () => 3};\nreturn db.size();")["result"].toInt(), 3);
        // A property called like a namespace is not the namespace.
        TEST_ASSERT(scripting.check("const o = {db: {x: 1}};\nreturn o.db.x;").ok);
    }

    TEST_FUNCTION(testDisabledMethodsDoNotExist)
    {
        TEST_START;

        FakeApi api;
        QtLLM::Client client("key", api.url());
        QtLLM::Scripting scripting(&client);
        Db db;
        registerDb(scripting, db);
        int changes = 0;
        QObject::connect(&scripting, &QtLLM::Scripting::methodsChanged, [&]() { ++changes; });

        TEST_ASSERT(scripting.hasMethod("db.setStatus"));
        TEST_ASSERT(scripting.isMethodEnabled("db.setStatus"));
        TEST_ASSERT(!scripting.isMethodEnabled("db.unknown"));

        scripting.setMethodEnabled("db.setStatus", false);
        TEST_COMPARE(changes, 1);
        TEST_ASSERT(!scripting.isMethodEnabled("db.setStatus"));
        TEST_ASSERT(scripting.hasMethod("db.setStatus"));               // still registered
        TEST_ASSERT(!scripting.apiDefinition().contains("setStatus"));
        TEST_ASSERT(scripting.apiDefinition().contains("function read("));

        const QtLLM::Scripting::Check check = scripting.check("db.setStatus('o1', 'archived');");
        TEST_ASSERT(!check.ok);
        TEST_COMPARE(check.phase, QString("check"));
        TEST_ASSERT(check.message.contains("disabled"));
        // Reached by a computed name it is simply not there.
        const QJsonObject result = runScript(scripting, "db['set' + 'Status']('o1', 'archived');");
        TEST_COMPARE(result["status"].toString(), QString("error"));
        TEST_COMPARE(db.writes, 0);

        // A whole group; a method's own switch wins over its group's.
        scripting.setMethodGroupEnabled("Database", false);
        TEST_ASSERT(!scripting.isMethodEnabled("db.read"));
        scripting.setMethodEnabled("db.read", true);
        TEST_ASSERT(scripting.isMethodEnabled("db.read"));
        TEST_ASSERT(!scripting.isMethodEnabled("db.list"));
        TEST_COMPARE(runScript(scripting, "return db.read('o2').name;")["result"].toString(), QString("Object 2"));

        // The policy is by name: it holds for a method registered later.
        scripting.setMethodEnabled("db.later", false);
        scripting.registerMethod(QtLLM::ScriptMethod("db.later"), [](const QJsonObject&) -> QJsonValue { return 1; });
        TEST_ASSERT(!scripting.isMethodEnabled("db.later"));

        // The built-in ones are methods like any other.
        TEST_ASSERT(scripting.isMethodEnabled("log"));
        scripting.setMethodEnabled("log", false);
        TEST_ASSERT(!scripting.check("log('x');").ok);

        scripting.unregisterMethod("db.read");
        TEST_ASSERT(!scripting.hasMethod("db.read"));
    }

    TEST_FUNCTION(testArgumentsAreChecked)
    {
        TEST_START;

        FakeApi api;
        QtLLM::Client client("key", api.url());
        QtLLM::Scripting scripting(&client);
        Db db;
        registerDb(scripting, db);

        // A value outside the enum: the script stops there, with the signature.
        QJsonObject result = runScript(scripting, "const id = 'o1';\n\ndb.setStatus(id, 'deleted');\n");
        TEST_COMPARE(result["status"].toString(), QString("error"));
        TEST_COMPARE(result["phase"].toString(), QString("runtime"));
        TEST_COMPARE(result["line"].toInt(), 3);
        TEST_ASSERT(result["message"].toString().contains("db.setStatus"));
        TEST_ASSERT(result["message"].toString().contains("Signature: db.setStatus(id: string, status: \"active\" | \"archived\")"));
        TEST_COMPARE(db.writes, 0);

        // Missing and surplus arguments.
        result = runScript(scripting, "db.setStatus('o1');");
        TEST_COMPARE(result["phase"].toString(), QString("runtime"));
        TEST_ASSERT(result["message"].toString().contains("status"));
        result = runScript(scripting, "db.read('o1', 'o2');");
        TEST_ASSERT(result["message"].toString().contains("takes 1 argument"));
        result = runScript(scripting, "db.list(1);");
        TEST_ASSERT(result["message"].toString().contains("takes no arguments"));

        // Named arguments work as well, and the enum's spelling is repaired.
        result = runScript(scripting, "return db.setStatus({id: 'o1', status: 'ARCHIVED'}).changed;");
        TEST_COMPARE(result["status"].toString(), QString("ok"));
        TEST_ASSERT(result["result"].toBool());
        TEST_COMPARE(db.objects["o1"]["status"].toString(), QString("archived"));

        // The definition's own check.
        QJsonObject named;
        QString error;
        QtLLM::ScriptMethod method("x.y");
        method.addParameter("count", "integer", "How many").addParameter("label", "string", "Text", false);
        TEST_ASSERT(method.bindArguments(QJsonArray{"3"}, named, &error));     // "3" -> 3
        TEST_COMPARE(named["count"].toInt(), 3);
        TEST_ASSERT(!named.contains("label"));
        TEST_ASSERT(!method.bindArguments(QJsonArray{}, named, &error));
        TEST_ASSERT(error.contains("count"));
        TEST_ASSERT(QtLLM::ScriptMethod::isValidName("db.readObject"));
        TEST_ASSERT(QtLLM::ScriptMethod::isValidName("log"));
        TEST_ASSERT(!QtLLM::ScriptMethod::isValidName("a.b.c"));
        TEST_ASSERT(!QtLLM::ScriptMethod::isValidName("read-object"));
        TEST_ASSERT(!QtLLM::ScriptMethod::isValidName(""));
    }

    TEST_FUNCTION(testFailingMethodThrows)
    {
        TEST_START;

        FakeApi api;
        QtLLM::Client client("key", api.url());
        QtLLM::Scripting scripting(&client);
        Db db;
        registerDb(scripting, db);

        // Not caught: the script ends there and says where.
        QJsonObject result = runScript(scripting,
            "function load(id) {\n"
            "    return db.read(id);\n"
            "}\n"
            "db.setStatus('o1', 'archived');\n"
            "load('missing');\n"
            "db.setStatus('o2', 'archived');\n");
        TEST_COMPARE(result["status"].toString(), QString("error"));
        TEST_COMPARE(result["phase"].toString(), QString("runtime"));
        TEST_COMPARE(result["line"].toInt(), 2);
        TEST_ASSERT(result["message"].toString().contains("db.read: no object 'missing'"));
        // What happened before stays done, and the result says what that was.
        TEST_COMPARE(db.archived(), 1);
        TEST_COMPARE(result["calls"].toObject()["db.setStatus"].toInt(), 1);

        // Caught: the script goes on.
        result = runScript(scripting,
            "let failed = 0;\n"
            "for (const id of ['o2', 'nope', 'o3']) {\n"
            "    try { db.setStatus(id, 'archived'); } catch (e) { failed++; log(e.message); }\n"
            "}\n"
            "return failed;\n");
        TEST_COMPARE(result["status"].toString(), QString("ok"));
        TEST_COMPARE(result["result"].toInt(), 1);
        TEST_COMPARE(db.archived(), 3);
        TEST_ASSERT(result["log"].toArray().first().toString().contains("no object 'nope'"));
    }

    TEST_FUNCTION(testReturnValueLogAndThrow)
    {
        TEST_START;

        FakeApi api;
        QtLLM::Client client("key", api.url());
        QtLLM::Scripting scripting(&client);
        QStringList logged;
        QObject::connect(&scripting, &QtLLM::Scripting::runLog, [&](const QString& text) { logged << text; });
        QList<double> progress;
        QObject::connect(&scripting, &QtLLM::Scripting::runProgress,
                         [&](double done, double total, const QString&) { progress << done << total; });

        QJsonObject result = runScript(scripting,
            "log('start', 3, {a: 1});\nprogress(1, 2, 'half');\nreturn [1, 'two', {three: 3}, null];\n");
        TEST_COMPARE(result["status"].toString(), QString("ok"));
        TEST_COMPARE(result["result"].toArray().size(), 4);
        TEST_COMPARE(result["result"].toArray()[2].toObject()["three"].toInt(), 3);
        TEST_COMPARE(result["log"].toArray().size(), 1);
        TEST_COMPARE(result["log"].toArray()[0].toString(), QString("start 3 {\"a\":1}"));
        TEST_COMPARE(logged, QStringList{"start 3 {\"a\":1}"});
        TEST_COMPARE(progress, (QList<double>{1, 2}));
        TEST_ASSERT(!result.contains("note"));

        // Nothing returned: said so, because it usually is a mistake.
        result = runScript(scripting, "const a = 1 + 1;");
        TEST_COMPARE(result["status"].toString(), QString("ok"));
        TEST_ASSERT(result["result"].isNull());
        TEST_ASSERT(result["note"].toString().contains("return"));

        // The script's own throw, of an error and of a plain value.
        result = runScript(scripting, "const n = 2;\nif (n > 1)\n    throw new Error('too many: ' + n);\n");
        TEST_COMPARE(result["phase"].toString(), QString("runtime"));
        TEST_COMPARE(result["message"].toString(), QString("too many: 2"));
        TEST_COMPARE(result["line"].toInt(), 3);
        result = runScript(scripting, "throw 'just text';");
        TEST_COMPARE(result["phase"].toString(), QString("runtime"));
        TEST_COMPARE(result["message"].toString(), QString("just text"));
        result = runScript(scripting, "const o = null;\nreturn o.field;\n");
        TEST_ASSERT(result["message"].toString().startsWith("TypeError"));
        TEST_COMPARE(result["line"].toInt(), 2);

        // Only the end of a long log goes back; all of it was signalled.
        logged.clear();
        result = runScript(scripting, "for (let i = 0; i < 50; i++) log('line ' + i);\nreturn 0;");
        TEST_COMPARE(logged.size(), 50);
        TEST_COMPARE(result["log"].toArray().size(), 20);
        TEST_COMPARE(result["log"].toArray().last().toString(), QString("line 49"));
        TEST_COMPARE(result["log_lines"].toInt(), 50);

        // A script that is syntactically the language Qt 5 does not have yet.
        TEST_COMPARE(scripting.check("return (async () => 1)();").phase, QString("syntax"));
    }

    TEST_FUNCTION(testCallLimit)
    {
        TEST_START;

        FakeApi api;
        QtLLM::Client client("key", api.url());
        QtLLM::Scripting scripting(&client);
        Db db;
        registerDb(scripting, db);
        QtLLM::Scripting::Limits limits;
        limits.maxCalls = 10;
        scripting.setLimits(limits);

        // Catching the failure does not get a script past the limit.
        const QJsonObject result = runScript(scripting,
            "for (let i = 0; i < 100; i++) {\n    try { db.read('o1'); } catch (e) {}\n}\nreturn 'finished';\n");
        TEST_COMPARE(result["status"].toString(), QString("error"));
        TEST_COMPARE(result["phase"].toString(), QString("limit"));
        TEST_ASSERT(result["message"].toString().contains("10"));
        TEST_COMPARE(db.reads, 10);
        TEST_COMPARE(result["calls"].toObject()["db.read"].toInt(), 10);

        // log and the like are not what the limit is about.
        const QJsonObject chatty = runScript(scripting, "for (let i = 0; i < 30; i++) log(i);\nreturn 1;");
        TEST_COMPARE(chatty["status"].toString(), QString("ok"));
    }

    TEST_FUNCTION(testEndlessLoopHitsTheTimeLimit)
    {
        TEST_START;

        FakeApi api;
        QtLLM::Client client("key", api.url());
        QtLLM::Scripting scripting(&client);
        QtLLM::Scripting::Limits limits;
        limits.timeoutMs = 300;
        scripting.setLimits(limits);

        QElapsedTimer timer;
        timer.start();
        QJsonObject result = runScript(scripting, "let n = 0;\nwhile (true) { try { n++; } catch (e) {} }\n");
        TEST_COMPARE(result["status"].toString(), QString("error"));
        TEST_COMPARE(result["phase"].toString(), QString("limit"));
        TEST_ASSERT(result["message"].toString().contains("time limit"));
        TEST_ASSERT(timer.elapsed() < 5000);
        TEST_ASSERT(!scripting.isRunning());

        // A sleeping script is woken for it.
        timer.restart();
        result = runScript(scripting, "sleep(60000);\nreturn 1;");
        TEST_COMPARE(result["phase"].toString(), QString("limit"));
        TEST_ASSERT(timer.elapsed() < 5000);

        // The next script runs normally.
        TEST_COMPARE(runScript(scripting, "sleep(20);\nreturn 5;")["result"].toInt(), 5);
    }

    TEST_FUNCTION(testStop)
    {
        TEST_START;

        FakeApi api;
        QtLLM::Client client("key", api.url());
        QJsonObject result;
        bool done = false;
        {
            QtLLM::Scripting scripting(&client);
            int started = 0;
            QObject::connect(&scripting, &QtLLM::Scripting::runStarted, [&]() { ++started; });

            TEST_ASSERT(scripting.run("sleep(60000);\nreturn 1;", Options(), [&](const QJsonObject& r) {
                result = r;
                done = true;
            }));
            TEST_COMPARE(started, 1);
            TEST_ASSERT(scripting.isRunning());

            // One at a time.
            QJsonObject second;
            TEST_ASSERT(!scripting.run("return 2;", Options(), [&](const QJsonObject& r) { second = r; }));
            TEST_COMPARE(second["status"].toString(), QString("error"));
            TEST_ASSERT(second["message"].toString().contains("another script"));

            scripting.stop();
            TEST_ASSERT(spin([&]() { return done; }, 5000));
            TEST_COMPARE(result["phase"].toString(), QString("cancelled"));
            TEST_ASSERT(!scripting.isRunning());

            // Destroying the object ends a running script without waiting for it.
            scripting.run("while (true) {}");
        }
        TEST_ASSERT(true);
    }

    TEST_FUNCTION(testAsynchronousMethod)
    {
        TEST_START;

        FakeApi api;
        QtLLM::Client client("key", api.url());
        QtLLM::Scripting scripting(&client);

        QtLLM::ScriptMethod later("net.fetch");
        later.addParameter("what", "string", "What to get").setReadOnly(true);
        scripting.registerAsyncMethod(later, [](const QJsonObject& args, QtLLM::ScriptCompletion done) {
            const QString what = args["what"].toString();
            QTimer::singleShot(30, qApp, [done, what]() {
                if (what == "bad")
                    done.fail("cannot get that");
                else
                    done.finish(QJsonObject{{"got", what}});
                done.finish("ignored");     // only the first answer counts
            });
        });

        QElapsedTimer timer;
        timer.start();
        QJsonObject result = runScript(scripting, "return net.fetch('a').got + net.fetch('b').got;");
        TEST_COMPARE(result["result"].toString(), QString("ab"));
        TEST_ASSERT(timer.elapsed() >= 50);

        result = runScript(scripting, "return net.fetch('bad');");
        TEST_COMPARE(result["phase"].toString(), QString("runtime"));
        TEST_ASSERT(result["message"].toString().contains("net.fetch: cannot get that"));

        // A method that never answers does not keep a stopped script alive.
        QtLLM::ScriptMethod never("net.never");
        scripting.registerAsyncMethod(never, [](const QJsonObject&, QtLLM::ScriptCompletion) {});
        bool done = false;
        scripting.run("net.never();", Options(), [&](const QJsonObject& r) {
            result = r;
            done = true;
        });
        spin([]() { return false; }, 100);
        TEST_ASSERT(scripting.isRunning());
        scripting.stop();
        TEST_ASSERT(spin([&]() { return done; }, 5000));
        TEST_COMPARE(result["phase"].toString(), QString("cancelled"));
    }

    // Reading is done, changing is written down.
    TEST_FUNCTION(testDryRun)
    {
        TEST_START;

        FakeApi api;
        QtLLM::Client client("key", api.url());
        QtLLM::Scripting scripting(&client);
        Db db;
        registerDb(scripting, db);

        Options options;
        options.dryRun = true;
        const QJsonObject result = runScript(scripting,
            "let n = 0;\n"
            "for (const id of db.list()) {\n"
            "    if (db.read(id).age > 20 && db.setStatus(id, 'archived').changed) n++;\n"
            "}\n"
            "return n;\n", options);

        TEST_COMPARE(result["status"].toString(), QString("ok"));
        TEST_ASSERT(result["dry_run"].toBool());
        TEST_COMPARE(result["result"].toInt(), 4);          // answered with the method's example
        TEST_COMPARE(db.reads, 6);
        TEST_COMPARE(db.writes, 0);
        TEST_COMPARE(db.archived(), 0);
        const QJsonObject would = result["would_call"].toObject();
        TEST_COMPARE(would.keys(), QStringList{"db.setStatus"});
        TEST_COMPARE(would["db.setStatus"].toObject()["count"].toInt(), 4);
        const QJsonArray first = would["db.setStatus"].toObject()["first_arguments"].toArray();
        TEST_COMPARE(first.size(), 3);
        TEST_COMPARE(first[0].toArray()[0].toString(), QString("o3"));
        TEST_COMPARE(first[0].toArray()[1].toString(), QString("archived"));
        TEST_ASSERT(scripting.runs().last().dryRun);
    }

    // Tools as methods - only the ones the application names.
    TEST_FUNCTION(testExposedTools)
    {
        TEST_START;

        FakeApi api;
        QtLLM::Client client("key", api.url());
        QtLLM::Tool add;
        add.setName("add").setDescription("Adds two numbers.").setGroup("Math").setReadOnly(true)
           .addParameter("a", "number", "First", true).addParameter("b", "number", "Second", true);
        int calls = 0;
        client.registerTool(add, [&](const QJsonObject& input) {
            ++calls;
            if (input["a"].toDouble() < 0)
                return QtLLM::toolError("negative numbers are not supported");
            return QtLLM::toolOk({{"sum", input["a"].toDouble() + input["b"].toDouble()}});
        });
        QtLLM::Tool secret;
        secret.setName("secret-tool").setDescription("Not for scripts.").setGroup("Other");
        client.registerTool(secret, [](const QJsonObject&) { return QtLLM::toolOk(); });
        int invoked = 0;
        QObject::connect(&client, &QtLLM::Client::toolInvoked, [&]() { ++invoked; });

        QtLLM::Scripting scripting(&client);
        TEST_ASSERT(!scripting.hasMethod("tools.add"));
        TEST_COMPARE(runScript(scripting, "return tools.add({a: 1, b: 2});")["phase"].toString(), QString("runtime"));

        scripting.exposeToolGroup("Math");
        TEST_ASSERT(scripting.hasMethod("tools.add"));
        TEST_ASSERT(!scripting.hasMethod("tools.secret_tool"));
        TEST_ASSERT(scripting.apiDefinition().contains("function add(args: { a: number, b: number }): object;"));

        QJsonObject result = runScript(scripting,
            "let total = 0;\nfor (let i = 0; i < 5; i++) total += tools.add({a: i, b: '1'}).sum;\nreturn total;\n");
        TEST_COMPARE(result["status"].toString(), QString("ok"));
        TEST_COMPARE(result["result"].toInt(), 15);
        TEST_COMPARE(calls, 5);
        TEST_COMPARE(invoked, 0);       // a script's calls are not chat events

        // A tool's error is a script's exception; so is input that does not fit.
        result = runScript(scripting, "return tools.add({a: -1, b: 1});");
        TEST_ASSERT(result["message"].toString().contains("tools.add: negative numbers are not supported"));
        result = runScript(scripting, "return tools.add({a: 1});");
        TEST_COMPARE(result["phase"].toString(), QString("runtime"));
        TEST_ASSERT(result["message"].toString().contains("Signature: tools.add("));
        TEST_ASSERT(!scripting.check("tools.subtract({a: 1});").ok);

        // The consent handler is asked once per tool and script, not per call.
        int asked = 0;
        client.setToolConsentHandler([&](const QString&, const QJsonObject&) {
            ++asked;
            return true;
        });
        runScript(scripting, "for (let i = 0; i < 4; i++) tools.add({a: i, b: 1});");
        TEST_COMPARE(asked, 1);
        client.setToolConsentHandler([&](const QString&, const QJsonObject&) {
            ++asked;
            return false;
        });
        calls = 0;
        result = runScript(scripting,
            "let refused = 0;\nfor (let i = 0; i < 3; i++) { try { tools.add({a: i, b: 1}); } catch (e) { refused++; } }\nreturn refused;");
        TEST_COMPARE(result["result"].toInt(), 3);
        TEST_COMPARE(asked, 2);
        TEST_COMPARE(calls, 0);
        client.setToolConsentHandler(nullptr);

        // Everything - except what makes no sense inside a script. A '-' in a
        // tool name becomes '_'.
        scripting.exposeAllTools();
        TEST_ASSERT(scripting.hasMethod("tools.secret_tool"));
        TEST_ASSERT(!scripting.hasMethod("tools.run_script"));
        TEST_ASSERT(!scripting.hasMethod("tools.script_api"));
        TEST_COMPARE(runScript(scripting, "return tools.secret_tool().status;")["result"].toString(), QString("ok"));
        // One tool taken out again; a disabled tool is no method.
        scripting.exposeTool("secret-tool", false);
        TEST_ASSERT(!scripting.hasMethod("tools.secret_tool"));
        client.setToolEnabled("add", false);
        TEST_ASSERT(!scripting.hasMethod("tools.add"));
        client.setToolEnabled("add", true);
        // Switched like any method.
        scripting.setMethodEnabled("tools.add", false);
        TEST_ASSERT(!scripting.check("tools.add({a: 1, b: 2});").ok);
    }

    TEST_FUNCTION(testApiDefinition)
    {
        TEST_START;

        FakeApi api;
        QtLLM::Client client("key", api.url());
        QtLLM::Scripting scripting(&client);
        Db db;
        registerDb(scripting, db);

        QtLLM::ScriptMethod find("db.find");
        find.setDescription("Finds objects by a text in\n  their name.").setReadOnly(true)
            .addParameter("text", "string", "What the name must contain")
            .addParameter("limit", "integer", "How many at most", false)
            .setDefault("limit", 10).setRange("limit", 1, 100)
            .addParameter("fields", "array", "Which fields to return", false)
            .setReturns("array", "The matches", QJsonArray{QJsonObject{{"id", "o1"}, {"name", "Pump"}}});
        scripting.registerMethod(find, [](const QJsonObject&) -> QJsonValue { return QJsonArray(); });

        TEST_COMPARE(find.signature(),
                     QString("db.find(text: string, limit?: number, fields?: any[]): { id: string, name: string }[]"));
        const QString declaration = find.declaration("  ");
        TEST_ASSERT(declaration.startsWith("  /** Finds objects by a text in their name. Read-only.\n"));
        TEST_ASSERT(declaration.contains("   *  @param limit How many at most (default 10, 1 to 100)\n"));
        TEST_ASSERT(declaration.contains("   *  @returns The matches */\n"));
        TEST_ASSERT(declaration.endsWith("  function find(text: string, limit?: number, fields?: any[]): { id: string, name: string }[];\n"));

        const QString all = scripting.apiDefinition();
        TEST_ASSERT(all.contains("declare namespace db {\n"));
        TEST_ASSERT(all.contains("  function read(id: string): { age: number, id: string, name: string, status: string };\n"));
        TEST_ASSERT(all.contains("  function setStatus(id: string, status: \"active\" | \"archived\"): { changed: boolean };\n"));
        TEST_ASSERT(all.contains("  function list(): string[];\n"));
        TEST_ASSERT(all.contains("declare function sleep(ms: number): void;\n"));
        TEST_ASSERT(all.contains("declare const args: any;"));

        // A query: the matching methods only.
        const QString some = scripting.apiDefinition("setStatus");
        TEST_ASSERT(some.contains("function setStatus("));
        TEST_ASSERT(!some.contains("function read("));
        TEST_ASSERT(!some.contains("declare function log("));
        const QString byNamespace = scripting.apiDefinition("db");
        TEST_ASSERT(byNamespace.contains("function read(") && byNamespace.contains("function find("));
        const QString byWord = scripting.apiDefinition("contain");       // from a description
        TEST_ASSERT(byWord.contains("function find("));
        TEST_ASSERT(scripting.apiDefinition("zebra").contains("Nothing matches"));

        // Too much for the budget: an index, one line per method.
        scripting.setApiBudgetChars(300);
        const QString index = scripting.apiDefinition();
        TEST_ASSERT(index.contains("db.find - Finds objects by a text in their name."));
        TEST_ASSERT(index.contains("query"));
        TEST_ASSERT(!index.contains("declare namespace"));
        // The model is told what to do with an index.
        QJsonObject indexFromTool;
        client.callTool("script_api", QJsonObject(), [&](const QJsonObject& r) { indexFromTool = r; });
        TEST_ASSERT(indexFromTool["hint"].toString().contains("script_api"));
        TEST_ASSERT(scripting.apiDefinition("db.find").contains("function find("));
        scripting.setApiBudgetChars(6000);

        // What the model's tool returns.
        QJsonObject fromTool;
        client.callTool("script_api", QJsonObject{{"query", "list"}}, [&](const QJsonObject& r) { fromTool = r; });
        TEST_COMPARE(fromTool["status"].toString(), QString("ok"));
        TEST_ASSERT(fromTool["definition"].toString().contains("function list(): string[];"));
    }

    // The model's side: run_script, and what comes back.
    TEST_FUNCTION(testModelRunsScripts)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(scriptCall("for (const id of db.list() {\n}"));                         // broken
        api.replies.enqueue(scriptCall("let n = 0;\nfor (const id of db.list()) {\n"
                                       "    db.setStatus(id, 'archived');\n    n++;\n}\nreturn n;", {}, "t2"));
        api.replies.enqueue(scriptCall("return args.a * 2;", QJsonObject{{"args", QJsonObject{{"a", 21}}}}, "t3"));
        api.replies.enqueue(FakeApi::textReply("All six are archived."));

        QtLLM::Client client("key", api.url());
        QtLLM::Scripting scripting(&client);
        Db db;
        registerDb(scripting, db);

        const Outcome outcome = turn(client, "Archive everything.");
        TEST_ASSERT(outcome.errors.isEmpty());
        TEST_COMPARE(outcome.response, QString("All six are archived."));
        TEST_COMPARE(outcome.toolResults.size(), 3);
        TEST_COMPARE(outcome.toolResults[0]["phase"].toString(), QString("syntax"));
        TEST_COMPARE(outcome.toolResults[0]["line"].toInt(), 1);
        TEST_COMPARE(outcome.toolResults[1]["status"].toString(), QString("ok"));
        TEST_COMPARE(outcome.toolResults[1]["result"].toInt(), 6);
        TEST_COMPARE(outcome.toolResults[2]["result"].toInt(), 42);
        TEST_COMPARE(db.archived(), 6);

        // The script's result is all that went back - not six calls.
        TEST_COMPARE(api.messageRequests, 4);
        TEST_ASSERT(scripting.runs().last().external);
        TEST_COMPARE(scripting.runs().size(), 2);       // the broken one never ran
    }

    TEST_FUNCTION(testApprovalAlways)
    {
        TEST_START;

        FakeApi api;
        QtLLM::Client client("key", api.url());
        QtLLM::Scripting scripting(&client);
        Db db;
        registerDb(scripting, db);
        scripting.setApproval(QtLLM::Scripting::Approval::Always);

        QList<QtLLM::Scripting::ApprovalRequest> requests;
        QtLLM::Scripting::ApprovalDecision answer;
        scripting.setApprovalHandler([&](const QtLLM::Scripting::ApprovalRequest& request,
                                         std::function<void(const QtLLM::Scripting::ApprovalDecision&)> decide) {
            requests << request;
            // Like a window of the application would: later, not from inside the call.
            QTimer::singleShot(10, qApp, [decide, a = answer]() { decide(a); });
        });

        const QString code = "db.setStatus('o1', 'archived');\nreturn db.read('o1').status;";

        // Declined, with a word for the model.
        answer.approved = false;
        answer.reason = "leave o1 alone";
        api.replies.enqueue(scriptCall(code, QJsonObject{{"args", QJsonObject{{"x", 1}}}}));
        api.replies.enqueue(FakeApi::textReply("ok"));
        Outcome outcome = turn(client, "go");
        TEST_COMPARE(requests.size(), 1);
        TEST_COMPARE(requests[0].code, code);
        TEST_COMPARE(requests[0].args["x"].toInt(), 1);
        TEST_COMPARE(requests[0].methods.size(), 2);
        TEST_COMPARE(requests[0].changingMethods, QStringList{"db.setStatus"});
        TEST_COMPARE(outcome.toolResults.size(), 1);
        TEST_COMPARE(outcome.toolResults[0]["phase"].toString(), QString("declined"));
        TEST_COMPARE(outcome.toolResults[0]["reason"].toString(), QString("leave o1 alone"));
        TEST_COMPARE(db.writes, 0);
        TEST_ASSERT(scripting.runs().isEmpty());

        // Approved, and not to be asked again for this script.
        answer.approved = true;
        answer.reason.clear();
        answer.rememberForSession = true;
        api.replies.enqueue(scriptCall(code));
        api.replies.enqueue(FakeApi::textReply("ok"));
        outcome = turn(client, "go");
        TEST_COMPARE(requests.size(), 2);
        TEST_COMPARE(outcome.toolResults[0]["result"].toString(), QString("archived"));

        api.replies.enqueue(scriptCall(code));
        api.replies.enqueue(FakeApi::textReply("ok"));
        outcome = turn(client, "again");
        TEST_COMPARE(requests.size(), 2);                    // same code: not asked
        TEST_COMPARE(outcome.toolResults[0]["status"].toString(), QString("ok"));

        // "Always" means reading scripts too, and dry runs.
        api.replies.enqueue(scriptCall("return db.list().length;", QJsonObject{{"dry_run", true}}));
        api.replies.enqueue(FakeApi::textReply("ok"));
        outcome = turn(client, "count");
        TEST_COMPARE(requests.size(), 3);
        TEST_ASSERT(requests[2].dryRun);
        TEST_ASSERT(requests[2].changingMethods.isEmpty());

        // A script with a mistake is not shown to the user at all.
        api.replies.enqueue(scriptCall("return db.nothing();"));
        api.replies.enqueue(FakeApi::textReply("ok"));
        outcome = turn(client, "broken");
        TEST_COMPARE(requests.size(), 3);
        TEST_COMPARE(outcome.toolResults[0]["phase"].toString(), QString("check"));
    }

    TEST_FUNCTION(testApprovalForChangingScripts)
    {
        TEST_START;

        FakeApi api;
        QtLLM::Client client("key", api.url());
        QtLLM::Scripting scripting(&client);
        Db db;
        registerDb(scripting, db);
        scripting.setApproval(QtLLM::Scripting::Approval::ChangingScripts);

        int asked = 0;
        bool dynamicSeen = false;
        scripting.setApprovalHandler([&](const QtLLM::Scripting::ApprovalRequest& request,
                                         std::function<void(const QtLLM::Scripting::ApprovalDecision&)> decide) {
            ++asked;
            dynamicSeen = request.dynamic;
            QtLLM::Scripting::ApprovalDecision decision;
            decision.approved = true;
            decide(decision);
        });

        // Only reads: runs unasked.
        api.replies.enqueue(scriptCall("return db.list().map(id => db.read(id).age);"));
        api.replies.enqueue(FakeApi::textReply("ok"));
        Outcome outcome = turn(client, "ages");
        TEST_COMPARE(asked, 0);
        TEST_COMPARE(outcome.toolResults[0]["result"].toArray().size(), 6);

        // Changes something: asked.
        api.replies.enqueue(scriptCall("db.setStatus('o1', 'archived');\nreturn 1;"));
        api.replies.enqueue(FakeApi::textReply("ok"));
        outcome = turn(client, "archive");
        TEST_COMPARE(asked, 1);
        TEST_ASSERT(!dynamicSeen);
        TEST_COMPARE(db.archived(), 1);

        // By a computed name: asked, and the user is told why.
        api.replies.enqueue(scriptCall("db['set' + 'Status']('o2', 'archived');\nreturn 1;"));
        api.replies.enqueue(FakeApi::textReply("ok"));
        outcome = turn(client, "archive");
        TEST_COMPARE(asked, 2);
        TEST_ASSERT(dynamicSeen);
        TEST_COMPARE(db.archived(), 2);

        // A dry run changes nothing, so nobody is asked.
        api.replies.enqueue(scriptCall("db.setStatus('o3', 'archived');\nreturn 1;", QJsonObject{{"dry_run", true}}));
        api.replies.enqueue(FakeApi::textReply("ok"));
        outcome = turn(client, "try");
        TEST_COMPARE(asked, 2);
        TEST_COMPARE(db.archived(), 2);

        // A script that hides what it calls from the source gets past the
        // question - and is stopped when it tries to change something.
        const QString sneaky = "const d = [].constructor.constructor('return d' + 'b')();\n"
                               "d['set' + 'Status']('o4', 'archived');\nreturn 'done';";
        TEST_ASSERT(!scripting.check(sneaky).dynamic);       // the scanner does not see it
        api.replies.enqueue(scriptCall(sneaky));
        api.replies.enqueue(FakeApi::textReply("ok"));
        outcome = turn(client, "sneak");
        TEST_COMPARE(asked, 2);
        TEST_COMPARE(outcome.toolResults[0]["status"].toString(), QString("error"));
        TEST_COMPARE(outcome.toolResults[0]["phase"].toString(), QString("approval"));
        TEST_COMPARE(db.archived(), 2);
    }

    // The built-in dialog: shows the script, and Decline carries the reason.
    TEST_FUNCTION(testApprovalDialog)
    {
        TEST_START;

        FakeApi api;
        QtLLM::Client client("key", api.url());
        QtLLM::Scripting scripting(&client);
        Db db;
        registerDb(scripting, db);
        scripting.setApproval(QtLLM::Scripting::Approval::Always);

        auto withDialog = [](std::function<void(QDialog*)> act) {
            QTimer::singleShot(100, qApp, [act]() {
                for (QWidget* widget : QApplication::topLevelWidgets()) {
                    if (widget->objectName() == "scripthost_scriptApprovalDialog" && widget->isVisible()) {
                        act(qobject_cast<QDialog*>(widget));
                        return;
                    }
                }
            });
        };

        const QString code = "db.setStatus('o1', 'archived');\nreturn 1;";
        QString shownCode, shownUses;
        withDialog([&](QDialog* dialog) {
            shownCode = dialog->findChild<QPlainTextEdit*>("scripthost_scriptApprovalCode")->toPlainText();
            shownUses = dialog->findChild<QLabel*>("scripthost_scriptApprovalUses")->text();
            if (qEnvironmentVariableIsSet("QTLLM_TEST_GRABS"))
                dialog->grab().save(qEnvironmentVariable("QTLLM_TEST_GRABS") + "/script_approval.png");
            dialog->findChild<QLineEdit*>("scripthost_scriptApprovalReason")->setText("not today");
            dialog->findChild<QPushButton*>("scripthost_scriptApprovalDecline")->click();
        });
        api.replies.enqueue(scriptCall(code));
        api.replies.enqueue(FakeApi::textReply("ok"));
        Outcome outcome = turn(client, "go");
        TEST_COMPARE(shownCode, code);
        TEST_ASSERT(shownUses.contains("db.setStatus (changes)"));
        TEST_COMPARE(outcome.toolResults[0]["phase"].toString(), QString("declined"));
        TEST_COMPARE(outcome.toolResults[0]["reason"].toString(), QString("not today"));
        TEST_COMPARE(db.writes, 0);

        // Test run inside the dialog, then Run.
        QString testOutput;
        withDialog([&](QDialog* dialog) {
            dialog->findChild<QPushButton*>("scripthost_scriptApprovalTest")->click();
            auto* output = dialog->findChild<QPlainTextEdit*>("scripthost_scriptApprovalOutput");
            QTimer::singleShot(600, dialog, [&testOutput, output, dialog]() {
                testOutput = output->toPlainText();
                dialog->findChild<QPushButton*>("scripthost_scriptApprovalRun")->click();
            });
        });
        api.replies.enqueue(scriptCall(code));
        api.replies.enqueue(FakeApi::textReply("ok"));
        outcome = turn(client, "go");
        TEST_ASSERT(testOutput.contains("would_call"));
        TEST_COMPARE(outcome.toolResults[0]["status"].toString(), QString("ok"));
        TEST_COMPARE(db.writes, 1);         // once: the test run changed nothing
    }

    TEST_FUNCTION(testUserSettingsAndLocks)
    {
        TEST_START;

        FakeApi api;
        QtLLM::Client client("key", api.url());
        QtLLM::Scripting scripting(&client);
        Db db;
        registerDb(scripting, db);
        int settingsChanges = 0;
        QObject::connect(&scripting, &QtLLM::Scripting::settingsChanged, [&]() { ++settingsChanges; });

        // The user's choices, out and back in.
        TEST_ASSERT(scripting.setEnabledByUser(false));
        TEST_ASSERT(scripting.setApprovalRequiredByUser(QtLLM::Scripting::ScriptKind::NewChanging, true));
        TEST_ASSERT(scripting.setMethodEnabledByUser("db.setStatus", false));
        TEST_ASSERT(!scripting.setMethodEnabledByUser("db.unknown", false));
        const QJsonObject saved = scripting.userSettings();
        TEST_COMPARE(saved["enabled"].toBool(), false);
        TEST_COMPARE(saved["approval"].toObject()["new_changing"].toBool(), true);
        TEST_COMPARE(saved["approval"].toObject()["saved_changing"].toBool(), false);
        TEST_COMPARE(saved["methods"].toObject()["db.setStatus"].toBool(), false);
        TEST_COMPARE(saved["methods"].toObject()["db.read"].toBool(), true);

        QtLLM::Scripting other(&client);
        Db db2;
        registerDb(other, db2);
        other.applyUserSettings(saved);
        TEST_ASSERT(!other.isEnabled());
        TEST_ASSERT(other.isApprovalRequired(QtLLM::Scripting::ScriptKind::NewChanging));
        TEST_ASSERT(!other.isApprovalRequired(QtLLM::Scripting::ScriptKind::SavedChanging));
        TEST_ASSERT(!other.isMethodEnabled("db.setStatus"));
        TEST_ASSERT(other.isMethodEnabled("db.read"));

        // What the application fixes, the user cannot change - and stored
        // choices from before do not get around it.
        QtLLM::Scripting locked(&client);
        Db db3;
        registerDb(locked, db3);
        locked.setEnabledUserConfigurable(false);
        locked.setApproval(QtLLM::Scripting::Approval::Always);
        locked.setApprovalUserConfigurable(false);
        locked.setMethodEnabled("db.setStatus", false);
        locked.setMethodUserConfigurable("db.setStatus", false);
        locked.setMethodGroupUserConfigurable("Script", false);
        TEST_ASSERT(!locked.setEnabledByUser(false));
        TEST_ASSERT(!locked.setApprovalRequiredByUser(QtLLM::Scripting::ScriptKind::DryRun, false));
        TEST_ASSERT(!locked.setMethodEnabledByUser("db.setStatus", true));
        TEST_ASSERT(!locked.setMethodEnabledByUser("log", false));
        TEST_ASSERT(locked.setMethodEnabledByUser("db.read", false));
        TEST_ASSERT(!locked.isMethodUserConfigurable("db.setStatus"));
        TEST_ASSERT(locked.isMethodUserConfigurable("db.read"));

        locked.applyUserSettings(QJsonObject{{"enabled", false}, {"approval", "never"},
            {"methods", QJsonObject{{"db.setStatus", true}, {"db.list", false}}}});
        TEST_ASSERT(locked.isEnabled());
        TEST_ASSERT(locked.isApprovalRequired(QtLLM::Scripting::ScriptKind::NewReading));
        TEST_ASSERT(!locked.isMethodEnabled("db.setStatus"));
        TEST_ASSERT(!locked.isMethodEnabled("db.list"));
        const QJsonObject lockedSettings = locked.userSettings();
        TEST_ASSERT(!lockedSettings.contains("enabled"));
        TEST_ASSERT(!lockedSettings.contains("approval"));
        TEST_ASSERT(!lockedSettings["methods"].toObject().contains("db.setStatus"));
        TEST_ASSERT(!lockedSettings["methods"].toObject().contains("log"));
        TEST_ASSERT(settingsChanges >= 1);
    }

    // The debugger: stop before each call, see what goes in and comes out.
    TEST_FUNCTION(testCallStepping)
    {
        TEST_START;

        FakeApi api;
        QtLLM::Client client("key", api.url());
        QtLLM::Scripting scripting(&client);
        Db db;
        registerDb(scripting, db);

        struct Stop { int line; QString method; QJsonArray args; };
        QList<Stop> stops;
        QStringList returned;
        bool autoStep = true;
        QObject::connect(&scripting, &QtLLM::Scripting::runPaused,
                         [&](int line, const QString& method, const QJsonArray& args) {
            stops.append({line, method, args});
            if (autoStep)
                scripting.stepRun();
            else
                scripting.continueRun();
        });
        QObject::connect(&scripting, &QtLLM::Scripting::runCallFinished,
                         [&](int line, const QString& method, const QJsonArray&, const QJsonValue& value,
                             const QString& error) {
            returned << QString("%1:%2:%3").arg(line).arg(method,
                error.isEmpty() ? QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact)).left(24)
                                : error);
        });

        const QString code =
            "const ids = db.list();\n"                       // 1
            "let n = 0;\n"                                   // 2
            "for (const id of ids.slice(0, 2)) {\n"          // 3
            "    const o = db.read(id);\n"                   // 4
            "    if (o.age > 10)\n"                          // 5
            "        db.setStatus(id, 'archived');\n"        // 6
            "    n++;\n"                                     // 7
            "}\n"                                            // 8
            "pause('before the end');\n"                     // 9
            "return n;\n";                                   // 10

        // Step by step from the first call.
        Options options;
        options.debug = true;
        options.stepFromStart = true;
        QJsonObject result = runScript(scripting, code, options);
        TEST_COMPARE(result["result"].toInt(), 2);
        TEST_COMPARE(stops.size(), 5);
        TEST_COMPARE(stops[0].line, 1);
        TEST_COMPARE(stops[0].method, QString("db.list"));
        TEST_COMPARE(stops[1].line, 4);
        TEST_COMPARE(stops[1].method, QString("db.read"));
        TEST_COMPARE(stops[1].args[0].toString(), QString("o1"));
        TEST_COMPARE(stops[2].line, 4);
        TEST_COMPARE(stops[3].line, 6);
        TEST_COMPARE(stops[3].method, QString("db.setStatus"));
        TEST_COMPARE(stops[3].args[1].toString(), QString("archived"));
        TEST_COMPARE(stops[4].line, 9);
        TEST_COMPARE(stops[4].method, QString("pause"));
        TEST_COMPARE(returned.size(), 4);
        TEST_ASSERT(returned[0].startsWith("1:db.list:[[\"o1\""));
        TEST_ASSERT(returned[3].startsWith("6:db.setStatus:"));

        // Breakpoint: runs up to the calls on that line only.
        stops.clear();
        autoStep = false;
        Options breakpoint;
        breakpoint.debug = true;
        breakpoint.breakpoints = QSet<int>{4};
        result = runScript(scripting, code, breakpoint);
        TEST_COMPARE(result["result"].toInt(), 2);
        TEST_COMPARE(stops.size(), 3);                      // two reads and pause()
        TEST_COMPARE(stops[0].line, 4);
        TEST_COMPARE(stops[1].line, 4);
        TEST_COMPARE(stops[2].method, QString("pause"));

        // Stopping while paused ends the script; no time limit applies to a
        // script that waits for the user.
        QtLLM::Scripting::Limits limits;
        limits.timeoutMs = 200;
        scripting.setLimits(limits);
        QObject::disconnect(&scripting, &QtLLM::Scripting::runPaused, nullptr, nullptr);
        bool paused = false;
        QObject::connect(&scripting, &QtLLM::Scripting::runPaused, [&]() { paused = true; });
        bool done = false;
        scripting.run(code, options, [&](const QJsonObject& r) {
            result = r;
            done = true;
        });
        TEST_ASSERT(spin([&]() { return paused; }, 5000));
        spin([]() { return false; }, 500);
        TEST_ASSERT(scripting.isRunning());                 // still waiting, not timed out
        scripting.stop();
        TEST_ASSERT(spin([&]() { return done; }, 5000));
        TEST_COMPARE(result["phase"].toString(), QString("cancelled"));

        // Outside the debugger pause() does nothing.
        QObject::disconnect(&scripting, &QtLLM::Scripting::runPaused, nullptr, nullptr);
        limits.timeoutMs = 300000;
        scripting.setLimits(limits);
        TEST_COMPARE(runScript(scripting, "pause('x');\nreturn 7;")["result"].toInt(), 7);
    }

    // Saved scripts are plain files.
    TEST_FUNCTION(testScriptFiles)
    {
        TEST_START;

        QTemporaryDir userDir, shippedDir;
        TEST_ASSERT(userDir.isValid() && shippedDir.isValid());

        QtLLM::ScriptLibrary::Script script;
        script.name = "Archive old objects";
        script.description = "Archives every object\nolder than `days`.";
        script.parameters = {{"days", "number", "Age in days"}, {"dryRun", "boolean", ""}};
        script.code = "for (const id of db.list()) {\n    // ...\n}\nreturn 1;";
        script.origin = "user";

        const QString text = QtLLM::ScriptLibrary::toFileText(script);
        TEST_ASSERT(text.startsWith("// @name Archive old objects\n"
                                    "// @description Archives every object older than `days`.\n"
                                    "// @param days number Age in days\n"
                                    "// @param dryRun boolean\n"
                                    "// @origin user\n\n"
                                    "for (const id of db.list()) {\n"));
        const QtLLM::ScriptLibrary::Script parsed = QtLLM::ScriptLibrary::fromFileText(text, "fallback");
        TEST_COMPARE(parsed.name, script.name);
        TEST_COMPARE(parsed.description, QString("Archives every object older than `days`."));
        TEST_COMPARE(parsed.parameters.size(), 2);
        TEST_COMPARE(parsed.parameters[0].name, QString("days"));
        TEST_COMPARE(parsed.parameters[0].type, QString("number"));
        TEST_COMPARE(parsed.parameters[0].description, QString("Age in days"));
        TEST_COMPARE(parsed.code, script.code);
        // A file without a header is a script named after the file.
        const QtLLM::ScriptLibrary::Script bare = QtLLM::ScriptLibrary::fromFileText("return 1;\r\n", "quick-count");
        TEST_COMPARE(bare.name, QString("quick-count"));
        TEST_COMPARE(bare.code, QString("return 1;"));
        TEST_COMPARE(QtLLM::ScriptLibrary::fileBaseName("  Archive: old Objects! "), QString("archive-old-objects"));

        QtLLM::ScriptLibrary library;
        int changes = 0;
        QObject::connect(&library, &QtLLM::ScriptLibrary::changed, [&]() { ++changes; });
        QString error;
        TEST_ASSERT(!library.save(script, &error));                 // nowhere to save
        TEST_ASSERT(!error.isEmpty());

        library.setDirectory(userDir.path() + "/scripts");          // created on demand
        TEST_ASSERT(library.save(script, &error));
        TEST_ASSERT(QFile::exists(userDir.path() + "/scripts/archive-old-objects.js"));
        TEST_COMPARE(library.scripts().size(), 1);
        TEST_ASSERT(library.contains("archive OLD objects"));       // by name, any case
        TEST_ASSERT(library.contains("archive-old-objects"));       // by file name
        TEST_COMPARE(library.script("Archive old objects").code, script.code);
        TEST_ASSERT(!library.script("Archive old objects").readOnly);
        TEST_ASSERT(!library.script("nothing").isValid());

        // Saved again: replaced, not doubled.
        script.code = "return 2;";
        TEST_ASSERT(library.save(script));
        TEST_COMPARE(library.scripts().size(), 1);
        TEST_COMPARE(library.script("Archive old objects").code, QString("return 2;"));

        // Shipped with the application: listed, never changed.
        QFile shipped(shippedDir.path() + "/count.js");
        TEST_ASSERT(shipped.open(QIODevice::WriteOnly));
        shipped.write("// @name Count objects\n// @description Counts.\n\nreturn db.list().length;\n");
        shipped.close();
        QFile shadowed(shippedDir.path() + "/archive.js");
        TEST_ASSERT(shadowed.open(QIODevice::WriteOnly));
        shadowed.write("// @name Archive old objects\n\nreturn 'shipped';\n");
        shadowed.close();
        library.addReadOnlyDirectory(shippedDir.path());
        TEST_COMPARE(library.scripts().size(), 2);
        TEST_ASSERT(library.script("Count objects").readOnly);
        TEST_COMPARE(library.script("Count objects").origin, QString("application"));
        TEST_COMPARE(library.script("Archive old objects").code, QString("return 2;"));    // the user's wins
        TEST_ASSERT(!library.remove("Count objects", &error));
        TEST_ASSERT(QFile::exists(shippedDir.path() + "/count.js"));

        TEST_ASSERT(library.remove("Archive old objects"));
        TEST_COMPARE(library.script("Archive old objects").code, QString("return 'shipped';"));
        TEST_ASSERT(changes >= 4);
    }

    TEST_FUNCTION(testModelUsesSavedScripts)
    {
        TEST_START;

        QTemporaryDir dir;
        QtLLM::ScriptLibrary library;
        library.setDirectory(dir.path());
        QtLLM::ScriptLibrary::Script users;
        users.name = "Count old";
        users.description = "Counts objects older than minAge.";
        users.parameters = {{"minAge", "number", "Age"}};
        users.code = "return db.list().filter(id => db.read(id).age >= args.minAge).length;";
        users.origin = "user";
        TEST_ASSERT(library.save(users));

        FakeApi api;
        QtLLM::Client client("key", api.url());
        QtLLM::Scripting scripting(&client);
        scripting.setLibrary(&library);
        Db db;
        registerDb(scripting, db);

        auto call = [&](const QString& tool, const QJsonObject& input) {
            QJsonObject result;
            bool done = false;
            client.callTool(tool, input, [&](const QJsonObject& r) {
                result = r;
                done = true;
            });
            spin([&]() { return done; });
            return result;
        };

        // What there is, and running one by name.
        QJsonObject result = call("list_scripts", {});
        TEST_COMPARE(result["scripts"].toArray().size(), 1);
        TEST_COMPARE(result["scripts"].toArray()[0].toObject()["name"].toString(), QString("Count old"));
        TEST_COMPARE(result["scripts"].toArray()[0].toObject()["parameters"].toArray()[0].toObject()["name"].toString(),
                     QString("minAge"));
        result = call("read_script", QJsonObject{{"name", "count old"}});
        TEST_COMPARE(result["code"].toString(), users.code);
        result = call("run_script", QJsonObject{{"name", "Count old"}, {"args", QJsonObject{{"minAge", 50}}}});
        TEST_COMPARE(result["status"].toString(), QString("ok"));
        TEST_COMPARE(result["result"].toInt(), 2);
        TEST_COMPARE(scripting.runs().last().name, QString("Count old"));
        TEST_COMPARE(call("run_script", QJsonObject{{"name", "No such"}})["status"].toString(), QString("error"));
        TEST_COMPARE(call("read_script", QJsonObject{{"name", "No such"}})["status"].toString(), QString("error"));

        // Saving: checked first; a broken script is not saved.
        result = call("save_script", QJsonObject{{"name", "Broken"}, {"description", "x"},
                                                 {"code", "return db.nothing();"}});
        TEST_COMPARE(result["phase"].toString(), QString("check"));
        TEST_ASSERT(!library.contains("Broken"));

        result = call("save_script", QJsonObject{
            {"name", "Archive all"}, {"description", "Archives every object."},
            {"code", "for (const id of db.list()) db.setStatus(id, args.status);\nreturn 1;"},
            {"parameters", QJsonArray{QJsonObject{{"name", "status"}, {"type", "string"},
                                                  {"description", "New status"}}}}});
        TEST_COMPARE(result["status"].toString(), QString("ok"));
        TEST_COMPARE(library.script("Archive all").origin, QString("assistant"));
        TEST_COMPARE(library.script("Archive all").parameters.size(), 1);

        // Its own script it may replace; the user's it may not.
        result = call("save_script", QJsonObject{{"name", "Archive all"}, {"description", "Shorter."},
                                                 {"code", "return 2;"}});
        TEST_COMPARE(result["status"].toString(), QString("ok"));
        TEST_COMPARE(library.script("Archive all").code, QString("return 2;"));
        result = call("save_script", QJsonObject{{"name", "Count old"}, {"description", "Mine now."},
                                                 {"code", "return 0;"}});
        TEST_COMPARE(result["status"].toString(), QString("error"));
        TEST_COMPARE(library.script("Count old").code, users.code);
    }

    // llm.ask / llm.choose: a question to a model in the middle of a script.
    TEST_FUNCTION(testAskingAModelFromAScript)
    {
        TEST_START;

        FakeApi chatApi, helperApi;
        QtLLM::Client client("key", chatApi.url());
        QtLLM::Scripting scripting(&client);
        TEST_ASSERT(!scripting.hasLlm());
        TEST_ASSERT(!scripting.hasMethod("llm.ask"));

        scripting.setLlmConfigProvider([&]() {
            QtLLM::AgentConfig config;
            config.provider = QtLLM::Provider::Claude;
            config.url = helperApi.url();
            config.apiKey = "key";
            config.model = "test-model";
            return config;
        });
        TEST_ASSERT(scripting.hasLlm());
        TEST_ASSERT(scripting.hasMethod("llm.ask"));
        TEST_ASSERT(scripting.hasMethod("llm.choose"));
        TEST_ASSERT(scripting.apiDefinition().contains("declare namespace llm {"));
        TEST_ASSERT(scripting.apiDefinition().contains("function choose(prompt: string, options: string[], data?: any): string;"));

        helperApi.replies.enqueue(FakeApi::textReply(" outdated \n"));
        helperApi.replies.enqueue(FakeApi::textReply("I would say Keep."));
        helperApi.replies.enqueue(FakeApi::textReply("maybe"));
        QJsonObject result = runScript(scripting,
            "const a = llm.ask('Is this outdated? One word.', {name: 'Pump', year: 1990});\n"
            "const b = llm.choose('Keep or drop?', ['keep', 'drop'], 'an old pump');\n"
            "let c;\n"
            "try { c = llm.choose('Keep or drop?', ['keep', 'drop']); } catch (e) { c = 'failed: ' + e.message; }\n"
            "return [a, b, c];\n");
        TEST_COMPARE(result["status"].toString(), QString("ok"));
        const QJsonArray answers = result["result"].toArray();
        TEST_COMPARE(answers[0].toString(), QString("outdated"));
        TEST_COMPARE(answers[1].toString(), QString("keep"));                  // the one option the answer names
        TEST_ASSERT(answers[2].toString().startsWith("failed: llm.choose: the model answered \"maybe\""));
        TEST_COMPARE(result["calls"].toObject()["llm.choose"].toInt(), 2);

        // Every question is a conversation of its own, with the data in it.
        TEST_COMPARE(helperApi.messageRequests, 3);
        for (int i = 0; i < 3; ++i) {
            const QJsonObject request = QJsonDocument::fromJson(helperApi.messageBodies.value(i)).object();
            TEST_COMPARE(request["messages"].toArray().size(), 1);
            TEST_ASSERT(request["tools"].toArray().isEmpty());
        }
        TEST_ASSERT(helperApi.messageBodies[0].contains("1990"));
        TEST_ASSERT(helperApi.messageBodies[1].contains("- drop"));
        TEST_ASSERT(helperApi.messageBodies[1].contains("an old pump"));
        TEST_COMPARE(chatApi.messageRequests, 0);

        // Their own limit.
        scripting.setLlmCallLimit(2);
        for (int i = 0; i < 3; ++i)
            helperApi.replies.enqueue(FakeApi::textReply("yes"));
        result = runScript(scripting, "let n = 0;\nfor (let i = 0; i < 5; i++) { llm.ask('ok?'); n++; }\nreturn n;");
        TEST_COMPARE(result["phase"].toString(), QString("limit"));
        TEST_ASSERT(result["message"].toString().contains("llm.ask"));
        TEST_COMPARE(helperApi.messageRequests, 5);

        // The helper is held to the chat's model list; a refused question is
        // a script error, not a hang.
        helperApi.replies.clear();
        client.setModelFilter(QStringList{"some-other-model"});
        result = runScript(scripting, "return llm.ask('ok?');");
        TEST_COMPARE(result["status"].toString(), QString("error"));
        TEST_ASSERT(result["message"].toString().startsWith("llm.ask: "));
        TEST_COMPARE(helperApi.messageRequests, 5);
        client.setModelFilter(QStringList());

        scripting.setLlmConfigProvider(nullptr);
        TEST_ASSERT(!scripting.hasMethod("llm.ask"));
        TEST_ASSERT(!scripting.hasMethod("llm.choose"));
    }
};

TEST_INSTANTIATE(TST_Scripting);
