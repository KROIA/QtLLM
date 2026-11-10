#include "ClaudeCodeProtocol.h"
#include "ToolResult.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>

namespace QtLLM {

namespace {

const char* const kMcpServerName = "qtllm";

// Aliases the CLI resolves to the newest model of each family.
const QStringList kModelAliases = {QStringLiteral("sonnet"), QStringLiteral("opus"),
                                   QStringLiteral("haiku"), QStringLiteral("fable")};

// Directory the CLI runs in. Its conversations are stored per working
// directory, so a private one keeps them out of whatever project the host
// application happens to be started from.
QString workingDirectory()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                        + QStringLiteral("/QtLLM/claude-code");
    QDir().mkpath(dir);
    return dir;
}

// The subscription login only wins when nothing else is configured: an API
// key or a cloud-provider switch in the environment silently takes
// precedence inside the CLI and bills somewhere else.
QProcessEnvironment subscriptionEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const char* name : {"ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN", "ANTHROPIC_BASE_URL",
                             "ANTHROPIC_PROFILE", "CLAUDE_CODE_USE_BEDROCK",
                             "CLAUDE_CODE_USE_VERTEX", "CLAUDE_CODE_USE_FOUNDRY"})
        env.remove(QString::fromLatin1(name));
    return env;
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(data) == data.size();
}

// `--output-format json` prints one result object; tolerate anything a future
// CLI version prints around it by taking the last line that parses as one.
QJsonObject parseResult(const QByteArray& output)
{
    QJsonDocument doc = QJsonDocument::fromJson(output.trimmed());
    if (doc.isObject())
        return doc.object();
    const QList<QByteArray> lines = output.split('\n');
    for (int i = lines.size() - 1; i >= 0; --i) {
        doc = QJsonDocument::fromJson(lines[i].trimmed());
        if (doc.isObject() && doc.object().contains(QStringLiteral("result")))
            return doc.object();
    }
    return {};
}

void writeHttpResponse(QTcpSocket* socket, const QByteArray& status, const QByteArray& body)
{
    QByteArray response = "HTTP/1.1 " + status + "\r\n";
    if (!body.isEmpty())
        response += "Content-Type: application/json\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
    socket->write(response);
}

} // namespace


ClaudeCodeProtocol::ClaudeCodeProtocol(QObject* parent)
    : ProtocolBase(parent)
    , m_model(QStringLiteral("sonnet"))
    // Loopback only, and a fresh secret per instance: any other local program
    // that finds the port still cannot drive the application's tools.
    , m_serverToken(QUuid::createUuid().toString(QUuid::Id128)
                    + QUuid::createUuid().toString(QUuid::Id128))
{
    const QString base = workingDirectory() + QLatin1Char('/') + m_serverToken.left(8);
    m_systemPromptPath = base + QStringLiteral("-system-prompt.txt");
    m_mcpConfigPath    = base + QStringLiteral("-mcp.json");
}

ClaudeCodeProtocol::~ClaudeCodeProtocol()
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(2000);
    }
    QFile::remove(m_systemPromptPath);
    QFile::remove(m_mcpConfigPath);
}

QString ClaudeCodeProtocol::findExecutable()
{
    QString path = QStandardPaths::findExecutable(QStringLiteral("claude"));
    if (path.isEmpty()) {
        // The native installer's location, which a GUI process started before
        // the installer ran does not have on its PATH yet.
        path = QStandardPaths::findExecutable(QStringLiteral("claude"),
                                              {QDir::homePath() + QStringLiteral("/.local/bin")});
    }
    return path;
}

