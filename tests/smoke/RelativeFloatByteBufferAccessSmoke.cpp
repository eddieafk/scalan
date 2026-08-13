#include "scalanative/support/Diagnostics.h"
#include "scalanative/testing/TestResources.h"
#include "scalanative/tools/build/BuildDriver.h"

#include <string>

namespace {

constexpr std::string_view TestName =
    "v0.1.0-alpha0.1.0.smoke.relative-float-byte-buffer-lowering";

int relativeFloatByteBufferLowering() {
  const scalanative::testing::TestResource resource{
      "v0.1.0-alpha0.1.0", "run", "RelativeFloatByteBufferAccess.scala"};
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

  const bool floatNir =
      scalanative::testing::contains(result.nirText,
                                     "scala.scalanative.runtime.byteBufferGetFloat") &&
      scalanative::testing::contains(result.nirText,
                                     "scala.scalanative.runtime.byteBufferPutFloat");
  const bool floatLlvm =
      scalanative::testing::contains(
          result.llvmIr, "define internal float @__scalanative_native_bytes_get_float") &&
      scalanative::testing::contains(
          result.llvmIr, "define internal void @__scalanative_native_bytes_put_float") &&
      scalanative::testing::contains(
          result.llvmIr, "define internal float @__scalanative_byte_buffer_get_float") &&
      scalanative::testing::contains(
          result.llvmIr, "define internal ptr @__scalanative_byte_buffer_put_float") &&
      scalanative::testing::contains(result.llvmIr,
                                     "%value = bitcast i32 %bits to float") &&
      scalanative::testing::contains(result.llvmIr,
                                     "%bits = bitcast float %value to i32") &&
      scalanative::testing::contains(
          result.llvmIr,
          "call float @__scalanative_native_bytes_get_float(ptr %array, i32 "
          "%backing_index, i1 %little_endian)") &&
      scalanative::testing::contains(
          result.llvmIr,
          "call void @__scalanative_native_bytes_put_float(ptr %array, i32 "
          "%backing_index, "
          "float %value, i1 %little_endian)") &&
      scalanative::testing::contains(result.llvmIr,
                                     "Runtime ABI = 'scalanative-runtime-71'");
  if (!floatNir || !floatLlvm) {
    return scalanative::testing::fail(
        TestName, "relative Float ByteBuffer access was not lowered as expected:\n" +
                      result.nirText + "\n" + result.llvmIr);
  }
  return 0;
}

} // namespace

int main() {
  return relativeFloatByteBufferLowering();
}
