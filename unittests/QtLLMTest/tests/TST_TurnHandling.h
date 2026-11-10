#pragma once

#include "UnitTest.h"
#include "QtLLM.h"
#include "FakeApi.h"
#include <QCoreApplication>
#include <QEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <atomic>
#include <functional>
#include <QImage>
#include <QThread>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQueue>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

// Turn-level behaviour of the Claude protocol against a canned local endpoint:
// what ends a turn, what counts as an error, and what is left in the history.
class TST_TurnHandling : public UnitTest::Test
{
    TEST_CLASS(TST_TurnHandling)
public:
    TST_TurnHandling()
        : Test("TST_TurnHandling")
    {
        ADD_TEST(TST_TurnHandling::testTruncatedToolUseIsNotStored);
        ADD_TEST(TST_TurnHandling::testUnknownToolIsNotATurnError);
        ADD_TEST(TST_TurnHandling::testCancelRequest);
        ADD_TEST(TST_TurnHandling::testStaleThinkingBlocksAreDroppedAndRetried);
        ADD_TEST(TST_TurnHandling::testFallbackModelPrefersCostEffective);
        ADD_TEST(TST_TurnHandling::testEnumCaseIsNormalizedForHandler);
        ADD_TEST(TST_TurnHandling::testLargeToolResultIsCapped);
        ADD_TEST(TST_TurnHandling::testEffortIsSentAndDroppedWhenRejected);
        ADD_TEST(TST_TurnHandling::testImageResultIsSentAsImageBlock);
        ADD_TEST(TST_TurnHandling::testStreamedAnswerArrivesInPieces);
        ADD_TEST(TST_TurnHandling::testStreamedToolCallSurvivesSplitChunks);
        ADD_TEST(TST_TurnHandling::testStreamingCanBeTurnedOff);
        ADD_TEST(TST_TurnHandling::testInventedParameterIsRejectedWhenValidating);
        ADD_TEST(TST_TurnHandling::testCompactionReplacesHistoryWithSummary);
        ADD_TEST(TST_TurnHandling::testAutoCompactionAtTheLimit);
        ADD_TEST(TST_TurnHandling::testDropOldestKeepsToolRoundsWhole);
        ADD_TEST(TST_TurnHandling::testToolsDoNotRunWhileSummarizing);
        ADD_TEST(TST_TurnHandling::testFailedCompactionLeavesConversationAlone);
        ADD_TEST(TST_TurnHandling::testAsyncToolAnswersLater);
        ADD_TEST(TST_TurnHandling::testAsyncResultsKeepCallOrder);
        ADD_TEST(TST_TurnHandling::testAsyncToolTimesOut);
        ADD_TEST(TST_TurnHandling::testBackgroundToolRunsOffTheGuiThread);
        ADD_TEST(TST_TurnHandling::testCancelWhileAsyncToolIsPending);
        ADD_TEST(TST_TurnHandling::testWaitToolPausesTheTurn);
        ADD_TEST(TST_TurnHandling::testSubagentToolDelegatesAndCleansUp);
        ADD_TEST(TST_TurnHandling::testSubagentToolLimitsParallelAgents);
        ADD_TEST(TST_TurnHandling::testClaudeCodeProviderConstruction);
    }

private:

    struct Outcome
    {
        QString     response;
        QStringList errors;
        bool        finished = false;
    };

    // Runs the event loop until the condition holds (or the timeout passes).
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

    // Sends one prompt and spins until the turn ends (or 5 s pass).
    static Outcome runTurn(QtLLM::Client& client, const QString& prompt)
    {
        Outcome outcome;
        QEventLoop loop;
        auto c1 = QObject::connect(&client, &QtLLM::Client::responseReady, [&](const QString& text) {
            outcome.response = text;
            outcome.finished = true;
            loop.quit();
        });
        auto c2 = QObject::connect(&client, &QtLLM::Client::errorOccurred, [&](const QString& message) {
            outcome.errors.append(message);
        });
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        client.sendPrompt(prompt);
        loop.exec();
        QObject::disconnect(c1);
        QObject::disconnect(c2);
        return outcome;
    }

