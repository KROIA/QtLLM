#pragma once
#include "QtLLM_base.h"
#include "UsageStats.h"
#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QMap>
#include <functional>

namespace QtLLM {

// Callable invoked synchronously on the Qt thread; return value is sent back to the LLM as the tool result.
using ToolHandler = std::function<QJsonObject(const QJsonObject&)>;

// Abstract base for LLM provider protocol implementations.
class QT_LLM_API ProtocolBase : public QObject
{
    Q_OBJECT
public:
    explicit ProtocolBase(QObject* parent = nullptr);
    ~ProtocolBase() override;

    virtual void setModel(const QString& model) = 0;
    virtual void setMaxTokens(int maxTokens) = 0;
    virtual void setSystemPrompt(const QString& systemPrompt) = 0;
    // Schemas must be in the format expected by the active provider (Claude or OpenAI); Client::syncToolsToProtocol picks the correct format.
    virtual void setTools(const QList<QJsonObject>& toolSchemas,
                          const QMap<QString, ToolHandler>& handlers) = 0;

    // Append userMessage to internal history and send the request to the API.
    virtual void beginTurn(const QString& userMessage) = 0;

    virtual void clearHistory() = 0;

    // Reset accumulated session statistics (called by Client::clearConversation).
    virtual void clearStats() = 0;

signals:
    void responseReady(const QString& assembledText);
    void toolInvoked(const QString& toolName, const QJsonObject& input);
    void toolCompleted(const QString& toolName, const QJsonObject& result);
    void errorOccurred(const QString& message);
    void requestStarted();
    void requestFinished();
    // Emitted once per completed turn (after requestFinished) with full token and cost data.
    void statsReady(const QtLLM::UsageStats& stats);
};

} // namespace QtLLM
