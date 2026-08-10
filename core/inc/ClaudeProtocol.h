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
