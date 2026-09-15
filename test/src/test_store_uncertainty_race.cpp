#include "agamemnon/store.hpp"

#include <barrier>
#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace agamemnon::test {
namespace {

// All observations are recorded at the actual fake GitHub boundary or after an
// actual Store call returns. No pre-call marker proves entry into a Store lock.
class UncertainGitHub final : public IGitHubClient {
 public:
  std::promise<void> update_entered;
  std::promise<void> release_update;
  std::shared_future<void> release = release_update.get_future().share();

  std::vector<json> list_issues(std::string_view label) override {
    std::lock_guard lock(mutex_);
    trace_.push_back({{"event", "hydrate"}, {"label", label}});
    std::vector<json> result;
    for (const auto& [number, issue] : issues_)
      if (issue.at("label") == label)
        result.push_back({{"number", number}, {"body", issue.at("body")}});
    if (label == "agamemnon-hmas-task") uncertain_ = false;
    return result;
  }
  std::vector<json> list_issues_including_closed(std::string_view label) override {
    return list_issues(label);
  }
  std::string create_issue(std::string_view title, std::string_view body,
                           std::string_view label) override {
    std::lock_guard lock(mutex_);
    const auto number = ++next_number_;
    trace_.push_back({{"event", "create"}, {"title", title}, {"uncertain", uncertain_}});
    if (uncertain_) violations_.push_back("new GitHub issue before reconciliation");
    issues_[number] = {{"label", label}, {"body", body}};
    return std::to_string(number);
  }
  void update_issue_body(std::string_view number, std::string_view body) override {
    {
      std::lock_guard lock(mutex_);
      issues_.at(std::stoi(std::string(number)))["body"] = body;
      trace_.push_back({{"event", "update_committed"}, {"number", number}});
    }
    update_entered.set_value();
    release.wait();
    {
      std::lock_guard lock(mutex_);
      uncertain_ = true;
      trace_.push_back({{"event", "update_response_lost"}});
    }
    throw std::runtime_error("update response lost after commit");
  }
  void close_issue(std::string_view) override {
    throw std::runtime_error("unexpected close in uncertainty test");
  }
  void observed_snapshot(const std::vector<HmasTask>& tasks) {
    std::lock_guard lock(mutex_);
    bool found_seed = false;
    for (const auto& task : tasks) {
      if (task.id == "leaf") found_seed = true;
      trace_.push_back({{"event", "snapshot_returned"},
                        {"id", task.id},
                        {"state", task_state_to_string(task.state)},
                        {"uncertain", uncertain_}});
      if (uncertain_ && task.id == "leaf" && task.state == TaskState::Pending)
        violations_.push_back("stale HMAS snapshot after uncertain write");
    }
    if (!found_seed) throw std::logic_error("seed task absent from snapshot");
  }
  json evidence() {
    std::lock_guard lock(mutex_);
    return {{"trace", trace_}, {"violations", violations_}};
  }

 private:
  std::mutex mutex_;
  int next_number_ = 0;
  bool uncertain_ = false;
  std::map<int, json> issues_;
  json trace_ = json::array();
  std::vector<std::string> violations_;
};

enum class WaitingOperation { Create, ReadOne, ReadLayer, ReadParent, ReadBrief };

class StoreUncertaintyRace : public ::testing::TestWithParam<WaitingOperation> {};

TEST_P(StoreUncertaintyRace, NoNewWriteOrStaleSnapshotBeforeReconciliation) {
  // Bounded stress: a barrier and short delay create contention but do not prove
  // that any waiter passed the hydration fast path. RED requires an observed
  // forbidden call/return in the fixture trace, not a scheduling assumption.
  constexpr int kAttempts = 8;
  constexpr int kWaiters = 8;
  for (int attempt = 0; attempt < kAttempts; ++attempt) {
    auto github = std::make_shared<UncertainGitHub>();
    Store store(github);
    HmasTask initial;
    initial.id = "leaf";
    initial.layer = HmasLayer::L3_TaskAgent;
    initial.state = TaskState::Pending;
    initial.parent_task_id = "parent";
    initial.brief_id = "brief";
    store.create_hmas_task(initial);
    auto entered = github->update_entered.get_future();
    auto writer = std::async(std::launch::async, [&] {
      try {
        store.update_hmas_task_state(initial.id, TaskState::InProgress);
        return false;
      } catch (const std::runtime_error&) {
        return true;
      }
    });
    const bool did_enter = entered.wait_for(std::chrono::seconds{2}) == std::future_status::ready;
    if (!did_enter) {
      github->release_update.set_value();
      (void)writer.get();
      FAIL() << "the actual GitHub update boundary was not reached";
    }

    std::barrier start{kWaiters + 1};
    std::vector<std::future<void>> waiters;
    for (int i = 0; i < kWaiters; ++i) {
      waiters.push_back(std::async(std::launch::async, [&, i] {
        start.arrive_and_wait();
        try {
          switch (GetParam()) {
            case WaitingOperation::Create: {
              auto next = initial;
              next.id = "next-" + std::to_string(i);
              store.create_hmas_task(next);
              break;
            }
            case WaitingOperation::ReadOne: {
              const auto task = store.get_hmas_task(initial.id);
              if (!task) throw std::logic_error("seed task disappeared");
              github->observed_snapshot({*task});
              break;
            }
            case WaitingOperation::ReadLayer:
              github->observed_snapshot(store.list_hmas_tasks_by_layer(initial.layer));
              break;
            case WaitingOperation::ReadParent:
              github->observed_snapshot(store.list_hmas_tasks_by_parent(initial.parent_task_id));
              break;
            case WaitingOperation::ReadBrief:
              github->observed_snapshot(store.list_hmas_tasks_by_brief(initial.brief_id));
              break;
          }
        } catch (const std::runtime_error&) {
          // An uncertain operation may fail closed. Fresh retry below must work.
        }
      }));
    }
    start.arrive_and_wait();
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    github->release_update.set_value();
    const bool lost_response = writer.get();
    for (auto& waiter : waiters) waiter.get();
    ASSERT_TRUE(lost_response);

    const auto evidence = github->evidence();
    const auto refreshed = store.get_hmas_task(initial.id);
    ASSERT_TRUE(refreshed.has_value());
    EXPECT_EQ(refreshed->state, TaskState::InProgress);
    if (!evidence.at("violations").empty()) {
      ADD_FAILURE() << "Observed forbidden ordering at attempt " << attempt << ":\n"
                    << evidence.dump(2);
      return;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(HMAS, StoreUncertaintyRace,
                         ::testing::Values(WaitingOperation::Create, WaitingOperation::ReadOne,
                                           WaitingOperation::ReadLayer,
                                           WaitingOperation::ReadParent,
                                           WaitingOperation::ReadBrief),
                         [](const ::testing::TestParamInfo<WaitingOperation>& info) {
                           switch (info.param) {
                             case WaitingOperation::Create:
                               return "Create";
                             case WaitingOperation::ReadOne:
                               return "ReadOne";
                             case WaitingOperation::ReadLayer:
                               return "ReadLayer";
                             case WaitingOperation::ReadParent:
                               return "ReadParent";
                             case WaitingOperation::ReadBrief:
                               return "ReadBrief";
                           }
                           return "Unknown";
                         });

}  // namespace
}  // namespace agamemnon::test
