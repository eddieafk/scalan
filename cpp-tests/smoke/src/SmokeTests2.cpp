#include "scalanative/nir/Builder.h"
#include "scalanative/nir/Verifier.h"
#include "scalanative/support/Diagnostics.h"
#include "scalanative/support/SourceSpan.h"
#include "scalanative/support/StdNames.h"
#include "scalanative/tools/build/BuildCache.h"
#include "scalanative/tools/build/BuildConfig.h"
#include "scalanative/tools/build/BuildDriver.h"
#include "scalanative/tools/build/BuildReport.h"
#include "scalanative/tools/codegen/LlvmCodegen.h"
#include "scalanative/tools/interflow/InterflowOptimizer.h"
#include "scalanative/tools/linker/Linker.h"

#include <algorithm>
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

std::size_t countOccurrences(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) {
    return 0;
  }

  std::size_t count = 0;
  std::size_t offset = 0;
  while ((offset = haystack.find(needle, offset)) != std::string_view::npos) {
    ++count;
    offset += needle.size();
  }
  return count;
}

std::string definitionText(std::string_view nirText, std::string_view name) {
  const std::string header = "define @" + std::string(name);
  const std::size_t start = nirText.find(header);
  if (start == std::string_view::npos) {
    return {};
  }
  std::size_t end = nirText.size();
  auto consider = [&](std::string_view marker) {
    const std::size_t found = nirText.find(marker, start + 1);
    if (found != std::string_view::npos && found < end) {
      end = found;
    }
  };
  consider("\nclass @");
  consider("\nfield @");
  consider("\nmodule @");
  consider("\ndefine @");
  return std::string(nirText.substr(start, end - start));
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

const scalanative::tools::interflow::PassReport*
findPassReport(const scalanative::tools::interflow::InterflowResult& result,
               std::string_view name) {
  for (const scalanative::tools::interflow::PassReport& report : result.reports) {
    if (report.name == name) {
      return &report;
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

int smokeBuildReportJson() {
  constexpr const char* source = R"(package demo.report

object Main {
  def main = println(42)
}
)";
  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path cacheDirectory =
      temporary / "cpp-scalanative-smoke-build-report-cache";
  const std::filesystem::path outputPath =
      temporary / "cpp-scalanative-smoke-build-report.nir";
  std::error_code ignored;
  std::filesystem::remove_all(cacheDirectory, ignored);
  std::filesystem::remove(outputPath, ignored);

  scalanative::tools::build::BuildOptions options;
  options.action = scalanative::tools::build::BuildAction::EmitNir;
  options.cacheDirectory = cacheDirectory;
  options.outputPath = outputPath;
  options.runtimeLibraries = {"runtime\"quoted\nline"};
  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  const scalanative::tools::build::BuildResult success =
      driver.buildSource("BuildReport.scala", source, options, diagnostics);
  const std::string successReport =
      scalanative::tools::build::buildReportJson(success, options);

  constexpr const char* invalidSource = R"(object Broken {
  def value: Int = "wrong"
}
)";
  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult failure =
      driver.buildSource("BrokenReport.scala", invalidSource, {}, invalidDiagnostics);
  const std::string failureReport =
      scalanative::tools::build::buildReportJson(failure, {});

  const bool valid =
      success.ok && !success.cacheHit && !success.cacheKey.empty() &&
      success.diagnostics.empty() && contains(successReport, "\"schemaVersion\": 1") &&
      contains(successReport, "\"ok\": true") &&
      contains(successReport, "\"action\": \"emit-nir\"") &&
      contains(successReport, "\"sourcePath\": \"BuildReport.scala\"") &&
      contains(successReport, "\"frontend\": {\"applicable\": true, \"enabled\": true, "
                              "\"hit\": false") &&
      contains(successReport, "runtime\\\"quoted\\nline") &&
      contains(successReport, outputPath.string()) && !failure.ok &&
      !failure.diagnostics.empty() && failure.diagnostics.front().range.valid &&
      failure.diagnostics.front().range.path == "BrokenReport.scala" &&
      failure.diagnostics.front().range.startLine != 0 &&
      contains(failureReport, "\"ok\": false") &&
      contains(failureReport, "\"severity\": \"error\"") &&
      contains(failureReport, "\"path\": \"BrokenReport.scala\"") &&
      contains(failureReport, "\"diagnosticCounts\": {\"errors\": ");

  std::filesystem::remove_all(cacheDirectory, ignored);
  std::filesystem::remove(outputPath, ignored);
  return expect(valid, "machine-readable build report was incomplete or invalid");
}

int smokeBuildConfigurationJson() {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "cpp-scalanative-smoke-config";
  const std::filesystem::path configPath = directory / "project.json";
  const std::filesystem::path unknownPath = directory / "unknown.json";
  const std::filesystem::path malformedPath = directory / "malformed.json";
  const std::filesystem::path duplicatePath = directory / "duplicate.json";
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  std::filesystem::create_directories(directory, ignored);
  if (ignored) {
    return fail("could not create build configuration smoke directory");
  }

  {
    std::ofstream config(configPath, std::ios::binary);
    config << R"({
  "schemaVersion": 1,
  "source": "src/Main.scala",
  "action": "build-binary",
  "output": "out/Main",
  "optimizationLevel": 3,
  "debugInfo": false,
  "target": "x86_64-unknown-linux-gnu",
  "sysroot": "sdk",
  "gc": "hybrid\u002dmode",
  "cacheDirectory": ".cache",
  "runtimeLibraries": ["lib/runtime.a"],
  "linkLibraries": ["m", "lib/native.a", "-pthread"],
  "linkMode": "static",
  "linker": "lld",
  "optimizationReport": "out/optimization.json",
  "buildReport": "out/build.json"
})";
  }
  {
    std::ofstream unknown(unknownPath, std::ios::binary);
    unknown << R"({"schemaVersion":1,"optimisationLevel":2})";
  }
  {
    std::ofstream malformed(malformedPath, std::ios::binary);
    malformed << "{\n  \"schemaVersion\": 1,\n  \"action\": ]\n}\n";
  }
  {
    std::ofstream duplicate(duplicatePath, std::ios::binary);
    duplicate << R"({"schemaVersion":1,"schemaVersion":1})";
  }

  const scalanative::tools::build::BuildConfigLoadResult loaded =
      scalanative::tools::build::loadBuildConfiguration(configPath);
  const scalanative::tools::build::BuildConfigLoadResult unknown =
      scalanative::tools::build::loadBuildConfiguration(unknownPath);
  const scalanative::tools::build::BuildConfigLoadResult malformed =
      scalanative::tools::build::loadBuildConfiguration(malformedPath);
  const scalanative::tools::build::BuildConfigLoadResult duplicate =
      scalanative::tools::build::loadBuildConfiguration(duplicatePath);
  const std::filesystem::path base =
      std::filesystem::weakly_canonical(directory, ignored);

  bool valid = loaded.configuration.has_value();
  if (valid) {
    const scalanative::tools::build::BuildConfiguration& configuration =
        *loaded.configuration;
    const scalanative::tools::build::BuildOptions& options = configuration.options;
    valid =
        options.configurationPath == base / "project.json" &&
        configuration.sourcePath == base / "src/Main.scala" &&
        options.action == scalanative::tools::build::BuildAction::BuildBinary &&
        options.optimizationLevel == scalanative::tools::build::OptimizationLevel::O3 &&
        options.optimize && !options.debugInfo &&
        options.targetTriple == "x86_64-unknown-linux-gnu" &&
        options.sysroot == base / "sdk" && options.gcMode == "hybrid-mode" &&
        options.cacheDirectory == base / ".cache" &&
        options.outputPath == base / "out/Main" &&
        options.optimizationReportPath == base / "out/optimization.json" &&
        configuration.buildReportPath == base / "out/build.json" &&
        options.linkMode == scalanative::tools::build::LinkMode::Static &&
        options.linkerMode == scalanative::tools::build::LinkerMode::Lld &&
        options.runtimeLibraries ==
            std::vector<std::string>{(base / "lib/runtime.a").string()} &&
        options.linkLibraries ==
            std::vector<std::string>{"m", (base / "lib/native.a").string(), "-pthread"};
  }
  valid = valid && !unknown.configuration.has_value() &&
          contains(unknown.error, "unknown configuration key 'optimisationLevel'") &&
          !malformed.configuration.has_value() &&
          contains(malformed.error, "malformed.json:3:") &&
          !duplicate.configuration.has_value() &&
          contains(duplicate.error, "duplicate JSON object key 'schemaVersion'");

  std::filesystem::remove_all(directory, ignored);
  return expect(valid, "persistent build configuration parsing was incorrect");
}

int smokeBuildDriverEmitNir() {
  constexpr const char* source = R"(package demo.fast

object Main {
  def main = 0
}
)";

  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildOptions options;
  options.action = scalanative::tools::build::BuildAction::EmitNir;

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("FastSmoke.scala", source, options, diagnostics);

  if (int code = expect(result.ok, "fast build-driver smoke did not succeed")) {
    return code;
  }
  return expect(
      contains(result.nirText, "define @demo.fast.Main.main : ()Int") &&
          contains(result.nirText, "ret Int 0") &&
          contains(result.nirText, "define @scala.scalanative.runtime.main : ()Int"),
      "fast build-driver smoke did not emit expected NIR");
}

int smokeBuildDriverUsesDistinctOptimizationLevels() {
  constexpr const char* source = R"(package demo.levels

object Main {
  def main = {
    val one = 1
    val two = one + 1
    two
  }
}
)";

  scalanative::tools::build::BuildDriver driver;
  auto build = [&](scalanative::tools::build::OptimizationLevel level) {
    scalanative::tools::build::BuildOptions options;
    options.action = scalanative::tools::build::BuildAction::EmitNir;
    options.optimizationLevel = level;
    scalanative::support::DiagnosticEngine diagnostics;
    return driver.buildSource("OptimizationLevels.scala", source, options, diagnostics);
  };

  const scalanative::tools::build::BuildResult basic =
      build(scalanative::tools::build::OptimizationLevel::O1);
  const scalanative::tools::build::BuildResult standard =
      build(scalanative::tools::build::OptimizationLevel::O2);
  const scalanative::tools::build::BuildResult aggressive =
      build(scalanative::tools::build::OptimizationLevel::O3);
  if (int code = expect(basic.ok && standard.ok && aggressive.ok,
                        "optimization-level builds did not succeed")) {
    return code;
  }

  auto phaseContains = [](const scalanative::tools::build::BuildResult& result,
                          std::string_view text) {
    return std::any_of(result.phaseLog.begin(), result.phaseLog.end(),
                       [&](const std::string& phase) { return contains(phase, text); });
  };
  return expect(
      contains(basic.optimizationReportText, "\"optimizationLevel\": \"O1\"") &&
          countOccurrences(basic.optimizationReportText, "\"name\": ") == 5 &&
          !contains(basic.optimizationReportText, "fold-cleaned-constants") &&
          contains(standard.optimizationReportText, "\"optimizationLevel\": \"O2\"") &&
          countOccurrences(standard.optimizationReportText, "\"name\": ") == 8 &&
          contains(standard.optimizationReportText, "fold-cleaned-constants") &&
          !contains(standard.optimizationReportText,
                    "propagate-aggressive-local-constants") &&
          contains(aggressive.optimizationReportText,
                   "\"optimizationLevel\": \"O3\"") &&
          countOccurrences(aggressive.optimizationReportText, "\"name\": ") == 12 &&
          contains(aggressive.optimizationReportText,
                   "propagate-aggressive-local-constants") &&
          contains(aggressive.optimizationReportText, "simplify-aggressive-blocks") &&
          phaseContains(basic, "opt-level=O1") &&
          phaseContains(standard, "opt-level=O2") &&
          phaseContains(aggressive, "opt-level=O3"),
      "build optimization levels did not select distinct Interflow pipelines");
}

int smokeBuildDriverUsesIncrementalCache() {
  constexpr const char* source = R"(package demo.cache

object Main {
  def main = {
    val answer = 40 + 2
    println(answer)
  }
}
)";

  const std::filesystem::path cacheDirectory =
      std::filesystem::temp_directory_path() /
      "cpp-scalanative-smoke-incremental-cache";
  std::error_code ignored;
  std::filesystem::remove_all(cacheDirectory, ignored);

  scalanative::tools::build::BuildDriver driver;
  auto build = [&](bool debugInfo, std::string sourceText) {
    scalanative::tools::build::BuildOptions options;
    options.action = scalanative::tools::build::BuildAction::EmitLlvm;
    options.optimizationLevel = scalanative::tools::build::OptimizationLevel::O2;
    options.debugInfo = debugInfo;
    options.cacheDirectory = cacheDirectory;
    scalanative::support::DiagnosticEngine diagnostics;
    return driver.buildSource("CacheMain.scala", std::move(sourceText), options,
                              diagnostics);
  };

  const scalanative::tools::build::BuildResult first = build(true, source);
  const scalanative::tools::build::BuildResult second = build(true, source);
  const scalanative::tools::build::BuildResult changedFlags = build(false, source);
  const scalanative::tools::build::BuildResult changedSource =
      build(true, std::string(source) + "\n");
  auto buildNir = [&]() {
    scalanative::tools::build::BuildOptions options;
    options.action = scalanative::tools::build::BuildAction::EmitNir;
    options.optimizationLevel = scalanative::tools::build::OptimizationLevel::O2;
    options.cacheDirectory = cacheDirectory;
    scalanative::support::DiagnosticEngine diagnostics;
    return driver.buildSource("CacheMain.scala", source, options, diagnostics);
  };
  const scalanative::tools::build::BuildResult firstNir = buildNir();
  const scalanative::tools::build::BuildResult secondNir = buildNir();

  auto phaseContains = [](const scalanative::tools::build::BuildResult& result,
                          std::string_view text) {
    return std::any_of(result.phaseLog.begin(), result.phaseLog.end(),
                       [&](const std::string& phase) { return contains(phase, text); });
  };
  const bool cacheEntryExists =
      !first.cacheKey.empty() &&
      std::filesystem::exists(cacheDirectory / first.cacheKey / "manifest.txt");
  const bool valid =
      first.ok && second.ok && changedFlags.ok && changedSource.ok && firstNir.ok &&
      secondNir.ok && !first.cacheHit && second.cacheHit && !changedFlags.cacheHit &&
      !changedSource.cacheHit && !firstNir.cacheHit && secondNir.cacheHit &&
      first.cacheKey == second.cacheKey && firstNir.cacheKey == secondNir.cacheKey &&
      first.cacheKey != firstNir.cacheKey && first.cacheKey != changedFlags.cacheKey &&
      first.cacheKey != changedSource.cacheKey && first.llvmIr == second.llvmIr &&
      first.optimizationReportText == second.optimizationReportText &&
      firstNir.nirText == secondNir.nirText &&
      firstNir.optimizationReportText == secondNir.optimizationReportText &&
      phaseContains(first, "cache: miss ") && phaseContains(first, "cache: stored ") &&
      phaseContains(second, "cache: hit ") && !phaseContains(second, "lexer:") &&
      cacheEntryExists;

  std::filesystem::remove_all(cacheDirectory, ignored);
  return expect(valid, "incremental cache did not reuse or invalidate LLVM entries");
}

int smokeBuildDriverCachesNativeObjects() {
  constexpr const char* source = R"(package demo.nativecache

object Main {
  def main = println(40 + 2)
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path cacheDirectory =
      temporary / "cpp-scalanative-smoke-native-object-cache";
  const std::filesystem::path firstBinary =
      temporary / "cpp-scalanative-smoke-native-object-first";
  const std::filesystem::path secondBinary =
      temporary / "cpp-scalanative-smoke-native-object-second";
  const std::filesystem::path lldFirstBinary =
      temporary / "cpp-scalanative-smoke-native-object-lld-first";
  const std::filesystem::path lldSecondBinary =
      temporary / "cpp-scalanative-smoke-native-object-lld-second";
  const std::filesystem::path sysrootBinary =
      temporary / "cpp-scalanative-smoke-native-object-sysroot";
  const std::filesystem::path invalidSysrootBinary =
      temporary / "cpp-scalanative-smoke-native-object-invalid-sysroot";
  const std::filesystem::path invalidSysroot =
      temporary / "cpp-scalanative-smoke-missing-sysroot";
  const std::filesystem::path emptySysroot =
      temporary / "cpp-scalanative-smoke-empty-sysroot";
  const std::filesystem::path invalidTargetBinary =
      temporary / "cpp-scalanative-smoke-native-object-invalid-target";
  const std::filesystem::path incompleteSysrootBinary =
      temporary / "cpp-scalanative-smoke-native-object-incomplete-sysroot";
  const std::filesystem::path incompatibleLinkerBinary =
      temporary / "cpp-scalanative-smoke-native-object-incompatible-linker";
  const std::filesystem::path staticBinary =
      temporary / "cpp-scalanative-smoke-native-object-static";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-native-object.out";
  const std::filesystem::path lldOutput =
      temporary / "cpp-scalanative-smoke-native-object-lld.out";
  const std::filesystem::path sysrootOutput =
      temporary / "cpp-scalanative-smoke-native-object-sysroot.out";
  const std::filesystem::path staticOutput =
      temporary / "cpp-scalanative-smoke-native-object-static.out";
  std::error_code ignored;
  std::filesystem::remove_all(cacheDirectory, ignored);
  std::filesystem::remove_all(invalidSysroot, ignored);
  std::filesystem::remove_all(emptySysroot, ignored);
  std::filesystem::create_directories(emptySysroot, ignored);
  const bool emptySysrootCreated = !ignored;
  ignored.clear();

  scalanative::tools::build::BuildDriver driver;
  auto build = [&](const std::filesystem::path& outputPath,
                   scalanative::tools::build::LinkMode linkMode =
                       scalanative::tools::build::LinkMode::Default,
                   scalanative::tools::build::LinkerMode linkerMode =
                       scalanative::tools::build::LinkerMode::Default,
                   const std::filesystem::path& sysroot = {},
                   std::string targetTriple = {}) {
    scalanative::tools::build::BuildOptions options;
    options.action = scalanative::tools::build::BuildAction::BuildBinary;
    options.optimizationLevel = scalanative::tools::build::OptimizationLevel::O2;
    options.linkMode = linkMode;
    options.linkerMode = linkerMode;
    options.sysroot = sysroot;
    options.targetTriple = std::move(targetTriple);
    options.cacheDirectory = cacheDirectory;
    options.outputPath = outputPath;
    scalanative::support::DiagnosticEngine diagnostics;
    return driver.buildSource("NativeCacheMain.scala", source, options, diagnostics);
  };

  const scalanative::tools::build::BuildResult first = build(firstBinary);
  const scalanative::tools::build::BuildResult second = build(secondBinary);
  if (!first.ok || !second.ok) {
    const std::string diagnostics = first.diagnosticsText + second.diagnosticsText;
    std::filesystem::remove_all(cacheDirectory, ignored);
    if (contains(diagnostics, "clang toolchain not found")) {
      return 0;
    }
    return fail("native object cache builds failed: " + diagnostics);
  }
  const scalanative::tools::build::BuildResult lldFirst =
      build(lldFirstBinary, scalanative::tools::build::LinkMode::Default,
            scalanative::tools::build::LinkerMode::Lld);
  const scalanative::tools::build::BuildResult lldSecond =
      build(lldSecondBinary, scalanative::tools::build::LinkMode::Default,
            scalanative::tools::build::LinkerMode::Lld);
  const scalanative::tools::build::BuildResult invalidSysrootBuild =
      build(invalidSysrootBinary, scalanative::tools::build::LinkMode::Default,
            scalanative::tools::build::LinkerMode::Default, invalidSysroot);
  const scalanative::tools::build::BuildResult invalidTargetBuild =
      build(invalidTargetBinary, scalanative::tools::build::LinkMode::Default,
            scalanative::tools::build::LinkerMode::Default, {}, "not-a-real-target");
#ifdef __linux__
  const scalanative::tools::build::BuildResult sysrootBuild =
      build(sysrootBinary, scalanative::tools::build::LinkMode::Default,
            scalanative::tools::build::LinkerMode::Default, "/");
  const scalanative::tools::build::BuildResult incompleteSysrootBuild =
      build(incompleteSysrootBinary, scalanative::tools::build::LinkMode::Default,
            scalanative::tools::build::LinkerMode::Default, emptySysroot);
#ifdef __x86_64__
  const scalanative::tools::build::BuildResult incompatibleLinkerBuild =
      build(incompatibleLinkerBinary, scalanative::tools::build::LinkMode::Default,
            scalanative::tools::build::LinkerMode::Default, emptySysroot,
            "aarch64-unknown-linux-gnu");
#endif
#endif
  const scalanative::tools::build::BuildResult staticBuild =
      build(staticBinary, scalanative::tools::build::LinkMode::Static);

  auto phaseContains = [](const scalanative::tools::build::BuildResult& result,
                          std::string_view text) {
    return std::any_of(result.phaseLog.begin(), result.phaseLog.end(),
                       [&](const std::string& phase) { return contains(phase, text); });
  };
  const std::string runCommand = secondBinary.string() + " > " + output.string();
  const bool ran = std::system(runCommand.c_str()) == 0;
  const std::string outputText = readTextFile(output);
  bool lldRan = false;
  std::string lldOutputText;
  if (lldSecond.ok) {
    const std::string lldRunCommand =
        lldSecondBinary.string() + " > " + lldOutput.string();
    lldRan = std::system(lldRunCommand.c_str()) == 0;
    lldOutputText = readTextFile(lldOutput);
  }
  bool sysrootOutcome = true;
  bool incompatibleLinkerOutcome = true;
#ifdef __linux__
  bool sysrootRan = false;
  std::string sysrootOutputText;
  if (sysrootBuild.ok) {
    const std::string sysrootRunCommand =
        sysrootBinary.string() + " > " + sysrootOutput.string();
    sysrootRan = std::system(sysrootRunCommand.c_str()) == 0;
    sysrootOutputText = readTextFile(sysrootOutput);
  }
  sysrootOutcome = sysrootBuild.ok && sysrootBuild.cacheHit &&
                   !sysrootBuild.objectCacheHit && !sysrootBuild.binaryCacheHit &&
                   sysrootBuild.objectCacheKey != second.objectCacheKey &&
                   sysrootBuild.linkFingerprint != second.linkFingerprint &&
                   phaseContains(sysrootBuild, "sysroot=/") &&
                   phaseContains(sysrootBuild, "'--sysroot=/'") &&
                   phaseContains(sysrootBuild, "native-cache: object miss ") &&
                   phaseContains(sysrootBuild, "native-cache: binary miss ") &&
                   sysrootRan && sysrootOutputText == "42\n";
  const bool incompleteSysrootOutcome =
      emptySysrootCreated && !incompleteSysrootBuild.ok &&
      contains(incompleteSysrootBuild.diagnosticsText,
               "native link capability probe failed") &&
      contains(incompleteSysrootBuild.diagnosticsText,
               "could not resolve linker plan inputs");
  sysrootOutcome = sysrootOutcome && incompleteSysrootOutcome;
#ifdef __x86_64__
  incompatibleLinkerOutcome =
      !incompatibleLinkerBuild.ok &&
      contains(incompatibleLinkerBuild.diagnosticsText,
               "native link capability probe failed") &&
      (contains(incompatibleLinkerBuild.diagnosticsText,
                "does not support target emulation 'aarch64linux'") ||
       contains(incompatibleLinkerBuild.diagnosticsText,
                "could not resolve linker plan inputs"));
#endif
#endif
  bool staticRan = false;
  std::string staticOutputText;
  if (staticBuild.ok) {
    const std::string staticRunCommand =
        staticBinary.string() + " > " + staticOutput.string();
    staticRan = std::system(staticRunCommand.c_str()) == 0;
    staticOutputText = readTextFile(staticOutput);
  }
  const std::string changedLinkFingerprint =
      scalanative::tools::build::BuildCache::linkFingerprint(
          scalanative::tools::build::NativeLinkFingerprintInputs{second.objectCacheKey,
                                                                 "different-toolchain",
                                                                 {},
                                                                 {},
                                                                 "default",
                                                                 "default",
                                                                 {"implicit:-lm"}});
  const bool lldOutcome =
      lldFirst.ok && lldSecond.ok
          ? lldFirst.cacheHit && lldFirst.objectCacheHit && !lldFirst.binaryCacheHit &&
                lldSecond.cacheHit && lldSecond.objectCacheHit &&
                lldSecond.binaryCacheHit &&
                lldFirst.objectCacheKey == second.objectCacheKey &&
                lldFirst.linkFingerprint == lldSecond.linkFingerprint &&
                lldFirst.linkFingerprint != second.linkFingerprint &&
                phaseContains(lldFirst, "linker=lld") &&
                phaseContains(lldFirst, "-fuse-ld=") &&
                phaseContains(lldFirst, "native-cache: binary miss ") &&
                phaseContains(lldSecond, "native-cache: binary hit ") && lldRan &&
                lldOutputText == "42\n"
          : !lldFirst.ok && !lldSecond.ok &&
                contains(lldFirst.diagnosticsText, "LLD linker not found") &&
                contains(lldSecond.diagnosticsText, "LLD linker not found");
  const bool staticOutcome =
      staticBuild.ok
          ? staticBuild.cacheHit && staticBuild.objectCacheHit &&
                staticBuild.objectCacheKey == second.objectCacheKey &&
                staticBuild.linkFingerprint != second.linkFingerprint &&
                phaseContains(staticBuild, "link-mode=static") &&
                phaseContains(staticBuild, "'-static'") && staticRan &&
                staticOutputText == "42\n"
          : !staticBuild.cacheHit && !staticBuild.objectCacheHit &&
                phaseContains(staticBuild, "native-target: capability probe") &&
                contains(staticBuild.diagnosticsText,
                         "static linkage is unavailable") &&
                contains(staticBuild.diagnosticsText, "could not be resolved by clang");
  const bool invalidSysrootOutcome =
      !invalidSysrootBuild.ok &&
      contains(invalidSysrootBuild.diagnosticsText, "native sysroot does not exist") &&
      contains(invalidSysrootBuild.diagnosticsText, invalidSysroot.string());
  const bool invalidTargetOutcome =
      !invalidTargetBuild.ok &&
      contains(invalidTargetBuild.diagnosticsText,
               "clang cannot use native target 'not-a-real-target'");
  const bool valid =
      !first.cacheHit && !first.objectCacheHit && !first.binaryCacheHit &&
      second.cacheHit && second.objectCacheHit && second.binaryCacheHit &&
      !first.objectCacheKey.empty() && first.objectCacheKey == second.objectCacheKey &&
      !first.linkFingerprint.empty() &&
      first.linkFingerprint == second.linkFingerprint &&
      second.linkFingerprint != changedLinkFingerprint &&
      phaseContains(first, "native-cache: object miss ") &&
      phaseContains(first, "native-cache: object stored ") &&
      phaseContains(first, "native-cache: binary miss ") &&
      phaseContains(first, "native-cache: binary stored ") &&
      phaseContains(first, "'-c'") &&
      phaseContains(second, "native-cache: object hit ") &&
      phaseContains(second, "native-cache: binary hit ") &&
      !phaseContains(second, "'-c'") && ran && outputText == "42\n" && lldOutcome &&
      sysrootOutcome && incompatibleLinkerOutcome && invalidSysrootOutcome &&
      invalidTargetOutcome && staticOutcome;
  std::ostringstream failure;
  failure << "native object cache did not skip backend compilation"
          << " (lld=" << lldOutcome << ", sysroot=" << sysrootOutcome
          << ", incompatible-linker=" << incompatibleLinkerOutcome
          << ", invalid-sysroot=" << invalidSysrootOutcome
          << ", invalid-target=" << invalidTargetOutcome << ", static=" << staticOutcome
          << ')';
  if (!lldFirst.ok || !lldSecond.ok) {
    failure << "\nLLD diagnostics:\n"
            << lldFirst.diagnosticsText << lldSecond.diagnosticsText;
  }
#ifdef __linux__
  if (!incompleteSysrootBuild.ok) {
    failure << "\nIncomplete sysroot diagnostics:\n"
            << incompleteSysrootBuild.diagnosticsText;
  }
#ifdef __x86_64__
  if (!incompatibleLinkerBuild.ok) {
    failure << "\nIncompatible linker diagnostics:\n"
            << incompatibleLinkerBuild.diagnosticsText;
  }
#endif
#endif
  if (!staticBuild.ok) {
    failure << "\nStatic diagnostics:\n" << staticBuild.diagnosticsText;
  }

  std::filesystem::remove_all(cacheDirectory, ignored);
  std::filesystem::remove(firstBinary, ignored);
  std::filesystem::remove(secondBinary, ignored);
  std::filesystem::remove(lldFirstBinary, ignored);
  std::filesystem::remove(lldSecondBinary, ignored);
  std::filesystem::remove(sysrootBinary, ignored);
  std::filesystem::remove(invalidSysrootBinary, ignored);
  std::filesystem::remove(invalidTargetBinary, ignored);
  std::filesystem::remove(incompleteSysrootBinary, ignored);
  std::filesystem::remove(incompatibleLinkerBinary, ignored);
  std::filesystem::remove(staticBinary, ignored);
  std::filesystem::remove(output, ignored);
  std::filesystem::remove(lldOutput, ignored);
  std::filesystem::remove(sysrootOutput, ignored);
  std::filesystem::remove(staticOutput, ignored);
  std::filesystem::remove(firstBinary.string() + ".ll", ignored);
  std::filesystem::remove(firstBinary.string() + ".link.o", ignored);
  std::filesystem::remove(secondBinary.string() + ".link.o", ignored);
  std::filesystem::remove(lldFirstBinary.string() + ".link.o", ignored);
  std::filesystem::remove(lldSecondBinary.string() + ".link.o", ignored);
  std::filesystem::remove(sysrootBinary.string() + ".link.o", ignored);
  std::filesystem::remove(invalidSysrootBinary.string() + ".link.o", ignored);
  std::filesystem::remove(invalidTargetBinary.string() + ".link.o", ignored);
  std::filesystem::remove(incompleteSysrootBinary.string() + ".link.o", ignored);
  std::filesystem::remove(incompatibleLinkerBinary.string() + ".link.o", ignored);
  std::filesystem::remove(staticBinary.string() + ".link.o", ignored);
  std::filesystem::remove_all(emptySysroot, ignored);
  return expect(valid, failure.str());
}

int smokeBuildDriverEmitsSourceDebugMetadata() {
  constexpr const char* source = R"(package demo.debug

class DebugBase(val base: Int)
class DebugPair(val left: Int, var right: Int) extends DebugBase(left)

object Main {
  def answer(value: Int): Int =
    value + 1

  def adjusted(value: Int): Int = {
    val offset = 1
    val inner = {
      val doubled = value + value
      doubled + offset
    }
    var total = inner + offset
    total
  }

  def pairTotal(pair: DebugPair): Int =
    pair.base + pair.left + pair.right

  def main = {
    val pair = new DebugPair(20, 20)
    println(adjusted(answer(pairTotal(pair))))
  }
}
)";

  scalanative::tools::build::BuildDriver driver;
  auto build = [&](bool optimize, bool debugInfo = true) {
    scalanative::tools::build::BuildOptions options;
    options.action = scalanative::tools::build::BuildAction::EmitLlvm;
    options.optimize = optimize;
    options.debugInfo = debugInfo;
    scalanative::support::DiagnosticEngine diagnostics;
    return driver.buildSource("virtual/debug/DebugMain.scala", source, options,
                              diagnostics);
  };

  const scalanative::tools::build::BuildResult plain = build(false);
  const scalanative::tools::build::BuildResult optimized = build(true);
  const scalanative::tools::build::BuildResult stripped = build(false, false);
  if (int code = expect(plain.ok && optimized.ok && stripped.ok,
                        "source debug metadata builds did not succeed")) {
    return code;
  }

  return expect(
      contains(plain.llvmIr, "!llvm.dbg.cu = !{") &&
          contains(plain.llvmIr, "!DIFile(filename: \"DebugMain.scala\", directory: "
                                 "\"virtual/debug\")") &&
          contains(plain.llvmIr, "!DICompileUnit(language: DW_LANG_C_plus_plus") &&
          contains(plain.llvmIr, "isOptimized: false") &&
          contains(plain.llvmIr, "!DISubprogram(name: \"answer\", linkageName: "
                                 "\"demo_debug_Main_answer\"") &&
          contains(plain.llvmIr, "line: 7") &&
          contains(plain.llvmIr,
                   "define i32 @demo_debug_Main_answer(i32 %value) !dbg !") &&
          contains(plain.llvmIr, "declare void @llvm.donothing()") &&
          contains(plain.llvmIr, "call void @llvm.donothing(), !dbg !") &&
          contains(plain.llvmIr, "!DILocation(line: 8, column: 11, scope: !") &&
          contains(plain.llvmIr, "!DIBasicType(name: \"Int\", size: 32, encoding: "
                                 "DW_ATE_signed)") &&
          contains(plain.llvmIr, "!DILocalVariable(name: \"value\", arg: 1") &&
          contains(plain.llvmIr, "call void @llvm.dbg.value(metadata i32 %value") &&
          contains(plain.llvmIr, "!DILocalVariable(name: \"offset\"") &&
          contains(plain.llvmIr, "call void @llvm.dbg.value(metadata i32 %offset") &&
          contains(plain.llvmIr, "!DILocalVariable(name: \"doubled\"") &&
          countOccurrences(plain.llvmIr, "!DILexicalBlock(") >= 2 &&
          contains(plain.llvmIr, "!DILocalVariable(name: \"total\"") &&
          contains(plain.llvmIr,
                   "call void @llvm.dbg.declare(metadata ptr %total_slot") &&
          contains(plain.llvmIr, "!DICompositeType(tag: DW_TAG_structure_type, name: "
                                 "\"demo.debug.DebugPair\"") &&
          contains(plain.llvmIr, "!DIDerivedType(tag: DW_TAG_inheritance, scope: !") &&
          contains(plain.llvmIr, "!DIDerivedType(tag: DW_TAG_member, name: \"base\"") &&
          contains(plain.llvmIr, "size: 32, offset: 64") &&
          contains(plain.llvmIr, "!DIDerivedType(tag: DW_TAG_member, name: \"left\"") &&
          contains(plain.llvmIr, "size: 32, offset: 96") &&
          contains(plain.llvmIr,
                   "!DIDerivedType(tag: DW_TAG_member, name: \"right\"") &&
          contains(plain.llvmIr, "size: 32, offset: 128") &&
          contains(optimized.llvmIr, "isOptimized: true") &&
          contains(stripped.llvmIr,
                   "%scalanative.source_frame = type { ptr, ptr, ptr, i32, i32 }") &&
          contains(stripped.llvmIr, "@.source.function.") &&
          contains(stripped.llvmIr, "@.source.file.") &&
          !contains(stripped.llvmIr, "!llvm.dbg.cu") &&
          !contains(stripped.llvmIr, "!DILocation") &&
          !contains(stripped.llvmIr, "llvm.donothing") &&
          !contains(stripped.llvmIr, "llvm.dbg.value") &&
          !contains(stripped.llvmIr, "llvm.dbg.declare"),
      "LLVM debug metadata did not retain source function information");
}

int smokeCodegenRejectsUnsupportedReachableValue() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();
  const scalanative::support::SourceSpan valueSpan{scalanative::support::SourceId{7},
                                                   11, 3};
  scalanative::nir::FunctionBodyBuilder body;
  (void)body.addReturn("Int", scalanative::nir::unknownValue("mystery", valueSpan),
                       noSpan);

  scalanative::nir::ModuleBuilder module("demo.codegen.failure");
  module.addFunctionDef("demo.codegen.failure.Main.value", "()Int",
                        std::move(body).build(), noSpan);

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module).build());
  scalanative::tools::codegen::LlvmCodegen codegen;
  const scalanative::tools::codegen::CodegenResult result = codegen.emit(program);

  bool namedFunction = false;
  bool retainedSpan = false;
  for (const scalanative::tools::codegen::CodegenError& error : result.errors) {
    namedFunction =
        namedFunction || contains(error.message, "demo.codegen.failure.Main.value");
    retainedSpan = retainedSpan || (error.span.source == valueSpan.source &&
                                    error.span.start == valueSpan.start &&
                                    error.span.length == valueSpan.length);
  }
  return expect(!result.ok && namedFunction && retainedSpan &&
                    !contains(result.llvmIr, "llvm.donothing") &&
                    !contains(result.llvmIr, "llvm.dbg.value") &&
                    !contains(result.llvmIr, "llvm.dbg.declare") &&
                    !contains(result.llvmIr, "!DILocation") &&
                    contains(result.llvmIr, "; unsupported return expression"),
                "codegen accepted an unsupported reachable value");
}

int smokeTypedClassCastRuntime() {
  constexpr const char* source = R"(package demo.classcast

class BaseValue
class OtherValue

object CastRules {
  def referenceFailure(value: BaseValue): String =
    try {
      println(value.asInstanceOf[OtherValue])
      "missed reference cast failure"
    } catch {
      case error: ClassCastException => error.getMessage
    }

  def boxedFailure(value: Any): String =
    try {
      println(value.asInstanceOf[Long])
      "missed boxed cast failure"
    } catch {
      case error: ClassCastException => error.getMessage
    }

}

object Main {
  def main = {
    println(CastRules.referenceFailure(new BaseValue()))
    println(CastRules.boxedFailure(7))
    val missing: BaseValue = null.asInstanceOf[BaseValue]
    println(if (missing == null) 1 else 0)
  }
}
)";

  scalanative::tools::build::BuildDriver driver;
  scalanative::support::DiagnosticEngine diagnostics;
  const scalanative::tools::build::BuildResult result =
      driver.buildSource("ClassCast.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "typed class-cast build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "class @java.lang.ClassCastException : "
                                   "@java.lang.RuntimeException") &&
              contains(result.nirText,
                       "as-instance-of[demo.classcast.OtherValue](%value)") &&
              contains(result.nirText, "unbox[Long](%value)") &&
              contains(result.llvmIr, "@__type_java_lang_ClassCastException =") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_throw_class_cast() "
                       "noreturn") &&
              contains(result.llvmIr,
                       "c\"Value cannot be cast to requested type\\00\"") &&
              countOccurrences(result.llvmIr,
                               "call void @__scalanative_throw_class_cast()") == 12 &&
              contains(result.llvmIr,
                       "fail:\n  call void @__scalanative_throw_class_cast()\n  "
                       "unreachable\nsuccess:"),
          "class casts did not lower to one typed runtime failure")) {
    return code;
  }

  constexpr const char* boxedOnlySource = R"(package demo.classcast.boxedonly
object Main {
  def cast(value: Any): Long = value.asInstanceOf[Long]
  def main = println(cast(7))
}
)";
  scalanative::tools::build::BuildOptions optimizedOptions;
  optimizedOptions.optimize = true;
  scalanative::support::DiagnosticEngine boxedOnlyDiagnostics;
  const scalanative::tools::build::BuildResult boxedOnly = driver.buildSource(
      "BoxedCastOnly.scala", boxedOnlySource, optimizedOptions, boxedOnlyDiagnostics);
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

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path caughtBinary =
      temporary / "cpp-scalanative-smoke-caught-class-cast";
  const std::filesystem::path caughtOutput =
      temporary / "cpp-scalanative-smoke-caught-class-cast.out";
  std::error_code ignored;
  std::filesystem::remove(caughtBinary, ignored);
  std::filesystem::remove(caughtOutput, ignored);

  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = caughtBinary;
  scalanative::support::DiagnosticEngine caughtDiagnostics;
  const scalanative::tools::build::BuildResult caught =
      driver.buildSource("ClassCast.scala", source, binaryOptions, caughtDiagnostics);
  if (!caught.ok) {
    return expect(contains(caught.diagnosticsText, "clang toolchain not found"),
                  "caught class-cast native build failed: " + caught.diagnosticsText);
  }
  const std::string caughtCommand =
      caughtBinary.string() + " > " + caughtOutput.string() + " 2>&1";
  const int caughtStatus = std::system(caughtCommand.c_str());
  const std::string caughtText = readTextFile(caughtOutput);
  std::filesystem::remove(caughtBinary, ignored);
  std::filesystem::remove(caughtOutput, ignored);
  if (int code =
          expect(caughtStatus == 0 && caughtText ==
                                          "Value cannot be cast to requested type\n"
                                          "Value cannot be cast to requested type\n1\n",
                 "typed reference or boxed cast was not catchable (status=" +
                     std::to_string(caughtStatus) + ", output='" + caughtText + "')")) {
    return code;
  }

  constexpr const char* uncaughtSource = R"(package demo.classcast.uncaught
class BaseValue
class OtherValue
object Main {
  def main = {
    val value: BaseValue = new BaseValue()
    println(value.asInstanceOf[OtherValue])
    println("after")
  }
}
)";
  const std::filesystem::path uncaughtBinary =
      temporary / "cpp-scalanative-smoke-uncaught-class-cast";
  const std::filesystem::path uncaughtOutput =
      temporary / "cpp-scalanative-smoke-uncaught-class-cast.out";
  std::filesystem::remove(uncaughtBinary, ignored);
  std::filesystem::remove(uncaughtOutput, ignored);
  binaryOptions.outputPath = uncaughtBinary;
  scalanative::support::DiagnosticEngine uncaughtDiagnostics;
  const scalanative::tools::build::BuildResult uncaught = driver.buildSource(
      "UncaughtClassCast.scala", uncaughtSource, binaryOptions, uncaughtDiagnostics);
  if (!uncaught.ok) {
    return expect(contains(uncaught.diagnosticsText, "clang toolchain not found"),
                  "uncaught class-cast native build failed: " +
                      uncaught.diagnosticsText);
  }
  const std::string uncaughtCommand =
      uncaughtBinary.string() + " > " + uncaughtOutput.string() + " 2>&1";
  const int uncaughtStatus = std::system(uncaughtCommand.c_str());
  const std::string uncaughtText = readTextFile(uncaughtOutput);
  std::filesystem::remove(uncaughtBinary, ignored);
  std::filesystem::remove(uncaughtOutput, ignored);
  return expect(
      uncaughtStatus != 0 &&
          contains(uncaught.llvmIr, "@__type_java_lang_ClassCastException =") &&
          contains(uncaughtText, "Uncaught exception: ClassCastException: "
                                 "Value cannot be cast to requested type\n") &&
          contains(uncaughtText,
                   "\tat demo.classcast.uncaught.Main.main(UncaughtClassCast.scala:") &&
          !contains(uncaughtText, "after\n"),
      "uncaught incompatible cast did not retain and report its typed failure");
}

int smokeTypedNullReceiverRuntime() {
  constexpr const char* source = R"(package demo.nullreceiver

class Box(val value: Int) {
  var current: Int = 2
  def constant: Int = 11
  def consume(delta: Int): Int = delta
}

trait Named {
  def name: String
}

class Entry extends Named {
  override def name: String = "entry"
}

object NullRules {
  def effect(): Int = {
    println("effect")
    9
  }

  def fieldRead(box: Box): String =
    try {
      println(box.value)
      "missed field read failure"
    } catch {
      case error: NullPointerException => error.getMessage
    }

  def fieldWrite(box: Box): String =
    try {
      box.current = effect()
      "missed field write failure"
    } catch {
      case error: NullPointerException => error.getMessage
    }

  def directCall(box: Box): String =
    try {
      println(box.constant)
      "missed direct call failure"
    } catch {
      case error: NullPointerException => error.getMessage
    }

  def argumentCall(box: Box): String =
    try {
      println(box.consume(effect()))
      "missed argument call failure"
    } catch {
      case error: NullPointerException => error.getMessage
    }

  def virtualCall(named: Named): String =
    try {
      println(named.name)
      "missed virtual call failure"
    } catch {
      case error: NullPointerException => error.getMessage
    }

  def localDirect(): String = {
    val box: Box = null
    try {
      println(box.constant)
      "missed local direct call failure"
    } catch {
      case error: NullPointerException => error.getMessage
    }
  }
}

object Main {
  def main = {
    val box: Box = null
    val named: Named = null
    println(NullRules.fieldRead(box))
    println(NullRules.fieldWrite(box))
    println(NullRules.directCall(box))
    println(NullRules.argumentCall(box))
    println(NullRules.virtualCall(named))
    println(NullRules.localDirect())
    val valid = new Box(7)
    println(valid.value)
    valid.current = 8
    println(valid.current)
    println(valid.consume(9))
  }
}
)";

  scalanative::tools::build::BuildDriver driver;
  scalanative::support::DiagnosticEngine diagnostics;
  const scalanative::tools::build::BuildResult result =
      driver.buildSource("NullReceiver.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "typed null-receiver build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.llvmIr,
                   "define internal ptr "
                   "@__scalanative_require_non_null_receiver(ptr %receiver)") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_throw_null_receiver() "
                       "noreturn") &&
              contains(result.llvmIr, "c\"Receiver cannot be null\\00\"") &&
              countOccurrences(
                  result.llvmIr,
                  "call ptr @__scalanative_require_non_null_receiver(ptr ") >= 6 &&
              contains(result.llvmIr, "@__type_java_lang_NullPointerException ="),
          "instance member dereferences did not lower to a shared null guard")) {
    return code;
  }

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path caughtBinary =
      temporary / "cpp-scalanative-smoke-caught-null-receiver";
  const std::filesystem::path caughtOutput =
      temporary / "cpp-scalanative-smoke-caught-null-receiver.out";
  std::error_code ignored;
  std::filesystem::remove(caughtBinary, ignored);
  std::filesystem::remove(caughtOutput, ignored);

  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = caughtBinary;
  scalanative::support::DiagnosticEngine caughtDiagnostics;
  const scalanative::tools::build::BuildResult caught = driver.buildSource(
      "NullReceiver.scala", source, binaryOptions, caughtDiagnostics);
  if (!caught.ok) {
    return expect(contains(caught.diagnosticsText, "clang toolchain not found"),
                  "caught null-receiver native build failed: " +
                      caught.diagnosticsText);
  }
  const std::string caughtCommand =
      caughtBinary.string() + " > " + caughtOutput.string() + " 2>&1";
  const int caughtStatus = std::system(caughtCommand.c_str());
  const std::string caughtText = readTextFile(caughtOutput);
  std::filesystem::remove(caughtBinary, ignored);
  std::filesystem::remove(caughtOutput, ignored);
  if (int code = expect(
          caughtStatus == 0 && caughtText == "Receiver cannot be null\n"
                                             "effect\nReceiver cannot be null\n"
                                             "Receiver cannot be null\n"
                                             "effect\nReceiver cannot be null\n"
                                             "Receiver cannot be null\n"
                                             "Receiver cannot be null\n7\n8\n9\n",
          "typed null receivers were not catchable in evaluation order (status=" +
              std::to_string(caughtStatus) + ", output='" + caughtText + "')")) {
    return code;
  }

  constexpr const char* uncaughtSource = R"(package demo.nullreceiver.uncaught
class Box(val value: Int)
object Main {
  def read(box: Box): Int = box.value
  def main = {
    val box: Box = null
    println(read(box))
    println("after")
  }
}
)";
  const std::filesystem::path uncaughtBinary =
      temporary / "cpp-scalanative-smoke-uncaught-null-receiver";
  const std::filesystem::path uncaughtOutput =
      temporary / "cpp-scalanative-smoke-uncaught-null-receiver.out";
  std::filesystem::remove(uncaughtBinary, ignored);
  std::filesystem::remove(uncaughtOutput, ignored);
  binaryOptions.outputPath = uncaughtBinary;
  scalanative::support::DiagnosticEngine uncaughtDiagnostics;
  const scalanative::tools::build::BuildResult uncaught = driver.buildSource(
      "UncaughtNullReceiver.scala", uncaughtSource, binaryOptions, uncaughtDiagnostics);
  if (!uncaught.ok) {
    return expect(contains(uncaught.diagnosticsText, "clang toolchain not found"),
                  "uncaught null-receiver native build failed: " +
                      uncaught.diagnosticsText);
  }
  const std::string uncaughtCommand =
      uncaughtBinary.string() + " > " + uncaughtOutput.string() + " 2>&1";
  const int uncaughtStatus = std::system(uncaughtCommand.c_str());
  const std::string uncaughtText = readTextFile(uncaughtOutput);
  std::filesystem::remove(uncaughtBinary, ignored);
  std::filesystem::remove(uncaughtOutput, ignored);
  return expect(
      uncaughtStatus != 0 &&
          contains(uncaught.llvmIr, "@__type_java_lang_NullPointerException =") &&
          contains(uncaught.llvmIr,
                   "call ptr @__scalanative_require_non_null_receiver(ptr ") &&
          contains(uncaughtText, "Uncaught exception: NullPointerException: "
                                 "Receiver cannot be null\n") &&
          contains(uncaughtText, "UncaughtNullReceiver.scala:") &&
          !contains(uncaughtText, "after\n"),
      "optimized uncaught null receiver did not retain and report its typed failure");
}

int smokeTypedStringReceiverRuntime() {
  constexpr const char* source = R"(package demo.stringreceiver

object StringRules {
  def effect(): String = {
    println("argument")
    "Scala"
  }

  def length(value: String): String =
    try {
      println(value.length)
      "missed String.length failure"
    } catch {
      case error: NullPointerException => error.getMessage
    }

  def hash(value: String): String =
    try {
      println(value.hashCode)
      "missed String.hashCode failure"
    } catch {
      case error: NullPointerException => error.getMessage
    }

  def text(value: String): String =
    try {
      println(value.toString)
      "missed String.toString failure"
    } catch {
      case error: NullPointerException => error.getMessage
    }

  def equal(value: String): String =
    try {
      println(value.equals(effect()))
      "missed String.equals failure"
    } catch {
      case error: NullPointerException => error.getMessage
    }
}

object Main {
  def main = {
    val missing: String = null
    println(StringRules.length(missing))
    println(StringRules.hash(missing))
    println(StringRules.text(missing))
    println(StringRules.equal(missing))

    val value = "Scala"
    println(value.length)
    println(value.hashCode == "Scala".hashCode)
    println(value.toString)
    println(value.equals(StringRules.effect()))
    println(value.equals(null))
    val other: Any = 7
    println(value.equals(other))
    println(missing == null)
  }
}
)";

  scalanative::tools::build::BuildDriver driver;
  scalanative::support::DiagnosticEngine diagnostics;
  const scalanative::tools::build::BuildResult result =
      driver.buildSource("StringReceiver.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "typed String-receiver build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText,
                   "call %scala.scalanative.runtime.stringLength(%value)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.stringHashCode(%value)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.stringToString(%value)") &&
              contains(result.nirText, "call "
                                       "%scala.scalanative.runtime.stringEquals(%value,"
                                       " box[String](call %effect()))") &&
              contains(result.llvmIr,
                       "define internal i1 @__scalanative_string_equals_object") &&
              countOccurrences(
                  result.llvmIr,
                  "call ptr @__scalanative_require_non_null_receiver(ptr ") >= 4 &&
              contains(result.llvmIr, "@__type_java_lang_NullPointerException ="),
          "String member intrinsics did not retain and lower typed receiver checks")) {
    return code;
  }

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path caughtBinary =
      temporary / "cpp-scalanative-smoke-caught-string-receiver";
  const std::filesystem::path caughtOutput =
      temporary / "cpp-scalanative-smoke-caught-string-receiver.out";
  std::error_code ignored;
  std::filesystem::remove(caughtBinary, ignored);
  std::filesystem::remove(caughtOutput, ignored);

  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = caughtBinary;
  scalanative::support::DiagnosticEngine caughtDiagnostics;
  const scalanative::tools::build::BuildResult caught = driver.buildSource(
      "StringReceiver.scala", source, binaryOptions, caughtDiagnostics);
  if (!caught.ok) {
    return expect(contains(caught.diagnosticsText, "clang toolchain not found"),
                  "caught String-receiver native build failed: " +
                      caught.diagnosticsText);
  }
  const std::string caughtCommand =
      caughtBinary.string() + " > " + caughtOutput.string() + " 2>&1";
  const int caughtStatus = std::system(caughtCommand.c_str());
  const std::string caughtText = readTextFile(caughtOutput);
  std::filesystem::remove(caughtBinary, ignored);
  std::filesystem::remove(caughtOutput, ignored);
  if (int code = expect(
          caughtStatus == 0 && caughtText == "Receiver cannot be null\n"
                                             "Receiver cannot be null\n"
                                             "Receiver cannot be null\n"
                                             "argument\nReceiver cannot be null\n"
                                             "5\ntrue\nScala\nargument\ntrue\n"
                                             "false\nfalse\ntrue\n",
          "typed String receivers were not catchable in evaluation order (status=" +
              std::to_string(caughtStatus) + ", output='" + caughtText + "')")) {
    return code;
  }

  constexpr const char* uncaughtSource = R"(package demo.stringreceiver.uncaught
object Main {
  def size(value: String): Int = value.length
  def main = {
    val value: String = null
    println(size(value))
    println("after")
  }
}
)";
  const std::filesystem::path uncaughtBinary =
      temporary / "cpp-scalanative-smoke-uncaught-string-receiver";
  const std::filesystem::path uncaughtOutput =
      temporary / "cpp-scalanative-smoke-uncaught-string-receiver.out";
  std::filesystem::remove(uncaughtBinary, ignored);
  std::filesystem::remove(uncaughtOutput, ignored);
  binaryOptions.outputPath = uncaughtBinary;
  scalanative::support::DiagnosticEngine uncaughtDiagnostics;
  const scalanative::tools::build::BuildResult uncaught =
      driver.buildSource("UncaughtStringReceiver.scala", uncaughtSource, binaryOptions,
                         uncaughtDiagnostics);
  if (!uncaught.ok) {
    return expect(contains(uncaught.diagnosticsText, "clang toolchain not found"),
                  "uncaught String-receiver native build failed: " +
                      uncaught.diagnosticsText);
  }
  const std::string uncaughtCommand =
      uncaughtBinary.string() + " > " + uncaughtOutput.string() + " 2>&1";
  const int uncaughtStatus = std::system(uncaughtCommand.c_str());
  const std::string uncaughtText = readTextFile(uncaughtOutput);
  std::filesystem::remove(uncaughtBinary, ignored);
  std::filesystem::remove(uncaughtOutput, ignored);
  return expect(
      uncaughtStatus != 0 &&
          contains(uncaught.llvmIr, "@__type_java_lang_NullPointerException =") &&
          contains(uncaught.llvmIr,
                   "call ptr @__scalanative_require_non_null_receiver(ptr ") &&
          contains(uncaughtText, "Uncaught exception: NullPointerException: "
                                 "Receiver cannot be null\n") &&
          contains(uncaughtText, "UncaughtStringReceiver.scala:") &&
          !contains(uncaughtText, "after\n"),
      "optimized uncaught String receiver did not retain and report its typed "
      "failure");
}

int smokeTypedAnyReceiverRuntime() {
  constexpr const char* source = R"(package demo.anyreceiver

class Plain(val value: Int)

object AnyRules {
  def effect(): String = {
    println("argument")
    "argument value"
  }

  def equal(value: Any): String =
    try {
      println(value.equals(effect()))
      "missed Any.equals failure"
    } catch {
      case error: NullPointerException => error.getMessage
    }

  def hash(value: Any): String =
    try {
      println(value.hashCode)
      "missed Any.hashCode failure"
    } catch {
      case error: NullPointerException => error.getMessage
    }

  def text(value: Any): String =
    try {
      println(value.toString)
      "missed Any.toString failure"
    } catch {
      case error: NullPointerException => error.getMessage
    }

  def equalPlain(value: Plain): String =
    try {
      println(value.equals(effect()))
      "missed class equals failure"
    } catch {
      case error: NullPointerException => error.getMessage
    }
}

object Main {
  def main = {
    val missing: Any = null
    println(AnyRules.equal(missing))
    println(AnyRules.hash(missing))
    println(AnyRules.text(missing))

    val missingPlain: Plain = null
    println(AnyRules.equalPlain(missingPlain))

    val direct: Any = "Scala"
    println(direct.equals("Scala"))
    println(direct.hashCode == "Scala".hashCode)
    println(direct.toString)

    val box = new Plain(1)
    println(box.equals(box))
    println(box.equals(new Plain(1)))
    println(box.hashCode == box.hashCode)

    println(missing == null)
    println("missing=" + missing)
    println({}.equals(missing))
  }
}
)";

  scalanative::tools::build::BuildDriver driver;
  scalanative::support::DiagnosticEngine diagnostics;
  const scalanative::tools::build::BuildResult result =
      driver.buildSource("AnyReceiver.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "typed Any-receiver build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          countOccurrences(result.nirText,
                           "call %scala.scalanative.runtime.anyReceiverEquals(") >= 2 &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.anyReceiverHashCode(%value)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.anyReceiverToString(%value)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.anyEquals(box[Unit](block()), "
                       "%missing)") &&
              !contains(result.nirText,
                        "call %scala.scalanative.runtime.anyReceiverEquals(%missing, "
                        "%missing)") &&
              countOccurrences(
                  result.llvmIr,
                  "call ptr @__scalanative_require_non_null_receiver(ptr ") >= 4 &&
              contains(result.llvmIr, "@__type_java_lang_NullPointerException ="),
          "Any member intrinsics did not preserve checked invocation and null-safe "
          "value operations")) {
    return code;
  }

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path caughtBinary =
      temporary / "cpp-scalanative-smoke-caught-any-receiver";
  const std::filesystem::path caughtOutput =
      temporary / "cpp-scalanative-smoke-caught-any-receiver.out";
  std::error_code ignored;
  std::filesystem::remove(caughtBinary, ignored);
  std::filesystem::remove(caughtOutput, ignored);

  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = caughtBinary;
  scalanative::support::DiagnosticEngine caughtDiagnostics;
  const scalanative::tools::build::BuildResult caught =
      driver.buildSource("AnyReceiver.scala", source, binaryOptions, caughtDiagnostics);
  if (!caught.ok) {
    return expect(contains(caught.diagnosticsText, "clang toolchain not found"),
                  "caught Any-receiver native build failed: " + caught.diagnosticsText);
  }
  const std::string caughtCommand =
      caughtBinary.string() + " > " + caughtOutput.string() + " 2>&1";
  const int caughtStatus = std::system(caughtCommand.c_str());
  const std::string caughtText = readTextFile(caughtOutput);
  std::filesystem::remove(caughtBinary, ignored);
  std::filesystem::remove(caughtOutput, ignored);
  if (int code = expect(
          caughtStatus == 0 && caughtText == "argument\nReceiver cannot be null\n"
                                             "Receiver cannot be null\n"
                                             "Receiver cannot be null\n"
                                             "argument\nReceiver cannot be null\n"
                                             "true\ntrue\nScala\ntrue\nfalse\n"
                                             "true\ntrue\nmissing=null\nfalse\n",
          "typed Any receivers were not catchable in evaluation order (status=" +
              std::to_string(caughtStatus) + ", output='" + caughtText + "')")) {
    return code;
  }

  constexpr const char* uncaughtSource = R"(package demo.anyreceiver.uncaught
object Main {
  def hash(value: Any): Int = value.hashCode
  def main = {
    val value: Any = null
    println(hash(value))
    println("after")
  }
}
)";
  const std::filesystem::path uncaughtBinary =
      temporary / "cpp-scalanative-smoke-uncaught-any-receiver";
  const std::filesystem::path uncaughtOutput =
      temporary / "cpp-scalanative-smoke-uncaught-any-receiver.out";
  std::filesystem::remove(uncaughtBinary, ignored);
  std::filesystem::remove(uncaughtOutput, ignored);
  binaryOptions.outputPath = uncaughtBinary;
  scalanative::support::DiagnosticEngine uncaughtDiagnostics;
  const scalanative::tools::build::BuildResult uncaught = driver.buildSource(
      "UncaughtAnyReceiver.scala", uncaughtSource, binaryOptions, uncaughtDiagnostics);
  if (!uncaught.ok) {
    return expect(contains(uncaught.diagnosticsText, "clang toolchain not found"),
                  "uncaught Any-receiver native build failed: " +
                      uncaught.diagnosticsText);
  }
  const std::string uncaughtCommand =
      uncaughtBinary.string() + " > " + uncaughtOutput.string() + " 2>&1";
  const int uncaughtStatus = std::system(uncaughtCommand.c_str());
  const std::string uncaughtText = readTextFile(uncaughtOutput);
  std::filesystem::remove(uncaughtBinary, ignored);
  std::filesystem::remove(uncaughtOutput, ignored);
  return expect(
      uncaughtStatus != 0 &&
          contains(uncaught.llvmIr, "@__type_java_lang_NullPointerException =") &&
          contains(uncaught.llvmIr,
                   "call ptr @__scalanative_require_non_null_receiver(ptr ") &&
          contains(uncaughtText, "Uncaught exception: NullPointerException: "
                                 "Receiver cannot be null\n") &&
          contains(uncaughtText, "UncaughtAnyReceiver.scala:") &&
          !contains(uncaughtText, "after\n"),
      "optimized uncaught Any receiver did not retain and report its typed failure");
}

int smokeNullThrowRuntime() {
  constexpr const char* source = R"(package demo.nullthrow

object ThrowRules {
  def direct(): String =
    try {
      throw null
    } catch {
      case error: NullPointerException => error.getMessage
    }

  def indirect(value: Throwable): String =
    try {
      throw value
    } catch {
      case error: NullPointerException => error.getMessage
    }

  def effect(): Throwable = {
    println("operand")
    val value: Throwable = null
    value
  }

  def evaluated(): String =
    try {
      throw effect()
    } catch {
      case error: NullPointerException => error.getMessage
    }

  def finalized(): String =
    try {
      throw null
    } catch {
      case error: NullPointerException => error.getMessage
    } finally {
      println("finally")
    }

  def ordinary(): String =
    try {
      throw new Exception("ordinary")
    } catch {
      case error: Exception => error.getMessage
    }
}

object Main {
  def main = {
    println(ThrowRules.direct())
    val missing: Throwable = null
    println(ThrowRules.indirect(missing))
    println(ThrowRules.evaluated())
    println(ThrowRules.finalized())
    println(ThrowRules.ordinary())
  }
}
)";

  scalanative::tools::build::BuildDriver driver;
  scalanative::support::DiagnosticEngine diagnostics;
  const scalanative::tools::build::BuildResult result =
      driver.buildSource("NullThrow.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "null-throw build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          countOccurrences(result.nirText, "throw(null)") >= 2 &&
              contains(result.nirText, "throw(%value)") &&
              contains(result.llvmIr,
                       "define internal ptr "
                       "@__scalanative_require_non_null_thrown_exception") &&
              contains(result.llvmIr,
                       "call void @__scalanative_throw_null_exception()") &&
              contains(result.llvmIr, "c\"Thrown exception cannot be null\\00\"") &&
              contains(result.llvmIr, "@__type_java_lang_NullPointerException ="),
          "null throws did not retain their typed NIR and runtime guard")) {
    return code;
  }

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path caughtBinary =
      temporary / "cpp-scalanative-smoke-caught-null-throw";
  const std::filesystem::path caughtOutput =
      temporary / "cpp-scalanative-smoke-caught-null-throw.out";
  std::error_code ignored;
  std::filesystem::remove(caughtBinary, ignored);
  std::filesystem::remove(caughtOutput, ignored);

  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.outputPath = caughtBinary;
  scalanative::support::DiagnosticEngine caughtDiagnostics;
  const scalanative::tools::build::BuildResult caught =
      driver.buildSource("NullThrow.scala", source, binaryOptions, caughtDiagnostics);
  if (!caught.ok) {
    return expect(contains(caught.diagnosticsText, "clang toolchain not found"),
                  "caught null-throw native build failed: " + caught.diagnosticsText);
  }
  const std::string caughtCommand =
      caughtBinary.string() + " > " + caughtOutput.string() + " 2>&1";
  const int caughtStatus = std::system(caughtCommand.c_str());
  const std::string caughtText = readTextFile(caughtOutput);
  std::filesystem::remove(caughtBinary, ignored);
  std::filesystem::remove(caughtOutput, ignored);
  if (int code = expect(
          caughtStatus == 0 && caughtText ==
                                   "Thrown exception cannot be null\n"
                                   "Thrown exception cannot be null\n"
                                   "operand\nThrown exception cannot be null\n"
                                   "finally\nThrown exception cannot be null\n"
                                   "ordinary\n",
          "null throws were not catchable with operand and finally order (status=" +
              std::to_string(caughtStatus) + ", output='" + caughtText + "')")) {
    return code;
  }

  constexpr const char* uncaughtSource = R"(package demo.nullthrow.uncaught
object Main {
  def main = {
    throw null
    println("after")
  }
}
)";
  const std::filesystem::path uncaughtBinary =
      temporary / "cpp-scalanative-smoke-uncaught-null-throw";
  const std::filesystem::path uncaughtOutput =
      temporary / "cpp-scalanative-smoke-uncaught-null-throw.out";
  std::filesystem::remove(uncaughtBinary, ignored);
  std::filesystem::remove(uncaughtOutput, ignored);
  binaryOptions.outputPath = uncaughtBinary;
  scalanative::support::DiagnosticEngine uncaughtDiagnostics;
  const scalanative::tools::build::BuildResult uncaught = driver.buildSource(
      "UncaughtNullThrow.scala", uncaughtSource, binaryOptions, uncaughtDiagnostics);
  if (!uncaught.ok) {
    return expect(contains(uncaught.diagnosticsText, "clang toolchain not found"),
                  "uncaught null-throw native build failed: " +
                      uncaught.diagnosticsText);
  }
  const std::string uncaughtCommand =
      uncaughtBinary.string() + " > " + uncaughtOutput.string() + " 2>&1";
  const int uncaughtStatus = std::system(uncaughtCommand.c_str());
  const std::string uncaughtText = readTextFile(uncaughtOutput);
  std::filesystem::remove(uncaughtBinary, ignored);
  std::filesystem::remove(uncaughtOutput, ignored);
  return expect(
      uncaughtStatus != 0 &&
          contains(uncaught.llvmIr, "@__type_java_lang_NullPointerException =") &&
          contains(uncaught.llvmIr,
                   "call ptr "
                   "@__scalanative_require_non_null_thrown_exception(ptr null)") &&
          contains(uncaughtText, "Uncaught exception: NullPointerException: "
                                 "Thrown exception cannot be null\n") &&
          contains(uncaughtText, "UncaughtNullThrow.scala:") &&
          !contains(uncaughtText, "after\n"),
      "optimized uncaught null throw did not retain and report its typed failure");
}

int smokeUncaughtThrowRuntime() {
  constexpr const char* source = R"(package demo.exceptions

class Failure(val detail: String) extends Exception(detail) {
  override def toString: String = "Failure: " + getMessage
}

class RootCause extends Exception("root cause") {
  override def toString: String = "RootCause: " + getMessage
}

class DefaultFailure(message: String) extends Exception(message)
trait CopyMarker
class CopyBase(val value: Int)
class CopyChild(value: Int) extends CopyBase(value) with CopyMarker
class CopySibling(value: Int) extends CopyBase(value)
class PrintFormattingFailure extends Exception("print formatter") {
  override def toString: String = throw new Exception("print formatter failed")
}
class FatalError extends Error("fatal")

class ZoneValue(val code: Int)

object CauseRules {
  def freshCause(): Throwable =
    new Exception("fresh").getCause

  def explicitNullCause(): Throwable = {
    val value = new Exception("explicit null")
    value.initCause(null)
    value.getCause
  }

  def repeatedInitialization(): String = {
    val value = new Exception("repeat")
    value.initCause(null)
    try {
      value.initCause(new Exception("late"))
      "missed repeated initialization"
    } catch {
      case error: IllegalStateException => error.getMessage
    }
  }

  def selfCausation(): String = {
    val value = new Exception("self")
    try {
      value.initCause(value)
      "missed self-causation"
    } catch {
      case error: IllegalArgumentException => error.getMessage
    }
  }

  def nullSuppressed(): String = {
    val value = new Exception("null suppressed")
    try {
      value.addSuppressed(null)
      "missed null suppressed"
    } catch {
      case error: IllegalArgumentException => error.getMessage
    }
  }

  def selfSuppression(): String = {
    val value = new Exception("self suppressed")
    try {
      value.addSuppressed(value)
      "missed self-suppression"
    } catch {
      case error: IllegalArgumentException => error.getMessage
    }
  }

  def nullStackTrace(): String = {
    val value = new Exception("null stack trace")
    try {
      value.setStackTrace(null)
      "missed null stack trace"
    } catch {
      case error: NullPointerException => error.getMessage
    }
  }

  def preservedStackTraceMessage(
      value: Throwable,
      before: Array[StackTraceElement],
      error: NullPointerException
  ): String = {
    val after = value.getStackTrace
    error.getMessage + "|" + before(0).functionName + "|" +
      after(0).functionName
  }

  def nullStackFrame(): String = {
    val value = new Exception("null stack frame")
    val before = value.getStackTrace
    try {
      value.setStackTrace(Array[StackTraceElement](null))
      "missed null stack frame"
    } catch {
      case error: NullPointerException =>
        preservedStackTraceMessage(value, before, error)
    }
  }
}

object HandlerLocalRules {
  def liveAcrossCatch(): String = {
    var booleanValue = false
    var intValue = 1
    var longValue = 2L
    var floatValue = 1.0F
    var doubleValue = 2.0
    var charValue = 'a'
    var stringValue = "before"
    var referenceValue: DefaultFailure = null
    try {
      booleanValue = true
      intValue = 7
      longValue = 8L
      floatValue = 2.5F
      doubleValue = 3.5
      charValue = 'z'
      stringValue = "after"
      referenceValue = new DefaultFailure("retained")
      throw new DefaultFailure("trigger")
    } catch {
      case caught: DefaultFailure =>
        booleanValue + "|" + intValue + "|" + longValue + "|" +
          (floatValue == 2.5F) + "|" + (doubleValue == 3.5) + "|" +
          charValue + "|" + stringValue + "|" + referenceValue.getMessage +
          "|" + caught.getMessage
    }
  }
}

object ArrayRules {
  def nullLength(): String = {
    val values: Array[Int] = null
    try {
      println(values.length)
      "missed null array length"
    } catch {
      case error: NullPointerException => error.getMessage
    }
  }

  def negativeRead(): String = {
    val values = Array(7)
    try {
      println(values(-1))
      "missed negative array index"
    } catch {
      case error: ArrayIndexOutOfBoundsException => error.getMessage
    }
  }

  def preservedWriteMessage(
      values: Array[Int],
      error: IndexOutOfBoundsException
  ): String =
    error.getMessage + "|" + values(0)

  def upperWrite(): String = {
    val values = Array(7)
    try {
      values(1) = 9
      "missed upper array index"
    } catch {
      case error: IndexOutOfBoundsException =>
        preservedWriteMessage(values, error)
    }
  }

  def referenceRead(): String = {
    val values = Array[Throwable](new Exception("entry"))
    try {
      println(values(1))
      "missed reference array index"
    } catch {
      case error: ArrayIndexOutOfBoundsException => error.getMessage
    }
  }

  def dynamicDefaults(length: Int): String = {
    val ints = new Array[Int](length)
    val booleans = new Array[Boolean](length)
    val longs = new Array[Long](length)
    val doubles = new Array[Double](length)
    val floats = new Array[Float](length)
    val chars = new Array[Char](length)
    val strings = new Array[String](length)
    val failures = new Array[DefaultFailure](length)
    val anyValues = new Array[Any](length)
    ints(1) = 9
    chars(0) = 'z'
    ints.length + "|" + ints(0) + "|" + ints(1) + "|" + booleans(0) +
      "|" + longs(0) + "|" + doubles(0) + "|" + floats(0) + "|" +
      chars(0) + "|" + (strings(0) == null) + "|" +
      (failures(0) == null) + "|" + (anyValues(0) == null)
  }

  def emptyBehavior(): String = {
    val booleans = Array.empty[Boolean]
    val ints = Array.empty[Int]
    val longs = Array.empty[Long]
    val doubles = Array.empty[Double]
    val floats = Array.empty[Float]
    val chars = Array.empty[Char]
    val strings = Array.empty[String]
    val failures = Array.empty[DefaultFailure]
    val anyValues = Array.empty[Any]
    val matrix = Array.empty[Array[Int]]
    booleans.length + "|" + ints.length + "|" + longs.length + "|" +
      doubles.length + "|" + floats.length + "|" + chars.length + "|" +
      strings.length + "|" + failures.length + "|" + anyValues.length + "|" +
      matrix.length
  }

  def fillBehavior(): String = {
    var counter = 0
    val ints = Array.fill[Int](3)({
      counter = counter + 1
      counter
    })
    val skipped = Array.fill[Int](0)({
      counter = counter + 100
      counter
    })
    var lengthCalls = 0
    val fixed = Array.fill[Int]({
      lengthCalls = lengthCalls + 1
      2
    })(9)
    val booleans = Array.fill[Boolean](2)(true)
    val chars = Array.fill[Char](2)('q')
    val floats = Array.fill[Float](2)(1.5F)
    val longs = Array.fill[Long](2)(8L)
    val doubles = Array.fill[Double](2)(2.5)
    val strings = Array.fill[String](2)("filled")
    val failures =
      Array.fill[DefaultFailure](2)(new DefaultFailure("filled"))
    val anyValues = Array.fill[Any](2)(7)
    val matrix = Array.fill[Array[Int]](2)(Array(1))
    matrix(0)(0) = 9
    val integerValues =
      ints(0) + "," + ints(1) + "," + ints(2) + "|" + counter
    val scalarValues =
      skipped.length + "|" + lengthCalls + "|" + fixed(1) + "|" +
        booleans(1) + "|" + chars(1) + "|" + (floats(0) == 1.5F) + "|" +
        longs(1) + "|" + (doubles(0) == 2.5) + "|" + strings(0)
    val referenceValues =
      (failures(0) == failures(1)) + "|" +
        anyValues(1).asInstanceOf[Int] + "|" + matrix(0)(0) + "|" +
        matrix(1)(0) + "|" + (matrix(0) == matrix(1))
    integerValues + "|" + scalarValues + "|" + referenceValues
  }

  def multiFillBehavior(): String = {
    var dimensionCalls = 0
    var elementCalls = 0
    val matrix = Array.fill[Int]({
      dimensionCalls = dimensionCalls + 1
      2
    }, {
      dimensionCalls = dimensionCalls + 1
      3
    })({
      elementCalls = elementCalls + 1
      elementCalls
    })
    matrix(0)(0) = 20
    val cube = Array.fill[String](2, 1, 2)("cell")
    val failures =
      Array.fill[DefaultFailure](2, 2)(new DefaultFailure("cell"))
    val skipped = Array.fill[Int](0, 0 - 1)({
      elementCalls = elementCalls + 100
      elementCalls
    })
    var negativeElementCalls = 0
    val negative = try {
      println(Array.fill[Int](1, 0 - 1)({
        negativeElementCalls = negativeElementCalls + 1
        7
      }).length)
      "missed negative inner fill dimension"
    } catch {
      case failure: NegativeArraySizeException => failure.getMessage
    }
    matrix.length + "|" + matrix(0).length + "|" + matrix(0)(0) + "|" +
      matrix(0)(1) + "|" + matrix(1)(0) + "|" + elementCalls + "|" +
      dimensionCalls + "|" + (matrix(0) == matrix(1)) + "|" +
      cube(1)(0)(1) + "|" + (failures(0) == failures(1)) + "|" +
      (failures(0)(0) == failures(0)(1)) + "|" +
      (failures(0)(0) == failures(1)(0)) + "|" + skipped.length + "|" +
      negative + "|" + negativeElementCalls
  }

  def rangeBehavior(): String = {
    var argumentCalls = 0
    val ascending = Array.range({
      argumentCalls = argumentCalls + 1
      1
    }, {
      argumentCalls = argumentCalls + 1
      8
    }, {
      argumentCalls = argumentCalls + 1
      2
    })
    val defaults = Array.range(2, 5)
    val descending = Array.range(7, 0, 0 - 3)
    val emptyPositive = Array.range(5, 1, 2)
    val emptyNegative = Array.range(1, 5, 0 - 2)
    val minimum = (0 - 2147483647) - 1
    val minimumStep = Array.range(2147483647, minimum, minimum)
    ascending.length + "|" + ascending(0) + "|" + ascending(1) + "|" +
      ascending(2) + "|" + ascending(3) + "|" + defaults.length + "|" +
      defaults(0) + "|" + defaults(2) + "|" + descending.length + "|" +
      descending(0) + "|" + descending(1) + "|" + descending(2) + "|" +
      emptyPositive.length + "|" + emptyNegative.length + "|" +
      minimumStep.length + "|" + minimumStep(0) + "|" + minimumStep(1) +
      "|" + argumentCalls
  }

  def rangeFailures(): String = {
    val zeroStep = try {
      println(Array.range(1, 5, 0).length)
      "missed zero range step"
    } catch {
      case failure: IllegalArgumentException => failure.getMessage
    }
    val minimum = (0 - 2147483647) - 1
    val tooLarge = try {
      println(Array.range(minimum, 2147483647).length)
      "missed oversized array range"
    } catch {
      case failure: IllegalArgumentException => failure.getMessage
    }
    zeroStep + "|" + tooLarge
  }

  def concatBehavior(): String = {
    var argumentCalls = 0
    val ints = Array.concat[Int](
      {
        argumentCalls = argumentCalls + 1
        Array(1, 2)
      },
      {
        argumentCalls = argumentCalls + 1
        Array.empty[Int]
      },
      {
        argumentCalls = argumentCalls + 1
        Array(3, 4)
      }
    )
    val empty = Array.concat[Int]()
    val booleans = Array.concat[Boolean](Array(true), Array(false))
    val chars = Array.concat[Char](Array('a'), Array('b'))
    val floats = Array.concat[Float](Array(1.5F), Array(2.5F))
    val longs = Array.concat[Long](Array(7L), Array(8L))
    val doubles = Array.concat[Double](Array(2.5), Array(3.5))
    val strings =
      Array.concat[String](Array("left"), Array.empty[String], Array("right"))
    val failure = new DefaultFailure("first")
    val failures = Array.concat[DefaultFailure](
      Array[DefaultFailure](failure),
      Array[DefaultFailure](new DefaultFailure("second"))
    )
    val anyValues =
      Array.concat[Any](Array[Any](1), Array.empty[Any], Array[Any](7))
    val first = Array(1)
    val second = Array(2)
    val nested = Array.concat[Array[Int]](
      Array[Array[Int]](first),
      Array[Array[Int]](second)
    )
    nested(0)(0) = 9
    ints.length + "|" + ints(0) + "|" + ints(1) + "|" + ints(2) + "|" +
      ints(3) + "|" + argumentCalls + "|" + empty.length + "|" +
      booleans(1) + "|" + chars(1) + "|" + (floats(1) == 2.5F) + "|" +
      longs(1) + "|" + (doubles(1) == 3.5) + "|" + strings(1) + "|" +
      (failures(0) == failure) + "|" + anyValues(1).asInstanceOf[Int] + "|" +
      first(0) + "|" + second(0) + "|" + (nested(0) == first)
  }

  def concatFailure(): String = {
    val missing: Array[Int] = null
    var argumentCalls = 0
    try {
      println(Array.concat[Int](
        {
          argumentCalls = argumentCalls + 1
          Array(1)
        },
        {
          argumentCalls = argumentCalls + 1
          missing
        },
        {
          argumentCalls = argumentCalls + 1
          Array(3)
        }
      ).length)
      "missed null concat input"
    } catch {
      case failure: NullPointerException =>
        failure.getMessage + "|" + argumentCalls
    }
  }

  def negativeFill(): String = {
    var evaluated = 0
    try {
      println(Array.fill[Int](0 - 1)({
        evaluated = 1
        7
      }).length)
      "missed negative fill length"
    } catch {
      case failure: NegativeArraySizeException =>
        failure.getMessage + "|" + evaluated
    }
  }

  def nestedOfDim(rows: Int, columns: Int): String = {
    val matrix = Array.ofDim[Array[Int]](rows)
    val row = Array.ofDim[Int](columns)
    row(1) = 7
    matrix(0) = row
    val literal = Array[Array[Int]](row)
    val selected = matrix(0)
    val literalRow = literal(0)
    matrix.length + "|" + (matrix(1) == null) + "|" + selected.length +
      "|" + selected(0) + "|" + selected(1) + "|" + literalRow(1)
  }

  def multiDimensional(rows: Int, columns: Int): String = {
    val matrix = Array.ofDim[Int](rows, columns)
    matrix(0)(1) = 11
    val cube = Array.ofDim[String](2, 2, 2)
    cube(1)(0)(1) = "deep"
    matrix.length + "|" + matrix(0).length + "|" + matrix(0)(0) + "|" +
      matrix(0)(1) + "|" + matrix(1)(0) + "|" + cube.length + "|" +
      cube(1).length + "|" + cube(1)(0).length + "|" + cube(1)(0)(1)
  }

  def cloneBehavior(): String = {
    val ints = Array(1, 2, 3)
    val intCopy = ints.clone()
    intCopy(1) = 9
    val booleans = Array[Boolean](true, false).clone()
    val longs = Array[Long](3L, 4L).clone()
    val doubles = Array[Double](1.5, 2.5).clone()
    val floats = Array[Float](1.5F, 2.5F).clone()
    val chars = Array[Char]('a', 'b').clone()
    val strings = Array[String]("left", "right").clone()
    val shared = new DefaultFailure("shared")
    val references = Array[DefaultFailure](shared).clone()
    val matrix = Array.ofDim[Int](1, 2)
    matrix(0)(1) = 7
    val matrixCopy = matrix.clone()
    matrixCopy(0)(1) = 8
    matrixCopy(0) = Array(4, 5)
    ints(1) + "|" + intCopy(1) + "|" + booleans(1) + "|" + longs(1) +
      "|" + (doubles(0) == 1.5) + "|" + (floats(0) == 1.5F) + "|" +
      chars(1) + "|" + strings(1) + "|" + (references(0) == shared) +
      "|" + matrix(0)(1) + "|" + matrixCopy(0)(1) + "|" +
      (matrix(0) == matrixCopy(0))
  }

  def nullClone(): String = {
    val values: Array[Int] = null
    try {
      println(values.clone().length)
      "missed null array clone"
    } catch {
      case failure: NullPointerException => "null clone: " + failure.getMessage
    }
  }

  def copyBehavior(): String = {
    val forward = Array(1, 2, 3, 4, 5)
    Array.copy(forward, 0, forward, 1, 4)
    val backward = Array(1, 2, 3, 4, 5)
    Array.copy(backward, 1, backward, 0, 4)
    val booleans = Array[Boolean](false, false)
    Array.copy(Array[Boolean](true, false), 0, booleans, 0, 2)
    val chars = Array[Char]('a', 'b')
    Array.copy(Array[Char]('x', 'y'), 0, chars, 0, 2)
    val floats = Array[Float](0.0F)
    Array.copy(Array[Float](1.5F), 0, floats, 0, 1)
    val longs = Array[Long](0L)
    Array.copy(Array[Long](9L), 0, longs, 0, 1)
    val doubles = Array[Double](0.0)
    Array.copy(Array[Double](2.5), 0, doubles, 0, 1)
    val strings = Array[String]("empty")
    Array.copy(Array[String]("right"), 0, strings, 0, 1)
    val shared = new DefaultFailure("copied")
    val references = Array[DefaultFailure](null)
    Array.copy(Array[DefaultFailure](shared), 0, references, 0, 1)
    val sourceMatrix = Array.ofDim[Int](1, 2)
    sourceMatrix(0)(1) = 7
    val destinationMatrix = Array.ofDim[Int](1, 2)
    Array.copy(sourceMatrix, 0, destinationMatrix, 0, 1)
    destinationMatrix(0)(1) = 8
    forward(0) + "," + forward(1) + "," + forward(2) + "," +
      forward(3) + "," + forward(4) + "|" + backward(0) + "," +
      backward(1) + "," + backward(2) + "," + backward(3) + "," +
      backward(4) + "|" + booleans(0) + "|" + booleans(1) + "|" +
      chars(1) + "|" + (floats(0) == 1.5F) + "|" + longs(0) + "|" +
      (doubles(0) == 2.5) + "|" + strings(0) + "|" +
      (references(0) == shared) + "|" + sourceMatrix(0)(1)
  }

  def copyFailures(): String = {
    val source = Array(1, 2)
    val destination = Array(0, 0)
    val negative = try {
      Array.copy(source, 0, destination, 0, 0 - 1)
      "missed negative copy length"
    } catch {
      case failure: ArrayIndexOutOfBoundsException => failure.getMessage
    }
    val overflow = try {
      Array.copy(source, 1, destination, 0, 2)
      "missed overflowing copy range"
    } catch {
      case failure: ArrayIndexOutOfBoundsException => failure.getMessage
    }
    negative + "|" + overflow + "|" + destination(0)
  }

  def referenceCopyBehavior(): String = {
    val first = new CopyChild(4)
    val second = new CopyChild(7)
    val children = Array[CopyChild](first, second)
    val bases = new Array[CopyBase](2)
    Array.copy(children, 0, bases, 0, 2)
    val markers = new Array[CopyMarker](2)
    Array.copy(children, 0, markers, 0, 2)
    val anyValues = new Array[Any](2)
    Array.copy(children, 0, anyValues, 0, 2)
    val nullable = Array[CopyBase](null, first)
    val narrowed = new Array[CopyChild](2)
    Array.copy(nullable, 0, narrowed, 0, 2)
    val baseValue = bases(0).value
    val baseIdentity = bases(1) == second
    val markerIdentity = markers(0) == first
    val anyIdentity = anyValues(1) == second
    val nullAccepted = narrowed(0) == null
    val narrowedIdentity = narrowed(1) == first
    baseValue + "|" + baseIdentity + "|" + markerIdentity + "|" + anyIdentity +
      "|" + nullAccepted + "|" + narrowedIdentity
  }

  def referenceCopyFailure(): String = {
    val first = new CopyChild(11)
    val source = Array[CopyBase](first, new CopySibling(12), new CopyChild(13))
    val destination = new Array[CopyChild](3)
    try {
      Array.copy(source, 0, destination, 0, 3)
      "missed incompatible array element"
    } catch {
      case failure: ArrayStoreException =>
        failure.getMessage + "|" + (destination(0) == first) + "|" +
          (destination(1) == null) + "|" + (destination(2) == null)
    }
  }

  def nullCopy(): String = {
    val source: Array[Int] = null
    val destination = Array(0)
    try {
      Array.copy(source, 0, destination, 0, 0)
      "missed null array copy"
    } catch {
      case failure: NullPointerException => "null copy: " + failure.getMessage
    }
  }

  def negativeInnerDimension(): String =
    try {
      println(Array.ofDim[Int](0, 0 - 1).length)
      "missed negative inner dimension"
    } catch {
      case failure: NegativeArraySizeException =>
        "negative inner dimension: " + failure.getMessage
    }

  def negativeSize(): String =
    try {
      println(new Array[Double](0 - 1).length)
      "missed negative array size"
    } catch {
      case error: Error => "NegativeArraySizeException incorrectly matched Error"
      case failure: NegativeArraySizeException =>
        "negative array: " + failure.getMessage + "|" + failure.getCause
    }
}

object RuntimeRules {
  def nullArrayAsRuntimeException(): String = {
    val values: Array[Int] = null
    try {
      println(values.length)
      "missed RuntimeException catch"
    } catch {
      case error: RuntimeException => "runtime parent: " + error.getMessage
    }
  }
}

object ArithmeticRules {
  def divideInt(value: Int, divisor: Int): Int = value / divisor
  def divideLong(value: Long, divisor: Long): Long = value / divisor
  def remainderLong(value: Long, divisor: Long): Long = value % divisor

  def intDivisionByZero(): String =
    try {
      divideInt(42, 0)
      "missed Int division by zero"
    } catch {
      case error: ArithmeticException =>
        "int arithmetic: " + error.getMessage + "|" + error.getCause
    }

  def longRemainderByZero(): String =
    try {
      remainderLong(42L, 0L)
      "missed Long remainder by zero"
    } catch {
      case error: RuntimeException =>
        "long arithmetic parent: " + error.getMessage
    }

  def minimumDivision(): String = {
    val minInt = (0 - 2147483647) - 1
    val minLong = (0L - 9223372036854775807L) - 1L
    divideInt(minInt, 0 - 1) + "|" + divideLong(minLong, 0L - 1L)
  }

  def floatingDivision(): Boolean =
    (1.0 / 0.0) > 0.0
}

object AssertionRules {
  def behavior(): String = {
    assert(true)
    try {
      assert(false)
      "missed failed assertion"
    } catch {
      case exception: Exception => "AssertionError incorrectly matched Exception"
      case error: AssertionError =>
        "assertion branch: " + error.getMessage + "|" + error.getCause
    }
  }
}

object AssumptionRules {
  def behavior(): String = {
    assume(true)
    try {
      assume(false)
      "missed failed assumption"
    } catch {
      case exception: Exception => "AssertionError incorrectly matched Exception"
      case error: AssertionError =>
        "assumption branch: " + error.getMessage + "|" + error.getCause
    }
  }
}

object NotImplementedRules {
  def pending(): String = ???

  def behavior(): String =
    try {
      pending()
      "missed unimplemented expression"
    } catch {
      case exception: Exception =>
        "NotImplementedError incorrectly matched Exception"
      case error: NotImplementedError =>
        "not implemented branch: " + error.getMessage + "|" + error.getCause
    }
}

object PrintTraceRules {
  def formattingFailure(): String =
    try {
      new PrintFormattingFailure().printStackTrace()
      "missed print formatting failure"
    } catch {
      case failure: Exception =>
        "print formatting branch: " + failure.getMessage
    }

  def behavior(): String = {
    val failure = new DefaultFailure("printed root")
    failure.initCause(new RootCause())
    failure.addSuppressed(new DefaultFailure("printed suppressed"))
    failure.fillInStackTrace
    failure.printStackTrace()
    "continued after printStackTrace"
  }
}

object RequirementRules {
  def behavior(): String = {
    require(true)
    try {
      require(false)
      "missed failed requirement"
    } catch {
      case error: Error => "IllegalArgumentException incorrectly matched Error"
      case failure: IllegalArgumentException =>
        "requirement branch: " + failure.getMessage + "|" + failure.getCause
    }
  }
}

object ErrorRules {
  def siblingDispatch(): String =
    try {
      throw new FatalError()
    } catch {
      case exception: Exception => "Error incorrectly matched Exception"
      case error: Error =>
        "error branch: " + error.getMessage + "|" + error.getCause
    }
}

object Thrower {
  def ordinaryZone(): Int =
    Zone.scoped({
      val value = new ZoneValue(17)
      value.code
    })

  def crossZoneFailure(): Int =
    Zone.scoped({
      Zone.scoped({
        if (true) throw new DefaultFailure("zoned") else 0
      })
    })

  def crossZone(): String =
    try {
      crossZoneFailure()
      "missed cross-zone throw"
    } catch {
      case failure: DefaultFailure => failure.toString
    }

  def handled(): String =
    throw new Exception("handled")

  def chained(): Exception = {
    val outer = new Exception("outer")
    outer.initCause(new Exception("root"))
    outer
  }

  def captureCause(): Throwable =
    try {
      throw new RootCause()
    } catch {
      case cause: Throwable => cause
    }

  def captureFailure(): Failure =
    try {
      throw new Failure("boom")
    } catch {
      case failure: Failure => failure
    }

  def relayFailure(failure: DefaultFailure): DefaultFailure =
    try {
      throw failure
    } catch {
      case caught: DefaultFailure => caught
    }

  def refillFailure(failure: Failure): Throwable =
    failure.fillInStackTrace

  def replaceTrace(failure: Failure): String = {
    val input = Array(
      new StackTraceElement("custom.Entry", "Synthetic.scala", 7, 3),
      new StackTraceElement("custom.Helper", "Synthetic.scala", 8, 4)
    )
    failure.setStackTrace(input)
    input(0) = new StackTraceElement("mutated.Input", "Changed.scala", 1, 1)
    val firstCopy = failure.getStackTrace
    val firstText = firstCopy(0).toString
    firstCopy(0) = new StackTraceElement("mutated.Copy", "Changed.scala", 2, 2)
    val freshCopy = failure.getStackTrace
    firstText + "|" + freshCopy(0).toString + "|" + freshCopy.length
  }

  def explicitEmptyTrace(): Int = {
    val failure = new DefaultFailure("empty trace")
    failure.setStackTrace(Array[StackTraceElement]())
    try {
      throw failure
    } catch {
      case handled: DefaultFailure => handled.getStackTrace.length
    }
  }

  def addSuppressedFailures(failure: Throwable): Unit = {
    failure.addSuppressed(new Exception("first suppressed"))
    failure.addSuppressed(new RootCause())
  }

  def inspectSuppressed(failure: Throwable): String = {
    val firstCopy = failure.getSuppressed
    val firstMessage = firstCopy(0).getMessage
    firstCopy(0) = new Exception("mutated suppressed")
    val freshCopy = failure.getSuppressed
    firstMessage + "|" + freshCopy(0).getMessage + "|" +
      freshCopy(1).getMessage + "|" + freshCopy.length
  }

  def detachedSuppressed(): Array[Throwable] = {
    val owner = new Exception("detached owner")
    owner.addSuppressed(new Exception("detached"))
    owner.getSuppressed
  }

  def replacedSuppressed(): Array[Throwable] = {
    val owner = new Exception("replacement owner")
    owner.addSuppressed(new Exception("original"))
    val copy = owner.getSuppressed
    copy(0) = new Exception("replacement")
    copy
  }

  def inspectFailure(failure: Failure): String = {
    val frames = failure.getStackTrace
    val originalName = frames(0).functionName
    frames(0) = frames(1)
    val changedName = frames(0).functionName
    val fresh = failure.getStackTrace
    val first = fresh(0)
    val validLine = if (first.lineNumber > 0) 1 else 0
    val validColumn = if (first.columnNumber > 0) 1 else 0
    originalName + "|" + changedName + "|" + first.functionName + "|" +
      first.fileName + "|" + validLine + "|" + validColumn + "|" + first.toString
  }

  def fail(detailed: Failure, cause: Throwable): Int = {
    detailed.initCause(cause)
    val failure: Throwable = detailed
    throw failure
  }
}

object Main {
  def main = {
    println(Thrower.ordinaryZone())
    println(Thrower.crossZone())
    gcCollect()
    println(gcLiveObjectCount())
    println(new DefaultFailure("unthrown").getStackTrace.length)
    val constructed = new DefaultFailure("constructed")
    val constructedFrames = constructed.getStackTrace
    println(constructedFrames(0).functionName)
    val relayed = Thrower.relayFailure(constructed)
    val relayedFrames = relayed.getStackTrace
    println(relayedFrames(0).functionName)
    println(new DefaultFailure("dynamic").toString)
    println(new DefaultFailure("").toString)
    println(new DefaultFailure(null).toString)
    println(new StackTraceElement(null, null, 0, 0).toString)
    println(Thrower.explicitEmptyTrace())
    println(new DefaultFailure("none").getSuppressed.length)
    println(CauseRules.freshCause())
    println(CauseRules.explicitNullCause())
    println(CauseRules.repeatedInitialization())
    println(CauseRules.selfCausation())
    println(CauseRules.nullSuppressed())
    println(CauseRules.selfSuppression())
    println(CauseRules.nullStackTrace())
    println(CauseRules.nullStackFrame())
    println(ArrayRules.nullLength())
    println(ArrayRules.negativeRead())
    println(ArrayRules.upperWrite())
    println(ArrayRules.referenceRead())
    println(ArrayRules.dynamicDefaults(3))
    println(ArrayRules.emptyBehavior())
    println(ArrayRules.fillBehavior())
    println(ArrayRules.multiFillBehavior())
    println(ArrayRules.rangeBehavior())
    println(ArrayRules.rangeFailures())
    println(ArrayRules.concatBehavior())
    println(ArrayRules.concatFailure())
    println(HandlerLocalRules.liveAcrossCatch())
    println(ArrayRules.negativeFill())
    println(ArrayRules.nestedOfDim(2, 3))
    println(ArrayRules.multiDimensional(2, 3))
    println(ArrayRules.cloneBehavior())
    println(ArrayRules.nullClone())
    println(ArrayRules.copyBehavior())
    println(ArrayRules.copyFailures())
    println(ArrayRules.referenceCopyBehavior())
    println(ArrayRules.referenceCopyFailure())
    println(ArrayRules.nullCopy())
    println(ArrayRules.negativeInnerDimension())
    println(ArrayRules.negativeSize())
    println(RuntimeRules.nullArrayAsRuntimeException())
    println(ArithmeticRules.intDivisionByZero())
    println(ArithmeticRules.longRemainderByZero())
    println(ArithmeticRules.minimumDivision())
    println(ArithmeticRules.floatingDivision())
    println(AssertionRules.behavior())
    println(AssumptionRules.behavior())
    println(NotImplementedRules.behavior())
    println(PrintTraceRules.formattingFailure())
    println(PrintTraceRules.behavior())
    println(RequirementRules.behavior())
    println(ErrorRules.siblingDispatch())
    val detached = Thrower.detachedSuppressed()
    gcCollect()
    println(detached(0).getMessage)
    val replaced = Thrower.replacedSuppressed()
    gcCollect()
    println(replaced(0).getMessage)
    val retainedCause = Thrower.captureCause()
    val retainedFailure = Thrower.captureFailure()
    val outer = Thrower.chained()
    println(Thrower.inspectFailure(retainedFailure))
    val refilled = Thrower.refillFailure(retainedFailure)
    println(refilled.getMessage)
    val refilledFrames = refilled.getStackTrace
    println(refilledFrames(0).functionName)
    println(Thrower.replaceTrace(retainedFailure))
    Thrower.addSuppressedFailures(retainedFailure)
    gcCollect()
    println(Thrower.inspectSuppressed(retainedFailure))
    println(outer.getMessage)
    println(outer.getCause.getMessage)
    println(try Thrower.handled() catch {
      case error: Throwable => error.getMessage
    })
    println("before")
    Zone.scoped({
      Zone.scoped({
        Thrower.fail(retainedFailure, retainedCause)
      })
    })
    println("after")
  }
}
)";

  scalanative::tools::build::BuildDriver driver;
  scalanative::support::DiagnosticEngine diagnostics;
  const scalanative::tools::build::BuildResult result =
      driver.buildSource("UncaughtThrow.scala", source, {}, diagnostics);
  if (int code = expect(result.ok, "uncaught throw build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  const std::string handlerFreeMutableName =
      "define ptr @demo_exceptions_ArrayRules_fillBehavior()";
  const std::size_t handlerFreeMutableStart =
      result.llvmIr.find(handlerFreeMutableName);
  const std::size_t handlerFreeMutableEnd =
      handlerFreeMutableStart == std::string::npos
          ? std::string::npos
          : result.llvmIr.find("\n}\n", handlerFreeMutableStart);
  const std::string handlerFreeMutableIr =
      handlerFreeMutableStart == std::string::npos ||
              handlerFreeMutableEnd == std::string::npos
          ? std::string{}
          : result.llvmIr.substr(handlerFreeMutableStart,
                                 handlerFreeMutableEnd - handlerFreeMutableStart);
  if (int code = expect(
          !handlerFreeMutableIr.empty() &&
              !contains(handlerFreeMutableIr, "volatile") &&
              contains(result.nirText,
                       "define @demo.exceptions.Thrower.fail : "
                       "(demo.exceptions.Failure,java.lang.Throwable)Int") &&
              contains(result.nirText,
                       "class @java.lang.Exception : @java.lang.Throwable") &&
              contains(result.nirText,
                       "class @java.lang.Error : @java.lang.Throwable") &&
              contains(result.nirText,
                       "class @java.lang.AssertionError : @java.lang.Error") &&
              contains(result.nirText,
                       "class @scala.NotImplementedError : @java.lang.Error") &&
              contains(result.nirText,
                       "class @demo.exceptions.FatalError : @java.lang.Error") &&
              contains(result.nirText, "class @java.lang.RuntimeException : "
                                       "@java.lang.Exception") &&
              contains(result.nirText, "class @java.lang.ArithmeticException : "
                                       "@java.lang.RuntimeException") &&
              contains(result.nirText,
                       "field @java.lang.Throwable.message$field : String") &&
              contains(result.nirText, "field @java.lang.Throwable.cause : "
                                       "java.lang.Throwable") &&
              contains(result.nirText,
                       "field @java.lang.Throwable.stackTrace : Long") &&
              contains(result.nirText, "define @java.lang.Throwable.getMessage") &&
              contains(result.nirText, "define @java.lang.Throwable.getCause") &&
              contains(result.nirText, "define @java.lang.Throwable.initCause") &&
              contains(result.nirText,
                       "define @java.lang.Throwable.fillInStackTrace") &&
              contains(result.nirText, "define @java.lang.Throwable.getStackTrace") &&
              contains(result.nirText, "define @java.lang.Throwable.setStackTrace") &&
              contains(result.nirText, "define @java.lang.Throwable.addSuppressed") &&
              contains(result.nirText, "define @java.lang.Throwable.getSuppressed") &&
              contains(result.nirText, "define @java.lang.Throwable.printStackTrace") &&
              contains(result.nirText, "class @scala.scalanative.runtime."
                                       "SuppressedExceptionNode") &&
              contains(result.nirText,
                       "field @java.lang.Throwable.suppressedHead : "
                       "scala.scalanative.runtime.SuppressedExceptionNode") &&
              contains(result.nirText,
                       "field @java.lang.Throwable.suppressedCount : Int") &&
              contains(result.nirText,
                       "field @scala.scalanative.runtime."
                       "SuppressedExceptionNode.exception : java.lang.Throwable") &&
              contains(result.nirText, "class @java.lang.StackTraceElement") &&
              contains(result.nirText,
                       "field @java.lang.StackTraceElement.functionName$field : "
                       "String") &&
              contains(result.nirText,
                       "field @java.lang.StackTraceElement.fileName$field : String") &&
              contains(result.nirText,
                       "field @java.lang.StackTraceElement.lineNumber$field : Int") &&
              contains(result.nirText,
                       "field @java.lang.StackTraceElement.columnNumber$field : Int") &&
              contains(result.nirText,
                       "define @java.lang.StackTraceElement.toString") &&
              contains(result.nirText, "class @java.lang.IllegalArgumentException : "
                                       "@java.lang.RuntimeException") &&
              contains(result.nirText, "class @java.lang.IllegalStateException : "
                                       "@java.lang.RuntimeException") &&
              contains(result.nirText, "class @java.lang.NullPointerException : "
                                       "@java.lang.RuntimeException") &&
              contains(result.nirText, "class @java.lang.ClassCastException : "
                                       "@java.lang.RuntimeException") &&
              contains(result.nirText, "class @java.lang.ArrayStoreException : "
                                       "@java.lang.RuntimeException") &&
              contains(result.nirText, "class @java.lang.IndexOutOfBoundsException : "
                                       "@java.lang.RuntimeException") &&
              contains(result.nirText,
                       "class @java.lang.ArrayIndexOutOfBoundsException : "
                       "@java.lang.IndexOutOfBoundsException") &&
              contains(result.nirText, "class @java.lang.NegativeArraySizeException : "
                                       "@java.lang.RuntimeException") &&
              contains(result.nirText, "define @java.lang.Exception.toString") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.throwableToString : "
                       "(java.lang.Throwable)String") &&
              contains(result.nirText, "declare @scala.scalanative.runtime.assert : "
                                       "(Boolean)Unit") &&
              contains(result.nirText, "declare @scala.scalanative.runtime.assume : "
                                       "(Boolean)Unit") &&
              contains(result.nirText, "declare @scala.scalanative.runtime.require : "
                                       "(Boolean)Unit") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.assert(true)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.assert(false)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.assume(true)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.assume(false)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.require(true)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.require(false)") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.intArrayAlloc : "
                       "(Int)Array [ Int ]") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.intArrayAlloc(%length)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.booleanArrayAlloc(%length)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.longArrayAlloc(%length)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.doubleArrayAlloc(%length)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.floatArrayAlloc(%length)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.charArrayAlloc(%length)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayAlloc(%length)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.referenceArrayAlloc."
                       "demo.exceptions.DefaultFailure(%length)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.booleanArrayAlloc(0)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.intArrayAlloc(0)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.longArrayAlloc(0)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.doubleArrayAlloc(0)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.floatArrayAlloc(0)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.charArrayAlloc(0)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayAlloc(0)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.referenceArrayAlloc."
                       "demo.exceptions.DefaultFailure(0)") &&
              contains(
                  result.nirText,
                  "call %scala.scalanative.runtime.referenceArrayAlloc.Object(0)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.referenceArrayAlloc."
                       "nested$Array$5bInt$5d(0)") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.arrayFill.Int : "
                       "(Int,Int)Array [ Int ]") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayFill.Boolean(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayFill.Int(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayFill.Char(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayFill.Float(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayFill.Long(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayFill.Double(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayFill.String(") &&
              contains(result.nirText, "call %scala.scalanative.runtime.arrayFill."
                                       "demo.exceptions.DefaultFailure(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayFill.Object(2, "
                       "box[Int](7))") &&
              contains(result.nirText, "call %scala.scalanative.runtime.arrayFill."
                                       "nested$Array$5bInt$5d(") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.arrayFill.Int.2 : "
                       "(Int,Int,Int)Array [ Array [ Int ] ]") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.arrayFill.String.3 : "
                       "(Int,Int,Int,String)Array [ Array [ Array [ String ] ] ]") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.arrayFill."
                       "demo.exceptions.DefaultFailure.2 : "
                       "(Int,Int,demo.exceptions.DefaultFailure)"
                       "Array [ Array [ demo.exceptions.DefaultFailure ] ]") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.arrayRange : "
                       "(Int,Int,Int)Array [ Int ]") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayRange(2, 5, 1)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayRange(7, 0, (0 - 3))") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.arrayConcat.Int.3 : "
                       "(Array [ Int ],Array [ Int ],Array [ Int ])Array [ Int ]") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.arrayConcat.Int.0 : "
                       "()Array [ Int ]") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayConcat.Boolean.2(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayConcat.Char.2(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayConcat.Float.2(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayConcat.Long.2(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayConcat.Double.2(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayConcat.String.3(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayConcat.Object.3(") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.arrayConcat."
                       "demo.exceptions.DefaultFailure.2 : "
                       "(Array [ demo.exceptions.DefaultFailure ],Array [ "
                       "demo.exceptions.DefaultFailure ])Array [ "
                       "demo.exceptions.DefaultFailure ]") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.arrayConcat."
                       "nested$Array$5bInt$5d.2 : "
                       "(Array [ Array [ Int ] ],Array [ Array [ Int ] ])"
                       "Array [ Array [ Int ] ]") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.referenceArrayAlloc."
                       "nested$Array$5bInt$5d : "
                       "(Int)Array [ Array [ Int ] ]") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.referenceArrayAlloc."
                       "nested$Array$5bInt$5d(%rows)") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.referenceArrayApply."
                       "nested$Array$5bInt$5d : "
                       "(Array [ Array [ Int ] ],Int)Array [ Int ]") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.referenceArrayUpdate."
                       "nested$Array$5bInt$5d : "
                       "(Array [ Array [ Int ] ],Int,Array [ Int ])Unit") &&
              contains(result.nirText, "new Array [ Array [ Int ] ](%row)") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.arrayOfDim.Int.2 : "
                       "(Int,Int)Array [ Array [ Int ] ]") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayOfDim.Int.2(%rows, "
                       "%columns)") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.intArrayClone : "
                       "(Array [ Int ])Array [ Int ]") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.intArrayClone(%ints)") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.booleanArrayClone(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.longArrayClone(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.doubleArrayClone(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.floatArrayClone(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.charArrayClone(") &&
              contains(result.nirText, "call %scala.scalanative.runtime.arrayClone(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.referenceArrayClone."
                       "demo.exceptions.DefaultFailure(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.referenceArrayClone."
                       "nested$Array$5bInt$5d(%matrix)") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.arrayCopy.Int : "
                       "(Array [ Int ],Int,Array [ Int ],Int,Int)Unit") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayCopy.Int(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayCopy.Boolean(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayCopy.Char(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayCopy.Float(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayCopy.Long(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayCopy.Double(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.arrayCopy.String(") &&
              contains(result.nirText, "call %scala.scalanative.runtime.arrayCopy."
                                       "demo.exceptions.DefaultFailure(") &&
              contains(result.nirText, "call %scala.scalanative.runtime.arrayCopy."
                                       "nested$Array$5bInt$5d(") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.referenceArrayCopy."
                       "demo.exceptions.CopyChild.to.demo.exceptions.CopyBase : "
                       "(Array [ demo.exceptions.CopyChild ],Int,Array [ "
                       "demo.exceptions.CopyBase ],Int,Int)Unit") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.referenceArrayCopy."
                       "demo.exceptions.CopyChild.to.demo.exceptions.CopyMarker(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.referenceArrayCopy."
                       "demo.exceptions.CopyChild.to.Object(") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.referenceArrayCopy."
                       "demo.exceptions.CopyBase.to.demo.exceptions.CopyChild(") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.arrayOfDim.String.3 : "
                       "(Int,Int,Int)Array [ Array [ Array [ String ] ] ]") &&
              contains(result.nirText,
                       "new scala.NotImplementedError(\"Implementation is "
                       "missing\")") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.throwableToString(%this)") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.fillInStackTrace : "
                       "(java.lang.Throwable)java.lang.Throwable") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.fillInStackTrace(%this)") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.getStackTrace : "
                       "(java.lang.Throwable)Array [ "
                       "java.lang.StackTraceElement ]") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.getStackTrace(%this)") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.setStackTrace : "
                       "(java.lang.Throwable,Array [ "
                       "java.lang.StackTraceElement ])Int") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.setStackTrace(%this, "
                       "%stackTrace)") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.addSuppressed : "
                       "(java.lang.Throwable,java.lang.Throwable)Unit") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.addSuppressed(%this, "
                       "%exception)") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.getSuppressed : "
                       "(java.lang.Throwable)Array [ java.lang.Throwable ]") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.getSuppressed(%this)") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.printStackTrace : "
                       "(java.lang.Throwable)Unit") &&
              contains(result.nirText,
                       "call %scala.scalanative.runtime.printStackTrace(%this)") &&
              contains(result.nirText, "declare @scala.scalanative.runtime."
                                       "stackTraceElementToString : "
                                       "(java.lang.StackTraceElement)String") &&
              contains(result.nirText, "call %scala.scalanative.runtime."
                                       "stackTraceElementToString(%this)") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.referenceArrayApply."
                       "java.lang.StackTraceElement") &&
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.referenceArrayUpdate."
                       "java.lang.StackTraceElement") &&
              contains(result.nirText, "throw %failure") &&
              contains(result.llvmIr, "Runtime ABI = 'cpp-scalanative-runtime-56'") &&
              contains(result.llvmIr, "@__type_java_lang_ArrayStoreException =") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_throw_array_store() "
                       "noreturn") &&
              contains(result.llvmIr,
                       "c\"Array element is not compatible with destination "
                       "type\\00\"") &&
              contains(result.llvmIr, "%element_matches = call i1 "
                                      "@__scalanative_is_instance_of(ptr %element, ptr "
                                      "%target_descriptor)") &&
              contains(result.llvmIr, "array_fill_loop_") &&
              contains(result.llvmIr, "array_fill_body_") &&
              contains(result.llvmIr, "array_fill_latch_") &&
              contains(result.llvmIr, "array_fill_done_") &&
              contains(result.llvmIr, "array_range_positive_") &&
              contains(result.llvmIr, "array_range_negative_") &&
              contains(result.llvmIr, "array_range_loop_") &&
              contains(result.llvmIr,
                       "define internal void "
                       "@__scalanative_throw_array_range_zero_step() noreturn") &&
              contains(result.llvmIr, "c\"Array range is too large\\00\"") &&
              contains(result.llvmIr, "array_concat_allocate_") &&
              contains(result.llvmIr, "array_concat_too_large_") &&
              contains(result.llvmIr,
                       "define internal void "
                       "@__scalanative_throw_array_concat_too_large() noreturn") &&
              contains(result.llvmIr, "c\"Array concatenation is too large\\00\"") &&
              contains(result.llvmIr, "call void @llvm.memcpy.p0.p0.i64(ptr %") &&
              contains(result.llvmIr, "store volatile i1 ") &&
              contains(result.llvmIr, "store volatile i32 ") &&
              contains(result.llvmIr, "store volatile i64 ") &&
              contains(result.llvmIr, "store volatile float ") &&
              contains(result.llvmIr, "store volatile double ") &&
              contains(result.llvmIr, "store volatile ptr ") &&
              contains(result.llvmIr, "load volatile i1, ptr %") &&
              contains(result.llvmIr, "load volatile i32, ptr %") &&
              contains(result.llvmIr, "load volatile i64, ptr %") &&
              contains(result.llvmIr, "load volatile float, ptr %") &&
              contains(result.llvmIr, "load volatile double, ptr %") &&
              contains(result.llvmIr, "load volatile ptr, ptr %") &&
              contains(result.llvmIr, "@__type_java_lang_AssertionError =") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_throw_assertion() "
                       "noreturn") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_assert(i1 %condition)") &&
              contains(result.llvmIr, "c\"Assertion failed\\00\"") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_throw_assumption() "
                       "noreturn") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_assume(i1 %condition)") &&
              contains(result.llvmIr, "c\"Assumption failed\\00\"") &&
              contains(result.llvmIr, "@__type_scala_NotImplementedError =") &&
              contains(result.llvmIr, "c\"Implementation is missing\\00\"") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_throw_requirement() "
                       "noreturn") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_require(i1 %condition)") &&
              contains(result.llvmIr, "c\"Requirement failed\\00\"") &&
              contains(result.llvmIr, "@__type_java_lang_ArithmeticException =") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_throw_arithmetic() "
                       "noreturn") &&
              contains(result.llvmIr, "define internal i32 @__scalanative_int_div(") &&
              contains(result.llvmIr, "define internal i64 @__scalanative_long_div(") &&
              contains(result.llvmIr, "define internal i64 @__scalanative_long_rem(") &&
              contains(result.llvmIr, "c\"Integer divisor cannot be zero\\00\"") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_throw_null_array() "
                       "noreturn") &&
              contains(result.llvmIr,
                       "define internal void "
                       "@__scalanative_throw_array_index_out_of_bounds() noreturn") &&
              contains(result.llvmIr,
                       "define internal i64 @__scalanative_array_length(ptr "
                       "%array)") &&
              contains(result.llvmIr, "c\"Array cannot be null\\00\"") &&
              contains(result.llvmIr, "c\"Array index is out of bounds\\00\"") &&
              contains(result.llvmIr,
                       "@__type_java_lang_NegativeArraySizeException =") &&
              contains(result.llvmIr,
                       "@__ancestors_java_lang_NegativeArraySizeException = private "
                       "constant [3 x ptr] [ptr @__type_java_lang_RuntimeException, "
                       "ptr @__type_java_lang_Exception, ptr "
                       "@__type_java_lang_Throwable]") &&
              contains(result.llvmIr,
                       "define internal ptr @__scalanative_array_alloc(i32 %length, "
                       "i64 %element_size)") &&
              contains(result.llvmIr,
                       "define internal void "
                       "@__scalanative_throw_negative_array_size() noreturn") &&
              contains(result.llvmIr, "c\"Array size cannot be negative\\00\"") &&
              contains(result.llvmIr, "call ptr @__scalanative_array_alloc(i32 ") &&
              contains(result.llvmIr, ", i64 1)") &&
              contains(result.llvmIr, ", i64 4)") &&
              contains(result.llvmIr, ", i64 8)") &&
              contains(result.llvmIr, "array_dim_negative_") &&
              contains(result.llvmIr, "array_of_dim_loop_") &&
              contains(result.llvmIr,
                       "call void @__scalanative_array_reference_set(ptr %") &&
              countOccurrences(result.llvmIr,
                               "call void "
                               "@__scalanative_throw_array_index_out_of_bounds()") ==
                  12 &&
              contains(result.llvmIr, "; Scala Native runtime resource: lifecycle") &&
              contains(result.llvmIr, "; Scala Native runtime resource: exceptions") &&
              contains(result.llvmIr,
                       "define internal ptr @__scalanative_exception_cause(") &&
              contains(result.llvmIr,
                       "%uninitialized = icmp eq ptr %cause, %exception") &&
              contains(result.llvmIr, "c\"Caused by: %s\\0A\\00\"") &&
              contains(result.llvmIr, "%scalanative.arena = type { ptr, i64, ptr }") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_zone_destroy_all()") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_throw(ptr %exception) "
                       "noreturn") &&
              contains(result.llvmIr,
                       "define internal void @__scalanative_report_exception(ptr "
                       "%exception, i1 %uncaught)") &&
              contains(result.llvmIr,
                       "define internal void "
                       "@__scalanative_report_uncaught_exception(ptr %exception)") &&
              contains(result.llvmIr,
                       "define internal void "
                       "@__scalanative_print_stack_trace(ptr %exception)") &&
              contains(result.llvmIr, "c\"%s\\0A\\00\"") &&
              contains(result.llvmIr,
                       "define internal void "
                       "@__scalanative_capture_exception_trace(ptr %exception)") &&
              countOccurrences(
                  result.llvmIr,
                  "call void @__scalanative_capture_exception_trace(ptr ") >= 3 &&
              contains(result.llvmIr,
                       "define internal void "
                       "@__scalanative_release_exception_trace(ptr %exception)") &&
              contains(result.llvmIr,
                       "define internal ptr "
                       "@__scalanative_fill_in_stack_trace(ptr %exception)") &&
              contains(result.llvmIr,
                       "define internal ptr "
                       "@__scalanative_get_stack_trace(ptr %exception)") &&
              contains(result.llvmIr,
                       "define internal i32 "
                       "@__scalanative_set_stack_trace(ptr %exception, ptr %array)") &&
              contains(result.llvmIr, "invalid_frame:\n  call void @free(ptr "
                                      "%replacement)\n  ret i32 2") &&
              contains(result.llvmIr, "invalid_array:\n  ret i32 1") &&
              contains(result.llvmIr, "@__scalanative_suppressed_array_descriptor") &&
              contains(result.llvmIr,
                       "@__trace_offsets_suppressed_array = private constant [65 x "
                       "i32]") &&
              contains(result.llvmIr,
                       "define internal void "
                       "@__scalanative_add_suppressed(ptr %exception, ptr "
                       "%suppressed)") &&
              contains(result.llvmIr,
                       "define internal ptr "
                       "@__scalanative_get_suppressed(ptr %exception)") &&
              contains(result.llvmIr, "define internal void "
                                      "@__scalanative_report_suppressed_exceptions(ptr "
                                      "%exception)") &&
              contains(result.llvmIr,
                       "define internal ptr "
                       "@__scalanative_stack_trace_element_to_string(ptr %frame)") &&
              contains(result.llvmIr, "c\"%s(%s:%d:%d)\\00\"") &&
              contains(result.llvmIr,
                       "call void @__scalanative_release_exception_trace(ptr "
                       "%object)") &&
              contains(result.llvmIr,
                       "define internal ptr "
                       "@__scalanative_exception_trace_slot(ptr %exception)") &&
              contains(result.llvmIr,
                       "define internal ptr "
                       "@__scalanative_throwable_to_string(ptr %exception)") &&
              contains(result.llvmIr, "define internal ptr "
                                      "@__scalanative_gc_object_alloc(i64 %size, ptr "
                                      "%descriptor)") &&
              countOccurrences(result.llvmIr,
                               "call ptr @__scalanative_gc_object_alloc(i64 ") >= 2 &&
              contains(result.llvmIr, "call ptr @__scalanative_object_alloc(i64 ") &&
              contains(result.llvmIr,
                       "%last_dot = call ptr @strrchr(ptr %type_name, i32 46)") &&
              contains(
                  result.llvmIr,
                  "%scalanative.source_frame = type { ptr, ptr, ptr, i32, i32 }") &&
              contains(result.llvmIr,
                       "%scalanative.exception_trace = type { i32, [64 x "
                       "%scalanative.exception_trace_entry] }") &&
              contains(result.llvmIr,
                       "store ptr %__source_frame, ptr @__scalanative_source_stack") &&
              contains(result.llvmIr, "@.source.function.") &&
              contains(result.llvmIr, "@.source.file.") &&
              contains(result.llvmIr, "call void @__scalanative_zone_destroy_all()") &&
              contains(result.llvmIr,
                       "call ptr "
                       "@__scalanative_require_non_null_thrown_exception(ptr ") &&
              contains(result.llvmIr, "call void @__scalanative_throw(ptr "),
          "throw did not retain its typed NIR or abnormal-exit runtime cleanup")) {
    return code;
  }

  constexpr const char* arrayOnlySource = R"(package demo.arrayonly
object Main {
  def main = {
    val values = Array(1)
    println(values(0))
  }
}
)";
  scalanative::tools::build::BuildOptions arrayOnlyOptions;
  arrayOnlyOptions.optimize = true;
  scalanative::support::DiagnosticEngine arrayOnlyDiagnostics;
  const scalanative::tools::build::BuildResult arrayOnly = driver.buildSource(
      "ArrayOnly.scala", arrayOnlySource, arrayOnlyOptions, arrayOnlyDiagnostics);
  if (int code = expect(
          arrayOnly.ok &&
              contains(arrayOnly.llvmIr, "@__type_java_lang_NullPointerException =") &&
              contains(arrayOnly.llvmIr,
                       "@__type_java_lang_IndexOutOfBoundsException =") &&
              contains(arrayOnly.llvmIr,
                       "@__type_java_lang_ArrayIndexOutOfBoundsException ="),
          "array intrinsics did not retain their typed runtime failures")) {
    if (!arrayOnly.ok) {
      std::cerr << arrayOnly.diagnosticsText;
    }
    return code;
  }

  constexpr const char* dynamicArrayOnlySource = R"(package demo.dynamicarrayonly
object Main {
  def allocate(rows: Int, columns: Int): Array[Array[Long]] =
    Array.ofDim[Long](rows, columns)

  def behavior(rows: Int, columns: Int): String = {
    val values = allocate(rows, columns)
    values(0)(1) = 7L
    values.length + "|" + values(0).length + "|" + values(0)(0) + "|" +
      values(0)(1) + "|" + values(1)(0) + "|" + allocate(1, 1)(0)(0)
  }

  def main = println(behavior(2, 3))
}
)";
  scalanative::tools::build::BuildOptions dynamicArrayOnlyOptions;
  dynamicArrayOnlyOptions.optimize = true;
  scalanative::support::DiagnosticEngine dynamicArrayOnlyDiagnostics;
  const scalanative::tools::build::BuildResult dynamicArrayOnly =
      driver.buildSource("DynamicArrayOnly.scala", dynamicArrayOnlySource,
                         dynamicArrayOnlyOptions, dynamicArrayOnlyDiagnostics);
  if (int code = expect(
          dynamicArrayOnly.ok &&
              contains(dynamicArrayOnly.llvmIr,
                       "@__type_java_lang_NegativeArraySizeException =") &&
              contains(dynamicArrayOnly.llvmIr,
                       "@__ancestors_java_lang_NegativeArraySizeException = private "
                       "constant [3 x ptr] [ptr @__type_java_lang_RuntimeException, "
                       "ptr @__type_java_lang_Exception, ptr "
                       "@__type_java_lang_Throwable]") &&
              contains(dynamicArrayOnly.llvmIr,
                       "call ptr @__scalanative_array_alloc(i32 ") &&
              countOccurrences(dynamicArrayOnly.llvmIr, ", i64 8)") >= 2 &&
              contains(dynamicArrayOnly.llvmIr, "array_of_dim_loop_") &&
              contains(dynamicArrayOnly.llvmIr,
                       "call ptr @__scalanative_array_reference_at(ptr ") &&
              contains(dynamicArrayOnly.llvmIr,
                       "call void @__scalanative_array_reference_set(ptr "),
          "multidimensional array allocation did not retain its typed runtime path")) {
    if (!dynamicArrayOnly.ok) {
      std::cerr << dynamicArrayOnly.diagnosticsText;
    }
    return code;
  }

  constexpr const char* cloneOnlySource = R"(package demo.cloneonly
object Main {
  def cloneValues(values: Array[Int]): Array[Int] = values.clone()
  def main = println(cloneValues(Array(4, 5))(1))
}
)";
  scalanative::tools::build::BuildOptions cloneOnlyOptions;
  cloneOnlyOptions.optimize = true;
  scalanative::support::DiagnosticEngine cloneOnlyDiagnostics;
  const scalanative::tools::build::BuildResult cloneOnly = driver.buildSource(
      "CloneOnly.scala", cloneOnlySource, cloneOnlyOptions, cloneOnlyDiagnostics);
  if (int code = expect(
          cloneOnly.ok &&
              contains(cloneOnly.llvmIr, "@__type_java_lang_NullPointerException =") &&
              contains(cloneOnly.llvmIr, "call ptr @__scalanative_array_clone(ptr ") &&
              contains(cloneOnly.llvmIr, "call void @llvm.memcpy.p0.p0.i64(ptr "),
          "optimized array clone did not retain its checked native path")) {
    if (!cloneOnly.ok) {
      std::cerr << cloneOnly.diagnosticsText;
    }
    return code;
  }

  constexpr const char* copyOnlySource = R"(package demo.copyonly
object Main {
  def copyValues(source: Array[Int], destination: Array[Int]): Unit =
    Array.copy(source, 0, destination, 0, source.length)

  def main = {
    val destination = Array(0, 0)
    copyValues(Array(4, 5), destination)
    println(destination(1))
  }
}
)";
  scalanative::tools::build::BuildOptions copyOnlyOptions;
  copyOnlyOptions.optimize = true;
  scalanative::support::DiagnosticEngine copyOnlyDiagnostics;
  const scalanative::tools::build::BuildResult copyOnly = driver.buildSource(
      "CopyOnly.scala", copyOnlySource, copyOnlyOptions, copyOnlyDiagnostics);
  if (int code = expect(
          copyOnly.ok &&
              contains(copyOnly.llvmIr, "@__type_java_lang_NullPointerException =") &&
              contains(copyOnly.llvmIr,
                       "@__type_java_lang_ArrayIndexOutOfBoundsException =") &&
              contains(copyOnly.llvmIr, "call void @__scalanative_array_copy(ptr ") &&
              contains(copyOnly.llvmIr, ", i64 4, ptr null, i1 false)") &&
              contains(copyOnly.llvmIr, "call void @llvm.memmove.p0.p0.i64(ptr "),
          "optimized Array.copy did not retain its checked overlap-safe path")) {
    if (!copyOnly.ok) {
      std::cerr << copyOnly.diagnosticsText;
    }
    return code;
  }

  constexpr const char* referenceCopyOnlySource = R"(package demo.referencecopyonly
class Base(val value: Int)
class Child(value: Int) extends Base(value)
object Main {
  def copyValues(source: Array[Base], destination: Array[Child]): Unit =
    Array.copy(source, 0, destination, 0, source.length)

  def main = {
    val destination = new Array[Child](1)
    copyValues(Array[Base](new Child(9)), destination)
    println(destination(0).value)
  }
}
)";
  scalanative::tools::build::BuildOptions referenceCopyOnlyOptions;
  referenceCopyOnlyOptions.optimize = true;
  scalanative::support::DiagnosticEngine referenceCopyOnlyDiagnostics;
  const scalanative::tools::build::BuildResult referenceCopyOnly =
      driver.buildSource("ReferenceCopyOnly.scala", referenceCopyOnlySource,
                         referenceCopyOnlyOptions, referenceCopyOnlyDiagnostics);
  if (int code =
          expect(referenceCopyOnly.ok &&
                     contains(referenceCopyOnly.llvmIr,
                              "@__type_java_lang_ArrayStoreException =") &&
                     contains(referenceCopyOnly.llvmIr,
                              "@__type_demo_referencecopyonly_Child =") &&
                     contains(referenceCopyOnly.llvmIr,
                              "call void @__scalanative_array_copy(ptr ") &&
                     contains(referenceCopyOnly.llvmIr,
                              "ptr @__type_demo_referencecopyonly_Child, i1 true)"),
                 "optimized cross-reference Array.copy did not retain its checked "
                 "destination-type path")) {
    if (!referenceCopyOnly.ok) {
      std::cerr << referenceCopyOnly.diagnosticsText;
    }
    return code;
  }

  constexpr const char* fillOnlySource = R"(package demo.fillonly
object Main {
  def make(rows: Int, columns: Int): Array[Array[Int]] =
    Array.fill[Int](rows, columns)(4)
  def main = println(make(2, 2)(1)(1))
}
)";
  scalanative::tools::build::BuildOptions fillOnlyOptions;
  fillOnlyOptions.optimize = true;
  scalanative::support::DiagnosticEngine fillOnlyDiagnostics;
  const scalanative::tools::build::BuildResult fillOnly = driver.buildSource(
      "FillOnly.scala", fillOnlySource, fillOnlyOptions, fillOnlyDiagnostics);
  if (int code = expect(
          fillOnly.ok &&
              contains(fillOnly.llvmIr,
                       "@__type_java_lang_NegativeArraySizeException =") &&
              countOccurrences(fillOnly.llvmIr,
                               "call ptr @__scalanative_array_alloc(i32 ") >= 2 &&
              contains(fillOnly.llvmIr, ", i64 4)") &&
              contains(fillOnly.llvmIr, ", i64 8)") &&
              countOccurrences(fillOnly.llvmIr, "array_fill_loop_") >= 2 &&
              contains(fillOnly.llvmIr, "store i32 4, ptr %") &&
              contains(fillOnly.llvmIr, "store ptr %"),
          "optimized multidimensional Array.fill did not retain its nested "
          "allocation and loop path")) {
    if (!fillOnly.ok) {
      std::cerr << fillOnly.diagnosticsText;
    }
    return code;
  }

  constexpr const char* rangeOnlySource = R"(package demo.rangeonly
object Main {
  def make(start: Int, end: Int, step: Int): Array[Int] =
    Array.range(start, end, step)
  def main = println(make(1, 8, 2)(3))
}
)";
  scalanative::tools::build::BuildOptions rangeOnlyOptions;
  rangeOnlyOptions.optimize = true;
  scalanative::support::DiagnosticEngine rangeOnlyDiagnostics;
  const scalanative::tools::build::BuildResult rangeOnly = driver.buildSource(
      "RangeOnly.scala", rangeOnlySource, rangeOnlyOptions, rangeOnlyDiagnostics);
  if (int code = expect(
          rangeOnly.ok &&
              contains(rangeOnly.llvmIr,
                       "@__type_java_lang_IllegalArgumentException =") &&
              contains(rangeOnly.llvmIr,
                       "call void @__scalanative_throw_array_range_zero_step()") &&
              contains(rangeOnly.llvmIr, "call ptr @__scalanative_array_alloc(i32 %") &&
              contains(rangeOnly.llvmIr, ", i64 4)") &&
              contains(rangeOnly.llvmIr, "array_range_loop_") &&
              contains(rangeOnly.llvmIr, "store i32 %"),
          "optimized Array.range did not retain its checked allocation and loop "
          "path")) {
    if (!rangeOnly.ok) {
      std::cerr << rangeOnly.diagnosticsText;
    }
    return code;
  }

  constexpr const char* concatOnlySource = R"(package demo.concatonly
object Main {
  def join(left: Array[Int], right: Array[Int]): Array[Int] =
    Array.concat[Int](left, right)
  def main = println(join(Array(1), Array(2, 3))(2))
}
)";
  scalanative::tools::build::BuildOptions concatOnlyOptions;
  concatOnlyOptions.optimize = true;
  scalanative::support::DiagnosticEngine concatOnlyDiagnostics;
  const scalanative::tools::build::BuildResult concatOnly = driver.buildSource(
      "ConcatOnly.scala", concatOnlySource, concatOnlyOptions, concatOnlyDiagnostics);
  if (int code = expect(
          concatOnly.ok &&
              contains(concatOnly.llvmIr, "@__type_java_lang_NullPointerException =") &&
              contains(concatOnly.llvmIr,
                       "@__type_java_lang_IllegalArgumentException =") &&
              contains(concatOnly.llvmIr,
                       "call void @__scalanative_throw_array_concat_too_large()") &&
              contains(concatOnly.llvmIr,
                       "call ptr @__scalanative_array_alloc(i32 %") &&
              contains(concatOnly.llvmIr, ", i64 4)") &&
              countOccurrences(concatOnly.llvmIr,
                               "call void @llvm.memcpy.p0.p0.i64(ptr %") >= 2,
          "optimized Array.concat did not retain its checked width-aware bulk-copy "
          "path")) {
    if (!concatOnly.ok) {
      std::cerr << concatOnly.diagnosticsText;
    }
    return code;
  }

  constexpr const char* invalidDynamicArraySource =
      R"(package demo.invaliddynamicarray
object Broken {
  def wrongLength = new Array[Int]("three")
  def missingLength = new Array[String]()
  def unsupportedElement = new Array[Unit](1)
  def wrongOfDimLength = Array.ofDim[Int]("three")
  def missingOfDimLength = Array.ofDim[String]()
  def unsupportedNestedElement = Array.ofDim[Array[Unit]](1)
  def unsupportedEmptyElement = Array.empty[Unit]
  def unsupportedFillElement = Array.fill[Unit](1)({})
  def wrongFillLength = Array.fill[Int]("one")(1)
  def wrongInnerFillLength = Array.fill[Int](1, "two")(1)
  def wrongFillElement = Array.fill[Int](1)("one")
  def missingFillDimensions = Array.fill[Int]()(0)
  def tooManyFillElements = Array.fill[Int](1)(0, 1)
  def missingRangeEnd = Array.range(1)
  def tooManyRangeArguments = Array.range(1, 2, 3, 4)
  def wrongRangeArgument = Array.range(1, "end")
  def unsupportedConcatElement = Array.concat[Unit]()
  def nonArrayConcatArgument = Array.concat[Int](1)
  def wrongConcatArrayType = Array.concat[Int](Array(1L))
  def cloneWithArgument = Array(1).clone(2)
  def copyMissingLength = Array.copy(Array(1), 0, Array(0), 0)
  def copyNonArraySource = Array.copy(1, 0, Array(0), 0, 1)
  def copyNonArrayDestination = Array.copy(Array(1), 0, 2, 0, 1)
  def copyDifferentArrays = Array.copy(Array(1), 0, Array(1L), 0, 1)
  def copyWrongPosition = Array.copy(Array(1), "zero", Array(0), 0, 1)
  def wrongNestedIndex = Array.ofDim[Int](1, 1)(0)("zero")
  def missingNestedIndex = Array.ofDim[Int](1, 1)(0)()
  def wrongNestedWrite = {
    val values = Array.ofDim[Int](1, 1)
    values(0)(0) = "bad"
  }
}
)";
  scalanative::support::DiagnosticEngine invalidDynamicArrayDiagnostics;
  const scalanative::tools::build::BuildResult invalidDynamicArray =
      driver.buildSource("InvalidDynamicArray.scala", invalidDynamicArraySource, {},
                         invalidDynamicArrayDiagnostics);
  if (int code = expect(
          !invalidDynamicArray.ok &&
              contains(invalidDynamicArray.diagnosticsText,
                       "Array constructor length must have type Int") &&
              contains(invalidDynamicArray.diagnosticsText,
                       "Array constructor requires exactly one Int length") &&
              contains(invalidDynamicArray.diagnosticsText,
                       "Array constructor type argument must be a supported scalar, "
                       "reference, or nested array type in this subset") &&
              contains(invalidDynamicArray.diagnosticsText,
                       "Array.ofDim dimensions must have type Int") &&
              contains(invalidDynamicArray.diagnosticsText,
                       "Array.ofDim requires at least one Int dimension") &&
              contains(invalidDynamicArray.diagnosticsText,
                       "Array.ofDim type argument must be a supported scalar, "
                       "reference, or nested array type in this subset") &&
              contains(invalidDynamicArray.diagnosticsText,
                       "Array.empty type argument must be a supported scalar, "
                       "reference, or nested array type in this subset") &&
              contains(invalidDynamicArray.diagnosticsText,
                       "Array.fill type argument must be a supported scalar, "
                       "reference, or nested array type in this subset") &&
              contains(invalidDynamicArray.diagnosticsText,
                       "Array.fill dimensions must have type Int") &&
              contains(invalidDynamicArray.diagnosticsText,
                       "Array.fill element does not conform to its declared type") &&
              contains(invalidDynamicArray.diagnosticsText,
                       "Array.fill requires at least one Int dimension") &&
              contains(invalidDynamicArray.diagnosticsText,
                       "Array.fill requires exactly one element expression") &&
              contains(invalidDynamicArray.diagnosticsText,
                       "Array.range requires start, end, and an optional step") &&
              contains(invalidDynamicArray.diagnosticsText,
                       "Array.range arguments must have type Int") &&
              contains(invalidDynamicArray.diagnosticsText,
                       "Array.concat type argument must be a supported scalar, "
                       "reference, or nested array type in this subset") &&
              contains(invalidDynamicArray.diagnosticsText,
                       "Array.concat arguments must match its declared array type") &&
              contains(invalidDynamicArray.diagnosticsText,
                       "array clone does not accept arguments") &&
              contains(invalidDynamicArray.diagnosticsText,
                       "Array.copy requires source, source position, destination, "
                       "destination position, and length") &&
              contains(invalidDynamicArray.diagnosticsText,
                       "Array.copy source must have a statically known array type") &&
              contains(invalidDynamicArray.diagnosticsText,
                       "Array.copy destination must have a statically known array "
                       "type") &&
              contains(invalidDynamicArray.diagnosticsText,
                       "Array.copy requires identical primitive, String, or nested "
                       "array element types; only class, trait, object, and Any "
                       "arrays may differ") &&
              contains(invalidDynamicArray.diagnosticsText,
                       "Array.copy positions and length must have type Int") &&
              contains(invalidDynamicArray.diagnosticsText,
                       "array index must have type Int") &&
              contains(invalidDynamicArray.diagnosticsText,
                       "array indexing requires exactly one Int index") &&
              contains(invalidDynamicArray.diagnosticsText,
                       "array assignment value does not conform to the element type"),
          "frontend accepted malformed dynamic array constructors")) {
    return code;
  }

  constexpr const char* arithmeticOnlySource = R"(package demo.arithmeticonly
object Main {
  def divide(value: Int, divisor: Int): Int = value / divisor
  def main = println(divide(84, 2))
}
)";
  scalanative::tools::build::BuildOptions arithmeticOnlyOptions;
  arithmeticOnlyOptions.optimize = true;
  scalanative::support::DiagnosticEngine arithmeticOnlyDiagnostics;
  const scalanative::tools::build::BuildResult arithmeticOnly =
      driver.buildSource("ArithmeticOnly.scala", arithmeticOnlySource,
                         arithmeticOnlyOptions, arithmeticOnlyDiagnostics);
  if (int code = expect(
          arithmeticOnly.ok &&
              contains(arithmeticOnly.llvmIr,
                       "@__type_java_lang_ArithmeticException =") &&
              contains(arithmeticOnly.llvmIr,
                       "@__ancestors_java_lang_ArithmeticException = private "
                       "constant [3 x ptr] [ptr @__type_java_lang_RuntimeException, "
                       "ptr @__type_java_lang_Exception, ptr "
                       "@__type_java_lang_Throwable]") &&
              contains(arithmeticOnly.llvmIr,
                       "define internal i32 @__scalanative_int_div("),
          "integer arithmetic did not retain its typed runtime failure")) {
    if (!arithmeticOnly.ok) {
      std::cerr << arithmeticOnly.diagnosticsText;
    }
    return code;
  }

  constexpr const char* assertionOnlySource = R"(package demo.assertiononly
object Main {
  def main = {
    assert(true)
    println("assertion-only passed")
  }
}
)";
  scalanative::tools::build::BuildOptions assertionOnlyOptions;
  assertionOnlyOptions.optimize = true;
  scalanative::support::DiagnosticEngine assertionOnlyDiagnostics;
  const scalanative::tools::build::BuildResult assertionOnly =
      driver.buildSource("AssertionOnly.scala", assertionOnlySource,
                         assertionOnlyOptions, assertionOnlyDiagnostics);
  if (int code = expect(
          assertionOnly.ok &&
              contains(assertionOnly.llvmIr, "@__type_java_lang_AssertionError =") &&
              contains(assertionOnly.llvmIr,
                       "@__ancestors_java_lang_AssertionError = private constant "
                       "[2 x ptr] [ptr @__type_java_lang_Error, ptr "
                       "@__type_java_lang_Throwable]") &&
              contains(assertionOnly.llvmIr, "call void @__scalanative_assert(i1 1)"),
          "assert builtin did not retain its typed runtime failure")) {
    if (!assertionOnly.ok) {
      std::cerr << assertionOnly.diagnosticsText;
    }
    return code;
  }

  constexpr const char* assumptionOnlySource = R"(package demo.assumptiononly
object Main {
  def main = {
    assume(true)
    println("assumption-only passed")
  }
}
)";
  scalanative::tools::build::BuildOptions assumptionOnlyOptions;
  assumptionOnlyOptions.optimize = true;
  scalanative::support::DiagnosticEngine assumptionOnlyDiagnostics;
  const scalanative::tools::build::BuildResult assumptionOnly =
      driver.buildSource("AssumptionOnly.scala", assumptionOnlySource,
                         assumptionOnlyOptions, assumptionOnlyDiagnostics);
  if (int code = expect(
          assumptionOnly.ok &&
              contains(assumptionOnly.llvmIr, "@__type_java_lang_AssertionError =") &&
              contains(assumptionOnly.llvmIr,
                       "@__ancestors_java_lang_AssertionError = private constant "
                       "[2 x ptr] [ptr @__type_java_lang_Error, ptr "
                       "@__type_java_lang_Throwable]") &&
              contains(assumptionOnly.llvmIr, "call void @__scalanative_assume(i1 1)"),
          "assume builtin did not retain its typed runtime failure")) {
    if (!assumptionOnly.ok) {
      std::cerr << assumptionOnly.diagnosticsText;
    }
    return code;
  }

  constexpr const char* notImplementedOnlySource =
      R"(package demo.notimplementedonly
object Main {
  def missing(): Int = ???
  def main = println(missing())
}
)";
  scalanative::tools::build::BuildOptions notImplementedOnlyOptions;
  notImplementedOnlyOptions.optimize = true;
  scalanative::support::DiagnosticEngine notImplementedOnlyDiagnostics;
  const scalanative::tools::build::BuildResult notImplementedOnly =
      driver.buildSource("NotImplementedOnly.scala", notImplementedOnlySource,
                         notImplementedOnlyOptions, notImplementedOnlyDiagnostics);
  if (int code = expect(
          notImplementedOnly.ok &&
              contains(notImplementedOnly.llvmIr,
                       "@__type_scala_NotImplementedError =") &&
              contains(notImplementedOnly.llvmIr,
                       "@__ancestors_scala_NotImplementedError = private constant "
                       "[2 x ptr] [ptr @__type_java_lang_Error, ptr "
                       "@__type_java_lang_Throwable]") &&
              contains(notImplementedOnly.llvmIr,
                       "c\"Implementation is missing\\00\"") &&
              contains(notImplementedOnly.llvmIr,
                       "call void @__scalanative_throw(ptr "),
          "??? did not retain its bottom-typed NotImplementedError failure")) {
    if (!notImplementedOnly.ok) {
      std::cerr << notImplementedOnly.diagnosticsText;
    }
    return code;
  }

  constexpr const char* printTraceOnlySource = R"(package demo.printtraceonly
class PrintedFailure(message: String) extends Exception(message) {
  override def toString: String = "PrintedFailure: " + getMessage
}
object Main {
  def main = {
    val failure = new PrintedFailure("optimized")
    failure.fillInStackTrace
    failure.printStackTrace()
    println("print-only continued")
  }
}
)";
  scalanative::tools::build::BuildOptions printTraceOnlyOptions;
  printTraceOnlyOptions.optimize = true;
  scalanative::support::DiagnosticEngine printTraceOnlyDiagnostics;
  const scalanative::tools::build::BuildResult printTraceOnly =
      driver.buildSource("PrintTraceOnly.scala", printTraceOnlySource,
                         printTraceOnlyOptions, printTraceOnlyDiagnostics);
  if (int code = expect(
          printTraceOnly.ok &&
              contains(printTraceOnly.llvmIr, "c\"PrintedFailure: \\00\"") &&
              contains(printTraceOnly.llvmIr,
                       "define internal void "
                       "@__scalanative_print_stack_trace(ptr %exception)") &&
              contains(printTraceOnly.llvmIr,
                       "call void @__scalanative_print_stack_trace(ptr "),
          "printStackTrace did not retain dynamic formatting after optimization")) {
    if (!printTraceOnly.ok) {
      std::cerr << printTraceOnly.diagnosticsText;
    }
    return code;
  }

  constexpr const char* requirementOnlySource = R"(package demo.requirementonly
object Main {
  def main = {
    require(true)
    println("requirement-only passed")
  }
}
)";
  scalanative::tools::build::BuildOptions requirementOnlyOptions;
  requirementOnlyOptions.optimize = true;
  scalanative::support::DiagnosticEngine requirementOnlyDiagnostics;
  const scalanative::tools::build::BuildResult requirementOnly =
      driver.buildSource("RequirementOnly.scala", requirementOnlySource,
                         requirementOnlyOptions, requirementOnlyDiagnostics);
  if (int code = expect(
          requirementOnly.ok &&
              contains(requirementOnly.llvmIr,
                       "@__type_java_lang_IllegalArgumentException =") &&
              contains(requirementOnly.llvmIr,
                       "@__ancestors_java_lang_IllegalArgumentException = private "
                       "constant [3 x ptr] [ptr @__type_java_lang_RuntimeException, "
                       "ptr @__type_java_lang_Exception, ptr "
                       "@__type_java_lang_Throwable]") &&
              contains(requirementOnly.llvmIr,
                       "call void @__scalanative_require(i1 1)"),
          "require builtin did not retain its typed runtime failure")) {
    if (!requirementOnly.ok) {
      std::cerr << requirementOnly.diagnosticsText;
    }
    return code;
  }

  constexpr const char* invalidSource = R"(package demo.exceptions
class Plain
object Invalid {
  def primitive(): Int = throw 42
  def plain(value: Plain): Int = throw value
  def widened(value: Object): Int = throw value
}
)";
  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid =
      driver.buildSource("InvalidThrow.scala", invalidSource, {}, invalidDiagnostics);
  if (int code = expect(
          !invalid.ok && countOccurrences(
                             invalid.diagnosticsText,
                             "throw operand must conform to Throwable or be null") == 3,
          "source typechecking accepted a non-Throwable throw operand")) {
    return code;
  }

  constexpr const char* invalidCauseSource = R"(package demo.exceptions
object InvalidCause {
  def value = new Exception("outer").initCause("not throwable")
}
)";
  scalanative::support::DiagnosticEngine invalidCauseDiagnostics;
  const scalanative::tools::build::BuildResult invalidCause = driver.buildSource(
      "InvalidCause.scala", invalidCauseSource, {}, invalidCauseDiagnostics);
  if (int code = expect(
          !invalidCause.ok &&
              contains(invalidCause.diagnosticsText,
                       "argument 0 of type String does not conform to parameter type "
                       "java.lang.Throwable"),
          "Throwable.initCause accepted a non-Throwable argument")) {
    return code;
  }

  scalanative::nir::FunctionBodyBuilder invalidThrowBody;
  (void)invalidThrowBody.addThrow(
      scalanative::nir::literalValue("42", "Int",
                                     scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());
  scalanative::nir::Module invalidModule;
  invalidModule.name = "demo.invalidthrow";
  invalidModule.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                       "demo.invalidthrow.Plain",
                                       "@java.lang.Object",
                                       {},
                                       scalanative::support::SourceSpan::none()});
  invalidModule.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef, "demo.invalidthrow.fail", "()Int",
       std::move(invalidThrowBody).build(), scalanative::support::SourceSpan::none()});
  scalanative::nir::FunctionBodyBuilder invalidPlainThrowBody;
  (void)invalidPlainThrowBody.addParameter("value", "demo.invalidthrow.Plain",
                                           scalanative::support::SourceSpan::none());
  (void)invalidPlainThrowBody.addThrow(
      scalanative::nir::localValue("value", scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());
  invalidModule.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef, "demo.invalidthrow.plain",
       "(demo.invalidthrow.Plain)Nothing", std::move(invalidPlainThrowBody).build(),
       scalanative::support::SourceSpan::none()});
  const scalanative::nir::VerifyResult verification =
      scalanative::nir::Verifier{}.verify(invalidModule);
  if (int code = expect(
          !verification.ok &&
              std::any_of(verification.errors.begin(), verification.errors.end(),
                          [](const std::string& error) {
                            return contains(error,
                                            "throws non-Throwable value of type Int");
                          }) &&
              std::any_of(verification.errors.begin(), verification.errors.end(),
                          [](const std::string& error) {
                            return contains(error, "throws non-Throwable value of type "
                                                   "demo.invalidthrow.Plain");
                          }),
          "NIR verification accepted a non-Throwable throw operand")) {
    return code;
  }

  scalanative::nir::FunctionBodyBuilder invalidNestedThrowBody;
  (void)invalidNestedThrowBody.addReturn(
      "Int",
      scalanative::nir::throwValue(
          scalanative::nir::literalValue("42", "Int",
                                         scalanative::support::SourceSpan::none()),
          scalanative::support::SourceSpan::none()),
      scalanative::support::SourceSpan::none());
  scalanative::nir::Value malformedNestedThrow;
  malformedNestedThrow.kind = scalanative::nir::ValueKind::Throw;
  malformedNestedThrow.type = "Nothing";
  malformedNestedThrow.text = "throw";
  scalanative::nir::FunctionBodyBuilder malformedNestedThrowBody;
  (void)malformedNestedThrowBody.addReturn("Int", std::move(malformedNestedThrow),
                                           scalanative::support::SourceSpan::none());
  scalanative::nir::Module invalidNestedModule;
  invalidNestedModule.name = "demo.invalidnestedthrow";
  invalidNestedModule.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.invalidnestedthrow.primitive", "()Int",
       std::move(invalidNestedThrowBody).build(),
       scalanative::support::SourceSpan::none()});
  invalidNestedModule.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.invalidnestedthrow.malformed", "()Int",
       std::move(malformedNestedThrowBody).build(),
       scalanative::support::SourceSpan::none()});
  const scalanative::nir::VerifyResult nestedVerification =
      scalanative::nir::Verifier{}.verify(invalidNestedModule);
  if (int code = expect(
          !nestedVerification.ok &&
              std::any_of(
                  nestedVerification.errors.begin(), nestedVerification.errors.end(),
                  [](const std::string& error) {
                    return contains(error, "throws non-Throwable value of type Int");
                  }) &&
              std::any_of(nestedVerification.errors.begin(),
                          nestedVerification.errors.end(),
                          [](const std::string& error) {
                            return contains(error, "has malformed throw value");
                          }),
          "NIR verification accepted an invalid nested throw value")) {
    return code;
  }

  constexpr const char* nestedSource = R"(package demo.nestedexceptions

class Failure(val code: Int) extends Exception("nested failure")

object Nested {
  def choose(flag: Boolean): Int =
    if (flag) throw new Failure(11) else 21

  def printChoice(flag: Boolean): Unit =
    println(if (flag) throw new Failure(12) else 22)

  def loop(flag: Boolean): Unit = {
    while (flag) {
      throw new Failure(13)
    }
  }

  def scoped(flag: Boolean): Int =
    Zone.scoped({
      if (flag) throw new Failure(14) else 24
    })
}

object Main {
  def main = {
    println("nested-before")
    println(Nested.choose(false))
    Nested.printChoice(false)
    Nested.loop(false)
    println(Nested.scoped(false))
    Zone.scoped({
      Nested.printChoice(true)
    })
    println("nested-after")
  }
}
)";

  scalanative::tools::build::BuildOptions nestedIrOptions;
  nestedIrOptions.optimize = true;
  scalanative::support::DiagnosticEngine nestedIrDiagnostics;
  const scalanative::tools::build::BuildResult nestedIr = driver.buildSource(
      "NestedThrow.scala", nestedSource, nestedIrOptions, nestedIrDiagnostics);
  if (int code = expect(nestedIr.ok, "nested throw build did not succeed")) {
    std::cerr << nestedIr.diagnosticsText;
    return code;
  }
  if (int code =
          expect(countOccurrences(nestedIr.nirText, "throw(") >= 4 &&
                     !contains(nestedIr.nirText,
                               "throw expression requires terminator lowering") &&
                     countOccurrences(nestedIr.llvmIr,
                                      "call void @__scalanative_throw(ptr ") >= 4 &&
                     countOccurrences(nestedIr.llvmIr, "throw_cont_") >= 4 &&
                     contains(nestedIr.llvmIr, "unreachable\nthrow_cont_") &&
                     contains(nestedIr.llvmIr, " = phi i32 "),
                 "nested throw did not survive optimized NIR and LLVM lowering")) {
    return code;
  }

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binaryPath =
      temporary / "cpp-scalanative-smoke-uncaught-throw-active";
  const std::filesystem::path outputPath =
      temporary / "cpp-scalanative-smoke-uncaught-throw-active.out";
  std::error_code ignored;
  std::filesystem::remove(binaryPath, ignored);
  std::filesystem::remove(outputPath, ignored);

  scalanative::tools::build::BuildOptions options;
  options.action = scalanative::tools::build::BuildAction::BuildBinary;
  options.optimizationLevel = scalanative::tools::build::OptimizationLevel::O2;
  options.outputPath = binaryPath;
  scalanative::support::DiagnosticEngine binaryDiagnostics;
  const scalanative::tools::build::BuildResult binary =
      driver.buildSource("UncaughtThrow.scala", source, options, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "uncaught throw native build failed: " + binary.diagnosticsText);
  }

  const std::string command =
      binaryPath.string() + " > " + outputPath.string() + " 2>&1";
  const int status = std::system(command.c_str());
  const std::string output = readTextFile(outputPath);
  std::filesystem::remove(binaryPath, ignored);
  std::filesystem::remove(outputPath, ignored);
  if (int code = expect(
          status != 0 && contains(output, "outer\n") && contains(output, "root\n") &&
              contains(output, "17\nDefaultFailure: zoned\n0\n") &&
              contains(output, "DefaultFailure: zoned\n0\n1\n"
                               "demo.exceptions.Main.main\n"
                               "demo.exceptions.Main.main\n") &&
              contains(output, "DefaultFailure: dynamic\n") &&
              countOccurrences(output, "DefaultFailure\n") == 2 &&
              contains(output, "Cause already initialized\n") &&
              contains(output, "An exception cannot cause itself\n") &&
              contains(output, "Suppressed exception cannot be null\n") &&
              contains(output, "An exception cannot suppress itself\n") &&
              contains(output, "Stack trace cannot be null\n") &&
              contains(output, "Stack trace cannot contain null elements|"
                               "demo.exceptions.CauseRules.nullStackFrame|"
                               "demo.exceptions.CauseRules.nullStackFrame\n") &&
              contains(output, "Array cannot be null\n"
                               "Array index is out of bounds\n"
                               "Array index is out of bounds|7\n"
                               "Array index is out of bounds\n") &&
              contains(output, "runtime parent: Array cannot be null\n") &&
              !contains(output, "missed RuntimeException catch") &&
              contains(output,
                       "int arithmetic: Integer divisor cannot be zero|null\n") &&
              contains(output,
                       "long arithmetic parent: Integer divisor cannot be zero\n") &&
              contains(output, "-2147483648|-9223372036854775808\ntrue\n") &&
              !contains(output, "missed Int division by zero") &&
              !contains(output, "missed Long remainder by zero") &&
              contains(output, "assertion branch: Assertion failed|null\n") &&
              !contains(output, "missed failed assertion") &&
              contains(output, "assumption branch: Assumption failed|null\n") &&
              !contains(output, "missed failed assumption") &&
              countOccurrences(output,
                               "AssertionError incorrectly matched Exception") == 0 &&
              contains(output,
                       "not implemented branch: Implementation is missing|null\n") &&
              !contains(output, "missed unimplemented expression") &&
              !contains(output, "NotImplementedError incorrectly matched Exception") &&
              contains(output, "print formatting branch: print formatter failed\n") &&
              !contains(output, "missed print formatting failure") &&
              contains(output, "DefaultFailure: printed root\n") &&
              contains(output, "Suppressed: DefaultFailure: printed suppressed\n") &&
              contains(output, "Caused by: RootCause: root cause\n") &&
              contains(output, "\tat demo.exceptions.PrintTraceRules.behavior(") &&
              contains(output, "continued after printStackTrace\n") &&
              !contains(output, "Uncaught exception: DefaultFailure: printed root\n") &&
              countOccurrences(output, "Uncaught exception:") == 1 &&
              contains(output, "requirement branch: Requirement failed|null\n") &&
              !contains(output, "missed failed requirement") &&
              !contains(output, "IllegalArgumentException incorrectly matched Error") &&
              contains(output, "error branch: fatal|null\n") &&
              !contains(output, "Error incorrectly matched Exception") &&
              !contains(output, "missed repeated initialization") &&
              !contains(output, "missed self-causation") &&
              !contains(output, "missed null suppressed") &&
              !contains(output, "missed self-suppression") &&
              !contains(output, "missed null stack trace") &&
              !contains(output, "missed null stack frame") &&
              !contains(output, "missed null array length") &&
              !contains(output, "missed negative array index") &&
              !contains(output, "missed upper array index") &&
              !contains(output, "missed reference array index") &&
              contains(output, "3|0|9|false|0|0.000000|0.000000|z|true|true|true\n") &&
              contains(output, "0|0|0|0|0|0|0|0|0|0\n") &&
              contains(output, "1,2,3|3|0|1|9|true|q|true|8|true|filled|false|7|9|1|"
                               "false\n") &&
              contains(output, "2|3|20|2|4|6|2|false|cell|false|false|false|0|"
                               "Array size cannot be negative|0\n") &&
              !contains(output, "missed negative inner fill dimension") &&
              contains(output, "4|1|3|5|7|3|2|4|3|7|4|1|0|0|2|2147483647|-1|3\n") &&
              contains(output, "zero step|Array range is too large\n") &&
              !contains(output, "missed zero range step") &&
              !contains(output, "missed oversized array range") &&
              contains(output, "4|1|2|3|4|3|0|false|b|true|8|true|right|true|7|9|"
                               "2|true\n") &&
              contains(output, "Array cannot be null|3\n") &&
              !contains(output, "missed null concat input") &&
              contains(output, "true|7|8|true|true|z|after|retained|trigger\n") &&
              contains(output, "Array size cannot be negative|0\n") &&
              !contains(output, "missed negative fill length") &&
              contains(output, "2|true|3|0|7|7\n") &&
              contains(output, "2|3|0|11|0|2|2|2|deep\n") &&
              contains(output, "2|9|false|4|true|true|b|right|true|8|5|false\n") &&
              contains(output, "null clone: Array cannot be null\n") &&
              !contains(output, "missed null array clone") &&
              contains(output, "1,1,2,3,4|2,3,4,5,5|true|false|y|true|9|true|"
                               "right|true|8\n") &&
              contains(output, "Array index is out of bounds|"
                               "Array index is out of bounds|0\n") &&
              !contains(output, "missed negative copy length") &&
              !contains(output, "missed overflowing copy range") &&
              contains(output, "4|true|true|true|true|true\n") &&
              contains(output, "Array element is not compatible with destination "
                               "type|true|true|true\n") &&
              !contains(output, "missed incompatible array element") &&
              contains(output, "null copy: Array cannot be null\n") &&
              !contains(output, "missed null array copy") &&
              contains(output,
                       "negative inner dimension: Array size cannot be negative\n") &&
              !contains(output, "missed negative inner dimension") &&
              contains(output,
                       "negative array: Array size cannot be negative|null\n") &&
              !contains(output, "missed negative array size") &&
              !contains(output,
                        "NegativeArraySizeException incorrectly matched Error") &&
              contains(output, "detached\nreplacement\n") &&
              contains(output, "handled\n") && contains(output, "before\n") &&
              contains(output, "boom\ndemo.exceptions.Thrower.refillFailure\n"
                               "custom.Entry(Synthetic.scala:7:3)|"
                               "custom.Entry(Synthetic.scala:7:3)|2\n"
                               "first suppressed|first suppressed|root cause|2\n"
                               "outer\nroot\nhandled\n") &&
              contains(output, "demo.exceptions.Thrower.captureFailure|"
                               "demo.exceptions.Main.main|"
                               "demo.exceptions.Thrower.captureFailure|"
                               "UncaughtThrow.scala|1|1|"
                               "demo.exceptions.Thrower.captureFailure("
                               "UncaughtThrow.scala:") &&
              contains(output, "<unknown>(<unknown>:0:0)\n0\n0\nnull\nnull\n") &&
              contains(output, "Uncaught exception: Failure: boom\n") &&
              contains(output, "\tat custom.Entry(Synthetic.scala:7)\n") &&
              contains(output, "\tat custom.Helper(Synthetic.scala:8)\n") &&
              contains(output, "Suppressed: Exception: first suppressed\n") &&
              contains(output, "Suppressed: RootCause: root cause\n") &&
              countOccurrences(output, "Suppressed:") == 3 &&
              countOccurrences(
                  output, "\tat demo.exceptions.Thrower.addSuppressedFailures(") == 2 &&
              !contains(output, "\tat demo.exceptions.Thrower.refillFailure("
                                "UncaughtThrow.scala:") &&
              !contains(output, "\tat demo.exceptions.Thrower.captureFailure("
                                "UncaughtThrow.scala:") &&
              !contains(output,
                        "\tat demo.exceptions.Thrower.fail(UncaughtThrow.scala:") &&
              !contains(output, "demo.exceptions.Thrower.handled(") &&
              contains(output, "Caused by: RootCause: root cause\n") &&
              contains(output, "\tat demo.exceptions.Thrower.captureCause("
                               "UncaughtThrow.scala:") &&
              countOccurrences(output, "Caused by:") == 2 &&
              !contains(output, "after\n"),
          "uncaught throw did not terminate at the throw site (status=" +
              std::to_string(status) + ", output='" + output + "')")) {
    return code;
  }

  constexpr const char* recursiveDiagnosticSource = R"(package demo.reporting

class FormattingFailure extends Exception("outer") {
  override def toString: String = throw new Exception("formatter failed")
}

object Main {
  def main = {
    println("format-before")
    throw new FormattingFailure()
  }
}
)";
  const std::filesystem::path recursiveBinaryPath =
      temporary / "cpp-scalanative-smoke-recursive-exception-report";
  const std::filesystem::path recursiveOutputPath =
      temporary / "cpp-scalanative-smoke-recursive-exception-report.out";
  std::filesystem::remove(recursiveBinaryPath, ignored);
  std::filesystem::remove(recursiveOutputPath, ignored);
  scalanative::tools::build::BuildOptions recursiveOptions = options;
  recursiveOptions.optimize = true;
  recursiveOptions.outputPath = recursiveBinaryPath;
  scalanative::support::DiagnosticEngine recursiveDiagnostics;
  const scalanative::tools::build::BuildResult recursiveBinary =
      driver.buildSource("RecursiveExceptionReport.scala", recursiveDiagnosticSource,
                         recursiveOptions, recursiveDiagnostics);
  if (!recursiveBinary.ok) {
    return expect(
        contains(recursiveBinary.diagnosticsText, "clang toolchain not found"),
        "recursive exception-report native build failed: " +
            recursiveBinary.diagnosticsText);
  }
  const std::string recursiveCommand =
      recursiveBinaryPath.string() + " > " + recursiveOutputPath.string() + " 2>&1";
  const int recursiveStatus = std::system(recursiveCommand.c_str());
  const std::string recursiveOutput = readTextFile(recursiveOutputPath);
  std::filesystem::remove(recursiveBinaryPath, ignored);
  std::filesystem::remove(recursiveOutputPath, ignored);
  if (int code = expect(
          recursiveStatus != 0 && contains(recursiveOutput, "format-before\n") &&
              contains(recursiveOutput, "Uncaught exception: java.lang.Exception\n") &&
              countOccurrences(recursiveOutput, "Uncaught exception:") == 1,
          "recursive exception reporting did not fall back to the runtime type "
          "name (status=" +
              std::to_string(recursiveStatus) + ", output='" + recursiveOutput +
              "')")) {
    return code;
  }

  constexpr const char* circularCauseSource = R"(package demo.circularcause

object Main {
  def main = {
    val first = new Exception("first")
    val second = new Exception("second")
    first.initCause(second)
    second.initCause(first)
    throw first
  }
}
)";
  const std::filesystem::path circularBinaryPath =
      temporary / "cpp-scalanative-smoke-circular-exception-cause";
  const std::filesystem::path circularOutputPath =
      temporary / "cpp-scalanative-smoke-circular-exception-cause.out";
  std::filesystem::remove(circularBinaryPath, ignored);
  std::filesystem::remove(circularOutputPath, ignored);
  scalanative::tools::build::BuildOptions circularOptions = options;
  circularOptions.optimize = true;
  circularOptions.outputPath = circularBinaryPath;
  scalanative::support::DiagnosticEngine circularDiagnostics;
  const scalanative::tools::build::BuildResult circularBinary =
      driver.buildSource("CircularExceptionCause.scala", circularCauseSource,
                         circularOptions, circularDiagnostics);
  if (!circularBinary.ok) {
    return expect(contains(circularBinary.diagnosticsText, "clang toolchain not found"),
                  "circular exception-cause native build failed: " +
                      circularBinary.diagnosticsText);
  }
  const std::string circularCommand =
      circularBinaryPath.string() + " > " + circularOutputPath.string() + " 2>&1";
  const int circularStatus = std::system(circularCommand.c_str());
  const std::string circularOutput = readTextFile(circularOutputPath);
  std::filesystem::remove(circularBinaryPath, ignored);
  std::filesystem::remove(circularOutputPath, ignored);
  if (int code = expect(
          circularStatus != 0 &&
              contains(circularOutput, "Uncaught exception: Exception: first\n") &&
              contains(circularOutput, "Caused by: Exception: second\n") &&
              contains(circularOutput, "Caused by: [circular reference]\n") &&
              countOccurrences(circularOutput, "Caused by:") == 2,
          "circular exception-cause reporting did not terminate safely (status=" +
              std::to_string(circularStatus) + ", output='" + circularOutput + "')")) {
    return code;
  }

  const std::filesystem::path nestedBinaryPath =
      temporary / "cpp-scalanative-smoke-nested-throw-active";
  const std::filesystem::path nestedOutputPath =
      temporary / "cpp-scalanative-smoke-nested-throw-active.out";
  std::filesystem::remove(nestedBinaryPath, ignored);
  std::filesystem::remove(nestedOutputPath, ignored);
  scalanative::tools::build::BuildOptions nestedBinaryOptions;
  nestedBinaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  nestedBinaryOptions.optimize = true;
  nestedBinaryOptions.optimizationLevel =
      scalanative::tools::build::OptimizationLevel::O2;
  nestedBinaryOptions.outputPath = nestedBinaryPath;
  scalanative::support::DiagnosticEngine nestedBinaryDiagnostics;
  const scalanative::tools::build::BuildResult nestedBinary = driver.buildSource(
      "NestedThrow.scala", nestedSource, nestedBinaryOptions, nestedBinaryDiagnostics);
  if (!nestedBinary.ok) {
    return expect(contains(nestedBinary.diagnosticsText, "clang toolchain not found"),
                  "nested throw native build failed: " + nestedBinary.diagnosticsText);
  }

  const std::string nestedCommand =
      nestedBinaryPath.string() + " > " + nestedOutputPath.string() + " 2>&1";
  const int nestedStatus = std::system(nestedCommand.c_str());
  const std::string nestedOutput = readTextFile(nestedOutputPath);
  std::filesystem::remove(nestedBinaryPath, ignored);
  std::filesystem::remove(nestedOutputPath, ignored);
  return expect(nestedStatus != 0 && contains(nestedOutput, "nested-before\n") &&
                    contains(nestedOutput, "21\n22\n24\n") &&
                    !contains(nestedOutput, "nested-after\n"),
                "nested throw did not preserve normal branches or terminate its "
                "throwing branch (status=" +
                    std::to_string(nestedStatus) + ", output='" + nestedOutput + "')");
}

int smokeTryCatchNirStage() {
  constexpr const char* source = R"(package demo.handlers

class Failure(val code: Int) extends Exception("failure")
class OtherFailure(val code: Int) extends Exception("other failure")
trait Signal
class SignaledFailure(val code: Int) extends Exception("signaled failure") with Signal

object Thrower {
  def failure(code: Int): Int = throw new Failure(code)
  def failureRef(code: Int): Failure = throw new Failure(code)
  def other(code: Int): Int = throw new OtherFailure(code)
  def signaled(code: Int): Int = throw new SignaledFailure(code)
  def existing(failure: Failure): Unit = throw failure
}

object Main {
  def recover(flag: Boolean): Int =
    try {
      if (flag) throw new Failure(7) else 11
    } catch {
      case failure: Failure => failure.code + 1
      case other: OtherFailure => other.code + 2
    } finally {
      println("cleanup")
    }

  def ensure(): Int =
    try 19 finally println("ensured")

  def catchAll(flag: Boolean): Int =
    try {
      if (flag) throw new OtherFailure(9) else 23
    } catch {
      case _ => 29
    }

  def ordered(): Int =
    try Thrower.other(31) catch {
      case failure: Failure => failure.code + 1
      case other: OtherFailure => other.code + 2
    } finally println("ordered-cleanup")

  def rethrow(): Int =
    try {
      try Thrower.other(41) catch {
        case failure: Failure => failure.code
      } finally println("inner-cleanup")
    } catch {
      case other: OtherFailure => other.code + 4
    } finally println("outer-cleanup")

  def finallyOnlyThrow(): Int =
    try {
      try Thrower.failure(50) finally println("ensured-throw")
    } catch {
      case failure: Failure => failure.code + 1
    }

  def handlerRethrow(): Int =
    try {
      try Thrower.failure(80) catch {
        case failure: Failure => throw new OtherFailure(failure.code)
      }
    } catch {
      case other: OtherFailure => other.code + 1
    }

  def finalizerOverrides(): Int =
    try {
      try Thrower.failure(70) finally Thrower.other(71)
    } catch {
      case other: OtherFailure => other.code
    }

  def zoned(): Int = {
    val failure = new Failure(60)
    try Zone.scoped({ Thrower.existing(failure); 0 }) catch {
      case caught: Failure => Zone.scoped({ caught.code + 1 })
    } finally println("zone-cleanup")
  }

  def reference(flag: Boolean): Failure =
    try {
      if (flag) Thrower.failureRef(90) else new Failure(91)
    } catch {
      case failure: Failure => failure
    } finally println("reference-cleanup")

  def traitCatch(): Int =
    try Thrower.signaled(93) catch {
      case failure: Failure => failure.code
      case signal: Signal => 94
    }

  def main = {
    println(recover(false))
    println(recover(true))
    println(ensure())
    println(catchAll(false))
    println(catchAll(true))
    println(ordered())
    println(rethrow())
    println(finallyOnlyThrow())
    println(handlerRethrow())
    println(finalizerOverrides())
    println(zoned())
    println(reference(false).code)
    println(reference(true).code)
    println(traitCatch())
  }
}
)";

  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildOptions nirOptions;
  nirOptions.action = scalanative::tools::build::BuildAction::EmitNir;
  scalanative::support::DiagnosticEngine diagnostics;
  const scalanative::tools::build::BuildResult result =
      driver.buildSource("TryCatchStage.scala", source, nirOptions, diagnostics);
  if (int code = expect(result.ok, "try/catch NIR build did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }
  if (int code = expect(
          contains(result.nirText, "ret Int try(") &&
              contains(result.nirText, "catch %failure : demo.handlers.Failure =>") &&
              contains(result.nirText,
                       "catch %other : demo.handlers.OtherFailure =>") &&
              contains(result.nirText, "catch %$catch") &&
              contains(result.nirText, " : Object => 29") &&
              countOccurrences(result.nirText, "finally(") >= 8 &&
              contains(result.nirText,
                       "finally(block(call %scala.scalanative.runtime.println") &&
              contains(result.nirText, "throw(new demo.handlers.Failure(7))"),
          "try/catch source did not retain typed handlers and finalizers in NIR")) {
    return code;
  }

  scalanative::tools::build::BuildOptions optimizedOptions = nirOptions;
  optimizedOptions.optimize = true;
  scalanative::support::DiagnosticEngine optimizedDiagnostics;
  const scalanative::tools::build::BuildResult optimized = driver.buildSource(
      "TryCatchStage.scala", source, optimizedOptions, optimizedDiagnostics);
  if (int code = expect(optimized.ok && contains(optimized.nirText, "ret Int try(") &&
                            countOccurrences(optimized.nirText, "catch %") >= 11 &&
                            countOccurrences(optimized.nirText, "finally(") >= 8 &&
                            contains(optimized.nirText, "demo.handlers.Failure") &&
                            contains(optimized.nirText, "demo.handlers.OtherFailure"),
                        "linker or Interflow lost try/catch handler metadata")) {
    std::cerr << optimized.diagnosticsText;
    return code;
  }

  constexpr const char* primitiveCatchSource = R"(package demo.handlers
object Invalid {
  def recover(): Int =
    try 1 catch {
      case value: Int => value
    }
}
)";
  scalanative::tools::build::BuildOptions checkOptions;
  checkOptions.action = scalanative::tools::build::BuildAction::Check;
  scalanative::support::DiagnosticEngine primitiveDiagnostics;
  const scalanative::tools::build::BuildResult primitiveCatch = driver.buildSource(
      "PrimitiveCatch.scala", primitiveCatchSource, checkOptions, primitiveDiagnostics);
  if (int code = expect(
          !primitiveCatch.ok &&
              contains(primitiveCatch.diagnosticsText,
                       "catch type must be a known class, trait, or Object: Int"),
          "typechecking accepted a primitive catch type")) {
    return code;
  }

  constexpr const char* invalidHandlerSource = R"(package demo.handlers
class Plain
class Failure(val code: Int) extends Exception("failure")
trait Signal
class SignaledFailure(val code: Int) extends Exception("signaled") with Signal
object Invalid {
  def plain(): Int =
    try 1 catch {
      case value: Plain => 2
    }

  def afterCatchAll(): Int =
    try 1 catch {
      case _ => 2
      case failure: Failure => failure.code
    }

  def subtypeAfterParent(): Int =
    try 1 catch {
      case failure: Exception => 2
      case failure: Failure => failure.code
    }

  def classAfterTrait(): Int =
    try 1 catch {
      case signal: Signal => 2
      case failure: SignaledFailure => failure.code
    }

  def traitAfterThrowable(): Int =
    try 1 catch {
      case failure: Throwable => 2
      case signal: Signal => 3
    }
}
)";
  scalanative::support::DiagnosticEngine invalidHandlerDiagnostics;
  const scalanative::tools::build::BuildResult invalidHandlers =
      driver.buildSource("InvalidHandlers.scala", invalidHandlerSource, checkOptions,
                         invalidHandlerDiagnostics);
  if (int code =
          expect(!invalidHandlers.ok &&
                     contains(invalidHandlers.diagnosticsText,
                              "catch class must conform to Throwable: Plain") &&
                     contains(invalidHandlers.diagnosticsText,
                              "catch-all handler must be last") &&
                     countOccurrences(invalidHandlers.diagnosticsText,
                                      "is unreachable after") == 3,
                 "typechecking accepted an impossible, shadowed, or post-catch-all "
                 "handler")) {
    return code;
  }

  constexpr const char* leakedBindingSource = R"(package demo.handlers
class Failure(val code: Int) extends Exception("failure")
object Invalid {
  def leak(): Int = {
    try 1 catch {
      case failure: Failure => failure.code
    }
    failure.code
  }
}
)";
  scalanative::support::DiagnosticEngine leakedBindingDiagnostics;
  const scalanative::tools::build::BuildResult leakedBinding =
      driver.buildSource("LeakedCatchBinding.scala", leakedBindingSource, checkOptions,
                         leakedBindingDiagnostics);
  if (int code = expect(!leakedBinding.ok && contains(leakedBinding.diagnosticsText,
                                                      "unresolved identifier: failure"),
                        "catch binding escaped its lexical handler scope")) {
    return code;
  }

  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();
  scalanative::nir::Value primitiveTry = scalanative::nir::tryValue(
      scalanative::nir::literalValue("1", "Int", noSpan),
      {scalanative::nir::catchValue("value", "Int",
                                    scalanative::nir::literalValue("2", "Int", noSpan),
                                    "Int", noSpan)},
      "Int", noSpan);
  scalanative::nir::Value plainClassTry = scalanative::nir::tryValue(
      scalanative::nir::literalValue("1", "Int", noSpan),
      {scalanative::nir::catchValue("value", "demo.invalidhandlers.Plain",
                                    scalanative::nir::literalValue("2", "Int", noSpan),
                                    "Int", noSpan)},
      "Int", noSpan);
  scalanative::nir::Value catchAllFirstTry = scalanative::nir::tryValue(
      scalanative::nir::literalValue("1", "Int", noSpan),
      {scalanative::nir::catchValue("all", "Object",
                                    scalanative::nir::literalValue("2", "Int", noSpan),
                                    "Int", noSpan),
       scalanative::nir::catchValue("failure", "demo.invalidhandlers.Failure",
                                    scalanative::nir::literalValue("3", "Int", noSpan),
                                    "Int", noSpan)},
      "Int", noSpan);
  scalanative::nir::Value subtypeAfterParentTry = scalanative::nir::tryValue(
      scalanative::nir::literalValue("1", "Int", noSpan),
      {scalanative::nir::catchValue("parent", "java.lang.Exception",
                                    scalanative::nir::literalValue("2", "Int", noSpan),
                                    "Int", noSpan),
       scalanative::nir::catchValue("failure", "demo.invalidhandlers.Failure",
                                    scalanative::nir::literalValue("3", "Int", noSpan),
                                    "Int", noSpan)},
      "Int", noSpan);
  scalanative::nir::Value classAfterTraitTry = scalanative::nir::tryValue(
      scalanative::nir::literalValue("1", "Int", noSpan),
      {scalanative::nir::catchValue("signal", "demo.invalidhandlers.Signal",
                                    scalanative::nir::literalValue("2", "Int", noSpan),
                                    "Int", noSpan),
       scalanative::nir::catchValue("failure", "demo.invalidhandlers.SignaledFailure",
                                    scalanative::nir::literalValue("3", "Int", noSpan),
                                    "Int", noSpan)},
      "Int", noSpan);
  scalanative::nir::Value traitAfterThrowableTry = scalanative::nir::tryValue(
      scalanative::nir::literalValue("1", "Int", noSpan),
      {scalanative::nir::catchValue("failure", "java.lang.Throwable",
                                    scalanative::nir::literalValue("2", "Int", noSpan),
                                    "Int", noSpan),
       scalanative::nir::catchValue("signal", "demo.invalidhandlers.Signal",
                                    scalanative::nir::literalValue("3", "Int", noSpan),
                                    "Int", noSpan)},
      "Int", noSpan);
  scalanative::nir::Value misorderedTry;
  misorderedTry.kind = scalanative::nir::ValueKind::Try;
  misorderedTry.type = "Int";
  misorderedTry.text = "try";
  misorderedTry.operands.push_back(scalanative::nir::literalValue("1", "Int", noSpan));
  misorderedTry.operands.push_back(
      scalanative::nir::finallyValue(scalanative::nir::unitValue(noSpan), noSpan));
  misorderedTry.operands.push_back(scalanative::nir::catchValue(
      "error", "Object", scalanative::nir::literalValue("3", "Int", noSpan), "Int",
      noSpan));

  scalanative::nir::FunctionBodyBuilder primitiveBody;
  (void)primitiveBody.addReturn("Int", std::move(primitiveTry), noSpan);
  scalanative::nir::FunctionBodyBuilder plainClassBody;
  (void)plainClassBody.addReturn("Int", std::move(plainClassTry), noSpan);
  scalanative::nir::FunctionBodyBuilder catchAllFirstBody;
  (void)catchAllFirstBody.addReturn("Int", std::move(catchAllFirstTry), noSpan);
  scalanative::nir::FunctionBodyBuilder subtypeAfterParentBody;
  (void)subtypeAfterParentBody.addReturn("Int", std::move(subtypeAfterParentTry),
                                         noSpan);
  scalanative::nir::FunctionBodyBuilder classAfterTraitBody;
  (void)classAfterTraitBody.addReturn("Int", std::move(classAfterTraitTry), noSpan);
  scalanative::nir::FunctionBodyBuilder traitAfterThrowableBody;
  (void)traitAfterThrowableBody.addReturn("Int", std::move(traitAfterThrowableTry),
                                          noSpan);
  scalanative::nir::FunctionBodyBuilder misorderedBody;
  (void)misorderedBody.addReturn("Int", std::move(misorderedTry), noSpan);
  scalanative::nir::Module malformedModule;
  malformedModule.name = "demo.invalidhandlers";
  malformedModule.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                         "java.lang.Throwable",
                                         "@java.lang.Object",
                                         {},
                                         noSpan});
  malformedModule.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                         "java.lang.Exception",
                                         "@java.lang.Throwable",
                                         {},
                                         noSpan});
  malformedModule.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                         "demo.invalidhandlers.Plain",
                                         "@java.lang.Object",
                                         {},
                                         noSpan});
  malformedModule.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                         "demo.invalidhandlers.Failure",
                                         "@java.lang.Exception",
                                         {},
                                         noSpan});
  malformedModule.definitions.push_back({scalanative::nir::DefinitionKind::Trait,
                                         "demo.invalidhandlers.Signal",
                                         "@java.lang.Object",
                                         {},
                                         noSpan});
  malformedModule.definitions.push_back({
      scalanative::nir::DefinitionKind::Class,
      "demo.invalidhandlers.SignaledFailure",
      "@java.lang.Throwable with @demo.invalidhandlers.Signal",
      {},
      noSpan,
  });
  malformedModule.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                         "demo.invalidhandlers.primitive", "()Int",
                                         std::move(primitiveBody).build(), noSpan});
  malformedModule.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                         "demo.invalidhandlers.plain", "()Int",
                                         std::move(plainClassBody).build(), noSpan});
  malformedModule.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                         "demo.invalidhandlers.catchAllFirst", "()Int",
                                         std::move(catchAllFirstBody).build(), noSpan});
  malformedModule.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.invalidhandlers.subtypeAfterParent", "()Int",
       std::move(subtypeAfterParentBody).build(), noSpan});
  malformedModule.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.invalidhandlers.classAfterTrait", "()Int",
       std::move(classAfterTraitBody).build(), noSpan});
  malformedModule.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.invalidhandlers.traitAfterThrowable", "()Int",
       std::move(traitAfterThrowableBody).build(), noSpan});
  malformedModule.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                         "demo.invalidhandlers.misordered", "()Int",
                                         std::move(misorderedBody).build(), noSpan});
  const scalanative::nir::VerifyResult malformedVerification =
      scalanative::nir::Verifier{}.verify(malformedModule);
  if (int code = expect(
          !malformedVerification.ok &&
              std::any_of(malformedVerification.errors.begin(),
                          malformedVerification.errors.end(),
                          [](const std::string& error) {
                            return contains(error,
                                            "catches non-reference or unresolved "
                                            "type Int");
                          }) &&
              std::any_of(malformedVerification.errors.begin(),
                          malformedVerification.errors.end(),
                          [](const std::string& error) {
                            return contains(
                                error, "catches class outside Throwable hierarchy: "
                                       "demo.invalidhandlers.Plain");
                          }) &&
              std::any_of(malformedVerification.errors.begin(),
                          malformedVerification.errors.end(),
                          [](const std::string& error) {
                            return contains(error, "has catch handler after catch-all");
                          }) &&
              std::any_of(malformedVerification.errors.begin(),
                          malformedVerification.errors.end(),
                          [](const std::string& error) {
                            return contains(
                                error,
                                "unreachable catch type demo.invalidhandlers.Failure "
                                "after java.lang.Exception");
                          }) &&
              std::any_of(malformedVerification.errors.begin(),
                          malformedVerification.errors.end(),
                          [](const std::string& error) {
                            return contains(
                                error, "unreachable catch type "
                                       "demo.invalidhandlers.SignaledFailure after "
                                       "demo.invalidhandlers.Signal");
                          }) &&
              std::any_of(malformedVerification.errors.begin(),
                          malformedVerification.errors.end(),
                          [](const std::string& error) {
                            return contains(
                                error,
                                "unreachable catch type demo.invalidhandlers.Signal "
                                "after java.lang.Throwable");
                          }) &&
              std::any_of(malformedVerification.errors.begin(),
                          malformedVerification.errors.end(),
                          [](const std::string& error) {
                            return contains(
                                error,
                                "requires ordered catches followed by at most one "
                                "finalizer");
                          }),
          "NIR verification accepted malformed handler metadata")) {
    return code;
  }

  scalanative::tools::build::BuildOptions llvmOptions;
  llvmOptions.action = scalanative::tools::build::BuildAction::EmitLlvm;
  scalanative::support::DiagnosticEngine llvmDiagnostics;
  const scalanative::tools::build::BuildResult llvm =
      driver.buildSource("TryCatchStage.scala", source, llvmOptions, llvmDiagnostics);
  if (int code = expect(
          llvm.ok &&
              contains(llvm.llvmIr, "Runtime ABI = 'cpp-scalanative-runtime-56'") &&
              contains(llvm.llvmIr, "%scalanative.exception_handler = type { ptr, ptr, "
                                    "ptr, ptr, ptr }") &&
              contains(llvm.llvmIr, "declare i32 @_setjmp(ptr) returns_twice") &&
              contains(
                  llvm.llvmIr,
                  "define internal void @__scalanative_zone_unwind_to(ptr %target)") &&
              contains(llvm.llvmIr,
                       "call i1 @__scalanative_is_instance_of(ptr %try_exception_") &&
              contains(llvm.llvmIr, "call void @longjmp(ptr %jump, i32 1)") &&
              countOccurrences(llvm.llvmIr, "call i32 @_setjmp(ptr %try_jump_") >= 10,
          "codegen did not lower typed handlers into runtime transfer and dispatch")) {
    std::cerr << llvm.diagnosticsText;
    return code;
  }

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binaryPath =
      temporary / "cpp-scalanative-smoke-try-catch-active";
  const std::filesystem::path outputPath =
      temporary / "cpp-scalanative-smoke-try-catch-active.out";
  std::error_code ignored;
  std::filesystem::remove(binaryPath, ignored);
  std::filesystem::remove(outputPath, ignored);

  scalanative::tools::build::BuildOptions binaryOptions;
  binaryOptions.action = scalanative::tools::build::BuildAction::BuildBinary;
  binaryOptions.optimize = true;
  binaryOptions.optimizationLevel = scalanative::tools::build::OptimizationLevel::O2;
  binaryOptions.outputPath = binaryPath;
  scalanative::support::DiagnosticEngine binaryDiagnostics;
  const scalanative::tools::build::BuildResult binary = driver.buildSource(
      "TryCatchStage.scala", source, binaryOptions, binaryDiagnostics);
  if (!binary.ok) {
    return expect(contains(binary.diagnosticsText, "clang toolchain not found"),
                  "try/catch native build failed: " + binary.diagnosticsText);
  }

  const std::string command =
      binaryPath.string() + " > " + outputPath.string() + " 2>&1";
  const int status = std::system(command.c_str());
  const std::string output = readTextFile(outputPath);
  std::filesystem::remove(binaryPath, ignored);
  std::filesystem::remove(outputPath, ignored);
  constexpr const char* expectedOutput = "cleanup\n11\n"
                                         "cleanup\n8\n"
                                         "ensured\n19\n"
                                         "23\n29\n"
                                         "ordered-cleanup\n33\n"
                                         "inner-cleanup\nouter-cleanup\n45\n"
                                         "ensured-throw\n51\n"
                                         "81\n71\n"
                                         "zone-cleanup\n61\n"
                                         "reference-cleanup\n91\n"
                                         "reference-cleanup\n90\n"
                                         "94\n";
  return expect(status == 0 && output == expectedOutput,
                "try/catch native execution produced the wrong control flow "
                "(status=" +
                    std::to_string(status) + ", output='" + output + "')");
}

int smokeReferenceGenericsNativeRuntime() {
  constexpr const char* source = R"(package demo.generics

trait Producer[A] {
  def produce(): A
}

class Label(val code: Int)

class Box[A](val value: A) {
  def get(): A = value
  def replaced(next: A): Box[A] = new Box[A](next)
  def choose[B](next: B): B = next
}

class Pair[A, B](val first: A, val second: B)
class Restricted[A <: Label](val value: A)

object Main {
  def identity[A](value: A): A = value

  def main = {
    val box: Box[Label] = new Box[Label](new Label(40))
    val pair: Pair[Label, Label] =
      new Pair[Label, Label](box.get(), box.choose[Label](new Label(2)))
    val bounded = new Restricted[Label](pair.first)
    val text = new Box[String]("reference")
    val producer: Producer[Label] = null
    val replaced = box.replaced(pair.second)

    println(bounded.value.code + identity[Label](pair.second).code)
    println(replaced.get().code)
    println(text.get())
  }
}
)";
  constexpr const char* invalidSource = R"(class Base
class Child extends Base
class Box[A](val value: A)
class Bounded[A <: Base](val value: A)
class LowerBounded[A >: Child]
class Empty[A]
trait Parent[A]

object InvalidGenerics {
  def identity[A](value: A): A = value
  val unsupported = new Empty[Nothing]()
  val arity = new Box[Base, Child](new Base)
  val bounded = new Bounded[String]("no")
  val lowerBounded = new LowerBounded[String]()
  val child: Box[Child] = new Box[Child](new Child)
  val invariant: Box[Base] = child
  val missingTypeArguments: Box = new Box[Base](new Base)
}

class MissingGenericChild extends Parent
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "cpp-scalanative-smoke-reference-generics";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-reference-generics.out";
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
      driver.buildSource("ReferenceGenerics.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidGenerics.scala", invalidSource, {}, invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("reference-generics native build failed: " + result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string text = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  return expect(
      status == 0 && text == "42\n2\nreference\n" && !invalid.ok &&
          contains(invalid.diagnosticsText,
                   "type argument Nothing for Empty must be a supported primitive "
                   "or reference type") &&
          contains(invalid.diagnosticsText,
                   "type application to Box has 2 arguments but expected 1") &&
          contains(invalid.diagnosticsText,
                   "type argument String for A does not conform to upper bound "
                   "Base") &&
          contains(invalid.diagnosticsText,
                   "type argument String for A does not conform to lower bound "
                   "Child") &&
          contains(invalid.diagnosticsText,
                   "initializer type Box [ Child ] does not conform to declared "
                   "type Box [ Base ]") &&
          contains(invalid.diagnosticsText,
                   "generic type Box requires 1 explicit type arguments") &&
          contains(invalid.diagnosticsText,
                   "generic parent Parent requires explicit type arguments") &&
          contains(result.nirText, "field @demo.generics.Box.value : Object") &&
          contains(result.nirText, "define @demo.generics.Box.get : "
                                   "(demo.generics.Box)Object") &&
          contains(result.nirText,
                   "define @demo.generics.Main.identity : (Object)Object") &&
          contains(result.nirText, "as-instance-of[demo.generics.Label]") &&
          !contains(result.nirText, "demo.generics.Box ["),
      "reference generic parsing, typing, erasure, diagnostics, or native "
      "execution diverged (status=" +
          std::to_string(status) + ", output='" + text + "', diagnostics='" +
          invalid.diagnosticsText + "')");
}

int smokeVarianceAndGenericInheritanceNativeRuntime() {
  constexpr const char* source = R"(package demo.variantgenerics

class Animal(val name: String)
class Dog(name: String) extends Animal(name)

trait Source[+A] {
  def get(): A
}

trait Sink[-A] {
  def put(value: A): Int
}

trait Handler[-A] {
  def handle(value: A): Int
}

trait Registry[+A] {
  def register(handler: Handler[A]): Int
}

trait NamedSource[+A] extends Source[A]

class DogSource(val dog: Dog) extends Source[Dog] {
  override def get(): Dog = dog
}

class NamedDogSource(val dog: Dog) extends NamedSource[Dog] {
  override def get(): Dog = dog
}

class ValueSource[+A](val value: A) extends Source[A] {
  override def get(): A = value
}

class AnimalSink extends Sink[Animal] {
  override def put(value: Animal): Int = 7
}

class Holder[A](val value: A)
class DogHolder(value: Dog) extends Holder[Dog](value)
class DefaultSource[A](val value: A) {
  def get(): A = value
}
class DogDefaultSource(value: Dog) extends DefaultSource[Dog](value) {
  override def get(): Dog = super.get()
}

object Main {
  def main = {
    val direct: Source[Animal] = new DogSource(new Dog("direct"))
    val transitive: Source[Animal] =
      new NamedDogSource(new Dog("transitive"))
    val forwarded: Source[Animal] =
      new ValueSource[Dog](new Dog("forwarded"))
    val sink: Sink[Dog] = new AnimalSink
    val holder: Holder[Dog] = new DogHolder(new Dog("inherited field"))
    val defaulted = new DogDefaultSource(new Dog("generic super"))

    println(direct.get().name)
    println(transitive.get().name)
    println(forwarded.get().name)
    println(sink.put(new Dog("ignored")))
    println(holder.value.name)
    println(defaulted.get().name)
  }
}
)";
  constexpr const char* invalidSource = R"(class Animal
class Dog extends Animal

trait BadSource[+A] {
  def consume(value: A): Unit = println("consume")
}

trait BadSink[-A] {
  def produce(): A = null
}

class Mutable[+A](var value: A)
class Invariant[A]
trait NestedInvariant[+A] {
  def nested(): Invariant[A]
}
trait Producer[+A] {
  def get(): A
}
trait Consumer[-A] {
  def put(value: A): Int
}
trait Exact[A] {
  def get(): A
}
trait Parent[A]

class WrongOverride extends Exact[Dog] {
  override def get(): Animal = new Animal
}
class MissingParent extends Parent

object InvalidVariance {
  val invariant: Invariant[Animal] = new Invariant[Dog]()
  val animals: Producer[Animal] = null
  val wrongCovariance: Producer[Dog] = animals
  val dogs: Consumer[Dog] = null
  val wrongContravariance: Consumer[Animal] = dogs
}
)";
  constexpr const char* invalidMethodVarianceSource = R"(object InvalidMethodVariance {
  def identity[+A](value: A): A = value
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "cpp-scalanative-smoke-variance-inheritance";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-variance-inheritance.out";
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
      driver.buildSource("VarianceAndInheritance.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidVariance.scala", invalidSource, {}, invalidDiagnostics);
  scalanative::support::DiagnosticEngine invalidMethodDiagnostics;
  const scalanative::tools::build::BuildResult invalidMethod =
      driver.buildSource("InvalidMethodVariance.scala", invalidMethodVarianceSource, {},
                         invalidMethodDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("variance and generic-inheritance native build failed: " +
                result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string text = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  return expect(
      status == 0 &&
          text ==
              "direct\ntransitive\nforwarded\n7\ninherited field\ngeneric super\n" &&
          !invalid.ok &&
          contains(invalid.diagnosticsText,
                   "covariant type parameter A occurs in contravariant position in "
                   "parameter value of method consume") &&
          contains(invalid.diagnosticsText,
                   "contravariant type parameter A occurs in covariant position in "
                   "return type of method produce") &&
          contains(invalid.diagnosticsText,
                   "covariant type parameter A occurs in invariant position in "
                   "constructor parameter value") &&
          contains(invalid.diagnosticsText,
                   "covariant type parameter A occurs in invariant position in "
                   "return type of method nested") &&
          contains(invalid.diagnosticsText,
                   "initializer type Invariant [ Dog ] does not conform to declared "
                   "type Invariant [ Animal ]") &&
          contains(invalid.diagnosticsText,
                   "initializer type Producer [ Animal ] does not conform to "
                   "declared type Producer [ Dog ]") &&
          contains(invalid.diagnosticsText,
                   "initializer type Consumer [ Dog ] does not conform to declared "
                   "type Consumer [ Animal ]") &&
          contains(invalid.diagnosticsText,
                   "override get return type Animal does not match inherited return "
                   "type Dog") &&
          contains(invalid.diagnosticsText,
                   "generic parent Parent requires explicit type arguments") &&
          !invalidMethod.ok &&
          contains(invalidMethod.diagnosticsText,
                   "method type parameters cannot declare variance") &&
          contains(result.nirText, "declare @demo.variantgenerics.Source.get : "
                                   "(demo.variantgenerics.Source)Object") &&
          contains(result.nirText, "define @demo.variantgenerics.DogSource.get : "
                                   "(demo.variantgenerics.DogSource)Object") &&
          contains(result.nirText, "define @demo.variantgenerics.AnimalSink.put : "
                                   "(demo.variantgenerics.AnimalSink,Object)Int") &&
          contains(result.nirText,
                   "field @demo.variantgenerics.Holder.value : Object") &&
          contains(result.nirText, "as-instance-of[demo.variantgenerics.Animal]") &&
          !contains(result.nirText, "demo.variantgenerics.Source ["),
      "variance, generic inheritance, erased NIR, diagnostics, or native "
      "execution diverged (status=" +
          std::to_string(status) + ", output='" + text + "', diagnostics='" +
          invalid.diagnosticsText + "')");
}

int smokeContextualAbstractionsNativeRuntime() {
  constexpr const char* source = R"(package demo.contextual

class Dog(val name: String)

trait Show[A] {
  def show(value: A): String
}

class DogShow(val prefix: String) extends Show[Dog] {
  override def show(value: Dog): String = prefix + value.name
}

object Main {
  given dogShow: Show[Dog] = new DogShow("dog:")

  def render[A](value: A)(using show: Show[A]): String =
    show.show(value)

  def forwarded[A](value: A)(using show: Show[A]): String =
    render(value)

  def explicit(value: Dog): String =
    render(value)(using dogShow)

  def locally(value: Dog)(using show: Show[Dog]): String =
    render(value)

  def main = {
    println(render(new Dog("inferred")))
    println(forwarded(new Dog("forwarded")))
    println(explicit(new Dog("explicit")))
    println(locally(new Dog("local"))(using new DogShow("local:")))
  }
}
)";
  constexpr const char* invalidSource = R"(package demo.invalidcontextual

class Dog

trait Show[A] {
  def show(value: A): String
}

class DogShow extends Show[Dog] {
  override def show(value: Dog): String = "dog"
}

object MissingContext {
  def render[A](value: A)(using show: Show[A]): String = show.show(value)
  val missing = render(new Dog)
}

object AmbiguousContext {
  given first: Show[Dog] = new DogShow
  given second: Show[Dog] = new DogShow

  def render[A](value: A)(using show: Show[A]): String = show.show(value)
  val ambiguous = render(new Dog)
}
)";
  constexpr const char* invalidClauseSource = R"(trait Show[A]

object InvalidContextualClauses {
  def untyped(value: Int)(using show): Int = value
  def reversed(using show: Show[Int])(value: Int): Int = value
  given missingInitializer: Show[Int]
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "cpp-scalanative-smoke-contextual-abstractions";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-contextual-abstractions.out";
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
      driver.buildSource("ContextualAbstractions.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidContextual.scala", invalidSource, {}, invalidDiagnostics);
  scalanative::support::DiagnosticEngine invalidClauseDiagnostics;
  const scalanative::tools::build::BuildResult invalidClause =
      driver.buildSource("InvalidContextualClauses.scala", invalidClauseSource, {},
                         invalidClauseDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("contextual-abstractions native build failed: " +
                result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string text = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  return expect(
      status == 0 &&
          text == "dog:inferred\ndog:forwarded\ndog:explicit\nlocal:local\n" &&
          !invalid.ok &&
          contains(invalid.diagnosticsText,
                   "no given value found for context parameter show of type "
                   "demo.invalidcontextual.Show [ demo.invalidcontextual.Dog ] "
                   "required by render") &&
          contains(invalid.diagnosticsText,
                   "ambiguous given values for context parameter show of type "
                   "demo.invalidcontextual.Show [ demo.invalidcontextual.Dog ] "
                   "required by render: first, second") &&
          !invalidClause.ok &&
          contains(invalidClause.diagnosticsText,
                   "using parameter requires an explicit type") &&
          contains(invalidClause.diagnosticsText,
                   "ordinary parameter clauses cannot follow a using clause") &&
          contains(invalidClause.diagnosticsText,
                   "given declaration requires an initializer") &&
          contains(result.nirText, "define @demo.contextual.DogShow.show : "
                                   "(demo.contextual.DogShow,Object)String") &&
          contains(result.nirText,
                   "let %value : demo.contextual.Dog = "
                   "as-instance-of[demo.contextual.Dog](%value$erased)") &&
          contains(result.nirText, "define @demo.contextual.Main.render : "
                                   "(Object,demo.contextual.Show)String") &&
          contains(result.nirText, "define @demo.contextual.Main.forwarded : "
                                   "(Object,demo.contextual.Show)String") &&
          contains(result.nirText, "ret String call %render(%value, %show)") &&
          contains(result.nirText, "call %demo.contextual.Main.dogShow()") &&
          !contains(result.nirText, "demo.contextual.Show ["),
      "contextual abstraction parsing, generic-aware search, erased lowering, "
      "diagnostics, or native execution diverged (status=" +
          std::to_string(status) + ", output='" + text + "', diagnostics='" +
          invalid.diagnosticsText + "', clause-diagnostics='" +
          invalidClause.diagnosticsText + "')");
}

int smokePrimitiveGenericsNativeRuntime() {
  constexpr const char* source = R"(package demo.primitivegenerics

class Cell[A](var value: A) {
  def get(): A = value

  def set(next: A): Unit = {
    value = next
  }

  def choose[B](next: B): B = next
}

object Main {
  def identity[A](value: A): A = value

  def main = {
    val ints = new Cell[Int](40)
    ints.value = identity[Int](41)
    ints.set(ints.choose[Int](42))

    println(ints.get())
    println(new Cell[Long](7L).get())
    println(if (new Cell[Boolean](true).get()) 1 else 0)
    println(new Cell[Char]('Z').get())
    println(new Cell[Byte](1.toByte).get().toInt)
    println(new Cell[Short](2.toShort).get().toInt)
    println(if (new Cell[Float](1.5f).get() > 1.0f) 1 else 0)
    println(if (new Cell[Double](2.5).get() > 2.0) 1 else 0)
    println(if (new Cell[Symbol]('ready).get() == 'ready) 1 else 0)
    println(new Cell[Unit](println("unit")).get().toString)
  }
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "cpp-scalanative-smoke-primitive-generics";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-primitive-generics.out";
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
      driver.buildSource("PrimitiveGenerics.scala", source, options, diagnostics);
  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("primitive-generics native build failed: " + result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string text = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  const std::vector<std::string_view> boxedTypes = {
      "Unit", "Boolean", "Byte",   "Short", "Int",
      "Long", "Float",   "Double", "Char",  "Symbol"};
  const bool hasEveryBoxingPair =
      std::all_of(boxedTypes.begin(), boxedTypes.end(), [&](std::string_view type) {
        return contains(result.nirText, "box[" + std::string(type) + "]") &&
               contains(result.nirText, "unbox[" + std::string(type) + "]");
      });

  return expect(
      status == 0 && text == "42\n7\n1\nZ\n1\n2\n1\n1\n1\nunit\n()\n" &&
          hasEveryBoxingPair &&
          contains(result.nirText,
                   "field @demo.primitivegenerics.Cell.value : Object") &&
          contains(result.nirText, "define @demo.primitivegenerics.Cell.get : "
                                   "(demo.primitivegenerics.Cell)Object") &&
          contains(result.nirText, "define @demo.primitivegenerics.Cell.set : "
                                   "(demo.primitivegenerics.Cell,Object)Unit") &&
          contains(result.nirText,
                   "assign %ints.value = box[Int](unbox[Int](call %identity") &&
          !contains(result.nirText, "demo.primitivegenerics.Cell ["),
      "primitive generic boxing, unboxing, mutation, erasure, optimization, or "
      "native execution diverged (status=" +
          std::to_string(status) + ", output='" + text + "', diagnostics='" +
          result.diagnosticsText + "')");
}

int smokeGenericInferenceNativeRuntime() {
  constexpr const char* source = R"(package demo.genericinference

class Label(val code: Int)

class Box[A](val value: A) {
  def get(): A = value
  def choose[B](next: B): B = next
}

class Pair[A, B](val first: A, val second: B)
class Restricted[A <: Label](val value: A)

object Main {
  def identity[A](value: A): A = value
  def unwrap[A](box: Box[A]): A = box.get()
  def wider[A](left: A, right: A): A = right
  def duplicated[A](value: A): Pair[A, A] = new Pair(value, value)

  def main = {
    val numbers = new Box(40)
    val pair = new Pair("inferred", 7L)
    val nested = unwrap(new Box(new Label(2)))
    val bounded = new Restricted(new Label(3))
    val duplicate = duplicated(new Label(4))

    println(identity(numbers.choose(41)))
    println(pair.first)
    println(pair.second)
    println(nested.code + bounded.value.code)
    println(wider(1, 9L))
    println(duplicate.first.code + duplicate.second.code)
  }
}
)";
  constexpr const char* invalidSource = R"(class Base
class Other
class Box[A](val value: A)
class Empty[A]

object InvalidInference {
  def noEvidence[A](): A = null
  def same[A](left: A, right: A): A = left
  def unwrap[A](box: Box[A]): A = box.value
  def bounded[A <: Base](value: A): A = value

  val missingMethod = noEvidence()
  val missingConstructor = new Empty()
  val conflicting = same(1, "no")
  val conflictingReferences = same(new Base, new Other)
  val wrongShape = unwrap("no")
  val badBound = bounded("no")
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "cpp-scalanative-smoke-generic-inference";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-generic-inference.out";
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
      driver.buildSource("GenericInference.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidGenericInference.scala", invalidSource, {}, invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("generic-inference native build failed: " + result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string text = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  return expect(
      status == 0 && text == "41\ninferred\n7\n5\n9\n8\n" && !invalid.ok &&
          contains(invalid.diagnosticsText,
                   "cannot infer type argument A for noEvidence from value "
                   "arguments; use explicit type arguments") &&
          contains(invalid.diagnosticsText,
                   "cannot infer type argument A for Empty from value arguments; "
                   "use explicit type arguments") &&
          contains(invalid.diagnosticsText,
                   "conflicting inferred types Int and String for type parameter A "
                   "of same") &&
          contains(invalid.diagnosticsText,
                   "conflicting inferred types Base and Other for type parameter A "
                   "of same") &&
          contains(invalid.diagnosticsText,
                   "cannot infer type argument A for unwrap from value arguments; "
                   "use explicit type arguments") &&
          contains(invalid.diagnosticsText,
                   "type argument String for A does not conform to upper bound "
                   "Base") &&
          contains(result.nirText,
                   "define @demo.genericinference.Main.identity : (Object)Object") &&
          contains(result.nirText, "define @demo.genericinference.Main.unwrap : "
                                   "(demo.genericinference.Box)Object") &&
          contains(result.nirText, "new demo.genericinference.Box(box[Int](40))") &&
          contains(result.nirText,
                   "new demo.genericinference.Pair(box[String](\"inferred\"), "
                   "box[Long](7L))") &&
          contains(result.nirText,
                   "unbox[Long](call %wider(box[Int](1), box[Long](9L)))") &&
          contains(result.nirText,
                   "as-instance-of[demo.genericinference.Label](call %unwrap") &&
          !contains(result.nirText, "demo.genericinference.Box ["),
      "generic argument inference, diagnostics, erasure, optimization, or native "
      "execution diverged (status=" +
          std::to_string(status) + ", output='" + text + "', diagnostics='" +
          invalid.diagnosticsText + "')");
}

int smokeExpectedGenericInferenceNativeRuntime() {
  constexpr const char* source = R"(package demo.expectedinference

class Label(val code: Int)
class Token[A]
class Duo[A, B](val value: A)

object Main {
  def empty[A](): A = null
  def convert[A, B](value: A): B = null
  def token[A](): Token[A] = new Token()
  def delegated(): Label = {
    empty()
  }
  def conditional(flag: Boolean): Label =
    if (flag) new Label(1) else empty()
  def returned(): Label = return empty()
  def bounded[A <: Label](): A = null

  val field: Label = empty()

  def main = {
    val label: Label = empty()
    val partial: Label = convert(42)
    val marker: Token[Label] = new Token()
    val nested: Token[Label] = token()
    val duo: Duo[Int, Label] = new Duo(7)
    val boundedLabel: Label = bounded()

    println(if (field == null) 1 else 0)
    println(if (label == null) 1 else 0)
    println(if (partial == null) 1 else 0)
    println(if (delegated() == null) 1 else 0)
    println(if (conditional(false) == null) 1 else 0)
    println(if (returned() == null) 1 else 0)
    println(if (marker == null) 0 else 1)
    println(if (nested == null) 0 else 1)
    println(if (boundedLabel == null) 1 else 0)
    println(duo.value)
  }
}
)";
  constexpr const char* invalidSource = R"(class Label
class Other
class Token[A]
class Duo[A, B](val value: A)

object InvalidExpectedInference {
  def empty[A](): A = null
  def identity[A](value: A): A = value
  def bounded[A <: Label](): A = null

  val noExpected = empty()
  val argumentWins: String = identity(1)
  val wrongShape: Label = new Token()
  val constructorArgumentWins: Duo[String, Label] = new Duo(1)
  val badBound: Other = bounded()
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "cpp-scalanative-smoke-expected-generic-inference";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-expected-generic-inference.out";
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
      "ExpectedGenericInference.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidExpectedGenericInference.scala", invalidSource, {}, invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("expected generic-inference native build failed: " +
                result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string text = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  return expect(
      status == 0 && text == "1\n1\n1\n1\n1\n1\n1\n1\n1\n7\n" && !invalid.ok &&
          contains(invalid.diagnosticsText,
                   "cannot infer type argument A for empty from value arguments; "
                   "use explicit type arguments") &&
          contains(invalid.diagnosticsText,
                   "initializer type Int does not conform to declared type String") &&
          contains(invalid.diagnosticsText,
                   "cannot infer type argument A for Token from value arguments or "
                   "expected result type; use explicit type arguments") &&
          contains(invalid.diagnosticsText,
                   "initializer type Duo [ Int, Label ] does not conform to "
                   "declared type Duo [ String, Label ]") &&
          contains(invalid.diagnosticsText,
                   "type argument Other for A does not conform to upper bound "
                   "Label") &&
          contains(result.nirText, "field @demo.expectedinference.Main.field$field : "
                                   "demo.expectedinference.Label") &&
          contains(result.nirText,
                   "as-instance-of[demo.expectedinference.Label](call %convert("
                   "box[Int](42)))") &&
          contains(result.nirText,
                   "if(%flag, new demo.expectedinference.Label(1), "
                   "as-instance-of[demo.expectedinference.Label](call %empty()))") &&
          contains(result.nirText, "define @demo.expectedinference.Main.token : "
                                   "()demo.expectedinference.Token") &&
          contains(result.nirText, "new demo.expectedinference.Token") &&
          contains(result.nirText, "new demo.expectedinference.Duo(box[Int](7))") &&
          !contains(result.nirText, "demo.expectedinference.Token [") &&
          !contains(result.nirText, "demo.expectedinference.Duo ["),
      "expected generic inference, constraint precedence, diagnostics, null-cast "
      "optimization, erasure, or native execution diverged (status=" +
          std::to_string(status) + ", output='" + text + "', diagnostics='" +
          invalid.diagnosticsText + "')");
}

int smokeByteAndShortNativeRuntime() {
  constexpr const char* source = R"(package demo.narrow

object Main {
  def byteValue(seed: Int): Byte = (seed + 130).toByte
  def shortValue(seed: Int): Short = (seed + 40000).toShort

  def main = {
    val byte = byteValue(0)
    val short = shortValue(0)
    val bytes = Array[Byte](byte, 1.toByte)
    val shorts = Array.fill[Short](2)(short)
    val joined = Array.concat[Byte](bytes, Array[Byte](3.toByte))
    val copied = Array.fill[Short](2)(0.toShort)
    Array.copy(shorts, 0, copied, 0, 2)
    val values = Array[Any](byte, short)

    println(byte + "|" + short + "|" + byte.toInt + "|" + short.toInt)
    println((byte + short) + "|" + (-byte) + "|" + (short * 2))
    println(joined.length + "|" + joined(0) + "|" + joined(2) + "|" +
      copied(0) + "|" + copied(1))
    println(values(0).isInstanceOf[Byte] + "|" +
      values(1).isInstanceOf[Short] + "|" +
      values(0).asInstanceOf[Byte] + "|" +
      values(1).asInstanceOf[Short])
    println(byte.hashCode + "|" + short.hashCode + "|" +
      byte.toString + "|" + short.toString)
    println(sizeof[Byte] + "|" + sizeof[Short])
  }
}
)";
  constexpr const char* invalidSource = R"(object InvalidNarrowing {
  val byte: Byte = 1
  val short: Short = 2
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary = temporary / "cpp-scalanative-smoke-byte-short";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-byte-short.out";
  std::filesystem::remove(binary);
  std::filesystem::remove(output);

  scalanative::tools::build::BuildDriver driver;
  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildOptions options;
  options.action = scalanative::tools::build::BuildAction::BuildBinary;
  options.optimize = true;
  options.outputPath = binary;
  const scalanative::tools::build::BuildResult result =
      driver.buildSource("ByteShort.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidNarrowing.scala", invalidSource, {}, invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("Byte/Short native build failed: " + result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string text = readTextFile(output);
  std::filesystem::remove(binary);
  std::filesystem::remove(output);

  return expect(
      status == 0 &&
          text == "-126|-25536|-126|-25536\n"
                  "-25662|126|-51072\n"
                  "3|-126|3|-25536|-25536\n"
                  "true|true|-126|-25536\n"
                  "-126|-25536|-126|-25536\n"
                  "1|2\n" &&
          !invalid.ok &&
          contains(invalid.diagnosticsText,
                   "initializer type Int does not conform to declared type Byte") &&
          contains(invalid.diagnosticsText,
                   "initializer type Int does not conform to declared type Short") &&
          contains(result.nirText,
                   "declare @scala.scalanative.runtime.intToByte : (Int)Byte") &&
          contains(result.nirText,
                   "declare @scala.scalanative.runtime.intToShort : (Int)Short") &&
          contains(result.nirText,
                   "declare @scala.scalanative.runtime.byteArrayApply : "
                   "(Array [ Byte ],Int)Byte") &&
          contains(result.nirText,
                   "declare @scala.scalanative.runtime.shortArrayApply : "
                   "(Array [ Short ],Int)Short") &&
          contains(result.llvmIr, "Runtime ABI = 'cpp-scalanative-runtime-56'") &&
          contains(result.llvmIr, "@__scalanative_boxed_Byte =") &&
          contains(result.llvmIr, "@__scalanative_boxed_Short =") &&
          contains(result.llvmIr, "define internal i8 @__scalanative_array_byte_at") &&
          contains(result.llvmIr,
                   "define internal i16 @__scalanative_array_short_at") &&
          contains(result.llvmIr, "trunc i32 %") &&
          contains(result.llvmIr, "sext i8 %") && contains(result.llvmIr, "sext i16 %"),
      "Byte/Short typing, conversion, storage, boxing, optimization, or native "
      "runtime behavior diverged (status=" +
          std::to_string(status) + ", output='" + text + "', diagnostics='" +
          invalid.diagnosticsText + "', nir-conversions=" +
          std::to_string(
              contains(result.nirText,
                       "declare @scala.scalanative.runtime.intToByte : (Int)Byte")) +
          ", nir-byte-array=" +
          std::to_string(contains(result.nirText,
                                  "declare @scala.scalanative.runtime.byteArrayApply : "
                                  "(Array [ Byte ],Int)Byte")) +
          ", llvm-byte-box=" +
          std::to_string(contains(result.llvmIr, "@__scalanative_boxed_Byte =")) +
          ", llvm-byte-array=" +
          std::to_string(contains(result.llvmIr,
                                  "define internal i8 @__scalanative_array_byte_at")) +
          ", llvm-trunc=" + std::to_string(contains(result.llvmIr, "trunc i32 %")) +
          ")");
}

int smokeZoneAllocatedBytesNativeRuntime() {
  constexpr const char* source = R"(package demo.zonebytes

object Main {
  def checksum(): Int =
    Zone.scoped({
      val bytes = Zone.allocBytes(6)
      var index = 0
      var total = 0
      while (index < bytes.length) {
        bytes(index) = (index * 17 - 20).toByte
        total = total + bytes(index).toInt
        index = index + 1
      }
      total
    })

  def zeroedFacts(): Int =
    Zone.scoped({
      val bytes = Zone.allocBytes(4)
      bytes.length * 100 + bytes(0).toInt + bytes(3).toInt
    })

  def rejectNegativeLength(): String =
    try {
      Zone.scoped({
        Zone.allocBytes(0 - 1).length
      })
      "negative length was accepted"
    } catch {
      case failure: NegativeArraySizeException =>
        "negative length: " + failure.getMessage
    }

  def bigEndianRoundTrip(): Int =
    Zone.scoped({
      val bytes = Zone.allocBytes(2)
      NativeBytes.putShortBE(bytes, 0, 4660.toShort)
      NativeBytes.getShortBE(bytes, 0).toInt
    })

  def littleEndianRoundTrip(): Int =
    Zone.scoped({
      val bytes = Zone.allocBytes(2)
      NativeBytes.putShortLE(bytes, 0, (0 - 292).toShort)
      NativeBytes.getShortLE(bytes, 0).toInt
    })

  def crossEndianRead(): Int =
    Zone.scoped({
      val bytes = Zone.allocBytes(2)
      NativeBytes.putShortBE(bytes, 0, 4660.toShort)
      NativeBytes.getShortLE(bytes, 0).toInt
    })

  def rejectPartialWrite(): Int =
    Zone.scoped({
      val bytes = Zone.allocBytes(2)
      bytes(0) = 7.toByte
      bytes(1) = 9.toByte
      try {
        NativeBytes.putShortBE(bytes, 1, 4660.toShort)
        0 - 1
      } catch {
        case failure: ArrayIndexOutOfBoundsException =>
          bytes(0).toInt * 100 + bytes(1).toInt
      }
    })

  def ordinaryArrayRoundTrip(): Int = {
    val bytes = Array[Byte](0.toByte, 0.toByte)
    NativeBytes.putShortLE(bytes, 0, 4660.toShort)
    NativeBytes.getShortLE(bytes, 0).toInt
  }

  def main = {
    println(checksum())
    println(zeroedFacts())
    println(rejectNegativeLength())
    println(bigEndianRoundTrip())
    println(littleEndianRoundTrip())
    println(crossEndianRead())
    println(rejectPartialWrite())
    println(ordinaryArrayRoundTrip())
  }
}
)";
  constexpr const char* outsideZone = R"(object OutsideZone {
  val bytes = Zone.allocBytes(4)
}
)";
  constexpr const char* escapingZone = R"(object EscapingZone {
  def run(): Int = {
    var escaped: Array[Byte] = null
    Zone.scoped({
      escaped = Zone.allocBytes(4)
      0
    })
  }
}
)";
  constexpr const char* invalidNativeBytes = R"(object InvalidNativeBytes {
  val ints = Array[Int](1, 2)
  val bytes = Array[Byte](1.toByte, 2.toByte)
  val wrongStorage = NativeBytes.getShortBE(ints, 0)
  NativeBytes.putShortLE(bytes, 0, 1)
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary = temporary / "cpp-scalanative-smoke-zone-bytes";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-zone-bytes.out";
  std::error_code ignored;
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  scalanative::tools::build::BuildDriver driver;
  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildOptions options;
  options.action = scalanative::tools::build::BuildAction::BuildBinary;
  options.optimize = true;
  options.outputPath = binary;
  const scalanative::tools::build::BuildResult result =
      driver.buildSource("ZoneBytes.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine outsideDiagnostics;
  const scalanative::tools::build::BuildResult outside =
      driver.buildSource("OutsideZone.scala", outsideZone, {}, outsideDiagnostics);
  scalanative::support::DiagnosticEngine escapeDiagnostics;
  const scalanative::tools::build::BuildResult escape =
      driver.buildSource("EscapingZone.scala", escapingZone, {}, escapeDiagnostics);
  scalanative::support::DiagnosticEngine invalidNativeDiagnostics;
  const scalanative::tools::build::BuildResult invalidNative = driver.buildSource(
      "InvalidNativeBytes.scala", invalidNativeBytes, {}, invalidNativeDiagnostics);

  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();
  scalanative::nir::FunctionBodyBuilder outsideNirBody;
  (void)outsideNirBody.addReturn(
      "Array [ Byte ]",
      scalanative::nir::callValue(
          scalanative::nir::localValue(
              std::string(scalanative::support::StdNames::RuntimeZoneAllocBytes),
              noSpan),
          {scalanative::nir::literalValue("4", "Int", noSpan)}, noSpan),
      noSpan);
  scalanative::nir::Module outsideNir;
  outsideNir.name = "demo.OutsideZoneBytesNir";
  outsideNir.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeZoneAllocBytes),
       "(Int)Array [ Byte ]",
       {},
       noSpan});
  outsideNir.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                    "demo.OutsideZoneBytesNir.run", "()Array [ Byte ]",
                                    std::move(outsideNirBody).build(), noSpan});
  const scalanative::nir::VerifyResult outsideNirResult =
      scalanative::nir::Verifier().verify(outsideNir);
  const bool nirRejectedOutsideAllocation =
      !outsideNirResult.ok &&
      std::any_of(outsideNirResult.errors.begin(), outsideNirResult.errors.end(),
                  [](const std::string& error) {
                    return contains(error,
                                    "calls zoneAllocBytes outside a zone-scoped value");
                  });

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("zone-owned Byte storage build failed: " + result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string text = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  return expect(
      status == 0 &&
          text == "135\n"
                  "400\n"
                  "negative length: Array size cannot be negative\n"
                  "4660\n"
                  "-292\n"
                  "13330\n"
                  "709\n"
                  "4660\n" &&
          !outside.ok &&
          contains(outside.diagnosticsText,
                   "Zone.allocBytes is only valid inside a Zone.scoped body") &&
          !escape.ok &&
          contains(escape.diagnosticsText,
                   "Zone.scoped reference cannot be assigned to an outer variable") &&
          !invalidNative.ok &&
          contains(invalidNative.diagnosticsText,
                   "getShortBE storage must have type Array[Byte]") &&
          contains(invalidNative.diagnosticsText,
                   "putShortLE value must have type Short") &&
          nirRejectedOutsideAllocation &&
          contains(result.nirText,
                   "declare @scala.scalanative.runtime.zoneAllocBytes : "
                   "(Int)Array [ Byte ]") &&
          contains(result.nirText,
                   "call %scala.scalanative.runtime.zoneAllocBytes(6)") &&
          contains(result.nirText,
                   "declare @scala.scalanative.runtime.nativeBytesGetShortBE : "
                   "(Array [ Byte ],Int)Short") &&
          contains(result.nirText,
                   "declare @scala.scalanative.runtime.nativeBytesPutShortLE : "
                   "(Array [ Byte ],Int,Short)Unit") &&
          contains(result.llvmIr, "Runtime ABI = 'cpp-scalanative-runtime-56'") &&
          contains(result.llvmIr, "load ptr, ptr @__scalanative_current_zone") &&
          contains(result.llvmIr, "call ptr @__scalanative_arena_alloc(ptr %") &&
          contains(result.llvmIr,
                   "call void @__scalanative_throw_negative_array_size()") &&
          contains(result.llvmIr, "define internal ptr "
                                  "@__scalanative_native_bytes_short_slot") &&
          contains(result.llvmIr, "define internal i16 "
                                  "@__scalanative_native_bytes_get_short") &&
          contains(result.llvmIr, "define internal void "
                                  "@__scalanative_native_bytes_put_short") &&
          contains(result.llvmIr, "%range_fits = icmp ule i64 %end, %length"),
      "Zone.allocBytes typing, lifetime validation, arena lowering, or native "
      "behavior diverged (status=" +
          std::to_string(status) + ", output='" + text + "', outside='" +
          outside.diagnosticsText + "', escape='" + escape.diagnosticsText + "')");
}

int smokeByteBufferStateNativeRuntime() {
  constexpr const char* source = R"(package demo.bytebuffer

object Main {
  def initialState(): Int = {
    val buffer = ByteBuffer.wrap(Array[Byte](0.toByte, 0.toByte, 0.toByte, 0.toByte,
      0.toByte, 0.toByte, 0.toByte, 0.toByte))
    buffer.capacity() * 10000 +
      buffer.position() * 1000 +
      buffer.limit() * 100 +
      buffer.remaining() * 10 +
      (if (buffer.hasRemaining()) 1 else 0)
  }

  def zoneTransitions(): Int =
    Zone.scoped({
      val buffer = ByteBuffer.wrap(Zone.allocBytes(8))
      buffer.position(6)
      buffer.flip()
      buffer.position(2).limit(4)
      buffer.capacity() * 10000 +
        buffer.position() * 1000 +
        buffer.limit() * 100 +
        buffer.remaining() * 10 +
        (if (buffer.hasRemaining()) 1 else 0)
    })

  def controlState(): Int = {
    val buffer = ByteBuffer.wrap(Array[Byte](0.toByte, 0.toByte, 0.toByte, 0.toByte,
      0.toByte, 0.toByte, 0.toByte, 0.toByte))
    buffer.limit(6).position(5).rewind()
    val rewindState = buffer.position() * 10 + buffer.limit()
    buffer.clear()
    rewindState * 10000 + buffer.position() * 100 + buffer.limit()
  }

  def clampedPosition(): Int = {
    val buffer = ByteBuffer.wrap(Array[Byte](0.toByte, 0.toByte, 0.toByte, 0.toByte,
      0.toByte, 0.toByte, 0.toByte, 0.toByte))
    buffer.position(7)
    buffer.limit(3)
    buffer.position() * 100 +
      buffer.limit() * 10 +
      buffer.remaining() +
      (if (buffer.hasRemaining()) 1 else 0)
  }

  def emptyState(): Int = {
    val buffer = ByteBuffer.wrap(Array[Byte]())
    buffer.capacity() * 100 +
      buffer.remaining() * 10 +
      (if (buffer.hasRemaining()) 1 else 0)
  }

  def relativeRoundTrip(): Int = {
    val buffer = ByteBuffer.wrap(Array[Byte](0.toByte, 0.toByte, 0.toByte))
    buffer.put(7.toByte).put((0 - 2).toByte)
    val writePosition = buffer.position()
    buffer.flip()
    val first = buffer.get().toInt
    val second = buffer.get().toInt
    first * 10000 + (second + 128) * 100 +
      writePosition * 10 + buffer.position()
  }

  def zoneRelativeAccess(): Int =
    Zone.scoped({
      val buffer = ByteBuffer.wrap(Zone.allocBytes(2))
      buffer.put(12.toByte).put(34.toByte).flip()
      buffer.get().toInt * 1000 +
        buffer.get().toInt * 10 +
        buffer.position()
    })

  def rejectUnderflow(): String = {
    val buffer = ByteBuffer.wrap(Array[Byte](1.toByte))
    buffer.position(1)
    try {
      buffer.get()
      "underflow was accepted"
    } catch {
      case failure: BufferUnderflowException =>
        "underflow: " + failure.getMessage + " @" + buffer.position()
    }
  }

  def rejectOverflow(): String = {
    val bytes = Array[Byte](9.toByte)
    val buffer = ByteBuffer.wrap(bytes)
    buffer.limit(0)
    try {
      buffer.put(3.toByte)
      "overflow was accepted"
    } catch {
      case failure: BufferOverflowException =>
        "overflow: " + failure.getMessage + " @" +
          buffer.position() + "/" + bytes(0).toInt
    }
  }

  def rejectPosition(): String =
    try {
      ByteBuffer.wrap(Array[Byte](0.toByte, 0.toByte)).position(3)
      "position was accepted"
    } catch {
      case failure: IllegalArgumentException =>
        "position: " + failure.getMessage
    }

  def rejectLimit(): String =
    try {
      ByteBuffer.wrap(Array[Byte](0.toByte, 0.toByte)).limit(3)
      "limit was accepted"
    } catch {
      case failure: IllegalArgumentException =>
        "limit: " + failure.getMessage
    }

  def rejectNullStorage(): String =
    try {
      val bytes: Array[Byte] = null
      ByteBuffer.wrap(bytes)
      "null storage was accepted"
    } catch {
      case failure: NullPointerException =>
        "null storage: " + failure.getMessage
    }

  def rejectNullReceiver(): String =
    try {
      val buffer: ByteBuffer = null
      buffer.remaining()
      "null receiver was accepted"
    } catch {
      case failure: NullPointerException =>
        "null receiver: " + failure.getMessage
    }

  def main = {
    println(initialState())
    println(zoneTransitions())
    println(controlState())
    println(clampedPosition())
    println(emptyState())
    println(relativeRoundTrip())
    println(zoneRelativeAccess())
    println(rejectUnderflow())
    println(rejectOverflow())
    println(rejectPosition())
    println(rejectLimit())
    println(rejectNullStorage())
    println(rejectNullReceiver())
  }
}
)";
  constexpr const char* escapingBuffer = R"(object EscapingBuffer {
  def run(): Int = {
    var escaped: ByteBuffer = null
    Zone.scoped({
      escaped = ByteBuffer.wrap(Zone.allocBytes(4))
      0
    })
  }
}
)";
  constexpr const char* invalidBuffer = R"(object InvalidBuffer {
  val wrongStorage = ByteBuffer.wrap(Array[Int](1, 2))
  val buffer = ByteBuffer.wrap(Array[Byte](0.toByte, 0.toByte))
  buffer.position(1.toShort)
  buffer.clear(1)
  buffer.get(1)
  buffer.put(1)
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "cpp-scalanative-smoke-byte-buffer-state";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-byte-buffer-state.out";
  std::error_code ignored;
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  scalanative::tools::build::BuildDriver driver;
  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildOptions options;
  options.action = scalanative::tools::build::BuildAction::BuildBinary;
  options.optimize = true;
  options.outputPath = binary;
  const scalanative::tools::build::BuildResult result =
      driver.buildSource("ByteBufferState.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine escapeDiagnostics;
  const scalanative::tools::build::BuildResult escape =
      driver.buildSource("EscapingBuffer.scala", escapingBuffer, {}, escapeDiagnostics);
  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid =
      driver.buildSource("InvalidBuffer.scala", invalidBuffer, {}, invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("ByteBuffer state build failed: " + result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string text = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  return expect(
      status == 0 &&
          text == "80881\n"
                  "82421\n"
                  "60008\n"
                  "330\n"
                  "0\n"
                  "82622\n"
                  "12342\n"
                  "underflow: ByteBuffer underflow @1\n"
                  "overflow: ByteBuffer overflow @0/9\n"
                  "position: ByteBuffer position is out of bounds\n"
                  "limit: ByteBuffer limit is out of bounds\n"
                  "null storage: Array cannot be null\n"
                  "null receiver: Receiver cannot be null\n" &&
          !escape.ok &&
          contains(escape.diagnosticsText,
                   "Zone.scoped reference cannot be assigned to an outer variable") &&
          !invalid.ok &&
          contains(invalid.diagnosticsText,
                   "ByteBuffer.wrap storage must have type Array[Byte]") &&
          contains(invalid.diagnosticsText, "position value must have type Int") &&
          contains(invalid.diagnosticsText, "clear does not accept arguments") &&
          contains(invalid.diagnosticsText, "get does not accept arguments") &&
          contains(invalid.diagnosticsText, "put value must have type Byte") &&
          contains(result.nirText,
                   "declare @scala.scalanative.runtime.byteBufferWrap : "
                   "(Array [ Byte ])java.nio.ByteBuffer") &&
          contains(result.nirText,
                   "declare @scala.scalanative.runtime.byteBufferSetPosition : "
                   "(java.nio.ByteBuffer,Int)java.nio.ByteBuffer") &&
          contains(result.nirText,
                   "declare @scala.scalanative.runtime.byteBufferHasRemaining : "
                   "(java.nio.ByteBuffer)Boolean") &&
          contains(result.nirText, "declare @scala.scalanative.runtime.byteBufferGet : "
                                   "(java.nio.ByteBuffer)Byte") &&
          contains(result.nirText, "declare @scala.scalanative.runtime.byteBufferPut : "
                                   "(java.nio.ByteBuffer,Byte)java.nio.ByteBuffer") &&
          contains(result.nirText, "call %scala.scalanative.runtime.byteBufferWrap(") &&
          contains(result.nirText, "call %scala.scalanative.runtime.byteBufferFlip(") &&
          contains(result.nirText, "call %scala.scalanative.runtime.byteBufferGet(") &&
          contains(result.nirText, "call %scala.scalanative.runtime.byteBufferPut(") &&
          contains(result.llvmIr, "Runtime ABI = 'cpp-scalanative-runtime-56'") &&
          contains(result.llvmIr,
                   "define internal ptr @__scalanative_byte_buffer_wrap") &&
          contains(result.llvmIr,
                   "call ptr @__scalanative_box_alloc(i64 32, ptr null)") &&
          contains(result.llvmIr,
                   "define internal ptr @__scalanative_byte_buffer_set_position") &&
          contains(result.llvmIr,
                   "define internal ptr @__scalanative_byte_buffer_set_limit") &&
          contains(result.llvmIr,
                   "define internal i32 @__scalanative_byte_buffer_remaining") &&
          contains(result.llvmIr,
                   "define internal i1 @__scalanative_byte_buffer_has_remaining") &&
          contains(result.llvmIr,
                   "define internal i8 @__scalanative_byte_buffer_get") &&
          contains(result.llvmIr,
                   "define internal ptr @__scalanative_byte_buffer_put") &&
          contains(result.llvmIr, "@__type_java_nio_BufferUnderflowException =") &&
          contains(result.llvmIr, "@__type_java_nio_BufferOverflowException =") &&
          contains(result.llvmIr,
                   "call void @__scalanative_throw_byte_buffer_underflow()") &&
          contains(result.llvmIr,
                   "call void @__scalanative_throw_byte_buffer_overflow()") &&
          contains(result.llvmIr,
                   "call void @__scalanative_throw_byte_buffer_position()") &&
          contains(result.llvmIr, "call void @__scalanative_throw_byte_buffer_limit()"),
      "ByteBuffer typing, ownership, NIR lowering, state transitions, or native "
      "behavior diverged (status=" +
          std::to_string(status) + ", output='" + text + "', escape='" +
          escape.diagnosticsText + "', invalid='" + invalid.diagnosticsText + "')");
}

int smokeOptimizedNativeEquivalence() {
  constexpr const char* source = R"(package demo.equivalence

object Main {
  def effect(value: Int): Unit =
    println(value)

  def observed(value: Boolean, marker: Int): Boolean = {
    println(marker)
    value
  }

  def compute(seed: Int): Int = {
    val folded = 1 + 2
    val dead = 40 + 2
    if (true) folded + seed else dead
  }

  def flags(value: Int): Int =
    ((value & 7) | 8) ^ 2

  def longFlags(value: Long): Long =
    (value & 15L) ^ 3L

  def intLeft(value: Int, count: Int): Int = value << count
  def intRight(value: Int, count: Int): Int = value >> count
  def intUnsignedRight(value: Int, count: Int): Int = value >>> count
  def longLeft(value: Long, count: Int): Long = value << count
  def longUnsignedRight(value: Long, count: Int): Long = value >>> count

  def main = {
    var marker: Unit = effect(1)
    marker = effect(2)
    marker
    println(compute(6))
    println(flags(13))
    println(longFlags(26L))
    println(observed(false, 3) & observed(true, 4))
    println(intLeft(3, 4))
    println(intRight(-16, 2))
    println(intUnsignedRight(-16, 2))
    println(intLeft(1, 32))
    println(longLeft(1L, 40))
    println(longUnsignedRight(-8L, 1))
    println(longLeft(1L, 64))
  }
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path plainBinary =
      temporary / "cpp-scalanative-smoke-equivalence-plain";
  const std::filesystem::path optimizedBinary =
      temporary / "cpp-scalanative-smoke-equivalence-optimized";
  const std::filesystem::path plainOutput =
      temporary / "cpp-scalanative-smoke-equivalence-plain.out";
  const std::filesystem::path optimizedOutput =
      temporary / "cpp-scalanative-smoke-equivalence-optimized.out";
  std::filesystem::remove(plainBinary);
  std::filesystem::remove(optimizedBinary);
  std::filesystem::remove(plainOutput);
  std::filesystem::remove(optimizedOutput);

  scalanative::tools::build::BuildDriver driver;
  auto build = [&](bool optimize, const std::filesystem::path& outputPath) {
    scalanative::support::DiagnosticEngine diagnostics;
    scalanative::tools::build::BuildOptions options;
    options.action = scalanative::tools::build::BuildAction::BuildBinary;
    options.optimize = optimize;
    options.outputPath = outputPath;
    return driver.buildSource("NativeEquivalence.scala", source, options, diagnostics);
  };

  const scalanative::tools::build::BuildResult plain = build(false, plainBinary);
  const scalanative::tools::build::BuildResult optimized = build(true, optimizedBinary);
  if (!plain.ok || !optimized.ok) {
    const std::string diagnostics = plain.diagnosticsText + optimized.diagnosticsText;
    if (contains(diagnostics, "clang toolchain not found")) {
      return 0;
    }
    return fail("native optimization equivalence builds failed: " + diagnostics);
  }

  const std::string plainCommand = plainBinary.string() + " > " + plainOutput.string();
  const std::string optimizedCommand =
      optimizedBinary.string() + " > " + optimizedOutput.string();
  if (int code = expect(std::system(plainCommand.c_str()) == 0 &&
                            std::system(optimizedCommand.c_str()) == 0,
                        "native optimization equivalence binaries did not exit "
                        "successfully")) {
    return code;
  }

  const std::string plainText = readTextFile(plainOutput);
  const std::string optimizedText = readTextFile(optimizedOutput);
  if (int code =
          expect(plainText == "1\n2\n9\n15\n9\n3\n4\nfalse\n48\n-4\n1073741820\n1\n"
                              "1099511627776\n9223372036854775804\n1\n" &&
                     optimizedText == plainText,
                 "optimized native output diverged from the unoptimized "
                 "result")) {
    return code;
  }
  if (int code =
          expect(contains(plain.llvmIr, "and i32 %count, 31") &&
                     contains(plain.llvmIr, "shl i32 %value") &&
                     contains(plain.llvmIr, "ashr i32 %value") &&
                     contains(plain.llvmIr, "lshr i32 %value") &&
                     contains(plain.llvmIr, "and i32 %count, 63") &&
                     contains(plain.llvmIr, "zext i32 %tmp0 to i64") &&
                     contains(plain.llvmIr, "shl i64 %value") &&
                     contains(plain.llvmIr, "lshr i64 %value"),
                 "shift lowering did not emit masked signed and unsigned operations")) {
    return code;
  }
  const std::string_view report = optimized.optimizationReportText;
  const std::size_t passesOffset = report.find("\"passes\"");
  const std::string_view summary = report.substr(0, passesOffset);
  auto phaseContains = [](const scalanative::tools::build::BuildResult& result,
                          std::string_view text) {
    return std::any_of(result.phaseLog.begin(), result.phaseLog.end(),
                       [&](const std::string& phase) { return contains(phase, text); });
  };
  return expect(
      !contains(plain.llvmIr, "unsupported mutable local with Unit type") &&
          !report.empty() && passesOffset != std::string_view::npos &&
          !contains(summary, "\"changedValues\": 0") &&
          phaseContains(plain, "opt-level=O0") && phaseContains(plain, "'-O0'") &&
          phaseContains(optimized, "opt-level=O2") &&
          phaseContains(optimized, "'-O2'") && optimized.llvmIr != plain.llvmIr,
      "native equivalence smoke did not exercise Interflow");
}

int smokeBuildDriverOptimizedFieldReadSource() {
  constexpr const char* source = R"(package demo.sourcefields

class Pair(val left: Int, var right: Int)

object Main {
  def unrelated(): Unit =
    println(0)

  def effectInt(): Int = {
    println(1)
    7
  }

  def touch(pair: Pair): Unit =
    println(pair.right)

  def afterUnrelated: Int = {
    val pair = new Pair(10, 11)
    unrelated()
    pair.left
  }

  def afterPassed: Int = {
    val pair = new Pair(20, 21)
    touch(pair)
    pair.left
  }

  def unusedEffectfulArgument: Int = {
    val pair = new Pair(effectInt(), 31)
    42
  }

  def main = {
    println(afterUnrelated)
    println(afterPassed)
    println(unusedEffectfulArgument)
  }
}
)";

  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildOptions options;
  options.action = scalanative::tools::build::BuildAction::EmitNir;
  options.optimize = true;

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("OptimizedFieldReads.scala", source, options, diagnostics);

  if (int code =
          expect(result.ok, "optimized source field-read smoke did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }

  const std::string unrelated =
      definitionText(result.nirText, "demo.sourcefields.Main.afterUnrelated");
  const std::string passed =
      definitionText(result.nirText, "demo.sourcefields.Main.afterPassed");
  const std::string effectful =
      definitionText(result.nirText, "demo.sourcefields.Main.unusedEffectfulArgument");
  if (int code =
          expect(!unrelated.empty() && !passed.empty() && !effectful.empty(),
                 "optimized source field-read smoke lost expected definitions")) {
    return code;
  }

  if (int code = expect(contains(unrelated, "eval call %unrelated()") &&
                            contains(unrelated, "ret Int 10") &&
                            !contains(unrelated, "new demo.sourcefields.Pair") &&
                            !contains(unrelated, "ret Int %pair.left"),
                        "source field read did not fold after unrelated call")) {
    return code;
  }

  if (int code = expect(contains(passed, "new demo.sourcefields.Pair(20, 21)") &&
                            contains(passed, "eval call %touch(%pair)") &&
                            contains(passed, "ret Int %pair.left") &&
                            !contains(passed, "ret Int 20"),
                        "source field read folded after object-passing call")) {
    return code;
  }

  return expect(!contains(effectful, "new demo.sourcefields.Pair(") &&
                    contains(effectful, "eval call %effectInt()") &&
                    contains(effectful, "ret Int 42"),
                "source field-read DCE did not retain only the effectful argument");
}

int smokeBuildDriverOptimizedInheritedFieldReadSource() {
  constexpr const char* source = R"(package demo.inheritedfields

class BaseCounter(val start: Int) {
  val doubled: Int = start + start
}

class ChildCounter(val seed: Int) extends BaseCounter(seed + 1) {
  val child: Int = start + seed
}

object Main {
  def unrelated(): Unit =
    println(0)

  def touch(base: BaseCounter): Unit =
    println(base.start)

  def inheritedStart: Int = {
    val child = new ChildCounter(4)
    unrelated()
    child.start
  }

  def inheritedDoubled: Int = {
    val child = new ChildCounter(4)
    unrelated()
    child.doubled
  }

  def childInitializer: Int = {
    val child = new ChildCounter(4)
    unrelated()
    child.child
  }

  def afterPassed: Int = {
    val child = new ChildCounter(4)
    touch(child)
    child.start
  }

  def main = {
    println(inheritedStart)
    println(inheritedDoubled)
    println(childInitializer)
    println(afterPassed)
  }
}
)";

  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildOptions options;
  options.action = scalanative::tools::build::BuildAction::EmitNir;
  options.optimize = true;

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildResult result = driver.buildSource(
      "OptimizedInheritedFieldReads.scala", source, options, diagnostics);

  if (int code =
          expect(result.ok, "optimized inherited field-read smoke did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }

  const std::string inheritedStart =
      definitionText(result.nirText, "demo.inheritedfields.Main.inheritedStart");
  const std::string inheritedDoubled =
      definitionText(result.nirText, "demo.inheritedfields.Main.inheritedDoubled");
  const std::string childInitializer =
      definitionText(result.nirText, "demo.inheritedfields.Main.childInitializer");
  const std::string afterPassed =
      definitionText(result.nirText, "demo.inheritedfields.Main.afterPassed");
  if (int code = expect(!inheritedStart.empty() && !inheritedDoubled.empty() &&
                            !childInitializer.empty() && !afterPassed.empty(),
                        "optimized inherited field-read smoke lost definitions")) {
    return code;
  }

  if (int code =
          expect(contains(inheritedStart, "eval call %unrelated()") &&
                     contains(inheritedStart, "ret Int 5") &&
                     !contains(inheritedStart, "new demo.inheritedfields.ChildCounter"),
                 "source inherited constructor field read did not fold")) {
    return code;
  }
  if (int code = expect(
          contains(inheritedDoubled, "eval call %unrelated()") &&
              contains(inheritedDoubled, "ret Int 10") &&
              !contains(inheritedDoubled, "new demo.inheritedfields.ChildCounter"),
          "source inherited initializer field read did not fold")) {
    return code;
  }
  if (int code = expect(
          contains(childInitializer, "eval call %unrelated()") &&
              contains(childInitializer, "ret Int 9") &&
              !contains(childInitializer, "new demo.inheritedfields.ChildCounter"),
          "source child initializer field read did not fold")) {
    return code;
  }

  return expect(contains(afterPassed, "new demo.inheritedfields.ChildCounter(4)") &&
                    contains(afterPassed, "eval call %touch(%child)") &&
                    contains(afterPassed, "ret Int %child.start") &&
                    !contains(afterPassed, "ret Int 5"),
                "source inherited field read folded after object-passing call");
}

int smokeBuildDriverOptimizedEffectfulUnitSource() {
  constexpr const char* source = R"(package demo.unitfold

object Main {
  def effectfulUnit(): Unit =
    println("effect")

  def equality: Boolean = {
    val left: Any = effectfulUnit()
    val right: Any = effectfulUnit()
    left == right
  }

  def hash: Int = {
    val value: Any = effectfulUnit()
    value.hashCode
  }

  def text: String = {
    val value: Any = effectfulUnit()
    value.toString
  }

  def concat: String = {
    val value: Any = effectfulUnit()
    "value=" + value
  }

  def main = {
    println(equality)
    println(hash)
    println(text)
    println(concat)
  }
}
)";

  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildOptions options;
  options.action = scalanative::tools::build::BuildAction::EmitNir;
  options.optimize = true;

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildResult result =
      driver.buildSource("OptimizedEffectfulUnit.scala", source, options, diagnostics);
  if (int code =
          expect(result.ok, "optimized effectful Unit source smoke did not succeed")) {
    std::cerr << result.diagnosticsText;
    return code;
  }

  const std::string equality =
      definitionText(result.nirText, "demo.unitfold.Main.equality");
  const std::string hash = definitionText(result.nirText, "demo.unitfold.Main.hash");
  const std::string text = definitionText(result.nirText, "demo.unitfold.Main.text");
  const std::string concat =
      definitionText(result.nirText, "demo.unitfold.Main.concat");
  if (int code =
          expect(!equality.empty() && !hash.empty() && !text.empty() && !concat.empty(),
                 "optimized effectful Unit source smoke lost definitions")) {
    return code;
  }

  const std::string effectCall = "eval call %effectfulUnit()";
  const std::size_t firstEffect = equality.find(effectCall);
  const std::size_t secondEffect =
      firstEffect == std::string::npos
          ? std::string::npos
          : equality.find(effectCall, firstEffect + effectCall.size());
  if (int code = expect(
          firstEffect != std::string::npos && secondEffect != std::string::npos &&
              firstEffect < secondEffect && contains(equality, "ret Boolean true") &&
              !contains(equality, "anyEquals") && !contains(equality, "box[Unit]"),
          "optimized source did not fold effectful Unit equality after both calls")) {
    return code;
  }
  if (int code =
          expect(contains(hash, effectCall) && contains(hash, "ret Int 0") &&
                     !contains(hash, "anyHashCode") && !contains(hash, "box[Unit]"),
                 "optimized source did not fold effectful Unit hash")) {
    return code;
  }
  if (int code =
          expect(contains(text, effectCall) && contains(text, "ret String \"()\"") &&
                     !contains(text, "anyToString") && !contains(text, "box[Unit]"),
                 "optimized source did not fold effectful Unit toString")) {
    return code;
  }
  if (int code = expect(contains(concat, effectCall) &&
                            contains(concat, "ret String \"value=()\"") &&
                            !contains(concat, "box[Unit]") && !contains(concat, " + "),
                        "optimized source did not fold effectful Unit concatenation")) {
    return code;
  }

  const std::string_view report = result.optimizationReportText;
  const std::size_t passCount = countOccurrences(report, "\"name\": ");
  if (int code = expect(
          contains(report, "\"optimized\": true") && contains(report, "\"ok\": true") &&
              passCount != 0 &&
              contains(report, "\"name\": \"propagate-local-constants\"") &&
              contains(report, "\"name\": \"fold-constants\"") &&
              contains(report, "\"name\": \"eliminate-dead-local-lets\"") &&
              contains(report, "\"name\": \"simplify-blocks\"") &&
              contains(report, "\"name\": \"fold-cleaned-constants\"") &&
              contains(report, "\"name\": \"eliminate-cleaned-dead-local-lets\"") &&
              contains(report, "\"name\": \"simplify-cleaned-blocks\"") &&
              contains(report, "\"name\": \"prune-unreachable-functions\"") &&
              countOccurrences(report, "\"validationErrorsBefore\": 0") == passCount &&
              countOccurrences(report, "\"validationErrorsAfter\": 0") == passCount,
          "effectful Unit optimization report was incomplete or validation-dirty")) {
    return code;
  }

  return 0;
}

int smokeInterflowPrunesUnusedDeclarations() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  scalanative::nir::FunctionBodyBuilder callerBody;
  (void)callerBody.addReturn(
      "Int",
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.Decls.used", noSpan), {},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Decls.caller", "()Int",
                                std::move(callerBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                "demo.interflow.Decls.used",
                                "()Int",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                "demo.interflow.Decls.unused",
                                "()Int",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.roots.push_back("demo.interflow.Decls.caller");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.Decls.caller");
  program.reachableGlobals.push_back("demo.interflow.Decls.used");
  program.reachableGlobals.push_back("demo.interflow.Decls.unused");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code = expect(result.ok, "interflow rejected declaration pruning program")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* caller =
      findDefinition(optimizedModule, "demo.interflow.Decls.caller");
  const scalanative::nir::Definition* used =
      findDefinition(optimizedModule, "demo.interflow.Decls.used");
  const scalanative::nir::Definition* unused =
      findDefinition(optimizedModule, "demo.interflow.Decls.unused");
  if (int code = expect(caller != nullptr && used != nullptr,
                        "interflow pruned a referenced declaration")) {
    return code;
  }
  if (int code =
          expect(unused == nullptr, "interflow kept an unused plain declaration")) {
    return code;
  }
  return expect(scalanative::nir::bodyToText(caller->body).back() ==
                    "ret Int call %demo.interflow.Decls.used()",
                "interflow rewrote declaration call unexpectedly");
}

int smokeInterflowLiteralBitwiseAndShiftFold() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();
  auto intValue = [&](long long value) {
    return scalanative::nir::literalValue(std::to_string(value), "Int", noSpan);
  };
  auto longValue = [&](long long value) {
    return scalanative::nir::literalValue(std::to_string(value) + "L", "Long", noSpan);
  };
  auto booleanValue = [&](bool value) {
    return scalanative::nir::literalValue(value ? "true" : "false", "Boolean", noSpan);
  };
  auto binary = [&](std::string operation, scalanative::nir::Value lhs,
                    scalanative::nir::Value rhs) {
    return scalanative::nir::binaryValue(std::move(operation), std::move(lhs),
                                         std::move(rhs), noSpan);
  };
  auto bodyReturning = [&](std::string type, scalanative::nir::Value value) {
    scalanative::nir::FunctionBodyBuilder body;
    (void)body.addReturn(std::move(type), std::move(value), noSpan);
    return std::move(body).build();
  };

  scalanative::nir::ModuleBuilder module("demo.interflow.bitshift");
  module.addFunctionDef(
      "demo.interflow.bitshift.Main.flags", "()Int",
      bodyReturning(
          "Int",
          binary("^", binary("|", binary("&", intValue(13), intValue(7)), intValue(8)),
                 intValue(2))),
      noSpan);
  module.addFunctionDef(
      "demo.interflow.bitshift.Main.unsignedInt", "()Int",
      bodyReturning("Int", binary(">>>", intValue(-16), intValue(34))), noSpan);
  module.addFunctionDef("demo.interflow.bitshift.Main.wrappedLong", "()Long",
                        bodyReturning("Long", binary("<<", longValue(1), intValue(68))),
                        noSpan);
  module.addFunctionDef(
      "demo.interflow.bitshift.Main.signedLong", "()Long",
      bodyReturning("Long", binary(">>", longValue(-8), intValue(65))), noSpan);
  module.addFunctionDef(
      "demo.interflow.bitshift.Main.unsignedLong", "()Long",
      bodyReturning("Long", binary(">>>", longValue(-8), intValue(1))), noSpan);
  module.addFunctionDef(
      "demo.interflow.bitshift.Main.booleanXor", "()Boolean",
      bodyReturning("Boolean", binary("^", booleanValue(true), booleanValue(true))),
      noSpan);

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module).build());
  program.roots = {
      "demo.interflow.bitshift.Main.flags",
      "demo.interflow.bitshift.Main.unsignedInt",
      "demo.interflow.bitshift.Main.wrappedLong",
      "demo.interflow.bitshift.Main.signedLong",
      "demo.interflow.bitshift.Main.unsignedLong",
      "demo.interflow.bitshift.Main.booleanXor",
  };
  program.reachableGlobals = program.roots;

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code = expect(result.ok, "interflow rejected literal bitwise/shift NIR")) {
    return code;
  }

  const scalanative::nir::Module& optimized = result.program.modules.front();
  auto returnText = [&](std::string_view name) {
    const scalanative::nir::Definition* definition = findDefinition(optimized, name);
    if (definition == nullptr) {
      return std::string{};
    }
    const std::vector<std::string> text =
        scalanative::nir::bodyToText(definition->body);
    return text.empty() ? std::string{} : text.back();
  };

  return expect(
      returnText("demo.interflow.bitshift.Main.flags") == "ret Int 15" &&
          returnText("demo.interflow.bitshift.Main.unsignedInt") ==
              "ret Int 1073741820" &&
          returnText("demo.interflow.bitshift.Main.wrappedLong") == "ret Long 16L" &&
          returnText("demo.interflow.bitshift.Main.signedLong") == "ret Long -4L" &&
          returnText("demo.interflow.bitshift.Main.unsignedLong") ==
              "ret Long 9223372036854775804L" &&
          returnText("demo.interflow.bitshift.Main.booleanXor") == "ret Boolean false",
      "interflow did not preserve fixed-width bitwise/shift literal semantics");
}

int smokeInterflowExactStringConcat() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();
  const auto stringLiteral = [&](std::string_view text) {
    return scalanative::nir::literalValue("\"" + std::string(text) + "\"", "String",
                                          noSpan);
  };
  const auto concat = [&](scalanative::nir::Value left, scalanative::nir::Value right) {
    return scalanative::nir::binaryValue("+", std::move(left), std::move(right),
                                         noSpan);
  };

  scalanative::nir::FunctionBodyBuilder boxedSymbolBody;
  (void)boxedSymbolBody.addLet(
      "symbolValue", "Object",
      scalanative::nir::boxValue(
          "Symbol", scalanative::nir::literalValue("'ready", "Symbol", noSpan), noSpan),
      noSpan);
  (void)boxedSymbolBody.addReturn(
      "String",
      concat(stringLiteral("symbol="),
             scalanative::nir::localValue("symbolValue", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder boxedStringBody;
  (void)boxedStringBody.addLet(
      "stringValue", "Object",
      scalanative::nir::boxValue("String", stringLiteral("Scala"), noSpan), noSpan);
  (void)boxedStringBody.addReturn(
      "String",
      concat(stringLiteral("project="),
             scalanative::nir::localValue("stringValue", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder primitiveBody;
  (void)primitiveBody.addReturn(
      "String",
      concat(concat(stringLiteral("n="),
                    scalanative::nir::literalValue("7", "Int", noSpan)),
             concat(stringLiteral(", ok="),
                    scalanative::nir::literalValue("false", "Boolean", noSpan))),
      noSpan);

  scalanative::nir::FunctionBodyBuilder nullBody;
  (void)nullBody.addReturn(
      "String",
      concat(stringLiteral("missing="),
             scalanative::nir::literalValue("null", "Null", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder objectBody;
  (void)objectBody.addLet(
      "box", "demo.interflow.ConcatBox",
      scalanative::nir::newValue("demo.interflow.ConcatBox", noSpan), noSpan);
  (void)objectBody.addReturn(
      "String",
      concat(stringLiteral("obj="), scalanative::nir::localValue("box", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder floatingBody;
  (void)floatingBody.addReturn(
      "String",
      concat(stringLiteral("f="),
             scalanative::nir::literalValue("1.5", "Double", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                "demo.interflow.ConcatBox",
                                "@java.lang.Object",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Concat.boxedSymbol", "()String",
                                std::move(boxedSymbolBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Concat.boxedString", "()String",
                                std::move(boxedStringBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Concat.primitive", "()String",
                                std::move(primitiveBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Concat.nullValue", "()String",
                                std::move(nullBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Concat.objectValue", "()String",
                                std::move(objectBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Concat.floatingValue", "()String",
                                std::move(floatingBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  const char* roots[] = {
      "scala.scalanative.runtime.main",      "demo.interflow.Concat.boxedSymbol",
      "demo.interflow.Concat.boxedString",   "demo.interflow.Concat.primitive",
      "demo.interflow.Concat.nullValue",     "demo.interflow.Concat.objectValue",
      "demo.interflow.Concat.floatingValue",
  };
  for (const char* root : roots) {
    program.roots.push_back(root);
    program.reachableGlobals.push_back(root);
  }

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code = expect(result.ok, "interflow rejected string concat program")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* boxedSymbol =
      findDefinition(optimizedModule, "demo.interflow.Concat.boxedSymbol");
  const scalanative::nir::Definition* boxedString =
      findDefinition(optimizedModule, "demo.interflow.Concat.boxedString");
  const scalanative::nir::Definition* primitive =
      findDefinition(optimizedModule, "demo.interflow.Concat.primitive");
  const scalanative::nir::Definition* nullValue =
      findDefinition(optimizedModule, "demo.interflow.Concat.nullValue");
  const scalanative::nir::Definition* objectValue =
      findDefinition(optimizedModule, "demo.interflow.Concat.objectValue");
  const scalanative::nir::Definition* floatingValue =
      findDefinition(optimizedModule, "demo.interflow.Concat.floatingValue");
  if (int code = expect(boxedSymbol != nullptr && boxedString != nullptr &&
                            primitive != nullptr && nullValue != nullptr &&
                            objectValue != nullptr && floatingValue != nullptr,
                        "interflow removed string concat roots")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(boxedSymbol->body).back() ==
                            "ret String \"symbol='ready\"",
                        "interflow did not fold boxed Symbol string concat")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(boxedString->body).back() ==
                            "ret String \"project=Scala\"",
                        "interflow did not fold boxed String string concat")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(primitive->body).back() ==
                            "ret String \"n=7, ok=false\"",
                        "interflow did not fold primitive string concat chain")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(nullValue->body).back() ==
                            "ret String \"missing=null\"",
                        "interflow did not fold null string concat")) {
    return code;
  }

  const std::vector<std::string> objectText =
      scalanative::nir::bodyToText(objectValue->body);
  if (int code = expect(objectText.size() > 2 && contains(objectText.back(), "%box"),
                        "interflow folded dynamic object string concat")) {
    return code;
  }
  return expect(
      contains(scalanative::nir::bodyToText(floatingValue->body).back(), "1.5"),
      "interflow folded floating string concat formatting");
}

int smokeInterflowAliasReferenceEquality() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();
  const std::string runtimeAnyEquals =
      std::string(scalanative::support::StdNames::RuntimeAnyEquals);
  const std::string runtimeAnyHashCode =
      std::string(scalanative::support::StdNames::RuntimeAnyHashCode);
  const std::string runtimeAnyToString =
      std::string(scalanative::support::StdNames::RuntimeAnyToString);
  const auto newBox = [&] {
    return scalanative::nir::newValue("demo.interflow.AliasBox", noSpan);
  };
  const auto newEffectBox = [&] {
    return scalanative::nir::newValue("demo.interflow.AliasEffectBox", noSpan);
  };
  const auto anyEquals = [&](scalanative::nir::Value left,
                             scalanative::nir::Value right) {
    std::vector<scalanative::nir::Value> arguments;
    arguments.push_back(std::move(left));
    arguments.push_back(std::move(right));
    return scalanative::nir::callValue(
        scalanative::nir::localValue(runtimeAnyEquals, noSpan), std::move(arguments),
        noSpan);
  };
  const auto anyHashCode = [&](scalanative::nir::Value value) {
    std::vector<scalanative::nir::Value> arguments;
    arguments.push_back(std::move(value));
    return scalanative::nir::callValue(
        scalanative::nir::localValue(runtimeAnyHashCode, noSpan), std::move(arguments),
        noSpan);
  };
  const auto anyToString = [&](scalanative::nir::Value value) {
    std::vector<scalanative::nir::Value> arguments;
    arguments.push_back(std::move(value));
    return scalanative::nir::callValue(
        scalanative::nir::localValue(runtimeAnyToString, noSpan), std::move(arguments),
        noSpan);
  };
  const auto effect = [&] {
    return scalanative::nir::callValue(
        scalanative::nir::localValue("demo.interflow.Alias.effect", noSpan), {},
        noSpan);
  };
  const auto effectInt = [&] {
    return scalanative::nir::callValue(
        scalanative::nir::localValue("demo.interflow.Alias.effectInt", noSpan), {},
        noSpan);
  };

  scalanative::nir::FunctionBodyBuilder sameRuntimeBody;
  (void)sameRuntimeBody.addLet("box", "demo.interflow.AliasBox", newBox(), noSpan);
  (void)sameRuntimeBody.addLet("alias", "demo.interflow.AliasBox",
                               scalanative::nir::localValue("box", noSpan), noSpan);
  (void)sameRuntimeBody.addReturn(
      "Boolean",
      anyEquals(scalanative::nir::localValue("box", noSpan),
                scalanative::nir::localValue("alias", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder sameRuntimeNotBody;
  (void)sameRuntimeNotBody.addLet("box", "demo.interflow.AliasBox", newBox(), noSpan);
  (void)sameRuntimeNotBody.addLet("alias", "demo.interflow.AliasBox",
                                  scalanative::nir::localValue("box", noSpan), noSpan);
  (void)sameRuntimeNotBody.addReturn(
      "Boolean",
      scalanative::nir::unaryValue(
          "!",
          anyEquals(scalanative::nir::localValue("box", noSpan),
                    scalanative::nir::localValue("alias", noSpan)),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder distinctRuntimeBody;
  (void)distinctRuntimeBody.addLet("left", "demo.interflow.AliasBox", newBox(), noSpan);
  (void)distinctRuntimeBody.addLet("right", "demo.interflow.AliasBox", newBox(),
                                   noSpan);
  (void)distinctRuntimeBody.addReturn(
      "Boolean",
      anyEquals(scalanative::nir::localValue("left", noSpan),
                scalanative::nir::localValue("right", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder distinctRuntimeNotBody;
  (void)distinctRuntimeNotBody.addLet("left", "demo.interflow.AliasBox", newBox(),
                                      noSpan);
  (void)distinctRuntimeNotBody.addLet("right", "demo.interflow.AliasBox", newBox(),
                                      noSpan);
  (void)distinctRuntimeNotBody.addReturn(
      "Boolean",
      scalanative::nir::unaryValue(
          "!",
          anyEquals(scalanative::nir::localValue("left", noSpan),
                    scalanative::nir::localValue("right", noSpan)),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder sameBinaryBody;
  (void)sameBinaryBody.addLet("box", "demo.interflow.AliasBox", newBox(), noSpan);
  (void)sameBinaryBody.addLet("alias", "demo.interflow.AliasBox",
                              scalanative::nir::localValue("box", noSpan), noSpan);
  (void)sameBinaryBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue("==", scalanative::nir::localValue("box", noSpan),
                                    scalanative::nir::localValue("alias", noSpan),
                                    noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder distinctBinaryBody;
  (void)distinctBinaryBody.addLet("left", "demo.interflow.AliasBox", newBox(), noSpan);
  (void)distinctBinaryBody.addLet("right", "demo.interflow.AliasBox", newBox(), noSpan);
  (void)distinctBinaryBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue("==", scalanative::nir::localValue("left", noSpan),
                                    scalanative::nir::localValue("right", noSpan),
                                    noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder distinctBinaryNotBody;
  (void)distinctBinaryNotBody.addLet("left", "demo.interflow.AliasBox", newBox(),
                                     noSpan);
  (void)distinctBinaryNotBody.addLet("right", "demo.interflow.AliasBox", newBox(),
                                     noSpan);
  (void)distinctBinaryNotBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue("!=", scalanative::nir::localValue("left", noSpan),
                                    scalanative::nir::localValue("right", noSpan),
                                    noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directEffectfulBinaryBody;
  (void)directEffectfulBinaryBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue("==", newEffectBox(), newEffectBox(), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directEffectfulRuntimeBody;
  (void)directEffectfulRuntimeBody.addReturn(
      "Boolean", anyEquals(newEffectBox(), newEffectBox()), noSpan);

  scalanative::nir::FunctionBodyBuilder directEffectfulNullBody;
  (void)directEffectfulNullBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "!=", newEffectBox(), scalanative::nir::literalValue("null", "Null", noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directEffectfulRuntimeNullBody;
  (void)directEffectfulRuntimeNullBody.addReturn(
      "Boolean",
      anyEquals(scalanative::nir::literalValue("null", "Null", noSpan), newEffectBox()),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localEffectfulNullBody;
  (void)localEffectfulNullBody.addLet("box", "demo.interflow.AliasEffectBox",
                                      newEffectBox(), noSpan);
  (void)localEffectfulNullBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "==", scalanative::nir::literalValue("null", "Null", noSpan),
          scalanative::nir::localValue("box", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localEffectfulRuntimeNullBody;
  (void)localEffectfulRuntimeNullBody.addLet("box", "demo.interflow.AliasEffectBox",
                                             newEffectBox(), noSpan);
  (void)localEffectfulRuntimeNullBody.addReturn(
      "Boolean",
      anyEquals(scalanative::nir::localValue("box", noSpan),
                scalanative::nir::literalValue("null", "Null", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directEffectfulBoxTypeTestBody;
  (void)directEffectfulBoxTypeTestBody.addReturn(
      "Boolean",
      scalanative::nir::isInstanceOfValue(
          "Unit", scalanative::nir::boxValue("Unit", effect(), noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localEffectfulBoxTypeMismatchBody;
  (void)localEffectfulBoxTypeMismatchBody.addLet(
      "boxed", "Object", scalanative::nir::boxValue("Unit", effect(), noSpan), noSpan);
  (void)localEffectfulBoxTypeMismatchBody.addReturn(
      "Boolean",
      scalanative::nir::isInstanceOfValue(
          "Int", scalanative::nir::localValue("boxed", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directEffectfulBoxNullBody;
  (void)directEffectfulBoxNullBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "!=", scalanative::nir::boxValue("Unit", effect(), noSpan),
          scalanative::nir::literalValue("null", "Null", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localEffectfulBoxRuntimeNullBody;
  (void)localEffectfulBoxRuntimeNullBody.addLet(
      "boxed", "Object", scalanative::nir::boxValue("Unit", effect(), noSpan), noSpan);
  (void)localEffectfulBoxRuntimeNullBody.addReturn(
      "Boolean",
      anyEquals(scalanative::nir::literalValue("null", "Null", noSpan),
                scalanative::nir::localValue("boxed", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directEffectfulDisjointBoxesBody;
  (void)directEffectfulDisjointBoxesBody.addReturn(
      "Boolean",
      anyEquals(scalanative::nir::boxValue("Unit", effect(), noSpan),
                scalanative::nir::boxValue("Int", effectInt(), noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localEffectfulDisjointBoxesBody;
  (void)localEffectfulDisjointBoxesBody.addLet(
      "left", "Object", scalanative::nir::boxValue("Unit", effect(), noSpan), noSpan);
  (void)localEffectfulDisjointBoxesBody.addLet(
      "right", "Object", scalanative::nir::boxValue("Int", effectInt(), noSpan),
      noSpan);
  (void)localEffectfulDisjointBoxesBody.addReturn(
      "Boolean",
      anyEquals(scalanative::nir::localValue("left", noSpan),
                scalanative::nir::localValue("right", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directEffectfulUnitBoxesBody;
  (void)directEffectfulUnitBoxesBody.addReturn(
      "Boolean",
      anyEquals(scalanative::nir::boxValue("Unit", effect(), noSpan),
                scalanative::nir::boxValue("Unit", effect(), noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localEffectfulUnitBoxesBody;
  (void)localEffectfulUnitBoxesBody.addLet(
      "left", "Object", scalanative::nir::boxValue("Unit", effect(), noSpan), noSpan);
  (void)localEffectfulUnitBoxesBody.addLet(
      "right", "Object", scalanative::nir::boxValue("Unit", effect(), noSpan), noSpan);
  (void)localEffectfulUnitBoxesBody.addReturn(
      "Boolean",
      anyEquals(scalanative::nir::localValue("left", noSpan),
                scalanative::nir::localValue("right", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directDynamicIntBoxesBody;
  (void)directDynamicIntBoxesBody.addReturn(
      "Boolean",
      anyEquals(scalanative::nir::boxValue("Int", effectInt(), noSpan),
                scalanative::nir::boxValue("Int", effectInt(), noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directEffectfulUnitHashBody;
  (void)directEffectfulUnitHashBody.addReturn(
      "Int", anyHashCode(scalanative::nir::boxValue("Unit", effect(), noSpan)), noSpan);

  scalanative::nir::FunctionBodyBuilder localEffectfulUnitHashBody;
  (void)localEffectfulUnitHashBody.addLet(
      "boxed", "Object", scalanative::nir::boxValue("Unit", effect(), noSpan), noSpan);
  (void)localEffectfulUnitHashBody.addReturn(
      "Int", anyHashCode(scalanative::nir::localValue("boxed", noSpan)), noSpan);

  scalanative::nir::FunctionBodyBuilder directDynamicIntHashBody;
  (void)directDynamicIntHashBody.addReturn(
      "Int", anyHashCode(scalanative::nir::boxValue("Int", effectInt(), noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directEffectfulUnitToStringBody;
  (void)directEffectfulUnitToStringBody.addReturn(
      "String", anyToString(scalanative::nir::boxValue("Unit", effect(), noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localEffectfulUnitToStringBody;
  (void)localEffectfulUnitToStringBody.addLet(
      "boxed", "Object", scalanative::nir::boxValue("Unit", effect(), noSpan), noSpan);
  (void)localEffectfulUnitToStringBody.addReturn(
      "String", anyToString(scalanative::nir::localValue("boxed", noSpan)), noSpan);

  scalanative::nir::FunctionBodyBuilder directDynamicIntToStringBody;
  (void)directDynamicIntToStringBody.addReturn(
      "String", anyToString(scalanative::nir::boxValue("Int", effectInt(), noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directEffectfulUnitConcatBody;
  (void)directEffectfulUnitConcatBody.addReturn(
      "String",
      scalanative::nir::binaryValue(
          "+", scalanative::nir::literalValue("\"value=\"", "String", noSpan),
          scalanative::nir::boxValue("Unit", effect(), noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localEffectfulUnitConcatBody;
  (void)localEffectfulUnitConcatBody.addLet(
      "boxed", "Object", scalanative::nir::boxValue("Unit", effect(), noSpan), noSpan);
  (void)localEffectfulUnitConcatBody.addReturn(
      "String",
      scalanative::nir::binaryValue(
          "+", scalanative::nir::literalValue("\"value=\"", "String", noSpan),
          scalanative::nir::localValue("boxed", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directDynamicIntConcatBody;
  (void)directDynamicIntConcatBody.addReturn(
      "String",
      scalanative::nir::binaryValue(
          "+", scalanative::nir::literalValue("\"value=\"", "String", noSpan),
          scalanative::nir::boxValue("Int", effectInt(), noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder sameHashBody;
  (void)sameHashBody.addLet("box", "demo.interflow.AliasBox", newBox(), noSpan);
  (void)sameHashBody.addLet("alias", "demo.interflow.AliasBox",
                            scalanative::nir::localValue("box", noSpan), noSpan);
  (void)sameHashBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "==", anyHashCode(scalanative::nir::localValue("box", noSpan)),
          anyHashCode(scalanative::nir::localValue("alias", noSpan)), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder sameHashNotBody;
  (void)sameHashNotBody.addLet("box", "demo.interflow.AliasBox", newBox(), noSpan);
  (void)sameHashNotBody.addLet("alias", "demo.interflow.AliasBox",
                               scalanative::nir::localValue("box", noSpan), noSpan);
  (void)sameHashNotBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "!=", anyHashCode(scalanative::nir::localValue("box", noSpan)),
          anyHashCode(scalanative::nir::localValue("alias", noSpan)), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder distinctHashBody;
  (void)distinctHashBody.addLet("left", "demo.interflow.AliasBox", newBox(), noSpan);
  (void)distinctHashBody.addLet("right", "demo.interflow.AliasBox", newBox(), noSpan);
  (void)distinctHashBody.addReturn(
      "Boolean",
      scalanative::nir::binaryValue(
          "==", anyHashCode(scalanative::nir::localValue("left", noSpan)),
          anyHashCode(scalanative::nir::localValue("right", noSpan)), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder effectBoxInitializerBody;
  (void)effectBoxInitializerBody.addParameter("this", "demo.interflow.AliasEffectBox",
                                              noSpan);
  (void)effectBoxInitializerBody.addEval(
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.Alias.effect", noSpan), {},
          noSpan),
      noSpan);
  (void)effectBoxInitializerBody.addReturn("Unit", scalanative::nir::unitValue(noSpan),
                                           noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                "demo.interflow.AliasBox",
                                "@java.lang.Object",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                "demo.interflow.AliasEffectBox",
                                "@java.lang.Object",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.AliasEffectBox.$init",
                                "(demo.interflow.AliasEffectBox)Unit",
                                std::move(effectBoxInitializerBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Alias.sameRuntime", "()Boolean",
                                std::move(sameRuntimeBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Alias.sameRuntimeNot", "()Boolean",
                                std::move(sameRuntimeNotBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Alias.distinctRuntime", "()Boolean",
                                std::move(distinctRuntimeBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Alias.distinctRuntimeNot", "()Boolean",
                                std::move(distinctRuntimeNotBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Alias.sameBinary", "()Boolean",
                                std::move(sameBinaryBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Alias.distinctBinary", "()Boolean",
                                std::move(distinctBinaryBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Alias.distinctBinaryNot", "()Boolean",
                                std::move(distinctBinaryNotBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Alias.directEffectfulBinary",
                                "()Boolean",
                                std::move(directEffectfulBinaryBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Alias.directEffectfulRuntime",
                                "()Boolean",
                                std::move(directEffectfulRuntimeBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Alias.directEffectfulNull", "()Boolean",
                                std::move(directEffectfulNullBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Alias.directEffectfulRuntimeNull", "()Boolean",
       std::move(directEffectfulRuntimeNullBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Alias.localEffectfulNull", "()Boolean",
                                std::move(localEffectfulNullBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Alias.localEffectfulRuntimeNull", "()Boolean",
       std::move(localEffectfulRuntimeNullBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Alias.directEffectfulBoxTypeTest", "()Boolean",
       std::move(directEffectfulBoxTypeTestBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Alias.localEffectfulBoxTypeMismatch", "()Boolean",
       std::move(localEffectfulBoxTypeMismatchBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Alias.directEffectfulBoxNull",
                                "()Boolean",
                                std::move(directEffectfulBoxNullBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Alias.localEffectfulBoxRuntimeNull", "()Boolean",
       std::move(localEffectfulBoxRuntimeNullBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Alias.directEffectfulDisjointBoxes", "()Boolean",
       std::move(directEffectfulDisjointBoxesBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Alias.localEffectfulDisjointBoxes", "()Boolean",
       std::move(localEffectfulDisjointBoxesBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Alias.directEffectfulUnitBoxes", "()Boolean",
       std::move(directEffectfulUnitBoxesBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Alias.localEffectfulUnitBoxes", "()Boolean",
       std::move(localEffectfulUnitBoxesBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Alias.directDynamicIntBoxes",
                                "()Boolean",
                                std::move(directDynamicIntBoxesBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Alias.directEffectfulUnitHash", "()Int",
                                std::move(directEffectfulUnitHashBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Alias.localEffectfulUnitHash", "()Int",
                                std::move(localEffectfulUnitHashBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Alias.directDynamicIntHash", "()Int",
                                std::move(directDynamicIntHashBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Alias.directEffectfulUnitToString", "()String",
       std::move(directEffectfulUnitToStringBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Alias.localEffectfulUnitToString", "()String",
       std::move(localEffectfulUnitToStringBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Alias.directDynamicIntToString", "()String",
       std::move(directDynamicIntToStringBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Alias.directEffectfulUnitConcat", "()String",
       std::move(directEffectfulUnitConcatBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Alias.localEffectfulUnitConcat", "()String",
       std::move(localEffectfulUnitConcatBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Alias.directDynamicIntConcat",
                                "()String",
                                std::move(directDynamicIntConcatBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Alias.sameHash", "()Boolean",
                                std::move(sameHashBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Alias.sameHashNot", "()Boolean",
                                std::move(sameHashNotBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Alias.distinctHash", "()Boolean",
                                std::move(distinctHashBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                runtimeAnyEquals,
                                "(Object,Object)Boolean",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                runtimeAnyHashCode,
                                "(Object)Int",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                runtimeAnyToString,
                                "(Object)String",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                "demo.interflow.Alias.effect",
                                "()Unit",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                "demo.interflow.Alias.effectInt",
                                "()Int",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  const std::vector<std::string> roots = {
      "scala.scalanative.runtime.main",
      "demo.interflow.Alias.sameRuntime",
      "demo.interflow.Alias.sameRuntimeNot",
      "demo.interflow.Alias.distinctRuntime",
      "demo.interflow.Alias.distinctRuntimeNot",
      "demo.interflow.Alias.sameBinary",
      "demo.interflow.Alias.distinctBinary",
      "demo.interflow.Alias.distinctBinaryNot",
      "demo.interflow.Alias.directEffectfulBinary",
      "demo.interflow.Alias.directEffectfulRuntime",
      "demo.interflow.Alias.directEffectfulNull",
      "demo.interflow.Alias.directEffectfulRuntimeNull",
      "demo.interflow.Alias.localEffectfulNull",
      "demo.interflow.Alias.localEffectfulRuntimeNull",
      "demo.interflow.Alias.directEffectfulBoxTypeTest",
      "demo.interflow.Alias.localEffectfulBoxTypeMismatch",
      "demo.interflow.Alias.directEffectfulBoxNull",
      "demo.interflow.Alias.localEffectfulBoxRuntimeNull",
      "demo.interflow.Alias.directEffectfulDisjointBoxes",
      "demo.interflow.Alias.localEffectfulDisjointBoxes",
      "demo.interflow.Alias.directEffectfulUnitBoxes",
      "demo.interflow.Alias.localEffectfulUnitBoxes",
      "demo.interflow.Alias.directDynamicIntBoxes",
      "demo.interflow.Alias.directEffectfulUnitHash",
      "demo.interflow.Alias.localEffectfulUnitHash",
      "demo.interflow.Alias.directDynamicIntHash",
      "demo.interflow.Alias.directEffectfulUnitToString",
      "demo.interflow.Alias.localEffectfulUnitToString",
      "demo.interflow.Alias.directDynamicIntToString",
      "demo.interflow.Alias.directEffectfulUnitConcat",
      "demo.interflow.Alias.localEffectfulUnitConcat",
      "demo.interflow.Alias.directDynamicIntConcat",
      "demo.interflow.Alias.sameHash",
      "demo.interflow.Alias.sameHashNot",
      "demo.interflow.Alias.distinctHash",
      runtimeAnyEquals,
      runtimeAnyHashCode,
      runtimeAnyToString,
      "demo.interflow.Alias.effect",
      "demo.interflow.Alias.effectInt",
  };
  for (const std::string& root : roots) {
    program.roots.push_back(root);
    program.reachableGlobals.push_back(root);
  }
  program.reachableGlobals.push_back("demo.interflow.AliasEffectBox");
  program.reachableGlobals.push_back("demo.interflow.AliasEffectBox.$init");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code = expect(result.ok, "interflow rejected alias equality program")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* sameRuntime =
      findDefinition(optimizedModule, "demo.interflow.Alias.sameRuntime");
  const scalanative::nir::Definition* sameRuntimeNot =
      findDefinition(optimizedModule, "demo.interflow.Alias.sameRuntimeNot");
  const scalanative::nir::Definition* distinctRuntime =
      findDefinition(optimizedModule, "demo.interflow.Alias.distinctRuntime");
  const scalanative::nir::Definition* distinctRuntimeNot =
      findDefinition(optimizedModule, "demo.interflow.Alias.distinctRuntimeNot");
  const scalanative::nir::Definition* sameBinary =
      findDefinition(optimizedModule, "demo.interflow.Alias.sameBinary");
  const scalanative::nir::Definition* distinctBinary =
      findDefinition(optimizedModule, "demo.interflow.Alias.distinctBinary");
  const scalanative::nir::Definition* distinctBinaryNot =
      findDefinition(optimizedModule, "demo.interflow.Alias.distinctBinaryNot");
  const scalanative::nir::Definition* directEffectfulBinary =
      findDefinition(optimizedModule, "demo.interflow.Alias.directEffectfulBinary");
  const scalanative::nir::Definition* directEffectfulRuntime =
      findDefinition(optimizedModule, "demo.interflow.Alias.directEffectfulRuntime");
  const scalanative::nir::Definition* directEffectfulNull =
      findDefinition(optimizedModule, "demo.interflow.Alias.directEffectfulNull");
  const scalanative::nir::Definition* directEffectfulRuntimeNull = findDefinition(
      optimizedModule, "demo.interflow.Alias.directEffectfulRuntimeNull");
  const scalanative::nir::Definition* localEffectfulNull =
      findDefinition(optimizedModule, "demo.interflow.Alias.localEffectfulNull");
  const scalanative::nir::Definition* localEffectfulRuntimeNull =
      findDefinition(optimizedModule, "demo.interflow.Alias.localEffectfulRuntimeNull");
  const scalanative::nir::Definition* directEffectfulBoxTypeTest = findDefinition(
      optimizedModule, "demo.interflow.Alias.directEffectfulBoxTypeTest");
  const scalanative::nir::Definition* localEffectfulBoxTypeMismatch = findDefinition(
      optimizedModule, "demo.interflow.Alias.localEffectfulBoxTypeMismatch");
  const scalanative::nir::Definition* directEffectfulBoxNull =
      findDefinition(optimizedModule, "demo.interflow.Alias.directEffectfulBoxNull");
  const scalanative::nir::Definition* localEffectfulBoxRuntimeNull = findDefinition(
      optimizedModule, "demo.interflow.Alias.localEffectfulBoxRuntimeNull");
  const scalanative::nir::Definition* directEffectfulDisjointBoxes = findDefinition(
      optimizedModule, "demo.interflow.Alias.directEffectfulDisjointBoxes");
  const scalanative::nir::Definition* localEffectfulDisjointBoxes = findDefinition(
      optimizedModule, "demo.interflow.Alias.localEffectfulDisjointBoxes");
  const scalanative::nir::Definition* directEffectfulUnitBoxes =
      findDefinition(optimizedModule, "demo.interflow.Alias.directEffectfulUnitBoxes");
  const scalanative::nir::Definition* localEffectfulUnitBoxes =
      findDefinition(optimizedModule, "demo.interflow.Alias.localEffectfulUnitBoxes");
  const scalanative::nir::Definition* directDynamicIntBoxes =
      findDefinition(optimizedModule, "demo.interflow.Alias.directDynamicIntBoxes");
  const scalanative::nir::Definition* directEffectfulUnitHash =
      findDefinition(optimizedModule, "demo.interflow.Alias.directEffectfulUnitHash");
  const scalanative::nir::Definition* localEffectfulUnitHash =
      findDefinition(optimizedModule, "demo.interflow.Alias.localEffectfulUnitHash");
  const scalanative::nir::Definition* directDynamicIntHash =
      findDefinition(optimizedModule, "demo.interflow.Alias.directDynamicIntHash");
  const scalanative::nir::Definition* directEffectfulUnitToString = findDefinition(
      optimizedModule, "demo.interflow.Alias.directEffectfulUnitToString");
  const scalanative::nir::Definition* localEffectfulUnitToString = findDefinition(
      optimizedModule, "demo.interflow.Alias.localEffectfulUnitToString");
  const scalanative::nir::Definition* directDynamicIntToString =
      findDefinition(optimizedModule, "demo.interflow.Alias.directDynamicIntToString");
  const scalanative::nir::Definition* directEffectfulUnitConcat =
      findDefinition(optimizedModule, "demo.interflow.Alias.directEffectfulUnitConcat");
  const scalanative::nir::Definition* localEffectfulUnitConcat =
      findDefinition(optimizedModule, "demo.interflow.Alias.localEffectfulUnitConcat");
  const scalanative::nir::Definition* directDynamicIntConcat =
      findDefinition(optimizedModule, "demo.interflow.Alias.directDynamicIntConcat");
  const scalanative::nir::Definition* sameHash =
      findDefinition(optimizedModule, "demo.interflow.Alias.sameHash");
  const scalanative::nir::Definition* sameHashNot =
      findDefinition(optimizedModule, "demo.interflow.Alias.sameHashNot");
  const scalanative::nir::Definition* distinctHash =
      findDefinition(optimizedModule, "demo.interflow.Alias.distinctHash");
  if (int code = expect(
          sameRuntime != nullptr && sameRuntimeNot != nullptr &&
              distinctRuntime != nullptr && distinctRuntimeNot != nullptr &&
              sameBinary != nullptr && distinctBinary != nullptr &&
              distinctBinaryNot != nullptr && directEffectfulBinary != nullptr &&
              directEffectfulRuntime != nullptr && directEffectfulNull != nullptr &&
              directEffectfulRuntimeNull != nullptr && localEffectfulNull != nullptr &&
              localEffectfulRuntimeNull != nullptr &&
              directEffectfulBoxTypeTest != nullptr &&
              localEffectfulBoxTypeMismatch != nullptr &&
              directEffectfulBoxNull != nullptr &&
              localEffectfulBoxRuntimeNull != nullptr &&
              directEffectfulDisjointBoxes != nullptr &&
              localEffectfulDisjointBoxes != nullptr &&
              directEffectfulUnitBoxes != nullptr &&
              localEffectfulUnitBoxes != nullptr && directDynamicIntBoxes != nullptr &&
              directEffectfulUnitHash != nullptr && localEffectfulUnitHash != nullptr &&
              directDynamicIntHash != nullptr &&
              directEffectfulUnitToString != nullptr &&
              localEffectfulUnitToString != nullptr &&
              directDynamicIntToString != nullptr &&
              directEffectfulUnitConcat != nullptr &&
              localEffectfulUnitConcat != nullptr &&
              directDynamicIntConcat != nullptr && sameHash != nullptr &&
              sameHashNot != nullptr && distinctHash != nullptr,
          "interflow removed alias equality roots")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(sameRuntime->body).back() ==
                            "ret Boolean true",
                        "interflow did not fold same-reference anyEquals")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(sameRuntimeNot->body).back() ==
                            "ret Boolean false",
                        "interflow did not fold negated same-reference anyEquals")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(sameBinary->body).back() ==
                            "ret Boolean true",
                        "interflow did not fold same-reference binary equality")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(distinctRuntime->body).back() ==
                            "ret Boolean false",
                        "interflow did not fold distinct fresh anyEquals")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(distinctRuntimeNot->body).back() ==
                            "ret Boolean true",
                        "interflow did not fold negated distinct fresh anyEquals")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(distinctBinary->body).back() ==
                            "ret Boolean false",
                        "interflow did not fold distinct fresh binary equality")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(distinctBinaryNot->body).back() ==
                            "ret Boolean true",
                        "interflow did not fold distinct fresh binary inequality")) {
    return code;
  }
  const std::vector<std::string> directEffectfulBinaryText =
      scalanative::nir::bodyToText(directEffectfulBinary->body);
  const std::string directEffectfulBinaryResult = directEffectfulBinaryText.size() > 1
                                                      ? directEffectfulBinaryText[1]
                                                      : std::string{};
  const std::string effectAllocation = "new demo.interflow.AliasEffectBox";
  const std::size_t leftAllocation = directEffectfulBinaryResult.find(effectAllocation);
  const std::size_t rightAllocation =
      leftAllocation == std::string::npos
          ? std::string::npos
          : directEffectfulBinaryResult.find(effectAllocation,
                                             leftAllocation + effectAllocation.size());
  if (int code = expect(
          directEffectfulBinaryText.size() == 2 &&
              leftAllocation != std::string::npos &&
              rightAllocation != std::string::npos &&
              leftAllocation < rightAllocation &&
              contains(directEffectfulBinaryResult, "; false)") &&
              !contains(directEffectfulBinaryResult, " == ") &&
              directEffectfulBinaryResult.rfind("ret Boolean ", 0) == 0,
          "interflow did not fold direct fresh equality after both allocations")) {
    return code;
  }
  const std::vector<std::string> directEffectfulRuntimeText =
      scalanative::nir::bodyToText(directEffectfulRuntime->body);
  const std::string directEffectfulRuntimeResult = directEffectfulRuntimeText.size() > 1
                                                       ? directEffectfulRuntimeText[1]
                                                       : std::string{};
  const std::size_t leftRuntimeAllocation =
      directEffectfulRuntimeResult.find(effectAllocation);
  const std::size_t rightRuntimeAllocation =
      leftRuntimeAllocation == std::string::npos
          ? std::string::npos
          : directEffectfulRuntimeResult.find(
                effectAllocation, leftRuntimeAllocation + effectAllocation.size());
  if (int code = expect(
          directEffectfulRuntimeText.size() == 2 &&
              leftRuntimeAllocation != std::string::npos &&
              rightRuntimeAllocation != std::string::npos &&
              leftRuntimeAllocation < rightRuntimeAllocation &&
              contains(directEffectfulRuntimeResult, "; false)") &&
              !contains(directEffectfulRuntimeResult, runtimeAnyEquals) &&
              directEffectfulRuntimeResult.rfind("ret Boolean ", 0) == 0,
          "interflow did not fold direct fresh anyEquals after both allocations")) {
    return code;
  }
  const std::vector<std::string> directEffectfulNullText =
      scalanative::nir::bodyToText(directEffectfulNull->body);
  const std::string directEffectfulNullResult =
      directEffectfulNullText.size() > 1 ? directEffectfulNullText[1] : std::string{};
  if (int code = expect(
          directEffectfulNullText.size() == 2 &&
              contains(directEffectfulNullResult, effectAllocation) &&
              contains(directEffectfulNullResult, "; true)") &&
              !contains(directEffectfulNullResult, " != ") &&
              directEffectfulNullResult.rfind("ret Boolean ", 0) == 0,
          "interflow did not fold direct fresh non-null comparison after effect")) {
    return code;
  }
  const std::vector<std::string> directEffectfulRuntimeNullText =
      scalanative::nir::bodyToText(directEffectfulRuntimeNull->body);
  const std::string directEffectfulRuntimeNullResult =
      directEffectfulRuntimeNullText.size() > 1 ? directEffectfulRuntimeNullText[1]
                                                : std::string{};
  if (int code = expect(
          directEffectfulRuntimeNullText.size() == 2 &&
              contains(directEffectfulRuntimeNullResult, effectAllocation) &&
              contains(directEffectfulRuntimeNullResult, "; false)") &&
              !contains(directEffectfulRuntimeNullResult, runtimeAnyEquals) &&
              directEffectfulRuntimeNullResult.rfind("ret Boolean ", 0) == 0,
          "interflow did not fold null/fresh anyEquals after allocation effect")) {
    return code;
  }
  const std::vector<std::string> localEffectfulNullText =
      scalanative::nir::bodyToText(localEffectfulNull->body);
  if (int code =
          expect(localEffectfulNullText.size() == 3 &&
                     contains(localEffectfulNullText[1], effectAllocation) &&
                     localEffectfulNullText.back() == "ret Boolean false",
                 "interflow did not fold evaluated fresh local null comparison")) {
    return code;
  }
  const std::vector<std::string> localEffectfulRuntimeNullText =
      scalanative::nir::bodyToText(localEffectfulRuntimeNull->body);
  if (int code =
          expect(localEffectfulRuntimeNullText.size() == 3 &&
                     contains(localEffectfulRuntimeNullText[1], effectAllocation) &&
                     localEffectfulRuntimeNullText.back() == "ret Boolean false" &&
                     !contains(localEffectfulRuntimeNullText.back(), runtimeAnyEquals),
                 "interflow did not fold evaluated fresh local anyEquals")) {
    return code;
  }
  const std::vector<std::string> directEffectfulBoxTypeTestText =
      scalanative::nir::bodyToText(directEffectfulBoxTypeTest->body);
  if (int code =
          expect(directEffectfulBoxTypeTestText.size() == 2 &&
                     contains(directEffectfulBoxTypeTestText[1],
                              "call %demo.interflow.Alias.effect()") &&
                     contains(directEffectfulBoxTypeTestText[1], "; true)") &&
                     !contains(directEffectfulBoxTypeTestText[1], "box[Unit]") &&
                     !contains(directEffectfulBoxTypeTestText[1], "is-instance-of") &&
                     directEffectfulBoxTypeTestText[1].rfind("ret Boolean ", 0) == 0,
                 "interflow did not fold effectful direct boxed type test")) {
    return code;
  }
  const std::vector<std::string> localEffectfulBoxTypeMismatchText =
      scalanative::nir::bodyToText(localEffectfulBoxTypeMismatch->body);
  if (int code = expect(localEffectfulBoxTypeMismatchText.size() == 3 &&
                            localEffectfulBoxTypeMismatchText[1] ==
                                "eval call %demo.interflow.Alias.effect()" &&
                            localEffectfulBoxTypeMismatchText[2] == "ret Boolean false",
                        "interflow did not fold effectful local boxed type mismatch")) {
    return code;
  }
  const std::vector<std::string> directEffectfulBoxNullText =
      scalanative::nir::bodyToText(directEffectfulBoxNull->body);
  if (int code =
          expect(directEffectfulBoxNullText.size() == 2 &&
                     contains(directEffectfulBoxNullText[1],
                              "call %demo.interflow.Alias.effect()") &&
                     contains(directEffectfulBoxNullText[1], "; true)") &&
                     !contains(directEffectfulBoxNullText[1], "box[Unit]") &&
                     !contains(directEffectfulBoxNullText[1], " != ") &&
                     directEffectfulBoxNullText[1].rfind("ret Boolean ", 0) == 0,
                 "interflow did not fold effectful direct box/null comparison")) {
    return code;
  }
  const std::vector<std::string> localEffectfulBoxRuntimeNullText =
      scalanative::nir::bodyToText(localEffectfulBoxRuntimeNull->body);
  if (int code =
          expect(localEffectfulBoxRuntimeNullText.size() == 3 &&
                     localEffectfulBoxRuntimeNullText[1] ==
                         "eval call %demo.interflow.Alias.effect()" &&
                     localEffectfulBoxRuntimeNullText[2] == "ret Boolean false" &&
                     !contains(localEffectfulBoxRuntimeNullText[2], runtimeAnyEquals),
                 "interflow did not fold effectful local box/null anyEquals")) {
    return code;
  }
  const std::vector<std::string> directEffectfulDisjointBoxesText =
      scalanative::nir::bodyToText(directEffectfulDisjointBoxes->body);
  const std::string directEffectfulDisjointBoxesResult =
      directEffectfulDisjointBoxesText.size() > 1 ? directEffectfulDisjointBoxesText[1]
                                                  : std::string{};
  const std::size_t directUnitEffect =
      directEffectfulDisjointBoxesResult.find("call %demo.interflow.Alias.effect()");
  const std::size_t directIntEffect =
      directEffectfulDisjointBoxesResult.find("call %demo.interflow.Alias.effectInt()");
  if (int code = expect(
          directEffectfulDisjointBoxesText.size() == 2 &&
              directUnitEffect != std::string::npos &&
              directIntEffect != std::string::npos &&
              directUnitEffect < directIntEffect &&
              contains(directEffectfulDisjointBoxesResult, "; false)") &&
              !contains(directEffectfulDisjointBoxesResult, "box[") &&
              !contains(directEffectfulDisjointBoxesResult, runtimeAnyEquals) &&
              directEffectfulDisjointBoxesResult.rfind("ret Boolean ", 0) == 0,
          "interflow did not fold disjoint direct boxes after ordered effects")) {
    return code;
  }
  const std::vector<std::string> localEffectfulDisjointBoxesText =
      scalanative::nir::bodyToText(localEffectfulDisjointBoxes->body);
  if (int code =
          expect(localEffectfulDisjointBoxesText.size() == 4 &&
                     localEffectfulDisjointBoxesText[1] ==
                         "eval call %demo.interflow.Alias.effect()" &&
                     localEffectfulDisjointBoxesText[2] ==
                         "eval call %demo.interflow.Alias.effectInt()" &&
                     localEffectfulDisjointBoxesText[3] == "ret Boolean false",
                 "interflow did not fold disjoint local boxes after ordered effects")) {
    return code;
  }
  const std::vector<std::string> directEffectfulUnitBoxesText =
      scalanative::nir::bodyToText(directEffectfulUnitBoxes->body);
  const std::string directEffectfulUnitBoxesResult =
      directEffectfulUnitBoxesText.size() > 1 ? directEffectfulUnitBoxesText[1]
                                              : std::string{};
  const std::string unitEffectCall = "call %demo.interflow.Alias.effect()";
  const std::size_t firstUnitEffect =
      directEffectfulUnitBoxesResult.find(unitEffectCall);
  const std::size_t secondUnitEffect =
      firstUnitEffect == std::string::npos
          ? std::string::npos
          : directEffectfulUnitBoxesResult.find(
                unitEffectCall, firstUnitEffect + unitEffectCall.size());
  if (int code =
          expect(directEffectfulUnitBoxesText.size() == 2 &&
                     firstUnitEffect != std::string::npos &&
                     secondUnitEffect != std::string::npos &&
                     firstUnitEffect < secondUnitEffect &&
                     contains(directEffectfulUnitBoxesResult, "; true)") &&
                     !contains(directEffectfulUnitBoxesResult, "box[") &&
                     !contains(directEffectfulUnitBoxesResult, runtimeAnyEquals) &&
                     directEffectfulUnitBoxesResult.rfind("ret Boolean ", 0) == 0,
                 "interflow did not fold direct Unit boxes after ordered effects")) {
    return code;
  }
  const std::vector<std::string> localEffectfulUnitBoxesText =
      scalanative::nir::bodyToText(localEffectfulUnitBoxes->body);
  if (int code =
          expect(localEffectfulUnitBoxesText.size() == 4 &&
                     localEffectfulUnitBoxesText[1] ==
                         "eval call %demo.interflow.Alias.effect()" &&
                     localEffectfulUnitBoxesText[2] ==
                         "eval call %demo.interflow.Alias.effect()" &&
                     localEffectfulUnitBoxesText[3] == "ret Boolean true",
                 "interflow did not fold local Unit boxes after ordered effects")) {
    return code;
  }
  const std::vector<std::string> directDynamicIntBoxesText =
      scalanative::nir::bodyToText(directDynamicIntBoxes->body);
  if (int code = expect(directDynamicIntBoxesText.size() == 2 &&
                            contains(directDynamicIntBoxesText[1], runtimeAnyEquals) &&
                            contains(directDynamicIntBoxesText[1], "box[Int]") &&
                            contains(directDynamicIntBoxesText[1],
                                     "call %demo.interflow.Alias.effectInt()"),
                        "interflow folded dynamic same-type Int boxes")) {
    return code;
  }
  const std::vector<std::string> directEffectfulUnitHashText =
      scalanative::nir::bodyToText(directEffectfulUnitHash->body);
  if (int code =
          expect(directEffectfulUnitHashText.size() == 2 &&
                     contains(directEffectfulUnitHashText[1],
                              "call %demo.interflow.Alias.effect()") &&
                     contains(directEffectfulUnitHashText[1], "; 0)") &&
                     !contains(directEffectfulUnitHashText[1], "box[Unit]") &&
                     !contains(directEffectfulUnitHashText[1], runtimeAnyHashCode) &&
                     directEffectfulUnitHashText[1].rfind("ret Int ", 0) == 0,
                 "interflow did not fold direct effectful Unit hash")) {
    return code;
  }
  const std::vector<std::string> localEffectfulUnitHashText =
      scalanative::nir::bodyToText(localEffectfulUnitHash->body);
  if (int code = expect(localEffectfulUnitHashText.size() == 3 &&
                            localEffectfulUnitHashText[1] ==
                                "eval call %demo.interflow.Alias.effect()" &&
                            localEffectfulUnitHashText[2] == "ret Int 0",
                        "interflow did not fold local effectful Unit hash")) {
    return code;
  }
  const std::vector<std::string> directDynamicIntHashText =
      scalanative::nir::bodyToText(directDynamicIntHash->body);
  if (int code = expect(directDynamicIntHashText.size() == 2 &&
                            contains(directDynamicIntHashText[1], runtimeAnyHashCode) &&
                            contains(directDynamicIntHashText[1], "box[Int]") &&
                            contains(directDynamicIntHashText[1],
                                     "call %demo.interflow.Alias.effectInt()"),
                        "interflow folded dynamic effectful Int hash")) {
    return code;
  }
  const std::vector<std::string> directEffectfulUnitToStringText =
      scalanative::nir::bodyToText(directEffectfulUnitToString->body);
  if (int code = expect(
          directEffectfulUnitToStringText.size() == 2 &&
              contains(directEffectfulUnitToStringText[1],
                       "call %demo.interflow.Alias.effect()") &&
              contains(directEffectfulUnitToStringText[1], "; \"()\")") &&
              !contains(directEffectfulUnitToStringText[1], "box[Unit]") &&
              !contains(directEffectfulUnitToStringText[1], runtimeAnyToString) &&
              directEffectfulUnitToStringText[1].rfind("ret String ", 0) == 0,
          "interflow did not fold direct effectful Unit toString")) {
    return code;
  }
  const std::vector<std::string> localEffectfulUnitToStringText =
      scalanative::nir::bodyToText(localEffectfulUnitToString->body);
  if (int code = expect(localEffectfulUnitToStringText.size() == 3 &&
                            localEffectfulUnitToStringText[1] ==
                                "eval call %demo.interflow.Alias.effect()" &&
                            localEffectfulUnitToStringText[2] == "ret String \"()\"",
                        "interflow did not fold local effectful Unit toString")) {
    return code;
  }
  const std::vector<std::string> directDynamicIntToStringText =
      scalanative::nir::bodyToText(directDynamicIntToString->body);
  if (int code =
          expect(directDynamicIntToStringText.size() == 2 &&
                     contains(directDynamicIntToStringText[1], runtimeAnyToString) &&
                     contains(directDynamicIntToStringText[1], "box[Int]") &&
                     contains(directDynamicIntToStringText[1],
                              "call %demo.interflow.Alias.effectInt()"),
                 "interflow folded dynamic effectful Int toString")) {
    return code;
  }
  const std::vector<std::string> directEffectfulUnitConcatText =
      scalanative::nir::bodyToText(directEffectfulUnitConcat->body);
  if (int code =
          expect(directEffectfulUnitConcatText.size() == 2 &&
                     contains(directEffectfulUnitConcatText[1],
                              "call %demo.interflow.Alias.effect()") &&
                     contains(directEffectfulUnitConcatText[1], "\"value=()\"") &&
                     !contains(directEffectfulUnitConcatText[1], "box[Unit]") &&
                     !contains(directEffectfulUnitConcatText[1], " + ") &&
                     directEffectfulUnitConcatText[1].rfind("ret String ", 0) == 0,
                 "interflow did not fold direct effectful Unit string concat")) {
    return code;
  }
  const std::vector<std::string> localEffectfulUnitConcatText =
      scalanative::nir::bodyToText(localEffectfulUnitConcat->body);
  if (int code =
          expect(localEffectfulUnitConcatText.size() == 3 &&
                     localEffectfulUnitConcatText[1] ==
                         "eval call %demo.interflow.Alias.effect()" &&
                     localEffectfulUnitConcatText[2] == "ret String \"value=()\"",
                 "interflow did not fold local effectful Unit string concat")) {
    return code;
  }
  const std::vector<std::string> directDynamicIntConcatText =
      scalanative::nir::bodyToText(directDynamicIntConcat->body);
  if (int code = expect(directDynamicIntConcatText.size() == 2 &&
                            contains(directDynamicIntConcatText[1], "box[Int]") &&
                            contains(directDynamicIntConcatText[1],
                                     "call %demo.interflow.Alias.effectInt()") &&
                            contains(directDynamicIntConcatText[1], " + "),
                        "interflow folded dynamic effectful Int string concat")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(sameHash->body).back() ==
                            "ret Boolean true",
                        "interflow did not fold same-reference hash equality")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(sameHashNot->body).back() ==
                            "ret Boolean false",
                        "interflow did not fold same-reference hash inequality")) {
    return code;
  }
  return expect(contains(scalanative::nir::bodyToText(distinctHash->body).back(),
                         runtimeAnyHashCode),
                "interflow folded distinct-reference hash equality");
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

  scalanative::nir::FunctionBodyBuilder localAddOneBody;
  (void)localAddOneBody.addParameter("value", "Int", noSpan);
  (void)localAddOneBody.addLet(
      "next", "Int",
      scalanative::nir::binaryValue("+", scalanative::nir::localValue("value", noSpan),
                                    scalanative::nir::literalValue("1", "Int", noSpan),
                                    noSpan),
      noSpan);
  (void)localAddOneBody.addReturn("Int", scalanative::nir::localValue("next", noSpan),
                                  noSpan);

  scalanative::nir::FunctionBodyBuilder localAddOneCallerBody;
  (void)localAddOneCallerBody.addReturn(
      "Int",
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.Inline.localAddOne", noSpan),
          {scalanative::nir::literalValue("41", "Int", noSpan)}, noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder addTwoBody;
  (void)addTwoBody.addParameter("value", "Int", noSpan);
  (void)addTwoBody.addReturn(
      "Int",
      scalanative::nir::binaryValue(
          "+",
          scalanative::nir::callValue(
              scalanative::nir::localValue("demo.interflow.Inline.addOne", noSpan),
              {scalanative::nir::localValue("value", noSpan)}, noSpan),
          scalanative::nir::literalValue("1", "Int", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder addTwoCallerBody;
  (void)addTwoCallerBody.addReturn(
      "Int",
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.Inline.addTwo", noSpan),
          {scalanative::nir::literalValue("40", "Int", noSpan)}, noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder addTwoViaLocalBody;
  (void)addTwoViaLocalBody.addParameter("value", "Int", noSpan);
  (void)addTwoViaLocalBody.addLet(
      "next", "Int",
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.Inline.addOne", noSpan),
          {scalanative::nir::localValue("value", noSpan)}, noSpan),
      noSpan);
  (void)addTwoViaLocalBody.addReturn(
      "Int",
      scalanative::nir::binaryValue("+", scalanative::nir::localValue("next", noSpan),
                                    scalanative::nir::literalValue("1", "Int", noSpan),
                                    noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder addTwoViaLocalCallerBody;
  (void)addTwoViaLocalCallerBody.addReturn(
      "Int",
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.Inline.addTwoViaLocal", noSpan),
          {scalanative::nir::literalValue("40", "Int", noSpan)}, noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder blockAddOneBody;
  (void)blockAddOneBody.addParameter("value", "Int", noSpan);
  (void)blockAddOneBody.addReturn(
      "Int",
      scalanative::nir::blockValue(
          {scalanative::nir::localLetValue(
               "next", "Int",
               scalanative::nir::binaryValue(
                   "+", scalanative::nir::localValue("value", noSpan),
                   scalanative::nir::literalValue("1", "Int", noSpan), noSpan),
               noSpan),
           scalanative::nir::localValue("next", noSpan)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder blockAddOneCallerBody;
  (void)blockAddOneCallerBody.addReturn(
      "Int",
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.Inline.blockAddOne", noSpan),
          {scalanative::nir::literalValue("41", "Int", noSpan)}, noSpan),
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
                                "demo.interflow.Inline.localAddOne", "(Int)Int",
                                std::move(localAddOneBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Inline.localAddOneCaller", "()Int",
                                std::move(localAddOneCallerBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Inline.addTwo", "(Int)Int",
                                std::move(addTwoBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Inline.addTwoCaller", "()Int",
                                std::move(addTwoCallerBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Inline.addTwoViaLocal", "(Int)Int",
                                std::move(addTwoViaLocalBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Inline.addTwoViaLocalCaller", "()Int",
                                std::move(addTwoViaLocalCallerBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Inline.blockAddOne", "(Int)Int",
                                std::move(blockAddOneBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Inline.blockAddOneCaller", "()Int",
                                std::move(blockAddOneCallerBody).build(), noSpan});
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
  const char* inspectedRoots[] = {
      "demo.interflow.Inline.caller",
      "demo.interflow.Inline.direct",
      "demo.interflow.Inline.shortNoArgLocal",
      "demo.interflow.Inline.addOneCaller",
      "demo.interflow.Inline.shortAddOneCaller",
      "demo.interflow.Inline.localAddOneCaller",
      "demo.interflow.Inline.addTwoCaller",
      "demo.interflow.Inline.addTwoViaLocalCaller",
      "demo.interflow.Inline.blockAddOneCaller",
      "demo.interflow.Inline.ignoredEffectfulCaller",
      "demo.interflow.Inline.effectfulCaller",
  };
  for (const char* root : inspectedRoots) {
    program.roots.push_back(root);
  }
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.Inline.constant");
  program.reachableGlobals.push_back("demo.interflow.Inline.caller");
  program.reachableGlobals.push_back("demo.interflow.Inline.direct");
  program.reachableGlobals.push_back("demo.interflow.Inline.shortNoArgLocal");
  program.reachableGlobals.push_back("demo.interflow.Inline.addOne");
  program.reachableGlobals.push_back("demo.interflow.Inline.addOneCaller");
  program.reachableGlobals.push_back("demo.interflow.Inline.shortAddOneCaller");
  program.reachableGlobals.push_back("demo.interflow.Inline.localAddOne");
  program.reachableGlobals.push_back("demo.interflow.Inline.localAddOneCaller");
  program.reachableGlobals.push_back("demo.interflow.Inline.addTwo");
  program.reachableGlobals.push_back("demo.interflow.Inline.addTwoCaller");
  program.reachableGlobals.push_back("demo.interflow.Inline.addTwoViaLocal");
  program.reachableGlobals.push_back("demo.interflow.Inline.addTwoViaLocalCaller");
  program.reachableGlobals.push_back("demo.interflow.Inline.blockAddOne");
  program.reachableGlobals.push_back("demo.interflow.Inline.blockAddOneCaller");
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
  const scalanative::tools::interflow::PassReport* foldReport =
      findPassReport(result, "fold-constants");
  const scalanative::tools::interflow::PassReport* lateFoldReport =
      findPassReport(result, "fold-cleaned-constants");
  if (int code =
          expect(foldReport != nullptr && foldReport->changedValues == 10 &&
                     foldReport->validationErrorsBefore == 0 &&
                     foldReport->validationErrorsAfter == 0 &&
                     lateFoldReport != nullptr && lateFoldReport->changedValues != 0 &&
                     lateFoldReport->validationErrorsBefore == 0 &&
                     lateFoldReport->validationErrorsAfter == 0,
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
  const scalanative::nir::Definition* localAddOneCaller =
      findDefinition(optimizedModule, "demo.interflow.Inline.localAddOneCaller");
  const scalanative::nir::Definition* addTwoCaller =
      findDefinition(optimizedModule, "demo.interflow.Inline.addTwoCaller");
  const scalanative::nir::Definition* addTwoViaLocalCaller =
      findDefinition(optimizedModule, "demo.interflow.Inline.addTwoViaLocalCaller");
  const scalanative::nir::Definition* blockAddOneCaller =
      findDefinition(optimizedModule, "demo.interflow.Inline.blockAddOneCaller");
  const scalanative::nir::Definition* ignoredEffectfulCaller =
      findDefinition(optimizedModule, "demo.interflow.Inline.ignoredEffectfulCaller");
  const scalanative::nir::Definition* effectfulCaller =
      findDefinition(optimizedModule, "demo.interflow.Inline.effectfulCaller");
  const scalanative::nir::Definition* constant =
      findDefinition(optimizedModule, "demo.interflow.Inline.constant");
  const scalanative::nir::Definition* addOne =
      findDefinition(optimizedModule, "demo.interflow.Inline.addOne");
  const scalanative::nir::Definition* localAddOne =
      findDefinition(optimizedModule, "demo.interflow.Inline.localAddOne");
  const scalanative::nir::Definition* addTwo =
      findDefinition(optimizedModule, "demo.interflow.Inline.addTwo");
  const scalanative::nir::Definition* addTwoViaLocal =
      findDefinition(optimizedModule, "demo.interflow.Inline.addTwoViaLocal");
  const scalanative::nir::Definition* blockAddOne =
      findDefinition(optimizedModule, "demo.interflow.Inline.blockAddOne");
  const scalanative::nir::Definition* ignored =
      findDefinition(optimizedModule, "demo.interflow.Inline.ignored");
  const scalanative::nir::Definition* effectful =
      findDefinition(optimizedModule, "demo.interflow.Inline.effectful");
  if (int code =
          expect(caller != nullptr && direct != nullptr && shortNoArgLocal != nullptr &&
                     addOneCaller != nullptr && shortAddOneCaller != nullptr &&
                     localAddOneCaller != nullptr && addTwoCaller != nullptr &&
                     addTwoViaLocalCaller != nullptr && blockAddOneCaller != nullptr &&
                     ignoredEffectfulCaller != nullptr && effectfulCaller != nullptr,
                 "interflow removed reachable inlining functions")) {
    return code;
  }
  if (int code = expect(
          constant == nullptr && addOne == nullptr && localAddOne == nullptr &&
              addTwo == nullptr && addTwoViaLocal == nullptr &&
              blockAddOne == nullptr && ignored != nullptr && effectful != nullptr,
          "interflow did not prune only now-unused pure helper functions")) {
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
  if (int code = expect(
          scalanative::nir::bodyToText(localAddOneCaller->body).back() == "ret Int 42",
          "interflow did not inline pure straight-line local-let function")) {
    return code;
  }
  if (int code = expect(
          scalanative::nir::bodyToText(addTwoCaller->body).back() == "ret Int 42",
          "interflow did not reach summary fixed point for pure wrapper")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(addTwoViaLocalCaller->body).back() ==
                     "ret Int 42",
                 "interflow did not summarize wrapper with exposed pure local call")) {
    return code;
  }
  if (int code = expect(
          scalanative::nir::bodyToText(blockAddOneCaller->body).back() == "ret Int 42",
          "interflow did not late-inline helper simplified after block cleanup")) {
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

int smokeInterflowEffectfulSameBranchIf() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  const auto intValue = [&](int value) {
    return scalanative::nir::literalValue(std::to_string(value), "Int", noSpan);
  };
  const auto boolValue = [&](bool value) {
    return scalanative::nir::literalValue(value ? "true" : "false", "Boolean", noSpan);
  };
  const auto effectBoolCall = [&] {
    return scalanative::nir::callValue(
        scalanative::nir::localValue("demo.interflow.If.effectBool", noSpan), {},
        noSpan);
  };

  scalanative::nir::FunctionBodyBuilder sameBranchBody;
  (void)sameBranchBody.addReturn(
      "Int",
      scalanative::nir::ifValue(effectBoolCall(), intValue(7), intValue(7), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder discardedBody;
  (void)discardedBody.addEval(
      scalanative::nir::ifValue(effectBoolCall(), intValue(7), intValue(7), noSpan),
      noSpan);
  (void)discardedBody.addReturn("Int", intValue(42), noSpan);

  scalanative::nir::FunctionBodyBuilder nestedDiscardedBody;
  (void)nestedDiscardedBody.addReturn(
      "Int",
      scalanative::nir::blockValue(
          {scalanative::nir::ifValue(effectBoolCall(), intValue(8), intValue(8),
                                     noSpan),
           intValue(99)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder discardedDifferentPureBranchesBody;
  (void)discardedDifferentPureBranchesBody.addEval(
      scalanative::nir::ifValue(effectBoolCall(), intValue(1), intValue(2), noSpan),
      noSpan);
  (void)discardedDifferentPureBranchesBody.addReturn("Int", intValue(43), noSpan);

  scalanative::nir::FunctionBodyBuilder nestedDifferentPureBranchesBody;
  (void)nestedDifferentPureBranchesBody.addReturn(
      "Int",
      scalanative::nir::blockValue(
          {scalanative::nir::ifValue(effectBoolCall(), intValue(1), intValue(2),
                                     noSpan),
           intValue(100)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder effectfulBranchBody;
  (void)effectfulBranchBody.addEval(scalanative::nir::ifValue(effectBoolCall(),
                                                              effectBoolCall(),
                                                              boolValue(false), noSpan),
                                    noSpan);
  (void)effectfulBranchBody.addReturn("Int", intValue(44), noSpan);

  scalanative::nir::FunctionBodyBuilder effectfulElseBranchBody;
  (void)effectfulElseBranchBody.addEval(
      scalanative::nir::ifValue(effectBoolCall(), boolValue(true), effectBoolCall(),
                                noSpan),
      noSpan);
  (void)effectfulElseBranchBody.addReturn("Int", intValue(47), noSpan);

  scalanative::nir::FunctionBodyBuilder discardedLogicalAndBody;
  (void)discardedLogicalAndBody.addEval(
      scalanative::nir::binaryValue("&&", effectBoolCall(), effectBoolCall(), noSpan),
      noSpan);
  (void)discardedLogicalAndBody.addReturn("Int", intValue(45), noSpan);

  scalanative::nir::FunctionBodyBuilder discardedLogicalOrBody;
  (void)discardedLogicalOrBody.addEval(
      scalanative::nir::binaryValue("||", effectBoolCall(), effectBoolCall(), noSpan),
      noSpan);
  (void)discardedLogicalOrBody.addReturn("Int", intValue(46), noSpan);

  scalanative::nir::FunctionBodyBuilder discardedWhileLogicalBody;
  (void)discardedWhileLogicalBody.addEval(
      scalanative::nir::whileValue(effectBoolCall(),
                                   scalanative::nir::binaryValue("&&", effectBoolCall(),
                                                                 effectBoolCall(),
                                                                 noSpan),
                                   noSpan),
      noSpan);
  (void)discardedWhileLogicalBody.addReturn("Int", intValue(48), noSpan);

  scalanative::nir::FunctionBodyBuilder returnedWhileLogicalBody;
  (void)returnedWhileLogicalBody.addReturn(
      "Unit",
      scalanative::nir::whileValue(effectBoolCall(),
                                   scalanative::nir::binaryValue("&&", effectBoolCall(),
                                                                 effectBoolCall(),
                                                                 noSpan),
                                   noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder booleanIdentityBody;
  (void)booleanIdentityBody.addReturn(
      "Boolean",
      scalanative::nir::ifValue(effectBoolCall(), boolValue(true), boolValue(false),
                                noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder booleanInvertedBody;
  (void)booleanInvertedBody.addReturn(
      "Boolean",
      scalanative::nir::ifValue(effectBoolCall(), boolValue(false), boolValue(true),
                                noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn("Int", intValue(0), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.If.sameBranch", "()Int",
                                std::move(sameBranchBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.If.discardedSameBranch", "()Int",
                                std::move(discardedBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.If.nestedDiscardedSameBranch", "()Int",
                                std::move(nestedDiscardedBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.If.discardedDifferentPureBranches", "()Int",
       std::move(discardedDifferentPureBranchesBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.If.nestedDifferentPureBranches", "()Int",
       std::move(nestedDifferentPureBranchesBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.If.effectfulBranch", "()Int",
                                std::move(effectfulBranchBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.If.effectfulElseBranch", "()Int",
                                std::move(effectfulElseBranchBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.If.discardedLogicalAnd", "()Int",
                                std::move(discardedLogicalAndBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.If.discardedLogicalOr", "()Int",
                                std::move(discardedLogicalOrBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.If.discardedWhileLogical", "()Int",
                                std::move(discardedWhileLogicalBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.If.returnedWhileLogical", "()Unit",
                                std::move(returnedWhileLogicalBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.If.booleanIdentity", "()Boolean",
                                std::move(booleanIdentityBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.If.booleanInverted", "()Boolean",
                                std::move(booleanInvertedBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                "demo.interflow.If.effectBool",
                                "()Boolean",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.roots.push_back("demo.interflow.If.sameBranch");
  program.roots.push_back("demo.interflow.If.discardedSameBranch");
  program.roots.push_back("demo.interflow.If.nestedDiscardedSameBranch");
  program.roots.push_back("demo.interflow.If.discardedDifferentPureBranches");
  program.roots.push_back("demo.interflow.If.nestedDifferentPureBranches");
  program.roots.push_back("demo.interflow.If.effectfulBranch");
  program.roots.push_back("demo.interflow.If.effectfulElseBranch");
  program.roots.push_back("demo.interflow.If.discardedLogicalAnd");
  program.roots.push_back("demo.interflow.If.discardedLogicalOr");
  program.roots.push_back("demo.interflow.If.discardedWhileLogical");
  program.roots.push_back("demo.interflow.If.returnedWhileLogical");
  program.roots.push_back("demo.interflow.If.booleanIdentity");
  program.roots.push_back("demo.interflow.If.booleanInverted");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.If.sameBranch");
  program.reachableGlobals.push_back("demo.interflow.If.discardedSameBranch");
  program.reachableGlobals.push_back("demo.interflow.If.nestedDiscardedSameBranch");
  program.reachableGlobals.push_back(
      "demo.interflow.If.discardedDifferentPureBranches");
  program.reachableGlobals.push_back("demo.interflow.If.nestedDifferentPureBranches");
  program.reachableGlobals.push_back("demo.interflow.If.effectfulBranch");
  program.reachableGlobals.push_back("demo.interflow.If.effectfulElseBranch");
  program.reachableGlobals.push_back("demo.interflow.If.discardedLogicalAnd");
  program.reachableGlobals.push_back("demo.interflow.If.discardedLogicalOr");
  program.reachableGlobals.push_back("demo.interflow.If.discardedWhileLogical");
  program.reachableGlobals.push_back("demo.interflow.If.returnedWhileLogical");
  program.reachableGlobals.push_back("demo.interflow.If.booleanIdentity");
  program.reachableGlobals.push_back("demo.interflow.If.booleanInverted");
  program.reachableGlobals.push_back("demo.interflow.If.effectBool");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected effectful same-branch if program")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* sameBranch =
      findDefinition(optimizedModule, "demo.interflow.If.sameBranch");
  const scalanative::nir::Definition* discarded =
      findDefinition(optimizedModule, "demo.interflow.If.discardedSameBranch");
  const scalanative::nir::Definition* nestedDiscarded =
      findDefinition(optimizedModule, "demo.interflow.If.nestedDiscardedSameBranch");
  const scalanative::nir::Definition* discardedDifferentPureBranches = findDefinition(
      optimizedModule, "demo.interflow.If.discardedDifferentPureBranches");
  const scalanative::nir::Definition* nestedDifferentPureBranches =
      findDefinition(optimizedModule, "demo.interflow.If.nestedDifferentPureBranches");
  const scalanative::nir::Definition* effectfulBranch =
      findDefinition(optimizedModule, "demo.interflow.If.effectfulBranch");
  const scalanative::nir::Definition* effectfulElseBranch =
      findDefinition(optimizedModule, "demo.interflow.If.effectfulElseBranch");
  const scalanative::nir::Definition* discardedLogicalAnd =
      findDefinition(optimizedModule, "demo.interflow.If.discardedLogicalAnd");
  const scalanative::nir::Definition* discardedLogicalOr =
      findDefinition(optimizedModule, "demo.interflow.If.discardedLogicalOr");
  const scalanative::nir::Definition* discardedWhileLogical =
      findDefinition(optimizedModule, "demo.interflow.If.discardedWhileLogical");
  const scalanative::nir::Definition* returnedWhileLogical =
      findDefinition(optimizedModule, "demo.interflow.If.returnedWhileLogical");
  const scalanative::nir::Definition* booleanIdentity =
      findDefinition(optimizedModule, "demo.interflow.If.booleanIdentity");
  const scalanative::nir::Definition* booleanInverted =
      findDefinition(optimizedModule, "demo.interflow.If.booleanInverted");
  if (int code = expect(
          sameBranch != nullptr && discarded != nullptr && nestedDiscarded != nullptr &&
              discardedDifferentPureBranches != nullptr &&
              nestedDifferentPureBranches != nullptr && effectfulBranch != nullptr &&
              effectfulElseBranch != nullptr && discardedLogicalAnd != nullptr &&
              discardedLogicalOr != nullptr && discardedWhileLogical != nullptr &&
              returnedWhileLogical != nullptr && booleanIdentity != nullptr &&
              booleanInverted != nullptr,
          "interflow removed effectful same-branch if root")) {
    return code;
  }

  const std::vector<std::string> sameBranchText =
      scalanative::nir::bodyToText(sameBranch->body);
  if (int code = expect(
          sameBranchText.size() == 2 &&
              contains(sameBranchText.back(), "call %demo.interflow.If.effectBool()") &&
              contains(sameBranchText.back(), "7") &&
              !contains(sameBranchText.back(), "if "),
          "interflow did not preserve effect while folding same-branch if")) {
    return code;
  }

  const std::vector<std::string> discardedText =
      scalanative::nir::bodyToText(discarded->body);
  if (int code =
          expect(discardedText.size() == 3 &&
                     discardedText[1] == "eval call %demo.interflow.If.effectBool()" &&
                     discardedText[2] == "ret Int 42",
                 "interflow did not trim discarded same-branch if result")) {
    return code;
  }

  const std::vector<std::string> nestedDiscardedText =
      scalanative::nir::bodyToText(nestedDiscarded->body);
  if (int code =
          expect(nestedDiscardedText.size() == 2 &&
                     contains(nestedDiscardedText.back(),
                              "call %demo.interflow.If.effectBool()") &&
                     contains(nestedDiscardedText.back(), "99") &&
                     !contains(nestedDiscardedText.back(), "if ") &&
                     !contains(nestedDiscardedText.back(), "8"),
                 "interflow did not trim nested discarded same-branch if result")) {
    return code;
  }

  const std::vector<std::string> discardedDifferentPureBranchesText =
      scalanative::nir::bodyToText(discardedDifferentPureBranches->body);
  if (int code = expect(discardedDifferentPureBranchesText.size() == 3 &&
                            discardedDifferentPureBranchesText[1] ==
                                "eval call %demo.interflow.If.effectBool()" &&
                            discardedDifferentPureBranchesText[2] == "ret Int 43",
                        "interflow did not trim discarded if with pure branches")) {
    return code;
  }

  const std::vector<std::string> nestedDifferentPureBranchesText =
      scalanative::nir::bodyToText(nestedDifferentPureBranches->body);
  if (int code =
          expect(nestedDifferentPureBranchesText.size() == 2 &&
                     contains(nestedDifferentPureBranchesText.back(),
                              "call %demo.interflow.If.effectBool()") &&
                     contains(nestedDifferentPureBranchesText.back(), "100") &&
                     !contains(nestedDifferentPureBranchesText.back(), "if("),
                 "interflow did not trim nested discarded if with pure branches")) {
    return code;
  }

  const std::vector<std::string> effectfulBranchText =
      scalanative::nir::bodyToText(effectfulBranch->body);
  if (int code = expect(effectfulBranchText.size() == 3 &&
                            contains(effectfulBranchText[1], "eval if(") &&
                            contains(effectfulBranchText[1],
                                     "block(call %demo.interflow.If.effectBool(); "
                                     "unit), unit") &&
                            !contains(effectfulBranchText[1], "false") &&
                            effectfulBranchText[2] == "ret Int 44",
                        "interflow did not trim discarded effectful then branch")) {
    return code;
  }

  const std::vector<std::string> effectfulElseBranchText =
      scalanative::nir::bodyToText(effectfulElseBranch->body);
  if (int code = expect(effectfulElseBranchText.size() == 3 &&
                            contains(effectfulElseBranchText[1], "eval if(") &&
                            contains(effectfulElseBranchText[1],
                                     "unit, block(call "
                                     "%demo.interflow.If.effectBool(); unit)") &&
                            !contains(effectfulElseBranchText[1], "true") &&
                            effectfulElseBranchText[2] == "ret Int 47",
                        "interflow did not trim discarded effectful else branch")) {
    return code;
  }

  const std::vector<std::string> discardedLogicalAndText =
      scalanative::nir::bodyToText(discardedLogicalAnd->body);
  if (int code = expect(
          discardedLogicalAndText.size() == 3 &&
              contains(discardedLogicalAndText[1],
                       "eval if(call %demo.interflow.If.effectBool()") &&
              contains(discardedLogicalAndText[1],
                       "block(call %demo.interflow.If.effectBool(); unit), unit") &&
              !contains(discardedLogicalAndText[1], "&&") &&
              discardedLogicalAndText[2] == "ret Int 45",
          "interflow did not preserve discarded && short-circuit effects")) {
    return code;
  }

  const std::vector<std::string> discardedLogicalOrText =
      scalanative::nir::bodyToText(discardedLogicalOr->body);
  if (int code = expect(
          discardedLogicalOrText.size() == 3 &&
              contains(discardedLogicalOrText[1],
                       "eval if(call %demo.interflow.If.effectBool()") &&
              contains(discardedLogicalOrText[1],
                       "unit, block(call %demo.interflow.If.effectBool(); unit)") &&
              !contains(discardedLogicalOrText[1], "||") &&
              discardedLogicalOrText[2] == "ret Int 46",
          "interflow did not preserve discarded || short-circuit effects")) {
    return code;
  }

  const std::vector<std::string> discardedWhileLogicalText =
      scalanative::nir::bodyToText(discardedWhileLogical->body);
  if (int code = expect(
          discardedWhileLogicalText.size() == 3 &&
              contains(discardedWhileLogicalText[1],
                       "eval while(call %demo.interflow.If.effectBool(), "
                       "if(call %demo.interflow.If.effectBool()") &&
              contains(discardedWhileLogicalText[1],
                       "block(call %demo.interflow.If.effectBool(); unit), unit") &&
              !contains(discardedWhileLogicalText[1], "&&") &&
              discardedWhileLogicalText[2] == "ret Int 48",
          "interflow did not trim discarded while body result")) {
    return code;
  }

  const std::vector<std::string> returnedWhileLogicalText =
      scalanative::nir::bodyToText(returnedWhileLogical->body);
  if (int code = expect(
          returnedWhileLogicalText.size() == 2 &&
              contains(returnedWhileLogicalText.back(),
                       "ret Unit while(call %demo.interflow.If.effectBool(), "
                       "if(call %demo.interflow.If.effectBool()") &&
              contains(returnedWhileLogicalText.back(),
                       "block(call %demo.interflow.If.effectBool(); unit), unit") &&
              !contains(returnedWhileLogicalText.back(), "&&"),
          "interflow did not trim an inherently discarded returned while body")) {
    return code;
  }

  const std::vector<std::string> booleanIdentityText =
      scalanative::nir::bodyToText(booleanIdentity->body);
  if (int code = expect(booleanIdentityText.size() == 2 &&
                            booleanIdentityText.back() ==
                                "ret Boolean call %demo.interflow.If.effectBool()",
                        "interflow did not fold effectful Boolean if identity")) {
    return code;
  }

  const std::vector<std::string> booleanInvertedText =
      scalanative::nir::bodyToText(booleanInverted->body);
  return expect(booleanInvertedText.size() == 2 &&
                    contains(booleanInvertedText.back(),
                             "(!call %demo.interflow.If.effectBool())") &&
                    !contains(booleanInvertedText.back(), "if("),
                "interflow did not fold effectful Boolean if negation");
}

int smokeInterflowExactDevirtualization() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  scalanative::nir::FunctionBodyBuilder baseValueBody;
  (void)baseValueBody.addParameter("this", "demo.interflow.Devirt.Base", noSpan);
  (void)baseValueBody.addReturn(
      "Int", scalanative::nir::literalValue("1", "Int", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder childValueBody;
  (void)childValueBody.addParameter("this", "demo.interflow.Devirt.Child", noSpan);
  (void)childValueBody.addReturn(
      "Int", scalanative::nir::literalValue("41", "Int", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder childAddBody;
  (void)childAddBody.addParameter("this", "demo.interflow.Devirt.Child", noSpan);
  (void)childAddBody.addParameter("value", "Int", noSpan);
  (void)childAddBody.addReturn(
      "Int",
      scalanative::nir::binaryValue("+", scalanative::nir::localValue("value", noSpan),
                                    scalanative::nir::literalValue("1", "Int", noSpan),
                                    noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder exactSelectBody;
  (void)exactSelectBody.addLet(
      "score", "demo.interflow.Devirt.Child",
      scalanative::nir::newValue("demo.interflow.Devirt.Child", noSpan), noSpan);
  (void)exactSelectBody.addReturn(
      "Int",
      scalanative::nir::binaryValue(
          "+",
          scalanative::nir::selectValue(scalanative::nir::localValue("score", noSpan),
                                        "value", noSpan),
          scalanative::nir::literalValue("1", "Int", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder exactCallBody;
  (void)exactCallBody.addLet(
      "score", "demo.interflow.Devirt.Child",
      scalanative::nir::newValue("demo.interflow.Devirt.Child", noSpan), noSpan);
  (void)exactCallBody.addReturn(
      "Int",
      scalanative::nir::callValue(
          scalanative::nir::selectValue(scalanative::nir::localValue("score", noSpan),
                                        "add", noSpan),
          {scalanative::nir::literalValue("41", "Int", noSpan)}, noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder dynamicParamBody;
  (void)dynamicParamBody.addParameter("score", "demo.interflow.Devirt.Base", noSpan);
  (void)dynamicParamBody.addReturn(
      "Int",
      scalanative::nir::selectValue(scalanative::nir::localValue("score", noSpan),
                                    "value", noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder leafParamBody;
  (void)leafParamBody.addParameter("score", "demo.interflow.Devirt.Child", noSpan);
  (void)leafParamBody.addReturn(
      "Int",
      scalanative::nir::binaryValue(
          "+",
          scalanative::nir::selectValue(scalanative::nir::localValue("score", noSpan),
                                        "value", noSpan),
          scalanative::nir::literalValue("1", "Int", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder leafCallWithDiscardableArgBody;
  (void)leafCallWithDiscardableArgBody.addReturn(
      "Int",
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.Devirt.leafParam", noSpan),
          {scalanative::nir::newValue("demo.interflow.Devirt.Child", noSpan)}, noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder identityBody;
  (void)identityBody.addParameter("score", "demo.interflow.Devirt.Child", noSpan);
  (void)identityBody.addReturn("demo.interflow.Devirt.Child",
                               scalanative::nir::localValue("score", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder identityTypeTestBody;
  (void)identityTypeTestBody.addReturn(
      "Boolean",
      scalanative::nir::isInstanceOfValue(
          "demo.interflow.Devirt.Child",
          scalanative::nir::callValue(
              scalanative::nir::localValue("demo.interflow.Devirt.identity", noSpan),
              {scalanative::nir::newValue("demo.interflow.Devirt.Child", noSpan)},
              noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder hierarchyUpcastBody;
  (void)hierarchyUpcastBody.addParameter("score", "demo.interflow.Devirt.Child",
                                         noSpan);
  (void)hierarchyUpcastBody.addReturn(
      "demo.interflow.Devirt.Base",
      scalanative::nir::asInstanceOfValue("demo.interflow.Devirt.Base",
                                          scalanative::nir::localValue("score", noSpan),
                                          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder hierarchyTypeTestBody;
  (void)hierarchyTypeTestBody.addLet(
      "score", "demo.interflow.Devirt.Child",
      scalanative::nir::newValue("demo.interflow.Devirt.Child", noSpan), noSpan);
  (void)hierarchyTypeTestBody.addReturn(
      "Boolean",
      scalanative::nir::isInstanceOfValue("demo.interflow.Devirt.Base",
                                          scalanative::nir::localValue("score", noSpan),
                                          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder hierarchyTypeMismatchBody;
  (void)hierarchyTypeMismatchBody.addLet(
      "score", "demo.interflow.Devirt.Child",
      scalanative::nir::newValue("demo.interflow.Devirt.Child", noSpan), noSpan);
  (void)hierarchyTypeMismatchBody.addReturn(
      "Boolean",
      scalanative::nir::isInstanceOfValue("demo.interflow.Devirt.Other",
                                          scalanative::nir::localValue("score", noSpan),
                                          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directHierarchyTypeTestBody;
  (void)directHierarchyTypeTestBody.addReturn(
      "Boolean",
      scalanative::nir::isInstanceOfValue(
          "demo.interflow.Devirt.Base",
          scalanative::nir::newValue("demo.interflow.Devirt.Child", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directHierarchyTypeMismatchBody;
  (void)directHierarchyTypeMismatchBody.addReturn(
      "Boolean",
      scalanative::nir::isInstanceOfValue(
          "demo.interflow.Devirt.Other",
          scalanative::nir::newValue("demo.interflow.Devirt.Child", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directEffectfulHierarchyTypeTestBody;
  (void)directEffectfulHierarchyTypeTestBody.addReturn(
      "Boolean",
      scalanative::nir::isInstanceOfValue(
          "demo.interflow.Devirt.Base",
          scalanative::nir::newValue("demo.interflow.Devirt.EffectChild", noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directNestedEffectfulTypeMismatchBody;
  (void)directNestedEffectfulTypeMismatchBody.addReturn(
      "Boolean",
      scalanative::nir::isInstanceOfValue(
          "demo.interflow.Devirt.Base",
          scalanative::nir::newValue(
              "demo.interflow.Devirt.Holder",
              {scalanative::nir::newValue("demo.interflow.Devirt.EffectChild", noSpan)},
              noSpan),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder nestedDeadAllocationBody;
  (void)nestedDeadAllocationBody.addReturn(
      "Int",
      scalanative::nir::blockValue(
          {scalanative::nir::localLetValue(
               "score", "demo.interflow.Devirt.Child",
               scalanative::nir::newValue("demo.interflow.Devirt.Child", noSpan),
               noSpan),
           scalanative::nir::literalValue("42", "Int", noSpan)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder nestedDeadObjectGraphBody;
  (void)nestedDeadObjectGraphBody.addLet(
      "holder", "demo.interflow.Devirt.Holder",
      scalanative::nir::newValue(
          "demo.interflow.Devirt.Holder",
          {scalanative::nir::newValue("demo.interflow.Devirt.Child", noSpan)}, noSpan),
      noSpan);
  (void)nestedDeadObjectGraphBody.addReturn(
      "Int", scalanative::nir::literalValue("43", "Int", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder nestedEffectfulObjectGraphBody;
  (void)nestedEffectfulObjectGraphBody.addLet(
      "holder", "demo.interflow.Devirt.Holder",
      scalanative::nir::newValue(
          "demo.interflow.Devirt.Holder",
          {scalanative::nir::newValue(
              "demo.interflow.Devirt.Holder",
              {scalanative::nir::newValue(
                  "demo.interflow.Devirt.Holder",
                  {scalanative::nir::newValue("demo.interflow.Devirt.EffectChild",
                                              noSpan)},
                  noSpan)},
              noSpan)},
          noSpan),
      noSpan);
  (void)nestedEffectfulObjectGraphBody.addReturn(
      "Int", scalanative::nir::literalValue("44", "Int", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder conditionalDeadAllocationBody;
  (void)conditionalDeadAllocationBody.addParameter("choose", "Boolean", noSpan);
  (void)conditionalDeadAllocationBody.addLet(
      "unused", "demo.interflow.Devirt.Holder",
      scalanative::nir::ifValue(
          scalanative::nir::localValue("choose", noSpan),
          scalanative::nir::newValue(
              "demo.interflow.Devirt.Holder",
              {scalanative::nir::newValue("demo.interflow.Devirt.Child", noSpan)},
              noSpan),
          scalanative::nir::newValue(
              "demo.interflow.Devirt.Holder",
              {scalanative::nir::newValue("demo.interflow.Devirt.Other", noSpan)},
              noSpan),
          noSpan),
      noSpan);
  (void)conditionalDeadAllocationBody.addReturn(
      "Int", scalanative::nir::literalValue("45", "Int", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder conditionalEffectfulAllocationBody;
  (void)conditionalEffectfulAllocationBody.addParameter("choose", "Boolean", noSpan);
  (void)conditionalEffectfulAllocationBody.addLet(
      "unused", "demo.interflow.Devirt.EffectChild",
      scalanative::nir::ifValue(
          scalanative::nir::localValue("choose", noSpan),
          scalanative::nir::newValue("demo.interflow.Devirt.EffectChild", noSpan),
          scalanative::nir::newValue("demo.interflow.Devirt.EffectChild", noSpan),
          noSpan),
      noSpan);
  (void)conditionalEffectfulAllocationBody.addReturn(
      "Int", scalanative::nir::literalValue("46", "Int", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder conditionalEffectConditionBody;
  (void)conditionalEffectConditionBody.addLet(
      "unused", "demo.interflow.Devirt.Holder",
      scalanative::nir::ifValue(
          scalanative::nir::callValue(
              scalanative::nir::localValue("demo.interflow.Devirt.effectBool", noSpan),
              {}, noSpan),
          scalanative::nir::newValue(
              "demo.interflow.Devirt.Holder",
              {scalanative::nir::newValue("demo.interflow.Devirt.Child", noSpan)},
              noSpan),
          scalanative::nir::newValue(
              "demo.interflow.Devirt.Holder",
              {scalanative::nir::newValue("demo.interflow.Devirt.Other", noSpan)},
              noSpan),
          noSpan),
      noSpan);
  (void)conditionalEffectConditionBody.addReturn(
      "Int", scalanative::nir::literalValue("47", "Int", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder conditionalMixedAllocationBody;
  (void)conditionalMixedAllocationBody.addParameter("choose", "Boolean", noSpan);
  (void)conditionalMixedAllocationBody.addLet(
      "unused", "demo.interflow.Devirt.Holder",
      scalanative::nir::ifValue(
          scalanative::nir::localValue("choose", noSpan),
          scalanative::nir::newValue(
              "demo.interflow.Devirt.Holder",
              {scalanative::nir::newValue("demo.interflow.Devirt.Child", noSpan)},
              noSpan),
          scalanative::nir::newValue(
              "demo.interflow.Devirt.Holder",
              {scalanative::nir::newValue("demo.interflow.Devirt.EffectChild", noSpan)},
              noSpan),
          noSpan),
      noSpan);
  (void)conditionalMixedAllocationBody.addReturn(
      "Int", scalanative::nir::literalValue("48", "Int", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder effectChildInitializerBody;
  (void)effectChildInitializerBody.addParameter(
      "this", "demo.interflow.Devirt.EffectChild", noSpan);
  (void)effectChildInitializerBody.addEval(
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.Devirt.effect", noSpan), {},
          noSpan),
      noSpan);
  (void)effectChildInitializerBody.addReturn(
      "Unit", scalanative::nir::unitValue(noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                "demo.interflow.Devirt.Base",
                                "@java.lang.Object",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                "demo.interflow.Devirt.Child",
                                "@demo.interflow.Devirt.Base",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                "demo.interflow.Devirt.Other",
                                "@java.lang.Object",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                "demo.interflow.Devirt.Holder",
                                "@java.lang.Object",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                "demo.interflow.Devirt.EffectChild",
                                "@demo.interflow.Devirt.Base",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::Field,
                                "demo.interflow.Devirt.Holder.child",
                                "Object",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Devirt.EffectChild.$init",
                                "(demo.interflow.Devirt.EffectChild)Unit",
                                std::move(effectChildInitializerBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Devirt.Base.value",
                                "(demo.interflow.Devirt.Base)Int",
                                std::move(baseValueBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Devirt.Child.value",
                                "(demo.interflow.Devirt.Child)Int",
                                std::move(childValueBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Devirt.Child.add",
                                "(demo.interflow.Devirt.Child,Int)Int",
                                std::move(childAddBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Devirt.exactSelect", "()Int",
                                std::move(exactSelectBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Devirt.exactCall", "()Int",
                                std::move(exactCallBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Devirt.dynamicParam",
                                "(demo.interflow.Devirt.Base)Int",
                                std::move(dynamicParamBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef, "demo.interflow.Devirt.leafParam",
       "(demo.interflow.Devirt.Child)Int", std::move(leafParamBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Devirt.leafCallWithDiscardableArg", "()Int",
       std::move(leafCallWithDiscardableArgBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef, "demo.interflow.Devirt.identity",
       "(demo.interflow.Devirt.Child)demo.interflow.Devirt.Child",
       std::move(identityBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Devirt.identityTypeTest", "()Boolean",
                                std::move(identityTypeTestBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Devirt.hierarchyUpcast",
       "(demo.interflow.Devirt.Child)demo.interflow.Devirt.Base",
       std::move(hierarchyUpcastBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Devirt.hierarchyTypeTest", "()Boolean",
                                std::move(hierarchyTypeTestBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Devirt.hierarchyTypeMismatch",
                                "()Boolean",
                                std::move(hierarchyTypeMismatchBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Devirt.directHierarchyTypeTest", "()Boolean",
       std::move(directHierarchyTypeTestBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Devirt.directHierarchyTypeMismatch", "()Boolean",
       std::move(directHierarchyTypeMismatchBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Devirt.directEffectfulHierarchyTypeTest", "()Boolean",
       std::move(directEffectfulHierarchyTypeTestBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Devirt.directNestedEffectfulTypeMismatch", "()Boolean",
       std::move(directNestedEffectfulTypeMismatchBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Devirt.nestedDeadAllocation", "()Int",
                                std::move(nestedDeadAllocationBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Devirt.nestedDeadObjectGraph", "()Int",
                                std::move(nestedDeadObjectGraphBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Devirt.nestedEffectfulObjectGraph", "()Int",
       std::move(nestedEffectfulObjectGraphBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Devirt.conditionalDeadAllocation", "(Boolean)Int",
       std::move(conditionalDeadAllocationBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Devirt.conditionalEffectfulAllocation", "(Boolean)Int",
       std::move(conditionalEffectfulAllocationBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Devirt.conditionalEffectCondition", "()Int",
       std::move(conditionalEffectConditionBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Devirt.conditionalMixedAllocation", "(Boolean)Int",
       std::move(conditionalMixedAllocationBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                "demo.interflow.Devirt.effect",
                                "()Unit",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                "demo.interflow.Devirt.effectBool",
                                "()Boolean",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.roots.push_back("demo.interflow.Devirt.exactSelect");
  program.roots.push_back("demo.interflow.Devirt.exactCall");
  program.roots.push_back("demo.interflow.Devirt.dynamicParam");
  program.roots.push_back("demo.interflow.Devirt.leafParam");
  program.roots.push_back("demo.interflow.Devirt.leafCallWithDiscardableArg");
  program.roots.push_back("demo.interflow.Devirt.identityTypeTest");
  program.roots.push_back("demo.interflow.Devirt.hierarchyUpcast");
  program.roots.push_back("demo.interflow.Devirt.hierarchyTypeTest");
  program.roots.push_back("demo.interflow.Devirt.hierarchyTypeMismatch");
  program.roots.push_back("demo.interflow.Devirt.directHierarchyTypeTest");
  program.roots.push_back("demo.interflow.Devirt.directHierarchyTypeMismatch");
  program.roots.push_back("demo.interflow.Devirt.directEffectfulHierarchyTypeTest");
  program.roots.push_back("demo.interflow.Devirt.directNestedEffectfulTypeMismatch");
  program.roots.push_back("demo.interflow.Devirt.nestedDeadAllocation");
  program.roots.push_back("demo.interflow.Devirt.nestedDeadObjectGraph");
  program.roots.push_back("demo.interflow.Devirt.nestedEffectfulObjectGraph");
  program.roots.push_back("demo.interflow.Devirt.conditionalDeadAllocation");
  program.roots.push_back("demo.interflow.Devirt.conditionalEffectfulAllocation");
  program.roots.push_back("demo.interflow.Devirt.conditionalEffectCondition");
  program.roots.push_back("demo.interflow.Devirt.conditionalMixedAllocation");
  program.reachableGlobals.push_back("demo.interflow.Devirt.Base");
  program.reachableGlobals.push_back("demo.interflow.Devirt.Child");
  program.reachableGlobals.push_back("demo.interflow.Devirt.Other");
  program.reachableGlobals.push_back("demo.interflow.Devirt.Holder");
  program.reachableGlobals.push_back("demo.interflow.Devirt.EffectChild");
  program.reachableGlobals.push_back("demo.interflow.Devirt.Holder.child");
  program.reachableGlobals.push_back("demo.interflow.Devirt.EffectChild.$init");
  program.reachableGlobals.push_back("demo.interflow.Devirt.Base.value");
  program.reachableGlobals.push_back("demo.interflow.Devirt.Child.value");
  program.reachableGlobals.push_back("demo.interflow.Devirt.Child.add");
  program.reachableGlobals.push_back("demo.interflow.Devirt.exactSelect");
  program.reachableGlobals.push_back("demo.interflow.Devirt.exactCall");
  program.reachableGlobals.push_back("demo.interflow.Devirt.dynamicParam");
  program.reachableGlobals.push_back("demo.interflow.Devirt.leafParam");
  program.reachableGlobals.push_back(
      "demo.interflow.Devirt.leafCallWithDiscardableArg");
  program.reachableGlobals.push_back("demo.interflow.Devirt.identity");
  program.reachableGlobals.push_back("demo.interflow.Devirt.identityTypeTest");
  program.reachableGlobals.push_back("demo.interflow.Devirt.hierarchyUpcast");
  program.reachableGlobals.push_back("demo.interflow.Devirt.hierarchyTypeTest");
  program.reachableGlobals.push_back("demo.interflow.Devirt.hierarchyTypeMismatch");
  program.reachableGlobals.push_back("demo.interflow.Devirt.directHierarchyTypeTest");
  program.reachableGlobals.push_back(
      "demo.interflow.Devirt.directHierarchyTypeMismatch");
  program.reachableGlobals.push_back(
      "demo.interflow.Devirt.directEffectfulHierarchyTypeTest");
  program.reachableGlobals.push_back(
      "demo.interflow.Devirt.directNestedEffectfulTypeMismatch");
  program.reachableGlobals.push_back("demo.interflow.Devirt.nestedDeadAllocation");
  program.reachableGlobals.push_back("demo.interflow.Devirt.nestedDeadObjectGraph");
  program.reachableGlobals.push_back(
      "demo.interflow.Devirt.nestedEffectfulObjectGraph");
  program.reachableGlobals.push_back("demo.interflow.Devirt.conditionalDeadAllocation");
  program.reachableGlobals.push_back(
      "demo.interflow.Devirt.conditionalEffectfulAllocation");
  program.reachableGlobals.push_back(
      "demo.interflow.Devirt.conditionalEffectCondition");
  program.reachableGlobals.push_back(
      "demo.interflow.Devirt.conditionalMixedAllocation");
  program.reachableGlobals.push_back("demo.interflow.Devirt.effect");
  program.reachableGlobals.push_back("demo.interflow.Devirt.effectBool");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code =
          expect(result.ok, "interflow rejected exact devirtualization program")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* exactSelect =
      findDefinition(optimizedModule, "demo.interflow.Devirt.exactSelect");
  const scalanative::nir::Definition* exactCall =
      findDefinition(optimizedModule, "demo.interflow.Devirt.exactCall");
  const scalanative::nir::Definition* dynamicParam =
      findDefinition(optimizedModule, "demo.interflow.Devirt.dynamicParam");
  const scalanative::nir::Definition* leafParam =
      findDefinition(optimizedModule, "demo.interflow.Devirt.leafParam");
  const scalanative::nir::Definition* leafCallWithDiscardableArg = findDefinition(
      optimizedModule, "demo.interflow.Devirt.leafCallWithDiscardableArg");
  const scalanative::nir::Definition* identity =
      findDefinition(optimizedModule, "demo.interflow.Devirt.identity");
  const scalanative::nir::Definition* identityTypeTest =
      findDefinition(optimizedModule, "demo.interflow.Devirt.identityTypeTest");
  const scalanative::nir::Definition* hierarchyUpcast =
      findDefinition(optimizedModule, "demo.interflow.Devirt.hierarchyUpcast");
  const scalanative::nir::Definition* hierarchyTypeTest =
      findDefinition(optimizedModule, "demo.interflow.Devirt.hierarchyTypeTest");
  const scalanative::nir::Definition* hierarchyTypeMismatch =
      findDefinition(optimizedModule, "demo.interflow.Devirt.hierarchyTypeMismatch");
  const scalanative::nir::Definition* directHierarchyTypeTest =
      findDefinition(optimizedModule, "demo.interflow.Devirt.directHierarchyTypeTest");
  const scalanative::nir::Definition* directHierarchyTypeMismatch = findDefinition(
      optimizedModule, "demo.interflow.Devirt.directHierarchyTypeMismatch");
  const scalanative::nir::Definition* directEffectfulHierarchyTypeTest = findDefinition(
      optimizedModule, "demo.interflow.Devirt.directEffectfulHierarchyTypeTest");
  const scalanative::nir::Definition* directNestedEffectfulTypeMismatch =
      findDefinition(optimizedModule,
                     "demo.interflow.Devirt.directNestedEffectfulTypeMismatch");
  const scalanative::nir::Definition* nestedDeadAllocation =
      findDefinition(optimizedModule, "demo.interflow.Devirt.nestedDeadAllocation");
  const scalanative::nir::Definition* nestedDeadObjectGraph =
      findDefinition(optimizedModule, "demo.interflow.Devirt.nestedDeadObjectGraph");
  const scalanative::nir::Definition* nestedEffectfulObjectGraph = findDefinition(
      optimizedModule, "demo.interflow.Devirt.nestedEffectfulObjectGraph");
  const scalanative::nir::Definition* conditionalDeadAllocation = findDefinition(
      optimizedModule, "demo.interflow.Devirt.conditionalDeadAllocation");
  const scalanative::nir::Definition* conditionalEffectfulAllocation = findDefinition(
      optimizedModule, "demo.interflow.Devirt.conditionalEffectfulAllocation");
  const scalanative::nir::Definition* conditionalEffectCondition = findDefinition(
      optimizedModule, "demo.interflow.Devirt.conditionalEffectCondition");
  const scalanative::nir::Definition* conditionalMixedAllocation = findDefinition(
      optimizedModule, "demo.interflow.Devirt.conditionalMixedAllocation");
  if (int code = expect(
          exactSelect != nullptr && exactCall != nullptr && dynamicParam != nullptr &&
              leafParam != nullptr && leafCallWithDiscardableArg != nullptr &&
              identityTypeTest != nullptr && hierarchyUpcast != nullptr &&
              hierarchyTypeTest != nullptr && hierarchyTypeMismatch != nullptr &&
              directHierarchyTypeTest != nullptr &&
              directHierarchyTypeMismatch != nullptr &&
              directEffectfulHierarchyTypeTest != nullptr &&
              directNestedEffectfulTypeMismatch != nullptr &&
              nestedDeadAllocation != nullptr && nestedDeadObjectGraph != nullptr &&
              nestedEffectfulObjectGraph != nullptr &&
              conditionalDeadAllocation != nullptr &&
              conditionalEffectfulAllocation != nullptr &&
              conditionalEffectCondition != nullptr &&
              conditionalMixedAllocation != nullptr,
          "interflow removed devirtualization smoke roots")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(exactSelect->body).back() == "ret Int 42",
                 "interflow did not devirtualize exact no-arg select")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(exactSelect->body).size() == 2,
                        "interflow did not remove dead exact receiver allocation")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(exactCall->body).back() == "ret Int 42",
                 "interflow did not devirtualize exact selected call")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(exactCall->body).size() == 2,
                        "interflow did not remove dead exact call allocation")) {
    return code;
  }
  const std::string leafParamReturn =
      scalanative::nir::bodyToText(leafParam->body).back();
  if (int code =
          expect(contains(leafParamReturn,
                          "call %demo.interflow.Devirt.Child.value(%score)") &&
                     contains(leafParamReturn, " + 1"),
                 "interflow did not retain the nullable closed-leaf invocation")) {
    return code;
  }
  const std::vector<std::string> leafCallWithDiscardableArgText =
      scalanative::nir::bodyToText(leafCallWithDiscardableArg->body);
  if (int code = expect(contains(leafCallWithDiscardableArgText.back(),
                                 "call %demo.interflow.Devirt.leafParam(") &&
                            contains(leafCallWithDiscardableArgText.back(),
                                     "new demo.interflow.Devirt.Child"),
                        "interflow erased a nullable wrapper invocation")) {
    return code;
  }
  if (int code = expect(leafCallWithDiscardableArgText.size() == 2,
                        "interflow expanded a retained nullable wrapper call")) {
    return code;
  }
  if (int code = expect(identity == nullptr,
                        "interflow did not prune now-unused identity helper")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(identityTypeTest->body).back() ==
                            "ret Boolean true",
                        "interflow did not fold identity new type test")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(identityTypeTest->body).size() == 2,
                 "interflow did not erase identity type-test allocation")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(hierarchyUpcast->body).back() ==
                            "ret demo.interflow.Devirt.Base %score",
                        "interflow did not erase hierarchy-safe upcast")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(hierarchyUpcast->body).size() == 3,
                        "interflow left extra work around hierarchy-safe upcast")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(hierarchyTypeTest->body).back() ==
                            "ret Boolean true",
                        "interflow did not fold hierarchy-safe type test")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(hierarchyTypeTest->body).size() == 2,
                 "interflow did not remove hierarchy type-test allocation")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(hierarchyTypeMismatch->body).back() ==
                     "ret Boolean false",
                 "interflow did not fold hierarchy-mismatch type test")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(hierarchyTypeMismatch->body).size() == 2,
                 "interflow did not remove hierarchy mismatch allocation")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(directHierarchyTypeTest->body).back() ==
                     "ret Boolean true",
                 "interflow did not fold direct hierarchy type test")) {
    return code;
  }
  if (int code = expect(
          scalanative::nir::bodyToText(directHierarchyTypeTest->body).size() == 2,
          "interflow did not remove direct hierarchy allocation")) {
    return code;
  }
  if (int code = expect(
          scalanative::nir::bodyToText(directHierarchyTypeMismatch->body).back() ==
              "ret Boolean false",
          "interflow did not fold direct hierarchy-mismatch type test")) {
    return code;
  }
  if (int code = expect(
          scalanative::nir::bodyToText(directHierarchyTypeMismatch->body).size() == 2,
          "interflow did not remove direct hierarchy mismatch allocation")) {
    return code;
  }
  const std::vector<std::string> directEffectfulHierarchyTypeTestText =
      scalanative::nir::bodyToText(directEffectfulHierarchyTypeTest->body);
  if (int code = expect(
          directEffectfulHierarchyTypeTestText.size() == 2 &&
              contains(directEffectfulHierarchyTypeTestText[1],
                       "new demo.interflow.Devirt.EffectChild") &&
              contains(directEffectfulHierarchyTypeTestText[1], "; true)") &&
              directEffectfulHierarchyTypeTestText[1].rfind("ret Boolean ", 0) == 0,
          "interflow did not fold a type test while retaining an effectful init")) {
    return code;
  }
  const std::vector<std::string> directNestedEffectfulTypeMismatchText =
      scalanative::nir::bodyToText(directNestedEffectfulTypeMismatch->body);
  if (int code = expect(
          directNestedEffectfulTypeMismatchText.size() == 2 &&
              contains(directNestedEffectfulTypeMismatchText[1],
                       "new demo.interflow.Devirt.EffectChild") &&
              !contains(directNestedEffectfulTypeMismatchText[1],
                        "new demo.interflow.Devirt.Holder") &&
              contains(directNestedEffectfulTypeMismatchText[1], "; false)") &&
              directNestedEffectfulTypeMismatchText[1].rfind("ret Boolean ", 0) == 0,
          "interflow did not fold a type mismatch while trimming its outer shell")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(nestedDeadAllocation->body).back() ==
                     "ret Int 42",
                 "interflow did not remove nested dead exact allocation")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(nestedDeadAllocation->body).size() == 2,
                 "interflow did not collapse nested allocation block")) {
    return code;
  }
  if (int code = expect(
          scalanative::nir::bodyToText(nestedDeadObjectGraph->body).size() == 2 &&
              scalanative::nir::bodyToText(nestedDeadObjectGraph->body).back() ==
                  "ret Int 43",
          "interflow did not remove a recursively discardable private object graph")) {
    return code;
  }
  const std::vector<std::string> nestedEffectfulObjectGraphText =
      scalanative::nir::bodyToText(nestedEffectfulObjectGraph->body);
  if (int code =
          expect(nestedEffectfulObjectGraphText.size() == 3 &&
                     contains(nestedEffectfulObjectGraphText[1],
                              "new demo.interflow.Devirt.EffectChild") &&
                     !contains(nestedEffectfulObjectGraphText[1],
                               "new demo.interflow.Devirt.Holder") &&
                     nestedEffectfulObjectGraphText[2] == "ret Int 44",
                 "interflow did not recursively remove direct-storage object shells")) {
    return code;
  }
  const std::vector<std::string> conditionalDeadAllocationText =
      scalanative::nir::bodyToText(conditionalDeadAllocation->body);
  if (int code =
          expect(conditionalDeadAllocationText.size() == 3 &&
                     conditionalDeadAllocationText[1] == "param %choose : Boolean" &&
                     conditionalDeadAllocationText[2] == "ret Int 45",
                 "interflow did not remove a conditional of discardable "
                 "allocations")) {
    return code;
  }
  const std::vector<std::string> conditionalEffectfulAllocationText =
      scalanative::nir::bodyToText(conditionalEffectfulAllocation->body);
  if (int code = expect(
          conditionalEffectfulAllocationText.size() == 4 &&
              conditionalEffectfulAllocationText[1] == "param %choose : Boolean" &&
              contains(conditionalEffectfulAllocationText[2],
                       "new demo.interflow.Devirt.EffectChild") &&
              conditionalEffectfulAllocationText[3] == "ret Int 46",
          "interflow removed a conditional with effectful allocation branches")) {
    return code;
  }
  const std::vector<std::string> conditionalEffectConditionText =
      scalanative::nir::bodyToText(conditionalEffectCondition->body);
  if (int code =
          expect(conditionalEffectConditionText.size() == 3 &&
                     conditionalEffectConditionText[1] ==
                         "eval call %demo.interflow.Devirt.effectBool()" &&
                     conditionalEffectConditionText[2] == "ret Int 47",
                 "interflow did not retain only the effectful condition of an unused "
                 "allocation conditional")) {
    return code;
  }
  const std::vector<std::string> conditionalMixedAllocationText =
      scalanative::nir::bodyToText(conditionalMixedAllocation->body);
  if (int code = expect(
          conditionalMixedAllocationText.size() == 4 &&
              conditionalMixedAllocationText[1] == "param %choose : Boolean" &&
              contains(conditionalMixedAllocationText[2], "eval if(%choose, unit") &&
              !contains(conditionalMixedAllocationText[2],
                        "new demo.interflow.Devirt.Child") &&
              contains(conditionalMixedAllocationText[2],
                       "new demo.interflow.Devirt.EffectChild") &&
              conditionalMixedAllocationText[3] == "ret Int 48",
          "interflow did not remove only the discardable allocation branch")) {
    return code;
  }
  return expect(scalanative::nir::bodyToText(dynamicParam->body).back() ==
                    "ret Int %score.value",
                "interflow devirtualized a non-exact parameter receiver");
}

int smokeInterflowExactConstructorFieldReads() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  const auto intValue = [&](int value) {
    return scalanative::nir::literalValue(std::to_string(value), "Int", noSpan);
  };
  const auto fieldBoxValues = [&](scalanative::nir::Value value,
                                  scalanative::nir::Value current) {
    std::vector<scalanative::nir::Value> fields;
    fields.push_back(std::move(value));
    fields.push_back(std::move(current));
    return scalanative::nir::newValue("demo.interflow.Fields.Box", std::move(fields),
                                      noSpan);
  };
  const auto initializedFieldBox = [&] {
    return scalanative::nir::newValue("demo.interflow.Fields.InitializedBox",
                                      {intValue(7), intValue(8)}, noSpan);
  };
  const auto fieldBox = [&](int value, int current) {
    return fieldBoxValues(intValue(value), intValue(current));
  };

  scalanative::nir::FunctionBodyBuilder initialBody;
  (void)initialBody.addLet("box", "demo.interflow.Fields.Box", fieldBox(7, 8), noSpan);
  (void)initialBody.addReturn(
      "Int",
      scalanative::nir::selectValue(scalanative::nir::localValue("box", noSpan),
                                    "value", noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder updatedBody;
  (void)updatedBody.addLet("box", "demo.interflow.Fields.Box", fieldBox(7, 8), noSpan);
  (void)updatedBody.addEval(
      scalanative::nir::assignValue(
          scalanative::nir::selectValue(scalanative::nir::localValue("box", noSpan),
                                        "current", noSpan),
          scalanative::nir::literalValue("9", "Int", noSpan), noSpan),
      noSpan);
  (void)updatedBody.addReturn(
      "Int",
      scalanative::nir::selectValue(scalanative::nir::localValue("box", noSpan),
                                    "current", noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder effectfulUpdateBody;
  (void)effectfulUpdateBody.addLet("box", "demo.interflow.Fields.Box", fieldBox(7, 8),
                                   noSpan);
  (void)effectfulUpdateBody.addEval(
      scalanative::nir::assignValue(
          scalanative::nir::selectValue(scalanative::nir::localValue("box", noSpan),
                                        "current", noSpan),
          scalanative::nir::callValue(
              scalanative::nir::localValue("demo.interflow.Fields.effectInt", noSpan),
              {}, noSpan),
          noSpan),
      noSpan);
  (void)effectfulUpdateBody.addReturn("Int", intValue(42), noSpan);

  scalanative::nir::FunctionBodyBuilder directPureBody;
  (void)directPureBody.addReturn(
      "Int", scalanative::nir::selectValue(fieldBox(7, 8), "value", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder directEffectfulArgumentBody;
  (void)directEffectfulArgumentBody.addReturn(
      "Int",
      scalanative::nir::selectValue(
          fieldBoxValues(scalanative::nir::callValue(
                             scalanative::nir::localValue(
                                 "demo.interflow.Fields.effectInt", noSpan),
                             {}, noSpan),
                         scalanative::nir::callValue(
                             scalanative::nir::localValue(
                                 "demo.interflow.Fields.effectInt", noSpan),
                             {}, noSpan)),
          "value", noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directEffectfulInitializerBody;
  (void)directEffectfulInitializerBody.addReturn(
      "Int", scalanative::nir::selectValue(initializedFieldBox(), "value", noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder unusedPureInitializerArgumentBody;
  (void)unusedPureInitializerArgumentBody.addLet(
      "box", "demo.interflow.Fields.PureInitializedBox",
      scalanative::nir::newValue(
          "demo.interflow.Fields.PureInitializedBox",
          {scalanative::nir::callValue(
               scalanative::nir::localValue("demo.interflow.Fields.effectInt", noSpan),
               {}, noSpan),
           intValue(0)},
          noSpan),
      noSpan);
  (void)unusedPureInitializerArgumentBody.addReturn("Int", intValue(43), noSpan);

  scalanative::nir::FunctionBodyBuilder afterUnrelatedCallBody;
  (void)afterUnrelatedCallBody.addLet("box", "demo.interflow.Fields.Box",
                                      fieldBox(7, 8), noSpan);
  (void)afterUnrelatedCallBody.addEval(
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.Fields.effect", noSpan), {},
          noSpan),
      noSpan);
  (void)afterUnrelatedCallBody.addReturn(
      "Int",
      scalanative::nir::selectValue(scalanative::nir::localValue("box", noSpan),
                                    "value", noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder afterPassedCallBody;
  (void)afterPassedCallBody.addLet("box", "demo.interflow.Fields.Box", fieldBox(7, 8),
                                   noSpan);
  (void)afterPassedCallBody.addEval(
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.Fields.effectBox", noSpan),
          {scalanative::nir::localValue("box", noSpan)}, noSpan),
      noSpan);
  (void)afterPassedCallBody.addReturn(
      "Int",
      scalanative::nir::selectValue(scalanative::nir::localValue("box", noSpan),
                                    "value", noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder shadowedUpdateBody;
  (void)shadowedUpdateBody.addLet("box", "demo.interflow.Fields.Box", fieldBox(7, 8),
                                  noSpan);
  (void)shadowedUpdateBody.addEval(
      scalanative::nir::assignValue(
          scalanative::nir::selectValue(scalanative::nir::localValue("box", noSpan),
                                        "current", noSpan),
          intValue(9), noSpan),
      noSpan);
  (void)shadowedUpdateBody.addEval(
      scalanative::nir::blockValue(
          {scalanative::nir::localLetValue("box", "demo.interflow.Fields.Box",
                                           fieldBox(1, 2), noSpan),
           scalanative::nir::callValue(
               scalanative::nir::localValue("demo.interflow.Fields.effectBox", noSpan),
               {scalanative::nir::localValue("box", noSpan)}, noSpan)},
          noSpan),
      noSpan);
  (void)shadowedUpdateBody.addReturn("Int", intValue(42), noSpan);

  scalanative::nir::FunctionBodyBuilder boxInitializerBody;
  (void)boxInitializerBody.addParameter("this", "demo.interflow.Fields.Box", noSpan);
  (void)boxInitializerBody.addReturn("Unit", scalanative::nir::unitValue(noSpan),
                                     noSpan);

  scalanative::nir::FunctionBodyBuilder initializedBoxInitializerBody;
  (void)initializedBoxInitializerBody.addParameter(
      "this", "demo.interflow.Fields.InitializedBox", noSpan);
  (void)initializedBoxInitializerBody.addEval(
      scalanative::nir::callValue(
          scalanative::nir::localValue("demo.interflow.Fields.effect", noSpan), {},
          noSpan),
      noSpan);
  (void)initializedBoxInitializerBody.addReturn(
      "Unit", scalanative::nir::unitValue(noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder pureInitializedBoxInitializerBody;
  (void)pureInitializedBoxInitializerBody.addParameter(
      "this", "demo.interflow.Fields.PureInitializedBox", noSpan);
  (void)pureInitializedBoxInitializerBody.addEval(
      scalanative::nir::assignValue(
          scalanative::nir::selectValue(scalanative::nir::localValue("this", noSpan),
                                        "current", noSpan),
          scalanative::nir::binaryValue(
              "+",
              scalanative::nir::selectValue(
                  scalanative::nir::localValue("this", noSpan), "value", noSpan),
              intValue(1), noSpan),
          noSpan),
      noSpan);
  (void)pureInitializedBoxInitializerBody.addReturn(
      "Unit", scalanative::nir::unitValue(noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                "demo.interflow.Fields.Box",
                                "@java.lang.Object",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                "demo.interflow.Fields.InitializedBox",
                                "@java.lang.Object",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                "demo.interflow.Fields.PureInitializedBox",
                                "@java.lang.Object",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::Field,
                                "demo.interflow.Fields.Box.value",
                                "Int",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::Field,
                                "demo.interflow.Fields.Box.current",
                                "Int",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::Field,
                                "demo.interflow.Fields.InitializedBox.value",
                                "Int",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::Field,
                                "demo.interflow.Fields.InitializedBox.current",
                                "Int",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::Field,
                                "demo.interflow.Fields.PureInitializedBox.value",
                                "Int",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::Field,
                                "demo.interflow.Fields.PureInitializedBox.current",
                                "Int",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Fields.Box.$init",
                                "(demo.interflow.Fields.Box)Unit",
                                std::move(boxInitializerBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Fields.InitializedBox.$init",
                                "(demo.interflow.Fields.InitializedBox)Unit",
                                std::move(initializedBoxInitializerBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Fields.PureInitializedBox.$init",
                                "(demo.interflow.Fields.PureInitializedBox)Unit",
                                std::move(pureInitializedBoxInitializerBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Fields.initial", "()Int",
                                std::move(initialBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Fields.updated", "()Int",
                                std::move(updatedBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Fields.effectfulUpdate", "()Int",
                                std::move(effectfulUpdateBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Fields.directPure", "()Int",
                                std::move(directPureBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Fields.directEffectfulArgument",
                                "()Int", std::move(directEffectfulArgumentBody).build(),
                                noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Fields.directEffectfulInitializer", "()Int",
       std::move(directEffectfulInitializerBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.Fields.unusedPureInitializerArgument", "()Int",
       std::move(unusedPureInitializerArgumentBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Fields.afterUnrelatedCall", "()Int",
                                std::move(afterUnrelatedCallBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Fields.afterPassedCall", "()Int",
                                std::move(afterPassedCallBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.Fields.shadowedUpdate", "()Int",
                                std::move(shadowedUpdateBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                "demo.interflow.Fields.effect",
                                "()Unit",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                "demo.interflow.Fields.effectInt",
                                "()Int",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                "demo.interflow.Fields.effectBox",
                                "(demo.interflow.Fields.Box)Unit",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.roots.push_back("demo.interflow.Fields.initial");
  program.roots.push_back("demo.interflow.Fields.updated");
  program.roots.push_back("demo.interflow.Fields.effectfulUpdate");
  program.roots.push_back("demo.interflow.Fields.directPure");
  program.roots.push_back("demo.interflow.Fields.directEffectfulArgument");
  program.roots.push_back("demo.interflow.Fields.directEffectfulInitializer");
  program.roots.push_back("demo.interflow.Fields.unusedPureInitializerArgument");
  program.roots.push_back("demo.interflow.Fields.afterUnrelatedCall");
  program.roots.push_back("demo.interflow.Fields.afterPassedCall");
  program.roots.push_back("demo.interflow.Fields.shadowedUpdate");
  program.reachableGlobals.push_back("demo.interflow.Fields.Box");
  program.reachableGlobals.push_back("demo.interflow.Fields.InitializedBox");
  program.reachableGlobals.push_back("demo.interflow.Fields.PureInitializedBox");
  program.reachableGlobals.push_back("demo.interflow.Fields.Box.value");
  program.reachableGlobals.push_back("demo.interflow.Fields.Box.current");
  program.reachableGlobals.push_back("demo.interflow.Fields.InitializedBox.value");
  program.reachableGlobals.push_back("demo.interflow.Fields.InitializedBox.current");
  program.reachableGlobals.push_back("demo.interflow.Fields.PureInitializedBox.value");
  program.reachableGlobals.push_back(
      "demo.interflow.Fields.PureInitializedBox.current");
  program.reachableGlobals.push_back("demo.interflow.Fields.Box.$init");
  program.reachableGlobals.push_back("demo.interflow.Fields.InitializedBox.$init");
  program.reachableGlobals.push_back("demo.interflow.Fields.PureInitializedBox.$init");
  program.reachableGlobals.push_back("demo.interflow.Fields.initial");
  program.reachableGlobals.push_back("demo.interflow.Fields.updated");
  program.reachableGlobals.push_back("demo.interflow.Fields.effectfulUpdate");
  program.reachableGlobals.push_back("demo.interflow.Fields.directPure");
  program.reachableGlobals.push_back("demo.interflow.Fields.directEffectfulArgument");
  program.reachableGlobals.push_back(
      "demo.interflow.Fields.directEffectfulInitializer");
  program.reachableGlobals.push_back(
      "demo.interflow.Fields.unusedPureInitializerArgument");
  program.reachableGlobals.push_back("demo.interflow.Fields.afterUnrelatedCall");
  program.reachableGlobals.push_back("demo.interflow.Fields.afterPassedCall");
  program.reachableGlobals.push_back("demo.interflow.Fields.shadowedUpdate");
  program.reachableGlobals.push_back("demo.interflow.Fields.effect");
  program.reachableGlobals.push_back("demo.interflow.Fields.effectInt");
  program.reachableGlobals.push_back("demo.interflow.Fields.effectBox");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code = expect(result.ok, "interflow rejected exact field-read program")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* initial =
      findDefinition(optimizedModule, "demo.interflow.Fields.initial");
  const scalanative::nir::Definition* updated =
      findDefinition(optimizedModule, "demo.interflow.Fields.updated");
  const scalanative::nir::Definition* effectfulUpdate =
      findDefinition(optimizedModule, "demo.interflow.Fields.effectfulUpdate");
  const scalanative::nir::Definition* directPure =
      findDefinition(optimizedModule, "demo.interflow.Fields.directPure");
  const scalanative::nir::Definition* directEffectfulArgument =
      findDefinition(optimizedModule, "demo.interflow.Fields.directEffectfulArgument");
  const scalanative::nir::Definition* directEffectfulInitializer = findDefinition(
      optimizedModule, "demo.interflow.Fields.directEffectfulInitializer");
  const scalanative::nir::Definition* unusedPureInitializerArgument = findDefinition(
      optimizedModule, "demo.interflow.Fields.unusedPureInitializerArgument");
  const scalanative::nir::Definition* afterUnrelatedCall =
      findDefinition(optimizedModule, "demo.interflow.Fields.afterUnrelatedCall");
  const scalanative::nir::Definition* afterPassedCall =
      findDefinition(optimizedModule, "demo.interflow.Fields.afterPassedCall");
  const scalanative::nir::Definition* shadowedUpdate =
      findDefinition(optimizedModule, "demo.interflow.Fields.shadowedUpdate");
  if (int code =
          expect(initial != nullptr && updated != nullptr && directPure != nullptr &&
                     effectfulUpdate != nullptr && directEffectfulArgument != nullptr &&
                     directEffectfulInitializer != nullptr &&
                     unusedPureInitializerArgument != nullptr &&
                     afterUnrelatedCall != nullptr && afterPassedCall != nullptr &&
                     shadowedUpdate != nullptr,
                 "interflow removed exact field-read smoke roots")) {
    return code;
  }

  if (int code =
          expect(scalanative::nir::bodyToText(initial->body).back() == "ret Int 7",
                 "interflow did not fold exact constructor field read")) {
    return code;
  }

  const std::vector<std::string> updatedText =
      scalanative::nir::bodyToText(updated->body);
  if (int code = expect(updatedText.size() == 2 && updatedText[1] == "ret Int 9",
                        "interflow did not refresh exact field after assignment")) {
    return code;
  }

  const std::vector<std::string> effectfulUpdateText =
      scalanative::nir::bodyToText(effectfulUpdate->body);
  if (int code =
          expect(effectfulUpdateText.size() == 4 &&
                     effectfulUpdateText[2] == "eval assign %box.current = call "
                                               "%demo.interflow.Fields.effectInt()" &&
                     effectfulUpdateText[3] == "ret Int 42",
                 "interflow dropped an effectful exact field assignment")) {
    return code;
  }

  if (int code =
          expect(scalanative::nir::bodyToText(directPure->body).back() == "ret Int 7",
                 "interflow did not fold direct exact constructor field read")) {
    return code;
  }

  const std::vector<std::string> directEffectfulArgumentText =
      scalanative::nir::bodyToText(directEffectfulArgument->body);
  const std::string directEffectfulArgumentResult =
      directEffectfulArgumentText.size() > 1 ? directEffectfulArgumentText[1]
                                             : std::string{};
  const std::string directFieldEffectCall = "call %demo.interflow.Fields.effectInt()";
  const std::size_t selectedFieldEffect =
      directEffectfulArgumentResult.find(directFieldEffectCall);
  const std::size_t laterFieldEffect =
      selectedFieldEffect == std::string::npos
          ? std::string::npos
          : directEffectfulArgumentResult.find(directFieldEffectCall,
                                               selectedFieldEffect +
                                                   directFieldEffectCall.size());
  if (int code = expect(
          directEffectfulArgumentText.size() == 2 &&
              selectedFieldEffect != std::string::npos &&
              laterFieldEffect != std::string::npos &&
              selectedFieldEffect < laterFieldEffect &&
              contains(directEffectfulArgumentResult,
                       "let %directFieldSelectResult : Int = ") &&
              contains(directEffectfulArgumentResult, "; %directFieldSelectResult)") &&
              !contains(directEffectfulArgumentResult,
                        "new demo.interflow.Fields.Box") &&
              !contains(directEffectfulArgumentResult, ".value") &&
              directEffectfulArgumentResult.rfind("ret Int ", 0) == 0,
          "interflow did not capture a direct field before later effects")) {
    return code;
  }

  if (int code = expect(
          scalanative::nir::bodyToText(directEffectfulInitializer->body).back() ==
              "ret Int new demo.interflow.Fields.InitializedBox(7, "
              "8).value",
          "interflow dropped an effectful direct constructor initializer")) {
    return code;
  }

  const std::vector<std::string> unusedPureInitializerArgumentText =
      scalanative::nir::bodyToText(unusedPureInitializerArgument->body);
  if (int code = expect(
          unusedPureInitializerArgumentText.size() == 3 &&
              unusedPureInitializerArgumentText[1] ==
                  "eval call %demo.interflow.Fields.effectInt()" &&
              unusedPureInitializerArgumentText[2] == "ret Int 43",
          "interflow did not remove a pure-initializer shell around an effect")) {
    return code;
  }

  const std::vector<std::string> afterUnrelatedCallText =
      scalanative::nir::bodyToText(afterUnrelatedCall->body);
  if (int code =
          expect(afterUnrelatedCallText.size() == 3 &&
                     afterUnrelatedCallText[1] ==
                         "eval call %demo.interflow.Fields.effect()" &&
                     afterUnrelatedCallText[2] == "ret Int 7",
                 "interflow did not preserve exact field read after unrelated call")) {
    return code;
  }

  const std::vector<std::string> afterPassedCallText =
      scalanative::nir::bodyToText(afterPassedCall->body);
  if (int code =
          expect(afterPassedCallText.size() == 4 &&
                     afterPassedCallText[2] ==
                         "eval call %demo.interflow.Fields.effectBox(%box)" &&
                     afterPassedCallText[3] == "ret Int %box.value",
                 "interflow folded exact field read after object-passing call")) {
    return code;
  }

  const std::vector<std::string> shadowedUpdateText =
      scalanative::nir::bodyToText(shadowedUpdate->body);
  bool sawShadowedBoxEffect = false;
  bool sawShadowedFieldAssign = false;
  for (const std::string& line : shadowedUpdateText) {
    sawShadowedBoxEffect =
        sawShadowedBoxEffect ||
        contains(line, "call %demo.interflow.Fields.effectBox(%box)");
    sawShadowedFieldAssign =
        sawShadowedFieldAssign || contains(line, "assign %box.current = 9");
  }
  return expect(sawShadowedBoxEffect && !sawShadowedFieldAssign &&
                    shadowedUpdateText.back() == "ret Int 42",
                "interflow let a shadowed object use block exact field assign DCE");
}

int smokeInterflowDiscardableArrayAllocation() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();
  const auto runtime = [&](std::string_view name) {
    return scalanative::nir::localValue(std::string(name), noSpan);
  };
  const auto arrayValue = [&](std::string_view elementType,
                              std::vector<scalanative::nir::Value> values) {
    return scalanative::nir::newValue("Array [ " + std::string(elementType) + " ]",
                                      std::move(values), noSpan);
  };
  const auto lengthCall = [&](scalanative::nir::Value array) {
    return scalanative::nir::callValue(
        runtime(scalanative::support::StdNames::RuntimeIntArrayLength),
        {std::move(array)}, noSpan);
  };
  const auto applyCall = [&](scalanative::nir::Value array,
                             scalanative::nir::Value index) {
    std::vector<scalanative::nir::Value> arguments;
    arguments.push_back(std::move(array));
    arguments.push_back(std::move(index));
    return scalanative::nir::callValue(
        runtime(scalanative::support::StdNames::RuntimeIntArrayApply),
        std::move(arguments), noSpan);
  };
  const auto updateCall = [&](scalanative::nir::Value array,
                              scalanative::nir::Value index,
                              scalanative::nir::Value value) {
    std::vector<scalanative::nir::Value> arguments;
    arguments.push_back(std::move(array));
    arguments.push_back(std::move(index));
    arguments.push_back(std::move(value));
    return scalanative::nir::callValue(
        runtime(scalanative::support::StdNames::RuntimeIntArrayUpdate),
        std::move(arguments), noSpan);
  };

  scalanative::nir::FunctionBodyBuilder lengthBody;
  (void)lengthBody.addLet(
      "values", "Array [ Int ]",
      arrayValue("Int", {scalanative::nir::literalValue("1", "Int", noSpan),
                         scalanative::nir::literalValue("2", "Int", noSpan),
                         scalanative::nir::literalValue("3", "Int", noSpan)}),
      noSpan);
  (void)lengthBody.addReturn(
      "Int", lengthCall(scalanative::nir::localValue("values", noSpan)), noSpan);

  scalanative::nir::FunctionBodyBuilder directLengthBody;
  (void)directLengthBody.addReturn(
      "Int",
      lengthCall(
          arrayValue("Int", {scalanative::nir::literalValue("1", "Int", noSpan),
                             scalanative::nir::literalValue("2", "Int", noSpan),
                             scalanative::nir::literalValue("3", "Int", noSpan)})),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directEffectfulLengthBody;
  (void)directEffectfulLengthBody.addReturn(
      "Int",
      lengthCall(arrayValue(
          "Int", {scalanative::nir::callValue(
                      runtime("demo.interflow.ArrayDead.effectInt"), {}, noSpan),
                  scalanative::nir::literalValue("2", "Int", noSpan)})),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directEffectfulObjectTestBody;
  (void)directEffectfulObjectTestBody.addReturn(
      "Boolean",
      scalanative::nir::isInstanceOfValue(
          "Object",
          arrayValue("Int",
                     {scalanative::nir::callValue(
                          runtime("demo.interflow.ArrayDead.effectInt"), {}, noSpan),
                      scalanative::nir::literalValue("2", "Int", noSpan)}),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localEffectfulObjectTestBody;
  (void)localEffectfulObjectTestBody.addLet(
      "values", "Array [ Int ]",
      arrayValue("Int", {scalanative::nir::callValue(
                             runtime("demo.interflow.ArrayDead.effectInt"), {}, noSpan),
                         scalanative::nir::literalValue("2", "Int", noSpan)}),
      noSpan);
  (void)localEffectfulObjectTestBody.addReturn(
      "Boolean",
      scalanative::nir::isInstanceOfValue(
          "Object", scalanative::nir::localValue("values", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directEffectfulObjectCastBody;
  (void)directEffectfulObjectCastBody.addReturn(
      "Object",
      scalanative::nir::asInstanceOfValue(
          "Object",
          arrayValue("Int",
                     {scalanative::nir::callValue(
                          runtime("demo.interflow.ArrayDead.effectInt"), {}, noSpan),
                      scalanative::nir::literalValue("2", "Int", noSpan)}),
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder localEffectfulObjectCastBody;
  (void)localEffectfulObjectCastBody.addLet(
      "values", "Array [ Int ]",
      arrayValue("Int", {scalanative::nir::callValue(
                             runtime("demo.interflow.ArrayDead.effectInt"), {}, noSpan),
                         scalanative::nir::literalValue("2", "Int", noSpan)}),
      noSpan);
  (void)localEffectfulObjectCastBody.addReturn(
      "Object",
      scalanative::nir::asInstanceOfValue(
          "Object", scalanative::nir::localValue("values", noSpan), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directApplyBody;
  (void)directApplyBody.addReturn(
      "Int",
      applyCall(arrayValue("Int", {scalanative::nir::literalValue("4", "Int", noSpan),
                                   scalanative::nir::literalValue("5", "Int", noSpan),
                                   scalanative::nir::literalValue("6", "Int", noSpan)}),
                scalanative::nir::literalValue("1", "Int", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder directEffectfulApplyBody;
  (void)directEffectfulApplyBody.addParameter("directArrayApplyResult", "Int", noSpan);
  (void)directEffectfulApplyBody.addReturn(
      "Int",
      applyCall(
          arrayValue("Int",
                     {scalanative::nir::literalValue("1", "Int", noSpan),
                      scalanative::nir::callValue(
                          runtime("demo.interflow.ArrayDead.effectInt"), {}, noSpan),
                      scalanative::nir::callValue(
                          runtime("demo.interflow.ArrayDead.effectInt"), {}, noSpan)}),
          scalanative::nir::literalValue("1", "Int", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder elementBody;
  (void)elementBody.addLet(
      "values", "Array [ Int ]",
      arrayValue("Int", {scalanative::nir::literalValue("4", "Int", noSpan),
                         scalanative::nir::literalValue("5", "Int", noSpan),
                         scalanative::nir::literalValue("6", "Int", noSpan)}),
      noSpan);
  (void)elementBody.addReturn(
      "Int",
      applyCall(scalanative::nir::localValue("values", noSpan),
                scalanative::nir::literalValue("1", "Int", noSpan)),
      noSpan);

  scalanative::nir::FunctionBodyBuilder nestedBody;
  (void)nestedBody.addReturn(
      "Int",
      scalanative::nir::blockValue(
          {scalanative::nir::localLetValue(
               "values", "Array [ Int ]",
               arrayValue("Int", {scalanative::nir::literalValue("7", "Int", noSpan),
                                  scalanative::nir::literalValue("8", "Int", noSpan)}),
               noSpan),
           scalanative::nir::literalValue("9", "Int", noSpan)},
          noSpan),
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
  (void)updatedBody.addReturn("Int", scalanative::nir::literalValue("9", "Int", noSpan),
                              noSpan);

  scalanative::nir::FunctionBodyBuilder dynamicUpdateBody;
  (void)dynamicUpdateBody.addParameter("index", "Int", noSpan);
  (void)dynamicUpdateBody.addLet(
      "values", "Array [ Int ]",
      arrayValue("Int", {scalanative::nir::literalValue("1", "Int", noSpan),
                         scalanative::nir::literalValue("2", "Int", noSpan)}),
      noSpan);
  (void)dynamicUpdateBody.addEval(
      updateCall(scalanative::nir::localValue("values", noSpan),
                 scalanative::nir::localValue("index", noSpan),
                 scalanative::nir::literalValue("9", "Int", noSpan)),
      noSpan);
  (void)dynamicUpdateBody.addReturn(
      "Int", scalanative::nir::literalValue("2", "Int", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder effectfulUpdateBody;
  (void)effectfulUpdateBody.addLet(
      "values", "Array [ Int ]",
      arrayValue("Int", {scalanative::nir::literalValue("1", "Int", noSpan),
                         scalanative::nir::literalValue("2", "Int", noSpan)}),
      noSpan);
  (void)effectfulUpdateBody.addEval(
      updateCall(scalanative::nir::localValue("values", noSpan),
                 scalanative::nir::literalValue("0", "Int", noSpan),
                 scalanative::nir::callValue(
                     runtime("demo.interflow.ArrayDead.effectInt"), {}, noSpan)),
      noSpan);
  (void)effectfulUpdateBody.addReturn(
      "Int", scalanative::nir::literalValue("7", "Int", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder directUpdateBody;
  (void)directUpdateBody.addEval(
      updateCall(
          arrayValue("Int", {scalanative::nir::literalValue("1", "Int", noSpan),
                             scalanative::nir::literalValue("2", "Int", noSpan)}),
          scalanative::nir::literalValue("0", "Int", noSpan),
          scalanative::nir::literalValue("9", "Int", noSpan)),
      noSpan);
  (void)directUpdateBody.addReturn(
      "Int", scalanative::nir::literalValue("13", "Int", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder directEffectfulUpdateBody;
  (void)directEffectfulUpdateBody.addEval(
      updateCall(
          arrayValue("Int",
                     {scalanative::nir::callValue(
                          runtime("demo.interflow.ArrayDead.effectInt"), {}, noSpan),
                      scalanative::nir::literalValue("2", "Int", noSpan)}),
          scalanative::nir::literalValue("1", "Int", noSpan),
          scalanative::nir::callValue(runtime("demo.interflow.ArrayDead.effectInt"), {},
                                      noSpan)),
      noSpan);
  (void)directEffectfulUpdateBody.addReturn(
      "Int", scalanative::nir::literalValue("14", "Int", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder directOutOfBoundsUpdateBody;
  (void)directOutOfBoundsUpdateBody.addEval(
      updateCall(
          arrayValue("Int", {scalanative::nir::literalValue("1", "Int", noSpan)}),
          scalanative::nir::literalValue("2", "Int", noSpan),
          scalanative::nir::literalValue("9", "Int", noSpan)),
      noSpan);
  (void)directOutOfBoundsUpdateBody.addReturn(
      "Int", scalanative::nir::literalValue("15", "Int", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder shadowedUpdateBody;
  (void)shadowedUpdateBody.addLet(
      "values", "Array [ Int ]",
      arrayValue("Int", {scalanative::nir::literalValue("1", "Int", noSpan),
                         scalanative::nir::literalValue("2", "Int", noSpan)}),
      noSpan);
  (void)shadowedUpdateBody.addEval(
      updateCall(scalanative::nir::localValue("values", noSpan),
                 scalanative::nir::literalValue("0", "Int", noSpan),
                 scalanative::nir::literalValue("9", "Int", noSpan)),
      noSpan);
  (void)shadowedUpdateBody.addEval(
      scalanative::nir::blockValue(
          {scalanative::nir::localLetValue(
               "values", "Array [ Int ]",
               arrayValue("Int", {scalanative::nir::literalValue("4", "Int", noSpan)}),
               noSpan),
           scalanative::nir::callValue(runtime("demo.interflow.ArrayDead.effectArray"),
                                       {scalanative::nir::localValue("values", noSpan)},
                                       noSpan)},
          noSpan),
      noSpan);
  (void)shadowedUpdateBody.addReturn(
      "Int", scalanative::nir::literalValue("9", "Int", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder effectfulElementBody;
  (void)effectfulElementBody.addLet(
      "values", "Array [ Object ]",
      arrayValue("Object",
                 {scalanative::nir::callValue(
                     runtime("demo.interflow.ArrayDead.effect"), {}, noSpan)}),
      noSpan);
  (void)effectfulElementBody.addReturn(
      "Int", scalanative::nir::literalValue("7", "Int", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder nestedAllocationElementsBody;
  (void)nestedAllocationElementsBody.addLet(
      "values", "Array [ Object ]",
      arrayValue(
          "Object",
          {scalanative::nir::newValue("demo.interflow.ArrayDead.Element", noSpan),
           scalanative::nir::newValue("demo.interflow.ArrayDead.Element", noSpan)}),
      noSpan);
  (void)nestedAllocationElementsBody.addReturn(
      "Int", scalanative::nir::literalValue("10", "Int", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder effectfulAllocationElementBody;
  (void)effectfulAllocationElementBody.addLet(
      "values", "Array [ Object ]",
      arrayValue("Object", {scalanative::nir::newValue(
                               "demo.interflow.ArrayDead.EffectElement", noSpan)}),
      noSpan);
  (void)effectfulAllocationElementBody.addReturn(
      "Int", scalanative::nir::literalValue("11", "Int", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder orderedEffectfulElementsBody;
  (void)orderedEffectfulElementsBody.addLet(
      "values", "Array [ Object ]",
      arrayValue(
          "Object",
          {scalanative::nir::callValue(runtime("demo.interflow.ArrayDead.effect"), {},
                                       noSpan),
           scalanative::nir::newValue("demo.interflow.ArrayDead.Element", noSpan),
           scalanative::nir::callValue(runtime("demo.interflow.ArrayDead.effectOther"),
                                       {}, noSpan)}),
      noSpan);
  (void)orderedEffectfulElementsBody.addReturn(
      "Int", scalanative::nir::literalValue("12", "Int", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder effectElementInitializerBody;
  (void)effectElementInitializerBody.addParameter(
      "this", "demo.interflow.ArrayDead.EffectElement", noSpan);
  (void)effectElementInitializerBody.addEval(
      scalanative::nir::callValue(runtime("demo.interflow.ArrayDead.effect"), {},
                                  noSpan),
      noSpan);
  (void)effectElementInitializerBody.addReturn(
      "Unit", scalanative::nir::unitValue(noSpan), noSpan);

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
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                "demo.interflow.ArrayDead.Element",
                                "@java.lang.Object",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::Class,
                                "demo.interflow.ArrayDead.EffectElement",
                                "@java.lang.Object",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayDead.EffectElement.$init",
                                "(demo.interflow.ArrayDead.EffectElement)Unit",
                                std::move(effectElementInitializerBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayDead.length", "()Int",
                                std::move(lengthBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayDead.directLength", "()Int",
                                std::move(directLengthBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayDead.directEffectfulLength",
                                "()Int", std::move(directEffectfulLengthBody).build(),
                                noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.ArrayDead.directEffectfulObjectTest", "()Boolean",
       std::move(directEffectfulObjectTestBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.ArrayDead.localEffectfulObjectTest", "()Boolean",
       std::move(localEffectfulObjectTestBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.ArrayDead.directEffectfulObjectCast", "()Object",
       std::move(directEffectfulObjectCastBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.ArrayDead.localEffectfulObjectCast", "()Object",
       std::move(localEffectfulObjectCastBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayDead.directApply", "()Int",
                                std::move(directApplyBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayDead.directEffectfulApply",
                                "(Int)Int", std::move(directEffectfulApplyBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayDead.element", "()Int",
                                std::move(elementBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayDead.nested", "()Int",
                                std::move(nestedBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayDead.updated", "()Int",
                                std::move(updatedBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayDead.dynamicUpdate", "(Int)Int",
                                std::move(dynamicUpdateBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayDead.effectfulUpdate", "()Int",
                                std::move(effectfulUpdateBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayDead.directUpdate", "()Int",
                                std::move(directUpdateBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayDead.directEffectfulUpdate",
                                "()Int", std::move(directEffectfulUpdateBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayDead.directOutOfBoundsUpdate",
                                "()Int", std::move(directOutOfBoundsUpdateBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayDead.shadowedUpdate", "()Int",
                                std::move(shadowedUpdateBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.ArrayDead.effectfulElement", "()Int",
                                std::move(effectfulElementBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.ArrayDead.nestedAllocationElements", "()Int",
       std::move(nestedAllocationElementsBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.ArrayDead.effectfulAllocationElement", "()Int",
       std::move(effectfulAllocationElementBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.ArrayDead.orderedEffectfulElements", "()Int",
       std::move(orderedEffectfulElementsBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                "demo.interflow.ArrayDead.effect",
                                "()Object",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                "demo.interflow.ArrayDead.effectInt",
                                "()Int",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                "demo.interflow.ArrayDead.effectOther",
                                "()Object",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                "demo.interflow.ArrayDead.effectArray",
                                "(Array [ Int ])Unit",
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
       std::string(scalanative::support::StdNames::RuntimeIntArrayApply),
       "(Array [ Int ],Int)Int",
       {},
       noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDecl,
       std::string(scalanative::support::StdNames::RuntimeIntArrayUpdate),
       "(Array [ Int ],Int,Int)Unit",
       {},
       noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.roots.push_back("demo.interflow.ArrayDead.length");
  program.roots.push_back("demo.interflow.ArrayDead.directLength");
  program.roots.push_back("demo.interflow.ArrayDead.directEffectfulLength");
  program.roots.push_back("demo.interflow.ArrayDead.directEffectfulObjectTest");
  program.roots.push_back("demo.interflow.ArrayDead.localEffectfulObjectTest");
  program.roots.push_back("demo.interflow.ArrayDead.directEffectfulObjectCast");
  program.roots.push_back("demo.interflow.ArrayDead.localEffectfulObjectCast");
  program.roots.push_back("demo.interflow.ArrayDead.directApply");
  program.roots.push_back("demo.interflow.ArrayDead.directEffectfulApply");
  program.roots.push_back("demo.interflow.ArrayDead.element");
  program.roots.push_back("demo.interflow.ArrayDead.nested");
  program.roots.push_back("demo.interflow.ArrayDead.updated");
  program.roots.push_back("demo.interflow.ArrayDead.dynamicUpdate");
  program.roots.push_back("demo.interflow.ArrayDead.effectfulUpdate");
  program.roots.push_back("demo.interflow.ArrayDead.directUpdate");
  program.roots.push_back("demo.interflow.ArrayDead.directEffectfulUpdate");
  program.roots.push_back("demo.interflow.ArrayDead.directOutOfBoundsUpdate");
  program.roots.push_back("demo.interflow.ArrayDead.shadowedUpdate");
  program.roots.push_back("demo.interflow.ArrayDead.effectfulElement");
  program.roots.push_back("demo.interflow.ArrayDead.nestedAllocationElements");
  program.roots.push_back("demo.interflow.ArrayDead.effectfulAllocationElement");
  program.roots.push_back("demo.interflow.ArrayDead.orderedEffectfulElements");
  program.reachableGlobals.push_back("demo.interflow.ArrayDead.Element");
  program.reachableGlobals.push_back("demo.interflow.ArrayDead.EffectElement");
  program.reachableGlobals.push_back("demo.interflow.ArrayDead.EffectElement.$init");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.ArrayDead.length");
  program.reachableGlobals.push_back("demo.interflow.ArrayDead.directLength");
  program.reachableGlobals.push_back("demo.interflow.ArrayDead.directEffectfulLength");
  program.reachableGlobals.push_back(
      "demo.interflow.ArrayDead.directEffectfulObjectTest");
  program.reachableGlobals.push_back(
      "demo.interflow.ArrayDead.localEffectfulObjectTest");
  program.reachableGlobals.push_back(
      "demo.interflow.ArrayDead.directEffectfulObjectCast");
  program.reachableGlobals.push_back(
      "demo.interflow.ArrayDead.localEffectfulObjectCast");
  program.reachableGlobals.push_back("demo.interflow.ArrayDead.directApply");
  program.reachableGlobals.push_back("demo.interflow.ArrayDead.directEffectfulApply");
  program.reachableGlobals.push_back("demo.interflow.ArrayDead.element");
  program.reachableGlobals.push_back("demo.interflow.ArrayDead.nested");
  program.reachableGlobals.push_back("demo.interflow.ArrayDead.updated");
  program.reachableGlobals.push_back("demo.interflow.ArrayDead.dynamicUpdate");
  program.reachableGlobals.push_back("demo.interflow.ArrayDead.effectfulUpdate");
  program.reachableGlobals.push_back("demo.interflow.ArrayDead.directUpdate");
  program.reachableGlobals.push_back("demo.interflow.ArrayDead.directEffectfulUpdate");
  program.reachableGlobals.push_back(
      "demo.interflow.ArrayDead.directOutOfBoundsUpdate");
  program.reachableGlobals.push_back("demo.interflow.ArrayDead.shadowedUpdate");
  program.reachableGlobals.push_back("demo.interflow.ArrayDead.effectfulElement");
  program.reachableGlobals.push_back(
      "demo.interflow.ArrayDead.nestedAllocationElements");
  program.reachableGlobals.push_back(
      "demo.interflow.ArrayDead.effectfulAllocationElement");
  program.reachableGlobals.push_back(
      "demo.interflow.ArrayDead.orderedEffectfulElements");
  program.reachableGlobals.push_back("demo.interflow.ArrayDead.effect");
  program.reachableGlobals.push_back("demo.interflow.ArrayDead.effectInt");
  program.reachableGlobals.push_back("demo.interflow.ArrayDead.effectOther");
  program.reachableGlobals.push_back("demo.interflow.ArrayDead.effectArray");
  program.reachableGlobals.push_back(
      std::string(scalanative::support::StdNames::RuntimeIntArrayLength));
  program.reachableGlobals.push_back(
      std::string(scalanative::support::StdNames::RuntimeIntArrayApply));
  program.reachableGlobals.push_back(
      std::string(scalanative::support::StdNames::RuntimeIntArrayUpdate));

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code = expect(result.ok,
                        "interflow rejected discardable array allocation program")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* length =
      findDefinition(optimizedModule, "demo.interflow.ArrayDead.length");
  const scalanative::nir::Definition* directLength =
      findDefinition(optimizedModule, "demo.interflow.ArrayDead.directLength");
  const scalanative::nir::Definition* directEffectfulLength =
      findDefinition(optimizedModule, "demo.interflow.ArrayDead.directEffectfulLength");
  const scalanative::nir::Definition* directEffectfulObjectTest = findDefinition(
      optimizedModule, "demo.interflow.ArrayDead.directEffectfulObjectTest");
  const scalanative::nir::Definition* localEffectfulObjectTest = findDefinition(
      optimizedModule, "demo.interflow.ArrayDead.localEffectfulObjectTest");
  const scalanative::nir::Definition* directEffectfulObjectCast = findDefinition(
      optimizedModule, "demo.interflow.ArrayDead.directEffectfulObjectCast");
  const scalanative::nir::Definition* localEffectfulObjectCast = findDefinition(
      optimizedModule, "demo.interflow.ArrayDead.localEffectfulObjectCast");
  const scalanative::nir::Definition* directApply =
      findDefinition(optimizedModule, "demo.interflow.ArrayDead.directApply");
  const scalanative::nir::Definition* directEffectfulApply =
      findDefinition(optimizedModule, "demo.interflow.ArrayDead.directEffectfulApply");
  const scalanative::nir::Definition* element =
      findDefinition(optimizedModule, "demo.interflow.ArrayDead.element");
  const scalanative::nir::Definition* nested =
      findDefinition(optimizedModule, "demo.interflow.ArrayDead.nested");
  const scalanative::nir::Definition* updated =
      findDefinition(optimizedModule, "demo.interflow.ArrayDead.updated");
  const scalanative::nir::Definition* dynamicUpdate =
      findDefinition(optimizedModule, "demo.interflow.ArrayDead.dynamicUpdate");
  const scalanative::nir::Definition* effectfulUpdate =
      findDefinition(optimizedModule, "demo.interflow.ArrayDead.effectfulUpdate");
  const scalanative::nir::Definition* directUpdate =
      findDefinition(optimizedModule, "demo.interflow.ArrayDead.directUpdate");
  const scalanative::nir::Definition* directEffectfulUpdate =
      findDefinition(optimizedModule, "demo.interflow.ArrayDead.directEffectfulUpdate");
  const scalanative::nir::Definition* directOutOfBoundsUpdate = findDefinition(
      optimizedModule, "demo.interflow.ArrayDead.directOutOfBoundsUpdate");
  const scalanative::nir::Definition* shadowedUpdate =
      findDefinition(optimizedModule, "demo.interflow.ArrayDead.shadowedUpdate");
  const scalanative::nir::Definition* effectfulElement =
      findDefinition(optimizedModule, "demo.interflow.ArrayDead.effectfulElement");
  const scalanative::nir::Definition* nestedAllocationElements = findDefinition(
      optimizedModule, "demo.interflow.ArrayDead.nestedAllocationElements");
  const scalanative::nir::Definition* effectfulAllocationElement = findDefinition(
      optimizedModule, "demo.interflow.ArrayDead.effectfulAllocationElement");
  const scalanative::nir::Definition* orderedEffectfulElements = findDefinition(
      optimizedModule, "demo.interflow.ArrayDead.orderedEffectfulElements");
  if (int code = expect(
          length != nullptr && directLength != nullptr &&
              directEffectfulLength != nullptr &&
              directEffectfulObjectTest != nullptr &&
              localEffectfulObjectTest != nullptr &&
              directEffectfulObjectCast != nullptr &&
              localEffectfulObjectCast != nullptr && directApply != nullptr &&
              directEffectfulApply != nullptr && element != nullptr &&
              nested != nullptr && updated != nullptr && dynamicUpdate != nullptr &&
              effectfulUpdate != nullptr && directUpdate != nullptr &&
              directEffectfulUpdate != nullptr && directOutOfBoundsUpdate != nullptr &&
              shadowedUpdate != nullptr && effectfulElement != nullptr &&
              nestedAllocationElements != nullptr &&
              effectfulAllocationElement != nullptr &&
              orderedEffectfulElements != nullptr,
          "interflow removed discardable array smoke roots")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(length->body).size() == 2 &&
                     scalanative::nir::bodyToText(length->body).back() == "ret Int 3",
                 "interflow did not remove unused folded array length allocation")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(directLength->body).size() == 2 &&
                            scalanative::nir::bodyToText(directLength->body).back() ==
                                "ret Int 3",
                        "interflow did not fold a direct fresh array length")) {
    return code;
  }
  const std::vector<std::string> directEffectfulLengthText =
      scalanative::nir::bodyToText(directEffectfulLength->body);
  if (int code = expect(
          directEffectfulLengthText.size() == 2 &&
              contains(directEffectfulLengthText[1],
                       "call %demo.interflow.ArrayDead.effectInt()") &&
              contains(directEffectfulLengthText[1], "; 2)") &&
              !contains(directEffectfulLengthText[1], "arrayLength") &&
              !contains(directEffectfulLengthText[1], "new Array") &&
              directEffectfulLengthText[1].rfind("ret Int ", 0) == 0,
          "interflow did not preserve effects while folding direct array length")) {
    return code;
  }
  const std::vector<std::string> directEffectfulObjectTestText =
      scalanative::nir::bodyToText(directEffectfulObjectTest->body);
  if (int code = expect(
          directEffectfulObjectTestText.size() == 2 &&
              contains(directEffectfulObjectTestText[1],
                       "call %demo.interflow.ArrayDead.effectInt()") &&
              contains(directEffectfulObjectTestText[1], "; true)") &&
              !contains(directEffectfulObjectTestText[1], "is-instance-of") &&
              !contains(directEffectfulObjectTestText[1], "new Array") &&
              directEffectfulObjectTestText[1].rfind("ret Boolean ", 0) == 0,
          "interflow did not preserve effects while folding direct array type test")) {
    return code;
  }
  const std::vector<std::string> localEffectfulObjectTestText =
      scalanative::nir::bodyToText(localEffectfulObjectTest->body);
  if (int code = expect(
          localEffectfulObjectTestText.size() == 3 &&
              localEffectfulObjectTestText[1] ==
                  "eval call %demo.interflow.ArrayDead.effectInt()" &&
              localEffectfulObjectTestText[2] == "ret Boolean true",
          "interflow did not preserve effects while folding local array type test")) {
    return code;
  }
  const std::vector<std::string> directEffectfulObjectCastText =
      scalanative::nir::bodyToText(directEffectfulObjectCast->body);
  if (int code =
          expect(directEffectfulObjectCastText.size() == 2 &&
                     contains(directEffectfulObjectCastText[1], "new Array [ Int ]") &&
                     contains(directEffectfulObjectCastText[1],
                              "call %demo.interflow.ArrayDead.effectInt()") &&
                     !contains(directEffectfulObjectCastText[1], "as-instance-of") &&
                     directEffectfulObjectCastText[1].rfind("ret Object ", 0) == 0,
                 "interflow did not erase direct array upcast to Object")) {
    return code;
  }
  const std::vector<std::string> localEffectfulObjectCastText =
      scalanative::nir::bodyToText(localEffectfulObjectCast->body);
  if (int code =
          expect(localEffectfulObjectCastText.size() == 3 &&
                     contains(localEffectfulObjectCastText[1], "new Array [ Int ]") &&
                     contains(localEffectfulObjectCastText[1],
                              "call %demo.interflow.ArrayDead.effectInt()") &&
                     localEffectfulObjectCastText[2] == "ret Object %values" &&
                     !contains(localEffectfulObjectCastText[2], "as-instance-of"),
                 "interflow did not erase local array upcast to Object")) {
    return code;
  }
  if (int code = expect(scalanative::nir::bodyToText(directApply->body).size() == 2 &&
                            scalanative::nir::bodyToText(directApply->body).back() ==
                                "ret Int 5",
                        "interflow did not fold direct fresh array apply")) {
    return code;
  }
  const std::vector<std::string> directEffectfulApplyText =
      scalanative::nir::bodyToText(directEffectfulApply->body);
  const std::string directEffectfulApplyResult =
      directEffectfulApplyText.size() > 2 ? directEffectfulApplyText[2] : std::string{};
  const std::string effectCall = "call %demo.interflow.ArrayDead.effectInt()";
  const std::size_t selectedEffect = directEffectfulApplyResult.find(effectCall);
  const std::size_t laterEffect =
      selectedEffect == std::string::npos
          ? std::string::npos
          : directEffectfulApplyResult.find(effectCall,
                                            selectedEffect + effectCall.size());
  if (int code = expect(
          directEffectfulApplyText.size() == 3 &&
              directEffectfulApplyText[1] == "param %directArrayApplyResult : Int" &&
              selectedEffect != std::string::npos && laterEffect != std::string::npos &&
              selectedEffect < laterEffect &&
              contains(directEffectfulApplyResult,
                       "let %directArrayApplyResult1 : Int = ") &&
              contains(directEffectfulApplyResult, "; %directArrayApplyResult1)") &&
              !contains(directEffectfulApplyResult, "arrayApply") &&
              !contains(directEffectfulApplyResult, "new Array") &&
              directEffectfulApplyResult.rfind("ret Int ", 0) == 0,
          "interflow did not capture direct array apply before later effects")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(element->body).size() == 2 &&
                     scalanative::nir::bodyToText(element->body).back() == "ret Int 5",
                 "interflow did not remove unused folded array apply allocation")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(nested->body).size() == 2 &&
                     scalanative::nir::bodyToText(nested->body).back() == "ret Int 9",
                 "interflow did not remove nested pure array allocation")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(updated->body).size() == 2 &&
                     scalanative::nir::bodyToText(updated->body).back() == "ret Int 9",
                 "interflow did not remove exact private array update")) {
    return code;
  }

  const std::vector<std::string> dynamicUpdateText =
      scalanative::nir::bodyToText(dynamicUpdate->body);
  if (int code = expect(dynamicUpdateText.size() > 2,
                        "interflow dropped dynamic-index array update")) {
    return code;
  }
  bool sawDynamicUpdate = false;
  for (const std::string& line : dynamicUpdateText) {
    sawDynamicUpdate =
        sawDynamicUpdate ||
        contains(line,
                 std::string(scalanative::support::StdNames::RuntimeIntArrayUpdate));
  }
  if (int code =
          expect(sawDynamicUpdate, "interflow lost dynamic-index array update call")) {
    return code;
  }

  const std::vector<std::string> effectfulUpdateText =
      scalanative::nir::bodyToText(effectfulUpdate->body);
  if (int code = expect(effectfulUpdateText.size() > 2,
                        "interflow dropped effectful array update value")) {
    return code;
  }
  bool sawEffectfulUpdate = false;
  for (const std::string& line : effectfulUpdateText) {
    sawEffectfulUpdate = sawEffectfulUpdate ||
                         contains(line, "call %demo.interflow.ArrayDead.effectInt()");
  }
  if (int code =
          expect(sawEffectfulUpdate, "interflow lost effectful array update value")) {
    return code;
  }

  if (int code = expect(scalanative::nir::bodyToText(directUpdate->body).size() == 2 &&
                            scalanative::nir::bodyToText(directUpdate->body).back() ==
                                "ret Int 13",
                        "interflow did not remove direct fresh array update")) {
    return code;
  }
  const std::vector<std::string> directEffectfulUpdateText =
      scalanative::nir::bodyToText(directEffectfulUpdate->body);
  const std::string directEffectfulUpdateEffects = directEffectfulUpdateText.size() > 1
                                                       ? directEffectfulUpdateText[1]
                                                       : std::string{};
  const std::size_t initialElementEffect =
      directEffectfulUpdateEffects.find(effectCall);
  const std::size_t assignedValueEffect =
      initialElementEffect == std::string::npos
          ? std::string::npos
          : directEffectfulUpdateEffects.find(effectCall,
                                              initialElementEffect + effectCall.size());
  if (int code = expect(
          directEffectfulUpdateText.size() == 3 &&
              initialElementEffect != std::string::npos &&
              assignedValueEffect != std::string::npos &&
              initialElementEffect < assignedValueEffect &&
              !contains(
                  directEffectfulUpdateEffects,
                  std::string(scalanative::support::StdNames::RuntimeIntArrayUpdate)) &&
              !contains(directEffectfulUpdateEffects, "new Array") &&
              directEffectfulUpdateText[2] == "ret Int 14",
          "interflow did not retain ordered effects of a direct array update")) {
    return code;
  }
  const std::vector<std::string> directOutOfBoundsUpdateText =
      scalanative::nir::bodyToText(directOutOfBoundsUpdate->body);
  if (int code = expect(
          directOutOfBoundsUpdateText.size() == 3 &&
              contains(directOutOfBoundsUpdateText[1], "new Array [ Int ]") &&
              contains(
                  directOutOfBoundsUpdateText[1],
                  std::string(scalanative::support::StdNames::RuntimeIntArrayUpdate)) &&
              directOutOfBoundsUpdateText[2] == "ret Int 15",
          "interflow removed a direct out-of-bounds array update")) {
    return code;
  }

  const std::vector<std::string> shadowedUpdateText =
      scalanative::nir::bodyToText(shadowedUpdate->body);
  bool sawShadowedArrayEffect = false;
  bool sawShadowedArrayUpdate = false;
  for (const std::string& line : shadowedUpdateText) {
    sawShadowedArrayEffect =
        sawShadowedArrayEffect ||
        contains(line, "call %demo.interflow.ArrayDead.effectArray(%values)");
    sawShadowedArrayUpdate =
        sawShadowedArrayUpdate ||
        contains(line,
                 std::string(scalanative::support::StdNames::RuntimeIntArrayUpdate));
  }
  if (int code =
          expect(sawShadowedArrayEffect && !sawShadowedArrayUpdate &&
                     shadowedUpdateText.back() == "ret Int 9",
                 "interflow let a shadowed array use block private update DCE")) {
    return code;
  }

  const std::vector<std::string> effectfulText =
      scalanative::nir::bodyToText(effectfulElement->body);
  if (int code = expect(
          effectfulText.size() == 3 &&
              effectfulText[1] == "eval call %demo.interflow.ArrayDead.effect()" &&
              effectfulText[2] == "ret Int 7",
          "interflow did not retain only the effectful array element evaluation")) {
    return code;
  }
  const std::vector<std::string> orderedEffectfulElementsText =
      scalanative::nir::bodyToText(orderedEffectfulElements->body);
  const std::string effectLine = orderedEffectfulElementsText.size() > 1
                                     ? orderedEffectfulElementsText[1]
                                     : std::string{};
  const std::size_t firstEffect =
      effectLine.find("call %demo.interflow.ArrayDead.effect()");
  const std::size_t secondEffect =
      effectLine.find("call %demo.interflow.ArrayDead.effectOther()");
  if (int code = expect(
          orderedEffectfulElementsText.size() == 3 &&
              firstEffect != std::string::npos && secondEffect != std::string::npos &&
              firstEffect < secondEffect &&
              !contains(effectLine, "new Array [ Object ]") &&
              !contains(effectLine, "new demo.interflow.ArrayDead.Element") &&
              orderedEffectfulElementsText[2] == "ret Int 12",
          "interflow did not preserve ordered effects while removing an array")) {
    return code;
  }
  if (int code = expect(
          scalanative::nir::bodyToText(nestedAllocationElements->body).size() == 2 &&
              scalanative::nir::bodyToText(nestedAllocationElements->body).back() ==
                  "ret Int 10",
          "interflow did not remove an array of discardable fresh allocations")) {
    return code;
  }
  const std::vector<std::string> effectfulAllocationElementText =
      scalanative::nir::bodyToText(effectfulAllocationElement->body);
  return expect(
      effectfulAllocationElementText.size() == 3 &&
          contains(effectfulAllocationElementText[1],
                   "new demo.interflow.ArrayDead.EffectElement") &&
          !contains(effectfulAllocationElementText[1], "new Array [ Object ]") &&
          effectfulAllocationElementText[2] == "ret Int 11",
      "interflow did not preserve an effectful element while removing its array");
}

int smokeInterflowUnassignedVarPromotion() {
  const scalanative::support::SourceSpan noSpan =
      scalanative::support::SourceSpan::none();

  const auto intValue = [&](int value) {
    return scalanative::nir::literalValue(std::to_string(value), "Int", noSpan);
  };
  const auto effectIntCall = [&] {
    return scalanative::nir::callValue(
        scalanative::nir::localValue("demo.interflow.VarCleanup.effectInt", noSpan), {},
        noSpan);
  };

  scalanative::nir::FunctionBodyBuilder foldedBody;
  (void)foldedBody.addVar("marker", "Int",
                          scalanative::nir::literalValue("7", "Int", noSpan), noSpan);
  (void)foldedBody.addEval(
      scalanative::nir::whileValue(
          scalanative::nir::literalValue("false", "Boolean", noSpan),
          scalanative::nir::assignValue(
              scalanative::nir::localValue("marker", noSpan),
              scalanative::nir::literalValue("99", "Int", noSpan), noSpan),
          noSpan),
      noSpan);
  (void)foldedBody.addReturn("Int", scalanative::nir::localValue("marker", noSpan),
                             noSpan);

  scalanative::nir::FunctionBodyBuilder assignedBody;
  (void)assignedBody.addVar("marker", "Int",
                            scalanative::nir::literalValue("7", "Int", noSpan), noSpan);
  (void)assignedBody.addEval(
      scalanative::nir::assignValue(scalanative::nir::localValue("marker", noSpan),
                                    scalanative::nir::literalValue("9", "Int", noSpan),
                                    noSpan),
      noSpan);
  (void)assignedBody.addReturn("Int", scalanative::nir::localValue("marker", noSpan),
                               noSpan);

  scalanative::nir::FunctionBodyBuilder blockLocalFoldedBody;
  (void)blockLocalFoldedBody.addReturn(
      "Int",
      scalanative::nir::blockValue(
          {scalanative::nir::localVarValue("inner", "Int", intValue(40), noSpan),
           scalanative::nir::binaryValue("+",
                                         scalanative::nir::localValue("inner", noSpan),
                                         intValue(2), noSpan)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder blockLocalAssignedBody;
  (void)blockLocalAssignedBody.addReturn(
      "Int",
      scalanative::nir::blockValue(
          {scalanative::nir::localVarValue("inner", "Int", intValue(40), noSpan),
           scalanative::nir::assignValue(scalanative::nir::localValue("inner", noSpan),
                                         intValue(9), noSpan),
           scalanative::nir::binaryValue("+",
                                         scalanative::nir::localValue("inner", noSpan),
                                         intValue(2), noSpan)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder shadowedTopLevelBody;
  (void)shadowedTopLevelBody.addVar("marker", "Int", intValue(7), noSpan);
  (void)shadowedTopLevelBody.addEval(
      scalanative::nir::blockValue(
          {scalanative::nir::localVarValue("marker", "Int", intValue(1), noSpan),
           scalanative::nir::assignValue(scalanative::nir::localValue("marker", noSpan),
                                         intValue(9), noSpan),
           scalanative::nir::localValue("marker", noSpan)},
          noSpan),
      noSpan);
  (void)shadowedTopLevelBody.addReturn(
      "Int", scalanative::nir::localValue("marker", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder effectOnlyLetBody;
  (void)effectOnlyLetBody.addReturn(
      "Int",
      scalanative::nir::blockValue(
          {scalanative::nir::localLetValue("unused", "Int", effectIntCall(), noSpan),
           intValue(5)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder finalEffectOnlyBindingBody;
  (void)finalEffectOnlyBindingBody.addReturn(
      "Unit",
      scalanative::nir::blockValue(
          {scalanative::nir::localLetValue("unused", "Int", effectIntCall(), noSpan)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder discardedEffectOnlyVarBody;
  (void)discardedEffectOnlyVarBody.addEval(
      scalanative::nir::blockValue(
          {scalanative::nir::localVarValue("unused", "Int", effectIntCall(), noSpan)},
          noSpan),
      noSpan);
  (void)discardedEffectOnlyVarBody.addReturn("Int", intValue(6), noSpan);

  scalanative::nir::FunctionBodyBuilder topLevelEffectOnlyLetBody;
  (void)topLevelEffectOnlyLetBody.addLet("unused", "Int", effectIntCall(), noSpan);
  (void)topLevelEffectOnlyLetBody.addReturn("Int", intValue(7), noSpan);

  scalanative::nir::FunctionBodyBuilder topLevelEffectOnlyVarBody;
  (void)topLevelEffectOnlyVarBody.addVar("unused", "Int", effectIntCall(), noSpan);
  (void)topLevelEffectOnlyVarBody.addReturn("Int", intValue(8), noSpan);

  scalanative::nir::FunctionBodyBuilder discardedEffectOnlyUnaryBody;
  (void)discardedEffectOnlyUnaryBody.addEval(
      scalanative::nir::unaryValue("-", effectIntCall(), noSpan), noSpan);
  (void)discardedEffectOnlyUnaryBody.addReturn("Int", intValue(9), noSpan);

  scalanative::nir::FunctionBodyBuilder discardedEffectOnlyBoxBody;
  (void)discardedEffectOnlyBoxBody.addEval(
      scalanative::nir::boxValue("Int", effectIntCall(), noSpan), noSpan);
  (void)discardedEffectOnlyBoxBody.addReturn("Int", intValue(10), noSpan);

  scalanative::nir::FunctionBodyBuilder discardedEffectOnlyTypeTestBody;
  (void)discardedEffectOnlyTypeTestBody.addEval(
      scalanative::nir::isInstanceOfValue(
          "Int", scalanative::nir::boxValue("Int", effectIntCall(), noSpan), noSpan),
      noSpan);
  (void)discardedEffectOnlyTypeTestBody.addReturn("Int", intValue(11), noSpan);

  scalanative::nir::FunctionBodyBuilder discardedEffectOnlyComparisonBody;
  (void)discardedEffectOnlyComparisonBody.addEval(
      scalanative::nir::binaryValue("==", effectIntCall(), intValue(0), noSpan),
      noSpan);
  (void)discardedEffectOnlyComparisonBody.addReturn("Int", intValue(12), noSpan);

  scalanative::nir::FunctionBodyBuilder nestedEffectOnlyComparisonBody;
  (void)nestedEffectOnlyComparisonBody.addReturn(
      "Int",
      scalanative::nir::blockValue(
          {scalanative::nir::binaryValue("<", effectIntCall(), effectIntCall(), noSpan),
           intValue(13)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder discardedArithmeticGuardBody;
  (void)discardedArithmeticGuardBody.addEval(
      scalanative::nir::binaryValue("+", effectIntCall(), intValue(1), noSpan), noSpan);
  (void)discardedArithmeticGuardBody.addReturn("Int", intValue(14), noSpan);

  scalanative::nir::FunctionBodyBuilder discardedZoneScopedBody;
  (void)discardedZoneScopedBody.addEval(
      scalanative::nir::zoneScopedValue(
          scalanative::nir::binaryValue("==", effectIntCall(), intValue(0), noSpan),
          noSpan),
      noSpan);
  (void)discardedZoneScopedBody.addReturn("Int", intValue(15), noSpan);

  scalanative::nir::FunctionBodyBuilder blockFinalLiteralInlineBody;
  (void)blockFinalLiteralInlineBody.addReturn(
      "Int",
      scalanative::nir::blockValue(
          {scalanative::nir::localLetValue(
               "answer", "Int",
               scalanative::nir::binaryValue("+", intValue(40), intValue(2), noSpan),
               noSpan),
           effectIntCall(), scalanative::nir::localValue("answer", noSpan)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder blockPromotedLiteralCallInlineBody;
  (void)blockPromotedLiteralCallInlineBody.addReturn(
      "Int",
      scalanative::nir::blockValue(
          {scalanative::nir::localVarValue("answer", "Int", intValue(42), noSpan),
           effectIntCall(),
           scalanative::nir::callValue(
               scalanative::nir::localValue("demo.interflow.VarCleanup.consumeInt",
                                            noSpan),
               {scalanative::nir::localValue("answer", noSpan)}, noSpan),
           intValue(16)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder blockFinalMutableGuardBody;
  (void)blockFinalMutableGuardBody.addVar("marker", "Int", intValue(7), noSpan);
  (void)blockFinalMutableGuardBody.addReturn(
      "Int",
      scalanative::nir::blockValue(
          {scalanative::nir::localLetValue(
               "seen", "Int", scalanative::nir::localValue("marker", noSpan), noSpan),
           scalanative::nir::assignValue(scalanative::nir::localValue("marker", noSpan),
                                         intValue(9), noSpan),
           scalanative::nir::localValue("seen", noSpan)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder topLevelPureLetInlineBody;
  (void)topLevelPureLetInlineBody.addParameter("value", "Int", noSpan);
  (void)topLevelPureLetInlineBody.addLet(
      "sum", "Int",
      scalanative::nir::binaryValue("+", scalanative::nir::localValue("value", noSpan),
                                    intValue(1), noSpan),
      noSpan);
  (void)topLevelPureLetInlineBody.addReturn(
      "Int",
      scalanative::nir::binaryValue("+", scalanative::nir::localValue("sum", noSpan),
                                    intValue(2), noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder topLevelPromotedLiteralInlineBody;
  (void)topLevelPromotedLiteralInlineBody.addVar("answer", "Int", intValue(42), noSpan);
  (void)topLevelPromotedLiteralInlineBody.addEval(effectIntCall(), noSpan);
  (void)topLevelPromotedLiteralInlineBody.addReturn(
      "Int", scalanative::nir::localValue("answer", noSpan), noSpan);

  scalanative::nir::FunctionBodyBuilder topLevelCaptureGuardBody;
  (void)topLevelCaptureGuardBody.addParameter("value", "Int", noSpan);
  (void)topLevelCaptureGuardBody.addLet(
      "sum", "Int",
      scalanative::nir::binaryValue("+", scalanative::nir::localValue("value", noSpan),
                                    intValue(1), noSpan),
      noSpan);
  (void)topLevelCaptureGuardBody.addReturn(
      "Int",
      scalanative::nir::blockValue(
          {scalanative::nir::localVarValue("value", "Int", intValue(40), noSpan),
           scalanative::nir::assignValue(scalanative::nir::localValue("value", noSpan),
                                         intValue(41), noSpan),
           scalanative::nir::binaryValue(
               "+", scalanative::nir::localValue("sum", noSpan),
               scalanative::nir::localValue("value", noSpan), noSpan)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder nestedFinalBlockBody;
  (void)nestedFinalBlockBody.addReturn(
      "Int",
      scalanative::nir::blockValue(
          {effectIntCall(),
           scalanative::nir::blockValue({effectIntCall(), intValue(9)}, noSpan)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder nestedEffectOnlyBlockBody;
  (void)nestedEffectOnlyBlockBody.addReturn(
      "Int",
      scalanative::nir::blockValue(
          {scalanative::nir::blockValue(
               {effectIntCall(),
                scalanative::nir::blockValue({effectIntCall(), intValue(9)}, noSpan),
                intValue(10)},
               noSpan),
           intValue(11)},
          noSpan),
      noSpan);

  scalanative::nir::FunctionBodyBuilder runtimeMainBody;
  (void)runtimeMainBody.addReturn(
      "Int", scalanative::nir::literalValue("0", "Int", noSpan), noSpan);

  scalanative::nir::Module module;
  module.name = "demo.interflow";
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.VarCleanup.folded", "()Int",
                                std::move(foldedBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.VarCleanup.assigned", "()Int",
                                std::move(assignedBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.VarCleanup.blockLocalFolded", "()Int",
                                std::move(blockLocalFoldedBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.VarCleanup.blockLocalAssigned", "()Int",
                                std::move(blockLocalAssignedBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.VarCleanup.shadowedTopLevel", "()Int",
                                std::move(shadowedTopLevelBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.VarCleanup.effectOnlyLet", "()Int",
                                std::move(effectOnlyLetBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.VarCleanup.finalEffectOnlyBinding",
                                "()Unit", std::move(finalEffectOnlyBindingBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.VarCleanup.discardedEffectOnlyVar",
                                "()Int", std::move(discardedEffectOnlyVarBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.VarCleanup.topLevelEffectOnlyLet",
                                "()Int", std::move(topLevelEffectOnlyLetBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.VarCleanup.topLevelEffectOnlyVar",
                                "()Int", std::move(topLevelEffectOnlyVarBody).build(),
                                noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.VarCleanup.discardedEffectOnlyUnary", "()Int",
       std::move(discardedEffectOnlyUnaryBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.VarCleanup.discardedEffectOnlyBox",
                                "()Int", std::move(discardedEffectOnlyBoxBody).build(),
                                noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.VarCleanup.discardedEffectOnlyTypeTest", "()Int",
       std::move(discardedEffectOnlyTypeTestBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.VarCleanup.discardedEffectOnlyComparison", "()Int",
       std::move(discardedEffectOnlyComparisonBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.VarCleanup.nestedEffectOnlyComparison", "()Int",
       std::move(nestedEffectOnlyComparisonBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.VarCleanup.discardedArithmeticGuard", "()Int",
       std::move(discardedArithmeticGuardBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.VarCleanup.discardedZoneScoped",
                                "()Int", std::move(discardedZoneScopedBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.VarCleanup.blockFinalLiteralInline",
                                "()Int", std::move(blockFinalLiteralInlineBody).build(),
                                noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.VarCleanup.blockPromotedLiteralCallInline", "()Int",
       std::move(blockPromotedLiteralCallInlineBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.VarCleanup.blockFinalMutableGuard",
                                "()Int", std::move(blockFinalMutableGuardBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.VarCleanup.topLevelPureLetInline",
                                "(Int)Int",
                                std::move(topLevelPureLetInlineBody).build(), noSpan});
  module.definitions.push_back(
      {scalanative::nir::DefinitionKind::FunctionDef,
       "demo.interflow.VarCleanup.topLevelPromotedLiteralInline", "()Int",
       std::move(topLevelPromotedLiteralInlineBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.VarCleanup.topLevelCaptureGuard",
                                "(Int)Int", std::move(topLevelCaptureGuardBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.VarCleanup.nestedFinalBlock", "()Int",
                                std::move(nestedFinalBlockBody).build(), noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "demo.interflow.VarCleanup.nestedEffectOnlyBlock",
                                "()Int", std::move(nestedEffectOnlyBlockBody).build(),
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                "demo.interflow.VarCleanup.effectInt",
                                "()Int",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDecl,
                                "demo.interflow.VarCleanup.consumeInt",
                                "(Int)Int",
                                {},
                                noSpan});
  module.definitions.push_back({scalanative::nir::DefinitionKind::FunctionDef,
                                "scala.scalanative.runtime.main", "()Int",
                                std::move(runtimeMainBody).build(), noSpan});

  scalanative::tools::linker::LinkedProgram program;
  program.modules.push_back(std::move(module));
  program.roots.push_back("scala.scalanative.runtime.main");
  program.roots.push_back("demo.interflow.VarCleanup.folded");
  program.roots.push_back("demo.interflow.VarCleanup.assigned");
  program.roots.push_back("demo.interflow.VarCleanup.blockLocalFolded");
  program.roots.push_back("demo.interflow.VarCleanup.blockLocalAssigned");
  program.roots.push_back("demo.interflow.VarCleanup.shadowedTopLevel");
  program.roots.push_back("demo.interflow.VarCleanup.effectOnlyLet");
  program.roots.push_back("demo.interflow.VarCleanup.finalEffectOnlyBinding");
  program.roots.push_back("demo.interflow.VarCleanup.discardedEffectOnlyVar");
  program.roots.push_back("demo.interflow.VarCleanup.topLevelEffectOnlyLet");
  program.roots.push_back("demo.interflow.VarCleanup.topLevelEffectOnlyVar");
  program.roots.push_back("demo.interflow.VarCleanup.discardedEffectOnlyUnary");
  program.roots.push_back("demo.interflow.VarCleanup.discardedEffectOnlyBox");
  program.roots.push_back("demo.interflow.VarCleanup.discardedEffectOnlyTypeTest");
  program.roots.push_back("demo.interflow.VarCleanup.discardedEffectOnlyComparison");
  program.roots.push_back("demo.interflow.VarCleanup.nestedEffectOnlyComparison");
  program.roots.push_back("demo.interflow.VarCleanup.discardedArithmeticGuard");
  program.roots.push_back("demo.interflow.VarCleanup.discardedZoneScoped");
  program.roots.push_back("demo.interflow.VarCleanup.blockFinalLiteralInline");
  program.roots.push_back("demo.interflow.VarCleanup.blockPromotedLiteralCallInline");
  program.roots.push_back("demo.interflow.VarCleanup.blockFinalMutableGuard");
  program.roots.push_back("demo.interflow.VarCleanup.topLevelPureLetInline");
  program.roots.push_back("demo.interflow.VarCleanup.topLevelPromotedLiteralInline");
  program.roots.push_back("demo.interflow.VarCleanup.topLevelCaptureGuard");
  program.roots.push_back("demo.interflow.VarCleanup.nestedFinalBlock");
  program.roots.push_back("demo.interflow.VarCleanup.nestedEffectOnlyBlock");
  program.reachableGlobals.push_back("scala.scalanative.runtime.main");
  program.reachableGlobals.push_back("demo.interflow.VarCleanup.folded");
  program.reachableGlobals.push_back("demo.interflow.VarCleanup.assigned");
  program.reachableGlobals.push_back("demo.interflow.VarCleanup.blockLocalFolded");
  program.reachableGlobals.push_back("demo.interflow.VarCleanup.blockLocalAssigned");
  program.reachableGlobals.push_back("demo.interflow.VarCleanup.shadowedTopLevel");
  program.reachableGlobals.push_back("demo.interflow.VarCleanup.effectOnlyLet");
  program.reachableGlobals.push_back(
      "demo.interflow.VarCleanup.finalEffectOnlyBinding");
  program.reachableGlobals.push_back(
      "demo.interflow.VarCleanup.discardedEffectOnlyVar");
  program.reachableGlobals.push_back("demo.interflow.VarCleanup.topLevelEffectOnlyLet");
  program.reachableGlobals.push_back("demo.interflow.VarCleanup.topLevelEffectOnlyVar");
  program.reachableGlobals.push_back(
      "demo.interflow.VarCleanup.discardedEffectOnlyUnary");
  program.reachableGlobals.push_back(
      "demo.interflow.VarCleanup.discardedEffectOnlyBox");
  program.reachableGlobals.push_back(
      "demo.interflow.VarCleanup.discardedEffectOnlyTypeTest");
  program.reachableGlobals.push_back(
      "demo.interflow.VarCleanup.discardedEffectOnlyComparison");
  program.reachableGlobals.push_back(
      "demo.interflow.VarCleanup.nestedEffectOnlyComparison");
  program.reachableGlobals.push_back(
      "demo.interflow.VarCleanup.discardedArithmeticGuard");
  program.reachableGlobals.push_back("demo.interflow.VarCleanup.discardedZoneScoped");
  program.reachableGlobals.push_back(
      "demo.interflow.VarCleanup.blockFinalLiteralInline");
  program.reachableGlobals.push_back(
      "demo.interflow.VarCleanup.blockPromotedLiteralCallInline");
  program.reachableGlobals.push_back(
      "demo.interflow.VarCleanup.blockFinalMutableGuard");
  program.reachableGlobals.push_back("demo.interflow.VarCleanup.topLevelPureLetInline");
  program.reachableGlobals.push_back(
      "demo.interflow.VarCleanup.topLevelPromotedLiteralInline");
  program.reachableGlobals.push_back("demo.interflow.VarCleanup.topLevelCaptureGuard");
  program.reachableGlobals.push_back("demo.interflow.VarCleanup.nestedFinalBlock");
  program.reachableGlobals.push_back("demo.interflow.VarCleanup.nestedEffectOnlyBlock");
  program.reachableGlobals.push_back("demo.interflow.VarCleanup.effectInt");
  program.reachableGlobals.push_back("demo.interflow.VarCleanup.consumeInt");

  scalanative::tools::interflow::InterflowOptimizer optimizer;
  scalanative::tools::interflow::InterflowResult result =
      optimizer.optimize(std::move(program));
  if (int code = expect(result.ok, "interflow rejected var cleanup program")) {
    return code;
  }

  const scalanative::nir::Module& optimizedModule = result.program.modules.front();
  const scalanative::nir::Definition* folded =
      findDefinition(optimizedModule, "demo.interflow.VarCleanup.folded");
  const scalanative::nir::Definition* assigned =
      findDefinition(optimizedModule, "demo.interflow.VarCleanup.assigned");
  const scalanative::nir::Definition* blockLocalFolded =
      findDefinition(optimizedModule, "demo.interflow.VarCleanup.blockLocalFolded");
  const scalanative::nir::Definition* blockLocalAssigned =
      findDefinition(optimizedModule, "demo.interflow.VarCleanup.blockLocalAssigned");
  const scalanative::nir::Definition* shadowedTopLevel =
      findDefinition(optimizedModule, "demo.interflow.VarCleanup.shadowedTopLevel");
  const scalanative::nir::Definition* effectOnlyLet =
      findDefinition(optimizedModule, "demo.interflow.VarCleanup.effectOnlyLet");
  const scalanative::nir::Definition* finalEffectOnlyBinding = findDefinition(
      optimizedModule, "demo.interflow.VarCleanup.finalEffectOnlyBinding");
  const scalanative::nir::Definition* discardedEffectOnlyVar = findDefinition(
      optimizedModule, "demo.interflow.VarCleanup.discardedEffectOnlyVar");
  const scalanative::nir::Definition* topLevelEffectOnlyLet = findDefinition(
      optimizedModule, "demo.interflow.VarCleanup.topLevelEffectOnlyLet");
  const scalanative::nir::Definition* topLevelEffectOnlyVar = findDefinition(
      optimizedModule, "demo.interflow.VarCleanup.topLevelEffectOnlyVar");
  const scalanative::nir::Definition* discardedEffectOnlyUnary = findDefinition(
      optimizedModule, "demo.interflow.VarCleanup.discardedEffectOnlyUnary");
  const scalanative::nir::Definition* discardedEffectOnlyBox = findDefinition(
      optimizedModule, "demo.interflow.VarCleanup.discardedEffectOnlyBox");
  const scalanative::nir::Definition* discardedEffectOnlyTypeTest = findDefinition(
      optimizedModule, "demo.interflow.VarCleanup.discardedEffectOnlyTypeTest");
  const scalanative::nir::Definition* discardedEffectOnlyComparison = findDefinition(
      optimizedModule, "demo.interflow.VarCleanup.discardedEffectOnlyComparison");
  const scalanative::nir::Definition* nestedEffectOnlyComparison = findDefinition(
      optimizedModule, "demo.interflow.VarCleanup.nestedEffectOnlyComparison");
  const scalanative::nir::Definition* discardedArithmeticGuard = findDefinition(
      optimizedModule, "demo.interflow.VarCleanup.discardedArithmeticGuard");
  const scalanative::nir::Definition* discardedZoneScoped =
      findDefinition(optimizedModule, "demo.interflow.VarCleanup.discardedZoneScoped");
  const scalanative::nir::Definition* blockFinalLiteralInline = findDefinition(
      optimizedModule, "demo.interflow.VarCleanup.blockFinalLiteralInline");
  const scalanative::nir::Definition* blockPromotedLiteralCallInline = findDefinition(
      optimizedModule, "demo.interflow.VarCleanup.blockPromotedLiteralCallInline");
  const scalanative::nir::Definition* blockFinalMutableGuard = findDefinition(
      optimizedModule, "demo.interflow.VarCleanup.blockFinalMutableGuard");
  const scalanative::nir::Definition* topLevelPureLetInline = findDefinition(
      optimizedModule, "demo.interflow.VarCleanup.topLevelPureLetInline");
  const scalanative::nir::Definition* topLevelPromotedLiteralInline = findDefinition(
      optimizedModule, "demo.interflow.VarCleanup.topLevelPromotedLiteralInline");
  const scalanative::nir::Definition* topLevelCaptureGuard =
      findDefinition(optimizedModule, "demo.interflow.VarCleanup.topLevelCaptureGuard");
  const scalanative::nir::Definition* nestedFinalBlock =
      findDefinition(optimizedModule, "demo.interflow.VarCleanup.nestedFinalBlock");
  const scalanative::nir::Definition* nestedEffectOnlyBlock = findDefinition(
      optimizedModule, "demo.interflow.VarCleanup.nestedEffectOnlyBlock");
  if (int code = expect(
          folded != nullptr && assigned != nullptr && blockLocalFolded != nullptr &&
              blockLocalAssigned != nullptr && shadowedTopLevel != nullptr &&
              effectOnlyLet != nullptr && finalEffectOnlyBinding != nullptr &&
              discardedEffectOnlyVar != nullptr && topLevelEffectOnlyLet != nullptr &&
              topLevelEffectOnlyVar != nullptr && discardedEffectOnlyUnary != nullptr &&
              discardedEffectOnlyBox != nullptr &&
              discardedEffectOnlyTypeTest != nullptr &&
              discardedEffectOnlyComparison != nullptr &&
              nestedEffectOnlyComparison != nullptr &&
              discardedArithmeticGuard != nullptr && discardedZoneScoped != nullptr &&
              blockFinalLiteralInline != nullptr &&
              blockPromotedLiteralCallInline != nullptr &&
              blockFinalMutableGuard != nullptr && topLevelPureLetInline != nullptr &&
              topLevelPromotedLiteralInline != nullptr &&
              topLevelCaptureGuard != nullptr && nestedFinalBlock != nullptr &&
              nestedEffectOnlyBlock != nullptr,
          "interflow removed var cleanup smoke roots")) {
    return code;
  }
  if (int code =
          expect(scalanative::nir::bodyToText(folded->body).size() == 2 &&
                     scalanative::nir::bodyToText(folded->body).back() == "ret Int 7",
                 "interflow did not promote and fold unassigned var")) {
    return code;
  }

  const std::vector<std::string> assignedText =
      scalanative::nir::bodyToText(assigned->body);
  if (int code = expect(assignedText.size() == 4,
                        "interflow unexpectedly simplified assigned var body")) {
    return code;
  }
  if (int code = expect(assignedText[1] == "var %marker : Int = 7" &&
                            assignedText[2] == "eval assign %marker = 9" &&
                            assignedText[3] == "ret Int %marker",
                        "interflow promoted a var with a remaining assignment")) {
    return code;
  }

  if (int code =
          expect(scalanative::nir::bodyToText(blockLocalFolded->body).size() == 2 &&
                     scalanative::nir::bodyToText(blockLocalFolded->body).back() ==
                         "ret Int 42",
                 "interflow did not promote and fold unassigned block-local var")) {
    return code;
  }

  const std::vector<std::string> blockLocalAssignedText =
      scalanative::nir::bodyToText(blockLocalAssigned->body);
  if (int code =
          expect(contains(blockLocalAssignedText.back(), "var %inner : Int = 40") &&
                     contains(blockLocalAssignedText.back(), "assign %inner = 9") &&
                     contains(blockLocalAssignedText.back(), "%inner + 2") &&
                     !contains(blockLocalAssignedText.back(), "ret Int 42"),
                 "interflow promoted an assigned block-local var")) {
    return code;
  }

  const std::vector<std::string> shadowedTopLevelText =
      scalanative::nir::bodyToText(shadowedTopLevel->body);
  if (int code =
          expect(shadowedTopLevelText.size() == 3 &&
                     contains(shadowedTopLevelText[1], "var %marker : Int = 1") &&
                     contains(shadowedTopLevelText[1], "assign %marker = 9") &&
                     shadowedTopLevelText[2] == "ret Int 7",
                 "interflow let a shadowed nested var block top-level var promotion")) {
    return code;
  }

  const std::vector<std::string> effectOnlyLetText =
      scalanative::nir::bodyToText(effectOnlyLet->body);
  if (int code =
          expect(effectOnlyLetText.size() == 2 &&
                     contains(effectOnlyLetText.back(),
                              "call %demo.interflow.VarCleanup.effectInt()") &&
                     contains(effectOnlyLetText.back(), "5") &&
                     !contains(effectOnlyLetText.back(), "let %unused"),
                 "interflow did not unwrap unused effect-only block-local let")) {
    return code;
  }

  const std::vector<std::string> finalEffectOnlyBindingText =
      scalanative::nir::bodyToText(finalEffectOnlyBinding->body);
  if (int code = expect(
          finalEffectOnlyBindingText.size() == 2 &&
              finalEffectOnlyBindingText.back() ==
                  "ret Unit block(call %demo.interflow.VarCleanup.effectInt(); unit)",
          "interflow did not normalize a final block-local binding to Unit")) {
    return code;
  }

  const std::vector<std::string> discardedEffectOnlyVarText =
      scalanative::nir::bodyToText(discardedEffectOnlyVar->body);
  if (int code =
          expect(discardedEffectOnlyVarText.size() == 3 &&
                     discardedEffectOnlyVarText[1] ==
                         "eval call %demo.interflow.VarCleanup.effectInt()" &&
                     discardedEffectOnlyVarText[2] == "ret Int 6",
                 "interflow did not unwrap discarded effect-only block-local var")) {
    return code;
  }

  const std::vector<std::string> topLevelEffectOnlyLetText =
      scalanative::nir::bodyToText(topLevelEffectOnlyLet->body);
  if (int code = expect(topLevelEffectOnlyLetText.size() == 3 &&
                            topLevelEffectOnlyLetText[1] ==
                                "eval call %demo.interflow.VarCleanup.effectInt()" &&
                            topLevelEffectOnlyLetText[2] == "ret Int 7",
                        "interflow did not demote unused effect-only top-level let")) {
    return code;
  }

  const std::vector<std::string> topLevelEffectOnlyVarText =
      scalanative::nir::bodyToText(topLevelEffectOnlyVar->body);
  if (int code = expect(topLevelEffectOnlyVarText.size() == 3 &&
                            topLevelEffectOnlyVarText[1] ==
                                "eval call %demo.interflow.VarCleanup.effectInt()" &&
                            topLevelEffectOnlyVarText[2] == "ret Int 8",
                        "interflow did not demote unused effect-only top-level var")) {
    return code;
  }

  const std::vector<std::string> discardedEffectOnlyUnaryText =
      scalanative::nir::bodyToText(discardedEffectOnlyUnary->body);
  if (int code = expect(discardedEffectOnlyUnaryText.size() == 3 &&
                            discardedEffectOnlyUnaryText[1] ==
                                "eval call %demo.interflow.VarCleanup.effectInt()" &&
                            discardedEffectOnlyUnaryText[2] == "ret Int 9",
                        "interflow did not unwrap discarded effect-only unary")) {
    return code;
  }

  const std::vector<std::string> discardedEffectOnlyBoxText =
      scalanative::nir::bodyToText(discardedEffectOnlyBox->body);
  if (int code = expect(discardedEffectOnlyBoxText.size() == 3 &&
                            discardedEffectOnlyBoxText[1] ==
                                "eval call %demo.interflow.VarCleanup.effectInt()" &&
                            discardedEffectOnlyBoxText[2] == "ret Int 10",
                        "interflow did not unwrap discarded effect-only box")) {
    return code;
  }

  const std::vector<std::string> discardedEffectOnlyTypeTestText =
      scalanative::nir::bodyToText(discardedEffectOnlyTypeTest->body);
  if (int code = expect(discardedEffectOnlyTypeTestText.size() == 3 &&
                            discardedEffectOnlyTypeTestText[1] ==
                                "eval call %demo.interflow.VarCleanup.effectInt()" &&
                            discardedEffectOnlyTypeTestText[2] == "ret Int 11",
                        "interflow did not unwrap discarded effect-only type test")) {
    return code;
  }

  const std::vector<std::string> discardedEffectOnlyComparisonText =
      scalanative::nir::bodyToText(discardedEffectOnlyComparison->body);
  if (int code = expect(discardedEffectOnlyComparisonText.size() == 3 &&
                            discardedEffectOnlyComparisonText[1] ==
                                "eval call %demo.interflow.VarCleanup.effectInt()" &&
                            discardedEffectOnlyComparisonText[2] == "ret Int 12",
                        "interflow did not unwrap discarded effect-only comparison")) {
    return code;
  }

  const std::vector<std::string> nestedEffectOnlyComparisonText =
      scalanative::nir::bodyToText(nestedEffectOnlyComparison->body);
  if (int code =
          expect(nestedEffectOnlyComparisonText.size() == 2 &&
                     contains(nestedEffectOnlyComparisonText.back(),
                              "block(call %demo.interflow.VarCleanup.effectInt(); "
                              "call %demo.interflow.VarCleanup.effectInt(); 13)") &&
                     !contains(nestedEffectOnlyComparisonText.back(), "<"),
                 "interflow did not unwrap nested discarded effect-only comparison")) {
    return code;
  }

  const std::vector<std::string> discardedArithmeticGuardText =
      scalanative::nir::bodyToText(discardedArithmeticGuard->body);
  if (int code = expect(discardedArithmeticGuardText.size() == 3 &&
                            contains(discardedArithmeticGuardText[1], "eval (call ") &&
                            contains(discardedArithmeticGuardText[1],
                                     "%demo.interflow.VarCleanup.effectInt() + 1") &&
                            discardedArithmeticGuardText[2] == "ret Int 14",
                        "interflow unwrapped a discarded arithmetic result")) {
    return code;
  }

  const std::vector<std::string> discardedZoneScopedText =
      scalanative::nir::bodyToText(discardedZoneScoped->body);
  if (int code = expect(discardedZoneScopedText.size() == 3 &&
                            discardedZoneScopedText[1] ==
                                "eval zone-scoped(block(call "
                                "%demo.interflow.VarCleanup.effectInt(); unit))" &&
                            discardedZoneScopedText[2] == "ret Int 15",
                        "interflow did not trim discarded zone-scoped body result")) {
    return code;
  }

  const std::vector<std::string> blockFinalLiteralInlineText =
      scalanative::nir::bodyToText(blockFinalLiteralInline->body);
  if (int code =
          expect(blockFinalLiteralInlineText.size() == 2 &&
                     blockFinalLiteralInlineText.back() ==
                         "ret Int block(call "
                         "%demo.interflow.VarCleanup.effectInt(); 42)",
                 "interflow did not inline literal-like block-local let into final "
                 "result")) {
    return code;
  }

  const std::vector<std::string> blockPromotedLiteralCallInlineText =
      scalanative::nir::bodyToText(blockPromotedLiteralCallInline->body);
  if (int code =
          expect(blockPromotedLiteralCallInlineText.size() == 2 &&
                     blockPromotedLiteralCallInlineText.back() ==
                         "ret Int block(call %demo.interflow.VarCleanup.effectInt(); "
                         "call %demo.interflow.VarCleanup.consumeInt(42); 16)",
                 "interflow did not inline promoted literal-like block-local let into "
                 "its later effectful use")) {
    return code;
  }

  const std::vector<std::string> blockFinalMutableGuardText =
      scalanative::nir::bodyToText(blockFinalMutableGuard->body);
  if (int code = expect(
          blockFinalMutableGuardText.size() == 3 &&
              blockFinalMutableGuardText[1] == "var %marker : Int = 7" &&
              contains(blockFinalMutableGuardText[2], "let %seen : Int = %marker") &&
              contains(blockFinalMutableGuardText[2], "assign %marker = 9") &&
              contains(blockFinalMutableGuardText[2], "%seen"),
          "interflow inlined a block-local let that depends on mutable state")) {
    return code;
  }

  const std::vector<std::string> topLevelPureLetInlineText =
      scalanative::nir::bodyToText(topLevelPureLetInline->body);
  if (int code =
          expect(topLevelPureLetInlineText.size() == 3 &&
                     topLevelPureLetInlineText[1] == "param %value : Int" &&
                     topLevelPureLetInlineText[2] == "ret Int ((%value + 1) + 2)",
                 "interflow did not inline adjacent single-use top-level let")) {
    return code;
  }

  const std::vector<std::string> topLevelPromotedLiteralInlineText =
      scalanative::nir::bodyToText(topLevelPromotedLiteralInline->body);
  if (int code =
          expect(topLevelPromotedLiteralInlineText.size() == 3 &&
                     topLevelPromotedLiteralInlineText[1] ==
                         "eval call %demo.interflow.VarCleanup.effectInt()" &&
                     topLevelPromotedLiteralInlineText[2] == "ret Int 42",
                 "interflow did not inline promoted literal-like top-level let across "
                 "an effect")) {
    return code;
  }

  const std::vector<std::string> topLevelCaptureGuardText =
      scalanative::nir::bodyToText(topLevelCaptureGuard->body);
  if (int code = expect(
          topLevelCaptureGuardText.size() == 4 &&
              topLevelCaptureGuardText[1] == "param %value : Int" &&
              topLevelCaptureGuardText[2] == "let %sum : Int = (%value + 1)" &&
              contains(topLevelCaptureGuardText[3], "var %value : Int = 40") &&
              contains(topLevelCaptureGuardText[3], "assign %value = 41") &&
              contains(topLevelCaptureGuardText[3], "(%sum + %value)"),
          "interflow captured a replacement local during top-level let inline")) {
    return code;
  }

  const std::vector<std::string> nestedFinalBlockText =
      scalanative::nir::bodyToText(nestedFinalBlock->body);
  if (int code =
          expect(nestedFinalBlockText.size() == 2 &&
                     contains(nestedFinalBlockText.back(),
                              "block(call %demo.interflow.VarCleanup.effectInt(); "
                              "call %demo.interflow.VarCleanup.effectInt(); 9)") &&
                     !contains(nestedFinalBlockText.back(), "; block("),
                 "interflow did not flatten final nested block value")) {
    return code;
  }

  const std::vector<std::string> nestedEffectOnlyBlockText =
      scalanative::nir::bodyToText(nestedEffectOnlyBlock->body);
  return expect(nestedEffectOnlyBlockText.size() == 2 &&
                    contains(nestedEffectOnlyBlockText.back(),
                             "block(call %demo.interflow.VarCleanup.effectInt(); "
                             "call %demo.interflow.VarCleanup.effectInt(); 11)") &&
                    !contains(nestedEffectOnlyBlockText.back(), "block(block("),
                "interflow did not flatten non-final nested effect-only block");
}

} // namespace

int main() {
  if (int code = smokeBuildConfigurationJson()) {
    return code;
  }
  if (int code = smokeBuildReportJson()) {
    return code;
  }
  if (int code = smokeReferenceGenericsNativeRuntime()) {
    return code;
  }
  if (int code = smokeVarianceAndGenericInheritanceNativeRuntime()) {
    return code;
  }
  if (int code = smokeContextualAbstractionsNativeRuntime()) {
    return code;
  }
  if (int code = smokePrimitiveGenericsNativeRuntime()) {
    return code;
  }
  if (int code = smokeGenericInferenceNativeRuntime()) {
    return code;
  }
  if (int code = smokeExpectedGenericInferenceNativeRuntime()) {
    return code;
  }
  if (int code = smokeByteAndShortNativeRuntime()) {
    return code;
  }
  if (int code = smokeZoneAllocatedBytesNativeRuntime()) {
    return code;
  }
  if (int code = smokeByteBufferStateNativeRuntime()) {
    return code;
  }
  if (int code = smokeBuildDriverEmitNir()) {
    return code;
  }
  if (int code = smokeBuildDriverUsesDistinctOptimizationLevels()) {
    return code;
  }
  if (int code = smokeBuildDriverUsesIncrementalCache()) {
    return code;
  }
  if (int code = smokeBuildDriverCachesNativeObjects()) {
    return code;
  }
  if (int code = smokeBuildDriverEmitsSourceDebugMetadata()) {
    return code;
  }
  if (int code = smokeCodegenRejectsUnsupportedReachableValue()) {
    return code;
  }
  if (int code = smokeTypedClassCastRuntime()) {
    return code;
  }
  if (int code = smokeTypedNullReceiverRuntime()) {
    return code;
  }
  if (int code = smokeTypedStringReceiverRuntime()) {
    return code;
  }
  if (int code = smokeTypedAnyReceiverRuntime()) {
    return code;
  }
  if (int code = smokeNullThrowRuntime()) {
    return code;
  }
  if (int code = smokeUncaughtThrowRuntime()) {
    return code;
  }
  if (int code = smokeTryCatchNirStage()) {
    return code;
  }
  if (int code = smokeOptimizedNativeEquivalence()) {
    return code;
  }
  if (int code = smokeBuildDriverOptimizedFieldReadSource()) {
    return code;
  }
  if (int code = smokeBuildDriverOptimizedInheritedFieldReadSource()) {
    return code;
  }
  if (int code = smokeBuildDriverOptimizedEffectfulUnitSource()) {
    return code;
  }
  if (int code = smokeInterflowPrunesUnusedDeclarations()) {
    return code;
  }
  if (int code = smokeInterflowLiteralBitwiseAndShiftFold()) {
    return code;
  }
  if (int code = smokeInterflowExactStringConcat()) {
    return code;
  }
  if (int code = smokeInterflowAliasReferenceEquality()) {
    return code;
  }
  if (int code = smokeInterflowPureCallInlining()) {
    return code;
  }
  if (int code = smokeInterflowEffectfulSameBranchIf()) {
    return code;
  }
  if (int code = smokeInterflowExactDevirtualization()) {
    return code;
  }
  if (int code = smokeInterflowExactConstructorFieldReads()) {
    return code;
  }
  if (int code = smokeInterflowDiscardableArrayAllocation()) {
    return code;
  }
  if (int code = smokeInterflowUnassignedVarPromotion()) {
    return code;
  }
  return 0;
}
