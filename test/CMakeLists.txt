cmake_minimum_required(VERSION 3.20)

find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
find_package(httplib REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)

# ── Unit tests ───────────────────────────────────────────────────────────────
add_executable(${PROJECT_NAME}_tests
  src/test_main.cpp
  src/test_store.cpp
  src/test_parse_port.cpp
  src/test_store_edge_cases.cpp
  src/test_store_concurrent.cpp
  src/test_routes_malformed.cpp
  src/test_peer_discovery.cpp
  src/test_server_limits.cpp
  src/test_circuit_breaker.cpp
  src/test_dead_letter_queue.cpp
  src/test_pagination.cpp
  src/test_store_hydration.cpp
  ${PROJECT_SOURCE_DIR}/src/routes.cpp
  ${PROJECT_SOURCE_DIR}/src/store.cpp
  ${PROJECT_SOURCE_DIR}/src/nats_client.cpp
  ${PROJECT_SOURCE_DIR}/src/version_info.cpp
)

target_compile_features(${PROJECT_NAME}_tests PRIVATE cxx_std_20)
set_target_properties(${PROJECT_NAME}_tests PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(${PROJECT_NAME}_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src)

# Suppress warnings from external headers (httplib, nlohmann, nats) in test target.
target_compile_options(${PROJECT_NAME}_tests PRIVATE
  -Wno-old-style-cast
  -Wno-useless-cast
  -Wno-conversion
  -Wno-sign-conversion
  -Wno-shadow
  -Wno-format-nonliteral
  -Wno-double-promotion
  -Wno-cast-align
)

target_link_libraries(${PROJECT_NAME}_tests
  PRIVATE
    ${PROJECT_NAME}::${PROJECT_NAME}
    agamemnon_lib
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    agamemnon_peer_discovery
    nats_static
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads)

target_compile_options(${PROJECT_NAME}_tests PRIVATE
  -Wno-old-style-cast
  -Wno-useless-cast
  -Wno-conversion
  -Wno-sign-conversion
  -Wno-shadow
  -Wno-format-nonliteral
  -Wno-double-promotion
  -Wno-cast-align
)

include(GoogleTest)
gtest_discover_tests(${PROJECT_NAME}_tests DISCOVERY_TIMEOUT 120)

# ── Route limits integration tests ───────────────────────────────────────────
add_executable(${PROJECT_NAME}_routes_tests src/test_routes_limits.cpp)

target_compile_features(${PROJECT_NAME}_routes_tests PRIVATE cxx_std_20)
set_target_properties(${PROJECT_NAME}_routes_tests PROPERTIES CXX_EXTENSIONS OFF)

target_compile_options(${PROJECT_NAME}_routes_tests PRIVATE
  -Wno-old-style-cast
  -Wno-useless-cast
  -Wno-conversion
  -Wno-sign-conversion
  -Wno-shadow
  -Wno-format-nonliteral
  -Wno-double-promotion
  -Wno-cast-align
)

target_include_directories(${PROJECT_NAME}_routes_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(${PROJECT_NAME}_routes_tests
  PRIVATE
    agamemnon_lib
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    nats_static
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads)

gtest_discover_tests(${PROJECT_NAME}_routes_tests DISCOVERY_TIMEOUT 120)

# ── Integration tests ────────────────────────────────────────────────────────
add_executable(${PROJECT_NAME}_integration_tests
  src/integration/test_agent_endpoints.cpp
  src/integration/test_task_lifecycle.cpp
  src/integration/test_chaos_endpoints.cpp)

target_compile_features(${PROJECT_NAME}_integration_tests PRIVATE cxx_std_20)
set_target_properties(${PROJECT_NAME}_integration_tests PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(${PROJECT_NAME}_integration_tests
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/integration
    ${CMAKE_SOURCE_DIR}/include)

# Suppress warnings from external headers (httplib, nats) in test binary.
target_compile_options(${PROJECT_NAME}_integration_tests PRIVATE
  -Wno-old-style-cast
  -Wno-useless-cast
  -Wno-conversion
  -Wno-sign-conversion
  -Wno-shadow
  -Wno-format-nonliteral
  -Wno-double-promotion
  -Wno-cast-align)

target_link_libraries(${PROJECT_NAME}_integration_tests
  PRIVATE
    ${PROJECT_NAME}::${PROJECT_NAME}
    agamemnon_lib
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json)

gtest_discover_tests(${PROJECT_NAME}_integration_tests
  PROPERTIES LABELS "integration")
