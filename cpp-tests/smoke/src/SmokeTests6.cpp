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

int smokeInlineErasedValue() {
  constexpr const char* source = R"(package demo.inlineerased

import scala.compiletime.erasedValue
import scala.compiletime.{erasedValue => erased}
import scala.compiletime.constValue
import scala.compiletime.{constValue => constant}

trait ErasedResult {
  def text(): String
}

class PreciseResult(val value: String) extends ErasedResult {
  def text(): String = value
  def preciseOnly(): String = value
}

class FallbackResult(val value: String) extends ErasedResult {
  def text(): String = value
  def fallbackOnly(): String = value
}

class Other

object TypeKinds {
  transparent inline def valueOf[T] = constValue[T]

  transparent inline def qualifiedValueOf[T] =
    scala.compiletime.constValue[T]

  transparent inline def aliasedValueOf[T] = constant[T]

  transparent inline def integerChoice[N]: String =
    inline constValue[N] match {
      case 0 => "constant-zero"
      case 7 => "constant-seven"
      case _ => "constant-other"
    }

  transparent inline def booleanChoice[B]: String =
    inline constValue[B] match {
      case true => "constant-true"
      case _ => "constant-false"
    }

  transparent inline def constantResult[B]: ErasedResult =
    inline constValue[B] match {
      case true => new PreciseResult("constant-precise")
      case _ => new FallbackResult("constant-fallback")
    }

  transparent inline def nameOf[T]: String =
    inline erasedValue[T] match {
      case _: String => "string"
      case _: Int => "int"
      case _: Long => "long"
      case _ => "other"
    }

  transparent inline def aliasNameOf[T]: String =
    inline erased[T] match {
      case _: String => "alias-string"
      case _ => "alias-other"
    }

  transparent inline def resultFor[T]: ErasedResult =
    inline scala.compiletime.erasedValue[T] match {
      case _: String => new PreciseResult("precise")
      case _ => new FallbackResult("fallback")
    }
}

object Main {
  def directConstant: Int = constValue[11]
  def intConstant: Int = TypeKinds.valueOf[42]
  def longConstant: Long = TypeKinds.qualifiedValueOf[9000000000L]
  def stringConstant: String = TypeKinds.aliasedValueOf["literal"]
  def booleanConstant: Boolean = TypeKinds.valueOf[true]
  def charConstant: Char = TypeKinds.valueOf['x']
  def constantMatchSeven: String = TypeKinds.integerChoice[7]
  def constantMatchOther: String = TypeKinds.integerChoice[9]
  def constantBooleanFalse: String = TypeKinds.booleanChoice[false]
  def constantPrecise: String = TypeKinds.constantResult[true].preciseOnly()
  def constantFallback: String = TypeKinds.constantResult[false].fallbackOnly()
  def stringName: String = TypeKinds.nameOf[String]
  def intName: String = TypeKinds.nameOf[Int]
  def longName: String = TypeKinds.nameOf[Long]
  def otherName: String = TypeKinds.nameOf[Other]
  def aliasStringName: String = TypeKinds.aliasNameOf[String]
  def aliasOtherName: String = TypeKinds.aliasNameOf[Other]
  def preciseResult: String = TypeKinds.resultFor[String].preciseOnly()
  def fallbackResult: String = TypeKinds.resultFor[Other].fallbackOnly()

  def main(args: Array[String]): Unit = {
    println(directConstant)
    println(intConstant)
    println(longConstant)
    println(stringConstant)
    println(booleanConstant)
    println(charConstant)
    println(constantMatchSeven)
    println(constantMatchOther)
    println(constantBooleanFalse)
    println(constantPrecise)
    println(constantFallback)
    println(stringName)
    println(intName)
    println(longName)
    println(otherName)
    println(aliasStringName)
    println(aliasOtherName)
    println(preciseResult)
    println(fallbackResult)
  }
}
)";

  constexpr const char* invalidSource = R"(package demo.invaliderased

import scala.compiletime.erasedValue
import scala.compiletime.constValue

object Main {
  val nonConstant = constValue[String]
  val malformedConstant = constValue[1, 2]

  def unresolvedConstant[T] = constValue[T]

  inline def deferredConstant[T] = constValue[T]

