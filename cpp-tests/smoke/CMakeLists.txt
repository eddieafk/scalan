# SmokeTests.cpp is retained as a broad reference suite. The default smoke target
# uses the lightweight active subset to keep edit-build-test cycles fast.
add_executable(cpp-smoke-tests src/SmokeTests2.cpp)

target_link_libraries(
  cpp-smoke-tests
  PRIVATE cpp-scalanative::frontend cpp-scalanative::tools-build
          cpp-scalanative::tools-linker cpp-scalanative::tools-interflow
          cpp-scalanative::nir cpp-scalanative::support)

cpp_scalanative_target_defaults(cpp-smoke-tests)

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
