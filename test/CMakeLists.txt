cmake_minimum_required(VERSION 3.20)

find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
find_package(httplib REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(${PROJECT_NAME}_tests
  src/test_main.cpp
  src/test_store.cpp
  src/test_parse_port.cpp
  src/test_store_edge_cases.cpp
  src/test_store_concurrent.cpp
  src/test_routes_malformed.cpp
  src/test_peer_discovery.cpp
  ${PROJECT_SOURCE_DIR}/src/store.cpp
  ${PROJECT_SOURCE_DIR}/src/routes.cpp
  ${PROJECT_SOURCE_DIR}/src/nats_client.cpp)

target_compile_features(${PROJECT_NAME}_tests PRIVATE cxx_std_20)
set_target_properties(${PROJECT_NAME}_tests PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(${PROJECT_NAME}_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src)

target_compile_options(${PROJECT_NAME}_tests PRIVATE
  -Wno-old-style-cast
  -Wno-useless-cast
  -Wno-conversion
  -Wno-sign-conversion
  -Wno-shadow
  -Wno-format-nonliteral
  -Wno-double-promotion
  -Wno-cast-align)

target_link_libraries(${PROJECT_NAME}_tests
  PRIVATE
    ${PROJECT_NAME}::${PROJECT_NAME}
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    agamemnon_peer_discovery
    nats_static
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads)

include(GoogleTest)
gtest_discover_tests(${PROJECT_NAME}_tests DISCOVERY_TIMEOUT 120)
