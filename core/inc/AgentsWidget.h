#pragma once
#include "QtLLM_base.h"
#include <QWidget>
#include <QTimer>

class QTableWidget;
class QLabel;
class QTextBrowser;

namespace QtLLM {

class Agent;

// Read-only view of every background agent in the process: the running ones,
// the ones that finished in this session (AgentRegistry::finishedAgents()),
// and for the selected one what it was asked, what it answered, the tools it
// called, how long it worked and what it cost.
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

    // Shows or hides the finished-agents table (default: shown). The registry
    // keeps recording either way.
    void setHistoryVisible(bool visible);
    bool isHistoryVisible() const;

private slots:
    void refresh();

private:
    void fillRunning();
    void fillFinished();
    void selectFrom(QTableWidget* table);
    void updateDetails();

    QTableWidget* m_running  = nullptr;
    QTableWidget* m_finished = nullptr;
    QWidget*      m_history  = nullptr;
    QLabel*       m_empty    = nullptr;
    QTextBrowser* m_details  = nullptr;
    int           m_selectedId = 0;     // AgentRecord::id, 0 = nothing selected
    int           m_shownId    = 0;     // whose details m_details holds
    QString       m_shownHtml;
    QTimer        m_tick;    // keeps uptime and in-flight counters moving
};

} // namespace QtLLM
