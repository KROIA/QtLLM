#pragma once

#include "UnitTest.h"
#include "QtLLM.h"
#include "ClaudeCodeProtocol.h"
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonObject>
#include <QTimer>
#include <iostream>

// End-to-end check of the subscription provider: a real `claude -p` run that
// has to call an application tool over the loopback MCP endpoint, then a second
// turn that proves the conversation is resumed. Skipped unless Claude Code is
// installed and logged in. Uses a few hundred tokens of the logged-in plan.
class TST_ClaudeCodeIntegration : public UnitTest::Test
{
    TEST_CLASS(TST_ClaudeCodeIntegration)
public:
    TST_ClaudeCodeIntegration()
        : Test("TST_ClaudeCodeIntegration")
    {
        ADD_TEST(TST_ClaudeCodeIntegration::testToolCallAndResume);
        ADD_TEST(TST_ClaudeCodeIntegration::testSubagentsUseTheChatsTools);
        ADD_TEST(TST_ClaudeCodeIntegration::testAgentsTalkToEachOther);
    }

private:

    static bool loggedInOrSkip()
    {
        QJsonObject status;
        QEventLoop loop;
        QObject context;
        QtLLM::ClaudeCodeProtocol::queryAuthStatus(&context, [&](const QJsonObject& s) {
            status = s;
            loop.quit();
        });
        QTimer::singleShot(15000, &loop, &QEventLoop::quit);
        loop.exec();
        if (!status["loggedIn"].toBool()) {
            std::cout << "  Skipping: Claude Code CLI not installed or not logged in\n";
            return false;
        }
        std::cout << "  Claude Code login: " << status["subscriptionType"].toString().toStdString() << "\n";
        return true;
    }

    static QString ask(QtLLM::Client& client, const QString& prompt, QString& error)
    {
        QString text;
        QEventLoop loop;
        auto c1 = QObject::connect(&client, &QtLLM::Client::responseReady, [&](const QString& t) {
            text = t;
            loop.quit();
        });
        auto c2 = QObject::connect(&client, &QtLLM::Client::errorOccurred, [&](const QString& m) {
            error = m;
            loop.quit();
        });
        QTimer::singleShot(180000, &loop, &QEventLoop::quit);
        client.sendPrompt(prompt);
        loop.exec();
        QObject::disconnect(c1);
        QObject::disconnect(c2);
        return text;
    }

    // What a user tried in the example application: "start two agents that
    // wait and then answer". Each subagent needs the chat's tools for that
    // (here: wait and a tool of the application), and both run side by side.
    TEST_FUNCTION(testSubagentsUseTheChatsTools)
    {
        TEST_START;

        if (!loggedInOrSkip())
            return;

        QtLLM::Client client(QtLLM::Provider::ClaudeCode, QString());
        client.setModel("haiku");
        client.setSystemPrompt("You are an assistant inside a desktop application. "
                               "Answer in one short line.");
        QtLLM::BuiltinTools::registerTools(&client, {QtLLM::BuiltinTool::Wait});
        QtLLM::Tool code;
        code.setName("get_secret_code").setDescription("Returns the secret code of this application.");
        int codeCalls = 0;
        client.registerTool(code, [&](const QJsonObject&) {
            ++codeCalls;
            return QtLLM::toolOk({{"code", "KIWI-42"}});
        });

        int spawned = 0;
        QStringList agentCalls;
        QtLLM::registerSubagentTool(&client,
            []() {
                QtLLM::AgentConfig config;
                config.provider     = QtLLM::Provider::ClaudeCode;
                config.model        = "haiku";
                config.systemPrompt = "You are a subagent working on one task. Reply with the result only.";
                return config;
            },
            [&](QtLLM::Agent* agent) {
                ++spawned;
                QObject::connect(agent, &QtLLM::Client::toolInvoked,
                                 [&](const QString& tool, const QJsonObject&) { agentCalls.append(tool); });
            });

        QElapsedTimer clock;
        clock.start();
        QString error;
        const QString answer = ask(client,
            "Start two subagents at the same time. Each one must first wait 5 seconds with its "
            "wait tool, then call get_secret_code and reply with the code. Tell me what they "
            "returned.", error);
        TEST_ASSERT_M(error.isEmpty(), ("error: " + error).toStdString().c_str());
        std::cout << "  Response: " << answer.toStdString() << "\n";
        std::cout << "  Subagents: " << spawned << ", their tool calls: "
                  << agentCalls.join(", ").toStdString() << ", " << clock.elapsed() / 1000 << " s\n";

        TEST_COMPARE(spawned, 2);
        TEST_COMPARE(agentCalls.count("wait"), 2);
        TEST_COMPARE(agentCalls.count("get_secret_code"), 2);
        TEST_COMPARE(codeCalls, 2);
        TEST_ASSERT(answer.contains("KIWI-42"));
        TEST_ASSERT(clock.elapsed() >= 5000);
    }