    // A response cut off by max_tokens can end in a tool_use block that never
    // gets a tool_result. Stored as-is, it makes every later request fail.
    TEST_FUNCTION(testTruncatedToolUseIsNotStored)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::reply("max_tokens", QJsonArray{
            QJsonObject{{"type", "text"}, {"text", "partial"}},
            QJsonObject{{"type", "tool_use"}, {"id", "t1"}, {"name", "x"}, {"input", QJsonObject()}}}));

        QtLLM::Client client("key", api.url());
        const Outcome outcome = runTurn(client, "hi");

        TEST_ASSERT(outcome.finished);
        TEST_ASSERT(outcome.errors.isEmpty());
        TEST_ASSERT(outcome.response.startsWith("partial"));
        TEST_ASSERT(outcome.response.contains("truncated"));

        const QByteArray stored = QJsonDocument(client.exportConversation()["messages"].toArray())
                                      .toJson(QJsonDocument::Compact);
        TEST_ASSERT(stored.contains("partial"));
        TEST_ASSERT(!stored.contains("tool_use"));
    }

    // The model inventing a tool name is its mistake to recover from: it gets
    // an error result, the turn carries on and the application sees no error.
    TEST_FUNCTION(testUnknownToolIsNotATurnError)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::reply("tool_use", QJsonArray{
            QJsonObject{{"type", "tool_use"}, {"id", "t1"}, {"name", "nope"}, {"input", QJsonObject()}}}));
        api.replies.enqueue(FakeApi::reply("end_turn", QJsonArray{
            QJsonObject{{"type", "text"}, {"text", "done"}}}));

        QtLLM::Client client("key", api.url());
        const Outcome outcome = runTurn(client, "hi");

        TEST_ASSERT(outcome.errors.isEmpty());
        TEST_COMPARE(outcome.response, QString("done"));
        TEST_COMPARE(api.messageRequests, 2);
        TEST_COMPARE(client.conversationHistory().size(), 2);   // user + assistant
    }

    TEST_FUNCTION(testCancelRequest)
    {
        TEST_START;

        FakeApi api;
        api.silent = true;

        QtLLM::Client client("key", api.url());
        int finished = 0, responses = 0, errors = 0;
        QObject::connect(&client, &QtLLM::Client::requestFinished, [&]() { ++finished; });
        QObject::connect(&client, &QtLLM::Client::responseReady, [&](const QString&) { ++responses; });
        QObject::connect(&client, &QtLLM::Client::errorOccurred, [&](const QString&) { ++errors; });

        client.cancelRequest();              // idle: nothing happens
        TEST_COMPARE(finished, 0);

        client.sendPrompt("first");
        client.sendPrompt("queued");
        client.cancelRequest();

        QEventLoop loop;
        QTimer::singleShot(300, &loop, &QEventLoop::quit);
        loop.exec();

        TEST_COMPARE(finished, 1);
        TEST_COMPARE(responses, 0);
        TEST_COMPARE(errors, 0);
        TEST_ASSERT(client.exportConversation()["messages"].toArray().isEmpty());

        // The client is usable again afterwards.
        api.silent = false;
        api.replies.enqueue(FakeApi::reply("end_turn", QJsonArray{QJsonObject{{"type", "text"}, {"text", "ok"}}}));
        TEST_COMPARE(runTurn(client, "again").response, QString("ok"));
    }

    // Changing the tool list mid-conversation invalidates earlier thinking
    // blocks; the API then rejects the request. The turn must recover by
    // itself instead of surfacing the error.
    TEST_FUNCTION(testStaleThinkingBlocksAreDroppedAndRetried)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::reply("end_turn", QJsonArray{
            QJsonObject{{"type", "thinking"}, {"thinking", ""}, {"signature", "SIG-ONE"}},
            QJsonObject{{"type", "text"}, {"text", "first"}}}));
        api.replies.enqueue(QByteArray(
            "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":"
            "\"messages.1.content.0: Invalid `signature` in `thinking` block. "
            "The block is bound to a different conversation.\"}}"));
        api.replies.enqueue(FakeApi::textReply("second"));

        QtLLM::Client client("key", api.url());
        TEST_COMPARE(runTurn(client, "one").response, QString("first"));

        QtLLM::Tool tool;
        tool.setName("added_later").setDescription("changes the tool list");
        client.registerTool(tool, [](const QJsonObject&) { return QJsonObject(); });

        const Outcome outcome = runTurn(client, "two");
        TEST_ASSERT(outcome.errors.isEmpty());
        TEST_COMPARE(outcome.response, QString("second"));
        TEST_COMPARE(api.messageRequests, 3);
        TEST_ASSERT(api.messageBodies.value(1).contains("SIG-ONE"));    // rejected attempt
        TEST_ASSERT(!api.messageBodies.value(2).contains("SIG-ONE"));   // retry without thinking
        TEST_ASSERT(api.messageBodies.value(2).contains("first"));      // text survives
    }

    // An unavailable model must not be replaced by whatever the provider
    // happens to list first (on the Anthropic API: the most expensive one).
    TEST_FUNCTION(testFallbackModelPrefersCostEffective)
    {
        TEST_START;

        using QtLLM::Client;
        using QtLLM::Provider;
        TEST_COMPARE(Client::defaultModel(Provider::Claude), QString("claude-sonnet-5-5"));
        TEST_COMPARE(Client::pickFallbackModel(Provider::Claude,
                         {"claude-fable-5-2", "claude-opus-5-5", "claude-sonnet-5-5", "claude-haiku-5-5"}),
                     QString("claude-sonnet-5-5"));
        TEST_COMPARE(Client::pickFallbackModel(Provider::Claude,
                         {"claude-auto", "claude-opus-5-5", "claude-haiku-5-5"}),
                     QString("claude-haiku-5-5"));
        TEST_COMPARE(Client::pickFallbackModel(Provider::Claude, {"some-gateway-model"}),
                     QString("some-gateway-model"));
        TEST_COMPARE(Client::pickFallbackModel(Provider::Ollama, {"qwen2.5:7b", "llama3.2"}),
                     QString("llama3.2"));
    }

    // Validation accepts "OpenFile" for the enum value "openFile"; the handler
    // compares exactly, so it has to receive the declared spelling.
    TEST_FUNCTION(testEnumCaseIsNormalizedForHandler)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::toolUseReply("pick",
            QJsonObject{{"mode", "OpenFile"}, {"note", "Keep Me"}}));
        api.replies.enqueue(FakeApi::textReply("done"));

        QtLLM::Client client("key", api.url());
        client.setValidateToolInput(true);
        QtLLM::Tool tool;
        tool.setName("pick").setDescription("enum test")
            .addEnumParameter("mode", {"openFile", "saveFile"}, "mode", true)
            .addParameter("note", "string", "free text", false);
        QJsonObject received;
        client.registerTool(tool, [&](const QJsonObject& args) {
            received = args;
            return QtLLM::toolOk();
        });

        TEST_COMPARE(runTurn(client, "go").response, QString("done"));
        TEST_COMPARE(received["mode"].toString(), QString("openFile"));
        TEST_COMPARE(received["note"].toString(), QString("Keep Me"));   // non-enum untouched
    }

    // A tool result stays in the conversation for good; an oversized one must
    // reach the model shortened, as valid JSON that says so.
    TEST_FUNCTION(testLargeToolResultIsCapped)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::toolUseReply("dump", QJsonObject()));
        api.replies.enqueue(FakeApi::toolUseReply("small", QJsonObject(), "t2"));
        api.replies.enqueue(FakeApi::textReply("done"));

        QtLLM::Client client("key", api.url());
        client.setMaxToolResultChars(1000);
        QtLLM::Tool dump, little;
        dump.setName("dump").setDescription("returns a lot");
        little.setName("small").setDescription("returns little");
        client.registerTool(dump, [](const QJsonObject&) {
            return QtLLM::toolOk({{"text", QString(30000, QChar('x'))}});
        });
        client.registerTool(little, [](const QJsonObject&) {
            return QtLLM::toolOk({{"text", "tiny"}});
        });
        QStringList truncated;
        QObject::connect(&client, &QtLLM::Client::toolResultTruncated,
                         [&](const QString& name, int) { truncated.append(name); });

        TEST_COMPARE(runTurn(client, "go").response, QString("done"));
        TEST_COMPARE(truncated, QStringList{"dump"});

        const QByteArray second = api.messageBodies.value(1);
        TEST_ASSERT(second.contains("truncated"));
        TEST_ASSERT(second.contains("original_chars"));
        TEST_ASSERT(second.size() < 6000);                       // the 30k payload is gone
        TEST_ASSERT(api.messageBodies.value(2).contains("tiny")); // small results untouched
    }

    TEST_FUNCTION(testEffortIsSentAndDroppedWhenRejected)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(QByteArray(
            "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\","
            "\"message\":\"output_config.effort: Extra inputs are not permitted\"}}"));
        api.replies.enqueue(FakeApi::textReply("one"));
        api.replies.enqueue(FakeApi::textReply("two"));

        QtLLM::Client client("key", api.url());
        client.setEffort(QtLLM::Effort::Low);

        const Outcome first = runTurn(client, "a");
        TEST_ASSERT(first.errors.isEmpty());
        TEST_COMPARE(first.response, QString("one"));
        TEST_ASSERT(api.messageBodies.value(0).contains("\"output_config\":{\"effort\":\"low\"}"));
        TEST_ASSERT(!api.messageBodies.value(1).contains("output_config"));

        // Remembered for the model: the next turn does not try again.
        TEST_COMPARE(runTurn(client, "b").response, QString("two"));
        TEST_COMPARE(api.messageRequests, 3);
        TEST_ASSERT(!api.messageBodies.value(2).contains("output_config"));
    }

    // A picture returned by a tool must reach the model as an image block,
    // not as base64 text inside the JSON.
    TEST_FUNCTION(testImageResultIsSentAsImageBlock)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::toolUseReply("shot", QJsonObject()));
        api.replies.enqueue(FakeApi::textReply("seen"));

        QtLLM::Client client("key", api.url());
        client.setMaxToolResultChars(50);   // the cap must not touch the image
        QtLLM::Tool tool;
        tool.setName("shot").setDescription("returns a picture");
        client.registerTool(tool, [](const QJsonObject&) {
            QImage image(3000, 10, QImage::Format_RGB32);
            image.fill(Qt::red);
            return QtLLM::toolImage(image, {{"what", "red"}});
        });
        QJsonObject completed;
        QObject::connect(&client, &QtLLM::Client::toolCompleted,
                         [&](const QString&, const QJsonObject& result) { completed = result; });

        TEST_COMPARE(runTurn(client, "look").response, QString("seen"));

        const QByteArray second = api.messageBodies.value(1);
        TEST_ASSERT(second.contains("\"type\":\"image\""));
        TEST_ASSERT(second.contains("\"media_type\":\"image/png\""));
        TEST_ASSERT(!second.contains("$images"));
        // Listeners get the JSON without megabytes of base64.
        TEST_COMPARE(completed["images"].toInt(), 1);
        TEST_ASSERT(!completed.contains(QtLLM::kToolImagesKey));
        TEST_COMPARE(completed["what"].toString(), QString("red"));

        // Scaled down to the default maximum edge before sending.
        QImage probe(3000, 10, QImage::Format_RGB32);
        const QJsonObject direct = QtLLM::toolImage(probe);
        TEST_COMPARE(direct[QtLLM::kToolImagesKey].toArray().first().toObject()["width"].toInt(), 1568);
    }

    TEST_FUNCTION(testStreamedAnswerArrivesInPieces)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::sse("end_turn", QJsonArray{
            QJsonObject{{"type", "text"}, {"text", "Hello, streamed world"}}}));

        QtLLM::Client client("key", api.url());
        QStringList deltas;
        QObject::connect(&client, &QtLLM::Client::responseDelta,
                         [&](const QString& text) { deltas.append(text); });

        const Outcome outcome = runTurn(client, "hi");
        TEST_ASSERT(outcome.errors.isEmpty());
        TEST_COMPARE(outcome.response, QString("Hello, streamed world"));
        TEST_COMPARE(deltas.size(), 2);
        TEST_COMPARE(deltas.join(QString()), QString("Hello, streamed world"));
        TEST_ASSERT(api.messageBodies.value(0).contains("\"stream\":true"));
        // Token counts come from the stream's start and end events.
        TEST_COMPARE(client.usageStats().inputTokens, 7);
        TEST_COMPARE(client.usageStats().cacheReadInputTokens, 3);
        TEST_COMPARE(client.usageStats().outputTokens, 5);
    }

    // Tool input arrives as fragments of JSON text, and the network cuts the
    // stream wherever it likes - here in the middle of an event.
    TEST_FUNCTION(testStreamedToolCallSurvivesSplitChunks)
    {
        TEST_START;

        FakeApi api;
        api.splitWrites = true;
        api.replies.enqueue(FakeApi::sse("tool_use", QJsonArray{
            QJsonObject{{"type", "text"}, {"text", "Let me check. "}},
            QJsonObject{{"type", "tool_use"}, {"id", "t1"}, {"name", "lookup"},
                        {"input", QJsonObject{{"city", QString::fromUtf8("Z\xc3\xbcrich")}, {"days", 3}}}}}));
        api.replies.enqueue(FakeApi::sse("end_turn", QJsonArray{
            QJsonObject{{"type", "text"}, {"text", "Sunny."}}}));

        QtLLM::Client client("key", api.url());
        QtLLM::Tool tool;
        tool.setName("lookup").setDescription("weather")
            .addParameter("city", "string", "city", true)
            .addParameter("days", "integer", "days", false);
        QJsonObject received;
        client.registerTool(tool, [&](const QJsonObject& args) {
            received = args;
            return QtLLM::toolOk({{"forecast", "sun"}});
        });
        QString streamed;
        QObject::connect(&client, &QtLLM::Client::responseDelta,
                         [&](const QString& text) { streamed += text; });

        const Outcome outcome = runTurn(client, "weather?");
        TEST_ASSERT(outcome.errors.isEmpty());
        TEST_COMPARE(outcome.response, QString("Sunny."));
        TEST_COMPARE(received["city"].toString(), QString::fromUtf8("Z\xc3\xbcrich"));
        TEST_COMPARE(received["days"].toInt(), 3);
        TEST_COMPARE(streamed, QString("Let me check. Sunny."));
        TEST_COMPARE(api.messageRequests, 2);
    }

    TEST_FUNCTION(testStreamingCanBeTurnedOff)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::textReply("whole"));

        QtLLM::Client client("key", api.url());
        client.setStreamingEnabled(false);
        int deltas = 0;
        QObject::connect(&client, &QtLLM::Client::responseDelta, [&](const QString&) { ++deltas; });

        TEST_COMPARE(runTurn(client, "hi").response, QString("whole"));
        TEST_COMPARE(deltas, 0);
        TEST_ASSERT(!api.messageBodies.value(0).contains("\"stream\""));
    }

    // With validation on, a parameter the tool does not have is an error the
    // model can act on - not something silently ignored.
    TEST_FUNCTION(testInventedParameterIsRejectedWhenValidating)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::toolUseReply("resize",
            QJsonObject{{"widht", 10}, {"height", "20"}}));
        api.replies.enqueue(FakeApi::toolUseReply("resize",
            QJsonObject{{"width", 10}, {"height", "20"}}, "t2"));
        api.replies.enqueue(FakeApi::textReply("done"));

        QtLLM::Client client("key", api.url());
        client.setValidateToolInput(true);
        QtLLM::Tool tool;
        tool.setName("resize").setDescription("resize")
            .addParameter("width", "integer", "w", true)
            .addParameter("height", "integer", "h", true);
        int calls = 0;
        QJsonObject received;
        client.registerTool(tool, [&](const QJsonObject& args) {
            ++calls;
            received = args;
            return QtLLM::toolOk();
        });

        TEST_COMPARE(runTurn(client, "go").response, QString("done"));
        TEST_COMPARE(calls, 1);                                  // the misspelled call never ran
        TEST_ASSERT(api.messageBodies.value(1).contains("widht"));
        TEST_ASSERT(api.messageBodies.value(1).contains("did_you_mean"));
        TEST_ASSERT(received["height"].isDouble());              // "20" arrived as a number
        TEST_COMPARE(received["height"].toInt(), 20);
    }

    // Summarize: one request, after which the summary is the whole
    // conversation - and the next turn is built on it alone.
    TEST_FUNCTION(testCompactionReplacesHistoryWithSummary)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::textReply("answer one"));
        api.replies.enqueue(FakeApi::textReply("answer two"));
        api.replies.enqueue(FakeApi::textReply("THE-SUMMARY"));
        api.replies.enqueue(FakeApi::textReply("answer three"));

        QtLLM::Client client("key", api.url());
        TEST_ASSERT(!client.compactConversation());          // empty: nothing to do
        runTurn(client, "first question");
        runTurn(client, "second question");

        int started = 0, finished = 0, responses = 0;
        int before = -1, after = -1;
        QStringList failures;
        QObject::connect(&client, &QtLLM::Client::compactionStarted, [&]() { ++started; });
        QObject::connect(&client, &QtLLM::Client::compactionFinished, [&](int b, int a) {
            ++finished; before = b; after = a;
        });
        QObject::connect(&client, &QtLLM::Client::compactionFailed,
                         [&](const QString& m) { failures.append(m); });
        QObject::connect(&client, &QtLLM::Client::responseReady, [&](const QString&) { ++responses; });

        TEST_ASSERT(client.compactConversation());
        TEST_ASSERT(client.isCompacting());
        TEST_ASSERT(!client.compactConversation());          // one at a time
        TEST_ASSERT(spinUntil([&]() { return finished == 1 || !failures.isEmpty(); }));

        TEST_ASSERT(failures.isEmpty());
        TEST_COMPARE(started, 1);
        TEST_COMPARE(responses, 0);                          // the summary is not an answer
        TEST_ASSERT(!client.isCompacting());
        TEST_ASSERT(before > 0);
        TEST_ASSERT(after > 0);

        const QJsonArray messages = client.exportConversation()["messages"].toArray();
        TEST_COMPARE(messages.size(), 1);
        const QString only = messages.first().toObject()["content"].toString();
        TEST_ASSERT(only.contains("<conversation-summary>"));
        TEST_ASSERT(only.contains("THE-SUMMARY"));
        TEST_COMPARE(client.conversationHistory().size(), 1);

        // The summary request went out with the conversation and the instruction.
        TEST_ASSERT(api.messageBodies.value(2).contains("first question"));
        TEST_ASSERT(api.messageBodies.value(2).contains("about to be compacted"));

        // The next turn sees the summary and nothing of the old turns.
        TEST_COMPARE(runTurn(client, "third question").response, QString("answer three"));
        const QByteArray next = api.messageBodies.value(3);
        TEST_ASSERT(next.contains("THE-SUMMARY"));
        TEST_ASSERT(next.contains("third question"));
        TEST_ASSERT(!next.contains("first question"));
        TEST_ASSERT(!next.contains("answer two"));
    }

    // A user-set limit below the model's window, and compaction that starts
    // by itself when the conversation reaches it.
    TEST_FUNCTION(testAutoCompactionAtTheLimit)
    {
        TEST_START;

        FakeApi api;
        // This reply reports 7 + 3 input and 5 output tokens: a conversation of 15.
        api.replies.enqueue(FakeApi::sse("end_turn", QJsonArray{
            QJsonObject{{"type", "text"}, {"text", "long answer"}}}));
        api.replies.enqueue(FakeApi::textReply("AUTO-SUMMARY"));

        QtLLM::Client client("key", api.url());
        QtLLM::ContextPolicy policy;
        policy.autoCompact = true;
        policy.limitTokens = 16;                 // 90 % of 16 = 14.4, reached by 15
        client.setContextPolicy(policy);
        TEST_COMPARE(client.contextBreakdown().contextWindowTokens, 16);

        int finished = 0;
        QObject::connect(&client, &QtLLM::Client::compactionFinished, [&](int, int) { ++finished; });

        TEST_COMPARE(runTurn(client, "question").response, QString("long answer"));
        TEST_ASSERT(spinUntil([&]() { return finished == 1; }));
        TEST_COMPARE(api.messageRequests, 2);
        TEST_ASSERT(client.exportConversation()["messages"].toArray().first().toObject()["content"]
                        .toString().contains("AUTO-SUMMARY"));

        // Without the switch the same situation changes nothing.
        FakeApi api2;
        api2.replies.enqueue(FakeApi::sse("end_turn", QJsonArray{
            QJsonObject{{"type", "text"}, {"text", "long answer"}}}));
        QtLLM::Client manual("key", api2.url());
        policy.autoCompact = false;
        manual.setContextPolicy(policy);
        runTurn(manual, "question");
        spinUntil([]() { return false; }, 200);
        TEST_COMPARE(api2.messageRequests, 1);
        TEST_COMPARE(manual.contextBreakdown().exactUsedTokens, 15);   // straight from the reply's usage
    }

    // DropOldest removes whole turns from the front and never leaves a tool
    // result without the call it answers.
    TEST_FUNCTION(testDropOldestKeepsToolRoundsWhole)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::toolUseReply("probe", QJsonObject()));
        api.replies.enqueue(FakeApi::textReply(QString(400, QChar('a'))));
        api.replies.enqueue(FakeApi::textReply(QString(400, QChar('b'))));
        api.replies.enqueue(FakeApi::textReply("last answer"));

        QtLLM::Client client("key", api.url());
        QtLLM::Tool tool;
        tool.setName("probe").setDescription("probe");
        client.registerTool(tool, [](const QJsonObject&) { return QtLLM::toolOk(); });
        runTurn(client, "turn one");     // user, assistant(tool_use), user(tool_result), assistant
        runTurn(client, "turn two");
        runTurn(client, "turn three");
        TEST_COMPARE(client.exportConversation()["messages"].toArray().size(), 8);

        QtLLM::ContextPolicy policy;
        policy.strategy = QtLLM::ContextPolicy::Strategy::DropOldest;
        policy.limitTokens = 200;        // target: half of it
        client.setContextPolicy(policy);

        int finished = 0;
        QObject::connect(&client, &QtLLM::Client::compactionFinished, [&](int, int) { ++finished; });
        const int requestsBefore = api.messageRequests;
        TEST_ASSERT(client.compactConversation());
        TEST_COMPARE(finished, 1);                               // immediate, no request
        TEST_COMPARE(api.messageRequests, requestsBefore);

        const QJsonArray messages = client.exportConversation()["messages"].toArray();
        TEST_ASSERT(messages.size() < 8);
        TEST_ASSERT(!messages.isEmpty());
        // Starts with a prompt, not with a leftover tool result or answer.
        TEST_COMPARE(messages.first().toObject()["role"].toString(), QString("user"));
        TEST_ASSERT(messages.first().toObject()["content"].isString());
        TEST_COMPARE(messages.last().toObject()["content"].toArray().first().toObject()["text"].toString(),
                     QString("last answer"));
        const QByteArray text = QJsonDocument(messages).toJson(QJsonDocument::Compact);
        TEST_ASSERT(!text.contains("tool_result"));              // went together with its call
        TEST_ASSERT(!text.contains("turn one"));
    }

    // The summary request carries the tools; a model that calls one anyway
    // must not get it executed.
    TEST_FUNCTION(testToolsDoNotRunWhileSummarizing)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::textReply("answer"));
        api.replies.enqueue(FakeApi::toolUseReply("write_file", QJsonObject()));
        api.replies.enqueue(FakeApi::textReply("SUMMARY-AFTER-REFUSAL"));

        QtLLM::Client client("key", api.url());
        int calls = 0, invokedSignals = 0;
        QtLLM::Tool tool;
        tool.setName("write_file").setDescription("has side effects");
        client.registerTool(tool, [&](const QJsonObject&) { ++calls; return QtLLM::toolOk(); });
        QObject::connect(&client, &QtLLM::Client::toolInvoked,
                         [&](const QString&, const QJsonObject&) { ++invokedSignals; });
        runTurn(client, "question");

        int finished = 0;
        QObject::connect(&client, &QtLLM::Client::compactionFinished, [&](int, int) { ++finished; });
        TEST_ASSERT(client.compactConversation());
        TEST_ASSERT(spinUntil([&]() { return finished == 1; }));

        TEST_COMPARE(calls, 0);
        TEST_COMPARE(invokedSignals, 0);
        TEST_ASSERT(api.messageBodies.value(2).contains("not available while the conversation is being summarized"));
        TEST_ASSERT(client.exportConversation()["messages"].toArray().first().toObject()["content"]
                        .toString().contains("SUMMARY-AFTER-REFUSAL"));
    }

    TEST_FUNCTION(testFailedCompactionLeavesConversationAlone)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::textReply("answer"));
        api.replies.enqueue(QByteArray(
            "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"nope\"}}"));

        QtLLM::Client client("key", api.url());
        runTurn(client, "question");
        const QJsonArray before = client.exportConversation()["messages"].toArray();

        QStringList failures, errors;
        QObject::connect(&client, &QtLLM::Client::compactionFailed,
                         [&](const QString& m) { failures.append(m); });
        QObject::connect(&client, &QtLLM::Client::errorOccurred,
                         [&](const QString& m) { errors.append(m); });
        TEST_ASSERT(client.compactConversation());
        TEST_ASSERT(spinUntil([&]() { return !failures.isEmpty(); }));

        TEST_COMPARE(failures, QStringList{"nope"});
        TEST_ASSERT(errors.isEmpty());                           // not reported as a failed chat turn
        TEST_ASSERT(!client.isCompacting());
        TEST_ASSERT(client.exportConversation()["messages"].toArray() == before);
        TEST_COMPARE(client.conversationHistory().size(), 2);
    }

    // An asynchronous tool returns at once and reports later; the turn waits
    // for it without blocking the event loop.
    TEST_FUNCTION(testAsyncToolAnswersLater)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::toolUseReply("slow", QJsonObject{{"n", 2}}));
        api.replies.enqueue(FakeApi::textReply("done"));

        QtLLM::Client client("key", api.url());
        QtLLM::Tool tool;
        tool.setName("slow").setDescription("takes a while").addParameter("n", "integer", "n", true);
        bool handlerReturned = false;
        client.registerAsyncTool(tool, [&](const QJsonObject& input, QtLLM::ToolCompletion done) {
            const int n = input["n"].toInt();
            done.progress("working...");
            QTimer::singleShot(60, [done, n]() {
                done.finish(QtLLM::toolOk({{"doubled", n * 2}}));
                done.finish(QtLLM::toolOk({{"doubled", -1}}));   // a second answer is ignored
            });
            handlerReturned = true;
        });
        QStringList progress;
        QObject::connect(&client, &QtLLM::Client::toolProgress,
                         [&](const QString& name, const QString& text) { progress.append(name + ":" + text); });

        const Outcome outcome = runTurn(client, "go");
        TEST_ASSERT(handlerReturned);
        TEST_ASSERT(outcome.errors.isEmpty());
        TEST_COMPARE(outcome.response, QString("done"));
        TEST_COMPARE(progress, QStringList{"slow:working..."});
        TEST_ASSERT(api.messageBodies.value(1).contains("\\\"doubled\\\":4"));
        TEST_ASSERT(!api.messageBodies.value(1).contains("-1"));
        TEST_COMPARE(client.usageStats().toolCalls, 1);
    }

    // Two calls in one response: the first answers after the second. The
    // results still go back in the order of the calls.
    TEST_FUNCTION(testAsyncResultsKeepCallOrder)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::reply("tool_use", QJsonArray{
            QJsonObject{{"type", "tool_use"}, {"id", "first"}, {"name", "slow"}, {"input", QJsonObject()}},
            QJsonObject{{"type", "tool_use"}, {"id", "second"}, {"name", "fast"}, {"input", QJsonObject()}}}));
        api.replies.enqueue(FakeApi::textReply("both"));

        QtLLM::Client client("key", api.url());
        QtLLM::Tool slow, fast;
        slow.setName("slow").setDescription("slow");
        fast.setName("fast").setDescription("fast");
        QStringList order;
        client.registerAsyncTool(slow, [&](const QJsonObject&, QtLLM::ToolCompletion done) {
            QTimer::singleShot(80, [done, &order]() {
                order.append("slow");
                done.finish(QtLLM::toolOk({{"from", "SLOW"}}));
            });
        });
        client.registerTool(fast, [&](const QJsonObject&) {
            order.append("fast");
            return QtLLM::toolOk({{"from", "FAST"}});
        });

        TEST_COMPARE(runTurn(client, "go").response, QString("both"));
        TEST_COMPARE(order, (QStringList{"fast", "slow"}));        // finished in this order
        const QByteArray second = api.messageBodies.value(1);
        TEST_ASSERT(second.indexOf("SLOW") > 0);
        TEST_ASSERT(second.indexOf("SLOW") < second.indexOf("FAST"));   // sent in call order
        TEST_COMPARE(api.messageRequests, 2);                      // one request for the round
    }

    TEST_FUNCTION(testAsyncToolTimesOut)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::toolUseReply("stuck", QJsonObject()));
        api.replies.enqueue(FakeApi::textReply("gave up"));

        QtLLM::Client client("key", api.url());
        QtLLM::Tool tool;
        tool.setName("stuck").setDescription("never answers").setTimeoutMs(150);
        QtLLM::ToolCompletion kept;
        client.registerAsyncTool(tool, [&](const QJsonObject&, QtLLM::ToolCompletion done) { kept = done; });

        TEST_COMPARE(runTurn(client, "go").response, QString("gave up"));
        TEST_ASSERT(api.messageBodies.value(1).contains("did not answer"));
        TEST_ASSERT(kept.isFinished());                 // the tool can see it is too late
        kept.finish(QtLLM::toolOk());                   // and a late answer is harmless
    }

    TEST_FUNCTION(testBackgroundToolRunsOffTheGuiThread)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::toolUseReply("crunch", QJsonObject()));
        api.replies.enqueue(FakeApi::textReply("crunched"));

        QtLLM::Client client("key", api.url());
        QtLLM::Tool tool;
        tool.setName("crunch").setDescription("heavy work");
        QThread* const guiThread = QThread::currentThread();
        std::atomic<bool> ranElsewhere{false};
        client.registerBackgroundTool(tool, [&](const QJsonObject&) {
            ranElsewhere = (QThread::currentThread() != guiThread);
            QThread::msleep(50);
            return QtLLM::toolOk({{"answer", 42}});
        });
        QThread* completedOn = nullptr;
        QObject::connect(&client, &QtLLM::Client::toolCompleted,
                         [&](const QString&, const QJsonObject&) { completedOn = QThread::currentThread(); });

        TEST_COMPARE(runTurn(client, "go").response, QString("crunched"));
        TEST_ASSERT(ranElsewhere.load());
        TEST_ASSERT(completedOn == guiThread);          // the result came back to the GUI thread
        TEST_ASSERT(api.messageBodies.value(1).contains("\\\"answer\\\":42"));
    }

    // A result that arrives after the turn was cancelled belongs to nothing.
    // The wait tool holds the turn for the given time without blocking the
    // event loop, and tolerates a number sent as text.
    TEST_FUNCTION(testWaitToolPausesTheTurn)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::toolUseReply("wait", QJsonObject{{"seconds", "0.3"}}));
        api.replies.enqueue(FakeApi::textReply("done"));

        QtLLM::Client client("key", api.url());
        QtLLM::BuiltinTools::registerTools(&client, {QtLLM::BuiltinTool::Wait});
        int ticks = 0;
        QTimer ticker;
        QObject::connect(&ticker, &QTimer::timeout, [&]() { ++ticks; });
        ticker.start(20);

        QElapsedTimer clock;
        clock.start();
        const Outcome outcome = runTurn(client, "go");
        TEST_COMPARE(outcome.response, QString("done"));
        TEST_ASSERT(clock.elapsed() >= 280);
        TEST_ASSERT(ticks >= 5);   // the event loop kept running meanwhile
        TEST_ASSERT(api.messageBodies.value(1).contains("waited_seconds"));
    }

    static QtLLM::AgentConfig subagentConfig(const FakeApi& api)
    {
        QtLLM::AgentConfig config;
        config.provider = QtLLM::Provider::Claude;
        config.url      = api.url();
        config.apiKey   = "key";
        config.model    = "test-model";
        return config;
    }

    // run_subagent: the task goes to a fresh agent on its own endpoint, its
    // answer comes back as the tool result, and the agent is gone afterwards.
    TEST_FUNCTION(testSubagentToolDelegatesAndCleansUp)
    {
        TEST_START;

        FakeApi api, subApi;
        api.replies.enqueue(FakeApi::toolUseReply("run_subagent",
            QJsonObject{{"task", "Invent a title"}, {"name", "titler"}}));
        api.replies.enqueue(FakeApi::textReply("done"));
        // The subagent uses one of the chat's tools before it answers.
        subApi.replies.enqueue(FakeApi::toolUseReply("probe", QJsonObject{{"n", 7}}));
        subApi.replies.enqueue(FakeApi::textReply("Sunny Chat"));

        QtLLM::Client client("key", api.url());
        int probed = 0;
        QtLLM::Tool probe;
        probe.setName("probe").setDescription("a tool of the chat").addParameter("n", "integer", "n", true);
        client.registerTool(probe, [&](const QJsonObject& input) {
            probed += input["n"].toInt();
            return QtLLM::toolOk();
        });
        QtLLM::BuiltinTools::registerTools(&client, {QtLLM::BuiltinTool::MessageBox});
        QStringList spawned;
        QtLLM::registerSubagentTool(&client,
            [&]() { return subagentConfig(subApi); },
            [&](QtLLM::Agent* agent) { spawned.append(agent->config().name); });

        const Outcome outcome = runTurn(client, "main chat prompt");
        TEST_ASSERT(outcome.errors.isEmpty());
        TEST_COMPARE(outcome.response, QString("done"));
        TEST_COMPARE(spawned, QStringList{"titler"});
        TEST_ASSERT(subApi.messageBodies.value(0).contains("Invent a title"));
        TEST_ASSERT(!subApi.messageBodies.value(0).contains("main chat prompt"));   // nothing of the chat
        TEST_ASSERT(api.messageBodies.value(1).contains("Sunny Chat"));

        // The agent was offered the chat's tools - but neither run_subagent
        // (agents must not multiply) nor a tool that needs the user.
        TEST_COMPARE(probed, 7);
        TEST_ASSERT(subApi.messageBodies.value(0).contains("\"probe\""));
        TEST_ASSERT(!subApi.messageBodies.value(0).contains("run_subagent"));
        TEST_ASSERT(!subApi.messageBodies.value(0).contains(
            QtLLM::BuiltinTools::toolName(QtLLM::BuiltinTool::MessageBox).toUtf8()));
        TEST_ASSERT(api.messageBodies.value(0).contains("run_subagent"));

        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        TEST_ASSERT(client.findChildren<QtLLM::Agent*>().isEmpty());
    }

    // Two calls in one response with room for one agent: the first runs, the
    // second is refused with an error the model can read.
    TEST_FUNCTION(testSubagentToolLimitsParallelAgents)
    {
        TEST_START;

        FakeApi api, subApi;
        api.replies.enqueue(FakeApi::reply("tool_use", QJsonArray{
            QJsonObject{{"type", "tool_use"}, {"id", "a"}, {"name", "run_subagent"},
                        {"input", QJsonObject{{"task", "first task"}}}},
            QJsonObject{{"type", "tool_use"}, {"id", "b"}, {"name", "run_subagent"},
                        {"input", QJsonObject{{"task", "second task"}}}}}));
        api.replies.enqueue(FakeApi::textReply("done"));
        subApi.replies.enqueue(FakeApi::textReply("first answer"));

        QtLLM::Client client("key", api.url());
        int spawned = 0;
        QtLLM::registerSubagentTool(&client,
            [&]() { return subagentConfig(subApi); },
            [&](QtLLM::Agent*) { ++spawned; },
            1);

        const Outcome outcome = runTurn(client, "go");
        TEST_COMPARE(outcome.response, QString("done"));
        TEST_COMPARE(spawned, 1);
        TEST_COMPARE(subApi.messageRequests, 1);
        TEST_ASSERT(api.messageBodies.value(1).contains("first answer"));
        TEST_ASSERT(api.messageBodies.value(1).contains("already running"));
    }

    TEST_FUNCTION(testCancelWhileAsyncToolIsPending)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::toolUseReply("slow", QJsonObject()));
        api.replies.enqueue(FakeApi::textReply("fresh"));

        QtLLM::Client client("key", api.url());
        QtLLM::Tool tool;
        tool.setName("slow").setDescription("slow");
        QtLLM::ToolCompletion kept;
        client.registerAsyncTool(tool, [&](const QJsonObject&, QtLLM::ToolCompletion done) { kept = done; });
        int responses = 0;
        QObject::connect(&client, &QtLLM::Client::responseReady, [&](const QString&) { ++responses; });

        client.sendPrompt("go");
        TEST_ASSERT(spinUntil([&]() { return !kept.isFinished() && api.messageRequests == 1
                                             && client.exportConversation()["messages"].toArray().size() == 2; }));
        client.cancelRequest();
        TEST_ASSERT(client.exportConversation()["messages"].toArray().isEmpty());

        kept.finish(QtLLM::toolOk({{"late", true}}));
        spinUntil([]() { return false; }, 150);
        TEST_COMPARE(responses, 0);
        TEST_COMPARE(api.messageRequests, 1);           // nothing was sent for the late result
        TEST_ASSERT(client.exportConversation()["messages"].toArray().isEmpty());

        TEST_COMPARE(runTurn(client, "again").response, QString("fresh"));
    }

    TEST_FUNCTION(testClaudeCodeProviderConstruction)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::ClaudeCode, QString());
        TEST_ASSERT(client.provider() == QtLLM::Provider::ClaudeCode);
        TEST_COMPARE(client.model(), QString("sonnet"));

        // No model list endpoint: aliases are offered, a full id is kept.
        client.setModel("claude-opus-5-5");
        QStringList models;
        QEventLoop loop;
        QObject::connect(&client, &QtLLM::Client::modelsAvailable, [&](const QStringList& list) {
            models = list;
            loop.quit();
        });
        QTimer::singleShot(2000, &loop, &QEventLoop::quit);
        client.fetchAvailableModels();
        loop.exec();
        TEST_ASSERT(models.contains("sonnet"));
        TEST_COMPARE(client.model(), QString("claude-opus-5-5"));

        // Switching away and back works on one Client.
        client.setProvider(QtLLM::Provider::Ollama, "http://localhost:11434");
        TEST_ASSERT(client.provider() == QtLLM::Provider::Ollama);
        client.contextBreakdown();           // no context provider must not crash
    }
};

TEST_INSTANTIATE(TST_TurnHandling);
