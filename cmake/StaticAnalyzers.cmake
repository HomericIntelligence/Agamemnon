option(${PROJECT_NAME}_ENABLE_CLANG_TIDY "Enable clang-tidy" ON)
option(${PROJECT_NAME}_ENABLE_CPPCHECK "Enable cppcheck" ON)

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