void ClaudeCodeProtocol::queryAuthStatus(QObject* context,
                                         std::function<void(const QJsonObject&)> callback)
{
    const QString exe = findExecutable();
    if (exe.isEmpty() || !context) {
        if (context)
            QTimer::singleShot(0, context, [callback]() { callback(QJsonObject()); });
        return;
    }

    // Deliberately not a child of `context`: a child process object is
    // destroyed from within the context's destructor, where ~QProcess waits
    // for the process and emits finished() - delivering the callback to a
    // half-destroyed context (a dialog whose widgets are already gone).
    auto* process = new QProcess;
    process->setProcessEnvironment(subscriptionEnvironment());
    QObject::connect(context, &QObject::destroyed, process, [process]() {
        process->disconnect();
        process->kill();
        process->deleteLater();
    });
    auto done = [process, callback]() {
        callback(QJsonDocument::fromJson(process->readAllStandardOutput()).object());
        process->deleteLater();
    };
    QObject::connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     context, [done](int, QProcess::ExitStatus) { done(); });
    QObject::connect(process, &QProcess::errorOccurred, context,
                     [done](QProcess::ProcessError error) {
                         if (error == QProcess::FailedToStart)
                             done();
                     });
    process->start(exe, {QStringLiteral("auth"), QStringLiteral("status")});
}

void ClaudeCodeProtocol::setModel(const QString& model)              { m_model = model; }
void ClaudeCodeProtocol::setMaxTokens(int)                           {}
void ClaudeCodeProtocol::setSystemPrompt(const QString& systemPrompt) { m_systemPrompt = systemPrompt; }

void ClaudeCodeProtocol::setTools(const QList<QJsonObject>& toolSchemas,
                                  const QMap<QString, ToolCallHandler>& handlers)
{
    m_toolSchemas  = toolSchemas;
    m_toolHandlers = handlers;
}

QJsonArray ClaudeCodeProtocol::conversationMessages() const { return m_history; }

void ClaudeCodeProtocol::clearHistory()
{
    m_history = QJsonArray();
    m_sessionId.clear();
    m_carryOver.clear();
}

void ClaudeCodeProtocol::replaceHistory(const QJsonArray& messages)
{
    m_history = messages;
    m_sessionId.clear();
    QStringList parts;
    for (const QJsonValue& message : messages)
        parts.append(message.toObject()["content"].toString());
    m_carryOver = parts.join(QStringLiteral("\n\n"));
}

void ClaudeCodeProtocol::clearStats()
{
    m_sessionInputTokens = m_sessionOutputTokens = 0;
    m_sessionCacheCreationInputTokens = m_sessionCacheReadInputTokens = 0;
    m_sessionToolCalls = m_sessionTurnCount = 0;
}

void ClaudeCodeProtocol::fetchModels()
{
    // There is no endpoint to ask: the CLI accepts family aliases and full ids.
    // A full id the application already configured is kept as valid.
    // Built when delivered, not when asked: the model may be set in between.
    QTimer::singleShot(0, this, [this]() {
        QStringList models = kModelAliases;
        if (m_model.startsWith(QLatin1String("claude-")) && !models.contains(m_model))
            models.append(m_model);
        emit modelsFetched(models);
    });
}

void ClaudeCodeProtocol::beginTurn(const QString& userMessage)
{
    if (m_turnInProgress) {
        m_pendingTurns.enqueue(userMessage);
        return;
    }
    startNewTurn(userMessage);
}

void ClaudeCodeProtocol::drainQueue()
{
    m_turnInProgress = false;
    if (!m_pendingTurns.isEmpty())
        startNewTurn(m_pendingTurns.dequeue());
}

void ClaudeCodeProtocol::rollbackFailedTurn()
{
    while (m_history.size() > m_historyLenBeforeTurn)
        m_history.removeAt(m_history.size() - 1);
}

void ClaudeCodeProtocol::failTurn(const QString& message)
{
    rollbackFailedTurn();
    emit errorOccurred(message);
    emit requestFinished();
    drainQueue();
}

bool ClaudeCodeProtocol::cancelTurn()
{
    if (!m_turnInProgress)
        return false;
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->deleteLater();
        m_process = nullptr;
    }
    m_pendingTurns.clear();
    rollbackFailedTurn();
    emit requestFinished();
    drainQueue();
    return true;
}

