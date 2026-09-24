#include "StageProfile.h"

StageProfile::StageProfile(QString key,
                           std::optional<AgentDefinition> agent,
                           std::shared_ptr<const TransitionPolicy> exitPolicy)
    : m_key(std::move(key)),
      m_agent(std::move(agent)),
      // Tanpa policy eksplisit, stage dianggap tanpa gate
      m_exitPolicy(exitPolicy ? std::move(exitPolicy) : std::make_shared<AutoAdvance>()) {
}
