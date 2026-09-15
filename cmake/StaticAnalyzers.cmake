# Default OFF: clang-tidy costs 55-95s per translation unit, and the Build and
# Test matrix (6 entries x debug/release x gcc/clang, plus ASAN) inherits this
# default. That pinned every matrix job at its 30-minute timeout-minutes cap and
# cancelled them under normal concurrent PR load, so the required
# ubuntu-24.04-* contexts never reported and PRs sat permanently BLOCKED (#515).
#
# clang-tidy is enforced by the two jobs that opt in explicitly, so nothing is
# lost by defaulting it off here:
#   - .github/workflows/_required.yml  -> the required `lint` job
#   - .github/workflows/static-analysis.yml -> `clang-tidy`, feeds
#     "All Static Analysis Checks"
# The Dockerfile and scripts/lint.sh already pass -D...=OFF for the same reason.
# Pass -D${PROJECT_NAME}_ENABLE_CLANG_TIDY=ON to build/analyse locally.
option(${PROJECT_NAME}_ENABLE_CLANG_TIDY "Enable clang-tidy" OFF)

# cppcheck gets the same treatment for the same reason (#517). Nothing installs it --
# no workflow does, and the ubuntu-24.04 runner image does not ship it -- so this branch
# currently only reaches `message(WARNING "cppcheck not found")`. Defaulting it ON
# anyway means that the moment anyone installs cppcheck, *every* build configuration
# (the six-entry Build and Test matrix plus install, package, release, sanitizers,
# coverage, CodeQL and integration tests) starts running it per translation unit. That
# is precisely how #515 pinned those jobs at their 30-minute cap. Opt in explicitly with
# -D${PROJECT_NAME}_ENABLE_CPPCHECK=ON.
option(${PROJECT_NAME}_ENABLE_CPPCHECK "Enable cppcheck" OFF)

if(${PROJECT_NAME}_ENABLE_CLANG_TIDY)
  find_program(CLANGTIDY clang-tidy)
  if(CLANGTIDY)
    # Query GCC's built-in include directory to pass to clang-tidy
    # This ensures clang-tidy can find sysroot headers like stddef.h
    execute_process(
      COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=include
      OUTPUT_VARIABLE GCC_INCLUDE_DIR
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(CMAKE_CXX_CLANG_TIDY ${CLANGTIDY} --extra-arg=-Wno-unknown-warning-option --extra-arg=-isystem${GCC_INCLUDE_DIR})
  else()
    message(WARNING "clang-tidy not found")
  endif()
endif()

if(${PROJECT_NAME}_ENABLE_CPPCHECK)
  find_program(CPPCHECK cppcheck)
  if(CPPCHECK)
    set(CMAKE_CXX_CPPCHECK ${CPPCHECK} --suppress=missingInclude --enable=all
                           --inline-suppr --inconclusive)
  else()
    message(WARNING "cppcheck not found")
  endif()
endif()
