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

int smokeInlineSummonFrom() {
  constexpr const char* source = R"(package demo.inlinecalls

trait Named[A] {
  def label(): String
}

class NamedValue[A](val value: String) extends Named[A] {
  def label(): String = value
}

object Selectors {
  val prefix: String = "selected:"

  inline def selected[A]: String =
    summonFrom {
      case found: Named[A] => prefix + found.label()
      case _ => "fallback"
    }

  inline def nested[A]: String = "nested:" + selected[A]

  inline def decorated[A](value: String): String =
    summonFrom {
      case found: Named[A] =>
        prefix + found.label() + ":" + value + ":" + value
      case _ => "fallback:" + value + ":" + value
    }

  inline def nestedValue[A](value: String): String =
    "nested-value:" + decorated[A](value)

  inline def passed[A](value: A): A = value
}

object Main {
  given intNamed: Named[Int] = new NamedValue[Int]("member-int")

  def nextValue(): String = {
    println("effect")
    "value"
  }

  def intSelected: String = Selectors.selected[Int]

  def localSelected: String = {
    given localString: Named[String] =
      new NamedValue[String]("local-string")
    Selectors.selected[String]()
  }

  def fallbackSelected: String = Selectors.selected[Boolean]
  def nestedSelected: String = Selectors.nested[Int]
  def valueSelected: String = Selectors.decorated[Int](nextValue())
  def valueFallback: String = Selectors.decorated[Boolean]("bool")
  def nestedValueSelected: String = Selectors.nestedValue[Int]("nested")
  def passedInt: Int = Selectors.passed[Int](42)
  def passedString: String = Selectors.passed[String]("forty-two")

  def main(args: Array[String]): Unit = {
    println(intSelected)
    println(localSelected)
    println(fallbackSelected)
    println(nestedSelected)
    println(valueSelected)
    println(valueFallback)
    println(nestedValueSelected)
    println(passedInt)
    println(passedString)
  }
}
)";
  constexpr const char* invalidSource =
      R"(package demo.invalidinlinecalls

object Main {
  inline def nonGeneric: String = "non-generic"
  inline def withContext[A](using value: A): String = "context-parameter"
  inline def inferred[A](value: A): A = value
  def unsupportedInference: Int = inferred(1)
  inline def missingBody[A]: String
  inline def recursive[A]: String = recursive[A]
  inline val unsupported: String = "value"
}

class Instance {
  inline def unsupportedInstance[A]: String = "instance"
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "cpp-scalanative-smoke-inline-summon-from";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-inline-summon-from.out";
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
      driver.buildSource("InlineSummonFrom.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidInlineSummonFrom.scala", invalidSource, {}, invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("inline summonFrom native build failed: " + result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string text = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  const std::string_view intSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.intSelected");
  const std::string_view localSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.localSelected");
  const std::string_view fallbackSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.fallbackSelected");
  const std::string_view nestedSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.nestedSelected");
  const std::string_view valueSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.valueSelected");
  const std::string_view valueFallback =
      functionText(result.nirText, "demo.inlinecalls.Main.valueFallback");
  const std::string_view nestedValueSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.nestedValueSelected");
  const std::string_view passedInt =
      functionText(result.nirText, "demo.inlinecalls.Main.passedInt");
  const std::string_view passedString =
      functionText(result.nirText, "demo.inlinecalls.Main.passedString");

  const bool valid =
      status == 0 &&
      text == "selected:member-int\nselected:local-string\nfallback\n"
              "nested:selected:member-int\n"
              "effect\nselected:member-int:value:value\n"
              "fallback:bool:bool\n"
              "nested-value:selected:member-int:nested:nested\n"
              "42\nforty-two\n" &&
      !invalid.ok &&
      contains(invalid.diagnosticsText,
               "inline call-site specialization currently requires a generic "
               "method") &&
      contains(invalid.diagnosticsText,
               "inline call-site specialization does not support contextual "
               "parameters yet") &&
      contains(invalid.diagnosticsText,
               "inline call-site specialization currently requires explicit type "
               "arguments") &&
      contains(invalid.diagnosticsText, "inline method requires an implementation") &&
      contains(invalid.diagnosticsText,
               "recursive inline call-site specialization is not supported yet") &&
      contains(invalid.diagnosticsText,
               "inline call-site specialization currently requires a top-level or "
               "object method") &&
      contains(invalid.diagnosticsText,
               "'inline' must modify a def in this milestone") &&
      contains(intSelected, "call %demo.inlinecalls.Main.intNamed()") &&
      contains(intSelected, "call %demo.inlinecalls.Selectors.prefix()") &&
      !contains(intSelected, "%demo.inlinecalls.Selectors.selected") &&
      contains(localSelected, "local-string") &&
      contains(localSelected, "call %demo.inlinecalls.Selectors.prefix()") &&
      !contains(localSelected, "%demo.inlinecalls.Selectors.selected") &&
      contains(fallbackSelected, "\"fallback\"") &&
      !contains(fallbackSelected, "%demo.inlinecalls.Selectors.prefix") &&
      !contains(fallbackSelected, "%demo.inlinecalls.Selectors.selected") &&
      contains(nestedSelected, "\"nested:\"") &&
      contains(nestedSelected, "call %demo.inlinecalls.Selectors.prefix()") &&
      !contains(nestedSelected, "%demo.inlinecalls.Selectors.nested") &&
      !contains(nestedSelected, "%demo.inlinecalls.Selectors.selected") &&
      contains(valueSelected, "let %value : String") &&
      countOccurrences(valueSelected, "nextValue") == 1 &&
      contains(valueSelected, "call %demo.inlinecalls.Selectors.prefix()") &&
      !contains(valueSelected, "%demo.inlinecalls.Selectors.decorated") &&
      contains(valueFallback, "\"fallback:\"") &&
      !contains(valueFallback, "%demo.inlinecalls.Selectors.prefix") &&
      !contains(valueFallback, "%demo.inlinecalls.Selectors.decorated") &&
      contains(nestedValueSelected, "\"nested-value:\"") &&
      !contains(nestedValueSelected, "%demo.inlinecalls.Selectors.nestedValue") &&
      !contains(nestedValueSelected, "%demo.inlinecalls.Selectors.decorated") &&
      contains(passedInt, "let %value : Int = 42") &&
      !contains(passedInt, "%demo.inlinecalls.Selectors.passed") &&
      contains(passedString, "let %value : String = \"forty-two\"") &&
      !contains(passedString, "%demo.inlinecalls.Selectors.passed");
  return valid ? 0
               : fail("inline summonFrom smoke test failed (output='" + text +
                      "', diagnostics='" + result.diagnosticsText +
                      "', invalid-diagnostics='" + invalid.diagnosticsText +
                      "', int-selected='" + std::string(intSelected) +
                      "', local-selected='" + std::string(localSelected) +
                      "', fallback-selected='" + std::string(fallbackSelected) +
                      "', nested-selected='" + std::string(nestedSelected) +
                      "', value-selected='" + std::string(valueSelected) +
                      "', value-fallback='" + std::string(valueFallback) +
                      "', nested-value-selected='" + std::string(nestedValueSelected) +
                      "', passed-int='" + std::string(passedInt) +
                      "', passed-string='" + std::string(passedString) + "')");
}

} // namespace

int runSmokeTests5() {
  return smokeInlineSummonFrom();
}
