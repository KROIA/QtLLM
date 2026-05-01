#pragma once
#include "QtLLM_base.h"
#include "Tool.h"
#include <QObject>
#include <QString>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>
#include <QMap>
#include <functional>

namespace QtLLM {

using ToolHandler = std::function<QJsonObject(const QJsonObject&)>;

// Forward declare internal class — never expose ClaudeProtocol in the public header
class ClaudeProtocol;

class QT_LLM_API Client : public QObject
{
    Q_OBJECT
public:
    explicit Client(const QString& apiKey,
                    const QString& url = QStringLiteral("https://api.anthropic.com/v1/messages"),
                    QObject* parent = nullptr);
    ~Client() override;

    void setModel(const QString& model);
    void setMaxTokens(int maxTokens);
    void setSystemPrompt(const QString& systemPrompt);

    // Register a tool the LLM can call. The handler is called synchronously when the LLM invokes the tool.
    void registerTool(const Tool& tool, ToolHandler handler);

    // Overload: register using the low-level raw JSON schema directly
    void registerTool(const QString& name,
                      const QString& description,
                      const QJsonObject& parameterSchema,
                      ToolHandler handler);

    void unregisterTool(const QString& toolName);

    // Appends userMessage to history and sends the full conversation to the API.
    void sendPrompt(const QString& userMessage);

    // Clears conversation history. Preserves registered tools and settings.
    void clearConversation();

    QJsonArray conversationHistory() const;

signals:
    void responseReady(const QString& text);
    void toolInvoked(const QString& toolName, const QJsonObject& input);
    void toolCompleted(const QString& toolName, const QJsonObject& result);
    void errorOccurred(const QString& errorMessage);
    void requestStarted();
    void requestFinished();

private:
    void syncToolsToProtocol();

    struct RegisteredTool {
        QJsonObject schema;   // the full toApiObject() JSON
        ToolHandler handler;
    };

    QMap<QString, RegisteredTool> m_tools;
    QJsonArray                    m_history;
    ClaudeProtocol*               m_protocol;
};

} // namespace QtLLM
