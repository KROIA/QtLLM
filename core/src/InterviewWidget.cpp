#include "InterviewWidget.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QCheckBox>
#include <QButtonGroup>
#include <QLineEdit>
#include <QPushButton>
#include <QJsonArray>

namespace QtLLM
{

namespace
{
// The card has a fixed light background. Indicators drawn by the application
// style (dark themes, host style sheets) can be invisible on it, so the
// option buttons draw their own.
const char* const kOptionButtonStyle =
    "QRadioButton, QCheckBox { background: transparent; border: none; color: #1a1a1a; spacing: 6px; }"
    "QRadioButton:disabled, QCheckBox:disabled { color: #808080; }"
    "QRadioButton::indicator, QCheckBox::indicator {"
    "  width: 12px; height: 12px; background-color: white; border: 1px solid #4a6f96; }"
    "QRadioButton::indicator { border-radius: 7px; }"
    "QCheckBox::indicator { border-radius: 2px; }"
    "QRadioButton::indicator:hover, QCheckBox::indicator:hover { border-color: #2980b9; background-color: #e8f2fc; }"
    "QRadioButton::indicator:checked {"
    "  background-color: qradialgradient(cx:0.5, cy:0.5, radius:0.5, fx:0.5, fy:0.5,"
    "    stop:0 #2980b9, stop:0.5 #2980b9, stop:0.6 white, stop:1 white);"
    "  border-color: #2980b9; }"
    "QCheckBox::indicator:checked { background-color: #2980b9; border-color: #1f6391; }"
    "QRadioButton::indicator:disabled, QCheckBox::indicator:disabled { border-color: #aaaaaa; background-color: #f4f4f4; }"
    "QRadioButton::indicator:checked:disabled {"
    "  background-color: qradialgradient(cx:0.5, cy:0.5, radius:0.5, fx:0.5, fy:0.5,"
    "    stop:0 #8a8a8a, stop:0.5 #8a8a8a, stop:0.6 #f4f4f4, stop:1 #f4f4f4); }"
    "QCheckBox::indicator:checked:disabled { background-color: #8a8a8a; }";
}

InterviewWidget::InterviewWidget(const QJsonObject& request, QWidget* parent)
    : QFrame(parent)
{
    setFrameShape(QFrame::NoFrame);
    // The card has a fixed light background, so its text must not follow a
    // dark application palette.
    QPalette cardPalette = palette();
    for (QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        cardPalette.setColor(role, QColor(0x1a, 0x1a, 0x1a));
        cardPalette.setColor(QPalette::Disabled, role, QColor(0x80, 0x80, 0x80));
    }
    // Controls on it (tour buttons, check boxes, line edits) stay light as well.
    cardPalette.setColor(QPalette::Button, QColor(0xe1, 0xe1, 0xe1));
    cardPalette.setColor(QPalette::Base, Qt::white);
    cardPalette.setColor(QPalette::Window, QColor(0xf0, 0xf0, 0xf0));
    setPalette(cardPalette);
    setStyleSheet(
        "QtLLM--InterviewWidget {"
        "  background-color: #F0F6FF;"
        "  border: 1px solid #7BA7D7;"
        "  border-radius: 8px;"
        "}");

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6);

    const QJsonArray questions = request["questions"].toArray();
    for (const QJsonValue& val : questions)
        buildQuestion(val.toObject(), layout);

    // Degenerate request (no questions) — still show buttons so the tool
    // handler's event loop can always be released by the user.
    QHBoxLayout* buttonRow = new QHBoxLayout();
    buttonRow->addStretch();

    m_skipButton = new QPushButton(tr("Skip"), this);
    m_skipButton->setStyleSheet(
        "QPushButton { background-color: transparent; color: #666;"
        "  border: 1px solid #aaa; border-radius: 4px; padding: 4px 12px; }"
        "QPushButton:hover { background-color: #e4e4e4; }");
    buttonRow->addWidget(m_skipButton);

