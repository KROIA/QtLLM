#pragma once

#include "UnitTest.h"
#include "QtLLM.h"
#include <QString>
#include <QTableWidget>

// Background agents. None of these tests touch the network: the configured
// endpoint is the discard port and the test never spins an event loop, so no
// reply can ever arrive and every assertion below is deterministic.
class TST_Agent : public UnitTest::Test
{
    TEST_CLASS(TST_Agent)
public:
    TST_Agent()
        : Test("TST_Agent")
    {
        ADD_TEST(TST_Agent::testRegistryTracksAgentLifetime);
        ADD_TEST(TST_Agent::testSecondAskIsQueuedBehindTheFirst);
        ADD_TEST(TST_Agent::testDestroyingAgentDropsPendingCallback);
        ADD_TEST(TST_Agent::testBudgetCapDrainsQueueWithFailure);
        ADD_TEST(TST_Agent::testTransientErrorKeepsQueue);
        ADD_TEST(TST_Agent::testEmptyNameGetsFallbackName);
        ADD_TEST(TST_Agent::testSystemPromptReachesClient);
        ADD_TEST(TST_Agent::testAgentTagsItsUsageSamples);
        ADD_TEST(TST_Agent::testAgentsWidgetSurvivesAgentDestruction);
        ADD_TEST(TST_Agent::testNothingIsSentBeforeTheModelListArrives);
        ADD_TEST(TST_Agent::testFallbackModelChosenWhenPrimaryIsNotOffered);
        ADD_TEST(TST_Agent::testFirstOfferedModelUsedWhenNoCandidateMatches);
        ADD_TEST(TST_Agent::testUnknownModelListProceedsWithConfiguredModel);
    }

private:

    static QtLLM::AgentConfig makeConfig(const QString& name)
    {
        QtLLM::AgentConfig config;
        config.name     = name;
        config.provider = QtLLM::Provider::Ollama;
        config.url      = QStringLiteral("http://127.0.0.1:9/api/chat");  // discard port
        config.model    = QStringLiteral("test-model");
        return config;
    }


    TEST_FUNCTION(testRegistryTracksAgentLifetime)
    {
        TEST_START;

        QtLLM::AgentRegistry& registry = QtLLM::AgentRegistry::instance();
        const int before = registry.agents().size();

        QtLLM::Agent* agent = new QtLLM::Agent(makeConfig("analyzer"));
        TEST_ASSERT_EQUAL(registry.agents().size(), before + 1);
        TEST_ASSERT(registry.agents().contains(agent));

        delete agent;
        TEST_ASSERT_EQUAL(registry.agents().size(), before);
    }


    // An agent cannot send anything until it knows which models the provider
    // actually offers — sending first is what produces "Model not found".
    static void announceModels(QtLLM::Agent& agent, const QStringList& models)
    {
        emit agent.modelsAvailable(models);
    }


    TEST_FUNCTION(testSecondAskIsQueuedBehindTheFirst)
    {
        TEST_START;

        QtLLM::Agent agent(makeConfig("queue"));
        TEST_ASSERT(agent.state() == QtLLM::Agent::State::Idle);

        agent.ask("first",  [](const QString&, bool) {});
        agent.ask("second", [](const QString&, bool) {});
        announceModels(agent, { "test-model" });

        // The first turn is in flight and can never resolve here, so exactly
        // one prompt must still be waiting.
        TEST_ASSERT_EQUAL(agent.queuedPrompts(), 1);
        TEST_ASSERT(agent.state() == QtLLM::Agent::State::Busy);
    }


    TEST_FUNCTION(testDestroyingAgentDropsPendingCallback)
    {
        TEST_START;

        bool called = false;
        QtLLM::Agent* agent = new QtLLM::Agent(makeConfig("dropper"));
        agent->ask("work", [&called](const QString&, bool) { called = true; });

        delete agent;

        TEST_ASSERT_FALSE(called);
    }


    TEST_FUNCTION(testBudgetCapDrainsQueueWithFailure)
    {
        TEST_START;

        QtLLM::AgentConfig config = makeConfig("spender");
        config.costCapUsd = 0.01;
        QtLLM::Agent agent(config);

        int  callbacks = 0;
        bool lastOk    = true;
        agent.ask("in flight", [&](const QString&, bool) { ++callbacks; });
        agent.ask("queued",    [&](const QString&, bool ok) { ++callbacks; lastOk = ok; });
        announceModels(agent, { "test-model" });

        QtLLM::UsageStats stats;
        stats.sessionCostUsd = 0.5;  // blows straight past the cap
        emit agent.statsUpdated(stats);

        TEST_ASSERT(agent.state() == QtLLM::Agent::State::BudgetExceeded);
        TEST_ASSERT_EQUAL(agent.queuedPrompts(), 0);
        TEST_ASSERT_EQUAL(callbacks, 1);   // only the queued one is dropped
        TEST_ASSERT_FALSE(lastOk);
    }


