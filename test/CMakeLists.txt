cmake_minimum_required(VERSION 3.20)

find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
find_package(httplib REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(prometheus-cpp REQUIRED)

# ── Unit tests ───────────────────────────────────────────────────────────────
add_executable(${PROJECT_NAME}_tests
  src/test_version.cpp
  src/test_store.cpp
  src/test_parse_port.cpp
  src/test_store_edge_cases.cpp
  src/test_store_concurrent.cpp
  src/test_routes.cpp
  src/test_routes_malformed.cpp
  src/test_nats_client.cpp
  src/test_peer_discovery.cpp
  src/test_server_limits.cpp
  src/test_circuit_breaker.cpp
  src/test_dead_letter_queue.cpp
  src/test_pagination.cpp
  src/test_store_hydration.cpp
  src/test_auth.cpp
  src/test_rate_limiter.cpp
  src/test_shutdown.cpp
  src/test_hmas_types.cpp
  src/test_orchestrator.cpp
  src/test_planning_breakdown.cpp
  src/test_state_machine.cpp
  src/test_github_client.cpp
  src/test_agent_envelope.cpp
  src/test_orchestrator_persistence.cpp
)

target_compile_features(${PROJECT_NAME}_tests PRIVATE cxx_std_20)
set_target_properties(${PROJECT_NAME}_tests PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(${PROJECT_NAME}_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src)

# Suppress warnings from external headers in test binary
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
    ProjectAgamemnon_core
    agamemnon_peer_discovery
    GTest::gtest_main
    GTest::gmock
    httplib::httplib
    nats_static
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    prometheus-cpp::core)

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
    ProjectAgamemnon_core
    GTest::gtest_main
    httplib::httplib
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
    ProjectAgamemnon_core
    GTest::gtest_main
    httplib::httplib)

gtest_discover_tests(${PROJECT_NAME}_integration_tests
  PROPERTIES LABELS "integration")

# ── Ported HMAS agent executor + work-stealing tests (ADR-015) ───────────────
# Behavioural ports of Keystone's agent / work-stealing gtests, adapted to use
# Agamemnon's dependency-free core::InProcessRouter instead of Keystone's
# MessageBus. Exercises the full L0→L3 delegation/coordination paths and the
# work-stealing queue + scheduler.
add_executable(${PROJECT_NAME}_agents_tests
  src/agents/test_work_stealing_queue.cpp
  src/agents/test_work_stealing_scheduler.cpp
  src/agents/test_agent_concepts.cpp
  src/agents/test_basic_delegation.cpp
  src/agents/test_module_coordination.cpp
  src/agents/test_component_coordination.cpp
  # Coverage for ported executors that the behavioural suites did not reach:
  # core metrics, chaos failure injector, work-stealing PullOrSteal awaitables,
  # and the TaskAgent bash-execution strategy (ADR-015 port).
  src/agents/test_metrics.cpp
  src/agents/test_failure_injector.cpp
  src/agents/test_pull_or_steal.cpp
  src/agents/test_task_execution_strategy.cpp
  src/agents/test_logger.cpp
  src/agents/test_message_factory.cpp
  src/agents/test_agent_core.cpp)

target_compile_features(${PROJECT_NAME}_agents_tests PRIVATE cxx_std_20)
set_target_properties(${PROJECT_NAME}_agents_tests PROPERTIES CXX_EXTENSIONS OFF)

# Ported Keystone behavioural tests: exempt from clang-tidy / cppcheck (parity
# port, linted upstream — see agamemnon_agents target rationale).
set_target_properties(${PROJECT_NAME}_agents_tests PROPERTIES
  CXX_CLANG_TIDY ""
  CXX_CPPCHECK "")

target_compile_options(${PROJECT_NAME}_agents_tests PRIVATE
  -Wno-old-style-cast
  -Wno-useless-cast
  -Wno-conversion
  -Wno-sign-conversion
  -Wno-shadow
  -Wno-format-nonliteral
  -Wno-double-promotion
  -Wno-cast-align
  -Wno-unused-parameter
  -Wno-unused-variable)

target_link_libraries(${PROJECT_NAME}_agents_tests
  PRIVATE
    agamemnon_agents
    GTest::gtest_main
    GTest::gmock
    Threads::Threads)

gtest_discover_tests(${PROJECT_NAME}_agents_tests
  DISCOVERY_TIMEOUT 120
  PROPERTIES LABELS "agents")