    m_submitButton = new QPushButton(tr("Submit"), this);
    m_submitButton->setStyleSheet(
        "QPushButton { background-color: #2980b9; color: white;"
        "  border-radius: 4px; padding: 4px 16px; }"
        "QPushButton:hover:enabled { background-color: #3498db; }"
        "QPushButton:disabled { background-color: #b0c4d4; }");
    buttonRow->addWidget(m_submitButton);
    layout->addLayout(buttonRow);

    connect(m_submitButton, &QPushButton::clicked, this, &InterviewWidget::onSubmitClicked);
    connect(m_skipButton,   &QPushButton::clicked, this, &InterviewWidget::onSkipClicked);

    updateSubmitEnabled();
}

void InterviewWidget::buildQuestion(const QJsonObject& questionObj, QVBoxLayout* layout)
{
    QuestionBlock block;
    block.question    = questionObj["question"].toString();
    block.multiSelect = questionObj["multiSelect"].toBool(false);
    const bool allowCustom = questionObj["allowCustom"].toBool(true);

    const QString header = questionObj["header"].toString();
    if (!header.isEmpty()) {
        QLabel* chip = new QLabel(header.toHtmlEscaped(), this);
        chip->setStyleSheet(
            "background-color: #7BA7D7; color: white; border-radius: 3px;"
            "padding: 1px 6px; font-size: 10px; font-weight: bold;");
        QHBoxLayout* chipRow = new QHBoxLayout();
        chipRow->addWidget(chip);
        chipRow->addStretch();
        layout->addLayout(chipRow);
    }

    QLabel* questionLabel = new QLabel(block.question.toHtmlEscaped(), this);
    questionLabel->setWordWrap(true);
    questionLabel->setStyleSheet("font-weight: bold; background: transparent; border: none;");
    layout->addWidget(questionLabel);

    // Exclusive group only for single-select; the custom toggle joins it so
    // picking "Other" deselects the fixed options and vice versa.
    QButtonGroup* group = nullptr;
    if (!block.multiSelect) {
        group = new QButtonGroup(this);
        group->setExclusive(true);
    }

    const QJsonArray options = questionObj["options"].toArray();
    for (const QJsonValue& val : options) {
        const QJsonObject opt = val.toObject();
        const QString label       = opt["label"].toString();
        const QString description = opt["description"].toString();
        if (label.isEmpty())
            continue;

        QAbstractButton* button;
        if (block.multiSelect)
            button = new QCheckBox(label, this);
        else
            button = new QRadioButton(label, this);
        button->setStyleSheet(kOptionButtonStyle);
        if (group)
            group->addButton(button);
        connect(button, &QAbstractButton::toggled, this, &InterviewWidget::updateSubmitEnabled);
        layout->addWidget(button);

        if (!description.isEmpty()) {
            QLabel* descLabel = new QLabel(description.toHtmlEscaped(), this);
            descLabel->setWordWrap(true);
            descLabel->setStyleSheet(
                "color: #666; font-size: 10px; background: transparent;"
                "border: none; margin-left: 22px;");
            layout->addWidget(descLabel);
        }

        block.options.append({button, label});
    }

    if (allowCustom) {
        QHBoxLayout* customRow = new QHBoxLayout();
        QAbstractButton* toggle;
        if (block.multiSelect)
            toggle = new QCheckBox(tr("Other:"), this);
        else
            toggle = new QRadioButton(tr("Other:"), this);
        toggle->setStyleSheet(kOptionButtonStyle);
        if (group)
            group->addButton(toggle);
        customRow->addWidget(toggle);

        QLineEdit* edit = new QLineEdit(this);
        edit->setPlaceholderText(tr("Type a custom answer..."));
        edit->setStyleSheet(
            "background-color: white; border: 1px solid #aaa;"
            "border-radius: 3px; padding: 2px 4px;");
        customRow->addWidget(edit, 1);
        layout->addLayout(customRow);

        connect(toggle, &QAbstractButton::toggled, this, &InterviewWidget::updateSubmitEnabled);
        connect(edit, &QLineEdit::textChanged, this, &InterviewWidget::updateSubmitEnabled);
        // Typing implies choosing "Other"
        connect(edit, &QLineEdit::textEdited, toggle, [toggle](const QString& text) {
            if (!text.isEmpty())
                toggle->setChecked(true);
        });

        block.customToggle = toggle;
        block.customEdit   = edit;
    }

    m_blocks.append(block);
}

