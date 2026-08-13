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

int smokeRuntimePolymorphicFunctionClosures() {
  constexpr const char* source = R"(package demo.runtimepolyclosure

class Box[A](val value: A)

object Runtime {
  def identity(): [A] => A => A =
    [A] => (value: A) => value

  def text(): [A] => A => String =
    [A] => (value: A) => value.toString

  def constant(): [A] => A => Int =
    [A] => (value: A) => 7

  def boxed(): [A] => A => Box[A] =
    [A] => (value: A) => new Box[A](value)

  def invokeIdentity(function: [A] => A => A): String =
    function[Int](42).toString + ":" + function[String]("same")

  def invokeText(function: [A] => A => String): String =
    function[Boolean](true)

  def forward(function: [A] => A => A): [A] => A => A =
    function

  def passed: String =
    invokeIdentity([A] => (value: A) => value)

  def returned: String = {
    val function = identity()
    val alias = function
    alias[Int](7).toString + ":" + alias[String]("runtime")
  }

  def forwarded: String = {
    val function = forward(identity())
    function[Boolean](false).toString + ":" + function[String]("forward")
  }

  def textValue: String =
    invokeText(text())

  def constantValue: Int = {
    val function = constant()
    function[String]("ignored")
  }

  def boxedValue: String = {
    val function = boxed()
    val number = function[Int](5)
    val text = function[String]("box")
    number.value.toString + ":" + text.value
  }

  def mapped: String = {
    val function = identity()
    val result = (1, "x", true).map(function)
    result._1.toString + ":" + result._2 + ":" + result._3.toString
  }

  def mappedText: String = {
    val function = text()
    val result = (2, false).map(function)
    result._1 + ":" + result._2
  }

  def mappedBoxed: String = {
    val function = boxed()
    val result = (6, "mapped").map(function)
    result._1.value.toString + ":" + result._2.value
  }
}

class Effects(var trace: String) {
  def tupleValue: (Int, String) = {
    trace = trace + "T"
    val result = (3, "z")
    result
  }

  def functionValue(): [A] => A => A = {
    trace = trace + "F"
    Runtime.identity()
  }

  def argumentValue: Int = {
    trace = trace + "A"
    9
  }

  def mapped: String = {
    trace = ""
    val result = tupleValue.map(functionValue())
    trace + ":" + result._1.toString + ":" + result._2
  }

  def empty: String = {
    trace = ""
    EmptyTuple.map(functionValue())
    trace
  }

  def invoked: String = {
    trace = ""
    val result = functionValue()[Int](argumentValue)
    trace + ":" + result.toString
  }
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Runtime.passed)
    println(Runtime.returned)
    println(Runtime.forwarded)
    println(Runtime.textValue)
    println(Runtime.constantValue)
    println(Runtime.boxedValue)
    println(Runtime.mapped)
    println(Runtime.mappedText)
    println(Runtime.mappedBoxed)
    val effects = new Effects("")
    println(effects.mapped)
    println(effects.empty)
    println(effects.invoked)
  }
}
)";

  constexpr const char* invalidSource = R"(package demo.invalidruntimepolyclosure

object Values {
  def identity(): [A] => A => A =
    [A] => (value: A) => value

  def missingValue(function: [A] => A => A): Int =
    function[Int]()

  def acceptsText(function: [A] => A => String): String =
    function[Int](1)

  def wrongSignature: String =
    acceptsText(identity())

