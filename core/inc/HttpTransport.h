#pragma once
#include "QtLLM_base.h"
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QPointer>

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

    // Simple GET request with optional custom headers.
    void get(const QUrl& url,
             const QList<QPair<QByteArray, QByteArray>>& headers = {});

    bool isBusy() const;

    // Aborts the request and emits errorOccurred() if no reply arrives within
    // this many ms. Qt5's QNetworkAccessManager has no built-in timeout, so
    // without this a stalled proxy / MITM SSL inspection / dropped connection
    // hangs forever with no error and no reply - silently, from the caller's
    // perspective indistinguishable from "nothing happened". Default: 60s.
    void setTimeoutMs(int ms);

signals:
    void replyReceived(const QByteArray& data);
    void errorOccurred(const QString& message);

private slots:
    void onReplyFinished(QNetworkReply* reply);
    void onTimeout();

private:
    QNetworkAccessManager*  m_nam;
    bool                    m_busy;
    QTimer                  m_timeoutTimer;
    int                     m_timeoutMs = 60000;
    QPointer<QNetworkReply> m_currentReply;
};

} // namespace QtLLM
