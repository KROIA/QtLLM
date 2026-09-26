#include "AgentRegistry.h"
#include "Agent.h"

namespace QtLLM {

AgentRegistry::AgentRegistry(QObject* parent)
    : QObject(parent)
{
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
    if (m_agents.removeAll(agent) > 0)
        emit agentDestroyed(name);
}

} // namespace QtLLM