bool InterviewWidget::isAnswered() const
{
    return m_answered;
}

QJsonObject InterviewWidget::result() const
{
    return m_result;
}

bool InterviewWidget::allQuestionsAnswered() const
{
    for (const QuestionBlock& block : m_blocks) {
        bool answered = false;
        for (const OptionRow& row : block.options) {
            if (row.button->isChecked()) {
                answered = true;
                break;
            }
        }
        if (!answered && block.customToggle && block.customToggle->isChecked()
            && !block.customEdit->text().trimmed().isEmpty())
            answered = true;
        if (!answered)
            return false;
    }
    return true;
}

void InterviewWidget::updateSubmitEnabled()
{
    if (m_answered)
        return;
    m_submitButton->setEnabled(allQuestionsAnswered());
}

QJsonObject InterviewWidget::collectAnswers() const
{
    QJsonObject answers;
    for (int i = 0; i < m_blocks.size(); ++i) {
        const QuestionBlock& block = m_blocks.at(i);
        QStringList selected;
        for (const OptionRow& row : block.options) {
            if (row.button->isChecked())
                selected.append(row.label);
        }
        if (block.customToggle && block.customToggle->isChecked()) {
            const QString custom = block.customEdit->text().trimmed();
            if (!custom.isEmpty())
                selected.append(custom);
        }

        // Answers are keyed by question text; an empty or repeated text would
        // overwrite an earlier answer, so only then append the 1-based position.
        QString key = block.question;
        if (key.isEmpty() || answers.contains(key)) {
            const QString base = key.isEmpty() ? QStringLiteral("Question") : key;
            key = QStringLiteral("%1 (#%2)").arg(base).arg(i + 1);
            for (int n = 2; answers.contains(key); ++n)
                key = QStringLiteral("%1 (#%2.%3)").arg(base).arg(i + 1).arg(n);
        }

        if (block.multiSelect)
            answers[key] = QJsonArray::fromStringList(selected);
        else
            answers[key] = selected.isEmpty() ? QString() : selected.first();
    }
    return answers;
}

void InterviewWidget::onSubmitClicked()
{
    if (m_answered)
        return;
    QJsonObject result;
    result["status"]  = QStringLiteral("answered");
    result["answers"] = collectAnswers();
    finish(result);
}

void InterviewWidget::onSkipClicked()
{
    if (m_answered)
        return;
    QJsonObject result;
    result["status"] = QStringLiteral("skipped");
    finish(result);
}

void InterviewWidget::finish(const QJsonObject& result)
{
    m_answered = true;
    m_result   = result;
    lockInputs();
    emit finished(result);
}

void InterviewWidget::lockInputs()
{
    for (const QuestionBlock& block : m_blocks) {
        for (const OptionRow& row : block.options)
            row.button->setEnabled(false);
        if (block.customToggle)
            block.customToggle->setEnabled(false);
        if (block.customEdit)
            block.customEdit->setEnabled(false);
    }
    m_submitButton->setEnabled(false);
    m_skipButton->setEnabled(false);
    const bool skipped = (m_result["status"].toString() == QLatin1String("skipped"));
    m_submitButton->setText(skipped ? tr("Skipped") : tr("Answered ✓"));
    m_skipButton->setVisible(false);
    setStyleSheet(
        "QtLLM--InterviewWidget {"
        "  background-color: #EDEDED;"
        "  border: 1px solid #bbb;"
        "  border-radius: 8px;"
        "}");
}

} // namespace QtLLM
