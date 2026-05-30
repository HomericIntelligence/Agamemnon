cmake_minimum_required(VERSION 3.20)

project(
  ProjectAgamemnon
  VERSION 0.1.0
  DESCRIPTION "Planning, coordination, and agentic orchestration for HomericIntelligence"
  LANGUAGES CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(
    FATAL_ERROR
      "In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there."
  )
endif()

include(cmake/StandardSettings.cmake)
include(cmake/CompilerWarnings.cmake)
include(cmake/StaticAnalyzers.cmake)
include(cmake/Doxygen.cmake)
include(cmake/SourcesAndHeaders.cmake)

# ── External dependencies ────────────────────────────────────────────────────
# cpp-httplib, nlohmann_json, libcurl, and prometheus-cpp are provided by Conan (run `just deps` first).
# nats.c stays as FetchContent (not reliably available on ConanCenter).
find_package(httplib REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(prometheus-cpp REQUIRED)
# ADR-015: lock-free MPMC queue backing the ported HMAS agent runtime.
find_package(concurrentqueue REQUIRED)
find_package(Threads REQUIRED)

include(FetchContent)
FetchContent_Declare(
  nats_c
  GIT_REPOSITORY https://github.com/nats-io/nats.c.git
  GIT_TAG        aed20fe4227a99c6ae3dc97fc35469cc24f081e7 # v3.12.0
)
set(NATS_BUILD_STREAMING OFF CACHE BOOL "" FORCE)
set(NATS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING_SAVED "${BUILD_TESTING}")
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nats_c)
set(BUILD_TESTING "${BUILD_TESTING_SAVED}" CACHE BOOL "" FORCE)

# ── Library target (version only — keeps tests linking clean) ────────────────
add_library(${PROJECT_NAME} ${headers} ${sources})
add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_20)
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(
  ${PROJECT_NAME}
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(
  ${PROJECT_NAME}
  PUBLIC
    nlohmann_json::nlohmann_json
  PRIVATE
    prometheus-cpp::core)

set_project_warnings(${PROJECT_NAME})

# ── Peer discovery (object lib — shared by server and tests) ─────────────────
add_library(agamemnon_peer_discovery OBJECT src/peer_discovery.cpp)
target_compile_features(agamemnon_peer_discovery PUBLIC cxx_std_20)
set_target_properties(agamemnon_peer_discovery PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(agamemnon_peer_discovery PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_link_libraries(agamemnon_peer_discovery PUBLIC nlohmann_json::nlohmann_json)
target_compile_options(agamemnon_peer_discovery PRIVATE
  -Wno-old-style-cast
  -Wno-conversion
  -Wno-sign-conversion
)

# ── ProjectAgamemnon_core — shared by server executable and tests ─────────────
# Excludes server_main.cpp so gtest_main does not conflict with main().
add_library(ProjectAgamemnon_core STATIC
  src/routes.cpp
  src/store.cpp
  src/nats_client.cpp
  src/circuit_breaker.cpp
  src/dead_letter_queue.cpp
  src/github_client.cpp
  src/rate_limiter.cpp
  src/auth.cpp
  src/metrics.cpp
  src/hmas_types.cpp
  src/orchestrator.cpp
  src/planning_breakdown.cpp
  src/state_machine.cpp
  src/agents/agent_envelope.cpp
)

target_compile_features(ProjectAgamemnon_core PUBLIC cxx_std_20)
set_target_properties(ProjectAgamemnon_core PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(ProjectAgamemnon_core
  PUBLIC  ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(ProjectAgamemnon_core
  PUBLIC
    httplib::httplib
    nlohmann_json::nlohmann_json
    nats_static
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    prometheus-cpp::core
)

target_compile_options(ProjectAgamemnon_core PRIVATE
  -Wno-old-style-cast
  -Wno-useless-cast
  -Wno-conversion
  -Wno-sign-conversion
  -Wno-shadow
  -Wno-format-nonliteral
  -Wno-double-promotion
  -Wno-cast-align
)

# ── agamemnon_agents — ported HMAS C++ agent executors + work-stealing ───────
# ADR-015 (HomericIntelligence/Odysseus#143): the L0→L3 HMAS agent hierarchy,
# coordination/strategy types, and the work-stealing scheduler were extracted
# from ProjectKeystone into ProjectAgamemnon. These are the runtime *executors*
# that complement Agamemnon's task-as-data Orchestrator/PlanningBreakdown.
#
# Standalone build: agent-to-agent transport depends only on the abstract
# core::IMessageRouter (impl: core::InProcessRouter, vendored here) — NOT on
# Keystone's MessageBus/NATS C++ library. The only new third-party dependency
# is the header-only concurrentqueue (same lib Keystone used). Sources retain
# the keystone:: namespace to mark provenance and minimise the port surface.
add_library(agamemnon_agents STATIC
  # core primitives
  src/core/message.cpp
  src/core/metrics.cpp
  src/core/failure_injector.cpp
  # concurrency / work-stealing scheduler
  src/concurrency/logger.cpp
  src/concurrency/scheduler_accessor.cpp
  src/concurrency/work_stealing_queue.cpp
  src/concurrency/pull_or_steal.cpp
  src/concurrency/work_stealing_scheduler.cpp
  # HMAS agent hierarchy (L0 → L3) + strategies
  src/agents/agent_core.cpp
  src/agents/async_agent.cpp
  src/agents/chief_architect_agent.cpp
  src/agents/component_lead_agent.cpp
  src/agents/module_lead_agent.cpp
  src/agents/task_agent.cpp
  src/agents/task_execution_strategy.cpp
)

target_compile_features(agamemnon_agents PUBLIC cxx_std_20)
set_target_properties(agamemnon_agents PROPERTIES CXX_EXTENSIONS OFF)

# Vendored port: exempt from clang-tidy / cppcheck. These sources are a
# byte-faithful ADR-015 port of Keystone (legacy cast/style idioms preserved
# for parity); they are linted in their origin repo, not re-linted here.
set_target_properties(agamemnon_agents PROPERTIES
  CXX_CLANG_TIDY ""
  CXX_CPPCHECK "")

# The ported sources use Keystone-relative includes ("agents/...", "core/...",
# "concurrency/...") rooted at the vendored include/agamemnon_agents tree.
target_include_directories(agamemnon_agents
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/agamemnon_agents>
)

target_link_libraries(agamemnon_agents
  PUBLIC
    concurrentqueue::concurrentqueue
    Threads::Threads
)

# Suppress external-header / legacy-cast warnings (matches other Agamemnon
# targets); the ported code is treated as vendored.
target_compile_options(agamemnon_agents PRIVATE
  -Wno-old-style-cast
  -Wno-useless-cast
  -Wno-conversion
  -Wno-sign-conversion
  -Wno-shadow
  -Wno-format-nonliteral
  -Wno-double-promotion
  -Wno-cast-align
  -Wno-unused-parameter
)

# ── Server executable ────────────────────────────────────────────────────────
add_executable(${PROJECT_NAME}_server src/server_main.cpp)

target_compile_features(${PROJECT_NAME}_server PRIVATE cxx_std_20)
set_target_properties(${PROJECT_NAME}_server PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(
  ${PROJECT_NAME}_server
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(${PROJECT_NAME}_server PRIVATE ProjectAgamemnon_core agamemnon_peer_discovery agamemnon_agents)

# Suppress warnings from external headers on the server target
target_compile_options(${PROJECT_NAME}_server PRIVATE
  -Wno-old-style-cast
  -Wno-useless-cast
  -Wno-conversion
  -Wno-sign-conversion
  -Wno-shadow
  -Wno-format-nonliteral
  -Wno-double-promotion
  -Wno-cast-align
)


# Suppress warnings from external headers on the server target
target_compile_options(${PROJECT_NAME}_server PRIVATE
  -Wno-old-style-cast
  -Wno-useless-cast
  -Wno-conversion
  -Wno-sign-conversion
  -Wno-shadow
  -Wno-format-nonliteral
  -Wno-double-promotion
  -Wno-cast-align
)

# ── Healthcheck binary ───────────────────────────────────────────────────────
add_executable(${PROJECT_NAME}_healthcheck src/healthcheck_main.cpp)

target_compile_features(${PROJECT_NAME}_healthcheck PRIVATE cxx_std_20)
set_target_properties(${PROJECT_NAME}_healthcheck PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(
  ${PROJECT_NAME}_healthcheck
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(
  ${PROJECT_NAME}_healthcheck
  PRIVATE
    httplib::httplib
    OpenSSL::SSL
    OpenSSL::Crypto
)

target_compile_options(${PROJECT_NAME}_healthcheck PRIVATE
  -Wno-old-style-cast
  -Wno-useless-cast
  -Wno-conversion
  -Wno-sign-conversion
  -Wno-shadow
  -Wno-double-promotion
  -Wno-cast-align
)

install(TARGETS ${PROJECT_NAME}_healthcheck
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ── Tests ────────────────────────────────────────────────────────────────────
if(${PROJECT_NAME}_BUILD_TESTING)
  enable_testing()
  add_subdirectory(test)
endif()

if(${PROJECT_NAME}_ENABLE_DOXYGEN)
  enable_doxygen()
endif()

# ── Install ──────────────────────────────────────────────────────────────────
include(GNUInstallDirs)
install(
  TARGETS ${PROJECT_NAME}
  EXPORT ${PROJECT_NAME}Targets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(
  EXPORT ${PROJECT_NAME}Targets
  FILE ${PROJECT_NAME}Targets.cmake
  NAMESPACE ${PROJECT_NAME}::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME})

install(TARGETS ${PROJECT_NAME}_server
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
