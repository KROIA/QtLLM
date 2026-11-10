#include "AgentRegistry.h"
#include "Agent.h"

namespace QtLLM {

AgentRegistry::AgentRegistry(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<QtLLM::AgentRecord>("QtLLM::AgentRecord");
}

AgentRegistry& AgentRegistry::instance()
{
    static AgentRegistry registry;
    return registry;
}

QList<Agent*> AgentRegistry::agents() const
{
    return m_agents;
}

QList<AgentRecord> AgentRegistry::finishedAgents() const
{
    return m_finished;
}

void AgentRegistry::clearFinished()
{
    m_finished.clear();
}

void AgentRegistry::add(Agent* agent)
{
    if (!agent || m_agents.contains(agent))
        return;
    m_agents.append(agent);
    emit agentSpawned(agent);
}

void AgentRegistry::remove(Agent* agent)
{
    if (!agent)
        return;
    const QString name = agent->config().name;
    if (m_agents.removeAll(agent) == 0)
        return;
    // A copy, so a slot that clears the history cannot pull it from under the signal.
    const AgentRecord record = agent->record();
    m_finished.prepend(record);
    while (m_finished.size() > kMaxFinished)
        m_finished.removeLast();
    emit agentFinished(record);
    emit agentDestroyed(name);
}

} // namespace QtLLM