    // Two living agents: one knows something, the other has to find out who
    // that is and ask.
    TEST_FUNCTION(testAgentsTalkToEachOther)
    {
        TEST_START;

        if (!loggedInOrSkip())
            return;

        auto make = [](const QString& name, const QString& description, const QString& prompt) {
            QtLLM::AgentConfig config;
            config.name         = name;
            config.provider     = QtLLM::Provider::ClaudeCode;
            config.model        = "haiku";
            config.messaging    = true;
            config.description  = description;
            config.systemPrompt = prompt;
            return config;
        };
        QtLLM::Agent librarian(make("librarian", "Knows the build number of this application.",
            "You are the librarian. Answer questions with your tools, in one short line."));
        QtLLM::Agent clerk(make("clerk", "Handles requests from the front desk.",
            "You are the clerk. You know nothing yourself; other agents may. Answer in one short line."));

        int codeCalls = 0;
        QtLLM::Tool code;
        code.setName("get_build_number").setDescription("Returns the build number of this application.");
        librarian.registerTool(code, [&](const QJsonObject&) {
            ++codeCalls;
            return QtLLM::toolOk({{"build_number", "KIWI-42"}});
        });
        QStringList clerkCalls;
        QObject::connect(&clerk, &QtLLM::Client::toolInvoked,
                         [&](const QString& tool, const QJsonObject&) { clerkCalls.append(tool); });

        QEventLoop loop;
        QString answer;
        bool ok = false;
        QTimer::singleShot(180000, &loop, &QEventLoop::quit);
        clerk.ask("What is the build number of this application? Find out which agent knows it, "
                  "ask that agent, and reply with the build number.",
                  [&](const QString& text, bool success) { answer = text; ok = success; loop.quit(); });
        loop.exec();

        std::cout << "  Response: " << answer.toStdString() << "\n";
        std::cout << "  Clerk's tool calls: " << clerkCalls.join(", ").toStdString() << "\n";
        TEST_ASSERT_M(ok, ("failed: " + answer).toStdString().c_str());
        TEST_ASSERT(clerkCalls.contains("message_agent"));
        std::cout << "  Librarian looked the build number up " << codeCalls << " time(s)" << std::endl;
        TEST_ASSERT(codeCalls >= 1);   // a model may check twice
        TEST_ASSERT(answer.contains("KIWI-42"));
        TEST_ASSERT(librarian.record().tasks.value(0).prompt.contains("Message from agent 'clerk'"));
    }

    TEST_FUNCTION(testToolCallAndResume)
    {
        TEST_START;

        if (!loggedInOrSkip())
            return;

        QtLLM::Client client(QtLLM::Provider::ClaudeCode, QString());
        client.setModel("haiku");
        client.setSystemPrompt("You are a test assistant. Use the provided tools; answer in one short line.");

        int toolCalls = 0;
        QtLLM::Tool tool;
        tool.setName("get_secret_number")
            .setDescription("Returns the secret number. The only way to learn it.")
            .addParameter("reason", "string", "Why the number is needed.", false);
        client.registerTool(tool, [&](const QJsonObject&) {
            ++toolCalls;
            return QtLLM::toolOk({{"number", 417923}});
        });

        QString error;
        const QString first = ask(client, "What is the secret number? Call the tool.", error);
        TEST_ASSERT_M(error.isEmpty(), ("Claude Code error: " + error).toStdString().c_str());
        std::cout << "  Response 1: " << first.toStdString() << "\n";
        TEST_ASSERT_M(toolCalls >= 1, "Expected the application tool to be called through MCP");
        TEST_ASSERT(first.contains("417923") || first.contains("417,923"));
        TEST_COMPARE(client.usageStats().toolCalls, toolCalls);

        // Second turn must see the first one (conversation resumed by session id).
        const int callsBefore = toolCalls;
        const QString second = ask(client,
            "Without calling any tool: what number did you just tell me? Digits only.", error);
        TEST_ASSERT_M(error.isEmpty(), ("Claude Code error: " + error).toStdString().c_str());
        std::cout << "  Response 2: " << second.toStdString() << "\n";
        TEST_ASSERT(second.contains("417923") || second.contains("417,923"));
        TEST_COMPARE(toolCalls, callsBefore);
    }
};

TEST_INSTANTIATE(TST_ClaudeCodeIntegration);
