#include "scalanative/support/Diagnostics.h"
#include "scalanative/testing/TestResources.h"
#include "scalanative/tools/build/BuildDriver.h"

#include <string>

namespace {

constexpr std::string_view TestName =
    "v0.1.0-alpha0.1.0.smoke.byte-buffer-mark-reset-lowering";

int byteBufferMarkResetLowering() {
  const scalanative::testing::TestResource resource{
      "v0.1.0-alpha0.1.0", "run", "ByteBufferMarkReset.scala"};
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

  const bool stateNir =
      scalanative::testing::contains(result.nirText,
                                     "scala.scalanative.runtime.byteBufferMark") &&
      scalanative::testing::contains(result.nirText,
                                     "scala.scalanative.runtime.byteBufferReset") &&
      scalanative::testing::contains(result.nirText,
                                     "java.nio.InvalidMarkException");
  const bool stateLlvm =
      scalanative::testing::contains(
          result.llvmIr, "define internal ptr @__scalanative_byte_buffer_mark") &&
      scalanative::testing::contains(
          result.llvmIr, "define internal ptr @__scalanative_byte_buffer_reset") &&
      scalanative::testing::contains(
          result.llvmIr, "call void @__scalanative_throw_byte_buffer_invalid_mark()") &&
      scalanative::testing::contains(result.llvmIr,
                                     "Runtime ABI = 'scalanative-runtime-63'");
  if (!stateNir || !stateLlvm) {
    return scalanative::testing::fail(
        TestName, "ByteBuffer mark/reset was not lowered as expected:\n" +
                      result.nirText + "\n" + result.llvmIr);
  }
  return 0;
}

} // namespace

int main() {
  return byteBufferMarkResetLowering();
}
