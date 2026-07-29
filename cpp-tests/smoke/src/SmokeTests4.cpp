#include "scalanative/support/Diagnostics.h"
#include "scalanative/tools/build/BuildDriver.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

int fail(const std::string& message) {
  std::cerr << message << '\n';
  return 1;
}

bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::string readTextFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

int smokeCompositeTypes() {
  constexpr const char* source = R"(package demo.compositetypes

trait Left
trait Right
trait Tagged

class LeftValue extends Left
class LeftTaggedValue extends Left with Tagged
class RightValue extends Right
class BothValue extends Right with Tagged
class Box[A](val value: A)

type Choice = Left | Right
type Required = Right & Tagged
type GenericChoice[A, B] = A | B
type GenericRequired[A, B] = A & B
type GroupedRequired = (Left | Right) & Tagged
type NestedRequired = Choice & Tagged
type GenericDistributed = GenericRequired[Choice, Tagged]
type DistributedSource = Tagged & (Left | Right)
type DistributedTarget = (Tagged & Left) | (Tagged & Right)

object Main {
  val unionField: Left | Right = new LeftValue
  val intersectionField: Right & Tagged = new BothValue
  val narrowedField: Right = intersectionField
  val unionBox: Box[Left | Right] =
    new Box[Left | Right](new LeftValue)

  def unionName(value: Left | Right): String = "union"
  def aliasUnionName(value: Choice): String = "alias-union"
  def genericUnionName(value: GenericChoice[Left, Right]): String =
    "generic-union"
  def intersectionName(value: Right & Tagged): String = "intersection"
  def genericIntersectionName(
      value: GenericRequired[Right, Tagged]): String =
    "generic-intersection"
  def precedenceName(value: Left | Right & Tagged): String = "precedence"
  def groupedName(value: GroupedRequired): String = "grouped"
  def nestedName(value: NestedRequired): String = "nested"
  def genericDistributedName(value: GenericDistributed): String =
    "generic-distributed"
  def distributedTargetName(value: DistributedTarget): String =
    "distributed-target"
  def distributedSourceName(value: DistributedSource): String =
    "distributed-source"
  def objectName(value: Object): String = "object"
  def rightName(value: Right): String = "right"
  def boxName(value: Box[Left | Right]): String = "box"
  def chooseLeft(): Choice = new LeftValue
  def chooseRight(): GenericChoice[Left, Right] = new RightValue
  def chooseBoth(): Required = new BothValue
  def narrowResult(value: Required): Right = value

  def main(args: Array[String]): Unit = {
    val localUnion: Choice = new RightValue
    val localIntersection: GenericRequired[Right, Tagged] = new BothValue
    val narrowedLocal: Right = localIntersection
    var narrowedVar: Right = new BothValue
    narrowedVar = localIntersection
    val nestedIntersection: NestedRequired = new LeftTaggedValue
    val genericDistributed: GenericDistributed = new BothValue
    val distributedSource: DistributedSource = new LeftTaggedValue
    val distributedTarget: DistributedTarget = new BothValue
    println(unionName(new LeftValue))
    println(unionName(new RightValue))
    println(aliasUnionName(unionField))
    println(genericUnionName(localUnion))
    println(intersectionName(new BothValue))
    println(genericIntersectionName(localIntersection))
    println(precedenceName(new LeftValue))
    println(precedenceName(new BothValue))
    println(groupedName(new LeftTaggedValue))
    println(groupedName(new BothValue))
    println(nestedName(nestedIntersection))
    println(genericDistributedName(genericDistributed))
    println(distributedTargetName(distributedSource))
    println(distributedSourceName(distributedTarget))
    println(objectName(localUnion))
    println(rightName(localIntersection))
    println(rightName(narrowedField))
    println(rightName(narrowedLocal))
    println(rightName(narrowedVar))
    println(rightName(narrowResult(localIntersection)))
    println(boxName(unionBox))
    println(unionName(chooseLeft()))
    println(unionName(chooseRight()))
    println(rightName(chooseBoth()))
  }
}
)";
  constexpr const char* invalidSource =
      R"(package demo.invalidcompositetypes

trait Left
trait Right
trait Tagged
class LeftValue extends Left
class RightValue extends Right
class Other

object Main {
  def union(value: Left | Right): Int = 1
  def intersection(value: Left & Tagged): Int = 2
  def needLeft(value: Left): Int = 3
  def grouped(value: (Left | Right) & Tagged): Int = 4

