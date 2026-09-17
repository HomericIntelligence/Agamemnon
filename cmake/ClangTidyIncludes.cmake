# Generates the list of --extra-arg=-isystem<dir> arguments forwarded to
# clang-tidy by parsing the live C++ compiler's own preprocessor search list
# (`${CMAKE_CXX_COMPILER} -E -x c++ -Wp,-v /dev/null`). GCC reports every
# directory it searches — libstdc++, multilib, include-fixed, and the compiler
# builtin includes (where stddef.h lives) — which a single
# `-print-file-name=include` lookup misses. Under conda/pixi GCC sysroots that
# omission made clang-tidy fail with "'stddef.h' file not found" (#211).
#
# On non-GCC compilers (or when the markers are absent) the variable is left
# empty and clang-tidy falls back to its built-in include resolution; the
# generated file is still written (with only its header) so consumers can
# validate the format version.

set(_AGAMEMNON_WPV_START_MARKER "#include <...> search starts here:")
set(_AGAMEMNON_WPV_END_MARKER "End of search list.")

function(_agamemnon_collect_clang_tidy_extra_args out_var)
  execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} -E -x c++ -Wp,-v /dev/null
    OUTPUT_QUIET
    ERROR_VARIABLE _wpv
    RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(WARNING
      "clang-tidy: '${CMAKE_CXX_COMPILER} -E -Wp,-v' returned ${_rc}; "
      "no extra system includes forwarded to clang-tidy")
    set(${out_var} "" PARENT_SCOPE)
    return()
  endif()

  string(FIND "${_wpv}" "${_AGAMEMNON_WPV_START_MARKER}" _start)
  string(FIND "${_wpv}" "${_AGAMEMNON_WPV_END_MARKER}" _end)
  if(_start LESS 0 OR _end LESS 0 OR _end LESS _start)
    message(STATUS
      "clang-tidy: -Wp,-v search-list markers not found for "
      "${CMAKE_CXX_COMPILER}; clang-tidy will rely on built-in include resolution")
    set(${out_var} "" PARENT_SCOPE)
    return()
  endif()

  string(LENGTH "${_AGAMEMNON_WPV_START_MARKER}" _marker_len)
  math(EXPR _body_start "${_start} + ${_marker_len}")
  math(EXPR _slice_len "${_end} - ${_body_start}")
  string(SUBSTRING "${_wpv}" ${_body_start} ${_slice_len} _body)

  string(REPLACE "\r" "" _body "${_body}")
  string(REPLACE "\n" ";" _lines "${_body}")

  set(_args "")
  foreach(_line IN LISTS _lines)
    string(STRIP "${_line}" _line)
    if(_line STREQUAL "")
      continue()
    endif()
    if(_line MATCHES "\\(framework directory\\)$")
      continue()
    endif()
    if(NOT IS_ABSOLUTE "${_line}")
      # Defensive: skip anything that is not a real path.
      continue()
    endif()
    list(APPEND _args "--extra-arg=-isystem${_line}")
  endforeach()

  if(_args)
    list(REMOVE_DUPLICATES _args)
  endif()
  set(${out_var} "${_args}" PARENT_SCOPE)
endfunction()

_agamemnon_collect_clang_tidy_extra_args(AGAMEMNON_CLANG_TIDY_EXTRA_ARGS)

list(LENGTH AGAMEMNON_CLANG_TIDY_EXTRA_ARGS _n)
message(STATUS
  "clang-tidy: forwarding ${_n} system include dirs from ${CMAKE_CXX_COMPILER}")
if(_n GREATER 0)
  message(VERBOSE "clang-tidy extra args: ${AGAMEMNON_CLANG_TIDY_EXTRA_ARGS}")
endif()

# Persist for scripts/lint.sh (and the ctest smoke driver) with a format-version
# header so consumers can reject stale or corrupted files.
set(_file "${CMAKE_BINARY_DIR}/clang-tidy-extra-args.txt")
set(_file_body "# format-version: 1\n")
string(APPEND _file_body "# generator: cmake/ClangTidyIncludes.cmake\n")
string(APPEND _file_body "# compiler: ${CMAKE_CXX_COMPILER}\n")
foreach(_a IN LISTS AGAMEMNON_CLANG_TIDY_EXTRA_ARGS)
  string(APPEND _file_body "${_a}\n")
endforeach()
file(WRITE "${_file}" "${_file_body}")
