// Minimal translation unit for the clang_tidy_stddef_smoke ctest regression
// guard (#211). <cstddef> transitively pulls bits/c++config.h and stddef.h —
// exactly the headers clang-tidy failed to resolve under conda/pixi GCC
// sysroots when only a single -isystem directory was forwarded.
#include <cstddef>
#include <string>

static_assert(sizeof(std::size_t) >= 4, "std::size_t must be at least 32 bits");

int clang_tidy_smoke() {
  std::string s = "smoke";
  return static_cast<int>(s.size());
}
