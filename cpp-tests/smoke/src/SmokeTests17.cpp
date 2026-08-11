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

int smokeStoredPolymorphicFunction() {
  constexpr const char* source = R"(package demo.storedpolymorphicfunction

class Box[A](val value: A)

class Renderer(val prefix: String) {
  val render = [A] => (value: A) => prefix + value.toString

  def own: String = render[Int](7)
}

class ReceiverSource {
  var calls: Int = 0

  def next: Renderer = {
    calls = calls + 1
    new Renderer("selected:")
  }
}

object Values {
  val identity = [A] => (value: A) => value
  val boxed = [A] => (value: A) => new Box(value)

  def member: String =
    identity[Int](42).toString + ":" + identity[String]("same")

  def boxedValue: Box[String] = boxed[String]("boxed")

  def local: String = {
    var calls = 0
    val observed = [A] => (value: A) => {
      calls = calls + 1
      calls + ":" + value.toString
    }
    observed[Int](7) + "|" + observed[String]("x") + ":" + calls
  }

  def selected: String = {
    val source = new ReceiverSource
    val result = source.next.render[Boolean](true)
    result + ":" + source.calls
  }

  def classOwned: String = new Renderer("class:").own
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Values.member)
    println(Values.boxedValue.value)
    println(Values.local)
    println(Values.selected)
    println(Values.classOwned)
  }
}
)";

  constexpr const char* invalidSource =
      R"(package demo.invalidstoredpolymorphicfunction

class Holder {
  val identity = [A] => (value: A) => value
}

object Values {
  val escaped = [A] => (value: A) => value
  val badBody = [A] => (value: A) => value % 2
  val wrongShape = [A] => (value: Int) => value
  var mutable = [A] => (value: A) => value

  val alias = escaped
  val selectedEscape = new Holder().identity
  val missingType = escaped(1)
  val twoTypes = escaped[Int, String](1)
  val missingValue = escaped[Int]()
  val extraValue = escaped[Int](1, 2)
  val wrongArgument = escaped[Int]("x")
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "cpp-scalanative-smoke-stored-polymorphic-function";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-stored-polymorphic-function.out";
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
      "StoredPolymorphicFunction.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidStoredPolymorphicFunction.scala", invalidSource, {}, invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("stored polymorphic function native build failed: " +
                result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string outputText = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  const std::string_view member =
      functionText(result.nirText, "demo.storedpolymorphicfunction.Values.member");
  const std::string_view boxed =
      functionText(result.nirText, "demo.storedpolymorphicfunction.Values.boxedValue");
  const std::string_view local =
      functionText(result.nirText, "demo.storedpolymorphicfunction.Values.local");
  const std::string_view selected =
      functionText(result.nirText, "demo.storedpolymorphicfunction.Values.selected");
  const std::string_view own =
      functionText(result.nirText, "demo.storedpolymorphicfunction.Renderer.own");

  const bool runtimeShape =
      contains(result.nirText, "trait @scala.PolyFunction : @java.lang.Object") &&
      !contains(result.nirText,
                "field @demo.storedpolymorphicfunction.Values.identity") &&
      !contains(result.nirText,
                "field @demo.storedpolymorphicfunction.Renderer.render") &&
      !contains(result.nirText, "let %observed : scala.PolyFunction");
  const bool lowered =
      contains(member, "unbox[Int](block(let %value : Object = box[Int](42)") &&
      contains(member,
               "unbox[String](block(let %value : Object = box[String](\"same\")") &&
      contains(boxed, "new demo.storedpolymorphicfunction.Box(%value)") &&
      countOccurrences(local, "assign %calls") == 2 &&
      countOccurrences(local, "let %value : Object") == 2 &&
      countOccurrences(selected, "%source.next") == 1 &&
      countOccurrences(selected,
                       "let %this : demo.storedpolymorphicfunction.Renderer") == 1 &&
      contains(own, "let %this : demo.storedpolymorphicfunction.Renderer = %this") &&
      countOccurrences(result.nirText, ".apply(") == 0 &&
      !contains(result.nirText, "<unlowered-polymorphic-function>");
  const bool invalidDiagnosticsMatch =
      !invalid.ok &&
      countOccurrences(invalid.diagnosticsText,
                       "remainder operator % requires numeric operands") == 1 &&
      contains(invalid.diagnosticsText,
               "stored polymorphic function literal must have the form "
               "[A] => (value: A) => body") &&
      contains(invalid.diagnosticsText,
               "polymorphic function literals are currently supported only as "
               "direct Tuple.map arguments, direct invocations, or immutable val "
               "initializers") &&
      contains(invalid.diagnosticsText,
               "polymorphic function value escaped must be invoked directly with "
               "one explicit type argument and one value argument") &&
      contains(invalid.diagnosticsText,
               "polymorphic function value identity must be invoked directly with "
               "one explicit type argument and one value argument") &&
      countOccurrences(
          invalid.diagnosticsText,
          "stored polymorphic function invocation requires exactly one explicit "
          "type argument and one value argument") == 4 &&
      contains(invalid.diagnosticsText,
               "argument type String does not conform to polymorphic function "
               "parameter Int");

  if (status == 0 &&
      outputText == "42:same\nboxed\n1:7|2:x:2\nselected:true:1\nclass:7\n" &&
      runtimeShape && lowered && invalidDiagnosticsMatch) {
    return 0;
  }

  return fail("stored polymorphic function smoke test failed (status=" +
              std::to_string(status) + ", output='" + outputText + "', diagnostics='" +
              result.diagnosticsText + "', invalid='" + invalid.diagnosticsText +
              "', member='" + std::string(member) + "', boxed='" + std::string(boxed) +
              "', local='" + std::string(local) + "', selected='" +
              std::string(selected) + "', own='" + std::string(own) + "')");
}

} // namespace

int runSmokeTests17() {
  return smokeStoredPolymorphicFunction();
}
