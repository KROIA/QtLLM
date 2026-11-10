#pragma once

#include "UnitTest.h"
#include "QtLLM.h"
#include "OpenAiProtocol.h"
#include "FakeOpenAi.h"
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <functional>

// OpenAiProtocol driven directly against a canned Chat Completions endpoint.
class TST_OpenAiProtocol : public UnitTest::Test
{
    TEST_CLASS(TST_OpenAiProtocol)
public:
    TST_OpenAiProtocol()
        : Test("TST_OpenAiProtocol")
    {
        ADD_TEST(TST_OpenAiProtocol::testPlainAnswer);
        ADD_TEST(TST_OpenAiProtocol::testRequestBody);
        ADD_TEST(TST_OpenAiProtocol::testToolAnswersLater);
        ADD_TEST(TST_OpenAiProtocol::testResultsKeepCallOrder);
        ADD_TEST(TST_OpenAiProtocol::testStreamedText);
        ADD_TEST(TST_OpenAiProtocol::testStreamedToolCallInFragments);
        ADD_TEST(TST_OpenAiProtocol::testInvalidArgumentsAreAnsweredWithAnError);
        ADD_TEST(TST_OpenAiProtocol::testImageParts);
        ADD_TEST(TST_OpenAiProtocol::testToolImagesFollowAsUserMessage);
        ADD_TEST(TST_OpenAiProtocol::testRejectedStreamOptionsIsRetriedWithout);
        ADD_TEST(TST_OpenAiProtocol::testHttpErrorRollsBackHistory);
        ADD_TEST(TST_OpenAiProtocol::testCancelTurn);
        ADD_TEST(TST_OpenAiProtocol::testModelList);
    }

private:

    struct Outcome
    {
        QString           response;
        QStringList       errors;
        QStringList       deltas;
        int               finished = 0;
        bool              ended = false;
        QtLLM::UsageStats stats;
    };

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

    // Starts a turn and spins until it answers or fails (or 5 s pass).
    static Outcome runTurn(QtLLM::OpenAiProtocol& protocol, const std::function<void()>& start)
    {
        Outcome outcome;
        QEventLoop loop;
        QList<QMetaObject::Connection> connections;
        connections << QObject::connect(&protocol, &QtLLM::ProtocolBase::responseReady, [&](const QString& text) {
            outcome.response = text;
            outcome.ended = true;
            loop.quit();
        });
        connections << QObject::connect(&protocol, &QtLLM::ProtocolBase::errorOccurred, [&](const QString& message) {
            outcome.errors.append(message);
            outcome.ended = true;
            loop.quit();
        });
        connections << QObject::connect(&protocol, &QtLLM::ProtocolBase::responseDelta, [&](const QString& text) {
            outcome.deltas.append(text);
        });
        connections << QObject::connect(&protocol, &QtLLM::ProtocolBase::requestFinished, [&]() {
            ++outcome.finished;
        });
        connections << QObject::connect(&protocol, &QtLLM::ProtocolBase::statsReady, [&](const QtLLM::UsageStats& s) {
            outcome.stats = s;
        });
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        start();
        if (!outcome.ended)
            loop.exec();
        for (const QMetaObject::Connection& c : connections)
            QObject::disconnect(c);
        return outcome;
    }

    static Outcome ask(QtLLM::OpenAiProtocol& protocol, const QString& prompt)
    {
        return runTurn(protocol, [&]() { protocol.beginTurn(prompt); });
    }

    static QJsonObject weatherSchema()
    {
        return QJsonObject{{"type", "function"}, {"function", QJsonObject{
            {"name", "get_weather"}, {"description", "Weather of a city"},
            {"parameters", QJsonObject{{"type", "object"},
                {"properties", QJsonObject{{"city", QJsonObject{{"type", "string"}}}}}}}}}};
    }

    static QJsonArray messagesOf(const QJsonObject& body)
    {
        return body["messages"].toArray();
    }