  def wrongResult(): [A] => A => String =
    [A] => (value: A) => value
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "cpp-scalanative-smoke-runtime-polymorphic-closure";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-runtime-polymorphic-closure.out";
  std::error_code ignored;
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildOptions options;
  options.action = scalanative::tools::build::BuildAction::BuildBinary;
  options.optimize = true;
  options.outputPath = binary;
  scalanative::support::DiagnosticEngine diagnostics;
  const scalanative::tools::build::BuildResult result = driver.buildSource(
      "RuntimePolymorphicClosure.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidRuntimePolymorphicClosure.scala", invalidSource, {}, invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("runtime polymorphic closure native build failed: " +
                result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string outputText = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  const std::string_view invoke =
      functionText(result.nirText, "demo.runtimepolyclosure.Runtime.invokeIdentity");
  const std::string_view passed =
      functionText(result.nirText, "demo.runtimepolyclosure.Runtime.passed");
  const std::string_view returned =
      functionText(result.nirText, "demo.runtimepolyclosure.Runtime.returned");
  const std::string_view mapped =
      functionText(result.nirText, "demo.runtimepolyclosure.Runtime.mapped");
  const std::string_view mappedText =
      functionText(result.nirText, "demo.runtimepolyclosure.Runtime.mappedText");
  const std::string_view boxedValue =
      functionText(result.nirText, "demo.runtimepolyclosure.Runtime.boxedValue");
  const std::string_view mappedBoxed =
      functionText(result.nirText, "demo.runtimepolyclosure.Runtime.mappedBoxed");
  const std::string_view effectMapped =
      functionText(result.nirText, "demo.runtimepolyclosure.Effects.mapped");
  const std::string_view effectEmpty =
      functionText(result.nirText, "demo.runtimepolyclosure.Effects.empty");
  const std::string_view effectInvoked =
      functionText(result.nirText, "demo.runtimepolyclosure.Effects.invoked");

  const std::size_t mappedReceiver = effectMapped.find("tupleMap$receiver$");
  const std::size_t mappedFunction = effectMapped.find("tupleMap$function$");
  const std::size_t invokedFunction = effectInvoked.find("functionValue");
  const std::size_t invokedArgument = effectInvoked.find("argumentValue");
  const bool runtimeAbi =
      contains(result.nirText, "declare @scala.PolyFunction.apply : "
                               "(scala.PolyFunction,Object)Object") &&
      countOccurrences(result.nirText,
                       "class @demo.runtimepolyclosure.$polyclosure$") >= 4 &&
      countOccurrences(result.nirText, ": @scala.PolyFunction") >= 4 &&
      countOccurrences(result.nirText, ".apply(") >= 12;
  const bool lowered =
      countOccurrences(invoke, ".apply(") == 2 &&
      contains(invoke, "runtimePoly$function$") &&
      contains(passed, "new demo.runtimepolyclosure.$polyclosure$") &&
      contains(returned, "let %function : scala.PolyFunction") &&
      contains(returned, "let %alias : scala.PolyFunction") &&
      contains(mapped, "new scala.Tuple3") &&
      countOccurrences(mapped, ".apply(") == 3 &&
      contains(mappedText, "new scala.Tuple2") &&
      countOccurrences(mappedText, ".apply(") == 2 &&
      countOccurrences(boxedValue, "as-instance-of[demo.runtimepolyclosure.Box]") ==
          2 &&
      contains(mappedBoxed, "new scala.Tuple2") &&
      countOccurrences(mappedBoxed, ".apply(") == 2 &&
      countOccurrences(mappedBoxed, "as-instance-of[demo.runtimepolyclosure.Box]") ==
          2 &&
      !contains(result.nirText, "<unlowered-polymorphic-function>");
  const bool evaluationOrder =
      mappedReceiver != std::string_view::npos &&
      mappedFunction != std::string_view::npos && mappedReceiver < mappedFunction &&
      countOccurrences(effectMapped, "tupleValue") == 1 &&
      countOccurrences(effectMapped, "functionValue") == 1 &&
      countOccurrences(effectEmpty, "functionValue") == 1 &&
      countOccurrences(effectEmpty, ".apply(") == 0 &&
      invokedFunction != std::string_view::npos &&
      invokedArgument != std::string_view::npos && invokedFunction < invokedArgument;
  const bool invalidDiagnosticsMatch =
      !invalid.ok &&
      contains(invalid.diagnosticsText,
               "runtime polymorphic function invocation requires exactly one "
               "explicit type argument and one value argument") &&
      contains(invalid.diagnosticsText,
               "polymorphic function argument signature does not conform to "
               "parameter signature") &&
      contains(invalid.diagnosticsText,
               "polymorphic function result type A does not conform to declared "
               "result type String");

  if (status == 0 &&
      outputText == "42:same\n7:runtime\nfalse:forward\ntrue\n7\n5:box\n"
                    "1:x:true\n2:false\n6:mapped\nTF:3:z\nF\nFA:9\n" &&
      runtimeAbi && lowered && evaluationOrder && invalidDiagnosticsMatch) {
    return 0;
  }

  return fail("runtime polymorphic closure smoke test failed (status=" +
              std::to_string(status) + ", output='" + outputText + "', diagnostics='" +
              result.diagnosticsText + "', invalid='" + invalid.diagnosticsText +
              "', invoke='" + std::string(invoke) + "', passed='" +
              std::string(passed) + "', returned='" + std::string(returned) +
              "', mapped='" + std::string(mapped) + "', mappedText='" +
              std::string(mappedText) + "', boxedValue='" + std::string(boxedValue) +
              "', mappedBoxed='" + std::string(mappedBoxed) + "', effectMapped='" +
              std::string(effectMapped) + "', effectEmpty='" +
              std::string(effectEmpty) + "', effectInvoked='" +
              std::string(effectInvoked) + "')");
}

} // namespace

int runSmokeTests19() {
  return smokeRuntimePolymorphicFunctionClosures();
}
