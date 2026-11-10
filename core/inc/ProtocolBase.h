#pragma once
#include "QtLLM_base.h"
#include "UsageStats.h"
#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QMap>
#include <functional>

namespace QtLLM {

// Callable invoked synchronously on the Qt thread; return value is sent back to the LLM as the tool result.
using ToolHandler = std::function<QJsonObject(const QJsonObject&)>;

// How much the model should think before answering - the main cost lever on
// current models. Default leaves it to the model.
enum class Effort { Default, Low, Medium, High, XHigh, Max };

// Wire name ("low" ... "max"); empty for Default.
QT_LLM_API QString effortName(Effort effort);

// How the protocol layer runs a tool: the result may arrive later. `done` is
// called on the GUI thread; only its first call counts.
using ToolResultCallback = std::function<void(const QJsonObject& result)>;
using ToolCallHandler = std::function<void(const QJsonObject& input, ToolResultCallback done)>;

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
    // Providers map this to what they have; one without such a control ignores it.
    virtual void setEffort(Effort effort) { m_effort = effort; }
    Effort effort() const { return m_effort; }
    // Deliver the answer while it is generated (responseDelta) instead of
    // only when complete. On by default; responseReady() is unaffected.
    virtual void setStreamingEnabled(bool enabled) { m_streamingEnabled = enabled; }
    bool streamingEnabled() const { return m_streamingEnabled; }
    // Schemas must be in the format expected by the active provider (Claude or OpenAI); Client::syncToolsToProtocol picks the correct format.
    virtual void setTools(const QList<QJsonObject>& toolSchemas,
                          const QMap<QString, ToolCallHandler>& handlers) = 0;

    // Append userMessage to internal history and send the request to the API.
    virtual void beginTurn(const QString& userMessage) = 0;
    // Same, with pictures attached to the user message. `images` has the
    // format of toolTakeImages(): [{"media_type":"image/png","data":"<base64>",
    // "width":W,"height":H}, ...]. The default drops the pictures; a provider
    // that can show them to the model overrides this and supportsImageInput().
    virtual void beginTurn(const QString& userMessage, const QJsonArray& images)
    {
        Q_UNUSED(images);
        beginTurn(userMessage);
    }
    virtual bool supportsImageInput() const { return false; }

    // false: the model is told to call at most one tool per response, for
    // applications whose tools must not run side by side. Providers without
    // such a control ignore it. Default true.
    virtual void setParallelToolCalls(bool enabled) { m_parallelToolCalls = enabled; }
    bool parallelToolCalls() const { return m_parallelToolCalls; }

    // Abort the in-flight turn (and drop queued ones): the pending request is
    // cancelled, everything the turn appended to the history is rolled back and
    // requestFinished() is emitted. No responseReady()/errorOccurred() follows.
    // Returns false (and does nothing) when no turn is in flight.
    virtual bool cancelTurn() = 0;

    virtual void clearHistory() = 0;

    // Replace the whole conversation - used by compaction, the one place where
    // history is rewritten. Messages are in the provider's own format (what
    // conversationMessages() returns). Only between turns.
    virtual void replaceHistory(const QJsonArray& messages) = 0;

    // Upper bound for the context the provider should work with, in tokens;
    // 0 = the model's own window. Only providers that size their context
    // themselves (Ollama) act on it.
    virtual void setContextLimit(int tokens) { Q_UNUSED(tokens); }

    // Reset accumulated session statistics (called by Client::clearConversation).
    virtual void clearStats() = 0;

    // Return the raw internal message history (including tool_use / tool_result blocks).
    virtual QJsonArray conversationMessages() const = 0;

    // Fetch the list of available models from the provider asynchronously.
    virtual void fetchModels() = 0;

signals:
    // Emitted when fetchModels() completes; empty list on error.
    void modelsFetched(const QStringList& models);
    void responseReady(const QString& assembledText);
    // A piece of answer text as it arrives. Text written before a tool call is
    // streamed too; responseReady() still carries the final answer in full.
    void responseDelta(const QString& text);
    void toolInvoked(const QString& toolName, const QJsonObject& input);
    void toolCompleted(const QString& toolName, const QJsonObject& result);
    void errorOccurred(const QString& message);
    void requestStarted();
    void requestFinished();
    // Emitted once per completed turn (after requestFinished) with full token and cost data.
    void statsReady(const QtLLM::UsageStats& stats);

protected:
    Effort m_effort = Effort::Default;
    bool   m_streamingEnabled = true;
    bool   m_parallelToolCalls = true;
};

} // namespace QtLLM
