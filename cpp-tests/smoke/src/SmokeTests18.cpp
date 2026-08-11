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

std::size_t countOccurrences(std::string_view haystack, std::string_view needle) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while ((offset = haystack.find(needle, offset)) != std::string_view::npos) {
    ++count;
    offset += needle.size();
  }
  return count;
}

std::string readTextFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

std::string_view functionText(std::string_view nir, std::string_view name) {
  const std::string marker = "define @" + std::string(name);
  const std::size_t start = nir.find(marker);
  if (start == std::string_view::npos) {
    return {};
  }
  const std::size_t end = nir.find("\ndefine @", start + marker.size());
  return nir.substr(start,
                    end == std::string_view::npos ? nir.size() - start : end - start);
}

int smokePolymorphicFunctionAliasesAndTupleMap() {
  constexpr const char* source = R"(package demo.polymorphicaliasmap

class Renderer(val prefix: String) {
  val render: [A] => A => String =
    [A] => (value: A) => prefix + value.toString
  val alias = render

  def own: String = {
    val mapped = (1, true).map(alias)
    mapped._1 + ":" + mapped._2
  }
}

class Source {
  var calls: Int = 0

  def next: Renderer = {
    calls = calls + 1
    new Renderer("selected:")
  }
}

object Values {
  val identity: [A] => A => A = [A] => (value: A) => value
  val same = identity

  def member: String = {
    val mapped = (42, "x", true).map(same)
    mapped._1.toString + ":" + mapped._2 + ":" + mapped._3.toString
  }

  def local: String = {
    val first = same
    val second = first
    val mapped = (7, "local").map(second)
    mapped._1.toString + ":" + mapped._2
  }

  def invoked: String = {
    val localAlias = same
    same[Int](5).toString + ":" + localAlias[String]("invoke")
  }

  def selected: String = {
    val source = new Source
    val mapped = (9, false).map(source.next.render)
    mapped._1 + ":" + mapped._2 + ":" + source.calls
  }

  def empty: Int = {
    val source = new Source
    EmptyTuple.map(source.next.render)
    source.calls
  }
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Values.member)
    println(Values.local)
    println(Values.invoked)
    println(Values.selected)
    println(Values.empty)
    println(new Renderer("own:").own)
  }
}
)";

  constexpr const char* invalidSource =
      R"(package demo.invalidpolymorphicaliasmap

class Holder {
  val identity = [A] => (value: A) => value
}

object Values {
  val identity = [A] => (value: A) => value
  var mutableAlias = identity
  val selectedAlias = new Holder().identity
  val wrongParameter: [A] => Int => A = [A] => (value: A) => value
  val wrongResult: [A] => A => String = [A] => (value: A) => value

  def invokeRuntime(function: [A] => A => A): Int = function[Int](1)
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "cpp-scalanative-smoke-polymorphic-alias-map";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-polymorphic-alias-map.out";
  std::error_code ignored;
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildOptions options;
  options.action = scalanative::tools::build::BuildAction::BuildBinary;
  options.optimize = true;
  options.outputPath = binary;
  scalanative::support::DiagnosticEngine diagnostics;
  const scalanative::tools::build::BuildResult result =
      driver.buildSource("PolymorphicAliasMap.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidPolymorphicAliasMap.scala", invalidSource, {}, invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("polymorphic alias/map native build failed: " + result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string outputText = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  const std::string_view member =
      functionText(result.nirText, "demo.polymorphicaliasmap.Values.member");
  const std::string_view local =
      functionText(result.nirText, "demo.polymorphicaliasmap.Values.local");
  const std::string_view invoked =
      functionText(result.nirText, "demo.polymorphicaliasmap.Values.invoked");
  const std::string_view selected =
      functionText(result.nirText, "demo.polymorphicaliasmap.Values.selected");
  const std::string_view empty =
      functionText(result.nirText, "demo.polymorphicaliasmap.Values.empty");
  const std::string_view own =
      functionText(result.nirText, "demo.polymorphicaliasmap.Renderer.own");

  const bool storageErased =
      !contains(result.nirText, "field @demo.polymorphicaliasmap.Values.identity") &&
      !contains(result.nirText, "field @demo.polymorphicaliasmap.Values.same") &&
      !contains(result.nirText, "field @demo.polymorphicaliasmap.Renderer.render") &&
      !contains(result.nirText, "field @demo.polymorphicaliasmap.Renderer.alias") &&
      !contains(local, "let %first : scala.PolyFunction") &&
      !contains(local, "let %second : scala.PolyFunction") &&
      !contains(invoked, "let %localAlias : scala.PolyFunction");
  const bool lowered =
      contains(result.nirText, "trait @scala.PolyFunction : @java.lang.Object") &&
      contains(member, "new scala.Tuple3") && contains(local, "new scala.Tuple2") &&
      countOccurrences(selected, "%source.next") == 1 &&
      countOccurrences(selected, "let %this : demo.polymorphicaliasmap.Renderer") ==
          1 &&
      countOccurrences(empty, "%source.next") == 1 &&
      countOccurrences(empty, "let %this : demo.polymorphicaliasmap.Renderer") == 1 &&
      countOccurrences(own, "let %this : demo.polymorphicaliasmap.Renderer") == 1 &&
      countOccurrences(result.nirText, ".apply(") == 0 &&
      !contains(result.nirText, "<unlowered-polymorphic-function>");
  const bool invalidDiagnosticsMatch =
      !invalid.ok &&
      contains(invalid.diagnosticsText,
               "polymorphic function value identity must be invoked directly with "
               "one explicit type argument and one value argument") &&
      contains(invalid.diagnosticsText,
               "unary polymorphic function type must have the form "
               "[A] => A => R") &&
      contains(invalid.diagnosticsText,
               "polymorphic function result type A does not conform to declared "
               "result type String") &&
      contains(invalid.diagnosticsText,
               "runtime polymorphic function values require closure-object "
               "lowering before invocation");

  if (status == 0 &&
      outputText == "42:x:true\n7:local\n5:invoke\nselected:9:selected:false:1\n1\n"
                    "own:1:own:true\n" &&
      storageErased && lowered && invalidDiagnosticsMatch) {
    return 0;
  }

  return fail(
      "polymorphic alias/map smoke test failed (status=" + std::to_string(status) +
      ", output='" + outputText + "', diagnostics='" + result.diagnosticsText +
      "', invalid='" + invalid.diagnosticsText + "', member='" + std::string(member) +
      "', local='" + std::string(local) + "', invoked='" + std::string(invoked) +
      "', selected='" + std::string(selected) + "', empty='" + std::string(empty) +
      "', own='" + std::string(own) + "')");
}

} // namespace

int runSmokeTests18() {
  return smokePolymorphicFunctionAliasesAndTupleMap();
}
