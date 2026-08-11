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

int smokeTupleInitLast() {
  constexpr const char* source = R"(package demo.tupleinitlast

class Box(val value: String)

class Counter {
  var calls: Int = 0

  def nextLabel: String = {
    calls = calls + 1
    "once"
  }

  def make: (String, Int, Box) = (nextLabel, calls, new Box("tail"))
}

object Values {
  def tuple: (Int, String, Boolean, Box) =
    (7, "middle", true, new Box("last"))

  def initial: (Int, String, Boolean) = tuple.init
  def initialFirst: Int = initial.head
  def initialLast: Boolean = initial.last
  def last: Box = tuple.last
  def lastText: String = last.value

  def twice: (Int, String) = tuple.init.init
  def twiceLast: String = twice.last
  def singleton: Int *: EmptyTuple = tuple.init.init.init
  def singletonLast: Int = singleton.last
  def empty: EmptyTuple = tuple.init.init.init.init
  def emptySize: Int = empty.size

  def consLast: String = (99 *: "end" *: EmptyTuple).last
  def directLast: String = (1, "direct").last

  def initOnce: String = {
    val counter = new Counter
    val value = counter.make.init.last
    value + ":" + counter.calls
  }

  def lastOnce: String = {
    val counter = new Counter
    val box = counter.make.last
    val value = box.value
    value + ":" + counter.calls
  }
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Values.initialFirst)
    println(Values.initialLast)
    println(Values.lastText)
    println(Values.twiceLast)
    println(Values.singletonLast)
    println(Values.emptySize)
    println(Values.consLast)
    println(Values.directLast)
    println(Values.initOnce)
    println(Values.lastOnce)
  }
}
)";

  constexpr const char* invalidSource = R"(package demo.invalidtupleinitlast

object Values {
  val emptyInit = EmptyTuple.init
  val emptyLast = EmptyTuple.last
  def openInit(value: Tuple): Tuple = value.init
  def openLast(value: Tuple): Object = value.last
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "cpp-scalanative-smoke-tuple-init-last";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-tuple-init-last.out";
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
      driver.buildSource("TupleInitLast.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidTupleInitLast.scala", invalidSource, {}, invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("tuple-init-last native build failed: " + result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string outputText = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  const std::string_view initial =
      functionText(result.nirText, "demo.tupleinitlast.Values.initial");
  const std::string_view initialLast =
      functionText(result.nirText, "demo.tupleinitlast.Values.initialLast");
  const std::string_view last =
      functionText(result.nirText, "demo.tupleinitlast.Values.last");
  const std::string_view twice =
      functionText(result.nirText, "demo.tupleinitlast.Values.twice");
  const std::string_view singleton =
      functionText(result.nirText, "demo.tupleinitlast.Values.singleton");
  const std::string_view empty =
      functionText(result.nirText, "demo.tupleinitlast.Values.empty");
  const std::string_view cons =
      functionText(result.nirText, "demo.tupleinitlast.Values.consLast");
  const std::string_view direct =
      functionText(result.nirText, "demo.tupleinitlast.Values.directLast");
  const std::string_view initOnce =
      functionText(result.nirText, "demo.tupleinitlast.Values.initOnce");
  const std::string_view lastOnce =
      functionText(result.nirText, "demo.tupleinitlast.Values.lastOnce");

  const bool runtimeShape =
      contains(result.nirText, "module @scala.EmptyTuple : @scala.Tuple") &&
      contains(result.nirText, "class @scala.Tuple1 : @scala.Tuple") &&
      contains(result.nirText, "class @scala.Tuple2 : @scala.Tuple") &&
      contains(result.nirText, "class @scala.Tuple3 : @scala.Tuple") &&
      contains(result.nirText, "class @scala.Tuple4 : @scala.Tuple") &&
      !contains(result.nirText, "class @scala.Tuple5 : @scala.Tuple");
  const bool lowered =
      contains(initial, "new scala.Tuple3") && contains(initial, "._1") &&
      contains(initial, "._2") && contains(initial, "._3") &&
      !contains(initial, "._4") && contains(initialLast, "unbox[Boolean]") &&
      contains(initialLast, "._3") &&
      contains(last, "as-instance-of[demo.tupleinitlast.Box]") &&
      contains(last, "._4") && contains(twice, "new scala.Tuple3") &&
      contains(twice, "new scala.Tuple2") && contains(singleton, "new scala.Tuple3") &&
      contains(singleton, "new scala.Tuple2") &&
      contains(singleton, "new scala.Tuple1") && contains(empty, "%scala.EmptyTuple") &&
      contains(cons, "unbox[String]") && contains(cons, "._2") &&
      contains(direct, "unbox[String]") && contains(direct, "._2") &&
      countOccurrences(initOnce, "%counter.make") == 1 &&
      contains(initOnce, "new scala.Tuple2") && contains(initOnce, "unbox[Int]") &&
      contains(initOnce, "._2") && countOccurrences(lastOnce, "%counter.make") == 1 &&
      contains(lastOnce, "as-instance-of[demo.tupleinitlast.Box]") &&
      contains(lastOnce, "._3") &&
      !contains(result.nirText, "<malformed-tuple-init>") &&
      !contains(result.nirText, "<malformed-tuple-last>");
  const bool invalidDiagnosticsMatch =
      !invalid.ok &&
      contains(invalid.diagnosticsText, "init is not available on EmptyTuple") &&
      contains(invalid.diagnosticsText, "last is not available on EmptyTuple") &&
      contains(invalid.diagnosticsText, "init requires a concrete tuple receiver") &&
      contains(invalid.diagnosticsText, "last requires a concrete tuple receiver");

  if (status == 0 &&
      outputText == "7\ntrue\nlast\nmiddle\n7\n0\nend\ndirect\n1:1\ntail:1\n" &&
      runtimeShape && lowered && invalidDiagnosticsMatch) {
    return 0;
  }

  return fail("tuple-init-last smoke test failed (status=" + std::to_string(status) +
              ", output='" + outputText + "', diagnostics='" + result.diagnosticsText +
              "', invalid='" + invalid.diagnosticsText + "', initial='" +
              std::string(initial) + "', initial-last='" + std::string(initialLast) +
              "', last='" + std::string(last) + "', twice='" + std::string(twice) +
              "', singleton='" + std::string(singleton) + "', empty='" +
              std::string(empty) + "', cons='" + std::string(cons) + "', direct='" +
              std::string(direct) + "', init-once='" + std::string(initOnce) +
              "', last-once='" + std::string(lastOnce) + "')");
}

} // namespace

int runSmokeTests12() {
  return smokeTupleInitLast();
}
