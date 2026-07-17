#include "scalanative/tools/build/BuildConfig.h"
#include "scalanative/tools/build/BuildDriver.h"
#include "scalanative/tools/build/BuildReport.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

void printUsage(std::ostream& out) {
  out << "Usage: cpp-scalanative [action] [options] <file.scala>\n";
  out << "Actions:\n";
  out << "  --compile      run the full LLVM IR scaffold pipeline (default)\n";
  out << "  --check        run source, NIR emission, and NIR checks\n";
  out << "  --emit-nir     print or write NIR\n";
  out << "  --emit-llvm    print or write LLVM IR\n";
  out << "  --build-object compile LLVM IR to a native object\n";
  out << "  --build-binary compile and link a native binary\n";
  out << "Options:\n";
  out << "  --config <path>       load a versioned JSON build configuration\n";
  out << "  --output <path>       write the selected emitted artifact\n";
  out << "  --target <triple>     select the native target triple\n";
  out << "  --sysroot <path>      select the native target filesystem root\n";
  out << "  --gc <mode>           select the runtime memory mode (default: hybrid)\n";
  out << "  --cache-dir <path>    reuse incremental artifacts from a directory\n";
  out << "  --runtime-lib <path>  add a runtime library input\n";
  out << "  --link-lib <name>     add a native link library input\n";
  out << "  --link-mode <mode>    select default or static native linkage\n";
  out << "  --static              shorthand for --link-mode static\n";
  out << "  --linker <name>       select the default linker or LLD\n";
  out << "  --optimize            use optimization level 2\n";
  out << "  --opt-level <0..3>    select Interflow/Clang optimization level\n";
  out << "  --debug-info          emit LLVM debug information (default)\n";
  out << "  --no-debug-info       omit LLVM debug information\n";
  out << "  --optimization-report <path>\n";
  out << "                       write interflow pass metrics as JSON\n";
  out << "  --build-report <path> write a machine-readable build report as JSON\n";
  out << "       cpp-scalanative --version\n";
}

bool setAction(scalanative::tools::build::BuildOptions& options,
               scalanative::tools::build::BuildAction action, bool& sawAction) {
  if (sawAction) {
    std::cerr << "error: multiple build actions were provided\n";
    printUsage(std::cerr);
    return false;
  }
  options.action = action;
  sawAction = true;
  return true;
}

bool readOptionValue(int& index, int argc, char** argv, std::string& value) {
  if (index + 1 >= argc) {
    std::cerr << "error: missing value for " << argv[index] << '\n';
    printUsage(std::cerr);
    return false;
  }
  ++index;
  value = argv[index];
  return true;
}

bool optionTakesValue(std::string_view option) {
  return option == "--config" || option == "--output" || option == "-o" ||
         option == "--target" || option == "--sysroot" || option == "--gc" ||
         option == "--cache-dir" || option == "--runtime-lib" ||
         option == "--link-lib" || option == "--link-mode" || option == "--linker" ||
         option == "--opt-level" || option == "--optimization-report" ||
         option == "--build-report";
}

std::filesystem::path normalizedAbsolutePath(const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::path absolute = std::filesystem::absolute(path, error);
  return (error ? path : absolute).lexically_normal();
}

