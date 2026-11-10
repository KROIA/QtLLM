#pragma once

#include "UnitTest.h"
#include "QtLLM.h"
#include "FakeApi.h"
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QHeaderView>
#include <QJsonObject>
#include <QLayout>
#include <QPushButton>
#include <QTableWidget>
#include <QTextBrowser>
#include <QThread>
#include <QTimer>
#include <functional>
#ifdef UNITTEST_GUI_ENABLED
#include "UnitTest_Gui.h"
#endif

// What an agent did, as Agent::record() and AgentRegistry::finishedAgents()
// keep it, and how the Agents tab shows it.
class TST_AgentHistory : public UnitTest::Test
{
    TEST_CLASS(TST_AgentHistory)
public:
    TST_AgentHistory()
        : Test("TST_AgentHistory")
    {
        ADD_TEST(TST_AgentHistory::testTaskWithToolCallIsRecorded);
        ADD_TEST(TST_AgentHistory::testFailedTaskRecordsItsError);
        ADD_TEST(TST_AgentHistory::testDestroyedAgentMovesToHistory);
        ADD_TEST(TST_AgentHistory::testHistoryIsCappedAndClearable);
        ADD_TEST(TST_AgentHistory::testSubagentAppearsInHistory);
#ifdef UNITTEST_GUI_ENABLED
        ADD_TEST(TST_AgentHistory::testWidgetShowsHistoryAndDetails);
#endif
    }

private:

    // Discard port and no event loop: nothing is ever answered.
    static QtLLM::AgentConfig offlineConfig(const QString& name)
    {
        QtLLM::AgentConfig config;
        config.name     = name;
        config.provider = QtLLM::Provider::Ollama;
        config.url      = QStringLiteral("http://127.0.0.1:9/api/chat");
        config.model    = QStringLiteral("test-model");
        return config;
    }

    static QtLLM::AgentConfig fakeApiConfig(const FakeApi& api, const QString& name = QString())
    {
        QtLLM::AgentConfig config;
        config.name     = name;
        config.provider = QtLLM::Provider::Claude;
        config.url      = api.url();
        config.apiKey   = "key";
        config.model    = "test-model";
        return config;
    }

    static bool spinUntil(const std::function<bool()>& condition, int timeoutMs = 5000)
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

    // A whole task without the network: dispatched, one tool call, answered,
    // usage reported - driven through the Client signals the agent listens to.
    static void runScriptedTask(QtLLM::Agent& agent, const QString& prompt, const QString& reply)
    {
        agent.ask(prompt, [](const QString&, bool) {});
        emit agent.modelsAvailable({ "test-model" });
        emit agent.toolInvoked("probe", QJsonObject{{"n", 7}});
        emit agent.toolCompleted("probe", QJsonObject{{"value", 42}});
        emit agent.responseReady(reply);
        QtLLM::UsageStats stats;
        stats.inputTokens    = 10;
        stats.outputTokens   = 5;
        stats.sessionCostUsd = 0.25;
        emit agent.statsUpdated(stats);
    }