  val badUnion: Left | Right = new Other
  val badIntersection: Left & Tagged = new LeftValue

  def main(args: Array[String]): Unit = {
    val choice: Left | Right = new RightValue
    union(new Other)
    intersection(new LeftValue)
    grouped(new LeftValue)
    println(needLeft(choice))
  }
}
)";
  constexpr const char* malformedSource =
      R"(package demo.malformedcompositetypes

trait Left

object Main {
  def broken(value: Left |): Int = 1
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "cpp-scalanative-smoke-composite-types";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-composite-types.out";
  std::error_code ignored;
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildOptions options;
  options.action = scalanative::tools::build::BuildAction::BuildBinary;
  options.outputPath = binary;
  scalanative::support::DiagnosticEngine diagnostics;
  const scalanative::tools::build::BuildResult result =
      driver.buildSource("CompositeTypes.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid =
      driver.buildSource("InvalidCompositeTypes.scala", invalidSource, {},
                         invalidDiagnostics);
  scalanative::support::DiagnosticEngine malformedDiagnostics;
  const scalanative::tools::build::BuildResult malformed =
      driver.buildSource("MalformedCompositeTypes.scala", malformedSource, {},
                         malformedDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("composite-type native build failed: " +
                result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string text = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  const bool valid =
      status == 0 &&
      text == "union\nunion\nalias-union\ngeneric-union\n"
              "intersection\ngeneric-intersection\n"
              "precedence\nprecedence\ngrouped\ngrouped\nnested\n"
              "generic-distributed\n"
              "distributed-target\ndistributed-source\n"
              "object\nright\nright\nright\nright\nright\nbox\n"
              "union\nunion\nright\n" &&
      !invalid.ok &&
      contains(invalid.diagnosticsText,
               "does not conform to declared type "
               "demo.invalidcompositetypes.Left | "
               "demo.invalidcompositetypes.Right") &&
      contains(invalid.diagnosticsText,
               "does not conform to declared type "
               "demo.invalidcompositetypes.Left & "
               "demo.invalidcompositetypes.Tagged") &&
      contains(invalid.diagnosticsText,
               "does not conform to parameter type "
               "demo.invalidcompositetypes.Left | "
               "demo.invalidcompositetypes.Right") &&
      contains(invalid.diagnosticsText,
               "does not conform to parameter type "
               "demo.invalidcompositetypes.Left & "
               "demo.invalidcompositetypes.Tagged") &&
      contains(invalid.diagnosticsText,
               "does not conform to parameter type "
               "demo.invalidcompositetypes.Left & "
               "demo.invalidcompositetypes.Tagged | "
               "demo.invalidcompositetypes.Right & "
               "demo.invalidcompositetypes.Tagged") &&
      contains(invalid.diagnosticsText,
               "type demo.invalidcompositetypes.Left | "
               "demo.invalidcompositetypes.Right does not conform to parameter "
               "type demo.invalidcompositetypes.Left") &&
      !malformed.ok &&
      contains(malformed.diagnosticsText,
               "malformed intersection or union type: Left |") &&
      contains(result.nirText,
               "field @demo.compositetypes.Main.unionField$field : Object") &&
      contains(result.nirText,
               "field @demo.compositetypes.Main.intersectionField$field : "
               "Object") &&
      contains(result.nirText,
               "define @demo.compositetypes.Main.unionName : (Object)String") &&
      contains(result.nirText,
               "define @demo.compositetypes.Main.intersectionName : "
               "(Object)String") &&
      contains(result.nirText,
               "let %localUnion : Object = new "
               "demo.compositetypes.RightValue") &&
      contains(result.nirText,
               "let %localIntersection : Object = new "
               "demo.compositetypes.BothValue") &&
      contains(result.nirText,
               "call %rightName(as-instance-of[demo.compositetypes.Right]"
               "(%localIntersection))") &&
      contains(result.nirText,
               "define @demo.compositetypes.Main.chooseLeft : ()Object") &&
      contains(result.nirText,
               "define @demo.compositetypes.Main.chooseBoth : ()Object") &&
      !contains(result.nirText, " | ") &&
      !contains(result.nirText, " & ");
  return valid
             ? 0
             : fail("composite-type smoke test failed (output='" + text +
                    "', diagnostics='" + result.diagnosticsText +
                    "', invalid-diagnostics='" + invalid.diagnosticsText +
                    "', malformed-diagnostics='" +
                    malformed.diagnosticsText + "')");
}

} // namespace

int runSmokeTests4() {
  return smokeCompositeTypes();
}