    // Streaming is on by default; a server answering with plain JSON still works.
    TEST_FUNCTION(testPlainAnswer)
    {
        TEST_START;

        FakeOpenAi api;
        api.enqueue(FakeOpenAi::completion("Hello"));
        api.enqueue(FakeOpenAi::completion("Half an ans", QJsonArray(), "length"));
        QtLLM::OpenAiProtocol protocol(api.baseUrl(), "");

        const Outcome outcome = ask(protocol, "hi");
        TEST_ASSERT(outcome.errors.isEmpty());
        TEST_COMPARE(outcome.response, QString("Hello"));
        TEST_COMPARE(outcome.finished, 1);
        TEST_COMPARE(protocol.conversationMessages().size(), 2);
        TEST_COMPARE(outcome.stats.inputTokens, 10);
        TEST_COMPARE(outcome.stats.outputTokens, 3);
        TEST_COMPARE(outcome.stats.contextTokens, 13);
        TEST_COMPARE(outcome.stats.sessionTurnCount, 1);

        const Outcome cut = ask(protocol, "more");
        TEST_ASSERT(cut.response.startsWith("Half an ans"));
        TEST_ASSERT(cut.response.contains("truncated"));
        TEST_COMPARE(cut.stats.sessionInputTokens, 20);
    }

    TEST_FUNCTION(testRequestBody)
    {
        TEST_START;

        FakeOpenAi api;
        for (int i = 0; i < 3; ++i)
            api.enqueue(FakeOpenAi::completion("ok"));
        // The full endpoint url (trailing slash included) works as well as the base.
        QtLLM::OpenAiProtocol protocol(api.baseUrl() + "/chat/completions/", "sk-test");
        protocol.setModel("m1");
        protocol.setSystemPrompt("Be brief.");
        protocol.setTools({weatherSchema()},
                          {{"get_weather", [](const QJsonObject&, QtLLM::ToolResultCallback done) { done({}); }}});

        ask(protocol, "hi");
        const QJsonObject first = api.body(0);
        const QJsonArray messages = messagesOf(first);
        TEST_COMPARE(messages.size(), 2);
        TEST_COMPARE(messages[0].toObject()["role"].toString(), QString("system"));
        TEST_COMPARE(messages[0].toObject()["content"].toString(), QString("Be brief."));
        TEST_COMPARE(messages[1].toObject()["role"].toString(), QString("user"));
        TEST_COMPARE(first["model"].toString(), QString("m1"));
        TEST_COMPARE(first["tools"].toArray().size(), 1);
        TEST_ASSERT(first.contains("max_tokens"));
        TEST_ASSERT(!first.contains("parallel_tool_calls"));
        TEST_ASSERT(!first.contains("reasoning_effort"));
        TEST_ASSERT(first["stream"].toBool());
        TEST_ASSERT(first["stream_options"].toObject()["include_usage"].toBool());
        TEST_ASSERT(api.heads.value(0).contains("authorization: bearer sk-test"));

        protocol.setParallelToolCalls(false);
        protocol.setEffort(QtLLM::Effort::XHigh);
        ask(protocol, "again");
        const QJsonObject second = api.body(1);
        TEST_ASSERT(second["parallel_tool_calls"].isBool());
        TEST_ASSERT(!second["parallel_tool_calls"].toBool());
        TEST_COMPARE(second["reasoning_effort"].toString(), QString("high"));

        protocol.setApiKey("");
        ask(protocol, "no key");
        TEST_ASSERT(!api.heads.value(2).contains("authorization:"));
    }

    TEST_FUNCTION(testToolAnswersLater)
    {
        TEST_START;

        FakeOpenAi api;
        api.enqueue(FakeOpenAi::completion("", QJsonArray{
            FakeOpenAi::toolCall("c1", "get_weather", "{\"city\":\"Rome\"}")}, "tool_calls"));
        api.enqueue(FakeOpenAi::completion("Sunny"));
        QtLLM::OpenAiProtocol protocol(api.baseUrl(), "");

        QJsonObject received;
        int invoked = 0, completed = 0;
        QObject::connect(&protocol, &QtLLM::ProtocolBase::toolInvoked, [&]() { ++invoked; });
        QObject::connect(&protocol, &QtLLM::ProtocolBase::toolCompleted, [&]() { ++completed; });
        protocol.setTools({weatherSchema()},
                          {{"get_weather", [&](const QJsonObject& input, QtLLM::ToolResultCallback done) {
                                received = input;
                                QTimer::singleShot(50, [done]() { done(QJsonObject{{"temp", 21}}); });
                            }}});

        const Outcome outcome = ask(protocol, "weather?");
        TEST_ASSERT(outcome.errors.isEmpty());
        TEST_COMPARE(outcome.response, QString("Sunny"));
        TEST_COMPARE(received["city"].toString(), QString("Rome"));
        TEST_COMPARE(invoked, 1);
        TEST_COMPARE(completed, 1);
        TEST_COMPARE(outcome.stats.toolCalls, 1);
        TEST_COMPARE(outcome.finished, 1);

        const QJsonArray messages = messagesOf(api.body(1));
        TEST_COMPARE(messages.size(), 3);
        const QJsonObject assistant = messages[1].toObject();
        TEST_COMPARE(assistant["tool_calls"].toArray()[0].toObject()["id"].toString(), QString("c1"));
        const QJsonObject tool = messages[2].toObject();
        TEST_COMPARE(tool["role"].toString(), QString("tool"));
        TEST_COMPARE(tool["tool_call_id"].toString(), QString("c1"));
        TEST_ASSERT(tool["content"].toString().contains("21"));
        TEST_COMPARE(protocol.conversationMessages().size(), 4);
    }

