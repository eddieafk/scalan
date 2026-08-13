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

int smokeCapturedRuntimePolymorphicFunctionClosures() {
  constexpr const char* source = R"(package demo.capturedruntimepolyclosure

class Box[A](val value: A)

class Renderer(var prefix: String, val suffix: String) {
  def decorate(value: String): String = prefix + value + suffix

  def function(): [A] => A => String =
    [A] => (value: A) => decorate(value.toString)

  def explicitThis(): [A] => A => String =
    [A] => (value: A) => this.prefix + value.toString + this.suffix
}

object Runtime {
  var objectPrefix = "before-object:"

  def render(prefix: String, suffix: String): [A] => A => String =
    [A] => (value: A) => prefix + value.toString + suffix

  def constant(number: Int): [A] => A => Int =
    [A] => (ignored: A) => number

  def retain[A](captured: A): [B] => B => A =
    [B] => (ignored: B) => captured

  def objectMember(): [A] => A => String =
    [A] => (value: A) => objectPrefix + value.toString

  def invokeText(function: [A] => A => String): String =
    function[Boolean](true)

  def passed: String = {
    val prefix = "passed:"
    invokeText([A] => (value: A) => prefix + value.toString)
  }

  def returned: String = {
    val function = render("returned:", "!")
    val alias = function
    alias[Int](7)
  }

  def mapped: String = {
    val function = render("", "!")
    val result = (1, false).map(function)
    result._1 + ":" + result._2
  }

  def receiver: String = {
    val renderer = new Renderer("before:", "!")
    val function = renderer.function()
    renderer.prefix = "after:"
    function[Int](9)
  }

  def explicitReceiver: String = {
    val renderer = new Renderer("explicit:", "?")
    renderer.explicitThis()[Boolean](true)
  }

  def primitiveCapture: Int = {
    val function = constant(11)
    function[String]("ignored")
  }

  def genericCapture: String = {
    val function = retain[Box[String]](new Box[String]("kept"))
    function[Int](1).value
  }

  def directFactory: String =
    render("factory:", "?")[String]("ok")

  def objectCapture: String = {
    val function = objectMember()
    Runtime.objectPrefix = "object:"
    function[Int](5)
  }
}

class Effects(var trace: String) {
  def tupleValue: (Int, String) = {
    trace = trace + "T"
    val result = (3, "z")
    result
  }

  def functionValue(): [A] => A => String = {
    trace = trace + "F"
    Runtime.render("", "!")
  }

  def mapped: String = {
    trace = ""
    val result = tupleValue.map(functionValue())
    trace + ":" + result._1 + ":" + result._2
  }

  def empty: String = {
    trace = ""
    EmptyTuple.map(functionValue())
    trace
  }
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Runtime.passed)
    println(Runtime.returned)
    println(Runtime.mapped)
    println(Runtime.receiver)
    println(Runtime.explicitReceiver)
    println(Runtime.primitiveCapture)
    println(Runtime.genericCapture)
    println(Runtime.directFactory)
    println(Runtime.objectCapture)
    val effects = new Effects("")
    println(effects.mapped)
    println(effects.empty)
  }
}
)";

  constexpr const char* invalidSource =
      R"(package demo.invalidcapturedruntimepolyclosure

object Invalid {
  def mutableLocal(): [A] => A => String = {
    var prefix = "mutable:";
    [A] => (value: A) => prefix + value.toString
  }

  def erasedLocal(): [A] => A => String = {
    val identity = [A] => (value: A) => value;
    [A] => (value: A) => identity[String]("erased")
  }
}

class Parent {
  def name: String = "parent"
}

