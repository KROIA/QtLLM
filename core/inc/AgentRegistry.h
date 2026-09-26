#pragma once
#include "QtLLM_base.h"
#include <QObject>
#include <QList>
#include <QString>

namespace QtLLM {

class Agent;

// Process-wide list of live background agents.
//
// Agents add themselves on construction and remove themselves on destruction,
// so host applications never call into this class. It exists so the Settings
// dialog can show every agent in the process without the application having to
// wire anything up — the same "bind it and it works" approach as
// ChatDockWidget::setClient().
class QT_LLM_API AgentRegistry : public QObject
{
    Q_OBJECT
public:
    static AgentRegistry& instance();

    // Live agents, in spawn order.
    QList<Agent*> agents() const;

signals:
    void agentSpawned(QtLLM::Agent* agent);
    void agentDestroyed(const QString& name);

private:
    friend class Agent;

    explicit AgentRegistry(QObject* parent = nullptr);

    void add(Agent* agent);
    void remove(Agent* agent);

    QList<Agent*> m_agents;
};

} // namespace QtLLM
