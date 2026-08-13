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

int smokeTupleOperations() {
  constexpr const char* source = R"(package demo.tupleoperations

class Box(val value: String)

class Counter {
  var calls: Int = 0

  def nextLabel: String = {
    calls = calls + 1
    "once"
  }

  def make: (String, Int, Boolean) = (nextLabel, calls, true)
}

object Values {
  def triple: (Int, String, Box) = (7, "middle", new Box("last"))
  def first: Int = triple.head

  def rest: String *: Box *: EmptyTuple = triple.tail
  def restHead: String = rest.head
  def restLast: Box = rest.tail.head
  def restLastText: String = restLast.value
  def restEnd: EmptyTuple = rest.tail.tail

  def tripleSize: Int = triple.size
  def exactSize: 3 = triple.size
  def restSize: Int = rest.size
  def emptySize: Int = EmptyTuple.size

  def singleton: Int *: EmptyTuple = 99 *: EmptyTuple
  def singletonHead: Int = singleton.head
  def singletonTail: EmptyTuple = singleton.tail

  def directHead: String = ("direct", 1).head
  def nestedTailHead: Boolean = (1, "drop", true).tail.tail.head

  def headOnce: String = {
    val counter = new Counter
    counter.make.head + ":" + counter.calls
  }

  def tailOnce: String = {
    val counter = new Counter
    val tail = counter.make.tail
    tail.head + ":" + counter.calls
  }

  def sizeOnce: String = {
    val counter = new Counter
    counter.make.size + ":" + counter.calls
  }
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Values.first)
    println(Values.restHead)
    println(Values.restLastText)
    println(Values.tripleSize)
    println(Values.restSize)
    println(Values.emptySize)
    println(Values.singletonHead)
    println(Values.directHead)
    println(Values.nestedTailHead)
    println(Values.headOnce)
    println(Values.tailOnce)
    println(Values.sizeOnce)
  }
}
)";

  constexpr const char* invalidSource = R"(package demo.invalidtupleoperations

object Values {
  val emptyHead = EmptyTuple.head
  val emptyTail = EmptyTuple.tail
  def openSize(value: Tuple): Int = value.size
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "scalanative-smoke-tuple-operations";
  const std::filesystem::path output =
      temporary / "scalanative-smoke-tuple-operations.out";
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
      driver.buildSource("TupleOperations.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidTupleOperations.scala", invalidSource, {}, invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("tuple-operations native build failed: " + result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string outputText = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  const std::string_view first =
      functionText(result.nirText, "demo.tupleoperations.Values.first");
  const std::string_view rest =
      functionText(result.nirText, "demo.tupleoperations.Values.rest");
  const std::string_view restLast =
      functionText(result.nirText, "demo.tupleoperations.Values.restLast");
  const std::string_view restEnd =
      functionText(result.nirText, "demo.tupleoperations.Values.restEnd");
  const std::string_view tripleSize =
      functionText(result.nirText, "demo.tupleoperations.Values.tripleSize");
  const std::string_view exactSize =
      functionText(result.nirText, "demo.tupleoperations.Values.exactSize");
  const std::string_view emptySize =
      functionText(result.nirText, "demo.tupleoperations.Values.emptySize");
  const std::string_view singletonTail =
      functionText(result.nirText, "demo.tupleoperations.Values.singletonTail");
  const std::string_view directHead =
      functionText(result.nirText, "demo.tupleoperations.Values.directHead");
  const std::string_view nestedTailHead =
      functionText(result.nirText, "demo.tupleoperations.Values.nestedTailHead");
  const std::string_view headOnce =
      functionText(result.nirText, "demo.tupleoperations.Values.headOnce");
  const std::string_view tailOnce =
      functionText(result.nirText, "demo.tupleoperations.Values.tailOnce");
  const std::string_view sizeOnce =
      functionText(result.nirText, "demo.tupleoperations.Values.sizeOnce");

  const bool runtimeShape =
      contains(result.nirText, "module @scala.EmptyTuple : @scala.Tuple") &&
      contains(result.nirText, "class @scala.Tuple1 : @scala.Tuple") &&
      contains(result.nirText, "class @scala.Tuple2 : @scala.Tuple") &&
      contains(result.nirText, "class @scala.Tuple3 : @scala.Tuple") &&
      !contains(result.nirText, "class @scala.Tuple4 : @scala.Tuple");
  const bool lowered =
      contains(first, "let %tupleOperation$receiver$") &&
      contains(first, "unbox[Int](%tupleOperation$receiver$") &&
      contains(first, "._1") && contains(rest, "new scala.Tuple2(") &&
      contains(rest, "._2") && contains(rest, "._3") &&
      contains(restLast, "new scala.Tuple1(") &&
      contains(restLast, "as-instance-of[demo.tupleoperations.Box]") &&
      contains(restEnd, "%scala.EmptyTuple") &&
      contains(tripleSize, ": scala.Tuple3 = %triple; 3") &&
      contains(exactSize, ": scala.Tuple3 = %triple; 3") &&
      contains(emptySize, ": scala.EmptyTuple = %scala.EmptyTuple; 0") &&
      contains(singletonTail, "%scala.EmptyTuple") &&
      contains(directHead, "new scala.Tuple2(box[String](\"direct\"), ") &&
      contains(directHead, "unbox[String]") &&
      contains(nestedTailHead, "new scala.Tuple2(") &&
      contains(nestedTailHead, "new scala.Tuple1(") &&
      contains(nestedTailHead, "unbox[Boolean]") &&
      countOccurrences(headOnce, "%counter.make") == 1 &&
      countOccurrences(tailOnce, "%counter.make") == 1 &&
      countOccurrences(sizeOnce, "%counter.make") == 1 &&
      !contains(result.nirText, "<malformed-tuple-tail>");
  const bool invalidDiagnosticsMatch =
      !invalid.ok &&
      contains(invalid.diagnosticsText, "head is not available on EmptyTuple") &&
      contains(invalid.diagnosticsText, "tail is not available on EmptyTuple") &&
      contains(invalid.diagnosticsText, "size requires a concrete tuple receiver");

  if (status == 0 &&
      outputText == "7\nmiddle\nlast\n3\n2\n0\n99\ndirect\ntrue\nonce:1\n"
                    "1:1\n3:1\n" &&
      runtimeShape && lowered && invalidDiagnosticsMatch) {
    return 0;
  }

  return fail("tuple-operations smoke test failed (status=" + std::to_string(status) +
              ", output='" + outputText + "', diagnostics='" + result.diagnosticsText +
              "', invalid='" + invalid.diagnosticsText + "', first='" +
              std::string(first) + "', rest='" + std::string(rest) + "', rest-last='" +
              std::string(restLast) + "', rest-end='" + std::string(restEnd) +
              "', triple-size='" + std::string(tripleSize) + "', exact-size='" +
              std::string(exactSize) + "', empty-size='" + std::string(emptySize) +
              "', direct-head='" + std::string(directHead) + "', nested-tail='" +
              std::string(nestedTailHead) + "', head-once='" + std::string(headOnce) +
              "', tail-once='" + std::string(tailOnce) + "', size-once='" +
              std::string(sizeOnce) + "')");
}

} // namespace

int runSmokeTests10() {
  return smokeTupleOperations();
}
