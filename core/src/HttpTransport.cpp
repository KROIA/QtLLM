#include "HttpTransport.h"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QThread>
#include <QDebug>

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

    m_timeoutTimer.setSingleShot(true);
    connect(&m_timeoutTimer, &QTimer::timeout, this, &HttpTransport::onTimeout);
}

HttpTransport::~HttpTransport() = default;

void HttpTransport::setTimeoutMs(int ms)
{
    m_timeoutMs = ms;
}

void HttpTransport::post(const QUrl& url,
                         const QByteArray& jsonBody,
                         const QList<QPair<QByteArray, QByteArray>>& headers)
{
    if (m_busy) {
        emit errorOccurred("Request already in progress");
        return;
    }

    m_busy = true;

    qDebug() << "[HttpTransport] POST" << url << "body bytes:" << jsonBody.size();

    QNetworkRequest request(url);
    request.setRawHeader("Content-Type", "application/json");
    for (const auto& pair : headers) {
        request.setRawHeader(pair.first, pair.second);
    }

    m_currentReply = m_nam->post(request, jsonBody);
    m_timeoutTimer.start(m_timeoutMs);
}

void HttpTransport::get(const QUrl& url,
                        const QList<QPair<QByteArray, QByteArray>>& headers)
{
    if (m_busy) {
        emit errorOccurred("Request already in progress");
        return;
    }

    m_busy = true;

    qDebug() << "[HttpTransport] GET" << url;

    QNetworkRequest request(url);
    for (const auto& pair : headers) {
        request.setRawHeader(pair.first, pair.second);
    }

    m_currentReply = m_nam->get(request);
    m_timeoutTimer.start(m_timeoutMs);
}

bool HttpTransport::isBusy() const
{
    return m_busy;
}

void HttpTransport::onTimeout()
{
    if (!m_currentReply)
        return;
    qDebug() << "[HttpTransport] request timed out after" << m_timeoutMs << "ms, aborting:" << m_currentReply->url();
    // abort() triggers QNetworkAccessManager::finished() itself, which runs
    // onReplyFinished() below - errorOccurred() fires from there (OperationCanceledError, empty body).
    m_currentReply->abort();
}

void HttpTransport::onReplyFinished(QNetworkReply* reply)
{
    m_timeoutTimer.stop();
    m_busy = false;

    // Always read the body first — the API returns parseable JSON even on HTTP errors
    QByteArray data = reply->readAll();

    qDebug() << "[HttpTransport] reply finished, error=" << reply->error()
             << reply->errorString() << "bytes=" << data.size()
             << "preview=" << data.left(300);

    if (reply->error() == QNetworkReply::OperationCanceledError) {
        // Our own timeout abort - whatever bytes had arrived so far are a
        // truncated body, not a complete (possibly error-shaped) JSON
        // response. Report it as a timeout rather than falling through to
        // replyReceived(), which would hand callers unparseable partial JSON
        // and surface as a confusing "failed to parse response" error.
        emit errorOccurred(QString("Request timed out after %1 ms").arg(m_timeoutMs));
    } else if (reply->error() != QNetworkReply::NoError && data.isEmpty()) {
        emit errorOccurred(reply->errorString());
    } else {
        emit replyReceived(data);
    }

    reply->deleteLater();
}

} // namespace QtLLM
