cmake_minimum_required(VERSION 3.20)
project(AgamemnonFleetContractTests LANGUAGES CXX)
enable_testing()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_path(JSON_INCLUDE nlohmann/json.hpp REQUIRED)
find_path(HTTPLIB_INCLUDE httplib.h PATH_SUFFIXES include/httplib REQUIRED)
find_path(GTEST_INCLUDE gtest/gtest.h REQUIRED)
find_path(PROMETHEUS_INCLUDE prometheus/registry.h REQUIRED)
find_library(GTEST_LIBRARY gtest REQUIRED)
find_library(GTEST_MAIN_LIBRARY gtest_main REQUIRED)
find_library(PROMETHEUS_LIBRARY prometheus-cpp-core REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(CORE_SOURCES routes store github_client github_webhook auth rate_limiter metrics
    orchestrator hmas_types planning_breakdown state_machine circuit_breaker dead_letter_queue)
foreach(SOURCE IN LISTS CORE_SOURCES)
  list(APPEND SOURCES ${ROOT}/src/${SOURCE}.cpp)
endforeach()
list(APPEND SOURCES ${ROOT}/src/fleet.cpp ${ROOT}/src/projects.cpp ${ROOT}/src/durable_epics.cpp)
add_library(fleet_native_core STATIC ${SOURCES} ${ROOT}/src/agents/agent_envelope.cpp)
target_include_directories(fleet_native_core PUBLIC ${ROOT}/include ${ROOT}/src)
target_include_directories(fleet_native_core SYSTEM PUBLIC
    ${JSON_INCLUDE} ${HTTPLIB_INCLUDE} ${PROMETHEUS_INCLUDE})
target_link_libraries(fleet_native_core PUBLIC
    ${PROMETHEUS_LIBRARY} OpenSSL::SSL OpenSSL::Crypto CURL::libcurl Threads::Threads)
add_executable(fleet_contract_tests ${ROOT}/test/src/test_fleet.cpp
    ${ROOT}/test/src/test_durable_epics.cpp
    ${ROOT}/test/src/test_projects.cpp
    ${ROOT}/test/src/test_parse_port.cpp
    ${ROOT}/test/src/test_github_client.cpp
    ${ROOT}/test/src/test_store.cpp
    ${ROOT}/test/src/test_store_edge_cases.cpp
    ${ROOT}/test/src/test_store_hydration.cpp
    ${ROOT}/test/src/test_orchestrator_persistence.cpp
    ${ROOT}/test/src/test_orchestrator.cpp ${ROOT}/test/src/test_hmas_types.cpp)
target_include_directories(fleet_contract_tests SYSTEM PRIVATE ${GTEST_INCLUDE})
target_link_libraries(fleet_contract_tests PRIVATE fleet_native_core ${GTEST_MAIN_LIBRARY} ${GTEST_LIBRARY})
add_test(NAME fleet_contract COMMAND fleet_contract_tests)
add_executable(fleet_fact_import import_bridge_facts.cpp)
target_link_libraries(fleet_fact_import PRIVATE fleet_native_core)

set(FLEET_NATIVE_NATS_PREFIX "" CACHE PATH "Optional existing nats.c build; never fetched here")
if(FLEET_NATIVE_NATS_PREFIX)
  find_path(NATS_INCLUDE nats.h HINTS ${FLEET_NATIVE_NATS_PREFIX}/_deps/natsc-src/src REQUIRED)
  find_library(NATS_LIBRARY NAMES nats_staticd nats_static natsd nats
      HINTS ${FLEET_NATIVE_NATS_PREFIX}/lib REQUIRED)
  add_executable(fleet_native_server ${ROOT}/src/server_main.cpp ${ROOT}/src/nats_client.cpp
      ${ROOT}/src/peer_discovery.cpp)
  target_include_directories(fleet_native_server SYSTEM PRIVATE ${NATS_INCLUDE})
  target_link_libraries(fleet_native_server PRIVATE fleet_native_core ${NATS_LIBRARY})
  find_library(NATS_DYNAMIC_LIBRARY NAMES natsd nats HINTS ${FLEET_NATIVE_NATS_PREFIX}/lib REQUIRED)
  add_executable(fleet_nats_endpoint_tests ${ROOT}/src/nats_client.cpp
      ${ROOT}/test/src/test_nats_explicit_url.cpp)
  target_include_directories(fleet_nats_endpoint_tests SYSTEM PRIVATE ${NATS_INCLUDE} ${GTEST_INCLUDE})
  target_link_libraries(fleet_nats_endpoint_tests PRIVATE fleet_native_core
      ${GTEST_MAIN_LIBRARY} ${GTEST_LIBRARY} ${NATS_DYNAMIC_LIBRARY})
  add_test(NAME fleet_nats_endpoint COMMAND fleet_nats_endpoint_tests)
  add_executable(fleet_epic_import ${ROOT}/src/nats_client.cpp import_epic.cpp)
  target_include_directories(fleet_epic_import SYSTEM PRIVATE ${NATS_INCLUDE})
  target_link_libraries(fleet_epic_import PRIVATE fleet_native_core ${NATS_LIBRARY})
  add_executable(fleet_jetstream_tests ${ROOT}/src/nats_client.cpp ${ROOT}/test/src/test_nats_durable.cpp)
  target_include_directories(fleet_jetstream_tests SYSTEM PRIVATE ${NATS_INCLUDE} ${GTEST_INCLUDE})
  target_link_libraries(fleet_jetstream_tests PRIVATE fleet_native_core
      ${GTEST_MAIN_LIBRARY} ${GTEST_LIBRARY} ${NATS_LIBRARY})
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(AGAMEMNON_TEST_NATS_SERVER "nats-server" CACHE STRING
      "Broker executable used only for isolated JetStream test processes")
  add_test(NAME fleet_jetstream COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_LIST_DIR}/private_jetstream.py
      ${AGAMEMNON_TEST_NATS_SERVER} $<TARGET_FILE:fleet_jetstream_tests>)
  set_tests_properties(fleet_jetstream PROPERTIES LABELS "integration" TIMEOUT 60 RUN_SERIAL TRUE)
endif()
