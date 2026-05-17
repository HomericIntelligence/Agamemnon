// Stub source for the library target — provides version symbols.
// The actual main() is in server_main.cpp (server executable target).
#include "projectagamemnon/version.hpp"

namespace projectagamemnon {

// NOLINT(misc-use-internal-linkage) below: both functions are declared in
// include/projectagamemnon/version.hpp and consumed externally; internal
// linkage would be incorrect.
const char* get_version() { return kVersion.data(); }       // NOLINT(misc-use-internal-linkage)
const char* get_project_name() { return kProjectName.data(); }  // NOLINT(misc-use-internal-linkage)

}  // namespace projectagamemnon
