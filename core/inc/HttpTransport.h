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

    // POST whose answer is delivered while it arrives: chunkReceived() for
    // every piece of the body, then streamFinished(). In this mode the timeout
    // is an idle timeout - it restarts with every chunk, so a long answer is
    // fine and only a stalled one is aborted.
    // An HTTP error status is not streamed: its body arrives in one piece
    // through replyReceived(), exactly as with post().
    void postStream(const QUrl& url,
                    const QByteArray& jsonBody,
                    const QList<QPair<QByteArray, QByteArray>>& headers = {});

    // Simple GET request with optional custom headers.
    void get(const QUrl& url,
             const QList<QPair<QByteArray, QByteArray>>& headers = {});

    bool isBusy() const;

    // Cancel the in-flight request silently: neither replyReceived() nor
    // errorOccurred() is emitted for it. No-op when idle.
    void abort();

    // HTTP status of the most recently finished reply (0 = none / no response).
    int lastStatusCode() const { return m_lastStatusCode; }

    // Aborts the request and emits errorOccurred() if no reply arrives within
    // this many ms. Qt5's QNetworkAccessManager has no built-in timeout, so
    // without this a stalled proxy / MITM SSL inspection / dropped connection
    // hangs forever with no error and no reply - silently, from the caller's
    // perspective indistinguishable from "nothing happened". Default: 60s.
    void setTimeoutMs(int ms);

signals:
    void replyReceived(const QByteArray& data);
    void chunkReceived(const QByteArray& data);
    void streamFinished();
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
    bool                    m_aborted = false;
    bool                    m_streaming = false;
    bool                    m_streamIsError = false;   // HTTP error status: buffer, don't stream
    QByteArray              m_streamErrorBody;
    int                     m_lastStatusCode = 0;
};

} // namespace QtLLM
