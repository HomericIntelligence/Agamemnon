# Regression guard for #211: run clang-tidy on a minimal TU that exercises the
# headers which failed to resolve under conda/pixi GCC sysroots
# (<cstddef> -> bits/c++config.h / stddef.h, <string>).
#
# Invoked by CTest (registered from cmake/StaticAnalyzers.cmake):
#   clang_tidy_stddef_smoke
#
# Required arguments (passed with -D by the registering add_test):
#   CLANG_TIDY_CMD   - path to the clang-tidy binary CMake found
#   EXTRA_ARGS_FILE  - generated clang-tidy-extra-args.txt (format-version: 1)
#   TU               - path to test/clang_tidy_smoke_tu.cpp

if(NOT DEFINED CLANG_TIDY_CMD OR NOT DEFINED EXTRA_ARGS_FILE OR NOT DEFINED TU)
  message(FATAL_ERROR "clang_tidy_stddef_smoke: CLANG_TIDY_CMD, EXTRA_ARGS_FILE and TU are required")
endif()

if(NOT EXISTS "${EXTRA_ARGS_FILE}")
  message(FATAL_ERROR "clang_tidy_stddef_smoke: extra-args file not found: ${EXTRA_ARGS_FILE} (re-run cmake configure)")
endif()

file(READ "${EXTRA_ARGS_FILE}" _raw)
string(REPLACE "\n" ";" _lines "${_raw}")

set(_extras "")
foreach(_line IN LISTS _lines)
  if(_line MATCHES "^--extra-arg=-isystem/")
    list(APPEND _extras "${_line}")
  endif()
endforeach()

# -std=c++17 (not c++20): older clang-tidy releases reject the c++20 spelling;
# it is sufficient for the headers this TU exercises.
execute_process(
  COMMAND ${CLANG_TIDY_CMD} ${_extras} ${TU} -- -std=c++17
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _err)

if(_out MATCHES "(stddef\\.h|cstddef|bits/c\\+\\+config\\.h)[^\n]*file not found"
   OR _err MATCHES "(stddef\\.h|cstddef|bits/c\\+\\+config\\.h)[^\n]*file not found")
  message(FATAL_ERROR "clang_tidy_stddef_smoke: header resolution regressed:\n${_out}${_err}")
endif()

if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "clang_tidy_stddef_smoke: clang-tidy exited ${_rc}:\n${_out}${_err}")
endif()

message(STATUS "clang_tidy_stddef_smoke: PASS (${_extras})")
