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

int smokeQualifiedCompanionCases() {
  constexpr const char* source = R"(package demo.qualifiedcases

trait Ordinal[A] {
  def ordinal(value: A): Int
}

class DerivedOrdinal[A](val mirror: scala.deriving.Mirror.SumOf[A])
    extends Ordinal[A] {
  override def ordinal(value: A): Int = mirror.ordinal(value)
}

class StringOrdinal extends Ordinal[String] {
  override def ordinal(value: String): Int = 99
}

object Ordinal {
  given stringOrdinal: Ordinal[String] = new StringOrdinal

  def derived[A](using mirror: scala.deriving.Mirror.SumOf[A]): Ordinal[A] =
    new DerivedOrdinal[A](mirror)
}

sealed trait Event derives Ordinal
object Event {
  object Started extends Event
  class Stopped(val code: Int) extends Event
}

sealed trait Maybe[+A] derives Ordinal
object Maybe {
  object Empty extends Maybe[Nothing]
  class Present[+A](val value: A) extends Maybe[A]
}

object Main {
  def ordinal[A](value: A)(using instance: Ordinal[A]): Int =
    instance.ordinal(value)

  def stopped(value: Event.Stopped): Event.Stopped = value

  def main = {
    println(ordinal[Event](Event.Started))
    println(ordinal[Event](new Event.Stopped(2)))
    println(ordinal[Maybe[String]](Maybe.Empty))
    println(ordinal[Maybe[String]](new Maybe.Present[String]("value")))
    println(stopped(new Event.Stopped(7)).code)
  }
}
)";
  constexpr const char* invalidSource = R"(package demo.invalidqualifiedcase

sealed trait Event
object Event {
  object Started extends Event
}

object Main {
  def value = new Event.Started()
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "cpp-scalanative-smoke-qualified-companion-cases";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-qualified-companion-cases.out";
  std::error_code ignored;
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildOptions options;
  options.action = scalanative::tools::build::BuildAction::BuildBinary;
  options.outputPath = binary;
  scalanative::support::DiagnosticEngine diagnostics;
  const scalanative::tools::build::BuildResult result =
      driver.buildSource("QualifiedCompanionCases.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidQualifiedCompanionCase.scala", invalidSource, {}, invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("qualified companion cases native build failed: " +
                result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string text = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  const bool valid =
      status == 0 && text == "0\n1\n0\n1\n7\n" && !invalid.ok &&
      contains(invalid.diagnosticsText,
               "constructor target is not a class: Event.Started") &&
      contains(result.nirText, "module @demo.qualifiedcases.Event$.Started : "
                               "@demo.qualifiedcases.Event") &&
      contains(result.nirText, "class @demo.qualifiedcases.Event$.Stopped : "
                               "@demo.qualifiedcases.Event") &&
      contains(result.nirText, "module @demo.qualifiedcases.Maybe$.Empty : "
                               "@demo.qualifiedcases.Maybe") &&
      contains(result.nirText, "class @demo.qualifiedcases.Maybe$.Present : "
                               "@demo.qualifiedcases.Maybe") &&
      contains(result.nirText, "new demo.qualifiedcases.Event$.Stopped") &&
      contains(result.nirText, "new demo.qualifiedcases.Maybe$.Present");
  return valid ? 0 : fail("qualified companion object/class access smoke test failed");
}

} // namespace

int runSmokeTests3() {
  return smokeQualifiedCompanionCases();
}
