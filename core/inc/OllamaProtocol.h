#pragma once
#include "QtLLM_base.h"
#include "ProtocolBase.h"
#include "HttpTransport.h"
#include <QUrl>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QMap>

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

private slots:
    void onReplyReceived(const QByteArray& data);
    void onTransportError(const QString& message);

private:
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
};

} // namespace QtLLM
