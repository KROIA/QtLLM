#pragma once
#include "QtLLM_base.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QMap>
#include <QUrl>
#include <QElapsedTimer>
#include <functional>
#include "ProtocolBase.h"
#include "HttpTransport.h"
#include <QQueue>
#include <QSharedPointer>
#include <QVector>

namespace QtLLM {

class QT_LLM_API ClaudeProtocol : public ProtocolBase
{
    Q_OBJECT
public:
    explicit ClaudeProtocol(const QString& apiKey,
                            const QUrl& url,
                            QObject* parent = nullptr);
    ~ClaudeProtocol() override;

    void setModel(const QString& model) override;
    void setMaxTokens(int maxTokens) override;
    void setSystemPrompt(const QString& systemPrompt) override;

    // Changing these after construction was previously impossible - Settings
    // dialogs that let the user edit the API key / endpoint had no way to
    // actually apply the change. Takes effect on the next request.
    void setApiKey(const QString& apiKey);
    void setUrl(const QUrl& url);

    // Called by Client when tool registration changes
    void setTools(const QList<QJsonObject>& toolSchemas,
                  const QMap<QString, ToolCallHandler>& handlers) override;

    // Append userMessage to internal history and send the request to the API.
    void beginTurn(const QString& userMessage) override;
    void beginTurn(const QString& userMessage, const QJsonArray& images) override;
    bool supportsImageInput() const override { return true; }

    bool cancelTurn() override;
    void clearHistory() override;
    void replaceHistory(const QJsonArray& messages) override;
    void clearStats() override;
    void fetchModels() override;
    QJsonArray conversationMessages() const override;

private slots:
    void onReplyReceived(const QByteArray& data);
    void onTransportError(const QString& message);
    void onModelsReplyReceived(const QByteArray& data);
    void onModelsTransportError(const QString& message);
    void onStreamChunk(const QByteArray& data);
    void onStreamFinished();

private:
    void        startNewTurn(const QJsonObject& userMessage);
    void        sendRequest();
    void        drainQueue();
    QJsonObject buildRequestBody() const;
    void        processResponse(const QJsonObject& responseJson);
    // Ends the turn successfully: folds the turn counters into the session
    // totals and emits requestFinished / responseReady / statsReady.
    void        finishTurn(const QString& text);
    // Ends the turn with a failure: rolls the history back and emits
    // errorOccurred / requestFinished.
    void        failTurn(const QString& message);
    // One round of tool calls. Handlers may answer later (asynchronous
    // tools); the round is complete - and the next request goes out - when
    // the last result is in. Results are sent in the order of the calls,
    // whatever order they arrive in.
    struct ToolRound
    {
        QVector<QJsonObject> results;     // tool_result blocks, by call index
        QVector<bool>        done;
        int                  pending = 0;
        int                  generation = 0;   // turn this round belongs to
        QJsonArray           content;          // the assistant message that asked
    };
    void        executeToolCalls(const QJsonArray& toolUseBlocks, const QJsonArray& content);
    void        finishToolRound(const QSharedPointer<ToolRound>& round);
    QString     assembleText(const QJsonArray& content) const;
    // Some models routed through a non-Anthropic gateway/proxy don't reliably
    // emit a real tool_use content block and instead end the turn with the
    // call serialized as plain JSON text (e.g.
    // {"name":"setWindowTitle","parameters":{"title":"..."}}). Recognizes
    // that shape - only when "name" matches an actually registered tool - and
    // returns it reshaped as a standard tool_use content block array; empty
    // if it doesn't match.
    QJsonArray  extractFallbackToolUse(const QString& text) const;
    // Discards any history entries appended since the current turn started
    // (the initial user message, plus any tool_use/tool_result round trips).
    // Called on every hard failure so a dangling, never-answered user/tool
    // entry doesn't corrupt the next request - Anthropic's Messages API
    // requires strictly alternating user/assistant roles, and a leftover
    // unanswered entry there previously broke (or silently hung) the very
    // next send.
    void        rollbackFailedTurn();
    // Current models bind every thinking block to the exact system prompt,
    // tool list and messages that preceded it. Enabling/disabling a tool or
    // editing the system prompt mid-conversation breaks that binding and the
    // API rejects the next request. The documented recovery is to resend the
    // history without its thinking blocks (text and tool calls stay).
    void        stripThinkingBlocks();
    bool        m_thinkingStripped = false;   // recovery already used this turn
    // Models that predate the effort parameter reject it; remembered per model
    // so only the first request pays for finding out.
    QStringList m_effortUnsupportedModels;

