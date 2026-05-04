# Sources and headers for the ProjectAgamemnon library target (version only).
# The server executable sources are declared directly in CMakeLists.txt.
#
# NOTE: src/main.cpp is NOT included here — it has a main() function that
# would conflict with GTest::gtest_main when the test binary links against
# this library.
set(sources
    src/version_info.cpp
    src/store.cpp
    src/rate_limiter.cpp)

set(headers
    include/projectagamemnon/version.hpp
    include/projectagamemnon/store.hpp
    include/projectagamemnon/peer_discovery.hpp
    include/projectagamemnon/rate_limiter.hpp)
