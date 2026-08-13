# SmokeTests.cpp is retained as a broad reference suite. New coverage goes into
# numbered translation units so incremental builds do not recompile the entire
# active suite.
add_executable(
  smoke-tests
  src/SmokeTests2.cpp
  src/SmokeTests3.cpp
  src/SmokeTests4.cpp
  src/SmokeTests5.cpp
  src/SmokeTests6.cpp
  src/SmokeTests7.cpp
  src/SmokeTests8.cpp
  src/SmokeTests9.cpp
  src/SmokeTests10.cpp
  src/SmokeTests11.cpp
  src/SmokeTests12.cpp
  src/SmokeTests13.cpp
  src/SmokeTests14.cpp
  src/SmokeTests15.cpp
  src/SmokeTests16.cpp
  src/SmokeTests17.cpp
  src/SmokeTests18.cpp
  src/SmokeTests19.cpp
  src/SmokeTests20.cpp)

target_link_libraries(
  smoke-tests
  PRIVATE scalanative::frontend scalanative::tools-build
          scalanative::tools-linker scalanative::tools-interflow
          scalanative::nir scalanative::support)

scalanative_target_defaults(smoke-tests)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  # The harness drives separately configured compiler libraries; production
  # optimization and debug symbols only increase its edit-build latency without
  # changing the compiler code under test.
  target_compile_options(smoke-tests PRIVATE -O0 -g0)
endif()

add_test(NAME smoke-tests COMMAND smoke-tests)

add_test(
  NAME fixture-hello
  COMMAND $<TARGET_FILE:scalanative> --emit-nir
          ${CMAKE_SOURCE_DIR}/tests/v0/tests/fixtures/Hello.scala)

add_test(
  NAME fixture-semantics
  COMMAND $<TARGET_FILE:scalanative> --emit-nir
          ${CMAKE_SOURCE_DIR}/tests/v0/tests/fixtures/Semantics.scala)

add_test(
  NAME fixture-control
  COMMAND $<TARGET_FILE:scalanative> --emit-nir
          ${CMAKE_SOURCE_DIR}/tests/v0/tests/fixtures/Control.scala)

add_test(
  NAME fixture-config
  COMMAND $<TARGET_FILE:scalanative> --config
          ${CMAKE_SOURCE_DIR}/tests/v0/tests/fixtures/build-config.json --opt-level 1
          --no-debug-info)
