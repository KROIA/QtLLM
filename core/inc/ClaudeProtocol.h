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
                  const QMap<QString, ToolHandler>& handlers) override;

    // Append userMessage to internal history and send the request to the API.
    void beginTurn(const QString& userMessage) override;

    void clearHistory() override;
    void clearStats() override;
    void fetchModels() override;
    QJsonArray conversationMessages() const override;

private slots:
    void onReplyReceived(const QByteArray& data);
    void onTransportError(const QString& message);
    void onModelsReplyReceived(const QByteArray& data);
    void onModelsTransportError(const QString& message);

private:
    void        startNewTurn(const QString& userMessage);
    void        sendRequest();
    void        drainQueue();
    QJsonObject buildRequestBody() const;
    void        processResponse(const QJsonObject& responseJson);
    void        executeToolCalls(const QJsonArray& toolUseBlocks);
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

    QString                    m_apiKey;
    QUrl                       m_url;
    QString                    m_model;
    int                        m_maxTokens;
    QString                    m_systemPrompt;
    QList<QJsonObject>         m_toolSchemas;
    QMap<QString, ToolHandler> m_toolHandlers;
    QJsonArray                 m_history;
    HttpTransport*             m_transport;
    HttpTransport*             m_modelsTransport;  // separate transport for model listing

    // Turn serialization queue
    bool           m_turnInProgress{false};
    QQueue<QString> m_pendingTurns;
    int            m_historyLenBeforeTurn = 0;

    // Tool-use loop guard
    static constexpr int kMaxToolIterations = 25;
    int    m_turnToolIterations = 0;

    // Stats tracking
    QElapsedTimer m_turnTimer;
    int    m_turnInputTokens              = 0;
    int    m_turnOutputTokens             = 0;
    int    m_turnCacheCreationInputTokens = 0;
    int    m_turnCacheReadInputTokens     = 0;
    int    m_turnToolCalls                = 0;
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
