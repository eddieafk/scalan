#include "scalanative/support/Diagnostics.h"
#include "scalanative/testing/TestResources.h"
#include "scalanative/tools/build/BuildDriver.h"

#include <string>

namespace {

constexpr std::string_view TestName =
    "v0.1.0-alpha0.1.0.smoke.indexed-double-byte-buffer-lowering";

int indexedDoubleByteBufferLowering() {
  const scalanative::testing::TestResource resource{
      "v0.1.0-alpha0.1.0", "run", "IndexedDoubleByteBufferAccess.scala"};
  if (!std::filesystem::is_regular_file(resource.path())) {
    return scalanative::testing::fail(TestName, "missing Scala resource: " +
                                                    resource.path().string());
  }

  scalanative::tools::build::BuildOptions options;
  options.action = scalanative::tools::build::BuildAction::EmitLlvm;
  scalanative::support::DiagnosticEngine diagnostics;
  const scalanative::tools::build::BuildResult result =
      scalanative::tools::build::BuildDriver{}.buildFile(resource.path(), options,
                                                         diagnostics);
  if (!result.ok) {
    return scalanative::testing::fail(TestName, "LLVM emission failed:\n" +
                                                    result.diagnosticsText);
  }

  const bool indexedDoubleNir =
      scalanative::testing::contains(
          result.nirText, "scala.scalanative.runtime.byteBufferGetDoubleAt") &&
      scalanative::testing::contains(
          result.nirText, "scala.scalanative.runtime.byteBufferPutDoubleAt");
  const bool indexedDoubleLlvm =
      scalanative::testing::contains(
          result.llvmIr,
          "define internal double @__scalanative_byte_buffer_get_double_at") &&
      scalanative::testing::contains(
          result.llvmIr,
          "define internal ptr @__scalanative_byte_buffer_put_double_at") &&
      scalanative::testing::contains(
          result.llvmIr,
          "call double @__scalanative_native_bytes_get_double(ptr %array, i32 "
          "%backing_index, i1 %little_endian)") &&
      scalanative::testing::contains(
          result.llvmIr,
          "call void @__scalanative_native_bytes_put_double(ptr %array, i32 "
          "%backing_index, "
          "double %value, i1 %little_endian)") &&
      scalanative::testing::contains(
          result.llvmIr, "call void @__scalanative_throw_byte_buffer_index()") &&
      scalanative::testing::contains(result.llvmIr,
                                     "Runtime ABI = 'scalanative-runtime-70'");
  if (!indexedDoubleNir || !indexedDoubleLlvm) {
    return scalanative::testing::fail(
        TestName, "indexed Double ByteBuffer access was not lowered as expected:\n" +
                      result.nirText + "\n" + result.llvmIr);
  }
  return 0;
}

} // namespace

int main() {
  return indexedDoubleByteBufferLowering();
}