bool writeBuildReport(const std::filesystem::path& path, std::string_view contents) {
  const std::filesystem::path parent = path.parent_path();
  if (!parent.empty()) {
    std::error_code directoryError;
    std::filesystem::create_directories(parent, directoryError);
    if (directoryError) {
      std::cerr << "error: could not create build report directory '" << parent.string()
                << "': " << directoryError.message() << '\n';
      return false;
    }
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    std::cerr << "error: could not open build report '" << path.string() << "'\n";
    return false;
  }
  output << contents;
  if (!output) {
    std::cerr << "error: could not write build report '" << path.string() << "'\n";
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  std::filesystem::path configurationPath;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--config") {
      if (i + 1 >= argc) {
        std::cerr << "error: missing value for --config\n";
        printUsage(std::cerr);
        return 2;
      }
      if (!configurationPath.empty()) {
        std::cerr << "error: multiple --config options were provided\n";
        return 2;
      }
      configurationPath = argv[++i];
      continue;
    }
    if (optionTakesValue(arg) && i + 1 < argc) {
      ++i;
    }
  }

  bool sawAction = false;
  bool sawCliSource = false;
  scalanative::tools::build::BuildOptions options;
  std::filesystem::path sourcePath;
  std::filesystem::path buildReportPath;
  if (!configurationPath.empty()) {
    scalanative::tools::build::BuildConfigLoadResult loaded =
        scalanative::tools::build::loadBuildConfiguration(configurationPath);
    if (!loaded.configuration.has_value()) {
      std::cerr << "error: " << loaded.error << '\n';
      return 2;
    }
    options = std::move(loaded.configuration->options);
    sourcePath = std::move(loaded.configuration->sourcePath);
    buildReportPath = std::move(loaded.configuration->buildReportPath);
  }

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--config") {
      ++i;
      continue;
    }
    if (arg == "--help" || arg == "-h") {
      printUsage(std::cout);
      return 0;
    }
    if (arg == "--version") {
      std::cout << "cpp-scalanative 0.1.0\n";
      return 0;
    }
    if (arg == "--compile") {
      if (!setAction(options, scalanative::tools::build::BuildAction::Compile,
                     sawAction)) {
        return 2;
      }
      continue;
    }
    if (arg == "--check") {
      if (!setAction(options, scalanative::tools::build::BuildAction::Check,
                     sawAction)) {
        return 2;
      }
      continue;
    }
    if (arg == "--emit-llvm") {
      if (!setAction(options, scalanative::tools::build::BuildAction::EmitLlvm,
                     sawAction)) {
        return 2;
      }
      continue;
    }
    if (arg == "--emit-nir") {
      if (!setAction(options, scalanative::tools::build::BuildAction::EmitNir,
                     sawAction)) {
        return 2;
      }
      continue;
    }
    if (arg == "--build-object") {
      if (!setAction(options, scalanative::tools::build::BuildAction::BuildObject,
                     sawAction)) {
        return 2;
      }
      continue;
    }
    if (arg == "--build-binary") {
      if (!setAction(options, scalanative::tools::build::BuildAction::BuildBinary,
                     sawAction)) {
        return 2;
      }
      continue;
    }
    if (arg == "--optimize") {
      options.optimize = true;
      options.optimizationLevel = scalanative::tools::build::OptimizationLevel::O2;
      continue;
    }
    if (arg == "--static") {
      options.linkMode = scalanative::tools::build::LinkMode::Static;
      continue;
    }
    if (arg == "--link-mode") {
      std::string value;
      if (!readOptionValue(i, argc, argv, value)) {
        return 2;
      }
      if (value == "default" || value == "dynamic") {
        options.linkMode = scalanative::tools::build::LinkMode::Default;
      } else if (value == "static") {
        options.linkMode = scalanative::tools::build::LinkMode::Static;
      } else {
        std::cerr << "error: --link-mode expects default or static\n";
        printUsage(std::cerr);
        return 2;
      }
      continue;
    }
    if (arg == "--linker") {
      std::string value;
      if (!readOptionValue(i, argc, argv, value)) {
        return 2;
      }
      if (value == "default" || value == "platform") {
        options.linkerMode = scalanative::tools::build::LinkerMode::Default;
      } else if (value == "lld") {
        options.linkerMode = scalanative::tools::build::LinkerMode::Lld;
      } else {
        std::cerr << "error: --linker expects default or lld\n";
        printUsage(std::cerr);
        return 2;
      }
      continue;
    }
    if (arg == "--opt-level") {
      std::string value;
      if (!readOptionValue(i, argc, argv, value)) {
        return 2;
      }
      if (value == "0") {
        options.optimizationLevel = scalanative::tools::build::OptimizationLevel::O0;
        options.optimize = false;
      } else if (value == "1") {
        options.optimizationLevel = scalanative::tools::build::OptimizationLevel::O1;
        options.optimize = true;
      } else if (value == "2") {
        options.optimizationLevel = scalanative::tools::build::OptimizationLevel::O2;
        options.optimize = true;
      } else if (value == "3") {
        options.optimizationLevel = scalanative::tools::build::OptimizationLevel::O3;
        options.optimize = true;
      } else {
        std::cerr << "error: --opt-level expects 0, 1, 2, or 3\n";
        printUsage(std::cerr);
        return 2;
      }
      continue;
    }
    if (arg == "--debug-info") {
      options.debugInfo = true;
      continue;
    }
    if (arg == "--no-debug-info") {
      options.debugInfo = false;
      continue;
    }
    if (arg == "--optimization-report") {
      std::string value;
      if (!readOptionValue(i, argc, argv, value)) {
        return 2;
      }
      options.optimizationReportPath = value;
      continue;
    }
    if (arg == "--build-report") {
      std::string value;
      if (!readOptionValue(i, argc, argv, value)) {
        return 2;
      }
      buildReportPath = value;
      continue;
    }
    if (arg == "--output" || arg == "-o") {
      std::string value;
      if (!readOptionValue(i, argc, argv, value)) {
        return 2;
      }
      options.outputPath = value;
      continue;
    }
    if (arg == "--target") {
      if (!readOptionValue(i, argc, argv, options.targetTriple)) {
        return 2;
      }
      continue;
    }
    if (arg == "--sysroot") {
      std::string value;
      if (!readOptionValue(i, argc, argv, value)) {
        return 2;
      }
      options.sysroot = value;
      continue;
    }
    if (arg == "--gc") {
      if (!readOptionValue(i, argc, argv, options.gcMode)) {
        return 2;
      }
      continue;
    }
    if (arg == "--cache-dir") {
      std::string value;
      if (!readOptionValue(i, argc, argv, value)) {
        return 2;
      }
      options.cacheDirectory = value;
      continue;
    }
    if (arg == "--runtime-lib") {
      std::string value;
      if (!readOptionValue(i, argc, argv, value)) {
        return 2;
      }
      options.runtimeLibraries.push_back(value);
      continue;
    }
    if (arg == "--link-lib") {
      std::string value;
      if (!readOptionValue(i, argc, argv, value)) {
        return 2;
      }
      options.linkLibraries.push_back(value);
      continue;
    }
    if (!arg.empty() && arg.front() == '-') {
      std::cerr << "error: unknown option: " << arg << '\n';
      printUsage(std::cerr);
      return 2;
    }
    if (sawCliSource) {
      std::cerr << "error: multiple source files are not wired yet\n";
      printUsage(std::cerr);
      return 2;
    }
    sourcePath = arg;
    sawCliSource = true;
  }

  if (sourcePath.empty()) {
    printUsage(std::cout);
    return 0;
  }
  if (!buildReportPath.empty() &&
      ((!options.outputPath.empty() &&
        normalizedAbsolutePath(buildReportPath) ==
            normalizedAbsolutePath(options.outputPath)) ||
       normalizedAbsolutePath(buildReportPath) == normalizedAbsolutePath(sourcePath))) {
    std::cerr << "error: --build-report must not overwrite the source or build "
                 "output\n";
    return 2;
  }
  if (!options.configurationPath.empty() &&
      ((!options.outputPath.empty() &&
        normalizedAbsolutePath(options.configurationPath) ==
            normalizedAbsolutePath(options.outputPath)) ||
       (!buildReportPath.empty() && normalizedAbsolutePath(options.configurationPath) ==
                                        normalizedAbsolutePath(buildReportPath)))) {
    std::cerr << "error: build outputs must not overwrite the configuration file\n";
    return 2;
  }

  scalanative::support::DiagnosticEngine diagnostics;
  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildResult result =
      driver.buildFile(sourcePath, options, diagnostics);

  bool reportWritten = true;
  if (!buildReportPath.empty()) {
    const std::filesystem::path normalizedReport =
        normalizedAbsolutePath(buildReportPath);
    const bool conflictsWithArtifact =
        std::any_of(result.producedArtifacts.begin(), result.producedArtifacts.end(),
                    [&](const std::filesystem::path& artifact) {
                      return normalizedReport == normalizedAbsolutePath(artifact);
                    });
    if (conflictsWithArtifact) {
      std::cerr << "error: --build-report must not overwrite a produced artifact\n";
      reportWritten = false;
    } else {
      reportWritten = writeBuildReport(
          buildReportPath, scalanative::tools::build::buildReportJson(result, options));
    }
  }

  if (!result.diagnosticsText.empty()) {
    std::cerr << result.diagnosticsText;
  }

  if (!result.ok || !reportWritten) {
    return 1;
  }

  if (options.action == scalanative::tools::build::BuildAction::EmitNir &&
      options.outputPath.empty()) {
    std::cout << result.nirText;
  }
  if (options.action == scalanative::tools::build::BuildAction::EmitLlvm &&
      options.outputPath.empty()) {
    std::cout << result.llvmIr;
  }
  if (options.action != scalanative::tools::build::BuildAction::EmitNir &&
      options.action != scalanative::tools::build::BuildAction::EmitLlvm) {
    for (const std::string& phase : result.phaseLog) {
      std::cout << phase << '\n';
    }
    std::cout << "cpp-scalanative scaffold build succeeded\n";
  }
  if (!options.outputPath.empty() &&
      (options.action == scalanative::tools::build::BuildAction::EmitNir ||
       options.action == scalanative::tools::build::BuildAction::EmitLlvm)) {
    for (const std::string& phase : result.phaseLog) {
      std::cout << phase << '\n';
    }
  }

  return 0;
}
