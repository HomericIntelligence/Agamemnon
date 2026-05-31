#pragma once

// ── ADR-015 PORT NOTE ────────────────────────────────────────────────────────
// InProcessRouter is an Agamemnon-local, minimal in-process message router for
// the ported HMAS agent runtime. It deliberately does NOT vendor Keystone's
// full MessageBus (BlazingMQ/NATS transport, agent_id_interning, the
// IAgentRegistry / ISchedulerIntegration interfaces) — that transport remains
// Keystone's responsibility. The ported agents only ever depend on the abstract
// `core::IMessageRouter` (via AgentCore::setMessageBus). This router implements
// exactly that interface plus the small registry + scheduler surface the
// behavioural agent tests need:
//   - registerAgent(id, shared_ptr<AgentCore>)
//   - setScheduler(WorkStealingScheduler*)  (push/async routing)
//   - routeMessage(msg)                     (IMessageRouter; sync or async)
// In production, agent-to-agent traffic crosses Keystone's NATS subjects; this
// router is the local, dependency-free analogue used for in-process delegation
// and unit/e2e coverage of the ported hierarchy.
// ─────────────────────────────────────────────────────────────────────────────

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "agents/agent_core.hpp"
#include "agents/concepts.hpp"
#include "concurrency/work_stealing_scheduler.hpp"
#include "core/i_message_router.hpp"
#include "core/message.hpp"

namespace keystone {
namespace core {

/**
 * @brief Minimal in-process implementation of IMessageRouter.
 *
 * Routes a KeystoneMessage to the registered agent identified by
 * msg.receiver_id. Without a scheduler it delivers synchronously via
 * AgentCore::receiveMessage (pull model). With a scheduler set, the agents'
 * own AsyncAgent::receiveMessage auto-processing kicks in (push model) because
 * registerAgent also propagates the scheduler onto each agent.
 */
class InProcessRouter : public IMessageRouter {
 public:
  InProcessRouter() = default;
  ~InProcessRouter() override = default;

  InProcessRouter(const InProcessRouter&) = delete;
  InProcessRouter& operator=(const InProcessRouter&) = delete;

  /**
   * @brief Register an agent so messages addressed to its ID are delivered.
   *
   * If a scheduler has already been set on this router, it is also stored on
   * the agent so async (push) auto-processing is enabled immediately.
   */
  void registerAgent(const std::string& agent_id, std::shared_ptr<agents::AgentCore> agent) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Matches Keystone MessageBus semantics: duplicate IDs are rejected.
    if (agents_.find(agent_id) != agents_.end()) {
      throw std::runtime_error("InProcessRouter::registerAgent: agent already registered: " +
                               agent_id);
    }
    if (agent && scheduler_ != nullptr) {
      agent->setScheduler(scheduler_);
    }
    agents_[agent_id] = std::move(agent);
  }

  /**
   * @brief Concept-checked single-argument registration (Agamemnon parity with
   * Keystone's templated MessageBus::registerAgent). Verifies at compile time
   * that the agent satisfies the full agents::Agent interface and derives its
   * ID from getAgentId().
   */
  template <agents::Agent A>
  void registerAgent(std::shared_ptr<A> agent) {
    if (!agent) {
      throw std::runtime_error("InProcessRouter::registerAgent: null agent pointer");
    }
    std::string agent_id = agent->getAgentId();
    std::shared_ptr<agents::AgentCore> base_agent = agent;
    registerAgent(agent_id, std::move(base_agent));
  }

  /**
   * @brief Remove a previously registered agent.
   */
  void unregisterAgent(const std::string& agent_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    agents_.erase(agent_id);
  }

  /**
   * @brief Enable async routing by storing a scheduler on this router and on
   * every currently-registered agent. Pass nullptr to revert to sync routing.
   */
  void setScheduler(concurrency::WorkStealingScheduler* scheduler) {
    std::lock_guard<std::mutex> lock(mutex_);
    scheduler_ = scheduler;
    for (auto& [id, agent] : agents_) {
      if (agent) {
        agent->setScheduler(scheduler);
      }
    }
  }

  concurrency::WorkStealingScheduler* getScheduler() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return scheduler_;
  }

  /**
   * @brief Whether an agent with the given ID is currently registered.
   */
  bool hasAgent(const std::string& agent_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return agents_.find(agent_id) != agents_.end();
  }

  /**
   * @brief Snapshot of all registered agent IDs.
   */
  std::vector<std::string> listAgents() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    ids.reserve(agents_.size());
    for (const auto& [id, agent] : agents_) {
      ids.push_back(id);
    }
    return ids;
  }

  /**
   * @brief IMessageRouter: route msg to the agent named by msg.receiver_id.
   *
   * @return true if a matching agent was found and the message delivered.
   */
  bool routeMessage(const KeystoneMessage& msg) override {
    std::shared_ptr<agents::AgentCore> target;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = agents_.find(msg.receiver_id);
      if (it == agents_.end()) {
        return false;
      }
      target = it->second;
    }
    if (!target) {
      return false;
    }
    // AgentCore::receiveMessage (sync inbox) or AsyncAgent::receiveMessage
    // (scheduler auto-processing) — the agent decides based on its stored
    // scheduler, which registerAgent/setScheduler keep in sync.
    target->receiveMessage(msg);
    return true;
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<agents::AgentCore>> agents_;
  concurrency::WorkStealingScheduler* scheduler_{nullptr};
};

}  // namespace core
}  // namespace keystone
