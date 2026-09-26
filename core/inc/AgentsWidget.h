#pragma once
#include "QtLLM_base.h"
#include <QWidget>
#include <QTimer>

class QTableWidget;
class QLabel;
class QTextEdit;

namespace QtLLM {

class Agent;

// Read-only view of every live background agent in the process.
//
// Binds itself to AgentRegistry, so it needs no wiring from the host
// application: any agent spawned anywhere shows up here. Read-only by design —
// the application owns its agents, and a Settings dialog deleting an object the
// application still holds a pointer to is a crash waiting to happen.
class QT_LLM_API AgentsWidget : public QWidget
{
    Q_OBJECT
public:
    explicit AgentsWidget(QWidget* parent = nullptr);
    ~AgentsWidget() override;

private slots:
    void refresh();
    void onSelectionChanged();

private:
    Agent* selectedAgent() const;
    void   updateDetails();

    QTableWidget* m_table   = nullptr;
    QLabel*       m_empty   = nullptr;
    QTextEdit*    m_details = nullptr;
    QTimer        m_tick;    // keeps uptime and in-flight counters moving
};

} // namespace QtLLM
