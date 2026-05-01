#include "HttpTransport.h"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QThread>

namespace QtLLM
{

HttpTransport::HttpTransport(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
    , m_busy(false)
{
    // This object must live on a thread that has an event loop
    Q_ASSERT(QThread::currentThread()->eventDispatcher());

    connect(m_nam, &QNetworkAccessManager::finished,
            this, &HttpTransport::onReplyFinished);
}

HttpTransport::~HttpTransport() = default;

void HttpTransport::post(const QUrl& url,
                         const QByteArray& jsonBody,
                         const QList<QPair<QByteArray, QByteArray>>& headers)
{
    if (m_busy) {
        emit errorOccurred("Request already in progress");
        return;
    }

    m_busy = true;

    QNetworkRequest request(url);
    request.setRawHeader("Content-Type", "application/json");
    for (const auto& pair : headers) {
        request.setRawHeader(pair.first, pair.second);
    }

    m_nam->post(request, jsonBody);
}

bool HttpTransport::isBusy() const
{
    return m_busy;
}

void HttpTransport::onReplyFinished(QNetworkReply* reply)
{
    m_busy = false;

    // Always read the body first — the API returns parseable JSON even on HTTP errors
    QByteArray data = reply->readAll();

    if (reply->error() != QNetworkReply::NoError && data.isEmpty()) {
        emit errorOccurred(reply->errorString());
    } else {
        emit replyReceived(data);
    }

    reply->deleteLater();
}

} // namespace QtLLM
