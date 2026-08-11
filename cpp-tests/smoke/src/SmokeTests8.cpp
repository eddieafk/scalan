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

int smokeCompiletimeSummonAll() {
  constexpr const char* source = R"(package demo.summonall

import scala.compiletime.summonAll
import scala.compiletime.{summonAll => collectAll}

trait Named[A] {
  def label(): String
}

class NamedValue[A](val value: String) extends Named[A] {
  def label(): String = value
}

class Token(val value: String)

object Token {
  given default: Token = new Token("companion-token")
}

object Evidence {
  given intNamed: Named[Int] = new NamedValue[Int]("int")
  given stringNamed: Named[String] = new NamedValue[String]("string")
  given longNamed: Named[Long] = new NamedValue[Long]("long")
  given number: Int = 73
}

import Evidence.given

object Values {
  type PairEvidence = (Named[Int], Named[String])

  inline def collect[T <: Tuple] = summonAll[T]

  def single: String = summonAll[Tuple1[Named[Long]]]._1.label()
  def directFirst: String = summonAll[PairEvidence]._1.label()
  def directSecond: String = summonAll[PairEvidence]._2.label()
  def aliased: String = collectAll[(Named[String], Named[Int])]._1.label()
  def qualified: String =
    scala.compiletime.summonAll[(Named[Int], Named[Long])]._2.label()
  def specialized: String = collect[(Named[Int], Named[String])]._2.label()
  def primitive: Int = summonAll[Tuple1[Int]]._1
  def triplePrimitive: Int = summonAll[(Int, Named[Int], Token)]._1
  def tripleToken: Token = summonAll[(Int, Named[Int], Token)]._3
  def companion: String = tripleToken.value
  def empty: Boolean = summonAll[EmptyTuple] == summonAll[EmptyTuple]
}

object Shadowing {
  def summonAll[A](): String = "ordinary-summon-all-shadow"
  def value: String = summonAll[Int]()
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Values.single)
    println(Values.directFirst)
    println(Values.directSecond)
    println(Values.aliased)
    println(Values.qualified)
    println(Values.specialized)
    println(Values.primitive)
    println(Values.triplePrimitive)
    println(Values.companion)
    println(Values.empty)
    println(Shadowing.value)
  }
}
)";

  constexpr const char* invalidSource = R"(package demo.invalidsummonall

import scala.compiletime.summonAll

trait Named[A]
class NamedValue[A] extends Named[A]
trait Missing

object Main {
  given firstLong: Named[Long] = new NamedValue[Long]
  given secondLong: Named[Long] = new NamedValue[Long]