    TEST_FUNCTION(testTaskWithToolCallIsRecorded)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::toolUseReply("probe", QJsonObject{{"n", 7}}));
        api.replies.enqueue(FakeApi::textReply("forty-two"));

        QtLLM::Agent agent(fakeApiConfig(api, "recorder"));
        QtLLM::Tool probe;
        probe.setName("probe").setDescription("probe").addParameter("n", "integer", "n", true);
        agent.registerTool(probe, [](const QJsonObject& input) {
            QThread::msleep(30);   // so the tool has a measurable duration
            return QtLLM::toolOk({{"value", input["n"].toInt() * 6}});
        });

        bool done = false;
        agent.ask("What is the answer?", [&](const QString&, bool) { done = true; });
        TEST_ASSERT(spinUntil([&]() { return done; }));

        const QtLLM::AgentRecord& record = agent.record();
        TEST_COMPARE(record.name, QString("recorder"));
        TEST_COMPARE(record.model, QString("test-model"));
        TEST_ASSERT(record.finishedAtMs == 0);
        TEST_COMPARE(int(record.tasks.size()), 1);
        const QtLLM::AgentTask& task = record.tasks.first();
        TEST_COMPARE(task.prompt, QString("What is the answer?"));
        TEST_COMPARE(task.reply, QString("forty-two"));
        TEST_ASSERT(task.ok && task.finished);
        TEST_ASSERT(task.startedAtMs >= record.spawnedAtMs);
        TEST_COMPARE(int(task.toolCalls.size()), 1);
        const QtLLM::AgentToolCall& call = task.toolCalls.first();
        TEST_COMPARE(call.name, QString("probe"));
        TEST_ASSERT(call.input.contains("7"));
        TEST_ASSERT(call.result.contains("42"));
        TEST_ASSERT(call.durationMs >= 25);
        TEST_ASSERT(task.durationMs >= call.durationMs);
        TEST_ASSERT(task.inputTokens > 0 && task.outputTokens > 0);

        TEST_COMPARE(record.turns, 1);
        TEST_COMPARE(record.toolCalls, 1);
        TEST_ASSERT(record.workingMs == task.durationMs);
        TEST_COMPARE(record.inputTokens, task.inputTokens);
    }


    TEST_FUNCTION(testFailedTaskRecordsItsError)
    {
        TEST_START;

        QtLLM::Agent agent(offlineConfig("failing"));
        agent.ask("first", [](const QString&, bool) {});
        agent.ask("second", [](const QString&, bool) {});
        emit agent.modelsAvailable({ "test-model" });
        emit agent.errorOccurred("connection refused");

        const QtLLM::AgentRecord& record = agent.record();
        TEST_COMPARE(int(record.tasks.size()), 2);
        TEST_ASSERT(record.tasks[0].finished && !record.tasks[0].ok);
        TEST_COMPARE(record.tasks[0].reply, QString("connection refused"));
        TEST_COMPARE(record.tasks[1].prompt, QString("second"));
        TEST_ASSERT(!record.tasks[1].finished);   // dispatched, in flight
        TEST_COMPARE(record.turns, 1);
    }


    TEST_FUNCTION(testDestroyedAgentMovesToHistory)
    {
        TEST_START;

        QtLLM::AgentRegistry& registry = QtLLM::AgentRegistry::instance();
        registry.clearFinished();
        QStringList announced;
        auto connection = QObject::connect(&registry, &QtLLM::AgentRegistry::agentFinished,
            [&](const QtLLM::AgentRecord& record) { announced.append(record.name); });

        QtLLM::Agent* older = new QtLLM::Agent(offlineConfig("older"));
        QtLLM::Agent* newer = new QtLLM::Agent(offlineConfig("newer"));
        runScriptedTask(*older, "Sort the list", "sorted");
        TEST_ASSERT(registry.finishedAgents().isEmpty());   // alive agents are not history

        delete older;
        delete newer;
        QObject::disconnect(connection);

        TEST_COMPARE(announced, QStringList({ "older", "newer" }));
        const QList<QtLLM::AgentRecord> history = registry.finishedAgents();
        TEST_COMPARE(int(history.size()), 2);
        TEST_COMPARE(history[0].name, QString("newer"));   // newest first
        const QtLLM::AgentRecord& record = history[1];
        TEST_COMPARE(record.name, QString("older"));
        TEST_ASSERT(record.finishedAtMs >= record.spawnedAtMs);
        TEST_COMPARE(int(record.tasks.size()), 1);
        TEST_COMPARE(record.tasks[0].prompt, QString("Sort the list"));
        TEST_COMPARE(record.tasks[0].reply, QString("sorted"));
        TEST_COMPARE(record.tasks[0].inputTokens, 10);
        TEST_ASSERT(record.tasks[0].costUsd == 0.25);
        TEST_ASSERT(record.costUsd == 0.25);
        TEST_COMPARE(int(record.tasks[0].toolCalls.size()), 1);
        TEST_ASSERT(record.tasks[0].toolCalls[0].result.contains("42"));
    }


    TEST_FUNCTION(testHistoryIsCappedAndClearable)
    {
        TEST_START;

        QtLLM::AgentRegistry& registry = QtLLM::AgentRegistry::instance();
        registry.clearFinished();
        const int total = QtLLM::AgentRegistry::kMaxFinished + 3;
        for (int i = 0; i < total; ++i)
            delete new QtLLM::Agent(offlineConfig(QString("capped-%1").arg(i)));

        const QList<QtLLM::AgentRecord> history = registry.finishedAgents();
        TEST_COMPARE(int(history.size()), QtLLM::AgentRegistry::kMaxFinished);
        TEST_COMPARE(history.first().name, QString("capped-%1").arg(total - 1));
        TEST_COMPARE(history.last().name, QString("capped-3"));

        registry.clearFinished();
        TEST_ASSERT(registry.finishedAgents().isEmpty());
    }


    // The case the history exists for: an agent the model spawned is deleted
    // as soon as it has answered, and is only visible afterwards.
    TEST_FUNCTION(testSubagentAppearsInHistory)
    {
        TEST_START;

        QtLLM::AgentRegistry::instance().clearFinished();
        FakeApi api, subApi;
        api.replies.enqueue(FakeApi::toolUseReply("run_subagent",
            QJsonObject{{"task", "Invent a title"}, {"name", "titler"}}));
        api.replies.enqueue(FakeApi::textReply("done"));
        subApi.replies.enqueue(FakeApi::toolUseReply("probe", QJsonObject{{"n", 7}}));
        subApi.replies.enqueue(FakeApi::textReply("Sunny Chat"));

        QtLLM::Client client("key", api.url());
        QtLLM::Tool probe;
        probe.setName("probe").setDescription("a tool of the chat").addParameter("n", "integer", "n", true);
        client.registerTool(probe, [](const QJsonObject&) { return QtLLM::toolOk(); });
        QtLLM::registerSubagentTool(&client, [&]() { return fakeApiConfig(subApi); });

        QString response;
        QObject::connect(&client, &QtLLM::Client::responseReady, [&](const QString& text) { response = text; });
        client.sendPrompt("main chat prompt");
        TEST_ASSERT(spinUntil([&]() { return !response.isEmpty(); }));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

        const QList<QtLLM::AgentRecord> history = QtLLM::AgentRegistry::instance().finishedAgents();
        TEST_COMPARE(int(history.size()), 1);
        const QtLLM::AgentRecord& record = history.first();
        TEST_COMPARE(record.name, QString("titler"));
        TEST_COMPARE(int(record.tasks.size()), 1);
        TEST_COMPARE(record.tasks[0].prompt, QString("Invent a title"));
        TEST_COMPARE(record.tasks[0].reply, QString("Sunny Chat"));
        TEST_COMPARE(int(record.tasks[0].toolCalls.size()), 1);
        TEST_COMPARE(record.tasks[0].toolCalls[0].name, QString("probe"));
    }


