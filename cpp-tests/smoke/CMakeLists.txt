# SmokeTests.cpp is retained as a broad reference suite. New coverage goes into
# numbered translation units so incremental builds do not recompile the entire
# active suite.
add_executable(
  cpp-smoke-tests
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
  src/SmokeTests19.cpp)

target_link_libraries(
  cpp-smoke-tests
  PRIVATE cpp-scalanative::frontend cpp-scalanative::tools-build
          cpp-scalanative::tools-linker cpp-scalanative::tools-interflow
          cpp-scalanative::nir cpp-scalanative::support)

cpp_scalanative_target_defaults(cpp-smoke-tests)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  # The harness drives separately configured compiler libraries; production
  # optimization and debug symbols only increase its edit-build latency without
  # changing the compiler code under test.
  target_compile_options(cpp-smoke-tests PRIVATE -O0 -g0)
endif()

add_test(NAME cpp-smoke-tests COMMAND cpp-smoke-tests)

add_test(
  NAME cpp-fixture-hello
  COMMAND $<TARGET_FILE:cpp-scalanative> --emit-nir
          ${CMAKE_SOURCE_DIR}/cpp-tests/fixtures/Hello.scala)

add_test(
  NAME cpp-fixture-semantics
  COMMAND $<TARGET_FILE:cpp-scalanative> --emit-nir
          ${CMAKE_SOURCE_DIR}/cpp-tests/fixtures/Semantics.scala)

add_test(
  NAME cpp-fixture-control
  COMMAND $<TARGET_FILE:cpp-scalanative> --emit-nir
          ${CMAKE_SOURCE_DIR}/cpp-tests/fixtures/Control.scala)

add_test(
  NAME cpp-fixture-config
  COMMAND $<TARGET_FILE:cpp-scalanative> --config
          ${CMAKE_SOURCE_DIR}/cpp-tests/fixtures/build-config.json --opt-level 1
          --no-debug-info)
