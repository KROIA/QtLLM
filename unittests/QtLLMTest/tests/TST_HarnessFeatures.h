#pragma once

// Features an application switches by code: which tools exist and who may
// change that, tools on demand, cost limit, one-tool-at-a-time, pictures in
// prompts, taking the last exchange back, continuing a saved conversation,
// the application's own help topics, and the chat / settings widgets for them.

#include "UnitTest.h"
#include "QtLLM.h"
#include "FakeApi.h"
#include "FakeOpenAi.h"
#include "ToolSettingsWidget.h"
#include "AppKnowledge.h"
#include <QCheckBox>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <functional>

class TST_HarnessFeatures : public UnitTest::Test
{
    TEST_CLASS(TST_HarnessFeatures)
public:
    TST_HarnessFeatures()
        : Test("TST_HarnessFeatures")
    {
        ADD_TEST(TST_HarnessFeatures::testToolSwitchesSurviveRegistration);
        ADD_TEST(TST_HarnessFeatures::testGroupSwitchAndToolOverride);
        ADD_TEST(TST_HarnessFeatures::testLockedToolsCannotBeSwitchedByTheUser);
        ADD_TEST(TST_HarnessFeatures::testDeferredToolsAreFoundAndCalled);
        ADD_TEST(TST_HarnessFeatures::testCallToolChecksArguments);
        ADD_TEST(TST_HarnessFeatures::testCostLimitStopsSending);
        ADD_TEST(TST_HarnessFeatures::testParallelToolCallsSwitch);
        ADD_TEST(TST_HarnessFeatures::testPromptWithPictures);
        ADD_TEST(TST_HarnessFeatures::testRemoveLastExchange);
        ADD_TEST(TST_HarnessFeatures::testImportConversation);
        ADD_TEST(TST_HarnessFeatures::testOpenAiProviderThroughClient);
        ADD_TEST(TST_HarnessFeatures::testKnowledgeTopics);
        ADD_TEST(TST_HarnessFeatures::testToolSettingsWidget);
        ADD_TEST(TST_HarnessFeatures::testSettingsDialogTabsAndProviders);
        ADD_TEST(TST_HarnessFeatures::testChatRegenerateAndEdit);
        ADD_TEST(TST_HarnessFeatures::testChatPicturesAndLoad);
    }

private:
    static bool spin(const std::function<bool()>& condition, int timeoutMs = 5000)
    {
        QEventLoop loop;
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, [&]() {
            if (condition())
                loop.quit();
        });
        poll.start(10);
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
        if (!condition())
            loop.exec();
        return condition();
    }

    struct Outcome
    {
        QString     response;
        QStringList errors;
    };

    // One prompt, until the answer or an error is there.
    static Outcome turn(QtLLM::Client& client, const QString& prompt,
                        const QList<QImage>& images = QList<QImage>())
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
        client.sendPrompt(prompt, images);
        spin([&]() { return done; });
        QObject::disconnect(c1);
        QObject::disconnect(c2);
        return outcome;
    }

    static QJsonObject body(const FakeApi& api, int index)
    {
        return QJsonDocument::fromJson(api.messageBodies.value(index)).object();
    }

    static QtLLM::Tool tool(const QString& name, const QString& group = QString(),
                            const QString& description = "does something")
    {
        QtLLM::Tool t;
        t.setName(name).setDescription(description).setGroup(group);
        return t;
    }

    static QJsonObject ok(const QJsonObject&) { return QtLLM::toolOk(); }

    // ------------------------------------------------------------ tool policy

    // The application states its policy once; it holds whenever the tool is
    // registered - before, after, or again.
    TEST_FUNCTION(testToolSwitchesSurviveRegistration)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        client.setToolEnabled("later", false);
        client.setToolUserConfigurable("later", false);
        TEST_ASSERT(!client.isToolEnabled("later"));           // unknown tools are "not enabled"

        client.registerTool(tool("later"), ok);
        TEST_ASSERT(!client.isToolEnabled("later"));
        TEST_ASSERT(!client.isToolUserConfigurable("later"));
        TEST_ASSERT(client.enabledToolNames().isEmpty());

        client.unregisterTool("later");
        client.registerTool(tool("later"), ok);
        TEST_ASSERT(!client.isToolEnabled("later"));

        client.setToolEnabled("later", true);
        TEST_COMPARE(client.enabledToolNames(), QStringList{"later"});

        int changes = 0;
        QObject::connect(&client, &QtLLM::Client::toolsChanged, [&]() { ++changes; });
        client.setToolEnabled("later", false);
        client.registerTool(tool("other"), ok);
        TEST_COMPARE(changes, 2);
    }

    TEST_FUNCTION(testGroupSwitchAndToolOverride)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        client.registerTool(tool("file_read", "Files"), ok);
        client.registerTool(tool("file_write", "Files"), ok);
        client.registerTool(tool("clock", "System"), ok);

        client.setToolGroupEnabled("Files", false);
        TEST_COMPARE(client.enabledToolNames(), QStringList{"clock"});

        // A tool's own switch wins over its group's, in either order.
        client.setToolEnabled("file_read", true);
        TEST_COMPARE(client.enabledToolNames(), QStringList({"clock", "file_read"}));
        client.setToolGroupEnabled("Files", false);
        TEST_ASSERT(client.isToolEnabled("file_read"));

        // A tool that joins the group later follows the group.
        client.registerTool(tool("file_delete", "Files"), ok);
        TEST_ASSERT(!client.isToolEnabled("file_delete"));
    }

    // "The application decides which tools the user may switch."
    TEST_FUNCTION(testLockedToolsCannotBeSwitchedByTheUser)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        client.registerTool(tool("free"), ok);
        client.registerTool(tool("forced_on"), ok);
        client.registerTool(tool("forced_off", "Danger"), ok);
        client.setToolUserConfigurable("forced_on", false);
        client.setToolGroupUserConfigurable("Danger", false);
        client.setToolGroupEnabled("Danger", false);

        TEST_ASSERT(client.setToolEnabledByUser("free", false));
        TEST_ASSERT(!client.isToolEnabled("free"));
        TEST_ASSERT(!client.setToolEnabledByUser("forced_on", false));
        TEST_ASSERT(client.isToolEnabled("forced_on"));
        TEST_ASSERT(!client.setToolEnabledByUser("forced_off", true));
        TEST_ASSERT(!client.isToolEnabled("forced_off"));
        TEST_ASSERT(!client.setToolEnabledByUser("unknown", true));

        // What gets stored for the user contains only what the user may decide...
        const QJsonObject stored = client.userToolSettings();
        TEST_COMPARE(stored.keys(), QStringList{"free"});
        TEST_ASSERT(stored["free"].toBool() == false);

        // ... and a stored file cannot unlock anything.
        client.applyUserToolSettings({{"free", true}, {"forced_on", false}, {"forced_off", true},
                                      {"gone", true}});
        TEST_ASSERT(client.isToolEnabled("free"));
        TEST_ASSERT(client.isToolEnabled("forced_on"));
        TEST_ASSERT(!client.isToolEnabled("forced_off"));

        // The application itself is not bound by the lock.
        client.setToolEnabled("forced_on", false);
        TEST_ASSERT(!client.isToolEnabled("forced_on"));
    }

    // ------------------------------------------------------------ tools on demand

    TEST_FUNCTION(testDeferredToolsAreFoundAndCalled)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::toolUseReply("find_tools", QJsonObject{{"query", "zebra report"}}));
        api.replies.enqueue(FakeApi::toolUseReply("call_tool", QJsonObject{
            {"name", "alpha_report"}, {"arguments", QJsonObject{{"n", "5"}}}}, "t2"));
        api.replies.enqueue(FakeApi::textReply("done"));

        QtLLM::Client client("key", api.url());
        int received = -1;
        QtLLM::Tool report = tool("alpha_report", "Reports", "Produces the quarterly zebra report.");
        report.addParameter("n", "integer", "How many quarters.", true);
        client.registerTool(report, [&](const QJsonObject& input) {
            received = input["n"].toInt();
            return QtLLM::toolOk({{"pages", 12}});
        });
        client.registerTool(tool("beta", "Reports", "Always listed."), ok);
        client.registerTool(tool("gamma_off", "Reports", "Deferred and disabled."), ok);
        client.setToolDeferred("alpha_report", true);
        client.setToolDeferred("gamma_off", true);
        client.setToolEnabled("gamma_off", false);
        TEST_ASSERT(client.isToolDeferred("alpha_report"));

        QStringList invoked;
        QObject::connect(&client, &QtLLM::Client::toolInvoked,
                         [&](const QString& name, const QJsonObject&) { invoked.append(name); });

        const Outcome outcome = turn(client, "make the report");
        TEST_ASSERT(outcome.errors.isEmpty());
        TEST_COMPARE(outcome.response, QString("done"));

        // Listed: beta and the two access tools. Not listed: the deferred one.
        QStringList listed;
        for (const QJsonValue& entry : body(api, 0)["tools"].toArray())
            listed.append(entry.toObject()["name"].toString());
        listed.sort();
        TEST_COMPARE(listed, QStringList({"beta", "call_tool", "find_tools"}));
        TEST_ASSERT(!api.messageBodies.value(0).contains("zebra"));
        TEST_ASSERT(api.messageBodies.value(0).contains("Reports (1)"));   // what is hidden, by group

        // The search result carries the definition; the disabled tool stays out.
        TEST_ASSERT(api.messageBodies.value(1).contains("alpha_report"));
        TEST_ASSERT(api.messageBodies.value(1).contains("zebra"));
        TEST_ASSERT(!api.messageBodies.value(1).contains("gamma_off"));

        // The call went through the real handler, input repaired ("5" -> 5),
        // and shows up under the tool's own name.
        TEST_COMPARE(received, 5);
        TEST_ASSERT(api.messageBodies.value(2).contains("pages"));
        TEST_ASSERT(invoked.contains("alpha_report"));

        // The tool list is the same in every request: the prompt cache holds.
        TEST_ASSERT(body(api, 0)["tools"] == body(api, 2)["tools"]);
    }

    TEST_FUNCTION(testCallToolChecksArguments)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::toolUseReply("call_tool", QJsonObject{
            {"name", "alpha_report"}, {"arguments", QJsonObject{{"quarters", 5}}}}));
        api.replies.enqueue(FakeApi::toolUseReply("call_tool", QJsonObject{{"name", "nothing"}}, "t2"));
        api.replies.enqueue(FakeApi::textReply("done"));

        QtLLM::Client client("key", api.url());
        int calls = 0;
        QtLLM::Tool report = tool("alpha_report");
        report.addParameter("n", "integer", "How many quarters.", true);
        client.registerTool(report, [&](const QJsonObject&) { ++calls; return QtLLM::toolOk(); });
        client.setToolGroupDeferred(QString(), true);   // tools without a group

        turn(client, "go");
        TEST_COMPARE(calls, 0);
        // The wrong call comes back with the parameters, so the model can
        // correct itself without searching again.
        TEST_ASSERT(api.messageBodies.value(1).contains("How many quarters"));
        TEST_ASSERT(api.messageBodies.value(2).contains("find_tools"));

        // Without deferred tools the two access tools are not there at all.
        client.setToolGroupDeferred(QString(), false);
        api.replies.enqueue(FakeApi::textReply("again"));
        turn(client, "go on");
        QStringList listed;
        for (const QJsonValue& entry : body(api, api.messageBodies.size() - 1)["tools"].toArray())
            listed.append(entry.toObject()["name"].toString());
        TEST_COMPARE(listed, QStringList{"alpha_report"});
    }

    // ------------------------------------------------------------ limits

    TEST_FUNCTION(testCostLimitStopsSending)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::textReply("one"));
        api.replies.enqueue(FakeApi::textReply("three"));

        QtLLM::Client client("key", api.url());
        client.setCostLimitUsd(0.000000001);
        double reported = 0.0;
        QObject::connect(&client, &QtLLM::Client::costLimitReached, [&](double spent) { reported = spent; });

        TEST_COMPARE(turn(client, "first").response, QString("one"));
        TEST_ASSERT(client.usageStats().sessionCostUsd > 0.0);

        const Outcome refused = turn(client, "second");
        TEST_COMPARE(refused.errors.size(), 1);
        TEST_ASSERT(refused.errors.value(0).contains("cost limit"));
        TEST_ASSERT(reported > 0.0);
        TEST_COMPARE(api.messageRequests, 1);
        TEST_COMPARE(client.conversationHistory().size(), 2);   // the refused prompt left no trace

        // A new conversation counts from zero.
        client.clearConversation();
        TEST_COMPARE(turn(client, "third").response, QString("three"));
    }

    TEST_FUNCTION(testParallelToolCallsSwitch)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::textReply("a"));
        api.replies.enqueue(FakeApi::textReply("b"));

        QtLLM::Client client("key", api.url());
        client.registerTool(tool("probe"), ok);
        turn(client, "one");
        TEST_ASSERT(!api.messageBodies.value(0).contains("disable_parallel_tool_use"));

        client.setParallelToolCalls(false);
        turn(client, "two");
        TEST_ASSERT(body(api, 1)["tool_choice"].toObject()["disable_parallel_tool_use"].toBool());
    }

    // ------------------------------------------------------------ pictures

    TEST_FUNCTION(testPromptWithPictures)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::textReply("a red square"));
        api.replies.enqueue(FakeApi::textReply("still red"));
        api.replies.enqueue(FakeApi::textReply("no picture"));

        QtLLM::Client client("key", api.url());
        TEST_ASSERT(client.supportsImageInput());
        QImage picture(8, 8, QImage::Format_RGB32);
        picture.fill(Qt::red);

        TEST_COMPARE(turn(client, "what is this?", {picture}).response, QString("a red square"));
        const QJsonArray content = body(api, 0)["messages"].toArray().last().toObject()["content"].toArray();
        TEST_COMPARE(content.size(), 2);
        TEST_COMPARE(content[0].toObject()["type"].toString(), QString("image"));
        TEST_ASSERT(!content[0].toObject()["source"].toObject()["data"].toString().isEmpty());
        TEST_COMPARE(content[1].toObject()["text"].toString(), QString("what is this?"));

        // Attached pictures go out with the next prompt, whoever sends it - once.
        client.attachImages({picture});
        turn(client, "and now?");
        auto lastPromptHasPicture = [&](int request) {
            const QJsonValue content = body(api, request)["messages"].toArray().last().toObject()["content"];
            for (const QJsonValue& block : content.toArray()) {
                if (block.toObject()["type"].toString() == "image")
                    return true;
            }
            return false;
        };
        TEST_ASSERT(lastPromptHasPicture(1));
        turn(client, "and without?");
        TEST_ASSERT(!lastPromptHasPicture(2));

        // A prompt with pictures still counts as the start of an exchange.
        TEST_COMPARE(client.removeLastExchange(), QString("and without?"));
        TEST_COMPARE(client.removeLastExchange(), QString("and now?"));

        // The subscription provider cannot take pictures: nothing is sent.
        QtLLM::Client cli(QtLLM::Provider::ClaudeCode, QString());
        TEST_ASSERT(!cli.supportsImageInput());
        const Outcome refused = turn(cli, "look", {picture});
        TEST_COMPARE(refused.errors.size(), 1);
        TEST_ASSERT(cli.conversationHistory().isEmpty());
    }

    // ------------------------------------------------------------ history

    TEST_FUNCTION(testRemoveLastExchange)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::textReply("answer one"));
        api.replies.enqueue(FakeApi::toolUseReply("probe", QJsonObject()));
        api.replies.enqueue(FakeApi::textReply("answer two"));
        api.replies.enqueue(FakeApi::textReply("answer three"));

        QtLLM::Client client("key", api.url());
        client.registerTool(tool("probe"), ok);
        TEST_ASSERT(client.removeLastExchange().isEmpty());        // nothing there yet

        turn(client, "question one");
        turn(client, "question two");                               // includes a tool round
        TEST_COMPARE(client.conversationHistory().size(), 4);

        TEST_COMPARE(client.removeLastExchange(), QString("question two"));
        TEST_COMPARE(client.conversationHistory().size(), 2);

        // What is sent next no longer contains it - neither the prompt nor
        // the tool round.
        turn(client, "question three");
        TEST_ASSERT(api.messageBodies.last().contains("question one"));
        TEST_ASSERT(!api.messageBodies.last().contains("question two"));
        TEST_ASSERT(!api.messageBodies.last().contains("tool_result"));

        // Not while a turn is running.
        api.silent = true;
        client.sendPrompt("question four");
        TEST_ASSERT(client.removeLastExchange().isEmpty());
        client.cancelRequest();
    }

    TEST_FUNCTION(testImportConversation)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::reply("end_turn", QJsonArray{
            QJsonObject{{"type", "thinking"}, {"thinking", "let me think"}, {"signature", "sig-abc"}},
            QJsonObject{{"type", "text"}, {"text", "answer one"}}}));
        api.replies.enqueue(FakeApi::textReply("answer two"));

        QJsonObject saved;
        {
            QtLLM::Client first("key", api.url());
            turn(first, "question one");
            saved = first.exportConversation();
        }

        QtLLM::Client second("key", api.url());
        TEST_ASSERT(second.importConversation(saved));
        TEST_COMPARE(second.conversationHistory().size(), 2);
        TEST_COMPARE(second.conversationHistory()[1].toObject()["content"].toString(), QString("answer one"));

        // It continues - without the thinking block, which belongs to the
        // conversation that produced it and would be rejected.
        TEST_COMPARE(turn(second, "question two").response, QString("answer two"));
        TEST_ASSERT(api.messageBodies.last().contains("question one"));
        TEST_ASSERT(api.messageBodies.last().contains("answer one"));
        TEST_ASSERT(!api.messageBodies.last().contains("sig-abc"));

        // Another provider's format is refused and changes nothing.
        QtLLM::Client other(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        TEST_ASSERT(!other.importConversation(saved));
        TEST_ASSERT(other.conversationHistory().isEmpty());
        TEST_ASSERT(!second.importConversation(QJsonObject{{"provider", "claude"}}));
    }

    // ------------------------------------------------------------ OpenAI-compatible provider

    TEST_FUNCTION(testOpenAiProviderThroughClient)
    {
        TEST_START;

        FakeOpenAi server;
        server.enqueue(FakeOpenAi::completion(QString(), QJsonArray{
            FakeOpenAi::toolCall("c1", "probe", "{\"n\":3}")}, "tool_calls"));
        server.enqueue(FakeOpenAi::completion("it is 6"));

        QtLLM::Client client(QtLLM::Provider::OpenAI, server.baseUrl(), "secret");
        TEST_ASSERT(client.provider() == QtLLM::Provider::OpenAI);
        client.setModel("m1");
        client.setStreamingEnabled(false);
        int doubled = 0;
        QtLLM::Tool probe = tool("probe");
        probe.addParameter("n", "integer", "n", true);
        client.registerTool(probe, [&](const QJsonObject& input) {
            doubled = input["n"].toInt() * 2;
            return QtLLM::toolOk({{"doubled", doubled}});
        });

        const Outcome outcome = turn(client, "double three");
        TEST_ASSERT(outcome.errors.isEmpty());
        TEST_COMPARE(outcome.response, QString("it is 6"));
        TEST_COMPARE(doubled, 6);
        // Tools go out in the OpenAI format, the key as a bearer token.
        const QJsonObject first = server.body(0);
        TEST_COMPARE(first["tools"].toArray()[0].toObject()["type"].toString(), QString("function"));
        TEST_COMPARE(first["model"].toString(), QString("m1"));
        TEST_ASSERT(server.heads.value(0).contains("authorization: bearer secret"));
        TEST_ASSERT(server.bodies.value(1).contains("doubled"));
        TEST_COMPARE(client.usageStats().toolCalls, 1);
    }

    // ------------------------------------------------------------ application knowledge

    TEST_FUNCTION(testKnowledgeTopics)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        QtLLM::AppKnowledge help(&client);
        TEST_ASSERT(!client.toolNames().contains("app_help_search"));   // nothing to search yet

        help.addTopic("export-pdf", "Exporting a document as PDF",
                      "Open the File menu and choose Export. Pick PDF as the format.", {"save as pdf"});
        help.addTopic("layers", "Working with layers", "Layers stack drawings on top of each other.");
        help.addTopic("long", "A long topic", QString("word ").repeated(4000));
        TEST_ASSERT(client.toolNames().contains("app_help_search"));
        TEST_ASSERT(client.toolNames().contains("app_help_read"));

        QJsonArray found = help.search({{"query", "how do I export a PDF"}})["topics"].toArray();
        TEST_ASSERT(!found.isEmpty());
        TEST_COMPARE(found[0].toObject()["id"].toString(), QString("export-pdf"));
        TEST_ASSERT(found[0].toObject()["excerpt"].toString().contains("Export"));

        TEST_COMPARE(help.search({})["topics"].toArray().size(), 3);        // no query: everything
        TEST_ASSERT(help.search({{"query", "xylophone"}})["topics"].toArray().isEmpty());

        const QJsonObject topic = help.read({{"id", "layers"}});
        TEST_ASSERT(topic["text"].toString().contains("stack drawings"));
        TEST_ASSERT(!topic.contains("nextOffset"));

        // A long topic is paged.
        const QJsonObject part = help.read({{"id", "long"}});
        TEST_ASSERT(part.contains("nextOffset"));
        TEST_COMPARE(part["text"].toString().size(), 8000);
        const QJsonObject last = help.read({{"id", "long"}, {"offset", 16000}});
        TEST_COMPARE(last["text"].toString().size(), 4000);
        TEST_ASSERT(!last.contains("nextOffset"));
        TEST_COMPARE(help.read({{"id", "nothing"}})["status"].toString(), QString("error"));

        // Topics from a folder of text files.
        QTemporaryDir dir;
        QFile file(dir.filePath("shortcuts.md"));
        TEST_ASSERT(file.open(QIODevice::WriteOnly));
        file.write("# Keyboard shortcuts\nCtrl+E exports.\n");
        file.close();
        TEST_COMPARE(help.addTopicsFromDirectory(dir.path()), 1);
        found = help.search({{"query", "keyboard"}})["topics"].toArray();
        TEST_COMPARE(found[0].toObject()["title"].toString(), QString("Keyboard shortcuts"));

        // Switched off: the tools go, the topics stay.
        help.setEnabled(false);
        TEST_ASSERT(!client.toolNames().contains("app_help_search"));
        help.setEnabled(true);
        TEST_COMPARE(help.topicIds().size(), 4);
        TEST_ASSERT(client.toolNames().contains("app_help_read"));
    }

    // ------------------------------------------------------------ widgets

    TEST_FUNCTION(testToolSettingsWidget)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        client.registerTool(tool("free", "Group"), ok);
        client.registerTool(tool("locked", "Group"), ok);
        client.setToolUserConfigurable("locked", false);

        QtLLM::ToolSettingsWidget widget;
        widget.setClient(&client);
        auto check = [&](const char* name) { return widget.findChild<QCheckBox*>(name); };
        TEST_ASSERT(check("qtllm_tool_free") && check("qtllm_tool_free")->isEnabled());
        TEST_ASSERT(check("qtllm_tool_locked") && !check("qtllm_tool_locked")->isEnabled());
        TEST_ASSERT(check("qtllm_tool_locked")->isChecked());   // its state is still shown
        widget.resize(520, 260);
        // Set QTLLM_TEST_GRABS to a folder to get a picture of what was just checked.
        if (qEnvironmentVariableIsSet("QTLLM_TEST_GRABS"))
            widget.grab().save(qEnvironmentVariable("QTLLM_TEST_GRABS") + "/tool_settings.png");

        // A dialog applies on OK: until then the client is untouched.
        check("qtllm_tool_free")->click();
        TEST_ASSERT(client.isToolEnabled("free"));
        // A tool arriving meanwhile rebuilds the list; the pending choice stays.
        client.registerTool(tool("newcomer"), ok);
        TEST_ASSERT(check("qtllm_tool_newcomer") != nullptr);
        TEST_ASSERT(!check("qtllm_tool_free")->isChecked());
        widget.apply();
        TEST_ASSERT(!client.isToolEnabled("free"));

        // Immediate mode, for a settings page without an Apply button.
        widget.setApplyImmediately(true);
        check("qtllm_tool_free")->click();
        TEST_ASSERT(client.isToolEnabled("free"));

        // Locked tools can be left out altogether.
        widget.setLockedToolsVisible(false);
        TEST_ASSERT(check("qtllm_tool_locked") == nullptr);
        TEST_ASSERT(check("qtllm_tool_free") != nullptr);

        // The application changes the policy: the list follows.
        client.setToolUserConfigurable("free", false);
        TEST_ASSERT(check("qtllm_tool_free") == nullptr);
    }

    TEST_FUNCTION(testSettingsDialogTabsAndProviders)
    {
        TEST_START;

        QtLLM::SettingsDialog dialog;
        using Tab = QtLLM::SettingsDialog::Tab;
        TEST_ASSERT(dialog.isTabVisible(Tab::Tools));
        dialog.setTabVisible(Tab::Tools, false);
        dialog.setTabVisible(Tab::Agents, false);
        TEST_ASSERT(!dialog.isTabVisible(Tab::Tools));
        TEST_ASSERT(!dialog.isTabVisible(Tab::Agents));
        TEST_ASSERT(dialog.isTabVisible(Tab::Context));

        // Only local providers on offer: the selection moves to one of them.
        using P = QtLLM::SettingsDialog::Provider;
        dialog.setAvailableProviders({P::Ollama, P::OpenAICompatible});
        TEST_ASSERT(dialog.provider() == P::Ollama);

        // The OpenAI-compatible choice reaches the client.
        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        dialog.setProvider(P::OpenAICompatible);
        dialog.setOpenAiUrl("http://127.0.0.1:9/v1");
        dialog.setModel("local-model");
        dialog.applyTo(&client);
        TEST_ASSERT(client.provider() == QtLLM::Provider::OpenAI);
        TEST_COMPARE(client.model(), QString("local-model"));
    }

    // A chat wired the way applications do it: messageSent -> sendPrompt,
    // responseReady -> addAssistantMessage.
    struct WiredChat
    {
        FakeApi api;
        QtLLM::Client client{"key", api.url()};
        QtLLM::ChatDockWidget chat;
        int answers = 0;

        WiredChat()
        {
            chat.setClient(&client);
            QObject::connect(&chat, &QtLLM::ChatDockWidget::messageSent,
                             [this](const QString& text) { client.sendPrompt(text); });
            QObject::connect(&client, &QtLLM::Client::responseReady, [this](const QString& text) {
                chat.addAssistantMessage(text);
                ++answers;
            });
            QObject::connect(&client, &QtLLM::Client::requestStarted, [this]() { chat.setLoading(true); });
            QObject::connect(&client, &QtLLM::Client::requestFinished, [this]() { chat.setLoading(false); });
        }

        bool ask(const QString& text)
        {
            const int before = answers;
            return chat.submitPrompt(text) && spin([&]() { return answers > before; });
        }
        QPushButton* button(const char* name) { return chat.findChild<QPushButton*>(name); }
    };

    TEST_FUNCTION(testChatRegenerateAndEdit)
    {
        TEST_START;

        WiredChat w;
        w.api.replies.enqueue(FakeApi::textReply("first try"));
        w.api.replies.enqueue(FakeApi::textReply("second try"));
        w.api.replies.enqueue(FakeApi::textReply("third try"));

        // Off until the application asks for it.
        TEST_ASSERT(w.button("qtllm_regenerateButton")->isHidden());
        w.chat.setRegenerateEnabled(true);
        TEST_ASSERT(!w.button("qtllm_regenerateButton")->isHidden());
        TEST_ASSERT(!w.button("qtllm_editLastButton")->isHidden());

        TEST_ASSERT(w.ask("tell me a joke"));
        TEST_COMPARE(w.client.conversationHistory().size(), 2);

        // Regenerate: the same question goes out again, the old answer is gone.
        w.button("qtllm_regenerateButton")->click();
        TEST_ASSERT(spin([&]() { return w.answers == 2; }));
        TEST_COMPARE(w.api.messageRequests, 2);
        TEST_COMPARE(w.client.conversationHistory().size(), 2);
        TEST_COMPARE(w.client.conversationHistory()[1].toObject()["content"].toString(), QString("second try"));
        TEST_ASSERT(!w.api.messageBodies.value(1).contains("first try"));

        // Edit: the text waits in the input field and the exchange is hidden,
        // but still part of the conversation.
        auto answerShown = [&]() {
            for (QLabel* label : w.chat.findChildren<QLabel*>()) {
                if (label->text().contains("second try"))
                    return label->isVisibleTo(&w.chat);
            }
            return false;
        };
        QPushButton* edit = w.button("qtllm_editLastButton");
        TEST_ASSERT(answerShown());
        edit->click();
        TEST_COMPARE(w.chat.inputText(), QString("tell me a joke"));
        TEST_ASSERT(edit->isChecked());
        TEST_ASSERT(!answerShown());
        TEST_COMPARE(w.client.conversationHistory().size(), 2);

        // A second click gives the edit up: everything is as before.
        edit->click();
        TEST_ASSERT(!edit->isChecked());
        TEST_ASSERT(answerShown());
        TEST_ASSERT(w.chat.inputText().isEmpty());
        TEST_COMPARE(w.client.conversationHistory().size(), 2);

        // So does Esc in the input field.
        edit->click();
        TEST_ASSERT(!answerShown());
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(w.chat.findChild<QTextEdit*>("qtllm_chatInput"), &escape);
        TEST_ASSERT(!edit->isChecked());
        TEST_ASSERT(answerShown());
        TEST_COMPARE(w.client.conversationHistory().size(), 2);
        TEST_COMPARE(w.api.messageRequests, 2);

        // Sending the changed message replaces the exchange.
        edit->click();
        TEST_ASSERT(w.ask("tell me a riddle"));
        TEST_ASSERT(!edit->isChecked());
        TEST_COMPARE(w.client.conversationHistory().size(), 2);
        TEST_COMPARE(w.client.conversationHistory()[0].toObject()["content"].toString(), QString("tell me a riddle"));
        TEST_ASSERT(!w.api.messageBodies.value(2).contains("second try"));
        TEST_ASSERT(!answerShown());
    }

    TEST_FUNCTION(testChatPicturesAndLoad)
    {
        TEST_START;

        WiredChat w;
        w.api.replies.enqueue(FakeApi::textReply("a blue square"));
        w.api.replies.enqueue(FakeApi::textReply("plain"));
        w.api.replies.enqueue(FakeApi::textReply("continued"));

        TEST_ASSERT(w.button("qtllm_attachButton")->isHidden());
        w.chat.setImageInputEnabled(true);
        TEST_ASSERT(!w.button("qtllm_attachButton")->isHidden());

        QImage picture(8, 8, QImage::Format_RGB32);
        picture.fill(Qt::blue);
        w.chat.attachImages({picture});
        TEST_ASSERT(!w.chat.findChild<QWidget*>("qtllm_attachRow")->isHidden());
        w.chat.setRegenerateEnabled(true);
        w.chat.setLoadButtonVisible(true);
        w.chat.resize(420, 520);
        // Set QTLLM_TEST_GRABS to a folder to get a picture of what was just checked.
        if (qEnvironmentVariableIsSet("QTLLM_TEST_GRABS"))
            w.chat.grab().save(qEnvironmentVariable("QTLLM_TEST_GRABS") + "/chat_features.png");

        // The application's messageSent -> sendPrompt(text) wiring is enough.
        TEST_ASSERT(w.ask("what colour?"));
        TEST_ASSERT(w.api.messageBodies.value(0).contains("\"type\":\"image\""));
        TEST_ASSERT(w.chat.findChild<QWidget*>("qtllm_attachRow")->isHidden());
        TEST_ASSERT(w.ask("and without one?"));
        TEST_ASSERT(!w.api.messageBodies.value(1).contains("\"type\":\"image\","));

        // Save, start over, continue where it was.
        const QJsonObject saved = w.client.exportConversation();
        w.client.clearConversation();
        w.chat.clearMessages();
        TEST_ASSERT(w.chat.loadConversation(saved));
        TEST_COMPARE(w.client.conversationHistory().size(), 4);
        TEST_ASSERT(w.ask("go on"));
        TEST_ASSERT(w.api.messageBodies.last().contains("a blue square"));
        TEST_ASSERT(!w.chat.loadConversation(QJsonObject{{"provider", "ollama"}}));
    }
};

TEST_INSTANTIATE(TST_HarnessFeatures);