    TEST_FUNCTION(testResultsKeepCallOrder)
    {
        TEST_START;

        FakeOpenAi api;
        api.enqueue(FakeOpenAi::completion("", QJsonArray{
            FakeOpenAi::toolCall("c1", "slow", "{}"),
            FakeOpenAi::toolCall("c2", "fast", "{}")}, "tool_calls"));
        api.enqueue(FakeOpenAi::completion("both"));
        QtLLM::OpenAiProtocol protocol(api.baseUrl(), "");

        QStringList order;
        protocol.setTools({}, {
            {"slow", [&](const QJsonObject&, QtLLM::ToolResultCallback done) {
                QTimer::singleShot(80, [done, &order]() {
                    order.append("slow");
                    done(QJsonObject{{"who", "slow"}});
                });
            }},
            {"fast", [&](const QJsonObject&, QtLLM::ToolResultCallback done) {
                order.append("fast");
                done(QJsonObject{{"who", "fast"}});
            }}});

        const Outcome outcome = ask(protocol, "go");
        TEST_COMPARE(outcome.response, QString("both"));
        TEST_COMPARE(order.join(","), QString("fast,slow"));
        const QJsonArray messages = messagesOf(api.body(1));
        TEST_COMPARE(messages.size(), 4);
        TEST_COMPARE(messages[2].toObject()["tool_call_id"].toString(), QString("c1"));
        TEST_ASSERT(messages[2].toObject()["content"].toString().contains("slow"));
        TEST_COMPARE(messages[3].toObject()["tool_call_id"].toString(), QString("c2"));
    }

    TEST_FUNCTION(testStreamedText)
    {
        TEST_START;

        FakeOpenAi api;
        api.enqueue(FakeOpenAi::sse({
            FakeOpenAi::deltaChunk({{"role", "assistant"}, {"content", "Hel"}}),
            FakeOpenAi::deltaChunk({{"content", "lo"}}),
            FakeOpenAi::deltaChunk({{"content", " world"}}),
            FakeOpenAi::deltaChunk({}, "stop")}));
        QtLLM::OpenAiProtocol protocol(api.baseUrl(), "");

        const Outcome outcome = ask(protocol, "hi");
        TEST_ASSERT(outcome.errors.isEmpty());
        TEST_COMPARE(outcome.deltas.join("|"), QString("Hel|lo| world"));
        TEST_COMPARE(outcome.response, QString("Hello world"));
        TEST_COMPARE(outcome.stats.inputTokens, 20);
        TEST_COMPARE(outcome.stats.contextTokens, 25);
    }

    // Arguments arrive as string pieces, and the reply is cut across TCP writes.
    TEST_FUNCTION(testStreamedToolCallInFragments)
    {
        TEST_START;

        FakeOpenAi api;
        api.splitWrites = true;
        auto piece = [](const QString& arguments) {
            return QJsonObject{{"tool_calls", QJsonArray{QJsonObject{
                {"index", 0}, {"function", QJsonObject{{"arguments", arguments}}}}}}};
        };
        const QJsonObject start{{"tool_calls", QJsonArray{QJsonObject{
            {"index", 0}, {"id", "c1"}, {"type", "function"},
            {"function", QJsonObject{{"name", "get_weather"}, {"arguments", ""}}}}}}};
        api.enqueue(FakeOpenAi::sse({
            FakeOpenAi::deltaChunk(start),
            FakeOpenAi::deltaChunk(piece("{\"ci")),
            FakeOpenAi::deltaChunk(piece("ty\":\"Ber")),
            FakeOpenAi::deltaChunk(piece("lin\"}")),
            FakeOpenAi::deltaChunk({}, "tool_calls")}));
        api.enqueue(FakeOpenAi::completion("Done"));
        QtLLM::OpenAiProtocol protocol(api.baseUrl(), "");

        QJsonObject received;
        protocol.setTools({weatherSchema()},
                          {{"get_weather", [&](const QJsonObject& input, QtLLM::ToolResultCallback done) {
                                received = input;
                                done(QJsonObject{{"ok", true}});
                            }}});

        const Outcome outcome = ask(protocol, "weather?");
        TEST_ASSERT(outcome.errors.isEmpty());
        TEST_COMPARE(outcome.response, QString("Done"));
        TEST_COMPARE(received["city"].toString(), QString("Berlin"));
        const QJsonObject call = messagesOf(api.body(1))[1].toObject()["tool_calls"].toArray()[0].toObject();
        TEST_COMPARE(call["id"].toString(), QString("c1"));
        TEST_COMPARE(call["function"].toObject()["arguments"].toString(), QString("{\"city\":\"Berlin\"}"));
    }

