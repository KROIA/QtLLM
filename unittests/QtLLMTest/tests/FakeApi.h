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

// A canned Anthropic Messages endpoint on a loopback port, for tests that need
// a whole turn (request -> tool call -> request -> answer) without a network.
//
// Every POST to .../messages is answered with the next queued body; other
// requests (model list, token counting) get an empty object. With `silent`
// set, message requests are received but never answered.
struct FakeApi
{
    QQueue<QByteArray> replies;
    QList<QByteArray>  messageBodies;   // request bodies received, in order
    QList<QByteArray>  messageHeads;    // their request lines + headers, lower-cased
    QHash<QTcpSocket*, QByteArray> buffers;
    bool               silent = false;
    // Send a reply in two pieces, cut in the middle - as a network would.
    bool               splitWrites = false;
    int                messageRequests = 0;
    // Last member on purpose: it is destroyed first, and destroying it closes
    // the open sockets, whose handlers still use the members above.
    QTcpServer         server;

    FakeApi()
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

    QString url() const
    {
        return QString("http://127.0.0.1:%1/v1/messages").arg(server.serverPort());
    }

    // A Messages API response body.
    static QByteArray reply(const QString& stopReason, const QJsonArray& content)
    {
        return QJsonDocument(QJsonObject{{"stop_reason", stopReason},
                                         {"content", content},
                                         {"usage", QJsonObject{{"input_tokens", 1}, {"output_tokens", 1}}}})
            .toJson(QJsonDocument::Compact);
    }

    // The same response as reply(), as the event stream the API sends for
    // "stream": true. Text and tool input are split into two deltas each.
    static QByteArray sse(const QString& stopReason, const QJsonArray& content)
    {
        auto event = [](const QString& name, const QJsonObject& data) {
            QJsonObject payload = data;
            payload["type"] = name;
            return QByteArray("event: ") + name.toUtf8() + "\ndata: "
                 + QJsonDocument(payload).toJson(QJsonDocument::Compact) + "\n\n";
        };
        QByteArray out = event("message_start", {{"message", QJsonObject{
            {"usage", QJsonObject{{"input_tokens", 7}, {"cache_read_input_tokens", 3}}}}}});
        for (int i = 0; i < content.size(); ++i) {
            const QJsonObject block = content[i].toObject();
            const QString type = block["type"].toString();
            QJsonObject start = block;
            if (type == "text")
                start["text"] = "";
            else if (type == "tool_use")
                start["input"] = QJsonObject();
            out += event("content_block_start", {{"index", i}, {"content_block", start}});

            QString text;
            QString deltaType, field;
            if (type == "text") {
                text = block["text"].toString(); deltaType = "text_delta"; field = "text";
            } else if (type == "tool_use") {
                text = QString::fromUtf8(QJsonDocument(block["input"].toObject()).toJson(QJsonDocument::Compact));
                deltaType = "input_json_delta"; field = "partial_json";
            }
            if (!deltaType.isEmpty()) {
                const int half = text.size() / 2;
                for (const QString& part : {text.left(half), text.mid(half)})
                    out += event("content_block_delta", {{"index", i},
                        {"delta", QJsonObject{{"type", deltaType}, {field, part}}}});
            }
            out += event("content_block_stop", {{"index", i}});
        }
        out += event("message_delta", {{"delta", QJsonObject{{"stop_reason", stopReason}}},
                                       {"usage", QJsonObject{{"output_tokens", 5}}}});
        out += event("message_stop", {});
        return out;
    }

    static QByteArray textReply(const QString& text)
    {
        return reply("end_turn", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}});
    }

    static QByteArray toolUseReply(const QString& toolName, const QJsonObject& input,
                                   const QString& id = "t1")
    {
        return reply("tool_use", QJsonArray{QJsonObject{
            {"type", "tool_use"}, {"id", id}, {"name", toolName}, {"input", input}}});
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

        QByteArray body = "{}";
        if (requestLine.startsWith("POST") && requestLine.contains("/messages ")) {
            ++messageRequests;
            messageBodies.append(requestBody);
            messageHeads.append(head);
            if (silent)
                return;
            if (!replies.isEmpty())
                body = replies.dequeue();
        }
        const QByteArray contentType = body.startsWith("event:") ? "text/event-stream" : "application/json";
        const QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: " + contentType
            + "\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size())
            + "\r\n\r\n" + body;
        if (!splitWrites) {
            socket->write(response);
            socket->disconnectFromHost();
            return;
        }
        const int cut = response.size() - body.size() / 2;   // in the middle of the body
        socket->write(response.left(cut));
        socket->flush();
        QTimer::singleShot(30, socket, [socket, response, cut]() {
            socket->write(response.mid(cut));
            socket->disconnectFromHost();
        });
    }
};
