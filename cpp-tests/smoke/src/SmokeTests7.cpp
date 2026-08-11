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

int smokeCompiletimeConstValueOpt() {
  constexpr const char* source = R"(package demo.constvalueopt

import scala.compiletime.constValueOpt
import scala.compiletime.{constValueOpt => optionalConstant}

object Values {
  transparent inline def optional[T] = constValueOpt[T]

  def direct: Int =
    constValueOpt[42].asInstanceOf[Some[42]].value

  def aliased: String =
    optionalConstant["present"].asInstanceOf[Some["present"]].value

  def qualified: Boolean =
    scala.compiletime.constValueOpt[true]
      .asInstanceOf[Some[true]]
      .value

  def specialized: String =
    optional["specialized"].asInstanceOf[Some["specialized"]].value

  def missing: Boolean = constValueOpt[String] == None
}

object Shadowing {
  def constValueOpt[A](): String = "ordinary-shadow"
  def value: String = constValueOpt[Int]()
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Values.direct)
    println(Values.aliased)
    println(Values.qualified)
    println(Values.specialized)
    println(Values.missing)
    println(Shadowing.value)
  }
}
)";

  constexpr const char* invalidSource = R"(package demo.invalidconstvalueopt

import scala.compiletime.constValueOpt

object Main {
  val malformed = constValueOpt[1, 2]
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "cpp-scalanative-smoke-compiletime-const-value-opt";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-compiletime-const-value-opt.out";
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
      "CompiletimeConstValueOpt.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidCompiletimeConstValueOpt.scala", invalidSource, {}, invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("constValueOpt native build failed: " + result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string outputText = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  const std::string_view direct =
      functionText(result.nirText, "demo.constvalueopt.Values.direct");
  const std::string_view aliased =
      functionText(result.nirText, "demo.constvalueopt.Values.aliased");
  const std::string_view qualified =
      functionText(result.nirText, "demo.constvalueopt.Values.qualified");
  const std::string_view specialized =
      functionText(result.nirText, "demo.constvalueopt.Values.specialized");
  const std::string_view missing =
      functionText(result.nirText, "demo.constvalueopt.Values.missing");
  const std::string_view shadowed =
      functionText(result.nirText, "demo.constvalueopt.Shadowing.value");

  const auto reducedSome = [](std::string_view function) {
    return !function.empty() && contains(function, "new scala.Some") &&
           !contains(function, "scala.compiletime.constValueOpt") &&
           !contains(function, "type-apply");
  };
  const bool runtimeShape =
      contains(result.nirText, "trait @scala.Option : @java.lang.Object") &&
      contains(result.nirText, "class @scala.Some : @scala.Option") &&
      contains(result.nirText, "field @scala.Some.value : Object") &&
      contains(result.nirText, "module @scala.None : @scala.Option");
  const bool reduced =
      reducedSome(direct) && contains(direct, "box[Int](42)") &&
      contains(direct, "unbox[Int]") && reducedSome(aliased) &&
      contains(aliased, "box[String](\"present\")") &&
      contains(aliased, "unbox[String]") && reducedSome(qualified) &&
      contains(qualified, "box[Boolean](true)") &&
      contains(qualified, "unbox[Boolean]") && reducedSome(specialized) &&
      contains(specialized, "box[String](\"specialized\")") && !missing.empty() &&
      contains(missing, "%scala.None") &&
      !contains(missing, "scala.compiletime.constValueOpt") && !shadowed.empty() &&
      contains(shadowed, "demo.constvalueopt.Shadowing.constValueOpt()");

  if (status == 0 &&
      outputText == "42\npresent\ntrue\nspecialized\ntrue\nordinary-shadow\n" &&
      runtimeShape && reduced && !invalid.ok &&
      contains(invalid.diagnosticsText,
               "constValueOpt requires exactly one type argument")) {
    return 0;
  }

  return fail("constValueOpt smoke test failed (status=" + std::to_string(status) +
              ", output='" + outputText + "', diagnostics='" + result.diagnosticsText +
              "', invalid='" + invalid.diagnosticsText + "', direct='" +
              std::string(direct) + "', aliased='" + std::string(aliased) +
              "', qualified='" + std::string(qualified) + "', specialized='" +
              std::string(specialized) + "', missing='" + std::string(missing) +
              "', shadowed='" + std::string(shadowed) + "')");
}

int smokeCompiletimeConstValueTuple() {
  constexpr const char* source = R"(package demo.constvaluetuple

import scala.compiletime.constValueTuple
import scala.compiletime.{constValueTuple => materializeConstants}

object Values {
  type NamedConstants = (99, "aliased type")

  transparent inline def materialize[T <: Tuple] = constValueTuple[T]

  def single: Int = constValueTuple[Tuple1[5]]._1
  def direct: Int = constValueTuple[(42, "present")]._1
  def text: String = constValueTuple[(42, "present")]._2
  def aliased: Boolean = materializeConstants[(true, 'x')]._1
  def aliasedType: String = constValueTuple[NamedConstants]._2
  def qualified: Char =
    scala.compiletime.constValueTuple[(true, 'x')]._2
  def specialized: String = materialize[(7, "specialized")]._2
  def triple: Long = constValueTuple[(1, 9000000000L, 2.5)]._2
  def empty: Boolean =
    constValueTuple[EmptyTuple] == constValueTuple[EmptyTuple]
}

object Shadowing {
  def constValueTuple[A](): String = "ordinary-tuple-shadow"
  def value: String = constValueTuple[Int]()
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Values.single)
    println(Values.direct)
    println(Values.text)
    println(Values.aliased)
    println(Values.aliasedType)
    println(Values.qualified)
    println(Values.specialized)
    println(Values.triple)
    println(Values.empty)
    println(Shadowing.value)
  }
}
)";

  constexpr const char* invalidSource = R"(package demo.invalidconstvaluetuple