#ifdef UNITTEST_GUI_ENABLED
    TEST_FUNCTION(testWidgetShowsHistoryAndDetails)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        QtLLM::AgentRegistry::instance().clearFinished();
        QtLLM::Agent* agent = new QtLLM::Agent(offlineConfig("gui-historian"));
        runScriptedTask(*agent, "Summarize the <GUI> test", "All done");
        delete agent;

        QtLLM::AgentsWidget widget;
        widget.resize(900, 700);
        TEST_ASSERT(UnitTest::Gui::showAndWait(&widget));
        auto* running  = UnitTest::Gui::find<QTableWidget>("qtllm_agentsRunningTable", &widget);
        auto* finished = UnitTest::Gui::find<QTableWidget>("qtllm_agentsFinishedTable", &widget);
        auto* details  = UnitTest::Gui::find<QTextBrowser>("qtllm_agentsDetails", &widget);
        auto* clear    = UnitTest::Gui::find<QPushButton>("qtllm_agentsClearHistory", &widget);
        TEST_ASSERT(running && finished && details && clear);

        // Full width: the table fills the widget, and its columns fill the table.
        const QMargins margins = widget.layout()->contentsMargins();
        TEST_COMPARE(running->width(), widget.width() - margins.left() - margins.right());
        TEST_COMPARE(running->horizontalHeader()->length(), running->viewport()->width());

        TEST_COMPARE(finished->rowCount(), 1);
        TEST_COMPARE(finished->item(0, 0)->text(), QString("gui-historian"));
        TEST_ASSERT(UnitTest::Gui::clickCell(finished, 0, 0));
        TEST_ASSERT(UnitTest::Gui::waitFor([&]() {
            const QString text = details->toPlainText();
            return text.contains("Summarize the <GUI> test") && text.contains("probe")
                && text.contains("All done");
        }));
        // Set QTLLM_TEST_GRABS to a folder to get a picture of what was just checked.
        if (qEnvironmentVariableIsSet("QTLLM_TEST_GRABS"))
            widget.grab().save(qEnvironmentVariable("QTLLM_TEST_GRABS") + "/agents_widget.png");

        TEST_ASSERT(UnitTest::Gui::click(clear));
        TEST_COMPARE(finished->rowCount(), 0);
        TEST_ASSERT(details->toPlainText().isEmpty());

        widget.setHistoryVisible(false);
        TEST_ASSERT(!finished->isVisible());
        TEST_ASSERT(running->isVisible());
    }
#endif

};

TEST_INSTANTIATE(TST_AgentHistory);
