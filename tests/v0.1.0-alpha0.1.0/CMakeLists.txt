set(test_version v0.1.0-alpha0.1.0)

scalanative_add_scala_test(
  VERSION "${test_version}"
  KIND val
  NAME polymorphic-function-types
  SOURCE val/PolymorphicFunctionTypes.scala)

scalanative_add_scala_test(
  VERSION "${test_version}"
  KIND inval
  NAME runtime-polymorphic-capture-diagnostics
  SOURCE inval/RuntimePolymorphicCaptureDiagnostics.scala)

scalanative_add_scala_test(
  VERSION "${test_version}"
  KIND run
  NAME captured-runtime-polymorphic-functions
  SOURCE run/CapturedRuntimePolymorphicFunctions.scala)

scalanative_add_smoke_test(
  VERSION "${test_version}"
  NAME captured-runtime-polymorphic-closure-nir
  SOURCE "${CMAKE_CURRENT_LIST_DIR}/../smoke/CapturedRuntimePolymorphicClosureSmoke.cpp")

add_custom_target(
  scalanative-tests-v0.1.0-alpha0.1.0
  DEPENDS scalanative-scala-test-runner
          scalanative-smoke-v0_1_0_alpha0_1_0_captured_runtime_polymorphic_closure_nir)