    // --- streaming (server-sent events) ------------------------------------
    // The stream is folded back into the response object the non-streaming
    // API returns, so everything after it (tool rounds, stop reasons, history,
    // statistics) is one code path.
    void handleStreamEvent(const QString& event, const QJsonObject& data);
    void resetStreamState();

    QByteArray             m_streamBuffer;        // bytes of a not yet complete event
    bool                   m_streamSniffed = false;
    bool                   m_streamIsPlainJson = false;  // endpoint ignored "stream": whole reply is one JSON
    QMap<int, QJsonObject> m_streamBlocks;        // content blocks by index
    QMap<int, QByteArray>  m_streamPartialJson;   // tool input, arrives as JSON text fragments
    QJsonObject            m_streamUsage;
    QString                m_streamStopReason;
    QJsonObject            m_streamStopDetails;
    QJsonObject            m_streamError;         // "error" event
    bool                   m_streamTextEmitted = false;
    bool                   m_streamUnsupported = false;  // endpoint rejects "stream"
    // Tool calls whose input did not arrive as valid JSON (cut-off stream):
    // answered with an error instead of running the handler on garbage.
    QStringList            m_invalidToolInputs;

    QString                    m_apiKey;
    QUrl                       m_url;
    QString                    m_model;
    int                        m_maxTokens;
    QString                    m_systemPrompt;
    QList<QJsonObject>         m_toolSchemas;
    QMap<QString, ToolCallHandler> m_toolHandlers;
    QJsonArray                 m_history;
    HttpTransport*             m_transport;
    HttpTransport*             m_modelsTransport;  // separate transport for model listing

    // Turn serialization queue
    bool           m_turnInProgress{false};
    QQueue<QJsonObject> m_pendingTurns;   // complete user messages
    int            m_historyLenBeforeTurn = 0;

    // Tool-use loop guard
    static constexpr int kMaxToolIterations = 25;
    int    m_turnToolIterations = 0;

    // Transient API failures (overloaded / rate limit / 5xx) are retried with
    // backoff instead of failing the turn the user is waiting on.
    static constexpr int kMaxRetries = 2;
    int    m_retryCount = 0;
    // Bumped whenever a turn ends, so a retry timer scheduled for a turn that
    // was cancelled in the meantime does nothing when it fires.
    int    m_turnGeneration = 0;
    bool   m_modelsFetchInProgress = false;

    // Stats tracking
    QElapsedTimer m_turnTimer;
    int    m_turnInputTokens              = 0;
    int    m_turnOutputTokens             = 0;
    int    m_turnCacheCreationInputTokens = 0;
    int    m_turnCacheReadInputTokens     = 0;
    int    m_turnToolCalls                = 0;
    int    m_lastContextTokens            = 0;   // input + cache + output of the latest response
    bool   m_turnTimerStarted             = false;
    int    m_sessionInputTokens              = 0;
    int    m_sessionOutputTokens             = 0;
    int    m_sessionCacheCreationInputTokens = 0;
    int    m_sessionCacheReadInputTokens     = 0;
    int    m_sessionToolCalls                = 0;
    int    m_sessionTurnCount                = 0;
    double m_sessionCostUsd                  = 0.0;
};

} // namespace QtLLM
