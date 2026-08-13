#include "scalanative/support/Diagnostics.h"
#include "scalanative/testing/TestResources.h"
#include "scalanative/tools/build/BuildDriver.h"

#include <string>

namespace {

constexpr std::string_view TestName =
    "v0.1.0-alpha0.1.0.smoke.relative-long-byte-buffer-lowering";

int relativeLongByteBufferLowering() {
  const scalanative::testing::TestResource resource{
      "v0.1.0-alpha0.1.0", "run", "RelativeLongByteBufferAccess.scala"};
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

  const bool longNir =
      scalanative::testing::contains(result.nirText,
                                     "scala.scalanative.runtime.byteBufferGetLong") &&
      scalanative::testing::contains(result.nirText,
                                     "scala.scalanative.runtime.byteBufferPutLong");
  const bool longLlvm =
      scalanative::testing::contains(
          result.llvmIr, "define internal i64 @__scalanative_native_bytes_get_long") &&
      scalanative::testing::contains(
          result.llvmIr, "define internal void @__scalanative_native_bytes_put_long") &&
      scalanative::testing::contains(
          result.llvmIr, "define internal i64 @__scalanative_byte_buffer_get_long") &&
      scalanative::testing::contains(
          result.llvmIr, "define internal ptr @__scalanative_byte_buffer_put_long") &&
      scalanative::testing::contains(
          result.llvmIr,
          "call i64 @__scalanative_native_bytes_get_long(ptr %array, i32 %position)") &&
      scalanative::testing::contains(
          result.llvmIr,
          "call void @__scalanative_native_bytes_put_long(ptr %array, i32 %position, "
          "i64 %value)") &&
      scalanative::testing::contains(result.llvmIr,
                                     "Runtime ABI = 'scalanative-runtime-63'");
  if (!longNir || !longLlvm) {
    return scalanative::testing::fail(
        TestName, "relative Long ByteBuffer access was not lowered as expected:\n" +
                      result.nirText + "\n" + result.llvmIr);
  }
  return 0;
}

} // namespace

int main() {
  return relativeLongByteBufferLowering();
}
