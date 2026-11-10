#pragma once
#include "QtLLM_base.h"
#include "ProtocolBase.h"
#include "HttpTransport.h"
#include <QUrl>
#include <QQueue>
#include <QSharedPointer>
#include <QVector>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QMap>
#include <QElapsedTimer>

namespace QtLLM {

// OpenAI-compatible Chat Completions (OpenAI, LM Studio, llama.cpp server,
// vLLM, Ollama's /v1). Tool schemas in OpenAI format:
// {"type":"function","function":{name, description, parameters}}.
class QT_LLM_API OpenAiProtocol : public ProtocolBase
{
    Q_OBJECT
public:
    // url: the API base ("http://localhost:1234/v1") or the full
    // ".../chat/completions"; a bare host gets "/v1". An empty apiKey sends no
    // Authorization header (local servers).
    OpenAiProtocol(const QString& url, const QString& apiKey, QObject* parent = nullptr);
    ~OpenAiProtocol() override;

    void setModel(const QString& model) override;
    void setMaxTokens(int maxTokens) override;
    void setSystemPrompt(const QString& systemPrompt) override;
    void setApiKey(const QString& apiKey);
    // When set, the bearer token comes from the provider instead of apiKey.
    void setAccessTokenProvider(AccessTokenProvider provider);
    void setUrl(const QString& url);
    QString baseUrl() const { return m_baseUrl; }
    void setTools(const QList<QJsonObject>& toolSchemas,
                  const QMap<QString, ToolCallHandler>& handlers) override;

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
    void onStreamChunk(const QByteArray& data);
    void onStreamFinished();
    void onModelsReplyReceived(const QByteArray& data);
    void onModelsTransportError(const QString& message);

private:
    void        startNewTurn(const QJsonObject& userMessage);
    void        drainQueue();
    QJsonObject buildRequestBody() const;
    QList<QPair<QByteArray, QByteArray>> requestHeaders() const;
    void        sendRequest();
    void        handleError(int status, const QJsonObject& root, const QByteArray& data);
    void        processResponse(const QJsonObject& responseJson);
    void        finishTurn(const QString& text);
    void        failTurn(const QString& message);
    void        rollbackFailedTurn();

    // One round of tool calls; handlers may answer later and in any order.
    struct ToolRound
    {
        QVector<QJsonObject> results;     // "tool" messages, by call index
        QVector<QJsonArray>  images;      // pictures from the results, by call index
        QVector<bool>        done;
        int                  pending = 0;
        int                  generation = 0;
        QString              text;
    };
    void        executeToolCalls(const QJsonArray& toolCalls, const QString& text);
    void        finishToolRound(const QSharedPointer<ToolRound>& round);

    // Streaming (server-sent events), folded back into the non-streaming
    // response shape so processResponse() is the only code path.
    struct StreamCall
    {
        QString id;
        QString name;
        QString arguments;
    };
    void        resetStreamState();
    void        handleStreamLine(const QByteArray& line);
    QByteArray             m_streamBuffer;
    bool                   m_streamSniffed = false;
    bool                   m_streamIsPlainJson = false;
    QString                m_streamContent;
    QMap<int, StreamCall>  m_streamCalls;
    QJsonObject            m_streamUsage;
    QString                m_streamFinishReason;
    QString                m_streamError;
    bool                   m_streamDone = false;

    QString                    m_baseUrl;
    QString                    m_apiKey;
    AccessTokenProvider        m_accessTokenProvider;
    QString                    m_model;
    int                        m_maxTokens;
    QString                    m_systemPrompt;
    QList<QJsonObject>         m_toolSchemas;
    QMap<QString, ToolCallHandler> m_toolHandlers;
    QJsonArray                 m_history;
    HttpTransport*             m_transport;
    HttpTransport*             m_modelsTransport;
    bool                       m_modelsFetchInProgress = false;

    // Optional fields this endpoint rejected; left out until setUrl().
    // "max_tokens" here means max_completion_tokens is sent instead.
    QStringList                m_rejectedFields;

    bool                m_turnInProgress = false;
    QQueue<QJsonObject> m_pendingTurns;
    int                 m_historyLenBeforeTurn = 0;
    int                 m_turnGeneration = 0;   // bumped whenever a turn ends

    static constexpr int kMaxToolIterations = 25;
    int                 m_turnToolIterations = 0;
    static constexpr int kMaxRetries = 2;
    int                 m_retryCount = 0;

    QElapsedTimer m_turnTimer;
    bool   m_turnTimerStarted    = false;
    int    m_turnInputTokens     = 0;
    int    m_turnOutputTokens    = 0;
    int    m_turnToolCalls       = 0;
    int    m_lastContextTokens   = 0;
    int    m_sessionInputTokens  = 0;
    int    m_sessionOutputTokens = 0;
    int    m_sessionToolCalls    = 0;
    int    m_sessionTurnCount    = 0;
    double m_sessionCostUsd      = 0.0;
};

} // namespace QtLLM
