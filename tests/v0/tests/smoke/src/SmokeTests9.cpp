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

int smokeTupleCons() {
  constexpr const char* source = R"(package demo.tuplecons

class Box(val value: String)

class Counter {
  var count: Int = 0

  def next(value: String): String = {
    count = count + 1
    value
  }
}

object Values {
  def singleton: Int *: EmptyTuple = 42 *: EmptyTuple
  def singletonValue: Int = singleton._1

  def chain: String *: Boolean *: Long *: EmptyTuple =
    "head" *: true *: 9000000000L *: EmptyTuple
  def chainHead: String = chain._1
  def chainMiddle: Boolean = chain._2
  def chainLast: Long = chain._3

  def groupedType: Int *: (String *: EmptyTuple) = 5 *: "grouped" *: EmptyTuple
  def groupedText: String = groupedType._2

  def tail: String *: Boolean *: EmptyTuple = "tail" *: true *: EmptyTuple
  def prepended: Int *: String *: Boolean *: EmptyTuple = 7 *: tail
  def prependedText: String = prepended._2

  def literalTail: Box *: (Int, String) =
    new Box("reference") *: (1, "literal tail")
  def literalTailBox: Box = literalTail._1
  def literalTailText: String = literalTail._3

  def evaluationOrder: String = {
    val counter = new Counter
    val values =
      counter.next("first") *: (counter.next("second"), counter.count)
    values._1 + ":" + values._2 + ":" + values._3
  }
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Values.singletonValue)
    println(Values.chainHead)
    println(Values.chainMiddle)
    println(Values.chainLast)
    println(Values.groupedText)
    println(Values.prependedText)
    println(Values.literalTailBox.value)
    println(Values.literalTailText)
    println(Values.evaluationOrder)
  }
}
)";

  constexpr const char* invalidSource = R"(package demo.invalidtuplecons

object Values {
  val nonTuple = 1 *: 2
  val tooLong =
    1 *: 2 *: 3 *: 4 *: 5 *: 6 *: 7 *: 8 *: 9 *: 10 *: 11 *:
    12 *: 13 *: 14 *: 15 *: 16 *: 17 *: 18 *: 19 *: 20 *: 21 *: 22 *:
    23 *: EmptyTuple
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary = temporary / "scalanative-smoke-tuple-cons";
  const std::filesystem::path output =
      temporary / "scalanative-smoke-tuple-cons.out";
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
      driver.buildSource("TupleCons.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidTupleCons.scala", invalidSource, {}, invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("tuple-cons native build failed: " + result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string outputText = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  const std::string_view singleton =
      functionText(result.nirText, "demo.tuplecons.Values.singleton");
  const std::string_view chain =
      functionText(result.nirText, "demo.tuplecons.Values.chain");
  const std::string_view chainMiddle =
      functionText(result.nirText, "demo.tuplecons.Values.chainMiddle");
  const std::string_view prepended =
      functionText(result.nirText, "demo.tuplecons.Values.prepended");
  const std::string_view literalTail =
      functionText(result.nirText, "demo.tuplecons.Values.literalTail");
  const std::string_view literalTailBox =
      functionText(result.nirText, "demo.tuplecons.Values.literalTailBox");
  const std::string_view order =
      functionText(result.nirText, "demo.tuplecons.Values.evaluationOrder");
  const std::size_t firstEvaluation = order.find("\"first\"");
  const std::size_t secondEvaluation = order.find("\"second\"");

  const bool runtimeShape =
      contains(result.nirText, "module @scala.EmptyTuple : @scala.Tuple") &&
      contains(result.nirText, "class @scala.Tuple1 : @scala.Tuple") &&
      contains(result.nirText, "class @scala.Tuple2 : @scala.Tuple") &&
      contains(result.nirText, "class @scala.Tuple3 : @scala.Tuple") &&
      !contains(result.nirText, "class @scala.Tuple4 : @scala.Tuple");
  const bool lowered = contains(singleton, "let %tupleCons$head$") &&
                       contains(singleton, "let %tupleCons$tail$") &&
                       contains(singleton, "%scala.EmptyTuple") &&
                       contains(singleton, "new scala.Tuple1(box[Int]") &&
                       contains(chain, "new scala.Tuple1(box[Long]") &&
                       contains(chain, "new scala.Tuple2(box[Boolean]") &&
                       contains(chain, "new scala.Tuple3(box[String]") &&
                       contains(chainMiddle, "unbox[Boolean]") &&
                       contains(prepended, ": scala.Tuple2 = %tail") &&
                       contains(prepended, "%tupleCons$tail$") &&
                       contains(prepended, "._1") && contains(prepended, "._2") &&
                       contains(literalTail, "new demo.tuplecons.Box(\"reference\")") &&
                       contains(literalTail, "new scala.Tuple2(box[Int](1), ") &&
                       contains(literalTail, "new scala.Tuple3(%tupleCons$head$") &&
                       contains(literalTailBox, "as-instance-of[demo.tuplecons.Box]") &&
                       firstEvaluation != std::string_view::npos &&
                       secondEvaluation != std::string_view::npos &&
                       firstEvaluation < secondEvaluation &&
                       !contains(result.nirText, "<malformed-tuple-cons>") &&
                       !contains(result.nirText, " *: ");
  const bool invalidDiagnosticsMatch =
      !invalid.ok &&
      contains(invalid.diagnosticsText, "*: right operand must be a concrete tuple") &&
      contains(invalid.diagnosticsText,
               "*: tuple construction supports at most 22 elements in this subset");

  if (status == 0 &&
      outputText ==
          "42\nhead\ntrue\n9000000000\ngrouped\ntail\nreference\nliteral tail\n"
          "first:second:2\n" &&
      runtimeShape && lowered && invalidDiagnosticsMatch) {
    return 0;
  }

  return fail("tuple-cons smoke test failed (status=" + std::to_string(status) +
              ", output='" + outputText + "', diagnostics='" + result.diagnosticsText +
              "', invalid='" + invalid.diagnosticsText + "', singleton='" +
              std::string(singleton) + "', chain='" + std::string(chain) +
              "', prepended='" + std::string(prepended) + "', literal-tail='" +
              std::string(literalTail) + "', order='" + std::string(order) + "')");
}

} // namespace

int runSmokeTests9() {
  return smokeTupleCons();
}
