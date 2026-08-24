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
    void setUrl(const QUrl& url);
    void setTools(const QList<QJsonObject>& toolSchemas,
                  const QMap<QString, ToolHandler>& handlers) override;

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
    void onCapabilityReplyReceived(const QByteArray& data);
    void onCapabilityTransportError(const QString& message);

private:
    void        startNewTurn(const QString& userMessage);
    void        drainQueue();
    QJsonObject buildRequestBody() const;
    void        processResponse(const QJsonObject& responseJson);
    void        executeToolCalls(const QJsonArray& toolCalls);
    void        sendRequest();
    // Some local models aren't trained on Ollama's native tool_calls response
    // format and instead emit the call as plain-text JSON in the message
    // content (e.g. {"name":"setWindowTitle","parameters":{"title":"..."}}).
    // Recognizes that shape - only when "name" matches an actually registered
    // tool, to avoid misinterpreting ordinary JSON-shaped prose - and returns
    // it reshaped as a standard tool_calls array; empty if it doesn't match.
    QJsonArray  extractFallbackToolCall(const QString& text) const;
    // Discards any history entries appended since the current turn started.
    // Called on every hard failure so a dangling, never-answered user/tool
    // entry doesn't linger in - and corrupt the shape of - the next request.
    void        rollbackFailedTurn();
    // fetchModels() only reports models Ollama itself says support tool
    // calling. /api/tags doesn't carry that info, so after listing model
    // names this queries /api/show for each one in turn (its "capabilities"
    // array includes "tools" when supported) and filters as it goes.
    void        checkNextModelCapability();

    QUrl                       m_url;
    QString                    m_model;
    int                        m_maxTokens;
    QString                    m_systemPrompt;
    QList<QJsonObject>         m_toolSchemas;
    QMap<QString, ToolHandler> m_toolHandlers;
    QJsonArray                 m_history;
    HttpTransport*             m_transport;
    HttpTransport*             m_modelsTransport;      // separate transport for model listing
    HttpTransport*             m_capabilityTransport;  // sequential per-model /api/show capability check

    QStringList                m_modelsPendingCapabilityCheck;
    QStringList                m_toolCapableModels;
    QString                    m_capabilityCheckModel;  // model the in-flight /api/show call is for
    // Guards against a second fetchModels() call (e.g. Client's automatic
    // startup validation racing an app-triggered fetch) interleaving with -
    // and corrupting - an in-progress multi-step capability check.
    bool                       m_modelsFetchInProgress = false;

    // Turn serialization queue
    bool            m_turnInProgress{false};
    QQueue<QString> m_pendingTurns;
    int             m_historyLenBeforeTurn = 0;

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