    TEST_FUNCTION(testTransientErrorKeepsQueue)
    {
        TEST_START;

        QtLLM::Agent agent(makeConfig("resilient"));

        bool firstOk = true;
        agent.ask("first",  [&](const QString&, bool ok) { firstOk = ok; });
        agent.ask("second", [](const QString&, bool) {});
        agent.ask("third",  [](const QString&, bool) {});
        announceModels(agent, { "test-model" });
        TEST_ASSERT_EQUAL(agent.queuedPrompts(), 2);

        emit agent.errorOccurred(QStringLiteral("connection refused"));

        // The failed turn reports failure; the rest of the queue survives and
        // the next entry is dispatched.
        TEST_ASSERT_FALSE(firstOk);
        TEST_ASSERT_EQUAL(agent.queuedPrompts(), 1);
        TEST_ASSERT_EQUAL(agent.lastError(), QString("connection refused"));
    }


    TEST_FUNCTION(testEmptyNameGetsFallbackName)
    {
        TEST_START;

        QtLLM::Agent agent(makeConfig(QString()));
        TEST_ASSERT_FALSE(agent.config().name.isEmpty());
    }


    TEST_FUNCTION(testSystemPromptReachesClient)
    {
        TEST_START;

        QtLLM::AgentConfig config = makeConfig("classifier");
        config.systemPrompt = QStringLiteral("You classify text into buckets.");
        QtLLM::Agent agent(config);

        // contextBreakdown() only reports system-prompt tokens if the prompt
        // actually made it into the underlying Client.
        TEST_ASSERT(agent.contextBreakdown().systemPromptTokens > 0);
    }


    TEST_FUNCTION(testAgentTagsItsUsageSamples)
    {
        TEST_START;

        QtLLM::Agent agent(makeConfig("tagger"));
        TEST_ASSERT_EQUAL(agent.usageAppTag(), QString("agent:tagger"));
    }


    // Destroying an agent rebuilds the table underneath a selection that still
    // points at the old rows. The widget must never read back through a pointer
    // to an agent that is already gone.
    TEST_FUNCTION(testAgentsWidgetSurvivesAgentDestruction)
    {
        TEST_START;

        QtLLM::Agent* first  = new QtLLM::Agent(makeConfig("first"));
        QtLLM::Agent* second = new QtLLM::Agent(makeConfig("second"));

        QtLLM::AgentsWidget widget;
        QTableWidget* table = widget.findChild<QTableWidget*>();
        TEST_ASSERT_NOT_NULL(table);

        const int baseline = table->rowCount() - 2;
        TEST_ASSERT_EQUAL(table->rowCount(), baseline + 2);

        table->selectRow(table->rowCount() - 1);   // select the last row
        delete first;                              // rebuilds while that row is selected
        TEST_ASSERT_EQUAL(table->rowCount(), baseline + 1);

        delete second;
        TEST_ASSERT_EQUAL(table->rowCount(), baseline);
    }


    // The bug this guards: an agent that sends immediately after construction
    // uses whatever model it was configured with, even when the provider does
    // not offer it, and the turn dies with "Model not found".
    TEST_FUNCTION(testNothingIsSentBeforeTheModelListArrives)
    {
        TEST_START;

        QtLLM::Agent agent(makeConfig("waiter"));
        agent.ask("hello", [](const QString&, bool) {});

        TEST_ASSERT(agent.state() == QtLLM::Agent::State::Starting);
        TEST_ASSERT_EQUAL(agent.queuedPrompts(), 1);

        announceModels(agent, { "test-model" });

        TEST_ASSERT_EQUAL(agent.queuedPrompts(), 0);
        TEST_ASSERT(agent.state() == QtLLM::Agent::State::Busy);
    }


    TEST_FUNCTION(testFallbackModelChosenWhenPrimaryIsNotOffered)
    {
        TEST_START;

        QtLLM::AgentConfig config = makeConfig("faller");
        config.model          = QStringLiteral("retired-model");
        config.fallbackModels = QStringList{ QStringLiteral("also-gone"), QStringLiteral("llama3.2") };
        QtLLM::Agent agent(config);

        QString announced;
        QObject::connect(&agent, &QtLLM::Client::modelChanged,
                         [&announced](const QString& model) { announced = model; });

        announceModels(agent, { QStringLiteral("llama3.2"), QStringLiteral("other") });

        TEST_ASSERT_EQUAL(agent.model(), QString("llama3.2"));
        TEST_ASSERT_EQUAL(announced, QString("llama3.2"));
    }


    TEST_FUNCTION(testFirstOfferedModelUsedWhenNoCandidateMatches)
    {
        TEST_START;

        QtLLM::AgentConfig config = makeConfig("desperate");
        config.model          = QStringLiteral("nope");
        config.fallbackModels = QStringList{ QStringLiteral("also-nope") };
        QtLLM::Agent agent(config);

        announceModels(agent, { QStringLiteral("only-this-one") });

        TEST_ASSERT_EQUAL(agent.model(), QString("only-this-one"));
    }


    // An empty list means the fetch failed, not that the provider has no
    // models. Guessing a replacement from no information would be worse than
    // trying what the application asked for.
    TEST_FUNCTION(testUnknownModelListProceedsWithConfiguredModel)
    {
        TEST_START;

        QtLLM::Agent agent(makeConfig("offline"));
        agent.ask("hello", [](const QString&, bool) {});

        announceModels(agent, {});

        TEST_ASSERT_EQUAL(agent.model(), QString("test-model"));
        TEST_ASSERT_EQUAL(agent.queuedPrompts(), 0);
    }

};

TEST_INSTANTIATE(TST_Agent);
