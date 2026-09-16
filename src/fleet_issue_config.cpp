#include "agamemnon/fleet_issue_config.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace agamemnon {
namespace {
using json = nlohmann::json;
constexpr std::size_t max_bytes = 65536;

[[noreturn]] void invalid() { throw std::invalid_argument("invalid_issue_import_configuration"); }

class Descriptor {
 public:
  explicit Descriptor(int value) : value_(value) {
    if (value_ < 0) invalid();
  }
  ~Descriptor() { ::close(value_); }
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  int get() const { return value_; }
  void replace(int value) {
    if (value < 0) invalid();
    ::close(value_);
    value_ = value;
  }

 private:
  int value_;
};

void private_file(const struct stat& info) {
  const auto mode = info.st_mode & 07777;
  if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || info.st_nlink != 1 ||
      (mode != 0400 && mode != 0600) || info.st_size < 0 ||
      static_cast<std::uintmax_t>(info.st_size) > max_bytes)
    invalid();
}

json read_configuration(const std::string& selected) {
  if (selected.empty() || selected.find('\0') != std::string::npos) invalid();
  const std::filesystem::path path(selected);
  if (path.filename().empty()) invalid();
  // Use the build loader's descriptor walk. Keep the parent open through the
  // final read so a renamed ancestor cannot select a different file.
  Descriptor directory(::open(path.is_absolute() ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  for (const auto& part : path.relative_path().parent_path()) {
    directory.replace(
        ::openat(directory.get(), part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  }
  struct stat named {};
  if (::fstatat(directory.get(), path.filename().c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0)
    invalid();
  // Check the named object before open: Darwin descriptor aliases can otherwise
  // share a borrowed descriptor's offset despite the supplied open flags.
  private_file(named);
  Descriptor input(::openat(directory.get(), path.filename().c_str(),
                            O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
  struct stat opened {};
  if (::fstat(input.get(), &opened) != 0) invalid();
  private_file(opened);
  if (named.st_dev != opened.st_dev || named.st_ino != opened.st_ino) invalid();

  std::string bytes;
  bytes.reserve(static_cast<std::size_t>(opened.st_size));
  std::array<char, 8192> buffer{};
  for (;;) {
    const auto count = ::read(input.get(), buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) invalid();
    if (count == 0) break;
    if (static_cast<std::size_t>(count) > max_bytes - bytes.size()) invalid();
    bytes.append(buffer.data(), static_cast<std::size_t>(count));
  }
  struct stat after {};
  if (::fstat(input.get(), &after) != 0) invalid();
  private_file(after);
  if (after.st_size != opened.st_size || bytes.size() != static_cast<std::size_t>(after.st_size))
    invalid();

  std::vector<std::set<std::string>> keys;
  return json::parse(bytes, [&keys](int depth, json::parse_event_t event, json& value) {
    if (depth > 64) invalid();
    if (event == json::parse_event_t::object_start) keys.emplace_back();
    if (event == json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
      invalid();
    if (event == json::parse_event_t::object_end) keys.pop_back();
    return true;
  });
}
}  // namespace

std::shared_ptr<const IssueImportConfiguration> load_issue_import_configuration(
    const std::optional<std::string>& path, const std::optional<std::string>& state_branch,
    bool durable_persistence, bool authenticated_api, bool research_enabled) {
  if (!path && !state_branch && !research_enabled) return nullptr;
  try {
    if (!path || !state_branch || path->empty() || state_branch->empty() || !durable_persistence ||
        !authenticated_api)
      invalid();
    const auto document = read_configuration(*path);
    if (!document.is_object() || document.size() != 2 ||
        document.value("schema", json()) != "hi/agamemnon/issue-intake-config/v1" ||
        !document.contains("repositories"))
      invalid();
    const IssueImportConfiguration configuration{document.at("repositories"), *state_branch};
    validate_issue_import_configuration(configuration);
    return std::make_shared<const IssueImportConfiguration>(configuration);
  } catch (const std::exception&) {
    // Filesystem and JSON errors can contain private operator data.
    invalid();
  }
}

}  // namespace agamemnon
