#pragma once
#include "QtLLM_base.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QMap>
#include <QUrl>
#include <functional>
#include "ProtocolBase.h"
#include "HttpTransport.h"

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

private slots:
    void onReplyReceived(const QByteArray& data);
    void onTransportError(const QString& message);

private:
    void        sendRequest();
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
};

} // namespace QtLLM
