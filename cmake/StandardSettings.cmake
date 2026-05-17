option(${PROJECT_NAME}_BUILD_TESTING "Build tests" ON)
option(${PROJECT_NAME}_ENABLE_DOXYGEN "Enable Doxygen documentation" OFF)
option(${PROJECT_NAME}_ENABLE_SANITIZERS "Enable sanitizers" OFF)
option(${PROJECT_NAME}_ENABLE_ASAN "Enable AddressSanitizer (ASAN)" OFF)
option(${PROJECT_NAME}_ENABLE_COVERAGE "Enable coverage reporting" OFF)
option(${PROJECT_NAME}_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)

if(${PROJECT_NAME}_ENABLE_COVERAGE)
  add_compile_options(-O0 --coverage)
  add_link_options(--coverage)
endif()

# AddressSanitizer -- catches dangling-reference / use-after-free regressions.
# Intended to guard against lambda captures of raw references in routes.cpp
# (see register_routes comment: raw Store*/NatsPublisher* are captured by value
# precisely to avoid this UB).
if(${PROJECT_NAME}_ENABLE_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer -O1 -g)
  add_link_options(-fsanitize=address)
endif()