  def unresolvedConstantCall[A] = deferredConstant[A]

  val escaped = erasedValue[String]
  val qualifiedEscaped = scala.compiletime.erasedValue[Int]
  val malformed = erasedValue[String, Int]

  inline def bound[T]: String =
    inline erasedValue[T] match {
      case value: String => value
      case _ => "fallback"
    }

  inline def catchBound[T]: String =
    inline erasedValue[T] match {
      case _: String => "string"
      case other => other.toString
    }

  inline def classify[T]: String =
    inline erasedValue[T] match {
      case _: String => "string"
      case _ => "other"
    }

  def unresolved[A]: String = classify[A]

  inline def ordinary[T]: String =
    erasedValue[T] match {
      case _: String => "string"
      case _ => "other"
    }
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "cpp-scalanative-smoke-inline-erased-value";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-inline-erased-value.out";
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
      driver.buildSource("InlineErasedValue.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidInlineErasedValue.scala", invalidSource, {}, invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("inline erasedValue native build failed: " + result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string outputText = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  const std::string_view stringName =
      functionText(result.nirText, "demo.inlineerased.Main.stringName");
  const std::string_view directConstant =
      functionText(result.nirText, "demo.inlineerased.Main.directConstant");
  const std::string_view intConstant =
      functionText(result.nirText, "demo.inlineerased.Main.intConstant");
  const std::string_view longConstant =
      functionText(result.nirText, "demo.inlineerased.Main.longConstant");
  const std::string_view stringConstant =
      functionText(result.nirText, "demo.inlineerased.Main.stringConstant");
  const std::string_view booleanConstant =
      functionText(result.nirText, "demo.inlineerased.Main.booleanConstant");
  const std::string_view charConstant =
      functionText(result.nirText, "demo.inlineerased.Main.charConstant");
  const std::string_view constantMatchSeven =
      functionText(result.nirText, "demo.inlineerased.Main.constantMatchSeven");
  const std::string_view constantMatchOther =
      functionText(result.nirText, "demo.inlineerased.Main.constantMatchOther");
  const std::string_view constantBooleanFalse = functionText(
      result.nirText, "demo.inlineerased.Main.constantBooleanFalse");
  const std::string_view constantPrecise =
      functionText(result.nirText, "demo.inlineerased.Main.constantPrecise");
  const std::string_view constantFallback =
      functionText(result.nirText, "demo.inlineerased.Main.constantFallback");
  const std::string_view intName =
      functionText(result.nirText, "demo.inlineerased.Main.intName");
  const std::string_view longName =
      functionText(result.nirText, "demo.inlineerased.Main.longName");
  const std::string_view otherName =
      functionText(result.nirText, "demo.inlineerased.Main.otherName");
  const std::string_view aliasStringName =
      functionText(result.nirText, "demo.inlineerased.Main.aliasStringName");
  const std::string_view aliasOtherName =
      functionText(result.nirText, "demo.inlineerased.Main.aliasOtherName");
  const std::string_view preciseResult =
      functionText(result.nirText, "demo.inlineerased.Main.preciseResult");
  const std::string_view fallbackResult =
      functionText(result.nirText, "demo.inlineerased.Main.fallbackResult");

  const auto fullyReduced = [](std::string_view function) {
    return !function.empty() && !contains(function, "$match") &&
           !contains(function, "is-instance-of") &&
           !contains(function, "constValue") &&
           !contains(function, "erasedValue") && !contains(function, "TypeKinds.");
  };
  const bool constantsReduced =
      fullyReduced(directConstant) && contains(directConstant, "11") &&
      fullyReduced(intConstant) && contains(intConstant, "42") &&
      fullyReduced(longConstant) && contains(longConstant, "9000000000L") &&
      fullyReduced(stringConstant) && contains(stringConstant, "\"literal\"") &&
      fullyReduced(booleanConstant) && contains(booleanConstant, "true") &&
      fullyReduced(charConstant) && contains(charConstant, "'x'") &&
      fullyReduced(constantMatchSeven) &&
      contains(constantMatchSeven, "\"constant-seven\"") &&
      !contains(constantMatchSeven, "constant-other") &&
      fullyReduced(constantMatchOther) &&
      contains(constantMatchOther, "\"constant-other\"") &&
      fullyReduced(constantBooleanFalse) &&
      contains(constantBooleanFalse, "\"constant-false\"") &&
      fullyReduced(constantPrecise) &&
      contains(constantPrecise, "demo.inlineerased.PreciseResult") &&
      contains(constantPrecise, ".preciseOnly") &&
      !contains(constantPrecise, "FallbackResult") &&
      fullyReduced(constantFallback) &&
      contains(constantFallback, "demo.inlineerased.FallbackResult") &&
      contains(constantFallback, ".fallbackOnly") &&
      !contains(constantFallback, "PreciseResult");
  const bool valid =
      status == 0 &&
      outputText == "11\n42\n9000000000\nliteral\ntrue\nx\nconstant-seven\n"
                    "constant-other\nconstant-false\nconstant-precise\n"
                    "constant-fallback\nstring\nint\nlong\nother\nalias-string\n"
                    "alias-other\n"
                    "precise\nfallback\n" &&
      !invalid.ok &&
      countOccurrences(invalid.diagnosticsText,
                       "constValue requires a constant singleton type") == 3 &&
      contains(invalid.diagnosticsText,
               "constValue requires exactly one type argument") &&
      constantsReduced &&
      countOccurrences(
          invalid.diagnosticsText,
          "erasedValue may only be used as the selector of an inline match") == 3 &&
      countOccurrences(
          invalid.diagnosticsText,
          "erasedValue inline match patterns cannot bind a runtime value") == 2 &&
      contains(invalid.diagnosticsText,
               "erasedValue requires exactly one type argument") &&
      contains(invalid.diagnosticsText,
               "inline match selector must be reducible from a compile-time value "
               "or static type") &&
      fullyReduced(stringName) && contains(stringName, "\"string\"") &&
      !contains(stringName, "\"other\"") && fullyReduced(intName) &&
      contains(intName, "\"int\"") && fullyReduced(longName) &&
      contains(longName, "\"long\"") && fullyReduced(otherName) &&
      contains(otherName, "\"other\"") && fullyReduced(aliasStringName) &&
      contains(aliasStringName, "\"alias-string\"") && fullyReduced(aliasOtherName) &&
      contains(aliasOtherName, "\"alias-other\"") && fullyReduced(preciseResult) &&
      contains(preciseResult, "demo.inlineerased.PreciseResult") &&
      contains(preciseResult, ".preciseOnly") &&
      !contains(preciseResult, "FallbackResult") && fullyReduced(fallbackResult) &&
      contains(fallbackResult, "demo.inlineerased.FallbackResult") &&
      contains(fallbackResult, ".fallbackOnly") &&
      !contains(fallbackResult, "PreciseResult");

  return valid ? 0
               : fail("inline erasedValue smoke test failed (output='" + outputText +
                      "', diagnostics='" + result.diagnosticsText +
                      "', invalid-diagnostics='" + invalid.diagnosticsText +
                      "', direct-constant='" + std::string(directConstant) +
                      "', int-constant='" + std::string(intConstant) +
                      "', long-constant='" + std::string(longConstant) +
                      "', string-constant='" + std::string(stringConstant) +
                      "', boolean-constant='" + std::string(booleanConstant) +
                      "', char-constant='" + std::string(charConstant) +
                      "', constant-match-seven='" +
                      std::string(constantMatchSeven) +
                      "', constant-match-other='" +
                      std::string(constantMatchOther) +
                      "', constant-boolean-false='" +
                      std::string(constantBooleanFalse) +
                      "', constant-precise='" + std::string(constantPrecise) +
                      "', constant-fallback='" + std::string(constantFallback) +
                      "', string-name='" + std::string(stringName) + "', int-name='" +
                      std::string(intName) + "', long-name='" + std::string(longName) +
                      "', other-name='" + std::string(otherName) +
                      "', alias-string-name='" + std::string(aliasStringName) +
                      "', alias-other-name='" + std::string(aliasOtherName) +
                      "', precise-result='" + std::string(preciseResult) +
                      "', fallback-result='" + std::string(fallbackResult) + "')");
}

} // namespace

int runSmokeTests6() {
  return smokeInlineErasedValue();
}
