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

int smokeTupleMap() {
  constexpr const char* source = R"(package demo.tuplemap

class Box[A](val value: A)

object Identity extends PolyFunction {
  def apply[A](value: A): A = value
}

object Wrap extends PolyFunction {
  def apply[A](value: A): Box[A] = new Box(value)
}

object Text extends PolyFunction {
  def apply[A](value: A): String = value.toString
}

class CountingText extends PolyFunction {
  var calls: Int = 0

  def apply[A](value: A): String = {
    calls = calls + 1
    calls + ":" + value.toString
  }
}

class MapperFactory {
  var calls: Int = 0
  val mapper: CountingText = new CountingText

  def nextMapper: CountingText = {
    calls = calls + 1
    mapper
  }
}

class TupleSource {
  var calls: Int = 0

  def next: Int = {
    calls = calls + 1
    7
  }

  def tuple: (Int, String, Boolean) = (next, "x", true)
}

object Values {
  def identity: (Int, String, Boolean) = (1, "same", true).map(Identity)

  def wrapped: (Box[Int], Box[String], Box[Boolean]) =
    (2, "boxed", false).map(Wrap)

  def text: (String, String, String) = (3, "text", true).map(Text)
  def empty: EmptyTuple = EmptyTuple.map(Text)

  def observed: String = {
    val source = new TupleSource
    val factory = new MapperFactory
    val mapped = source.tuple.map(factory.nextMapper)
    val first = mapped._1
    val second = mapped._2
    val third = mapped._3
    first + "|" + second + "|" + third + ":" + source.calls + ":" +
      factory.calls + ":" + factory.mapper.calls
  }

  def emptyObserved: String = {
    val factory = new MapperFactory
    val mapped = EmptyTuple.map(factory.nextMapper)
    mapped.size + ":" + factory.calls + ":" + factory.mapper.calls
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
    println(Values.empty.size)
    println(Values.observed)
    println(Values.emptyObserved)
  }
}
)";

  constexpr const char* invalidSource = R"(package demo.invalidtuplemap

object Text extends PolyFunction {
  def apply[A](value: A): String = value.toString
}

object StructuralOnly {
  def apply[A](value: A): String = value.toString
}

object Monomorphic extends PolyFunction {
  def apply(value: Int): String = value.toString
}

object WrongParameter extends PolyFunction {
  def apply[A](value: Int): String = value.toString
}

object Values {
  def open(value: Tuple): Tuple = value.map(Text)
  val missing = (1, "x").map()
  val extra = (1, "x").map(Text, Text)
  val structural = (1, "x").map(StructuralOnly)
  val monomorphic = (1, "x").map(Monomorphic)
  val wrongParameter = EmptyTuple.map(WrongParameter)
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary = temporary / "scalanative-smoke-tuple-map";
  const std::filesystem::path output =
      temporary / "scalanative-smoke-tuple-map.out";
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
      driver.buildSource("TupleMap.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidTupleMap.scala", invalidSource, {}, invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("tuple-map native build failed: " + result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string outputText = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  const std::string_view identity =
      functionText(result.nirText, "demo.tuplemap.Values.identity");
  const std::string_view wrapped =
      functionText(result.nirText, "demo.tuplemap.Values.wrapped");
  const std::string_view text =
      functionText(result.nirText, "demo.tuplemap.Values.text");
  const std::string_view empty =
      functionText(result.nirText, "demo.tuplemap.Values.empty");
  const std::string_view observed =
      functionText(result.nirText, "demo.tuplemap.Values.observed");
  const std::string_view emptyObserved =
      functionText(result.nirText, "demo.tuplemap.Values.emptyObserved");
  const std::string_view main = functionText(result.nirText, "demo.tuplemap.Main.main");

  const bool runtimeShape =
      contains(result.nirText, "trait @scala.PolyFunction : @java.lang.Object") &&
      contains(result.nirText, "module @scala.EmptyTuple : @scala.Tuple") &&
      contains(result.nirText, "class @scala.Tuple3 : @scala.Tuple") &&
      !contains(result.nirText, "class @scala.Tuple1 : @scala.Tuple") &&
      !contains(result.nirText, "class @scala.Tuple2 : @scala.Tuple") &&
      !contains(result.nirText, "class @scala.Tuple4 : @scala.Tuple");
  const std::size_t observedReceiver = observed.find("%source.tuple");
  const std::size_t observedFunction = observed.find("%factory.nextMapper");
  const bool lowered =
      countOccurrences(identity, "let %tupleMap$receiver$") == 1 &&
      countOccurrences(identity, "let %tupleMap$function$") == 1 &&
      countOccurrences(identity, ".apply(") == 3 &&
      contains(identity, "new scala.Tuple3(call") &&
      countOccurrences(wrapped, ".apply(") == 3 &&
      contains(wrapped, "new scala.Tuple3(call") &&
      countOccurrences(text, "box[String](call") == 3 &&
      countOccurrences(empty, "let %tupleMap$") == 2 &&
      countOccurrences(empty, ".apply(") == 0 && contains(empty, "%scala.EmptyTuple") &&
      countOccurrences(observed, "%source.tuple") == 1 &&
      countOccurrences(observed, "%factory.nextMapper") == 1 &&
      observedReceiver < observedFunction &&
      countOccurrences(observed, ".apply(") == 3 &&
      countOccurrences(emptyObserved, "%factory.nextMapper") == 1 &&
      countOccurrences(emptyObserved, ".apply(") == 0 &&
      contains(main, "unbox[Int](%Values.identity._1)") &&
      contains(main, "unbox[String](%Values.identity._2)") &&
      contains(main, "unbox[Boolean](%Values.identity._3)") &&
      countOccurrences(main, "as-instance-of[demo.tuplemap.Box]") == 3 &&
      !contains(result.nirText, "<malformed-tuple-map>");
  const bool invalidDiagnosticsMatch =
      !invalid.ok &&
      contains(invalid.diagnosticsText, "map requires a concrete tuple receiver") &&
      contains(invalid.diagnosticsText,
               "tuple map requires exactly one polymorphic function") &&
      contains(invalid.diagnosticsText,
               "tuple map function must define apply[A](value: A)");

  if (status == 0 &&
      outputText == "1\nsame\ntrue\n2\nboxed\nfalse\n3\ntext\ntrue\n0\n"
                    "1:7|2:x|3:true:1:1:3\n0:1:0\n" &&
      runtimeShape && lowered && invalidDiagnosticsMatch) {
    return 0;
  }

  return fail("tuple-map smoke test failed (status=" + std::to_string(status) +
              ", output='" + outputText + "', diagnostics='" + result.diagnosticsText +
              "', invalid='" + invalid.diagnosticsText + "', identity='" +
              std::string(identity) + "', wrapped='" + std::string(wrapped) +
              "', text='" + std::string(text) + "', empty='" + std::string(empty) +
              "', observed='" + std::string(observed) + "', empty-observed='" +
              std::string(emptyObserved) + "', main='" + std::string(main) + "')");
}

} // namespace

int runSmokeTests14() {
  return smokeTupleMap();
}
