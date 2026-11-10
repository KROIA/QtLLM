#pragma once

#include "UnitTest.h"
#include "QtLLM.h"
#include <QColor>
#include <QImage>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QString>
#include <iostream>


class TST_Integration : public UnitTest::Test
{
    TEST_CLASS(TST_Integration)
public:
    TST_Integration()
        : Test("TST_Integration")
    {
        ADD_TEST(TST_Integration::testEnvVarsPresent);
        ADD_TEST(TST_Integration::testSimpleTextResponse);
        ADD_TEST(TST_Integration::testToolUse);
        ADD_TEST(TST_Integration::testDeferredToolIsFoundAndCalled);
#if QTLLM_HAS_SCRIPTING
        ADD_TEST(TST_Integration::testModelWritesAScript);
#endif
        ADD_TEST(TST_Integration::testPromptWithPicture);
    }

private:

    // Tests
    TEST_FUNCTION(testEnvVarsPresent)
    {
        TEST_START;

        QString apiKey  = QString::fromUtf8(qgetenv("ANTHROPIC_FOUNDRY_API_KEY"));
        QString baseUrl = QString::fromUtf8(qgetenv("ANTHROPIC_FOUNDRY_BASE_URL"));

        TEST_ASSERT_M(!apiKey.isEmpty(),  "ANTHROPIC_FOUNDRY_API_KEY env var must be set");
        TEST_ASSERT_M(!baseUrl.isEmpty(), "ANTHROPIC_FOUNDRY_BASE_URL env var must be set");
    }


    TEST_FUNCTION(testSimpleTextResponse)
    {
        TEST_START;

        QString apiKey  = QString::fromUtf8(qgetenv("ANTHROPIC_FOUNDRY_API_KEY"));
        QString baseUrl = QString::fromUtf8(qgetenv("ANTHROPIC_FOUNDRY_BASE_URL"));

        if (apiKey.isEmpty() || baseUrl.isEmpty())
        {
            std::cout << "  Skipping: env var not set\n";
            return;
        }

        // ANTHROPIC_FOUNDRY_BASE_URL is a base URL — append the messages endpoint path
        QString endpointUrl = baseUrl + "/v1/messages";

        QtLLM::Client client(apiKey, endpointUrl);
        client.setModel("claude-sonnet-5-5");
        client.setMaxTokens(512);
        client.setSystemPrompt("Reply with exactly one short sentence.");

        QEventLoop loop;
        QString receivedText;
        bool gotError = false;
        QString errorMsg;

        QObject::connect(&client, &QtLLM::Client::responseReady, [&](const QString& text) {
            receivedText = text;
            loop.quit();
        });
        QObject::connect(&client, &QtLLM::Client::errorOccurred, [&](const QString& msg) {
            gotError = true;
            errorMsg = msg;
            loop.quit();
        });

        // Timeout after 30 seconds to avoid hanging indefinitely
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);

        client.sendPrompt("Say hello.");
        loop.exec();

        TEST_ASSERT_M(!gotError, ("API error: " + errorMsg).toStdString().c_str());
        TEST_ASSERT_M(!receivedText.isEmpty(), "Expected non-empty response text");
        std::cout << "  Response: " << receivedText.toStdString() << "\n";
    }


    // A deferred tool is not in the request; a real model has to look for it
    // with find_tools and run it through call_tool.
    TEST_FUNCTION(testDeferredToolIsFoundAndCalled)
    {
        TEST_START;

        QString apiKey  = QString::fromUtf8(qgetenv("ANTHROPIC_FOUNDRY_API_KEY"));
        QString baseUrl = QString::fromUtf8(qgetenv("ANTHROPIC_FOUNDRY_BASE_URL"));
        if (apiKey.isEmpty() || baseUrl.isEmpty()) {
            std::cout << "  Skipping: env var not set\n";
            return;
        }

        QtLLM::Client client(apiKey, baseUrl + "/v1/messages");
        client.setModel("claude-sonnet-5-5");
        client.setMaxTokens(512);
        client.setSystemPrompt("You are an assistant inside a desktop application. Answer in one short sentence.");

        QtLLM::Tool report;
        report.setName("warehouse_stock_report")
              .setGroup("Warehouse")
              .setDescription("Returns how many units of an article are in stock.")
              .addParameter("article", "string", "Article name.", true);
        QString asked;
        client.registerTool(report, [&](const QJsonObject& input) {
            asked = input["article"].toString();
            return QtLLM::toolOk({{"article", asked}, {"units", 1234}});
        });
        client.setToolGroupDeferred("Warehouse", true);

        QStringList invoked;
        QObject::connect(&client, &QtLLM::Client::toolInvoked,
                         [&](const QString& name, const QJsonObject&) { invoked.append(name); });
        QEventLoop loop;
        QString text, error;
        QObject::connect(&client, &QtLLM::Client::responseReady, [&](const QString& t) { text = t; loop.quit(); });
        QObject::connect(&client, &QtLLM::Client::errorOccurred, [&](const QString& m) { error = m; loop.quit(); });
        QTimer::singleShot(60000, &loop, &QEventLoop::quit);
        client.sendPrompt("How many hinges do we have in stock?");
        loop.exec();

        TEST_ASSERT_M(error.isEmpty(), ("error: " + error).toStdString().c_str());
        std::cout << "  Tools used: " << invoked.join(", ").toStdString() << "\n";
        std::cout << "  Response: " << text.toStdString() << "\n";
        TEST_ASSERT(invoked.contains("find_tools"));
        TEST_ASSERT(invoked.contains("call_tool"));
        TEST_ASSERT(asked.contains("hinge", Qt::CaseInsensitive));
        TEST_ASSERT(text.contains("1234") || text.contains("1,234"));
    }