  val notTuple = summonAll[String]
  val missing = summonAll[(Missing, Named[Int])]
  val ambiguous = summonAll[Tuple1[Named[Long]]]
  val malformed = summonAll[(Named[Int], Named[String]), Tuple1[Named[Long]]]
  def unresolved[T <: Tuple] = summonAll[T]
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "cpp-scalanative-smoke-compiletime-summon-all";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-compiletime-summon-all.out";
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
      driver.buildSource("CompiletimeSummonAll.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidCompiletimeSummonAll.scala", invalidSource, {}, invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("summonAll native build failed: " + result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string outputText = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  const std::string_view single =
      functionText(result.nirText, "demo.summonall.Values.single");
  const std::string_view directFirst =
      functionText(result.nirText, "demo.summonall.Values.directFirst");
  const std::string_view directSecond =
      functionText(result.nirText, "demo.summonall.Values.directSecond");
  const std::string_view aliased =
      functionText(result.nirText, "demo.summonall.Values.aliased");
  const std::string_view qualified =
      functionText(result.nirText, "demo.summonall.Values.qualified");
  const std::string_view specialized =
      functionText(result.nirText, "demo.summonall.Values.specialized");
  const std::string_view primitive =
      functionText(result.nirText, "demo.summonall.Values.primitive");
  const std::string_view triplePrimitive =
      functionText(result.nirText, "demo.summonall.Values.triplePrimitive");
  const std::string_view tripleToken =
      functionText(result.nirText, "demo.summonall.Values.tripleToken");
  const std::string_view empty =
      functionText(result.nirText, "demo.summonall.Values.empty");
  const std::string_view shadowed =
      functionText(result.nirText, "demo.summonall.Shadowing.value");

  const auto reducedTuple = [](std::string_view function, std::string_view tupleName) {
    return !function.empty() && contains(function, tupleName) &&
           !contains(function, "scala.compiletime.summonAll") &&
           !contains(function, "type-apply");
  };
  const bool runtimeShape =
      contains(result.nirText, "trait @scala.Tuple : @java.lang.Object") &&
      contains(result.nirText, "module @scala.EmptyTuple : @scala.Tuple") &&
      contains(result.nirText, "class @scala.Tuple1 : @scala.Tuple") &&
      contains(result.nirText, "field @scala.Tuple1._1 : Object") &&
      contains(result.nirText, "class @scala.Tuple2 : @scala.Tuple") &&
      contains(result.nirText, "field @scala.Tuple2._2 : Object") &&
      contains(result.nirText, "class @scala.Tuple3 : @scala.Tuple") &&
      contains(result.nirText, "field @scala.Tuple3._3 : Object") &&
      !contains(result.nirText, "scala.Tuple4");
  const bool reduced =
      reducedTuple(single, "new scala.Tuple1") &&
      contains(single, "call %demo.summonall.Evidence.longNamed()") &&
      reducedTuple(directFirst, "new scala.Tuple2") &&
      contains(directFirst, "call %demo.summonall.Evidence.intNamed()") &&
      contains(directFirst, "call %demo.summonall.Evidence.stringNamed()") &&
      reducedTuple(directSecond, "new scala.Tuple2") &&
      reducedTuple(aliased, "new scala.Tuple2") &&
      contains(aliased, "call %demo.summonall.Evidence.stringNamed()") &&
      reducedTuple(qualified, "new scala.Tuple2") &&
      contains(qualified, "call %demo.summonall.Evidence.longNamed()") &&
      reducedTuple(specialized, "new scala.Tuple2") &&
      contains(specialized, "call %demo.summonall.Evidence.stringNamed()") &&
      reducedTuple(primitive, "new scala.Tuple1") &&
      contains(primitive, "box[Int](call %demo.summonall.Evidence.number())") &&
      contains(primitive, "unbox[Int]") &&
      reducedTuple(triplePrimitive, "new scala.Tuple3") &&
      contains(triplePrimitive, "call %demo.summonall.Token$.default()") &&
      contains(triplePrimitive, "box[Int]") &&
      reducedTuple(tripleToken, "new scala.Tuple3") &&
      contains(tripleToken, "as-instance-of[demo.summonall.Token]") && !empty.empty() &&
      contains(empty, "%scala.EmptyTuple") &&
      !contains(empty, "scala.compiletime.summonAll") && !shadowed.empty() &&
      contains(shadowed, "demo.summonall.Shadowing.summonAll()");

  const bool invalidDiagnosticsMatch =
      !invalid.ok &&
      contains(invalid.diagnosticsText, "summonAll requires a tuple type") &&
      contains(invalid.diagnosticsText,
               "no given value found for context parameter element1 of type ") &&
      contains(invalid.diagnosticsText, "Missing required by summonAll") &&
      contains(invalid.diagnosticsText,
               "no given value found for context parameter element2 of type ") &&
      contains(invalid.diagnosticsText, "Named [ Int ] required by summonAll") &&
      contains(invalid.diagnosticsText,
               "ambiguous given values for context parameter element1 of type ") &&
      contains(invalid.diagnosticsText, "firstLong, secondLong") &&
      contains(invalid.diagnosticsText,
               "summonAll requires exactly one type argument") &&
      contains(invalid.diagnosticsText, "summonAll requires a concrete tuple type");

  if (status == 0 &&
      outputText == "long\nint\nstring\nstring\nlong\nstring\n73\n73\n"
                    "companion-token\ntrue\nordinary-summon-all-shadow\n" &&
      runtimeShape && reduced && invalidDiagnosticsMatch) {
    return 0;
  }

  return fail("summonAll smoke test failed (status=" + std::to_string(status) +
              ", output='" + outputText + "', diagnostics='" + result.diagnosticsText +
              "', invalid='" + invalid.diagnosticsText + "', single='" +
              std::string(single) + "', direct-first='" + std::string(directFirst) +
              "', direct-second='" + std::string(directSecond) + "', aliased='" +
              std::string(aliased) + "', qualified='" + std::string(qualified) +
              "', specialized='" + std::string(specialized) + "', primitive='" +
              std::string(primitive) + "', triple-primitive='" +
              std::string(triplePrimitive) + "', triple-token='" +
              std::string(tripleToken) + "', empty='" + std::string(empty) +
              "', shadowed='" + std::string(shadowed) + "')");
}

} // namespace

int runSmokeTests8() {
  return smokeCompiletimeSummonAll();
}
