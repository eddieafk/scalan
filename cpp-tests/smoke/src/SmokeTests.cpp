#include "scalanative/frontend/Lexer.h"
#include "scalanative/frontend/Parser.h"
#include "scalanative/frontend/Typechecker.h"
#include "scalanative/nir/Builder.h"
#include "scalanative/nir/Verifier.h"
#include "scalanative/runtime/RuntimeConfig.h"
#include "scalanative/support/Diagnostics.h"
#include "scalanative/support/SourceManager.h"
#include "scalanative/support/StdNames.h"
#include "scalanative/tools/build/BuildDriver.h"
#include "scalanative/tools/interflow/InterflowOptimizer.h"
#include "scalanative/tools/linker/Linker.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

int fail(const std::string& message) {
  std::cerr << message << '\n';
  return 1;
}

bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::string readTextFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

bool nonEmptyFile(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::exists(path, error) || error) {
    return false;
  }
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  return !error && size > 0;
}

bool containsString(const std::vector<std::string>& values, std::string_view needle) {
  for (const std::string& value : values) {
    if (value == needle) {
      return true;
    }
  }
  return false;
}

bool containsDefinition(const scalanative::nir::Module& module, std::string_view name) {
  for (const scalanative::nir::Definition& definition : module.definitions) {
    if (definition.name == name) {
      return true;
    }
  }
  return false;
}

const scalanative::nir::Definition*
findDefinition(const scalanative::nir::Module& module, std::string_view name) {
  for (const scalanative::nir::Definition& definition : module.definitions) {
    if (definition.name == name) {
      return &definition;
    }
  }
  return nullptr;
}

const scalanative::frontend::TypedDeclaration* findTypedDeclaration(
    const std::vector<scalanative::frontend::TypedDeclaration>& declarations,
    std::string_view symbolName) {
  for (const scalanative::frontend::TypedDeclaration& declaration : declarations) {
    if (declaration.symbolName == symbolName) {
      return &declaration;
    }
    if (const scalanative::frontend::TypedDeclaration* nested =
            findTypedDeclaration(declaration.members, symbolName)) {
      return nested;
    }
  }
  return nullptr;
}

int expect(bool condition, const std::string& message) {
  if (!condition) {
    return fail(message);
  }
  return 0;
}

int smokeBuildPipeline() {
  constexpr const char* source = "package demo\nobject Main { def main = 0 }\n";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::support::SourceManager sources;
  scalanative::support::SourceId id =
      sources.addVirtualFile("Smoke.scala", source, diagnostics);

  scalanative::frontend::Lexer lexer(sources, diagnostics);
  std::vector<scalanative::frontend::Token> tokens = lexer.lex(id);
  if (int code = expect(!tokens.empty(), "lexer produced no tokens")) {
    return code;
  }
  if (int code =
          expect(tokens.back().kind == scalanative::frontend::TokenKind::EndOfFile,
                 "lexer did not produce an EOF token")) {
    return code;
  }

  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildOptions options;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("Smoke.scala", source, options, diagnostics);

  if (!result.ok) {
    std::cerr << result.diagnosticsText;
    return fail("build driver failed smoke source");
  }

  if (int code = expect(contains(result.nirText, "scala.scalanative.runtime.main"),
                        "NIR text does not contain the runtime main root")) {
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "define @scala.scalanative.runtime.main : ()Int") &&
              contains(result.nirText, "ret Int call %demo.Main.main()"),
          "NIR did not emit a runtime main bridge to the discovered main")) {
    return code;
  }

  if (int code = expect(
          contains(result.llvmIr, "define i32 @scala_scalanative_runtime_main()") &&
              contains(result.llvmIr, "call i32 @demo_Main_main()"),
          "LLVM IR did not lower the runtime main bridge")) {
    return code;
  }
  return expect(
      contains(result.llvmIr, "@__scalanative_runtime_state = private global i8 0") &&
          contains(result.llvmIr,
                   "define internal void @__scalanative_runtime_startup()") &&
          contains(result.llvmIr,
                   "define internal void @__scalanative_runtime_shutdown()") &&
          contains(result.llvmIr, "define i32 @main() {\n"
                                  "entry:\n"
                                  "  call void @__scalanative_runtime_startup()\n"
                                  "  %runtime_main = call i32 "
                                  "@scala_scalanative_runtime_main()\n"
                                  "  call void @__scalanative_runtime_shutdown()\n"
                                  "  ret i32 %runtime_main\n"
                                  "}"),
      "LLVM IR main does not run the runtime lifecycle around the main bridge");
}

int smokeMainEntrypointMvp() {
  constexpr const char* source = R"(package demo.entry

object Main {
  def main(args: Array[String]): Unit = {
    if (args == null) {
      println(-1)
    } else {
      println(args.length)
      if (args.length > 0) {
        println(args(0))
      } else {
        println("empty")
      }
    }
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("MainArgs.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "main(args) entrypoint build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText,
                   "define @demo.entry.Main.main : (Array [ String ])Unit") &&
              contains(result.nirText, "param %args : Array [ String ]") &&
              contains(
                  result.nirText,
                  "define @scala.scalanative.runtime.main : (Array [ String ])Int") &&
              contains(result.nirText, "eval call %demo.entry.Main.main(%args)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayLength(%args)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayApply(%args, 0)") &&
              contains(result.nirText, "ret Int 0"),
          "NIR did not emit a runtime bridge for main(args: Array[String])")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "define i32 @main(i32 %argc, ptr %argv)") &&
              contains(result.llvmIr,
                       "define internal ptr @__scalanative_args_from_argv(i32 %argc, "
                       "ptr %argv)") &&
              contains(
                  result.llvmIr,
                  "call ptr @__scalanative_args_from_argv(i32 %argc, ptr %argv)") &&
              contains(result.llvmIr, "store i64 %argument_count, ptr %length_slot") &&
              contains(result.llvmIr, "load ptr, ptr %source_slot") &&
              contains(result.llvmIr, "getelementptr i8, ptr %args, i64 8") &&
              contains(result.llvmIr,
                       "define internal ptr @__scalanative_array_string_at(ptr %array, "
                       "i32 %index)") &&
              contains(result.llvmIr, "call void @llvm.trap()") &&
              contains(result.llvmIr,
                       "call ptr @__scalanative_array_string_at(ptr %args, i32 0)") &&
              contains(result.llvmIr, "call void @demo_entry_Main_main(ptr %args)") &&
              contains(result.llvmIr,
                       "define i32 @main(i32 %argc, ptr %argv) {\n"
                       "entry:\n"
                       "  call void @__scalanative_runtime_startup()\n"
                       "  %args = call ptr @__scalanative_args_from_argv(i32 %argc, "
                       "ptr %argv)\n"
                       "  %runtime_main = call i32 "
                       "@scala_scalanative_runtime_main(ptr %args)\n"
                       "  call void @__scalanative_runtime_shutdown()\n"
                       "  ret i32 %runtime_main"),
          "LLVM IR did not lower the main(args) runtime bridge")) {
    return code;
  }

  constexpr const char* invalidIndexSource = R"(package demo.entry

object InvalidMain {
  def main(args: Array[String]): Unit = {
    println(args("not-an-index"))
  }
}
)";
  scalanative::support::DiagnosticEngine invalidIndexDiagnostics;
  scalanative::tools::build::BuildResult invalidIndex = driver.buildSource(
      "InvalidMainArgs.scala", invalidIndexSource, {}, invalidIndexDiagnostics);
  if (int code = expect(!invalidIndex.ok && contains(invalidIndex.diagnosticsText,
                                                     "array index must have type Int"),
                        "array accepted a non-Int index")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-main-args";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-main-args.out";
  const std::filesystem::path emptyOutputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-main-args-empty.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);
  std::filesystem::remove(emptyOutputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("MainArgs.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "main(args) build-binary failed for a reason other than a "
                  "missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string emptyRunCommand =
      binaryPath.string() + " > " + emptyOutputPath.string();
  if (int code = expect(std::system(emptyRunCommand.c_str()) == 0,
                        "native main(args) smoke binary without arguments did not exit "
                        "successfully")) {
    return code;
  }
  if (int code = expect(
          readTextFile(emptyOutputPath) == "0\nempty\n",
          "native main(args) smoke binary did not receive an empty argv array")) {
    return code;
  }

  const std::string runCommand =
      binaryPath.string() + " first second > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native main(args) smoke binary with arguments did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "2\nfirst\n",
                "native main(args) smoke binary did not preserve argv contents");
}

int smokeUncaughtThrowMvp() {
  constexpr const char* source = R"(package demo.exceptions

class Failure(val code: Int) extends Exception("failure")

object Thrower {
  def fail(): Int = {
    val failure = new Failure(7)
    throw failure
  }
}

object Main {
  def main = {
    println("before")
    Zone.scoped({
      Zone.scoped({
        Thrower.fail()
      })
    })
    println("after")
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("UncaughtThrow.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "uncaught throw build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "define @demo.exceptions.Thrower.fail : ()Int") &&
              contains(result.nirText, "throw %failure"),
          "NIR did not emit a typed throw terminator")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "%scalanative.arena = type { ptr, i64, ptr }") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_zone_destroy_all()") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_throw(ptr %exception) "
                       "noreturn") &&
              contains(result.llvmIr, "call void @__scalanative_zone_destroy_all()") &&
              contains(result.llvmIr,
                       "call ptr @__scalanative_require_non_null_thrown_exception("
                       "ptr %failure") &&
              contains(result.llvmIr, "call void @__scalanative_throw(ptr %"),
          "LLVM did not lower throw through abnormal-exit cleanup")) {
    return code;
  }

  constexpr const char* invalidSource = R"(package demo.exceptions
class Plain
object Invalid {
  def primitive(): Int = throw 42
  def plain(value: Plain): Int = throw value
}
)";
  scalanative::support::DiagnosticEngine invalidDiagnostics;
  scalanative::tools::build::BuildResult invalid =
      driver.buildSource("InvalidThrow.scala", invalidSource, {}, invalidDiagnostics);
  if (int code = expect(
          !invalid.ok && contains(invalid.diagnosticsText,
                                  "throw operand must conform to Throwable or be null"),
          "throw accepted a non-Throwable operand")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-uncaught-throw";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-uncaught-throw.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "UncaughtThrow.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "uncaught throw native build failed: " + binary.diagnosticsText);
  }

  const std::string runCommand =
      binaryPath.string() + " > " + outputPath.string() + " 2>&1";
  if (int code = expect(std::system(runCommand.c_str()) != 0,
                        "uncaught throw binary exited successfully")) {
    return code;
  }
  const std::string output = readTextFile(outputPath);
  return expect(contains(output, "before\n") && !contains(output, "after\n"),
                "uncaught throw did not stop execution at the throw site");
}

int smokeStringArrayLiteralMvp() {
  constexpr const char* source = R"(package demo.arrays

object Main {
  def main = {
    val empty = Array[String]()
    val colors = Array("red", "blue")
    val values = Array(3, 7)
    colors(1) = "green"
    values(0) = 9
    println(empty.length)
    println(colors.length)
    println(colors(0))
    println(colors(1))
    println(values.length)
    println(values(0))
    println(values(1))
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("ArrayLiterals.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "Array[String] literal build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "new Array [ String ]") &&
              contains(result.nirText, "new Array [ String ](\"red\", \"blue\")") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayLength(%colors)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayApply(%colors, 0)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayUpdate(%colors, 1, "
                       "\"green\")") &&
              contains(result.nirText, "new Array [ Int ](3, 7)") &&
              contains(
                  result.nirText,
                  "call %scala.scalanative.runtime.intArrayUpdate(%values, 0, 9)") &&
              contains(
                  result.llvmIr,
                  "call ptr @__scalanative_program_arena_alloc(i64 16, ptr null)") &&
              contains(
                  result.llvmIr,
                  "call ptr @__scalanative_program_arena_alloc(i64 32, ptr null)") &&
              contains(result.llvmIr,
                       "call ptr @__scalanative_array_string_at(ptr %colors, i32 0)") &&
              contains(
                  result.llvmIr,
                  "define internal void @__scalanative_array_string_set(ptr %array, "
                  "i32 %index, ptr %value)") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_array_int_set(ptr %array, "
                       "i32 %index, i32 %value)"),
          "NIR or LLVM did not lower Array[String] literal operations")) {
    return code;
  }

  constexpr const char* invalidSource = R"(package demo.arrays

object InvalidArray {
  def main = {
    val values = Array("ok")
    values(0) = 1
  }
}
)";
  scalanative::support::DiagnosticEngine invalidDiagnostics;
  scalanative::tools::build::BuildResult invalid =
      driver.buildSource("InvalidArray.scala", invalidSource, {}, invalidDiagnostics);
  if (int code = expect(
          !invalid.ok &&
              contains(invalid.diagnosticsText,
                       "array assignment value does not conform to the element type"),
          "Array[String] assignment accepted a non-String value")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-array-literals";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-array-literals.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "ArrayLiterals.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "Array[String] literal binary failed to build: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native Array[String] literal smoke binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "0\n2\nred\ngreen\n2\n9\n7\n",
                "native Array[String] literal produced incorrect output");
}

int smokeArrayParametersMvp() {
  constexpr const char* source = R"(package demo.arrays

object Main {
  def replaceAndTotal(values: Array[Int], index: Int, replacement: Int): Int = {
    values(index) = replacement
    values(0) + values(1)
  }

  def main = {
    val values = Array(4, 6)
    println(replaceAndTotal(values, 1, 9))
    println(values(1))
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("ArrayParameters.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "array-parameter build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "define @demo.arrays.Main.replaceAndTotal : (Array "
                                   "[ Int ],Int,Int)Int") &&
              contains(
                  result.nirText,
                  "call %scala.scalanative.runtime.intArrayUpdate(%values, %index, "
                  "%replacement)") &&
              contains(result.llvmIr,
                       "define i32 @demo_arrays_Main_replaceAndTotal(ptr %values, i32 "
                       "%index, i32 %replacement)"),
          "NIR or LLVM did not preserve typed Array[Int] parameters")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-array-parameters";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-array-parameters.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "ArrayParameters.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "array-parameter binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code =
          expect(std::system(runCommand.c_str()) == 0,
                 "native array-parameter smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "13\n9\n",
                "native array parameter did not retain caller-visible mutation");
}

int smokeBooleanArrayMvp() {
  constexpr const char* source = R"(package demo.arrays

object Main {
  def flipAt(values: Array[Boolean], index: Int): Boolean = {
    values(index) = !values(index)
    values(index)
  }

  def asInt(value: Boolean): Int = if (value) 1 else 0

  def main = {
    val empty = Array[Boolean]()
    val flags = Array(true, false)
    println(empty.length)
    println(asInt(flipAt(flags, 1)))
    println(asInt(flags(0)))
    println(asInt(flags(1)))
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("BooleanArrays.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "Boolean array build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "new Array [ Boolean ]") &&
              contains(result.nirText, "new Array [ Boolean ](true, false)") &&
              contains(
                  result.nirText,
                  "define @demo.arrays.Main.flipAt : (Array [ Boolean ],Int)Boolean") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.booleanArrayUpdate(%values, "
                       "%index") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.booleanArrayApply(%values, "
                       "%index)") &&
              contains(result.llvmIr,
                       "define internal i1 @__scalanative_array_boolean_at(ptr %array, "
                       "i32 %index)") &&
              contains(
                  result.llvmIr,
                  "define internal void @__scalanative_array_boolean_set(ptr %array, "
                  "i32 %index, i1 %value)"),
          "NIR or LLVM did not lower Array[Boolean] operations")) {
    return code;
  }

  constexpr const char* invalidSource = R"(package demo.arrays

object InvalidBooleanArray {
  def main = {
    val flags = Array(true)
    flags(0) = 1
  }
}
)";
  scalanative::support::DiagnosticEngine invalidDiagnostics;
  scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidBooleanArray.scala", invalidSource, {}, invalidDiagnostics);
  if (int code = expect(
          !invalid.ok &&
              contains(invalid.diagnosticsText,
                       "array assignment value does not conform to the element type"),
          "Array[Boolean] assignment accepted a non-Boolean value")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-boolean-arrays";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-boolean-arrays.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "BooleanArrays.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "Boolean array binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code =
          expect(std::system(runCommand.c_str()) == 0,
                 "native Boolean array smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "0\n1\n1\n1\n",
                "native Boolean array did not preserve checked mutation and reads");
}

int smokeLongArrayMvp() {
  constexpr const char* source = R"(package demo.arrays

object Main {
  def replaceAndTotal(values: Array[Long], index: Int, replacement: Long): Long = {
    values(index) = replacement
    values(0) + values(1)
  }

  def main = {
    val empty = Array[Long]()
    val values = Array(4L, 6L)
    println(empty.length)
    println(replaceAndTotal(values, 1, 9L))
    println(values(1))
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("LongArrays.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "Long array build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "new Array [ Long ]") &&
              contains(result.nirText, "new Array [ Long ](4L, 6L)") &&
              contains(result.nirText, "define @demo.arrays.Main.replaceAndTotal : "
                                       "(Array [ Long ],Int,Long)Long") &&
              contains(
                  result.nirText,
                  "call %scala.scalanative.runtime.longArrayUpdate(%values, %index, "
                  "%replacement)") &&
              contains(
                  result.llvmIr,
                  "define internal i64 @__scalanative_array_long_at(ptr %array, i32 "
                  "%index)") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_array_long_set(ptr %array, "
                       "i32 %index, i64 %value)"),
          "NIR or LLVM did not lower Array[Long] operations")) {
    return code;
  }

  constexpr const char* invalidSource = R"(package demo.arrays

object InvalidLongArray {
  def main = {
    val values = Array(1L)
    values(0) = 1
  }
}
)";
  scalanative::support::DiagnosticEngine invalidDiagnostics;
  scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidLongArray.scala", invalidSource, {}, invalidDiagnostics);
  if (int code = expect(
          !invalid.ok &&
              contains(invalid.diagnosticsText,
                       "array assignment value does not conform to the element type"),
          "Array[Long] assignment accepted an Int value")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-long-arrays";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-long-arrays.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("LongArrays.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "Long array binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native Long array smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "0\n13\n9\n",
                "native Long array did not preserve checked mutation and reads");
}

int smokeClassArrayMvp() {
  constexpr const char* source = R"(package demo.arrays

class Entry(val value: Int)

object Main {
  def replaceAndRead(values: Array[Entry], index: Int, replacement: Entry): Int = {
    values(index) = replacement
    values(index).value
  }

  def main = {
    val empty = Array[Entry]()
    val values = Array[Entry](new Entry(3))
    val replacement = new Entry(9)
    println(empty.length)
    println(replaceAndRead(values, 0, replacement))
    println(values(0).value)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("ClassArrays.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "class array build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "new Array [ demo.arrays.Entry ]") &&
              contains(result.nirText,
                       "define @demo.arrays.Main.replaceAndRead : (Array [ "
                       "demo.arrays.Entry ],Int,demo.arrays.Entry)Int") &&
              contains(
                  result.nirText,
                  "declare "
                  "@scala.scalanative.runtime.referenceArrayApply.demo.arrays.Entry : "
                  "(Array [ demo.arrays.Entry ],Int)demo.arrays.Entry") &&
              contains(
                  result.nirText,
                  "call "
                  "%scala.scalanative.runtime.referenceArrayUpdate.demo.arrays.Entry("
                  "%values, %index, %replacement)") &&
              contains(
                  result.llvmIr,
                  "define internal ptr @__scalanative_array_reference_at(ptr %array, "
                  "i32 %index)") &&
              contains(
                  result.llvmIr,
                  "call ptr @__scalanative_array_reference_at(ptr %values, i32 0)"),
          "NIR or LLVM did not lower typed class-array operations")) {
    return code;
  }

  constexpr const char* invalidSource = R"(package demo.arrays

class Entry
class Other

object InvalidClassArray {
  def main = {
    val values = Array(new Entry())
    values(0) = new Other()
  }
}
)";
  scalanative::support::DiagnosticEngine invalidDiagnostics;
  scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidClassArray.scala", invalidSource, {}, invalidDiagnostics);
  if (int code = expect(
          !invalid.ok &&
              contains(invalid.diagnosticsText,
                       "array assignment value does not conform to the element type"),
          "Array[Entry] assignment accepted an unrelated class")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-class-arrays";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-class-arrays.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("ClassArrays.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "class array binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native class array smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "0\n9\n9\n",
                "native class array did not preserve typed mutation and member reads");
}

int smokeTraitArrayMvp() {
  constexpr const char* source = R"(package demo.arrays

trait Named {
  def name: String
  def score: Int
}

class Plain extends Named {
  def name: String = "plain"
  def score: Int = 1
}

class Fancy extends Named {
  def name: String = "fancy"
  def score: Int = 9
}

object Main {
  def replaceAndShow(values: Array[Named], index: Int, replacement: Named): String = {
    values(index) = replacement
    values(index).name + " " + values(index).score
  }

  def main = {
    val values = Array[Named](new Plain())
    println(replaceAndShow(values, 0, new Fancy()))
    println(values(0).name)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("TraitArrays.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "trait array build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(
              result.nirText,
              "define @demo.arrays.Main.replaceAndShow : (Array [ demo.arrays.Named "
              "],Int,demo.arrays.Named)String") &&
              contains(
                  result.nirText,
                  "declare "
                  "@scala.scalanative.runtime.referenceArrayApply.demo.arrays.Named : "
                  "(Array [ demo.arrays.Named ],Int)demo.arrays.Named") &&
              contains(
                  result.nirText,
                  "call "
                  "%scala.scalanative.runtime.referenceArrayUpdate.demo.arrays.Named("
                  "%values, %index, %replacement)") &&
              contains(result.llvmIr,
                       "call ptr @__scalanative_array_reference_at(ptr %values, i32 "
                       "%index)") &&
              contains(result.llvmIr,
                       "@__ancestors_demo_arrays_Fancy = private constant"),
          "NIR or LLVM did not preserve typed trait-array dispatch")) {
    return code;
  }

  constexpr const char* invalidSource = R"(package demo.arrays

trait Named
class Plain extends Named
class Other

object InvalidTraitArray {
  def main = {
    val values = Array[Named](new Plain())
    values(0) = new Other()
  }
}
)";
  scalanative::support::DiagnosticEngine invalidDiagnostics;
  scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidTraitArray.scala", invalidSource, {}, invalidDiagnostics);
  if (int code = expect(
          !invalid.ok &&
              contains(invalid.diagnosticsText,
                       "array assignment value does not conform to the element type"),
          "Array[Named] assignment accepted an unrelated class")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-trait-arrays";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-trait-arrays.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("TraitArrays.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "trait array binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native trait array smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "fancy 9\nfancy\n",
                "native trait array did not retain dynamic dispatch after mutation");
}

int smokeDoubleArrayMvp() {
  constexpr const char* source = R"(package demo.arrays

object Main {
  def replaceAndAverage(values: Array[Double], index: Int, replacement: Double): Double = {
    values(index) = replacement
    val total = values(0) + values(1)
    total / 2.0
  }

  def main = {
    val empty = Array[Double]()
    val values = Array(1.5, 2.5)
    println(empty.length)
    println(replaceAndAverage(values, 1, 4.5))
    println(values(1))
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("DoubleArrays.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "Double array build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "new Array [ Double ]") &&
              contains(result.nirText, "new Array [ Double ](1.5, 2.5)") &&
              contains(result.nirText,
                       "define @demo.arrays.Main.replaceAndAverage : (Array [ Double "
                       "],Int,Double)Double") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.doubleArrayUpdate(%values, "
                       "%index, %replacement)") &&
              contains(
                  result.llvmIr,
                  "define internal double @__scalanative_array_double_at(ptr %array, "
                  "i32 %index)") &&
              contains(
                  result.llvmIr,
                  "define internal void @__scalanative_array_double_set(ptr %array, "
                  "i32 %index, double %value)"),
          "NIR or LLVM did not lower Array[Double] operations")) {
    return code;
  }

  constexpr const char* invalidSource = R"(package demo.arrays

object InvalidDoubleArray {
  def main = {
    val values = Array(1.5)
    values(0) = 1
  }
}
)";
  scalanative::support::DiagnosticEngine invalidDiagnostics;
  scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidDoubleArray.scala", invalidSource, {}, invalidDiagnostics);
  if (int code = expect(
          !invalid.ok &&
              contains(invalid.diagnosticsText,
                       "array assignment value does not conform to the element type"),
          "Array[Double] assignment accepted an Int value")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-double-arrays";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-double-arrays.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "DoubleArrays.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "Double array binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native Double array smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "0\n3.000000\n4.500000\n",
                "native Double array did not preserve checked mutation and reads");
}

int smokeFloatArrayMvp() {
  constexpr const char* source = R"(package demo.arrays

object Main {
  def replaceAndAverage(values: Array[Float], index: Int, replacement: Float): Float = {
    values(index) = replacement
    val total = values(0) + values(1)
    total / 2.0F
  }

  def main = {
    val empty = Array[Float]()
    val values = Array(1.5F, 2.5F)
    println(empty.length)
    println(replaceAndAverage(values, 1, 4.5F))
    println(values(1))
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("FloatArrays.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "Float array build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "new Array [ Float ]") &&
              contains(result.nirText, "new Array [ Float ](1.5F, 2.5F)") &&
              contains(result.nirText,
                       "define @demo.arrays.Main.replaceAndAverage : (Array [ Float "
                       "],Int,Float)Float") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.floatArrayUpdate(%values, "
                       "%index, %replacement)") &&
              contains(
                  result.llvmIr,
                  "define internal float @__scalanative_array_float_at(ptr %array, "
                  "i32 %index)") &&
              contains(
                  result.llvmIr,
                  "define internal void @__scalanative_array_float_set(ptr %array, "
                  "i32 %index, float %value)"),
          "NIR or LLVM did not lower Array[Float] operations")) {
    return code;
  }

  constexpr const char* invalidSource = R"(package demo.arrays

object InvalidFloatArray {
  def main = {
    val values = Array(1.5F)
    values(0) = 1.5
  }
}
)";
  scalanative::support::DiagnosticEngine invalidDiagnostics;
  scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidFloatArray.scala", invalidSource, {}, invalidDiagnostics);
  if (int code = expect(
          !invalid.ok &&
              contains(invalid.diagnosticsText,
                       "array assignment value does not conform to the element type"),
          "Array[Float] assignment accepted a Double value")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-float-arrays";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-float-arrays.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("FloatArrays.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "Float array binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native Float array smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "0\n3.000000\n4.500000\n",
                "native Float array did not preserve checked mutation and reads");
}

int smokeCharArrayMvp() {
  constexpr const char* source = R"(package demo.arrays

object Main {
  def replaceAndRead(values: Array[Char], index: Int, replacement: Char): Char = {
    values(index) = replacement
    values(index)
  }

  def main = {
    val empty = Array[Char]()
    val letters = Array('a', 'b')
    println(empty.length)
    println(replaceAndRead(letters, 1, 'Z'))
    println(letters(1))
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("CharArrays.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "Char array build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "new Array [ Char ]") &&
              contains(result.nirText, "new Array [ Char ]('a', 'b')") &&
              contains(result.nirText,
                       "define @demo.arrays.Main.replaceAndRead : (Array [ Char "
                       "],Int,Char)Char") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.charArrayUpdate(%values, "
                       "%index, %replacement)") &&
              contains(result.llvmIr,
                       "define internal i32 @__scalanative_array_char_at(ptr %array, "
                       "i32 %index)") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_array_char_set(ptr %array, "
                       "i32 %index, i32 %value)"),
          "NIR or LLVM did not lower Array[Char] operations")) {
    return code;
  }

  constexpr const char* invalidSource = R"(package demo.arrays

object InvalidCharArray {
  def main = {
    val letters = Array('a')
    letters(0) = 1
  }
}
)";
  scalanative::support::DiagnosticEngine invalidDiagnostics;
  scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidCharArray.scala", invalidSource, {}, invalidDiagnostics);
  if (int code = expect(
          !invalid.ok &&
              contains(invalid.diagnosticsText,
                       "array assignment value does not conform to the element type"),
          "Array[Char] assignment accepted an Int value")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-char-arrays";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-char-arrays.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("CharArrays.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "Char array binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native Char array smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "0\nZ\nZ\n",
                "native Char array did not preserve checked mutation and reads");
}

int smokeAnyArrayMvp() {
  constexpr const char* source = R"(package demo.arrays

object Main {
  def replaceAndRead(values: Array[Any], index: Int, replacement: Int): Int = {
    values(index) = replacement
    values(index).asInstanceOf[Int]
  }

  def main = {
    val values = Array[Any](1, 2L, true, 'a')
    println(values.length)
    println(if (values(0).isInstanceOf[Int]) 1 else 0)
    println(if (values(1).isInstanceOf[Int]) 1 else 0)
    println(replaceAndRead(values, 0, 9))
    println(values(1).asInstanceOf[Long])
    println(if (values(2).asInstanceOf[Boolean]) 1 else 0)
    println(values(3).asInstanceOf[Char])
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("AnyArrays.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "Any array build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText,
                   "new Array [ Object ](box[Int](1), box[Long](2L), box[Boolean]("
                   "true), box[Char]('a'))") &&
              contains(result.nirText,
                       "define @demo.arrays.Main.replaceAndRead : (Array [ Object "
                       "],Int,Int)Int") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.referenceArrayUpdate.Object("
                       "%values, %index, box[Int](%replacement))") &&
              contains(result.nirText,
                       "unbox[Int](call %scala.scalanative.runtime.referenceArray"
                       "Apply.Object(%values, %index))") &&
              contains(result.nirText,
                       "unbox[Boolean](call %scala.scalanative.runtime.referenceArray"
                       "Apply.Object(%values, 2))") &&
              contains(result.nirText,
                       "is-instance-of[Int](call %scala.scalanative.runtime.reference"
                       "ArrayApply.Object(%values, 0))") &&
              contains(result.nirText,
                       "is-instance-of[Int](call %scala.scalanative.runtime.reference"
                       "ArrayApply.Object(%values, 1))") &&
              contains(result.llvmIr,
                       "call ptr @__scalanative_array_reference_at(ptr %values, "
                       "i32 %index)") &&
              contains(result.llvmIr, "call i32 @__scalanative_unbox_Int(ptr ") &&
              contains(result.llvmIr, "ptr @__scalanative_boxed_Int)"),
          "NIR or LLVM did not lower Array[Any] boxing and unboxing")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-any-arrays";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-any-arrays.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("AnyArrays.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "Any array binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native Any array smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "4\n1\n0\n9\n2\n1\na\n",
                "native Any array did not preserve boxed scalar values");
}

int smokeInferredMixedArrayMvp() {
  constexpr const char* source = R"(package demo.arrays

object Main {
  def firstValue(values: Array[Any]): Int = values(0).asInstanceOf[Int]

  def main = {
    val values = Array(1, 2L, true, 'a')
    println(values.length)
    println(if (values(0).isInstanceOf[Int]) 1 else 0)
    println(firstValue(values))
    println(values(1).asInstanceOf[Long])
    println(if (values(2).asInstanceOf[Boolean]) 1 else 0)
    println(values(3).asInstanceOf[Char])
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("MixedArrays.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "inferred mixed array build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText,
                   "new Array [ Object ](box[Int](1), box[Long](2L), box[Boolean]("
                   "true), box[Char]('a'))") &&
              contains(result.nirText,
                       "define @demo.arrays.Main.firstValue : (Array [ Object ])Int") &&
              contains(result.nirText,
                       "is-instance-of[Int](call %scala.scalanative.runtime.reference"
                       "ArrayApply.Object(%values, 0))") &&
              contains(result.llvmIr,
                       "call i1 @__scalanative_is_instance_of(ptr %tmp") &&
              contains(result.llvmIr, "ptr @__scalanative_boxed_Int)"),
          "inferred mixed Array literal did not lower through Object slots")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-mixed-arrays";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-mixed-arrays.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("MixedArrays.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "inferred mixed array binary failed to build: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(
          std::system(runCommand.c_str()) == 0,
          "native inferred mixed array smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "4\n1\n1\n2\n1\na\n",
                "native inferred mixed array did not preserve boxed scalar values");
}

int smokeAnyParametersMvp() {
  constexpr const char* source = R"(package demo.anyparameters

object Main {
  def inspect(value: Any): Int = {
    if (value.isInstanceOf[Int]) value.asInstanceOf[Int] else 0
  }

  def echo(value: Any): Any = value

  def main = {
    println(inspect(7))
    println(inspect(8L))
    println(echo(9).asInstanceOf[Int])
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("AnyParameters.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "Any parameter build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText,
                   "define @demo.anyparameters.Main.inspect : (Object)Int") &&
              contains(result.nirText,
                       "define @demo.anyparameters.Main.echo : (Object)Object") &&
              contains(result.nirText, "call %inspect(box[Int](7))") &&
              contains(result.nirText, "call %inspect(box[Long](8L))") &&
              contains(result.nirText, "unbox[Int](call %echo(box[Int](9)))") &&
              contains(result.llvmIr, "call ptr @__scalanative_box_alloc(i64 12, ptr "
                                      "@__scalanative_boxed_Int)") &&
              contains(result.llvmIr, "call ptr @__scalanative_box_alloc(i64 16, ptr "
                                      "@__scalanative_boxed_Long)"),
          "NIR or LLVM did not box scalar Any function arguments")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-any-parameters";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-any-parameters.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "AnyParameters.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "Any parameter binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code =
          expect(std::system(runCommand.c_str()) == 0,
                 "native Any parameter smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "7\n0\n9\n",
                "native Any parameter did not preserve boxed scalar values");
}

int smokeLocalAnyMvp() {
  constexpr const char* source = R"(package demo.localany

object Main {
  def main = {
    var current: Any = 1
    current = 2L
    val copied: Any = current
    println(current.asInstanceOf[Long])
    println(copied.asInstanceOf[Long])
    current = true
    println(if (current.asInstanceOf[Boolean]) 1 else 0)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("LocalAny.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "local Any build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code =
          expect(contains(result.nirText, "var %current : Object = box[Int](1)") &&
                     contains(result.nirText, "assign %current = box[Long](2L)") &&
                     contains(result.nirText, "let %copied : Object = %current") &&
                     contains(result.nirText, "assign %current = box[Boolean](true)") &&
                     contains(result.nirText, "unbox[Long](%current)") &&
                     contains(result.llvmIr, "alloca ptr") &&
                     contains(result.llvmIr, "call i64 @__scalanative_unbox_Long(ptr "),
                 "NIR or LLVM did not lower local Any boxing and mutation")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-local-any";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-local-any.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("LocalAny.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "local Any binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native local Any smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "2\n2\n1\n",
                "native local Any did not preserve boxed scalar values");
}

int smokeAnyFieldsMvp() {
  constexpr const char* source = R"(package demo.anyfields

class Box {
  var value: Any = 1

  def replaceWithLong(): Long = {
    value = 2L
    value.asInstanceOf[Long]
  }

  def replaceWithBoolean(): Int = {
    value = true
    if (value.asInstanceOf[Boolean]) 1 else 0
  }
}

object Main {
  var moduleValue: Any = 3

  def main = {
    val box = new Box()
    println(box.value.asInstanceOf[Int])
    println(box.replaceWithLong())
    println(box.replaceWithBoolean())
    moduleValue = 4L
    println(moduleValue.asInstanceOf[Long])
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("AnyFields.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "Any field build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "field @demo.anyfields.Box.value : Object") &&
              contains(result.nirText,
                       "field @demo.anyfields.Main.moduleValue$field : Object") &&
              contains(result.nirText, "box[Int](1)") &&
              contains(result.nirText, "box[Long](2L)") &&
              contains(result.nirText, "box[Boolean](true)") &&
              contains(result.nirText, "box[Int](3)") &&
              contains(result.nirText, "box[Long](4L)") &&
              contains(result.llvmIr, "call ptr @__scalanative_box_alloc(i64 12, ptr "
                                      "@__scalanative_boxed_Int)") &&
              contains(result.llvmIr, "call ptr @__scalanative_box_alloc(i64 16, ptr "
                                      "@__scalanative_boxed_Long)"),
          "NIR or LLVM did not lower Any field boxing and mutation")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-any-fields";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-any-fields.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("AnyFields.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "Any field binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native Any field smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "1\n2\n1\n4\n",
                "native Any fields did not preserve boxed scalar values");
}

int smokeAnyConstructorParametersMvp() {
  constexpr const char* source = R"(package demo.anyctor

class AnyBox(var value: Any) {
  def replaceWithLong(): Long = {
    value = 2L
    value.asInstanceOf[Long]
  }

  def replaceWithBoolean(): Int = {
    value = true
    if (value.asInstanceOf[Boolean]) 1 else 0
  }
}

class AnyParent(val value: Any)

class AnyChild(seed: Int) extends AnyParent(seed) {
  def parentInt: Int = value.asInstanceOf[Int]
}

object Main {
  def main = {
    val box = new AnyBox(1)
    println(box.value.asInstanceOf[Int])
    println(box.replaceWithLong())
    println(box.replaceWithBoolean())
    val child = new AnyChild(3)
    println(child.parentInt)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("AnyConstructorParameters.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "Any constructor parameter build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "field @demo.anyctor.AnyBox.value : Object") &&
              contains(result.nirText,
                       "field @demo.anyctor.AnyParent.value : Object") &&
              contains(result.nirText, "new demo.anyctor.AnyBox(box[Int](1))") &&
              contains(result.nirText, "eval assign %this.value = box[Long](2L)") &&
              contains(result.nirText,
                       "eval assign %this.value = box[Boolean](true)") &&
              contains(result.nirText,
                       "eval assign %super.value = box[Int](%this.seed)") &&
              contains(result.llvmIr, "ptr @__scalanative_boxed_Int") &&
              contains(result.llvmIr, "ptr @__scalanative_boxed_Long") &&
              contains(result.llvmIr, "ptr @__scalanative_boxed_Boolean"),
          "NIR or LLVM did not lower Any constructor parameter boxing")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-any-ctors";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-any-ctors.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "AnyConstructorParameters.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "Any constructor parameter binary failed to build: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native Any constructor parameter smoke binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "1\n2\n1\n3\n",
                "native Any constructor parameters did not preserve boxed values");
}

int smokeAnyUnitSymbolMvp() {
  constexpr const char* source = R"(package demo.anyunitsymbol

class AnyPayload(var value: Any) {
  def putUnit(): Int = {
    value = {}
    if (value.isInstanceOf[Unit]) 1 else 0
  }

  def putSymbol(): Int = {
    value = 'ready
    if (value.asInstanceOf[Symbol] == 'ready) 1 else 0
  }
}

object Main {
  def accept(value: Any): Int = {
    if (value.isInstanceOf[Symbol]) 1 else 0
  }

  def main = {
    val payload = new AnyPayload({})
    println(if (payload.value.isInstanceOf[Unit]) 1 else 0)
    println(payload.putSymbol())
    println(payload.putUnit())
    println(accept('ok))
    val explicit = Array[Any]({}, 'array)
    println(if (explicit(0).isInstanceOf[Unit]) 1 else 0)
    println(if (explicit(1).asInstanceOf[Symbol] == 'array) 1 else 0)
    val inferred = Array(1, 'mixed)
    println(if (inferred(1).asInstanceOf[Symbol] == 'mixed) 1 else 0)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("AnyUnitSymbol.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "Any Unit/Symbol build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText,
                   "field @demo.anyunitsymbol.AnyPayload.value : Object") &&
              contains(result.nirText, "box[Unit](block())") &&
              contains(result.nirText, "box[Symbol]('ready)") &&
              contains(result.nirText, "unbox[Symbol](%this.value) == 'ready") &&
              contains(
                  result.nirText,
                  "new Array [ Object ](box[Unit](block()), box[Symbol]('array))") &&
              contains(result.nirText,
                       "new Array [ Object ](box[Int](1), box[Symbol]('mixed))") &&
              contains(result.llvmIr, "ptr @__scalanative_boxed_Unit") &&
              contains(result.llvmIr, "ptr @__scalanative_boxed_Symbol") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_unbox_Unit") &&
              contains(result.llvmIr,
                       "define internal ptr @__scalanative_unbox_Symbol"),
          "NIR or LLVM did not lower Unit/Symbol Any boxing")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-any-unit-symbol";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-any-unit-symbol.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "AnyUnitSymbol.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "Any Unit/Symbol binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code =
          expect(std::system(runCommand.c_str()) == 0,
                 "native Any Unit/Symbol smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "1\n1\n1\n1\n1\n1\n1\n",
                "native Any Unit/Symbol did not preserve boxed values");
}

int smokeBuildDriverActions() {
  constexpr const char* source = "package demo.build\nobject Main { def main = 0 }\n";

  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-output.ll";
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine emitDiagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildOptions emitOptions;
  emitOptions.action = scalanative::tools::build::BuildAction::EmitLlvm;
  emitOptions.outputPath = outputPath;
  emitOptions.targetTriple = "x86_64-unknown-linux-gnu";
  emitOptions.gcMode = "hybrid";

  scalanative::tools::build::BuildResult emitted =
      driver.buildSource("BuildActions.scala", source, emitOptions, emitDiagnostics);
  if (int code = expect(emitted.ok, "build driver did not emit LLVM to a file")) {
    std::cerr << emitted.diagnosticsText;
    return code;
  }
  if (int code = expect(emitted.producedArtifacts.size() == 1 &&
                            emitted.producedArtifacts.front() == outputPath,
                        "build driver did not record emitted artifact path")) {
    return code;
  }
  const std::string written = readTextFile(outputPath);
  if (int code = expect(contains(written, "define i32 @demo_build_Main_main()"),
                        "emitted LLVM file did not contain lowered main method")) {
    return code;
  }
  bool sawConfig = false;
  bool sawWrite = false;
  for (const std::string& phase : emitted.phaseLog) {
    sawConfig = sawConfig || contains(phase, "build: action=emit-llvm, gc=hybrid") ||
                contains(phase, "target=x86_64-unknown-linux-gnu");
    sawWrite = sawWrite || contains(phase, "build: wrote LLVM IR to ");
  }
  if (int code = expect(sawConfig, "build driver did not log build config")) {
    return code;
  }
  if (int code = expect(sawWrite, "build driver did not log written LLVM output")) {
    return code;
  }

  const std::filesystem::path optimizedLlvmPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-optimized-output.ll";
  const std::filesystem::path optimizationReportPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-optimization-report.json";
  std::filesystem::remove(optimizedLlvmPath);
  std::filesystem::remove(optimizationReportPath);

  scalanative::support::DiagnosticEngine reportDiagnostics;
  scalanative::tools::build::BuildOptions reportOptions;
  reportOptions.action = scalanative::tools::build::BuildAction::EmitLlvm;
  reportOptions.optimize = true;
  reportOptions.outputPath = optimizedLlvmPath;
  reportOptions.optimizationReportPath = optimizationReportPath;
  scalanative::tools::build::BuildResult reported = driver.buildSource(
      "BuildActions.scala", source, reportOptions, reportDiagnostics);
  if (int code = expect(reported.ok, "build driver did not emit optimization report")) {
    std::cerr << reported.diagnosticsText;
    return code;
  }
  const std::string reportText = readTextFile(optimizationReportPath);
  if (int code =
          expect(!reported.optimizationReportText.empty() &&
                     reported.optimizationReportText == reportText,
                 "build driver did not retain the written optimization report")) {
    return code;
  }
  if (int code = expect(
          nonEmptyFile(optimizedLlvmPath) && nonEmptyFile(optimizationReportPath),
          "build driver did not write both optimized LLVM and report artifacts")) {
    return code;
  }
  if (int code =
          expect(std::find(reported.producedArtifacts.begin(),
                           reported.producedArtifacts.end(), optimizationReportPath) !=
                         reported.producedArtifacts.end() &&
                     std::find(reported.producedArtifacts.begin(),
                               reported.producedArtifacts.end(),
                               optimizedLlvmPath) != reported.producedArtifacts.end(),
                 "build driver did not record optimization report artifacts")) {
    return code;
  }
  if (int code = expect(
          contains(reportText, "\"optimized\": true") &&
              contains(reportText, "\"passes\"") &&
              contains(reportText, "\"name\": \"propagate-local-constants\"") &&
              contains(reportText, "\"name\": \"fold-constants\"") &&
              contains(reportText, "\"name\": \"eliminate-dead-local-lets\"") &&
              contains(reportText, "\"name\": \"simplify-blocks\"") &&
              contains(reportText, "\"name\": \"prune-unreachable-functions\"") &&
              contains(reportText, "\"durationMicros\""),
          "optimization report did not contain machine-readable pass metrics")) {
    return code;
  }

  constexpr const char* optimizedNirSource =
      "package demo.optimized\nobject Main { def main = { val base = 1; base + 2 } }\n";
  const std::filesystem::path optimizedNirPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-optimized-output.nir";
  const std::filesystem::path optimizedNirReportPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-optimized-nir-report.json";
  std::filesystem::remove(optimizedNirPath);
  std::filesystem::remove(optimizedNirReportPath);

  scalanative::support::DiagnosticEngine optimizedNirDiagnostics;
  scalanative::tools::build::BuildOptions optimizedNirOptions;
  optimizedNirOptions.action = scalanative::tools::build::BuildAction::EmitNir;
  optimizedNirOptions.optimize = true;
  optimizedNirOptions.outputPath = optimizedNirPath;
  optimizedNirOptions.optimizationReportPath = optimizedNirReportPath;
  scalanative::tools::build::BuildResult optimizedNir =
      driver.buildSource("OptimizedNir.scala", optimizedNirSource, optimizedNirOptions,
                         optimizedNirDiagnostics);
  if (int code =
          expect(optimizedNir.ok, "build driver did not emit optimized NIR report")) {
    std::cerr << optimizedNir.diagnosticsText;
    return code;
  }
  const std::string optimizedNirText = readTextFile(optimizedNirPath);
  if (int code = expect(!optimizedNir.optimizationReportText.empty() &&
                            optimizedNir.optimizationReportText ==
                                readTextFile(optimizedNirReportPath),
                        "build driver did not retain the optimized NIR report")) {
    return code;
  }
  if (int code = expect(
          optimizedNir.nirText == optimizedNirText && nonEmptyFile(optimizedNirPath) &&
              nonEmptyFile(optimizedNirReportPath),
          "build driver did not write optimized NIR and report artifacts")) {
    return code;
  }
  if (int code =
          expect(contains(optimizedNirText, "ret Int 3") &&
                     !contains(optimizedNirText, "(%base + 2)") &&
                     contains(optimizedNirText, "define "
                                                "@scala.scalanative.runtime.main"),
                 "emit-nir --optimize did not expose linked optimized NIR")) {
    return code;
  }

  scalanative::support::DiagnosticEngine invalidReportDiagnostics;
  scalanative::tools::build::BuildOptions invalidReportOptions;
  invalidReportOptions.action = scalanative::tools::build::BuildAction::EmitLlvm;
  invalidReportOptions.optimizationReportPath = optimizationReportPath;
  scalanative::tools::build::BuildResult invalidReport = driver.buildSource(
      "BuildActions.scala", source, invalidReportOptions, invalidReportDiagnostics);
  if (int code = expect(
          !invalidReport.ok && contains(invalidReport.diagnosticsText,
                                        "optimization report requires --optimize"),
          "build driver accepted an optimization report without --optimize")) {
    return code;
  }

  scalanative::support::DiagnosticEngine checkDiagnostics;
  scalanative::tools::build::BuildOptions checkOptions;
  checkOptions.action = scalanative::tools::build::BuildAction::Check;
  scalanative::tools::build::BuildResult checked =
      driver.buildSource("BuildActions.scala", source, checkOptions, checkDiagnostics);
  if (int code = expect(checked.ok, "build driver check action failed")) {
    std::cerr << checked.diagnosticsText;
    return code;
  }
  if (int code = expect(checked.llvmIr.empty(),
                        "check action should stop before LLVM codegen")) {
    return code;
  }

  const std::filesystem::path objectPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-output.o";
  std::filesystem::remove(objectPath);

  scalanative::support::DiagnosticEngine objectDiagnostics;
  scalanative::tools::build::BuildOptions objectOptions;
  objectOptions.action = scalanative::tools::build::BuildAction::BuildObject;
  objectOptions.outputPath = objectPath;
  scalanative::tools::build::BuildResult object = driver.buildSource(
      "BuildActions.scala", source, objectOptions, objectDiagnostics);
  if (!object.ok) {
    return expect(contains(object.diagnosticsText, "clang toolchain not found"),
                  "build-object failed for a reason other than a missing clang "
                  "toolchain: " +
                      object.diagnosticsText);
  }
  if (int code = expect(nonEmptyFile(objectPath),
                        "build-object did not write a non-empty object file")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-binary";
  std::filesystem::remove(binaryPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "BuildActions.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "build-binary failed for a reason other than a missing clang "
                  "toolchain: " +
                      binary.diagnosticsText);
  }
  if (int code = expect(nonEmptyFile(binaryPath),
                        "build-binary did not write a non-empty binary")) {
    return code;
  }

  if (int code = expect(std::system(binaryPath.string().c_str()) == 0,
                        "native smoke binary did not exit successfully")) {
    return code;
  }

  constexpr const char* printSource = R"(package demo.build
object PrintMain {
  def main = println(50)
}
)";

  const std::filesystem::path printBinaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-print";
  const std::filesystem::path printOutputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-print.out";
  std::filesystem::remove(printBinaryPath);
  std::filesystem::remove(printOutputPath);

  scalanative::support::DiagnosticEngine printDiagnostics;
  scalanative::tools::build::BuildOptions printOptions;
  printOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  printOptions.outputPath = printBinaryPath;
  scalanative::tools::build::BuildResult printed = driver.buildSource(
      "PrintActions.scala", printSource, printOptions, printDiagnostics);
  if (!printed.ok) {
    return expect(contains(printed.diagnosticsText, "clang toolchain not found"),
                  "println build-binary failed for a reason other than a missing "
                  "clang toolchain: " +
                      printed.diagnosticsText);
  }
  if (int code = expect(contains(printed.llvmIr, "call i32 (ptr, ...) @printf"),
                        "println(Int) did not lower to a printf call")) {
    return code;
  }
  if (int code = expect(contains(printed.llvmIr, "@.fmt.int"),
                        "println(Int) format string was not emitted into LLVM IR")) {
    return code;
  }

  const std::string runPrintCommand =
      printBinaryPath.string() + " > " + printOutputPath.string();
  if (int code = expect(std::system(runPrintCommand.c_str()) == 0,
                        "native println smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(printOutputPath) == "50\n",
                "native println(Int) smoke binary did not print expected output");
}

int smokeLlvmCodegenSubset() {
  constexpr const char* source = R"(package demo.codegen

object Main {
  def add(a: Int, b: Int): Int = a + b
  def choose(flag: Boolean): Int = if (flag) 1 else 2
  val called = add(1, 2)
  def blocky = {
    val local = 1
    local
  }
}

object Config {
  val answer: Int = 42
}

object UsesConfig {
  val selected = Config.answer
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("Codegen.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "codegen subset build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "define @demo.codegen.Main.add : (Int,Int)Int"),
          "NIR signature did not include method parameter types")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "define i32 @demo_codegen_Main_add(i32 %a, i32 %b)"),
          "LLVM IR did not lower Int method parameters")) {
    return code;
  }
  if (int code = expect(contains(result.llvmIr, "%tmp0 = add i32 %a, %b"),
                        "LLVM IR did not lower simple Int addition")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr,
                   "br i1 %flag, label %if_then_tmp0, label %if_else_tmp0") &&
              contains(result.llvmIr, "%tmp1 = phi i32 [ 1, %if_then_tmp0 ], [ 2, "
                                      "%if_else_tmp0 ]"),
          "LLVM IR did not lower simple if expression with branch control flow")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "call i32 @demo_codegen_Main_add(i32 1, i32 2)"),
          "LLVM IR did not lower same-owner method call")) {
    return code;
  }
  if (int code =
          expect(contains(result.llvmIr, "call i32 @demo_codegen_Config_answer()"),
                 "LLVM IR did not lower object member selection")) {
    return code;
  }
  return expect(contains(result.llvmIr, "%local = add i32 1, 0") &&
                    contains(result.llvmIr, "ret i32 %local"),
                "LLVM IR did not lower block-local Int value");
}

int smokeModuleSingletonsMvp() {
  constexpr const char* source = R"(package demo.modules

object Settings {
  println("settings init")
  val base: Int = 40
  var answer: Int = base + 2
}

object Reader {
  def read(settings: Settings): Int = settings.answer
}

object Main {
  def main = {
    val first: Settings = Settings
    val second: Settings = Settings
    println(Reader.read(first))
    second.answer = second.answer + 1
    println(first.answer)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("ModuleSingletons.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "module singleton build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "let %first : demo.modules.Settings = %Settings") &&
              contains(result.nirText,
                       "let %second : demo.modules.Settings = %Settings") &&
              contains(result.nirText,
                       "field @demo.modules.Settings.base$field : Int") &&
              contains(result.nirText,
                       "field @demo.modules.Settings.answer$field : Int") &&
              contains(result.nirText,
                       "define @demo.modules.Settings.answer_$eq : (Int)Unit") &&
              contains(result.nirText,
                       "define @demo.modules.Settings.$init : ()Unit") &&
              contains(result.nirText,
                       "eval call %scala.scalanative.runtime.println(\"settings "
                       "init\")") &&
              contains(result.nirText,
                       "eval assign %demo.modules.Settings.base$field = 40") &&
              contains(result.nirText,
                       "eval assign %demo.modules.Settings.answer$field = (%base + "
                       "2)") &&
              contains(result.nirText, "call %Reader.read(%first)") &&
              contains(result.nirText,
                       "eval assign %second.answer = (%second.answer + 1)"),
          "NIR did not preserve stored module initialization and mutation")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "@__type_demo_modules_Settings = private constant "
                                  "%scalanative.type_descriptor { i32 4") &&
              contains(result.llvmIr,
                       "@__module_instance_demo_modules_Settings = private global ptr "
                       "null") &&
              contains(result.llvmIr,
                       "@__scalanative_module_roots = private constant [3 x ptr]") &&
              contains(result.llvmIr,
                       "@__scalanative_module_root_count = private constant i32 3") &&
              contains(result.llvmIr,
                       "define internal ptr @__scalanative_module_root(i32 %index)") &&
              contains(
                  result.llvmIr,
                  "define internal ptr @__scalanative_module_demo_modules_Settings") &&
              contains(result.llvmIr, "ptr @__type_demo_modules_Settings, i32 2)") &&
              contains(result.llvmIr, "call ptr @__scalanative_alloc(i64 16, ptr "
                                      "@__type_demo_modules_Settings, i32 2)") &&
              contains(result.llvmIr, "call void @demo_modules_Settings__init()") &&
              contains(result.llvmIr, "getelementptr i8, ptr %tmp0, i64 8") &&
              contains(result.llvmIr, "getelementptr i8, ptr %tmp0, i64 12") &&
              contains(result.llvmIr,
                       "call ptr @__scalanative_module_demo_modules_Settings()"),
          "LLVM did not emit stored immortal module initialization and roots")) {
    return code;
  }

  const std::filesystem::path binaryPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-module-singletons";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-module-singletons.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "ModuleSingletons.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "module singleton binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "module singleton binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "settings init\n42\n43\n",
                "module singleton fields did not initialize once or preserve state");
}

int smokeSingletonPatternMvp() {
  constexpr const char* source = R"(package demo.singletonmatch

trait Signal
object Ready extends Signal
object Waiting extends Signal
object State {
  object Ready extends Signal
  object Waiting extends Signal

  def ready: Signal = Ready
  def waiting: Signal = Waiting
}
class Other extends Signal

object Main {
  def describe(value: Signal): String = value match {
    case Ready => "ready"
    case Waiting => "waiting"
    case _ => "other"
  }

  def known(value: Signal): String = value match {
    case Ready | Waiting => "known"
    case _ => "other"
  }

  def describeQualified(value: Signal): String = value match {
    case State.Ready => "state-ready"
    case State.Waiting => "state-waiting"
    case _ => "other"
  }

  def main = {
    println(describe(Ready))
    println(describe(Waiting))
    println(describe(new Other()))
    println(known(Ready))
    println(known(Waiting))
    println(known(new Other()))
    println(describeQualified(State.ready))
    println(describeQualified(State.waiting))
    println(describeQualified(new Other()))
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("SingletonPatterns.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "singleton-pattern build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code =
          expect(contains(result.nirText, "(%$match0 == %Ready)") &&
                     contains(result.nirText, "(%$match0 == %Waiting)") &&
                     contains(result.nirText, "(%$match2 == %State.Ready)") &&
                     contains(result.nirText, "(%$match2 == %State.Waiting)"),
                 "singleton patterns did not lower to module identity comparisons")) {
    return code;
  }

  const std::filesystem::path binaryPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-singleton-pattern";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-singleton-pattern.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "SingletonPatterns.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "singleton-pattern binary failed to build: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "singleton-pattern binary did not exit successfully")) {
    return code;
  }
  if (int code = expect(readTextFile(outputPath) ==
                            "ready\nwaiting\nother\nknown\nknown\nother\nstate-ready\n"
                            "state-waiting\nother\n",
                        "singleton patterns produced incorrect native output")) {
    return code;
  }

  constexpr const char* classPatternSource = R"(package demo.singletonmatch
trait Signal
class NotSingleton extends Signal
object Bad {
  def describe(value: Signal): String = value match {
    case NotSingleton => "invalid"
    case _ => "other"
  }
}
)";
  scalanative::support::DiagnosticEngine classPatternDiagnostics;
  scalanative::tools::build::BuildResult classPattern = driver.buildSource(
      "ClassSingletonPattern.scala", classPatternSource, {}, classPatternDiagnostics);
  if (int code = expect(
          !classPattern.ok &&
              contains(classPattern.diagnosticsText,
                       "singleton match pattern must name an object: NotSingleton"),
          "class singleton pattern did not produce a focused diagnostic")) {
    return code;
  }

  constexpr const char* bindingAlternativeSource = R"(package demo.singletonmatch
trait Signal
object Ready extends Signal
object Bad {
  def describe(value: Signal): String = value match {
    case Ready | binding => "invalid"
    case _ => "other"
  }
}
)";
  scalanative::support::DiagnosticEngine bindingAlternativeDiagnostics;
  scalanative::tools::build::BuildResult bindingAlternative =
      driver.buildSource("SingletonBindingAlternative.scala", bindingAlternativeSource,
                         {}, bindingAlternativeDiagnostics);
  if (int code = expect(
          !bindingAlternative.ok &&
              contains(
                  bindingAlternative.diagnosticsText,
                  "singleton pattern alternatives require capitalized object names"),
          "singleton binding alternative did not produce a focused diagnostic")) {
    return code;
  }

  constexpr const char* nestedClassPatternSource = R"(package demo.singletonmatch
trait Signal
object State {
  class NotSingleton extends Signal
}
object Bad {
  def describe(value: Signal): String = value match {
    case State.NotSingleton => "invalid"
    case _ => "other"
  }
}
)";
  scalanative::support::DiagnosticEngine nestedClassPatternDiagnostics;
  scalanative::tools::build::BuildResult nestedClassPattern =
      driver.buildSource("NestedClassSingletonPattern.scala", nestedClassPatternSource,
                         {}, nestedClassPatternDiagnostics);
  return expect(!nestedClassPattern.ok &&
                    contains(nestedClassPattern.diagnosticsText,
                             "singleton match pattern must name an object: "
                             "State.NotSingleton"),
                "nested class singleton pattern did not produce a focused diagnostic");
}

int smokeHybridMemoryRuntimeMvp() {
  constexpr const char* source = R"(package demo.memory

class Node(var next: Node, val value: Int)

object Roots {
  val root: Node = new Node(null, 7)
  root.next = root
}

object Collector {
  def keep(node: Node): Int = {
    gcCollect()
    node.value
  }
}

object Main {
  def main = {
    val temporary = new Node(null, 9)
    gcCollect()
    println(Roots.root.value)
    println(Roots.root.next.value)
    println(temporary.value)
    println(Collector.keep(new Node(null, 11)))
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("MemoryRuntime.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "hybrid memory runtime build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "field @demo.memory.Node.next : demo.memory.Node") &&
              contains(result.nirText,
                       "field @demo.memory.Roots.root$field : demo.memory.Node") &&
              contains(result.nirText,
                       "eval assign %demo.memory.Roots.root$field = new "
                       "demo.memory.Node(null, 7)") &&
              contains(result.nirText, "eval assign %root.next = %root") &&
              contains(result.nirText,
                       "eval call %scala.scalanative.runtime.gcCollect()") &&
              contains(result.nirText, "println(%Roots.root.next.value)"),
          "NIR did not preserve the rooted cyclic object graph")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "%scalanative.gc_node = type { ptr, i1, ptr }") &&
              contains(result.llvmIr,
                       "@__scalanative_gc_head = private global ptr null") &&
              contains(result.llvmIr,
                       "@__trace_offsets_demo_memory_Node = private constant [1 x "
                       "i32] [i32 8]") &&
              contains(result.llvmIr,
                       "@__trace_offsets_demo_memory_Roots = private constant [1 x "
                       "i32] [i32 8]") &&
              contains(result.llvmIr,
                       "call ptr @__scalanative_object_alloc(i64 20, ptr "
                       "@__type_demo_memory_Node)") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_gc_mark_object") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_gc_mark_children") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_gc_collect") &&
              contains(result.llvmIr,
                       "%scalanative.shadow_frame = type { ptr, i32, ptr }") &&
              contains(result.llvmIr,
                       "@__scalanative_shadow_stack = private global ptr null") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_gc_mark_shadow_roots") &&
              contains(result.llvmIr, "store ptr %temporary, ptr %__shadow_root_") &&
              contains(result.llvmIr, "store ptr %node, ptr %__shadow_root_") &&
              contains(result.llvmIr, "call void @free(ptr %object)") &&
              contains(result.llvmIr, "call void @__scalanative_gc_collect()"),
          "LLVM did not emit module-rooted mark/sweep metadata and runtime paths")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-hybrid-memory-runtime";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-hybrid-memory-runtime.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "MemoryRuntime.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "hybrid memory runtime binary failed to build: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "hybrid memory runtime binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "7\n7\n9\n11\n",
                "hybrid memory runtime corrupted the rooted cyclic graph");
}

int smokeScopedZoneRuntimeMvp() {
  constexpr const char* source = R"(package demo.zone

class Pair(val left: Int, val right: Int) {
  def total: Int = left + right
}

class Node(var next: Node, val value: Int)

object Main {
  def main = {
    val answer: Int = Zone.scoped({
      val pair = new Pair(20, 22)
      var bonus = 0
      bonus = bonus + 1
      pair.total + bonus
    })
    println(answer)
    Zone.scoped({
      val pair = new Pair(7, 8)
      println(pair.total)
    })
    val nested: Int = Zone.scoped({
      val pair = new Pair(1, 2)
      val inner = Zone.scoped({
        val pair = new Pair(4, 5)
        pair.total
      })
      pair.total + inner
    })
    println(nested)
    val localGraph: Int = Zone.scoped({
      val head = new Node(null, 7)
      val tail = new Node(null, 8)
      head.next = tail
      head.next.value
    })
    println(localGraph)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("ZoneScopes.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "scoped Zone build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "let %answer : Int = zone-scoped(block(let %pair : "
                                   "demo.zone.Pair = new demo.zone.Pair(20, 22)") &&
              contains(result.nirText, "var %bonus : Int = 0") &&
              contains(result.nirText, "assign %bonus = (%bonus + 1)") &&
              contains(result.nirText,
                       "eval zone-scoped(block(let %pair : demo.zone.Pair = new ") &&
              contains(result.nirText,
                       "let %inner : Int = zone-scoped(block(let %pair : "
                       "demo.zone.Pair = new demo.zone.Pair(4, 5)") &&
              contains(result.nirText, "assign %head.next = %tail; %head.next.value"),
          "NIR did not preserve scoped Zone lifetime boundaries")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr,
                   "%scalanative.arena_block = type { ptr, i64, i64, ptr }") &&
              contains(result.llvmIr,
                       "@__scalanative_current_zone = private global ptr null") &&
              contains(result.llvmIr,
                       "define internal ptr @__scalanative_arena_new_block") &&
              contains(result.llvmIr, "%doubled_capacity = shl i64 %capacity, 1") &&
              contains(result.llvmIr,
                       "define internal ptr @__scalanative_zone_enter()") &&
              contains(
                  result.llvmIr,
                  "define internal void @__scalanative_zone_exit(ptr %previous)") &&
              contains(result.llvmIr,
                       "define internal ptr @__scalanative_object_alloc") &&
              contains(result.llvmIr,
                       "call ptr @__scalanative_object_alloc(i64 16, ptr "
                       "@__type_demo_zone_Pair)") &&
              contains(result.llvmIr, "call ptr @__scalanative_zone_enter()") &&
              contains(result.llvmIr, "call void @__scalanative_zone_exit(ptr %"),
          "LLVM did not emit growable nested Zone allocation paths")) {
    return code;
  }

  constexpr const char* escapingSource = R"(package demo.zone

class Escaped(val value: Int)

object Escape {
  def main = {
    val escaped: Escaped = Zone.scoped({ new Escaped(1) })
    println(escaped.value)
  }
}
)";
  scalanative::support::DiagnosticEngine escapingDiagnostics;
  scalanative::tools::build::BuildResult escaping =
      driver.buildSource("ZoneEscape.scala", escapingSource, {}, escapingDiagnostics);
  if (int code =
          expect(!escaping.ok && contains(escaping.diagnosticsText,
                                          "references cannot escape the zone"),
                 "typechecker allowed a reference result to escape Zone.scoped")) {
    return code;
  }

  constexpr const char* leakedLocalSource = R"(package demo.zone

object LeakedLocal {
  def main = {
    Zone.scoped({
      val hidden = 7
      hidden
    })
    println(hidden)
  }
}
)";
  scalanative::support::DiagnosticEngine leakedLocalDiagnostics;
  scalanative::tools::build::BuildResult leakedLocal = driver.buildSource(
      "ZoneLeakedLocal.scala", leakedLocalSource, {}, leakedLocalDiagnostics);
  if (int code =
          expect(!leakedLocal.ok && contains(leakedLocal.diagnosticsText,
                                             "unresolved identifier: hidden"),
                 "Zone.scoped local remained visible outside its lexical block")) {
    return code;
  }

  constexpr const char* outerAssignmentSource = R"(package demo.zone

class OuterValue(val value: Int)

object OuterAssignment {
  def main = {
    var escaped: OuterValue = null
    Zone.scoped({
      val local = new OuterValue(1)
      escaped = local
      0
    })
    println(0)
  }
}
)";
  scalanative::support::DiagnosticEngine outerAssignmentDiagnostics;
  scalanative::tools::build::BuildResult outerAssignment =
      driver.buildSource("ZoneOuterAssignment.scala", outerAssignmentSource, {},
                         outerAssignmentDiagnostics);
  if (int code = expect(
          !outerAssignment.ok && contains(outerAssignment.diagnosticsText,
                                          "cannot be assigned to an outer variable"),
          "Zone.scoped reference escaped through an outer mutable local")) {
    return code;
  }

  constexpr const char* fieldStoreSource = R"(package demo.zone

class StoredValue(val value: Int)

object Holder {
  var escaped: StoredValue = null
}

object FieldStore {
  def main = Zone.scoped({
    val local = new StoredValue(2)
    Holder.escaped = local
    0
  })
}
)";
  scalanative::support::DiagnosticEngine fieldStoreDiagnostics;
  scalanative::tools::build::BuildResult fieldStore = driver.buildSource(
      "ZoneFieldStore.scala", fieldStoreSource, {}, fieldStoreDiagnostics);
  if (int code = expect(!fieldStore.ok &&
                            contains(fieldStore.diagnosticsText,
                                     "cannot be stored in an object outside the zone"),
                        "Zone.scoped reference escaped through a module field")) {
    return code;
  }

  constexpr const char* callArgumentSource = R"(package demo.zone

class CallValue(val value: Int)

object Sink {
  def consume(value: CallValue): Int = value.value
}

object CallArgument {
  def main = Zone.scoped({
    val local = new CallValue(3)
    Sink.consume(local)
  })
}
)";
  scalanative::support::DiagnosticEngine callArgumentDiagnostics;
  scalanative::tools::build::BuildResult callArgument = driver.buildSource(
      "ZoneCallArgument.scala", callArgumentSource, {}, callArgumentDiagnostics);
  if (int code =
          expect(!callArgument.ok && contains(callArgument.diagnosticsText,
                                              "cannot be passed to an ordinary call"),
                 "Zone.scoped reference escaped through an ordinary call argument")) {
    return code;
  }

  constexpr const char* receiverMethodSource = R"(package demo.zone

class ReceiverLeak(val value: Int) {
  def leak: Unit = {
    Registry.saved = this
  }
}

object Registry {
  var saved: ReceiverLeak = null
}

object ReceiverMethod {
  def main = Zone.scoped({
    val local = new ReceiverLeak(4)
    local.leak
    0
  })
}
)";
  scalanative::support::DiagnosticEngine receiverMethodDiagnostics;
  scalanative::tools::build::BuildResult receiverMethod = driver.buildSource(
      "ZoneReceiverMethod.scala", receiverMethodSource, {}, receiverMethodDiagnostics);
  if (int code = expect(
          !receiverMethod.ok && contains(receiverMethod.diagnosticsText,
                                         "receiver may escape through method leak"),
          "Zone.scoped receiver escaped through a known leaking method")) {
    return code;
  }

  constexpr const char* transitiveMethodSource = R"(package demo.zone

class TransitiveLeak(val value: Int) {
  def leak: Unit = {
    TransitiveRegistry.saved = this
  }

  def throughHelper: Unit = leak
}

object TransitiveRegistry {
  var saved: TransitiveLeak = null
}

object TransitiveMethod {
  def main = Zone.scoped({
    val local = new TransitiveLeak(5)
    local.throughHelper
    0
  })
}
)";
  scalanative::support::DiagnosticEngine transitiveMethodDiagnostics;
  scalanative::tools::build::BuildResult transitiveMethod =
      driver.buildSource("ZoneTransitiveMethod.scala", transitiveMethodSource, {},
                         transitiveMethodDiagnostics);
  if (int code =
          expect(!transitiveMethod.ok &&
                     contains(transitiveMethod.diagnosticsText,
                              "receiver may escape through method throughHelper"),
                 "Zone.scoped receiver escaped through a transitive helper call")) {
    return code;
  }

  constexpr const char* virtualMethodSource = R"(package demo.zone

class BaseReceiver {
  def run: Unit = {}
}

class ChildReceiver extends BaseReceiver {
  override def run: Unit = {
    OverrideRegistry.saved = this
  }
}

object OverrideRegistry {
  var saved: BaseReceiver = null
}

object VirtualMethod {
  def main = Zone.scoped({
    val local: BaseReceiver = new ChildReceiver()
    local.run
    0
  })
}
)";
  scalanative::support::DiagnosticEngine virtualMethodDiagnostics;
  scalanative::tools::build::BuildResult virtualMethod = driver.buildSource(
      "ZoneVirtualMethod.scala", virtualMethodSource, {}, virtualMethodDiagnostics);
  if (int code = expect(
          !virtualMethod.ok && contains(virtualMethod.diagnosticsText,
                                        "receiver may escape through method run"),
          "Zone.scoped receiver escaped through a base-typed leaking override")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-zone-scopes";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-zone-scopes.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("ZoneScopes.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "scoped Zone binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "scoped Zone binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "43\n15\n12\n8\n",
                "scoped Zone binary produced incorrect output");
}

int smokeGcTelemetryMvp() {
  constexpr const char* source = R"(package demo.gctelemetry

class Node(val value: Int)

object Main {
  def allocateDetached(): Long = {
    val temporary = new Node(42)
    gcLiveObjectCount()
  }

  def main = {
    val baseline = gcLiveObjectCount()
    val withDetached = allocateDetached()
    gcCollect()
    val afterCollection = gcLiveObjectCount()
    println(withDetached - baseline)
    println(afterCollection - baseline)
    println(gcCollectionCount())
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("GcTelemetry.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "GC telemetry build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code =
          expect(contains(result.nirText,
                          "call %scala.scalanative.runtime.gcLiveObjectCount()") &&
                     contains(result.nirText,
                              "call %scala.scalanative.runtime.gcCollectionCount()"),
                 "NIR did not preserve GC telemetry calls")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "load i64, ptr @__scalanative_gc_allocation_count") &&
              contains(result.llvmIr,
                       "load i64, ptr @__scalanative_gc_collection_count"),
          "LLVM did not lower GC telemetry calls to collector counters")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-gc-telemetry";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-gc-telemetry.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("GcTelemetry.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "GC telemetry binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "GC telemetry smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "1\n0\n1\n",
                "GC telemetry did not observe a detached allocation being swept");
}

int smokeAutomaticGcMvp() {
  constexpr const char* source = R"(package demo.gcstress

class Leaf(val value: Int)

class Pair(val left: Leaf, val right: Leaf) {
  def total: Int = left.value + right.value
}

object Main {
  def right(): Leaf = new Leaf(22)

  def main = {
    gcSetCollectionThreshold(1L)
    val pair = new Pair(new Leaf(20), right())
    println(pair.total)
    println(gcCollectionCount())
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("GcStress.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "automatic GC stress build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText,
                   "call %scala.scalanative.runtime.gcSetCollectionThreshold(1L)") &&
              contains(result.llvmIr,
                       "call void @__scalanative_gc_set_threshold(i64 1)") &&
              contains(result.llvmIr, "call void @__scalanative_gc_poll()"),
          "automatic GC threshold calls were not lowered")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-automatic-gc";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-automatic-gc.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("GcStress.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "automatic GC stress binary failed to build: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "automatic GC stress binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "42\n2\n",
                "automatic GC collected a nested temporary before construction");
}

int smokeClassInstanceMvp() {
  constexpr const char* source = R"(package demo.instance

class Counter(start: Int) {
  def value: Int = start
  def doubled: Int = value + value
  def shadow(start: Int): Int = start
  def message: String = "class instance method"
}

object Main {
  def main = {
    val counter = new Counter(42)
    println(counter.value)
    println(counter.doubled)
    println(counter.shadow(5))
    println(new Counter(7).message)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("ClassInstance.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "class instance MVP build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code =
          expect(contains(result.nirText, "define @demo.instance.Counter.value : "
                                          "(demo.instance.Counter)Int"),
                 "NIR did not add an implicit receiver to class methods")) {
    return code;
  }
  if (int code =
          expect(contains(result.nirText, "field @demo.instance.Counter.start : Int"),
                 "NIR did not emit a field for the class parameter")) {
    return code;
  }
  if (int code =
          expect(contains(result.nirText, "let %counter : demo.instance.Counter = new "
                                          "demo.instance.Counter(42)"),
                 "NIR did not preserve constructor arguments in class allocation")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "define i32 @demo_instance_Counter_value(ptr %this)"),
          "LLVM IR did not lower the class method receiver parameter")) {
    return code;
  }
  if (int code =
          expect(contains(result.nirText, "define @demo.instance.Counter.doubled : "
                                          "(demo.instance.Counter)Int"),
                 "NIR did not infer this-based class method return type")) {
    return code;
  }
  if (int code =
          expect(contains(result.nirText, "ret Int (%this.value + %this.value)") &&
                     contains(result.nirText, "define @demo.instance.Counter.shadow : "
                                              "(demo.instance.Counter,Int)Int") &&
                     contains(result.nirText, "ret Int %start"),
                 "NIR did not rewrite implicit this while preserving parameter "
                 "shadowing")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "call ptr @__scalanative_object_alloc(i64 12, ptr "
                                  "@__type_demo_instance_Counter)") &&
              contains(result.llvmIr,
                       "@__type_demo_instance_Counter = private constant "
                       "%scalanative.type_descriptor { i32 1, i32 1024, i64 12"),
          "LLVM IR did not allocate the class descriptor and field payload")) {
    return code;
  }
  if (int code = expect(contains(result.llvmIr, "store i32 42"),
                        "LLVM IR did not store constructor argument into the field")) {
    return code;
  }
  if (int code = expect(contains(result.llvmIr, "load i32"),
                        "LLVM IR did not load the stored class field")) {
    return code;
  }
  if (int code = expect(contains(result.llvmIr,
                                 "call i32 @demo_instance_Counter_value(ptr %counter)"),
                        "LLVM IR did not lower local instance method selection")) {
    return code;
  }
  if (int code =
          expect(contains(result.llvmIr,
                          "call i32 @demo_instance_Counter_doubled(ptr %counter)"),
                 "LLVM IR did not lower this-based class method selection")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr,
                   "call i32 @demo_instance_Counter_shadow(ptr %counter, i32 5)"),
          "LLVM IR did not lower class method with explicit parameter")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "call i32 @demo_instance_Counter_value(ptr %this)"),
          "LLVM IR did not lower this.value inside a class method")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "call ptr @demo_instance_Counter_message(ptr %tmp"),
          "LLVM IR did not lower direct new Counter().message selection")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-class-instance";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-class-instance.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "ClassInstance.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "class instance build-binary failed for a reason other than a "
                  "missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native class instance smoke binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "42\n84\n5\nclass instance method\n",
                "native class instance smoke binary did not print expected output");
}

int smokeClassFieldMvp() {
  constexpr const char* source = R"(package demo.fields

class Counter(start: Int) {
  val base: Int = start + 1
  val doubled: Int = base + base
  val label: String = "class field"

  def total: Int = doubled + start
}

object Main {
  def main = {
    val counter = new Counter(41)
    println(counter.base)
    println(counter.doubled)
    println(counter.total)
    println(counter.label)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("ClassFields.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "class field MVP build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "field @demo.fields.Counter.base : Int") &&
              contains(result.nirText, "field @demo.fields.Counter.doubled : Int") &&
              contains(result.nirText, "field @demo.fields.Counter.label : String"),
          "NIR did not emit class-body vals as stored fields")) {
    return code;
  }
  if (int code =
          expect(contains(result.nirText, "ret Int (%this.start + 1)") &&
                     contains(result.nirText, "ret Int (%this.base + %this.base)") &&
                     contains(result.nirText, "ret String \"class field\""),
                 "NIR did not preserve class field initializer bodies")) {
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "let %counter : demo.fields.Counter = new "
                                   "demo.fields.Counter(41)"),
          "NIR did not keep class-body fields out of the constructor argument list")) {
    return code;
  }
  if (int code =
          expect(contains(result.llvmIr, "call ptr @__scalanative_object_alloc(i64 32"),
                 "LLVM IR did not allocate constructor and body fields")) {
    return code;
  }
  if (int code = expect(contains(result.llvmIr, "store i32 41"),
                        "LLVM IR did not store constructor parameter field")) {
    return code;
  }
  if (int code = expect(contains(result.llvmIr, "store i32 %tmp"),
                        "LLVM IR did not store computed class-body Int fields")) {
    return code;
  }
  if (int code =
          expect(contains(result.llvmIr,
                          "store ptr getelementptr inbounds ([12 x i8], ptr @.str."),
                 "LLVM IR did not store string class-body field")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "call i32 @demo_fields_Counter_total(ptr %counter)"),
          "LLVM IR did not lower method reads of stored class-body fields")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-class-fields";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-class-fields.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("ClassFields.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "class field build-binary failed for a reason other than a "
                  "missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native class field smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "42\n84\n125\nclass field\n",
                "native class field smoke binary did not print expected output");
}

int smokeMutableFieldMvp() {
  constexpr const char* source = R"(package demo.mutable

class MutableCounter(start: Int) {
  var current: Int = start

  def increment: Int = {
    current = current + 1
    current
  }

  def add(amount: Int): Int = {
    this.current = this.current + amount
    current
  }
}

object Main {
  def main = {
    val counter = new MutableCounter(10)
    println(counter.increment)
    println(counter.increment)
    println(counter.add(5))
    counter.current = 100
    println(counter.current)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("MutableFields.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "mutable field MVP build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "field @demo.mutable.MutableCounter.current : Int"),
          "NIR did not emit mutable class var as a stored field")) {
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "eval assign %this.current = (%this.current + 1)") &&
              contains(result.nirText,
                       "eval assign %this.current = (%this.current + %amount)") &&
              contains(result.nirText, "eval assign %counter.current = 100"),
          "NIR did not preserve mutable field assignment expressions")) {
    return code;
  }
  if (int code =
          expect(contains(result.llvmIr, "call ptr @__scalanative_object_alloc(i64 16"),
                 "LLVM IR did not allocate mutable field payload")) {
    return code;
  }
  if (int code = expect(contains(result.llvmIr, "store i32 100"),
                        "LLVM IR did not lower external mutable field store")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr,
                   "call i32 @demo_mutable_MutableCounter_increment(ptr %counter)") &&
              contains(
                  result.llvmIr,
                  "call i32 @demo_mutable_MutableCounter_add(ptr %counter, i32 5)"),
          "LLVM IR did not lower mutable field methods")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-mutable-fields";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-mutable-fields.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "MutableFields.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "mutable field build-binary failed for a reason other than a "
                  "missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code =
          expect(std::system(runCommand.c_str()) == 0,
                 "native mutable field smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "11\n12\n17\n100\n",
                "native mutable field smoke binary did not print expected output");
}

int smokeClassParamFieldModifiersMvp() {
  constexpr const char* source = R"(package demo.paramfields

class ParamCounter(val start: Int, var current: Int) {
  def bump: Int = {
    current = current + start
    current
  }
}

object Main {
  def main = {
    val counter = new ParamCounter(3, 4)
    println(counter.start)
    println(counter.current)
    println(counter.bump)
    counter.current = 10
    println(counter.current)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("ParamFields.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "class parameter field modifier MVP build did not "
                                   "succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code =
          expect(contains(result.nirText,
                          "field @demo.paramfields.ParamCounter.start : Int") &&
                     contains(result.nirText,
                              "field @demo.paramfields.ParamCounter.current : Int"),
                 "NIR did not emit val/var class parameters as stored fields")) {
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "let %counter : demo.paramfields.ParamCounter = new "
                                   "demo.paramfields.ParamCounter(3, 4)") &&
              contains(result.nirText, "eval assign %this.current = "
                                       "(%this.current + %this.start)") &&
              contains(result.nirText, "eval assign %counter.current = 10"),
          "NIR did not preserve mutable class parameter field assignments")) {
    return code;
  }
  if (int code =
          expect(contains(result.llvmIr, "call ptr @__scalanative_object_alloc(i64 16"),
                 "LLVM IR did not allocate val/var parameter fields")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "store i32 10") &&
              contains(result.llvmIr,
                       "call i32 @demo_paramfields_ParamCounter_bump(ptr %counter)"),
          "LLVM IR did not lower mutable parameter field stores and calls")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-param-fields";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-param-fields.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("ParamFields.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "class parameter field build-binary failed for a reason other "
                  "than a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native class parameter field smoke binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(
      readTextFile(outputPath) == "3\n4\n7\n10\n",
      "native class parameter field smoke binary did not print expected output");
}

int smokeInheritanceMetadataMvp() {
  constexpr const char* source = R"(package demo.inherit

trait Named {
  def name: String = "named"
}

trait Labeled extends Named {
  def label: String = "labeled"
}

class BaseCounter {
  def zero: Int = 0
}

class FancyCounter extends BaseCounter with Labeled {
  def one: Int = 1
}

object Main extends Labeled {
  def main = println("inheritance metadata")
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("InheritanceMetadata.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "inheritance metadata MVP build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "trait @demo.inherit.Named : @java.lang.Object") &&
              contains(result.nirText,
                       "trait @demo.inherit.Labeled : @demo.inherit.Named") &&
              contains(result.nirText,
                       "class @demo.inherit.BaseCounter : @java.lang.Object") &&
              contains(result.nirText, "class @demo.inherit.FancyCounter : "
                                       "@demo.inherit.BaseCounter with "
                                       "@demo.inherit.Labeled") &&
              contains(result.nirText,
                       "module @demo.inherit.Main : @demo.inherit.Labeled"),
          "NIR did not preserve resolved class/trait/object parent metadata")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "define ptr @demo_inherit_Named_name") &&
              contains(result.llvmIr, "define void @demo_inherit_Main_main()") &&
              contains(result.llvmIr, "@__type_demo_inherit_Labeled = private constant "
                                      "%scalanative.type_descriptor { i32 3") &&
              contains(result.llvmIr,
                       "@__ancestors_demo_inherit_FancyCounter = private "
                       "constant [3 x ptr] [ptr "
                       "@__type_demo_inherit_Labeled, ptr "
                       "@__type_demo_inherit_Named, ptr "
                       "@__type_demo_inherit_BaseCounter]") &&
              contains(result.llvmIr,
                       "@__ancestors_demo_inherit_Labeled = private constant "
                       "[1 x ptr] [ptr @__type_demo_inherit_Named]") &&
              contains(result.llvmIr, "define internal i1 "
                                      "@__scalanative_is_instance_of(ptr %object, ptr "
                                      "%target)"),
          "LLVM IR did not lower inherited metadata declarations' methods")) {
    return code;
  }

  const std::filesystem::path binaryPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-inheritance-metadata";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-inheritance-metadata.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "InheritanceMetadata.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "inheritance metadata build-binary failed for a reason other "
                  "than a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native inheritance metadata smoke binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(
      readTextFile(outputPath) == "inheritance metadata\n",
      "native inheritance metadata smoke binary did not print expected output");
}

int smokeRuntimeTypeTestsMvp() {
  if (int code =
          expect(scalanative::support::StdNames::JavaLangObject == "java.lang.Object" &&
                     scalanative::support::StdNames::JavaLangClassCastException ==
                         "java.lang.ClassCastException" &&
                     scalanative::support::StdNames::JavaLangArrayStoreException ==
                         "java.lang.ArrayStoreException" &&
                     scalanative::support::StdNames::Constructor == "$init" &&
                     scalanative::support::StdNames::IsInstanceOf == "isInstanceOf" &&
                     scalanative::support::StdNames::AsInstanceOf == "asInstanceOf" &&
                     scalanative::support::StdNames::SizeOf == "sizeof" &&
                     scalanative::support::StdNames::StringLength == "length" &&
                     scalanative::support::StdNames::ToString == "toString" &&
                     scalanative::support::StdNames::Equals == "equals" &&
                     scalanative::support::StdNames::HashCode == "hashCode" &&
                     scalanative::support::StdNames::RuntimePrintln ==
                         "scala.scalanative.runtime.println" &&
                     scalanative::support::StdNames::GcCollect == "gcCollect" &&
                     scalanative::support::StdNames::GcLiveObjectCount ==
                         "gcLiveObjectCount" &&
                     scalanative::support::StdNames::GcCollectionCount ==
                         "gcCollectionCount" &&
                     scalanative::support::StdNames::GcSetCollectionThreshold ==
                         "gcSetCollectionThreshold" &&
                     scalanative::support::StdNames::Zone == "Zone" &&
                     scalanative::support::StdNames::ZoneScoped == "scoped" &&
                     scalanative::support::StdNames::ArrayEmpty == "empty" &&
                     scalanative::support::StdNames::ArrayFill == "fill" &&
                     scalanative::support::StdNames::ArrayRange == "range" &&
                     scalanative::support::StdNames::RuntimeGcCollect ==
                         "scala.scalanative.runtime.gcCollect" &&
                     scalanative::support::StdNames::RuntimeStringLength ==
                         "scala.scalanative.runtime.stringLength" &&
                     scalanative::support::StdNames::RuntimeStringToString ==
                         "scala.scalanative.runtime.stringToString" &&
                     scalanative::support::StdNames::RuntimeStringEquals ==
                         "scala.scalanative.runtime.stringEquals" &&
                     scalanative::support::StdNames::RuntimeArrayLength ==
                         "scala.scalanative.runtime.arrayLength" &&
                     scalanative::support::StdNames::RuntimeArrayApply ==
                         "scala.scalanative.runtime.arrayApply" &&
                     scalanative::support::StdNames::RuntimeArrayUpdate ==
                         "scala.scalanative.runtime.arrayUpdate" &&
                     scalanative::support::StdNames::RuntimeIntArrayLength ==
                         "scala.scalanative.runtime.intArrayLength" &&
                     scalanative::support::StdNames::RuntimeIntArrayApply ==
                         "scala.scalanative.runtime.intArrayApply" &&
                     scalanative::support::StdNames::RuntimeIntArrayUpdate ==
                         "scala.scalanative.runtime.intArrayUpdate" &&
                     scalanative::support::StdNames::RuntimeByteArrayLength ==
                         "scala.scalanative.runtime.byteArrayLength" &&
                     scalanative::support::StdNames::RuntimeByteArrayApply ==
                         "scala.scalanative.runtime.byteArrayApply" &&
                     scalanative::support::StdNames::RuntimeByteArrayUpdate ==
                         "scala.scalanative.runtime.byteArrayUpdate" &&
                     scalanative::support::StdNames::RuntimeShortArrayLength ==
                         "scala.scalanative.runtime.shortArrayLength" &&
                     scalanative::support::StdNames::RuntimeShortArrayApply ==
                         "scala.scalanative.runtime.shortArrayApply" &&
                     scalanative::support::StdNames::RuntimeShortArrayUpdate ==
                         "scala.scalanative.runtime.shortArrayUpdate" &&
                     scalanative::support::StdNames::RuntimeBooleanArrayLength ==
                         "scala.scalanative.runtime.booleanArrayLength" &&
                     scalanative::support::StdNames::RuntimeBooleanArrayApply ==
                         "scala.scalanative.runtime.booleanArrayApply" &&
                     scalanative::support::StdNames::RuntimeBooleanArrayUpdate ==
                         "scala.scalanative.runtime.booleanArrayUpdate" &&
                     scalanative::support::StdNames::RuntimeLongArrayLength ==
                         "scala.scalanative.runtime.longArrayLength" &&
                     scalanative::support::StdNames::RuntimeLongArrayApply ==
                         "scala.scalanative.runtime.longArrayApply" &&
                     scalanative::support::StdNames::RuntimeLongArrayUpdate ==
                         "scala.scalanative.runtime.longArrayUpdate" &&
                     scalanative::support::StdNames::RuntimeDoubleArrayLength ==
                         "scala.scalanative.runtime.doubleArrayLength" &&
                     scalanative::support::StdNames::RuntimeDoubleArrayApply ==
                         "scala.scalanative.runtime.doubleArrayApply" &&
                     scalanative::support::StdNames::RuntimeDoubleArrayUpdate ==
                         "scala.scalanative.runtime.doubleArrayUpdate" &&
                     scalanative::support::StdNames::RuntimeFloatArrayLength ==
                         "scala.scalanative.runtime.floatArrayLength" &&
                     scalanative::support::StdNames::RuntimeFloatArrayApply ==
                         "scala.scalanative.runtime.floatArrayApply" &&
                     scalanative::support::StdNames::RuntimeFloatArrayUpdate ==
                         "scala.scalanative.runtime.floatArrayUpdate" &&
                     scalanative::support::StdNames::RuntimeCharArrayLength ==
                         "scala.scalanative.runtime.charArrayLength" &&
                     scalanative::support::StdNames::RuntimeCharArrayApply ==
                         "scala.scalanative.runtime.charArrayApply" &&
                     scalanative::support::StdNames::RuntimeCharArrayUpdate ==
                         "scala.scalanative.runtime.charArrayUpdate" &&
                     scalanative::support::StdNames::RuntimeReferenceArrayLength ==
                         "scala.scalanative.runtime.referenceArrayLength" &&
                     scalanative::support::StdNames::RuntimeReferenceArrayApply ==
                         "scala.scalanative.runtime.referenceArrayApply" &&
                     scalanative::support::StdNames::RuntimeReferenceArrayUpdate ==
                         "scala.scalanative.runtime.referenceArrayUpdate" &&
                     scalanative::support::StdNames::RuntimeReferenceArrayCopy ==
                         "scala.scalanative.runtime.referenceArrayCopy" &&
                     scalanative::support::StdNames::RuntimeArrayFill ==
                         "scala.scalanative.runtime.arrayFill" &&
                     scalanative::support::StdNames::RuntimeArrayRange ==
                         "scala.scalanative.runtime.arrayRange" &&
                     scalanative::support::StdNames::RuntimeBooleanToString ==
                         "scala.scalanative.runtime.booleanToString" &&
                     scalanative::support::StdNames::RuntimeByteToString ==
                         "scala.scalanative.runtime.byteToString" &&
                     scalanative::support::StdNames::RuntimeShortToString ==
                         "scala.scalanative.runtime.shortToString" &&
                     scalanative::support::StdNames::RuntimeIntToString ==
                         "scala.scalanative.runtime.intToString" &&
                     scalanative::support::StdNames::RuntimeLongToString ==
                         "scala.scalanative.runtime.longToString" &&
                     scalanative::support::StdNames::RuntimeFloatToString ==
                         "scala.scalanative.runtime.floatToString" &&
                     scalanative::support::StdNames::RuntimeDoubleToString ==
                         "scala.scalanative.runtime.doubleToString" &&
                     scalanative::support::StdNames::RuntimeCharToString ==
                         "scala.scalanative.runtime.charToString" &&
                     scalanative::support::StdNames::RuntimeAnyToString ==
                         "scala.scalanative.runtime.anyToString" &&
                     scalanative::support::StdNames::RuntimeAnyReceiverToString ==
                         "scala.scalanative.runtime.anyReceiverToString" &&
                     scalanative::support::StdNames::RuntimeAnyEquals ==
                         "scala.scalanative.runtime.anyEquals" &&
                     scalanative::support::StdNames::RuntimeAnyReceiverEquals ==
                         "scala.scalanative.runtime.anyReceiverEquals" &&
                     scalanative::support::StdNames::RuntimeIntToByte ==
                         "scala.scalanative.runtime.intToByte" &&
                     scalanative::support::StdNames::RuntimeIntToShort ==
                         "scala.scalanative.runtime.intToShort" &&
                     scalanative::support::StdNames::RuntimeByteToInt ==
                         "scala.scalanative.runtime.byteToInt" &&
                     scalanative::support::StdNames::RuntimeShortToInt ==
                         "scala.scalanative.runtime.shortToInt" &&
                     scalanative::support::StdNames::RuntimeByteHashCode ==
                         "scala.scalanative.runtime.byteHashCode" &&
                     scalanative::support::StdNames::RuntimeShortHashCode ==
                         "scala.scalanative.runtime.shortHashCode" &&
                     scalanative::support::StdNames::RuntimeBooleanHashCode ==
                         "scala.scalanative.runtime.booleanHashCode" &&
                     scalanative::support::StdNames::RuntimeLongHashCode ==
                         "scala.scalanative.runtime.longHashCode" &&
                     scalanative::support::StdNames::RuntimeFloatHashCode ==
                         "scala.scalanative.runtime.floatHashCode" &&
                     scalanative::support::StdNames::RuntimeDoubleHashCode ==
                         "scala.scalanative.runtime.doubleHashCode" &&
                     scalanative::support::StdNames::RuntimeCharHashCode ==
                         "scala.scalanative.runtime.charHashCode" &&
                     scalanative::support::StdNames::RuntimeStringHashCode ==
                         "scala.scalanative.runtime.stringHashCode" &&
                     scalanative::support::StdNames::RuntimeSymbolHashCode ==
                         "scala.scalanative.runtime.symbolHashCode" &&
                     scalanative::support::StdNames::RuntimeAnyHashCode ==
                         "scala.scalanative.runtime.anyHashCode" &&
                     scalanative::support::StdNames::RuntimeAnyReceiverHashCode ==
                         "scala.scalanative.runtime.anyReceiverHashCode" &&
                     scalanative::support::StdNames::RuntimeFormat ==
                         "scala.scalanative.runtime.format" &&
                     scalanative::support::StdNames::RuntimeFormatBoolean ==
                         "scala.scalanative.runtime.formatBoolean",
                 "StdNames does not expose stable compiler-known names")) {
    return code;
  }

  constexpr const char* source = R"(package demo.typetest

trait Named {
  def name: String = "named"
}
trait Labeled extends Named {
  def label: String = "labeled"
}
class BaseValue
class FancyValue extends BaseValue with Labeled
class OtherValue
object Main {
  def report(value: BaseValue) = {
    println(value.isInstanceOf[BaseValue])
    println(value.isInstanceOf[FancyValue])
    println(value.isInstanceOf[Named])
    println(value.isInstanceOf[Labeled])
    println(value.isInstanceOf[OtherValue])
    println(value.asInstanceOf[BaseValue].isInstanceOf[FancyValue])
    println(value.asInstanceOf[Labeled].label)
    println(value.asInstanceOf[FancyValue].name)
  }
  def referenceCastFailure(value: BaseValue): String =
    try {
      println(value.asInstanceOf[OtherValue])
      "missed reference cast failure"
    } catch {
      case error: ClassCastException => error.getMessage
    }
  def boxedCastFailure(value: Any): String =
    try {
      println(value.asInstanceOf[Long])
      "missed boxed cast failure"
    } catch {
      case error: ClassCastException => error.getMessage
    }
  def main = {
    val value: BaseValue = new FancyValue()
    report(value)
    val missing: BaseValue = null.asInstanceOf[BaseValue]
    println(missing.isInstanceOf[BaseValue])
    println(referenceCastFailure(value))
    println(boxedCastFailure(7))
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("RuntimeTypeTests.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "runtime type-test build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "is-instance-of[demo.typetest.BaseValue](%value)") &&
              contains(result.nirText,
                       "is-instance-of[demo.typetest.FancyValue](%value)") &&
              contains(result.nirText, "is-instance-of[demo.typetest.Named](%value)") &&
              contains(result.nirText,
                       "is-instance-of[demo.typetest.Labeled](%value)") &&
              contains(result.nirText,
                       "is-instance-of[demo.typetest.OtherValue](%value)") &&
              contains(result.nirText,
                       "as-instance-of[demo.typetest.Labeled](%value).label") &&
              contains(result.nirText,
                       "as-instance-of[demo.typetest.BaseValue](null)") &&
              contains(result.nirText, "class @java.lang.ClassCastException : "
                                       "@java.lang.RuntimeException") &&
              contains(result.nirText, "unbox[Long](%value)"),
          "NIR did not preserve typed runtime tests and casts")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr,
                   "@__ancestors_demo_typetest_FancyValue = private constant [3 x "
                   "ptr] [ptr @__type_demo_typetest_Labeled, ptr "
                   "@__type_demo_typetest_Named, ptr "
                   "@__type_demo_typetest_BaseValue]") &&
              contains(result.llvmIr,
                       "call i1 @__scalanative_is_instance_of(ptr %value, ptr "
                       "@__type_demo_typetest_Named)") &&
              contains(result.llvmIr,
                       "call ptr @__scalanative_as_instance_of(ptr %value, ptr "
                       "@__type_demo_typetest_Labeled)") &&
              contains(result.llvmIr, "@__type_demo_typetest_OtherValue") &&
              contains(result.llvmIr, "@__type_java_lang_ClassCastException =") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_throw_class_cast() "
                       "noreturn") &&
              contains(result.llvmIr,
                       "c\"Value cannot be cast to requested type\\00\"") &&
              countOccurrences(result.llvmIr,
                               "call void @__scalanative_throw_class_cast()") == 10,
          "LLVM did not lower or retain runtime type-test metadata")) {
    return code;
  }

  constexpr const char* invalidSource = R"(package demo.typetest.invalid
class BaseValue
object Main {
  def main = {
    val value: BaseValue = new BaseValue()
    println(value.isInstanceOf[MissingValue])
  }
}
)";
  scalanative::support::DiagnosticEngine invalidDiagnostics;
  scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidRuntimeTypeTest.scala", invalidSource, {}, invalidDiagnostics);
  if (int code =
          expect(!invalid.ok && contains(invalid.diagnosticsText,
                                         "isInstanceOf target must be a known class or "
                                         "trait: MissingValue"),
                 "unknown isInstanceOf target did not produce a focused diagnostic")) {
    return code;
  }

  constexpr const char* invalidCastSource = R"(package demo.typetest.invalidcast
class BaseValue
object Main {
  def main = {
    val value: BaseValue = new BaseValue()
    println(value.asInstanceOf[MissingValue])
  }
}
)";
  scalanative::support::DiagnosticEngine invalidCastDiagnostics;
  scalanative::tools::build::BuildResult invalidCast = driver.buildSource(
      "InvalidRuntimeCast.scala", invalidCastSource, {}, invalidCastDiagnostics);
  if (int code = expect(
          !invalidCast.ok &&
              contains(
                  invalidCast.diagnosticsText,
                  "asInstanceOf target must be a known class or trait: MissingValue"),
          "unknown asInstanceOf target did not produce a focused diagnostic")) {
    return code;
  }

  constexpr const char* primitiveCastSource = R"(package demo.typetest.primitivecast
class Box
object Main {
  def main = {
    val value: Int = 1
    println(value.asInstanceOf[Box])
  }
}
)";
  scalanative::support::DiagnosticEngine primitiveCastDiagnostics;
  scalanative::tools::build::BuildResult primitiveCast = driver.buildSource(
      "PrimitiveRuntimeCast.scala", primitiveCastSource, {}, primitiveCastDiagnostics);
  if (int code = expect(
          !primitiveCast.ok &&
              contains(primitiveCast.diagnosticsText,
                       "asInstanceOf[T] receiver must be a reference value"),
          "primitive asInstanceOf receiver did not produce a focused diagnostic")) {
    return code;
  }

  const std::filesystem::path binaryPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-runtime-type-tests";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-runtime-type-tests.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "RuntimeTypeTests.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "runtime type-test build-binary failed for a reason other than a "
                  "missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native runtime type-test binary did not exit successfully")) {
    return code;
  }
  if (int code = expect(
          readTextFile(outputPath) == "1\n1\n1\n1\n0\n1\nlabeled\nnamed\n0\n"
                                      "Value cannot be cast to requested type\n"
                                      "Value cannot be cast to requested type\n",
          "native runtime type tests or checked casts produced incorrect results")) {
    return code;
  }

  constexpr const char* boxedOnlySource = R"(package demo.typetest.boxedonly
object Main {
  def cast(value: Any): Long = value.asInstanceOf[Long]
  def main = println(cast(7))
}
)";
  scalanative::tools::build::BuildOptions boxedOnlyOptions;
  boxedOnlyOptions.optimize = true;
  scalanative::support::DiagnosticEngine boxedOnlyDiagnostics;
  const scalanative::tools::build::BuildResult boxedOnly = driver.buildSource(
      "BoxedCastOnly.scala", boxedOnlySource, boxedOnlyOptions, boxedOnlyDiagnostics);
  if (int code = expect(
          boxedOnly.ok &&
              contains(boxedOnly.llvmIr, "@__type_java_lang_ClassCastException =") &&
              contains(boxedOnly.llvmIr, "call void @__scalanative_throw_class_cast()"),
          "boxed unboxing did not retain its typed runtime failure")) {
    if (!boxedOnly.ok) {
      std::cerr << boxedOnly.diagnosticsText;
    }
    return code;
  }

  constexpr const char* failingCastSource = R"(package demo.typetest.failure
class BaseValue
class OtherValue
object Main {
  def main = {
    val value: BaseValue = new BaseValue()
    println(value.asInstanceOf[OtherValue].isInstanceOf[OtherValue])
  }
}
)";
  const std::filesystem::path failingBinaryPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-runtime-failing-cast";
  const std::filesystem::path failingOutputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-runtime-failing-cast.out";
  std::filesystem::remove(failingBinaryPath);
  std::filesystem::remove(failingOutputPath);
  scalanative::support::DiagnosticEngine failingDiagnostics;
  binaryOptions.outputPath = failingBinaryPath;
  scalanative::tools::build::BuildResult failingBinary = driver.buildSource(
      "FailingRuntimeCast.scala", failingCastSource, binaryOptions, failingDiagnostics);
  if (!failingBinary.ok) {
    return expect(contains(failingBinary.diagnosticsText, "clang toolchain not found"),
                  "failing runtime-cast fixture did not build: " +
                      failingBinary.diagnosticsText);
  }
  const std::string failingRunCommand =
      failingBinaryPath.string() + " >" + failingOutputPath.string() + " 2>&1";
  const int failingStatus = std::system(failingRunCommand.c_str());
  const std::string failingOutput = readTextFile(failingOutputPath);
  std::filesystem::remove(failingBinaryPath);
  std::filesystem::remove(failingOutputPath);
  return expect(
      failingStatus != 0 &&
          contains(failingBinary.llvmIr, "@__type_java_lang_ClassCastException =") &&
          contains(failingOutput, "Uncaught exception: ClassCastException: "
                                  "Value cannot be cast to requested type\n") &&
          contains(failingOutput,
                   "\tat demo.typetest.failure.Main.main(FailingRuntimeCast.scala:"),
      "incompatible asInstanceOf cast did not report a typed failure");
}

int smokeShortCircuitMvp() {
  constexpr const char* source = R"(package demo.logic

object Main {
  def right(value: Int): Boolean = {
    println(value)
    true
  }

  def main = {
    println(false && right(10))
    println(true || right(20))
    println(true && right(30))
    println(false || right(40))
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("ShortCircuit.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "short-circuit build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(contains(result.nirText, "(false && call %right(10))") &&
                            contains(result.nirText, "(true || call %right(20))") &&
                            contains(result.llvmIr, "logical_rhs_tmp") &&
                            contains(result.llvmIr, "logical_short_tmp") &&
                            contains(result.llvmIr, "phi i1"),
                        "LLVM did not lower short-circuit Boolean operators")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-short-circuit";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-short-circuit.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "ShortCircuit.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "short-circuit binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "short-circuit smoke binary did not exit successfully")) {
    return code;
  }
  if (int code = expect(readTextFile(outputPath) == "0\n1\n30\n1\n40\n1\n",
                        "short-circuit operators evaluated a skipped right operand")) {
    return code;
  }

  constexpr const char* invalidSource = R"(package demo.logic
object Bad {
  def main = println(1 && true)
}
)";
  scalanative::support::DiagnosticEngine invalidDiagnostics;
  scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidShortCircuit.scala", invalidSource, {}, invalidDiagnostics);
  return expect(!invalid.ok &&
                    contains(invalid.diagnosticsText,
                             "logical operator && requires a Boolean left operand"),
                "non-Boolean short-circuit operand did not fail typecheck");
}

int smokeRemainderMvp() {
  constexpr const char* source = R"(package demo.remainder

object Main {
  def intRemainder(value: Int, divisor: Int): Int = value % divisor
  def longRemainder(value: Long, divisor: Long): Long = value % divisor
  def doubleRemainder(value: Double, divisor: Double): Double = value % divisor

  def main = {
    println(intRemainder(17, 5))
    println(intRemainder(0 - 17, 5))
    println(longRemainder(20L, 6L))
    println(doubleRemainder(5.75, 2.0))
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("Remainder.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "remainder build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code =
          expect(contains(result.nirText, "ret Int (%value % %divisor)") &&
                     contains(result.nirText, "ret Long (%value % %divisor)") &&
                     contains(result.nirText, "ret Double (%value % %divisor)") &&
                     contains(result.llvmIr, "srem i32 %value, %divisor") &&
                     contains(result.llvmIr, "srem i64 %value, %divisor") &&
                     contains(result.llvmIr, "frem double %value, %divisor"),
                 "LLVM did not lower integral and floating remainder operators")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-remainder";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-remainder.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("Remainder.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "remainder binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "remainder smoke binary did not exit successfully")) {
    return code;
  }
  if (int code = expect(readTextFile(outputPath) == "2\n-2\n2\n1.750000\n",
                        "native remainder values were incorrect")) {
    return code;
  }

  constexpr const char* invalidSource = R"(package demo.remainder
object Bad {
  def main = println(true % 2)
}
)";
  scalanative::support::DiagnosticEngine invalidDiagnostics;
  scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidRemainder.scala", invalidSource, {}, invalidDiagnostics);
  return expect(!invalid.ok &&
                    contains(invalid.diagnosticsText,
                             "remainder operator % requires numeric operands"),
                "non-numeric remainder operand did not fail typecheck");
}

int smokeUnaryOperatorsMvp() {
  constexpr const char* source = R"(package demo.unary

object Main {
  def negate(value: Int): Int = -value
  def retain(value: Long): Long = +value
  def negateFloat(value: Float): Float = -value
  def retainDouble(value: Double): Double = +value
  def invert(value: Boolean): Boolean = !value

  def main = {
    println(negate(7))
    println(retain(8L))
    println(negateFloat(1.25F))
    println(retainDouble(2.5))
    println(invert(false))
    println(!true)
    println(-(-5))
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("UnaryOperators.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "unary-operator build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(contains(result.nirText, "ret Int (-%value)") &&
                            contains(result.nirText, "ret Long (+%value)") &&
                            contains(result.nirText, "ret Float (-%value)") &&
                            contains(result.nirText, "ret Double (+%value)") &&
                            contains(result.nirText, "ret Boolean (!%value)") &&
                            contains(result.llvmIr, "sub i32 0, %value") &&
                            contains(result.llvmIr, "fneg float %value") &&
                            contains(result.llvmIr, "xor i1 %value, true"),
                        "NIR or LLVM did not lower unary operators")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-unary-operators";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-unary-operators.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "UnaryOperators.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "unary-operator binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "unary-operator smoke binary did not exit successfully")) {
    return code;
  }
  if (int code =
          expect(readTextFile(outputPath) == "-7\n8\n-1.250000\n2.500000\n1\n0\n5\n",
                 "native unary-operator values were incorrect")) {
    return code;
  }

  constexpr const char* invalidSource = R"(package demo.unary
object Bad {
  def main = println(!1)
}
)";
  scalanative::support::DiagnosticEngine invalidDiagnostics;
  scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidUnaryOperator.scala", invalidSource, {}, invalidDiagnostics);
  return expect(!invalid.ok &&
                    contains(invalid.diagnosticsText,
                             "logical negation operator ! requires a Boolean operand"),
                "non-Boolean logical negation did not fail typecheck");
}

int smokeStringConcatenationMvp() {
  constexpr const char* source = R"(package demo.strings

object Main {
  def join(left: String, right: String): String = left + right

  def main = {
    println(join("Scala ", "Native"))
    println("a" + "b" + "c")
    println("value=" + 7)
    println(8 + " items")
    println("long=" + 9L)
    println("ratio=" + 1.5)
    println("flag=" + true)
    println("char=" + 'Z')
    println("none=" + null)
    println("symbol=" + 'value)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("StringConcatenation.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "string-concatenation build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "ret String (%left + %right)") &&
              contains(result.llvmIr,
                       "define internal ptr @__scalanative_string_concat") &&
              contains(result.llvmIr,
                       "define internal ptr @__scalanative_string_from_int") &&
              contains(result.llvmIr,
                       "define internal ptr @__scalanative_string_from_boolean") &&
              contains(result.llvmIr, "call i64 @strlen(ptr %left)") &&
              contains(result.llvmIr, "call ptr @__scalanative_string_from_int") &&
              contains(result.llvmIr, "call ptr @__scalanative_string_concat"),
          "LLVM did not lower String concatenation through the runtime helper")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-string-concat";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-string-concat.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "StringConcatenation.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "string-concatenation binary failed to build: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code =
          expect(std::system(runCommand.c_str()) == 0,
                 "string-concatenation smoke binary did not exit successfully")) {
    return code;
  }
  if (int code = expect(readTextFile(outputPath) ==
                            "Scala Native\nabc\nvalue=7\n8 items\nlong=9\n"
                            "ratio=1.500000\nflag=true\nchar=Z\nnone=null\n"
                            "symbol='value\n",
                        "native String concatenation produced incorrect output")) {
    return code;
  }
  return 0;
}

int smokeStringEqualityMvp() {
  constexpr const char* source = R"(package demo.strings

object Main {
  def join(left: String, right: String): String = left + right

  def main = {
    val dynamic = join("Scala ", "Native")
    println(dynamic == "Scala Native")
    println(dynamic != "Scala")
    println("literal" == "literal")
    println("literal" != "other")
    println("literal" == null)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("StringEquality.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "string-equality build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "define internal i1 @__scalanative_string_equals") &&
              contains(result.llvmIr, "call i32 @strcmp(ptr %left, ptr %right)") &&
              contains(result.llvmIr, "call i1 @__scalanative_string_equals") &&
              contains(result.llvmIr, "xor i1 %tmp"),
          "LLVM did not lower String equality through the runtime helper")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-string-equality";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-string-equality.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "StringEquality.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "string-equality binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "string-equality smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "1\n1\n1\n1\n0\n",
                "native String equality did not compare contents or null safely");
}

int smokeCompilerKnownEqualsMvp() {
  constexpr const char* source = R"(package demo.equals

class EqualityBox(val value: Int)

object Main {
  def join(left: String, right: String): String = left + right

  def main = {
    val box = new EqualityBox(1)
    val same = box
    val other = new EqualityBox(1)
    val missing: EqualityBox = null
    val anyBox: Any = box
    val anySame: Any = same

    println(7.equals(7))
    println(7.equals(8))
    println(true.equals(false))
    println("Scala".equals(join("Sca", "la")))
    println(box.equals(same))
    println(box.equals(other))
    println(missing.equals(null))
    println(anyBox.equals(anySame))
    println(anyBox.equals(other))
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("Equals.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "compiler-known equals build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "(7 == 7)") &&
              contains(result.nirText, "(%box == %same)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.anyEquals(%anyBox, "
                       "%anySame)") &&
              contains(result.nirText, "(%missing == null)") &&
              contains(result.llvmIr, "call i1 @__scalanative_string_equals") &&
              contains(result.llvmIr, "define internal i1 @__scalanative_any_equals") &&
              contains(result.llvmIr, "call i1 @__scalanative_any_equals"),
          "compiler-known equals did not lower through equality NIR and "
          "LLVM comparisons")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-equals";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-equals.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("Equals.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "compiler-known equals binary failed to build: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native compiler-known equals smoke binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "1\n0\n0\n1\n1\n0\n1\n1\n0\n",
                "native compiler-known equals values were incorrect");
}

int smokeAnyEqualsMvp() {
  constexpr const char* source = R"(package demo.anyequals

class EqualityBox(val value: Int)

object Main {
  def main = {
    val seven: Any = 7
    val anotherSeven: Any = 7
    val eight: Any = 8
    val truth: Any = true
    val anotherTruth: Any = true
    val longValue: Any = 4294967296L
    val anotherLongValue: Any = 4294967296L
    val unitValue: Any = {}
    val anotherUnitValue: Any = {}
    val symbolValue: Any = 'ready
    val anotherSymbolValue: Any = 'ready
    val box = new EqualityBox(1)
    val sameBox: Any = box
    val otherBox: Any = new EqualityBox(1)

    println(seven == anotherSeven)
    println(seven == eight)
    println(seven.equals(7))
    println(7 == seven)
    println(truth == anotherTruth)
    println(longValue == anotherLongValue)
    println(unitValue == anotherUnitValue)
    println(symbolValue == anotherSymbolValue)
    println(sameBox == box)
    println(sameBox == otherBox)
    println(sameBox != otherBox)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("AnyEquals.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "Any equals build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "call %scala.scalanative.runtime.anyEquals") &&
              contains(result.nirText, "box[Int](7)") &&
              contains(result.nirText, "box[Unit](unit)") &&
              contains(result.nirText, "box[Symbol](") &&
              contains(result.llvmIr, "define internal i1 @__scalanative_any_equals") &&
              contains(result.llvmIr, "call i1 @__scalanative_any_equals"),
          "Any equals did not lower through boxed runtime equality")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-any-equals";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-any-equals.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("AnyEquals.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "Any equals binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native Any equals smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "1\n0\n1\n1\n1\n1\n1\n1\n1\n0\n1\n",
                "native Any equals values were incorrect");
}

int smokeAnyStringMvp() {
  constexpr const char* source = R"(package demo.anystring

object Main {
  def join(left: String, right: String): String = left + right

  def main = {
    val direct: Any = "Scala"
    val joined: Any = join("Sca", "la")
    val different: Any = "Native"
    val values = Array[Any]("left", join("ri", "ght"))
    val missing: Any = null

    println(direct.toString)
    println(direct == joined)
    println(direct.equals("Scala"))
    println("Scala".equals(direct))
    println(direct == different)
    println(direct.isInstanceOf[String])
    println(direct.asInstanceOf[String])
    println(direct.hashCode == "Scala".hashCode)
    println(values(0).asInstanceOf[String])
    println(values(1) == "right")
    println(missing == direct)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("AnyString.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "Any String build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "let %direct : Object = box[String](\"Scala\")") &&
              contains(result.nirText, "box[String](call %join(\"Sca\", \"la\"))") &&
              contains(result.nirText, "new Array [ Object ](box[String](\"left\"), "
                                       "box[String](call %join(\"ri\", \"ght\")))") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.anyEquals(%direct, "
                       "%joined)") &&
              contains(result.nirText, "unbox[String](%direct)") &&
              contains(result.llvmIr, "ptr @__scalanative_boxed_String") &&
              contains(result.llvmIr,
                       "define internal ptr @__scalanative_unbox_String") &&
              contains(result.llvmIr, "call i1 @__scalanative_any_equals") &&
              contains(result.llvmIr, "call i32 @__scalanative_any_hash_code"),
          "Any String did not lower through boxed String runtime support")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-any-string";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-any-string.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("AnyString.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "Any String binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native Any String smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) ==
                    "Scala\n1\n1\n1\n0\n1\nScala\n1\nleft\n1\n0\n",
                "native Any String values were incorrect");
}

int smokeUnitEqualsHashCodeMvp() {
  constexpr const char* source = R"(package demo.unithash

object Main {
  def main = {
    val unitAny: Any = {}
    val intAny: Any = 7

    println({}.equals({}))
    println({}.equals(7))
    println({}.equals(null))
    println({}.equals(unitAny))
    println({}.equals(intAny))
    println({}.hashCode)
    println(unitAny.hashCode == {}.hashCode)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("UnitEqualsHashCode.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "Unit equals/hashCode build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "let %unitAny : Object = box[Unit](unit)") &&
              contains(result.nirText, "let %intAny : Object = box[Int](7)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.println(true)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.println(false)") &&
              contains(result.nirText, "call %scala.scalanative.runtime.anyEquals") &&
              contains(result.nirText, "call %scala.scalanative.runtime.println(0)") &&
              contains(result.llvmIr, "define internal i1 @__scalanative_any_equals") &&
              contains(result.llvmIr,
                       "define internal i32 @__scalanative_any_hash_code"),
          "Unit equals/hashCode did not lower through constants and Any helpers")) {
    return code;
  }

  const std::filesystem::path binaryPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-unit-equals-hashcode";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-unit-equals-hashcode.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "UnitEqualsHashCode.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "Unit equals/hashCode binary failed to build: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native Unit equals/hashCode smoke binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "1\n0\n0\n1\n0\n0\n1\n",
                "native Unit equals/hashCode values were incorrect");
}

int smokeUnitOperatorEqualityMvp() {
  constexpr const char* source = R"(package demo.unitops

object Main {
  def main = {
    val unitAny: Any = {}
    val intAny: Any = 7

    println({} == {})
    println({} != {})
    println({} == 7)
    println(7 != {})
    println({} == null)
    println(null != {})
    println(unitAny == {})
    println({} != unitAny)
    println(intAny == {})
    println({} != intAny)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("UnitOperators.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "Unit operator equality build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "let %unitAny : Object = box[Unit](unit)") &&
              contains(result.nirText, "let %intAny : Object = box[Int](7)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.println(true)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.println(false)") &&
              contains(result.nirText, "call %scala.scalanative.runtime.anyEquals") &&
              !contains(result.nirText, "(block() == block())") &&
              !contains(result.nirText, "(block() != block())") &&
              !contains(result.nirText, "(block() == 7)") &&
              !contains(result.nirText, "(7 != block())"),
          "Unit ==/!= did not lower through constants and Any helpers")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-unit-operator-equality";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-unit-operator-equality.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "UnitOperators.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "Unit operator equality binary failed to build: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native Unit operator equality smoke binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "1\n0\n0\n1\n0\n1\n1\n0\n0\n1\n",
                "native Unit operator equality values were incorrect");
}

int smokeCompilerKnownHashCodeMvp() {
  constexpr const char* source = R"(package demo.hash

class HashBox(val value: Int)

object Main {
  def join(left: String, right: String): String = left + right

  def main = {
    val box = new HashBox(1)
    val same = box
    val missing: HashBox = null
    val anyInt: Any = 7
    val anySymbol: Any = 'ready
    val anyBox: Any = box

    println(7.hashCode)
    println(true.hashCode())
    println(4294967296L.hashCode)
    println('A'.hashCode)
    println("Scala".hashCode == join("Sca", "la").hashCode)
    println('ready.hashCode == "'ready".hashCode)
    println(missing.hashCode)
    println(box.hashCode == same.hashCode)
    println(anyInt.hashCode)
    println(anySymbol.hashCode == 'ready.hashCode)
    println(anyBox.hashCode == box.hashCode)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("HashCode.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "compiler-known hashCode build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "scala.scalanative.runtime.booleanHashCode") &&
              contains(result.nirText, "scala.scalanative.runtime.longHashCode") &&
              contains(result.nirText, "scala.scalanative.runtime.charHashCode") &&
              contains(result.nirText, "scala.scalanative.runtime.stringHashCode") &&
              contains(result.nirText, "scala.scalanative.runtime.symbolHashCode") &&
              contains(result.nirText, "scala.scalanative.runtime.anyHashCode") &&
              contains(result.llvmIr,
                       "define internal i32 @__scalanative_any_hash_code") &&
              contains(result.llvmIr, "call i32 @__scalanative_string_hash_code"),
          "compiler-known hashCode did not lower through typed hash helpers")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-hashcode";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-hashcode.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("HashCode.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "compiler-known hashCode binary failed to build: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native compiler-known hashCode smoke binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "7\n1231\n1\n65\n1\n1\n0\n1\n7\n1\n1\n",
                "native compiler-known hashCode values were incorrect");
}

int smokeStringLengthMvp() {
  constexpr const char* source = R"(package demo.strings

object Main {
  def join(left: String, right: String): String = left + right

  def main = {
    val dynamic = join("Scala ", "Native")
    println(dynamic.length)
    println("abc".length)
    println("".length)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("StringLength.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "string-length build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code =
          expect(contains(result.nirText,
                          "call %scala.scalanative.runtime.stringLength(%dynamic)") &&
                     contains(result.llvmIr, "call i64 @strlen(ptr %dynamic)") &&
                     contains(result.llvmIr, "trunc i64 %tmp"),
                 "NIR or LLVM did not lower String.length through the runtime call")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-string-length";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-string-length.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "StringLength.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "string-length binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "string-length smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "12\n3\n0\n",
                "native String.length produced incorrect output");
}

int smokePrimitiveToStringMvp() {
  constexpr const char* source = R"(package demo.strings

object Main {
  def main = {
    println(7.toString)
    println(9L.toString())
    println(1.5.toString)
    println(true.toString())
    println('Z'.toString)
    println("Scala".toString())
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("PrimitiveToString.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "primitive-toString build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "scala.scalanative.runtime.intToString") &&
              contains(result.nirText, "scala.scalanative.runtime.booleanToString") &&
              contains(result.llvmIr, "call ptr @__scalanative_string_from_int") &&
              contains(result.llvmIr, "call ptr @__scalanative_string_from_boolean"),
          "primitive toString did not lower through runtime conversion helpers")) {
    return code;
  }

  const std::filesystem::path binaryPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-primitive-tostring";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-primitive-tostring.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "PrimitiveToString.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "primitive-toString binary failed to build: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "primitive-toString smoke binary did not exit successfully")) {
    return code;
  }
  if (int code = expect(readTextFile(outputPath) == "7\n9\n1.500000\ntrue\nZ\nScala\n",
                        "native primitive toString values were incorrect")) {
    return code;
  }
  return 0;
}

int smokeAnyToStringMvp() {
  constexpr const char* source = R"(package demo.strings

class Box(val value: Int)

object Main {
  def main = {
    val intValue: Any = 7
    val booleanValue: Any = true
    val unitValue: Any = {}
    val symbolValue: Any = 'ready
    val nullValue: Any = null
    val objectValue: Any = new Box(3)

    println(intValue.toString)
    println(booleanValue.toString)
    println(unitValue.toString)
    println(symbolValue.toString)
    println(nullValue.toString)
    println(objectValue.toString)
    println(intValue)
    println("symbol=" + symbolValue)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("AnyToString.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "Any toString build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code =
          expect(contains(result.nirText, "scala.scalanative.runtime.anyToString") &&
                     contains(result.nirText,
                              "call %scala.scalanative.runtime.println(%intValue)") &&
                     contains(result.nirText, "(\"symbol=\" + %symbolValue)") &&
                     contains(result.llvmIr,
                              "define internal ptr @__scalanative_any_to_string") &&
                     contains(result.llvmIr, "ptr @.str.unit") &&
                     contains(result.llvmIr, "call ptr @__scalanative_any_to_string"),
                 "Any toString did not lower through the runtime conversion helper")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-any-tostring";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-any-tostring.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("AnyToString.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "Any toString binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native Any toString smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) ==
                    "7\ntrue\n()\n'ready\nnull\ndemo.strings.Box\n7\nsymbol='ready\n",
                "native Any toString values were incorrect");
}

int smokeCustomToStringMvp() {
  constexpr const char* source = R"(package demo.strings

class BasePrintable {
  def toString: String = "base-printable"
}

class FancyPrintable extends BasePrintable {
  override def toString: String = "fancy-printable"
}

object Main {
  def describe(value: BasePrintable): String = value.toString

  def main = {
    val base = new BasePrintable()
    val fancy = new FancyPrintable()
    val asBase: BasePrintable = fancy
    val asAny: Any = fancy

    println(base.toString)
    println(fancy.toString())
    println(describe(fancy))
    println("direct=" + fancy.toString)
    println(asBase)
    println(asAny)
    println("generic=" + asBase)
    println("any=" + asAny)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("CustomToString.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "custom toString build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "demo.strings.BasePrintable.toString") &&
              contains(result.nirText, "demo.strings.FancyPrintable.toString") &&
              contains(result.llvmIr, "call ptr @__scalanative_any_to_string") &&
              contains(result.llvmIr, "call ptr %to_string_function"),
          "custom toString should lower direct calls through selected methods and "
          "generic object formatting through the runtime Any helper")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-custom-tostring";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-custom-tostring.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "CustomToString.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "custom toString binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native custom toString smoke binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) ==
                    "base-printable\nfancy-printable\nfancy-printable\n"
                    "direct=fancy-printable\nfancy-printable\nfancy-printable\n"
                    "generic=fancy-printable\nany=fancy-printable\n",
                "native custom toString values were incorrect");
}

int smokeFormattedInterpolationMvp() {
  constexpr const char* source = R"(package demo.strings

object Main {
  def main = {
    val pi = 3.14159
    val count = 7
    val total = 9876543210L
    val initial = 'N'
    val enabled = true
    val disabled = false
    val project = "Scala Native"
    println(f"pi=$pi%.2f!")
    println(f"next=${pi + 1.0}%.1f")
    println(f"count=$count%04d")
    println(f"total=$total%012d")
    println(f"initial=$initial%c")
    println(f"enabled=$enabled%b")
    println(f"disabled=$disabled%b")
    println(f"project=$project%s")
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("FormattedInterpolation.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "formatted-interpolation build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "scala.scalanative.runtime.format") &&
              contains(result.nirText, "scala.scalanative.runtime.formatBoolean") &&
              contains(result.llvmIr, "@snprintf(ptr %") &&
              contains(result.llvmIr, "i64 512"),
          "f-interpolation did not lower through the format runtime path")) {
    return code;
  }

  scalanative::support::DiagnosticEngine optimizedNirDiagnostics;
  scalanative::tools::build::BuildOptions optimizedNirOptions;
  optimizedNirOptions.action = scalanative::tools::build::BuildAction::EmitNir;
  optimizedNirOptions.optimize = true;
  scalanative::tools::build::BuildResult optimizedNir =
      driver.buildSource("FormattedInterpolation.scala", source, optimizedNirOptions,
                         optimizedNirDiagnostics);
  if (int code =
          expect(optimizedNir.ok,
                 "formatted-interpolation optimized NIR build did not succeed")) {
    std::cerr << optimizedNir.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(optimizedNir.nirText, "println(\"initial=N\")") &&
              contains(optimizedNir.nirText, "println(\"enabled=true\")") &&
              contains(optimizedNir.nirText, "println(\"disabled=false\")") &&
              contains(optimizedNir.nirText, "println(\"project=Scala Native\")") &&
              contains(optimizedNir.nirText,
                       "call %scala.scalanative.runtime.format(\"%.2f\"") &&
              contains(optimizedNir.nirText,
                       "call %scala.scalanative.runtime.format(\"%04lld\"") &&
              !contains(optimizedNir.nirText,
                        "call %scala.scalanative.runtime.formatBoolean") &&
              !contains(optimizedNir.nirText,
                        "call %scala.scalanative.runtime.format(\"%c\"") &&
              !contains(optimizedNir.nirText,
                        "call %scala.scalanative.runtime.format(\"%s\""),
          "optimized NIR did not fold ABI-stable formatted interpolation while "
          "retaining libc-sensitive formats")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-formatted-interpolation";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-formatted-interpolation.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "FormattedInterpolation.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "formatted-interpolation binary failed to build: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code =
          expect(std::system(runCommand.c_str()) == 0,
                 "formatted-interpolation smoke binary did not exit successfully")) {
    return code;
  }
  if (int code =
          expect(readTextFile(outputPath) ==
                     "pi=3.14!\nnext=4.1\ncount=0007\ntotal=009876543210\ninitial=N\n"
                     "enabled=true\ndisabled=false\nproject=Scala Native\n",
                 "native f-interpolation values were incorrect")) {
    return code;
  }

  constexpr const char* invalidStringSource = R"(package demo.strings
object Bad {
  def main = {
    val enabled = true
    println(f"enabled=$enabled%s")
  }
}
)";
  scalanative::support::DiagnosticEngine invalidStringDiagnostics;
  scalanative::tools::build::BuildResult invalidString =
      driver.buildSource("InvalidStringFormattedInterpolation.scala",
                         invalidStringSource, {}, invalidStringDiagnostics);
  if (int code = expect(
          !invalidString.ok &&
              contains(invalidString.diagnosticsText,
                       "f-interpolation %...s specifiers require String values"),
          "invalid String f-interpolation did not produce a focused diagnostic")) {
    return code;
  }

  constexpr const char* invalidBooleanSource = R"(package demo.strings
object Bad {
  def main = {
    val initial = 'N'
    println(f"initial=$initial%b")
  }
}
)";
  scalanative::support::DiagnosticEngine invalidBooleanDiagnostics;
  scalanative::tools::build::BuildResult invalidBoolean =
      driver.buildSource("InvalidBooleanFormattedInterpolation.scala",
                         invalidBooleanSource, {}, invalidBooleanDiagnostics);
  return expect(!invalidBoolean.ok &&
                    contains(invalidBoolean.diagnosticsText,
                             "f-interpolation %...b specifiers require Boolean values"),
                "invalid Boolean f-interpolation did not produce a focused diagnostic");
}

int smokeStringInterpolationMvp() {
  constexpr const char* source = R"(package demo.strings

object Main {
  def greet(name: String): String =
    s"Hello, ${{ val punctuation = "!"; name + punctuation }}"
  def banner(name: String): String =
    s"""Banner: "$name"
ready"""
  def rawLine(name: String): String = raw"raw\n$name"

  def main = {
    val project = "Scala Native"
    val count = 7
    val symbol = 'value
    println(greet(project))
    println(s"${project + " works"}")
    println(s"count: $count")
    println(s"symbol $symbol")
    println(s"symbol ${symbol}")
    println(rawLine(project))
    println(banner(project))
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("StringInterpolation.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "string-interpolation build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code =
          expect(contains(result.nirText, "\"Hello, \"") &&
                     contains(result.nirText, "%name") &&
                     contains(result.llvmIr,
                              "define internal ptr @__scalanative_string_concat") &&
                     contains(result.llvmIr, "call ptr @__scalanative_string_concat"),
                 "s-interpolation did not desugar through String concatenation")) {
    return code;
  }

  const std::filesystem::path binaryPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-string-interpolation";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-string-interpolation.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "StringInterpolation.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "string-interpolation binary failed to build: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code =
          expect(std::system(runCommand.c_str()) == 0,
                 "string-interpolation smoke binary did not exit successfully")) {
    return code;
  }
  if (int code = expect(readTextFile(outputPath) ==
                            "Hello, Scala Native!\nScala Native works\ncount: 7\n"
                            "symbol 'value\nsymbol 'value\n"
                            "raw\\nScala Native\n"
                            "Banner: \"Scala Native\"\nready\n",
                        "native s-interpolation produced incorrect output")) {
    return code;
  }

  constexpr const char* formattedSource = R"(package demo.strings
object Bad {
  def main = {
    val count = 1
    println(f"count $count")
  }
}
)";
  scalanative::support::DiagnosticEngine formattedDiagnostics;
  scalanative::tools::build::BuildResult formatted =
      driver.buildSource("UnsupportedFormattedInterpolation.scala", formattedSource, {},
                         formattedDiagnostics);
  return expect(
      !formatted.ok &&
          contains(formatted.diagnosticsText,
                   "expected a %...f, %...d, %...c, %...b, or %...s specifier after "
                   "f-interpolation hole"),
      "missing f-interpolation specifier did not produce a focused diagnostic");
}

int smokeConditionalMvp() {
  constexpr const char* source = R"(package demo.condition

object Main {
  def choose(flag: Boolean): Int =
    if (flag) {
      println(10)
      1
    } else {
      println(20)
      2
    }

  def main = {
    println(choose(true))
    println(choose(false))
    if (false) println(99)
    if (true) println(30) else println(40)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("Conditionals.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "conditional build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(contains(result.llvmIr, "br i1 %flag, label %if_then_tmp0") &&
                            contains(result.llvmIr, "phi i32") &&
                            contains(result.llvmIr, "if_merge_"),
                        "LLVM did not emit branch-based conditional control flow")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-conditionals";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-conditionals.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "Conditionals.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "conditional binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "conditional smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "10\n1\n20\n2\n30\n",
                "native conditionals evaluated an untaken side-effecting branch");
}

int smokeMatchMvp() {
  constexpr const char* source = R"(package demo.matching

trait Tagged {
  def tag: String = "tagged-member"
}
class MatchBase
class MatchFancy extends MatchBase with Tagged {
  def label: String = "fancy-member"
}
class MatchTagged extends MatchBase with Tagged
class MatchOther extends MatchBase

object Main {
  def number(value: Int): String = value match {
    case 0 => "zero"
    case 1 => "one"
    case _ => "many"
  }

  def word(value: String): Int = value match {
    case "yes" => 1
    case "no" => 0
    case _ => -1
  }

  def sign(value: Int): Int = value match {
    case 0 => 0
    case positive if positive > 0 => 1
    case _ => -1
  }

  def echo(value: String): String = value match {
    case "yes" => "affirmative"
    case other => other
  }

  def preference(value: Int, prefer: Boolean): String = value match {
    case _ if prefer => "preferred"
    case _ => "fallback"
  }

  def band(value: Int): String = value match {
    case 0 | 1 => "low"
    case 2 | 3 if value == 3 => "guarded"
    case _ => "other"
  }

  def classify(value: MatchBase, preferFancy: Boolean): String = value match {
    case _: MatchFancy if preferFancy => "fancy"
    case _: Tagged => "tagged"
    case _ => "other"
  }

  def describe(value: MatchBase, preferFancy: Boolean): String = value match {
    case fancy: MatchFancy if preferFancy => fancy.label
    case tagged: Tagged => tagged.tag
    case _ => "other"
  }

  def grouped(value: MatchBase): String = value match {
    case _: MatchFancy | _: MatchTagged => "grouped"
    case _ => "other"
  }

  def selected: Int = {
    println("selector")
    1
  }

  def main = {
    println(number(0))
    println(number(3))
    println(word("yes"))
    println(word("other"))
    println(sign(8))
    println(sign(-3))
    println(echo("maybe"))
    println(preference(7, true))
    println(preference(7, false))
    println(band(1))
    println(band(2))
    println(band(3))
    println(classify(new MatchFancy(), true))
    println(classify(new MatchFancy(), false))
    println(classify(new MatchOther(), true))
    println(describe(new MatchFancy(), true))
    println(describe(new MatchFancy(), false))
    println(describe(new MatchTagged(), true))
    println(describe(new MatchOther(), true))
    println(grouped(new MatchFancy()))
    println(grouped(new MatchTagged()))
    println(grouped(new MatchOther()))
    println(number(selected))
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("BasicMatch.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "literal-match build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "%_match") && contains(result.llvmIr, "if_merge_") &&
              contains(result.llvmIr, "phi ptr") &&
              contains(result.llvmIr, "phi i32") &&
              contains(result.nirText, "is-instance-of[demo.matching.MatchFancy]") &&
              contains(result.nirText, "is-instance-of[demo.matching.Tagged]") &&
              contains(result.nirText, "as-instance-of[demo.matching.MatchFancy]") &&
              contains(result.nirText, "as-instance-of[demo.matching.Tagged]"),
          "literal match did not desugar into branch-based control flow")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-match";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-match.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("BasicMatch.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "literal-match binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "literal-match smoke binary did not exit successfully")) {
    return code;
  }
  if (int code =
          expect(readTextFile(outputPath) ==
                     "zero\nmany\n1\n-1\n1\n-"
                     "1\nmaybe\npreferred\nfallback\nlow\nother\nguarded\nfancy"
                     "\ntagged\nother\nfancy-member\ntagged-member\ntagged-member\n"
                     "other\ngrouped\ngrouped\nother\nselector\none\n",
                 "native literal match output was incorrect")) {
    return code;
  }

  constexpr const char* missingWildcardSource = R"(package demo.matching
object Bad {
  def name(value: Int): String = value match {
    case 1 => "one"
  }
}
)";
  scalanative::support::DiagnosticEngine missingWildcardDiagnostics;
  scalanative::tools::build::BuildResult missingWildcard =
      driver.buildSource("MissingWildcardMatch.scala", missingWildcardSource, {},
                         missingWildcardDiagnostics);
  if (int code = expect(
          !missingWildcard.ok &&
              contains(missingWildcard.diagnosticsText,
                       "match expression requires a final wildcard or binding case"),
          "missing match catch-all did not produce a focused diagnostic")) {
    return code;
  }

  constexpr const char* bindingPatternSource = R"(package demo.matching
object Bad {
  def name(value: Int): String = value match {
    case bound => "value"
    case _ => "other"
  }
}
)";
  scalanative::support::DiagnosticEngine bindingPatternDiagnostics;
  scalanative::tools::build::BuildResult bindingPattern = driver.buildSource(
      "BindingPatternMatch.scala", bindingPatternSource, {}, bindingPatternDiagnostics);
  if (int code = expect(
          !bindingPattern.ok && contains(bindingPattern.diagnosticsText,
                                         "catch-all match case must be the final case"),
          "non-final binding pattern did not produce a focused diagnostic")) {
    return code;
  }

  constexpr const char* invalidGuardSource = R"(package demo.matching
object Bad {
  def name(value: Int): String = value match {
    case _ if 1 => "one"
    case _ => "other"
  }
}
)";
  scalanative::support::DiagnosticEngine invalidGuardDiagnostics;
  scalanative::tools::build::BuildResult invalidGuard = driver.buildSource(
      "InvalidWildcardGuard.scala", invalidGuardSource, {}, invalidGuardDiagnostics);
  if (int code = expect(
          !invalidGuard.ok && contains(invalidGuard.diagnosticsText,
                                       "if condition requires a Boolean value"),
          "non-Boolean wildcard match guard did not produce a focused diagnostic")) {
    return code;
  }

  constexpr const char* bindingAlternativeSource = R"(package demo.matching
object Bad {
  def name(value: Int): String = value match {
    case bound | 1 => "value"
    case _ => "other"
  }
}
)";
  scalanative::support::DiagnosticEngine bindingAlternativeDiagnostics;
  scalanative::tools::build::BuildResult bindingAlternative =
      driver.buildSource("BindingAlternativeMatch.scala", bindingAlternativeSource, {},
                         bindingAlternativeDiagnostics);
  if (int code =
          expect(!bindingAlternative.ok &&
                     contains(bindingAlternative.diagnosticsText,
                              "match pattern alternatives require literal patterns"),
                 "binding match alternative did not produce a focused diagnostic")) {
    return code;
  }

  constexpr const char* missingTypePatternSource = R"(package demo.matching
class MatchBase
object Bad {
  def name(value: MatchBase): String = value match {
    case missing: MissingMatchType => "missing"
    case _ => "other"
  }
}
)";
  scalanative::support::DiagnosticEngine missingTypePatternDiagnostics;
  scalanative::tools::build::BuildResult missingTypePattern =
      driver.buildSource("MissingTypePattern.scala", missingTypePatternSource, {},
                         missingTypePatternDiagnostics);
  if (int code = expect(!missingTypePattern.ok &&
                            contains(missingTypePattern.diagnosticsText,
                                     "isInstanceOf target must be a known class or "
                                     "trait: MissingMatchType"),
                        "unknown type pattern did not produce a focused diagnostic")) {
    return code;
  }

  constexpr const char* literalTypePatternSource = R"(package demo.matching
class MatchBase
object Bad {
  def name(value: Int): String = value match {
    case 1: MatchBase => "one"
    case _ => "other"
  }
}
)";
  scalanative::support::DiagnosticEngine literalTypePatternDiagnostics;
  scalanative::tools::build::BuildResult literalTypePattern =
      driver.buildSource("LiteralTypePattern.scala", literalTypePatternSource, {},
                         literalTypePatternDiagnostics);
  if (int code =
          expect(!literalTypePattern.ok &&
                     contains(literalTypePattern.diagnosticsText,
                              "type patterns require a wildcard or binding name"),
                 "literal type pattern did not produce a focused diagnostic")) {
    return code;
  }

  constexpr const char* namedTypeAlternativeSource = R"(package demo.matching
class MatchBase
class MatchFancy extends MatchBase
class MatchTagged extends MatchBase
object Bad {
  def name(value: MatchBase): String = value match {
    case named: MatchFancy | _: MatchTagged => "matched"
    case _ => "other"
  }
}
)";
  scalanative::support::DiagnosticEngine namedTypeAlternativeDiagnostics;
  scalanative::tools::build::BuildResult namedTypeAlternative =
      driver.buildSource("NamedTypeAlternative.scala", namedTypeAlternativeSource, {},
                         namedTypeAlternativeDiagnostics);
  return expect(
      !namedTypeAlternative.ok &&
          contains(namedTypeAlternative.diagnosticsText,
                   "match type pattern alternatives require wildcard patterns"),
      "named type pattern alternative did not produce a focused diagnostic");
}

int smokeControlConditionValidation() {
  constexpr const char* source = R"(package demo.control

object Bad {
  def main = {
    if (1) println(10)
    while ("still") println(20)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("InvalidControlConditions.scala", source, {}, diagnostics);
  return expect(
      !result.ok &&
          contains(result.diagnosticsText, "if condition requires a Boolean value") &&
          contains(result.diagnosticsText, "while condition requires a Boolean value"),
      "non-Boolean control-flow conditions did not fail typecheck");
}

int smokeWhileMvp() {
  constexpr const char* source = R"(package demo.loop

object Main {
  def main = {
    var current = 0
    var total = 0
    while (current < 5) {
      total = total + current
      current = current + 1
    }
    while (false) println(99)
    println(total)
    println(current)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("Loops.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "while-loop build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "while((%current < 5), block(assign %total") &&
              contains(result.llvmIr, "while_condition_tmp0:") &&
              contains(result.llvmIr, "while_body_tmp0:") &&
              contains(result.llvmIr, "while_exit_tmp0:") &&
              contains(result.llvmIr, "icmp slt i32"),
          "LLVM did not lower while loops with condition/body/exit blocks")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-loops";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-loops.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("Loops.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "while-loop binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "while-loop smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "10\n5\n",
                "native while loop did not preserve mutable updates or branch guards");
}

int smokeComparisonMvp() {
  constexpr const char* source = R"(package demo.compare

object Main {
  def main = {
    println(3 < 4)
    println(9 == 9)
    println(5L >= 6L)
    println(2.5 < 3.0)
    println('A' < 'B')
    println(null == null)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("Comparisons.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "comparison build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(contains(result.nirText, "(3 < 4)") &&
                            contains(result.nirText, "(5L >= 6L)") &&
                            contains(result.llvmIr, "icmp slt i32 3, 4") &&
                            contains(result.llvmIr, "icmp sge i64 5, 6") &&
                            contains(result.llvmIr, "fcmp olt double") &&
                            contains(result.llvmIr, "icmp ult i32 65, 66") &&
                            contains(result.llvmIr, "icmp eq ptr null, null"),
                        "LLVM did not lower scalar and pointer comparisons")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-comparisons";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-comparisons.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("Comparisons.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "comparison binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "comparison smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "1\n1\n0\n1\n1\n1\n",
                "native comparison values were incorrect");
}

int smokeFloatingArithmeticMvp() {
  constexpr const char* source = R"(package demo.floating

object Main {
  def addFloat(left: Float, right: Float): Float = left + right
  def divideDouble(left: Double, right: Double): Double = left / right

  def main = {
    println(addFloat(1.5F, 2.25F))
    println(divideDouble(5.0, 2.0))
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("FloatingArithmetic.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "floating-arithmetic build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(contains(result.nirText, "ret Float (%left + %right)") &&
                            contains(result.nirText, "ret Double (%left / %right)") &&
                            contains(result.llvmIr, "fadd float %left, %right") &&
                            contains(result.llvmIr, "fdiv double %left, %right"),
                        "LLVM did not lower Float and Double arithmetic")) {
    return code;
  }

  const std::filesystem::path binaryPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-floating-arithmetic";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-floating-arithmetic.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "FloatingArithmetic.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "floating-arithmetic binary failed to build: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "floating-arithmetic smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "3.750000\n2.500000\n",
                "native Float or Double arithmetic produced incorrect output");
}

int smokeSizeOfMvp() {
  constexpr const char* source = R"(package demo.size

class SizePair(val left: Int, val right: Int)

object Main {
  def main = {
    println(sizeof[Unit])
    println(sizeof[Boolean])
    println(sizeof[Int])
    println(sizeof[Long])
    println(sizeof[SizePair])
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("SizeOf.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "sizeof build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(contains(result.nirText, "sizeof[Int]") &&
                            contains(result.nirText, "sizeof[demo.size.SizePair]"),
                        "NIR did not preserve sizeof type applications")) {
    return code;
  }
  if (int code = expect(contains(result.llvmIr, "@__type_demo_size_SizePair") &&
                            contains(result.llvmIr, "i32 16)"),
                        "LLVM did not lower sizeof values to ABI layout constants")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-sizeof";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-sizeof.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("SizeOf.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "sizeof binary failed to build: " + binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "sizeof smoke binary did not exit successfully")) {
    return code;
  }
  if (int code = expect(readTextFile(outputPath) == "0\n1\n4\n8\n16\n",
                        "sizeof smoke binary did not print ABI layout sizes")) {
    return code;
  }

  constexpr const char* traitSource = R"(package demo.size

trait Marker

object Bad {
  def main = println(sizeof[Marker])
}
)";
  scalanative::support::DiagnosticEngine traitDiagnostics;
  scalanative::tools::build::BuildResult traitResult =
      driver.buildSource("BadSizeOf.scala", traitSource, {}, traitDiagnostics);
  return expect(
      !traitResult.ok &&
          contains(traitResult.diagnosticsText,
                   "sizeof[T] requires a primitive or known concrete class: Marker"),
      "sizeof accepted a trait instead of rejecting a non-concrete type");
}

int smokeInheritedMethodMvp() {
  constexpr const char* source = R"(package demo.inherited

class BaseCounter {
  def zero: Int = 0
  def label: String = "base"
}

class FancyCounter extends BaseCounter {
  def one: Int = zero + 1
  def inheritedLabel: String = label
}

object Main {
  def main = {
    val counter = new FancyCounter()
    println(counter.zero)
    println(counter.one)
    println(counter.label)
    println(counter.inheritedLabel)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("InheritedMethods.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "inherited method MVP build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "class @demo.inherited.FancyCounter : "
                                   "@demo.inherited.BaseCounter") &&
              contains(result.nirText, "ret Int (%this.zero + 1)") &&
              contains(result.nirText, "ret String %this.label") &&
              contains(result.nirText, "eval call %scala.scalanative.runtime.println"
                                       "(%counter.zero)") &&
              contains(result.nirText, "eval call %scala.scalanative.runtime.println"
                                       "(%counter.label)"),
          "NIR did not preserve inherited method selections")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr,
                   "call i32 @demo_inherited_BaseCounter_zero(ptr %this)") &&
              contains(result.llvmIr,
                       "call ptr @demo_inherited_BaseCounter_label(ptr %this)") &&
              contains(result.llvmIr,
                       "call i32 @demo_inherited_BaseCounter_zero(ptr %counter)") &&
              contains(result.llvmIr,
                       "call ptr @demo_inherited_BaseCounter_label(ptr %counter)"),
          "LLVM IR did not resolve inherited selections to parent method symbols")) {
    return code;
  }

  const std::filesystem::path binaryPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-inherited-methods";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-inherited-methods.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "InheritedMethods.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "inherited method build-binary failed for a reason other than a "
                  "missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code =
          expect(std::system(runCommand.c_str()) == 0,
                 "native inherited method smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "0\n1\nbase\nbase\n",
                "native inherited method smoke binary did not print expected output");
}

int smokeInheritedFieldLayoutMvp() {
  constexpr const char* source = R"(package demo.inheritedfields

class BaseFields {
  val base: Int = 10
  var current: Int = base + 1
  val peer: BaseFields = null

  def baseValue: Int = base
  def currentValue: Int = current
}

class ChildFields extends BaseFields {
  val child: Int = base + current
  val childPeer: BaseFields = null

  def total: Int = base + current + child
  def bump: Int = {
    current = current + 1
    current
  }
}

object Main {
  def main = {
    val child = new ChildFields()
    println(child.base)
    println(child.currentValue)
    println(child.child)
    println(child.total)
    println(child.bump)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("InheritedFields.scala", source, {}, diagnostics);

  if (int code =
          expect(result.ok, "inherited field layout MVP build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "class @demo.inheritedfields.ChildFields : "
                                   "@demo.inheritedfields.BaseFields") &&
              contains(result.nirText, "ret Int (%this.base + %this.current)") &&
              contains(result.nirText,
                       "eval assign %this.current = (%this.current + 1)") &&
              contains(result.nirText,
                       "eval call %scala.scalanative.runtime.println(%child.base)"),
          "NIR did not preserve inherited field selections")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "call ptr @__scalanative_object_alloc(i64 40, ptr "
                                  "@__type_demo_inheritedfields_ChildFields)") &&
              contains(result.llvmIr,
                       "@__type_demo_inheritedfields_ChildFields = private constant") &&
              contains(result.llvmIr,
                       "@__trace_offsets_demo_inheritedfields_BaseFields = private "
                       "constant [1 x i32] [i32 16]") &&
              contains(result.llvmIr,
                       "@__trace_offsets_demo_inheritedfields_ChildFields = private "
                       "constant [2 x i32] [i32 16, i32 32]") &&
              contains(result.llvmIr,
                       "ptr @__trace_offsets_demo_inheritedfields_ChildFields, i32 "
                       "2, i32 0") &&
              contains(result.llvmIr,
                       "define internal i32 @__scalanative_object_ownership") &&
              contains(result.llvmIr,
                       "define internal i32 @__scalanative_trace_count") &&
              contains(result.llvmIr,
                       "define internal ptr @__scalanative_trace_reference") &&
              contains(result.llvmIr, "getelementptr i8, ptr %this, i64 8") &&
              contains(result.llvmIr, "getelementptr i8, ptr %this, i64 12") &&
              contains(result.llvmIr, "getelementptr i8, ptr %this, i64 16") &&
              contains(result.llvmIr, "getelementptr i8, ptr %this, i64 24") &&
              contains(result.llvmIr, "getelementptr i8, ptr %this, i64 32") &&
              contains(result.llvmIr,
                       "call i32 @demo_inheritedfields_BaseFields_currentValue") &&
              contains(result.llvmIr,
                       "call i32 @demo_inheritedfields_ChildFields_total"),
          "LLVM IR did not lower inherited field layout and access offsets")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-inherited-fields";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-inherited-fields.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "InheritedFields.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "inherited field layout build-binary failed for a reason other "
                  "than a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native inherited field layout smoke binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(
      readTextFile(outputPath) == "10\n11\n21\n42\n12\n",
      "native inherited field layout smoke binary did not print expected output");
}

int smokeParentConstructorArgsMvp() {
  constexpr const char* source = R"(package demo.parentargs

class BaseCounter(val start: Int) {
  val doubled: Int = start + start

  def total: Int = start + doubled
}

class ChildCounter(val seed: Int) extends BaseCounter(seed + 1) {
  val child: Int = start + seed

  def all: Int = total + child
}

object Main {
  def main = {
    val child = new ChildCounter(4)
    println(child.start)
    println(child.doubled)
    println(child.child)
    println(child.total)
    println(child.all)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("ParentConstructorArgs.scala", source, {}, diagnostics);

  if (int code =
          expect(result.ok, "parent constructor args MVP build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "class @demo.parentargs.ChildCounter : "
                                   "@demo.parentargs.BaseCounter") &&
              contains(result.nirText, "eval assign %super.start = (%this.seed + 1)") &&
              contains(result.nirText, "eval assign %this.doubled = "
                                       "(%this.start + %this.start)") &&
              contains(result.nirText,
                       "eval assign %this.child = (%this.start + %this.seed)"),
          "NIR did not replay parent constructor args before inherited fields")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "call ptr @__scalanative_object_alloc(i64 24, ptr "
                                  "@__type_demo_parentargs_ChildCounter)") &&
              contains(result.llvmIr,
                       "@__type_demo_parentargs_ChildCounter = private constant") &&
              contains(result.llvmIr, "getelementptr i8, ptr %this, i64 8") &&
              contains(result.llvmIr, "getelementptr i8, ptr %this, i64 12") &&
              contains(result.llvmIr, "getelementptr i8, ptr %this, i64 16") &&
              contains(result.llvmIr, "getelementptr i8, ptr %this, i64 20") &&
              contains(result.llvmIr, "call void @demo_parentargs_ChildCounter__init"),
          "LLVM IR did not lower parent constructor args and inherited layout")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-parent-constructor-args";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-parent-constructor-args.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "ParentConstructorArgs.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "parent constructor args build-binary failed for a reason other "
                  "than a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native parent constructor args smoke binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(
      readTextFile(outputPath) == "5\n10\n9\n15\n24\n",
      "native parent constructor args smoke binary did not print expected output");
}

int smokeSuperCallsMvp() {
  constexpr const char* source = R"(package demo.supercalls

class BaseScore(val seed: Int) {
  def value: Int = seed + 1
  def add(extra: Int): Int = value + extra
}

class FancyScore(seed: Int) extends BaseScore(seed) {
  override def value: Int = super.value + 10
  override def add(extra: Int): Int = super.add(extra) + value
}

object Main {
  def main = {
    val score = new FancyScore(4)
    println(score.value)
    println(score.add(2))
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("SuperCalls.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "super calls MVP build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code =
          expect(contains(result.nirText, "eval assign %super.seed = %this.seed") &&
                     contains(result.nirText, "ret Int (%super.value + 10)") &&
                     contains(result.nirText,
                              "ret Int (call %super.add(%extra) + %this.value)"),
                 "NIR did not preserve explicit super selections")) {
    return code;
  }
  if (int code =
          expect(contains(result.llvmIr,
                          "call i32 @demo_supercalls_BaseScore_value(ptr %this)") &&
                     contains(result.llvmIr,
                              "call i32 @demo_supercalls_BaseScore_add(ptr %this") &&
                     contains(result.llvmIr,
                              "call i32 @demo_supercalls_FancyScore_value(ptr %this)"),
                 "LLVM IR did not lower super calls to parent method symbols")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-super-calls";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-super-calls.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("SuperCalls.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "super calls build-binary failed for a reason other than a "
                  "missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native super calls smoke binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "15\n32\n",
                "native super calls smoke binary did not print expected output");
}

int smokeStaticOverrideMvp() {
  constexpr const char* source = R"(package demo.overrides

class BaseScore {
  def value: Int = 1
  def label: String = "base"
}

class FancyScore extends BaseScore {
  override def value: Int = 5
  override def label: String = "fancy"
  def doubled: Int = value + value
}

object Main {
  def main = {
    val score = new FancyScore()
    println(score.value)
    println(score.doubled)
    println(score.label)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("Overrides.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "static override MVP build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "class @demo.overrides.FancyScore : "
                                   "@demo.overrides.BaseScore") &&
              contains(result.nirText, "ret Int (%this.value + %this.value)") &&
              contains(result.nirText, "eval call %scala.scalanative.runtime.println"
                                       "(%score.value)") &&
              contains(result.nirText,
                       "ret Unit call %scala.scalanative.runtime.println"
                       "(%score.label)"),
          "NIR did not preserve static override selections")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr,
                   "call i32 @demo_overrides_FancyScore_value(ptr %this)") &&
              contains(result.llvmIr,
                       "call i32 @demo_overrides_FancyScore_value(ptr %score)") &&
              contains(result.llvmIr,
                       "call ptr @demo_overrides_FancyScore_label(ptr %score)"),
          "LLVM IR did not resolve override selections to child method symbols")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-static-overrides";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-static-overrides.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("Overrides.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "static override build-binary failed for a reason other than a "
                  "missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code =
          expect(std::system(runCommand.c_str()) == 0,
                 "native static override smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "5\n10\nfancy\n",
                "native static override smoke binary did not print expected output");
}

int smokeVirtualDispatchMvp() {
  constexpr const char* source = R"(package demo.virtual

class BaseScore {
  def value: Int = 1
  def label: String = "base"
}

class FancyScore extends BaseScore {
  override def value: Int = 7
  override def label: String = "fancy"
}

object Main {
  def read(score: BaseScore): Int = score.value
  def describe(score: BaseScore): String = score.label

  def main = {
    println(read(new BaseScore()))
    println(read(new FancyScore()))
    println(describe(new FancyScore()))
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("VirtualDispatch.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "virtual dispatch MVP build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(contains(result.nirText, "define @demo.virtual.Main.read : "
                                                 "(demo.virtual.BaseScore)Int") &&
                            contains(result.nirText, "ret Int %score.value") &&
                            contains(result.nirText, "ret String %score.label"),
                        "NIR did not preserve base-typed receiver selections")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "@__vtable_demo_virtual_BaseScore") &&
              contains(result.llvmIr, "@__vtable_demo_virtual_FancyScore") &&
              contains(result.llvmIr, "call ptr @__scalanative_object_alloc(i64 8, ptr "
                                      "@__type_demo_virtual_FancyScore)") &&
              contains(result.llvmIr, "getelementptr %scalanative.type_descriptor") &&
              contains(result.llvmIr, "getelementptr [2 x ptr]") &&
              contains(result.llvmIr, "call i32 %") &&
              contains(result.llvmIr, "call ptr %"),
          "LLVM IR did not lower base-typed selections through vtable slots")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-virtual-dispatch";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-virtual-dispatch.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "VirtualDispatch.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "virtual dispatch build-binary failed for a reason other than a "
                  "missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code =
          expect(std::system(runCommand.c_str()) == 0,
                 "native virtual dispatch smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "1\n7\nfancy\n",
                "native virtual dispatch smoke binary did not print expected output");
}

int smokeTraitDispatchMvp() {
  constexpr const char* source = R"(package demo.traitdispatch

trait Named {
  def name: String = "trait"
  def score: Int = 1
}

class Plain extends Named

class Fancy extends Named {
  override def name: String = "fancy"
  override def score: Int = 9
}

object Main {
  def show(named: Named) = {
    println(named.name)
    println(named.score)
  }

  def main = {
    show(new Plain())
    show(new Fancy())
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("TraitDispatch.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "trait dispatch MVP build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "define @demo.traitdispatch.Main.show : "
                                   "(demo.traitdispatch.Named)Unit") &&
              contains(result.nirText, "eval call %scala.scalanative.runtime.println"
                                       "(%named.name)") &&
              contains(result.nirText,
                       "ret Unit call %scala.scalanative.runtime.println"
                       "(%named.score)"),
          "NIR did not preserve trait-typed receiver selections")) {
    return code;
  }
  if (int code = expect(
          !contains(result.llvmIr, "@__vtable_demo_traitdispatch_Named") &&
              contains(result.llvmIr, "@__vtable_demo_traitdispatch_Plain") &&
              contains(result.llvmIr, "@__vtable_demo_traitdispatch_Fancy") &&
              contains(result.llvmIr, "call ptr @__scalanative_object_alloc(i64 8, ptr "
                                      "@__type_demo_traitdispatch_Plain)") &&
              contains(result.llvmIr, "call ptr @__scalanative_object_alloc(i64 8, ptr "
                                      "@__type_demo_traitdispatch_Fancy)") &&
              contains(result.llvmIr, "getelementptr [2 x ptr]") &&
              contains(result.llvmIr, "call i32 %") &&
              contains(result.llvmIr, "call ptr %"),
          "LLVM IR did not lower trait-typed selections through vtable slots")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-trait-dispatch";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-trait-dispatch.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "TraitDispatch.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "trait dispatch build-binary failed for a reason other than a "
                  "missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code =
          expect(std::system(runCommand.c_str()) == 0,
                 "native trait dispatch smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "trait\n1\nfancy\n9\n",
                "native trait dispatch smoke binary did not print expected output");
}

int smokeAbstractTraitMvp() {
  constexpr const char* source = R"(package demo.abstracttrait

trait Metric {
  def label: String
  def score: Int
  def describe: String = label
}

class BasicMetric extends Metric {
  override def label: String = "basic"
  override def score: Int = 2
}

class FancyMetric extends Metric {
  override def label: String = "fancy"
  override def score: Int = 9
}

object Main {
  def show(metric: Metric) = {
    println(metric.describe)
    println(metric.score)
  }

  def main = {
    show(new BasicMetric())
    show(new FancyMetric())
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("AbstractTrait.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "abstract trait MVP build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "declare @demo.abstracttrait.Metric.label : "
                                   "(demo.abstracttrait.Metric)String") &&
              contains(result.nirText, "declare @demo.abstracttrait.Metric.score : "
                                       "(demo.abstracttrait.Metric)Int") &&
              contains(result.nirText, "define @demo.abstracttrait.Metric.describe : "
                                       "(demo.abstracttrait.Metric)String") &&
              contains(result.nirText, "ret String %this.label"),
          "NIR did not preserve abstract trait declarations and default method")) {
    return code;
  }
  if (int code = expect(
          !contains(result.llvmIr, "@__vtable_demo_abstracttrait_Metric") &&
              contains(result.llvmIr, "@__vtable_demo_abstracttrait_BasicMetric") &&
              contains(result.llvmIr, "@__vtable_demo_abstracttrait_FancyMetric") &&
              contains(result.llvmIr, "getelementptr [3 x ptr]") &&
              contains(result.llvmIr, "call ptr %") &&
              contains(result.llvmIr, "call i32 %"),
          "LLVM IR did not build complete concrete vtables for abstract trait")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-abstract-trait";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-abstract-trait.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "AbstractTrait.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "abstract trait build-binary failed for a reason other than a "
                  "missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native abstract trait smoke binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "basic\n2\nfancy\n9\n",
                "native abstract trait smoke binary did not print expected output");
}

int smokeAbstractTypeMembersMvp() {
  constexpr const char* source = R"(package demo.abstracttypes

trait Carrier {
  type Element
  def label: String = "carrier"
}

trait IntCarrier extends Carrier {
  override type Element = Int
  override def label: String = "int"
}

class DirectCarrier extends Carrier {
  type Element = String
  override def label: String = "string"
}

class InheritedCarrier extends IntCarrier

object Main {
  def show(carrier: Carrier) =
    println(carrier.label)

  def main = {
    show(new DirectCarrier())
    show(new InheritedCarrier())
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("AbstractTypeMembers.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "abstract type member build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText,
                   "type @demo.abstracttypes.Carrier.Element : abstract") &&
              contains(result.nirText,
                       "type @demo.abstracttypes.IntCarrier.Element : Int") &&
              contains(result.nirText,
                       "type @demo.abstracttypes.DirectCarrier.Element : String") &&
              !contains(result.nirText,
                        "type @demo.abstracttypes.InheritedCarrier.Element"),
          "NIR did not preserve abstract, concrete, and inherited type-member "
          "metadata")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "ptr @demo_abstracttypes_DirectCarrier_label") &&
              contains(result.llvmIr, "ptr @demo_abstracttypes_IntCarrier_label"),
          "LLVM IR lost runtime dispatch beside type-member metadata")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-abstract-type-members";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-abstract-type-members.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "AbstractTypeMembers.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "abstract type member build-binary failed for a reason other "
                  "than a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native abstract type member binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "string\nint\n",
                "native abstract type member binary did not print expected "
                "output");
}

int smokeAbstractTypeSignaturesMvp() {
  constexpr const char* source = R"(package demo.typesignatures

trait Codec {
  type Value
  val seed: Value
  def default: Value
  def echo(value: Value): Value
}

trait IntCodec extends Codec {
  override type Value = Int
  override val seed: Value = 5
  override def default: Value = 7
  override def echo(value: Value): Value = value
}

class ConcreteCodec extends IntCodec

object Main {
  def show(codec: IntCodec) = {
    println(codec.seed)
    println(codec.default)
    println(codec.echo(9))
  }

  def main =
    show(new ConcreteCodec())
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("AbstractTypeSignatures.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "abstract type signature build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "declare @demo.typesignatures.Codec.echo : "
                                   "(demo.typesignatures.Codec,Object)Object") &&
              contains(result.nirText, "define @demo.typesignatures.IntCodec.echo : "
                                       "(demo.typesignatures.IntCodec,Object)Object") &&
              contains(result.nirText, "define @demo.typesignatures.IntCodec.default : "
                                       "(demo.typesignatures.IntCodec)Object") &&
              contains(result.nirText,
                       "field @demo.typesignatures.ConcreteCodec."
                       "seed$trait$demo$typesignatures$IntCodec$field : Object") &&
              contains(result.nirText, "param %value$boxed : Object") &&
              contains(result.nirText, "let %value : Int = unbox[Int](%value$boxed)") &&
              contains(result.nirText, "ret Object box[Int](%value)") &&
              contains(result.nirText, "call %codec.echo(box[Int](9))"),
          "NIR did not preserve the erased dependent ABI with explicit Int "
          "boxing")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "ptr @demo_typesignatures_IntCodec_echo") &&
              contains(result.llvmIr, "ptr @demo_typesignatures_ConcreteCodec_seed"),
          "LLVM IR did not retain specialized methods and value accessors")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-abstract-type-signatures";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-abstract-type-signatures.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "AbstractTypeSignatures.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "abstract type signature build-binary failed for a reason "
                  "other than a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native abstract type signature binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "5\n7\n9\n",
                "native abstract type signatures did not preserve the concrete "
                "ABI");
}

int smokeBoxedDependentScalarsMvp() {
  using scalanative::runtime::BoxedPrimitiveKind;
  const scalanative::runtime::BoxedPrimitiveDescriptor unitDescriptor =
      scalanative::runtime::boxedPrimitiveDescriptor(BoxedPrimitiveKind::Unit);
  const scalanative::runtime::BoxedPrimitiveDescriptor booleanDescriptor =
      scalanative::runtime::boxedPrimitiveDescriptor(BoxedPrimitiveKind::Boolean);
  const scalanative::runtime::BoxedPrimitiveDescriptor byteDescriptor =
      scalanative::runtime::boxedPrimitiveDescriptor(BoxedPrimitiveKind::Byte);
  const scalanative::runtime::BoxedPrimitiveDescriptor shortDescriptor =
      scalanative::runtime::boxedPrimitiveDescriptor(BoxedPrimitiveKind::Short);
  const scalanative::runtime::BoxedPrimitiveDescriptor longDescriptor =
      scalanative::runtime::boxedPrimitiveDescriptor(BoxedPrimitiveKind::Long);
  const scalanative::runtime::BoxedPrimitiveDescriptor symbolDescriptor =
      scalanative::runtime::boxedPrimitiveDescriptor(BoxedPrimitiveKind::Symbol);
  const scalanative::runtime::BoxedPrimitiveDescriptor stringDescriptor =
      scalanative::runtime::boxedPrimitiveDescriptor(BoxedPrimitiveKind::String);
  const scalanative::runtime::RuntimeTypeLayout unitLayout =
      scalanative::runtime::boxedPrimitiveTypeLayout(BoxedPrimitiveKind::Unit);
  const scalanative::runtime::RuntimeTypeLayout byteLayout =
      scalanative::runtime::boxedPrimitiveTypeLayout(BoxedPrimitiveKind::Byte);
  const scalanative::runtime::RuntimeTypeLayout shortLayout =
      scalanative::runtime::boxedPrimitiveTypeLayout(BoxedPrimitiveKind::Short);
  const scalanative::runtime::RuntimeTypeLayout longLayout =
      scalanative::runtime::boxedPrimitiveTypeLayout(BoxedPrimitiveKind::Long);
  const scalanative::runtime::RuntimeTypeLayout stringLayout =
      scalanative::runtime::boxedPrimitiveTypeLayout(BoxedPrimitiveKind::String);
  if (int code = expect(
          scalanative::runtime::boxedPrimitiveKind("Unit") ==
                  BoxedPrimitiveKind::Unit &&
              scalanative::runtime::boxedPrimitiveKind("Boolean") ==
                  BoxedPrimitiveKind::Boolean &&
              scalanative::runtime::boxedPrimitiveKind("Byte") ==
                  BoxedPrimitiveKind::Byte &&
              scalanative::runtime::boxedPrimitiveKind("Short") ==
                  BoxedPrimitiveKind::Short &&
              scalanative::runtime::boxedPrimitiveKind("Long") ==
                  BoxedPrimitiveKind::Long &&
              scalanative::runtime::boxedPrimitiveKind("Symbol") ==
                  BoxedPrimitiveKind::Symbol &&
              scalanative::runtime::boxedPrimitiveKind("String") ==
                  BoxedPrimitiveKind::String &&
              unitDescriptor.kind == 7 && unitDescriptor.payloadSize == 0 &&
              unitDescriptor.payloadAlignment == 1 && booleanDescriptor.kind == 1 &&
              booleanDescriptor.payloadSize == 1 &&
              booleanDescriptor.payloadAlignment == 1 && byteDescriptor.kind == 10 &&
              byteDescriptor.payloadSize == 1 && byteDescriptor.payloadAlignment == 1 &&
              shortDescriptor.kind == 11 && shortDescriptor.payloadSize == 2 &&
              shortDescriptor.payloadAlignment == 2 && longDescriptor.kind == 3 &&
              longDescriptor.payloadSize == 8 && longDescriptor.payloadAlignment == 8 &&
              symbolDescriptor.kind == 8 && symbolDescriptor.payloadSize == 8 &&
              symbolDescriptor.payloadAlignment == 8 && stringDescriptor.kind == 9 &&
              stringDescriptor.payloadSize == 8 &&
              stringDescriptor.payloadAlignment == 8 &&
              unitLayout.kind ==
                  scalanative::runtime::RuntimeTypeKind::BoxedPrimitive &&
              unitLayout.typeId == 7 && unitLayout.instanceSize == 8 &&
              unitLayout.instanceAlignment == 8 && unitLayout.payloadOffset == 8 &&
              unitLayout.payloadSize == 0 && unitLayout.payloadAlignment == 1 &&
              byteLayout.typeId == 10 && byteLayout.instanceSize == 9 &&
              byteLayout.instanceAlignment == 8 && byteLayout.payloadOffset == 8 &&
              byteLayout.payloadSize == 1 && byteLayout.payloadAlignment == 1 &&
              shortLayout.typeId == 11 && shortLayout.instanceSize == 10 &&
              shortLayout.instanceAlignment == 8 && shortLayout.payloadOffset == 8 &&
              shortLayout.payloadSize == 2 && shortLayout.payloadAlignment == 2 &&
              longLayout.kind ==
                  scalanative::runtime::RuntimeTypeKind::BoxedPrimitive &&
              longLayout.typeId == 3 && longLayout.instanceSize == 16 &&
              longLayout.instanceAlignment == 8 && longLayout.payloadOffset == 8 &&
              longLayout.payloadSize == 8 && longLayout.payloadAlignment == 8 &&
              stringLayout.kind ==
                  scalanative::runtime::RuntimeTypeKind::BoxedPrimitive &&
              stringLayout.typeId == 9 && stringLayout.instanceSize == 16 &&
              stringLayout.instanceAlignment == 8 && stringLayout.payloadOffset == 8 &&
              stringLayout.payloadSize == 8 && stringLayout.payloadAlignment == 8 &&
              std::string_view(scalanative::runtime::runtimeTypeKindName(
                  longLayout.kind)) == "boxed-primitive" &&
              std::string_view(scalanative::runtime::runtimeTypeKindName(
                  scalanative::runtime::RuntimeTypeKind::Trait)) == "trait" &&
              std::string_view(scalanative::runtime::runtimeTypeKindName(
                  scalanative::runtime::RuntimeTypeKind::Module)) == "module" &&
              scalanative::runtime::objectOwnershipTag(
                  scalanative::runtime::ObjectOwnership::Gc) == 0 &&
              scalanative::runtime::objectOwnershipTag(
                  scalanative::runtime::ObjectOwnership::Arena) == 1 &&
              std::string_view(scalanative::runtime::objectOwnershipName(
                  scalanative::runtime::ObjectOwnership::Immortal)) == "immortal" &&
              scalanative::runtime::runtimeAbiName() == "cpp-scalanative-runtime-52",
          "boxed primitive runtime metadata is not stable")) {
    return code;
  }

  constexpr const char* source = R"(package demo.boxedscalars

trait ScalarBox {
  type Item
  def value: Item
  def echo(value: Item): Item
}
class BooleanBox extends ScalarBox {
  override type Item = Boolean
  override def value: Item = true
  override def echo(value: Item): Item = value
}
class LongBox extends ScalarBox {
  override type Item = Long
  override def value: Item = 42L
  override def echo(value: Item): Item = value
}
class FloatBox extends ScalarBox {
  override type Item = Float
  override def value: Item = 1.5F
  override def echo(value: Item): Item = value
}
class DoubleBox extends ScalarBox {
  override type Item = Double
  override def value: Item = 2.25
  override def echo(value: Item): Item = value
}
class CharBox extends ScalarBox {
  override type Item = Char
  override def value: Item = 'Z'
  override def echo(value: Item): Item = value
}
object Main {
  def main = {
    val booleanBox: BooleanBox = new BooleanBox()
    val longBox: LongBox = new LongBox()
    val floatBox: FloatBox = new FloatBox()
    val doubleBox: DoubleBox = new DoubleBox()
    val charBox: CharBox = new CharBox()
    val forwarded = longBox
    println(booleanBox.echo(booleanBox.value))
    println(longBox.echo(longBox.value))
    println(forwarded.echo(forwarded.value))
    println(floatBox.echo(floatBox.value))
    println(doubleBox.echo(doubleBox.value))
    println(charBox.echo(charBox.value))
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("BoxedDependentScalars.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "boxed dependent scalar build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "box[Boolean](true)") &&
              contains(result.nirText, "unbox[Boolean](%value$boxed)") &&
              contains(result.nirText, "box[Long](42L)") &&
              contains(result.nirText, "unbox[Long](%value$boxed)") &&
              contains(result.nirText,
                       "let %forwarded : demo.boxedscalars.LongBox = %longBox") &&
              contains(
                  result.nirText,
                  "call %forwarded.echo(box[Long](unbox[Long](%forwarded.value)))") &&
              contains(result.nirText, "box[Float](1.5F)") &&
              contains(result.nirText, "unbox[Float](%value$boxed)") &&
              contains(result.nirText, "box[Double](2.25)") &&
              contains(result.nirText, "unbox[Double](%value$boxed)") &&
              contains(result.nirText, "box[Char]('Z')") &&
              contains(result.nirText, "unbox[Char](%value$boxed)"),
          "NIR did not emit scalar-specific dependent boxing operations")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "Runtime ABI = 'cpp-scalanative-runtime-52'") &&
              contains(result.llvmIr,
                       "@__scalanative_boxed_Unit = private constant "
                       "%scalanative.type_descriptor { i32 2, i32 7, i64 8, i32 8, "
                       "i32 8, i32 0, i32 1") &&
              contains(result.llvmIr,
                       "@__scalanative_boxed_Boolean = private constant "
                       "%scalanative.type_descriptor { i32 2, i32 1, i64 9, i32 8, "
                       "i32 8, i32 1, i32 1") &&
              contains(result.llvmIr,
                       "@__scalanative_boxed_Int = private constant "
                       "%scalanative.type_descriptor { i32 2, i32 2, i64 12, i32 8, "
                       "i32 8, i32 4, i32 4") &&
              contains(result.llvmIr,
                       "@__scalanative_boxed_Long = private constant "
                       "%scalanative.type_descriptor { i32 2, i32 3, i64 16, i32 8, "
                       "i32 8, i32 8, i32 8") &&
              contains(result.llvmIr,
                       "@__scalanative_boxed_Float = private constant "
                       "%scalanative.type_descriptor { i32 2, i32 4, i64 12, i32 8, "
                       "i32 8, i32 4, i32 4") &&
              contains(result.llvmIr,
                       "@__scalanative_boxed_Double = private constant "
                       "%scalanative.type_descriptor { i32 2, i32 5, i64 16, i32 8, "
                       "i32 8, i32 8, i32 8") &&
              contains(result.llvmIr,
                       "@__scalanative_boxed_Char = private constant "
                       "%scalanative.type_descriptor { i32 2, i32 6, i64 12, i32 8, "
                       "i32 8, i32 4, i32 4") &&
              contains(result.llvmIr,
                       "@__scalanative_boxed_Symbol = private constant "
                       "%scalanative.type_descriptor { i32 2, i32 8, i64 16, i32 8, "
                       "i32 8, i32 8, i32 8") &&
              contains(result.llvmIr,
                       "@__scalanative_boxed_String = private constant "
                       "%scalanative.type_descriptor { i32 2, i32 9, i64 16, i32 8, "
                       "i32 8, i32 8, i32 8") &&
              contains(result.llvmIr, "call ptr @__scalanative_box_alloc(i64 9, ptr "
                                      "@__scalanative_boxed_Boolean)") &&
              contains(result.llvmIr, "call ptr @__scalanative_box_alloc(i64 16, ptr "
                                      "@__scalanative_boxed_Long)") &&
              contains(result.llvmIr,
                       "define internal ptr @__scalanative_arena_alloc") &&
              contains(result.llvmIr, "define internal ptr @__scalanative_box_alloc") &&
              contains(result.llvmIr,
                       "call void @__scalanative_program_arena_destroy()") &&
              contains(result.llvmIr, "call i64 @__scalanative_unbox_Long(ptr %") &&
              contains(result.llvmIr,
                       "call i1 @__scalanative_is_instance_of(ptr %object, ptr "
                       "@__scalanative_boxed_Long)") &&
              contains(result.llvmIr,
                       "define internal ptr @__scalanative_unbox_String") &&
              contains(result.llvmIr, "call void @llvm.trap()"),
          "LLVM did not emit checked boxed primitive runtime metadata")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-boxed-dependent-scalars";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-boxed-dependent-scalars.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "BoxedDependentScalars.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "boxed dependent scalar build-binary failed for a reason other "
                  "than a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native boxed dependent scalar binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "1\n42\n42\n1.500000\n2.250000\nZ\n",
                "native boxed dependent scalars did not preserve their payloads");
}

int smokeBoundedTypeMembersMvp() {
  constexpr const char* source = R"(package demo.boundedtypes

class BaseItem {
  def label: String = "base"
}

class SpecialItem extends BaseItem {
  override def label: String = "special"
}

trait ItemCodec {
  type Item <: BaseItem
  def describe(item: Item): String
}

trait SpecialCodec extends ItemCodec {
  override type Item = SpecialItem
  override def describe(item: Item): String = item.label
}

class ConcreteCodec extends SpecialCodec

object Main {
  def show(codec: SpecialCodec, item: SpecialItem) =
    println(codec.describe(item))

  def main =
    show(new ConcreteCodec(), new SpecialItem())
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("BoundedTypeMembers.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "bounded type member build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText,
                   "type @demo.boundedtypes.ItemCodec.Item : abstract <: "
                   "demo.boundedtypes.BaseItem") &&
              contains(result.nirText, "type @demo.boundedtypes.SpecialCodec.Item : "
                                       "demo.boundedtypes.SpecialItem") &&
              contains(result.nirText,
                       "define @demo.boundedtypes.SpecialCodec.describe : "
                       "(demo.boundedtypes.SpecialCodec,"
                       "demo.boundedtypes.BaseItem)String"),
          "NIR did not preserve the bounded reference-erased method ABI")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "ptr @demo_boundedtypes_SpecialCodec_describe") &&
              contains(result.llvmIr, "ptr @demo_boundedtypes_SpecialItem_label"),
          "LLVM IR did not retain bounded-type specialization dispatch")) {
    return code;
  }

  const std::filesystem::path binaryPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-bounded-type-members";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-bounded-type-members.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "BoundedTypeMembers.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "bounded type member build-binary failed for a reason other "
                  "than a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native bounded type member binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "special\n",
                "native bounded type member binary did not preserve the "
                "specialized subtype");
}

int smokeIntervalTypeMembersMvp() {
  constexpr const char* source = R"(package demo.intervaltypes

class WiderItem {
  def label: String = "wider"
}
class UpperItem extends WiderItem {
  override def label: String = "upper"
}
class MiddleItem extends UpperItem {
  override def label: String = "middle"
}
class LowerItem extends MiddleItem {
  override def label: String = "lower"
}

trait ItemCodec {
  type Item >: LowerItem <: UpperItem
  def describe(item: Item): String
}
trait MiddleCodec extends ItemCodec {
  override type Item = MiddleItem
  override def describe(item: Item): String = item.label
}
class ConcreteCodec extends MiddleCodec

object Main {
  def show(codec: MiddleCodec, item: MiddleItem) =
    println(codec.describe(item))
  def main = show(new ConcreteCodec(), new MiddleItem())
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("IntervalTypeMembers.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "interval type member build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText,
                   "type @demo.intervaltypes.ItemCodec.Item : abstract >: "
                   "demo.intervaltypes.LowerItem <: "
                   "demo.intervaltypes.UpperItem") &&
              contains(result.nirText, "type @demo.intervaltypes.MiddleCodec.Item : "
                                       "demo.intervaltypes.MiddleItem") &&
              contains(result.nirText,
                       "define @demo.intervaltypes.MiddleCodec.describe : "
                       "(demo.intervaltypes.MiddleCodec,"
                       "demo.intervaltypes.UpperItem)String"),
          "NIR did not preserve the interval upper-bound method ABI")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-interval-type-members";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-interval-type-members.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "IntervalTypeMembers.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "interval type member build-binary failed for a reason other "
                  "than a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native interval type member binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "middle\n",
                "native interval type member binary did not preserve the "
                "selected alias");
}

int smokePathDependentTypesMvp() {
  constexpr const char* source = R"(package demo.pathdependent

class IntBox {
  type Item = Int
  def seed: Item = 7
}

object Main {
  def echo(box: IntBox, value: box.Item): box.Item = value
  def projected(value: IntBox#Item): IntBox#Item = value
  def main = {
    val box = new IntBox()
    println(echo(box, box.seed))
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("PathDependentTypes.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "path-dependent type build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "type @demo.pathdependent.IntBox.Item : Int") &&
              contains(result.nirText, "define @demo.pathdependent.Main.echo : "
                                       "(demo.pathdependent.IntBox,Int)Int") &&
              contains(result.nirText,
                       "define @demo.pathdependent.Main.projected : (Int)Int"),
          "NIR did not specialize a stable path-dependent concrete alias")) {
    return code;
  }

  scalanative::support::DiagnosticEngine typedDiagnostics;
  scalanative::support::SourceManager typedSources;
  const scalanative::support::SourceId typedSource =
      typedSources.addVirtualFile("PathDependentTypes.scala", source, typedDiagnostics);
  scalanative::frontend::Lexer typedLexer(typedSources, typedDiagnostics);
  scalanative::frontend::Parser typedParser(typedLexer.lex(typedSource),
                                            typedDiagnostics);
  scalanative::frontend::Typechecker provenanceTypechecker(typedDiagnostics);
  const scalanative::frontend::TypedModule typedModule =
      provenanceTypechecker.typecheck(typedParser.parse());
  const scalanative::frontend::TypedDeclaration* echo =
      findTypedDeclaration(typedModule.declarations, "demo.pathdependent.Main.echo");
  const scalanative::frontend::TypedDeclaration* projected = findTypedDeclaration(
      typedModule.declarations, "demo.pathdependent.Main.projected");
  const bool sawConcretePathValue = std::any_of(
      typedModule.expressionTypes.begin(), typedModule.expressionTypes.end(),
      [](const scalanative::frontend::TypedExpressionInfo& info) {
        return info.type.kind == scalanative::frontend::SimpleTypeKind::Int &&
               info.type.pathDependent && info.type.dependentPathName == "box.Item" &&
               info.type.resolvedAliasName == "demo.pathdependent.IntBox.Item";
      });
  if (int code =
          expect(!typedDiagnostics.hasErrors() && echo != nullptr &&
                     projected != nullptr && echo->parameterTypes.size() == 2 &&
                     echo->parameterTypes[1].kind ==
                         scalanative::frontend::SimpleTypeKind::Int &&
                     echo->parameterTypes[1].pathDependent &&
                     echo->parameterTypes[1].dependentOwnerName ==
                         "demo.pathdependent.IntBox" &&
                     echo->parameterTypes[1].dependentMemberName == "Item" &&
                     echo->parameterTypes[1].dependentPathName == "box.Item" &&
                     echo->parameterTypes[1].resolvedAliasName ==
                         "demo.pathdependent.IntBox.Item" &&
                     echo->inferredType.pathDependent &&
                     echo->inferredType.dependentPathName == "box.Item" &&
                     projected->parameterTypes.size() == 1 &&
                     projected->parameterTypes[0].kind ==
                         scalanative::frontend::SimpleTypeKind::Int &&
                     projected->parameterTypes[0].typeProjection &&
                     projected->parameterTypes[0].dependentOwnerName ==
                         "demo.pathdependent.IntBox" &&
                     projected->parameterTypes[0].dependentMemberName == "Item" &&
                     projected->parameterTypes[0].resolvedAliasName ==
                         "demo.pathdependent.IntBox.Item" &&
                     sawConcretePathValue,
                 "typechecker lost concrete dependent alias provenance")) {
    return code;
  }

  const std::filesystem::path binaryPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-path-dependent-types";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-path-dependent-types.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "PathDependentTypes.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "path-dependent type build-binary failed for a reason other "
                  "than a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native path-dependent type binary did not exit "
                        "successfully")) {
    return code;
  }
  if (int code = expect(readTextFile(outputPath) == "7\n",
                        "native path-dependent type binary did not preserve the "
                        "selected concrete alias")) {
    return code;
  }

  constexpr const char* unstablePrefixSource = R"(package demo.bad
class IntBox {
  type Item = Int
}
class Broken(var box: IntBox) {
  type Alias = box.Item
}
)";
  scalanative::support::DiagnosticEngine unstablePrefixDiagnostics;
  scalanative::tools::build::BuildResult unstablePrefix = driver.buildSource(
      "UnstablePathPrefix.scala", unstablePrefixSource, {}, unstablePrefixDiagnostics);
  if (int code =
          expect(!unstablePrefix.ok &&
                     contains(unstablePrefix.diagnosticsText,
                              "unstable path-dependent type prefix: box is a variable"),
                 "mutable value was accepted as a path-dependent type prefix")) {
    return code;
  }

  constexpr const char* missingMemberSource = R"(package demo.bad
class Box
object Main {
  def broken(box: Box, value: box.Missing): Int = 1
}
)";
  scalanative::support::DiagnosticEngine missingMemberDiagnostics;
  scalanative::tools::build::BuildResult missingMember = driver.buildSource(
      "MissingPathTypeMember.scala", missingMemberSource, {}, missingMemberDiagnostics);
  if (int code = expect(!missingMember.ok &&
                            contains(missingMember.diagnosticsText,
                                     "unresolved path-dependent type member Missing on "
                                     "demo.bad.Box"),
                        "missing path-dependent type member did not fail typecheck")) {
    return code;
  }

  constexpr const char* abstractSignatureSource = R"(package demo.bad
trait Box {
  type Item
}
object Main {
  def echo(box: Box, value: box.Item): box.Item = value
}
)";
  scalanative::support::DiagnosticEngine abstractSignatureDiagnostics;
  scalanative::tools::build::BuildResult abstractSignature =
      driver.buildSource("AbstractPathSignature.scala", abstractSignatureSource, {},
                         abstractSignatureDiagnostics);
  return expect(
      abstractSignature.ok &&
          contains(abstractSignature.nirText,
                   "define @demo.bad.Main.echo : (demo.bad.Box,Object)Object"),
      "unbounded path-dependent signature did not use the object-erased ABI");
}

int smokeTypeProjectionsMvp() {
  constexpr const char* source = R"(package demo.projections

trait Carrier {
  type Element
}
trait IntCarrier extends Carrier {
  override type Element = Int
}
object Main {
  def echo(value: IntCarrier#Element): IntCarrier#Element = value
  def main = println(echo(13))
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("TypeProjections.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "type projection build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText,
                   "type @demo.projections.Carrier.Element : abstract") &&
              contains(result.nirText,
                       "type @demo.projections.IntCarrier.Element : Int") &&
              contains(result.nirText, "define @demo.projections.Main.echo : (Int)Int"),
          "NIR did not specialize an inherited concrete type projection")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-type-projections";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-type-projections.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "TypeProjections.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "type projection build-binary failed for a reason other than "
                  "a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native type projection binary did not exit "
                        "successfully")) {
    return code;
  }
  if (int code = expect(readTextFile(outputPath) == "13\n",
                        "native type projection binary did not preserve its "
                        "concrete alias")) {
    return code;
  }

  constexpr const char* unknownOwnerSource = R"(package demo.bad
object Main {
  def broken(value: Missing#Element): Int = 1
}
)";
  scalanative::support::DiagnosticEngine unknownOwnerDiagnostics;
  scalanative::tools::build::BuildResult unknownOwner = driver.buildSource(
      "UnknownProjectionOwner.scala", unknownOwnerSource, {}, unknownOwnerDiagnostics);
  if (int code = expect(!unknownOwner.ok &&
                            contains(unknownOwner.diagnosticsText,
                                     "unresolved type projection owner: Missing"),
                        "unknown type projection owner did not fail typecheck")) {
    return code;
  }

  constexpr const char* missingProjectedMemberSource = R"(package demo.bad
trait Box
object Main {
  def broken(value: Box#Missing): Int = 1
}
)";
  scalanative::support::DiagnosticEngine missingProjectedMemberDiagnostics;
  scalanative::tools::build::BuildResult missingProjectedMember =
      driver.buildSource("MissingProjectedMember.scala", missingProjectedMemberSource,
                         {}, missingProjectedMemberDiagnostics);
  if (int code = expect(
          !missingProjectedMember.ok &&
              contains(missingProjectedMember.diagnosticsText,
                       "unresolved projected type member Missing on demo.bad.Box"),
          "missing projected type member did not fail typecheck")) {
    return code;
  }

  constexpr const char* projectedValueSource = R"(package demo.bad
class Box {
  val Element: Int = 1
}
object Main {
  def broken(value: Box#Element): Int = value
}
)";
  scalanative::support::DiagnosticEngine projectedValueDiagnostics;
  scalanative::tools::build::BuildResult projectedValue = driver.buildSource(
      "ProjectedValue.scala", projectedValueSource, {}, projectedValueDiagnostics);
  if (int code =
          expect(!projectedValue.ok &&
                     contains(projectedValue.diagnosticsText,
                              "projected member Element on demo.bad.Box is not a type "
                              "member"),
                 "value member was accepted as a projected type")) {
    return code;
  }

  constexpr const char* abstractProjectionSource = R"(package demo.bad
trait Box {
  type Element
}
object Main {
  def echo(value: Box#Element): Box#Element = value
}
)";
  scalanative::support::DiagnosticEngine abstractProjectionDiagnostics;
  scalanative::tools::build::BuildResult abstractProjection =
      driver.buildSource("AbstractProjection.scala", abstractProjectionSource, {},
                         abstractProjectionDiagnostics);
  return expect(abstractProjection.ok &&
                    contains(abstractProjection.nirText,
                             "define @demo.bad.Main.echo : (Object)Object"),
                "unbounded type projection did not use the object-erased ABI");
}

int smokeAbstractDependentReferenceMvp() {
  constexpr const char* source = R"(package demo.abstractdependent

class BaseItem {
  def label: String = "base"
}
class SpecialItem extends BaseItem {
  override def label: String = "special"
}
trait Box {
  type Item <: BaseItem
  def value: Item
}
class SpecialBox extends Box {
  override type Item = SpecialItem
  override def value: Item = new SpecialItem()
}
object Main {
  def show(box: Box, item: box.Item): String = item.label
  def widen(box: Box, item: box.Item): Box#Item = item
  def projectedLabel(item: Box#Item): String = item.label
  def main = {
    val box: Box = new SpecialBox()
    println(show(box, box.value))
    println(projectedLabel(widen(box, box.value)))
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("AbstractDependentReferences.scala", source, {}, diagnostics);
  if (int code =
          expect(result.ok, "bounded abstract dependent build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText,
                   "type @demo.abstractdependent.Box.Item : abstract <: "
                   "demo.abstractdependent.BaseItem") &&
              contains(result.nirText, "declare @demo.abstractdependent.Box.value : "
                                       "(demo.abstractdependent.Box)"
                                       "demo.abstractdependent.BaseItem") &&
              contains(result.nirText, "define @demo.abstractdependent.Main.show : "
                                       "(demo.abstractdependent.Box,"
                                       "demo.abstractdependent.BaseItem)String") &&
              contains(result.nirText, "define @demo.abstractdependent.Main.widen : "
                                       "(demo.abstractdependent.Box,"
                                       "demo.abstractdependent.BaseItem)"
                                       "demo.abstractdependent.BaseItem") &&
              contains(result.nirText,
                       "define @demo.abstractdependent.Main.projectedLabel : "
                       "(demo.abstractdependent.BaseItem)String") &&
              contains(result.nirText,
                       "define @demo.abstractdependent.SpecialBox.value : "
                       "(demo.abstractdependent.SpecialBox)"
                       "demo.abstractdependent.BaseItem"),
          "NIR did not erase bounded abstract dependent types to their reference "
          "upper bound")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-abstract-dependent-reference";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-abstract-dependent-reference.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "AbstractDependentReferences.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "bounded abstract dependent build-binary failed for a reason "
                  "other than a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native bounded abstract dependent binary did not exit "
                        "successfully")) {
    return code;
  }
  if (int code = expect(readTextFile(outputPath) == "special\nspecial\n",
                        "native bounded abstract dependent dispatch lost the "
                        "concrete override or projection widening")) {
    return code;
  }

  constexpr const char* crossPathSource = R"(package demo.bad
class Base
trait Box {
  type Item <: Base
}
object Main {
  def broken(left: Box, right: Box, value: left.Item): right.Item = value
}
)";
  scalanative::support::DiagnosticEngine crossPathDiagnostics;
  scalanative::tools::build::BuildResult crossPath = driver.buildSource(
      "CrossPathIdentity.scala", crossPathSource, {}, crossPathDiagnostics);
  if (int code =
          expect(!crossPath.ok &&
                     contains(crossPath.diagnosticsText,
                              "initializer type left.Item does not conform to declared "
                              "type right.Item"),
                 "distinct stable dependent paths were treated as the same type")) {
    return code;
  }

  constexpr const char* projectionNarrowingSource = R"(package demo.bad
class Base
trait Box {
  type Item <: Base
}
object Main {
  def broken(box: Box, value: Box#Item): box.Item = value
}
)";
  scalanative::support::DiagnosticEngine projectionNarrowingDiagnostics;
  scalanative::tools::build::BuildResult projectionNarrowing =
      driver.buildSource("ProjectionNarrowing.scala", projectionNarrowingSource, {},
                         projectionNarrowingDiagnostics);
  if (int code =
          expect(!projectionNarrowing.ok &&
                     contains(projectionNarrowing.diagnosticsText,
                              "initializer type demo.bad.Box#Item does not conform to "
                              "declared type box.Item"),
                 "owner projection was narrowed unsafely to a stable dependent path")) {
    return code;
  }

  constexpr const char* primitiveUpperSource = R"(package demo.bad
trait NumericBox {
  type Item <: Int
}
object Main {
  def echo(box: NumericBox, value: box.Item): box.Item = value
}
)";
  scalanative::support::DiagnosticEngine primitiveUpperDiagnostics;
  scalanative::tools::build::BuildResult primitiveUpper =
      driver.buildSource("PrimitiveDependentUpper.scala", primitiveUpperSource, {},
                         primitiveUpperDiagnostics);
  return expect(
      primitiveUpper.ok &&
          contains(primitiveUpper.nirText,
                   "define @demo.bad.Main.echo : (demo.bad.NumericBox,Object)Object"),
      "Int-bounded abstract dependent type did not use the object-erased ABI");
}

int smokeAbstractTraitValMvp() {
  constexpr const char* source = R"(package demo.abstractval

trait NamedScore {
  val name: String
  val score: Int
  def label: String = name
  def doubled: Int = score + score
}

class ConcreteScore extends NamedScore {
  override val name: String = "concrete"
  override val score: Int = 6
}

object Main {
  def show(value: NamedScore) = {
    println(value.name)
    println(value.label)
    println(value.score)
    println(value.doubled)
  }

  def main = {
    val value = new ConcreteScore()
    show(value)
    println(value.name)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("AbstractTraitVal.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "abstract trait val build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "declare @demo.abstractval.NamedScore.name : "
                                   "(demo.abstractval.NamedScore)String") &&
              contains(result.nirText,
                       "field @demo.abstractval.ConcreteScore.name$field : String") &&
              contains(result.nirText, "define @demo.abstractval.ConcreteScore.name : "
                                       "(demo.abstractval.ConcreteScore)String") &&
              contains(result.nirText, "%this.name$field"),
          "NIR did not split concrete abstract-val storage from its accessor")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "ptr @demo_abstractval_ConcreteScore_name") &&
              contains(result.llvmIr, "ptr @demo_abstractval_ConcreteScore_score") &&
              contains(result.llvmIr, "call ptr %") &&
              contains(result.llvmIr, "call i32 %"),
          "LLVM IR did not dispatch abstract trait values through accessors")) {
    return code;
  }

  const std::filesystem::path binaryPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-abstract-trait-val";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-abstract-trait-val.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "AbstractTraitVal.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "abstract trait val build-binary failed for a reason other "
                  "than a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native abstract trait val binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "concrete\nconcrete\n6\n12\nconcrete\n",
                "native abstract trait val binary did not print expected output");
}

int smokeAbstractTraitConstructorValMvp() {
  constexpr const char* source = R"(package demo.constructorval

trait NamedScore {
  val name: String
  val score: Int
  def label: String = name
  def doubled: Int = score + score
}

class ConstructorScore(val name: String, val score: Int) extends NamedScore

class ChildScore(name: String, score: Int)
    extends ConstructorScore(name, score)

object Main {
  def show(value: NamedScore) = {
    println(value.name)
    println(value.label)
    println(value.score)
    println(value.doubled)
  }

  def main = {
    val direct = new ConstructorScore("direct", 7)
    val child = new ChildScore("child", 5)
    show(direct)
    show(child)
    println(direct.name)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("AbstractTraitConstructorVal.scala", source, {}, diagnostics);

  if (int code =
          expect(result.ok, "abstract trait constructor val build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText,
                   "field @demo.constructorval.ConstructorScore.name$field : "
                   "String") &&
              contains(result.nirText,
                       "define @demo.constructorval.ConstructorScore.name : "
                       "(demo.constructorval.ConstructorScore)String") &&
              contains(result.nirText, "ret String %this.name$field") &&
              contains(result.nirText, "eval assign %super.name$field = %this.name") &&
              contains(result.nirText, "eval assign %super.score$field = %this.score"),
          "NIR did not lower constructor vals to accessor-backed fields")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "ptr @demo_constructorval_ConstructorScore_name") &&
              contains(result.llvmIr,
                       "ptr @demo_constructorval_ConstructorScore_score") &&
              contains(result.llvmIr, "call ptr %") &&
              contains(result.llvmIr, "call i32 %"),
          "LLVM IR did not dispatch constructor val accessors")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-abstract-trait-constructor-val";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-abstract-trait-constructor-val.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "AbstractTraitConstructorVal.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "abstract trait constructor val build-binary failed for a "
                  "reason other than a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native abstract trait constructor val binary did not "
                        "exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) ==
                    "direct\ndirect\n7\n14\nchild\nchild\n5\n10\ndirect\n",
                "native abstract trait constructor val binary did not print "
                "expected output");
}

int smokeInitializedTraitValsMvp() {
  constexpr const char* source = R"(package demo.traitvals

trait RootValues {
  val root: Int = {
    println("root init")
    2
  }
  def rootValue: Int = root
}

trait LeftValues extends RootValues {
  val left: Int = {
    println("left init")
    3
  }
  def leftValue: Int = left
}

trait RightValues {
  val right: Int = {
    println("right init")
    5
  }
  def rightValue: Int = right
}

class CombinedValues extends LeftValues with RightValues {
  def total: Int = root + left + right
}

class ChildValues extends CombinedValues

object Main {
  def show(value: CombinedValues) = {
    println(value.rootValue)
    println(value.leftValue)
    println(value.rightValue)
    println(value.total)
    println(value.total)
  }

  def main = {
    show(new CombinedValues())
    show(new ChildValues())
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("InitializedTraitVals.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "initialized trait val build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "declare @demo.traitvals.RootValues.root : "
                                   "(demo.traitvals.RootValues)Int") &&
              contains(result.nirText,
                       "field @demo.traitvals.CombinedValues."
                       "root$trait$demo$traitvals$RootValues$field : Int") &&
              contains(result.nirText, "define @demo.traitvals.CombinedValues.root : "
                                       "(demo.traitvals.CombinedValues)Int") &&
              contains(result.nirText,
                       "eval assign %this."
                       "root$trait$demo$traitvals$RootValues$field = 2") &&
              contains(result.nirText,
                       "eval assign %this."
                       "left$trait$demo$traitvals$LeftValues$field = 3") &&
              contains(result.nirText,
                       "eval assign %this."
                       "right$trait$demo$traitvals$RightValues$field = 5") &&
              !contains(result.nirText,
                        "field @demo.traitvals.ChildValues.root$trait$"),
          "NIR did not materialize initialized trait vals once in the base "
          "class")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "ptr @demo_traitvals_CombinedValues_root") &&
              contains(result.llvmIr, "ptr @demo_traitvals_CombinedValues_left") &&
              contains(result.llvmIr, "ptr @demo_traitvals_CombinedValues_right"),
          "LLVM IR did not place initialized trait val getters in vtables")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-initialized-trait-vals";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-initialized-trait-vals.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "InitializedTraitVals.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "initialized trait val build-binary failed for a reason other "
                  "than a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native initialized trait val binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) ==
                    "root init\nleft init\nright init\n2\n3\n5\n10\n10\n"
                    "root init\nleft init\nright init\n2\n3\n5\n10\n10\n",
                "native initialized trait val binary did not preserve one-time ordered "
                "initialization");
}

int smokeShadowedTraitValsMvp() {
  constexpr const char* source = R"(package demo.shadowedvals

trait LeftValue {
  val value: Int = {
    println("left init")
    3
  }
}

trait RightValue {
  val value: Int = {
    println("right init")
    9
  }
}

class CombinedValue extends LeftValue with RightValue {
  def current: Int = value
  def left: Int = super[LeftValue].value
  def right: Int = super[RightValue].value
}

class ChildValue extends CombinedValue

object Main {
  def show(value: CombinedValue) = {
    println(value.current)
    println(value.left)
    println(value.right)
    println(value.current)
  }

  def main = {
    show(new CombinedValue())
    show(new ChildValue())
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("ShadowedTraitVals.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "shadowed trait val build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code =
          expect(contains(result.nirText,
                          "value$trait$demo$shadowedvals$LeftValue$field : Int") &&
                     contains(result.nirText,
                              "value$trait$demo$shadowedvals$RightValue$field : Int") &&
                     contains(result.nirText,
                              "eval assign %this."
                              "value$trait$demo$shadowedvals$LeftValue$field = 3") &&
                     contains(result.nirText,
                              "eval assign %this."
                              "value$trait$demo$shadowedvals$RightValue$field = 9") &&
                     contains(result.nirText,
                              "ret Int %this."
                              "value$trait$demo$shadowedvals$RightValue$field") &&
                     contains(result.nirText,
                              "ret Int %super."
                              "$trait-value$demo$shadowedvals$LeftValue$value") &&
                     contains(result.nirText,
                              "ret Int %super."
                              "$trait-value$demo$shadowedvals$RightValue$value") &&
                     !contains(result.nirText,
                               "field @demo.shadowedvals.ChildValue.value$trait$"),
                 "NIR did not preserve owner-specific shadowed trait value storage")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "@demo_shadowedvals_CombinedValue_value") &&
              contains(result.llvmIr,
                       "@demo_shadowedvals_CombinedValue__trait_value_demo_"
                       "shadowedvals_LeftValue_value") &&
              contains(result.llvmIr,
                       "@demo_shadowedvals_CombinedValue__trait_value_demo_"
                       "shadowedvals_RightValue_value"),
          "LLVM IR did not retain public and owner-specific trait value getters")) {
    return code;
  }

  const std::filesystem::path binaryPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-shadowed-trait-vals";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-shadowed-trait-vals.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "ShadowedTraitVals.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "shadowed trait val build-binary failed for a reason other "
                  "than a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native shadowed trait val binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(
      readTextFile(outputPath) == "left init\nright init\n9\n3\n9\n9\n"
                                  "left init\nright init\n9\n3\n9\n9\n",
      "native shadowed trait val binary did not preserve owner-specific values");
}

int smokeTraitVarsMvp() {
  constexpr const char* source = R"(package demo.traitvars

trait MutableCounter {
  var current: Int
  def bump: Int = {
    current = current + 1
    current
  }
}

trait InitializedCounter extends MutableCounter {
  override var current: Int = {
    println("trait var init")
    2
  }
}

class TraitCounter extends InitializedCounter
class ParameterCounter(var current: Int) extends MutableCounter
class BodyCounter extends MutableCounter {
  override var current: Int = 30
}

object Main {
  def change(counter: MutableCounter, value: Int) = {
    counter.current = value
    println(counter.bump)
    println(counter.current)
  }

  def main = {
    change(new TraitCounter(), 5)
    change(new ParameterCounter(10), 20)
    change(new BodyCounter(), 40)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("TraitVars.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "trait var build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "declare @demo.traitvars.MutableCounter.current : "
                                   "(demo.traitvars.MutableCounter)Int") &&
              contains(result.nirText,
                       "declare @demo.traitvars.MutableCounter.current_$eq : "
                       "(demo.traitvars.MutableCounter,Int)Unit") &&
              contains(result.nirText, "eval assign %counter.current = %value") &&
              contains(result.nirText,
                       "define @demo.traitvars.TraitCounter.current_$eq") &&
              contains(result.nirText,
                       "define @demo.traitvars.ParameterCounter.current_$eq") &&
              contains(result.nirText,
                       "define @demo.traitvars.BodyCounter.current_$eq"),
          "NIR did not emit trait var getter/setter implementations")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "ptr @demo_traitvars_TraitCounter_current__eq") &&
              contains(result.llvmIr,
                       "ptr @demo_traitvars_ParameterCounter_current__eq") &&
              contains(result.llvmIr, "ptr @demo_traitvars_BodyCounter_current__eq") &&
              contains(result.llvmIr, "call void %"),
          "LLVM IR did not place trait var setters in vtables or dispatch "
          "through them")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-trait-vars";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-trait-vars.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("TraitVars.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "trait var build-binary failed for a reason other than a "
                  "missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native trait var binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "trait var init\n6\n6\n21\n21\n41\n41\n",
                "native trait var binary did not preserve mutable trait state");
}

int smokeMixedTraitAccessorsMvp() {
  constexpr const char* source = R"(package demo.mixedaccessors

trait Readable {
  val value: Int
}

trait Mutable {
  var value: Int = 7

  def increment: Int = {
    value = value + 1
    value
  }
}

class State extends Readable with Mutable

object Main {
  def read(state: Readable) =
    println(state.value)

  def update(state: Mutable) = {
    state.value = 20
    println(state.increment)
  }

  def main = {
    val state = new State()
    read(state)
    update(state)
    read(state)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("MixedTraitAccessors.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "mixed trait accessor build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "declare @demo.mixedaccessors.Readable.value : "
                                   "(demo.mixedaccessors.Readable)Int") &&
              contains(result.nirText,
                       "declare @demo.mixedaccessors.Mutable.value_$eq : "
                       "(demo.mixedaccessors.Mutable,Int)Unit") &&
              contains(result.nirText, "define @demo.mixedaccessors.State.value :") &&
              contains(result.nirText,
                       "define @demo.mixedaccessors.State.value_$eq :") &&
              contains(result.nirText, "eval assign %state.value = 20"),
          "NIR did not let a mutable accessor satisfy the readable contract")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "ptr @demo_mixedaccessors_State_value") &&
              contains(result.llvmIr, "ptr @demo_mixedaccessors_State_value__eq") &&
              contains(result.llvmIr, "call void %"),
          "LLVM IR did not share the getter and dispatch the mutable setter")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-mixed-trait-accessors";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-mixed-trait-accessors.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "MixedTraitAccessors.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "mixed trait accessor build-binary failed for a reason other "
                  "than a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native mixed trait accessor binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "7\n21\n21\n",
                "native mixed trait accessors did not share mutable state");
}

int smokeShadowedTraitVarsMvp() {
  constexpr const char* source = R"(package demo.shadowedvars

trait LeftState {
  var value: Int = {
    println("left var init")
    3
  }
}

trait RightState {
  var value: Int = {
    println("right var init")
    9
  }
}

class CombinedState extends LeftState with RightState {
  def current: Int = value
  def left: Int = super[LeftState].value
  def right: Int = super[RightState].value

  def updateCurrent(next: Int): Int = {
    value = next
    value
  }

  def updateLeft(next: Int): Int = {
    super[LeftState].value = next
    super[LeftState].value
  }

  def updateRight(next: Int): Int = {
    super[RightState].value = next
    super[RightState].value
  }
}

object Main {
  def main = {
    val state = new CombinedState()
    println(state.current)
    println(state.left)
    println(state.right)
    println(state.updateLeft(4))
    println(state.current)
    println(state.updateRight(10))
    println(state.current)
    println(state.updateCurrent(12))
    println(state.left)
    println(state.right)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("ShadowedTraitVars.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "shadowed trait var build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code =
          expect(contains(result.nirText,
                          "value$trait$demo$shadowedvars$LeftState$field : Int") &&
                     contains(result.nirText,
                              "value$trait$demo$shadowedvars$RightState$field : Int") &&
                     contains(result.nirText,
                              "define @demo.shadowedvars.CombinedState."
                              "$trait-value$demo$shadowedvars$LeftState$value_$eq") &&
                     contains(result.nirText,
                              "define @demo.shadowedvars.CombinedState."
                              "$trait-value$demo$shadowedvars$RightState$value_$eq") &&
                     contains(result.nirText,
                              "eval assign %super.$trait-value$demo$shadowedvars$"
                              "LeftState$value = %next") &&
                     contains(result.nirText,
                              "eval assign %super.$trait-value$demo$shadowedvars$"
                              "RightState$value = %next"),
                 "NIR did not preserve owner-specific shadowed trait var setters")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "@demo_shadowedvars_CombinedState__trait_value_demo_"
                                  "shadowedvars_LeftState_value__eq") &&
              contains(result.llvmIr,
                       "@demo_shadowedvars_CombinedState__trait_value_demo_"
                       "shadowedvars_RightState_value__eq") &&
              contains(result.llvmIr, "ptr @demo_shadowedvars_CombinedState_value__eq"),
          "LLVM IR did not retain hidden and public shadowed trait setters")) {
    return code;
  }

  const std::filesystem::path binaryPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-shadowed-trait-vars";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-shadowed-trait-vars.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "ShadowedTraitVars.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "shadowed trait var build-binary failed for a reason other "
                  "than a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native shadowed trait var binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(
      readTextFile(outputPath) ==
          "left var init\nright var init\n9\n3\n9\n4\n9\n10\n10\n12\n4\n12\n",
      "native shadowed trait var binary did not preserve owner-specific state");
}

int smokeTraitHierarchyMvp() {
  constexpr const char* source = R"(package demo.traitchain

trait RootMetric {
  def label: String
  def score: Int
}

trait LabeledMetric extends RootMetric {
  override def label: String = "labeled"
  def doubled: Int = score + score
}

class ConcreteMetric extends LabeledMetric {
  override def score: Int = 6
}

object Main {
  def showRoot(metric: RootMetric) = {
    println(metric.label)
    println(metric.score)
  }

  def showLabeled(metric: LabeledMetric) =
    println(metric.doubled)

  def main = {
    val metric = new ConcreteMetric()
    showRoot(metric)
    showLabeled(metric)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("TraitHierarchy.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "trait hierarchy MVP build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "trait @demo.traitchain.LabeledMetric : "
                                   "@demo.traitchain.RootMetric") &&
              contains(result.nirText, "declare @demo.traitchain.RootMetric.score : "
                                       "(demo.traitchain.RootMetric)Int") &&
              contains(result.nirText, "define @demo.traitchain.LabeledMetric.label : "
                                       "(demo.traitchain.LabeledMetric)String") &&
              contains(result.nirText,
                       "define @demo.traitchain.LabeledMetric.doubled : "
                       "(demo.traitchain.LabeledMetric)Int") &&
              contains(result.nirText, "ret Int (%this.score + %this.score)"),
          "NIR did not preserve transitive abstract/default trait members")) {
    return code;
  }
  if (int code = expect(
          !contains(result.llvmIr, "@__vtable_demo_traitchain_RootMetric") &&
              !contains(result.llvmIr, "@__vtable_demo_traitchain_LabeledMetric") &&
              contains(result.llvmIr, "@__vtable_demo_traitchain_ConcreteMetric") &&
              contains(result.llvmIr, "ptr @demo_traitchain_LabeledMetric_label") &&
              contains(result.llvmIr, "ptr @demo_traitchain_ConcreteMetric_score") &&
              contains(result.llvmIr, "ptr @demo_traitchain_LabeledMetric_doubled") &&
              contains(result.llvmIr, "getelementptr [3 x ptr]"),
          "LLVM IR did not assemble transitive concrete trait vtable")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-trait-hierarchy";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-trait-hierarchy.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "TraitHierarchy.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "trait hierarchy build-binary failed for a reason other than a "
                  "missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native trait hierarchy smoke binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "labeled\n6\n12\n",
                "native trait hierarchy smoke binary did not print expected output");
}

int smokeTraitCompositionMvp() {
  constexpr const char* source = R"(package demo.composition

trait Named {
  def name: String
}

trait Scored {
  def score: Int
}

trait BaseLabel {
  def label: String = "base-label"
}

trait FancyLabel {
  def label: String = "fancy-label"
}

class Base(val seed: Int) {
  def base: Int = seed
}

class Combined(seed: Int)
    extends Base(seed)
    with Named
    with Scored
    with BaseLabel
    with FancyLabel {
  override def name: String = "combined"
  override def score: Int = base + 2
}

object Main {
  def showName(value: Named) = println(value.name)
  def showScore(value: Scored) = println(value.score)
  def showLabel(value: BaseLabel) = println(value.label)

  def main = {
    val value = new Combined(5)
    showName(value)
    showScore(value)
    showLabel(value)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("TraitComposition.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "trait composition MVP build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code =
          expect(contains(result.nirText,
                          "class @demo.composition.Combined : "
                          "@demo.composition.Base with @demo.composition.Named with "
                          "@demo.composition.Scored with @demo.composition.BaseLabel "
                          "with @demo.composition.FancyLabel") &&
                     contains(result.nirText, "eval assign %super.seed = %this.seed"),
                 "NIR did not preserve ordered superclass and trait composition")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "@__vtable_demo_composition_Combined") &&
              contains(result.llvmIr, "ptr @demo_composition_FancyLabel_label") &&
              !contains(result.llvmIr, "@__vtable_demo_composition_BaseLabel") &&
              !contains(result.llvmIr, "@__vtable_demo_composition_FancyLabel") &&
              contains(result.llvmIr, "getelementptr [4 x ptr]"),
          "LLVM IR did not compose stable trait slots with rightmost precedence")) {
    return code;
  }

  const std::filesystem::path binaryPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-trait-composition";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-trait-composition.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "TraitComposition.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "trait composition build-binary failed for a reason other than "
                  "a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native trait composition smoke binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "combined\n7\nfancy-label\n",
                "native trait composition smoke binary did not print expected "
                "output");
}

int smokeQualifiedSuperMvp() {
  constexpr const char* source = R"(package demo.qualifiedsuper

trait LeftValue {
  def label: String = "left"
  def value: Int = 1
}

trait RightValue {
  def label: String = "right"
  def value: Int = 10
}

class CombinedValue extends LeftValue with RightValue {
  def leftLabel: String = super[LeftValue].label
  def rightLabel: String = super[RightValue].label
  override def value: Int =
    super[LeftValue].value + super[RightValue].value
}

object Main {
  def main = {
    val combined = new CombinedValue()
    println(combined.leftLabel)
    println(combined.rightLabel)
    println(combined.value)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("QualifiedSuper.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "qualified super MVP build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "%super[demo.qualifiedsuper.LeftValue].label") &&
              contains(result.nirText,
                       "%super[demo.qualifiedsuper.RightValue].label") &&
              contains(result.nirText, "%super[demo.qualifiedsuper.LeftValue].value") &&
              contains(result.nirText, "%super[demo.qualifiedsuper.RightValue].value"),
          "NIR did not preserve qualified super parent selection")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr,
                   "call ptr @demo_qualifiedsuper_LeftValue_label(ptr %this)") &&
              contains(result.llvmIr,
                       "call ptr @demo_qualifiedsuper_RightValue_label(ptr %this)") &&
              contains(result.llvmIr,
                       "call i32 @demo_qualifiedsuper_LeftValue_value(ptr %this)") &&
              contains(result.llvmIr,
                       "call i32 @demo_qualifiedsuper_RightValue_value(ptr %this)"),
          "LLVM IR did not lower qualified super calls to requested trait "
          "methods")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-qualified-super";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-qualified-super.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "QualifiedSuper.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "qualified super build-binary failed for a reason other than "
                  "a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native qualified super smoke binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "left\nright\n11\n",
                "native qualified super smoke binary did not print expected "
                "output");
}

int smokeTraitSuperChainMvp() {
  constexpr const char* source = R"(package demo.traitsuper

trait BaseValue {
  def value: Int = 1
}

trait PlusTen extends BaseValue {
  override def value: Int = super.value + 10
}

class CombinedValue extends BaseValue with PlusTen {
  override def value: Int = super.value + 100
}

object Main {
  def main = {
    val combined = new CombinedValue()
    println(combined.value)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("TraitSuperChain.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "trait super chain build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "%stack-super[demo.traitsuper.PlusTen].value") &&
              contains(result.llvmIr, "ptr @demo_traitsuper_BaseValue_value") &&
              contains(result.llvmIr, "call i32 %") &&
              contains(result.llvmIr,
                       "call i32 @demo_traitsuper_PlusTen_value(ptr %this)"),
          "LLVM IR did not follow the rightmost trait super chain")) {
    return code;
  }

  const std::filesystem::path binaryPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-trait-super-chain";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-trait-super-chain.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "TraitSuperChain.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "trait super chain build-binary failed for a reason other "
                  "than a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native trait super chain binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "111\n",
                "native trait super chain binary did not print expected output");
}

int smokeTransitiveSuperLookupMvp() {
  constexpr const char* source = R"(package demo.transitivesuper

trait RootValue {
  def value: Int = 1
}

trait MiddleValue extends RootValue {
  def middle: Int = 5
}

trait Marker {
  def marker: Int = 2
}

class CombinedValue extends MiddleValue with Marker {
  override def value: Int = super.value + 10
}

object Main {
  def main = {
    val combined = new CombinedValue()
    println(combined.value)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("TransitiveSuper.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "transitive super lookup build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr,
                   "call i32 @demo_transitivesuper_RootValue_value(ptr %this)") &&
              !contains(result.llvmIr, "@demo_transitivesuper_Marker_value"),
          "LLVM IR did not skip parents without the requested super member")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-transitive-super";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-transitive-super.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "TransitiveSuper.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "transitive super build-binary failed for a reason other than "
                  "a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native transitive super binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "11\n",
                "native transitive super binary did not print expected output");
}

int smokeDiamondLinearizationMvp() {
  constexpr const char* source = R"(package demo.diamond

trait RootValue {
  def value: Int = 1
  def root: Int = 7
}

trait LeftValue extends RootValue {
  override def value: Int = 10
}

trait RightValue extends RootValue {
  def right: Int = 20
}

class DiamondValue extends LeftValue with RightValue {
  def inheritedValue: Int = value
  def superValue: Int = super.value
}

object Main {
  def main = {
    val diamond = new DiamondValue()
    println(diamond.value)
    println(diamond.inheritedValue)
    println(diamond.superValue)
    println(diamond.right)
    println(diamond.root)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("DiamondLinearization.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "diamond linearization build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code =
          expect(contains(result.llvmIr,
                          "call i32 @demo_diamond_LeftValue_value(ptr %this)") &&
                     contains(result.llvmIr, "ptr @demo_diamond_LeftValue_value") &&
                     !contains(result.llvmIr, "ptr @demo_diamond_RootValue_value, ptr "
                                              "@demo_diamond_LeftValue_value"),
                 "LLVM IR did not use the C3-selected diamond override")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-diamond-linearization";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-diamond-linearization.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "DiamondLinearization.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "diamond linearization build-binary failed for a reason other "
                  "than a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native diamond linearization binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "10\n10\n10\n20\n7\n",
                "native diamond linearization binary did not print expected "
                "output");
}

int smokeStackableTraitSuperMvp() {
  constexpr const char* source = R"(package demo.stackable

trait RootValue {
  def value: Int = 1
}

trait AddTwo extends RootValue {
  override def value: Int = super.value + 2
}

trait AddTen extends RootValue {
  override def value: Int = super.value + 10
}

class StackedValue extends RootValue with AddTwo with AddTen

object Main {
  def main = {
    val stacked = new StackedValue()
    println(stacked.value)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("StackableTraits.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "stackable trait super build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "%stack-super[demo.stackable.AddTwo].value") &&
              contains(result.nirText, "%stack-super[demo.stackable.AddTen].value") &&
              contains(result.llvmIr, "ptr @demo_stackable_RootValue_value") &&
              contains(result.llvmIr, "ptr @demo_stackable_AddTwo_value") &&
              contains(result.llvmIr, "ptr @demo_stackable_AddTen_value") &&
              contains(result.llvmIr, "call i32 %"),
          "stackable trait super did not lower through hidden vtable slots")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-stackable-traits";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-stackable-traits.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "StackableTraits.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "stackable traits build-binary failed for a reason other than "
                  "a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native stackable traits binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "13\n",
                "native stackable traits binary did not print expected output");
}

int smokeLocalTypedDispatchMvp() {
  constexpr const char* source = R"(package demo.localtyped

class BaseScore {
  def value: Int = 1
  def label: String = "base"
}

class FancyScore extends BaseScore {
  override def value: Int = 9
  override def label: String = "fancy"
}

object Main {
  def main = {
    val score: BaseScore = new FancyScore()
    println(score.value)
    println(score.label)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("LocalTypedDispatch.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "local typed dispatch MVP build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code =
          expect(contains(result.nirText, "let %score : demo.localtyped.BaseScore = "
                                          "new demo.localtyped.FancyScore") &&
                     contains(result.nirText, "%score.value") &&
                     contains(result.nirText, "%score.label"),
                 "NIR did not preserve declared static type for block-local value")) {
    return code;
  }
  if (int code =
          expect(contains(result.llvmIr, "@__vtable_demo_localtyped_BaseScore") &&
                     contains(result.llvmIr, "@__vtable_demo_localtyped_FancyScore") &&
                     contains(result.llvmIr, "getelementptr [2 x ptr]") &&
                     contains(result.llvmIr, "call i32 %") &&
                     contains(result.llvmIr, "call ptr %"),
                 "LLVM IR did not dispatch widened block-local selections virtually")) {
    return code;
  }

  const std::filesystem::path binaryPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-local-typed-dispatch";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-local-typed-dispatch.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "LocalTypedDispatch.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "local typed dispatch build-binary failed for a reason other "
                  "than a missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native local typed dispatch smoke binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "9\nfancy\n",
                "native local typed dispatch smoke binary did not print expected "
                "output");
}

int smokeLocalVarMvp() {
  constexpr const char* source = R"(package demo.localvars

object Main {
  def total(start: Int): Int = {
    var current = start
    current = current + 1
    current = current + 2
    current
  }

  def main = println(total(10))
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("LocalVars.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "local var MVP build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "var %current : Int = %start") &&
              contains(result.nirText, "eval assign %current = (%current + 1)") &&
              contains(result.nirText, "eval assign %current = (%current + 2)") &&
              contains(result.nirText, "ret Int %current"),
          "NIR did not preserve local var mutation")) {
    return code;
  }
  if (int code =
          expect(contains(result.llvmIr, "%current_slot = alloca i32") &&
                     contains(result.llvmIr, "store i32 %start, ptr %current_slot") &&
                     contains(result.llvmIr, "load i32, ptr %current_slot"),
                 "LLVM IR did not lower local var to a stack slot")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-local-vars";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-local-vars.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("LocalVars.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "local var build-binary failed for a reason other than a "
                  "missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native local var smoke binary did not exit successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "13\n",
                "native local var smoke binary did not print expected output");
}

int smokeConstructorBodyMvp() {
  constexpr const char* source = R"(package demo.ctor

class ConstructedCounter(start: Int) {
  var current: Int = start

  current = current + 5
  val snapshot: Int = current
  current = current + 1
  val finalValue: Int = current + snapshot

  def value: Int = finalValue
  def snapshotValue: Int = snapshot
  def currentValue: Int = current
}

object Main {
  def main = {
    val counter = new ConstructedCounter(10)
    println(counter.snapshotValue)
    println(counter.currentValue)
    println(counter.value)
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("ConstructorBody.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "constructor body MVP build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(contains(result.nirText,
                                 "define @demo.ctor.ConstructedCounter.$init : "
                                 "(demo.ctor.ConstructedCounter)Unit"),
                        "NIR did not emit a synthetic class initializer")) {
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "field @demo.ctor.ConstructedCounter.snapshot : "
                                   "Int") &&
              contains(result.nirText,
                       "field @demo.ctor.ConstructedCounter.finalValue : Int"),
          "NIR did not emit interleaved class-body fields")) {
    return code;
  }
  const std::size_t currentInit =
      result.nirText.find("eval assign %this.current = %this.start");
  const std::size_t plusFive =
      result.nirText.find("eval assign %this.current = (%this.current + 5)");
  const std::size_t snapshotInit =
      result.nirText.find("eval assign %this.snapshot = %this.current");
  const std::size_t plusOne =
      result.nirText.find("eval assign %this.current = (%this.current + 1)");
  const std::size_t finalValueInit = result.nirText.find(
      "eval assign %this.finalValue = (%this.current + %this.snapshot)");
  if (int code = expect(
          currentInit != std::string::npos && plusFive != std::string::npos &&
              snapshotInit != std::string::npos && plusOne != std::string::npos &&
              finalValueInit != std::string::npos && currentInit < plusFive &&
              plusFive < snapshotInit && snapshotInit < plusOne &&
              plusOne < finalValueInit,
          "NIR did not preserve source order for field initializers and "
          "constructor statements")) {
    return code;
  }
  if (int code =
          expect(contains(result.llvmIr,
                          "define void @demo_ctor_ConstructedCounter__init(ptr %this)"),
                 "LLVM IR did not lower the synthetic class initializer")) {
    return code;
  }
  if (int code =
          expect(contains(result.llvmIr,
                          "call void @demo_ctor_ConstructedCounter__init(ptr %tmp"),
                 "LLVM IR did not call the synthetic initializer after allocation")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-constructor-body";
  const std::filesystem::path outputPath = std::filesystem::temp_directory_path() /
                                           "cpp-scalanative-smoke-constructor-body.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary = driver.buildSource(
      "ConstructorBody.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "constructor body build-binary failed for a reason other than a "
                  "missing clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native constructor body smoke binary did not exit "
                        "successfully")) {
    return code;
  }
  return expect(readTextFile(outputPath) == "15\n16\n31\n",
                "native constructor body smoke binary did not print expected output");
}

int smokeImportMvp() {
  constexpr const char* source = R"(package demo.imports

object Library {
  object Config {
    val answer: Int = 42
  }

  class Counter {
    def value: Int = 7
  }
}

import demo.imports.Library.{Config, Counter => Tally}

object Main {
  val typed: Tally = new Tally()
  def read(counter: Tally): Int = counter.value

  def main = {
    println(Config.answer)
    println(read(typed))
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("Imports.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "import MVP build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(contains(result.nirText,
                                 "define @demo.imports.Library.Config.answer : ()Int"),
                        "NIR did not emit the imported nested object member")) {
    return code;
  }
  if (int code =
          expect(contains(result.nirText, "define @demo.imports.Main.typed : "
                                          "()demo.imports.Library.Counter"),
                 "NIR did not resolve imported class alias in declared value type")) {
    return code;
  }
  if (int code = expect(contains(result.nirText, "define @demo.imports.Main.read : "
                                                 "(demo.imports.Library.Counter)Int"),
                        "NIR did not resolve imported class alias in parameter type")) {
    return code;
  }
  if (int code =
          expect(contains(result.nirText, "call %scala.scalanative.runtime.println"
                                          "(%demo.imports.Library.Config.answer)"),
                 "NIR did not resolve imported object alias in selection")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "call i32 @demo_imports_Library_Config_answer()"),
          "LLVM IR did not lower imported object member selection")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "define i32 @demo_imports_Main_read(ptr %counter)"),
          "LLVM IR did not lower imported class alias parameter type")) {
    return code;
  }
  if (int code =
          expect(contains(result.llvmIr,
                          "call i32 @demo_imports_Library_Counter_value(ptr %counter)"),
                 "LLVM IR did not lower imported class instance method call")) {
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr, "call ptr @demo_imports_Main_typed()") &&
              contains(result.llvmIr, "call i32 @demo_imports_Main_read(ptr %tmp"),
          "LLVM IR did not lower imported typed value through read call")) {
    return code;
  }

  const std::filesystem::path binaryPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-imports";
  const std::filesystem::path outputPath =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-imports.out";
  std::filesystem::remove(binaryPath);
  std::filesystem::remove(outputPath);

  scalanative::support::DiagnosticEngine binaryDiagnostics;
  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.outputPath = binaryPath;
  scalanative::tools::build::BuildResult binary =
      driver.buildSource("Imports.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "import build-binary failed for a reason other than a missing "
                  "clang toolchain: " +
                      binary.diagnosticsText);
  }

  const std::string runCommand = binaryPath.string() + " > " + outputPath.string();
  if (int code = expect(std::system(runCommand.c_str()) == 0,
                        "native import smoke binary did not exit successfully")) {
    return code;
  }
  if (int code = expect(readTextFile(outputPath) == "42\n7\n",
                        "native import smoke binary did not print expected output")) {
    return code;
  }

  constexpr const char* invalidSource = R"(package demo.imports
object Library {
  object Config {
    val answer: Int = 42
  }
}
import demo.imports.Library._
object Bad {
  def main = println(answer)
}
)";
  scalanative::support::DiagnosticEngine invalidDiagnostics;
  scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidWildcardImport.scala", invalidSource, {}, invalidDiagnostics);
  if (int code = expect(!invalid.ok && contains(invalid.diagnosticsText,
                                                "unresolved identifier: answer"),
                        "wildcard import leaked a nested member into the scope")) {
    return code;
  }

  constexpr const char* invalidSelectorSource = R"(package demo.imports
object Library
import demo.imports.Library.{Missing}
object Bad {
  def main = println(0)
}
)";
  scalanative::support::DiagnosticEngine invalidSelectorDiagnostics;
  scalanative::tools::build::BuildResult invalidSelector =
      driver.buildSource("InvalidImportSelector.scala", invalidSelectorSource, {},
                         invalidSelectorDiagnostics);
  return expect(!invalidSelector.ok && contains(invalidSelector.diagnosticsText,
                                                "unresolved import selector: Missing"),
                "missing import selector did not produce a focused diagnostic");
}

int smokeSourceNormalization() {
  scalanative::support::SourceManager sources;
  scalanative::support::DiagnosticEngine diagnostics;

  scalanative::support::SourceId crlf = sources.addVirtualFile(
      "CRLF.scala", "object A {\r\n\tval x = 1\r}\r\n", diagnostics);
  if (int code = expect(sources.text(crlf) == "object A {\n\tval x = 1\n}\n",
                        "source manager did not normalize CRLF/CR newlines")) {
    return code;
  }
  if (int code = expect(sources.lineText(crlf, 2) == "\tval x = 1",
                        "lineText did not return the normalized second line")) {
    return code;
  }

  auto [line, column] = sources.lineColumn({crlf, 11, 1});
  if (int code = expect(line == 2 && column == 1,
                        "lineColumn did not map normalized offsets correctly")) {
    return code;
  }

  scalanative::support::SourceId lf =
      sources.addVirtualFile("LF.scala", "object A {\n\tval x = 1\n}\n", diagnostics);
  if (int code = expect(sources.contentHash(crlf) == sources.contentHash(lf),
                        "normalized source hashes are not stable")) {
    return code;
  }

  std::string bom;
  bom.push_back(static_cast<char>(0xEF));
  bom.push_back(static_cast<char>(0xBB));
  bom.push_back(static_cast<char>(0xBF));
  bom += "object Bom\n";
  scalanative::support::SourceId bomId =
      sources.addVirtualFile("Bom.scala", bom, diagnostics);
  const scalanative::support::SourceFile* file = sources.get(bomId);
  if (int code = expect(file != nullptr && file->hadByteOrderMark,
                        "source manager did not record UTF-8 BOM")) {
    return code;
  }
  return expect(sources.text(bomId) == "object Bom\n",
                "source manager did not strip UTF-8 BOM");
}

int smokeUtf8Validation() {
  scalanative::support::SourceManager sources;
  scalanative::support::DiagnosticEngine diagnostics;

  std::string invalid = "object Bad ";
  invalid.push_back(static_cast<char>(0xFF));
  invalid += '\n';
  [[maybe_unused]] scalanative::support::SourceId badId =
      sources.addVirtualFile("Bad.scala", invalid, diagnostics);

  if (int code = expect(diagnostics.errorCount() == 1,
                        "invalid UTF-8 did not produce one source error")) {
    return code;
  }

  std::ostringstream rendered;
  diagnostics.render(sources, rendered);
  if (int code = expect(contains(rendered.str(), "invalid UTF-8 sequence"),
                        "invalid UTF-8 diagnostic text was not rendered")) {
    return code;
  }

  scalanative::support::DiagnosticEngine buildDiagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("Bad.scala", invalid, {}, buildDiagnostics);
  return expect(!result.ok && contains(result.diagnosticsText, "invalid UTF-8"),
                "build driver did not stop on invalid UTF-8");
}

int smokeDiagnosticFixIts() {
  scalanative::support::SourceManager sources;
  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::support::SourceId id =
      sources.addVirtualFile("Fix.scala", "val bad = 1\n", diagnostics);

  scalanative::support::SourceSpan span{id, 4, 3};
  diagnostics.error(span, "bad identifier",
                    {scalanative::support::FixIt{span, "good"}});

  std::ostringstream rendered;
  diagnostics.render(sources, rendered);
  if (int code = expect(contains(rendered.str(), "^~~"),
                        "diagnostic underline did not cover the span")) {
    return code;
  }
  return expect(contains(rendered.str(), "fix-it: 1:5: replace with \"good\""),
                "diagnostic fix-it was not rendered");
}

int smokeLexerScalaTokens() {
  using scalanative::frontend::Token;
  using scalanative::frontend::TokenKind;

  constexpr const char* source = R"(package demo
object Main {
  override def token = 3
  val `type` = 0x2A
  val dec = 1.5e2
  val ch = 'x'
  val sym = 'name
  val ok = true
  val no = false
  val equal = 1 == 1
  val text = """hello
world"""
  val sum = 1 +
    2
  val selected = demo
    .value
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::support::SourceManager sources;
  scalanative::support::SourceId id =
      sources.addVirtualFile("Lexer.scala", source, diagnostics);
  scalanative::frontend::Lexer lexer(sources, diagnostics);
  std::vector<Token> tokens = lexer.lex(id);

  if (int code = expect(!diagnostics.hasErrors(),
                        "lexer reported errors for Scala token smoke source")) {
    return code;
  }

  bool sawInferredSemicolon = false;
  bool sawBacktickIdentifier = false;
  bool sawHexInteger = false;
  bool sawFloating = false;
  bool sawChar = false;
  bool sawSymbol = false;
  bool sawTripleString = false;
  bool sawKeywordTrueFalseSurface = false;
  bool sawKeywordOverride = false;
  bool sawEqualityOperator = false;
  bool sawVirtualSemicolon = false;

  for (const Token& token : tokens) {
    sawInferredSemicolon = sawInferredSemicolon ||
                           (token.kind == TokenKind::Semicolon && token.text == "\n");
    sawVirtualSemicolon =
        sawVirtualSemicolon || (token.kind == TokenKind::Semicolon && token.isVirtual);
    sawBacktickIdentifier =
        sawBacktickIdentifier ||
        (token.kind == TokenKind::Identifier && token.text == "type");
    sawHexInteger = sawHexInteger ||
                    (token.kind == TokenKind::IntegerLiteral && token.text == "0x2A");
    sawFloating = sawFloating ||
                  (token.kind == TokenKind::FloatingLiteral && token.text == "1.5e2");
    sawChar = sawChar || (token.kind == TokenKind::CharLiteral && token.text == "'x'");
    sawSymbol =
        sawSymbol || (token.kind == TokenKind::SymbolLiteral && token.text == "'name");
    sawTripleString =
        sawTripleString || (token.kind == TokenKind::StringLiteral &&
                            contains(token.text, "\"\"\"hello\nworld\"\"\""));
    sawKeywordTrueFalseSurface = sawKeywordTrueFalseSurface ||
                                 token.kind == TokenKind::KeywordTrue ||
                                 token.kind == TokenKind::KeywordFalse;
    sawKeywordOverride = sawKeywordOverride || token.kind == TokenKind::KeywordOverride;
    sawEqualityOperator = sawEqualityOperator ||
                          (token.kind == TokenKind::Operator && token.text == "==");
  }

  if (int code =
          expect(sawInferredSemicolon, "lexer did not infer newline semicolons")) {
    return code;
  }
  if (int code = expect(sawVirtualSemicolon,
                        "lexer did not mark inferred semicolon as virtual")) {
    return code;
  }
  if (int code = expect(sawBacktickIdentifier,
                        "lexer did not parse backtick identifier content")) {
    return code;
  }
  if (int code = expect(sawHexInteger, "lexer did not parse hex integer")) {
    return code;
  }
  if (int code = expect(sawFloating, "lexer did not parse floating literal")) {
    return code;
  }
  if (int code = expect(sawChar, "lexer did not parse character literal")) {
    return code;
  }
  if (int code = expect(sawSymbol, "lexer did not parse symbol literal")) {
    return code;
  }
  if (int code =
          expect(sawTripleString, "lexer did not parse triple-quoted string literal")) {
    return code;
  }
  if (int code =
          expect(sawKeywordOverride, "lexer did not surface override as a keyword")) {
    return code;
  }
  if (int code = expect(sawEqualityOperator,
                        "lexer did not preserve equality as one operator token")) {
    return code;
  }

  for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
    if (tokens[i].kind == TokenKind::Operator && tokens[i].text == "+") {
      if (int code = expect(tokens[i + 1].kind != TokenKind::Semicolon,
                            "lexer inserted semicolon after trailing operator")) {
        return code;
      }
    }
    if (tokens[i].kind == TokenKind::Dot) {
      if (int code = expect(i == 0 || tokens[i - 1].kind != TokenKind::Semicolon,
                            "lexer inserted semicolon before dotted selection")) {
        return code;
      }
    }
  }

  return expect(sawKeywordTrueFalseSurface,
                "lexer did not classify true/false keywords");
}

int smokeLexerTriviaAndInterpolation() {
  using scalanative::frontend::Token;
  using scalanative::frontend::TokenKind;
  using scalanative::frontend::TriviaKind;

  constexpr const char* source = R"(// leading object comment
object Main {
  /* leading val
     block comment */ val msg = s"hello $who ${1 + 2}"
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::support::SourceManager sources;
  scalanative::support::SourceId id =
      sources.addVirtualFile("Interpolation.scala", source, diagnostics);
  scalanative::frontend::Lexer lexer(sources, diagnostics);
  std::vector<Token> tokens = lexer.lex(id);

  if (int code = expect(!diagnostics.hasErrors(),
                        "lexer reported errors for interpolation smoke source")) {
    return code;
  }

  bool objectHasLineCommentTrivia = false;
  bool valHasBlockCommentTrivia = false;
  bool sawInterpolationStart = false;
  bool sawInterpolationTextPart = false;
  bool sawInterpolationIdentifier = false;
  bool sawInterpolationBlockStart = false;
  bool sawInterpolationBlockExpression = false;
  bool sawInterpolationBlockEnd = false;
  bool sawInterpolationEnd = false;

  for (const Token& token : tokens) {
    if (token.kind == TokenKind::KeywordObject) {
      for (const scalanative::frontend::Trivia& trivia : token.leadingTrivia) {
        objectHasLineCommentTrivia =
            objectHasLineCommentTrivia || trivia.kind == TriviaKind::LineComment;
      }
    }
    if (token.kind == TokenKind::KeywordVal) {
      for (const scalanative::frontend::Trivia& trivia : token.leadingTrivia) {
        valHasBlockCommentTrivia =
            valHasBlockCommentTrivia ||
            (trivia.kind == TriviaKind::BlockComment && trivia.containsNewline);
      }
    }

    sawInterpolationStart =
        sawInterpolationStart ||
        (token.kind == TokenKind::InterpolatedStringStart && token.text == "s\"");
    sawInterpolationTextPart =
        sawInterpolationTextPart ||
        (token.kind == TokenKind::InterpolatedStringPart && token.text == "hello ");
    sawInterpolationIdentifier =
        sawInterpolationIdentifier ||
        (token.kind == TokenKind::InterpolationIdentifier && token.text == "who");
    sawInterpolationBlockStart =
        sawInterpolationBlockStart || token.kind == TokenKind::InterpolationStart;
    sawInterpolationBlockExpression =
        sawInterpolationBlockExpression ||
        (token.kind == TokenKind::IntegerLiteral && token.text == "1") ||
        (token.kind == TokenKind::Operator && token.text == "+") ||
        (token.kind == TokenKind::IntegerLiteral && token.text == "2");
    sawInterpolationBlockEnd =
        sawInterpolationBlockEnd || token.kind == TokenKind::InterpolationEnd;
    sawInterpolationEnd =
        sawInterpolationEnd || token.kind == TokenKind::InterpolatedStringEnd;
  }

  if (int code = expect(objectHasLineCommentTrivia,
                        "object token did not keep leading line comment trivia")) {
    return code;
  }
  if (int code = expect(valHasBlockCommentTrivia,
                        "val token did not keep leading block comment trivia")) {
    return code;
  }
  if (int code = expect(sawInterpolationStart,
                        "lexer did not emit interpolated string start token")) {
    return code;
  }
  if (int code = expect(sawInterpolationTextPart,
                        "lexer did not emit interpolated string text part")) {
    return code;
  }
  if (int code = expect(sawInterpolationIdentifier,
                        "lexer did not emit interpolation identifier")) {
    return code;
  }
  if (int code = expect(sawInterpolationBlockStart,
                        "lexer did not emit interpolation block start")) {
    return code;
  }
  if (int code = expect(sawInterpolationBlockExpression,
                        "lexer did not tokenize interpolation block expression")) {
    return code;
  }
  if (int code = expect(sawInterpolationBlockEnd,
                        "lexer did not emit interpolation block end")) {
    return code;
  }
  return expect(sawInterpolationEnd,
                "lexer did not emit interpolated string end token");
}

int smokeLexerLiteralDiagnostics() {
  constexpr const char* source =
      "object Bad { val a = 1bad val b = \"bad\\q\" val c = '\\q' }\n";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::support::SourceManager sources;
  scalanative::support::SourceId id =
      sources.addVirtualFile("BadLiterals.scala", source, diagnostics);
  scalanative::frontend::Lexer lexer(sources, diagnostics);
  [[maybe_unused]] std::vector<scalanative::frontend::Token> tokens = lexer.lex(id);

  if (int code = expect(diagnostics.errorCount() == 3,
                        "literal validation did not report expected errors")) {
    return code;
  }

  std::ostringstream rendered;
  diagnostics.render(sources, rendered);
  if (int code = expect(contains(rendered.str(), "invalid numeric literal suffix"),
                        "numeric suffix diagnostic was not rendered")) {
    return code;
  }
  if (int code = expect(contains(rendered.str(), "unknown string escape sequence"),
                        "string escape diagnostic was not rendered")) {
    return code;
  }
  return expect(contains(rendered.str(), "unknown character escape sequence"),
                "character escape diagnostic was not rendered");
}

int smokeParserMinimalAst() {
  using scalanative::frontend::AstDeclaration;
  using scalanative::frontend::AstDeclarationKind;
  using scalanative::frontend::AstExpressionKind;

  constexpr const char* source = R"(package demo.parser

object Main {
  def add(a: Int, b: Int): Int = a + b
  val called = add(1, 2)
  val selected = demo.value
  def blocky = {
    val local: Int = 1
    local
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::support::SourceManager sources;
  scalanative::support::SourceId id =
      sources.addVirtualFile("Parser.scala", source, diagnostics);
  scalanative::frontend::Lexer lexer(sources, diagnostics);
  std::vector<scalanative::frontend::Token> tokens = lexer.lex(id);
  scalanative::frontend::Parser parser(std::move(tokens), diagnostics);
  scalanative::frontend::AstModule module = parser.parse();

  if (int code = expect(!diagnostics.hasErrors(),
                        "parser reported errors for minimal AST smoke source")) {
    std::ostringstream rendered;
    diagnostics.render(sources, rendered);
    std::cerr << rendered.str();
    return code;
  }

  if (int code = expect(module.packageName == "demo.parser",
                        "parser did not parse qualified package name")) {
    return code;
  }
  if (int code = expect(module.declarations.size() == 2,
                        "parser did not keep package and object declarations")) {
    return code;
  }

  const AstDeclaration& object = module.declarations[1];
  if (int code =
          expect(object.kind == AstDeclarationKind::Object && object.name == "Main",
                 "parser did not parse object declaration")) {
    return code;
  }
  if (int code = expect(object.members.size() == 4,
                        "parser did not parse object member declarations")) {
    return code;
  }

  const AstDeclaration& add = object.members[0];
  if (int code = expect(add.kind == AstDeclarationKind::Def && add.name == "add",
                        "parser did not parse method declaration")) {
    return code;
  }
  if (int code = expect(add.parameters.size() == 2 && add.parameters[0] == "a: Int" &&
                            add.parameters[1] == "b: Int",
                        "parser did not parse method parameters")) {
    return code;
  }
  if (int code = expect(add.declaredType == "Int",
                        "parser did not parse method return type")) {
    return code;
  }
  if (int code = expect(
          add.hasInitializer && add.initializer.kind == AstExpressionKind::Binary &&
              add.initializer.text == "+" && add.initializer.children.size() == 2,
          "parser did not parse binary method initializer")) {
    return code;
  }

  const AstDeclaration& called = object.members[1];
  if (int code =
          expect(called.kind == AstDeclarationKind::Val && called.name == "called",
                 "parser did not parse val declaration")) {
    return code;
  }
  if (int code = expect(called.hasInitializer &&
                            called.initializer.kind == AstExpressionKind::Call &&
                            called.initializer.children.size() == 3,
                        "parser did not parse call initializer")) {
    return code;
  }

  const AstDeclaration& selected = object.members[2];
  if (int code = expect(selected.hasInitializer &&
                            selected.initializer.kind == AstExpressionKind::Select &&
                            selected.initializer.text == "value",
                        "parser did not parse selected initializer")) {
    return code;
  }

  const AstDeclaration& blocky = object.members[3];
  if (int code = expect(blocky.hasInitializer &&
                            blocky.initializer.kind == AstExpressionKind::Block &&
                            blocky.initializer.children.size() == 2,
                        "parser did not parse block expression initializer")) {
    return code;
  }
  if (int code = expect(blocky.initializer.children[0].kind ==
                                AstExpressionKind::LocalDeclaration &&
                            blocky.initializer.children[0].text == "local" &&
                            blocky.initializer.children[0].declaredType == "Int",
                        "parser did not preserve local declaration in block")) {
    return code;
  }

  std::string debug = scalanative::frontend::debugString(module);
  return expect(contains(debug, "binary +") && contains(debug, "call") &&
                    contains(debug, "local-declaration local"),
                "AST debug string did not include parsed expressions");
}

int smokeAstValidationAndTypecheck() {
  using scalanative::frontend::SimpleTypeKind;

  constexpr const char* source = R"(package demo.semantic

object Config {
  val answer: Int = 42
}

object Main {
  val i: Int = 1
  val d = 1.5
  val s = "hello" + " world"
  def add(a: Int, b: Int): Int = a + b
  val called = add(1, 2)
  val selected = Config.answer
  def blocky = {
    val local = 1
    local
  }
}
)";

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("Semantic.scala", source, {}, diagnostics);

  if (int code = expect(result.ok, "semantic smoke build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code =
          expect(contains(result.nirText, "define @demo.semantic.Main.i : ()Int"),
                 "NIR did not use declared Int type for val i")) {
    return code;
  }
  if (int code =
          expect(contains(result.nirText, "define @demo.semantic.Main.d : ()Double"),
                 "NIR did not use inferred Double type for val d")) {
    return code;
  }
  if (int code =
          expect(contains(result.nirText, "define @demo.semantic.Main.s : ()String"),
                 "NIR did not infer String type for string concatenation")) {
    return code;
  }
  if (int code =
          expect(contains(result.nirText, "define @demo.semantic.Main.called : ()Int"),
                 "NIR did not infer Int type for member call")) {
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "define @demo.semantic.Main.selected : ()Int"),
          "NIR did not infer Int type for selected object member")) {
    return code;
  }
  if (int code =
          expect(contains(result.nirText, "define @demo.semantic.Main.blocky : ()Int"),
                 "NIR did not infer Int type for block-local expression")) {
    return code;
  }

  scalanative::support::DiagnosticEngine parseDiagnostics;
  scalanative::support::SourceManager sources;
  scalanative::support::SourceId id =
      sources.addVirtualFile("Semantic.scala", source, parseDiagnostics);
  scalanative::frontend::Lexer lexer(sources, parseDiagnostics);
  std::vector<scalanative::frontend::Token> tokens = lexer.lex(id);
  scalanative::frontend::Parser parser(std::move(tokens), parseDiagnostics);
  scalanative::frontend::AstModule module = parser.parse();
  scalanative::frontend::Typechecker typechecker(parseDiagnostics);
  scalanative::frontend::TypedModule typed = typechecker.typecheck(module);

  const scalanative::frontend::TypedDeclaration& object = typed.declarations[2];
  auto expressionType = [&](const scalanative::frontend::AstExpression& expression) {
    for (auto info = typed.expressionTypes.rbegin();
         info != typed.expressionTypes.rend(); ++info) {
      if (info->span.source == expression.span.source &&
          info->span.start == expression.span.start &&
          info->span.length == expression.span.length) {
        return info->type;
      }
    }
    return scalanative::frontend::TypeInfo{};
  };
  if (int code = expect(object.members[0].inferredType.kind == SimpleTypeKind::Int,
                        "typechecker did not preserve declared Int type")) {
    return code;
  }
  if (int code = expect(object.members[1].inferredType.kind == SimpleTypeKind::Double,
                        "typechecker did not infer Double type")) {
    return code;
  }
  if (int code = expect(object.members[2].inferredType.kind == SimpleTypeKind::String,
                        "typechecker did not infer String type")) {
    return code;
  }
  if (int code = expect(object.members[4].inferredType.kind == SimpleTypeKind::Int,
                        "typechecker did not infer member call return type")) {
    return code;
  }
  if (int code = expect(object.members[5].inferredType.kind == SimpleTypeKind::Int,
                        "typechecker did not infer selected object member type")) {
    return code;
  }
  if (int code = expect(object.members[6].inferredType.kind == SimpleTypeKind::Int,
                        "typechecker did not infer block-local expression type")) {
    return code;
  }
  const scalanative::frontend::AstExpression& call = object.members[4].initializer;
  const scalanative::frontend::AstExpression& block = object.members[6].initializer;
  return expect(expressionType(call).kind == SimpleTypeKind::Int &&
                    expressionType(block.children.back()).kind == SimpleTypeKind::Int &&
                    expressionType(block.children.front().children.front()).kind ==
                        SimpleTypeKind::Int,
                "typechecker did not publish recursive typed-expression annotations");
}

int smokeSemanticDiagnostics() {
  constexpr const char* duplicateSource = R"(package demo.bad
object Main {
  val x = 1
  val x = 2
}
)";

  scalanative::support::DiagnosticEngine duplicateDiagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult duplicate =
      driver.buildSource("Duplicate.scala", duplicateSource, {}, duplicateDiagnostics);
  if (int code = expect(
          !duplicate.ok && contains(duplicate.diagnosticsText, "duplicate declaration"),
          "duplicate declaration did not fail AST validation")) {
    return code;
  }

  constexpr const char* mismatchSource = R"(package demo.bad
object Main {
  val x: Int = "nope"
}
)";

  scalanative::support::DiagnosticEngine mismatchDiagnostics;
  scalanative::tools::build::BuildResult mismatch =
      driver.buildSource("Mismatch.scala", mismatchSource, {}, mismatchDiagnostics);
  if (int code =
          expect(!mismatch.ok && contains(mismatch.diagnosticsText,
                                          "does not conform to declared type Int"),
                 "type mismatch did not fail typecheck")) {
    return code;
  }

  constexpr const char* localObjectMismatchSource = R"(package demo.bad
class BaseScore
class OtherScore
object Main {
  def bad = {
    val score: BaseScore = new OtherScore()
    score
  }
}
)";

  scalanative::support::DiagnosticEngine localObjectMismatchDiagnostics;
  scalanative::tools::build::BuildResult localObjectMismatch =
      driver.buildSource("LocalObjectMismatch.scala", localObjectMismatchSource, {},
                         localObjectMismatchDiagnostics);
  if (int code =
          expect(!localObjectMismatch.ok &&
                     contains(localObjectMismatch.diagnosticsText,
                              "does not conform to declared type demo.bad.BaseScore"),
                 "local declared object type mismatch did not fail typecheck")) {
    return code;
  }

  constexpr const char* unresolvedSource = R"(package demo.bad
object Main {
  val x: Int = missing
}
)";

  scalanative::support::DiagnosticEngine unresolvedDiagnostics;
  scalanative::tools::build::BuildResult unresolved = driver.buildSource(
      "Unresolved.scala", unresolvedSource, {}, unresolvedDiagnostics);
  if (int code = expect(!unresolved.ok && contains(unresolved.diagnosticsText,
                                                   "unresolved identifier: missing"),
                        "unresolved identifier did not fail typecheck")) {
    return code;
  }

  constexpr const char* localOrderSource = R"(package demo.bad
object Main {
  def x = {
    local
    val local = 1
  }
}
)";

  scalanative::support::DiagnosticEngine localOrderDiagnostics;
  scalanative::tools::build::BuildResult localOrder = driver.buildSource(
      "LocalOrder.scala", localOrderSource, {}, localOrderDiagnostics);
  if (int code =
          expect(!localOrder.ok && contains(localOrder.diagnosticsText,
                                            "unresolved identifier: local"),
                 "block-local identifier before declaration did not fail typecheck")) {
    return code;
  }

  constexpr const char* assignLocalValSource = R"(package demo.bad
object Main {
  def x = {
    val local = 1
    local = 2
  }
}
)";

  scalanative::support::DiagnosticEngine assignLocalValDiagnostics;
  scalanative::tools::build::BuildResult assignLocalVal = driver.buildSource(
      "AssignLocalVal.scala", assignLocalValSource, {}, assignLocalValDiagnostics);
  if (int code = expect(!assignLocalVal.ok &&
                            contains(assignLocalVal.diagnosticsText,
                                     "cannot assign to immutable value: local"),
                        "assignment to local val did not fail typecheck")) {
    return code;
  }

  constexpr const char* unresolvedMemberSource = R"(package demo.bad
object Config {
  val answer: Int = 42
}
object Main {
  val x: Int = Config.missing
}
)";

  scalanative::support::DiagnosticEngine memberDiagnostics;
  scalanative::tools::build::BuildResult member = driver.buildSource(
      "UnresolvedMember.scala", unresolvedMemberSource, {}, memberDiagnostics);
  if (int code = expect(!member.ok &&
                            contains(member.diagnosticsText,
                                     "unresolved member: missing on demo.bad.Config"),
                        "unresolved selected member did not fail typecheck")) {
    return code;
  }

  constexpr const char* unresolvedParentSource = R"(package demo.bad
class Broken extends Missing
)";

  scalanative::support::DiagnosticEngine unresolvedParentDiagnostics;
  scalanative::tools::build::BuildResult unresolvedParent =
      driver.buildSource("UnresolvedParent.scala", unresolvedParentSource, {},
                         unresolvedParentDiagnostics);
  if (int code =
          expect(!unresolvedParent.ok && contains(unresolvedParent.diagnosticsText,
                                                  "unresolved parent type: Missing"),
                 "unresolved parent type did not fail typecheck")) {
    return code;
  }

  constexpr const char* objectParentSource = R"(package demo.bad
object Config
class Broken extends Config
)";

  scalanative::support::DiagnosticEngine objectParentDiagnostics;
  scalanative::tools::build::BuildResult objectParent = driver.buildSource(
      "ObjectParent.scala", objectParentSource, {}, objectParentDiagnostics);
  if (int code = expect(!objectParent.ok &&
                            contains(objectParent.diagnosticsText,
                                     "cannot extend non-class or non-trait: Config"),
                        "object parent did not fail inheritance typecheck")) {
    return code;
  }

  constexpr const char* traitExtendsClassSource = R"(package demo.bad
class Base
trait Broken extends Base
)";

  scalanative::support::DiagnosticEngine traitExtendsClassDiagnostics;
  scalanative::tools::build::BuildResult traitExtendsClass =
      driver.buildSource("TraitExtendsClass.scala", traitExtendsClassSource, {},
                         traitExtendsClassDiagnostics);
  if (int code = expect(!traitExtendsClass.ok &&
                            contains(traitExtendsClass.diagnosticsText,
                                     "trait Broken can only extend another trait"),
                        "trait extending class did not fail inheritance typecheck")) {
    return code;
  }

  constexpr const char* missingAbstractMethodSource = R"(package demo.bad
trait Named {
  def name: String
}
class Broken extends Named
)";

  scalanative::support::DiagnosticEngine missingAbstractMethodDiagnostics;
  scalanative::tools::build::BuildResult missingAbstractMethod =
      driver.buildSource("MissingAbstractMethod.scala", missingAbstractMethodSource, {},
                         missingAbstractMethodDiagnostics);
  if (int code =
          expect(!missingAbstractMethod.ok &&
                     contains(missingAbstractMethod.diagnosticsText,
                              "class Broken must implement abstract method name"),
                 "missing abstract trait implementation did not fail typecheck")) {
    return code;
  }

  constexpr const char* missingAbstractValueSource = R"(package demo.bad
trait Named {
  val name: String
}
class Broken extends Named
)";

  scalanative::support::DiagnosticEngine missingAbstractValueDiagnostics;
  scalanative::tools::build::BuildResult missingAbstractValue =
      driver.buildSource("MissingAbstractValue.scala", missingAbstractValueSource, {},
                         missingAbstractValueDiagnostics);
  if (int code = expect(!missingAbstractValue.ok &&
                            contains(missingAbstractValue.diagnosticsText,
                                     "class Broken must implement abstract value name"),
                        "missing abstract trait value did not fail typecheck")) {
    return code;
  }

  constexpr const char* mismatchedAbstractValueSource = R"(package demo.bad
trait Scored {
  val score: Int
}
class Broken extends Scored {
  override val score: String = "bad"
}
)";

  scalanative::support::DiagnosticEngine mismatchedAbstractValueDiagnostics;
  scalanative::tools::build::BuildResult mismatchedAbstractValue =
      driver.buildSource("MismatchedAbstractValue.scala", mismatchedAbstractValueSource,
                         {}, mismatchedAbstractValueDiagnostics);
  if (int code = expect(
          !mismatchedAbstractValue.ok &&
              contains(mismatchedAbstractValue.diagnosticsText,
                       "override score value type String does not match inherited "
                       "value type Int"),
          "abstract trait value type mismatch did not fail typecheck")) {
    return code;
  }

  constexpr const char* plainConstructorParameterSource = R"(package demo.bad
trait Named {
  val name: String
}
class Broken(name: String) extends Named
)";

  scalanative::support::DiagnosticEngine plainConstructorParameterDiagnostics;
  scalanative::tools::build::BuildResult plainConstructorParameter = driver.buildSource(
      "PlainConstructorParameter.scala", plainConstructorParameterSource, {},
      plainConstructorParameterDiagnostics);
  if (int code =
          expect(!plainConstructorParameter.ok &&
                     contains(plainConstructorParameter.diagnosticsText,
                              "class Broken must implement abstract value name with a "
                              "val or var constructor parameter or class member"),
                 "plain constructor parameter satisfied an abstract trait value")) {
    return code;
  }

  constexpr const char* mismatchedConstructorValueSource = R"(package demo.bad
trait Scored {
  val score: Int
}
class Broken(val score: String) extends Scored
)";

  scalanative::support::DiagnosticEngine mismatchedConstructorValueDiagnostics;
  scalanative::tools::build::BuildResult mismatchedConstructorValue =
      driver.buildSource("MismatchedConstructorValue.scala",
                         mismatchedConstructorValueSource, {},
                         mismatchedConstructorValueDiagnostics);
  if (int code =
          expect(!mismatchedConstructorValue.ok &&
                     contains(mismatchedConstructorValue.diagnosticsText,
                              "constructor value score type String does not match "
                              "inherited value type Int"),
                 "constructor val type mismatch did not fail typecheck")) {
    return code;
  }

  constexpr const char* missingTransitiveAbstractMethodSource =
      R"(package demo.bad
trait Root {
  def value: Int
}
trait Middle extends Root
class Broken extends Middle
)";

  scalanative::support::DiagnosticEngine missingTransitiveAbstractMethodDiagnostics;
  scalanative::tools::build::BuildResult missingTransitiveAbstractMethod =
      driver.buildSource("MissingTransitiveAbstractMethod.scala",
                         missingTransitiveAbstractMethodSource, {},
                         missingTransitiveAbstractMethodDiagnostics);
  if (int code =
          expect(!missingTransitiveAbstractMethod.ok &&
                     contains(missingTransitiveAbstractMethod.diagnosticsText,
                              "class Broken must implement abstract method value"),
                 "missing transitive abstract trait implementation did not fail "
                 "typecheck")) {
    return code;
  }

  constexpr const char* multipleClassParentsSource = R"(package demo.bad
class First
class Second
class Broken extends First with Second
)";

  scalanative::support::DiagnosticEngine multipleClassParentsDiagnostics;
  scalanative::tools::build::BuildResult multipleClassParents =
      driver.buildSource("MultipleClassParents.scala", multipleClassParentsSource, {},
                         multipleClassParentsDiagnostics);
  if (int code =
          expect(!multipleClassParents.ok &&
                     contains(multipleClassParents.diagnosticsText,
                              "multiple class parents are not supported: Broken"),
                 "multiple class inheritance did not fail typecheck")) {
    return code;
  }

  constexpr const char* cyclicInheritanceSource = R"(package demo.bad
trait FirstCycle extends SecondCycle
trait SecondCycle extends FirstCycle
)";

  scalanative::support::DiagnosticEngine cyclicInheritanceDiagnostics;
  scalanative::tools::build::BuildResult cyclicInheritance =
      driver.buildSource("CyclicInheritance.scala", cyclicInheritanceSource, {},
                         cyclicInheritanceDiagnostics);
  if (int code =
          expect(!cyclicInheritance.ok && contains(cyclicInheritance.diagnosticsText,
                                                   "cyclic inheritance involving"),
                 "cyclic source inheritance did not fail typecheck")) {
    return code;
  }

  constexpr const char* inconsistentLinearizationSource =
      R"(package demo.bad
trait RootX
trait RootY
trait OrderXY extends RootX with RootY
trait OrderYX extends RootY with RootX
class BrokenOrder extends OrderXY with OrderYX
)";

  scalanative::support::DiagnosticEngine inconsistentLinearizationDiagnostics;
  scalanative::tools::build::BuildResult inconsistentLinearization = driver.buildSource(
      "InconsistentLinearization.scala", inconsistentLinearizationSource, {},
      inconsistentLinearizationDiagnostics);
  if (int code =
          expect(!inconsistentLinearization.ok &&
                     contains(inconsistentLinearization.diagnosticsText,
                              "inconsistent parent linearization for BrokenOrder"),
                 "inconsistent source parent precedence did not fail typecheck")) {
    return code;
  }

  constexpr const char* lateClassParentSource = R"(package demo.bad
trait Named
class Base
class Broken extends Named with Base
)";

  scalanative::support::DiagnosticEngine lateClassParentDiagnostics;
  scalanative::tools::build::BuildResult lateClassParent = driver.buildSource(
      "LateClassParent.scala", lateClassParentSource, {}, lateClassParentDiagnostics);
  if (int code =
          expect(!lateClassParent.ok &&
                     contains(lateClassParent.diagnosticsText,
                              "class parent must be the first extends parent: Base"),
                 "class parent after trait mixin did not fail typecheck")) {
    return code;
  }

  constexpr const char* unrelatedQualifiedSuperSource = R"(package demo.bad
trait Parent {
  def value: Int = 1
}
trait Unrelated {
  def value: Int = 2
}
class Broken extends Parent {
  override def value: Int = super[Unrelated].value
}
)";

  scalanative::support::DiagnosticEngine unrelatedQualifiedSuperDiagnostics;
  scalanative::tools::build::BuildResult unrelatedQualifiedSuper =
      driver.buildSource("UnrelatedQualifiedSuper.scala", unrelatedQualifiedSuperSource,
                         {}, unrelatedQualifiedSuperDiagnostics);
  if (int code =
          expect(!unrelatedQualifiedSuper.ok &&
                     contains(unrelatedQualifiedSuper.diagnosticsText,
                              "qualified super type is not a direct parent: Unrelated"),
                 "qualified super call to an unrelated trait did not fail typecheck")) {
    return code;
  }

  constexpr const char* incompatibleInheritedMethodsSource =
      R"(package demo.bad
trait IntValue {
  def value(input: Int): Int = input
}
trait StringValue {
  def value(input: String): Int = 1
}
class Broken extends IntValue with StringValue
)";

  scalanative::support::DiagnosticEngine incompatibleInheritedMethodsDiagnostics;
  scalanative::tools::build::BuildResult incompatibleInheritedMethods =
      driver.buildSource("IncompatibleInheritedMethods.scala",
                         incompatibleInheritedMethodsSource, {},
                         incompatibleInheritedMethodsDiagnostics);
  if (int code =
          expect(!incompatibleInheritedMethods.ok &&
                     contains(incompatibleInheritedMethods.diagnosticsText,
                              "class Broken inherits incompatible method value"),
                 "incompatible inherited method signatures did not fail typecheck")) {
    return code;
  }

  constexpr const char* missingAbstractTypeSource = R"(package demo.bad
trait Carrier {
  type Element
}
class Broken extends Carrier
)";

  scalanative::support::DiagnosticEngine missingAbstractTypeDiagnostics;
  scalanative::tools::build::BuildResult missingAbstractType =
      driver.buildSource("MissingAbstractType.scala", missingAbstractTypeSource, {},
                         missingAbstractTypeDiagnostics);
  if (int code = expect(
          !missingAbstractType.ok &&
              contains(missingAbstractType.diagnosticsText,
                       "class Broken must implement abstract type member Element"),
          "missing abstract type member implementation did not fail typecheck")) {
    return code;
  }

  constexpr const char* nonTypeAbstractImplementationSource =
      R"(package demo.bad
trait Carrier {
  type Element
}
class Broken extends Carrier {
  val Element: Int = 1
}
)";

  scalanative::support::DiagnosticEngine nonTypeAbstractImplementationDiagnostics;
  scalanative::tools::build::BuildResult nonTypeAbstractImplementation =
      driver.buildSource("NonTypeAbstractImplementation.scala",
                         nonTypeAbstractImplementationSource, {},
                         nonTypeAbstractImplementationDiagnostics);
  if (int code =
          expect(!nonTypeAbstractImplementation.ok &&
                     contains(nonTypeAbstractImplementation.diagnosticsText,
                              "cannot implement inherited type member Element with a "
                              "non-type declaration"),
                 "value declaration satisfied an abstract type member")) {
    return code;
  }

  constexpr const char* incompatibleInheritedTypeAliasesSource =
      R"(package demo.bad
trait IntCarrier {
  type Element = Int
}
trait StringCarrier {
  type Element = String
}
class Broken extends IntCarrier with StringCarrier
)";

  scalanative::support::DiagnosticEngine incompatibleInheritedTypeAliasesDiagnostics;
  scalanative::tools::build::BuildResult incompatibleInheritedTypeAliases =
      driver.buildSource("IncompatibleInheritedTypeAliases.scala",
                         incompatibleInheritedTypeAliasesSource, {},
                         incompatibleInheritedTypeAliasesDiagnostics);
  if (int code = expect(
          !incompatibleInheritedTypeAliases.ok &&
              contains(incompatibleInheritedTypeAliases.diagnosticsText,
                       "class Broken inherits incompatible type aliases for "
                       "Element: String") &&
              contains(incompatibleInheritedTypeAliases.diagnosticsText, "and Int"),
          "incompatible inherited type aliases did not fail typecheck")) {
    return code;
  }

  constexpr const char* topLevelTypeSource = R"(package demo.bad
type Element = Int
)";

  scalanative::support::DiagnosticEngine topLevelTypeDiagnostics;
  scalanative::tools::build::BuildResult topLevelType = driver.buildSource(
      "TopLevelType.scala", topLevelTypeSource, {}, topLevelTypeDiagnostics);
  if (int code = expect(!topLevelType.ok &&
                            contains(topLevelType.diagnosticsText,
                                     "type declarations are only supported as members"),
                        "top-level type alias escaped the member-only MVP boundary")) {
    return code;
  }

  constexpr const char* mismatchedSpecializedMethodSource =
      R"(package demo.bad
trait Codec {
  type Value
  def echo(value: Value): Value
}
trait Broken extends Codec {
  type Value = Int
  override def echo(value: String): String = value
}
)";

  scalanative::support::DiagnosticEngine mismatchedSpecializedMethodDiagnostics;
  scalanative::tools::build::BuildResult mismatchedSpecializedMethod =
      driver.buildSource("MismatchedSpecializedMethod.scala",
                         mismatchedSpecializedMethodSource, {},
                         mismatchedSpecializedMethodDiagnostics);
  if (int code = expect(
          !mismatchedSpecializedMethod.ok &&
              contains(mismatchedSpecializedMethod.diagnosticsText,
                       "override echo parameter 0 type String does not match "
                       "inherited parameter type Int") &&
              contains(mismatchedSpecializedMethod.diagnosticsText,
                       "override echo return type String does not match inherited "
                       "return type Int"),
          "abstract type alias was not substituted during override checks")) {
    return code;
  }

  constexpr const char* inheritedAbstractTypeDefaultSource =
      R"(package demo.bad
trait Codec {
  type Value
  def echo(value: Value): Value = value
}
class Broken extends Codec {
  type Value = Int
}
object Main {
  def main = {
    val codec: Broken = new Broken()
    println(codec.echo(11))
  }
}
)";

  scalanative::support::DiagnosticEngine inheritedAbstractTypeDefaultDiagnostics;
  scalanative::tools::build::BuildResult inheritedAbstractTypeDefault =
      driver.buildSource("InheritedAbstractTypeDefault.scala",
                         inheritedAbstractTypeDefaultSource, {},
                         inheritedAbstractTypeDefaultDiagnostics);
  if (int code =
          expect(inheritedAbstractTypeDefault.ok &&
                     contains(inheritedAbstractTypeDefault.nirText,
                              "define @demo.bad.Codec.echo : "
                              "(demo.bad.Codec,Object)Object") &&
                     contains(inheritedAbstractTypeDefault.nirText,
                              "call %codec.echo(box[Int](11))") &&
                     contains(inheritedAbstractTypeDefault.nirText,
                              "unbox[Int](call %codec.echo"),
                 "inherited abstract-type default did not preserve its erased ABI")) {
    return code;
  }

  constexpr const char* unresolvedRuntimeTypeSource = R"(package demo.bad
trait Codec {
  type Value
}
class Broken extends Codec {
  def echo(value: Value): Value = value
}
)";

  scalanative::support::DiagnosticEngine unresolvedRuntimeTypeDiagnostics;
  scalanative::tools::build::BuildResult unresolvedRuntimeType =
      driver.buildSource("UnresolvedRuntimeType.scala", unresolvedRuntimeTypeSource, {},
                         unresolvedRuntimeTypeDiagnostics);
  if (int code =
          expect(!unresolvedRuntimeType.ok &&
                     contains(unresolvedRuntimeType.diagnosticsText,
                              "class Broken must implement abstract type member Value"),
                 "concrete class retained an unresolved abstract type member")) {
    return code;
  }

  constexpr const char* invalidBoundAliasSource = R"(package demo.bad
class Base
class Other
trait Box {
  type Item <: Base
}
class Broken extends Box {
  type Item = Other
}
)";

  scalanative::support::DiagnosticEngine invalidBoundAliasDiagnostics;
  scalanative::tools::build::BuildResult invalidBoundAlias =
      driver.buildSource("InvalidBoundAlias.scala", invalidBoundAliasSource, {},
                         invalidBoundAliasDiagnostics);
  if (int code =
          expect(!invalidBoundAlias.ok &&
                     contains(invalidBoundAlias.diagnosticsText,
                              "type member Item alias target demo.bad.Other does not "
                              "conform to inherited upper bound demo.bad.Base"),
                 "unrelated alias satisfied an upper-bounded type member")) {
    return code;
  }

  constexpr const char* invalidBoundRefinementSource = R"(package demo.bad
class Base
class Other
trait Root {
  type Item <: Base
}
trait Broken extends Root {
  override type Item <: Other
}
)";

  scalanative::support::DiagnosticEngine invalidBoundRefinementDiagnostics;
  scalanative::tools::build::BuildResult invalidBoundRefinement =
      driver.buildSource("InvalidBoundRefinement.scala", invalidBoundRefinementSource,
                         {}, invalidBoundRefinementDiagnostics);
  if (int code =
          expect(!invalidBoundRefinement.ok &&
                     contains(invalidBoundRefinement.diagnosticsText,
                              "type member Item upper bound demo.bad.Other does not "
                              "conform to inherited upper bound demo.bad.Base"),
                 "abstract type refinement weakened its inherited upper bound")) {
    return code;
  }

  constexpr const char* conflictingInheritedBoundsSource =
      R"(package demo.bad
class Base
class Left extends Base
class Right extends Base
trait LeftBox {
  type Item <: Left
}
trait RightBox {
  type Item <: Right
}
trait Broken extends LeftBox with RightBox
)";

  scalanative::support::DiagnosticEngine conflictingInheritedBoundsDiagnostics;
  scalanative::tools::build::BuildResult conflictingInheritedBounds =
      driver.buildSource("ConflictingInheritedBounds.scala",
                         conflictingInheritedBoundsSource, {},
                         conflictingInheritedBoundsDiagnostics);
  if (int code = expect(!conflictingInheritedBounds.ok &&
                            contains(conflictingInheritedBounds.diagnosticsText,
                                     "trait Broken inherits type member Item") &&
                            contains(conflictingInheritedBounds.diagnosticsText,
                                     "does not conform to inherited upper bound "
                                     "demo.bad.Left"),
                        "incompatible inherited upper bounds did not fail typecheck")) {
    return code;
  }

  constexpr const char* inconsistentTypeIntervalSource =
      R"(package demo.bad
class Lower
class Upper
trait Broken {
  type Item >: Lower <: Upper
}
)";

  scalanative::support::DiagnosticEngine inconsistentTypeIntervalDiagnostics;
  scalanative::tools::build::BuildResult inconsistentTypeInterval = driver.buildSource(
      "InconsistentTypeInterval.scala", inconsistentTypeIntervalSource, {},
      inconsistentTypeIntervalDiagnostics);
  if (int code =
          expect(!inconsistentTypeInterval.ok &&
                     contains(inconsistentTypeInterval.diagnosticsText,
                              "type member Item lower bound demo.bad.Lower does not "
                              "conform to upper bound demo.bad.Upper"),
                 "inconsistent type-member interval did not fail typecheck")) {
    return code;
  }

  constexpr const char* aliasBelowLowerBoundSource = R"(package demo.bad
class Upper
class Middle extends Upper
class Lower extends Middle
class Other extends Upper
trait Box {
  type Item >: Lower <: Upper
}
class Broken extends Box {
  type Item = Other
}
)";

  scalanative::support::DiagnosticEngine aliasBelowLowerBoundDiagnostics;
  scalanative::tools::build::BuildResult aliasBelowLowerBound =
      driver.buildSource("AliasBelowLowerBound.scala", aliasBelowLowerBoundSource, {},
                         aliasBelowLowerBoundDiagnostics);
  if (int code =
          expect(!aliasBelowLowerBound.ok &&
                     contains(aliasBelowLowerBound.diagnosticsText,
                              "type member Item alias target demo.bad.Other does not "
                              "preserve inherited lower bound demo.bad.Lower"),
                 "alias outside a type member's lower bound passed typecheck")) {
    return code;
  }

  constexpr const char* aliasAboveUpperBoundSource = R"(package demo.bad
class Wider
class Upper extends Wider
class Lower extends Upper
trait Box {
  type Item >: Lower <: Upper
}
class Broken extends Box {
  type Item = Wider
}
)";

  scalanative::support::DiagnosticEngine aliasAboveUpperBoundDiagnostics;
  scalanative::tools::build::BuildResult aliasAboveUpperBound =
      driver.buildSource("AliasAboveUpperBound.scala", aliasAboveUpperBoundSource, {},
                         aliasAboveUpperBoundDiagnostics);
  if (int code =
          expect(!aliasAboveUpperBound.ok &&
                     contains(aliasAboveUpperBound.diagnosticsText,
                              "type member Item alias target demo.bad.Wider does not "
                              "conform to inherited upper bound demo.bad.Upper"),
                 "alias outside a type member's upper bound passed typecheck")) {
    return code;
  }

  constexpr const char* weakenedLowerRefinementSource = R"(package demo.bad
class Upper
class Middle extends Upper
class Lower extends Middle
trait Root {
  type Item >: Middle <: Upper
}
trait Broken extends Root {
  override type Item >: Lower
}
)";

  scalanative::support::DiagnosticEngine weakenedLowerRefinementDiagnostics;
  scalanative::tools::build::BuildResult weakenedLowerRefinement =
      driver.buildSource("WeakenedLowerRefinement.scala", weakenedLowerRefinementSource,
                         {}, weakenedLowerRefinementDiagnostics);
  if (int code =
          expect(!weakenedLowerRefinement.ok &&
                     contains(weakenedLowerRefinement.diagnosticsText,
                              "type member Item lower bound demo.bad.Lower does not "
                              "preserve inherited lower bound demo.bad.Middle"),
                 "abstract type refinement widened its inherited lower bound")) {
    return code;
  }

  constexpr const char* missingInheritedSetterSource = R"(package demo.bad
trait MutableValue {
  var value: Int = 1
}
trait ReadOnlyValue {
  val value: Int = 2
}
class Broken extends MutableValue with ReadOnlyValue
)";

  scalanative::support::DiagnosticEngine missingInheritedSetterDiagnostics;
  scalanative::tools::build::BuildResult missingInheritedSetter =
      driver.buildSource("MissingInheritedSetter.scala", missingInheritedSetterSource,
                         {}, missingInheritedSetterDiagnostics);
  if (int code =
          expect(!missingInheritedSetter.ok &&
                     contains(missingInheritedSetter.diagnosticsText,
                              "class Broken inherits incompatible accessor shape for "
                              "value") &&
                     contains(missingInheritedSetter.diagnosticsText,
                              "does not provide the setter required by variable"),
                 "read-only precedence over an inherited variable did not fail "
                 "typecheck")) {
    return code;
  }

  constexpr const char* incompatibleInheritedAccessorTypesSource =
      R"(package demo.bad
trait IntState {
  var value: Int = 1
}
trait StringState {
  var value: String = "bad"
}
class Broken extends IntState with StringState
)";

  scalanative::support::DiagnosticEngine incompatibleInheritedAccessorTypesDiagnostics;
  scalanative::tools::build::BuildResult incompatibleInheritedAccessorTypes =
      driver.buildSource("IncompatibleInheritedAccessorTypes.scala",
                         incompatibleInheritedAccessorTypesSource, {},
                         incompatibleInheritedAccessorTypesDiagnostics);
  if (int code = expect(
          !incompatibleInheritedAccessorTypes.ok &&
              contains(incompatibleInheritedAccessorTypes.diagnosticsText,
                       "class Broken inherits incompatible accessor types for "
                       "value: String") &&
              contains(incompatibleInheritedAccessorTypes.diagnosticsText, "and Int"),
          "incompatible inherited variable types did not fail typecheck")) {
    return code;
  }

  constexpr const char* incompatibleInheritedMemberShapeSource =
      R"(package demo.bad
trait ComputedValue {
  def value: Int = 1
}
trait MutableValue {
  var value: Int = 2
}
class Broken extends ComputedValue with MutableValue
)";

  scalanative::support::DiagnosticEngine incompatibleInheritedMemberShapeDiagnostics;
  scalanative::tools::build::BuildResult incompatibleInheritedMemberShape =
      driver.buildSource("IncompatibleInheritedMemberShape.scala",
                         incompatibleInheritedMemberShapeSource, {},
                         incompatibleInheritedMemberShapeDiagnostics);
  if (int code =
          expect(!incompatibleInheritedMemberShape.ok &&
                     contains(incompatibleInheritedMemberShape.diagnosticsText,
                              "class Broken inherits incompatible member shape for "
                              "value: variable") &&
                     contains(incompatibleInheritedMemberShape.diagnosticsText,
                              "conflicts with method"),
                 "incompatible inherited method/accessor shape did not fail "
                 "typecheck")) {
    return code;
  }

  constexpr const char* untypedAbstractMethodSource = R"(package demo.bad
trait Broken {
  def value
}
)";

  scalanative::support::DiagnosticEngine untypedAbstractMethodDiagnostics;
  scalanative::tools::build::BuildResult untypedAbstractMethod =
      driver.buildSource("UntypedAbstractMethod.scala", untypedAbstractMethodSource, {},
                         untypedAbstractMethodDiagnostics);
  if (int code =
          expect(!untypedAbstractMethod.ok &&
                     contains(untypedAbstractMethod.diagnosticsText,
                              "abstract trait method value requires an explicit return "
                              "type"),
                 "untyped abstract trait method did not fail typecheck")) {
    return code;
  }

  constexpr const char* untypedAbstractValueSource = R"(package demo.bad
trait Broken {
  val value
}
)";

  scalanative::support::DiagnosticEngine untypedAbstractValueDiagnostics;
  scalanative::tools::build::BuildResult untypedAbstractValue =
      driver.buildSource("UntypedAbstractValue.scala", untypedAbstractValueSource, {},
                         untypedAbstractValueDiagnostics);
  if (int code =
          expect(!untypedAbstractValue.ok &&
                     contains(untypedAbstractValue.diagnosticsText,
                              "abstract trait value value requires an explicit type"),
                 "untyped abstract trait value did not fail typecheck")) {
    return code;
  }

  constexpr const char* untypedAbstractVarSource = R"(package demo.bad
trait Broken {
  var value
}
)";

  scalanative::support::DiagnosticEngine untypedAbstractVarDiagnostics;
  scalanative::tools::build::BuildResult untypedAbstractVar =
      driver.buildSource("UntypedAbstractVar.scala", untypedAbstractVarSource, {},
                         untypedAbstractVarDiagnostics);
  if (int code = expect(
          !untypedAbstractVar.ok &&
              contains(untypedAbstractVar.diagnosticsText,
                       "abstract trait variable value requires an explicit type"),
          "untyped abstract trait variable did not fail typecheck")) {
    return code;
  }

  constexpr const char* missingAbstractVarSource = R"(package demo.bad
trait Mutable {
  var value: Int
}
class Broken extends Mutable
)";

  scalanative::support::DiagnosticEngine missingAbstractVarDiagnostics;
  scalanative::tools::build::BuildResult missingAbstractVar =
      driver.buildSource("MissingAbstractVar.scala", missingAbstractVarSource, {},
                         missingAbstractVarDiagnostics);
  if (int code =
          expect(!missingAbstractVar.ok &&
                     contains(missingAbstractVar.diagnosticsText,
                              "class Broken must implement abstract variable value"),
                 "missing abstract trait variable did not fail typecheck")) {
    return code;
  }

  constexpr const char* immutableAbstractVarSource = R"(package demo.bad
trait Mutable {
  var value: Int
}
class Broken extends Mutable {
  override val value: Int = 1
}
)";

  scalanative::support::DiagnosticEngine immutableAbstractVarDiagnostics;
  scalanative::tools::build::BuildResult immutableAbstractVar =
      driver.buildSource("ImmutableAbstractVar.scala", immutableAbstractVarSource, {},
                         immutableAbstractVarDiagnostics);
  if (int code = expect(!immutableAbstractVar.ok &&
                            contains(immutableAbstractVar.diagnosticsText,
                                     "cannot implement inherited variable value with a "
                                     "non-variable declaration"),
                        "immutable value satisfied an abstract trait variable")) {
    return code;
  }

  constexpr const char* mismatchedAbstractVarSource = R"(package demo.bad
trait Mutable {
  var value: Int
}
class Broken extends Mutable {
  override var value: String = "bad"
}
)";

  scalanative::support::DiagnosticEngine mismatchedAbstractVarDiagnostics;
  scalanative::tools::build::BuildResult mismatchedAbstractVar =
      driver.buildSource("MismatchedAbstractVar.scala", mismatchedAbstractVarSource, {},
                         mismatchedAbstractVarDiagnostics);
  if (int code =
          expect(!mismatchedAbstractVar.ok &&
                     contains(mismatchedAbstractVar.diagnosticsText,
                              "override value variable type String does not match "
                              "inherited variable type Int"),
                 "abstract trait variable type mismatch did not fail typecheck")) {
    return code;
  }

  constexpr const char* mismatchedConstructorVarSource = R"(package demo.bad
trait Mutable {
  var value: Int
}
class Broken(var value: String) extends Mutable
)";

  scalanative::support::DiagnosticEngine mismatchedConstructorVarDiagnostics;
  scalanative::tools::build::BuildResult mismatchedConstructorVar = driver.buildSource(
      "MismatchedConstructorVar.scala", mismatchedConstructorVarSource, {},
      mismatchedConstructorVarDiagnostics);
  if (int code =
          expect(!mismatchedConstructorVar.ok &&
                     contains(mismatchedConstructorVar.diagnosticsText,
                              "constructor variable value type String does not match "
                              "inherited variable type Int"),
                 "constructor trait variable type mismatch did not fail typecheck")) {
    return code;
  }

  constexpr const char* missingParentArgsSource = R"(package demo.bad
class Base(val start: Int)
class Broken extends Base
)";

  scalanative::support::DiagnosticEngine missingParentArgsDiagnostics;
  scalanative::tools::build::BuildResult missingParentArgs =
      driver.buildSource("MissingParentArgs.scala", missingParentArgsSource, {},
                         missingParentArgsDiagnostics);
  if (int code =
          expect(!missingParentArgs.ok &&
                     contains(missingParentArgs.diagnosticsText,
                              "parent constructor for Base has 0 arguments but "
                              "expected 1"),
                 "missing parent constructor arguments did not fail typecheck")) {
    return code;
  }

  constexpr const char* invalidSuperSource = R"(package demo.bad
object Main {
  val value: Int = super.value
}
)";

  scalanative::support::DiagnosticEngine invalidSuperDiagnostics;
  scalanative::tools::build::BuildResult invalidSuper = driver.buildSource(
      "InvalidSuper.scala", invalidSuperSource, {}, invalidSuperDiagnostics);
  if (int code =
          expect(!invalidSuper.ok &&
                     contains(invalidSuper.diagnosticsText,
                              "super is only available in classes with a parent"),
                 "invalid super usage did not fail typecheck")) {
    return code;
  }

  constexpr const char* overrideReturnSource = R"(package demo.bad
class Base {
  def value: Int = 1
}
class Broken extends Base {
  override def value: String = "bad"
}
)";

  scalanative::support::DiagnosticEngine overrideReturnDiagnostics;
  scalanative::tools::build::BuildResult overrideReturn = driver.buildSource(
      "OverrideReturn.scala", overrideReturnSource, {}, overrideReturnDiagnostics);
  if (int code = expect(!overrideReturn.ok &&
                            contains(overrideReturn.diagnosticsText,
                                     "override value return type String does not match "
                                     "inherited return type Int"),
                        "override return type mismatch did not fail typecheck")) {
    return code;
  }

  constexpr const char* overrideParameterSource = R"(package demo.bad
class Base {
  def pick(value: Int): Int = value
}
class Broken extends Base {
  override def pick(value: String): Int = 0
}
)";

  scalanative::support::DiagnosticEngine overrideParameterDiagnostics;
  scalanative::tools::build::BuildResult overrideParameter =
      driver.buildSource("OverrideParameter.scala", overrideParameterSource, {},
                         overrideParameterDiagnostics);
  if (int code = expect(!overrideParameter.ok &&
                            contains(overrideParameter.diagnosticsText,
                                     "override pick parameter 0 type String does not "
                                     "match inherited parameter type Int"),
                        "override parameter type mismatch did not fail typecheck")) {
    return code;
  }

  constexpr const char* overrideNonMethodSource = R"(package demo.bad
class Base {
  def value: Int = 1
}
class Broken extends Base {
  val value: Int = 2
}
)";

  scalanative::support::DiagnosticEngine overrideNonMethodDiagnostics;
  scalanative::tools::build::BuildResult overrideNonMethod =
      driver.buildSource("OverrideNonMethod.scala", overrideNonMethodSource, {},
                         overrideNonMethodDiagnostics);
  if (int code = expect(!overrideNonMethod.ok &&
                            contains(overrideNonMethod.diagnosticsText,
                                     "cannot override inherited method value with "
                                     "non-method declaration"),
                        "non-method override did not fail typecheck")) {
    return code;
  }

  constexpr const char* overrideNothingSource = R"(package demo.bad
class Broken {
  override def value: Int = 1
}
)";

  scalanative::support::DiagnosticEngine overrideNothingDiagnostics;
  scalanative::tools::build::BuildResult overrideNothing = driver.buildSource(
      "OverrideNothing.scala", overrideNothingSource, {}, overrideNothingDiagnostics);
  if (int code =
          expect(!overrideNothing.ok && contains(overrideNothing.diagnosticsText,
                                                 "override value overrides nothing"),
                 "override without inherited method did not fail typecheck")) {
    return code;
  }

  constexpr const char* constructorAritySource = R"(package demo.bad
class Counter(start: Int)
object Main {
  val counter = new Counter()
}
)";

  scalanative::support::DiagnosticEngine constructorArityDiagnostics;
  scalanative::tools::build::BuildResult constructorArity =
      driver.buildSource("ConstructorArity.scala", constructorAritySource, {},
                         constructorArityDiagnostics);
  if (int code = expect(!constructorArity.ok &&
                            contains(constructorArity.diagnosticsText,
                                     "constructor for Counter has 0 arguments but "
                                     "expected 1"),
                        "constructor arity mismatch did not fail typecheck")) {
    return code;
  }

  constexpr const char* constructorTypeSource = R"(package demo.bad
class Counter(start: Int)
object Main {
  val counter = new Counter("nope")
}
)";

  scalanative::support::DiagnosticEngine constructorTypeDiagnostics;
  scalanative::tools::build::BuildResult constructorType = driver.buildSource(
      "ConstructorType.scala", constructorTypeSource, {}, constructorTypeDiagnostics);
  if (int code = expect(!constructorType.ok &&
                            contains(constructorType.diagnosticsText,
                                     "constructor argument 0 of type String does "
                                     "not conform to field type Int"),
                        "constructor argument type mismatch did not fail "
                        "typecheck")) {
    return code;
  }

  constexpr const char* assignToValSource = R"(package demo.bad
class Box {
  val value: Int = 1
}
object Main {
  def main = {
    val box = new Box()
    box.value = 2
  }
}
)";

  scalanative::support::DiagnosticEngine assignToValDiagnostics;
  scalanative::tools::build::BuildResult assignToVal = driver.buildSource(
      "AssignToVal.scala", assignToValSource, {}, assignToValDiagnostics);
  if (int code =
          expect(!assignToVal.ok && contains(assignToVal.diagnosticsText,
                                             "cannot assign to immutable value: value"),
                 "assignment to val field did not fail typecheck")) {
    return code;
  }

  constexpr const char* assignToValParameterSource = R"(package demo.bad
class Box(val value: Int)
object Main {
  def main = {
    val box = new Box(1)
    box.value = 2
  }
}
)";

  scalanative::support::DiagnosticEngine assignToValParameterDiagnostics;
  scalanative::tools::build::BuildResult assignToValParameter =
      driver.buildSource("AssignToValParameter.scala", assignToValParameterSource, {},
                         assignToValParameterDiagnostics);
  if (int code = expect(!assignToValParameter.ok &&
                            contains(assignToValParameter.diagnosticsText,
                                     "cannot assign to immutable value: value"),
                        "assignment to val constructor parameter field did not fail "
                        "typecheck")) {
    return code;
  }

  constexpr const char* assignWrongTypeSource = R"(package demo.bad
class Box {
  var value: Int = 1
}
object Main {
  def main = {
    val box = new Box()
    box.value = "nope"
  }
}
)";

  scalanative::support::DiagnosticEngine assignWrongTypeDiagnostics;
  scalanative::tools::build::BuildResult assignWrongType = driver.buildSource(
      "AssignWrongType.scala", assignWrongTypeSource, {}, assignWrongTypeDiagnostics);
  if (int code = expect(!assignWrongType.ok &&
                            contains(assignWrongType.diagnosticsText,
                                     "assignment value type String does not conform to "
                                     "target type Int"),
                        "assignment value type mismatch did not fail typecheck")) {
    return code;
  }

  constexpr const char* badPrintSource = R"(package demo.bad
object Main {
  def main = println()
}
)";

  scalanative::support::DiagnosticEngine printDiagnostics;
  scalanative::tools::build::BuildResult print =
      driver.buildSource("BadPrint.scala", badPrintSource, {}, printDiagnostics);
  return expect(!print.ok && contains(print.diagnosticsText,
                                      "call to println has 0 arguments but expected 1"),
                "println arity mismatch did not fail typecheck");
}

int smokeNirVerifier() {
  scalanative::nir::FunctionBodyBuilder validBody;
  (void)validBody.addReturn("Int",
                            scalanative::nir::literalValue(
                                "1", "Int", scalanative::support::SourceSpan::none()),
                            scalanative::support::SourceSpan::none());

  scalanative::nir::Module valid;
  valid.name = "demo.Good";
  valid.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                               "demo.Good.value", "()Int", std::move(validBody).build(),
                               scalanative::support::SourceSpan::none()});

  scalanative::nir::Verifier verifier;
  if (int code = expect(verifier.verify(valid).ok,
                        "NIR verifier rejected a valid function definition")) {
    return code;
  }
  if (int code = expect(valid.definitions.front().body.instructions.size() == 1 &&
                            valid.definitions.front().body.terminator() != nullptr,
                        "structured NIR function body did not keep its terminator")) {
    return code;
  }
  std::vector<std::string> validBodyText =
      scalanative::nir::bodyToText(valid.definitions.front().body);
  if (int code = expect(validBodyText.size() == 2 && validBodyText[0] == "entry:" &&
                            validBodyText[1] == "ret Int 1",
                        "structured NIR body did not render expected text")) {
    return code;
  }

  scalanative::nir::FunctionBodyBuilder invalidUnaryBody;
  (void)invalidUnaryBody.addReturn(
      "Boolean",
      scalanative::nir::unaryValue(
          "!",
          scalanative::nir::literalValue("1", "Int",
                                         scalanative::support::SourceSpan::none()),
          scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());
  scalanative::nir::Module invalidUnary;
  invalidUnary.name = "demo.InvalidUnary";
  invalidUnary.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                      "demo.InvalidUnary.run", "()Boolean",
                                      std::move(invalidUnaryBody).build(),
                                      scalanative::support::SourceSpan::none()});
  const scalanative::nir::VerifyResult invalidUnaryResult =
      verifier.verify(invalidUnary);
  if (int code = expect(
          !invalidUnaryResult.ok &&
              std::any_of(
                  invalidUnaryResult.errors.begin(), invalidUnaryResult.errors.end(),
                  [](const std::string& error) {
                    return contains(error, "applies ! to a non-Boolean operand");
                  }),
          "NIR verifier accepted logical negation of an Int")) {
    return code;
  }

  scalanative::nir::FunctionBodyBuilder primitiveStringConcatBody;
  (void)primitiveStringConcatBody.addReturn(
      "String",
      scalanative::nir::binaryValue(
          "+",
          scalanative::nir::literalValue("\"count \"", "String",
                                         scalanative::support::SourceSpan::none()),
          scalanative::nir::literalValue("1", "Int",
                                         scalanative::support::SourceSpan::none()),
          scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());
  scalanative::nir::Module primitiveStringConcat;
  primitiveStringConcat.name = "demo.PrimitiveStringConcat";
  primitiveStringConcat.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef, "demo.PrimitiveStringConcat.run",
       "()String", std::move(primitiveStringConcatBody).build(),
       scalanative::support::SourceSpan::none()});
  if (int code = expect(verifier.verify(primitiveStringConcat).ok,
                        "NIR verifier rejected primitive String concatenation")) {
    return code;
  }

  scalanative::nir::FunctionBodyBuilder symbolStringConcatBody;
  (void)symbolStringConcatBody.addReturn(
      "String",
      scalanative::nir::binaryValue(
          "+",
          scalanative::nir::literalValue("\"symbol \"", "String",
                                         scalanative::support::SourceSpan::none()),
          scalanative::nir::literalValue("'value", "Symbol",
                                         scalanative::support::SourceSpan::none()),
          scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());
  scalanative::nir::Module symbolStringConcat;
  symbolStringConcat.name = "demo.SymbolStringConcat";
  symbolStringConcat.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef, "demo.SymbolStringConcat.run",
       "()String", std::move(symbolStringConcatBody).build(),
       scalanative::support::SourceSpan::none()});
  if (int code = expect(verifier.verify(symbolStringConcat).ok,
                        "NIR verifier rejected Symbol String concatenation")) {
    return code;
  }

  scalanative::nir::FunctionBodyBuilder invalidControlBody;
  (void)invalidControlBody.addEval(
      scalanative::nir::ifValue(
          scalanative::nir::literalValue("1", "Int",
                                         scalanative::support::SourceSpan::none()),
          scalanative::nir::unitValue(scalanative::support::SourceSpan::none()),
          scalanative::nir::unitValue(scalanative::support::SourceSpan::none()),
          scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());
  (void)invalidControlBody.addEval(
      scalanative::nir::whileValue(
          scalanative::nir::literalValue("\"still\"", "String",
                                         scalanative::support::SourceSpan::none()),
          scalanative::nir::unitValue(scalanative::support::SourceSpan::none()),
          scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());
  (void)invalidControlBody.addReturn(
      "Int",
      scalanative::nir::literalValue("0", "Int",
                                     scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());
  scalanative::nir::Module invalidControl;
  invalidControl.name = "demo.InvalidControl";
  invalidControl.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                        "demo.InvalidControl.run", "()Int",
                                        std::move(invalidControlBody).build(),
                                        scalanative::support::SourceSpan::none()});
  const scalanative::nir::VerifyResult invalidControlResult =
      verifier.verify(invalidControl);
  if (int code = expect(
          !invalidControlResult.ok &&
              std::any_of(invalidControlResult.errors.begin(),
                          invalidControlResult.errors.end(),
                          [](const std::string& error) {
                            return contains(error, "has non-Boolean if condition: Int");
                          }) &&
              std::any_of(invalidControlResult.errors.begin(),
                          invalidControlResult.errors.end(),
                          [](const std::string& error) {
                            return contains(error,
                                            "has non-Boolean while condition: String");
                          }),
          "NIR verifier accepted non-Boolean control-flow conditions")) {
    return code;
  }

  scalanative::nir::FunctionBodyBuilder zoneEscapeBody;
  (void)zoneEscapeBody.addVar(
      "escaped", "demo.ZoneEscape.Value",
      scalanative::nir::literalValue("null", "Null",
                                     scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());
  std::vector<scalanative::nir::Value> escapingZoneValues;
  escapingZoneValues.push_back(scalanative::nir::localLetValue(
      "local", "demo.ZoneEscape.Value",
      scalanative::nir::newValue("demo.ZoneEscape.Value",
                                 scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none()));
  escapingZoneValues.push_back(scalanative::nir::assignValue(
      scalanative::nir::localValue("escaped", scalanative::support::SourceSpan::none()),
      scalanative::nir::localValue("local", scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none()));
  escapingZoneValues.push_back(scalanative::nir::literalValue(
      "0", "Int", scalanative::support::SourceSpan::none()));
  (void)zoneEscapeBody.addReturn(
      "Int",
      scalanative::nir::zoneScopedValue(
          scalanative::nir::blockValue(std::move(escapingZoneValues),
                                       scalanative::support::SourceSpan::none()),
          scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());
  scalanative::nir::Module invalidZoneEscape;
  invalidZoneEscape.name = "demo.ZoneEscape";
  invalidZoneEscape.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                           "demo.ZoneEscape.Value",
                                           "@java.lang.Object",
                                           {},
                                           scalanative::support::SourceSpan::none()});
  invalidZoneEscape.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef, "demo.ZoneEscape.run", "()Int",
       std::move(zoneEscapeBody).build(), scalanative::support::SourceSpan::none()});
  scalanative::nir::VerifyResult invalidZoneEscapeResult =
      verifier.verify(invalidZoneEscape);
  if (int code = expect(
          !invalidZoneEscapeResult.ok &&
              std::any_of(invalidZoneEscapeResult.errors.begin(),
                          invalidZoneEscapeResult.errors.end(),
                          [](const std::string& error) {
                            return contains(error,
                                            "assigns a scoped-zone reference to an "
                                            "outer local");
                          }),
          "NIR verifier accepted a scoped-zone reference stored in an outer local")) {
    return code;
  }

  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();
  scalanative::nir::FunctionBodyBuilder invalidSizeOfBody;
  (void)invalidSizeOfBody.addReturn(
      "Int", scalanative::nir::sizeOfValue("demo.NirSizeOf.Marker", noSpan), noSpan);
  scalanative::nir::Module invalidNirSizeOf;
  invalidNirSizeOf.name = "demo.NirSizeOf";
  invalidNirSizeOf.definitions.push_back({scalanative::nir::DefinitionKind::Trait,
                                          "demo.NirSizeOf.Marker",
                                          "@java.lang.Object",
                                          {},
                                          noSpan});
  invalidNirSizeOf.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef, "demo.NirSizeOf.run", "()Int",
       std::move(invalidSizeOfBody).build(), noSpan});
  scalanative::nir::VerifyResult invalidNirSizeOfResult =
      verifier.verify(invalidNirSizeOf);
  if (int code = expect(
          !invalidNirSizeOfResult.ok &&
              std::any_of(invalidNirSizeOfResult.errors.begin(),
                          invalidNirSizeOfResult.errors.end(),
                          [](const std::string& error) {
                            return contains(
                                error, "applies sizeof to a non-concrete class type: "
                                       "demo.NirSizeOf.Marker");
                          }),
          "NIR verifier accepted sizeof on a trait")) {
    return code;
  }

  scalanative::nir::FunctionBodyBuilder leakBody;
  (void)leakBody.addParameter("this", "demo.NirEffects.Receiver", noSpan);
  (void)leakBody.addEval(
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.NirEffects.sink", noSpan),
          {scalanative::nir::localValue("this", noSpan)}, noSpan),
      noSpan);
  (void)leakBody.addReturn("Unit", scalanative::nir::unitValue(noSpan), noSpan);
  scalanative::nir::FunctionBodyBuilder helperBody;
  (void)helperBody.addParameter("this", "demo.NirEffects.Receiver", noSpan);
  (void)helperBody.addEval(
      scalanative::nir::callValue(
          scalanative::nir::selectValue(scalanative::nir::localValue("this", noSpan),
                                        "leak", noSpan),
          {}, noSpan),
      noSpan);
  (void)helperBody.addReturn("Unit", scalanative::nir::unitValue(noSpan), noSpan);
  std::vector<scalanative::nir::Value> verifierZoneValues;
  verifierZoneValues.push_back(scalanative::nir::localLetValue(
      "local", "demo.NirEffects.Receiver",
      scalanative::nir::newValue("demo.NirEffects.Receiver", noSpan), noSpan));
  verifierZoneValues.push_back(scalanative::nir::callValue(
      scalanative::nir::selectValue(scalanative::nir::localValue("local", noSpan),
                                    "helper", noSpan),
      {}, noSpan));
  verifierZoneValues.push_back(scalanative::nir::literalValue("0", "Int", noSpan));
  scalanative::nir::FunctionBodyBuilder verifierZoneBody;
  (void)verifierZoneBody.addReturn(
      "Int",
      scalanative::nir::zoneScopedValue(
          scalanative::nir::blockValue(std::move(verifierZoneValues), noSpan), noSpan),
      noSpan);
  scalanative::nir::Module invalidNirMethodEffect;
  invalidNirMethodEffect.name = "demo.NirEffects";
  invalidNirMethodEffect.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                                "demo.NirEffects.Receiver",
                                                "@java.lang.Object",
                                                {},
                                                noSpan});
  invalidNirMethodEffect.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       "demo.NirEffects.sink",
       "(demo.NirEffects.Receiver)Unit",
       {},
       noSpan});
  invalidNirMethodEffect.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef, "demo.NirEffects.Receiver.leak",
       "(demo.NirEffects.Receiver)Unit", std::move(leakBody).build(), noSpan});
  invalidNirMethodEffect.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef, "demo.NirEffects.Receiver.helper",
       "(demo.NirEffects.Receiver)Unit", std::move(helperBody).build(), noSpan});
  invalidNirMethodEffect.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef, "demo.NirEffects.run", "()Int",
       std::move(verifierZoneBody).build(), noSpan});
  scalanative::nir::VerifyResult invalidNirMethodEffectResult =
      verifier.verify(invalidNirMethodEffect);
  if (int code = expect(
          !invalidNirMethodEffectResult.ok &&
              std::any_of(invalidNirMethodEffectResult.errors.begin(),
                          invalidNirMethodEffectResult.errors.end(),
                          [](const std::string& error) {
                            return contains(
                                error, "invokes a method that may leak a scoped-zone "
                                       "receiver: demo.NirEffects.Receiver.helper");
                          }),
          "NIR verifier accepted a zone call through a leaking helper method")) {
    return code;
  }

  scalanative::nir::FunctionBodyBuilder boxBody;
  (void)boxBody.addReturn("Object",
                          scalanative::nir::boxValue(
                              "Int",
                              scalanative::nir::literalValue(
                                  "7", "Int", scalanative::support::SourceSpan::none()),
                              scalanative::support::SourceSpan::none()),
                          scalanative::support::SourceSpan::none());
  scalanative::nir::FunctionBodyBuilder unboxBody;
  (void)unboxBody.addParameter("value", "Object",
                               scalanative::support::SourceSpan::none());
  (void)unboxBody.addReturn("Int",
                            scalanative::nir::unboxValue(
                                "Int",
                                scalanative::nir::localValue(
                                    "value", scalanative::support::SourceSpan::none()),
                                scalanative::support::SourceSpan::none()),
                            scalanative::support::SourceSpan::none());
  scalanative::nir::Module validBoxing;
  validBoxing.name = "demo.ValidBoxing";
  validBoxing.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                     "demo.ValidBoxing.box", "()Object",
                                     std::move(boxBody).build(),
                                     scalanative::support::SourceSpan::none()});
  validBoxing.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                     "demo.ValidBoxing.unbox", "(Object)Int",
                                     std::move(unboxBody).build(),
                                     scalanative::support::SourceSpan::none()});
  if (int code = expect(verifier.verify(validBoxing).ok,
                        "NIR verifier rejected valid Int boxing operations")) {
    return code;
  }

  scalanative::nir::Module validScalarBoxing;
  validScalarBoxing.name = "demo.ValidScalarBoxing";
  const std::vector<std::pair<std::string, std::string>> scalarLiterals = {
      {"Boolean", "true"},
      {"Long", "42L"},
      {"Float", "1.5F"},
      {"Double", "2.25"},
      {"Char", "'Z'"}};
  for (const auto& [type, literal] : scalarLiterals) {
    scalanative::nir::FunctionBodyBuilder scalarBoxBody;
    (void)scalarBoxBody.addReturn(
        "Object",
        scalanative::nir::boxValue(
            type,
            scalanative::nir::literalValue(literal, type,
                                           scalanative::support::SourceSpan::none()),
            scalanative::support::SourceSpan::none()),
        scalanative::support::SourceSpan::none());
    scalanative::nir::FunctionBodyBuilder scalarUnboxBody;
    (void)scalarUnboxBody.addParameter("value", "Object",
                                       scalanative::support::SourceSpan::none());
    (void)scalarUnboxBody.addReturn(
        type,
        scalanative::nir::unboxValue(
            type,
            scalanative::nir::localValue("value",
                                         scalanative::support::SourceSpan::none()),
            scalanative::support::SourceSpan::none()),
        scalanative::support::SourceSpan::none());
    validScalarBoxing.definitions.push_back(
        {scalanative::nir::DefinitionKind::FunctionDef,
         "demo.ValidScalarBoxing.box" + type, "()Object",
         std::move(scalarBoxBody).build(), scalanative::support::SourceSpan::none()});
    validScalarBoxing.definitions.push_back(
        {scalanative::nir::DefinitionKind::FunctionDef,
         "demo.ValidScalarBoxing.unbox" + type, "(Object)" + type,
         std::move(scalarUnboxBody).build(), scalanative::support::SourceSpan::none()});
  }
  if (int code = expect(verifier.verify(validScalarBoxing).ok,
                        "NIR verifier rejected valid scalar boxing operations")) {
    return code;
  }

  scalanative::nir::FunctionBodyBuilder invalidBoxBody;
  (void)invalidBoxBody.addReturn(
      "Object",
      scalanative::nir::boxValue(
          "Int",
          scalanative::nir::literalValue("bad", "String",
                                         scalanative::support::SourceSpan::none()),
          scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());
  scalanative::nir::FunctionBodyBuilder invalidUnboxBody;
  (void)invalidUnboxBody.addReturn(
      "Int",
      scalanative::nir::unboxValue(
          "Int",
          scalanative::nir::literalValue("1", "Int",
                                         scalanative::support::SourceSpan::none()),
          scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());
  scalanative::nir::Module invalidBoxing;
  invalidBoxing.name = "demo.InvalidBoxing";
  invalidBoxing.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                       "demo.InvalidBoxing.box", "()Object",
                                       std::move(invalidBoxBody).build(),
                                       scalanative::support::SourceSpan::none()});
  invalidBoxing.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                       "demo.InvalidBoxing.unbox", "()Int",
                                       std::move(invalidUnboxBody).build(),
                                       scalanative::support::SourceSpan::none()});
  const scalanative::nir::VerifyResult invalidBoxingResult =
      verifier.verify(invalidBoxing);
  if (int code = expect(
          !invalidBoxingResult.ok &&
              std::any_of(invalidBoxingResult.errors.begin(),
                          invalidBoxingResult.errors.end(),
                          [](const std::string& error) {
                            return contains(error, "boxes value of type String as Int");
                          }) &&
              std::any_of(
                  invalidBoxingResult.errors.begin(), invalidBoxingResult.errors.end(),
                  [](const std::string& error) {
                    return contains(error,
                                    "unboxes non-reference value of type Int as Int");
                  }),
          "NIR verifier accepted malformed Int boxing operations")) {
    return code;
  }

  scalanative::nir::Module validTypes;
  validTypes.name = "demo.ValidTypes";
  validTypes.definitions.push_back({scalanative::nir::DefinitionKind::Trait,
                                    "demo.ValidTypes.Carrier",
                                    "@java.lang.Object",
                                    {},
                                    scalanative::support::SourceSpan::none()});
  validTypes.definitions.push_back({scalanative::nir::DefinitionKind::TypeMember,
                                    "demo.ValidTypes.Carrier.Element",
                                    "abstract",
                                    {},
                                    scalanative::support::SourceSpan::none()});
  validTypes.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                    "demo.ValidTypes.Concrete",
                                    "@demo.ValidTypes.Carrier",
                                    {},
                                    scalanative::support::SourceSpan::none()});
  validTypes.definitions.push_back({scalanative::nir::DefinitionKind::TypeMember,
                                    "demo.ValidTypes.Concrete.Element",
                                    "Int",
                                    {},
                                    scalanative::support::SourceSpan::none()});
  if (int code = expect(verifier.verify(validTypes).ok,
                        "NIR verifier rejected a resolved abstract type member")) {
    return code;
  }

  scalanative::nir::Module unresolvedTypes;
  unresolvedTypes.name = "demo.UnresolvedTypes";
  unresolvedTypes.definitions.push_back({scalanative::nir::DefinitionKind::Trait,
                                         "demo.UnresolvedTypes.Carrier",
                                         "@java.lang.Object",
                                         {},
                                         scalanative::support::SourceSpan::none()});
  unresolvedTypes.definitions.push_back({scalanative::nir::DefinitionKind::TypeMember,
                                         "demo.UnresolvedTypes.Carrier.Element",
                                         "abstract",
                                         {},
                                         scalanative::support::SourceSpan::none()});
  unresolvedTypes.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                         "demo.UnresolvedTypes.Broken",
                                         "@demo.UnresolvedTypes.Carrier",
                                         {},
                                         scalanative::support::SourceSpan::none()});
  scalanative::nir::VerifyResult unresolvedTypesResult =
      verifier.verify(unresolvedTypes);
  if (int code = expect(
          !unresolvedTypesResult.ok &&
              std::any_of(unresolvedTypesResult.errors.begin(),
                          unresolvedTypesResult.errors.end(),
                          [](const std::string& error) {
                            return contains(
                                error, "must implement abstract type member Element");
                          }),
          "NIR verifier accepted an unresolved concrete type member")) {
    return code;
  }

  scalanative::nir::FunctionBodyBuilder unspecializedDefaultBody;
  (void)unspecializedDefaultBody.addParameter("this",
                                              "demo.UnspecializedDefault.Carrier",
                                              scalanative::support::SourceSpan::none());
  (void)unspecializedDefaultBody.addParameter("value",
                                              "demo.UnspecializedDefault.Carrier.Value",
                                              scalanative::support::SourceSpan::none());
  (void)unspecializedDefaultBody.addReturn(
      "demo.UnspecializedDefault.Carrier.Value",
      scalanative::nir::localValue("value", scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());
  scalanative::nir::Module unspecializedDefault;
  unspecializedDefault.name = "demo.UnspecializedDefault";
  unspecializedDefault.definitions.push_back(
      {scalanative::nir::DefinitionKind::Trait,
       "demo.UnspecializedDefault.Carrier",
       "@java.lang.Object",
       {},
       scalanative::support::SourceSpan::none()});
  unspecializedDefault.definitions.push_back(
      {scalanative::nir::DefinitionKind::TypeMember,
       "demo.UnspecializedDefault.Carrier.Value",
       "abstract",
       {},
       scalanative::support::SourceSpan::none()});
  unspecializedDefault.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.UnspecializedDefault.Carrier.echo",
       "(demo.UnspecializedDefault.Carrier,"
       "demo.UnspecializedDefault.Carrier.Value)"
       "demo.UnspecializedDefault.Carrier.Value",
       std::move(unspecializedDefaultBody).build(),
       scalanative::support::SourceSpan::none()});
  unspecializedDefault.definitions.push_back(
      {scalanative::nir::DefinitionKind::Class,
       "demo.UnspecializedDefault.Broken",
       "@demo.UnspecializedDefault.Carrier",
       {},
       scalanative::support::SourceSpan::none()});
  unspecializedDefault.definitions.push_back(
      {scalanative::nir::DefinitionKind::TypeMember,
       "demo.UnspecializedDefault.Broken.Value",
       "Int",
       {},
       scalanative::support::SourceSpan::none()});
  scalanative::nir::VerifyResult unspecializedDefaultResult =
      verifier.verify(unspecializedDefault);
  if (int code = expect(
          !unspecializedDefaultResult.ok &&
              std::any_of(unspecializedDefaultResult.errors.begin(),
                          unspecializedDefaultResult.errors.end(),
                          [](const std::string& error) {
                            return contains(
                                error, "must override inherited member echo to "
                                       "specialize its abstract type-member runtime "
                                       "signature");
                          }),
          "NIR verifier accepted an unspecialized inherited default ABI")) {
    return code;
  }

  scalanative::nir::Module validBoundedTypes;
  validBoundedTypes.name = "demo.ValidBoundedTypes";
  validBoundedTypes.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                           "demo.ValidBoundedTypes.Base",
                                           "@java.lang.Object",
                                           {},
                                           scalanative::support::SourceSpan::none()});
  validBoundedTypes.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                           "demo.ValidBoundedTypes.Child",
                                           "@demo.ValidBoundedTypes.Base",
                                           {},
                                           scalanative::support::SourceSpan::none()});
  validBoundedTypes.definitions.push_back({scalanative::nir::DefinitionKind::Trait,
                                           "demo.ValidBoundedTypes.Box",
                                           "@java.lang.Object",
                                           {},
                                           scalanative::support::SourceSpan::none()});
  validBoundedTypes.definitions.push_back({scalanative::nir::DefinitionKind::TypeMember,
                                           "demo.ValidBoundedTypes.Box.Item",
                                           "abstract <: demo.ValidBoundedTypes.Base",
                                           {},
                                           scalanative::support::SourceSpan::none()});
  validBoundedTypes.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                           "demo.ValidBoundedTypes.Concrete",
                                           "@demo.ValidBoundedTypes.Box",
                                           {},
                                           scalanative::support::SourceSpan::none()});
  validBoundedTypes.definitions.push_back({scalanative::nir::DefinitionKind::TypeMember,
                                           "demo.ValidBoundedTypes.Concrete.Item",
                                           "demo.ValidBoundedTypes.Child",
                                           {},
                                           scalanative::support::SourceSpan::none()});
  if (int code = expect(verifier.verify(validBoundedTypes).ok,
                        "NIR verifier rejected a conforming bounded alias")) {
    return code;
  }

  scalanative::nir::Module invalidBoundedTypes = validBoundedTypes;
  invalidBoundedTypes.name = "demo.InvalidBoundedTypes";
  invalidBoundedTypes.definitions.insert(invalidBoundedTypes.definitions.begin() + 2,
                                         {scalanative::nir::DefinitionKind::Class,
                                          "demo.ValidBoundedTypes.Other",
                                          "@java.lang.Object",
                                          {},
                                          scalanative::support::SourceSpan::none()});
  invalidBoundedTypes.definitions.back().signature = "demo.ValidBoundedTypes.Other";
  scalanative::nir::VerifyResult invalidBoundedTypesResult =
      verifier.verify(invalidBoundedTypes);
  if (int code = expect(
          !invalidBoundedTypesResult.ok &&
              std::any_of(invalidBoundedTypesResult.errors.begin(),
                          invalidBoundedTypesResult.errors.end(),
                          [](const std::string& error) {
                            return contains(error,
                                            "does not conform to inherited upper bound "
                                            "demo.ValidBoundedTypes.Base");
                          }),
          "NIR verifier accepted an alias outside its inherited upper bound")) {
    return code;
  }

  scalanative::nir::Module validIntervalTypes;
  validIntervalTypes.name = "demo.ValidIntervalTypes";
  validIntervalTypes.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                            "demo.ValidIntervalTypes.Wider",
                                            "@java.lang.Object",
                                            {},
                                            scalanative::support::SourceSpan::none()});
  validIntervalTypes.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                            "demo.ValidIntervalTypes.Upper",
                                            "@demo.ValidIntervalTypes.Wider",
                                            {},
                                            scalanative::support::SourceSpan::none()});
  validIntervalTypes.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                            "demo.ValidIntervalTypes.Middle",
                                            "@demo.ValidIntervalTypes.Upper",
                                            {},
                                            scalanative::support::SourceSpan::none()});
  validIntervalTypes.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                            "demo.ValidIntervalTypes.Lower",
                                            "@demo.ValidIntervalTypes.Middle",
                                            {},
                                            scalanative::support::SourceSpan::none()});
  validIntervalTypes.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                            "demo.ValidIntervalTypes.Other",
                                            "@demo.ValidIntervalTypes.Upper",
                                            {},
                                            scalanative::support::SourceSpan::none()});
  validIntervalTypes.definitions.push_back({scalanative::nir::DefinitionKind::Trait,
                                            "demo.ValidIntervalTypes.Box",
                                            "@java.lang.Object",
                                            {},
                                            scalanative::support::SourceSpan::none()});
  validIntervalTypes.definitions.push_back(
      {scalanative::nir::DefinitionKind::TypeMember,
       "demo.ValidIntervalTypes.Box.Item",
       "abstract >: demo.ValidIntervalTypes.Lower <: "
       "demo.ValidIntervalTypes.Upper",
       {},
       scalanative::support::SourceSpan::none()});
  validIntervalTypes.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                            "demo.ValidIntervalTypes.Concrete",
                                            "@demo.ValidIntervalTypes.Box",
                                            {},
                                            scalanative::support::SourceSpan::none()});
  validIntervalTypes.definitions.push_back(
      {scalanative::nir::DefinitionKind::TypeMember,
       "demo.ValidIntervalTypes.Concrete.Item",
       "demo.ValidIntervalTypes.Middle",
       {},
       scalanative::support::SourceSpan::none()});
  if (int code = expect(verifier.verify(validIntervalTypes).ok,
                        "NIR verifier rejected a conforming interval alias")) {
    return code;
  }

  scalanative::nir::Module aliasBelowInterval = validIntervalTypes;
  aliasBelowInterval.name = "demo.AliasBelowInterval";
  aliasBelowInterval.definitions.back().signature = "demo.ValidIntervalTypes.Other";
  scalanative::nir::VerifyResult aliasBelowIntervalResult =
      verifier.verify(aliasBelowInterval);
  if (int code =
          expect(!aliasBelowIntervalResult.ok &&
                     std::any_of(aliasBelowIntervalResult.errors.begin(),
                                 aliasBelowIntervalResult.errors.end(),
                                 [](const std::string& error) {
                                   return contains(
                                       error, "does not preserve inherited lower bound "
                                              "demo.ValidIntervalTypes.Lower");
                                 }),
                 "NIR verifier accepted an alias outside an inherited lower bound")) {
    return code;
  }

  scalanative::nir::Module aliasAboveInterval = validIntervalTypes;
  aliasAboveInterval.name = "demo.AliasAboveInterval";
  aliasAboveInterval.definitions.back().signature = "demo.ValidIntervalTypes.Wider";
  scalanative::nir::VerifyResult aliasAboveIntervalResult =
      verifier.verify(aliasAboveInterval);
  if (int code = expect(
          !aliasAboveIntervalResult.ok &&
              std::any_of(aliasAboveIntervalResult.errors.begin(),
                          aliasAboveIntervalResult.errors.end(),
                          [](const std::string& error) {
                            return contains(error,
                                            "does not conform to inherited upper bound "
                                            "demo.ValidIntervalTypes.Upper");
                          }),
          "NIR verifier accepted an alias outside an inherited upper bound")) {
    return code;
  }

  scalanative::nir::Module invalidInterval = validIntervalTypes;
  invalidInterval.name = "demo.InvalidInterval";
  invalidInterval.definitions[6].signature =
      "abstract >: demo.ValidIntervalTypes.Lower <: "
      "demo.ValidIntervalTypes.Other";
  scalanative::nir::VerifyResult invalidIntervalResult =
      verifier.verify(invalidInterval);
  if (int code = expect(
          !invalidIntervalResult.ok &&
              std::any_of(invalidIntervalResult.errors.begin(),
                          invalidIntervalResult.errors.end(),
                          [](const std::string& error) {
                            return contains(
                                error, "lower bound demo.ValidIntervalTypes.Lower does "
                                       "not conform to upper bound "
                                       "demo.ValidIntervalTypes.Other");
                          }),
          "NIR verifier accepted an inconsistent type interval")) {
    return code;
  }

  scalanative::nir::Module cyclicGraph;
  cyclicGraph.name = "demo.CyclicGraph";
  cyclicGraph.definitions.push_back({scalanative::nir::DefinitionKind::Trait,
                                     "demo.CyclicGraph.First",
                                     "@demo.CyclicGraph.Second",
                                     {},
                                     scalanative::support::SourceSpan::none()});
  cyclicGraph.definitions.push_back({scalanative::nir::DefinitionKind::Trait,
                                     "demo.CyclicGraph.Second",
                                     "@demo.CyclicGraph.First",
                                     {},
                                     scalanative::support::SourceSpan::none()});
  scalanative::nir::VerifyResult cyclicGraphResult = verifier.verify(cyclicGraph);
  if (int code = expect(!cyclicGraphResult.ok &&
                            std::any_of(cyclicGraphResult.errors.begin(),
                                        cyclicGraphResult.errors.end(),
                                        [](const std::string& error) {
                                          return contains(error,
                                                          "has cyclic inheritance");
                                        }),
                        "NIR verifier accepted cyclic inheritance metadata")) {
    return code;
  }

  scalanative::nir::Module inconsistentGraph;
  inconsistentGraph.name = "demo.InconsistentGraph";
  inconsistentGraph.definitions.push_back({scalanative::nir::DefinitionKind::Trait,
                                           "demo.InconsistentGraph.X",
                                           "@java.lang.Object",
                                           {},
                                           scalanative::support::SourceSpan::none()});
  inconsistentGraph.definitions.push_back({scalanative::nir::DefinitionKind::Trait,
                                           "demo.InconsistentGraph.Y",
                                           "@java.lang.Object",
                                           {},
                                           scalanative::support::SourceSpan::none()});
  inconsistentGraph.definitions.push_back(
      {scalanative::nir::DefinitionKind::Trait,
       "demo.InconsistentGraph.XY",
       "@demo.InconsistentGraph.X with @demo.InconsistentGraph.Y",
       {},
       scalanative::support::SourceSpan::none()});
  inconsistentGraph.definitions.push_back(
      {scalanative::nir::DefinitionKind::Trait,
       "demo.InconsistentGraph.YX",
       "@demo.InconsistentGraph.Y with @demo.InconsistentGraph.X",
       {},
       scalanative::support::SourceSpan::none()});
  inconsistentGraph.definitions.push_back(
      {scalanative::nir::DefinitionKind::Class,
       "demo.InconsistentGraph.Broken",
       "@demo.InconsistentGraph.XY with @demo.InconsistentGraph.YX",
       {},
       scalanative::support::SourceSpan::none()});
  scalanative::nir::VerifyResult inconsistentGraphResult =
      verifier.verify(inconsistentGraph);
  if (int code = expect(!inconsistentGraphResult.ok &&
                            std::any_of(inconsistentGraphResult.errors.begin(),
                                        inconsistentGraphResult.errors.end(),
                                        [](const std::string& error) {
                                          return contains(
                                              error,
                                              "has inconsistent parent linearization");
                                        }),
                        "NIR verifier accepted inconsistent parent precedence")) {
    return code;
  }

  scalanative::nir::Module module;
  module.name = "demo.Bad";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                "demo.Bad.dup",
                                "()Int",
                                {},
                                scalanative::support::SourceSpan::none()});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.Bad.dup",
                                "Int",
                                {},
                                scalanative::support::SourceSpan::none()});

  scalanative::nir::FunctionBodyBuilder undefinedBody;
  (void)undefinedBody.addReturn(
      "Int",
      scalanative::nir::localValue("missing", scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef, "demo.Bad.undefined", "()Int",
       std::move(undefinedBody).build(), scalanative::support::SourceSpan::none()});

  scalanative::nir::FunctionBodyBuilder mismatchBody;
  (void)mismatchBody.addReturn(
      "Int",
      scalanative::nir::literalValue("\"bad\"", "String",
                                     scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef, "demo.Bad.mismatch", "()Int",
       std::move(mismatchBody).build(), scalanative::support::SourceSpan::none()});

  scalanative::nir::FunctionBodyBuilder missingParamBody;
  (void)missingParamBody.addReturn(
      "Int",
      scalanative::nir::literalValue("0", "Int",
                                     scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.Bad.missingParam", "(Int)Int",
                                std::move(missingParamBody).build(),
                                scalanative::support::SourceSpan::none()});

  scalanative::nir::FunctionBodyBuilder invalidThrowBody;
  (void)invalidThrowBody.addThrow(
      scalanative::nir::literalValue("42", "Int",
                                     scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef, "demo.Bad.invalidThrow", "()Int",
       std::move(invalidThrowBody).build(), scalanative::support::SourceSpan::none()});

  scalanative::nir::FunctionBody afterTerminatorBody;
  afterTerminatorBody.instructions.push_back(
      {scalanative::nir::InstructionKind::Return,
       {},
       "Int",
       scalanative::nir::literalValue("0", "Int",
                                      scalanative::support::SourceSpan::none()),
       scalanative::support::SourceSpan::none()});
  afterTerminatorBody.instructions.push_back(
      {scalanative::nir::InstructionKind::Eval,
       {},
       {},
       scalanative::nir::literalValue("1", "Int",
                                      scalanative::support::SourceSpan::none()),
       scalanative::support::SourceSpan::none()});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.Bad.afterTerminator", "()Int",
                                std::move(afterTerminatorBody),
                                scalanative::support::SourceSpan::none()});
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                "demo.Bad.Child",
                                "@demo.Bad.MissingParent",
                                {},
                                scalanative::support::SourceSpan::none()});

  scalanative::nir::VerifyResult result = verifier.verify(module);
  if (int code = expect(!result.ok, "NIR verifier accepted malformed module")) {
    return code;
  }

  bool sawDuplicate = false;
  bool sawInvalidSignature = false;
  bool sawUndefined = false;
  bool sawMismatch = false;
  bool sawMissingParam = false;
  bool sawInvalidThrow = false;
  bool sawAfterTerminator = false;
  bool sawUnresolvedParent = false;
  for (const std::string& error : result.errors) {
    sawDuplicate =
        sawDuplicate || contains(error, "duplicate NIR definition: demo.Bad.dup");
    sawInvalidSignature =
        sawInvalidSignature ||
        contains(error, "invalid NIR function signature for 'demo.Bad.dup'");
    sawUndefined = sawUndefined ||
                   contains(error, "references undefined local or global: missing");
    sawMismatch = sawMismatch || contains(error, "returns value of type String as Int");
    sawMissingParam =
        sawMissingParam || contains(error, "0 parameter instructions but 1 signature");
    sawInvalidThrow =
        sawInvalidThrow || contains(error, "throws non-Throwable value of type Int");
    sawAfterTerminator =
        sawAfterTerminator || contains(error, "has instructions after a terminator");
    sawUnresolvedParent =
        sawUnresolvedParent || contains(error, "extends unresolved parent: "
                                               "@demo.Bad.MissingParent");
  }

  if (int code =
          expect(sawDuplicate, "NIR verifier did not reject duplicate definitions")) {
    return code;
  }
  if (int code = expect(sawInvalidSignature,
                        "NIR verifier did not reject invalid function signatures")) {
    return code;
  }
  if (int code =
          expect(sawUndefined, "NIR verifier did not reject undefined references")) {
    return code;
  }
  if (int code = expect(sawMismatch,
                        "NIR verifier did not reject return value type mismatch")) {
    return code;
  }
  if (int code = expect(sawMissingParam,
                        "NIR verifier did not reject parameter count mismatch")) {
    return code;
  }
  if (int code = expect(sawInvalidThrow,
                        "NIR verifier did not reject a primitive throw operand")) {
    return code;
  }
  if (int code =
          expect(sawAfterTerminator, "NIR verifier did not reject instructions after a "
                                     "terminator")) {
    return code;
  }
  return expect(sawUnresolvedParent,
                "NIR verifier did not reject unresolved parent metadata");
}

int smokeLinkerReachability() {
  scalanative::nir::FunctionBodyBuilder userMainBody;
  (void)userMainBody.addReturn(
      "Int",
      scalanative::nir::binaryValue(
          "+",
          scalanative::nir::literalValue("1", "Int",
                                         scalanative::support::SourceSpan::none()),
          scalanative::nir::literalValue("2", "Int",
                                         scalanative::support::SourceSpan::none()),
          scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int",
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.link.Main.main",
                                       scalanative::support::SourceSpan::none()),
          {}, scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());

  scalanative::nir::FunctionBodyBuilder unusedBody;
  (void)unusedBody.addReturn("Int",
                             scalanative::nir::literalValue(
                                 "99", "Int", scalanative::support::SourceSpan::none()),
                             scalanative::support::SourceSpan::none());

  scalanative::nir::Module module;
  module.name = "demo.link";
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef, "demo.link.Main.main", "()Int",
       std::move(userMainBody).build(), scalanative::support::SourceSpan::none()});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(),
                                scalanative::support::SourceSpan::none()});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef, "demo.link.Unused.value", "()Int",
       std::move(unusedBody).build(), scalanative::support::SourceSpan::none()});

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::linker::Linker linker;
  scalanative::tools::linker::LinkResult linked =
      linker.link({std::move(module)}, diagnostics);
  if (int code = expect(linked.ok, "linker rejected reachable NIR module")) {
    return code;
  }
  if (int code = expect(linked.program.roots.size() == 1 &&
                            linked.program.roots[0] == "scala.scalanative.runtime.main",
                        "linker did not prefer the runtime main root")) {
    return code;
  }
  if (int code = expect(
          containsString(linked.program.reachableGlobals,
                         "scala.scalanative.runtime.main") &&
              containsString(linked.program.reachableGlobals, "demo.link.Main.main"),
          "linker did not mark runtime main and user main reachable")) {
    return code;
  }
  if (int code = expect(
          !containsString(linked.program.reachableGlobals, "demo.link.Unused.value"),
          "linker marked an unused definition reachable")) {
    return code;
  }

  scalanative::nir::FunctionBodyBuilder traitNameBody;
  (void)traitNameBody.addParameter("this", "demo.dispatch.Named",
                                   scalanative::support::SourceSpan::none());
  (void)traitNameBody.addReturn(
      "String",
      scalanative::nir::literalValue("\"trait\"", "String",
                                     scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());

  scalanative::nir::FunctionBodyBuilder fancyNameBody;
  (void)fancyNameBody.addParameter("this", "demo.dispatch.Fancy",
                                   scalanative::support::SourceSpan::none());
  (void)fancyNameBody.addReturn(
      "String",
      scalanative::nir::literalValue("\"fancy\"", "String",
                                     scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());

  scalanative::nir::FunctionBodyBuilder unrelatedNameBody;
  (void)unrelatedNameBody.addParameter("this", "demo.dispatch.Unrelated",
                                       scalanative::support::SourceSpan::none());
  (void)unrelatedNameBody.addReturn(
      "String",
      scalanative::nir::literalValue("\"unused\"", "String",
                                     scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());

  scalanative::nir::FunctionBodyBuilder showBody;
  (void)showBody.addParameter("named", "demo.dispatch.Named",
                              scalanative::support::SourceSpan::none());
  (void)showBody.addReturn("String",
                           scalanative::nir::selectValue(
                               scalanative::nir::localValue(
                                   "named", scalanative::support::SourceSpan::none()),
                               "name", scalanative::support::SourceSpan::none()),
                           scalanative::support::SourceSpan::none());

  scalanative::nir::FunctionBodyBuilder dispatchMainBody;
  (void)dispatchMainBody.addEval(
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.dispatch.Main.show",
                                       scalanative::support::SourceSpan::none()),
          {scalanative::nir::newValue("demo.dispatch.Fancy",
                                      scalanative::support::SourceSpan::none())},
          scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());
  (void)dispatchMainBody.addReturn(
      "Int",
      scalanative::nir::literalValue("0", "Int",
                                     scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());

  scalanative::nir::Module dispatchModule;
  dispatchModule.name = "demo.dispatch";
  dispatchModule.definitions.push_back({scalanative::nir::DefinitionKind::Trait,
                                        "demo.dispatch.Named",
                                        "@java.lang.Object",
                                        {},
                                        scalanative::support::SourceSpan::none()});
  dispatchModule.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                        "demo.dispatch.Fancy",
                                        "@demo.dispatch.Named",
                                        {},
                                        scalanative::support::SourceSpan::none()});
  dispatchModule.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                        "demo.dispatch.Unrelated",
                                        "@java.lang.Object",
                                        {},
                                        scalanative::support::SourceSpan::none()});
  dispatchModule.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef, "demo.dispatch.Named.name",
       "(demo.dispatch.Named)String", std::move(traitNameBody).build(),
       scalanative::support::SourceSpan::none()});
  dispatchModule.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef, "demo.dispatch.Fancy.name",
       "(demo.dispatch.Fancy)String", std::move(fancyNameBody).build(),
       scalanative::support::SourceSpan::none()});
  dispatchModule.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef, "demo.dispatch.Unrelated.name",
       "(demo.dispatch.Unrelated)String", std::move(unrelatedNameBody).build(),
       scalanative::support::SourceSpan::none()});
  dispatchModule.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef, "demo.dispatch.Main.show",
       "(demo.dispatch.Named)String", std::move(showBody).build(),
       scalanative::support::SourceSpan::none()});
  dispatchModule.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                        "scala.scalanative.runtime.main", "()Int",
                                        std::move(dispatchMainBody).build(),
                                        scalanative::support::SourceSpan::none()});

  scalanative::support::DiagnosticEngine dispatchDiagnostics;
  scalanative::tools::linker::LinkResult dispatchLinked =
      linker.link({std::move(dispatchModule)}, dispatchDiagnostics);
  if (int code = expect(dispatchLinked.ok,
                        "linker rejected virtual dispatch reachability module")) {
    return code;
  }
  if (int code = expect(containsString(dispatchLinked.program.reachableGlobals,
                                       "demo.dispatch.Named.name") &&
                            containsString(dispatchLinked.program.reachableGlobals,
                                           "demo.dispatch.Fancy.name"),
                        "linker did not retain reachable virtual override targets")) {
    return code;
  }
  if (int code =
          expect(!containsString(dispatchLinked.program.reachableGlobals,
                                 "demo.dispatch.Unrelated.name"),
                 "linker retained unrelated same-named method as a dispatch target")) {
    return code;
  }

  scalanative::nir::Module firstDuplicate;
  firstDuplicate.name = "first";
  firstDuplicate.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                        "demo.Duplicate.value",
                                        "()Int",
                                        {},
                                        scalanative::support::SourceSpan::none()});
  scalanative::nir::Module secondDuplicate;
  secondDuplicate.name = "second";
  secondDuplicate.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                         "demo.Duplicate.value",
                                         "()Int",
                                         {},
                                         scalanative::support::SourceSpan::none()});

  scalanative::support::DiagnosticEngine duplicateDiagnostics;
  scalanative::tools::linker::LinkResult duplicate = linker.link(
      {std::move(firstDuplicate), std::move(secondDuplicate)}, duplicateDiagnostics);
  return expect(!duplicate.ok, "linker accepted duplicate globals");
}

int smokeInterflowOptimizer() {
  scalanative::nir::FunctionBodyBuilder userMainBody;
  (void)userMainBody.addLet("base",
                            scalanative::nir::literalValue(
                                "1", "Int", scalanative::support::SourceSpan::none()),
                            scalanative::support::SourceSpan::none());
  (void)userMainBody.addReturn(
      "Int",
      scalanative::nir::binaryValue(
          "+",
          scalanative::nir::localValue("base",
                                       scalanative::support::SourceSpan::none()),
          scalanative::nir::literalValue("2", "Int",
                                         scalanative::support::SourceSpan::none()),
          scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int",
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.Main.main",
                                       scalanative::support::SourceSpan::none()),
          {}, scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());

  scalanative::nir::FunctionBodyBuilder unusedBody;
  (void)unusedBody.addReturn("Int",
                             scalanative::nir::literalValue(
                                 "7", "Int", scalanative::support::SourceSpan::none()),
                             scalanative::support::SourceSpan::none());

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Main.main", "()Int",
                                std::move(userMainBody).build(),
                                scalanative::support::SourceSpan::none()});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(),
                                scalanative::support::SourceSpan::none()});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Unused.value", "()Int",
                                std::move(unusedBody).build(),
                                scalanative::support::SourceSpan::none()});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.Main.main");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code = expect(result.ok, "interflow rejected valid linked program")) {
    return code;
  }
  if (int code = expect(result.removedDefinitions == 1,
                        "interflow did not remove exactly one unreachable function")) {
    return code;
  }
  if (int code =
          expect(result.changedValues == 3,
                 "interflow did not change exactly three structured values; saw " +
                     std::to_string(result.changedValues))) {
    return code;
  }
  if (int code =
          expect(result.reports.size() == 5 &&
                     result.reports[0].name == "propagate-local-constants" &&
                     result.reports[0].changedValues == 1 &&
                     result.reports[0].validationErrorsBefore == 0 &&
                     result.reports[0].validationErrorsAfter == 0 &&
                     result.reports[1].name == "fold-constants" &&
                     result.reports[1].changedValues == 1 &&
                     result.reports[1].validationErrorsBefore == 0 &&
                     result.reports[1].validationErrorsAfter == 0 &&
                     result.reports[2].name == "eliminate-dead-local-lets" &&
                     result.reports[2].changedValues == 1 &&
                     result.reports[2].validationErrorsBefore == 0 &&
                     result.reports[2].validationErrorsAfter == 0 &&
                     result.reports[3].name == "simplify-blocks" &&
                     result.reports[3].changedValues == 0 &&
                     result.reports[3].validationErrorsBefore == 0 &&
                     result.reports[3].validationErrorsAfter == 0,
                 "interflow did not report validation-clean propagate, fold, DCE, "
                 "and block simplification passes")) {
    return code;
  }
  if (int code =
          expect(result.reports[4].name == "prune-unreachable-functions" &&
                     result.reports[4].removedDefinitions == 1 &&
                     result.reports[4].validationErrorsBefore == 0 &&
                     result.reports[4].validationErrorsAfter == 0,
                 "interflow did not report validation-clean propagate, fold, and prune "
                 "passes")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  if (int code = expect(
          containsDefinition(optimizedModule, "scala.scalanative.runtime.main") &&
              containsDefinition(optimizedModule, "demo.interflow.Main.main"),
          "interflow removed a reachable function")) {
    return code;
  }
  const scalanative::nir::Definition* foldedMain =
      findDefinition(optimizedModule, "demo.interflow.Main.main");
  if (int code = expect(
          foldedMain != nullptr &&
              scalanative::nir::bodyToText(foldedMain->body).back() == "ret Int 3",
          "interflow did not fold integer addition in reachable function")) {
    return code;
  }
  return expect(!containsDefinition(optimizedModule, "demo.interflow.Unused.value"),
                "interflow kept an unreachable function");
}

int smokeInterflowPureCallInlining() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  scalanative::nir::FunctionBodyBuilder constantBody;
  (void)constantBody.addReturn(
      "Int", scalanative::nir::literalValue("41", "Int", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder callerBody;
  (void)callerBody.addReturn(
      "Int",
      scalanative::nir::binaryValue(
          "+",
          scalanative::nir::callValue(
              scalanative::nir::localValue("demo.interflow.Inline.constant", noSpan),
              {}, noSpan),
          scalanative::nir::literalValue("1", "Int", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directBody;
  (void)directBody.addReturn(
      "Int",
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.Inline.constant", noSpan), {},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder shortNoArgLocalBody;
  (void)shortNoArgLocalBody.addReturn(
      "Int",
      scalanative::nir::binaryValue(
          "+", scalanative::nir::localValue("constant", noSpan),
          scalanative::nir::literalValue("1", "Int", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder addOneBody;
  (void)addOneBody.addParameter("value", "Int", noSpan);
  (void)addOneBody.addReturn(
      "Int",
      scalanative::nir::binaryValue("+", scalanative::nir::localValue("value", noSpan),
                                    scalanative::nir::literalValue("1", "Int", noSpan),
                                    noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder addOneCallerBody;
  (void)addOneCallerBody.addReturn(
      "Int",
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.Inline.addOne", noSpan),
          {scalanative::nir::literalValue("41", "Int", noSpan)}, noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder shortAddOneCallerBody;
  (void)shortAddOneCallerBody.addReturn(
      "Int",
      scalanative::nir::callValue(scalanative::nir::localValue("addOne", noSpan),
                                  {scalanative::nir::literalValue("41", "Int", noSpan)},
                                  noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder ignoredBody;
  (void)ignoredBody.addParameter("value", "Int", noSpan);
  (void)ignoredBody.addReturn("Int", scalanative::nir::literalValue("7", "Int", noSpan),
                              noSpan);

  scalanative::nir::FunctionBodyBuilder ignoredEffectfulCallerBody;
  (void)ignoredEffectfulCallerBody.addReturn(
      "Int",
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.Inline.ignored", noSpan),
          {scalanative::nir::callValue(
              scalanative::nir::localValue("demo.interflow.Inline.effect", noSpan), {},
              noSpan)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder effectfulBody;
  (void)effectfulBody.addReturn(
      "Int",
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.Inline.effect", noSpan), {},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder effectfulCallerBody;
  (void)effectfulCallerBody.addReturn(
      "Int",
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.Inline.effectful", noSpan), {},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Inline.constant", "()Int",
                                std::move(constantBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Inline.caller", "()Int",
                                std::move(callerBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Inline.direct", "()Int",
                                std::move(directBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Inline.shortNoArgLocal", "()Int",
                                std::move(shortNoArgLocalBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Inline.addOne", "(Int)Int",
                                std::move(addOneBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Inline.addOneCaller", "()Int",
                                std::move(addOneCallerBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Inline.shortAddOneCaller", "()Int",
                                std::move(shortAddOneCallerBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Inline.ignored", "(Int)Int",
                                std::move(ignoredBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Inline.ignoredEffectfulCaller", "()Int",
                                std::move(ignoredEffectfulCallerBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Inline.effectful", "()Int",
                                std::move(effectfulBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Inline.effectfulCaller", "()Int",
                                std::move(effectfulCallerBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                "demo.interflow.Inline.effect",
                                "()Int",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.Inline.constant");
  program.reachableGlobals.push_back("demo.interflow.Inline.caller");
  program.reachableGlobals.push_back("demo.interflow.Inline.direct");
  program.reachableGlobals.push_back("demo.interflow.Inline.shortNoArgLocal");
  program.reachableGlobals.push_back("demo.interflow.Inline.addOne");
  program.reachableGlobals.push_back("demo.interflow.Inline.addOneCaller");
  program.reachableGlobals.push_back("demo.interflow.Inline.shortAddOneCaller");
  program.reachableGlobals.push_back("demo.interflow.Inline.ignored");
  program.reachableGlobals.push_back("demo.interflow.Inline.ignoredEffectfulCaller");
  program.reachableGlobals.push_back("demo.interflow.Inline.effectful");
  program.reachableGlobals.push_back("demo.interflow.Inline.effectfulCaller");
  program.reachableGlobals.push_back("demo.interflow.Inline.effect");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code = expect(result.ok, "interflow rejected valid pure inlining program")) {
    return code;
  }
  if (int code = expect(
          result.reports.size() == 5 && result.reports[1].name == "fold-constants" &&
              result.reports[1].changedValues == 5 &&
              result.reports[1].validationErrorsBefore == 0 &&
              result.reports[1].validationErrorsAfter == 0,
          "interflow did not report validation-clean pure call inlining")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* caller =
      findDefinition(optimizedModule, "demo.interflow.Inline.caller");
  const scalanative::nir::Definition* direct =
      findDefinition(optimizedModule, "demo.interflow.Inline.direct");
  const scalanative::nir::Definition* shortNoArgLocal =
      findDefinition(optimizedModule, "demo.interflow.Inline.shortNoArgLocal");
  const scalanative::nir::Definition* addOneCaller =
      findDefinition(optimizedModule, "demo.interflow.Inline.addOneCaller");
  const scalanative::nir::Definition* shortAddOneCaller =
      findDefinition(optimizedModule, "demo.interflow.Inline.shortAddOneCaller");
  const scalanative::nir::Definition* ignoredEffectfulCaller =
      findDefinition(optimizedModule, "demo.interflow.Inline.ignoredEffectfulCaller");
  const scalanative::nir::Definition* effectfulCaller =
      findDefinition(optimizedModule, "demo.interflow.Inline.effectfulCaller");
  if (int code =
          expect(caller != nullptr && direct != nullptr && shortNoArgLocal != nullptr &&
                     addOneCaller != nullptr && shortAddOneCaller != nullptr &&
                     ignoredEffectfulCaller != nullptr && effectfulCaller != nullptr,
                 "interflow removed reachable inlining functions")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(caller->body).back() == "ret Int 42",
                 "interflow did not inline and fold pure no-arg call")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(direct->body).back() == "ret Int 41",
                 "interflow did not inline direct pure no-arg call")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(shortNoArgLocal->body).back() ==
                            "ret Int 42",
                        "interflow did not inline owner-relative no-arg local value")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(addOneCaller->body).back() ==
                            "ret Int 42",
                        "interflow did not substitute pure direct-call arguments")) {
    return code;
  }
  if (int code = expect(
          scalanative::nir::bodyToText(shortAddOneCaller->body).back() == "ret Int 42",
          "interflow did not inline owner-relative direct-call arguments")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(ignoredEffectfulCaller->body).back() ==
                     "ret Int call %demo.interflow.Inline.ignored(call "
                     "%demo.interflow.Inline.effect())",
                 "interflow incorrectly dropped an effectful ignored argument")) {
    return code;
  }
  return expect(scalanative::nir::bodyToText(effectfulCaller->body).back() ==
                    "ret Int call %demo.interflow.Inline.effectful()",
                "interflow incorrectly inlined an effectful wrapper");
}

int smokeInterflowDeadLocalLetElimination() {
  scalanative::nir::FunctionBodyBuilder userMainBody;
  (void)userMainBody.addLet("deadA",
                            scalanative::nir::literalValue(
                                "1", "Int", scalanative::support::SourceSpan::none()),
                            scalanative::support::SourceSpan::none());
  (void)userMainBody.addLet(
      "deadB",
      scalanative::nir::binaryValue(
          "+",
          scalanative::nir::localValue("deadA",
                                       scalanative::support::SourceSpan::none()),
          scalanative::nir::literalValue("2", "Int",
                                         scalanative::support::SourceSpan::none()),
          scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());
  (void)userMainBody.addLet(
      "effect", "Int",
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.Dead.effect",
                                       scalanative::support::SourceSpan::none()),
          {}, scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());
  (void)userMainBody.addReturn(
      "Int",
      scalanative::nir::literalValue("0", "Int",
                                     scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int",
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.Dead.main",
                                       scalanative::support::SourceSpan::none()),
          {}, scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                "demo.interflow.Dead.effect",
                                "()Int",
                                {},
                                scalanative::support::SourceSpan::none()});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Dead.main", "()Int",
                                std::move(userMainBody).build(),
                                scalanative::support::SourceSpan::none()});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(),
                                scalanative::support::SourceSpan::none()});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.Dead.main");
  program.reachableGlobals.push_back("demo.interflow.Dead.effect");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid dead-let elimination program")) {
    return code;
  }
  if (int code = expect(
          result.reports.size() == 5 &&
              result.reports[2].name == "eliminate-dead-local-lets" &&
              result.reports[2].changedValues == 2 &&
              result.reports[2].validationErrorsBefore == 0 &&
              result.reports[2].validationErrorsAfter == 0 &&
              result.reports[3].name == "simplify-blocks" &&
              result.reports[3].changedValues == 0,
          "interflow did not report validation-clean dead local let elimination")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* optimizedMain =
      findDefinition(optimizedModule, "demo.interflow.Dead.main");
  if (int code = expect(optimizedMain != nullptr,
                        "interflow removed the reachable dead-let test function")) {
    return code;
  }
  const std::vector<std::string> body =
      scalanative::nir::bodyToText(optimizedMain->body);
  return expect(body.size() == 3 && body[0] == "entry:" &&
                    body[1] ==
                        "let %effect : Int = call %demo.interflow.Dead.effect()" &&
                    body[2] == "ret Int 0",
                "interflow did not remove only pure unused lets");
}

int smokeInterflowNestedDeadLocalLetElimination() {
  std::vector<scalanative::nir::Value> blockValues;
  blockValues.push_back(scalanative::nir::localLetValue(
      "innerDead", "Int",
      scalanative::nir::literalValue("1", "Int",
                                     scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none()));
  blockValues.push_back(scalanative::nir::localLetValue(
      "innerEffect", "Int",
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.Nested.effect",
                                       scalanative::support::SourceSpan::none()),
          {}, scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none()));
  blockValues.push_back(scalanative::nir::literalValue(
      "5", "Int", scalanative::support::SourceSpan::none()));

  scalanative::nir::FunctionBodyBuilder userMainBody;
  (void)userMainBody.addReturn(
      "Int",
      scalanative::nir::blockValue(std::move(blockValues),
                                   scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int",
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.Nested.main",
                                       scalanative::support::SourceSpan::none()),
          {}, scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                "demo.interflow.Nested.effect",
                                "()Int",
                                {},
                                scalanative::support::SourceSpan::none()});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Nested.main", "()Int",
                                std::move(userMainBody).build(),
                                scalanative::support::SourceSpan::none()});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(),
                                scalanative::support::SourceSpan::none()});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.Nested.main");
  program.reachableGlobals.push_back("demo.interflow.Nested.effect");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid nested dead-let program")) {
    return code;
  }
  if (int code =
          expect(result.reports.size() == 5 &&
                     result.reports[2].name == "eliminate-dead-local-lets" &&
                     result.reports[2].changedValues == 1 &&
                     result.reports[2].validationErrorsBefore == 0 &&
                     result.reports[2].validationErrorsAfter == 0 &&
                     result.reports[3].name == "simplify-blocks" &&
                     result.reports[3].changedValues == 0,
                 "interflow did not report validation-clean nested dead local let "
                 "elimination")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* optimizedMain =
      findDefinition(optimizedModule, "demo.interflow.Nested.main");
  if (int code = expect(optimizedMain != nullptr,
                        "interflow removed the reachable nested dead-let function")) {
    return code;
  }
  const std::string body = scalanative::nir::bodyToText(optimizedMain->body).back();
  return expect(!contains(body, "innerDead") &&
                    contains(body, "let %innerEffect : Int = call "
                                   "%demo.interflow.Nested.effect()") &&
                    contains(body, "ret Int block("),
                "interflow did not remove only pure unused nested block lets");
}

int smokeInterflowDeadLocalVarElimination() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  std::vector<scalanative::nir::Value> blockValues;
  blockValues.push_back(scalanative::nir::localVarValue(
      "innerDead", "Int", scalanative::nir::literalValue("2", "Int", noSpan), noSpan));
  blockValues.push_back(scalanative::nir::literalValue("5", "Int", noSpan));

  scalanative::nir::FunctionBodyBuilder userMainBody;
  (void)userMainBody.addVar("deadTop", "Int",
                            scalanative::nir::literalValue("1", "Int", noSpan), noSpan);
  (void)userMainBody.addVar(
      "effect", "Int",
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.DeadVar.effect", noSpan), {},
          noSpan),
      noSpan);
  (void)userMainBody.addReturn(
      "Int", scalanative::nir::blockValue(std::move(blockValues), noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int",
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.DeadVar.main", noSpan), {},
          noSpan),
      noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                "demo.interflow.DeadVar.effect",
                                "()Int",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.DeadVar.main", "()Int",
                                std::move(userMainBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.DeadVar.main");
  program.reachableGlobals.push_back("demo.interflow.DeadVar.effect");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid dead-var elimination program")) {
    return code;
  }
  if (int code = expect(
          result.reports.size() == 5 &&
              result.reports[2].name == "eliminate-dead-local-lets" &&
              result.reports[2].changedValues == 2 &&
              result.reports[2].validationErrorsBefore == 0 &&
              result.reports[2].validationErrorsAfter == 0 &&
              result.reports[3].name == "simplify-blocks" &&
              result.reports[3].changedValues == 1 &&
              result.reports[3].validationErrorsBefore == 0 &&
              result.reports[3].validationErrorsAfter == 0,
          "interflow did not report validation-clean dead local var elimination")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* optimizedMain =
      findDefinition(optimizedModule, "demo.interflow.DeadVar.main");
  if (int code = expect(optimizedMain != nullptr,
                        "interflow removed the reachable dead-var test function")) {
    return code;
  }
  const std::vector<std::string> body =
      scalanative::nir::bodyToText(optimizedMain->body);
  return expect(body.size() == 3 && body[0] == "entry:" &&
                    body[1] ==
                        "var %effect : Int = call %demo.interflow.DeadVar.effect()" &&
                    body[2] == "ret Int 5",
                "interflow did not remove only pure unused vars");
}

int smokeInterflowLocalVarBlockShellSimplification() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  std::vector<scalanative::nir::Value> blockValues;
  blockValues.push_back(scalanative::nir::localVarValue(
      "value", "Int", scalanative::nir::literalValue("8", "Int", noSpan), noSpan));
  blockValues.push_back(scalanative::nir::localValue("value", noSpan));

  scalanative::nir::FunctionBodyBuilder userMainBody;
  (void)userMainBody.addReturn(
      "Int", scalanative::nir::blockValue(std::move(blockValues), noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.VarShell.main", "()Int",
                                std::move(userMainBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.VarShell.main");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid local-var block shell program")) {
    return code;
  }
  if (int code = expect(result.reports.size() == 5 &&
                            result.reports[2].name == "eliminate-dead-local-lets" &&
                            result.reports[2].changedValues == 0 &&
                            result.reports[3].name == "simplify-blocks" &&
                            result.reports[3].changedValues == 1 &&
                            result.reports[3].validationErrorsBefore == 0 &&
                            result.reports[3].validationErrorsAfter == 0,
                        "interflow did not report validation-clean local-var shell "
                        "simplification")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* optimizedMain =
      findDefinition(optimizedModule, "demo.interflow.VarShell.main");
  if (int code = expect(optimizedMain != nullptr,
                        "interflow removed the reachable local-var shell function")) {
    return code;
  }
  const std::vector<std::string> body =
      scalanative::nir::bodyToText(optimizedMain->body);
  return expect(body.size() == 2 && body[0] == "entry:" && body[1] == "ret Int 8",
                "interflow did not collapse pure local-var block shell");
}

int smokeInterflowSingleUsePureLetBlockInlining() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  std::vector<scalanative::nir::Value> blockValues;
  blockValues.push_back(scalanative::nir::localLetValue(
      "adjusted", "Int",
      scalanative::nir::binaryValue("+", scalanative::nir::localValue("value", noSpan),
                                    scalanative::nir::literalValue("1", "Int", noSpan),
                                    noSpan),
      noSpan));
  blockValues.push_back(scalanative::nir::binaryValue(
      "*", scalanative::nir::localValue("adjusted", noSpan),
      scalanative::nir::literalValue("2", "Int", noSpan), noSpan));

  scalanative::nir::FunctionBodyBuilder userMainBody;
  (void)userMainBody.addParameter("value", "Int", noSpan);
  (void)userMainBody.addReturn(
      "Int", scalanative::nir::blockValue(std::move(blockValues), noSpan), noSpan);

  std::vector<scalanative::nir::Value> chainedBlockValues;
  chainedBlockValues.push_back(scalanative::nir::localLetValue(
      "base", "Int",
      scalanative::nir::binaryValue("+", scalanative::nir::localValue("value", noSpan),
                                    scalanative::nir::literalValue("1", "Int", noSpan),
                                    noSpan),
      noSpan));
  chainedBlockValues.push_back(scalanative::nir::localLetValue(
      "doubled", "Int",
      scalanative::nir::binaryValue("*", scalanative::nir::localValue("base", noSpan),
                                    scalanative::nir::literalValue("2", "Int", noSpan),
                                    noSpan),
      noSpan));
  chainedBlockValues.push_back(scalanative::nir::localValue("doubled", noSpan));

  scalanative::nir::FunctionBodyBuilder chainedBody;
  (void)chainedBody.addParameter("value", "Int", noSpan);
  (void)chainedBody.addReturn(
      "Int", scalanative::nir::blockValue(std::move(chainedBlockValues), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.SingleUseBlock.main", "(Int)Int",
                                std::move(userMainBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.SingleUseBlock.chained", "(Int)Int",
                                std::move(chainedBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.SingleUseBlock.main");
  program.reachableGlobals.push_back("demo.interflow.SingleUseBlock.chained");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid single-use block-let program")) {
    return code;
  }
  if (int code =
          expect(result.reports.size() == 5 &&
                     result.reports[2].name == "eliminate-dead-local-lets" &&
                     result.reports[2].changedValues == 0 &&
                     result.reports[3].name == "simplify-blocks" &&
                     result.reports[3].changedValues == 3 &&
                     result.reports[3].validationErrorsBefore == 0 &&
                     result.reports[3].validationErrorsAfter == 0,
                 "interflow did not report validation-clean single-use block-let "
                 "inlining")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* optimizedMain =
      findDefinition(optimizedModule, "demo.interflow.SingleUseBlock.main");
  const scalanative::nir::Definition* optimizedChained =
      findDefinition(optimizedModule, "demo.interflow.SingleUseBlock.chained");
  if (int code = expect(optimizedMain != nullptr && optimizedChained != nullptr,
                        "interflow removed reachable single-use block-let functions")) {
    return code;
  }
  const std::vector<std::string> body =
      scalanative::nir::bodyToText(optimizedMain->body);
  if (int code = expect(body.size() == 3 && body[0] == "entry:" &&
                            body[1] == "param %value : Int" &&
                            body[2] == "ret Int ((%value + 1) * 2)",
                        "interflow did not inline the single-use pure block let")) {
    return code;
  }

  const std::vector<std::string> chainedBodyText =
      scalanative::nir::bodyToText(optimizedChained->body);
  return expect(chainedBodyText.size() == 3 && chainedBodyText[0] == "entry:" &&
                    chainedBodyText[1] == "param %value : Int" &&
                    chainedBodyText[2] == "ret Int ((%value + 1) * 2)",
                "interflow did not inline chained single-use pure block lets");
}

int smokeInterflowNestedPureBlockSimplification() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  std::vector<scalanative::nir::Value> innerValues;
  innerValues.push_back(scalanative::nir::literalValue("1", "Int", noSpan));

  std::vector<scalanative::nir::Value> outerValues;
  outerValues.push_back(scalanative::nir::blockValue(std::move(innerValues), noSpan));
  outerValues.push_back(scalanative::nir::literalValue("2", "Int", noSpan));

  scalanative::nir::FunctionBodyBuilder userMainBody;
  (void)userMainBody.addReturn(
      "Int", scalanative::nir::blockValue(std::move(outerValues), noSpan), noSpan);

  constexpr std::string_view BoxType = "demo.interflow.NestedBlock.Box";
  std::vector<scalanative::nir::Value> structuredInnerValues;
  structuredInnerValues.push_back(scalanative::nir::isInstanceOfValue(
      std::string(BoxType), scalanative::nir::localValue("box", noSpan), noSpan));

  std::vector<scalanative::nir::Value> structuredOuterValues;
  structuredOuterValues.push_back(
      scalanative::nir::blockValue(std::move(structuredInnerValues), noSpan));
  structuredOuterValues.push_back(scalanative::nir::literalValue("3", "Int", noSpan));

  scalanative::nir::FunctionBodyBuilder structuredDiscardBody;
  (void)structuredDiscardBody.addParameter("box", std::string(BoxType), noSpan);
  (void)structuredDiscardBody.addReturn(
      "Int", scalanative::nir::blockValue(std::move(structuredOuterValues), noSpan),
      noSpan);

  std::vector<scalanative::nir::Value> evalValues;
  evalValues.push_back(scalanative::nir::literalValue("1", "Int", noSpan));

  scalanative::nir::FunctionBodyBuilder evalDiscardBody;
  (void)evalDiscardBody.addEval(
      scalanative::nir::blockValue(std::move(evalValues), noSpan), noSpan);
  (void)evalDiscardBody.addReturn(
      "Int", scalanative::nir::literalValue("4", "Int", noSpan), noSpan);

  std::vector<scalanative::nir::Value> deadLetValues;
  deadLetValues.push_back(scalanative::nir::literalValue("1", "Int", noSpan));

  scalanative::nir::FunctionBodyBuilder deadLetBody;
  (void)deadLetBody.addLet(
      "unused", "Int", scalanative::nir::blockValue(std::move(deadLetValues), noSpan),
      noSpan);
  (void)deadLetBody.addReturn("Int", scalanative::nir::literalValue("5", "Int", noSpan),
                              noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                std::string(BoxType),
                                "@java.lang.Object",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.NestedBlock.main", "()Int",
                                std::move(userMainBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.NestedBlock.structuredDiscard",
                                "(" + std::string(BoxType) + ")Int",
                                std::move(structuredDiscardBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.NestedBlock.evalDiscard", "()Int",
                                std::move(evalDiscardBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.NestedBlock.deadLet", "()Int",
                                std::move(deadLetBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.NestedBlock.main");
  program.reachableGlobals.push_back("demo.interflow.NestedBlock.structuredDiscard");
  program.reachableGlobals.push_back("demo.interflow.NestedBlock.evalDiscard");
  program.reachableGlobals.push_back("demo.interflow.NestedBlock.deadLet");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid nested pure block program")) {
    return code;
  }
  if (int code = expect(result.reports.size() == 5 &&
                            result.reports[2].name == "eliminate-dead-local-lets" &&
                            result.reports[2].changedValues == 0 &&
                            result.reports[3].name == "simplify-blocks" &&
                            result.reports[3].changedValues == 10 &&
                            result.reports[3].validationErrorsBefore == 0 &&
                            result.reports[3].validationErrorsAfter == 0,
                        "interflow did not report validation-clean nested block "
                        "simplification")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* optimizedMain =
      findDefinition(optimizedModule, "demo.interflow.NestedBlock.main");
  const scalanative::nir::Definition* structuredDiscard =
      findDefinition(optimizedModule, "demo.interflow.NestedBlock.structuredDiscard");
  const scalanative::nir::Definition* evalDiscard =
      findDefinition(optimizedModule, "demo.interflow.NestedBlock.evalDiscard");
  const scalanative::nir::Definition* deadLet =
      findDefinition(optimizedModule, "demo.interflow.NestedBlock.deadLet");
  if (int code = expect(optimizedMain != nullptr && structuredDiscard != nullptr &&
                            evalDiscard != nullptr && deadLet != nullptr,
                        "interflow removed the reachable nested pure block function")) {
    return code;
  }
  const std::vector<std::string> body =
      scalanative::nir::bodyToText(optimizedMain->body);
  if (int code =
          expect(body.size() == 2 && body[0] == "entry:" && body[1] == "ret Int 2",
                 "interflow did not collapse nested pure block discard")) {
    return code;
  }

  const std::vector<std::string> structuredBody =
      scalanative::nir::bodyToText(structuredDiscard->body);
  if (int code = expect(
          structuredBody.size() == 3 && structuredBody[0] == "entry:" &&
              structuredBody[1] == "param %box : demo.interflow.NestedBlock.Box" &&
              structuredBody[2] == "ret Int 3",
          "interflow did not remove structured pure value exposed by block "
          "simplification")) {
    return code;
  }

  const std::vector<std::string> evalBody =
      scalanative::nir::bodyToText(evalDiscard->body);
  if (int code = expect(
          evalBody.size() == 2 && evalBody[0] == "entry:" && evalBody[1] == "ret Int 4",
          "interflow did not remove eval made discardable by block simplification")) {
    return code;
  }

  const std::vector<std::string> deadLetBodyText =
      scalanative::nir::bodyToText(deadLet->body);
  return expect(deadLetBodyText.size() == 2 && deadLetBodyText[0] == "entry:" &&
                    deadLetBodyText[1] == "ret Int 5",
                "interflow did not remove dead let made pure by block simplification");
}

int smokeInterflowDeadPureDiscardElimination() {
  constexpr std::string_view ItemType = "demo.interflow.PureDiscard.Item";

  std::vector<scalanative::nir::Value> blockValues;
  blockValues.push_back(scalanative::nir::literalValue(
      "1", "Int", scalanative::support::SourceSpan::none()));
  blockValues.push_back(scalanative::nir::sizeOfValue(
      std::string(ItemType), scalanative::support::SourceSpan::none()));
  blockValues.push_back(scalanative::nir::localLetValue(
      "innerDead", "Int",
      scalanative::nir::literalValue("2", "Int",
                                     scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none()));
  blockValues.push_back(scalanative::nir::literalValue(
      "4", "Int", scalanative::support::SourceSpan::none()));

  scalanative::nir::FunctionBodyBuilder userMainBody;
  (void)userMainBody.addEval(
      scalanative::nir::sizeOfValue(std::string(ItemType),
                                    scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());
  (void)userMainBody.addEval(
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.PureDiscard.effect",
                                       scalanative::support::SourceSpan::none()),
          {}, scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());
  (void)userMainBody.addReturn(
      "Int",
      scalanative::nir::blockValue(std::move(blockValues),
                                   scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int",
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.PureDiscard.main",
                                       scalanative::support::SourceSpan::none()),
          {}, scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                std::string(ItemType),
                                "@java.lang.Object",
                                {},
                                scalanative::support::SourceSpan::none()});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                "demo.interflow.PureDiscard.effect",
                                "()Int",
                                {},
                                scalanative::support::SourceSpan::none()});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.PureDiscard.main", "()Int",
                                std::move(userMainBody).build(),
                                scalanative::support::SourceSpan::none()});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(),
                                scalanative::support::SourceSpan::none()});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.PureDiscard.main");
  program.reachableGlobals.push_back("demo.interflow.PureDiscard.effect");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid dead pure discard program")) {
    return code;
  }
  if (int code = expect(result.reports.size() == 5 &&
                            result.reports[2].name == "eliminate-dead-local-lets" &&
                            result.reports[2].changedValues == 4 &&
                            result.reports[2].validationErrorsBefore == 0 &&
                            result.reports[2].validationErrorsAfter == 0 &&
                            result.reports[3].name == "simplify-blocks" &&
                            result.reports[3].changedValues == 1 &&
                            result.reports[3].validationErrorsBefore == 0 &&
                            result.reports[3].validationErrorsAfter == 0,
                        "interflow did not report validation-clean dead pure discard "
                        "elimination")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* optimizedMain =
      findDefinition(optimizedModule, "demo.interflow.PureDiscard.main");
  if (int code = expect(optimizedMain != nullptr,
                        "interflow removed the reachable pure-discard function")) {
    return code;
  }
  const std::vector<std::string> body =
      scalanative::nir::bodyToText(optimizedMain->body);
  return expect(body.size() == 3 && body[0] == "entry:" &&
                    body[1] == "eval call %demo.interflow.PureDiscard.effect()" &&
                    body[2] == "ret Int 4",
                "interflow did not remove only dead pure discard values");
}

int smokeInterflowDeadTypeTestElimination() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();
  constexpr std::string_view BoxType = "demo.interflow.TypeTestDce.Box";

  scalanative::nir::FunctionBodyBuilder unusedLetBody;
  (void)unusedLetBody.addParameter("box", std::string(BoxType), noSpan);
  (void)unusedLetBody.addLet(
      "unused", "Boolean",
      scalanative::nir::isInstanceOfValue(
          std::string(BoxType), scalanative::nir::localValue("box", noSpan), noSpan),
      noSpan);
  (void)unusedLetBody.addReturn(
      "Int", scalanative::nir::literalValue("1", "Int", noSpan), noSpan);

  std::vector<scalanative::nir::Value> blockValues;
  blockValues.push_back(scalanative::nir::isInstanceOfValue(
      std::string(BoxType), scalanative::nir::localValue("box", noSpan), noSpan));
  blockValues.push_back(scalanative::nir::literalValue("2", "Int", noSpan));

  scalanative::nir::FunctionBodyBuilder blockBody;
  (void)blockBody.addParameter("box", std::string(BoxType), noSpan);
  (void)blockBody.addReturn(
      "Int", scalanative::nir::blockValue(std::move(blockValues), noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                std::string(BoxType),
                                "@java.lang.Object",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.TypeTestDce.unusedLet",
                                "(" + std::string(BoxType) + ")Int",
                                std::move(unusedLetBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.TypeTestDce.blockOperand",
                                "(" + std::string(BoxType) + ")Int",
                                std::move(blockBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.TypeTestDce.unusedLet");
  program.reachableGlobals.push_back("demo.interflow.TypeTestDce.blockOperand");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code = expect(result.ok, "interflow rejected valid dead type-test program")) {
    return code;
  }
  if (int code = expect(result.reports.size() == 5 &&
                            result.reports[2].name == "eliminate-dead-local-lets" &&
                            result.reports[2].changedValues == 2 &&
                            result.reports[2].validationErrorsBefore == 0 &&
                            result.reports[2].validationErrorsAfter == 0 &&
                            result.reports[3].name == "simplify-blocks" &&
                            result.reports[3].changedValues == 1 &&
                            result.reports[3].validationErrorsBefore == 0 &&
                            result.reports[3].validationErrorsAfter == 0,
                        "interflow did not report validation-clean dead type-test "
                        "elimination")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* unusedLet =
      findDefinition(optimizedModule, "demo.interflow.TypeTestDce.unusedLet");
  const scalanative::nir::Definition* blockOperand =
      findDefinition(optimizedModule, "demo.interflow.TypeTestDce.blockOperand");
  if (int code = expect(unusedLet != nullptr && blockOperand != nullptr,
                        "interflow removed reachable type-test DCE functions")) {
    return code;
  }

  const std::vector<std::string> unusedLetText =
      scalanative::nir::bodyToText(unusedLet->body);
  if (int code = expect(unusedLetText.size() == 3 &&
                            unusedLetText[1] ==
                                "param %box : demo.interflow.TypeTestDce.Box" &&
                            unusedLetText[2] == "ret Int 1",
                        "interflow did not remove unused local type-test")) {
    return code;
  }

  const std::vector<std::string> blockOperandText =
      scalanative::nir::bodyToText(blockOperand->body);
  return expect(blockOperandText.size() == 3 &&
                    blockOperandText[1] ==
                        "param %box : demo.interflow.TypeTestDce.Box" &&
                    blockOperandText[2] == "ret Int 2",
                "interflow did not remove dead block type-test operand");
}

int smokeInterflowNestedLocalConstantPropagation() {
  scalanative::nir::FunctionBodyBuilder userMainBody;
  (void)userMainBody.addLet("value",
                            scalanative::nir::literalValue(
                                "99", "Int", scalanative::support::SourceSpan::none()),
                            scalanative::support::SourceSpan::none());

  std::vector<scalanative::nir::Value> blockValues;
  blockValues.push_back(scalanative::nir::localLetValue(
      "value", "Int",
      scalanative::nir::literalValue("1", "Int",
                                     scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none()));
  blockValues.push_back(scalanative::nir::localLetValue(
      "total", "Int",
      scalanative::nir::binaryValue(
          "+",
          scalanative::nir::localValue("value",
                                       scalanative::support::SourceSpan::none()),
          scalanative::nir::literalValue("2", "Int",
                                         scalanative::support::SourceSpan::none()),
          scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none()));
  blockValues.push_back(
      scalanative::nir::localValue("total", scalanative::support::SourceSpan::none()));
  (void)userMainBody.addReturn(
      "Int",
      scalanative::nir::blockValue(std::move(blockValues),
                                   scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int",
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.NestedPropagation.main",
                                       scalanative::support::SourceSpan::none()),
          {}, scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.NestedPropagation.main", "()Int",
                                std::move(userMainBody).build(),
                                scalanative::support::SourceSpan::none()});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(),
                                scalanative::support::SourceSpan::none()});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.NestedPropagation.main");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid nested propagation program")) {
    return code;
  }
  if (int code = expect(
          result.reports.size() == 5 &&
              result.reports[0].name == "propagate-local-constants" &&
              result.reports[0].changedValues == 1 &&
              result.reports[1].name == "fold-constants" &&
              result.reports[1].changedValues == 1 &&
              result.reports[2].name == "eliminate-dead-local-lets" &&
              result.reports[2].changedValues == 3 &&
              result.reports[3].name == "simplify-blocks" &&
              result.reports[3].changedValues == 1,
          "interflow did not report scoped nested propagation, fold, DCE, and "
          "block simplification; saw propagate=" +
              std::to_string(result.reports.size() > 0 ? result.reports[0].changedValues
                                                       : 0) +
              ", fold=" +
              std::to_string(result.reports.size() > 1 ? result.reports[1].changedValues
                                                       : 0) +
              ", dce=" +
              std::to_string(result.reports.size() > 2 ? result.reports[2].changedValues
                                                       : 0) +
              ", blocks=" +
              std::to_string(result.reports.size() > 3 ? result.reports[3].changedValues
                                                       : 0))) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* optimizedMain =
      findDefinition(optimizedModule, "demo.interflow.NestedPropagation.main");
  if (int code =
          expect(optimizedMain != nullptr,
                 "interflow removed the reachable nested propagation function")) {
    return code;
  }
  const std::vector<std::string> body =
      scalanative::nir::bodyToText(optimizedMain->body);
  return expect(body.size() == 2 && body.back() == "ret Int 3",
                "interflow did not collapse the shadowed block-local constant");
}

int smokeInterflowImmutableAliasPropagation() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  scalanative::nir::FunctionBodyBuilder immutableBody;
  (void)immutableBody.addParameter("value", "Int", noSpan);
  (void)immutableBody.addLet("alias", "Int",
                             scalanative::nir::localValue("value", noSpan), noSpan);
  (void)immutableBody.addReturn(
      "Int",
      scalanative::nir::binaryValue("+", scalanative::nir::localValue("alias", noSpan),
                                    scalanative::nir::literalValue("0", "Int", noSpan),
                                    noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder mutableBody;
  (void)mutableBody.addVar("current", "Int",
                           scalanative::nir::literalValue("1", "Int", noSpan), noSpan);
  (void)mutableBody.addLet("alias", "Int",
                           scalanative::nir::localValue("current", noSpan), noSpan);
  (void)mutableBody.addEval(
      scalanative::nir::assignValue(scalanative::nir::localValue("current", noSpan),
                                    scalanative::nir::literalValue("2", "Int", noSpan),
                                    noSpan),
      noSpan);
  (void)mutableBody.addReturn("Int", scalanative::nir::localValue("alias", noSpan),
                              noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Alias.immutableValue", "(Int)Int",
                                std::move(immutableBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Alias.mutableValue", "()Int",
                                std::move(mutableBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.Alias.immutableValue");
  program.reachableGlobals.push_back("demo.interflow.Alias.mutableValue");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid alias propagation program")) {
    return code;
  }
  if (int code = expect(
          result.reports.size() == 5 &&
              result.reports[0].name == "propagate-local-constants" &&
              result.reports[0].changedValues == 1 &&
              result.reports[1].name == "fold-constants" &&
              result.reports[1].changedValues == 1 &&
              result.reports[2].name == "eliminate-dead-local-lets" &&
              result.reports[2].changedValues == 1,
          "interflow did not report validation-clean immutable alias propagation")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* immutableMain =
      findDefinition(optimizedModule, "demo.interflow.Alias.immutableValue");
  const scalanative::nir::Definition* mutableMain =
      findDefinition(optimizedModule, "demo.interflow.Alias.mutableValue");
  if (int code = expect(immutableMain != nullptr && mutableMain != nullptr,
                        "interflow removed reachable alias propagation functions")) {
    return code;
  }

  const std::vector<std::string> immutableText =
      scalanative::nir::bodyToText(immutableMain->body);
  if (int code = expect(immutableText.size() == 3 &&
                            immutableText[1] == "param %value : Int" &&
                            immutableText[2] == "ret Int %value",
                        "interflow did not propagate immutable local alias")) {
    return code;
  }

  const std::vector<std::string> mutableText =
      scalanative::nir::bodyToText(mutableMain->body);
  std::ostringstream mutableTextOut;
  for (const std::string& line : mutableText) {
    mutableTextOut << line << '\n';
  }
  return expect(mutableText.size() == 5 && mutableText[1] == "var %current : Int = 1" &&
                    mutableText[2] == "let %alias : Int = %current" &&
                    mutableText[3] == "eval assign %current = 2" &&
                    mutableText[4] == "ret Int %alias",
                "interflow incorrectly propagated a mutable local alias:\n" +
                    mutableTextOut.str());
}

int smokeInterflowLiteralExpressionPropagation() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  scalanative::nir::FunctionBodyBuilder arithmeticBody;
  (void)arithmeticBody.addLet(
      "sum", "Int",
      scalanative::nir::binaryValue(
          "+", scalanative::nir::literalValue("1", "Int", noSpan),
          scalanative::nir::literalValue("2", "Int", noSpan), noSpan),
      noSpan);
  (void)arithmeticBody.addReturn("Int", scalanative::nir::localValue("sum", noSpan),
                                 noSpan);

  scalanative::nir::FunctionBodyBuilder unaryBody;
  (void)unaryBody.addLet(
      "negative", "Int",
      scalanative::nir::unaryValue(
          "-", scalanative::nir::literalValue("4", "Int", noSpan), noSpan),
      noSpan);
  (void)unaryBody.addReturn("Int", scalanative::nir::localValue("negative", noSpan),
                            noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.LiteralExpr.arithmetic", "()Int",
                                std::move(arithmeticBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.LiteralExpr.unary", "()Int",
                                std::move(unaryBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.LiteralExpr.arithmetic");
  program.reachableGlobals.push_back("demo.interflow.LiteralExpr.unary");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid literal expression program")) {
    return code;
  }
  if (int code = expect(result.reports.size() == 5 &&
                            result.reports[0].name == "propagate-local-constants" &&
                            result.reports[0].changedValues == 2 &&
                            result.reports[0].validationErrorsBefore == 0 &&
                            result.reports[0].validationErrorsAfter == 0 &&
                            result.reports[1].name == "fold-constants" &&
                            result.reports[1].changedValues == 4 &&
                            result.reports[1].validationErrorsBefore == 0 &&
                            result.reports[1].validationErrorsAfter == 0 &&
                            result.reports[2].name == "eliminate-dead-local-lets" &&
                            result.reports[2].changedValues == 2,
                        "interflow did not report validation-clean literal expression "
                        "propagation")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* arithmetic =
      findDefinition(optimizedModule, "demo.interflow.LiteralExpr.arithmetic");
  const scalanative::nir::Definition* unary =
      findDefinition(optimizedModule, "demo.interflow.LiteralExpr.unary");
  if (int code = expect(arithmetic != nullptr && unary != nullptr,
                        "interflow removed reachable literal expression functions")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(arithmetic->body).back() == "ret Int 3",
                 "interflow did not propagate and fold literal arithmetic")) {
    return code;
  }
  return expect(scalanative::nir::bodyToText(unary->body).back() == "ret Int -4",
                "interflow did not propagate and fold unary literal expression");
}

int smokeInterflowLiteralIfPropagation() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  scalanative::nir::FunctionBodyBuilder truthyBody;
  (void)truthyBody.addLet("chosen", "Int",
                          scalanative::nir::ifValue(
                              scalanative::nir::literalValue("true", "Boolean", noSpan),
                              scalanative::nir::literalValue("1", "Int", noSpan),
                              scalanative::nir::literalValue("2", "Int", noSpan),
                              noSpan),
                          noSpan);
  (void)truthyBody.addReturn("Int", scalanative::nir::localValue("chosen", noSpan),
                             noSpan);

  scalanative::nir::FunctionBodyBuilder falsyBody;
  (void)falsyBody.addLet("chosen", "Boolean",
                         scalanative::nir::ifValue(
                             scalanative::nir::literalValue("false", "Boolean", noSpan),
                             scalanative::nir::literalValue("false", "Boolean", noSpan),
                             scalanative::nir::literalValue("true", "Boolean", noSpan),
                             noSpan),
                         noSpan);
  (void)falsyBody.addReturn("Boolean", scalanative::nir::localValue("chosen", noSpan),
                            noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.LiteralIf.truthy", "()Int",
                                std::move(truthyBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.LiteralIf.falsy", "()Boolean",
                                std::move(falsyBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.LiteralIf.truthy");
  program.reachableGlobals.push_back("demo.interflow.LiteralIf.falsy");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code = expect(result.ok, "interflow rejected valid literal-if program")) {
    return code;
  }
  if (int code = expect(
          result.reports.size() == 5 &&
              result.reports[0].name == "propagate-local-constants" &&
              result.reports[0].changedValues == 2 &&
              result.reports[0].validationErrorsBefore == 0 &&
              result.reports[0].validationErrorsAfter == 0 &&
              result.reports[1].name == "fold-constants" &&
              result.reports[1].changedValues == 4 &&
              result.reports[1].validationErrorsBefore == 0 &&
              result.reports[1].validationErrorsAfter == 0 &&
              result.reports[2].name == "eliminate-dead-local-lets" &&
              result.reports[2].changedValues == 2,
          "interflow did not report validation-clean literal-if propagation; "
          "saw propagate=" +
              std::to_string(result.reports.size() > 0 ? result.reports[0].changedValues
                                                       : 0) +
              ", fold=" +
              std::to_string(result.reports.size() > 1 ? result.reports[1].changedValues
                                                       : 0) +
              ", dce=" +
              std::to_string(result.reports.size() > 2 ? result.reports[2].changedValues
                                                       : 0))) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* truthy =
      findDefinition(optimizedModule, "demo.interflow.LiteralIf.truthy");
  const scalanative::nir::Definition* falsy =
      findDefinition(optimizedModule, "demo.interflow.LiteralIf.falsy");
  if (int code = expect(truthy != nullptr && falsy != nullptr,
                        "interflow removed reachable literal-if functions")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(truthy->body).back() == "ret Int 1",
                 "interflow did not propagate and fold literal Int if")) {
    return code;
  }
  return expect(scalanative::nir::bodyToText(falsy->body).back() == "ret Boolean true",
                "interflow did not propagate and fold literal Boolean if");
}

int smokeInterflowSizeOfPropagation() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  scalanative::nir::FunctionBodyBuilder userMainBody;
  (void)userMainBody.addLet("width", "Int",
                            scalanative::nir::sizeOfValue("Int", noSpan), noSpan);
  (void)userMainBody.addReturn(
      "Int",
      scalanative::nir::binaryValue("+", scalanative::nir::localValue("width", noSpan),
                                    scalanative::nir::literalValue("0", "Int", noSpan),
                                    noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder sameTypeEqualsBody;
  (void)sameTypeEqualsBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue("==", scalanative::nir::sizeOfValue("Int", noSpan),
                                    scalanative::nir::sizeOfValue("Int", noSpan),
                                    noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder sameTypeNotEqualsBody;
  (void)sameTypeNotEqualsBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue("!=", scalanative::nir::sizeOfValue("Long", noSpan),
                                    scalanative::nir::sizeOfValue("Long", noSpan),
                                    noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder sameTypeLessEqualsBody;
  (void)sameTypeLessEqualsBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue("<=", scalanative::nir::sizeOfValue("Char", noSpan),
                                    scalanative::nir::sizeOfValue("Char", noSpan),
                                    noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder sameTypeGreaterBody;
  (void)sameTypeGreaterBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          ">", scalanative::nir::sizeOfValue("Boolean", noSpan),
          scalanative::nir::sizeOfValue("Boolean", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder sameTypeDifferenceBody;
  (void)sameTypeDifferenceBody.addReturn(
      "Int",
      scalanative::nir::binaryValue("-", scalanative::nir::sizeOfValue("Int", noSpan),
                                    scalanative::nir::sizeOfValue("Int", noSpan),
                                    noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.SizeOfAlias.main", "()Int",
                                std::move(userMainBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.SizeOfAlias.sameTypeEquals",
                                "()Boolean", std::move(sameTypeEqualsBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.SizeOfAlias.sameTypeNotEquals",
                                "()Boolean", std::move(sameTypeNotEqualsBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.SizeOfAlias.sameTypeLessEquals",
                                "()Boolean", std::move(sameTypeLessEqualsBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.SizeOfAlias.sameTypeGreater",
                                "()Boolean", std::move(sameTypeGreaterBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.SizeOfAlias.sameTypeDifference",
                                "()Int", std::move(sameTypeDifferenceBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.SizeOfAlias.main");
  program.reachableGlobals.push_back("demo.interflow.SizeOfAlias.sameTypeEquals");
  program.reachableGlobals.push_back("demo.interflow.SizeOfAlias.sameTypeNotEquals");
  program.reachableGlobals.push_back("demo.interflow.SizeOfAlias.sameTypeLessEquals");
  program.reachableGlobals.push_back("demo.interflow.SizeOfAlias.sameTypeGreater");
  program.reachableGlobals.push_back("demo.interflow.SizeOfAlias.sameTypeDifference");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid sizeof propagation program")) {
    return code;
  }
  if (int code =
          expect(result.reports.size() == 5 &&
                     result.reports[0].name == "propagate-local-constants" &&
                     result.reports[0].changedValues == 1 &&
                     result.reports[1].name == "fold-constants" &&
                     result.reports[1].changedValues == 6 &&
                     result.reports[2].name == "eliminate-dead-local-lets" &&
                     result.reports[2].changedValues == 1,
                 "interflow did not report validation-clean sizeof propagation")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* optimizedMain =
      findDefinition(optimizedModule, "demo.interflow.SizeOfAlias.main");
  const scalanative::nir::Definition* optimizedEquals =
      findDefinition(optimizedModule, "demo.interflow.SizeOfAlias.sameTypeEquals");
  const scalanative::nir::Definition* optimizedNotEquals =
      findDefinition(optimizedModule, "demo.interflow.SizeOfAlias.sameTypeNotEquals");
  const scalanative::nir::Definition* optimizedLessEquals =
      findDefinition(optimizedModule, "demo.interflow.SizeOfAlias.sameTypeLessEquals");
  const scalanative::nir::Definition* optimizedGreater =
      findDefinition(optimizedModule, "demo.interflow.SizeOfAlias.sameTypeGreater");
  const scalanative::nir::Definition* optimizedDifference =
      findDefinition(optimizedModule, "demo.interflow.SizeOfAlias.sameTypeDifference");
  if (int code =
          expect(optimizedMain != nullptr && optimizedEquals != nullptr &&
                     optimizedNotEquals != nullptr && optimizedLessEquals != nullptr &&
                     optimizedGreater != nullptr && optimizedDifference != nullptr,
                 "interflow removed a reachable sizeof propagation function")) {
    return code;
  }

  const std::vector<std::string> body =
      scalanative::nir::bodyToText(optimizedMain->body);
  if (int code = expect(body.size() == 2 && body.back() == "ret Int sizeof[Int]",
                        "interflow did not propagate and simplify sizeof alias")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(optimizedEquals->body).back() ==
                            "ret Boolean true",
                        "interflow did not fold same-type sizeof equality")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(optimizedNotEquals->body).back() ==
                            "ret Boolean false",
                        "interflow did not fold same-type sizeof inequality")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(optimizedLessEquals->body).back() ==
                     "ret Boolean true",
                 "interflow did not fold same-type sizeof less-equals")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(optimizedGreater->body).back() ==
                            "ret Boolean false",
                        "interflow did not fold same-type sizeof greater-than")) {
    return code;
  }
  return expect(scalanative::nir::bodyToText(optimizedDifference->body).back() ==
                    "ret Int 0",
                "interflow did not fold same-type sizeof subtraction");
}

int smokeInterflowBoxUnboxFold() {
  scalanative::nir::FunctionBodyBuilder userMainBody;
  (void)userMainBody.addReturn(
      "Int",
      scalanative::nir::unboxValue(
          "Int",
          scalanative::nir::boxValue(
              "Int",
              scalanative::nir::literalValue("42", "Int",
                                             scalanative::support::SourceSpan::none()),
              scalanative::support::SourceSpan::none()),
          scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());

  scalanative::nir::FunctionBodyBuilder localBody;
  (void)localBody.addLet("boxed", "Object",
                         scalanative::nir::boxValue(
                             "Int",
                             scalanative::nir::literalValue(
                                 "42", "Int", scalanative::support::SourceSpan::none()),
                             scalanative::support::SourceSpan::none()),
                         scalanative::support::SourceSpan::none());
  (void)localBody.addReturn("Int",
                            scalanative::nir::unboxValue(
                                "Int",
                                scalanative::nir::localValue(
                                    "boxed", scalanative::support::SourceSpan::none()),
                                scalanative::support::SourceSpan::none()),
                            scalanative::support::SourceSpan::none());

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int",
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.Boxed.main",
                                       scalanative::support::SourceSpan::none()),
          {}, scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Boxed.main", "()Int",
                                std::move(userMainBody).build(),
                                scalanative::support::SourceSpan::none()});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Boxed.local", "()Int",
                                std::move(localBody).build(),
                                scalanative::support::SourceSpan::none()});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(),
                                scalanative::support::SourceSpan::none()});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.Boxed.main");
  program.reachableGlobals.push_back("demo.interflow.Boxed.local");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code = expect(result.ok, "interflow rejected valid box/unbox fold program")) {
    return code;
  }
  if (int code =
          expect(result.changedValues == 3,
                 "interflow did not report exactly three box/unbox changes; saw " +
                     std::to_string(result.changedValues))) {
    return code;
  }
  if (int code = expect(
          result.reports.size() == 5 && result.reports[1].name == "fold-constants" &&
              result.reports[1].changedValues == 2 &&
              result.reports[1].validationErrorsBefore == 0 &&
              result.reports[1].validationErrorsAfter == 0 &&
              result.reports[2].name == "eliminate-dead-local-lets" &&
              result.reports[2].changedValues == 1 &&
              result.reports[2].validationErrorsBefore == 0 &&
              result.reports[2].validationErrorsAfter == 0,
          "interflow did not fold box/unbox during the local simplification pass")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* foldedMain =
      findDefinition(optimizedModule, "demo.interflow.Boxed.main");
  const scalanative::nir::Definition* foldedLocal =
      findDefinition(optimizedModule, "demo.interflow.Boxed.local");
  if (int code = expect(foldedMain != nullptr &&
                            scalanative::nir::bodyToText(foldedMain->body).back() ==
                                "ret Int 42",
                        "interflow did not replace unbox[Int](box[Int](42)) with 42")) {
    return code;
  }
  if (int code = expect(
          foldedLocal != nullptr &&
              scalanative::nir::bodyToText(foldedLocal->body).size() == 2 &&
              scalanative::nir::bodyToText(foldedLocal->body).back() == "ret Int 42",
          "interflow did not fold unbox[Int] of a known boxed local")) {
    return code;
  }
  const std::string body = scalanative::nir::bodyToText(foldedMain->body).back();
  const std::string localBodyText =
      scalanative::nir::bodyToText(foldedLocal->body).back();
  return expect(!contains(body, "box[") && !contains(body, "unbox[") &&
                    !contains(localBodyText, "box[") &&
                    !contains(localBodyText, "unbox["),
                "interflow kept a redundant box/unbox pair in optimized NIR");
}

int smokeInterflowExactBoxedAnyEqualsFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();
  const auto anyEquals = [&]() {
    return scalanative::nir::localValue("scala.scalanative.runtime.anyEquals", noSpan);
  };

  scalanative::nir::FunctionBodyBuilder equalBody;
  (void)equalBody.addReturn(
      "Boolean",
      scalanative::nir::callValue(
          anyEquals(),
          {scalanative::nir::boxValue(
               "Int", scalanative::nir::literalValue("7", "Int", noSpan), noSpan),
           scalanative::nir::boxValue(
               "Int", scalanative::nir::literalValue("7", "Int", noSpan), noSpan)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder differentPayloadBody;
  (void)differentPayloadBody.addReturn(
      "Boolean",
      scalanative::nir::callValue(
          anyEquals(),
          {scalanative::nir::boxValue(
               "Int", scalanative::nir::literalValue("7", "Int", noSpan), noSpan),
           scalanative::nir::boxValue(
               "Int", scalanative::nir::literalValue("8", "Int", noSpan), noSpan)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder differentTypeBody;
  (void)differentTypeBody.addReturn(
      "Boolean",
      scalanative::nir::callValue(
          anyEquals(),
          {scalanative::nir::boxValue(
               "Int", scalanative::nir::literalValue("7", "Int", noSpan), noSpan),
           scalanative::nir::boxValue(
               "Long", scalanative::nir::literalValue("7L", "Long", noSpan), noSpan)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder floatRoundedEqualBody;
  (void)floatRoundedEqualBody.addReturn(
      "Boolean",
      scalanative::nir::callValue(
          anyEquals(),
          {scalanative::nir::boxValue(
               "Float", scalanative::nir::literalValue("1.0F", "Float", noSpan),
               noSpan),
           scalanative::nir::boxValue(
               "Float", scalanative::nir::literalValue("1.00000001F", "Float", noSpan),
               noSpan)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder nullEqualBody;
  (void)nullEqualBody.addReturn(
      "Boolean",
      scalanative::nir::callValue(
          anyEquals(),
          {scalanative::nir::literalValue("null", "Null", noSpan),
           scalanative::nir::literalValue("null", "Null", noSpan)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder nullBoxedBody;
  (void)nullBoxedBody.addReturn(
      "Boolean",
      scalanative::nir::callValue(
          anyEquals(),
          {scalanative::nir::literalValue("null", "Null", noSpan),
           scalanative::nir::boxValue(
               "Int", scalanative::nir::literalValue("7", "Int", noSpan), noSpan)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder nullStringBody;
  (void)nullStringBody.addReturn(
      "Boolean",
      scalanative::nir::callValue(
          anyEquals(),
          {scalanative::nir::literalValue("null", "Null", noSpan),
           scalanative::nir::literalValue("\"Scala\"", "String", noSpan)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder stringEqualBody;
  (void)stringEqualBody.addReturn(
      "Boolean",
      scalanative::nir::callValue(
          anyEquals(),
          {scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
           scalanative::nir::literalValue("\"Scala\"", "String", noSpan)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder stringDifferentBody;
  (void)stringDifferentBody.addReturn(
      "Boolean",
      scalanative::nir::callValue(
          anyEquals(),
          {scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
           scalanative::nir::literalValue("\"Native\"", "String", noSpan)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localBody;
  (void)localBody.addLet(
      "left", "Object",
      scalanative::nir::boxValue(
          "String", scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
          noSpan),
      noSpan);
  (void)localBody.addLet(
      "right", "Object",
      scalanative::nir::boxValue(
          "String", scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
          noSpan),
      noSpan);
  (void)localBody.addReturn(
      "Boolean",
      scalanative::nir::callValue(anyEquals(),
                                  {scalanative::nir::localValue("left", noSpan),
                                   scalanative::nir::localValue("right", noSpan)},
                                  noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localStringBody;
  (void)localStringBody.addLet(
      "left", "String", scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
      noSpan);
  (void)localStringBody.addLet(
      "right", "String", scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
      noSpan);
  (void)localStringBody.addReturn(
      "Boolean",
      scalanative::nir::callValue(anyEquals(),
                                  {scalanative::nir::localValue("left", noSpan),
                                   scalanative::nir::localValue("right", noSpan)},
                                  noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localNullBoxedBody;
  (void)localNullBoxedBody.addLet(
      "missing", "Object", scalanative::nir::literalValue("null", "Null", noSpan),
      noSpan);
  (void)localNullBoxedBody.addLet(
      "value", "Object",
      scalanative::nir::boxValue(
          "Int", scalanative::nir::literalValue("7", "Int", noSpan), noSpan),
      noSpan);
  (void)localNullBoxedBody.addReturn(
      "Boolean",
      scalanative::nir::callValue(anyEquals(),
                                  {scalanative::nir::localValue("missing", noSpan),
                                   scalanative::nir::localValue("value", noSpan)},
                                  noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localNullStringBody;
  (void)localNullStringBody.addLet(
      "missing", "Object", scalanative::nir::literalValue("null", "Null", noSpan),
      noSpan);
  (void)localNullStringBody.addLet(
      "value", "String", scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
      noSpan);
  (void)localNullStringBody.addReturn(
      "Boolean",
      scalanative::nir::callValue(anyEquals(),
                                  {scalanative::nir::localValue("missing", noSpan),
                                   scalanative::nir::localValue("value", noSpan)},
                                  noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                "scala.scalanative.runtime.anyEquals",
                                "(Object,Object)Boolean",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.AnyEquals.equal", "()Boolean",
                                std::move(equalBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.AnyEquals.differentPayload",
                                "()Boolean", std::move(differentPayloadBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.AnyEquals.differentType", "()Boolean",
                                std::move(differentTypeBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.AnyEquals.floatRoundedEqual",
                                "()Boolean", std::move(floatRoundedEqualBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.AnyEquals.nullEqual", "()Boolean",
                                std::move(nullEqualBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.AnyEquals.nullBoxed", "()Boolean",
                                std::move(nullBoxedBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.AnyEquals.nullString", "()Boolean",
                                std::move(nullStringBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.AnyEquals.stringEqual", "()Boolean",
                                std::move(stringEqualBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.AnyEquals.stringDifferent", "()Boolean",
                                std::move(stringDifferentBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.AnyEquals.local", "()Boolean",
                                std::move(localBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.AnyEquals.localString", "()Boolean",
                                std::move(localStringBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.AnyEquals.localNullBoxed", "()Boolean",
                                std::move(localNullBoxedBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.AnyEquals.localNullString", "()Boolean",
                                std::move(localNullStringBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.AnyEquals.equal");
  program.reachableGlobals.push_back("demo.interflow.AnyEquals.differentPayload");
  program.reachableGlobals.push_back("demo.interflow.AnyEquals.differentType");
  program.reachableGlobals.push_back("demo.interflow.AnyEquals.floatRoundedEqual");
  program.reachableGlobals.push_back("demo.interflow.AnyEquals.nullEqual");
  program.reachableGlobals.push_back("demo.interflow.AnyEquals.nullBoxed");
  program.reachableGlobals.push_back("demo.interflow.AnyEquals.nullString");
  program.reachableGlobals.push_back("demo.interflow.AnyEquals.stringEqual");
  program.reachableGlobals.push_back("demo.interflow.AnyEquals.stringDifferent");
  program.reachableGlobals.push_back("demo.interflow.AnyEquals.local");
  program.reachableGlobals.push_back("demo.interflow.AnyEquals.localString");
  program.reachableGlobals.push_back("demo.interflow.AnyEquals.localNullBoxed");
  program.reachableGlobals.push_back("demo.interflow.AnyEquals.localNullString");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid exact boxed anyEquals program")) {
    return code;
  }
  if (int code = expect(result.reports.size() == 5 &&
                            result.reports[1].name == "fold-constants" &&
                            result.reports[1].changedValues == 13 &&
                            result.reports[1].validationErrorsBefore == 0 &&
                            result.reports[1].validationErrorsAfter == 0 &&
                            result.reports[2].name == "eliminate-dead-local-lets" &&
                            result.reports[2].changedValues == 8 &&
                            result.reports[2].validationErrorsBefore == 0 &&
                            result.reports[2].validationErrorsAfter == 0,
                        "interflow did not report validation-clean exact boxed "
                        "anyEquals folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* equal =
      findDefinition(optimizedModule, "demo.interflow.AnyEquals.equal");
  const scalanative::nir::Definition* differentPayload =
      findDefinition(optimizedModule, "demo.interflow.AnyEquals.differentPayload");
  const scalanative::nir::Definition* differentType =
      findDefinition(optimizedModule, "demo.interflow.AnyEquals.differentType");
  const scalanative::nir::Definition* floatRoundedEqual =
      findDefinition(optimizedModule, "demo.interflow.AnyEquals.floatRoundedEqual");
  const scalanative::nir::Definition* nullEqual =
      findDefinition(optimizedModule, "demo.interflow.AnyEquals.nullEqual");
  const scalanative::nir::Definition* nullBoxed =
      findDefinition(optimizedModule, "demo.interflow.AnyEquals.nullBoxed");
  const scalanative::nir::Definition* nullString =
      findDefinition(optimizedModule, "demo.interflow.AnyEquals.nullString");
  const scalanative::nir::Definition* stringEqual =
      findDefinition(optimizedModule, "demo.interflow.AnyEquals.stringEqual");
  const scalanative::nir::Definition* stringDifferent =
      findDefinition(optimizedModule, "demo.interflow.AnyEquals.stringDifferent");
  const scalanative::nir::Definition* local =
      findDefinition(optimizedModule, "demo.interflow.AnyEquals.local");
  const scalanative::nir::Definition* localString =
      findDefinition(optimizedModule, "demo.interflow.AnyEquals.localString");
  const scalanative::nir::Definition* localNullBoxed =
      findDefinition(optimizedModule, "demo.interflow.AnyEquals.localNullBoxed");
  const scalanative::nir::Definition* localNullString =
      findDefinition(optimizedModule, "demo.interflow.AnyEquals.localNullString");
  if (int code = expect(
          equal != nullptr && differentPayload != nullptr && differentType != nullptr &&
              floatRoundedEqual != nullptr && nullEqual != nullptr &&
              nullBoxed != nullptr && nullString != nullptr && stringEqual != nullptr &&
              stringDifferent != nullptr && local != nullptr &&
              localString != nullptr && localNullBoxed != nullptr &&
              localNullString != nullptr,
          "interflow removed reachable exact boxed anyEquals functions")) {
    return code;
  }
  if (int code = expect(
          scalanative::nir::bodyToText(equal->body).back() == "ret Boolean true" &&
              scalanative::nir::bodyToText(differentPayload->body).back() ==
                  "ret Boolean false" &&
              scalanative::nir::bodyToText(differentType->body).back() ==
                  "ret Boolean false" &&
              scalanative::nir::bodyToText(floatRoundedEqual->body).back() ==
                  "ret Boolean true" &&
              scalanative::nir::bodyToText(nullEqual->body).back() ==
                  "ret Boolean true" &&
              scalanative::nir::bodyToText(nullBoxed->body).back() ==
                  "ret Boolean false" &&
              scalanative::nir::bodyToText(nullString->body).back() ==
                  "ret Boolean false" &&
              scalanative::nir::bodyToText(stringEqual->body).back() ==
                  "ret Boolean true" &&
              scalanative::nir::bodyToText(stringDifferent->body).back() ==
                  "ret Boolean false" &&
              scalanative::nir::bodyToText(local->body).back() == "ret Boolean true" &&
              scalanative::nir::bodyToText(localString->body).back() ==
                  "ret Boolean true" &&
              scalanative::nir::bodyToText(localNullBoxed->body).back() ==
                  "ret Boolean false" &&
              scalanative::nir::bodyToText(localNullString->body).back() ==
                  "ret Boolean false",
          "interflow did not fold exact boxed anyEquals results")) {
    return code;
  }
  return expect(scalanative::nir::bodyToText(local->body).size() == 2 &&
                    scalanative::nir::bodyToText(localString->body).size() == 2 &&
                    scalanative::nir::bodyToText(localNullBoxed->body).size() == 2 &&
                    scalanative::nir::bodyToText(localNullString->body).size() == 2,
                "interflow did not remove dead exact boxed anyEquals locals");
}

int smokeInterflowExactRuntimeHashCodeFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();
  const auto runtime = [&](std::string_view name) {
    return scalanative::nir::localValue(std::string(name), noSpan);
  };
  const auto hashCall = [&](std::string_view name, scalanative::nir::Value value) {
    return scalanative::nir::callValue(runtime(name), {std::move(value)}, noSpan);
  };

  scalanative::nir::FunctionBodyBuilder booleanTrueBody;
  (void)booleanTrueBody.addReturn(
      "Int",
      hashCall(scalanative::support::StdNames::RuntimeBooleanHashCode,
               scalanative::nir::literalValue("true", "Boolean", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder booleanFalseBody;
  (void)booleanFalseBody.addReturn(
      "Int",
      hashCall(scalanative::support::StdNames::RuntimeBooleanHashCode,
               scalanative::nir::literalValue("false", "Boolean", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder wideLongBody;
  (void)wideLongBody.addReturn(
      "Int",
      hashCall(scalanative::support::StdNames::RuntimeLongHashCode,
               scalanative::nir::literalValue("4294967296L", "Long", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder floatBody;
  (void)floatBody.addReturn(
      "Int",
      hashCall(scalanative::support::StdNames::RuntimeFloatHashCode,
               scalanative::nir::literalValue("1.5F", "Float", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder doubleBody;
  (void)doubleBody.addReturn(
      "Int",
      hashCall(scalanative::support::StdNames::RuntimeDoubleHashCode,
               scalanative::nir::literalValue("2.25", "Double", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder floatNegativeZeroBody;
  (void)floatNegativeZeroBody.addReturn(
      "Int",
      hashCall(scalanative::support::StdNames::RuntimeFloatHashCode,
               scalanative::nir::literalValue("-0.0F", "Float", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder charBody;
  (void)charBody.addReturn(
      "Int",
      hashCall(scalanative::support::StdNames::RuntimeCharHashCode,
               scalanative::nir::literalValue("'A'", "Char", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder stringBody;
  (void)stringBody.addReturn(
      "Int",
      hashCall(scalanative::support::StdNames::RuntimeStringHashCode,
               scalanative::nir::literalValue("\"Scala\"", "String", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder symbolBody;
  (void)symbolBody.addReturn(
      "Int",
      hashCall(scalanative::support::StdNames::RuntimeSymbolHashCode,
               scalanative::nir::literalValue("'ready", "Symbol", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder stringNullBody;
  (void)stringNullBody.addReturn(
      "Int",
      hashCall(scalanative::support::StdNames::RuntimeStringHashCode,
               scalanative::nir::literalValue("null", "Null", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder anyNullBody;
  (void)anyNullBody.addReturn(
      "Int",
      hashCall(scalanative::support::StdNames::RuntimeAnyHashCode,
               scalanative::nir::literalValue("null", "Null", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder anyUnitBody;
  (void)anyUnitBody.addReturn(
      "Int",
      hashCall(scalanative::support::StdNames::RuntimeAnyHashCode,
               scalanative::nir::boxValue("Unit", scalanative::nir::unitValue(noSpan),
                                          noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder anyIntBody;
  (void)anyIntBody.addReturn(
      "Int",
      hashCall(scalanative::support::StdNames::RuntimeAnyHashCode,
               scalanative::nir::boxValue(
                   "Int", scalanative::nir::literalValue("7", "Int", noSpan), noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder anyFloatBody;
  (void)anyFloatBody.addReturn(
      "Int",
      hashCall(scalanative::support::StdNames::RuntimeAnyHashCode,
               scalanative::nir::boxValue(
                   "Float", scalanative::nir::literalValue("1.5F", "Float", noSpan),
                   noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder anyDoubleBody;
  (void)anyDoubleBody.addReturn(
      "Int",
      hashCall(scalanative::support::StdNames::RuntimeAnyHashCode,
               scalanative::nir::boxValue(
                   "Double", scalanative::nir::literalValue("2.25", "Double", noSpan),
                   noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder anyStringBody;
  (void)anyStringBody.addReturn(
      "Int",
      hashCall(scalanative::support::StdNames::RuntimeAnyHashCode,
               scalanative::nir::boxValue(
                   "String",
                   scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
                   noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localBoxedLongBody;
  (void)localBoxedLongBody.addLet(
      "value", "Object",
      scalanative::nir::boxValue(
          "Long", scalanative::nir::literalValue("4294967296L", "Long", noSpan),
          noSpan),
      noSpan);
  (void)localBoxedLongBody.addReturn(
      "Int",
      hashCall(scalanative::support::StdNames::RuntimeAnyHashCode,
               scalanative::nir::localValue("value", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localBoxedDoubleBody;
  (void)localBoxedDoubleBody.addLet(
      "value", "Object",
      scalanative::nir::boxValue(
          "Double", scalanative::nir::literalValue("-0.0", "Double", noSpan), noSpan),
      noSpan);
  (void)localBoxedDoubleBody.addReturn(
      "Int",
      hashCall(scalanative::support::StdNames::RuntimeAnyHashCode,
               scalanative::nir::localValue("value", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder chainedBoxedBody;
  (void)chainedBoxedBody.addLet(
      "boxed", "Object",
      scalanative::nir::boxValue(
          "Int", scalanative::nir::literalValue("11", "Int", noSpan), noSpan),
      noSpan);
  (void)chainedBoxedBody.addLet("first", "Object",
                                scalanative::nir::localValue("boxed", noSpan), noSpan);
  (void)chainedBoxedBody.addLet("second", "Object",
                                scalanative::nir::localValue("first", noSpan), noSpan);
  (void)chainedBoxedBody.addReturn(
      "Int",
      hashCall(scalanative::support::StdNames::RuntimeAnyHashCode,
               scalanative::nir::localValue("second", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localStringBody;
  (void)localStringBody.addLet(
      "value", "String", scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
      noSpan);
  (void)localStringBody.addReturn(
      "Int",
      hashCall(scalanative::support::StdNames::RuntimeStringHashCode,
               scalanative::nir::localValue("value", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeBooleanHashCode),
       "(Boolean)Int",
       {},
       noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeLongHashCode),
       "(Long)Int",
       {},
       noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeFloatHashCode),
       "(Float)Int",
       {},
       noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeDoubleHashCode),
       "(Double)Int",
       {},
       noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeCharHashCode),
       "(Char)Int",
       {},
       noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeStringHashCode),
       "(String)Int",
       {},
       noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeSymbolHashCode),
       "(Symbol)Int",
       {},
       noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeAnyHashCode),
       "(Object)Int",
       {},
       noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Hash.booleanTrue", "()Int",
                                std::move(booleanTrueBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Hash.booleanFalse", "()Int",
                                std::move(booleanFalseBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Hash.wideLong", "()Int",
                                std::move(wideLongBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Hash.floatValue", "()Int",
                                std::move(floatBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Hash.doubleValue", "()Int",
                                std::move(doubleBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Hash.floatNegativeZero", "()Int",
                                std::move(floatNegativeZeroBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Hash.charValue", "()Int",
                                std::move(charBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Hash.stringValue", "()Int",
                                std::move(stringBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Hash.symbolValue", "()Int",
                                std::move(symbolBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Hash.stringNull", "()Int",
                                std::move(stringNullBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Hash.anyNull", "()Int",
                                std::move(anyNullBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Hash.anyUnit", "()Int",
                                std::move(anyUnitBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Hash.anyInt", "()Int",
                                std::move(anyIntBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Hash.anyFloat", "()Int",
                                std::move(anyFloatBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Hash.anyDouble", "()Int",
                                std::move(anyDoubleBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Hash.anyString", "()Int",
                                std::move(anyStringBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Hash.localBoxedLong", "()Int",
                                std::move(localBoxedLongBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Hash.localBoxedDouble", "()Int",
                                std::move(localBoxedDoubleBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Hash.chainedBoxed", "()Int",
                                std::move(chainedBoxedBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Hash.localString", "()Int",
                                std::move(localStringBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  const std::vector<std::string> roots = {
      "demo.interflow.Hash.booleanTrue",    "demo.interflow.Hash.booleanFalse",
      "demo.interflow.Hash.wideLong",       "demo.interflow.Hash.floatValue",
      "demo.interflow.Hash.doubleValue",    "demo.interflow.Hash.floatNegativeZero",
      "demo.interflow.Hash.charValue",      "demo.interflow.Hash.stringValue",
      "demo.interflow.Hash.symbolValue",    "demo.interflow.Hash.stringNull",
      "demo.interflow.Hash.anyNull",        "demo.interflow.Hash.anyUnit",
      "demo.interflow.Hash.anyInt",         "demo.interflow.Hash.anyFloat",
      "demo.interflow.Hash.anyDouble",      "demo.interflow.Hash.anyString",
      "demo.interflow.Hash.localBoxedLong", "demo.interflow.Hash.localBoxedDouble",
      "demo.interflow.Hash.chainedBoxed",   "demo.interflow.Hash.localString"};
  program.reachableGlobals.insert(program.reachableGlobals.end(), roots.begin(),
                                  roots.end());

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid exact runtime hash program")) {
    return code;
  }
  if (int code = expect(result.reports.size() == 5 &&
                            result.reports[1].name == "fold-constants" &&
                            result.reports[1].changedValues == 19 &&
                            result.reports[1].validationErrorsBefore == 0 &&
                            result.reports[1].validationErrorsAfter == 0 &&
                            result.reports[2].name == "eliminate-dead-local-lets" &&
                            result.reports[2].changedValues == 6 &&
                            result.reports[2].validationErrorsBefore == 0 &&
                            result.reports[2].validationErrorsAfter == 0,
                        "interflow did not report validation-clean exact runtime "
                        "hash folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const std::vector<std::pair<std::string, std::string>> expected = {
      {"demo.interflow.Hash.booleanTrue", "ret Int 1231"},
      {"demo.interflow.Hash.booleanFalse", "ret Int 1237"},
      {"demo.interflow.Hash.wideLong", "ret Int 1"},
      {"demo.interflow.Hash.floatValue", "ret Int 1069547520"},
      {"demo.interflow.Hash.doubleValue", "ret Int 1073872896"},
      {"demo.interflow.Hash.floatNegativeZero", "ret Int -2147483648"},
      {"demo.interflow.Hash.charValue", "ret Int 65"},
      {"demo.interflow.Hash.stringValue", "ret Int 79698214"},
      {"demo.interflow.Hash.symbolValue", "ret Int 1224923612"},
      {"demo.interflow.Hash.stringNull",
       "ret Int call %scala.scalanative.runtime.stringHashCode(null)"},
      {"demo.interflow.Hash.anyNull", "ret Int 0"},
      {"demo.interflow.Hash.anyUnit", "ret Int 0"},
      {"demo.interflow.Hash.anyInt", "ret Int 7"},
      {"demo.interflow.Hash.anyFloat", "ret Int 1069547520"},
      {"demo.interflow.Hash.anyDouble", "ret Int 1073872896"},
      {"demo.interflow.Hash.anyString", "ret Int 79698214"},
      {"demo.interflow.Hash.localBoxedLong", "ret Int 1"},
      {"demo.interflow.Hash.localBoxedDouble", "ret Int -2147483648"},
      {"demo.interflow.Hash.chainedBoxed", "ret Int 11"},
      {"demo.interflow.Hash.localString", "ret Int 79698214"}};
  for (const auto& [name, expectedReturn] : expected) {
    const scalanative::nir::Definition* definition =
        findDefinition(optimizedModule, name);
    if (int code = expect(definition != nullptr,
                          "interflow removed reachable exact runtime hash function")) {
      return code;
    }
    const std::vector<std::string> body =
        scalanative::nir::bodyToText(definition->body);
    const std::string actualReturn = body.empty() ? "<empty>" : body.back();
    if (int code = expect(!body.empty() && actualReturn == expectedReturn,
                          "interflow did not fold exact runtime hash result for " +
                              name + ": expected `" + expectedReturn + "`, got `" +
                              actualReturn + "`")) {
      return code;
    }
    if ((name == "demo.interflow.Hash.localBoxedLong" ||
         name == "demo.interflow.Hash.localBoxedDouble" ||
         name == "demo.interflow.Hash.chainedBoxed" ||
         name == "demo.interflow.Hash.localString") &&
        body.size() != 2) {
      return expect(false, "interflow did not remove dead exact runtime hash locals");
    }
  }
  return 0;
}

int smokeInterflowExactRuntimeStringLengthFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();
  const auto stringLength = [&]() {
    return scalanative::nir::localValue(
        std::string(scalanative::support::StdNames::RuntimeStringLength), noSpan);
  };
  const auto lengthCall = [&](scalanative::nir::Value value) {
    return scalanative::nir::callValue(stringLength(), {std::move(value)}, noSpan);
  };

  scalanative::nir::FunctionBodyBuilder directBody;
  (void)directBody.addReturn(
      "Int", lengthCall(scalanative::nir::literalValue("\"Scala\"", "String", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder emptyBody;
  (void)emptyBody.addReturn(
      "Int", lengthCall(scalanative::nir::literalValue("\"\"", "String", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder escapedBody;
  (void)escapedBody.addReturn(
      "Int", lengthCall(scalanative::nir::literalValue("\"A\\nB\"", "String", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder tripleBody;
  (void)tripleBody.addReturn("Int",
                             lengthCall(scalanative::nir::literalValue(
                                 "\"\"\"Native\"\"\"", "String", noSpan)),
                             noSpan);

  scalanative::nir::FunctionBodyBuilder localBody;
  (void)localBody.addLet("value", "String",
                         scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
                         noSpan);
  (void)localBody.addReturn(
      "Int", lengthCall(scalanative::nir::localValue("value", noSpan)), noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeStringLength),
       "(String)Int",
       {},
       noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.StringLength.direct", "()Int",
                                std::move(directBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.StringLength.empty", "()Int",
                                std::move(emptyBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.StringLength.escaped", "()Int",
                                std::move(escapedBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.StringLength.triple", "()Int",
                                std::move(tripleBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.StringLength.local", "()Int",
                                std::move(localBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.StringLength.direct");
  program.reachableGlobals.push_back("demo.interflow.StringLength.empty");
  program.reachableGlobals.push_back("demo.interflow.StringLength.escaped");
  program.reachableGlobals.push_back("demo.interflow.StringLength.triple");
  program.reachableGlobals.push_back("demo.interflow.StringLength.local");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid exact string-length program")) {
    return code;
  }
  if (int code = expect(result.reports.size() == 5 &&
                            result.reports[1].name == "fold-constants" &&
                            result.reports[1].changedValues == 5 &&
                            result.reports[1].validationErrorsBefore == 0 &&
                            result.reports[1].validationErrorsAfter == 0 &&
                            result.reports[2].name == "eliminate-dead-local-lets" &&
                            result.reports[2].changedValues == 1 &&
                            result.reports[2].validationErrorsBefore == 0 &&
                            result.reports[2].validationErrorsAfter == 0,
                        "interflow did not report validation-clean exact "
                        "string-length folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const std::vector<std::pair<std::string, std::string>> expected = {
      {"demo.interflow.StringLength.direct", "ret Int 5"},
      {"demo.interflow.StringLength.empty", "ret Int 0"},
      {"demo.interflow.StringLength.escaped", "ret Int 3"},
      {"demo.interflow.StringLength.triple", "ret Int 6"},
      {"demo.interflow.StringLength.local", "ret Int 5"}};
  for (const auto& [name, expectedReturn] : expected) {
    const scalanative::nir::Definition* definition =
        findDefinition(optimizedModule, name);
    if (int code = expect(definition != nullptr,
                          "interflow removed reachable exact string-length function")) {
      return code;
    }
    const std::vector<std::string> body =
        scalanative::nir::bodyToText(definition->body);
    const std::string actualReturn = body.empty() ? "<empty>" : body.back();
    if (int code = expect(!body.empty() && actualReturn == expectedReturn,
                          "interflow did not fold exact string-length result for " +
                              name + ": expected `" + expectedReturn + "`, got `" +
                              actualReturn + "`")) {
      return code;
    }
    if (name == "demo.interflow.StringLength.local" && body.size() != 2) {
      return expect(false, "interflow did not remove dead string-length local");
    }
  }
  return 0;
}

int smokeInterflowExactRuntimeArrayLengthFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();
  const std::string objectArrayLength =
      std::string(scalanative::support::StdNames::RuntimeReferenceArrayLength) +
      ".Object";
  const auto runtime = [&](std::string_view name) {
    return scalanative::nir::localValue(std::string(name), noSpan);
  };
  const auto lengthCall = [&](std::string_view name, scalanative::nir::Value value) {
    return scalanative::nir::callValue(runtime(name), {std::move(value)}, noSpan);
  };
  const auto arrayValue = [&](std::string_view elementType,
                              std::vector<scalanative::nir::Value> values) {
    return scalanative::nir::newValue("Array [ " + std::string(elementType) + " ]",
                                      std::move(values), noSpan);
  };

  scalanative::nir::FunctionBodyBuilder stringBody;
  (void)stringBody.addLet(
      "values", "Array [ String ]",
      arrayValue("String",
                 {scalanative::nir::literalValue("\"red\"", "String", noSpan),
                  scalanative::nir::literalValue("\"blue\"", "String", noSpan)}),
      noSpan);
  (void)stringBody.addReturn(
      "Int",
      lengthCall(scalanative::support::StdNames::RuntimeArrayLength,
                 scalanative::nir::localValue("values", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder intBody;
  (void)intBody.addLet(
      "values", "Array [ Int ]",
      arrayValue("Int", {scalanative::nir::literalValue("1", "Int", noSpan),
                         scalanative::nir::literalValue("2", "Int", noSpan),
                         scalanative::nir::literalValue("3", "Int", noSpan)}),
      noSpan);
  (void)intBody.addReturn(
      "Int",
      lengthCall(scalanative::support::StdNames::RuntimeIntArrayLength,
                 scalanative::nir::localValue("values", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder aliasBody;
  (void)aliasBody.addLet(
      "values", "Array [ Boolean ]",
      arrayValue("Boolean",
                 {scalanative::nir::literalValue("true", "Boolean", noSpan),
                  scalanative::nir::literalValue("false", "Boolean", noSpan),
                  scalanative::nir::literalValue("true", "Boolean", noSpan)}),
      noSpan);
  (void)aliasBody.addLet("alias", "Array [ Boolean ]",
                         scalanative::nir::localValue("values", noSpan), noSpan);
  (void)aliasBody.addReturn(
      "Int",
      lengthCall(scalanative::support::StdNames::RuntimeBooleanArrayLength,
                 scalanative::nir::localValue("alias", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder objectBody;
  (void)objectBody.addLet(
      "values", "Array [ Object ]",
      arrayValue(
          "Object",
          {scalanative::nir::boxValue(
               "Int", scalanative::nir::literalValue("7", "Int", noSpan), noSpan),
           scalanative::nir::boxValue(
               "String", scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
               noSpan)}),
      noSpan);
  (void)objectBody.addReturn(
      "Int",
      lengthCall(objectArrayLength, scalanative::nir::localValue("values", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder paramBody;
  (void)paramBody.addParameter("values", "Array [ Int ]", noSpan);
  (void)paramBody.addReturn(
      "Int",
      lengthCall(scalanative::support::StdNames::RuntimeIntArrayLength,
                 scalanative::nir::localValue("values", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directBody;
  (void)directBody.addReturn(
      "Int",
      lengthCall(
          scalanative::support::StdNames::RuntimeIntArrayLength,
          arrayValue("Int", {scalanative::nir::literalValue("9", "Int", noSpan)})),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeArrayLength),
       "(Array [ String ])Int",
       {},
       noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeIntArrayLength),
       "(Array [ Int ])Int",
       {},
       noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeBooleanArrayLength),
       "(Array [ Boolean ])Int",
       {},
       noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                objectArrayLength,
                                "(Array [ Object ])Int",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayLength.stringValue", "()Int",
                                std::move(stringBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayLength.intValue", "()Int",
                                std::move(intBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayLength.aliasValue", "()Int",
                                std::move(aliasBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayLength.objectValue", "()Int",
                                std::move(objectBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayLength.paramValue",
                                "(Array [ Int ])Int", std::move(paramBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayLength.directAllocation", "()Int",
                                std::move(directBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back(
      std::string(scalanative::support::StdNames::RuntimeIntArrayLength));
  const std::vector<std::string> roots = {
      "demo.interflow.ArrayLength.stringValue",
      "demo.interflow.ArrayLength.intValue",
      "demo.interflow.ArrayLength.aliasValue",
      "demo.interflow.ArrayLength.objectValue",
      "demo.interflow.ArrayLength.paramValue",
      "demo.interflow.ArrayLength.directAllocation"};
  program.reachableGlobals.insert(program.reachableGlobals.end(), roots.begin(),
                                  roots.end());

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (!result.ok) {
    std::ostringstream errors;
    for (const std::string& error : result.errors) {
      errors << error << '\n';
    }
    return expect(false, "interflow rejected valid exact array-length program: " +
                             errors.str());
  }
  if (int code = expect(result.reports.size() == 5 &&
                            result.reports[0].name == "propagate-local-constants" &&
                            result.reports[0].changedValues == 1 &&
                            result.reports[1].name == "fold-constants" &&
                            result.reports[1].changedValues == 4 &&
                            result.reports[1].validationErrorsBefore == 0 &&
                            result.reports[1].validationErrorsAfter == 0 &&
                            result.reports[2].name == "eliminate-dead-local-lets" &&
                            result.reports[2].changedValues == 1 &&
                            result.reports[2].validationErrorsBefore == 0 &&
                            result.reports[2].validationErrorsAfter == 0,
                        "interflow did not report validation-clean exact runtime "
                        "array-length folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const std::vector<std::pair<std::string, std::string>> expected = {
      {"demo.interflow.ArrayLength.stringValue", "ret Int 2"},
      {"demo.interflow.ArrayLength.intValue", "ret Int 3"},
      {"demo.interflow.ArrayLength.aliasValue", "ret Int 3"},
      {"demo.interflow.ArrayLength.objectValue", "ret Int 2"}};
  for (const auto& [name, expectedReturn] : expected) {
    const scalanative::nir::Definition* definition =
        findDefinition(optimizedModule, name);
    if (int code = expect(definition != nullptr,
                          "interflow removed reachable exact array-length function")) {
      return code;
    }
    const std::vector<std::string> body =
        scalanative::nir::bodyToText(definition->body);
    const std::string actualReturn = body.empty() ? "<empty>" : body.back();
    if (int code = expect(!body.empty() && actualReturn == expectedReturn,
                          "interflow did not fold exact array-length result for " +
                              name + ": expected `" + expectedReturn + "`, got `" +
                              actualReturn + "`")) {
      return code;
    }
    if (int code = expect(body.size() == 3 && contains(body[1], "new Array [ "),
                          "interflow dropped exact array allocation while folding "
                          "array length")) {
      return code;
    }
  }

  const scalanative::nir::Definition* paramValue =
      findDefinition(optimizedModule, "demo.interflow.ArrayLength.paramValue");
  const scalanative::nir::Definition* directAllocation =
      findDefinition(optimizedModule, "demo.interflow.ArrayLength.directAllocation");
  if (int code = expect(paramValue != nullptr && directAllocation != nullptr,
                        "interflow removed reachable runtime array-length guards")) {
    return code;
  }
  const std::vector<std::string> paramBodyText =
      scalanative::nir::bodyToText(paramValue->body);
  if (int code = expect(
          !paramBodyText.empty() &&
              contains(paramBodyText.back(),
                       "call %scala.scalanative.runtime.intArrayLength(%values)"),
          "interflow folded parameter array length too aggressively")) {
    return code;
  }
  const std::vector<std::string> directBodyText =
      scalanative::nir::bodyToText(directAllocation->body);
  return expect(!directBodyText.empty() &&
                    contains(directBodyText.back(),
                             "call %scala.scalanative.runtime.intArrayLength(") &&
                    contains(directBodyText.back(), "new Array [ Int ]"),
                "interflow folded direct array allocation length too aggressively");
}

int smokeInterflowExactRuntimeArrayApplyFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();
  constexpr std::string_view BoxType = "demo.interflow.ArrayApply.Box";
  const std::string objectArrayApply =
      std::string(scalanative::support::StdNames::RuntimeReferenceArrayApply) +
      ".Object";
  const std::string objectArrayUpdate =
      std::string(scalanative::support::StdNames::RuntimeReferenceArrayUpdate) +
      ".Object";
  const std::string boxArrayApply =
      std::string(scalanative::support::StdNames::RuntimeReferenceArrayApply) + "." +
      std::string(BoxType);
  const auto runtime = [&](std::string_view name) {
    return scalanative::nir::localValue(std::string(name), noSpan);
  };
  const auto applyCall = [&](std::string_view name, scalanative::nir::Value array,
                             scalanative::nir::Value index) {
    std::vector<scalanative::nir::Value> arguments;
    arguments.push_back(std::move(array));
    arguments.push_back(std::move(index));
    return scalanative::nir::callValue(runtime(name), std::move(arguments), noSpan);
  };
  const auto lengthCall = [&](std::string_view name, scalanative::nir::Value array) {
    return scalanative::nir::callValue(runtime(name), {std::move(array)}, noSpan);
  };
  const auto updateCallFor = [&](std::string_view name, scalanative::nir::Value array,
                                 scalanative::nir::Value index,
                                 scalanative::nir::Value value) {
    std::vector<scalanative::nir::Value> arguments;
    arguments.push_back(std::move(array));
    arguments.push_back(std::move(index));
    arguments.push_back(std::move(value));
    return scalanative::nir::callValue(runtime(name), std::move(arguments), noSpan);
  };
  const auto updateCall = [&](scalanative::nir::Value array,
                              scalanative::nir::Value index,
                              scalanative::nir::Value value) {
    return updateCallFor(scalanative::support::StdNames::RuntimeIntArrayUpdate,
                         std::move(array), std::move(index), std::move(value));
  };
  const auto arrayValue = [&](std::string_view elementType,
                              std::vector<scalanative::nir::Value> values) {
    return scalanative::nir::newValue("Array [ " + std::string(elementType) + " ]",
                                      std::move(values), noSpan);
  };

  scalanative::nir::FunctionBodyBuilder stringBody;
  (void)stringBody.addLet(
      "values", "Array [ String ]",
      arrayValue("String",
                 {scalanative::nir::literalValue("\"red\"", "String", noSpan),
                  scalanative::nir::literalValue("\"blue\"", "String", noSpan)}),
      noSpan);
  (void)stringBody.addReturn(
      "String",
      applyCall(scalanative::support::StdNames::RuntimeArrayApply,
                scalanative::nir::localValue("values", noSpan),
                scalanative::nir::literalValue("1", "Int", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder intBody;
  (void)intBody.addLet(
      "values", "Array [ Int ]",
      arrayValue("Int", {scalanative::nir::literalValue("1", "Int", noSpan),
                         scalanative::nir::literalValue("2", "Int", noSpan),
                         scalanative::nir::literalValue("3", "Int", noSpan)}),
      noSpan);
  (void)intBody.addReturn(
      "Int",
      applyCall(scalanative::support::StdNames::RuntimeIntArrayApply,
                scalanative::nir::localValue("values", noSpan),
                scalanative::nir::literalValue("2", "Int", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder aliasBody;
  (void)aliasBody.addLet(
      "values", "Array [ Boolean ]",
      arrayValue("Boolean",
                 {scalanative::nir::literalValue("true", "Boolean", noSpan),
                  scalanative::nir::literalValue("false", "Boolean", noSpan)}),
      noSpan);
  (void)aliasBody.addLet("alias", "Array [ Boolean ]",
                         scalanative::nir::localValue("values", noSpan), noSpan);
  (void)aliasBody.addReturn(
      "Boolean",
      applyCall(scalanative::support::StdNames::RuntimeBooleanArrayApply,
                scalanative::nir::localValue("alias", noSpan),
                scalanative::nir::literalValue("0", "Int", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder referenceBody;
  (void)referenceBody.addLet(
      "values", "Array [ Object ]",
      arrayValue(
          "Object",
          {scalanative::nir::boxValue(
               "Int", scalanative::nir::literalValue("7", "Int", noSpan), noSpan),
           scalanative::nir::literalValue("\"Scala\"", "String", noSpan)}),
      noSpan);
  (void)referenceBody.addReturn(
      "Object",
      applyCall(objectArrayApply, scalanative::nir::localValue("values", noSpan),
                scalanative::nir::literalValue("0", "Int", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localIndexBody;
  (void)localIndexBody.addLet(
      "values", "Array [ Char ]",
      arrayValue("Char", {scalanative::nir::literalValue("'a'", "Char", noSpan),
                          scalanative::nir::literalValue("'b'", "Char", noSpan)}),
      noSpan);
  (void)localIndexBody.addLet(
      "index", "Int", scalanative::nir::literalValue("1", "Int", noSpan), noSpan);
  (void)localIndexBody.addReturn(
      "Char",
      applyCall(scalanative::support::StdNames::RuntimeCharArrayApply,
                scalanative::nir::localValue("values", noSpan),
                scalanative::nir::localValue("index", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder updatedBody;
  (void)updatedBody.addLet(
      "values", "Array [ Int ]",
      arrayValue("Int", {scalanative::nir::literalValue("1", "Int", noSpan),
                         scalanative::nir::literalValue("2", "Int", noSpan)}),
      noSpan);
  (void)updatedBody.addEval(
      updateCall(scalanative::nir::localValue("values", noSpan),
                 scalanative::nir::literalValue("0", "Int", noSpan),
                 scalanative::nir::literalValue("9", "Int", noSpan)),
      noSpan);
  (void)updatedBody.addReturn(
      "Int",
      applyCall(scalanative::support::StdNames::RuntimeIntArrayApply,
                scalanative::nir::localValue("values", noSpan),
                scalanative::nir::literalValue("0", "Int", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder aliasUpdatedBody;
  (void)aliasUpdatedBody.addLet(
      "values", "Array [ Int ]",
      arrayValue("Int", {scalanative::nir::literalValue("1", "Int", noSpan),
                         scalanative::nir::literalValue("2", "Int", noSpan)}),
      noSpan);
  (void)aliasUpdatedBody.addLet("alias", "Array [ Int ]",
                                scalanative::nir::localValue("values", noSpan), noSpan);
  (void)aliasUpdatedBody.addEval(
      updateCall(scalanative::nir::localValue("alias", noSpan),
                 scalanative::nir::literalValue("1", "Int", noSpan),
                 scalanative::nir::literalValue("8", "Int", noSpan)),
      noSpan);
  (void)aliasUpdatedBody.addReturn(
      "Int",
      applyCall(scalanative::support::StdNames::RuntimeIntArrayApply,
                scalanative::nir::localValue("values", noSpan),
                scalanative::nir::literalValue("1", "Int", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder referenceUpdatedBody;
  (void)referenceUpdatedBody.addLet(
      "values", "Array [ Object ]",
      arrayValue(
          "Object",
          {scalanative::nir::boxValue(
               "Int", scalanative::nir::literalValue("1", "Int", noSpan), noSpan),
           scalanative::nir::boxValue(
               "Int", scalanative::nir::literalValue("2", "Int", noSpan), noSpan)}),
      noSpan);
  (void)referenceUpdatedBody.addEval(
      updateCallFor(
          objectArrayUpdate, scalanative::nir::localValue("values", noSpan),
          scalanative::nir::literalValue("1", "Int", noSpan),
          scalanative::nir::boxValue(
              "Int", scalanative::nir::literalValue("11", "Int", noSpan), noSpan)),
      noSpan);
  (void)referenceUpdatedBody.addReturn(
      "Object",
      applyCall(objectArrayApply, scalanative::nir::localValue("values", noSpan),
                scalanative::nir::literalValue("1", "Int", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder dynamicUpdateLengthBody;
  (void)dynamicUpdateLengthBody.addParameter("index", "Int", noSpan);
  (void)dynamicUpdateLengthBody.addLet(
      "values", "Array [ Int ]",
      arrayValue("Int", {scalanative::nir::literalValue("1", "Int", noSpan),
                         scalanative::nir::literalValue("2", "Int", noSpan)}),
      noSpan);
  (void)dynamicUpdateLengthBody.addEval(
      updateCall(scalanative::nir::localValue("values", noSpan),
                 scalanative::nir::localValue("index", noSpan),
                 scalanative::nir::literalValue("9", "Int", noSpan)),
      noSpan);
  (void)dynamicUpdateLengthBody.addReturn(
      "Int",
      lengthCall(scalanative::support::StdNames::RuntimeIntArrayLength,
                 scalanative::nir::localValue("values", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder dynamicUpdateApplyBody;
  (void)dynamicUpdateApplyBody.addParameter("index", "Int", noSpan);
  (void)dynamicUpdateApplyBody.addLet(
      "values", "Array [ Int ]",
      arrayValue("Int", {scalanative::nir::literalValue("1", "Int", noSpan),
                         scalanative::nir::literalValue("2", "Int", noSpan)}),
      noSpan);
  (void)dynamicUpdateApplyBody.addEval(
      updateCall(scalanative::nir::localValue("values", noSpan),
                 scalanative::nir::localValue("index", noSpan),
                 scalanative::nir::literalValue("9", "Int", noSpan)),
      noSpan);
  (void)dynamicUpdateApplyBody.addReturn(
      "Int",
      applyCall(scalanative::support::StdNames::RuntimeIntArrayApply,
                scalanative::nir::localValue("values", noSpan),
                scalanative::nir::literalValue("0", "Int", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder paramBody;
  (void)paramBody.addParameter("values", "Array [ Int ]", noSpan);
  (void)paramBody.addReturn(
      "Int",
      applyCall(scalanative::support::StdNames::RuntimeIntArrayApply,
                scalanative::nir::localValue("values", noSpan),
                scalanative::nir::literalValue("0", "Int", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder outOfBoundsBody;
  (void)outOfBoundsBody.addLet(
      "values", "Array [ Int ]",
      arrayValue("Int", {scalanative::nir::literalValue("1", "Int", noSpan)}), noSpan);
  (void)outOfBoundsBody.addReturn(
      "Int",
      applyCall(scalanative::support::StdNames::RuntimeIntArrayApply,
                scalanative::nir::localValue("values", noSpan),
                scalanative::nir::literalValue("3", "Int", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directBody;
  (void)directBody.addReturn(
      "Int",
      applyCall(scalanative::support::StdNames::RuntimeIntArrayApply,
                arrayValue("Int", {scalanative::nir::literalValue("9", "Int", noSpan)}),
                scalanative::nir::literalValue("0", "Int", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder effectfulElementBody;
  (void)effectfulElementBody.addLet(
      "values", "Array [ " + std::string(BoxType) + " ]",
      arrayValue(BoxType, {scalanative::nir::newValue(std::string(BoxType), noSpan)}),
      noSpan);
  (void)effectfulElementBody.addReturn(
      std::string(BoxType),
      applyCall(boxArrayApply, scalanative::nir::localValue("values", noSpan),
                scalanative::nir::literalValue("0", "Int", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                std::string(BoxType),
                                "@java.lang.Object",
                                {},
                                noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeArrayApply),
       "(Array [ String ],Int)String",
       {},
       noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeIntArrayApply),
       "(Array [ Int ],Int)Int",
       {},
       noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeIntArrayLength),
       "(Array [ Int ])Int",
       {},
       noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeBooleanArrayApply),
       "(Array [ Boolean ],Int)Boolean",
       {},
       noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeCharArrayApply),
       "(Array [ Char ],Int)Char",
       {},
       noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                objectArrayApply,
                                "(Array [ Object ],Int)Object",
                                {},
                                noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeIntArrayUpdate),
       "(Array [ Int ],Int,Int)Unit",
       {},
       noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                objectArrayUpdate,
                                "(Array [ Object ],Int,Object)Unit",
                                {},
                                noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       boxArrayApply,
       "(Array [ " + std::string(BoxType) + " ],Int)" + std::string(BoxType),
       {},
       noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayApply.stringValue", "()String",
                                std::move(stringBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayApply.intValue", "()Int",
                                std::move(intBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayApply.aliasValue", "()Boolean",
                                std::move(aliasBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayApply.referenceValue", "()Object",
                                std::move(referenceBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayApply.localIndex", "()Char",
                                std::move(localIndexBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayApply.updatedValue", "()Int",
                                std::move(updatedBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayApply.aliasUpdatedValue", "()Int",
                                std::move(aliasUpdatedBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayApply.referenceUpdatedValue",
                                "()Object", std::move(referenceUpdatedBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayApply.dynamicUpdateLength",
                                "(Int)Int", std::move(dynamicUpdateLengthBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayApply.dynamicUpdateApply",
                                "(Int)Int", std::move(dynamicUpdateApplyBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayApply.paramValue",
                                "(Array [ Int ])Int", std::move(paramBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayApply.outOfBounds", "()Int",
                                std::move(outOfBoundsBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayApply.directAllocation", "()Int",
                                std::move(directBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayApply.effectfulElement",
                                "()" + std::string(BoxType),
                                std::move(effectfulElementBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back(
      std::string(scalanative::support::StdNames::RuntimeIntArrayApply));
  program.reachableGlobals.push_back(
      std::string(scalanative::support::StdNames::RuntimeIntArrayUpdate));
  program.reachableGlobals.push_back(objectArrayApply);
  program.reachableGlobals.push_back(objectArrayUpdate);
  program.reachableGlobals.push_back(boxArrayApply);
  const std::vector<std::string> roots = {
      "demo.interflow.ArrayApply.stringValue",
      "demo.interflow.ArrayApply.intValue",
      "demo.interflow.ArrayApply.aliasValue",
      "demo.interflow.ArrayApply.referenceValue",
      "demo.interflow.ArrayApply.localIndex",
      "demo.interflow.ArrayApply.updatedValue",
      "demo.interflow.ArrayApply.aliasUpdatedValue",
      "demo.interflow.ArrayApply.referenceUpdatedValue",
      "demo.interflow.ArrayApply.dynamicUpdateLength",
      "demo.interflow.ArrayApply.dynamicUpdateApply",
      "demo.interflow.ArrayApply.paramValue",
      "demo.interflow.ArrayApply.outOfBounds",
      "demo.interflow.ArrayApply.directAllocation",
      "demo.interflow.ArrayApply.effectfulElement"};
  program.reachableGlobals.insert(program.reachableGlobals.end(), roots.begin(),
                                  roots.end());

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (!result.ok) {
    std::ostringstream errors;
    for (const std::string& error : result.errors) {
      errors << error << '\n';
    }
    return expect(false, "interflow rejected valid exact array-apply program: " +
                             errors.str());
  }
  if (int code = expect(result.reports.size() == 5 &&
                            result.reports[0].name == "propagate-local-constants" &&
                            result.reports[0].changedValues == 3 &&
                            result.reports[1].name == "fold-constants" &&
                            result.reports[1].changedValues == 9 &&
                            result.reports[1].validationErrorsBefore == 0 &&
                            result.reports[1].validationErrorsAfter == 0 &&
                            result.reports[2].name == "eliminate-dead-local-lets" &&
                            result.reports[2].changedValues == 3 &&
                            result.reports[2].validationErrorsBefore == 0 &&
                            result.reports[2].validationErrorsAfter == 0,
                        "interflow did not report validation-clean exact runtime "
                        "array-apply folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const std::vector<std::pair<std::string, std::string>> expected = {
      {"demo.interflow.ArrayApply.stringValue", "ret String \"blue\""},
      {"demo.interflow.ArrayApply.intValue", "ret Int 3"},
      {"demo.interflow.ArrayApply.aliasValue", "ret Boolean true"},
      {"demo.interflow.ArrayApply.referenceValue", "ret Object box[Int](7)"},
      {"demo.interflow.ArrayApply.localIndex", "ret Char 'b'"},
      {"demo.interflow.ArrayApply.updatedValue", "ret Int 9"},
      {"demo.interflow.ArrayApply.aliasUpdatedValue", "ret Int 8"},
      {"demo.interflow.ArrayApply.referenceUpdatedValue", "ret Object box[Int](11)"},
      {"demo.interflow.ArrayApply.dynamicUpdateLength", "ret Int 2"}};
  for (const auto& [name, expectedReturn] : expected) {
    const scalanative::nir::Definition* definition =
        findDefinition(optimizedModule, name);
    if (int code = expect(definition != nullptr,
                          "interflow removed reachable exact array-apply function")) {
      return code;
    }
    const std::vector<std::string> body =
        scalanative::nir::bodyToText(definition->body);
    const std::string actualReturn = body.empty() ? "<empty>" : body.back();
    if (int code = expect(!body.empty() && actualReturn == expectedReturn,
                          "interflow did not fold exact array-apply result for " +
                              name + ": expected `" + expectedReturn + "`, got `" +
                              actualReturn + "`")) {
      return code;
    }
    const bool keptArrayAllocation =
        std::any_of(body.begin(), body.end(), [](const std::string& line) {
          return contains(line, "new Array [ ");
        });
    if (int code = expect(keptArrayAllocation,
                          "interflow dropped exact array allocation while folding "
                          "array apply")) {
      return code;
    }
    if ((name == "demo.interflow.ArrayApply.updatedValue" ||
         name == "demo.interflow.ArrayApply.aliasUpdatedValue" ||
         name == "demo.interflow.ArrayApply.referenceUpdatedValue") &&
        std::none_of(body.begin(), body.end(), [](const std::string& line) {
          return contains(line, "ArrayUpdate");
        })) {
      return expect(false,
                    "interflow dropped exact array update while folding later apply");
    }
  }

  const std::vector<std::string> guarded = {
      "demo.interflow.ArrayApply.dynamicUpdateApply",
      "demo.interflow.ArrayApply.paramValue", "demo.interflow.ArrayApply.outOfBounds",
      "demo.interflow.ArrayApply.directAllocation",
      "demo.interflow.ArrayApply.effectfulElement"};
  for (const std::string& name : guarded) {
    const scalanative::nir::Definition* definition =
        findDefinition(optimizedModule, name);
    if (int code = expect(definition != nullptr,
                          "interflow removed reachable runtime array-apply guard")) {
      return code;
    }
    const std::vector<std::string> body =
        scalanative::nir::bodyToText(definition->body);
    const std::string text = body.empty() ? "" : body.back();
    const bool keptCall =
        contains(text, "call %scala.scalanative.runtime.intArrayApply") ||
        contains(text, "call %scala.scalanative.runtime.referenceArrayApply");
    if (int code = expect(keptCall,
                          "interflow folded guarded array apply too aggressively for " +
                              name)) {
      return code;
    }
  }
  return 0;
}

int smokeInterflowExactRuntimeToStringFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();
  const auto runtime = [&](std::string_view name) {
    return scalanative::nir::localValue(std::string(name), noSpan);
  };
  const auto toStringCall = [&](std::string_view name, scalanative::nir::Value value) {
    return scalanative::nir::callValue(runtime(name), {std::move(value)}, noSpan);
  };

  scalanative::nir::FunctionBodyBuilder intBody;
  (void)intBody.addReturn(
      "String",
      toStringCall(scalanative::support::StdNames::RuntimeIntToString,
                   scalanative::nir::literalValue("-7", "Int", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder longBody;
  (void)longBody.addReturn(
      "String",
      toStringCall(scalanative::support::StdNames::RuntimeLongToString,
                   scalanative::nir::literalValue("4294967296L", "Long", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder booleanTrueBody;
  (void)booleanTrueBody.addReturn(
      "String",
      toStringCall(scalanative::support::StdNames::RuntimeBooleanToString,
                   scalanative::nir::literalValue("true", "Boolean", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder booleanFalseBody;
  (void)booleanFalseBody.addReturn(
      "String",
      toStringCall(scalanative::support::StdNames::RuntimeBooleanToString,
                   scalanative::nir::literalValue("false", "Boolean", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder charBody;
  (void)charBody.addReturn(
      "String",
      toStringCall(scalanative::support::StdNames::RuntimeCharToString,
                   scalanative::nir::literalValue("'Z'", "Char", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder anyNullBody;
  (void)anyNullBody.addReturn(
      "String",
      toStringCall(scalanative::support::StdNames::RuntimeAnyToString,
                   scalanative::nir::literalValue("null", "Null", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder anyUnitBody;
  (void)anyUnitBody.addReturn(
      "String",
      toStringCall(scalanative::support::StdNames::RuntimeAnyToString,
                   scalanative::nir::boxValue(
                       "Unit", scalanative::nir::unitValue(noSpan), noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder anyBooleanBody;
  (void)anyBooleanBody.addReturn(
      "String",
      toStringCall(scalanative::support::StdNames::RuntimeAnyToString,
                   scalanative::nir::boxValue(
                       "Boolean",
                       scalanative::nir::literalValue("true", "Boolean", noSpan),
                       noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder anyIntBody;
  (void)anyIntBody.addReturn(
      "String",
      toStringCall(
          scalanative::support::StdNames::RuntimeAnyToString,
          scalanative::nir::boxValue(
              "Int", scalanative::nir::literalValue("-7", "Int", noSpan), noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder anyLongBody;
  (void)anyLongBody.addReturn(
      "String",
      toStringCall(scalanative::support::StdNames::RuntimeAnyToString,
                   scalanative::nir::boxValue(
                       "Long",
                       scalanative::nir::literalValue("4294967296L", "Long", noSpan),
                       noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder anyCharBody;
  (void)anyCharBody.addReturn(
      "String",
      toStringCall(
          scalanative::support::StdNames::RuntimeAnyToString,
          scalanative::nir::boxValue(
              "Char", scalanative::nir::literalValue("'Z'", "Char", noSpan), noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder anyStringBody;
  (void)anyStringBody.addReturn(
      "String",
      toStringCall(scalanative::support::StdNames::RuntimeAnyToString,
                   scalanative::nir::boxValue(
                       "String",
                       scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
                       noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder anySymbolBody;
  (void)anySymbolBody.addReturn(
      "String",
      toStringCall(scalanative::support::StdNames::RuntimeAnyToString,
                   scalanative::nir::boxValue(
                       "Symbol",
                       scalanative::nir::literalValue("'ready", "Symbol", noSpan),
                       noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localAnyBooleanBody;
  (void)localAnyBooleanBody.addLet(
      "value", "Object",
      scalanative::nir::boxValue(
          "Boolean", scalanative::nir::literalValue("false", "Boolean", noSpan),
          noSpan),
      noSpan);
  (void)localAnyBooleanBody.addReturn(
      "String",
      toStringCall(scalanative::support::StdNames::RuntimeAnyToString,
                   scalanative::nir::localValue("value", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localAnyLongBody;
  (void)localAnyLongBody.addLet(
      "value", "Object",
      scalanative::nir::boxValue(
          "Long", scalanative::nir::literalValue("-9L", "Long", noSpan), noSpan),
      noSpan);
  (void)localAnyLongBody.addReturn(
      "String",
      toStringCall(scalanative::support::StdNames::RuntimeAnyToString,
                   scalanative::nir::localValue("value", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeIntToString),
       "(Int)String",
       {},
       noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeLongToString),
       "(Long)String",
       {},
       noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeBooleanToString),
       "(Boolean)String",
       {},
       noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeCharToString),
       "(Char)String",
       {},
       noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeAnyToString),
       "(Object)String",
       {},
       noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ToString.intValue", "()String",
                                std::move(intBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ToString.longValue", "()String",
                                std::move(longBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ToString.booleanTrue", "()String",
                                std::move(booleanTrueBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ToString.booleanFalse", "()String",
                                std::move(booleanFalseBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ToString.charValue", "()String",
                                std::move(charBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ToString.anyNull", "()String",
                                std::move(anyNullBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ToString.anyUnit", "()String",
                                std::move(anyUnitBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ToString.anyBoolean", "()String",
                                std::move(anyBooleanBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ToString.anyInt", "()String",
                                std::move(anyIntBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ToString.anyLong", "()String",
                                std::move(anyLongBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ToString.anyChar", "()String",
                                std::move(anyCharBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ToString.anyString", "()String",
                                std::move(anyStringBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ToString.anySymbol", "()String",
                                std::move(anySymbolBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ToString.localAnyBoolean", "()String",
                                std::move(localAnyBooleanBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ToString.localAnyLong", "()String",
                                std::move(localAnyLongBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  const std::vector<std::string> roots = {
      "demo.interflow.ToString.intValue",    "demo.interflow.ToString.longValue",
      "demo.interflow.ToString.booleanTrue", "demo.interflow.ToString.booleanFalse",
      "demo.interflow.ToString.charValue",   "demo.interflow.ToString.anyNull",
      "demo.interflow.ToString.anyUnit",     "demo.interflow.ToString.anyBoolean",
      "demo.interflow.ToString.anyInt",      "demo.interflow.ToString.anyLong",
      "demo.interflow.ToString.anyChar",     "demo.interflow.ToString.anyString",
      "demo.interflow.ToString.anySymbol",   "demo.interflow.ToString.localAnyBoolean",
      "demo.interflow.ToString.localAnyLong"};
  program.reachableGlobals.insert(program.reachableGlobals.end(), roots.begin(),
                                  roots.end());

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code = expect(result.ok,
                        "interflow rejected valid exact runtime toString program")) {
    return code;
  }
  if (int code = expect(result.reports.size() == 5 &&
                            result.reports[1].name == "fold-constants" &&
                            result.reports[1].changedValues == 15 &&
                            result.reports[1].validationErrorsBefore == 0 &&
                            result.reports[1].validationErrorsAfter == 0 &&
                            result.reports[2].name == "eliminate-dead-local-lets" &&
                            result.reports[2].changedValues == 2 &&
                            result.reports[2].validationErrorsBefore == 0 &&
                            result.reports[2].validationErrorsAfter == 0,
                        "interflow did not report validation-clean exact runtime "
                        "toString folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const std::vector<std::pair<std::string, std::string>> expected = {
      {"demo.interflow.ToString.intValue", "ret String \"-7\""},
      {"demo.interflow.ToString.longValue", "ret String \"4294967296\""},
      {"demo.interflow.ToString.booleanTrue", "ret String \"true\""},
      {"demo.interflow.ToString.booleanFalse", "ret String \"false\""},
      {"demo.interflow.ToString.charValue", "ret String \"Z\""},
      {"demo.interflow.ToString.anyNull", "ret String \"null\""},
      {"demo.interflow.ToString.anyUnit", "ret String \"()\""},
      {"demo.interflow.ToString.anyBoolean", "ret String \"true\""},
      {"demo.interflow.ToString.anyInt", "ret String \"-7\""},
      {"demo.interflow.ToString.anyLong", "ret String \"4294967296\""},
      {"demo.interflow.ToString.anyChar", "ret String \"Z\""},
      {"demo.interflow.ToString.anyString", "ret String \"Scala\""},
      {"demo.interflow.ToString.anySymbol", "ret String \"'ready\""},
      {"demo.interflow.ToString.localAnyBoolean", "ret String \"false\""},
      {"demo.interflow.ToString.localAnyLong", "ret String \"-9\""}};
  for (const auto& [name, expectedReturn] : expected) {
    const scalanative::nir::Definition* definition =
        findDefinition(optimizedModule, name);
    if (int code =
            expect(definition != nullptr,
                   "interflow removed reachable exact runtime toString function")) {
      return code;
    }
    const std::vector<std::string> body =
        scalanative::nir::bodyToText(definition->body);
    const std::string actualReturn = body.empty() ? "<empty>" : body.back();
    if (int code = expect(!body.empty() && actualReturn == expectedReturn,
                          "interflow did not fold exact runtime toString result for " +
                              name + ": expected `" + expectedReturn + "`, got `" +
                              actualReturn + "`")) {
      return code;
    }
    if ((name == "demo.interflow.ToString.localAnyBoolean" ||
         name == "demo.interflow.ToString.localAnyLong") &&
        body.size() != 2) {
      return expect(false, "interflow did not remove dead runtime toString local");
    }
  }
  return 0;
}

int smokeInterflowExactRuntimeFormatFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();
  const auto runtime = [&](std::string_view name) {
    return scalanative::nir::localValue(std::string(name), noSpan);
  };
  const auto formatCall = [&](std::string_view name, scalanative::nir::Value format,
                              scalanative::nir::Value value) {
    return scalanative::nir::callValue(runtime(name),
                                       {std::move(format), std::move(value)}, noSpan);
  };

  scalanative::nir::FunctionBodyBuilder stringBody;
  (void)stringBody.addReturn(
      "String",
      formatCall(scalanative::support::StdNames::RuntimeFormat,
                 scalanative::nir::literalValue("\"%s\"", "String", noSpan),
                 scalanative::nir::literalValue("\"Scala Native\"", "String", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localStringBody;
  (void)localStringBody.addLet(
      "format", "String", scalanative::nir::literalValue("\"%s\"", "String", noSpan),
      noSpan);
  (void)localStringBody.addLet(
      "value", "String", scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
      noSpan);
  (void)localStringBody.addReturn(
      "String",
      formatCall(scalanative::support::StdNames::RuntimeFormat,
                 scalanative::nir::localValue("format", noSpan),
                 scalanative::nir::localValue("value", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder charBody;
  (void)charBody.addReturn(
      "String",
      formatCall(scalanative::support::StdNames::RuntimeFormat,
                 scalanative::nir::literalValue("\"%c\"", "String", noSpan),
                 scalanative::nir::literalValue("'N'", "Char", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder booleanTrueBody;
  (void)booleanTrueBody.addReturn(
      "String",
      formatCall(scalanative::support::StdNames::RuntimeFormatBoolean,
                 scalanative::nir::literalValue("\"%s\"", "String", noSpan),
                 scalanative::nir::literalValue("true", "Boolean", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder booleanFalseBody;
  (void)booleanFalseBody.addReturn(
      "String",
      formatCall(scalanative::support::StdNames::RuntimeFormatBoolean,
                 scalanative::nir::literalValue("\"%s\"", "String", noSpan),
                 scalanative::nir::literalValue("false", "Boolean", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localBooleanBody;
  (void)localBooleanBody.addLet(
      "value", "Boolean", scalanative::nir::literalValue("false", "Boolean", noSpan),
      noSpan);
  (void)localBooleanBody.addReturn(
      "String",
      formatCall(scalanative::support::StdNames::RuntimeFormatBoolean,
                 scalanative::nir::literalValue("\"%s\"", "String", noSpan),
                 scalanative::nir::localValue("value", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder paddedIntBody;
  (void)paddedIntBody.addReturn(
      "String",
      formatCall(scalanative::support::StdNames::RuntimeFormat,
                 scalanative::nir::literalValue("\"%04lld\"", "String", noSpan),
                 scalanative::nir::literalValue("7", "Int", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeFormat),
       "(String,Unknown)String",
       {},
       noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeFormatBoolean),
       "(String,Unknown)String",
       {},
       noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Format.stringValue", "()String",
                                std::move(stringBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Format.localString", "()String",
                                std::move(localStringBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Format.charValue", "()String",
                                std::move(charBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Format.booleanTrue", "()String",
                                std::move(booleanTrueBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Format.booleanFalse", "()String",
                                std::move(booleanFalseBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Format.localBoolean", "()String",
                                std::move(localBooleanBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Format.paddedInt", "()String",
                                std::move(paddedIntBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back(
      std::string(scalanative::support::StdNames::RuntimeFormat));
  const std::vector<std::string> roots = {
      "demo.interflow.Format.stringValue",  "demo.interflow.Format.localString",
      "demo.interflow.Format.charValue",    "demo.interflow.Format.booleanTrue",
      "demo.interflow.Format.booleanFalse", "demo.interflow.Format.localBoolean",
      "demo.interflow.Format.paddedInt"};
  program.reachableGlobals.insert(program.reachableGlobals.end(), roots.begin(),
                                  roots.end());

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (!result.ok) {
    std::ostringstream errors;
    for (const std::string& error : result.errors) {
      errors << error << '\n';
    }
    return expect(false, "interflow rejected valid exact runtime format program: " +
                             errors.str());
  }
  if (int code = expect(result.reports.size() == 5 &&
                            result.reports[1].name == "fold-constants" &&
                            result.reports[1].changedValues == 6 &&
                            result.reports[1].validationErrorsBefore == 0 &&
                            result.reports[1].validationErrorsAfter == 0 &&
                            result.reports[2].name == "eliminate-dead-local-lets" &&
                            result.reports[2].changedValues == 3 &&
                            result.reports[2].validationErrorsBefore == 0 &&
                            result.reports[2].validationErrorsAfter == 0,
                        "interflow did not report validation-clean exact runtime "
                        "format folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const std::vector<std::pair<std::string, std::string>> expected = {
      {"demo.interflow.Format.stringValue", "ret String \"Scala Native\""},
      {"demo.interflow.Format.localString", "ret String \"Scala\""},
      {"demo.interflow.Format.charValue", "ret String \"N\""},
      {"demo.interflow.Format.booleanTrue", "ret String \"true\""},
      {"demo.interflow.Format.booleanFalse", "ret String \"false\""},
      {"demo.interflow.Format.localBoolean", "ret String \"false\""}};
  for (const auto& [name, expectedReturn] : expected) {
    const scalanative::nir::Definition* definition =
        findDefinition(optimizedModule, name);
    if (int code =
            expect(definition != nullptr,
                   "interflow removed reachable exact runtime format function")) {
      return code;
    }
    const std::vector<std::string> body =
        scalanative::nir::bodyToText(definition->body);
    const std::string actualReturn = body.empty() ? "<empty>" : body.back();
    if (int code = expect(!body.empty() && actualReturn == expectedReturn,
                          "interflow did not fold exact runtime format result for " +
                              name + ": expected `" + expectedReturn + "`, got `" +
                              actualReturn + "`")) {
      return code;
    }
    if ((name == "demo.interflow.Format.localString" ||
         name == "demo.interflow.Format.localBoolean") &&
        body.size() != 2) {
      return expect(false, "interflow did not remove dead runtime format locals");
    }
  }

  const scalanative::nir::Definition* paddedInt =
      findDefinition(optimizedModule, "demo.interflow.Format.paddedInt");
  if (int code = expect(paddedInt != nullptr,
                        "interflow removed reachable runtime padded format function")) {
    return code;
  }
  const std::vector<std::string> paddedBody =
      scalanative::nir::bodyToText(paddedInt->body);
  return expect(
      !paddedBody.empty() &&
          contains(paddedBody.back(), "call %scala.scalanative.runtime.format"),
      "interflow folded padded numeric runtime format too aggressively");
}

int smokeInterflowExactNullComparisonFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  scalanative::nir::FunctionBodyBuilder nullEqualBody;
  (void)nullEqualBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "==", scalanative::nir::literalValue("null", "Null", noSpan),
          scalanative::nir::literalValue("null", "Null", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder boxedNotNullBody;
  (void)boxedNotNullBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "!=",
          scalanative::nir::boxValue(
              "Int", scalanative::nir::literalValue("7", "Int", noSpan), noSpan),
          scalanative::nir::literalValue("null", "Null", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder stringNullBody;
  (void)stringNullBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "==", scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
          scalanative::nir::literalValue("null", "Null", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localBody;
  (void)localBody.addLet("missing", "Object",
                         scalanative::nir::literalValue("null", "Null", noSpan),
                         noSpan);
  (void)localBody.addLet(
      "value", "Object",
      scalanative::nir::boxValue(
          "String", scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
          noSpan),
      noSpan);
  (void)localBody.addReturn("Boolean",
                            scalanative::nir::binaryValue(
                                "!=", scalanative::nir::localValue("missing", noSpan),
                                scalanative::nir::localValue("value", noSpan), noSpan),
                            noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.NullComparison.nullEqual", "()Boolean",
                                std::move(nullEqualBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.NullComparison.boxedNotNull",
                                "()Boolean", std::move(boxedNotNullBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.NullComparison.stringNull", "()Boolean",
                                std::move(stringNullBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.NullComparison.local", "()Boolean",
                                std::move(localBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.NullComparison.nullEqual");
  program.reachableGlobals.push_back("demo.interflow.NullComparison.boxedNotNull");
  program.reachableGlobals.push_back("demo.interflow.NullComparison.stringNull");
  program.reachableGlobals.push_back("demo.interflow.NullComparison.local");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid exact null comparison program")) {
    return code;
  }
  if (int code =
          expect(result.reports.size() == 5 &&
                     result.reports[0].name == "propagate-local-constants" &&
                     result.reports[0].changedValues == 1 &&
                     result.reports[0].validationErrorsBefore == 0 &&
                     result.reports[0].validationErrorsAfter == 0 &&
                     result.reports[1].name == "fold-constants" &&
                     result.reports[1].changedValues == 4 &&
                     result.reports[1].validationErrorsBefore == 0 &&
                     result.reports[1].validationErrorsAfter == 0 &&
                     result.reports[2].name == "eliminate-dead-local-lets" &&
                     result.reports[2].changedValues == 2 &&
                     result.reports[2].validationErrorsBefore == 0 &&
                     result.reports[2].validationErrorsAfter == 0,
                 "interflow did not report validation-clean exact null comparison "
                 "folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* nullEqual =
      findDefinition(optimizedModule, "demo.interflow.NullComparison.nullEqual");
  const scalanative::nir::Definition* boxedNotNull =
      findDefinition(optimizedModule, "demo.interflow.NullComparison.boxedNotNull");
  const scalanative::nir::Definition* stringNull =
      findDefinition(optimizedModule, "demo.interflow.NullComparison.stringNull");
  const scalanative::nir::Definition* local =
      findDefinition(optimizedModule, "demo.interflow.NullComparison.local");
  if (int code = expect(nullEqual != nullptr && boxedNotNull != nullptr &&
                            stringNull != nullptr && local != nullptr,
                        "interflow removed reachable exact null comparison "
                        "functions")) {
    return code;
  }
  if (int code = expect(
          scalanative::nir::bodyToText(nullEqual->body).back() == "ret Boolean true" &&
              scalanative::nir::bodyToText(boxedNotNull->body).back() ==
                  "ret Boolean true" &&
              scalanative::nir::bodyToText(stringNull->body).back() ==
                  "ret Boolean false" &&
              scalanative::nir::bodyToText(local->body).back() == "ret Boolean true",
          "interflow did not fold exact null comparison results")) {
    return code;
  }
  return expect(scalanative::nir::bodyToText(local->body).size() == 2,
                "interflow did not remove dead exact null comparison locals");
}

int smokeInterflowSameTypeCastFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();
  constexpr std::string_view BoxType = "demo.interflow.Cast.Box";

  scalanative::nir::FunctionBodyBuilder userMainBody;
  (void)userMainBody.addReturn(
      std::string(BoxType),
      scalanative::nir::asInstanceOfValue(
          std::string(BoxType),
          scalanative::nir::asInstanceOfValue(
              std::string(BoxType),
              scalanative::nir::newValue(std::string(BoxType), noSpan), noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder paramBody;
  (void)paramBody.addParameter("box", std::string(BoxType), noSpan);
  (void)paramBody.addReturn(
      std::string(BoxType),
      scalanative::nir::asInstanceOfValue(
          std::string(BoxType), scalanative::nir::localValue("box", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder nullBody;
  (void)nullBody.addReturn(std::string(BoxType),
                           scalanative::nir::asInstanceOfValue(
                               std::string(BoxType),
                               scalanative::nir::literalValue("null", "Null", noSpan),
                               noSpan),
                           noSpan);

  scalanative::nir::FunctionBodyBuilder boxedObjectBody;
  (void)boxedObjectBody.addReturn(
      "Object",
      scalanative::nir::asInstanceOfValue(
          "Object",
          scalanative::nir::boxValue(
              "Int", scalanative::nir::literalValue("7", "Int", noSpan), noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder stringObjectBody;
  (void)stringObjectBody.addReturn(
      "Object",
      scalanative::nir::asInstanceOfValue(
          "Object", scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localStringObjectBody;
  (void)localStringObjectBody.addLet(
      "direct", "String", scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
      noSpan);
  (void)localStringObjectBody.addReturn(
      "Object",
      scalanative::nir::asInstanceOfValue(
          "Object", scalanative::nir::localValue("direct", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int",
      scalanative::nir::literalValue("0", "Int",
                                     scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                "Object",
                                "@java.lang.Object",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                std::string(BoxType),
                                "@java.lang.Object",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Cast.main", "()" + std::string(BoxType),
                                std::move(userMainBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Cast.param",
                                "(" + std::string(BoxType) + ")" + std::string(BoxType),
                                std::move(paramBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef, "demo.interflow.Cast.nullValue",
       "()" + std::string(BoxType), std::move(nullBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Cast.boxedObject", "()Object",
                                std::move(boxedObjectBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Cast.stringObject", "()Object",
                                std::move(stringObjectBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Cast.localStringObject", "()Object",
                                std::move(localStringObjectBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.Cast.main");
  program.reachableGlobals.push_back("demo.interflow.Cast.param");
  program.reachableGlobals.push_back("demo.interflow.Cast.nullValue");
  program.reachableGlobals.push_back("demo.interflow.Cast.boxedObject");
  program.reachableGlobals.push_back("demo.interflow.Cast.stringObject");
  program.reachableGlobals.push_back("demo.interflow.Cast.localStringObject");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code = expect(result.ok, "interflow rejected valid same-type cast program")) {
    return code;
  }
  if (int code = expect(
          result.reports.size() == 5 && result.reports[1].name == "fold-constants" &&
              result.reports[1].changedValues == 6 &&
              result.reports[1].validationErrorsBefore == 0 &&
              result.reports[1].validationErrorsAfter == 0 &&
              result.reports[2].name == "eliminate-dead-local-lets" &&
              result.reports[2].changedValues == 1 &&
              result.reports[2].validationErrorsBefore == 0 &&
              result.reports[2].validationErrorsAfter == 0,
          "interflow did not report validation-clean same-type cast folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* foldedMain =
      findDefinition(optimizedModule, "demo.interflow.Cast.main");
  const scalanative::nir::Definition* foldedParam =
      findDefinition(optimizedModule, "demo.interflow.Cast.param");
  const scalanative::nir::Definition* foldedNull =
      findDefinition(optimizedModule, "demo.interflow.Cast.nullValue");
  const scalanative::nir::Definition* boxedObject =
      findDefinition(optimizedModule, "demo.interflow.Cast.boxedObject");
  const scalanative::nir::Definition* stringObject =
      findDefinition(optimizedModule, "demo.interflow.Cast.stringObject");
  const scalanative::nir::Definition* localStringObject =
      findDefinition(optimizedModule, "demo.interflow.Cast.localStringObject");
  if (int code = expect(foldedMain != nullptr && foldedParam != nullptr &&
                            foldedNull != nullptr && boxedObject != nullptr &&
                            stringObject != nullptr && localStringObject != nullptr,
                        "interflow removed reachable same-type cast functions")) {
    return code;
  }
  const std::string body = scalanative::nir::bodyToText(foldedMain->body).back();
  if (int code =
          expect(body == "ret demo.interflow.Cast.Box new demo.interflow.Cast.Box" &&
                     !contains(body, "as-instance-of"),
                 "interflow kept a redundant same-type cast in optimized NIR")) {
    return code;
  }

  const std::vector<std::string> paramBodyText =
      scalanative::nir::bodyToText(foldedParam->body);
  if (int code =
          expect(paramBodyText.size() == 3 &&
                     paramBodyText[1] == "param %box : demo.interflow.Cast.Box" &&
                     paramBodyText[2] == "ret demo.interflow.Cast.Box %box",
                 "interflow did not fold same-type local parameter cast")) {
    return code;
  }

  const std::string nullBodyText =
      scalanative::nir::bodyToText(foldedNull->body).back();
  if (int code = expect(nullBodyText == "ret demo.interflow.Cast.Box null" &&
                            !contains(nullBodyText, "as-instance-of"),
                        "interflow did not fold null reference cast")) {
    return code;
  }

  if (int code = expect(scalanative::nir::bodyToText(boxedObject->body).back() ==
                            "ret Object box[Int](7)",
                        "interflow did not fold boxed upcast to Object")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(stringObject->body).back() ==
                            "ret Object \"Scala\"",
                        "interflow did not fold string upcast to Object")) {
    return code;
  }
  const std::vector<std::string> localStringObjectText =
      scalanative::nir::bodyToText(localStringObject->body);
  return expect(localStringObjectText.size() == 2 &&
                    localStringObjectText.back() == "ret Object \"Scala\"",
                "interflow did not fold local string upcast to Object");
}

int smokeInterflowNullTypeTestFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();
  constexpr std::string_view TargetType = "demo.interflow.TypeTest.Target";

  scalanative::nir::FunctionBodyBuilder directBody;
  (void)directBody.addReturn("Boolean",
                             scalanative::nir::isInstanceOfValue(
                                 std::string(TargetType),
                                 scalanative::nir::literalValue("null", "Null", noSpan),
                                 noSpan),
                             noSpan);

  scalanative::nir::FunctionBodyBuilder aliasBody;
  (void)aliasBody.addLet("missing", "Object",
                         scalanative::nir::literalValue("null", "Null", noSpan),
                         noSpan);
  (void)aliasBody.addReturn("Boolean",
                            scalanative::nir::isInstanceOfValue(
                                std::string(TargetType),
                                scalanative::nir::localValue("missing", noSpan),
                                noSpan),
                            noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                std::string(TargetType),
                                "@java.lang.Object",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.TypeTest.direct", "()Boolean",
                                std::move(directBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.TypeTest.alias", "()Boolean",
                                std::move(aliasBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.TypeTest.direct");
  program.reachableGlobals.push_back("demo.interflow.TypeTest.alias");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code = expect(result.ok, "interflow rejected valid null type-test program")) {
    return code;
  }
  if (int code =
          expect(result.reports.size() == 5 &&
                     result.reports[0].name == "propagate-local-constants" &&
                     result.reports[0].changedValues == 1 &&
                     result.reports[1].name == "fold-constants" &&
                     result.reports[1].changedValues == 2 &&
                     result.reports[1].validationErrorsBefore == 0 &&
                     result.reports[1].validationErrorsAfter == 0 &&
                     result.reports[2].name == "eliminate-dead-local-lets" &&
                     result.reports[2].changedValues == 1,
                 "interflow did not report validation-clean null type-test folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* direct =
      findDefinition(optimizedModule, "demo.interflow.TypeTest.direct");
  const scalanative::nir::Definition* alias =
      findDefinition(optimizedModule, "demo.interflow.TypeTest.alias");
  if (int code = expect(direct != nullptr && alias != nullptr,
                        "interflow removed reachable null type-test functions")) {
    return code;
  }

  if (int code = expect(scalanative::nir::bodyToText(direct->body).back() ==
                            "ret Boolean false",
                        "interflow did not fold direct null type-test")) {
    return code;
  }
  return expect(scalanative::nir::bodyToText(alias->body).back() == "ret Boolean false",
                "interflow did not fold propagated null type-test");
}

int smokeInterflowNullDerivedAliasPropagation() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();
  constexpr std::string_view BoxType = "demo.interflow.NullAlias.Box";

  scalanative::nir::FunctionBodyBuilder castBody;
  (void)castBody.addLet("missing", std::string(BoxType),
                        scalanative::nir::asInstanceOfValue(
                            std::string(BoxType),
                            scalanative::nir::literalValue("null", "Null", noSpan),
                            noSpan),
                        noSpan);
  (void)castBody.addReturn(std::string(BoxType),
                           scalanative::nir::localValue("missing", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder testBody;
  (void)testBody.addLet("matches", "Boolean",
                        scalanative::nir::isInstanceOfValue(
                            std::string(BoxType),
                            scalanative::nir::literalValue("null", "Null", noSpan),
                            noSpan),
                        noSpan);
  (void)testBody.addReturn("Boolean", scalanative::nir::localValue("matches", noSpan),
                           noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                std::string(BoxType),
                                "@java.lang.Object",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.NullAlias.castValue",
                                "()" + std::string(BoxType),
                                std::move(castBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.NullAlias.typeTest", "()Boolean",
                                std::move(testBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.NullAlias.castValue");
  program.reachableGlobals.push_back("demo.interflow.NullAlias.typeTest");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid null-derived alias program")) {
    return code;
  }
  if (int code = expect(result.reports.size() == 5 &&
                            result.reports[0].name == "propagate-local-constants" &&
                            result.reports[0].changedValues == 2 &&
                            result.reports[0].validationErrorsBefore == 0 &&
                            result.reports[0].validationErrorsAfter == 0 &&
                            result.reports[1].name == "fold-constants" &&
                            result.reports[1].changedValues == 4 &&
                            result.reports[1].validationErrorsBefore == 0 &&
                            result.reports[1].validationErrorsAfter == 0 &&
                            result.reports[2].name == "eliminate-dead-local-lets" &&
                            result.reports[2].changedValues == 2,
                        "interflow did not report validation-clean null-derived alias "
                        "propagation")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* castValue =
      findDefinition(optimizedModule, "demo.interflow.NullAlias.castValue");
  const scalanative::nir::Definition* typeTest =
      findDefinition(optimizedModule, "demo.interflow.NullAlias.typeTest");
  if (int code = expect(castValue != nullptr && typeTest != nullptr,
                        "interflow removed reachable null-derived alias functions")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(castValue->body).back() ==
                            "ret demo.interflow.NullAlias.Box null",
                        "interflow did not propagate and fold null cast alias")) {
    return code;
  }
  return expect(scalanative::nir::bodyToText(typeTest->body).back() ==
                    "ret Boolean false",
                "interflow did not propagate and fold null type-test alias");
}

int smokeInterflowExactNonNullTypeTestFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  scalanative::nir::FunctionBodyBuilder intBody;
  (void)intBody.addReturn(
      "Boolean",
      scalanative::nir::isInstanceOfValue(
          "Int",
          scalanative::nir::boxValue(
              "Int", scalanative::nir::literalValue("1", "Int", noSpan), noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder unitBody;
  (void)unitBody.addReturn("Boolean",
                           scalanative::nir::isInstanceOfValue(
                               "Unit",
                               scalanative::nir::boxValue(
                                   "Unit", scalanative::nir::unitValue(noSpan), noSpan),
                               noSpan),
                           noSpan);

  scalanative::nir::FunctionBodyBuilder symbolBody;
  (void)symbolBody.addReturn(
      "Boolean",
      scalanative::nir::isInstanceOfValue(
          "Symbol",
          scalanative::nir::boxValue(
              "Symbol", scalanative::nir::literalValue("'ready", "Symbol", noSpan),
              noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder stringBody;
  (void)stringBody.addReturn(
      "Boolean",
      scalanative::nir::isInstanceOfValue(
          "String", scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder stringMismatchBody;
  (void)stringMismatchBody.addReturn(
      "Boolean",
      scalanative::nir::isInstanceOfValue(
          "Symbol", scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder boxedObjectBody;
  (void)boxedObjectBody.addReturn(
      "Boolean",
      scalanative::nir::isInstanceOfValue(
          "Object",
          scalanative::nir::boxValue(
              "Int", scalanative::nir::literalValue("1", "Int", noSpan), noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder stringObjectBody;
  (void)stringObjectBody.addReturn(
      "Boolean",
      scalanative::nir::isInstanceOfValue(
          "Object", scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localStringBody;
  (void)localStringBody.addLet(
      "direct", "Object",
      scalanative::nir::boxValue(
          "String", scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
          noSpan),
      noSpan);
  (void)localStringBody.addReturn(
      "Boolean",
      scalanative::nir::isInstanceOfValue(
          "String", scalanative::nir::localValue("direct", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder intMismatchBody;
  (void)intMismatchBody.addReturn(
      "Boolean",
      scalanative::nir::isInstanceOfValue(
          "Long",
          scalanative::nir::boxValue(
              "Int", scalanative::nir::literalValue("1", "Int", noSpan), noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localStringMismatchBody;
  (void)localStringMismatchBody.addLet(
      "direct", "Object",
      scalanative::nir::boxValue(
          "String", scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
          noSpan),
      noSpan);
  (void)localStringMismatchBody.addReturn(
      "Boolean",
      scalanative::nir::isInstanceOfValue(
          "Symbol", scalanative::nir::localValue("direct", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localStringObjectBody;
  (void)localStringObjectBody.addLet(
      "direct", "String", scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
      noSpan);
  (void)localStringObjectBody.addReturn(
      "Boolean",
      scalanative::nir::isInstanceOfValue(
          "Object", scalanative::nir::localValue("direct", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localBareStringMismatchBody;
  (void)localBareStringMismatchBody.addLet(
      "direct", "String", scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
      noSpan);
  (void)localBareStringMismatchBody.addReturn(
      "Boolean",
      scalanative::nir::isInstanceOfValue(
          "Symbol", scalanative::nir::localValue("direct", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localFoldedResultBody;
  (void)localFoldedResultBody.addLet(
      "matches", "Boolean",
      scalanative::nir::isInstanceOfValue(
          "String",
          scalanative::nir::boxValue(
              "String", scalanative::nir::literalValue("\"Scala\"", "String", noSpan),
              noSpan),
          noSpan),
      noSpan);
  (void)localFoldedResultBody.addReturn(
      "Boolean", scalanative::nir::localValue("matches", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                "Object",
                                "@java.lang.Object",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ExactTypeTest.boxedInt", "()Boolean",
                                std::move(intBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ExactTypeTest.boxedUnit", "()Boolean",
                                std::move(unitBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ExactTypeTest.boxedSymbol", "()Boolean",
                                std::move(symbolBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ExactTypeTest.stringLiteral",
                                "()Boolean", std::move(stringBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ExactTypeTest.stringMismatch",
                                "()Boolean", std::move(stringMismatchBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ExactTypeTest.boxedObject", "()Boolean",
                                std::move(boxedObjectBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ExactTypeTest.stringObject",
                                "()Boolean", std::move(stringObjectBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ExactTypeTest.localString", "()Boolean",
                                std::move(localStringBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ExactTypeTest.intMismatch", "()Boolean",
                                std::move(intMismatchBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ExactTypeTest.localStringMismatch",
                                "()Boolean", std::move(localStringMismatchBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ExactTypeTest.localStringObject",
                                "()Boolean", std::move(localStringObjectBody).build(),
                                noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.ExactTypeTest.localBareStringMismatch", "()Boolean",
       std::move(localBareStringMismatchBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ExactTypeTest.localFoldedResult",
                                "()Boolean", std::move(localFoldedResultBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.ExactTypeTest.boxedInt");
  program.reachableGlobals.push_back("demo.interflow.ExactTypeTest.boxedUnit");
  program.reachableGlobals.push_back("demo.interflow.ExactTypeTest.boxedSymbol");
  program.reachableGlobals.push_back("demo.interflow.ExactTypeTest.stringLiteral");
  program.reachableGlobals.push_back("demo.interflow.ExactTypeTest.stringMismatch");
  program.reachableGlobals.push_back("demo.interflow.ExactTypeTest.boxedObject");
  program.reachableGlobals.push_back("demo.interflow.ExactTypeTest.stringObject");
  program.reachableGlobals.push_back("demo.interflow.ExactTypeTest.localString");
  program.reachableGlobals.push_back("demo.interflow.ExactTypeTest.intMismatch");
  program.reachableGlobals.push_back(
      "demo.interflow.ExactTypeTest.localStringMismatch");
  program.reachableGlobals.push_back("demo.interflow.ExactTypeTest.localStringObject");
  program.reachableGlobals.push_back(
      "demo.interflow.ExactTypeTest.localBareStringMismatch");
  program.reachableGlobals.push_back("demo.interflow.ExactTypeTest.localFoldedResult");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid exact type-test program")) {
    return code;
  }
  if (int code = expect(
          result.reports.size() == 5 && result.reports[1].name == "fold-constants" &&
              result.reports[1].changedValues == 14 &&
              result.reports[1].validationErrorsBefore == 0 &&
              result.reports[1].validationErrorsAfter == 0 &&
              result.reports[2].name == "eliminate-dead-local-lets" &&
              result.reports[2].changedValues == 5 &&
              result.reports[2].validationErrorsBefore == 0 &&
              result.reports[2].validationErrorsAfter == 0,
          "interflow did not report validation-clean exact type-test folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* boxedInt =
      findDefinition(optimizedModule, "demo.interflow.ExactTypeTest.boxedInt");
  const scalanative::nir::Definition* boxedUnit =
      findDefinition(optimizedModule, "demo.interflow.ExactTypeTest.boxedUnit");
  const scalanative::nir::Definition* boxedSymbol =
      findDefinition(optimizedModule, "demo.interflow.ExactTypeTest.boxedSymbol");
  const scalanative::nir::Definition* stringLiteral =
      findDefinition(optimizedModule, "demo.interflow.ExactTypeTest.stringLiteral");
  const scalanative::nir::Definition* stringMismatch =
      findDefinition(optimizedModule, "demo.interflow.ExactTypeTest.stringMismatch");
  const scalanative::nir::Definition* boxedObject =
      findDefinition(optimizedModule, "demo.interflow.ExactTypeTest.boxedObject");
  const scalanative::nir::Definition* stringObject =
      findDefinition(optimizedModule, "demo.interflow.ExactTypeTest.stringObject");
  const scalanative::nir::Definition* localString =
      findDefinition(optimizedModule, "demo.interflow.ExactTypeTest.localString");
  const scalanative::nir::Definition* intMismatch =
      findDefinition(optimizedModule, "demo.interflow.ExactTypeTest.intMismatch");
  const scalanative::nir::Definition* localStringMismatch = findDefinition(
      optimizedModule, "demo.interflow.ExactTypeTest.localStringMismatch");
  const scalanative::nir::Definition* localStringObject =
      findDefinition(optimizedModule, "demo.interflow.ExactTypeTest.localStringObject");
  const scalanative::nir::Definition* localBareStringMismatch = findDefinition(
      optimizedModule, "demo.interflow.ExactTypeTest.localBareStringMismatch");
  const scalanative::nir::Definition* localFoldedResult =
      findDefinition(optimizedModule, "demo.interflow.ExactTypeTest.localFoldedResult");
  if (int code = expect(
          boxedInt != nullptr && boxedUnit != nullptr && boxedSymbol != nullptr &&
              stringLiteral != nullptr && stringMismatch != nullptr &&
              boxedObject != nullptr && stringObject != nullptr &&
              localString != nullptr && intMismatch != nullptr &&
              localStringMismatch != nullptr && localStringObject != nullptr &&
              localBareStringMismatch != nullptr && localFoldedResult != nullptr,
          "interflow removed reachable exact type-test functions")) {
    return code;
  }
  if (int code = expect(
          scalanative::nir::bodyToText(boxedInt->body).back() == "ret Boolean true" &&
              scalanative::nir::bodyToText(boxedUnit->body).back() ==
                  "ret Boolean true" &&
              scalanative::nir::bodyToText(boxedSymbol->body).back() ==
                  "ret Boolean true" &&
              scalanative::nir::bodyToText(stringLiteral->body).back() ==
                  "ret Boolean true" &&
              scalanative::nir::bodyToText(stringMismatch->body).back() ==
                  "ret Boolean false" &&
              scalanative::nir::bodyToText(boxedObject->body).back() ==
                  "ret Boolean true" &&
              scalanative::nir::bodyToText(stringObject->body).back() ==
                  "ret Boolean true" &&
              scalanative::nir::bodyToText(localString->body).back() ==
                  "ret Boolean true" &&
              scalanative::nir::bodyToText(intMismatch->body).back() ==
                  "ret Boolean false" &&
              scalanative::nir::bodyToText(localStringMismatch->body).back() ==
                  "ret Boolean false" &&
              scalanative::nir::bodyToText(localStringObject->body).back() ==
                  "ret Boolean true" &&
              scalanative::nir::bodyToText(localBareStringMismatch->body).back() ==
                  "ret Boolean false" &&
              scalanative::nir::bodyToText(localFoldedResult->body).back() ==
                  "ret Boolean true",
          "interflow did not fold exact non-null type tests")) {
    return code;
  }
  const std::vector<std::string> localStringText =
      scalanative::nir::bodyToText(localString->body);
  const std::vector<std::string> localStringMismatchText =
      scalanative::nir::bodyToText(localStringMismatch->body);
  const std::vector<std::string> localStringObjectText =
      scalanative::nir::bodyToText(localStringObject->body);
  const std::vector<std::string> localBareStringMismatchText =
      scalanative::nir::bodyToText(localBareStringMismatch->body);
  const std::vector<std::string> localFoldedResultText =
      scalanative::nir::bodyToText(localFoldedResult->body);
  return expect(localStringText.size() == 2 &&
                    localStringText.back() == "ret Boolean true" &&
                    localStringMismatchText.size() == 2 &&
                    localStringMismatchText.back() == "ret Boolean false" &&
                    localStringObjectText.size() == 2 &&
                    localStringObjectText.back() == "ret Boolean true" &&
                    localBareStringMismatchText.size() == 2 &&
                    localBareStringMismatchText.back() == "ret Boolean false" &&
                    localFoldedResultText.size() == 2 &&
                    localFoldedResultText.back() == "ret Boolean true",
                "interflow did not remove dead exact type-test boxes");
}

int smokeInterflowAlgebraicIdentityFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  scalanative::nir::FunctionBodyBuilder intBody;
  (void)intBody.addParameter("value", "Int", noSpan);
  (void)intBody.addReturn(
      "Int",
      scalanative::nir::binaryValue(
          "-",
          scalanative::nir::binaryValue(
              "*",
              scalanative::nir::binaryValue(
                  "+", scalanative::nir::localValue("value", noSpan),
                  scalanative::nir::literalValue("0", "Int", noSpan), noSpan),
              scalanative::nir::literalValue("1", "Int", noSpan), noSpan),
          scalanative::nir::literalValue("0", "Int", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder booleanBody;
  (void)booleanBody.addParameter("flag", "Boolean", noSpan);
  (void)booleanBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "||",
          scalanative::nir::binaryValue(
              "&&", scalanative::nir::localValue("flag", noSpan),
              scalanative::nir::literalValue("true", "Boolean", noSpan), noSpan),
          scalanative::nir::literalValue("false", "Boolean", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder floatBody;
  (void)floatBody.addParameter("value", "Float", noSpan);
  (void)floatBody.addReturn("Float",
                            scalanative::nir::binaryValue(
                                "*", scalanative::nir::localValue("value", noSpan),
                                scalanative::nir::literalValue("1.0F", "Float", noSpan),
                                noSpan),
                            noSpan);

  scalanative::nir::FunctionBodyBuilder doubleBody;
  (void)doubleBody.addParameter("value", "Double", noSpan);
  (void)doubleBody.addReturn(
      "Double",
      scalanative::nir::binaryValue(
          "/",
          scalanative::nir::binaryValue(
              "*", scalanative::nir::literalValue("1.0", "Double", noSpan),
              scalanative::nir::localValue("value", noSpan), noSpan),
          scalanative::nir::literalValue("1.0", "Double", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Identity.intValue", "(Int)Int",
                                std::move(intBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Identity.booleanValue",
                                "(Boolean)Boolean", std::move(booleanBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Identity.floatValue", "(Float)Float",
                                std::move(floatBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Identity.doubleValue", "(Double)Double",
                                std::move(doubleBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.Identity.intValue");
  program.reachableGlobals.push_back("demo.interflow.Identity.booleanValue");
  program.reachableGlobals.push_back("demo.interflow.Identity.floatValue");
  program.reachableGlobals.push_back("demo.interflow.Identity.doubleValue");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid algebraic identity program")) {
    return code;
  }
  if (int code = expect(
          result.reports.size() == 5 && result.reports[1].name == "fold-constants" &&
              result.reports[1].changedValues == 4 &&
              result.reports[1].validationErrorsBefore == 0 &&
              result.reports[1].validationErrorsAfter == 0,
          "interflow did not report validation-clean algebraic identity folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* foldedInt =
      findDefinition(optimizedModule, "demo.interflow.Identity.intValue");
  const scalanative::nir::Definition* foldedBoolean =
      findDefinition(optimizedModule, "demo.interflow.Identity.booleanValue");
  const scalanative::nir::Definition* foldedFloat =
      findDefinition(optimizedModule, "demo.interflow.Identity.floatValue");
  const scalanative::nir::Definition* foldedDouble =
      findDefinition(optimizedModule, "demo.interflow.Identity.doubleValue");
  if (int code = expect(foldedInt != nullptr && foldedBoolean != nullptr &&
                            foldedFloat != nullptr && foldedDouble != nullptr,
                        "interflow removed reachable algebraic identity functions")) {
    return code;
  }

  const std::vector<std::string> intText =
      scalanative::nir::bodyToText(foldedInt->body);
  if (int code = expect(intText.size() == 3 && intText[1] == "param %value : Int" &&
                            intText[2] == "ret Int %value",
                        "interflow did not collapse Int identity arithmetic")) {
    return code;
  }

  const std::vector<std::string> booleanText =
      scalanative::nir::bodyToText(foldedBoolean->body);
  if (int code =
          expect(booleanText.size() == 3 && booleanText[1] == "param %flag : Boolean" &&
                     booleanText[2] == "ret Boolean %flag",
                 "interflow did not collapse Boolean logical identities")) {
    return code;
  }

  const std::vector<std::string> floatText =
      scalanative::nir::bodyToText(foldedFloat->body);
  if (int code =
          expect(floatText.size() == 3 && floatText[1] == "param %value : Float" &&
                     floatText[2] == "ret Float %value",
                 "interflow did not collapse Float multiplicative identity")) {
    return code;
  }

  const std::vector<std::string> doubleText =
      scalanative::nir::bodyToText(foldedDouble->body);
  return expect(doubleText.size() == 3 && doubleText[1] == "param %value : Double" &&
                    doubleText[2] == "ret Double %value",
                "interflow did not collapse Double multiplicative identities");
}

int smokeInterflowIntegerNegationIdentityFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  scalanative::nir::FunctionBodyBuilder zeroMinusIntBody;
  (void)zeroMinusIntBody.addParameter("value", "Int", noSpan);
  (void)zeroMinusIntBody.addReturn(
      "Int",
      scalanative::nir::binaryValue(
          "-", scalanative::nir::literalValue("0", "Int", noSpan),
          scalanative::nir::localValue("value", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder intTimesMinusOneBody;
  (void)intTimesMinusOneBody.addParameter("value", "Int", noSpan);
  (void)intTimesMinusOneBody.addReturn(
      "Int",
      scalanative::nir::binaryValue("*", scalanative::nir::localValue("value", noSpan),
                                    scalanative::nir::literalValue("-1", "Int", noSpan),
                                    noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder minusOneTimesLongBody;
  (void)minusOneTimesLongBody.addParameter("value", "Long", noSpan);
  (void)minusOneTimesLongBody.addReturn(
      "Long",
      scalanative::nir::binaryValue(
          "*", scalanative::nir::literalValue("-1L", "Long", noSpan),
          scalanative::nir::localValue("value", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Negation.zeroMinusInt", "(Int)Int",
                                std::move(zeroMinusIntBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Negation.intTimesMinusOne", "(Int)Int",
                                std::move(intTimesMinusOneBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Negation.minusOneTimesLong",
                                "(Long)Long", std::move(minusOneTimesLongBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.Negation.zeroMinusInt");
  program.reachableGlobals.push_back("demo.interflow.Negation.intTimesMinusOne");
  program.reachableGlobals.push_back("demo.interflow.Negation.minusOneTimesLong");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid integer negation program")) {
    return code;
  }
  if (int code = expect(result.reports.size() == 5 &&
                            result.reports[1].name == "fold-constants" &&
                            result.reports[1].changedValues == 3 &&
                            result.reports[1].validationErrorsBefore == 0 &&
                            result.reports[1].validationErrorsAfter == 0,
                        "interflow did not report validation-clean integer "
                        "negation identity folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* zeroMinusInt =
      findDefinition(optimizedModule, "demo.interflow.Negation.zeroMinusInt");
  const scalanative::nir::Definition* intTimesMinusOne =
      findDefinition(optimizedModule, "demo.interflow.Negation.intTimesMinusOne");
  const scalanative::nir::Definition* minusOneTimesLong =
      findDefinition(optimizedModule, "demo.interflow.Negation.minusOneTimesLong");
  if (int code = expect(zeroMinusInt != nullptr && intTimesMinusOne != nullptr &&
                            minusOneTimesLong != nullptr,
                        "interflow removed reachable integer negation functions")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(zeroMinusInt->body).back() ==
                            "ret Int (-%value)",
                        "interflow did not fold zero-minus Int to negation")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(intTimesMinusOne->body).back() ==
                            "ret Int (-%value)",
                        "interflow did not fold Int multiplication by minus one")) {
    return code;
  }
  return expect(scalanative::nir::bodyToText(minusOneTimesLong->body).back() ==
                    "ret Long (-%value)",
                "interflow did not fold Long multiplication by minus one");
}

int smokeInterflowAbsorbingIdentityFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  scalanative::nir::FunctionBodyBuilder intProductBody;
  (void)intProductBody.addParameter("value", "Int", noSpan);
  (void)intProductBody.addReturn(
      "Int",
      scalanative::nir::binaryValue("*", scalanative::nir::localValue("value", noSpan),
                                    scalanative::nir::literalValue("0", "Int", noSpan),
                                    noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder longProductBody;
  (void)longProductBody.addParameter("value", "Long", noSpan);
  (void)longProductBody.addReturn(
      "Long",
      scalanative::nir::binaryValue(
          "*", scalanative::nir::literalValue("0L", "Long", noSpan),
          scalanative::nir::localValue("value", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder moduloBody;
  (void)moduloBody.addParameter("value", "Int", noSpan);
  (void)moduloBody.addReturn(
      "Int",
      scalanative::nir::binaryValue("%", scalanative::nir::localValue("value", noSpan),
                                    scalanative::nir::literalValue("1", "Int", noSpan),
                                    noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder andFalseBody;
  (void)andFalseBody.addParameter("flag", "Boolean", noSpan);
  (void)andFalseBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "&&", scalanative::nir::localValue("flag", noSpan),
          scalanative::nir::literalValue("false", "Boolean", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder orTrueBody;
  (void)orTrueBody.addParameter("flag", "Boolean", noSpan);
  (void)orTrueBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "||", scalanative::nir::literalValue("true", "Boolean", noSpan),
          scalanative::nir::localValue("flag", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Absorbing.intProduct", "(Int)Int",
                                std::move(intProductBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Absorbing.longProduct", "(Long)Long",
                                std::move(longProductBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Absorbing.modulo", "(Int)Int",
                                std::move(moduloBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Absorbing.andFalse", "(Boolean)Boolean",
                                std::move(andFalseBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Absorbing.orTrue", "(Boolean)Boolean",
                                std::move(orTrueBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.Absorbing.intProduct");
  program.reachableGlobals.push_back("demo.interflow.Absorbing.longProduct");
  program.reachableGlobals.push_back("demo.interflow.Absorbing.modulo");
  program.reachableGlobals.push_back("demo.interflow.Absorbing.andFalse");
  program.reachableGlobals.push_back("demo.interflow.Absorbing.orTrue");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid absorbing identity program")) {
    return code;
  }
  if (int code = expect(
          result.reports.size() == 5 && result.reports[1].name == "fold-constants" &&
              result.reports[1].changedValues == 5 &&
              result.reports[1].validationErrorsBefore == 0 &&
              result.reports[1].validationErrorsAfter == 0,
          "interflow did not report validation-clean absorbing identity folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* intProduct =
      findDefinition(optimizedModule, "demo.interflow.Absorbing.intProduct");
  const scalanative::nir::Definition* longProduct =
      findDefinition(optimizedModule, "demo.interflow.Absorbing.longProduct");
  const scalanative::nir::Definition* modulo =
      findDefinition(optimizedModule, "demo.interflow.Absorbing.modulo");
  const scalanative::nir::Definition* andFalse =
      findDefinition(optimizedModule, "demo.interflow.Absorbing.andFalse");
  const scalanative::nir::Definition* orTrue =
      findDefinition(optimizedModule, "demo.interflow.Absorbing.orTrue");
  if (int code =
          expect(intProduct != nullptr && longProduct != nullptr && modulo != nullptr &&
                     andFalse != nullptr && orTrue != nullptr,
                 "interflow removed reachable absorbing identity functions")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(intProduct->body).back() == "ret Int 0",
                 "interflow did not fold Int multiplication by zero")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(longProduct->body).back() ==
                            "ret Long 0L",
                        "interflow did not fold Long multiplication by zero")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(modulo->body).back() == "ret Int 0",
                 "interflow did not fold Int modulo by one")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(andFalse->body).back() ==
                            "ret Boolean false",
                        "interflow did not fold Boolean conjunction with false")) {
    return code;
  }
  return expect(scalanative::nir::bodyToText(orTrue->body).back() == "ret Boolean true",
                "interflow did not fold Boolean disjunction with true");
}

int smokeInterflowLongFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  scalanative::nir::FunctionBodyBuilder arithmeticBody;
  (void)arithmeticBody.addReturn(
      "Long",
      scalanative::nir::binaryValue(
          "-",
          scalanative::nir::binaryValue(
              "*",
              scalanative::nir::binaryValue(
                  "+", scalanative::nir::literalValue("5L", "Long", noSpan),
                  scalanative::nir::literalValue("7L", "Long", noSpan), noSpan),
              scalanative::nir::literalValue("2L", "Long", noSpan), noSpan),
          scalanative::nir::literalValue("4L", "Long", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder comparisonBody;
  (void)comparisonBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          ">=", scalanative::nir::literalValue("9L", "Long", noSpan),
          scalanative::nir::literalValue("9L", "Long", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder identityBody;
  (void)identityBody.addParameter("value", "Long", noSpan);
  (void)identityBody.addReturn(
      "Long",
      scalanative::nir::unaryValue(
          "+",
          scalanative::nir::binaryValue(
              "*",
              scalanative::nir::binaryValue(
                  "+", scalanative::nir::localValue("value", noSpan),
                  scalanative::nir::literalValue("0L", "Long", noSpan), noSpan),
              scalanative::nir::literalValue("1L", "Long", noSpan), noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.LongFold.arithmetic", "()Long",
                                std::move(arithmeticBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.LongFold.comparison", "()Boolean",
                                std::move(comparisonBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.LongFold.identity", "(Long)Long",
                                std::move(identityBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.LongFold.arithmetic");
  program.reachableGlobals.push_back("demo.interflow.LongFold.comparison");
  program.reachableGlobals.push_back("demo.interflow.LongFold.identity");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code = expect(result.ok, "interflow rejected valid Long fold program")) {
    return code;
  }
  if (int code = expect(result.reports.size() == 5 &&
                            result.reports[1].name == "fold-constants" &&
                            result.reports[1].changedValues == 3 &&
                            result.reports[1].validationErrorsBefore == 0 &&
                            result.reports[1].validationErrorsAfter == 0,
                        "interflow did not report validation-clean Long folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* arithmetic =
      findDefinition(optimizedModule, "demo.interflow.LongFold.arithmetic");
  const scalanative::nir::Definition* comparison =
      findDefinition(optimizedModule, "demo.interflow.LongFold.comparison");
  const scalanative::nir::Definition* identity =
      findDefinition(optimizedModule, "demo.interflow.LongFold.identity");
  if (int code =
          expect(arithmetic != nullptr && comparison != nullptr && identity != nullptr,
                 "interflow removed reachable Long fold functions")) {
    return code;
  }

  if (int code = expect(scalanative::nir::bodyToText(arithmetic->body).back() ==
                            "ret Long 20L",
                        "interflow did not fold Long literal arithmetic")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(comparison->body).back() ==
                            "ret Boolean true",
                        "interflow did not fold Long literal comparison")) {
    return code;
  }

  const std::vector<std::string> identityText =
      scalanative::nir::bodyToText(identity->body);
  return expect(identityText.size() == 3 && identityText[1] == "param %value : Long" &&
                    identityText[2] == "ret Long %value",
                "interflow did not fold Long identity operations");
}

int smokeInterflowSameLocalComparisonFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  scalanative::nir::FunctionBodyBuilder intBody;
  (void)intBody.addParameter("value", "Int", noSpan);
  (void)intBody.addReturn("Boolean",
                          scalanative::nir::binaryValue(
                              "==", scalanative::nir::localValue("value", noSpan),
                              scalanative::nir::localValue("value", noSpan), noSpan),
                          noSpan);

  scalanative::nir::FunctionBodyBuilder longBody;
  (void)longBody.addParameter("value", "Long", noSpan);
  (void)longBody.addReturn("Boolean",
                           scalanative::nir::binaryValue(
                               "<", scalanative::nir::localValue("value", noSpan),
                               scalanative::nir::localValue("value", noSpan), noSpan),
                           noSpan);

  scalanative::nir::FunctionBodyBuilder booleanBody;
  (void)booleanBody.addParameter("flag", "Boolean", noSpan);
  (void)booleanBody.addReturn("Boolean",
                              scalanative::nir::binaryValue(
                                  "!=", scalanative::nir::localValue("flag", noSpan),
                                  scalanative::nir::localValue("flag", noSpan), noSpan),
                              noSpan);

  scalanative::nir::FunctionBodyBuilder stringBody;
  (void)stringBody.addParameter("text", "String", noSpan);
  (void)stringBody.addReturn("Boolean",
                             scalanative::nir::binaryValue(
                                 "==", scalanative::nir::localValue("text", noSpan),
                                 scalanative::nir::localValue("text", noSpan), noSpan),
                             noSpan);

  scalanative::nir::FunctionBodyBuilder symbolBody;
  (void)symbolBody.addParameter("symbol", "Symbol", noSpan);
  (void)symbolBody.addReturn("Boolean",
                             scalanative::nir::binaryValue(
                                 "!=", scalanative::nir::localValue("symbol", noSpan),
                                 scalanative::nir::localValue("symbol", noSpan),
                                 noSpan),
                             noSpan);

  scalanative::nir::FunctionBodyBuilder objectBody;
  (void)objectBody.addParameter("value", "Object", noSpan);
  (void)objectBody.addReturn("Boolean",
                             scalanative::nir::binaryValue(
                                 "==", scalanative::nir::localValue("value", noSpan),
                                 scalanative::nir::localValue("value", noSpan), noSpan),
                             noSpan);

  scalanative::nir::FunctionBodyBuilder boxBody;
  (void)boxBody.addParameter("box", "demo.interflow.SameLocal.Box", noSpan);
  (void)boxBody.addReturn("Boolean",
                          scalanative::nir::binaryValue(
                              "!=", scalanative::nir::localValue("box", noSpan),
                              scalanative::nir::localValue("box", noSpan), noSpan),
                          noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                "demo.interflow.SameLocal.Box",
                                "@java.lang.Object",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.SameLocal.intEquals", "(Int)Boolean",
                                std::move(intBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.SameLocal.longLess", "(Long)Boolean",
                                std::move(longBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.SameLocal.booleanNotEqual",
                                "(Boolean)Boolean", std::move(booleanBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.SameLocal.stringEquals",
                                "(String)Boolean", std::move(stringBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.SameLocal.symbolNotEqual",
                                "(Symbol)Boolean", std::move(symbolBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.SameLocal.objectEquals",
                                "(Object)Boolean", std::move(objectBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.SameLocal.boxNotEqual",
                                "(demo.interflow.SameLocal.Box)Boolean",
                                std::move(boxBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.SameLocal.intEquals");
  program.reachableGlobals.push_back("demo.interflow.SameLocal.longLess");
  program.reachableGlobals.push_back("demo.interflow.SameLocal.booleanNotEqual");
  program.reachableGlobals.push_back("demo.interflow.SameLocal.stringEquals");
  program.reachableGlobals.push_back("demo.interflow.SameLocal.symbolNotEqual");
  program.reachableGlobals.push_back("demo.interflow.SameLocal.objectEquals");
  program.reachableGlobals.push_back("demo.interflow.SameLocal.boxNotEqual");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid same-local comparison program")) {
    return code;
  }
  if (int code = expect(
          result.reports.size() == 5 && result.reports[1].name == "fold-constants" &&
              result.reports[1].changedValues == 7 &&
              result.reports[1].validationErrorsBefore == 0 &&
              result.reports[1].validationErrorsAfter == 0,
          "interflow did not report validation-clean same-local comparison folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* intEquals =
      findDefinition(optimizedModule, "demo.interflow.SameLocal.intEquals");
  const scalanative::nir::Definition* longLess =
      findDefinition(optimizedModule, "demo.interflow.SameLocal.longLess");
  const scalanative::nir::Definition* booleanNotEqual =
      findDefinition(optimizedModule, "demo.interflow.SameLocal.booleanNotEqual");
  const scalanative::nir::Definition* stringEquals =
      findDefinition(optimizedModule, "demo.interflow.SameLocal.stringEquals");
  const scalanative::nir::Definition* symbolNotEqual =
      findDefinition(optimizedModule, "demo.interflow.SameLocal.symbolNotEqual");
  const scalanative::nir::Definition* objectEquals =
      findDefinition(optimizedModule, "demo.interflow.SameLocal.objectEquals");
  const scalanative::nir::Definition* boxNotEqual =
      findDefinition(optimizedModule, "demo.interflow.SameLocal.boxNotEqual");
  if (int code = expect(
          intEquals != nullptr && longLess != nullptr && booleanNotEqual != nullptr &&
              stringEquals != nullptr && symbolNotEqual != nullptr &&
              objectEquals != nullptr && boxNotEqual != nullptr,
          "interflow removed reachable same-local comparison functions")) {
    return code;
  }

  if (int code = expect(scalanative::nir::bodyToText(intEquals->body).back() ==
                            "ret Boolean true",
                        "interflow did not fold same Int local equality")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(longLess->body).back() ==
                            "ret Boolean false",
                        "interflow did not fold same Long local less-than")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(booleanNotEqual->body).back() ==
                            "ret Boolean false",
                        "interflow did not fold same Boolean local inequality")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(stringEquals->body).back() ==
                            "ret Boolean true",
                        "interflow did not fold same String local equality")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(symbolNotEqual->body).back() ==
                            "ret Boolean false",
                        "interflow did not fold same Symbol local inequality")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(objectEquals->body).back() ==
                            "ret Boolean true",
                        "interflow did not fold same Object local equality")) {
    return code;
  }
  return expect(scalanative::nir::bodyToText(boxNotEqual->body).back() ==
                    "ret Boolean false",
                "interflow did not fold same class-like local inequality");
}

int smokeInterflowSameLocalArithmeticFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  scalanative::nir::FunctionBodyBuilder intBody;
  (void)intBody.addParameter("value", "Int", noSpan);
  (void)intBody.addReturn("Int",
                          scalanative::nir::binaryValue(
                              "-", scalanative::nir::localValue("value", noSpan),
                              scalanative::nir::localValue("value", noSpan), noSpan),
                          noSpan);

  scalanative::nir::FunctionBodyBuilder longBody;
  (void)longBody.addParameter("value", "Long", noSpan);
  (void)longBody.addReturn("Long",
                           scalanative::nir::binaryValue(
                               "-", scalanative::nir::localValue("value", noSpan),
                               scalanative::nir::localValue("value", noSpan), noSpan),
                           noSpan);

  scalanative::nir::FunctionBodyBuilder moduloBody;
  (void)moduloBody.addParameter("value", "Int", noSpan);
  (void)moduloBody.addReturn("Int",
                             scalanative::nir::binaryValue(
                                 "%", scalanative::nir::localValue("value", noSpan),
                                 scalanative::nir::localValue("value", noSpan), noSpan),
                             noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.SameArithmetic.intSubtract", "(Int)Int",
                                std::move(intBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.SameArithmetic.longSubtract",
                                "(Long)Long", std::move(longBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.SameArithmetic.modulo", "(Int)Int",
                                std::move(moduloBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.SameArithmetic.intSubtract");
  program.reachableGlobals.push_back("demo.interflow.SameArithmetic.longSubtract");
  program.reachableGlobals.push_back("demo.interflow.SameArithmetic.modulo");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid same-local arithmetic program")) {
    return code;
  }
  if (int code = expect(
          result.reports.size() == 5 && result.reports[1].name == "fold-constants" &&
              result.reports[1].changedValues == 2 &&
              result.reports[1].validationErrorsBefore == 0 &&
              result.reports[1].validationErrorsAfter == 0,
          "interflow did not report validation-clean same-local arithmetic folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* intSubtract =
      findDefinition(optimizedModule, "demo.interflow.SameArithmetic.intSubtract");
  const scalanative::nir::Definition* longSubtract =
      findDefinition(optimizedModule, "demo.interflow.SameArithmetic.longSubtract");
  const scalanative::nir::Definition* modulo =
      findDefinition(optimizedModule, "demo.interflow.SameArithmetic.modulo");
  if (int code =
          expect(intSubtract != nullptr && longSubtract != nullptr && modulo != nullptr,
                 "interflow removed reachable same-local arithmetic functions")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(intSubtract->body).back() == "ret Int 0",
                 "interflow did not fold same Int local subtraction")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(longSubtract->body).back() ==
                            "ret Long 0L",
                        "interflow did not fold same Long local subtraction")) {
    return code;
  }
  return expect(scalanative::nir::bodyToText(modulo->body).back() ==
                    "ret Int (%value % %value)",
                "interflow incorrectly folded same-local modulo by itself");
}

int smokeInterflowSameLocalBooleanFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  scalanative::nir::FunctionBodyBuilder andBody;
  (void)andBody.addParameter("flag", "Boolean", noSpan);
  (void)andBody.addReturn("Boolean",
                          scalanative::nir::binaryValue(
                              "&&", scalanative::nir::localValue("flag", noSpan),
                              scalanative::nir::localValue("flag", noSpan), noSpan),
                          noSpan);

  scalanative::nir::FunctionBodyBuilder orBody;
  (void)orBody.addParameter("flag", "Boolean", noSpan);
  (void)orBody.addReturn("Boolean",
                         scalanative::nir::binaryValue(
                             "||", scalanative::nir::localValue("flag", noSpan),
                             scalanative::nir::localValue("flag", noSpan), noSpan),
                         noSpan);

  scalanative::nir::FunctionBodyBuilder andComplementBody;
  (void)andComplementBody.addParameter("flag", "Boolean", noSpan);
  (void)andComplementBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "&&", scalanative::nir::localValue("flag", noSpan),
          scalanative::nir::unaryValue(
              "!", scalanative::nir::localValue("flag", noSpan), noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder orComplementBody;
  (void)orComplementBody.addParameter("flag", "Boolean", noSpan);
  (void)orComplementBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "||",
          scalanative::nir::unaryValue(
              "!", scalanative::nir::localValue("flag", noSpan), noSpan),
          scalanative::nir::localValue("flag", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder equalsComplementBody;
  (void)equalsComplementBody.addParameter("flag", "Boolean", noSpan);
  (void)equalsComplementBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "==", scalanative::nir::localValue("flag", noSpan),
          scalanative::nir::unaryValue(
              "!", scalanative::nir::localValue("flag", noSpan), noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder notEqualsComplementBody;
  (void)notEqualsComplementBody.addParameter("flag", "Boolean", noSpan);
  (void)notEqualsComplementBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "!=",
          scalanative::nir::unaryValue(
              "!", scalanative::nir::localValue("flag", noSpan), noSpan),
          scalanative::nir::localValue("flag", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.SameBoolean.andSelf",
                                "(Boolean)Boolean", std::move(andBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.SameBoolean.orSelf", "(Boolean)Boolean",
                                std::move(orBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.SameBoolean.andComplement",
                                "(Boolean)Boolean",
                                std::move(andComplementBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.SameBoolean.orComplement",
                                "(Boolean)Boolean", std::move(orComplementBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.SameBoolean.equalsComplement",
                                "(Boolean)Boolean",
                                std::move(equalsComplementBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.SameBoolean.notEqualsComplement",
                                "(Boolean)Boolean",
                                std::move(notEqualsComplementBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.SameBoolean.andSelf");
  program.reachableGlobals.push_back("demo.interflow.SameBoolean.orSelf");
  program.reachableGlobals.push_back("demo.interflow.SameBoolean.andComplement");
  program.reachableGlobals.push_back("demo.interflow.SameBoolean.orComplement");
  program.reachableGlobals.push_back("demo.interflow.SameBoolean.equalsComplement");
  program.reachableGlobals.push_back("demo.interflow.SameBoolean.notEqualsComplement");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid same-local Boolean program")) {
    return code;
  }
  if (int code = expect(
          result.reports.size() == 5 && result.reports[1].name == "fold-constants" &&
              result.reports[1].changedValues == 6 &&
              result.reports[1].validationErrorsBefore == 0 &&
              result.reports[1].validationErrorsAfter == 0,
          "interflow did not report validation-clean same-local Boolean folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* andSelf =
      findDefinition(optimizedModule, "demo.interflow.SameBoolean.andSelf");
  const scalanative::nir::Definition* orSelf =
      findDefinition(optimizedModule, "demo.interflow.SameBoolean.orSelf");
  const scalanative::nir::Definition* andComplement =
      findDefinition(optimizedModule, "demo.interflow.SameBoolean.andComplement");
  const scalanative::nir::Definition* orComplement =
      findDefinition(optimizedModule, "demo.interflow.SameBoolean.orComplement");
  const scalanative::nir::Definition* equalsComplement =
      findDefinition(optimizedModule, "demo.interflow.SameBoolean.equalsComplement");
  const scalanative::nir::Definition* notEqualsComplement =
      findDefinition(optimizedModule, "demo.interflow.SameBoolean.notEqualsComplement");
  if (int code =
          expect(andSelf != nullptr && orSelf != nullptr && andComplement != nullptr &&
                     orComplement != nullptr && equalsComplement != nullptr &&
                     notEqualsComplement != nullptr,
                 "interflow removed reachable same-local Boolean functions")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(andSelf->body).back() ==
                            "ret Boolean %flag",
                        "interflow did not fold same-local Boolean conjunction")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(orSelf->body).back() ==
                            "ret Boolean %flag",
                        "interflow did not fold same-local Boolean disjunction")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(andComplement->body).back() ==
                            "ret Boolean false",
                        "interflow did not fold same-local Boolean contradiction")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(orComplement->body).back() ==
                            "ret Boolean true",
                        "interflow did not fold same-local Boolean tautology")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(equalsComplement->body).back() ==
                            "ret Boolean false",
                        "interflow did not fold same-local Boolean complement "
                        "equality")) {
    return code;
  }
  return expect(scalanative::nir::bodyToText(notEqualsComplement->body).back() ==
                    "ret Boolean true",
                "interflow did not fold same-local Boolean complement inequality");
}

int smokeInterflowBooleanAbsorptionFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();
  constexpr std::string_view BoxType = "demo.interflow.BooleanAbsorption.Box";
  const auto flag = [&]() { return scalanative::nir::localValue("flag", noSpan); };
  const auto other = [&]() { return scalanative::nir::localValue("other", noSpan); };
  const auto typeTest = [&]() {
    return scalanative::nir::isInstanceOfValue(
        std::string(BoxType), scalanative::nir::localValue("box", noSpan), noSpan);
  };
  const auto effectCall = [&]() {
    return scalanative::nir::callValue(
        scalanative::nir::localValue("demo.interflow.BooleanAbsorption.effect", noSpan),
        {}, noSpan);
  };

  scalanative::nir::FunctionBodyBuilder andRightBody;
  (void)andRightBody.addParameter("flag", "Boolean", noSpan);
  (void)andRightBody.addParameter("other", "Boolean", noSpan);
  (void)andRightBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "&&", flag(), scalanative::nir::binaryValue("||", flag(), other(), noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder andLeftBody;
  (void)andLeftBody.addParameter("flag", "Boolean", noSpan);
  (void)andLeftBody.addParameter("other", "Boolean", noSpan);
  (void)andLeftBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "&&", scalanative::nir::binaryValue("||", other(), flag(), noSpan), flag(),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder orRightBody;
  (void)orRightBody.addParameter("flag", "Boolean", noSpan);
  (void)orRightBody.addParameter("other", "Boolean", noSpan);
  (void)orRightBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "||", flag(), scalanative::nir::binaryValue("&&", flag(), other(), noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder orLeftBody;
  (void)orLeftBody.addParameter("flag", "Boolean", noSpan);
  (void)orLeftBody.addParameter("other", "Boolean", noSpan);
  (void)orLeftBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "||", scalanative::nir::binaryValue("&&", other(), flag(), noSpan), flag(),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder structuredBody;
  (void)structuredBody.addParameter("box", std::string(BoxType), noSpan);
  (void)structuredBody.addParameter("other", "Boolean", noSpan);
  (void)structuredBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "&&", typeTest(),
          scalanative::nir::binaryValue("||", typeTest(), other(), noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder andComplementBody;
  (void)andComplementBody.addParameter("flag", "Boolean", noSpan);
  (void)andComplementBody.addParameter("other", "Boolean", noSpan);
  (void)andComplementBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "&&", flag(),
          scalanative::nir::binaryValue(
              "||", scalanative::nir::unaryValue("!", flag(), noSpan), other(), noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder orComplementBody;
  (void)orComplementBody.addParameter("flag", "Boolean", noSpan);
  (void)orComplementBody.addParameter("other", "Boolean", noSpan);
  (void)orComplementBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "||", flag(),
          scalanative::nir::binaryValue(
              "&&", scalanative::nir::unaryValue("!", flag(), noSpan), other(), noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder structuredComplementBody;
  (void)structuredComplementBody.addParameter("box", std::string(BoxType), noSpan);
  (void)structuredComplementBody.addParameter("other", "Boolean", noSpan);
  (void)structuredComplementBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "&&", typeTest(),
          scalanative::nir::binaryValue(
              "||", scalanative::nir::unaryValue("!", typeTest(), noSpan), other(),
              noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder effectGuardBody;
  (void)effectGuardBody.addParameter("flag", "Boolean", noSpan);
  (void)effectGuardBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "&&", flag(),
          scalanative::nir::binaryValue("||", flag(), effectCall(), noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder effectComplementGuardBody;
  (void)effectComplementGuardBody.addParameter("flag", "Boolean", noSpan);
  (void)effectComplementGuardBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "&&", flag(),
          scalanative::nir::binaryValue(
              "||", scalanative::nir::unaryValue("!", flag(), noSpan), effectCall(),
              noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                std::string(BoxType),
                                "@java.lang.Object",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.BooleanAbsorption.andRight",
                                "(Boolean,Boolean)Boolean",
                                std::move(andRightBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.BooleanAbsorption.andLeft",
                                "(Boolean,Boolean)Boolean",
                                std::move(andLeftBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.BooleanAbsorption.orRight",
                                "(Boolean,Boolean)Boolean",
                                std::move(orRightBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.BooleanAbsorption.orLeft",
                                "(Boolean,Boolean)Boolean",
                                std::move(orLeftBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.BooleanAbsorption.structured",
                                "(" + std::string(BoxType) + ",Boolean)Boolean",
                                std::move(structuredBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.BooleanAbsorption.andComplement",
                                "(Boolean,Boolean)Boolean",
                                std::move(andComplementBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.BooleanAbsorption.orComplement",
                                "(Boolean,Boolean)Boolean",
                                std::move(orComplementBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.BooleanAbsorption.structuredComplement",
                                "(" + std::string(BoxType) + ",Boolean)Boolean",
                                std::move(structuredComplementBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.BooleanAbsorption.effectGuard",
                                "(Boolean)Boolean", std::move(effectGuardBody).build(),
                                noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.BooleanAbsorption.effectComplementGuard", "(Boolean)Boolean",
       std::move(effectComplementGuardBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                "demo.interflow.BooleanAbsorption.effect",
                                "()Boolean",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.BooleanAbsorption.andRight");
  program.reachableGlobals.push_back("demo.interflow.BooleanAbsorption.andLeft");
  program.reachableGlobals.push_back("demo.interflow.BooleanAbsorption.orRight");
  program.reachableGlobals.push_back("demo.interflow.BooleanAbsorption.orLeft");
  program.reachableGlobals.push_back("demo.interflow.BooleanAbsorption.structured");
  program.reachableGlobals.push_back("demo.interflow.BooleanAbsorption.andComplement");
  program.reachableGlobals.push_back("demo.interflow.BooleanAbsorption.orComplement");
  program.reachableGlobals.push_back(
      "demo.interflow.BooleanAbsorption.structuredComplement");
  program.reachableGlobals.push_back("demo.interflow.BooleanAbsorption.effectGuard");
  program.reachableGlobals.push_back(
      "demo.interflow.BooleanAbsorption.effectComplementGuard");
  program.reachableGlobals.push_back("demo.interflow.BooleanAbsorption.effect");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid Boolean absorption program")) {
    return code;
  }
  if (int code = expect(
          result.reports.size() == 5 && result.reports[1].name == "fold-constants" &&
              result.reports[1].changedValues == 8 &&
              result.reports[1].validationErrorsBefore == 0 &&
              result.reports[1].validationErrorsAfter == 0,
          "interflow did not report validation-clean Boolean absorption folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* andRight =
      findDefinition(optimizedModule, "demo.interflow.BooleanAbsorption.andRight");
  const scalanative::nir::Definition* andLeft =
      findDefinition(optimizedModule, "demo.interflow.BooleanAbsorption.andLeft");
  const scalanative::nir::Definition* orRight =
      findDefinition(optimizedModule, "demo.interflow.BooleanAbsorption.orRight");
  const scalanative::nir::Definition* orLeft =
      findDefinition(optimizedModule, "demo.interflow.BooleanAbsorption.orLeft");
  const scalanative::nir::Definition* structured =
      findDefinition(optimizedModule, "demo.interflow.BooleanAbsorption.structured");
  const scalanative::nir::Definition* andComplement =
      findDefinition(optimizedModule, "demo.interflow.BooleanAbsorption.andComplement");
  const scalanative::nir::Definition* orComplement =
      findDefinition(optimizedModule, "demo.interflow.BooleanAbsorption.orComplement");
  const scalanative::nir::Definition* structuredComplement = findDefinition(
      optimizedModule, "demo.interflow.BooleanAbsorption.structuredComplement");
  const scalanative::nir::Definition* effectGuard =
      findDefinition(optimizedModule, "demo.interflow.BooleanAbsorption.effectGuard");
  const scalanative::nir::Definition* effectComplementGuard = findDefinition(
      optimizedModule, "demo.interflow.BooleanAbsorption.effectComplementGuard");
  const scalanative::nir::Definition* effect =
      findDefinition(optimizedModule, "demo.interflow.BooleanAbsorption.effect");
  if (int code =
          expect(andRight != nullptr && andLeft != nullptr && orRight != nullptr &&
                     orLeft != nullptr && structured != nullptr &&
                     andComplement != nullptr && orComplement != nullptr &&
                     structuredComplement != nullptr && effectGuard != nullptr &&
                     effectComplementGuard != nullptr && effect != nullptr,
                 "interflow removed reachable Boolean absorption functions")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(andRight->body).back() ==
                            "ret Boolean %flag",
                        "interflow did not fold right Boolean and absorption")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(andLeft->body).back() ==
                            "ret Boolean %flag",
                        "interflow did not fold left Boolean and absorption")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(orRight->body).back() ==
                            "ret Boolean %flag",
                        "interflow did not fold right Boolean or absorption")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(orLeft->body).back() ==
                            "ret Boolean %flag",
                        "interflow did not fold left Boolean or absorption")) {
    return code;
  }
  if (int code = expect(
          scalanative::nir::bodyToText(structured->body).back() ==
              "ret Boolean is-instance-of[demo.interflow.BooleanAbsorption.Box](%box)",
          "interflow did not fold structured Boolean absorption")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(andComplement->body).back() ==
                            "ret Boolean (%flag && %other)",
                        "interflow did not fold Boolean complemented and absorption")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(orComplement->body).back() ==
                            "ret Boolean (%flag || %other)",
                        "interflow did not fold Boolean complemented or absorption")) {
    return code;
  }
  if (int code = expect(
          scalanative::nir::bodyToText(structuredComplement->body).back() ==
              "ret Boolean (is-instance-of[demo.interflow.BooleanAbsorption.Box](%box) "
              "&& %other)",
          "interflow did not fold structured Boolean complemented absorption")) {
    return code;
  }
  if (int code = expect(
          scalanative::nir::bodyToText(effectGuard->body).back() ==
              "ret Boolean (%flag && (%flag || call "
              "%demo.interflow.BooleanAbsorption.effect()))",
          "interflow incorrectly folded absorption across an effectful operand")) {
    return code;
  }
  return expect(scalanative::nir::bodyToText(effectComplementGuard->body).back() ==
                    "ret Boolean (%flag && ((!%flag) || call "
                    "%demo.interflow.BooleanAbsorption.effect()))",
                "interflow incorrectly folded complemented absorption across an "
                "effectful operand");
}

int smokeInterflowPureStructuredOperandFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();
  constexpr std::string_view BoxType = "demo.interflow.PureStructured.Box";
  const auto increment = [&]() {
    return scalanative::nir::binaryValue(
        "+", scalanative::nir::localValue("value", noSpan),
        scalanative::nir::literalValue("1", "Int", noSpan), noSpan);
  };
  const auto typeTest = [&]() {
    return scalanative::nir::isInstanceOfValue(
        std::string(BoxType), scalanative::nir::localValue("box", noSpan), noSpan);
  };

  scalanative::nir::FunctionBodyBuilder subtractBody;
  (void)subtractBody.addParameter("value", "Int", noSpan);
  (void)subtractBody.addReturn(
      "Int", scalanative::nir::binaryValue("-", increment(), increment(), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder equalsBody;
  (void)equalsBody.addParameter("value", "Int", noSpan);
  (void)equalsBody.addReturn(
      "Boolean", scalanative::nir::binaryValue("==", increment(), increment(), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder booleanBody;
  (void)booleanBody.addParameter("box", std::string(BoxType), noSpan);
  (void)booleanBody.addReturn(
      "Boolean", scalanative::nir::binaryValue("||", typeTest(), typeTest(), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder booleanContradictionBody;
  (void)booleanContradictionBody.addParameter("box", std::string(BoxType), noSpan);
  (void)booleanContradictionBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "&&", typeTest(), scalanative::nir::unaryValue("!", typeTest(), noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder booleanTautologyBody;
  (void)booleanTautologyBody.addParameter("box", std::string(BoxType), noSpan);
  (void)booleanTautologyBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "||", scalanative::nir::unaryValue("!", typeTest(), noSpan), typeTest(),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder moduloBody;
  (void)moduloBody.addParameter("value", "Int", noSpan);
  (void)moduloBody.addReturn(
      "Int", scalanative::nir::binaryValue("%", increment(), increment(), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                std::string(BoxType),
                                "@java.lang.Object",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.PureStructured.subtract", "(Int)Int",
                                std::move(subtractBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.PureStructured.equals", "(Int)Boolean",
                                std::move(equalsBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.PureStructured.booleanIdempotent",
                                "(" + std::string(BoxType) + ")Boolean",
                                std::move(booleanBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.PureStructured.booleanContradiction",
                                "(" + std::string(BoxType) + ")Boolean",
                                std::move(booleanContradictionBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.PureStructured.booleanTautology",
                                "(" + std::string(BoxType) + ")Boolean",
                                std::move(booleanTautologyBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.PureStructured.modulo", "(Int)Int",
                                std::move(moduloBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.PureStructured.subtract");
  program.reachableGlobals.push_back("demo.interflow.PureStructured.equals");
  program.reachableGlobals.push_back("demo.interflow.PureStructured.booleanIdempotent");
  program.reachableGlobals.push_back(
      "demo.interflow.PureStructured.booleanContradiction");
  program.reachableGlobals.push_back("demo.interflow.PureStructured.booleanTautology");
  program.reachableGlobals.push_back("demo.interflow.PureStructured.modulo");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code = expect(result.ok,
                        "interflow rejected valid pure structured operand program")) {
    return code;
  }
  if (int code = expect(
          result.reports.size() == 5 && result.reports[1].name == "fold-constants" &&
              result.reports[1].changedValues == 5 &&
              result.reports[1].validationErrorsBefore == 0 &&
              result.reports[1].validationErrorsAfter == 0,
          "interflow did not report validation-clean pure structured operand "
          "folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* subtract =
      findDefinition(optimizedModule, "demo.interflow.PureStructured.subtract");
  const scalanative::nir::Definition* equals =
      findDefinition(optimizedModule, "demo.interflow.PureStructured.equals");
  const scalanative::nir::Definition* booleanIdempotent = findDefinition(
      optimizedModule, "demo.interflow.PureStructured.booleanIdempotent");
  const scalanative::nir::Definition* booleanContradiction = findDefinition(
      optimizedModule, "demo.interflow.PureStructured.booleanContradiction");
  const scalanative::nir::Definition* booleanTautology =
      findDefinition(optimizedModule, "demo.interflow.PureStructured.booleanTautology");
  const scalanative::nir::Definition* modulo =
      findDefinition(optimizedModule, "demo.interflow.PureStructured.modulo");
  if (int code =
          expect(subtract != nullptr && equals != nullptr &&
                     booleanIdempotent != nullptr && booleanContradiction != nullptr &&
                     booleanTautology != nullptr && modulo != nullptr,
                 "interflow removed reachable pure structured operand functions")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(subtract->body).back() == "ret Int 0",
                 "interflow did not fold identical pure Int subtraction")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(equals->body).back() ==
                            "ret Boolean true",
                        "interflow did not fold identical pure Int comparison")) {
    return code;
  }
  if (int code = expect(
          scalanative::nir::bodyToText(booleanIdempotent->body).back() ==
              "ret Boolean is-instance-of[demo.interflow.PureStructured.Box](%box)",
          "interflow did not fold identical pure Boolean operands")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(booleanContradiction->body).back() ==
                     "ret Boolean false",
                 "interflow did not fold pure Boolean contradiction")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(booleanTautology->body).back() ==
                            "ret Boolean true",
                        "interflow did not fold pure Boolean tautology")) {
    return code;
  }
  return expect(scalanative::nir::bodyToText(modulo->body).back() ==
                    "ret Int ((%value + 1) % (%value + 1))",
                "interflow incorrectly folded modulo by identical pure operand");
}

int smokeInterflowBooleanComparisonCanonicalization() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  scalanative::nir::FunctionBodyBuilder equalsTrueBody;
  (void)equalsTrueBody.addParameter("flag", "Boolean", noSpan);
  (void)equalsTrueBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "==", scalanative::nir::localValue("flag", noSpan),
          scalanative::nir::literalValue("true", "Boolean", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder falseEqualsBody;
  (void)falseEqualsBody.addParameter("flag", "Boolean", noSpan);
  (void)falseEqualsBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "==", scalanative::nir::literalValue("false", "Boolean", noSpan),
          scalanative::nir::localValue("flag", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder notEqualsTrueBody;
  (void)notEqualsTrueBody.addParameter("flag", "Boolean", noSpan);
  (void)notEqualsTrueBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "!=", scalanative::nir::localValue("flag", noSpan),
          scalanative::nir::literalValue("true", "Boolean", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder falseNotEqualsBody;
  (void)falseNotEqualsBody.addParameter("flag", "Boolean", noSpan);
  (void)falseNotEqualsBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "!=", scalanative::nir::literalValue("false", "Boolean", noSpan),
          scalanative::nir::localValue("flag", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder negatedEqualsFalseBody;
  (void)negatedEqualsFalseBody.addParameter("flag", "Boolean", noSpan);
  (void)negatedEqualsFalseBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "==",
          scalanative::nir::unaryValue(
              "!", scalanative::nir::localValue("flag", noSpan), noSpan),
          scalanative::nir::literalValue("false", "Boolean", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder trueNotEqualsNegatedBody;
  (void)trueNotEqualsNegatedBody.addParameter("flag", "Boolean", noSpan);
  (void)trueNotEqualsNegatedBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "!=", scalanative::nir::literalValue("true", "Boolean", noSpan),
          scalanative::nir::unaryValue(
              "!", scalanative::nir::localValue("flag", noSpan), noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder negatedEqualityBody;
  (void)negatedEqualityBody.addParameter("left", "Int", noSpan);
  (void)negatedEqualityBody.addParameter("right", "Int", noSpan);
  (void)negatedEqualityBody.addReturn(
      "Boolean",
      scalanative::nir::unaryValue(
          "!",
          scalanative::nir::binaryValue(
              "==", scalanative::nir::localValue("left", noSpan),
              scalanative::nir::localValue("right", noSpan), noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder negatedInequalityBody;
  (void)negatedInequalityBody.addParameter("left", "Int", noSpan);
  (void)negatedInequalityBody.addParameter("right", "Int", noSpan);
  (void)negatedInequalityBody.addReturn(
      "Boolean",
      scalanative::nir::unaryValue(
          "!",
          scalanative::nir::binaryValue(
              "!=", scalanative::nir::localValue("left", noSpan),
              scalanative::nir::localValue("right", noSpan), noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder negatedLessThanBody;
  (void)negatedLessThanBody.addParameter("left", "Int", noSpan);
  (void)negatedLessThanBody.addParameter("right", "Int", noSpan);
  (void)negatedLessThanBody.addReturn(
      "Boolean",
      scalanative::nir::unaryValue(
          "!",
          scalanative::nir::binaryValue(
              "<", scalanative::nir::localValue("left", noSpan),
              scalanative::nir::localValue("right", noSpan), noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder negatedGreaterOrEqualBody;
  (void)negatedGreaterOrEqualBody.addParameter("left", "Int", noSpan);
  (void)negatedGreaterOrEqualBody.addParameter("right", "Int", noSpan);
  (void)negatedGreaterOrEqualBody.addReturn(
      "Boolean",
      scalanative::nir::unaryValue(
          "!",
          scalanative::nir::binaryValue(
              ">=", scalanative::nir::localValue("left", noSpan),
              scalanative::nir::localValue("right", noSpan), noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directGreaterBody;
  (void)directGreaterBody.addParameter("left", "Int", noSpan);
  (void)directGreaterBody.addParameter("right", "Int", noSpan);
  (void)directGreaterBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(">", scalanative::nir::localValue("right", noSpan),
                                    scalanative::nir::localValue("left", noSpan),
                                    noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directGreaterOrEqualBody;
  (void)directGreaterOrEqualBody.addParameter("left", "Int", noSpan);
  (void)directGreaterOrEqualBody.addParameter("right", "Int", noSpan);
  (void)directGreaterOrEqualBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(">=", scalanative::nir::localValue("right", noSpan),
                                    scalanative::nir::localValue("left", noSpan),
                                    noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.BoolCompare.equalsTrue",
                                "(Boolean)Boolean", std::move(equalsTrueBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.BoolCompare.falseEquals",
                                "(Boolean)Boolean", std::move(falseEqualsBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.BoolCompare.notEqualsTrue",
                                "(Boolean)Boolean",
                                std::move(notEqualsTrueBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.BoolCompare.falseNotEquals",
                                "(Boolean)Boolean",
                                std::move(falseNotEqualsBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.BoolCompare.negatedEqualsFalse",
                                "(Boolean)Boolean",
                                std::move(negatedEqualsFalseBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.BoolCompare.trueNotEqualsNegated",
                                "(Boolean)Boolean",
                                std::move(trueNotEqualsNegatedBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.BoolCompare.negatedEquality",
                                "(Int,Int)Boolean",
                                std::move(negatedEqualityBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.BoolCompare.negatedInequality",
                                "(Int,Int)Boolean",
                                std::move(negatedInequalityBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.BoolCompare.negatedLessThan",
                                "(Int,Int)Boolean",
                                std::move(negatedLessThanBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.BoolCompare.negatedGreaterOrEqual",
                                "(Int,Int)Boolean",
                                std::move(negatedGreaterOrEqualBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.BoolCompare.directGreater",
                                "(Int,Int)Boolean",
                                std::move(directGreaterBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.BoolCompare.directGreaterOrEqual",
                                "(Int,Int)Boolean",
                                std::move(directGreaterOrEqualBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.BoolCompare.equalsTrue");
  program.reachableGlobals.push_back("demo.interflow.BoolCompare.falseEquals");
  program.reachableGlobals.push_back("demo.interflow.BoolCompare.notEqualsTrue");
  program.reachableGlobals.push_back("demo.interflow.BoolCompare.falseNotEquals");
  program.reachableGlobals.push_back("demo.interflow.BoolCompare.negatedEqualsFalse");
  program.reachableGlobals.push_back("demo.interflow.BoolCompare.trueNotEqualsNegated");
  program.reachableGlobals.push_back("demo.interflow.BoolCompare.negatedEquality");
  program.reachableGlobals.push_back("demo.interflow.BoolCompare.negatedInequality");
  program.reachableGlobals.push_back("demo.interflow.BoolCompare.negatedLessThan");
  program.reachableGlobals.push_back(
      "demo.interflow.BoolCompare.negatedGreaterOrEqual");
  program.reachableGlobals.push_back("demo.interflow.BoolCompare.directGreater");
  program.reachableGlobals.push_back("demo.interflow.BoolCompare.directGreaterOrEqual");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid Boolean comparison program")) {
    return code;
  }
  if (int code = expect(result.reports.size() == 5 &&
                            result.reports[1].name == "fold-constants" &&
                            result.reports[1].changedValues == 12 &&
                            result.reports[1].validationErrorsBefore == 0 &&
                            result.reports[1].validationErrorsAfter == 0,
                        "interflow did not report validation-clean Boolean comparison "
                        "canonicalization")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* equalsTrue =
      findDefinition(optimizedModule, "demo.interflow.BoolCompare.equalsTrue");
  const scalanative::nir::Definition* falseEquals =
      findDefinition(optimizedModule, "demo.interflow.BoolCompare.falseEquals");
  const scalanative::nir::Definition* notEqualsTrue =
      findDefinition(optimizedModule, "demo.interflow.BoolCompare.notEqualsTrue");
  const scalanative::nir::Definition* falseNotEquals =
      findDefinition(optimizedModule, "demo.interflow.BoolCompare.falseNotEquals");
  const scalanative::nir::Definition* negatedEqualsFalse =
      findDefinition(optimizedModule, "demo.interflow.BoolCompare.negatedEqualsFalse");
  const scalanative::nir::Definition* trueNotEqualsNegated = findDefinition(
      optimizedModule, "demo.interflow.BoolCompare.trueNotEqualsNegated");
  const scalanative::nir::Definition* negatedEquality =
      findDefinition(optimizedModule, "demo.interflow.BoolCompare.negatedEquality");
  const scalanative::nir::Definition* negatedInequality =
      findDefinition(optimizedModule, "demo.interflow.BoolCompare.negatedInequality");
  const scalanative::nir::Definition* negatedLessThan =
      findDefinition(optimizedModule, "demo.interflow.BoolCompare.negatedLessThan");
  const scalanative::nir::Definition* negatedGreaterOrEqual = findDefinition(
      optimizedModule, "demo.interflow.BoolCompare.negatedGreaterOrEqual");
  const scalanative::nir::Definition* directGreater =
      findDefinition(optimizedModule, "demo.interflow.BoolCompare.directGreater");
  const scalanative::nir::Definition* directGreaterOrEqual = findDefinition(
      optimizedModule, "demo.interflow.BoolCompare.directGreaterOrEqual");
  if (int code =
          expect(equalsTrue != nullptr && falseEquals != nullptr &&
                     notEqualsTrue != nullptr && falseNotEquals != nullptr &&
                     negatedEqualsFalse != nullptr && trueNotEqualsNegated != nullptr &&
                     negatedEquality != nullptr && negatedInequality != nullptr &&
                     negatedLessThan != nullptr && negatedGreaterOrEqual != nullptr &&
                     directGreater != nullptr && directGreaterOrEqual != nullptr,
                 "interflow removed reachable Boolean comparison functions")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(equalsTrue->body).back() ==
                            "ret Boolean %flag",
                        "interflow did not collapse Boolean equality with true")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(falseEquals->body).back() ==
                            "ret Boolean (!%flag)",
                        "interflow did not collapse Boolean equality with false")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(notEqualsTrue->body).back() ==
                            "ret Boolean (!%flag)",
                        "interflow did not collapse Boolean inequality with true")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(falseNotEquals->body).back() ==
                            "ret Boolean %flag",
                        "interflow did not collapse Boolean inequality with false")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(negatedEqualsFalse->body).back() ==
                     "ret Boolean %flag",
                 "interflow did not cancel negated Boolean equality with false")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(trueNotEqualsNegated->body).back() ==
                     "ret Boolean %flag",
                 "interflow did not cancel negated Boolean inequality with true")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(negatedEquality->body).back() ==
                            "ret Boolean (%left != %right)",
                        "interflow did not invert negated equality")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(negatedInequality->body).back() ==
                            "ret Boolean (%left == %right)",
                        "interflow did not invert negated inequality")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(negatedLessThan->body).back() ==
                            "ret Boolean (%right <= %left)",
                        "interflow did not invert negated less-than")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(negatedGreaterOrEqual->body).back() ==
                     "ret Boolean (%left < %right)",
                 "interflow did not invert negated greater-or-equal")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(directGreater->body).back() ==
                            "ret Boolean (%left < %right)",
                        "interflow did not canonicalize direct greater-than")) {
    return code;
  }
  return expect(scalanative::nir::bodyToText(directGreaterOrEqual->body).back() ==
                    "ret Boolean (%left <= %right)",
                "interflow did not canonicalize direct greater-or-equal");
}

int smokeInterflowBooleanDeMorganCanonicalization() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  scalanative::nir::FunctionBodyBuilder andBody;
  (void)andBody.addParameter("left", "Boolean", noSpan);
  (void)andBody.addParameter("right", "Boolean", noSpan);
  (void)andBody.addReturn(
      "Boolean",
      scalanative::nir::unaryValue(
          "!",
          scalanative::nir::binaryValue(
              "&&", scalanative::nir::localValue("left", noSpan),
              scalanative::nir::localValue("right", noSpan), noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder orBody;
  (void)orBody.addParameter("left", "Boolean", noSpan);
  (void)orBody.addParameter("right", "Boolean", noSpan);
  (void)orBody.addReturn("Boolean",
                         scalanative::nir::unaryValue(
                             "!",
                             scalanative::nir::binaryValue(
                                 "||", scalanative::nir::localValue("left", noSpan),
                                 scalanative::nir::localValue("right", noSpan), noSpan),
                             noSpan),
                         noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.DeMorgan.negatedAnd",
                                "(Boolean,Boolean)Boolean", std::move(andBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.DeMorgan.negatedOr",
                                "(Boolean,Boolean)Boolean", std::move(orBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.DeMorgan.negatedAnd");
  program.reachableGlobals.push_back("demo.interflow.DeMorgan.negatedOr");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid Boolean De Morgan program")) {
    return code;
  }
  if (int code = expect(result.reports.size() == 5 &&
                            result.reports[1].name == "fold-constants" &&
                            result.reports[1].changedValues == 2 &&
                            result.reports[1].validationErrorsBefore == 0 &&
                            result.reports[1].validationErrorsAfter == 0,
                        "interflow did not report validation-clean Boolean De "
                        "Morgan canonicalization")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* negatedAnd =
      findDefinition(optimizedModule, "demo.interflow.DeMorgan.negatedAnd");
  const scalanative::nir::Definition* negatedOr =
      findDefinition(optimizedModule, "demo.interflow.DeMorgan.negatedOr");
  if (int code = expect(negatedAnd != nullptr && negatedOr != nullptr,
                        "interflow removed reachable De Morgan functions")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(negatedAnd->body).back() ==
                            "ret Boolean ((!%left) || (!%right))",
                        "interflow did not canonicalize negated Boolean and")) {
    return code;
  }
  return expect(scalanative::nir::bodyToText(negatedOr->body).back() ==
                    "ret Boolean ((!%left) && (!%right))",
                "interflow did not canonicalize negated Boolean or");
}

int smokeInterflowUnaryIdentityFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  scalanative::nir::FunctionBodyBuilder intBody;
  (void)intBody.addParameter("value", "Int", noSpan);
  (void)intBody.addReturn(
      "Int",
      scalanative::nir::unaryValue("+", scalanative::nir::localValue("value", noSpan),
                                   noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder longBody;
  (void)longBody.addParameter("value", "Long", noSpan);
  (void)longBody.addReturn(
      "Long",
      scalanative::nir::unaryValue(
          "-",
          scalanative::nir::unaryValue(
              "-", scalanative::nir::localValue("value", noSpan), noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder floatBody;
  (void)floatBody.addParameter("value", "Float", noSpan);
  (void)floatBody.addReturn(
      "Float",
      scalanative::nir::unaryValue("+", scalanative::nir::localValue("value", noSpan),
                                   noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder doubleBody;
  (void)doubleBody.addParameter("value", "Double", noSpan);
  (void)doubleBody.addReturn(
      "Double",
      scalanative::nir::unaryValue("+", scalanative::nir::localValue("value", noSpan),
                                   noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder booleanBody;
  (void)booleanBody.addParameter("flag", "Boolean", noSpan);
  (void)booleanBody.addReturn(
      "Boolean",
      scalanative::nir::unaryValue(
          "!",
          scalanative::nir::unaryValue(
              "!", scalanative::nir::localValue("flag", noSpan), noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.UnaryIdentity.intValue", "(Int)Int",
                                std::move(intBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.UnaryIdentity.longValue", "(Long)Long",
                                std::move(longBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.UnaryIdentity.floatValue",
                                "(Float)Float", std::move(floatBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.UnaryIdentity.doubleValue",
                                "(Double)Double", std::move(doubleBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.UnaryIdentity.booleanValue",
                                "(Boolean)Boolean", std::move(booleanBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.UnaryIdentity.intValue");
  program.reachableGlobals.push_back("demo.interflow.UnaryIdentity.longValue");
  program.reachableGlobals.push_back("demo.interflow.UnaryIdentity.floatValue");
  program.reachableGlobals.push_back("demo.interflow.UnaryIdentity.doubleValue");
  program.reachableGlobals.push_back("demo.interflow.UnaryIdentity.booleanValue");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code = expect(result.ok, "interflow rejected valid unary identity program")) {
    return code;
  }
  if (int code = expect(
          result.reports.size() == 5 && result.reports[1].name == "fold-constants" &&
              result.reports[1].changedValues == 5 &&
              result.reports[1].validationErrorsBefore == 0 &&
              result.reports[1].validationErrorsAfter == 0,
          "interflow did not report validation-clean unary identity folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* foldedInt =
      findDefinition(optimizedModule, "demo.interflow.UnaryIdentity.intValue");
  const scalanative::nir::Definition* foldedLong =
      findDefinition(optimizedModule, "demo.interflow.UnaryIdentity.longValue");
  const scalanative::nir::Definition* foldedFloat =
      findDefinition(optimizedModule, "demo.interflow.UnaryIdentity.floatValue");
  const scalanative::nir::Definition* foldedDouble =
      findDefinition(optimizedModule, "demo.interflow.UnaryIdentity.doubleValue");
  const scalanative::nir::Definition* foldedBoolean =
      findDefinition(optimizedModule, "demo.interflow.UnaryIdentity.booleanValue");
  if (int code = expect(foldedInt != nullptr && foldedLong != nullptr &&
                            foldedFloat != nullptr && foldedDouble != nullptr &&
                            foldedBoolean != nullptr,
                        "interflow removed reachable unary identity functions")) {
    return code;
  }

  const std::vector<std::string> intText =
      scalanative::nir::bodyToText(foldedInt->body);
  if (int code = expect(intText.size() == 3 && intText[1] == "param %value : Int" &&
                            intText[2] == "ret Int %value",
                        "interflow did not collapse unary Int identity")) {
    return code;
  }

  const std::vector<std::string> longText =
      scalanative::nir::bodyToText(foldedLong->body);
  if (int code = expect(longText.size() == 3 && longText[1] == "param %value : Long" &&
                            longText[2] == "ret Long %value",
                        "interflow did not collapse double Long negation")) {
    return code;
  }

  const std::vector<std::string> floatText =
      scalanative::nir::bodyToText(foldedFloat->body);
  if (int code =
          expect(floatText.size() == 3 && floatText[1] == "param %value : Float" &&
                     floatText[2] == "ret Float %value",
                 "interflow did not collapse unary Float identity")) {
    return code;
  }

  const std::vector<std::string> doubleText =
      scalanative::nir::bodyToText(foldedDouble->body);
  if (int code =
          expect(doubleText.size() == 3 && doubleText[1] == "param %value : Double" &&
                     doubleText[2] == "ret Double %value",
                 "interflow did not collapse unary Double identity")) {
    return code;
  }

  const std::vector<std::string> booleanText =
      scalanative::nir::bodyToText(foldedBoolean->body);
  return expect(booleanText.size() == 3 && booleanText[1] == "param %flag : Boolean" &&
                    booleanText[2] == "ret Boolean %flag",
                "interflow did not collapse double Boolean negation");
}

int smokeInterflowFloatingUnaryLiteralFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  scalanative::nir::FunctionBodyBuilder negativeFloatBody;
  (void)negativeFloatBody.addReturn(
      "Float",
      scalanative::nir::unaryValue(
          "-", scalanative::nir::literalValue("1.25F", "Float", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder positiveDoubleBody;
  (void)positiveDoubleBody.addReturn(
      "Double",
      scalanative::nir::unaryValue(
          "+", scalanative::nir::literalValue("2.5", "Double", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder negativeZeroBody;
  (void)negativeZeroBody.addReturn(
      "Double",
      scalanative::nir::unaryValue(
          "-", scalanative::nir::literalValue("0.0", "Double", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.FloatingUnary.negativeFloat", "()Float",
                                std::move(negativeFloatBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.FloatingUnary.positiveDouble",
                                "()Double", std::move(positiveDoubleBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.FloatingUnary.negativeZero", "()Double",
                                std::move(negativeZeroBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.FloatingUnary.negativeFloat");
  program.reachableGlobals.push_back("demo.interflow.FloatingUnary.positiveDouble");
  program.reachableGlobals.push_back("demo.interflow.FloatingUnary.negativeZero");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code = expect(result.ok,
                        "interflow rejected valid floating unary literal program")) {
    return code;
  }
  if (int code = expect(result.reports.size() == 5 &&
                            result.reports[1].name == "fold-constants" &&
                            result.reports[1].changedValues == 3 &&
                            result.reports[1].validationErrorsBefore == 0 &&
                            result.reports[1].validationErrorsAfter == 0,
                        "interflow did not report validation-clean floating unary "
                        "literal folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* negativeFloat =
      findDefinition(optimizedModule, "demo.interflow.FloatingUnary.negativeFloat");
  const scalanative::nir::Definition* positiveDouble =
      findDefinition(optimizedModule, "demo.interflow.FloatingUnary.positiveDouble");
  const scalanative::nir::Definition* negativeZero =
      findDefinition(optimizedModule, "demo.interflow.FloatingUnary.negativeZero");
  if (int code =
          expect(negativeFloat != nullptr && positiveDouble != nullptr &&
                     negativeZero != nullptr,
                 "interflow removed reachable floating unary literal functions")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(negativeFloat->body).back() ==
                            "ret Float -1.25F",
                        "interflow did not fold negative Float literal")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(positiveDouble->body).back() ==
                            "ret Double 2.5",
                        "interflow did not fold positive Double literal")) {
    return code;
  }
  return expect(scalanative::nir::bodyToText(negativeZero->body).back() ==
                    "ret Double -0.0",
                "interflow did not preserve negative floating zero literal");
}

int smokeInterflowIfCanonicalization() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  scalanative::nir::FunctionBodyBuilder truthyBody;
  (void)truthyBody.addParameter("flag", "Boolean", noSpan);
  (void)truthyBody.addReturn(
      "Boolean",
      scalanative::nir::ifValue(
          scalanative::nir::localValue("flag", noSpan),
          scalanative::nir::literalValue("true", "Boolean", noSpan),
          scalanative::nir::literalValue("false", "Boolean", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder negatedBody;
  (void)negatedBody.addParameter("flag", "Boolean", noSpan);
  (void)negatedBody.addReturn(
      "Boolean",
      scalanative::nir::ifValue(
          scalanative::nir::localValue("flag", noSpan),
          scalanative::nir::literalValue("false", "Boolean", noSpan),
          scalanative::nir::literalValue("true", "Boolean", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder negatedConditionBody;
  (void)negatedConditionBody.addParameter("flag", "Boolean", noSpan);
  (void)negatedConditionBody.addReturn(
      "Boolean",
      scalanative::nir::ifValue(
          scalanative::nir::unaryValue(
              "!", scalanative::nir::localValue("flag", noSpan), noSpan),
          scalanative::nir::literalValue("true", "Boolean", noSpan),
          scalanative::nir::literalValue("false", "Boolean", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder invertedNegatedConditionBody;
  (void)invertedNegatedConditionBody.addParameter("flag", "Boolean", noSpan);
  (void)invertedNegatedConditionBody.addReturn(
      "Boolean",
      scalanative::nir::ifValue(
          scalanative::nir::unaryValue(
              "!", scalanative::nir::localValue("flag", noSpan), noSpan),
          scalanative::nir::literalValue("false", "Boolean", noSpan),
          scalanative::nir::literalValue("true", "Boolean", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder conditionThenFalseBody;
  (void)conditionThenFalseBody.addParameter("flag", "Boolean", noSpan);
  (void)conditionThenFalseBody.addReturn(
      "Boolean",
      scalanative::nir::ifValue(
          scalanative::nir::localValue("flag", noSpan),
          scalanative::nir::localValue("flag", noSpan),
          scalanative::nir::literalValue("false", "Boolean", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder falseThenConditionBody;
  (void)falseThenConditionBody.addParameter("flag", "Boolean", noSpan);
  (void)falseThenConditionBody.addReturn(
      "Boolean",
      scalanative::nir::ifValue(
          scalanative::nir::localValue("flag", noSpan),
          scalanative::nir::literalValue("false", "Boolean", noSpan),
          scalanative::nir::localValue("flag", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder negatedThenTrueBody;
  (void)negatedThenTrueBody.addParameter("flag", "Boolean", noSpan);
  (void)negatedThenTrueBody.addReturn(
      "Boolean",
      scalanative::nir::ifValue(
          scalanative::nir::localValue("flag", noSpan),
          scalanative::nir::unaryValue(
              "!", scalanative::nir::localValue("flag", noSpan), noSpan),
          scalanative::nir::literalValue("true", "Boolean", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder trueThenNegatedBody;
  (void)trueThenNegatedBody.addParameter("flag", "Boolean", noSpan);
  (void)trueThenNegatedBody.addReturn(
      "Boolean",
      scalanative::nir::ifValue(
          scalanative::nir::localValue("flag", noSpan),
          scalanative::nir::literalValue("true", "Boolean", noSpan),
          scalanative::nir::unaryValue(
              "!", scalanative::nir::localValue("flag", noSpan), noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder negatedConditionLocalThenTrueBody;
  (void)negatedConditionLocalThenTrueBody.addParameter("flag", "Boolean", noSpan);
  (void)negatedConditionLocalThenTrueBody.addReturn(
      "Boolean",
      scalanative::nir::ifValue(
          scalanative::nir::unaryValue(
              "!", scalanative::nir::localValue("flag", noSpan), noSpan),
          scalanative::nir::localValue("flag", noSpan),
          scalanative::nir::literalValue("true", "Boolean", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder negatedConditionLocalThenFalseBody;
  (void)negatedConditionLocalThenFalseBody.addParameter("flag", "Boolean", noSpan);
  (void)negatedConditionLocalThenFalseBody.addReturn(
      "Boolean",
      scalanative::nir::ifValue(
          scalanative::nir::unaryValue(
              "!", scalanative::nir::localValue("flag", noSpan), noSpan),
          scalanative::nir::localValue("flag", noSpan),
          scalanative::nir::literalValue("false", "Boolean", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder negatedConditionTrueThenLocalBody;
  (void)negatedConditionTrueThenLocalBody.addParameter("flag", "Boolean", noSpan);
  (void)negatedConditionTrueThenLocalBody.addReturn(
      "Boolean",
      scalanative::nir::ifValue(
          scalanative::nir::unaryValue(
              "!", scalanative::nir::localValue("flag", noSpan), noSpan),
          scalanative::nir::literalValue("true", "Boolean", noSpan),
          scalanative::nir::localValue("flag", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder negatedConditionFalseThenLocalBody;
  (void)negatedConditionFalseThenLocalBody.addParameter("flag", "Boolean", noSpan);
  (void)negatedConditionFalseThenLocalBody.addReturn(
      "Boolean",
      scalanative::nir::ifValue(
          scalanative::nir::unaryValue(
              "!", scalanative::nir::localValue("flag", noSpan), noSpan),
          scalanative::nir::literalValue("false", "Boolean", noSpan),
          scalanative::nir::localValue("flag", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder sameBody;
  (void)sameBody.addParameter("flag", "Boolean", noSpan);
  (void)sameBody.addReturn(
      "Int",
      scalanative::nir::ifValue(scalanative::nir::localValue("flag", noSpan),
                                scalanative::nir::literalValue("7", "Int", noSpan),
                                scalanative::nir::literalValue("7", "Int", noSpan),
                                noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder whenTrueAndBody;
  (void)whenTrueAndBody.addParameter("flag", "Boolean", noSpan);
  (void)whenTrueAndBody.addParameter("other", "Boolean", noSpan);
  (void)whenTrueAndBody.addReturn(
      "Boolean",
      scalanative::nir::ifValue(
          scalanative::nir::localValue("flag", noSpan),
          scalanative::nir::localValue("other", noSpan),
          scalanative::nir::literalValue("false", "Boolean", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder whenFalseAndBody;
  (void)whenFalseAndBody.addParameter("flag", "Boolean", noSpan);
  (void)whenFalseAndBody.addParameter("other", "Boolean", noSpan);
  (void)whenFalseAndBody.addReturn(
      "Boolean",
      scalanative::nir::ifValue(
          scalanative::nir::localValue("flag", noSpan),
          scalanative::nir::literalValue("false", "Boolean", noSpan),
          scalanative::nir::localValue("other", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder whenTrueOrBody;
  (void)whenTrueOrBody.addParameter("flag", "Boolean", noSpan);
  (void)whenTrueOrBody.addParameter("other", "Boolean", noSpan);
  (void)whenTrueOrBody.addReturn(
      "Boolean",
      scalanative::nir::ifValue(
          scalanative::nir::localValue("flag", noSpan),
          scalanative::nir::literalValue("true", "Boolean", noSpan),
          scalanative::nir::localValue("other", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder whenFalseOrBody;
  (void)whenFalseOrBody.addParameter("flag", "Boolean", noSpan);
  (void)whenFalseOrBody.addParameter("other", "Boolean", noSpan);
  (void)whenFalseOrBody.addReturn(
      "Boolean",
      scalanative::nir::ifValue(
          scalanative::nir::localValue("flag", noSpan),
          scalanative::nir::localValue("other", noSpan),
          scalanative::nir::literalValue("true", "Boolean", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder negatedConditionWhenFalseAndBody;
  (void)negatedConditionWhenFalseAndBody.addParameter("flag", "Boolean", noSpan);
  (void)negatedConditionWhenFalseAndBody.addParameter("other", "Boolean", noSpan);
  (void)negatedConditionWhenFalseAndBody.addReturn(
      "Boolean",
      scalanative::nir::ifValue(
          scalanative::nir::unaryValue(
              "!", scalanative::nir::localValue("flag", noSpan), noSpan),
          scalanative::nir::literalValue("false", "Boolean", noSpan),
          scalanative::nir::localValue("other", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.IfCanon.truthy", "(Boolean)Boolean",
                                std::move(truthyBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.IfCanon.negated", "(Boolean)Boolean",
                                std::move(negatedBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.IfCanon.negatedCondition",
                                "(Boolean)Boolean",
                                std::move(negatedConditionBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.IfCanon.invertedNegatedCondition", "(Boolean)Boolean",
       std::move(invertedNegatedConditionBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.IfCanon.conditionThenFalse",
                                "(Boolean)Boolean",
                                std::move(conditionThenFalseBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.IfCanon.falseThenCondition",
                                "(Boolean)Boolean",
                                std::move(falseThenConditionBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.IfCanon.negatedThenTrue",
                                "(Boolean)Boolean",
                                std::move(negatedThenTrueBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.IfCanon.trueThenNegated",
                                "(Boolean)Boolean",
                                std::move(trueThenNegatedBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.IfCanon.negatedConditionLocalThenTrue", "(Boolean)Boolean",
       std::move(negatedConditionLocalThenTrueBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.IfCanon.negatedConditionLocalThenFalse", "(Boolean)Boolean",
       std::move(negatedConditionLocalThenFalseBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.IfCanon.negatedConditionTrueThenLocal", "(Boolean)Boolean",
       std::move(negatedConditionTrueThenLocalBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.IfCanon.negatedConditionFalseThenLocal", "(Boolean)Boolean",
       std::move(negatedConditionFalseThenLocalBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.IfCanon.same", "(Boolean)Int",
                                std::move(sameBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.IfCanon.whenTrueAnd",
                                "(Boolean,Boolean)Boolean",
                                std::move(whenTrueAndBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.IfCanon.whenFalseAnd",
                                "(Boolean,Boolean)Boolean",
                                std::move(whenFalseAndBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.IfCanon.whenTrueOr",
                                "(Boolean,Boolean)Boolean",
                                std::move(whenTrueOrBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.IfCanon.whenFalseOr",
                                "(Boolean,Boolean)Boolean",
                                std::move(whenFalseOrBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.IfCanon.negatedConditionWhenFalseAnd",
                                "(Boolean,Boolean)Boolean",
                                std::move(negatedConditionWhenFalseAndBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.IfCanon.truthy");
  program.reachableGlobals.push_back("demo.interflow.IfCanon.negated");
  program.reachableGlobals.push_back("demo.interflow.IfCanon.negatedCondition");
  program.reachableGlobals.push_back("demo.interflow.IfCanon.invertedNegatedCondition");
  program.reachableGlobals.push_back("demo.interflow.IfCanon.conditionThenFalse");
  program.reachableGlobals.push_back("demo.interflow.IfCanon.falseThenCondition");
  program.reachableGlobals.push_back("demo.interflow.IfCanon.negatedThenTrue");
  program.reachableGlobals.push_back("demo.interflow.IfCanon.trueThenNegated");
  program.reachableGlobals.push_back(
      "demo.interflow.IfCanon.negatedConditionLocalThenTrue");
  program.reachableGlobals.push_back(
      "demo.interflow.IfCanon.negatedConditionLocalThenFalse");
  program.reachableGlobals.push_back(
      "demo.interflow.IfCanon.negatedConditionTrueThenLocal");
  program.reachableGlobals.push_back(
      "demo.interflow.IfCanon.negatedConditionFalseThenLocal");
  program.reachableGlobals.push_back("demo.interflow.IfCanon.same");
  program.reachableGlobals.push_back("demo.interflow.IfCanon.whenTrueAnd");
  program.reachableGlobals.push_back("demo.interflow.IfCanon.whenFalseAnd");
  program.reachableGlobals.push_back("demo.interflow.IfCanon.whenTrueOr");
  program.reachableGlobals.push_back("demo.interflow.IfCanon.whenFalseOr");
  program.reachableGlobals.push_back(
      "demo.interflow.IfCanon.negatedConditionWhenFalseAnd");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid if-canonicalization program")) {
    return code;
  }
  if (int code = expect(
          result.reports.size() == 5 && result.reports[1].name == "fold-constants" &&
              result.reports[1].changedValues == 18 &&
              result.reports[1].validationErrorsBefore == 0 &&
              result.reports[1].validationErrorsAfter == 0,
          "interflow did not report validation-clean if canonicalization")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* truthy =
      findDefinition(optimizedModule, "demo.interflow.IfCanon.truthy");
  const scalanative::nir::Definition* negated =
      findDefinition(optimizedModule, "demo.interflow.IfCanon.negated");
  const scalanative::nir::Definition* negatedCondition =
      findDefinition(optimizedModule, "demo.interflow.IfCanon.negatedCondition");
  const scalanative::nir::Definition* invertedNegatedCondition = findDefinition(
      optimizedModule, "demo.interflow.IfCanon.invertedNegatedCondition");
  const scalanative::nir::Definition* conditionThenFalse =
      findDefinition(optimizedModule, "demo.interflow.IfCanon.conditionThenFalse");
  const scalanative::nir::Definition* falseThenCondition =
      findDefinition(optimizedModule, "demo.interflow.IfCanon.falseThenCondition");
  const scalanative::nir::Definition* negatedThenTrue =
      findDefinition(optimizedModule, "demo.interflow.IfCanon.negatedThenTrue");
  const scalanative::nir::Definition* trueThenNegated =
      findDefinition(optimizedModule, "demo.interflow.IfCanon.trueThenNegated");
  const scalanative::nir::Definition* negatedConditionLocalThenTrue = findDefinition(
      optimizedModule, "demo.interflow.IfCanon.negatedConditionLocalThenTrue");
  const scalanative::nir::Definition* negatedConditionLocalThenFalse = findDefinition(
      optimizedModule, "demo.interflow.IfCanon.negatedConditionLocalThenFalse");
  const scalanative::nir::Definition* negatedConditionTrueThenLocal = findDefinition(
      optimizedModule, "demo.interflow.IfCanon.negatedConditionTrueThenLocal");
  const scalanative::nir::Definition* negatedConditionFalseThenLocal = findDefinition(
      optimizedModule, "demo.interflow.IfCanon.negatedConditionFalseThenLocal");
  const scalanative::nir::Definition* same =
      findDefinition(optimizedModule, "demo.interflow.IfCanon.same");
  const scalanative::nir::Definition* whenTrueAnd =
      findDefinition(optimizedModule, "demo.interflow.IfCanon.whenTrueAnd");
  const scalanative::nir::Definition* whenFalseAnd =
      findDefinition(optimizedModule, "demo.interflow.IfCanon.whenFalseAnd");
  const scalanative::nir::Definition* whenTrueOr =
      findDefinition(optimizedModule, "demo.interflow.IfCanon.whenTrueOr");
  const scalanative::nir::Definition* whenFalseOr =
      findDefinition(optimizedModule, "demo.interflow.IfCanon.whenFalseOr");
  const scalanative::nir::Definition* negatedConditionWhenFalseAnd = findDefinition(
      optimizedModule, "demo.interflow.IfCanon.negatedConditionWhenFalseAnd");
  if (int code = expect(
          truthy != nullptr && negated != nullptr && negatedCondition != nullptr &&
              invertedNegatedCondition != nullptr && conditionThenFalse != nullptr &&
              falseThenCondition != nullptr && negatedThenTrue != nullptr &&
              trueThenNegated != nullptr && negatedConditionLocalThenTrue != nullptr &&
              negatedConditionLocalThenFalse != nullptr &&
              negatedConditionTrueThenLocal != nullptr &&
              negatedConditionFalseThenLocal != nullptr && same != nullptr &&
              whenTrueAnd != nullptr && whenFalseAnd != nullptr &&
              whenTrueOr != nullptr && whenFalseOr != nullptr &&
              negatedConditionWhenFalseAnd != nullptr,
          "interflow removed reachable if-canonicalization functions")) {
    return code;
  }

  const std::vector<std::string> truthyText =
      scalanative::nir::bodyToText(truthy->body);
  if (int code =
          expect(truthyText.size() == 3 && truthyText[1] == "param %flag : Boolean" &&
                     truthyText[2] == "ret Boolean %flag",
                 "interflow did not collapse Boolean identity if")) {
    return code;
  }

  const std::vector<std::string> negatedText =
      scalanative::nir::bodyToText(negated->body);
  if (int code =
          expect(negatedText.size() == 3 && negatedText[1] == "param %flag : Boolean" &&
                     negatedText[2] == "ret Boolean (!%flag)",
                 "interflow did not collapse Boolean negating if")) {
    return code;
  }

  const std::vector<std::string> negatedConditionText =
      scalanative::nir::bodyToText(negatedCondition->body);
  if (int code = expect(negatedConditionText.size() == 3 &&
                            negatedConditionText[1] == "param %flag : Boolean" &&
                            negatedConditionText[2] == "ret Boolean (!%flag)",
                        "interflow did not preserve negated condition if")) {
    return code;
  }

  const std::vector<std::string> invertedNegatedConditionText =
      scalanative::nir::bodyToText(invertedNegatedCondition->body);
  if (int code =
          expect(invertedNegatedConditionText.size() == 3 &&
                     invertedNegatedConditionText[1] == "param %flag : Boolean" &&
                     invertedNegatedConditionText[2] == "ret Boolean %flag",
                 "interflow did not cancel inverted negated condition if")) {
    return code;
  }

  const std::vector<std::string> conditionThenFalseText =
      scalanative::nir::bodyToText(conditionThenFalse->body);
  if (int code = expect(conditionThenFalseText.size() == 3 &&
                            conditionThenFalseText[1] == "param %flag : Boolean" &&
                            conditionThenFalseText[2] == "ret Boolean %flag",
                        "interflow did not collapse condition/false if")) {
    return code;
  }

  const std::vector<std::string> falseThenConditionText =
      scalanative::nir::bodyToText(falseThenCondition->body);
  if (int code = expect(falseThenConditionText.size() == 3 &&
                            falseThenConditionText[1] == "param %flag : Boolean" &&
                            falseThenConditionText[2] == "ret Boolean false",
                        "interflow did not collapse false/condition if")) {
    return code;
  }

  const std::vector<std::string> negatedThenTrueText =
      scalanative::nir::bodyToText(negatedThenTrue->body);
  if (int code = expect(negatedThenTrueText.size() == 3 &&
                            negatedThenTrueText[1] == "param %flag : Boolean" &&
                            negatedThenTrueText[2] == "ret Boolean (!%flag)",
                        "interflow did not collapse negated/true if")) {
    return code;
  }

  const std::vector<std::string> trueThenNegatedText =
      scalanative::nir::bodyToText(trueThenNegated->body);
  if (int code = expect(trueThenNegatedText.size() == 3 &&
                            trueThenNegatedText[1] == "param %flag : Boolean" &&
                            trueThenNegatedText[2] == "ret Boolean true",
                        "interflow did not collapse true/negated if")) {
    return code;
  }

  const std::vector<std::string> negatedConditionLocalThenTrueText =
      scalanative::nir::bodyToText(negatedConditionLocalThenTrue->body);
  if (int code =
          expect(negatedConditionLocalThenTrueText.size() == 3 &&
                     negatedConditionLocalThenTrueText[1] == "param %flag : Boolean" &&
                     negatedConditionLocalThenTrueText[2] == "ret Boolean %flag",
                 "interflow did not collapse negated-condition local/true if")) {
    return code;
  }

  const std::vector<std::string> negatedConditionLocalThenFalseText =
      scalanative::nir::bodyToText(negatedConditionLocalThenFalse->body);
  if (int code =
          expect(negatedConditionLocalThenFalseText.size() == 3 &&
                     negatedConditionLocalThenFalseText[1] == "param %flag : Boolean" &&
                     negatedConditionLocalThenFalseText[2] == "ret Boolean false",
                 "interflow did not collapse negated-condition local/false if")) {
    return code;
  }

  const std::vector<std::string> negatedConditionTrueThenLocalText =
      scalanative::nir::bodyToText(negatedConditionTrueThenLocal->body);
  if (int code =
          expect(negatedConditionTrueThenLocalText.size() == 3 &&
                     negatedConditionTrueThenLocalText[1] == "param %flag : Boolean" &&
                     negatedConditionTrueThenLocalText[2] == "ret Boolean true",
                 "interflow did not collapse negated-condition true/local if")) {
    return code;
  }

  const std::vector<std::string> negatedConditionFalseThenLocalText =
      scalanative::nir::bodyToText(negatedConditionFalseThenLocal->body);
  if (int code =
          expect(negatedConditionFalseThenLocalText.size() == 3 &&
                     negatedConditionFalseThenLocalText[1] == "param %flag : Boolean" &&
                     negatedConditionFalseThenLocalText[2] == "ret Boolean %flag",
                 "interflow did not collapse negated-condition false/local if")) {
    return code;
  }

  const std::vector<std::string> sameText = scalanative::nir::bodyToText(same->body);
  if (int code =
          expect(sameText.size() == 3 && sameText[1] == "param %flag : Boolean" &&
                     sameText[2] == "ret Int 7",
                 "interflow did not collapse pure identical if branches")) {
    return code;
  }

  if (int code = expect(scalanative::nir::bodyToText(whenTrueAnd->body).back() ==
                            "ret Boolean (%flag && %other)",
                        "interflow did not lower true-branch Boolean if to and")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(whenFalseAnd->body).back() ==
                            "ret Boolean ((!%flag) && %other)",
                        "interflow did not lower false-branch Boolean if to and")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(whenTrueOr->body).back() ==
                            "ret Boolean (%flag || %other)",
                        "interflow did not lower true-branch Boolean if to or")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(whenFalseOr->body).back() ==
                            "ret Boolean ((!%flag) || %other)",
                        "interflow did not lower false-branch Boolean if to or")) {
    return code;
  }
  return expect(
      scalanative::nir::bodyToText(negatedConditionWhenFalseAnd->body).back() ==
          "ret Boolean (%flag && %other)",
      "interflow did not cancel negated condition while lowering Boolean if");
}

int smokeInterflowConstantFalseWhileFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  scalanative::nir::FunctionBodyBuilder loopBody;
  (void)loopBody.addVar("current", "Int",
                        scalanative::nir::literalValue("0", "Int", noSpan), noSpan);
  (void)loopBody.addReturn(
      "Unit",
      scalanative::nir::whileValue(
          scalanative::nir::literalValue("false", "Boolean", noSpan),
          scalanative::nir::assignValue(
              scalanative::nir::localValue("current", noSpan),
              scalanative::nir::literalValue("1", "Int", noSpan), noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.FalseWhile.main", "()Unit",
                                std::move(loopBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.FalseWhile.main");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid constant-false while program")) {
    return code;
  }
  if (int code = expect(
          result.reports.size() == 5 && result.reports[1].name == "fold-constants" &&
              result.reports[1].changedValues == 1 &&
              result.reports[1].validationErrorsBefore == 0 &&
              result.reports[1].validationErrorsAfter == 0 &&
              result.reports[2].name == "eliminate-dead-local-lets" &&
              result.reports[2].changedValues == 1 &&
              result.reports[2].validationErrorsBefore == 0 &&
              result.reports[2].validationErrorsAfter == 0,
          "interflow did not report validation-clean constant-false while folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* foldedMain =
      findDefinition(optimizedModule, "demo.interflow.FalseWhile.main");
  if (int code = expect(foldedMain != nullptr,
                        "interflow removed the reachable constant-false while "
                        "function")) {
    return code;
  }

  const std::vector<std::string> body = scalanative::nir::bodyToText(foldedMain->body);
  return expect(body.size() == 2 && body[1] == "ret Unit unit",
                "interflow did not collapse constant-false while body");
}

int smokeInterflowLiteralComparisonFold() {
  scalanative::nir::FunctionBodyBuilder userMainBody;
  (void)userMainBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "&&",
          scalanative::nir::binaryValue(
              "<",
              scalanative::nir::literalValue("1", "Int",
                                             scalanative::support::SourceSpan::none()),
              scalanative::nir::literalValue("2", "Int",
                                             scalanative::support::SourceSpan::none()),
              scalanative::support::SourceSpan::none()),
          scalanative::nir::binaryValue(
              "&&",
              scalanative::nir::binaryValue(
                  "!=",
                  scalanative::nir::literalValue(
                      "true", "Boolean", scalanative::support::SourceSpan::none()),
                  scalanative::nir::literalValue(
                      "false", "Boolean", scalanative::support::SourceSpan::none()),
                  scalanative::support::SourceSpan::none()),
              scalanative::nir::binaryValue(
                  "&&",
                  scalanative::nir::binaryValue(
                      "<",
                      scalanative::nir::literalValue(
                          "'\\n'", "Char", scalanative::support::SourceSpan::none()),
                      scalanative::nir::literalValue(
                          "'a'", "Char", scalanative::support::SourceSpan::none()),
                      scalanative::support::SourceSpan::none()),
                  scalanative::nir::binaryValue(
                      "&&",
                      scalanative::nir::binaryValue(
                          "==",
                          scalanative::nir::unitValue(
                              scalanative::support::SourceSpan::none()),
                          scalanative::nir::unitValue(
                              scalanative::support::SourceSpan::none()),
                          scalanative::support::SourceSpan::none()),
                      scalanative::nir::binaryValue(
                          "&&",
                          scalanative::nir::binaryValue(
                              "==",
                              scalanative::nir::literalValue(
                                  "null", "Null",
                                  scalanative::support::SourceSpan::none()),
                              scalanative::nir::literalValue(
                                  "null", "Null",
                                  scalanative::support::SourceSpan::none()),
                              scalanative::support::SourceSpan::none()),
                          scalanative::nir::binaryValue(
                              "&&",
                              scalanative::nir::binaryValue(
                                  "==",
                                  scalanative::nir::literalValue(
                                      "\"line\\n\"", "String",
                                      scalanative::support::SourceSpan::none()),
                                  scalanative::nir::literalValue(
                                      "\"\"\"line\n\"\"\"", "String",
                                      scalanative::support::SourceSpan::none()),
                                  scalanative::support::SourceSpan::none()),
                              scalanative::nir::binaryValue(
                                  "!=",
                                  scalanative::nir::literalValue(
                                      "'ready", "Symbol",
                                      scalanative::support::SourceSpan::none()),
                                  scalanative::nir::literalValue(
                                      "'waiting", "Symbol",
                                      scalanative::support::SourceSpan::none()),
                                  scalanative::support::SourceSpan::none()),
                              scalanative::support::SourceSpan::none()),
                          scalanative::support::SourceSpan::none()),
                      scalanative::support::SourceSpan::none()),
                  scalanative::support::SourceSpan::none()),
              scalanative::support::SourceSpan::none()),
          scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int",
      scalanative::nir::literalValue("0", "Int",
                                     scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Comparison.main", "()Boolean",
                                std::move(userMainBody).build(),
                                scalanative::support::SourceSpan::none()});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(),
                                scalanative::support::SourceSpan::none()});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.Comparison.main");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code = expect(result.ok,
                        "interflow rejected valid literal comparison fold program")) {
    return code;
  }
  if (int code = expect(result.changedValues == 1,
                        "interflow did not report exactly one comparison fold; saw " +
                            std::to_string(result.changedValues))) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* foldedMain =
      findDefinition(optimizedModule, "demo.interflow.Comparison.main");
  if (int code = expect(foldedMain != nullptr &&
                            scalanative::nir::bodyToText(foldedMain->body).back() ==
                                "ret Boolean true",
                        "interflow did not fold literal comparisons to true")) {
    return code;
  }
  const std::string body = scalanative::nir::bodyToText(foldedMain->body).back();
  return expect(!contains(body, "&&") && !contains(body, "<") &&
                    !contains(body, "!=") && !contains(body, "=="),
                "interflow kept literal comparison operators in optimized NIR");
}

int smokeInterflowStringLiteralConcatFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  scalanative::nir::FunctionBodyBuilder simpleBody;
  (void)simpleBody.addReturn(
      "String",
      scalanative::nir::binaryValue(
          "+", scalanative::nir::literalValue("\"hello \"", "String", noSpan),
          scalanative::nir::literalValue("\"world\"", "String", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder escapedBody;
  (void)escapedBody.addReturn(
      "String",
      scalanative::nir::binaryValue(
          "+", scalanative::nir::literalValue(R"("line\n")", "String", noSpan),
          scalanative::nir::literalValue(R"("\"quote\"")", "String", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.StringConcat.simple", "()String",
                                std::move(simpleBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.StringConcat.escaped", "()String",
                                std::move(escapedBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.StringConcat.simple");
  program.reachableGlobals.push_back("demo.interflow.StringConcat.escaped");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected valid string concat fold program")) {
    return code;
  }
  if (int code = expect(
          result.reports.size() == 5 && result.reports[1].name == "fold-constants" &&
              result.reports[1].changedValues == 2 &&
              result.reports[1].validationErrorsBefore == 0 &&
              result.reports[1].validationErrorsAfter == 0,
          "interflow did not report validation-clean string concat folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* simple =
      findDefinition(optimizedModule, "demo.interflow.StringConcat.simple");
  const scalanative::nir::Definition* escaped =
      findDefinition(optimizedModule, "demo.interflow.StringConcat.escaped");
  if (int code = expect(simple != nullptr && escaped != nullptr,
                        "interflow removed reachable string concat functions")) {
    return code;
  }

  const std::string simpleBodyText = scalanative::nir::bodyToText(simple->body).back();
  if (int code = expect(simpleBodyText == "ret String \"hello world\"",
                        "interflow did not fold simple literal string concat")) {
    return code;
  }

  const std::string escapedBodyText =
      scalanative::nir::bodyToText(escaped->body).back();
  return expect(escapedBodyText == R"(ret String "line\n\"quote\"")" &&
                    !contains(simpleBodyText, " + ") &&
                    !contains(escapedBodyText, " + "),
                "interflow did not fold and re-escape literal string concat");
}

int smokeInterflowFloatingLiteralComparisonFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  scalanative::nir::FunctionBodyBuilder floatBody;
  (void)floatBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "<=", scalanative::nir::literalValue("1.5F", "Float", noSpan),
          scalanative::nir::literalValue("1.5F", "Float", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder doubleBody;
  (void)doubleBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          ">", scalanative::nir::literalValue("2.0", "Double", noSpan),
          scalanative::nir::literalValue("4.0", "Double", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.FloatingComparison.floatValue",
                                "()Boolean", std::move(floatBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.FloatingComparison.doubleValue",
                                "()Boolean", std::move(doubleBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.FloatingComparison.floatValue");
  program.reachableGlobals.push_back("demo.interflow.FloatingComparison.doubleValue");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code = expect(
          result.ok, "interflow rejected valid floating literal comparison program")) {
    return code;
  }
  if (int code = expect(
          result.reports.size() == 5 && result.reports[1].name == "fold-constants" &&
              result.reports[1].changedValues == 2 &&
              result.reports[1].validationErrorsBefore == 0 &&
              result.reports[1].validationErrorsAfter == 0,
          "interflow did not report validation-clean floating comparison folding")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* foldedFloat =
      findDefinition(optimizedModule, "demo.interflow.FloatingComparison.floatValue");
  const scalanative::nir::Definition* foldedDouble =
      findDefinition(optimizedModule, "demo.interflow.FloatingComparison.doubleValue");
  if (int code = expect(foldedFloat != nullptr && foldedDouble != nullptr,
                        "interflow removed reachable floating comparison functions")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(foldedFloat->body).back() ==
                            "ret Boolean true",
                        "interflow did not fold Float literal comparison")) {
    return code;
  }
  return expect(scalanative::nir::bodyToText(foldedDouble->body).back() ==
                    "ret Boolean false",
                "interflow did not fold Double literal comparison");
}

} // namespace

int main() {
  if (int code = smokeBuildPipeline()) {
    return code;
  }
  if (int code = smokeMainEntrypointMvp()) {
    return code;
  }
  if (int code = smokeUncaughtThrowMvp()) {
    return code;
  }
  if (int code = smokeStringArrayLiteralMvp()) {
    return code;
  }
  if (int code = smokeArrayParametersMvp()) {
    return code;
  }
  if (int code = smokeBooleanArrayMvp()) {
    return code;
  }
  if (int code = smokeLongArrayMvp()) {
    return code;
  }
  if (int code = smokeClassArrayMvp()) {
    return code;
  }
  if (int code = smokeTraitArrayMvp()) {
    return code;
  }
  if (int code = smokeDoubleArrayMvp()) {
    return code;
  }
  if (int code = smokeFloatArrayMvp()) {
    return code;
  }
  if (int code = smokeCharArrayMvp()) {
    return code;
  }
  if (int code = smokeAnyArrayMvp()) {
    return code;
  }
  if (int code = smokeInferredMixedArrayMvp()) {
    return code;
  }
  if (int code = smokeAnyParametersMvp()) {
    return code;
  }
  if (int code = smokeLocalAnyMvp()) {
    return code;
  }
  if (int code = smokeAnyFieldsMvp()) {
    return code;
  }
  if (int code = smokeAnyConstructorParametersMvp()) {
    return code;
  }
  if (int code = smokeAnyUnitSymbolMvp()) {
    return code;
  }
  if (int code = smokeBuildDriverActions()) {
    return code;
  }
  if (int code = smokeLlvmCodegenSubset()) {
    return code;
  }
  if (int code = smokeModuleSingletonsMvp()) {
    return code;
  }
  if (int code = smokeSingletonPatternMvp()) {
    return code;
  }
  if (int code = smokeHybridMemoryRuntimeMvp()) {
    return code;
  }
  if (int code = smokeGcTelemetryMvp()) {
    return code;
  }
  if (int code = smokeAutomaticGcMvp()) {
    return code;
  }
  if (int code = smokeScopedZoneRuntimeMvp()) {
    return code;
  }
  if (int code = smokeClassInstanceMvp()) {
    return code;
  }
  if (int code = smokeClassFieldMvp()) {
    return code;
  }
  if (int code = smokeMutableFieldMvp()) {
    return code;
  }
  if (int code = smokeClassParamFieldModifiersMvp()) {
    return code;
  }
  if (int code = smokeInheritanceMetadataMvp()) {
    return code;
  }
  if (int code = smokeRuntimeTypeTestsMvp()) {
    return code;
  }
  if (int code = smokeShortCircuitMvp()) {
    return code;
  }
  if (int code = smokeRemainderMvp()) {
    return code;
  }
  if (int code = smokeUnaryOperatorsMvp()) {
    return code;
  }
  if (int code = smokeStringConcatenationMvp()) {
    return code;
  }
  if (int code = smokeStringEqualityMvp()) {
    return code;
  }
  if (int code = smokeCompilerKnownEqualsMvp()) {
    return code;
  }
  if (int code = smokeAnyEqualsMvp()) {
    return code;
  }
  if (int code = smokeAnyStringMvp()) {
    return code;
  }
  if (int code = smokeUnitEqualsHashCodeMvp()) {
    return code;
  }
  if (int code = smokeUnitOperatorEqualityMvp()) {
    return code;
  }
  if (int code = smokeCompilerKnownHashCodeMvp()) {
    return code;
  }
  if (int code = smokeStringLengthMvp()) {
    return code;
  }
  if (int code = smokePrimitiveToStringMvp()) {
    return code;
  }
  if (int code = smokeAnyToStringMvp()) {
    return code;
  }
  if (int code = smokeCustomToStringMvp()) {
    return code;
  }
  if (int code = smokeFormattedInterpolationMvp()) {
    return code;
  }
  if (int code = smokeStringInterpolationMvp()) {
    return code;
  }
  if (int code = smokeConditionalMvp()) {
    return code;
  }
  if (int code = smokeMatchMvp()) {
    return code;
  }
  if (int code = smokeControlConditionValidation()) {
    return code;
  }
  if (int code = smokeWhileMvp()) {
    return code;
  }
  if (int code = smokeComparisonMvp()) {
    return code;
  }
  if (int code = smokeFloatingArithmeticMvp()) {
    return code;
  }
  if (int code = smokeSizeOfMvp()) {
    return code;
  }
  if (int code = smokeInheritedMethodMvp()) {
    return code;
  }
  if (int code = smokeInheritedFieldLayoutMvp()) {
    return code;
  }
  if (int code = smokeParentConstructorArgsMvp()) {
    return code;
  }
  if (int code = smokeSuperCallsMvp()) {
    return code;
  }
  if (int code = smokeStaticOverrideMvp()) {
    return code;
  }
  if (int code = smokeVirtualDispatchMvp()) {
    return code;
  }
  if (int code = smokeTraitDispatchMvp()) {
    return code;
  }
  if (int code = smokeAbstractTraitMvp()) {
    return code;
  }
  if (int code = smokeAbstractTypeMembersMvp()) {
    return code;
  }
  if (int code = smokeAbstractTypeSignaturesMvp()) {
    return code;
  }
  if (int code = smokeBoxedDependentScalarsMvp()) {
    return code;
  }
  if (int code = smokeBoundedTypeMembersMvp()) {
    return code;
  }
  if (int code = smokeIntervalTypeMembersMvp()) {
    return code;
  }
  if (int code = smokePathDependentTypesMvp()) {
    return code;
  }
  if (int code = smokeTypeProjectionsMvp()) {
    return code;
  }
  if (int code = smokeAbstractDependentReferenceMvp()) {
    return code;
  }
  if (int code = smokeAbstractTraitValMvp()) {
    return code;
  }
  if (int code = smokeAbstractTraitConstructorValMvp()) {
    return code;
  }
  if (int code = smokeInitializedTraitValsMvp()) {
    return code;
  }
  if (int code = smokeShadowedTraitValsMvp()) {
    return code;
  }
  if (int code = smokeTraitVarsMvp()) {
    return code;
  }
  if (int code = smokeMixedTraitAccessorsMvp()) {
    return code;
  }
  if (int code = smokeShadowedTraitVarsMvp()) {
    return code;
  }
  if (int code = smokeTraitHierarchyMvp()) {
    return code;
  }
  if (int code = smokeTraitCompositionMvp()) {
    return code;
  }
  if (int code = smokeQualifiedSuperMvp()) {
    return code;
  }
  if (int code = smokeTraitSuperChainMvp()) {
    return code;
  }
  if (int code = smokeTransitiveSuperLookupMvp()) {
    return code;
  }
  if (int code = smokeDiamondLinearizationMvp()) {
    return code;
  }
  if (int code = smokeStackableTraitSuperMvp()) {
    return code;
  }
  if (int code = smokeLocalTypedDispatchMvp()) {
    return code;
  }
  if (int code = smokeLocalVarMvp()) {
    return code;
  }
  if (int code = smokeConstructorBodyMvp()) {
    return code;
  }
  if (int code = smokeImportMvp()) {
    return code;
  }
  if (int code = smokeSourceNormalization()) {
    return code;
  }
  if (int code = smokeUtf8Validation()) {
    return code;
  }
  if (int code = smokeDiagnosticFixIts()) {
    return code;
  }
  if (int code = smokeLexerScalaTokens()) {
    return code;
  }
  if (int code = smokeLexerTriviaAndInterpolation()) {
    return code;
  }
  if (int code = smokeLexerLiteralDiagnostics()) {
    return code;
  }
  if (int code = smokeParserMinimalAst()) {
    return code;
  }
  if (int code = smokeAstValidationAndTypecheck()) {
    return code;
  }
  if (int code = smokeSemanticDiagnostics()) {
    return code;
  }
  if (int code = smokeNirVerifier()) {
    return code;
  }
  if (int code = smokeLinkerReachability()) {
    return code;
  }
  if (int code = smokeInterflowOptimizer()) {
    return code;
  }
  if (int code = smokeInterflowPureCallInlining()) {
    return code;
  }
  if (int code = smokeInterflowDeadLocalLetElimination()) {
    return code;
  }
  if (int code = smokeInterflowNestedDeadLocalLetElimination()) {
    return code;
  }
  if (int code = smokeInterflowDeadLocalVarElimination()) {
    return code;
  }
  if (int code = smokeInterflowLocalVarBlockShellSimplification()) {
    return code;
  }
  if (int code = smokeInterflowSingleUsePureLetBlockInlining()) {
    return code;
  }
  if (int code = smokeInterflowNestedPureBlockSimplification()) {
    return code;
  }
  if (int code = smokeInterflowDeadPureDiscardElimination()) {
    return code;
  }
  if (int code = smokeInterflowDeadTypeTestElimination()) {
    return code;
  }
  if (int code = smokeInterflowNestedLocalConstantPropagation()) {
    return code;
  }
  if (int code = smokeInterflowImmutableAliasPropagation()) {
    return code;
  }
  if (int code = smokeInterflowLiteralExpressionPropagation()) {
    return code;
  }
  if (int code = smokeInterflowLiteralIfPropagation()) {
    return code;
  }
  if (int code = smokeInterflowSizeOfPropagation()) {
    return code;
  }
  if (int code = smokeInterflowBoxUnboxFold()) {
    return code;
  }
  if (int code = smokeInterflowExactBoxedAnyEqualsFold()) {
    return code;
  }
  if (int code = smokeInterflowExactRuntimeHashCodeFold()) {
    return code;
  }
  if (int code = smokeInterflowExactRuntimeStringLengthFold()) {
    return code;
  }
  if (int code = smokeInterflowExactRuntimeArrayLengthFold()) {
    return code;
  }
  if (int code = smokeInterflowExactRuntimeArrayApplyFold()) {
    return code;
  }
  if (int code = smokeInterflowExactRuntimeToStringFold()) {
    return code;
  }
  if (int code = smokeInterflowExactRuntimeFormatFold()) {
    return code;
  }
  if (int code = smokeInterflowExactNullComparisonFold()) {
    return code;
  }
  if (int code = smokeInterflowSameTypeCastFold()) {
    return code;
  }
  if (int code = smokeInterflowNullTypeTestFold()) {
    return code;
  }
  if (int code = smokeInterflowNullDerivedAliasPropagation()) {
    return code;
  }
  if (int code = smokeInterflowExactNonNullTypeTestFold()) {
    return code;
  }
  if (int code = smokeInterflowAlgebraicIdentityFold()) {
    return code;
  }
  if (int code = smokeInterflowIntegerNegationIdentityFold()) {
    return code;
  }
  if (int code = smokeInterflowAbsorbingIdentityFold()) {
    return code;
  }
  if (int code = smokeInterflowLongFold()) {
    return code;
  }
  if (int code = smokeInterflowSameLocalComparisonFold()) {
    return code;
  }
  if (int code = smokeInterflowSameLocalArithmeticFold()) {
    return code;
  }
  if (int code = smokeInterflowSameLocalBooleanFold()) {
    return code;
  }
  if (int code = smokeInterflowBooleanAbsorptionFold()) {
    return code;
  }
  if (int code = smokeInterflowPureStructuredOperandFold()) {
    return code;
  }
  if (int code = smokeInterflowBooleanComparisonCanonicalization()) {
    return code;
  }
  if (int code = smokeInterflowBooleanDeMorganCanonicalization()) {
    return code;
  }
  if (int code = smokeInterflowUnaryIdentityFold()) {
    return code;
  }
  if (int code = smokeInterflowFloatingUnaryLiteralFold()) {
    return code;
  }
  if (int code = smokeInterflowIfCanonicalization()) {
    return code;
  }
  if (int code = smokeInterflowConstantFalseWhileFold()) {
    return code;
  }
  if (int code = smokeInterflowLiteralComparisonFold()) {
    return code;
  }
  if (int code = smokeInterflowStringLiteralConcatFold()) {
    return code;
  }
  if (int code = smokeInterflowFloatingLiteralComparisonFold()) {
    return code;
  }
  return 0;
}