#if QTLLM_HAS_SCRIPTING
    // "Do this for every article": the model looks up the methods, writes one
    // script and gets one result back - instead of a tool call per article.
    TEST_FUNCTION(testModelWritesAScript)
    {
        TEST_START;

        QString apiKey  = QString::fromUtf8(qgetenv("ANTHROPIC_FOUNDRY_API_KEY"));
        QString baseUrl = QString::fromUtf8(qgetenv("ANTHROPIC_FOUNDRY_BASE_URL"));
        if (apiKey.isEmpty() || baseUrl.isEmpty()) {
            std::cout << "  Skipping: env var not set\n";
            return;
        }

        QtLLM::Client client(apiKey, baseUrl + "/v1/messages");
        client.setModel("claude-sonnet-5-5");
        client.setMaxTokens(1024);
        client.setSystemPrompt("You are an assistant inside a warehouse application. Answer in one short sentence.");

        // 40 articles; every third one is low on stock.
        QMap<QString, int> stock;
        int expectedLow = 0;
        for (int i = 1; i <= 40; ++i) {
            const int units = i % 3 == 0 ? 10 + i : 200 + i;
            expectedLow += units < 50 ? 1 : 0;
            stock.insert(QString("article-%1").arg(i, 2, 10, QChar('0')), units);
        }
        const QMap<QString, int> before = stock;

        QtLLM::Scripting scripting(&client);
        QtLLM::ScriptMethod list("stock.list");
        list.setDescription("The names of all articles.").setReadOnly(true)
            .setReturns("array", "Article names", QJsonArray{"article-01"});
        scripting.registerMethod(list, [&](const QJsonObject&) -> QJsonValue {
            return QJsonArray::fromStringList(stock.keys());
        });
        QtLLM::ScriptMethod get("stock.get");
        get.setDescription("One article with its units in stock.").setReadOnly(true)
           .addParameter("name", "string", "Article name")
           .setReturns("object", "The article", QJsonObject{{"name", "article-01"}, {"units", 120}});
        scripting.registerMethod(get, [&](const QJsonObject& args) -> QJsonValue {
            const QString name = args["name"].toString();
            if (!stock.contains(name))
                throw QtLLM::ScriptError("no article '" + name + "'");
            return QJsonObject{{"name", name}, {"units", stock.value(name)}};
        });
        QtLLM::ScriptMethod setUnits("stock.setUnits");
        setUnits.setDescription("Sets the units in stock of an article.")
                .addParameter("name", "string", "Article name")
                .addParameter("units", "integer", "The new number of units");
        scripting.registerMethod(setUnits, [&](const QJsonObject& args) -> QJsonValue {
            const QString name = args["name"].toString();
            if (!stock.contains(name))
                throw QtLLM::ScriptError("no article '" + name + "'");
            stock[name] = args["units"].toInt();
            return QJsonValue();
        });

        QStringList invoked;
        QList<QJsonObject> scriptResults;
        QObject::connect(&client, &QtLLM::Client::toolInvoked,
                         [&](const QString& name, const QJsonObject&) { invoked.append(name); });
        QObject::connect(&client, &QtLLM::Client::toolCompleted,
                         [&](const QString& name, const QJsonObject& result) {
            if (name == "run_script")
                scriptResults.append(result);
        });
        QEventLoop loop;
        QString text, error;
        QObject::connect(&client, &QtLLM::Client::responseReady, [&](const QString& t) { text = t; loop.quit(); });
        QObject::connect(&client, &QtLLM::Client::errorOccurred, [&](const QString& m) { error = m; loop.quit(); });
        QTimer::singleShot(120000, &loop, &QEventLoop::quit);
        client.sendPrompt("Add 100 units to every article that has fewer than 50 units in stock, "
                          "and tell me how many articles you changed.");
        loop.exec();

        TEST_ASSERT_M(error.isEmpty(), ("error: " + error).toStdString().c_str());
        std::cout << "  Tools used: " << invoked.join(", ").toStdString() << "\n";
        for (const QJsonObject& result : scriptResults) {
            std::cout << "  Script result: "
                      << QJsonDocument(result).toJson(QJsonDocument::Compact).left(300).toStdString() << "\n";
        }
        std::cout << "  Response: " << text.toStdString() << "\n";

        TEST_ASSERT(invoked.contains("run_script"));
        // A handful of tool calls for forty articles, not eighty.
        TEST_ASSERT(invoked.size() <= 8);
        int wrong = 0;
        for (auto it = before.constBegin(); it != before.constEnd(); ++it) {
            const int expected = it.value() < 50 ? it.value() + 100 : it.value();
            wrong += stock.value(it.key()) == expected ? 0 : 1;
        }
        TEST_COMPARE(wrong, 0);
        TEST_ASSERT(text.contains(QString::number(expectedLow)));
    }
