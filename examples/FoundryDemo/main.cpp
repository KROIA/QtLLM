#include <QCoreApplication>
#include <QJsonObject>
#include <QString>
#include <iostream>
#include "QtLLM.h"

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    QString apiKey = QString::fromUtf8(qgetenv("ANTHROPIC_FOUNDRY_API_KEY"));
    QString baseUrl = QString::fromUtf8(qgetenv("ANTHROPIC_FOUNDRY_BASE_URL"));

    if (apiKey.isEmpty() || baseUrl.isEmpty()) {
        std::cerr << "Error: ANTHROPIC_FOUNDRY_API_KEY and ANTHROPIC_FOUNDRY_BASE_URL must be set.\n";
        return 1;
    }

    // ANTHROPIC_FOUNDRY_BASE_URL is a base URL — append the messages endpoint path
    QtLLM::Client client(apiKey, baseUrl + "/v1/messages");
    client.setModel("claude-sonnet-4-5");
    client.setMaxTokens(1024);
    client.setSystemPrompt("You are a helpful assistant with access to tools. Use the provided tools when appropriate.");

    // Register the "add" tool
    QtLLM::Tool addTool;
    addTool.setName("add")
           .setDescription("Adds two numbers and returns the result.")
           .addParameter("a", "number", "First number to add.", true)
           .addParameter("b", "number", "Second number to add.", true);

    client.registerTool(addTool, [](const QJsonObject& args) -> QJsonObject {
        return QJsonObject{{"result", args["a"].toDouble() + args["b"].toDouble()}};
    });

    // Register the "get_weather" tool
    QtLLM::Tool weatherTool;
    weatherTool.setName("get_weather")
               .setDescription("Returns a dummy weather report for a given city.")
               .addParameter("city", "string", "The name of the city.", true);

    client.registerTool(weatherTool, [](const QJsonObject& args) -> QJsonObject {
        return QJsonObject{
            {"city", args["city"].toString()},
            {"temperature", 22},
            {"condition", "sunny"}
        };
    });

    QObject::connect(&client, &QtLLM::Client::responseReady, [](const QString& text) {
        std::cout << "Assistant: " << text.toStdString() << "\n";
    });

    QObject::connect(&client, &QtLLM::Client::toolInvoked, [](const QString& name, const QJsonObject&) {
        std::cout << "[Tool called: " << name.toStdString() << "]\n";
    });

    QObject::connect(&client, &QtLLM::Client::toolCompleted, [](const QString& name, const QJsonObject&) {
        std::cout << "[Tool result ready: " << name.toStdString() << "]\n";
    });

    QObject::connect(&client, &QtLLM::Client::errorOccurred, [&app](const QString& message) {
        std::cerr << "Error: " << message.toStdString() << "\n";
        app.exit(1);
    });

    QObject::connect(&client, &QtLLM::Client::requestFinished, [&app]() {
        app.quit();
    });

    client.sendPrompt("What is 17 plus 25? Also tell me the weather in Berlin.");

    return app.exec();
}