void ClaudeCodeProtocol::startNewTurn(const QString& userMessage)
{
    m_turnInProgress       = true;
    m_turnToolCalls        = 0;
    m_historyLenBeforeTurn = m_history.size();
    m_turnTimer.start();

    m_history.append(QJsonObject{{"role", "user"}, {"content", userMessage}});
    emit requestStarted();

    const QString exe = findExecutable();
    if (exe.isEmpty()) {
        failTurn(QStringLiteral(
            "Claude Code CLI not found. Install it (https://claude.com/claude-code), "
            "run `claude` once and log in with your claude.ai account."));
        return;
    }

    // Prompt text and config go through files/stdin, never the command line:
    // Windows caps a command line at 32k characters and mangles quoting.
    const QString dir = workingDirectory();
    const QString systemPromptPath = m_systemPromptPath;
    // Without an explicit system prompt the CLI would use its own coding-agent
    // prompt - thousands of tokens describing a job this application does not do.
    const QString systemPrompt = m_systemPrompt.isEmpty()
        ? QStringLiteral("You are a helpful assistant integrated into a desktop application.")
        : m_systemPrompt;
    if (!writeFile(systemPromptPath, systemPrompt.toUtf8())) {
        failTurn(QStringLiteral("Cannot write %1").arg(systemPromptPath));
        return;
    }

    QStringList args = {
        QStringLiteral("-p"),
        QStringLiteral("--system-prompt-file"), systemPromptPath,
        // Nothing from the user's coding setup: no built-in tools (shell, file
        // edits), no settings/hooks/plugins/CLAUDE.md, no other MCP servers.
        QStringLiteral("--tools"), QString(),
        QStringLiteral("--setting-sources"), QString(),
        QStringLiteral("--strict-mcp-config"),
        QStringLiteral("--disable-slash-commands"),
    };
    if (!m_model.isEmpty())
        args << QStringLiteral("--model") << m_model;
    if (!m_sessionId.isEmpty())
        args << QStringLiteral("--resume") << m_sessionId;
    if (m_effort != Effort::Default)
        args << QStringLiteral("--effort") << effortName(m_effort);
    if (m_streamingEnabled) {
        // One JSON object per line; partial messages carry the text deltas.
        args << QStringLiteral("--output-format") << QStringLiteral("stream-json")
             << QStringLiteral("--verbose") << QStringLiteral("--include-partial-messages");
    } else {
        args << QStringLiteral("--output-format") << QStringLiteral("json");
    }

    if (!m_toolSchemas.isEmpty()) {
        if (!ensureServer()) {
            failTurn(QStringLiteral("Cannot open a local port for the tool endpoint: %1")
                         .arg(m_server->errorString()));
            return;
        }
        const QJsonObject server{
            {"type", "http"},
            {"url", QStringLiteral("http://127.0.0.1:%1/mcp").arg(m_server->serverPort())},
            {"headers", QJsonObject{{"Authorization", QStringLiteral("Bearer ") + m_serverToken}}}};
        const QJsonObject config{{"mcpServers", QJsonObject{{kMcpServerName, server}}}};
        const QString configPath = m_mcpConfigPath;
        if (!writeFile(configPath, QJsonDocument(config).toJson(QJsonDocument::Compact))) {
            failTurn(QStringLiteral("Cannot write %1").arg(configPath));
            return;
        }
        args << QStringLiteral("--mcp-config") << configPath
             // The application's own tools need no per-call approval from the
             // CLI; Client's consent handler is the gate for those.
             << QStringLiteral("--allowedTools") << QStringLiteral("mcp__") + kMcpServerName;
    }

    m_process = new QProcess(this);
    m_process->setWorkingDirectory(dir);
    m_process->setProcessEnvironment(subscriptionEnvironment());
    QProcess* process = m_process;
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, process](int exitCode, QProcess::ExitStatus) {
                if (process == m_process)
                    onProcessFinished(exitCode);
            });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (process != m_process || error != QProcess::FailedToStart)
            return;
        m_process->deleteLater();
        m_process = nullptr;
        failTurn(QStringLiteral("Failed to start Claude Code CLI: %1").arg(process->errorString()));
    });

    m_stdout.clear();
    m_stdoutParsed = 0;
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process]() {
        if (process == m_process)
            onProcessOutput();
    });

    process->start(exe, args);
    if (m_sessionId.isEmpty() && !m_carryOver.isEmpty()) {
        process->write((m_carryOver + QStringLiteral("\n\n")).toUtf8());
        m_carryOver.clear();
    }
    process->write(userMessage.toUtf8());
    process->closeWriteChannel();
}

