#include "scalanative/support/Diagnostics.h"
#include "scalanative/testing/TestResources.h"
#include "scalanative/tools/build/BuildDriver.h"

#include <string>

namespace {

constexpr std::string_view TestName =
    "v0.1.0-alpha0.1.0.smoke.indexed-byte-buffer-lowering";

int indexedByteBufferLowering() {
  const scalanative::testing::TestResource resource{"v0.1.0-alpha0.1.0", "run",
                                                    "IndexedByteBufferAccess.scala"};
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

  const bool indexedNir =
      scalanative::testing::contains(result.nirText,
                                     "scala.scalanative.runtime.byteBufferGetAt") &&
      scalanative::testing::contains(result.nirText,
                                     "scala.scalanative.runtime.byteBufferPutAt");
  const bool indexedLlvm =
      scalanative::testing::contains(
          result.llvmIr, "define internal i8 @__scalanative_byte_buffer_get_at") &&
      scalanative::testing::contains(
          result.llvmIr, "define internal ptr @__scalanative_byte_buffer_put_at") &&
      scalanative::testing::contains(
          result.llvmIr, "call void @__scalanative_throw_byte_buffer_index()") &&
      scalanative::testing::contains(result.llvmIr,
                                     "Runtime ABI = 'scalanative-runtime-68'");
  if (!indexedNir || !indexedLlvm) {
    return scalanative::testing::fail(
        TestName, "indexed ByteBuffer access was not lowered as expected:\n" +
                      result.nirText + "\n" + result.llvmIr);
  }
  return 0;
}

} // namespace

int main() {
  return indexedByteBufferLowering();
}
