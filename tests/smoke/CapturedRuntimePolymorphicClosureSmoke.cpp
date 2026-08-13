#include "scalanative/support/Diagnostics.h"
#include "scalanative/testing/TestResources.h"
#include "scalanative/tools/build/BuildDriver.h"

#include <string>

namespace {

constexpr std::string_view TestName =
    "v0.1.0-alpha0.1.0.smoke.captured-runtime-polymorphic-closure-nir";

int capturedRuntimePolymorphicClosureNir() {
  const scalanative::testing::TestResource resource{
      "v0.1.0-alpha0.1.0", "run", "CapturedRuntimePolymorphicFunctions.scala"};
  if (!std::filesystem::is_regular_file(resource.path())) {
    return scalanative::testing::fail(TestName, "missing Scala resource: " +
                                                    resource.path().string());
  }

  scalanative::tools::build::BuildOptions options;
  options.action = scalanative::tools::build::BuildAction::EmitNir;
  scalanative::support::DiagnosticEngine diagnostics;
  const scalanative::tools::build::BuildResult result =
      scalanative::tools::build::BuildDriver{}.buildFile(resource.path(), options,
                                                         diagnostics);
  if (!result.ok) {
    return scalanative::testing::fail(TestName, "NIR emission failed:\n" +
                                                    result.diagnosticsText);
  }

  constexpr std::string_view closurePrefix =
      "class @tests.v010alpha010.capturedruntimepolymorphicfunctions.$polyclosure$";
  const bool typedEnvironment =
      scalanative::testing::countOccurrences(result.nirText, closurePrefix) >= 5 &&
      scalanative::testing::contains(result.nirText, "$capture$0 : String") &&
      scalanative::testing::contains(result.nirText, "$capture$0 : Int") &&
      scalanative::testing::contains(result.nirText, "$capture$0 : Object") &&
      scalanative::testing::contains(
          result.nirText,
          "$capture$this : "
          "tests.v010alpha010.capturedruntimepolymorphicfunctions.Renderer");
  const bool reboundBodies =
      scalanative::testing::contains(result.nirText, "%this.$capture$0") &&
      scalanative::testing::contains(result.nirText, "%this.$capture$this.prefix") &&
      scalanative::testing::contains(
          result.nirText,
          "%tests.v010alpha010.capturedruntimepolymorphicfunctions.Functions."
          "objectPrefix()") &&
      !scalanative::testing::contains(result.nirText,
                                      "<unlowered-polymorphic-function>");
  if (!typedEnvironment || !reboundBodies) {
    return scalanative::testing::fail(
        TestName,
        "captured closure environment was not lowered as expected:\n" + result.nirText);
  }
  return 0;
}

} // namespace

int main() {
  return capturedRuntimePolymorphicClosureNir();
}