import scala.compiletime.constValueTuple

object Main {
  val notTuple = constValueTuple[1]
  val nonConstant = constValueTuple[(1, String)]
  val nested = constValueTuple[((1, 2), (3, 4))]
  val malformed = constValueTuple[(1, 2), (3, 4)]
  def unresolved[T <: Tuple] = constValueTuple[T]
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "cpp-scalanative-smoke-compiletime-const-value-tuple";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-compiletime-const-value-tuple.out";
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
      "CompiletimeConstValueTuple.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidCompiletimeConstValueTuple.scala", invalidSource, {},
      invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("constValueTuple native build failed: " + result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string outputText = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  const std::string_view direct =
      functionText(result.nirText, "demo.constvaluetuple.Values.direct");
  const std::string_view single =
      functionText(result.nirText, "demo.constvaluetuple.Values.single");
  const std::string_view text =
      functionText(result.nirText, "demo.constvaluetuple.Values.text");
  const std::string_view aliased =
      functionText(result.nirText, "demo.constvaluetuple.Values.aliased");
  const std::string_view aliasedType =
      functionText(result.nirText, "demo.constvaluetuple.Values.aliasedType");
  const std::string_view qualified =
      functionText(result.nirText, "demo.constvaluetuple.Values.qualified");
  const std::string_view specialized =
      functionText(result.nirText, "demo.constvaluetuple.Values.specialized");
  const std::string_view triple =
      functionText(result.nirText, "demo.constvaluetuple.Values.triple");
  const std::string_view empty =
      functionText(result.nirText, "demo.constvaluetuple.Values.empty");
  const std::string_view shadowed =
      functionText(result.nirText, "demo.constvaluetuple.Shadowing.value");

  const auto reducedTuple = [](std::string_view function,
                               std::string_view tupleName) {
    return !function.empty() && contains(function, tupleName) &&
           !contains(function, "scala.compiletime.constValueTuple") &&
           !contains(function, "type-apply");
  };
  const bool runtimeShape =
      contains(result.nirText, "trait @scala.Tuple : @java.lang.Object") &&
      contains(result.nirText, "module @scala.EmptyTuple : @scala.Tuple") &&
      contains(result.nirText, "class @scala.Tuple1 : @scala.Tuple") &&
      contains(result.nirText, "field @scala.Tuple1._1 : Object") &&
      contains(result.nirText, "class @scala.Tuple2 : @scala.Tuple") &&
      contains(result.nirText, "field @scala.Tuple2._1 : Object") &&
      contains(result.nirText, "field @scala.Tuple2._2 : Object") &&
      contains(result.nirText, "class @scala.Tuple3 : @scala.Tuple") &&
      contains(result.nirText, "field @scala.Tuple3._3 : Object") &&
      !contains(result.nirText, "scala.Tuple4");
  const bool reduced =
      reducedTuple(single, "new scala.Tuple1") &&
      contains(single, "box[Int](5)") && contains(single, "unbox[Int]") &&
      reducedTuple(direct, "new scala.Tuple2") &&
      contains(direct, "box[Int](42)") && contains(direct, "unbox[Int]") &&
      reducedTuple(text, "new scala.Tuple2") &&
      contains(text, "box[String](\"present\")") &&
      contains(text, "unbox[String]") &&
      reducedTuple(aliased, "new scala.Tuple2") &&
      contains(aliased, "box[Boolean](true)") &&
      contains(aliased, "unbox[Boolean]") &&
      reducedTuple(aliasedType, "new scala.Tuple2") &&
      contains(aliasedType, "box[String](\"aliased type\")") &&
      reducedTuple(qualified, "new scala.Tuple2") &&
      contains(qualified, "box[Char]('x')") &&
      contains(qualified, "unbox[Char]") &&
      reducedTuple(specialized, "new scala.Tuple2") &&
      contains(specialized, "box[String](\"specialized\")") &&
      reducedTuple(triple, "new scala.Tuple3") &&
      contains(triple, "box[Long](9000000000L)") &&
      contains(triple, "unbox[Long]") && !empty.empty() &&
      contains(empty, "%scala.EmptyTuple") &&
      !contains(empty, "scala.compiletime.constValueTuple") &&
      !shadowed.empty() &&
      contains(shadowed,
               "demo.constvaluetuple.Shadowing.constValueTuple()");

  if (status == 0 &&
      outputText ==
          "5\n42\npresent\ntrue\naliased type\nx\nspecialized\n9000000000\ntrue\n"
          "ordinary-tuple-shadow\n" &&
      runtimeShape && reduced && !invalid.ok &&
      contains(invalid.diagnosticsText,
               "constValueTuple requires a tuple type") &&
      contains(invalid.diagnosticsText,
               "constValueTuple requires constant singleton element types") &&
      contains(invalid.diagnosticsText,
               "constValueTuple requires exactly one type argument") &&
      contains(invalid.diagnosticsText,
               "constValueTuple requires a concrete tuple type")) {
    return 0;
  }

  return fail("constValueTuple smoke test failed (status=" +
              std::to_string(status) + ", output='" + outputText +
              "', diagnostics='" + result.diagnosticsText +
              "', invalid='" + invalid.diagnosticsText + "', direct='" +
              std::string(direct) + "', single='" + std::string(single) +
              "', text='" + std::string(text) +
              "', aliased='" + std::string(aliased) +
              "', aliased-type='" + std::string(aliasedType) +
              "', qualified='" + std::string(qualified) +
              "', specialized='" + std::string(specialized) +
              "', triple='" + std::string(triple) + "', empty='" +
              std::string(empty) + "', shadowed='" + std::string(shadowed) +
              "')");
}

} // namespace

int runSmokeTests7() {
  if (int code = smokeCompiletimeConstValueOpt()) {
    return code;
  }
  return smokeCompiletimeConstValueTuple();
}
