#pragma once

#include "agamemnon/github_client.hpp"

namespace agamemnon::test {

// Controlled durable Contents records outlive individual Store instances. A
// missing read never bypasses the conditional insertion at the write boundary.
class ConditionalAuthority : public MockGitHubClient {
 public:
  bool reject_next_fence_write = false;
  bool lose_next_fence_ack = false;
  bool mismatch_next_fence_ack = false;
  bool hide_fences = false;
  std::unordered_map<std::string, ImportFence> fences;

  std::optional<ImportFence> import_read_fence(const std::string& branch, const std::string& key,
                                               ImportContext& context) override {
    context.checkpoint();
    const auto found = fences.find(branch + "/" + key);
    if (hide_fences || found == fences.end()) return std::nullopt;
    return found->second;
  }

  ImportFence import_write_fence(const std::string& branch, const std::string& key,
                                 const json& document, const std::optional<std::string>& expected,
                                 ImportContext& context) override {
    context.checkpoint();
    if (reject_next_fence_write) {
      reject_next_fence_write = false;
      throw std::runtime_error("fixture_rejected_conditional_write");
    }
    const auto identity = branch + "/" + key;
    const auto found = fences.find(identity);
    if ((!expected && found != fences.end()) ||
        (expected && (found == fences.end() || *expected != found->second.sha)))
      throw std::runtime_error("fixture_conditional_conflict");
    ImportFence confirmed{std::string(40, 'a'), document};
    fences[identity] = confirmed;
    if (lose_next_fence_ack) {
      lose_next_fence_ack = false;
      throw std::runtime_error("fixture_lost_conditional_acknowledgement");
    }
    if (mismatch_next_fence_ack) {
      mismatch_next_fence_ack = false;
      confirmed.document["unexpected"] = true;
    }
    return confirmed;
  }
};

}  // namespace agamemnon::test
