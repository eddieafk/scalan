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

int smokePolymorphicFunctionInvocation() {
  constexpr const char* source = R"(package demo.polymorphicfunctioninvocation

class Box[A](val value: A)
class Token(val label: String)

class Source {
  var calls: Int = 0

  def next: Int = {
    calls = calls + 1
    7
  }
}

object Values {
  def identity: Int = ([A] => (value: A) => value)[Int](42)

  def text: String =
    ([A] => (value: A) => value.toString)[Boolean](true)

  def boxed: Box[String] =
    ([A] => (value: A) => new Box(value))[String]("boxed")

  def constant: Int =
    ([A] => (value: A) => 9)[String]("ignored")

  def reference: Token =
    ([A] => (value: A) => value)[Token](new Token("reference"))

  def observed: String = {
    val source = new Source
    var calls = 0
    val result = ([A] => (value: A) => {
      calls = calls + 1
      calls + ":" + value.toString
    })[Int](source.next)
    result + ":" + calls + ":" + source.calls
  }
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Values.identity)
    println(Values.text)
    println(Values.boxed.value)
    println(Values.constant)
    println(Values.reference.label)
    println(Values.observed)
  }
}
)";

  constexpr const char* invalidSource =
      R"(package demo.invalidpolymorphicfunctioninvocation

object Values {
  val missingType = ([A] => (value: A) => value)(1)
  val twoTypes = ([A] => (value: A) => value)[Int, String](1)
  val missingValue = ([A] => (value: A) => value)[Int]()
  val extraValue = ([A] => (value: A) => value)[Int](1, 2)
  val wrongArgument = ([A] => (value: A) => value)[Int]("x")
  val wrongShape = ([A] => (value: Int) => value)[Int](1)
  val nonGenericBody = ([A] => (value: A) => value % 2)[Int](1)
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "scalanative-smoke-polymorphic-function-invocation";
  const std::filesystem::path output =
      temporary / "scalanative-smoke-polymorphic-function-invocation.out";
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
      "PolymorphicFunctionInvocation.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid =
      driver.buildSource("InvalidPolymorphicFunctionInvocation.scala", invalidSource,
                         {}, invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("polymorphic function invocation native build failed: " +
                result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string outputText = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  const std::string_view identity = functionText(
      result.nirText, "demo.polymorphicfunctioninvocation.Values.identity");
  const std::string_view text =
      functionText(result.nirText, "demo.polymorphicfunctioninvocation.Values.text");
  const std::string_view boxed =
      functionText(result.nirText, "demo.polymorphicfunctioninvocation.Values.boxed");
  const std::string_view constant = functionText(
      result.nirText, "demo.polymorphicfunctioninvocation.Values.constant");
  const std::string_view reference = functionText(
      result.nirText, "demo.polymorphicfunctioninvocation.Values.reference");
  const std::string_view observed = functionText(
      result.nirText, "demo.polymorphicfunctioninvocation.Values.observed");

  const bool runtimeShape =
      contains(result.nirText, "trait @scala.PolyFunction : @java.lang.Object") &&
      !contains(result.nirText, "class @scala.Tuple1 : @scala.Tuple") &&
      !contains(result.nirText, "class @scala.Tuple2 : @scala.Tuple") &&
      !contains(result.nirText, "class @scala.Tuple3 : @scala.Tuple");
  const bool lowered =
      contains(identity,
               "unbox[Int](block(let %value : Object = box[Int](42); %value))") &&
      contains(text, "let %value : Object = box[Boolean](true)") &&
      contains(text, "anyReceiverToString(%value)") &&
      contains(boxed, "let %value : Object = box[String](\"boxed\")") &&
      contains(boxed, "new demo.polymorphicfunctioninvocation.Box(%value)") &&
      contains(constant, "let %value : Object = box[String](\"ignored\"); 9") &&
      contains(reference, "as-instance-of[demo.polymorphicfunctioninvocation.Token]") &&
      countOccurrences(reference, "new demo.polymorphicfunctioninvocation.Token") ==
          1 &&
      countOccurrences(observed, "%source.next") == 1 &&
      countOccurrences(observed, "assign %calls") == 1 &&
      countOccurrences(observed, "let %value : Object") == 1 &&
      countOccurrences(result.nirText, ".apply(") == 0 &&
      !contains(result.nirText, "<unlowered-polymorphic-function>");
  const bool invalidDiagnosticsMatch =
      !invalid.ok &&
      contains(invalid.diagnosticsText,
               "polymorphic function invocation requires exactly one explicit "
               "type argument and one value argument") &&
      contains(invalid.diagnosticsText,
               "argument type String does not conform to polymorphic function "
               "parameter Int") &&
      contains(invalid.diagnosticsText,
               "polymorphic function literal must have the form "
               "[A] => (value: A) => body") &&
      contains(invalid.diagnosticsText,
               "remainder operator % requires numeric operands");

  if (status == 0 && outputText == "42\ntrue\nboxed\n9\nreference\n1:7:1:1\n" &&
      runtimeShape && lowered && invalidDiagnosticsMatch) {
    return 0;
  }

  return fail("polymorphic function invocation smoke test failed (status=" +
              std::to_string(status) + ", output='" + outputText + "', diagnostics='" +
              result.diagnosticsText + "', invalid='" + invalid.diagnosticsText +
              "', identity='" + std::string(identity) + "', text='" +
              std::string(text) + "', boxed='" + std::string(boxed) + "', constant='" +
              std::string(constant) + "', reference='" + std::string(reference) +
              "', observed='" + std::string(observed) + "')");
}

} // namespace

int runSmokeTests16() {
  return smokePolymorphicFunctionInvocation();
}
