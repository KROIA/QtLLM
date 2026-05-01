#pragma once
#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QMap>
#include <QUrl>
#include <functional>
#include "HttpTransport.h"
// forward declare Tool to avoid circular includes — include Tool.h in .cpp
namespace QtLLM { class Tool; }

namespace QtLLM {

using ToolHandler = std::function<QJsonObject(const QJsonObject&)>;

class ClaudeProtocol : public QObject
{
    Q_OBJECT
public:
    explicit ClaudeProtocol(const QString& apiKey,
                            const QUrl& url,
                            QObject* parent = nullptr);
    ~ClaudeProtocol() override;

    void setModel(const QString& model);
    void setMaxTokens(int maxTokens);
    void setSystemPrompt(const QString& systemPrompt);

    // Called by Client when tool registration changes
    void setTools(const QList<QJsonObject>& toolSchemas,
                  const QMap<QString, ToolHandler>& handlers);

    // Begins a new request turn. history must already include the new user message.
    void sendTurn(const QJsonArray& history);

    void clearHistory();
    QJsonArray history() const;

signals:
    void responseReady(const QString& assembledText);
    void toolInvoked(const QString& toolName, const QJsonObject& input);
    void toolCompleted(const QString& toolName, const QJsonObject& result);
    void errorOccurred(const QString& message);
    void requestStarted();
    void requestFinished();

private slots:
    void onReplyReceived(const QByteArray& data);
    void onTransportError(const QString& message);

private:
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
