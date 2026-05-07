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
# cpp-httplib and nlohmann_json are provided by Conan (run `just deps` first).
# nats.c stays as FetchContent (not reliably available on ConanCenter).
find_package(httplib REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)

include(FetchContent)
FetchContent_Declare(
  nats_c
  GIT_REPOSITORY https://github.com/nats-io/nats.c.git
  GIT_TAG        v3.12.0
  GIT_SHALLOW    TRUE
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
  PRIVATE
    nlohmann_json::nlohmann_json)

set_project_warnings(${PROJECT_NAME})

# ── Server executable ────────────────────────────────────────────────────────
add_executable(${PROJECT_NAME}_server
  src/server_main.cpp
  src/routes.cpp
  src/nats_client.cpp
)

target_compile_features(${PROJECT_NAME}_server PRIVATE cxx_std_20)
set_target_properties(${PROJECT_NAME}_server PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(
  ${PROJECT_NAME}_server
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(
  ${PROJECT_NAME}_server
  PRIVATE
    ${PROJECT_NAME}::${PROJECT_NAME}
    httplib::httplib
    nlohmann_json::nlohmann_json
    nats_static
    OpenSSL::SSL
    OpenSSL::Crypto
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
