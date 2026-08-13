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

int smokePolymorphicTupleMap() {
  constexpr const char* source = R"(package demo.polymorphictuplemap

class Box[A](val value: A)

class TupleSource {
  var calls: Int = 0

  def next: Int = {
    calls = calls + 1
    7
  }

  def tuple: (Int, String, Boolean) = (next, "x", true)
}

object Values {
  def identity: (Int, String, Boolean) =
    (1, "same", true).map([A] => (value: A) => value)

  def wrapped: (Box[Int], Box[String], Box[Boolean]) =
    (2, "boxed", false).map([A] => (value: A) => new Box(value))

  def text: (String, String, String) =
    (3, "text", true).map([A] => (value: A) => value.toString)

  def constant: (Int, Int, Int) =
    (4, "constant", false).map([A] => (value: A) => 9)

  def observed: String = {
    val source = new TupleSource
    var calls = 0
    val mapped = source.tuple.map([A] => (value: A) => {
      calls = calls + 1
      calls + ":" + value.toString
    })
    mapped._1 + "|" + mapped._2 + "|" + mapped._3 + ":" +
      source.calls + ":" + calls
  }

  def emptyObserved: String = {
    var calls = 0
    val mapped = EmptyTuple.map([A] => (value: A) => {
      calls = calls + 1
      value
    })
    mapped.size + ":" + calls
  }
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Values.identity._1)
    println(Values.identity._2)
    println(Values.identity._3)
    println(Values.wrapped._1.value)
    println(Values.wrapped._2.value)
    println(Values.wrapped._3.value)
    println(Values.text._1)
    println(Values.text._2)
    println(Values.text._3)
    println(Values.constant._1)
    println(Values.constant._3)
    println(Values.observed)
    println(Values.emptyObserved)
  }
}
)";

  constexpr const char* invalidSource = R"(package demo.invalidpolymorphictuplemap

object Values {
  val twoTypes = (1, "x").map([A, B] => (value: A) => value)
  val wrongParameter = (1, "x").map([A] => (value: Int) => value)
  val bounded = (1, "x").map([A <: Object] => (value: A) => value)
  val nonGenericBody = (1, "x").map([A] => (value: A) => value % 2)
  var mutable = [A] => (value: A) => value
  def open(value: Tuple): Tuple =
    value.map([A] => (element: A) => element)
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "scalanative-smoke-polymorphic-tuple-map";
  const std::filesystem::path output =
      temporary / "scalanative-smoke-polymorphic-tuple-map.out";
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
      driver.buildSource("PolymorphicTupleMap.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidPolymorphicTupleMap.scala", invalidSource, {}, invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("polymorphic tuple-map native build failed: " + result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string outputText = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  const std::string_view identity =
      functionText(result.nirText, "demo.polymorphictuplemap.Values.identity");
  const std::string_view wrapped =
      functionText(result.nirText, "demo.polymorphictuplemap.Values.wrapped");
  const std::string_view text =
      functionText(result.nirText, "demo.polymorphictuplemap.Values.text");
  const std::string_view constant =
      functionText(result.nirText, "demo.polymorphictuplemap.Values.constant");
  const std::string_view observed =
      functionText(result.nirText, "demo.polymorphictuplemap.Values.observed");
  const std::string_view emptyObserved =
      functionText(result.nirText, "demo.polymorphictuplemap.Values.emptyObserved");
  const std::string_view main =
      functionText(result.nirText, "demo.polymorphictuplemap.Main.main");

  const bool runtimeShape =
      contains(result.nirText, "trait @scala.PolyFunction : @java.lang.Object") &&
      contains(result.nirText, "class @scala.Tuple3 : @scala.Tuple") &&
      !contains(result.nirText, "class @scala.Tuple1 : @scala.Tuple") &&
      !contains(result.nirText, "class @scala.Tuple2 : @scala.Tuple") &&
      !contains(result.nirText, "class @scala.Tuple4 : @scala.Tuple");
  const bool lowered =
      countOccurrences(identity, "let %tupleMap$receiver$") == 1 &&
      countOccurrences(identity, "let %value : Object") == 3 &&
      countOccurrences(identity, ".apply(") == 0 &&
      contains(identity, "new scala.Tuple3(block") &&
      countOccurrences(wrapped, "new demo.polymorphictuplemap.Box(%value)") == 3 &&
      countOccurrences(text, "box[String](block") == 3 &&
      countOccurrences(constant, "box[Int](block") == 3 &&
      countOccurrences(observed, "%source.tuple") == 1 &&
      countOccurrences(observed, "assign %calls") == 3 &&
      countOccurrences(observed, "let %value : Object") == 3 &&
      countOccurrences(emptyObserved, "let %tupleMap$receiver$") == 1 &&
      countOccurrences(emptyObserved, "let %value : Object") == 0 &&
      contains(main, "unbox[Int](%Values.identity._1)") &&
      contains(main, "unbox[String](%Values.identity._2)") &&
      contains(main, "unbox[Boolean](%Values.identity._3)") &&
      countOccurrences(main, "as-instance-of[demo.polymorphictuplemap.Box]") == 3 &&
      !contains(result.nirText, "<unlowered-polymorphic-function>") &&
      !contains(result.nirText, "<malformed-polymorphic-tuple-map>");
  const bool invalidDiagnosticsMatch =
      !invalid.ok &&
      contains(invalid.diagnosticsText,
               "tuple map polymorphic function literal must have the form "
               "[A] => (value: A) => body") &&
      contains(invalid.diagnosticsText,
               "remainder operator % requires numeric operands") &&
      contains(invalid.diagnosticsText,
               "polymorphic function literals are currently supported only as "
               "direct Tuple.map arguments, direct invocations, or immutable val "
               "initializers") &&
      contains(invalid.diagnosticsText, "map requires a concrete tuple receiver");

  if (status == 0 &&
      outputText == "1\nsame\ntrue\n2\nboxed\nfalse\n3\ntext\ntrue\n9\n9\n"
                    "1:7|2:x|3:true:1:3\n0:0\n" &&
      runtimeShape && lowered && invalidDiagnosticsMatch) {
    return 0;
  }

  return fail(
      "polymorphic tuple-map smoke test failed (status=" + std::to_string(status) +
      ", output='" + outputText + "', diagnostics='" + result.diagnosticsText +
      "', invalid='" + invalid.diagnosticsText + "', identity='" +
      std::string(identity) + "', wrapped='" + std::string(wrapped) + "', text='" +
      std::string(text) + "', constant='" + std::string(constant) + "', observed='" +
      std::string(observed) + "', empty-observed='" + std::string(emptyObserved) +
      "', main='" + std::string(main) + "')");
}

} // namespace

int runSmokeTests15() {
  return smokePolymorphicTupleMap();
}