void ClaudeCodeProtocol::onProcessOutput()
{
    m_stdout += m_process->readAllStandardOutput();
    if (!m_streamingEnabled)
        return;

    int end;
    while ((end = m_stdout.indexOf('\n', m_stdoutParsed)) >= 0) {
        const QByteArray line = m_stdout.mid(m_stdoutParsed, end - m_stdoutParsed);
        m_stdoutParsed = end + 1;
        if (!line.contains("text_delta"))
            continue;   // cheap filter: most lines are not text
        const QJsonObject obj = QJsonDocument::fromJson(line).object();
        // Only the main conversation; a nested agent's text is not the answer.
        if (obj["type"].toString() != QLatin1String("stream_event") || !obj["parent_tool_use_id"].isNull())
            continue;
        const QJsonObject delta = obj["event"].toObject()["delta"].toObject();
        if (delta["type"].toString() == QLatin1String("text_delta") && !delta["text"].toString().isEmpty())
            emit responseDelta(delta["text"].toString());
    }
}

void ClaudeCodeProtocol::onProcessFinished(int exitCode)
{
    onProcessOutput();
    const QByteArray out = m_stdout;
    const QByteArray err = m_process->readAllStandardError();
    m_process->deleteLater();
    m_process = nullptr;

    const QJsonObject result = parseResult(out);
    if (result.isEmpty()) {
        QString detail = QString::fromUtf8(err.isEmpty() ? out : err).trimmed().left(500);
        failTurn(QStringLiteral("Claude Code CLI failed (exit code %1): %2")
                     .arg(exitCode).arg(detail.isEmpty() ? QStringLiteral("no output") : detail));
        return;
    }

    // The session exists even when the turn failed; later turns continue it.
    const QString sessionId = result["session_id"].toString();
    const QString text      = result["result"].toString();

    if (result["is_error"].toBool() || exitCode != 0) {
        // Typical causes: not logged in, subscription limit reached, unknown model.
        failTurn(text.isEmpty() ? QStringLiteral("Claude Code CLI reported an error (%1)")
                                      .arg(result["subtype"].toString())
                                : text);
        return;
    }

    if (!sessionId.isEmpty())
        m_sessionId = sessionId;
    m_history.append(QJsonObject{{"role", "assistant"}, {"content", text}});
    finishTurn(text, result["usage"].toObject());
}

void ClaudeCodeProtocol::finishTurn(const QString& text, const QJsonObject& usage)
{
    UsageStats stats;
    stats.inputTokens              = usage["input_tokens"].toInt();
    stats.outputTokens             = usage["output_tokens"].toInt();
    stats.cacheCreationInputTokens = usage["cache_creation_input_tokens"].toInt();
    stats.cacheReadInputTokens     = usage["cache_read_input_tokens"].toInt();
    stats.toolCalls                = m_turnToolCalls;
    stats.durationMs               = m_turnTimer.elapsed();
    // The totals above cover every request of the turn; the last one alone
    // tells how large the conversation is now.
    const QJsonArray iterations = usage["iterations"].toArray();
    if (!iterations.isEmpty()) {
        const QJsonObject last = iterations.last().toObject();
        stats.contextTokens = last["input_tokens"].toInt() + last["cache_creation_input_tokens"].toInt()
                            + last["cache_read_input_tokens"].toInt() + last["output_tokens"].toInt();
    }

    m_sessionInputTokens              += stats.inputTokens;
    m_sessionOutputTokens             += stats.outputTokens;
    m_sessionCacheCreationInputTokens += stats.cacheCreationInputTokens;
    m_sessionCacheReadInputTokens     += stats.cacheReadInputTokens;
    m_sessionToolCalls                += m_turnToolCalls;
    ++m_sessionTurnCount;

    stats.sessionInputTokens              = m_sessionInputTokens;
    stats.sessionOutputTokens             = m_sessionOutputTokens;
    stats.sessionCacheCreationInputTokens = m_sessionCacheCreationInputTokens;
    stats.sessionCacheReadInputTokens     = m_sessionCacheReadInputTokens;
    stats.sessionToolCalls                = m_sessionToolCalls;
    stats.sessionTurnCount                = m_sessionTurnCount;
    stats.sessionCostUsd                  = 0.0;   // flat-rate subscription

    emit requestFinished();
    emit responseReady(text);
    emit statsReady(stats);
    drainQueue();
}

