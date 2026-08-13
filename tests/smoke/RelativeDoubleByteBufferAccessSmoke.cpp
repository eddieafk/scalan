#include "scalanative/support/Diagnostics.h"
#include "scalanative/testing/TestResources.h"
#include "scalanative/tools/build/BuildDriver.h"

#include <string>

namespace {

constexpr std::string_view TestName =
    "v0.1.0-alpha0.1.0.smoke.relative-double-byte-buffer-lowering";

int relativeDoubleByteBufferLowering() {
  const scalanative::testing::TestResource resource{
      "v0.1.0-alpha0.1.0", "run", "RelativeDoubleByteBufferAccess.scala"};
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

  const bool doubleNir =
      scalanative::testing::contains(
          result.nirText, "scala.scalanative.runtime.byteBufferGetDouble") &&
      scalanative::testing::contains(
          result.nirText, "scala.scalanative.runtime.byteBufferPutDouble");
  const bool doubleLlvm =
      scalanative::testing::contains(
          result.llvmIr,
          "define internal double @__scalanative_native_bytes_get_double") &&
      scalanative::testing::contains(
          result.llvmIr,
          "define internal void @__scalanative_native_bytes_put_double") &&
      scalanative::testing::contains(
          result.llvmIr,
          "define internal double @__scalanative_byte_buffer_get_double") &&
      scalanative::testing::contains(
          result.llvmIr,
          "define internal ptr @__scalanative_byte_buffer_put_double") &&
      scalanative::testing::contains(result.llvmIr,
                                     "%value = bitcast i64 %bits to double") &&
      scalanative::testing::contains(result.llvmIr,
                                     "%bits = bitcast double %value to i64") &&
      scalanative::testing::contains(
          result.llvmIr,
          "call double @__scalanative_native_bytes_get_double(ptr %array, i32 "
          "%position, i1 %little_endian)") &&
      scalanative::testing::contains(
          result.llvmIr,
          "call void @__scalanative_native_bytes_put_double(ptr %array, i32 "
          "%position, double %value, i1 %little_endian)") &&
      scalanative::testing::contains(result.llvmIr,
                                     "Runtime ABI = 'scalanative-runtime-69'");
  if (!doubleNir || !doubleLlvm) {
    return scalanative::testing::fail(
        TestName, "relative Double ByteBuffer access was not lowered as expected:\n" +
                      result.nirText + "\n" + result.llvmIr);
  }
  return 0;
}

} // namespace

int main() {
  return relativeDoubleByteBufferLowering();
}
