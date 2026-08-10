#pragma once
#include "QtLLM_base.h"
#include <QFrame>
#include <QJsonObject>
#include <QList>
#include <QString>

class QVBoxLayout;
class QAbstractButton;
class QLineEdit;
class QPushButton;

namespace QtLLM
{

// Inline interview card shown inside the chat conversation. Renders one or
// more questions — single-select (radio buttons), multi-select (checkboxes),
// and an optional free-text "Other" answer — with Submit/Skip buttons,
// mirroring the question widgets Claude Code shows in its CLI.
//
// Request format (matches the "ask_user_question" tool input, see InterviewTool):
// {
//   "questions": [
//     {
//       "question":    "Which database should we use?",   // required
//       "header":      "Database",                        // optional chip label
//       "multiSelect": false,                             // optional, default false
//       "allowCustom": true,                              // optional, default true
//       "options": [                                      // required, >= 1 entry
//         { "label": "Postgres", "description": "Relational, full SQL" },
//         { "label": "SQLite",   "description": "Embedded, zero setup" }
//       ]
//     }
//   ]
// }
class QT_LLM_API InterviewWidget : public QFrame
{
    Q_OBJECT
public:
    explicit InterviewWidget(const QJsonObject& request, QWidget* parent = nullptr);

    bool isAnswered() const;

    // { "status": "answered", "answers": { "<question>": "label" | ["l1","l2"] } }
    // or { "status": "skipped" }. Empty object until the user acted.
    QJsonObject result() const;

signals:
    // Emitted exactly once, when the user submits or skips.
    void finished(const QJsonObject& result);

private slots:
    void onSubmitClicked();
    void onSkipClicked();

private:
    struct OptionRow {
        QAbstractButton* button = nullptr;
        QString label;
    };
    struct QuestionBlock {
        QString question;
        bool multiSelect = false;
        QList<OptionRow> options;
        QAbstractButton* customToggle = nullptr;  // enables the free-text field
        QLineEdit* customEdit = nullptr;
    };

    void buildQuestion(const QJsonObject& questionObj, QVBoxLayout* layout);
    bool allQuestionsAnswered() const;
    void updateSubmitEnabled();
    QJsonObject collectAnswers() const;
    void finish(const QJsonObject& result);
    void lockInputs();

    QList<QuestionBlock> m_blocks;
    QPushButton* m_submitButton = nullptr;
    QPushButton* m_skipButton = nullptr;
    QJsonObject m_result;
    bool m_answered = false;
};

} // namespace QtLLM