    TEST_FUNCTION(testInvalidArgumentsAreAnsweredWithAnError)
    {
        TEST_START;

        FakeOpenAi api;
        api.enqueue(FakeOpenAi::completion("", QJsonArray{
            FakeOpenAi::toolCall("c1", "get_weather", "{\"city\": ")}, "tool_calls"));
        api.enqueue(FakeOpenAi::completion("Retried"));
        QtLLM::OpenAiProtocol protocol(api.baseUrl(), "");

        bool called = false;
        int invoked = 0;
        QObject::connect(&protocol, &QtLLM::ProtocolBase::toolInvoked, [&]() { ++invoked; });
        protocol.setTools({weatherSchema()},
                          {{"get_weather", [&](const QJsonObject&, QtLLM::ToolResultCallback done) {
                                called = true;
                                done({});
                            }}});

        const Outcome outcome = ask(protocol, "weather?");
        TEST_ASSERT(outcome.errors.isEmpty());
        TEST_COMPARE(outcome.response, QString("Retried"));
        TEST_ASSERT(!called);
        TEST_COMPARE(invoked, 0);
        const QJsonObject tool = messagesOf(api.body(1))[2].toObject();
        TEST_COMPARE(tool["tool_call_id"].toString(), QString("c1"));
        TEST_ASSERT(tool["content"].toString().contains("not valid JSON"));
    }

    TEST_FUNCTION(testImageParts)
    {
        TEST_START;

        FakeOpenAi api;
        api.enqueue(FakeOpenAi::completion("A cat"));
        QtLLM::OpenAiProtocol protocol(api.baseUrl(), "");
        TEST_ASSERT(protocol.supportsImageInput());

        const QJsonArray images{QJsonObject{{"media_type", "image/png"}, {"data", "AAAA"},
                                            {"width", 1}, {"height", 1}}};
        const Outcome outcome = runTurn(protocol, [&]() { protocol.beginTurn("look", images); });
        TEST_COMPARE(outcome.response, QString("A cat"));

        const QJsonArray parts = messagesOf(api.body(0))[0].toObject()["content"].toArray();
        TEST_COMPARE(parts.size(), 2);
        TEST_COMPARE(parts[0].toObject()["type"].toString(), QString("text"));
        TEST_COMPARE(parts[0].toObject()["text"].toString(), QString("look"));
        TEST_COMPARE(parts[1].toObject()["type"].toString(), QString("image_url"));
        TEST_COMPARE(parts[1].toObject()["image_url"].toObject()["url"].toString(),
                     QString("data:image/png;base64,AAAA"));
    }

    TEST_FUNCTION(testToolImagesFollowAsUserMessage)
    {
        TEST_START;

        FakeOpenAi api;
        api.enqueue(FakeOpenAi::completion("", QJsonArray{
            FakeOpenAi::toolCall("c1", "shot", "{}")}, "tool_calls"));
        api.enqueue(FakeOpenAi::completion("Seen"));
        QtLLM::OpenAiProtocol protocol(api.baseUrl(), "");
        protocol.setTools({}, {{"shot", [](const QJsonObject&, QtLLM::ToolResultCallback done) {
            QJsonObject result{{"status", "ok"}};
            result[QtLLM::kToolImagesKey] = QJsonArray{QJsonObject{{"media_type", "image/png"}, {"data", "BBBB"}}};
            done(result);
        }}});

        const Outcome outcome = ask(protocol, "look");
        TEST_COMPARE(outcome.response, QString("Seen"));
        const QJsonArray messages = messagesOf(api.body(1));
        TEST_COMPARE(messages.size(), 4);
        TEST_ASSERT(!messages[2].toObject()["content"].toString().contains("BBBB"));
        TEST_ASSERT(messages[2].toObject()["content"].toString().contains("\"images\":1"));
        const QJsonObject pictures = messages[3].toObject();
        TEST_COMPARE(pictures["role"].toString(), QString("user"));
        TEST_COMPARE(pictures["content"].toArray()[1].toObject()["image_url"].toObject()["url"].toString(),
                     QString("data:image/png;base64,BBBB"));
    }

