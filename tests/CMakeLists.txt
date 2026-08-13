add_executable(scalanative-scala-test-runner runner/ScalaTestRunner.cpp)
target_include_directories(
  scalanative-scala-test-runner PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(
  scalanative-scala-test-runner
  PRIVATE SCALANATIVE_TEST_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  scalanative-scala-test-runner
  PRIVATE scalanative::tools-build scalanative::support)
scalanative_target_defaults(scalanative-scala-test-runner)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(scalanative-scala-test-runner PRIVATE -O0 -g0)
endif()

function(scalanative_add_scala_test)
  set(one_value_args VERSION KIND NAME SOURCE)
  cmake_parse_arguments(TEST "" "${one_value_args}" "" ${ARGN})

  foreach(required VERSION KIND NAME SOURCE)
    if(NOT TEST_${required})
      message(FATAL_ERROR "scalanative_add_scala_test requires ${required}")
    endif()
  endforeach()

  set(supported_kinds val inval run)
  if(NOT TEST_KIND IN_LIST supported_kinds)
    message(FATAL_ERROR "unsupported Scala test kind: ${TEST_KIND}")
  endif()

  if(IS_ABSOLUTE "${TEST_SOURCE}")
    set(source "${TEST_SOURCE}")
  else()
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/${TEST_SOURCE}")
  endif()
  if(NOT EXISTS "${source}")
    message(FATAL_ERROR "Scala test resource does not exist: ${source}")
  endif()

  set(test_name "${TEST_VERSION}.${TEST_KIND}.${TEST_NAME}")
  add_test(
    NAME "${test_name}"
    COMMAND scalanative-scala-test-runner --kind "${TEST_KIND}" --name
            "${test_name}" --source "${source}")
  set_tests_properties(
    "${test_name}"
    PROPERTIES LABELS "${TEST_VERSION};${TEST_KIND};scala"
               SKIP_RETURN_CODE 77)
endfunction()

function(scalanative_add_smoke_test)
  set(one_value_args VERSION NAME SOURCE)
  cmake_parse_arguments(TEST "" "${one_value_args}" "" ${ARGN})

  foreach(required VERSION NAME SOURCE)
    if(NOT TEST_${required})
      message(FATAL_ERROR "scalanative_add_smoke_test requires ${required}")
    endif()
  endforeach()

  if(IS_ABSOLUTE "${TEST_SOURCE}")
    set(source "${TEST_SOURCE}")
  else()
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/${TEST_SOURCE}")
  endif()
  if(NOT EXISTS "${source}")
    message(FATAL_ERROR "C++ smoke test does not exist: ${source}")
  endif()

  string(MAKE_C_IDENTIFIER "${TEST_VERSION}_${TEST_NAME}" target_suffix)
  set(target "scalanative-smoke-${target_suffix}")
  set(test_name "${TEST_VERSION}.smoke.${TEST_NAME}")

  add_executable("${target}" "${source}")
  target_include_directories(
    "${target}" PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/include")
  target_compile_definitions(
    "${target}"
    PRIVATE SCALANATIVE_TEST_ROOT="${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
  target_link_libraries(
    "${target}"
    PRIVATE scalanative::tools-build scalanative::support)
  scalanative_target_defaults("${target}")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options("${target}" PRIVATE -O0 -g0)
  endif()

  add_test(NAME "${test_name}" COMMAND "${target}")
  set_tests_properties(
    "${test_name}"
    PROPERTIES LABELS "${TEST_VERSION};smoke;cpp" SKIP_RETURN_CODE 77)
endfunction()

add_subdirectory(v0.1.0-alpha0.1.0)