class Child extends Parent {
  def fromSuper(): [A] => A => String =
    [A] => (value: A) => super.name + value.toString
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "cpp-scalanative-smoke-captured-runtime-poly-closure";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-captured-runtime-poly-closure.out";
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
      "CapturedRuntimePolymorphicClosure.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid =
      driver.buildSource("InvalidCapturedRuntimePolymorphicClosure.scala",
                         invalidSource, {}, invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("captured runtime polymorphic closure native build failed: " +
                result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string outputText = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  const std::string_view render =
      functionText(result.nirText, "demo.capturedruntimepolyclosure.Runtime.render");
  const std::string_view retain =
      functionText(result.nirText, "demo.capturedruntimepolyclosure.Runtime.retain");
  const std::string_view passed =
      functionText(result.nirText, "demo.capturedruntimepolyclosure.Runtime.passed");
  const std::string_view returned =
      functionText(result.nirText, "demo.capturedruntimepolyclosure.Runtime.returned");
  const std::string_view mapped =
      functionText(result.nirText, "demo.capturedruntimepolyclosure.Runtime.mapped");
  const std::string_view receiver =
      functionText(result.nirText, "demo.capturedruntimepolyclosure.Renderer.function");
  const std::string_view explicitReceiver = functionText(
      result.nirText, "demo.capturedruntimepolyclosure.Renderer.explicitThis");
  const std::string_view effectMapped =
      functionText(result.nirText, "demo.capturedruntimepolyclosure.Effects.mapped");
  const std::string_view effectEmpty =
      functionText(result.nirText, "demo.capturedruntimepolyclosure.Effects.empty");

  const std::size_t mappedReceiver = effectMapped.find("tupleMap$receiver$");
  const std::size_t mappedFunction = effectMapped.find("tupleMap$function$");
  const bool closureAbi =
      contains(result.nirText, "declare @scala.PolyFunction.apply : "
                               "(scala.PolyFunction,Object)Object") &&
      countOccurrences(result.nirText,
                       "class @demo.capturedruntimepolyclosure.$polyclosure$") >= 6 &&
      countOccurrences(result.nirText,
                       "field @demo.capturedruntimepolyclosure.$polyclosure$") >= 7 &&
      countOccurrences(result.nirText,
                       "$capture$this : "
                       "demo.capturedruntimepolyclosure.Renderer") == 2 &&
      contains(result.nirText, "$capture$0 : Int") &&
      contains(result.nirText, "$capture$0 : Object");
  const bool captureLowering =
      contains(render, "(%prefix, %suffix)") && contains(retain, "(%captured)") &&
      contains(passed, "new demo.capturedruntimepolyclosure.$polyclosure$") &&
      contains(returned, "let %alias : scala.PolyFunction") &&
      contains(receiver, "(%this)") && contains(explicitReceiver, "(%this)") &&
      contains(result.nirText, ".$capture$this.decorate(") &&
      contains(result.nirText, ".$capture$this.prefix") &&
      contains(result.nirText, ".$capture$this.suffix") &&
      contains(result.nirText, "%this.$capture$0") &&
      !contains(result.nirText, "<unlowered-polymorphic-function>");
  const bool runtimeMapping = contains(mapped, "new scala.Tuple2") &&
                              countOccurrences(mapped, ".apply(") == 2 &&
                              mappedReceiver != std::string_view::npos &&
                              mappedFunction != std::string_view::npos &&
                              mappedReceiver < mappedFunction &&
                              countOccurrences(effectMapped, "tupleValue") == 1 &&
                              countOccurrences(effectMapped, "functionValue") == 1 &&
                              countOccurrences(effectMapped, ".apply(") == 2 &&
                              countOccurrences(effectEmpty, "functionValue") == 1 &&
                              countOccurrences(effectEmpty, ".apply(") == 0;
  const bool invalidDiagnosticsMatch =
      !invalid.ok &&
      contains(invalid.diagnosticsText,
               "runtime polymorphic function closures cannot capture mutable "
               "local values yet: prefix") &&
      contains(invalid.diagnosticsText,
               "runtime polymorphic function closures cannot capture erased "
               "compiler-known values yet: identity") &&
      contains(invalid.diagnosticsText,
               "runtime polymorphic function closures cannot capture super yet");

  if (status == 0 &&
      outputText == "passed:true\nreturned:7!\n1!:false!\nafter:9!\n"
                    "explicit:true?\n11\nkept\nfactory:ok?\nobject:5\n"
                    "TF:3!:z!\nF\n" &&
      closureAbi && captureLowering && runtimeMapping && invalidDiagnosticsMatch) {
    return 0;
  }

  return fail(
      "captured runtime polymorphic closure smoke test failed (status=" +
      std::to_string(status) + ", output='" + outputText + "', diagnostics='" +
      result.diagnosticsText + "', invalid='" + invalid.diagnosticsText +
      "', render='" + std::string(render) + "', retain='" + std::string(retain) +
      "', passed='" + std::string(passed) + "', returned='" + std::string(returned) +
      "', mapped='" + std::string(mapped) + "', receiver='" + std::string(receiver) +
      "', explicitReceiver='" + std::string(explicitReceiver) + "', effectMapped='" +
      std::string(effectMapped) + "', effectEmpty='" + std::string(effectEmpty) + "')");
}

} // namespace

int runSmokeTests20() {
  return smokeCapturedRuntimePolymorphicFunctionClosures();
}
