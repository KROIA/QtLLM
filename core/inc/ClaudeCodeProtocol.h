#pragma once
#include "QtLLM_base.h"
#include "ProtocolBase.h"
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QQueue>
#include <QString>
#include <functional>

class QProcess;
class QTcpServer;
class QTcpSocket;

namespace QtLLM {

// Runs turns through the locally installed Claude Code CLI (`claude -p`), so
// requests are covered by the user's own claude.ai subscription (Pro / Max /
// Team) instead of a pay-per-token API key.
//
// A subscription cannot call the Messages API directly - Anthropic only
// accepts those credentials from Claude Code. So the CLI is the transport: it
// owns the login, the agent loop and the conversation (resumed per turn by
// session id), and this class only supplies the prompt, the system prompt and
// the tools.
//
// Tools reach the CLI over MCP: this object serves the registered handlers on
// a loopback-only HTTP endpoint (random port, random bearer token) that the
// CLI is pointed at for the duration of each turn. Handlers therefore still
// run synchronously on the GUI thread, exactly as with the other providers.
//
// The CLI is started with its own built-in tools, user settings, hooks,
// plugins and CLAUDE.md all switched off: the model sees the application's
// system prompt and the application's tools, nothing from the user's coding
// setup.
//
// Requires: Claude Code installed and logged in once (`claude`, then /login).
class QT_LLM_API ClaudeCodeProtocol : public ProtocolBase
{
    Q_OBJECT
public:
    explicit ClaudeCodeProtocol(QObject* parent = nullptr);
    ~ClaudeCodeProtocol() override;

    // An alias ("sonnet", "opus", "haiku", "fable") or a full model id.
    void setModel(const QString& model) override;
    // Not applicable: the CLI manages output length itself.
    void setMaxTokens(int maxTokens) override;
    void setSystemPrompt(const QString& systemPrompt) override;
    // Schemas in Claude format ({"name","description","input_schema"}).
    void setTools(const QList<QJsonObject>& toolSchemas,
                  const QMap<QString, ToolCallHandler>& handlers) override;

    void beginTurn(const QString& userMessage) override;
    bool cancelTurn() override;
    void clearHistory() override;
    // The CLI owns the conversation, so a replaced history means: start a new
    // CLI session and hand it the (single, summarizing) message with the next
    // prompt.
    void replaceHistory(const QJsonArray& messages) override;
    void clearStats() override;
    void fetchModels() override;
    QJsonArray conversationMessages() const override;

    // Full path of the `claude` executable, empty when it is not installed.
    static QString findExecutable();

    // Asynchronously asks the CLI who is logged in. The callback receives the
    // parsed `claude auth status` object ("loggedIn", "email",
    // "subscriptionType", ...) or an empty object when the CLI is missing or
    // did not answer. It is not called once `context` has been destroyed.
    static void queryAuthStatus(QObject* context,
                                std::function<void(const QJsonObject&)> callback);

private:
    void startNewTurn(const QString& userMessage);
    void drainQueue();
    void rollbackFailedTurn();
    void finishTurn(const QString& text, const QJsonObject& usage);
    void failTurn(const QString& message);
    void onProcessFinished(int exitCode);
    void onProcessOutput();

    // --- loopback MCP endpoint -------------------------------------------
    bool        ensureServer();
    void        onSocketReadyRead(QTcpSocket* socket);
    QJsonObject handleRpc(const QJsonObject& request, bool* isNotification);
    // Runs the tool and answers the HTTP request when its result is in -
    // which for an asynchronous tool is later.
    void        startToolCall(QTcpSocket* socket, const QJsonObject& request);

    QString                    m_model;
    QString                    m_systemPrompt;
    QList<QJsonObject>         m_toolSchemas;
    QMap<QString, ToolCallHandler> m_toolHandlers;
    QJsonArray                 m_history;       // simplified user/assistant text turns
    QString                    m_sessionId;     // CLI conversation to resume; empty = new
    QString                    m_carryOver;     // text prepended to the first prompt of a new session

    QPointer<QProcess>         m_process;
    QByteArray                 m_stdout;        // everything the CLI printed this turn
    int                        m_stdoutParsed = 0;   // bytes already scanned for text deltas
    QTcpServer*                m_server = nullptr;
    QString                    m_serverToken;
    QString                    m_systemPromptPath;   // per-instance temp files handed to the CLI
    QString                    m_mcpConfigPath;
    QHash<QTcpSocket*, QByteArray> m_socketBuffers;
    QHash<QTcpSocket*, bool>       m_socketBusy;

    bool            m_turnInProgress = false;
    QQueue<QString> m_pendingTurns;
    int             m_historyLenBeforeTurn = 0;

    QElapsedTimer m_turnTimer;
    int  m_turnToolCalls      = 0;
    int  m_sessionInputTokens  = 0;
    int  m_sessionOutputTokens = 0;
    int  m_sessionCacheCreationInputTokens = 0;
    int  m_sessionCacheReadInputTokens     = 0;
    int  m_sessionToolCalls    = 0;
    int  m_sessionTurnCount    = 0;
};

} // namespace QtLLM