#endif

    // A picture attached to a prompt reaches the model as a picture.
    TEST_FUNCTION(testPromptWithPicture)
    {
        TEST_START;

        QString apiKey  = QString::fromUtf8(qgetenv("ANTHROPIC_FOUNDRY_API_KEY"));
        QString baseUrl = QString::fromUtf8(qgetenv("ANTHROPIC_FOUNDRY_BASE_URL"));
        if (apiKey.isEmpty() || baseUrl.isEmpty()) {
            std::cout << "  Skipping: env var not set\n";
            return;
        }

        QtLLM::Client client(apiKey, baseUrl + "/v1/messages");
        client.setModel("claude-sonnet-5-5");
        client.setMaxTokens(256);

        QImage picture(96, 96, QImage::Format_RGB32);
        picture.fill(QColor(0, 160, 0));

        QEventLoop loop;
        QString text, error;
        QObject::connect(&client, &QtLLM::Client::responseReady, [&](const QString& t) { text = t; loop.quit(); });
        QObject::connect(&client, &QtLLM::Client::errorOccurred, [&](const QString& m) { error = m; loop.quit(); });
        QTimer::singleShot(60000, &loop, &QEventLoop::quit);
        client.sendPrompt("Which single colour fills this picture? Answer with one word.", {picture});
        loop.exec();

        TEST_ASSERT_M(error.isEmpty(), ("error: " + error).toStdString().c_str());
        std::cout << "  Response: " << text.toStdString() << "\n";
        TEST_ASSERT(text.contains("green", Qt::CaseInsensitive));
    }

    TEST_FUNCTION(testToolUse)
    {
        TEST_START;

        QString apiKey  = QString::fromUtf8(qgetenv("ANTHROPIC_FOUNDRY_API_KEY"));
        QString baseUrl = QString::fromUtf8(qgetenv("ANTHROPIC_FOUNDRY_BASE_URL"));

        if (apiKey.isEmpty() || baseUrl.isEmpty())
        {
            std::cout << "  Skipping: env var not set\n";
            return;
        }

        // ANTHROPIC_FOUNDRY_BASE_URL is a base URL — append the messages endpoint path
        QString endpointUrl = baseUrl + "/v1/messages";

        QtLLM::Client client(apiKey, endpointUrl);
        client.setModel("claude-sonnet-5-5");
        client.setMaxTokens(512);

        QJsonObject paramSchema;
        paramSchema["type"] = "object";
        paramSchema["properties"] = QJsonObject{
            {"a", QJsonObject{{"type", "number"}}},
            {"b", QJsonObject{{"type", "number"}}}
        };
        paramSchema["required"] = QJsonArray{"a", "b"};

        bool toolWasCalled = false;

        client.registerTool(
            "add",
            "Adds two numbers.",
            paramSchema,
            [](const QJsonObject& args) {
                return QJsonObject{{"result", args["a"].toDouble() + args["b"].toDouble()}};
            }
        );

        QObject::connect(&client, &QtLLM::Client::toolInvoked, [&](const QString&, const QJsonObject&) {
            toolWasCalled = true;
        });

        QEventLoop loop;
        QString receivedText;
        bool gotError = false;
        QString errorMsg;

        QObject::connect(&client, &QtLLM::Client::responseReady, [&](const QString& text) {
            receivedText = text;
            loop.quit();
        });
        QObject::connect(&client, &QtLLM::Client::errorOccurred, [&](const QString& msg) {
            gotError = true;
            errorMsg = msg;
            loop.quit();
        });

        // Timeout after 30 seconds to avoid hanging indefinitely
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);

        client.sendPrompt("What is 7 plus 8? Use the add tool.");
        loop.exec();

        TEST_ASSERT_M(!gotError, ("API error: " + errorMsg).toStdString().c_str());
        TEST_ASSERT_M(toolWasCalled, "Expected the add tool to be called");
        TEST_ASSERT_M(!receivedText.isEmpty(), "Expected non-empty final response");
        std::cout << "  Tool was invoked: " << (toolWasCalled ? "yes" : "no") << "\n";
        std::cout << "  Response: " << receivedText.toStdString() << "\n";
    }

};

TEST_INSTANTIATE(TST_Integration);
