#include "scalanative/support/Diagnostics.h"
#include "scalanative/testing/TestResources.h"
#include "scalanative/tools/build/BuildDriver.h"

#include <string>

namespace {

constexpr std::string_view TestName =
    "v0.1.0-alpha0.1.0.smoke.indexed-float-byte-buffer-lowering";

int indexedFloatByteBufferLowering() {
  const scalanative::testing::TestResource resource{
      "v0.1.0-alpha0.1.0", "run", "IndexedFloatByteBufferAccess.scala"};
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

  const bool indexedFloatNir =
      scalanative::testing::contains(
          result.nirText, "scala.scalanative.runtime.byteBufferGetFloatAt") &&
      scalanative::testing::contains(
          result.nirText, "scala.scalanative.runtime.byteBufferPutFloatAt");
  const bool indexedFloatLlvm =
      scalanative::testing::contains(
          result.llvmIr,
          "define internal float @__scalanative_byte_buffer_get_float_at") &&
      scalanative::testing::contains(
          result.llvmIr,
          "define internal ptr @__scalanative_byte_buffer_put_float_at") &&
      scalanative::testing::contains(
          result.llvmIr,
          "call float @__scalanative_native_bytes_get_float(ptr %array, i32 %index)") &&
      scalanative::testing::contains(
          result.llvmIr,
          "call void @__scalanative_native_bytes_put_float(ptr %array, i32 %index, "
          "float %value)") &&
      scalanative::testing::contains(
          result.llvmIr, "call void @__scalanative_throw_byte_buffer_index()") &&
      scalanative::testing::contains(result.llvmIr,
                                     "Runtime ABI = 'scalanative-runtime-68'");
  if (!indexedFloatNir || !indexedFloatLlvm) {
    return scalanative::testing::fail(
        TestName, "indexed Float ByteBuffer access was not lowered as expected:\n" +
                      result.nirText + "\n" + result.llvmIr);
  }
  return 0;
}

} // namespace

int main() {
  return indexedFloatByteBufferLowering();
}
