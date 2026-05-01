#pragma once

#include <QObject>
#include <QNetworkAccessManager>

namespace QtLLM
{

class HttpTransport : public QObject
{
    Q_OBJECT
public:
    explicit HttpTransport(QObject* parent = nullptr);
    ~HttpTransport() override;

    // Sends a POST request. Sets headers: Content-Type: application/json,
    // x-api-key: apiKey, anthropic-version: 2023-06-01
    void post(const QUrl& url,
              const QByteArray& jsonBody,
              const QString& apiKey);

    bool isBusy() const;

signals:
    void replyReceived(const QByteArray& data);
    void errorOccurred(const QString& message);

private slots:
    void onReplyFinished(QNetworkReply* reply);

private:
    QNetworkAccessManager* m_nam;
    bool                   m_busy;
};

} // namespace QtLLM
