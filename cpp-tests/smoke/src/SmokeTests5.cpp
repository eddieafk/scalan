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
}

object Main {
  given intNamed: Named[Int] = new NamedValue[Int]("member-int")

  def intSelected: String = Selectors.selected[Int]

  def localSelected: String = {
    given localString: Named[String] =
      new NamedValue[String]("local-string")
    Selectors.selected[String]()
  }

  def fallbackSelected: String = Selectors.selected[Boolean]
  def nestedSelected: String = Selectors.nested[Int]

  def main(args: Array[String]): Unit = {
    println(intSelected)
    println(localSelected)
    println(fallbackSelected)
    println(nestedSelected)
  }
}
)";
  constexpr const char* invalidSource =
      R"(package demo.invalidinlinecalls

object Main {
  inline def nonGeneric: String = "non-generic"
  inline def withValue[A](value: A): String = "value-parameter"
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

  const bool valid =
      status == 0 &&
      text == "selected:member-int\nselected:local-string\nfallback\n"
              "nested:selected:member-int\n" &&
      !invalid.ok &&
      contains(invalid.diagnosticsText,
               "inline call-site specialization currently requires a generic "
               "method") &&
      contains(invalid.diagnosticsText,
               "inline call-site specialization currently requires a "
               "parameterless method") &&
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
      !contains(nestedSelected, "%demo.inlinecalls.Selectors.selected");
  return valid ? 0
               : fail("inline summonFrom smoke test failed (output='" + text +
                      "', diagnostics='" + result.diagnosticsText +
                      "', invalid-diagnostics='" + invalid.diagnosticsText +
                      "', int-selected='" + std::string(intSelected) +
                      "', local-selected='" + std::string(localSelected) +
                      "', fallback-selected='" + std::string(fallbackSelected) +
                      "', nested-selected='" + std::string(nestedSelected) + "')");
}

} // namespace

int runSmokeTests5() {
  return smokeInlineSummonFrom();
}
