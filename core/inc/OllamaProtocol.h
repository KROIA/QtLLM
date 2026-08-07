#pragma once
#include "QtLLM_base.h"
#include "ProtocolBase.h"
#include "HttpTransport.h"
#include <QUrl>
#include <QQueue>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QMap>
#include <QElapsedTimer>

namespace QtLLM {

class QT_LLM_API OllamaProtocol : public ProtocolBase
{
    Q_OBJECT
public:
    explicit OllamaProtocol(const QUrl& url, QObject* parent = nullptr);
    ~OllamaProtocol() override;

    void setModel(const QString& model) override;
    void setMaxTokens(int maxTokens) override;
    void setSystemPrompt(const QString& systemPrompt) override;
    void setTools(const QList<QJsonObject>& toolSchemas,
                  const QMap<QString, ToolHandler>& handlers) override;

    void beginTurn(const QString& userMessage) override;
    void clearHistory() override;
    void clearStats() override;
    void fetchModels() override;

private slots:
    void onReplyReceived(const QByteArray& data);
    void onTransportError(const QString& message);
    void onModelsReplyReceived(const QByteArray& data);
    void onModelsTransportError(const QString& message);

private:
    void        startNewTurn(const QString& userMessage);
    void        drainQueue();
    QJsonObject buildRequestBody() const;
    void        processResponse(const QJsonObject& responseJson);
    void        executeToolCalls(const QJsonArray& toolCalls);
    void        sendRequest();

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
    bool            m_turnInProgress{false};
    QQueue<QString> m_pendingTurns;

    // Stats tracking
    QElapsedTimer m_turnTimer;
    int  m_turnInputTokens  = 0;
    int  m_turnOutputTokens = 0;
    int  m_turnToolCalls    = 0;
    bool m_turnTimerStarted = false;
    int  m_sessionInputTokens  = 0;
    int  m_sessionOutputTokens = 0;
    int  m_sessionToolCalls    = 0;
    int  m_sessionTurnCount    = 0;
};

} // namespace QtLLM
