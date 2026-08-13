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

int smokeTupleConcat() {
  constexpr const char* source = R"(package demo.tupleconcat

class Box(val value: String)

class Counter {
  var calls: Int = 0

  def nextLeft: String = {
    calls = calls + 1
    "left"
  }

  def nextRight: Boolean = {
    calls = calls + 1
    true
  }

  def left: (String, Int) = (nextLeft, calls)
  def right: (Boolean, Box) = (nextRight, new Box("right"))
  def combined: (String, Int, Boolean, Box) = left ++ right
}

object Values {
  def left: (Int, String) = (7, "middle")
  def right: (Boolean, Box) = (true, new Box("last"))
  def merged: (Int, String, Boolean, Box) = left ++ right

  def first: Int = merged._1
  def second: String = merged._2
  def third: Boolean = merged._3
  def fourth: Box = merged._4

  def leftIdentity: (Int, String) = EmptyTuple ++ left
  def rightIdentity: (Int, String) = left ++ EmptyTuple
  def bothEmpty: EmptyTuple = EmptyTuple ++ EmptyTuple

  def consAndLiteral: (Int, String, Boolean) =
    (99 *: EmptyTuple) ++ ("end", true)

  def chained: (Int, String, Boolean, Box, Long) =
    (1, "two") ++ (true *: EmptyTuple) ++ (new Box("four"), 5L)

  def orderOnce: String = {
    val counter = new Counter
    val result = counter.combined
    val first = result._1
    val second = result._2
    val third = result._3
    val box = result._4
    val fourth = box.value
    first + ":" + second + ":" + third + ":" + fourth + ":" + counter.calls
  }
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Values.first)
    println(Values.second)
    println(Values.third)
    println(Values.fourth.value)
    println(Values.leftIdentity.last)
    println(Values.rightIdentity.head)
    println(Values.bothEmpty.size)
    println(Values.consAndLiteral.last)
    println(Values.chained.last)
    println(Values.orderOnce)
  }
}
)";

  constexpr const char* invalidSource = R"(package demo.invalidtupleconcat

object Values {
  def openLeft(value: Tuple): Tuple = value ++ (1, "x")
  def openRight(value: Tuple): Tuple = (1, "x") ++ value
  val nonTupleLeft = 1 ++ (2, 3)
  val nonTupleRight = (1, 2) ++ 3
  val overflow =
    (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11) ++
      (12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23)
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary = temporary / "scalanative-smoke-tuple-concat";
  const std::filesystem::path output =
      temporary / "scalanative-smoke-tuple-concat.out";
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
      driver.buildSource("TupleConcat.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidTupleConcat.scala", invalidSource, {}, invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("tuple-concat native build failed: " + result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string outputText = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  const std::string_view combined =
      functionText(result.nirText, "demo.tupleconcat.Counter.combined");
  const std::string_view merged =
      functionText(result.nirText, "demo.tupleconcat.Values.merged");
  const std::string_view first =
      functionText(result.nirText, "demo.tupleconcat.Values.first");
  const std::string_view third =
      functionText(result.nirText, "demo.tupleconcat.Values.third");
  const std::string_view fourth =
      functionText(result.nirText, "demo.tupleconcat.Values.fourth");
  const std::string_view leftIdentity =
      functionText(result.nirText, "demo.tupleconcat.Values.leftIdentity");
  const std::string_view rightIdentity =
      functionText(result.nirText, "demo.tupleconcat.Values.rightIdentity");
  const std::string_view bothEmpty =
      functionText(result.nirText, "demo.tupleconcat.Values.bothEmpty");
  const std::string_view cons =
      functionText(result.nirText, "demo.tupleconcat.Values.consAndLiteral");
  const std::string_view chained =
      functionText(result.nirText, "demo.tupleconcat.Values.chained");

  const bool runtimeShape =
      contains(result.nirText, "module @scala.EmptyTuple : @scala.Tuple") &&
      contains(result.nirText, "class @scala.Tuple1 : @scala.Tuple") &&
      contains(result.nirText, "class @scala.Tuple2 : @scala.Tuple") &&
      contains(result.nirText, "class @scala.Tuple3 : @scala.Tuple") &&
      contains(result.nirText, "class @scala.Tuple4 : @scala.Tuple") &&
      contains(result.nirText, "class @scala.Tuple5 : @scala.Tuple") &&
      !contains(result.nirText, "class @scala.Tuple6 : @scala.Tuple");
  const std::size_t leftCall = combined.find("%this.left");
  const std::size_t rightCall = combined.find("%this.right");
  const bool lowered =
      countOccurrences(combined, "%this.left") == 1 &&
      countOccurrences(combined, "%this.right") == 1 && leftCall < rightCall &&
      contains(combined, "new scala.Tuple4") &&
      contains(combined, "%tupleConcat$left$") &&
      contains(combined, "%tupleConcat$right$") &&
      contains(merged, "new scala.Tuple4") && contains(merged, "._1") &&
      contains(merged, "._2") && contains(first, "unbox[Int]") &&
      contains(third, "unbox[Boolean]") &&
      contains(fourth, "as-instance-of[demo.tupleconcat.Box]") &&
      contains(leftIdentity, "scala.EmptyTuple") &&
      contains(leftIdentity, "new scala.Tuple2") &&
      contains(rightIdentity, "scala.EmptyTuple") &&
      contains(rightIdentity, "new scala.Tuple2") &&
      countOccurrences(bothEmpty, "let %tupleConcat$") == 2 &&
      contains(bothEmpty, "%scala.EmptyTuple") && contains(cons, "new scala.Tuple1") &&
      contains(cons, "new scala.Tuple3") &&
      countOccurrences(chained, "let %tupleConcat$left$") == 2 &&
      countOccurrences(chained, "let %tupleConcat$right$") == 2 &&
      contains(chained, "new scala.Tuple3") && contains(chained, "new scala.Tuple5") &&
      !contains(result.nirText, "<malformed-tuple-concat>");
  const bool invalidDiagnosticsMatch =
      !invalid.ok &&
      contains(invalid.diagnosticsText, "++ left operand must be a concrete tuple") &&
      contains(invalid.diagnosticsText, "++ right operand must be a concrete tuple") &&
      contains(invalid.diagnosticsText,
               "++ tuple concatenation supports at most 22 elements in this subset");

  if (status == 0 &&
      outputText ==
          "7\nmiddle\ntrue\nlast\nmiddle\n7\n0\ntrue\n5\nleft:1:true:right:2\n" &&
      runtimeShape && lowered && invalidDiagnosticsMatch) {
    return 0;
  }

  return fail("tuple-concat smoke test failed (status=" + std::to_string(status) +
              ", output='" + outputText + "', diagnostics='" + result.diagnosticsText +
              "', invalid='" + invalid.diagnosticsText + "', combined='" +
              std::string(combined) + "', merged='" + std::string(merged) +
              "', first='" + std::string(first) + "', third='" + std::string(third) +
              "', fourth='" + std::string(fourth) + "', left-identity='" +
              std::string(leftIdentity) + "', right-identity='" +
              std::string(rightIdentity) + "', both-empty='" + std::string(bothEmpty) +
              "', cons='" + std::string(cons) + "', chained='" + std::string(chained) +
              "')");
}

} // namespace

int runSmokeTests13() {
  return smokeTupleConcat();
}
