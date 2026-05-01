#pragma once
#include "QtLLM_base.h"
#include <QObject>
#include <QNetworkAccessManager>

namespace QtLLM
{

class QT_LLM_API HttpTransport : public QObject
{
    Q_OBJECT
public:
    explicit HttpTransport(QObject* parent = nullptr);
    ~HttpTransport() override;

    // headers: list of (header-name, header-value) pairs appended to Content-Type
    void post(const QUrl& url,
              const QByteArray& jsonBody,
              const QList<QPair<QByteArray, QByteArray>>& headers = {});

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
