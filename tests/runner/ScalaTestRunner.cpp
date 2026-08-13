#include "scalanative/support/Diagnostics.h"
#include "scalanative/testing/TestResources.h"
#include "scalanative/tools/build/BuildDriver.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Arguments {
  std::string kind;
  std::string name;
  std::filesystem::path source;
};

std::optional<Arguments> parseArguments(int argc, char** argv) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option = argv[index];
    if (index + 1 >= argc) {
      return std::nullopt;
    }
    const std::string value = argv[++index];
    if (option == "--kind") {
      arguments.kind = value;
    } else if (option == "--name") {
      arguments.name = value;
    } else if (option == "--source") {
      arguments.source = value;
    } else {
      return std::nullopt;
    }
  }
  if (arguments.kind.empty() || arguments.name.empty() || arguments.source.empty()) {
    return std::nullopt;
  }
  return arguments;
}

std::vector<std::string> directives(std::string_view source,
                                    std::string_view directive) {
  const std::string prefix = "// " + std::string(directive) + ":";
  std::vector<std::string> values;
  std::istringstream lines{std::string(source)};
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const std::size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos ||
        !std::string_view(line).substr(first).starts_with(prefix)) {
      continue;
    }
    std::string value = line.substr(first + prefix.size());
    if (!value.empty() && value.front() == ' ') {
      value.erase(value.begin());
    }
    values.push_back(std::move(value));
  }
  return values;
}

std::string joinedOutput(const std::vector<std::string>& lines) {
  std::string output;
  for (const std::string& line : lines) {
    output += line;
    output += '\n';
  }
  return output;
}

std::string shellQuote(std::string_view value) {
  std::string quoted{"'"};
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted += character;
    }
  }
  quoted += '\'';
  return quoted;
}

std::string artifactStem(std::string_view name) {
  std::string stem;
  stem.reserve(name.size());
  for (const char character : name) {
    stem += (character >= 'a' && character <= 'z') ||
                    (character >= 'A' && character <= 'Z') ||
                    (character >= '0' && character <= '9')
                ? character
                : '-';
  }
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  return "scalanative-test-" + stem + "-" + std::to_string(nonce);
}

void removeRunArtifacts(const std::filesystem::path& binary,
                        const std::filesystem::path& output) {
  std::error_code ignored;
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);
  std::filesystem::path llvm = binary;
  llvm += ".ll";
  std::filesystem::remove(llvm, ignored);
}

int runValidTest(const Arguments& arguments) {
  scalanative::tools::build::BuildOptions options;
  options.action = scalanative::tools::build::BuildAction::EmitNir;
  scalanative::support::DiagnosticEngine diagnostics;
  const scalanative::tools::build::BuildResult result =
      scalanative::tools::build::BuildDriver{}.buildFile(arguments.source, options,
                                                         diagnostics);
  if (!result.ok) {
    return scalanative::testing::fail(
        arguments.name, "expected compilation to succeed:\n" + result.diagnosticsText);
  }
  return 0;
}

int runInvalidTest(const Arguments& arguments, std::string_view source) {
  const std::vector<std::string> expectedErrors = directives(source, "expected-error");
  if (expectedErrors.empty()) {
    return scalanative::testing::fail(
        arguments.name, "invalid test must declare at least one // expected-error:");
  }

  scalanative::support::DiagnosticEngine diagnostics;
  const scalanative::tools::build::BuildResult result =
      scalanative::tools::build::BuildDriver{}.buildFile(arguments.source, {},
                                                         diagnostics);
  if (result.ok) {
    return scalanative::testing::fail(arguments.name, "expected compilation to fail");
  }
  for (const std::string& expected : expectedErrors) {
    if (!scalanative::testing::contains(result.diagnosticsText, expected)) {
      return scalanative::testing::fail(arguments.name,
                                        "missing expected diagnostic '" + expected +
                                            "':\n" + result.diagnosticsText);
    }
  }
  return 0;
}

int runNativeTest(const Arguments& arguments, std::string_view source) {
  const std::vector<std::string> expectedLines = directives(source, "expected-output");
  if (expectedLines.empty()) {
    return scalanative::testing::fail(
        arguments.name, "run test must declare at least one // expected-output:");
  }

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary = temporary / artifactStem(arguments.name);
  std::filesystem::path output = binary;
  output += ".out";
  removeRunArtifacts(binary, output);

  scalanative::tools::build::BuildOptions options;
  options.action = scalanative::tools::build::BuildAction::BuildBinary;
  options.optimize = true;
  options.outputPath = binary;
  scalanative::support::DiagnosticEngine diagnostics;
  const scalanative::tools::build::BuildResult result =
      scalanative::tools::build::BuildDriver{}.buildFile(arguments.source, options,
                                                         diagnostics);
  if (!result.ok) {
    removeRunArtifacts(binary, output);
    if (scalanative::testing::contains(result.diagnosticsText,
                                       "clang toolchain not found")) {
      return 77;
    }
    return scalanative::testing::fail(arguments.name, "native compilation failed:\n" +
                                                          result.diagnosticsText);
  }

  const std::string command =
      shellQuote(binary.string()) + " > " + shellQuote(output.string()) + " 2>&1";
  const int status = std::system(command.c_str());
  std::ifstream outputFile(output, std::ios::binary);
  std::ostringstream actualContents;
  actualContents << outputFile.rdbuf();
  const std::string actual = actualContents.str();
  const std::string expected = joinedOutput(expectedLines);
  removeRunArtifacts(binary, output);

  if (status != 0) {
    return scalanative::testing::fail(arguments.name,
                                      "native program exited with status " +
                                          std::to_string(status) + ":\n" + actual);
  }
  if (actual != expected) {
    return scalanative::testing::fail(arguments.name, "expected output:\n" + expected +
                                                          "actual output:\n" + actual);
  }
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  const std::optional<Arguments> arguments = parseArguments(argc, argv);
  if (!arguments.has_value()) {
    return scalanative::testing::fail(
        "scala-test-runner",
        "usage: --kind <val|inval|run> --name <name> --source <file.scala>");
  }
  if (!std::filesystem::is_regular_file(arguments->source)) {
    return scalanative::testing::fail(arguments->name, "source does not exist: " +
                                                           arguments->source.string());
  }

  const std::string source = scalanative::testing::readTextFile(arguments->source);
  if (arguments->kind == "val") {
    return runValidTest(*arguments);
  }
  if (arguments->kind == "inval") {
    return runInvalidTest(*arguments, source);
  }
  if (arguments->kind == "run") {
    return runNativeTest(*arguments, source);
  }
  return scalanative::testing::fail(arguments->name,
                                    "unsupported test kind: " + arguments->kind);
}
