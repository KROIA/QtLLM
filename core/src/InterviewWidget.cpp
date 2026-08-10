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

InterviewWidget::InterviewWidget(const QJsonObject& request, QWidget* parent)
    : QFrame(parent)
{
    setFrameShape(QFrame::NoFrame);
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
        button->setStyleSheet("background: transparent; border: none;");
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
        toggle->setStyleSheet("background: transparent; border: none;");
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
    for (const QuestionBlock& block : m_blocks) {
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

        if (block.multiSelect)
            answers[block.question] = QJsonArray::fromStringList(selected);
        else
            answers[block.question] = selected.isEmpty() ? QString() : selected.first();
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
