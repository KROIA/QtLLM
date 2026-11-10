#pragma once

// Agents talking to each other: list_agents and message_agent.

#include "UnitTest.h"
#include "QtLLM.h"
#include "FakeApi.h"
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <functional>

class TST_AgentMessaging : public UnitTest::Test
{
    TEST_CLASS(TST_AgentMessaging)
public:
    TST_AgentMessaging()
        : Test("TST_AgentMessaging")
    {
        ADD_TEST(TST_AgentMessaging::testMessagingIsOptIn);
        ADD_TEST(TST_AgentMessaging::testAgentAsksAnotherAgent);
        ADD_TEST(TST_AgentMessaging::testListAgentsShowsOnlyReachableOnes);
        ADD_TEST(TST_AgentMessaging::testUnknownAgentIsAnError);
        ADD_TEST(TST_AgentMessaging::testWaitingForEachOtherIsRefused);
        ADD_TEST(TST_AgentMessaging::testClosedAgentEndsTheCall);
        ADD_TEST(TST_AgentMessaging::testChatCanMessageAgents);
        ADD_TEST(TST_AgentMessaging::testSubagentsDoNotSpeakInTheChatsName);
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

    static QtLLM::AgentConfig config(const QString& name, const FakeApi& api, bool messaging = true,
                                     const QString& description = QString())
    {
        QtLLM::AgentConfig c;
        c.name        = name;
        c.provider    = QtLLM::Provider::Claude;
        c.url         = api.url();
        c.apiKey      = "key";
        c.model       = "test-model";
        c.messaging   = messaging;
        c.description = description;
        return c;
    }

    static QByteArray messageCall(const QString& to, const QString& message, const QString& id = "t1")
    {
        return FakeApi::toolUseReply("message_agent", QJsonObject{{"to", to}, {"message", message}}, id);
    }

    // Asks and waits for the answer; `ok` tells whether the task succeeded.
    static QString askAndWait(QtLLM::Agent& agent, const QString& prompt, bool* ok = nullptr)
    {
        QString answer;
        bool done = false;
        agent.ask(prompt, [&](const QString& text, bool success) {
            answer = text;
            if (ok)
                *ok = success;
            done = true;
        });
        spin([&]() { return done; });
        return answer;
    }

    // Off unless the application asks for it: no tools, not addressable.
    TEST_FUNCTION(testMessagingIsOptIn)
    {
        TEST_START;

        FakeApi api;
        QtLLM::Agent quiet(config("quiet", api, false));
        QtLLM::Agent talker(config("talker", api, true));
        TEST_ASSERT(!quiet.toolNames().contains("message_agent"));
        TEST_ASSERT(talker.toolNames().contains("message_agent"));
        TEST_ASSERT(talker.toolNames().contains("list_agents"));
    }

    TEST_FUNCTION(testAgentAsksAnotherAgent)
    {
        TEST_START;

        FakeApi apiA, apiB;
        apiA.replies.enqueue(messageCall("Critic", "Is 'Sunny Chat' a good title?"));   // case does not matter
        apiA.replies.enqueue(FakeApi::textReply("The critic likes it."));
        apiB.replies.enqueue(FakeApi::textReply("Yes, it is cheerful."));

        QtLLM::Agent writer(config("writer", apiA));
        QtLLM::Agent critic(config("critic", apiB));

        bool ok = false;
        TEST_COMPARE(askAndWait(writer, "Get the title reviewed.", &ok), QString("The critic likes it."));
        TEST_ASSERT(ok);

        // The critic got the message as a task, with the sender's name.
        TEST_ASSERT(apiB.messageBodies.value(0).contains("Message from agent 'writer'"));
        TEST_ASSERT(apiB.messageBodies.value(0).contains("Sunny Chat"));
        TEST_ASSERT(!apiB.messageBodies.value(0).contains("Get the title reviewed"));   // nothing else of the writer
        // The answer came back as the tool result.
        TEST_ASSERT(apiA.messageBodies.value(1).contains("Yes, it is cheerful."));
        TEST_ASSERT(apiA.messageBodies.value(1).contains("critic"));
        TEST_COMPARE(critic.completedTurns(), 1);

        // The critic keeps its conversation: a second message sees the first.
        apiA.replies.enqueue(messageCall("critic", "And 'Rainy Chat'?"));
        apiA.replies.enqueue(FakeApi::textReply("done"));
        apiB.replies.enqueue(FakeApi::textReply("Less so."));
        askAndWait(writer, "One more.");
        TEST_ASSERT(apiB.messageBodies.value(1).contains("Sunny Chat"));
        TEST_ASSERT(apiB.messageBodies.value(1).contains("Rainy Chat"));
    }

    TEST_FUNCTION(testListAgentsShowsOnlyReachableOnes)
    {
        TEST_START;

        FakeApi apiA, apiB;
        apiA.replies.enqueue(FakeApi::toolUseReply("list_agents", QJsonObject()));
        apiA.replies.enqueue(FakeApi::textReply("done"));

        QtLLM::Agent writer(config("writer", apiA));
        QtLLM::Agent critic(config("critic", apiB, true, "Reviews titles and texts."));
        QtLLM::Agent hidden(config("hidden-worker", apiB, false, "Does not take messages."));

        askAndWait(writer, "Who is there?");
        const QByteArray result = apiA.messageBodies.value(1);
        TEST_ASSERT(result.contains("critic"));
        TEST_ASSERT(result.contains("Reviews titles and texts."));
        TEST_ASSERT(result.contains("idle"));
        TEST_ASSERT(!result.contains("hidden-worker"));
        // It learns its own name, and is not listed among the others.
        TEST_ASSERT(result.contains("\\\"you\\\":\\\"writer\\\""));
        TEST_ASSERT(!result.contains("\\\"name\\\":\\\"writer\\\""));
    }

    TEST_FUNCTION(testUnknownAgentIsAnError)
    {
        TEST_START;

        FakeApi apiA, apiB;
        apiA.replies.enqueue(messageCall("nobody", "hello"));
        apiA.replies.enqueue(messageCall("hidden-worker", "hello", "t2"));
        apiA.replies.enqueue(FakeApi::textReply("gave up"));

        QtLLM::Agent writer(config("writer", apiA));
        QtLLM::Agent critic(config("critic", apiB));
        QtLLM::Agent hidden(config("hidden-worker", apiB, false));

        TEST_COMPARE(askAndWait(writer, "Try."), QString("gave up"));
        // Each refusal names who can be written to.
        TEST_ASSERT(apiA.messageBodies.value(1).contains("no agent 'nobody' accepts messages"));
        TEST_ASSERT(apiA.messageBodies.value(1).contains("critic"));
        TEST_ASSERT(apiA.messageBodies.value(2).contains("no agent 'hidden-worker' accepts messages"));
        TEST_COMPARE(apiB.messageRequests, 0);
    }

    // A asks B; B, while answering, asks A back. A cannot take that message
    // before B has answered - without the check both would wait for the
    // tool's timeout.
    TEST_FUNCTION(testWaitingForEachOtherIsRefused)
    {
        TEST_START;

        FakeApi apiA, apiB;
        apiA.replies.enqueue(messageCall("b", "question"));
        apiA.replies.enqueue(FakeApi::textReply("a is done"));
        apiB.replies.enqueue(messageCall("a", "counter question"));
        apiB.replies.enqueue(FakeApi::textReply("b answers anyway"));

        QtLLM::Agent a(config("a", apiA));
        QtLLM::Agent b(config("b", apiB));

        TEST_COMPARE(askAndWait(a, "Ask b."), QString("a is done"));
        TEST_ASSERT(apiB.messageBodies.value(1).contains("waiting for your reply"));
        TEST_ASSERT(apiA.messageBodies.value(1).contains("b answers anyway"));
        TEST_COMPARE(apiA.messageRequests, 2);   // a never received the counter question

        // Nobody is left waiting: b can write to a now.
        apiB.replies.enqueue(messageCall("a", "later question"));
        apiB.replies.enqueue(FakeApi::textReply("b is done"));
        apiA.replies.enqueue(FakeApi::textReply("a answers"));
        TEST_COMPARE(askAndWait(b, "Ask a."), QString("b is done"));
        TEST_ASSERT(apiB.messageBodies.last().contains("a answers"));
    }

    TEST_FUNCTION(testClosedAgentEndsTheCall)
    {
        TEST_START;

        FakeApi apiA, apiB;
        apiA.replies.enqueue(messageCall("b", "are you there?"));
        apiA.replies.enqueue(FakeApi::textReply("b is gone"));
        apiB.silent = true;   // b never answers

        QtLLM::Agent a(config("a", apiA));
        auto* b = new QtLLM::Agent(config("b", apiB));

        QString answer;
        a.ask("Ask b.", [&](const QString& text, bool) { answer = text; });
        TEST_ASSERT(spin([&]() { return apiB.messageRequests == 1; }));
        delete b;
        TEST_ASSERT(spin([&]() { return !answer.isEmpty(); }));
        TEST_COMPARE(answer, QString("b is gone"));
        TEST_ASSERT(apiA.messageBodies.value(1).contains("closed before it answered"));
    }

    // The chat's client is no agent; with the tools registered it can write
    // to agents all the same.
    TEST_FUNCTION(testChatCanMessageAgents)
    {
        TEST_START;

        FakeApi chatApi, agentApi;
        chatApi.replies.enqueue(messageCall("critic", "What do you think?"));
        chatApi.replies.enqueue(FakeApi::textReply("The critic approves."));
        agentApi.replies.enqueue(FakeApi::textReply("Approved."));

        QtLLM::Client chat("key", chatApi.url());
        TEST_ASSERT(!chat.toolNames().contains("message_agent"));
        QtLLM::registerAgentMessagingTools(&chat);
        QtLLM::Agent critic(config("critic", agentApi));

        QString answer;
        QObject::connect(&chat, &QtLLM::Client::responseReady, [&](const QString& text) { answer = text; });
        chat.sendPrompt("Ask the critic.");
        TEST_ASSERT(spin([&]() { return !answer.isEmpty(); }));
        TEST_COMPARE(answer, QString("The critic approves."));
        TEST_ASSERT(agentApi.messageBodies.value(0).contains("Message from the main assistant"));
        TEST_ASSERT(chatApi.messageBodies.value(1).contains("Approved."));
    }

    // A subagent shares the chat's tools - but not these two, whose handlers
    // would send in the chat's name.
    TEST_FUNCTION(testSubagentsDoNotSpeakInTheChatsName)
    {
        TEST_START;

        FakeApi chatApi, subApi;
        chatApi.replies.enqueue(FakeApi::toolUseReply("run_subagent", QJsonObject{{"task", "work"}}));
        chatApi.replies.enqueue(FakeApi::textReply("done"));
        subApi.replies.enqueue(FakeApi::textReply("worked"));

        QtLLM::Client chat("key", chatApi.url());
        QtLLM::registerAgentMessagingTools(&chat);
        QtLLM::registerSubagentTool(&chat, [&]() { return config("sub", subApi, false); });

        QString answer;
        QObject::connect(&chat, &QtLLM::Client::responseReady, [&](const QString& text) { answer = text; });
        chat.sendPrompt("Delegate.");
        TEST_ASSERT(spin([&]() { return !answer.isEmpty(); }));
        TEST_ASSERT(chatApi.messageBodies.value(0).contains("message_agent"));
        TEST_ASSERT(!subApi.messageBodies.value(0).contains("message_agent"));
        TEST_ASSERT(!subApi.messageBodies.value(0).contains("list_agents"));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
};

TEST_INSTANTIATE(TST_AgentMessaging);
