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

int smokeTupleApply() {
  constexpr const char* source = R"(package demo.tupleapply

class Box(val value: String)

class Counter {
  var calls: Int = 0

  def nextLabel: String = {
    calls = calls + 1
    "once"
  }

  def make: (String, Int, Boolean) = (nextLabel, calls, true)
}

class Lookup {
  def apply(index: Int): String = "custom:" + index
}

object Values {
  def triple: (Int, String, Box) = (7, "middle", new Box("last"))

  def first: Int = triple(0)
  def second: String = triple.apply(1)
  def third: Box = triple(1 + 1)
  def thirdText: String = third.value

  def stableIndex: String = {
    val index: 1 = 1
    triple(index)
  }

  def direct: Boolean = (9, true)(1)
  def cons: Int = (99 *: EmptyTuple).apply(0)

  def receiverOnce: String = {
    val counter = new Counter
    counter.make.apply(1) + ":" + counter.calls
  }

  def customApply: String = new Lookup().apply(4)
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Values.first)
    println(Values.second)
    println(Values.thirdText)
    println(Values.stableIndex)
    println(Values.direct)
    println(Values.cons)
    println(Values.receiverOnce)
    println(Values.customApply)
  }
}
)";

  constexpr const char* invalidSource = R"(package demo.invalidtupleapply

object Values {
  val empty = EmptyTuple(0)
  val negative = (1, "x")(-1)
  val high = (1, "x").apply(2)
  def dynamic(index: Int): Object = (1, "x")(index)
  val wrongType = (1, "x")(0L)
  val missing = (1, "x").apply()
  def open(value: Tuple): Object = value(0)
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary = temporary / "cpp-scalanative-smoke-tuple-apply";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-tuple-apply.out";
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
      driver.buildSource("TupleApply.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidTupleApply.scala", invalidSource, {}, invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("tuple-apply native build failed: " + result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string outputText = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  const std::string_view first =
      functionText(result.nirText, "demo.tupleapply.Values.first");
  const std::string_view second =
      functionText(result.nirText, "demo.tupleapply.Values.second");
  const std::string_view third =
      functionText(result.nirText, "demo.tupleapply.Values.third");
  const std::string_view stable =
      functionText(result.nirText, "demo.tupleapply.Values.stableIndex");
  const std::string_view direct =
      functionText(result.nirText, "demo.tupleapply.Values.direct");
  const std::string_view cons =
      functionText(result.nirText, "demo.tupleapply.Values.cons");
  const std::string_view receiverOnce =
      functionText(result.nirText, "demo.tupleapply.Values.receiverOnce");
  const std::string_view custom =
      functionText(result.nirText, "demo.tupleapply.Values.customApply");

  const bool runtimeShape =
      contains(result.nirText, "module @scala.EmptyTuple : @scala.Tuple") &&
      contains(result.nirText, "class @scala.Tuple1 : @scala.Tuple") &&
      contains(result.nirText, "class @scala.Tuple2 : @scala.Tuple") &&
      contains(result.nirText, "class @scala.Tuple3 : @scala.Tuple") &&
      !contains(result.nirText, "class @scala.Tuple4 : @scala.Tuple");
  const bool lowered =
      contains(first, "let %tupleApply$receiver$") &&
      contains(first, "let %tupleApply$index$") && contains(first, "unbox[Int]") &&
      contains(first, "._1") && contains(second, "unbox[String]") &&
      contains(second, "._2") && contains(third, "let %tupleApply$index$") &&
      contains(third, "(1 + 1)") && contains(third, "._3") &&
      contains(third, "as-instance-of[demo.tupleapply.Box]") &&
      contains(stable, "let %index : Int = 1") && contains(stable, "unbox[String]") &&
      contains(stable, "._2") &&
      contains(direct, "new scala.Tuple2(box[Int](9), box[Boolean](true))") &&
      contains(direct, "unbox[Boolean]") && contains(direct, "._2") &&
      contains(cons, "new scala.Tuple1(box[Int]") && contains(cons, "unbox[Int]") &&
      contains(cons, "._1") && countOccurrences(receiverOnce, "%counter.make") == 1 &&
      contains(receiverOnce, "unbox[Int]") && contains(receiverOnce, "._2") &&
      contains(custom, "call new demo.tupleapply.Lookup.apply(4)") &&
      !contains(custom, "tupleApply$receiver$") &&
      !contains(result.nirText, "<malformed-tuple-apply>");
  const bool invalidDiagnosticsMatch =
      !invalid.ok &&
      contains(invalid.diagnosticsText,
               "tuple apply index 0 is outside tuple bounds [0, 0)") &&
      contains(invalid.diagnosticsText,
               "tuple apply index -1 is outside tuple bounds [0, 2)") &&
      contains(invalid.diagnosticsText,
               "tuple apply index 2 is outside tuple bounds [0, 2)") &&
      contains(invalid.diagnosticsText,
               "tuple apply index must be a compile-time constant Int") &&
      contains(invalid.diagnosticsText, "tuple apply index must have type Int") &&
      contains(invalid.diagnosticsText, "tuple apply requires exactly one Int index") &&
      contains(invalid.diagnosticsText, "apply requires a concrete tuple receiver");

  if (status == 0 &&
      outputText == "7\nmiddle\nlast\nmiddle\ntrue\n99\n1:1\ncustom:4\n" &&
      runtimeShape && lowered && invalidDiagnosticsMatch) {
    return 0;
  }

  return fail("tuple-apply smoke test failed (status=" + std::to_string(status) +
              ", output='" + outputText + "', diagnostics='" + result.diagnosticsText +
              "', invalid='" + invalid.diagnosticsText + "', first='" +
              std::string(first) + "', second='" + std::string(second) + "', third='" +
              std::string(third) + "', stable='" + std::string(stable) + "', direct='" +
              std::string(direct) + "', cons='" + std::string(cons) +
              "', receiver-once='" + std::string(receiverOnce) + "', custom='" +
              std::string(custom) + "')");
}

} // namespace

int runSmokeTests11() {
  return smokeTupleApply();
}