// ------------------------------------------------------------ MCP endpoint
//
// The smallest server the MCP "streamable HTTP" transport allows: every
// JSON-RPC request is a POST answered with a plain JSON body. No SSE stream,
// no sessions - neither is required for a tools-only server.

bool ClaudeCodeProtocol::ensureServer()
{
    if (m_server && m_server->isListening())
        return true;
    if (!m_server) {
        m_server = new QTcpServer(this);
        connect(m_server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket* socket = m_server->nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this,
                        [this, socket]() { onSocketReadyRead(socket); });
                connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
                    m_socketBuffers.remove(socket);
                    m_socketBusy.remove(socket);
                    socket->deleteLater();
                });
            }
        });
    }
    return m_server->listen(QHostAddress::LocalHost, 0);
}

void ClaudeCodeProtocol::onSocketReadyRead(QTcpSocket* socket)
{
    m_socketBuffers[socket] += socket->readAll();
    // A tool handler may open a modal dialog; more data arriving on this
    // socket meanwhile must wait for the loop below instead of re-entering it.
    if (m_socketBusy.value(socket))
        return;
    m_socketBusy[socket] = true;

    QPointer<ClaudeCodeProtocol> self(this);
    QPointer<QTcpSocket>         guard(socket);

    while (true) {
        QByteArray& buffer = m_socketBuffers[socket];
        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            break;

        const QList<QByteArray> headerLines = buffer.left(headerEnd).split('\n');
        const QByteArray method = headerLines.value(0).split(' ').value(0).trimmed();
        qint64     contentLength = 0;
        QByteArray authorization;
        for (int i = 1; i < headerLines.size(); ++i) {
            const int colon = headerLines[i].indexOf(':');
            if (colon < 0)
                continue;
            const QByteArray name  = headerLines[i].left(colon).trimmed().toLower();
            const QByteArray value = headerLines[i].mid(colon + 1).trimmed();
            if (name == "content-length")
                contentLength = value.toLongLong();
            else if (name == "authorization")
                authorization = value;
        }
        if (contentLength < 0 || contentLength > 64 * 1024 * 1024) {
            socket->disconnectFromHost();
            break;
        }
        if (buffer.size() < headerEnd + 4 + contentLength)
            break;   // body not complete yet

        const QByteArray body = buffer.mid(headerEnd + 4, int(contentLength));
        buffer.remove(0, headerEnd + 4 + int(contentLength));

        if (authorization != "Bearer " + m_serverToken.toLatin1()) {
            writeHttpResponse(socket, "401 Unauthorized", {});
            continue;
        }
        if (method != "POST") {
            // GET would open the optional server-to-client stream.
            writeHttpResponse(socket, "405 Method Not Allowed", {});
            continue;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.object()["method"].toString() == QLatin1String("tools/call")
            && doc.object().contains("id")) {
            // Answered when the tool is done. The socket stays marked busy
            // until then, so requests queued behind it keep their order; the
            // completion resumes this loop.
            startToolCall(socket, doc.object());
            return;
        }
        bool isNotification = false;
        const QJsonObject response = doc.isObject()
            ? handleRpc(doc.object(), &isNotification)
            : QJsonObject{{"jsonrpc", "2.0"}, {"id", QJsonValue()},
                          {"error", QJsonObject{{"code", -32700}, {"message", "Parse error"}}}};

        // The handler ran a nested event loop: either object may be gone.
        if (!self)
            return;
        if (!guard || guard->state() != QAbstractSocket::ConnectedState) {
            m_socketBuffers.remove(socket);
            m_socketBusy.remove(socket);
            return;
        }

        if (isNotification)
            writeHttpResponse(socket, "202 Accepted", {});
        else
            writeHttpResponse(socket, "200 OK", QJsonDocument(response).toJson(QJsonDocument::Compact));
    }

    if (self && guard)
        m_socketBusy[socket] = false;
}

