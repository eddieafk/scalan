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
  KIND inval
  NAME indexed-byte-buffer-diagnostics
  SOURCE inval/IndexedByteBufferDiagnostics.scala)

scalanative_add_scala_test(
  VERSION "${test_version}"
  KIND inval
  NAME byte-buffer-mark-diagnostics
  SOURCE inval/ByteBufferMarkDiagnostics.scala)

scalanative_add_scala_test(
  VERSION "${test_version}"
  KIND inval
  NAME relative-short-byte-buffer-diagnostics
  SOURCE inval/RelativeShortByteBufferDiagnostics.scala)

scalanative_add_scala_test(
  VERSION "${test_version}"
  KIND inval
  NAME indexed-short-byte-buffer-diagnostics
  SOURCE inval/IndexedShortByteBufferDiagnostics.scala)

scalanative_add_scala_test(
  VERSION "${test_version}"
  KIND inval
  NAME relative-int-byte-buffer-diagnostics
  SOURCE inval/RelativeIntByteBufferDiagnostics.scala)

scalanative_add_scala_test(
  VERSION "${test_version}"
  KIND inval
  NAME indexed-int-byte-buffer-diagnostics
  SOURCE inval/IndexedIntByteBufferDiagnostics.scala)

scalanative_add_scala_test(
  VERSION "${test_version}"
  KIND inval
  NAME relative-long-byte-buffer-diagnostics
  SOURCE inval/RelativeLongByteBufferDiagnostics.scala)

scalanative_add_scala_test(
  VERSION "${test_version}"
  KIND inval
  NAME indexed-long-byte-buffer-diagnostics
  SOURCE inval/IndexedLongByteBufferDiagnostics.scala)

scalanative_add_scala_test(
  VERSION "${test_version}"
  KIND inval
  NAME relative-float-byte-buffer-diagnostics
  SOURCE inval/RelativeFloatByteBufferDiagnostics.scala)

scalanative_add_scala_test(
  VERSION "${test_version}"
  KIND run
  NAME captured-runtime-polymorphic-functions
  SOURCE run/CapturedRuntimePolymorphicFunctions.scala)

scalanative_add_scala_test(
  VERSION "${test_version}"
  KIND run
  NAME indexed-byte-buffer-access
  SOURCE run/IndexedByteBufferAccess.scala)

scalanative_add_scala_test(
  VERSION "${test_version}"
  KIND run
  NAME byte-buffer-mark-reset
  SOURCE run/ByteBufferMarkReset.scala)

scalanative_add_scala_test(
  VERSION "${test_version}"
  KIND run
  NAME relative-short-byte-buffer-access
  SOURCE run/RelativeShortByteBufferAccess.scala)

scalanative_add_scala_test(
  VERSION "${test_version}"
  KIND run
  NAME indexed-short-byte-buffer-access
  SOURCE run/IndexedShortByteBufferAccess.scala)

scalanative_add_scala_test(
  VERSION "${test_version}"
  KIND run
  NAME relative-int-byte-buffer-access
  SOURCE run/RelativeIntByteBufferAccess.scala)

scalanative_add_scala_test(
  VERSION "${test_version}"
  KIND run
  NAME indexed-int-byte-buffer-access
  SOURCE run/IndexedIntByteBufferAccess.scala)

scalanative_add_scala_test(
  VERSION "${test_version}"
  KIND run
  NAME relative-long-byte-buffer-access
  SOURCE run/RelativeLongByteBufferAccess.scala)

scalanative_add_scala_test(
  VERSION "${test_version}"
  KIND run
  NAME indexed-long-byte-buffer-access
  SOURCE run/IndexedLongByteBufferAccess.scala)

scalanative_add_scala_test(
  VERSION "${test_version}"
  KIND run
  NAME relative-float-byte-buffer-access
  SOURCE run/RelativeFloatByteBufferAccess.scala)

scalanative_add_smoke_test(
  VERSION "${test_version}"
  NAME captured-runtime-polymorphic-closure-nir
  SOURCE "${CMAKE_CURRENT_LIST_DIR}/../smoke/CapturedRuntimePolymorphicClosureSmoke.cpp")

scalanative_add_smoke_test(
  VERSION "${test_version}"
  NAME indexed-byte-buffer-lowering
  SOURCE "${CMAKE_CURRENT_LIST_DIR}/../smoke/IndexedByteBufferAccessSmoke.cpp")

scalanative_add_smoke_test(
  VERSION "${test_version}"
  NAME byte-buffer-mark-reset-lowering
  SOURCE "${CMAKE_CURRENT_LIST_DIR}/../smoke/ByteBufferMarkResetSmoke.cpp")

scalanative_add_smoke_test(
  VERSION "${test_version}"
  NAME relative-short-byte-buffer-lowering
  SOURCE "${CMAKE_CURRENT_LIST_DIR}/../smoke/RelativeShortByteBufferAccessSmoke.cpp")

scalanative_add_smoke_test(
  VERSION "${test_version}"
  NAME indexed-short-byte-buffer-lowering
  SOURCE "${CMAKE_CURRENT_LIST_DIR}/../smoke/IndexedShortByteBufferAccessSmoke.cpp")

scalanative_add_smoke_test(
  VERSION "${test_version}"
  NAME relative-int-byte-buffer-lowering
  SOURCE "${CMAKE_CURRENT_LIST_DIR}/../smoke/RelativeIntByteBufferAccessSmoke.cpp")

scalanative_add_smoke_test(
  VERSION "${test_version}"
  NAME indexed-int-byte-buffer-lowering
  SOURCE "${CMAKE_CURRENT_LIST_DIR}/../smoke/IndexedIntByteBufferAccessSmoke.cpp")

scalanative_add_smoke_test(
  VERSION "${test_version}"
  NAME relative-long-byte-buffer-lowering
  SOURCE "${CMAKE_CURRENT_LIST_DIR}/../smoke/RelativeLongByteBufferAccessSmoke.cpp")

scalanative_add_smoke_test(
  VERSION "${test_version}"
  NAME indexed-long-byte-buffer-lowering
  SOURCE "${CMAKE_CURRENT_LIST_DIR}/../smoke/IndexedLongByteBufferAccessSmoke.cpp")

scalanative_add_smoke_test(
  VERSION "${test_version}"
  NAME relative-float-byte-buffer-lowering
  SOURCE "${CMAKE_CURRENT_LIST_DIR}/../smoke/RelativeFloatByteBufferAccessSmoke.cpp")

add_custom_target(
  scalanative-tests-v0.1.0-alpha0.1.0
  DEPENDS scalanative-scala-test-runner
          scalanative-smoke-v0_1_0_alpha0_1_0_captured_runtime_polymorphic_closure_nir
          scalanative-smoke-v0_1_0_alpha0_1_0_indexed_byte_buffer_lowering
          scalanative-smoke-v0_1_0_alpha0_1_0_byte_buffer_mark_reset_lowering
          scalanative-smoke-v0_1_0_alpha0_1_0_relative_short_byte_buffer_lowering
          scalanative-smoke-v0_1_0_alpha0_1_0_indexed_short_byte_buffer_lowering
          scalanative-smoke-v0_1_0_alpha0_1_0_relative_int_byte_buffer_lowering
          scalanative-smoke-v0_1_0_alpha0_1_0_indexed_int_byte_buffer_lowering
          scalanative-smoke-v0_1_0_alpha0_1_0_relative_long_byte_buffer_lowering
          scalanative-smoke-v0_1_0_alpha0_1_0_indexed_long_byte_buffer_lowering
          scalanative-smoke-v0_1_0_alpha0_1_0_relative_float_byte_buffer_lowering)
