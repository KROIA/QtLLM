#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

// A canned OpenAI Chat Completions endpoint on a loopback port.
//
// Every POST to /v1/chat/completions is answered with the next queued reply
// (status + body); GET /v1/models lists "m1" and "m2". With `silent` set, chat
// requests are received but never answered.
struct FakeOpenAi
{
    struct Reply
    {
        QByteArray body;
        int        status = 200;
    };

    QQueue<Reply>      replies;
    QList<QByteArray>  bodies;     // chat request bodies, in order
    QList<QByteArray>  heads;      // their request headers, lower-cased
    QHash<QTcpSocket*, QByteArray> buffers;
    bool               silent = false;
    // Send each reply in two pieces, cut in the middle of the body.
    bool               splitWrites = false;
    // Last member on purpose: destroyed first, closing the sockets whose
    // handlers still use the members above.
    QTcpServer         server;

    FakeOpenAi()
    {
        server.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&server, &QTcpServer::newConnection, [this]() {
            while (QTcpSocket* socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, [this, socket]() { onData(socket); });
                QObject::connect(socket, &QTcpSocket::disconnected, [this, socket]() {
                    buffers.remove(socket);
                    socket->deleteLater();
                });
            }
        });
    }

    QString baseUrl() const
    {
        return QString("http://127.0.0.1:%1/v1").arg(server.serverPort());
    }

    void enqueue(const QByteArray& body, int status = 200)
    {
        replies.enqueue(Reply{body, status});
    }

    QJsonObject body(int i) const
    {
        return QJsonDocument::fromJson(bodies.value(i)).object();
    }

    // A non-streamed chat.completion.
    static QByteArray completion(const QString& text, const QJsonArray& toolCalls = QJsonArray(),
                                 const QString& finish = "stop")
    {
        QJsonObject message{{"role", "assistant"}, {"content", text}};
        if (!toolCalls.isEmpty())
            message["tool_calls"] = toolCalls;
        return QJsonDocument(QJsonObject{
            {"choices", QJsonArray{QJsonObject{{"index", 0}, {"message", message}, {"finish_reason", finish}}}},
            {"usage", QJsonObject{{"prompt_tokens", 10}, {"completion_tokens", 3}}}})
            .toJson(QJsonDocument::Compact);
    }

    static QJsonObject toolCall(const QString& id, const QString& name, const QString& arguments)
    {
        return QJsonObject{{"id", id}, {"type", "function"},
                           {"function", QJsonObject{{"name", name}, {"arguments", arguments}}}};
    }

    // SSE: one "data:" line per chunk object, then the usage chunk and [DONE].
    static QByteArray sse(const QList<QJsonObject>& chunks)
    {
        QByteArray out;
        for (const QJsonObject& chunk : chunks)
            out += "data: " + QJsonDocument(chunk).toJson(QJsonDocument::Compact) + "\n\n";
        out += "data: " + QJsonDocument(QJsonObject{
            {"choices", QJsonArray()},
            {"usage", QJsonObject{{"prompt_tokens", 20}, {"completion_tokens", 5}}}})
            .toJson(QJsonDocument::Compact) + "\n\n";
        out += "data: [DONE]\n\n";
        return out;
    }

    static QJsonObject deltaChunk(const QJsonObject& delta, const QString& finish = QString())
    {
        QJsonObject choice{{"index", 0}, {"delta", delta}};
        choice["finish_reason"] = finish.isEmpty() ? QJsonValue() : QJsonValue(finish);
        return QJsonObject{{"choices", QJsonArray{choice}}, {"usage", QJsonValue()}};
    }

private:
    void onData(QTcpSocket* socket)
    {
        QByteArray& buffer = buffers[socket];
        buffer += socket->readAll();
        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;
        const QByteArray head = buffer.left(headerEnd).toLower();
        int length = 0;
        const int pos = head.indexOf("content-length:");
        if (pos >= 0) {
            int lineEnd = head.indexOf('\r', pos);
            if (lineEnd < 0)
                lineEnd = head.size();
            length = head.mid(pos + 15, lineEnd - pos - 15).trimmed().toInt();
        }
        if (buffer.size() < headerEnd + 4 + length)
            return;

        const QByteArray requestLine = buffer.left(buffer.indexOf('\r'));
        const QByteArray requestBody = buffer.mid(headerEnd + 4, length);
        buffer.clear();

        Reply reply{"{}", 404};
        if (requestLine.startsWith("POST /v1/chat/completions ")) {
            bodies.append(requestBody);
            heads.append(head);
            if (silent)
                return;
            reply = replies.isEmpty() ? Reply{"{}", 500} : replies.dequeue();
        } else if (requestLine.startsWith("GET /v1/models ")) {
            reply = Reply{QJsonDocument(QJsonObject{{"object", "list"}, {"data", QJsonArray{
                QJsonObject{{"id", "m1"}}, QJsonObject{{"id", "m2"}}}}}).toJson(QJsonDocument::Compact), 200};
        }
        const QByteArray contentType = reply.body.startsWith("data:") ? "text/event-stream" : "application/json";
        const QByteArray response = "HTTP/1.1 " + QByteArray::number(reply.status) + " X\r\nContent-Type: "
            + contentType + "\r\nConnection: close\r\nContent-Length: "
            + QByteArray::number(reply.body.size()) + "\r\n\r\n" + reply.body;
        if (!splitWrites) {
            socket->write(response);
            socket->disconnectFromHost();
            return;
        }
        const int cut = response.size() - reply.body.size() / 2;
        socket->write(response.left(cut));
        socket->flush();
        QTimer::singleShot(30, socket, [socket, response, cut]() {
            socket->write(response.mid(cut));
            socket->disconnectFromHost();
        });
    }
};