    TEST_FUNCTION(testRejectedStreamOptionsIsRetriedWithout)
    {
        TEST_START;

        FakeOpenAi api;
        api.enqueue("{\"error\":{\"message\":\"Unrecognized request argument supplied: stream_options\"}}", 400);
        api.enqueue(FakeOpenAi::sse({FakeOpenAi::deltaChunk({{"content", "ok"}}),
                                     FakeOpenAi::deltaChunk({}, "stop")}));
        api.enqueue(FakeOpenAi::sse({FakeOpenAi::deltaChunk({{"content", "again"}}),
                                     FakeOpenAi::deltaChunk({}, "stop")}));
        QtLLM::OpenAiProtocol protocol(api.baseUrl(), "");

        const Outcome outcome = ask(protocol, "hi");
        TEST_ASSERT(outcome.errors.isEmpty());
        TEST_COMPARE(outcome.response, QString("ok"));
        TEST_COMPARE(api.bodies.size(), 2);
        TEST_ASSERT(api.body(0).contains("stream_options"));
        TEST_ASSERT(!api.body(1).contains("stream_options"));
        TEST_COMPARE(protocol.conversationMessages().size(), 2);

        const Outcome next = ask(protocol, "more");
        TEST_COMPARE(next.response, QString("again"));
        TEST_ASSERT(!api.body(2).contains("stream_options"));
        TEST_ASSERT(api.body(2)["stream"].toBool());
    }

    TEST_FUNCTION(testHttpErrorRollsBackHistory)
    {
        TEST_START;

        FakeOpenAi api;
        api.enqueue(FakeOpenAi::completion("first"));
        api.enqueue("{\"error\":{\"message\":\"Invalid API key\"}}", 401);
        QtLLM::OpenAiProtocol protocol(api.baseUrl(), "");

        ask(protocol, "one");
        TEST_COMPARE(protocol.conversationMessages().size(), 2);

        const Outcome outcome = ask(protocol, "two");
        TEST_ASSERT(outcome.response.isEmpty());
        TEST_COMPARE(outcome.errors.size(), 1);
        TEST_ASSERT(outcome.errors.value(0).contains("401"));
        TEST_ASSERT(outcome.errors.value(0).contains("Invalid API key"));
        TEST_COMPARE(outcome.finished, 1);
        TEST_COMPARE(protocol.conversationMessages().size(), 2);
    }

    TEST_FUNCTION(testCancelTurn)
    {
        TEST_START;

        FakeOpenAi api;
        api.silent = true;
        QtLLM::OpenAiProtocol protocol(api.baseUrl(), "");
        int finished = 0, answers = 0, errors = 0;
        QObject::connect(&protocol, &QtLLM::ProtocolBase::requestFinished, [&]() { ++finished; });
        QObject::connect(&protocol, &QtLLM::ProtocolBase::responseReady, [&]() { ++answers; });
        QObject::connect(&protocol, &QtLLM::ProtocolBase::errorOccurred, [&]() { ++errors; });

        TEST_ASSERT(!protocol.cancelTurn());
        protocol.beginTurn("hi");
        TEST_ASSERT(spinUntil([&]() { return api.bodies.size() == 1; }));
        TEST_ASSERT(protocol.cancelTurn());
        TEST_COMPARE(finished, 1);
        TEST_COMPARE(protocol.conversationMessages().size(), 0);
        TEST_ASSERT(!protocol.cancelTurn());

        spinUntil([]() { return false; }, 100);
        TEST_COMPARE(answers, 0);
        TEST_COMPARE(errors, 0);
    }

    TEST_FUNCTION(testModelList)
    {
        TEST_START;

        FakeOpenAi api;
        // A bare host gets "/v1".
        QtLLM::OpenAiProtocol protocol(QString("http://127.0.0.1:%1").arg(api.server.serverPort()), "");
        TEST_COMPARE(protocol.baseUrl(), api.baseUrl());

        QStringList models;
        bool fetched = false;
        QObject::connect(&protocol, &QtLLM::ProtocolBase::modelsFetched, [&](const QStringList& list) {
            models = list;
            fetched = true;
        });
        protocol.fetchModels();
        TEST_ASSERT(spinUntil([&]() { return fetched; }));
        TEST_COMPARE(models.join(","), QString("m1,m2"));
    }
};

TEST_INSTANTIATE(TST_OpenAiProtocol);
