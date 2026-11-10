#include "ChatDockWidget.h"
#include "InterviewWidget.h"
#include "ContextUsageBar.h"
#include <QFrame>
#include <QScrollBar>
#include <QKeyEvent>
#include <QJsonArray>
#include <QMimeData>
#include <QUrl>
#include <QEventLoop>
#include <QPointer>
#include <QTextDocument>
#include <QToolTip>
#include <QRegularExpression>
#include <QStyle>
#include <QPixmap>
#include <QFileDialog>
#include <QJsonDocument>
#include <QToolButton>
#include <QFile>
#include <QDateTime>
#include <QPainter>
#include <QPainterPath>
#include "Client.h"

// Resources compiled into a STATIC library are not auto-registered — the
// consuming app must initialise them. Must live at global scope (the generated
// qInitResources_icons symbol is global, not in namespace QtLLM).
static void qtllmInitResources()
{
    Q_INIT_RESOURCE(icons);
}

namespace QtLLM
{
    class ChatInputTextEdit : public QTextEdit
    {
    public:
        using QTextEdit::QTextEdit;
        std::function<void()> onSubmit;
        // Returns true when it took pictures out of pasted / dropped data.
        std::function<bool(const QMimeData*)> onPictures;

    protected:
        bool canInsertFromMimeData(const QMimeData* source) const override
        {
            return source->hasImage() || QTextEdit::canInsertFromMimeData(source);
        }
        void insertFromMimeData(const QMimeData* source) override
        {
            if (onPictures && onPictures(source))
                return;
            QTextEdit::insertFromMimeData(source);
        }
        void keyPressEvent(QKeyEvent* event) override
        {
            if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
                !(event->modifiers() & Qt::ShiftModifier)) {
                if (onSubmit)
                    onSubmit();
                event->accept();
                return;
            }
            QTextEdit::keyPressEvent(event);
        }
    };

    // "287k" for 287431; small counts as they are.
    static QString humanTokenCount(int tokens)
    {
        return tokens >= 1000 ? QString::number(qRound(tokens / 1000.0)) + "k" : QString::number(tokens);
    }

    // The "compress" glyph: two arrows pushing towards a bar between them.
    // Painted rather than loaded, so it takes the theme's text colour and is
    // sharp at any scale factor.
    static QIcon compactIcon(const QColor& color)
    {
        const int size = 64;
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(color, 5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

        const qreal mid = size / 2.0;
        painter.drawLine(QPointF(12, mid), QPointF(size - 12, mid));        // the bar

        auto arrow = [&](qreal fromY, qreal tipY) {
            const qreal head = tipY > fromY ? -9 : 9;                          // barbs point back
            painter.drawLine(QPointF(mid, fromY), QPointF(mid, tipY));
            painter.drawLine(QPointF(mid, tipY), QPointF(mid - 9, tipY + head));
            painter.drawLine(QPointF(mid, tipY), QPointF(mid + 9, tipY + head));
        };
        arrow(6, mid - 9);            // from the top, down to the bar
        arrow(size - 6, mid + 9);     // from the bottom, up to the bar
        painter.end();
        return QIcon(pixmap);
    }

    ChatDockWidget::ChatDockWidget(QWidget* parent)
        : QDockWidget(parent)
    {
        qtllmInitResources();  // register :/icons/* (needed when linked statically)

        // Default the user bubble label to the OS username (the consumer can
        // override with setUserName(), e.g. an application-specific account name,
        // or "" to hide the header). Falls back to "You" if unavailable.
        QByteArray osUser = qgetenv("USERNAME");
        if (osUser.isEmpty())
            osUser = qgetenv("USER");
        if (!osUser.isEmpty())
            m_userName = QString::fromLocal8Bit(osUser);

        setupUI();
    }

    ChatDockWidget::~ChatDockWidget()
    {
    }

    void ChatDockWidget::setupUI()
    {
        setWindowTitle("LLM Assistant");
        setMinimumWidth(300);

        m_centralWidget = new QWidget(this);
        QVBoxLayout* mainLayout = new QVBoxLayout(m_centralWidget);
        mainLayout->setContentsMargins(4, 4, 4, 4);
        mainLayout->setSpacing(4);

        m_scrollArea = new QScrollArea(m_centralWidget);
        m_scrollArea->setWidgetResizable(true);
        m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        QWidget* messagesContainer = new QWidget(m_scrollArea);
        m_messagesLayout = new QVBoxLayout(messagesContainer);
        m_messagesLayout->setContentsMargins(4, 4, 4, 4);
        m_messagesLayout->setSpacing(2);
        m_messagesLayout->addStretch();

        m_scrollArea->setWidget(messagesContainer);
        mainLayout->addWidget(m_scrollArea, 1);

        m_loadingLabel = new QLabel("Thinking...", m_centralWidget);
        m_loadingLabel->setAlignment(Qt::AlignCenter);
        m_loadingLabel->setStyleSheet("color: #888; font-style: italic; padding: 4px;");
        m_loadingLabel->setVisible(false);
        mainLayout->addWidget(m_loadingLabel);

        m_statusLabel = new QLabel(m_centralWidget);
        m_statusLabel->setAlignment(Qt::AlignLeft);
        m_statusLabel->setStyleSheet("color: #2980b9; font-size: 11px; padding: 2px 4px;");
        m_statusLabel->setVisible(false);
        mainLayout->addWidget(m_statusLabel);

        m_tokenLabel = new QLabel(m_centralWidget);
        m_tokenLabel->setAlignment(Qt::AlignRight);
        m_tokenLabel->setStyleSheet("color: #888; font-size: 10px; padding: 0px 4px;");
        m_tokenLabel->setVisible(false);
        mainLayout->addWidget(m_tokenLabel);

        QHBoxLayout* contextRow = new QHBoxLayout();
        contextRow->setSpacing(4);

        m_contextBar = new ContextUsageBar(m_centralWidget);
        contextRow->addWidget(m_contextBar, 1);

        m_compactButton = new QPushButton(m_centralWidget);
        m_compactButton->setIcon(compactIcon(palette().color(QPalette::ButtonText)));
        m_compactButton->setFixedWidth(28);
        m_compactButton->setObjectName("qtllm_compactButton");
        m_compactButton->setToolTip(
            "Compact conversation\n\n"
            "Replaces the conversation so far with a summary of it, which frees most of "
            "the context window while the assistant still knows what you were doing.\n\n"
            "Use this when:\n"
            "- the context bar is filling up and you want to keep working on the same task\n"
            "- replies are getting slow or expensive in a long conversation\n\n"
            "Details of earlier messages are lost; the gist is kept.");
        // Regenerate / edit: hidden until setRegenerateEnabled(true).
        m_regenerateButton = new QPushButton(QString::fromUtf8("\xe2\x86\xbb"), m_centralWidget);
        m_regenerateButton->setFixedWidth(28);
        m_regenerateButton->setObjectName("qtllm_regenerateButton");
        m_regenerateButton->setToolTip("Regenerate\n\nRemoves the last answer and asks the same question again.");
        m_regenerateButton->setVisible(false);
        contextRow->addWidget(m_regenerateButton);
        connect(m_regenerateButton, &QPushButton::clicked, this, &ChatDockWidget::onRegenerateClicked);

        m_editLastButton = new QPushButton(QString::fromUtf8("\xe2\x9c\x8e"), m_centralWidget);
        m_editLastButton->setFixedWidth(28);
        m_editLastButton->setObjectName("qtllm_editLastButton");
        m_editLastButton->setToolTip("Edit last message\n\nTakes your last message and its answer out of the "
                                     "conversation and puts the message back into the input field.");
        m_editLastButton->setVisible(false);
        contextRow->addWidget(m_editLastButton);
        connect(m_editLastButton, &QPushButton::clicked, this, &ChatDockWidget::onEditLastClicked);

        contextRow->addWidget(m_compactButton);
        connect(m_compactButton, &QPushButton::clicked, this, &ChatDockWidget::onCompactClicked);

        m_clearButton = new QPushButton(QString::fromUtf8("\xf0\x9f\x97\x91"), m_centralWidget);
        m_clearButton->setFixedWidth(28);
        m_clearButton->setToolTip(
            "Clear context\n\n"
            "Wipes the conversation history sent to the model on every request - "
            "the model \"forgets\" everything said so far (system prompt and tools stay).\n\n"
            "Use this when:\n"
            "- the context bar is filling up and responses are getting slow/expensive\n"
            "- the conversation has drifted off-topic and old messages are just noise\n"
            "- you're starting a genuinely new task and don't need the old thread\n\n"
            "Don't use this if you still need the model to recall earlier details in this chat.");
        contextRow->addWidget(m_clearButton);

        mainLayout->addLayout(contextRow);

        auto* inputField = new ChatInputTextEdit(m_centralWidget);
        inputField->setPlaceholderText("Type a message... (Enter = Send, Shift+Enter = New line)");
        inputField->setMaximumHeight(80);
        inputField->setAcceptRichText(false);
        inputField->onSubmit = [this]() { onSendClicked(); };
        inputField->onPictures = [this](const QMimeData* data) { return takePictures(data); };
        m_inputField = inputField;

        // Pictures waiting to go out with the next message.
        m_attachRow = new QWidget(m_centralWidget);
        m_attachRow->setObjectName("qtllm_attachRow");
        auto* attachLayout = new QHBoxLayout(m_attachRow);
        attachLayout->setContentsMargins(0, 0, 0, 0);
        attachLayout->setSpacing(4);
        m_attachRow->setVisible(false);

        QHBoxLayout* buttonLayout = new QHBoxLayout();
        buttonLayout->setSpacing(4);

        m_sendButton = new QPushButton("Send", m_centralWidget);
        buttonLayout->addWidget(m_sendButton);

        m_saveButton = new QPushButton(m_centralWidget);
        {
            // QIcon(":/missing") is NOT null (it just paints blank), so load via
            // QPixmap and check isNull() to reliably fall back to a style icon.
            QPixmap savePm(QStringLiteral(":/icons/floppy_disk.png"));
            QIcon saveIcon = savePm.isNull()
                ? style()->standardIcon(QStyle::SP_DialogSaveButton)
                : QIcon(savePm);
            m_saveButton->setIcon(saveIcon);
        }
        m_saveButton->setFixedWidth(32);
        m_saveButton->setToolTip(QString::fromUtf16(u"Konversation als JSON speichern"));

        m_attachButton = new QPushButton(QString::fromUtf8("\xf0\x9f\x93\x8e"), m_centralWidget);
        m_attachButton->setFixedWidth(32);
        m_attachButton->setObjectName("qtllm_attachButton");
        m_attachButton->setToolTip("Attach pictures (or paste / drop them into the message field)");
        m_attachButton->setVisible(false);
        buttonLayout->addWidget(m_attachButton);
        connect(m_attachButton, &QPushButton::clicked, this, &ChatDockWidget::onAttachClicked);

        m_loadButton = new QPushButton(m_centralWidget);
        m_loadButton->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
        m_loadButton->setFixedWidth(32);
        m_loadButton->setObjectName("qtllm_loadButton");
        m_loadButton->setToolTip("Continue a saved conversation");
        m_loadButton->setVisible(false);
        buttonLayout->addWidget(m_loadButton);
        connect(m_loadButton, &QPushButton::clicked, this, &ChatDockWidget::onLoadClicked);

        buttonLayout->addWidget(m_saveButton);

        m_settingsButton = new QPushButton(QString::fromUtf8("\xe2\x9a\x99"), m_centralWidget);
        m_settingsButton->setFixedWidth(32);
        m_settingsButton->setToolTip("Settings");
        buttonLayout->addWidget(m_settingsButton);

        m_cancelButton = new QPushButton("Cancel", m_centralWidget);
        m_cancelButton->setStyleSheet("background-color: #e74c3c; color: white; padding: 4px 12px; border-radius: 4px;");
        m_cancelButton->setVisible(false);

        mainLayout->addWidget(m_attachRow);
        mainLayout->addWidget(m_inputField);
        mainLayout->addLayout(buttonLayout);
        mainLayout->addWidget(m_cancelButton);

        setWidget(m_centralWidget);

        m_cancelTimer.setSingleShot(true);
        m_cancelTimer.setInterval(5000);

        connect(m_sendButton, &QPushButton::clicked, this, &ChatDockWidget::onSendClicked);
        // Stable names: how GUI tests and widget-addressing tools find these.
        m_inputField->setObjectName("qtllm_chatInput");
        m_sendButton->setObjectName("qtllm_sendButton");
        m_cancelButton->setObjectName("qtllm_cancelButton");
        m_saveButton->setObjectName("qtllm_saveButton");
        m_clearButton->setObjectName("qtllm_clearButton");
        m_settingsButton->setObjectName("qtllm_settingsButton");
        m_contextBar->setObjectName("qtllm_contextBar");
        m_statusLabel->setObjectName("qtllm_statusLabel");
        m_loadingLabel->setObjectName("qtllm_loadingLabel");

        connect(m_cancelButton, &QPushButton::clicked, this, &ChatDockWidget::onCancelClicked);
        connect(m_saveButton, &QPushButton::clicked, this, &ChatDockWidget::onSaveClicked);
        connect(m_clearButton, &QPushButton::clicked, this, &ChatDockWidget::onClearClicked);
        connect(m_settingsButton, &QPushButton::clicked, this, &ChatDockWidget::settingsRequested);
        connect(&m_cancelTimer, &QTimer::timeout, this, &ChatDockWidget::onCancelTimerTimeout);
        m_streamRenderTimer.setSingleShot(true);
        m_streamRenderTimer.setInterval(50);
        connect(&m_streamRenderTimer, &QTimer::timeout, this, &ChatDockWidget::renderStreamingBubble);

        connect(m_scrollArea->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
            if (m_programmaticScroll)
                return;
            QScrollBar* bar = m_scrollArea->verticalScrollBar();
            m_autoScroll = (value >= bar->maximum() - 10);
        });
    }

    void ChatDockWidget::scrollToBottom()
    {
        if (!m_autoScroll)
            return;
        QTimer::singleShot(0, this, [this]() {
            m_programmaticScroll = true;
            m_scrollArea->verticalScrollBar()->setValue(
                m_scrollArea->verticalScrollBar()->maximum());
            m_programmaticScroll = false;
        });
        QTimer::singleShot(50, this, [this]() {
            m_programmaticScroll = true;
            m_scrollArea->verticalScrollBar()->setValue(
                m_scrollArea->verticalScrollBar()->maximum());
            m_programmaticScroll = false;
        });
    }

    void ChatDockWidget::addUserMessage(const QString& message)
    {
        QWidget* bubble = createMessageBubble(message, true);
        m_messagesLayout->insertWidget(m_messagesLayout->count() - 1, bubble);
        m_userBubbles.append(bubble);
        scrollToBottom();
    }

    void ChatDockWidget::addAssistantMessage(const QString& message)
    {
        if (m_streamLabel) {
            // The streamed bubble becomes the answer: final text, no duplicate.
            m_streamText = message;
            finishStreamingBubble();
            return;
        }
        QWidget* bubble = createMessageBubble(message, false);
        m_messagesLayout->insertWidget(m_messagesLayout->count() - 1, bubble);
        scrollToBottom();
    }

    void ChatDockWidget::appendAssistantDelta(const QString& text)
    {
        if (text.isEmpty())
            return;
        if (!m_streamLabel) {
            QWidget* bubble = createMessageBubble(QString(), false);
            m_messagesLayout->insertWidget(m_messagesLayout->count() - 1, bubble);
            m_streamLabel = bubble->findChild<QLabel*>();
            m_streamLabel->setObjectName("qtllm_streamingBubble");
            m_streamText.clear();
        }
        m_streamText += text;
        // Re-rendering markdown for every few characters makes a long answer
        // quadratic; a short timer batches the deltas.
        if (!m_streamRenderTimer.isActive())
            m_streamRenderTimer.start();
    }

    void ChatDockWidget::renderStreamingBubble()
    {
        if (!m_streamLabel)
            return;
        m_streamLabel->setText(assistantHtml(m_streamText));
        scrollToBottom();
    }

    void ChatDockWidget::finishStreamingBubble()
    {
        m_streamRenderTimer.stop();
        if (!m_streamLabel)
            return;
        renderStreamingBubble();
        m_streamLabel->setObjectName(QString());
        m_streamLabel = nullptr;
        m_streamText.clear();
    }

    InterviewWidget* ChatDockWidget::addInterviewWidget(const QJsonObject& request)
    {
        InterviewWidget* card = new InterviewWidget(request);
        // Marker so bubble-width/font loops leave the card's internal labels alone
        card->setProperty("qtllm_interview", true);
        connect(card, &InterviewWidget::finished,
                this, &ChatDockWidget::interviewFinished);
        m_messagesLayout->insertWidget(m_messagesLayout->count() - 1, card);
        m_autoScroll = true;  // a question demands attention — always reveal it
        scrollToBottom();
        return card;
    }

    QJsonObject ChatDockWidget::execInterview(const QJsonObject& request)
    {
        QPointer<InterviewWidget> card(addInterviewWidget(request));

        // "Thinking..." would be misleading while we wait on the user.
        const bool loadingWasVisible = m_loadingLabel->isVisible();
        m_loadingLabel->setVisible(false);

        QJsonObject result{{"status", "cancelled"}};
        QEventLoop loop;
        connect(card, &InterviewWidget::finished, &loop,
                [&result, &loop](const QJsonObject& r) {
                    result = r;
                    loop.quit();
                });
        connect(card, &QObject::destroyed, &loop, &QEventLoop::quit);

        // The dock itself may be torn down while the loop spins (app shutdown);
        // guard against touching members through a dangling this afterwards.
        QPointer<ChatDockWidget> self(this);
        loop.exec();

        if (self)
            m_loadingLabel->setVisible(loadingWasVisible);
        return result;
    }

    void ChatDockWidget::setLoading(bool loading)
    {
        m_isLoading = loading;
        m_loadingLabel->setVisible(loading);
        // Compacting replaces the history; not while a turn is using it.
        m_compactButton->setEnabled(!loading);
        m_regenerateButton->setEnabled(!loading);
        m_editLastButton->setEnabled(!loading);
        m_loadButton->setEnabled(!loading);

        if (loading) {
            m_cancelTimer.start();
        } else {
            m_cancelTimer.stop();
            m_cancelButton->setVisible(false);
            clearStatus();
        }
    }

    void ChatDockWidget::setStatusText(const QString& text)
    {
        m_statusLabel->setText(text);
        m_statusLabel->setVisible(true);
    }

    void ChatDockWidget::setFontSizePercent(int percent)
    {
        m_fontSizePercent = percent;
        double scale = percent / 100.0;
        int basePt = 9;
        int scaledPt = static_cast<int>(basePt * scale);
        if (scaledPt < 6) scaledPt = 6;

        for (int i = 0; i < m_messagesLayout->count(); ++i)
        {
            QLayoutItem* item = m_messagesLayout->itemAt(i);
            if (!item || !item->widget())
                continue;
            if (item->widget()->property("qtllm_interview").toBool())
                continue;  // interview cards manage their own labels
            QLabel* label = item->widget()->findChild<QLabel*>();
            if (label) {
                QFont f = label->font();
                f.setPointSize(scaledPt);
                label->setFont(f);
            }
        }

        QFont inputFont = m_inputField->font();
        inputFont.setPointSize(scaledPt);
        m_inputField->setFont(inputFont);
    }

    void ChatDockWidget::updateTokenUsage(int inputTokens, int outputTokens)
    {
        m_tokenLabel->setText(QString::fromUtf8("\xe2\x86\x91 %1  \xe2\x86\x93 %2")
                              .arg(inputTokens).arg(outputTokens));
        m_tokenLabel->setVisible(true);
    }

    void ChatDockWidget::setContextBreakdown(const ContextBreakdown& breakdown)
    {
        m_contextBar->setBreakdown(breakdown);
    }

    void ChatDockWidget::clearStatus()
    {
        m_statusLabel->clear();
        m_statusLabel->setVisible(false);
    }

    void ChatDockWidget::clearMessages()
    {
        while (m_messagesLayout->count() > 1)
        {
            QLayoutItem* item = m_messagesLayout->takeAt(0);
            if (item->widget())
                delete item->widget();
            delete item;
        }
        m_pendingToolCards.clear();
        m_userBubbles.clear();
    }

    void ChatDockWidget::setAssistantName(const QString& name)
    {
        m_assistantName = name;
    }

    QString ChatDockWidget::assistantName() const
    {
        return m_assistantName;
    }

    void ChatDockWidget::setUserName(const QString& name)
    {
        m_userName = name;
    }

    QString ChatDockWidget::userName() const
    {
        return m_userName;
    }

    void ChatDockWidget::setClient(Client* client)
    {
        if (m_toolStatusConn)
            disconnect(m_toolStatusConn);
        if (m_toolDebugInvokedConn)
            disconnect(m_toolDebugInvokedConn);
        if (m_toolDebugCompletedConn)
            disconnect(m_toolDebugCompletedConn);
        m_pendingToolCards.clear();
        m_client = client;
        if (!m_client)
            return;

        // Tools carrying a statusText (Tool::setStatusText) get it shown here
        // automatically while they execute — no app wiring needed.
        m_toolStatusConn = connect(m_client, &Client::toolInvoked, this,
            [this](const QString& toolName, const QJsonObject&) {
                for (const Tool& tool : m_client->registeredTools()) {
                    if (tool.name() == toolName) {
                        if (!tool.statusText().isEmpty())
                            setStatusText(tool.statusText());
                        return;
                    }
                }
            });

        // Debug tool-call cards. Always connected; the flag gates rendering so
        // toggling the setting needs no re-wiring.
        m_toolDebugInvokedConn = connect(m_client, &Client::toolInvoked, this,
            [this](const QString& toolName, const QJsonObject& input) {
                if (m_showToolCalls)
                    addToolCallCard(toolName, input);
            });
        m_toolDebugCompletedConn = connect(m_client, &Client::toolCompleted, this,
            [this](const QString& toolName, const QJsonObject& result) {
                if (m_showToolCalls)
                    completeToolCallCard(toolName, result);
            });

        connect(m_client, &Client::toolProgress, this,
                [this](const QString&, const QString& statusText) { setStatusText(statusText); });
        connect(m_client, &Client::compactionStarted, this, [this]() {
            m_compactButton->setEnabled(false);
            setStatusText("Compacting conversation...");
        });
        connect(m_client, &Client::compactionFinished, this, [this](int before, int after) {
            m_compactButton->setEnabled(!m_isLoading);
            clearStatus();
            addContextMarker(QString::fromUtf8("Context compacted \xc2\xb7 %1 \xe2\x86\x92 %2 tokens")
                                 .arg(humanTokenCount(before), humanTokenCount(after)));
        });
        connect(m_client, &Client::compactionFailed, this, [this](const QString& message) {
            m_compactButton->setEnabled(!m_isLoading);
            setStatusText("Compaction failed: " + message);
        });
        connect(m_client, &Client::responseDelta, this, &ChatDockWidget::appendAssistantDelta);
        // A new request inside the same turn (after a tool call): what follows
        // is a new paragraph of the same answer.
        connect(m_client, &Client::requestStarted, this, [this]() {
            if (m_streamLabel && !m_streamText.isEmpty() && !m_streamText.endsWith("\n\n"))
                m_streamText += "\n\n";
        });
        // A turn that ends without responseReady() (error) leaves the partial
        // answer as it is.
        connect(m_client, &Client::errorOccurred, this, [this]() { finishStreamingBubble(); });
        connect(m_client, &Client::contextChanged, this, [this]() {
            if (m_client)
                setContextBreakdown(m_client->contextBreakdown());
        });
        setContextBreakdown(m_client->contextBreakdown());
    }

    void ChatDockWidget::setShowToolCalls(bool show)
    {
        m_showToolCalls = show;
    }

    bool ChatDockWidget::showToolCalls() const
    {
        return m_showToolCalls;
    }

    static QString jsonBlock(const QJsonObject& obj)
    {
        return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Indented))
            .toHtmlEscaped();
    }

    void ChatDockWidget::addToolCallCard(const QString& toolName, const QJsonObject& input)
    {
        QString description;
        if (m_client) {
            for (const Tool& tool : m_client->registeredTools()) {
                if (tool.name() == toolName) {
                    description = tool.description();
                    break;
                }
            }
        }

        QWidget* card = new QWidget();
        QVBoxLayout* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(4, 2, 4, 2);
        cardLayout->setSpacing(0);

        QToolButton* header = new QToolButton(card);
        header->setCheckable(true);
        header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        header->setArrowType(Qt::RightArrow);
        header->setText(QStringLiteral("tool: %1").arg(toolName));
        header->setStyleSheet("QToolButton { border: none; color: #666; }");

        QLabel* details = new QLabel(card);
        details->setTextFormat(Qt::RichText);
        details->setWordWrap(true);
        details->setTextInteractionFlags(Qt::TextSelectableByMouse);
        details->setVisible(false);
        details->setStyleSheet(
            "background-color: #F4F4F4; border-left: 3px solid #BBB; padding: 6px; color: #333;");
        details->setProperty("toolName", toolName);
        details->setProperty("toolDescription", description);
        details->setProperty("toolInput", QString::fromUtf8(
            QJsonDocument(input).toJson(QJsonDocument::Compact)));
        details->setText(QString("<b>%1</b><br/>%2<pre>%3</pre><i>running...</i>")
            .arg(toolName.toHtmlEscaped(), description.toHtmlEscaped(), jsonBlock(input)));

        connect(header, &QToolButton::toggled, details, [header, details](bool on) {
            details->setVisible(on);
            header->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        });

        cardLayout->addWidget(header, 0, Qt::AlignLeft);
        cardLayout->addWidget(details);

        m_messagesLayout->insertWidget(m_messagesLayout->count() - 1, card);
        m_pendingToolCards[toolName].append(QPointer<QLabel>(details));
        scrollToBottom();
    }

    void ChatDockWidget::completeToolCallCard(const QString& toolName, const QJsonObject& result)
    {
        auto it = m_pendingToolCards.find(toolName);
        if (it == m_pendingToolCards.end())
            return;
        while (!it->isEmpty()) {
            QPointer<QLabel> details = it->takeFirst();
            if (!details)
                continue;   // card was destroyed (e.g. clearMessages)
            details->setText(QString("<b>%1</b><br/>%2<pre>%3</pre><b>Result</b><pre>%4</pre>")
                .arg(toolName.toHtmlEscaped(),
                     details->property("toolDescription").toString().toHtmlEscaped(),
                     jsonBlock(QJsonDocument::fromJson(
                         details->property("toolInput").toString().toUtf8()).object()),
                     jsonBlock(result)));
            break;
        }
        if (it->isEmpty())
            m_pendingToolCards.erase(it);
    }

    void ChatDockWidget::onSaveClicked()
    {
        // No client bound: let the app handle saving via the signal.
        if (!m_client) {
            emit saveConversationRequested();
            return;
        }

        const QString defName = QStringLiteral("conversation_%1.json")
            .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
        const QString path = QFileDialog::getSaveFileName(
            this, QString::fromUtf16(u"Konversation speichern"),
            defName, QStringLiteral("JSON (*.json)"));
        if (path.isEmpty())
            return;

        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            setStatusText(QString::fromUtf16(u"Speichern fehlgeschlagen."));
            return;
        }
        f.write(QJsonDocument(m_client->exportConversation()).toJson(QJsonDocument::Indented));
        f.close();
        setStatusText(QString::fromUtf16(u"Konversation gespeichert: ") + path);
    }

    void ChatDockWidget::setSendButtonText(const QString& text)
    {
        m_sendButton->setText(text);
    }

    void ChatDockWidget::setCancelButtonText(const QString& text)
    {
        m_cancelButton->setText(text);
    }

    void ChatDockWidget::setSettingsButtonTooltip(const QString& text)
    {
        m_settingsButton->setToolTip(text);
    }

    void ChatDockWidget::setInputPlaceholderText(const QString& text)
    {
        m_inputField->setPlaceholderText(text);
    }

    void ChatDockWidget::setLoadingText(const QString& text)
    {
        m_loadingLabel->setText(text);
    }

    void ChatDockWidget::setCancelledText(const QString& text)
    {
        m_cancelledText = text;
    }

    void ChatDockWidget::setBusyWarningText(const QString& text)
    {
        m_busyWarningText = text;
    }

    // --- Slots ---

    void ChatDockWidget::onSendClicked()
    {
        submitPrompt(m_inputField->toPlainText());
    }

    bool ChatDockWidget::submitPrompt(const QString& text)
    {
        QString trimmed = text.trimmed();
        if (trimmed.isEmpty() && m_pendingImages.isEmpty())
            return false;

        if (m_isLoading) {
            m_inputField->setToolTip(m_busyWarningText);
            QToolTip::showText(m_inputField->mapToGlobal(QPoint(0, -30)),
                m_busyWarningText, m_inputField, QRect(), 2000);
            return false;
        }

        QString shown = trimmed;
        if (!m_pendingImages.isEmpty()) {
            // Applications send what messageSent() carries and tend to skip
            // an empty text, so pictures alone still need words.
            if (trimmed.isEmpty())
                trimmed = shown = QStringLiteral("See the attached picture.");
            shown += QStringLiteral("\n[%1 picture(s) attached]").arg(m_pendingImages.size());
            if (m_client)
                m_client->attachImages(m_pendingImages);
            else
                emit imagesSubmitted(m_pendingImages);
            m_pendingImages.clear();
            updateAttachRow();
        }

        addUserMessage(shown);
        emit messageSent(trimmed);
        m_inputField->clear();
        return true;
    }

    // ------------------------------------------------------------ pictures

    void ChatDockWidget::setImageInputEnabled(bool enabled)
    {
        m_imageInput = enabled;
        m_attachButton->setVisible(enabled);
        if (!enabled) {
            m_pendingImages.clear();
            updateAttachRow();
        }
    }

    void ChatDockWidget::attachImages(const QList<QImage>& images)
    {
        if (m_client && !m_client->supportsImageInput()) {
            setStatusText("The current provider cannot take pictures.");
            return;
        }
        for (const QImage& image : images) {
            // ponytail: at most 8 per message; more is rarely what anyone wants
            // and every picture costs tokens.
            if (!image.isNull() && m_pendingImages.size() < 8)
                m_pendingImages.append(image);
        }
        updateAttachRow();
    }

    bool ChatDockWidget::takePictures(const QMimeData* data)
    {
        if (!m_imageInput)
            return false;
        QList<QImage> images;
        if (data->hasImage()) {
            images.append(qvariant_cast<QImage>(data->imageData()));
        } else {
            for (const QUrl& url : data->urls()) {
                const QImage image(url.toLocalFile());   // null for anything that is not a picture
                if (!image.isNull())
                    images.append(image);
            }
        }
        if (images.isEmpty())
            return false;
        attachImages(images);
        return true;
    }

    void ChatDockWidget::onAttachClicked()
    {
        const QStringList paths = QFileDialog::getOpenFileNames(
            this, "Attach pictures", QString(), "Pictures (*.png *.jpg *.jpeg *.bmp *.gif *.webp)");
        QList<QImage> images;
        for (const QString& path : paths)
            images.append(QImage(path));
        attachImages(images);
    }

    void ChatDockWidget::updateAttachRow()
    {
        QLayout* layout = m_attachRow->layout();
        while (QLayoutItem* item = layout->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        for (const QImage& image : m_pendingImages) {
            auto* thumb = new QLabel(m_attachRow);
            thumb->setPixmap(QPixmap::fromImage(image.scaledToHeight(40, Qt::SmoothTransformation)));
            thumb->setToolTip(QString("%1 x %2").arg(image.width()).arg(image.height()));
            layout->addWidget(thumb);
        }
        auto* remove = new QToolButton(m_attachRow);
        remove->setText(QString::fromUtf8("\xc3\x97"));
        remove->setObjectName("qtllm_attachClear");
        remove->setToolTip("Remove the attached pictures");
        connect(remove, &QToolButton::clicked, this, [this]() {
            m_pendingImages.clear();
            // Not under the button that is being clicked.
            QTimer::singleShot(0, this, [this]() { updateAttachRow(); });
        });
        layout->addWidget(remove);
        static_cast<QHBoxLayout*>(layout)->addStretch();
        m_attachRow->setVisible(!m_pendingImages.isEmpty());
    }

    // ------------------------------------------------------------ regenerate, edit, load

    void ChatDockWidget::setRegenerateEnabled(bool enabled)
    {
        m_regenerate = enabled;
        m_regenerateButton->setVisible(enabled);
        m_editLastButton->setVisible(enabled);
    }

    bool ChatDockWidget::removeLastExchangeBubbles()
    {
        while (!m_userBubbles.isEmpty() && !m_userBubbles.last())
            m_userBubbles.removeLast();
        if (m_userBubbles.isEmpty())
            return false;
        const int from = m_messagesLayout->indexOf(m_userBubbles.takeLast());
        if (from < 0)
            return false;
        while (m_messagesLayout->count() - 1 > from) {   // the last item is the stretch
            QLayoutItem* item = m_messagesLayout->takeAt(from);
            delete item->widget();
            delete item;
        }
        m_pendingToolCards.clear();
        return true;
    }

    QString ChatDockWidget::takeLastExchange()
    {
        if (!m_client || m_isLoading)
            return QString();
        // ponytail: assumes the last exchange in the conversation is the last
        // one typed here. A message the application injected without a bubble
        // (Client::sendToolMessage) in between breaks that pairing.
        const QString prompt = m_client->removeLastExchange();
        if (prompt.isEmpty()) {
            setStatusText("There is no message to take back.");
            return QString();
        }
        removeLastExchangeBubbles();
        return prompt;
    }

    void ChatDockWidget::onRegenerateClicked()
    {
        const QString prompt = takeLastExchange();
        if (!prompt.isEmpty())
            submitPrompt(prompt);
    }

    void ChatDockWidget::onEditLastClicked()
    {
        const QString prompt = takeLastExchange();
        if (prompt.isEmpty())
            return;
        setInputText(prompt);
        m_inputField->setFocus();
    }

    void ChatDockWidget::setLoadButtonVisible(bool visible)
    {
        m_loadButton->setVisible(visible);
    }

    bool ChatDockWidget::loadConversation(const QJsonObject& exported)
    {
        if (!m_client || !m_client->importConversation(exported))
            return false;
        clearMessages();
        for (const QJsonValue& value : m_client->conversationHistory()) {
            const QJsonObject message = value.toObject();
            if (message["role"].toString() == QLatin1String("user"))
                addUserMessage(message["content"].toString());
            else
                addAssistantMessage(message["content"].toString());
        }
        addContextMarker("Conversation loaded");
        return true;
    }

    void ChatDockWidget::onLoadClicked()
    {
        if (!m_client)
            return;
        const QString path = QFileDialog::getOpenFileName(
            this, "Continue a saved conversation", QString(), "JSON (*.json)");
        if (path.isEmpty())
            return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            setStatusText("Could not open " + path);
            return;
        }
        const QJsonObject exported = QJsonDocument::fromJson(file.readAll()).object();
        if (!loadConversation(exported))
            setStatusText(QString("This conversation cannot be continued here - it was saved with "
                                  "the provider \"%1\".").arg(exported["provider"].toString()));
    }

    void ChatDockWidget::setInputText(const QString& text)
    {
        m_inputField->setPlainText(text);
    }

    QString ChatDockWidget::inputText() const
    {
        return m_inputField->toPlainText();
    }

    void ChatDockWidget::onCancelClicked()
    {
        m_cancelButton->setVisible(false);
        m_cancelTimer.stop();
        finishStreamingBubble();   // keep what already arrived; the notice follows it
        setLoading(false);
        addAssistantMessage(m_cancelledText);
        // Stop the turn itself, not just the spinner: otherwise the request
        // keeps running and its tool calls and answer still arrive.
        if (m_client)
            m_client->cancelRequest();
        emit cancelRequested();
    }

    void ChatDockWidget::onCancelTimerTimeout()
    {
        m_cancelButton->setVisible(true);
    }

    void ChatDockWidget::onClearClicked()
    {
        if (m_client)
            m_client->clearConversation();
        addContextClearedMarker();
        emit clearContextRequested();
    }

    void ChatDockWidget::onCompactClicked()
    {
        if (!m_client) {
            emit compactRequested();
            return;
        }
        if (!m_client->compactConversation())
            setStatusText("Nothing to compact right now.");
    }

    void ChatDockWidget::addContextClearedMarker()
    {
        addContextMarker(QString::fromUtf16(u"Context cleared"));
    }

    void ChatDockWidget::addContextMarker(const QString& text)
    {
        QWidget* marker = new QWidget();
        marker->setProperty("qtllm_interview", true);  // opt out of bubble font/width auto-sizing
        QHBoxLayout* layout = new QHBoxLayout(marker);
        layout->setContentsMargins(0, 8, 0, 8);

        QFrame* line1 = new QFrame(marker);
        line1->setFrameShape(QFrame::HLine);
        line1->setStyleSheet("color: #bbb;");
        QLabel* label = new QLabel(text, marker);
        label->setStyleSheet("color: #999; font-size: 10px; padding: 0 6px;");
        QFrame* line2 = new QFrame(marker);
        line2->setFrameShape(QFrame::HLine);
        line2->setStyleSheet("color: #bbb;");

        layout->addWidget(line1, 1);
        layout->addWidget(label);
        layout->addWidget(line2, 1);

        m_messagesLayout->insertWidget(m_messagesLayout->count() - 1, marker);
        scrollToBottom();
    }

    void ChatDockWidget::resizeEvent(QResizeEvent* event)
    {
        QDockWidget::resizeEvent(event);
        updateBubbleWidths();
    }

    void ChatDockWidget::updateBubbleWidths()
    {
        if (!m_scrollArea)
            return;
        int availableWidth = m_scrollArea->viewport()->width() - 20;
        if (availableWidth < 100)
            availableWidth = 100;

        for (int i = 0; i < m_messagesLayout->count(); ++i)
        {
            QLayoutItem* item = m_messagesLayout->itemAt(i);
            if (!item || !item->widget())
                continue;
            if (item->widget()->property("qtllm_interview").toBool())
                continue;  // interview cards manage their own labels
            QLabel* label = item->widget()->findChild<QLabel*>();
            if (label)
                label->setMaximumWidth(availableWidth);
        }
    }

    static QString inlineMarkdownToHtml(const QString& text)
    {
        QString result = text;

        // Replace links with \x01<n>\x02 placeholders before bold/italic/code,
        // because those regexes would mangle * and ` inside the generated href.
        // Supports one level of nested [] in link text: [SwOption[1258]](url)
        QRegularExpression linkRx("\\[([^\\[\\]]*(?:\\[[^\\[\\]]*\\][^\\[\\]]*)*)\\]\\(([^)]+)\\)");
        QStringList linkFragments;
        QRegularExpressionMatchIterator it = linkRx.globalMatch(result);
        int offset = 0;
        while (it.hasNext()) {
            QRegularExpressionMatch m = it.next();
            QString linkText = m.captured(1).toHtmlEscaped();
            QString href = m.captured(2).toHtmlEscaped();
            linkFragments.append(QString("<a href=\"%1\">%2</a>").arg(href, linkText));
            QString placeholder = QString("\x01%1\x02").arg(linkFragments.size() - 1);
            result.replace(m.capturedStart() + offset, m.capturedLength(), placeholder);
            offset += placeholder.length() - m.capturedLength();
        }

        QRegularExpression boldRx("\\*\\*(.+?)\\*\\*");
        result.replace(boldRx, "<b>\\1</b>");
        QRegularExpression italicRx("(?<!\\*)\\*(?!\\*)(.+?)(?<!\\*)\\*(?!\\*)");
        result.replace(italicRx, "<i>\\1</i>");
        QRegularExpression codeRx("`(.+?)`");
        result.replace(codeRx, "<code style=\"background-color: #d0d0d0; padding: 1px 3px;\">\\1</code>");

        for (int i = 0; i < linkFragments.size(); ++i)
            result.replace(QString("\x01%1\x02").arg(i), linkFragments[i]);

        return result;
    }

    static QString convertMarkdownTables(const QString& markdown)
    {
        QStringList lines = markdown.split('\n');
        QString result;
        int i = 0;
        while (i < lines.size()) {
            if (i + 1 < lines.size() &&
                lines[i].contains('|') &&
                lines[i + 1].contains('|') &&
                lines[i + 1].contains('-')) {

                QStringList headers;
                for (const QString& cell : lines[i].split('|')) {
                    QString trimmed = cell.trimmed();
                    if (!trimmed.isEmpty())
                        headers.append(trimmed);
                }

                i += 2;

                result += "<table border=\"1\" cellpadding=\"4\" cellspacing=\"0\" "
                          "style=\"border-collapse: collapse; border-color: #aaa;\">\n<tr>";
                for (const QString& h : headers)
                    result += "<th style=\"background-color: #d0d0d0; padding: 4px 8px;\">" + inlineMarkdownToHtml(h) + "</th>";
                result += "</tr>\n";

                while (i < lines.size() && lines[i].contains('|')) {
                    QStringList cells;
                    for (const QString& cell : lines[i].split('|')) {
                        QString trimmed = cell.trimmed();
                        if (!trimmed.isEmpty())
                            cells.append(trimmed);
                    }
                    result += "<tr>";
                    for (const QString& c : cells)
                        result += "<td style=\"padding: 4px 8px;\">" + inlineMarkdownToHtml(c) + "</td>";
                    for (int j = cells.size(); j < headers.size(); ++j)
                        result += "<td></td>";
                    result += "</tr>\n";
                    ++i;
                }
                result += "</table>\n\n";
            } else {
                result += lines[i] + '\n';
                ++i;
            }
        }
        return result;
    }

    static QString markdownToHtml(const QString& markdown)
    {
        QString preprocessed = convertMarkdownTables(markdown);
        QTextDocument doc;
        doc.setMarkdown(preprocessed);
        return doc.toHtml();
    }

    // Appends a colored rectangle after every 6-digit hex color code mentioned
    // in assistant text, so colors are visible instead of just numeric.
    // Runs on the final HTML; the (?![^<]*>) lookahead skips matches inside
    // tag attributes (there '>' follows before any '<', in text nodes '>' is
    // always escaped as &gt;).
    static QString decorateColorSwatches(const QString& html)
    {
        static const QRegularExpression rx(
            "#([0-9a-fA-F]{6})\\b(?![^<]*>)");

        QString result;
        int last = 0;
        QRegularExpressionMatchIterator it = rx.globalMatch(html);
        while (it.hasNext()) {
            QRegularExpressionMatch m = it.next();
            result += html.mid(last, m.capturedEnd() - last);
            result += QString("<span style=\"color:#%1;\">&nbsp;&#9608;&#9608;&#9608;</span>")
                          .arg(m.captured(1));
            last = m.capturedEnd();
        }
        result += html.mid(last);
        return result;
    }

    QString ChatDockWidget::assistantHtml(const QString& text) const
    {
        int scaledPt = static_cast<int>(9 * m_fontSizePercent / 100.0);
        if (scaledPt < 6) scaledPt = 6;
        QString nameHtml;
        if (!m_assistantName.isEmpty()) {
            nameHtml = QString("<span style='font-weight: bold; font-size: %1pt;'>%2</span><br/>")
                .arg(qMax(scaledPt - 1, 6))
                .arg(m_assistantName.toHtmlEscaped());
        }
        return nameHtml + decorateColorSwatches(markdownToHtml(text));
    }

    QWidget* ChatDockWidget::createMessageBubble(const QString& text, bool isUser)
    {
        QWidget* container = new QWidget();
        QHBoxLayout* layout = new QHBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);

        QLabel* label = new QLabel();
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
        label->setOpenExternalLinks(true);
        label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        label->setTextFormat(Qt::RichText);

        int availableWidth = m_scrollArea->viewport()->width() - 20;
        if (availableWidth < 100)
            availableWidth = 100;
        label->setMaximumWidth(availableWidth);

        int scaledPt = static_cast<int>(9 * m_fontSizePercent / 100.0);
        if (scaledPt < 6) scaledPt = 6;
        QFont f = label->font();
        f.setPointSize(scaledPt);
        label->setFont(f);

        // Per-side name header. Empty name hides the header entirely on that side.
        const QString name = isUser ? m_userName : m_assistantName;
        QString nameHtml;
        if (!name.isEmpty()) {
            // Explicit <br/> — a block <div> does not reliably line-break before
            // inline body text in QLabel's rich-text subset (the user's plain text
            // ran onto the same line as the name).
            nameHtml = QString("<span style='font-weight: bold; font-size: %1pt;'>%2</span><br/>")
                .arg(qMax(scaledPt - 1, 6))
                .arg(name.toHtmlEscaped());
        }

        if (isUser) {
            label->setText(nameHtml + text.toHtmlEscaped().replace("\n", "<br>"));
            label->setStyleSheet(
                "background-color: #DCF8C6; color: #1a1a1a; border-radius: 8px; padding: 8px; margin: 4px;");
        } else {
            label->setText(assistantHtml(text));
            label->setStyleSheet(
                "background-color: #E8E8E8; color: #1a1a1a; border-radius: 8px; padding: 8px; margin: 4px;");
        }

        if (isUser) {
            layout->addStretch();
            layout->addWidget(label);
        } else {
            layout->addWidget(label);
            layout->addStretch();
        }

        return container;
    }
}
