#include "ChatDockWidget.h"
#include <QScrollBar>
#include <QKeyEvent>
#include <QTextDocument>
#include <QToolTip>
#include <QRegularExpression>
#include <QStyle>
#include <QPixmap>
#include <QFileDialog>
#include <QJsonDocument>
#include <QFile>
#include <QDateTime>
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

    protected:
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

        auto* inputField = new ChatInputTextEdit(m_centralWidget);
        inputField->setPlaceholderText("Type a message... (Enter = Send, Shift+Enter = New line)");
        inputField->setMaximumHeight(80);
        inputField->setAcceptRichText(false);
        inputField->onSubmit = [this]() { onSendClicked(); };
        m_inputField = inputField;

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
        buttonLayout->addWidget(m_saveButton);

        m_settingsButton = new QPushButton(QString::fromUtf8("\xe2\x9a\x99"), m_centralWidget);
        m_settingsButton->setFixedWidth(32);
        m_settingsButton->setToolTip("Settings");
        buttonLayout->addWidget(m_settingsButton);

        m_cancelButton = new QPushButton("Cancel", m_centralWidget);
        m_cancelButton->setStyleSheet("background-color: #e74c3c; color: white; padding: 4px 12px; border-radius: 4px;");
        m_cancelButton->setVisible(false);

        mainLayout->addWidget(m_inputField);
        mainLayout->addLayout(buttonLayout);
        mainLayout->addWidget(m_cancelButton);

        setWidget(m_centralWidget);

        m_cancelTimer.setSingleShot(true);
        m_cancelTimer.setInterval(5000);

        connect(m_sendButton, &QPushButton::clicked, this, &ChatDockWidget::onSendClicked);
        connect(m_cancelButton, &QPushButton::clicked, this, &ChatDockWidget::onCancelClicked);
        connect(m_saveButton, &QPushButton::clicked, this, &ChatDockWidget::onSaveClicked);
        connect(m_settingsButton, &QPushButton::clicked, this, &ChatDockWidget::settingsRequested);
        connect(&m_cancelTimer, &QTimer::timeout, this, &ChatDockWidget::onCancelTimerTimeout);

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
        scrollToBottom();
    }

    void ChatDockWidget::addAssistantMessage(const QString& message)
    {
        QWidget* bubble = createMessageBubble(message, false);
        m_messagesLayout->insertWidget(m_messagesLayout->count() - 1, bubble);
        scrollToBottom();
    }

    void ChatDockWidget::setLoading(bool loading)
    {
        m_isLoading = loading;
        m_loadingLabel->setVisible(loading);

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
        m_client = client;
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
        QString text = m_inputField->toPlainText().trimmed();
        if (text.isEmpty())
            return;

        if (m_isLoading) {
            m_inputField->setToolTip(m_busyWarningText);
            QToolTip::showText(m_inputField->mapToGlobal(QPoint(0, -30)),
                m_busyWarningText, m_inputField, QRect(), 2000);
            return;
        }

        addUserMessage(text);
        emit messageSent(text);
        m_inputField->clear();
    }

    void ChatDockWidget::onCancelClicked()
    {
        m_cancelButton->setVisible(false);
        m_cancelTimer.stop();
        setLoading(false);
        addAssistantMessage(m_cancelledText);
        emit cancelRequested();
    }

    void ChatDockWidget::onCancelTimerTimeout()
    {
        m_cancelButton->setVisible(true);
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
            QLabel* label = item->widget()->findChild<QLabel*>();
            if (label)
                label->setMaximumWidth(availableWidth);
        }
    }

    static QString inlineMarkdownToHtml(const QString& text)
    {
        QString result = text;
        QRegularExpression boldRx("\\*\\*(.+?)\\*\\*");
        result.replace(boldRx, "<b>\\1</b>");
        QRegularExpression italicRx("(?<!\\*)\\*(?!\\*)(.+?)(?<!\\*)\\*(?!\\*)");
        result.replace(italicRx, "<i>\\1</i>");
        QRegularExpression codeRx("`(.+?)`");
        result.replace(codeRx, "<code style=\"background-color: #d0d0d0; padding: 1px 3px;\">\\1</code>");
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
                "background-color: #DCF8C6; border-radius: 8px; padding: 8px; margin: 4px;");
        } else {
            label->setText(nameHtml + markdownToHtml(text));
            label->setStyleSheet(
                "background-color: #E8E8E8; border-radius: 8px; padding: 8px; margin: 4px;");
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