QJsonObject ClaudeCodeProtocol::handleRpc(const QJsonObject& request, bool* isNotification)
{
    const QString    method = request["method"].toString();
    const QJsonValue id     = request["id"];
    const QJsonObject params = request["params"].toObject();

    *isNotification = !request.contains("id");
    if (*isNotification)
        return {};

    QJsonObject response{{"jsonrpc", "2.0"}, {"id", id}};

    if (method == QLatin1String("initialize")) {
        const QString version = params["protocolVersion"].toString(QStringLiteral("2025-06-18"));
        response["result"] = QJsonObject{
            {"protocolVersion", version},
            {"capabilities", QJsonObject{{"tools", QJsonObject()}}},
            {"serverInfo", QJsonObject{{"name", kMcpServerName}, {"version", "1"}}}};
    } else if (method == QLatin1String("ping")) {
        response["result"] = QJsonObject();
    } else if (method == QLatin1String("tools/list")) {
        QJsonArray tools;
        for (const QJsonObject& schema : m_toolSchemas) {
            tools.append(QJsonObject{{"name", schema["name"]},
                                     {"description", schema["description"]},
                                     {"inputSchema", schema["input_schema"]}});
        }
        response["result"] = QJsonObject{{"tools", tools}};
    } else {
        response["error"] = QJsonObject{{"code", -32601}, {"message", "Method not found"}};
    }
    return response;
}

void ClaudeCodeProtocol::startToolCall(QTcpSocket* socket, const QJsonObject& request)
{
    const QJsonObject params    = request["params"].toObject();
    const QString     name      = params["name"].toString();
    const QJsonObject arguments = params["arguments"].toObject();
    const QJsonValue  id        = request["id"];

    // The handler may run a nested event loop or answer much later; this
    // object and the socket can both be gone by then.
    QPointer<ClaudeCodeProtocol> self(this);
    QPointer<QTcpSocket>         guard(socket);
    auto answered = QSharedPointer<bool>::create(false);

    auto respond = [this, self, guard, id, answered](const QJsonObject& mcpResult) {
        if (!self || *answered)
            return;
        *answered = true;
        if (!guard)
            return;
        if (guard->state() == QAbstractSocket::ConnectedState) {
            const QJsonObject response{{"jsonrpc", "2.0"}, {"id", id}, {"result", mcpResult}};
            writeHttpResponse(guard, "200 OK", QJsonDocument(response).toJson(QJsonDocument::Compact));
        }
        m_socketBusy[guard] = false;
        onSocketReadyRead(guard);   // requests that arrived meanwhile
    };
    auto textResult = [](const QString& text, bool isError) {
        return QJsonObject{
            {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}},
            {"isError", isError}};
    };

    if (!m_toolHandlers.contains(name)) {
        respond(textResult(QStringLiteral("Error: unknown tool \"%1\"").arg(name), true));
        return;
    }

    ++m_turnToolCalls;
    emit toolInvoked(name, arguments);

    auto done = [this, self, respond, textResult, name](const QJsonObject& handlerResult) {
        if (!self)
            return;
        QJsonObject result = handlerResult;
        const QJsonArray images = toolTakeImages(result);
        emit toolCompleted(name, result);

        QJsonObject response = textResult(
            QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)),
            result["status"].toString() == QLatin1String("error"));
        if (!images.isEmpty()) {
            QJsonArray content = response["content"].toArray();
            for (const QJsonValue& image : images) {
                const QJsonObject img = image.toObject();
                content.append(QJsonObject{{"type", "image"},
                                           {"data", img["data"]},
                                           {"mimeType", img["media_type"]}});
            }
            response["content"] = content;
        }
        respond(response);
    };

    const ToolCallHandler handler = m_toolHandlers.value(name);
    try {
        handler(arguments, done);
    } catch (const std::exception& e) {
        done(QJsonObject{{"status", "error"},
                         {"message", QString("Internal error: %1").arg(e.what())}});
    } catch (...) {
        done(QJsonObject{{"status", "error"},
                         {"message", "Internal error: unknown exception in tool handler"}});
    }
}

} // namespace QtLLM
