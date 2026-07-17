#include "scalanative/tools/build/BuildDriver.h"

#include "scalanative/frontend/CompilerPipeline.h"
#include "scalanative/nir/Nir.h"
#include "scalanative/nscplugin/NirEmitter.h"
#include "scalanative/runtime/RuntimeConfig.h"
#include "scalanative/support/SourceManager.h"
#include "scalanative/tools/build/BuildCache.h"
#include "scalanative/tools/build/Toolchain.h"
#include "scalanative/tools/checker/NirChecker.h"
#include "scalanative/tools/codegen/LlvmCodegen.h"
#include "scalanative/tools/interflow/InterflowOptimizer.h"
#include "scalanative/tools/linker/Linker.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <system_error>
#include <tuple>
#include <utility>

namespace scalanative::tools::build {

namespace {

constexpr std::string_view CompilerVersion = "cpp-scalanative-0.1.0";
constexpr std::string_view NirFormatVersion = "cpp-nir-text-v4";

bool writeTextOutput(const std::filesystem::path& path, std::string_view kind,
                     std::string_view contents, BuildResult& result,
                     support::DiagnosticEngine& diagnostics,
                     bool producedArtifact = true) {
  if (path.empty()) {
    return true;
  }

  const std::filesystem::path parent = path.parent_path();
  if (!parent.empty()) {
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
      diagnostics.error(support::SourceSpan::none(),
                        "could not create output directory '" + parent.string() +
                            "': " + error.message());
      return false;
    }
  }

  std::ofstream out(path, std::ios::binary);
  if (!out) {
    diagnostics.error(support::SourceSpan::none(),
                      "could not open output file '" + path.string() + "'");
    return false;
  }

  out << contents;
  if (!out) {
    diagnostics.error(support::SourceSpan::none(),
                      "could not write " + std::string(kind) + " output to '" +
                          path.string() + "'");
    return false;
  }

  if (producedArtifact) {
    result.producedArtifacts.push_back(path);
  }
  result.phaseLog.push_back("build: wrote " + std::string(kind) + " to " +
                            path.string());
  return true;
}

std::string buildConfigSummary(const BuildOptions& options) {
  std::ostringstream out;
  out << "build: action=" << buildActionName(options.action)
      << ", gc=" << (options.gcMode.empty() ? "hybrid" : options.gcMode)
      << ", opt-level=" << optimizationLevelName(effectiveOptimizationLevel(options))
      << ", link-mode=" << linkModeName(options.linkMode)
      << ", linker=" << linkerModeName(options.linkerMode)
      << ", debug-info=" << (options.debugInfo ? "on" : "off");
  if (!options.targetTriple.empty()) {
    out << ", target=" << options.targetTriple;
  }
  if (!options.sysroot.empty()) {
    out << ", sysroot=" << options.sysroot.string();
  }
  if (!options.cacheDirectory.empty()) {
    out << ", cache=" << options.cacheDirectory.string();
  }
  if (!options.configurationPath.empty()) {
    out << ", config=" << options.configurationPath.string();
  }
  if (!options.outputPath.empty()) {
    out << ", output=" << options.outputPath.string();
  }
  if (!options.optimizationReportPath.empty()) {
    out << ", opt-report=" << options.optimizationReportPath.string();
  }
  return out.str();
}

BuildCacheInputs buildCacheInputs(const support::SourceManager& sources,
                                  support::SourceId source,
                                  CachedArtifactKind artifactKind,
                                  const BuildOptions& options) {
  const support::SourceFile* sourceFile = sources.get(source);
  BuildCacheInputs inputs;
  inputs.sourceHash = sources.contentHash(source);
  inputs.sourcePath = sourceFile == nullptr ? std::string{} : sourceFile->path;
  inputs.artifactKind = artifactKind;
  inputs.compilerVersion = CompilerVersion;
  inputs.nirVersion = NirFormatVersion;
  inputs.runtimeAbi = runtime::runtimeAbiName();
  inputs.optimizationLevel = optimizationLevelName(effectiveOptimizationLevel(options));
  inputs.debugInfo = options.debugInfo;
  inputs.targetTriple = options.targetTriple;
  inputs.gcMode = options.gcMode;
  return inputs;
}

BuildSourceRange buildSourceRange(const support::SourceManager& sources,
                                  support::SourceSpan span) {
  BuildSourceRange range;
  if (!span.isValid()) {
    return range;
  }
  const support::SourceFile* file = sources.get(span.source);
  if (file == nullptr) {
    return range;
  }

  range.valid = true;
  range.path = file->path;
  range.startOffset = std::min(span.start, file->contents.size());
  const std::size_t remaining = file->contents.size() - range.startOffset;
  range.endOffset = range.startOffset + std::min(span.length, remaining);
  std::tie(range.startLine, range.startColumn) =
      sources.lineColumn(support::SourceSpan{span.source, range.startOffset, 0});
  std::tie(range.endLine, range.endColumn) =
      sources.lineColumn(support::SourceSpan{span.source, range.endOffset, 0});
  return range;
}

std::vector<BuildDiagnostic>
buildDiagnostics(const support::SourceManager& sources,
                 const support::DiagnosticEngine& diagnostics) {
  std::vector<BuildDiagnostic> result;
  result.reserve(diagnostics.diagnostics().size());
  for (const support::Diagnostic& diagnostic : diagnostics.diagnostics()) {
    BuildDiagnostic built;
    built.severity = support::severityName(diagnostic.severity);
    built.message = diagnostic.message;
    built.range = buildSourceRange(sources, diagnostic.span);
    built.fixIts.reserve(diagnostic.fixIts.size());
    for (const support::FixIt& fixIt : diagnostic.fixIts) {
      built.fixIts.push_back(
          BuildFixIt{buildSourceRange(sources, fixIt.span), fixIt.replacement});
    }
    result.push_back(std::move(built));
  }
  return result;
}

std::string normalizedSysroot(const BuildOptions& options) {
  if (options.sysroot.empty()) {
    return {};
  }
  std::error_code canonicalError;
  const std::filesystem::path canonical =
      std::filesystem::weakly_canonical(options.sysroot, canonicalError);
  return (canonicalError ? options.sysroot : canonical).string();
}

std::string jsonEscape(std::string_view text) {
  std::ostringstream out;
  for (char ch : text) {
    switch (ch) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\b':
      out << "\\b";
      break;
    case '\f':
      out << "\\f";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      if (static_cast<unsigned char>(ch) < 0x20) {
        out << "\\u00";
        constexpr char digits[] = "0123456789abcdef";
        out << digits[(static_cast<unsigned char>(ch) >> 4) & 0xf]
            << digits[static_cast<unsigned char>(ch) & 0xf];
      } else {
        out << ch;
      }
      break;
    }
  }
  return out.str();
}

interflow::OptimizationTier interflowTier(OptimizationLevel level) {
  switch (level) {
  case OptimizationLevel::O1:
    return interflow::OptimizationTier::Basic;
  case OptimizationLevel::O3:
    return interflow::OptimizationTier::Aggressive;
  case OptimizationLevel::O0:
  case OptimizationLevel::O2:
    return interflow::OptimizationTier::Standard;
  }
  return interflow::OptimizationTier::Standard;
}

std::string optimizationReportJson(const interflow::InterflowResult& optimized,
                                   OptimizationLevel optimizationLevel) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"optimized\": true,\n";
  out << "  \"optimizationLevel\": \"" << optimizationLevelName(optimizationLevel)
      << "\",\n";
  out << "  \"ok\": " << (optimized.ok ? "true" : "false") << ",\n";
  out << "  \"removedDefinitions\": " << optimized.removedDefinitions << ",\n";
  out << "  \"changedValues\": " << optimized.changedValues << ",\n";
  out << "  \"passes\": [\n";
  for (std::size_t i = 0; i < optimized.reports.size(); ++i) {
    const interflow::PassReport& report = optimized.reports[i];
    out << "    {\n";
    out << "      \"name\": \"" << jsonEscape(report.name) << "\",\n";
    out << "      \"definitionsBefore\": " << report.definitionsBefore << ",\n";
    out << "      \"definitionsAfter\": " << report.definitionsAfter << ",\n";
    out << "      \"removedDefinitions\": " << report.removedDefinitions << ",\n";
    out << "      \"changedValues\": " << report.changedValues << ",\n";
    out << "      \"validationErrorsBefore\": " << report.validationErrorsBefore
        << ",\n";
    out << "      \"validationErrorsAfter\": " << report.validationErrorsAfter << ",\n";
    out << "      \"durationMicros\": " << report.durationMicros << "\n";
    out << "    }" << (i + 1 == optimized.reports.size() ? "\n" : ",\n");
  }
  out << "  ]\n";
  out << "}\n";
  return out.str();
}

std::string linkedProgramNirText(const linker::LinkedProgram& program) {
  std::ostringstream out;
  for (const nir::Module& module : program.modules) {
    out << nir::toText(module);
  }
  return out.str();
}

std::filesystem::path defaultNativeOutputPath(const support::SourceManager& sources,
                                              support::SourceId source,
                                              BuildAction action) {
  const support::SourceFile* file = sources.get(source);
  std::filesystem::path sourcePath("a.out");
  if (file != nullptr) {
    sourcePath = file->path;
  }
  std::string stem = sourcePath.stem().string();
  if (stem.empty()) {
    stem = "a.out";
  }

  if (action == BuildAction::BuildObject) {
    return std::filesystem::path(stem + ".o");
  }
  return std::filesystem::path(stem);
}

std::filesystem::path
nativeIntermediateLlvmPath(const std::filesystem::path& outputPath) {
  std::filesystem::path directory = outputPath.parent_path();
  if (directory.empty()) {
    directory = std::filesystem::temp_directory_path();
  }

  std::string filename = outputPath.filename().string();
  if (filename.empty()) {
    filename = "a.out";
  }
  return directory / (filename + ".ll");
}

std::filesystem::path
nativeIntermediateObjectPath(const std::filesystem::path& outputPath) {
  std::filesystem::path directory = outputPath.parent_path();
  if (directory.empty()) {
    directory = std::filesystem::temp_directory_path();
  }

  std::string filename = outputPath.filename().string();
  if (filename.empty()) {
    filename = "a.out";
  }
  return directory / (filename + ".link.o");
}

std::optional<std::string> readBinaryFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  if (input.bad()) {
    return std::nullopt;
  }
  return contents.str();
}

std::string nativeInputFingerprint(const ResolvedNativeLinkInput& input) {
  std::ostringstream fingerprint;
  fingerprint << input.category << ':' << input.argument;
  if (!input.resolvedPath.has_value()) {
    return fingerprint.str();
  }

  const std::filesystem::path& path = *input.resolvedPath;
  std::error_code statusError;
  if (!std::filesystem::is_regular_file(path, statusError) || statusError) {
    return fingerprint.str();
  }

  std::error_code canonicalError;
  const std::filesystem::path canonical =
      std::filesystem::weakly_canonical(path, canonicalError);
  std::error_code sizeError;
  const std::uintmax_t size = std::filesystem::file_size(path, sizeError);
  std::error_code timeError;
  const auto modified = std::filesystem::last_write_time(path, timeError);
  fingerprint << ":path=" << (canonicalError ? path : canonical).string();
  fingerprint << ":size=" << (sizeError ? 0 : size);
  fingerprint << ":modified=" << (timeError ? 0 : modified.time_since_epoch().count());
  if (const std::optional<std::string> contents = readBinaryFile(path)) {
    fingerprint << ":hash=" << BuildCache::hash(*contents);
  }
  return fingerprint.str();
}

std::vector<std::string>
nativeLinkInputFingerprints(const NativeLinkResolution& resolution) {
  std::vector<std::string> inputs;
  inputs.reserve(resolution.inputs.size());
  for (const ResolvedNativeLinkInput& input : resolution.inputs) {
    inputs.push_back(nativeInputFingerprint(input));
  }
  return inputs;
}

std::vector<std::string> nativeLinkPlanFingerprints(const NativeLinkPlan& plan) {
  std::vector<std::string> inputs;
  inputs.reserve(plan.normalizedArguments.size() + plan.resolvedInputs.size());
  for (const std::string& argument : plan.normalizedArguments) {
    inputs.push_back("link-plan-argument:" + argument);
  }
  for (const ResolvedNativeLinkInput& input : plan.resolvedInputs) {
    inputs.push_back(nativeInputFingerprint(input));
  }
  return inputs;
}

bool makeExecutable(const std::filesystem::path& path,
                    support::DiagnosticEngine& diagnostics) {
#ifdef _WIN32
  (void)path;
  (void)diagnostics;
  return true;
#else
  std::error_code permissionError;
  std::filesystem::permissions(path,
                               std::filesystem::perms::owner_exec |
                                   std::filesystem::perms::group_exec |
                                   std::filesystem::perms::others_exec,
                               std::filesystem::perm_options::add, permissionError);
  if (!permissionError) {
    return true;
  }
  diagnostics.error(support::SourceSpan::none(),
                    "could not make cached native binary executable '" + path.string() +
                        "': " + permissionError.message());
  return false;
#endif
}

bool validateStaticLinkResolution(const BuildOptions& options,
                                  const NativeLinkResolution& resolution,
                                  support::DiagnosticEngine& diagnostics) {
  if (options.linkMode != LinkMode::Static ||
      resolution.missingStaticLibraries.empty()) {
    return true;
  }

  std::vector<std::string> missing = resolution.missingStaticLibraries;
  std::sort(missing.begin(), missing.end());
  missing.erase(std::unique(missing.begin(), missing.end()), missing.end());
  std::ostringstream message;
  message << "static linkage is unavailable";
  if (!options.targetTriple.empty()) {
    message << " for target '" << options.targetTriple << '\'';
  }
  message << ": required static link input" << (missing.size() == 1 ? " " : "s ");
  for (std::size_t index = 0; index < missing.size(); ++index) {
    if (index != 0) {
      message << ", ";
    }
    message << '\'' << missing[index] << '\'';
  }
  message << " could not be resolved by clang";
  diagnostics.error(support::SourceSpan::none(), message.str());
  return false;
}

bool validateNativeSysroot(const BuildOptions& options,
                           support::DiagnosticEngine& diagnostics) {
  if (options.sysroot.empty()) {
    return true;
  }
  std::error_code existsError;
  if (!std::filesystem::exists(options.sysroot, existsError) || existsError) {
    diagnostics.error(support::SourceSpan::none(), "native sysroot does not exist: '" +
                                                       options.sysroot.string() + "'");
    return false;
  }
  std::error_code directoryError;
  if (!std::filesystem::is_directory(options.sysroot, directoryError) ||
      directoryError) {
    diagnostics.error(support::SourceSpan::none(),
                      "native sysroot is not a directory: '" +
                          options.sysroot.string() + "'");
    return false;
  }
  return true;
}

bool validateLinkerSelection(const NativeToolchain& toolchain,
                             const BuildOptions& options,
                             support::DiagnosticEngine& diagnostics) {
  if (options.linkerMode != LinkerMode::Lld || toolchain.lld.has_value()) {
    return true;
  }
  diagnostics.error(support::SourceSpan::none(),
                    "LLD linker not found on PATH; install LLD or set "
                    "CPP_SCALANATIVE_LLD to an explicit linker path");
  return false;
}

bool requiresNativeCapabilityProbe(const BuildOptions& options) {
  return !options.targetTriple.empty() || !options.sysroot.empty() ||
         options.linkMode == LinkMode::Static || options.linkerMode == LinkerMode::Lld;
}

bool validateNativeToolchainCapabilities(const BuildOptions& options,
                                         BuildResult& result,
                                         support::DiagnosticEngine& diagnostics) {
  std::optional<NativeToolchain> toolchain = discoverNativeToolchain();
  if (!toolchain) {
    diagnostics.error(support::SourceSpan::none(),
                      "clang toolchain not found on PATH; set "
                      "CPP_SCALANATIVE_CLANG to an explicit clang path");
    return false;
  }

  const NativeTargetProbe target = probeNativeTarget(*toolchain, options);
  if (!target.ok) {
    const std::string requested =
        options.targetTriple.empty() ? std::string("host") : options.targetTriple;
    diagnostics.error(support::SourceSpan::none(), "clang cannot use native target '" +
                                                       requested +
                                                       "': " + target.error);
    return false;
  }
  result.phaseLog.push_back("native-target: capability probe " + target.targetTriple);

  if (options.action != BuildAction::BuildBinary) {
    return true;
  }
  if (!validateLinkerSelection(*toolchain, options, diagnostics)) {
    return false;
  }
  if (options.linkMode == LinkMode::Static) {
    const NativeLinkResolution resolution =
        resolveNativeLinkInputs(*toolchain, options);
    if (!validateStaticLinkResolution(options, resolution, diagnostics)) {
      return false;
    }
  }

  const NativeLinkPlan plan = probeNativeLinkPlan(*toolchain, options);
  if (!plan.ok) {
    diagnostics.error(support::SourceSpan::none(),
                      "native link capability probe failed for target '" +
                          target.targetTriple + "': " + plan.error);
    return false;
  }
  result.phaseLog.push_back(
      "native-link: capability probe linker=" +
      std::string(linkerModeName(options.linkerMode)) +
      ", resolved-inputs=" + std::to_string(plan.resolvedInputs.size()));
  return true;
}

bool recordToolchainInvocation(const ToolchainInvocation& invocation,
                               std::string_view operation, BuildResult& result,
                               support::DiagnosticEngine& diagnostics) {
  result.phaseLog.push_back("toolchain: " + invocation.command);
  if (invocation.ok) {
    return true;
  }

  std::string message = std::string(operation) + " failed with exit code " +
                        std::to_string(invocation.exitCode);
  if (!invocation.output.empty()) {
    message += ": " + invocation.output;
  }
  diagnostics.error(support::SourceSpan::none(), std::move(message));
  return false;
}

bool runNativeToolchain(BuildAction action, const std::filesystem::path& llvmIrPath,
                        const std::filesystem::path& outputPath,
                        const BuildOptions& options, BuildResult& result,
                        support::DiagnosticEngine& diagnostics) {
  std::optional<NativeToolchain> toolchain = discoverNativeToolchain();
  if (!toolchain) {
    diagnostics.error(support::SourceSpan::none(),
                      "clang toolchain not found on PATH; set "
                      "CPP_SCALANATIVE_CLANG to an explicit clang path");
    return false;
  }

  if (action == BuildAction::BuildBinary) {
    if (!validateLinkerSelection(*toolchain, options, diagnostics)) {
      return false;
    }
    const NativeLinkResolution resolution =
        resolveNativeLinkInputs(*toolchain, options);
    result.phaseLog.push_back(
        "native-link: mode=" + std::string(linkModeName(options.linkMode)) +
        ", linker=" + std::string(linkerModeName(options.linkerMode)) +
        ", resolved-inputs=" + std::to_string(resolution.inputs.size()));
    if (!validateStaticLinkResolution(options, resolution, diagnostics)) {
      return false;
    }
  }

  ToolchainInvocation invocation =
      action == BuildAction::BuildObject
          ? compileLlvmToObject(*toolchain, llvmIrPath, outputPath, options)
          : linkLlvmToBinary(*toolchain, llvmIrPath, outputPath, options);
  result.phaseLog.push_back("toolchain: " + invocation.command);

  if (!invocation.ok) {
    std::string message = std::string(buildActionName(action)) +
                          " failed with exit code " +
                          std::to_string(invocation.exitCode);
    if (!invocation.output.empty()) {
      message += ": " + invocation.output;
    }
    diagnostics.error(support::SourceSpan::none(), std::move(message));
    return false;
  }

  result.producedArtifacts.push_back(outputPath);
  result.phaseLog.push_back("toolchain: wrote " +
                            std::string(action == BuildAction::BuildObject
                                            ? "native object"
                                            : "native binary") +
                            " to " + outputPath.string());
  return true;
}

bool runCachedNativeToolchain(BuildAction action, std::string_view llvmIr,
                              const std::filesystem::path& outputPath,
                              const BuildOptions& options, BuildCache& buildCache,
                              BuildResult& result,
                              support::DiagnosticEngine& diagnostics) {
  std::optional<NativeToolchain> toolchain = discoverNativeToolchain();
  if (!toolchain) {
    diagnostics.error(support::SourceSpan::none(),
                      "clang toolchain not found on PATH; set "
                      "CPP_SCALANATIVE_CLANG to an explicit clang path");
    return false;
  }

  if (action == BuildAction::BuildBinary &&
      !validateLinkerSelection(*toolchain, options, diagnostics)) {
    return false;
  }
  const std::string toolchainFingerprint = nativeToolchainFingerprint(*toolchain);
  result.objectCacheKey = BuildCache::objectKey(NativeObjectCacheInputs{
      BuildCache::hash(llvmIr), toolchainFingerprint,
      std::string(optimizationLevelName(effectiveOptimizationLevel(options))),
      options.targetTriple, normalizedSysroot(options)});
  std::optional<NativeLinkResolution> linkResolution;
  if (action == BuildAction::BuildBinary) {
    linkResolution = resolveNativeLinkInputs(*toolchain, options);
    result.linkFingerprint = BuildCache::linkFingerprint(NativeLinkFingerprintInputs{
        result.objectCacheKey, toolchainFingerprint, options.targetTriple,
        normalizedSysroot(options), std::string(linkModeName(options.linkMode)),
        std::string(linkerModeName(options.linkerMode)),
        nativeLinkInputFingerprints(*linkResolution)});
    result.phaseLog.push_back(
        "native-link: mode=" + std::string(linkModeName(options.linkMode)) +
        ", linker=" + std::string(linkerModeName(options.linkerMode)) +
        ", resolved-inputs=" + std::to_string(linkResolution->inputs.size()));
    result.phaseLog.push_back("native-link: fingerprint " + result.linkFingerprint);
    if (!validateStaticLinkResolution(options, *linkResolution, diagnostics)) {
      return false;
    }
  }

  const std::filesystem::path objectPath =
      action == BuildAction::BuildObject ? outputPath
                                         : nativeIntermediateObjectPath(outputPath);
  std::string objectMissReason;
  std::optional<BuildCacheEntry> cachedObject = buildCache.load(
      result.objectCacheKey, CachedArtifactKind::Object, false, objectMissReason);
  if (cachedObject.has_value()) {
    result.objectCacheHit = true;
    result.phaseLog.push_back("native-cache: object hit " + result.objectCacheKey);
    if (!writeTextOutput(objectPath,
                         action == BuildAction::BuildObject
                             ? "cached native object"
                             : "intermediate native object",
                         cachedObject->artifact, result, diagnostics,
                         action == BuildAction::BuildObject)) {
      return false;
    }
  } else {
    result.phaseLog.push_back("native-cache: object miss " + result.objectCacheKey +
                              " (" + objectMissReason + ")");
    const std::filesystem::path llvmIrPath = nativeIntermediateLlvmPath(outputPath);
    if (!writeTextOutput(llvmIrPath, "intermediate LLVM IR", llvmIr, result,
                         diagnostics, false)) {
      return false;
    }
    const ToolchainInvocation compile =
        compileLlvmToObject(*toolchain, llvmIrPath, objectPath, options);
    if (!recordToolchainInvocation(compile, "build-object", result, diagnostics)) {
      return false;
    }
    result.phaseLog.push_back("toolchain: wrote " +
                              std::string(action == BuildAction::BuildObject
                                              ? "native object"
                                              : "intermediate native object") +
                              " to " + objectPath.string());
    if (action == BuildAction::BuildObject) {
      result.producedArtifacts.push_back(objectPath);
    }

    const std::optional<std::string> object = readBinaryFile(objectPath);
    if (!object.has_value()) {
      result.phaseLog.push_back("native-cache: object store skipped: could not read '" +
                                objectPath.string() + "'");
    } else {
      std::string cacheError;
      if (buildCache.store(result.objectCacheKey, CachedArtifactKind::Object, *object,
                           {}, cacheError)) {
        result.phaseLog.push_back("native-cache: object stored " +
                                  result.objectCacheKey);
      } else {
        result.phaseLog.push_back("native-cache: object store skipped: " + cacheError);
      }
    }
  }

  if (action == BuildAction::BuildObject) {
    return true;
  }

  const NativeLinkPlan linkPlan =
      inspectNativeLinkPlan(*toolchain, objectPath, outputPath, options);
  if (linkPlan.ok && linkResolution.has_value()) {
    std::vector<std::string> linkInputs = nativeLinkInputFingerprints(*linkResolution);
    std::vector<std::string> planInputs = nativeLinkPlanFingerprints(linkPlan);
    linkInputs.insert(linkInputs.end(), std::make_move_iterator(planInputs.begin()),
                      std::make_move_iterator(planInputs.end()));
    result.linkFingerprint = BuildCache::linkFingerprint(NativeLinkFingerprintInputs{
        result.objectCacheKey, toolchainFingerprint, options.targetTriple,
        normalizedSysroot(options), std::string(linkModeName(options.linkMode)),
        std::string(linkerModeName(options.linkerMode)), std::move(linkInputs)});
    result.phaseLog.push_back("native-link: complete fingerprint " +
                              result.linkFingerprint);

    std::string binaryMissReason;
    std::optional<BuildCacheEntry> cachedBinary = buildCache.load(
        result.linkFingerprint, CachedArtifactKind::Binary, false, binaryMissReason);
    if (cachedBinary.has_value()) {
      result.binaryCacheHit = true;
      result.phaseLog.push_back("native-cache: binary hit " + result.linkFingerprint);
      if (!writeTextOutput(outputPath, "cached native binary", cachedBinary->artifact,
                           result, diagnostics) ||
          !makeExecutable(outputPath, diagnostics)) {
        return false;
      }
      return true;
    }
    result.phaseLog.push_back("native-cache: binary miss " + result.linkFingerprint +
                              " (" + binaryMissReason + ")");
  } else {
    result.phaseLog.push_back("native-cache: binary disabled: " +
                              (linkPlan.error.empty()
                                   ? std::string("incomplete linker plan")
                                   : linkPlan.error));
  }

  const ToolchainInvocation link =
      linkObjectToBinary(*toolchain, objectPath, outputPath, options);
  if (!recordToolchainInvocation(link, "build-binary", result, diagnostics)) {
    return false;
  }
  result.producedArtifacts.push_back(outputPath);
  result.phaseLog.push_back("toolchain: wrote native binary to " + outputPath.string());
  if (linkPlan.ok && !result.linkFingerprint.empty()) {
    const std::optional<std::string> binary = readBinaryFile(outputPath);
    if (!binary.has_value()) {
      result.phaseLog.push_back("native-cache: binary store skipped: could not read '" +
                                outputPath.string() + "'");
    } else {
      std::string cacheError;
      if (buildCache.store(result.linkFingerprint, CachedArtifactKind::Binary, *binary,
                           {}, cacheError)) {
        result.phaseLog.push_back("native-cache: binary stored " +
                                  result.linkFingerprint);
      } else {
        result.phaseLog.push_back("native-cache: binary store skipped: " + cacheError);
      }
    }
  }
  return true;
}

} // namespace

std::string_view buildActionName(BuildAction action) {
  switch (action) {
  case BuildAction::Compile:
    return "compile";
  case BuildAction::Check:
    return "check";
  case BuildAction::EmitNir:
    return "emit-nir";
  case BuildAction::EmitLlvm:
    return "emit-llvm";
  case BuildAction::BuildObject:
    return "build-object";
  case BuildAction::BuildBinary:
    return "build-binary";
  }
  return "compile";
}

std::string_view optimizationLevelName(OptimizationLevel level) {
  switch (level) {
  case OptimizationLevel::O0:
    return "O0";
  case OptimizationLevel::O1:
    return "O1";
  case OptimizationLevel::O2:
    return "O2";
  case OptimizationLevel::O3:
    return "O3";
  }
  return "O0";
}

std::string_view linkModeName(LinkMode mode) {
  switch (mode) {
  case LinkMode::Default:
    return "default";
  case LinkMode::Static:
    return "static";
  }
  return "default";
}

std::string_view linkerModeName(LinkerMode mode) {
  switch (mode) {
  case LinkerMode::Default:
    return "default";
  case LinkerMode::Lld:
    return "lld";
  }
  return "default";
}

OptimizationLevel effectiveOptimizationLevel(const BuildOptions& options) {
  if (options.optimizationLevel != OptimizationLevel::O0) {
    return options.optimizationLevel;
  }
  return options.optimize ? OptimizationLevel::O2 : OptimizationLevel::O0;
}

BuildResult BuildDriver::buildFile(const std::filesystem::path& path,
                                   const BuildOptions& options,
                                   support::DiagnosticEngine& diagnostics) const {
  support::SourceManager sources;
  std::optional<support::SourceId> source = sources.addFile(path, diagnostics);
  if (!source) {
    BuildResult result;
    result.sourcePath = path.string();
    std::ostringstream rendered;
    diagnostics.render(sources, rendered);
    result.diagnosticsText = rendered.str();
    result.diagnostics = buildDiagnostics(sources, diagnostics);
    return result;
  }
  return buildLoadedSource(sources, *source, options, diagnostics);
}

BuildResult BuildDriver::buildSource(std::string name, std::string source,
                                     const BuildOptions& options,
                                     support::DiagnosticEngine& diagnostics) const {
  support::SourceManager sources;
  support::SourceId sourceId =
      sources.addVirtualFile(std::move(name), std::move(source), diagnostics);
  return buildLoadedSource(sources, sourceId, options, diagnostics);
}

BuildResult
BuildDriver::buildLoadedSource(support::SourceManager& sources,
                               support::SourceId source, const BuildOptions& options,
                               support::DiagnosticEngine& diagnostics) const {
  BuildResult result;
  if (const support::SourceFile* sourceFile = sources.get(source)) {
    result.sourcePath = sourceFile->path;
  }
  const OptimizationLevel optimizationLevel = effectiveOptimizationLevel(options);
  const bool optimize = optimizationLevel != OptimizationLevel::O0;
  auto finish = [&]() {
    std::ostringstream rendered;
    diagnostics.render(sources, rendered);
    result.diagnosticsText = rendered.str();
    result.diagnostics = buildDiagnostics(sources, diagnostics);
    result.ok = !diagnostics.hasErrors();
    return result;
  };

  if (diagnostics.hasErrors()) {
    return finish();
  }
  if ((options.action == BuildAction::BuildObject ||
       options.action == BuildAction::BuildBinary) &&
      !validateNativeSysroot(options, diagnostics)) {
    return finish();
  }
  if ((options.action == BuildAction::BuildObject ||
       options.action == BuildAction::BuildBinary) &&
      requiresNativeCapabilityProbe(options) &&
      !validateNativeToolchainCapabilities(options, result, diagnostics)) {
    return finish();
  }
  if (!options.optimizationReportPath.empty() &&
      (!optimize || options.action == BuildAction::Check)) {
    diagnostics.error(support::SourceSpan::none(),
                      "optimization report requires --optimize or --opt-level 1..3 "
                      "with emit-nir, compile, emit-llvm, build-object, or "
                      "build-binary");
    return finish();
  }

  const CachedArtifactKind cachedArtifactKind = options.action == BuildAction::EmitNir
                                                    ? CachedArtifactKind::Nir
                                                    : CachedArtifactKind::Llvm;
  std::optional<BuildCache> buildCache;
  std::string cacheMissReason;
  if (!options.cacheDirectory.empty() && options.action != BuildAction::Check) {
    buildCache.emplace(options.cacheDirectory);
    result.cacheKey =
        BuildCache::key(buildCacheInputs(sources, source, cachedArtifactKind, options));
    std::optional<BuildCacheEntry> cached = buildCache->load(
        result.cacheKey, cachedArtifactKind, optimize, cacheMissReason);
    if (cached.has_value()) {
      result.cacheHit = true;
      result.phaseLog.push_back(buildConfigSummary(options));
      result.phaseLog.push_back("cache: hit " + result.cacheKey);
      result.optimizationReportText = std::move(cached->optimizationReport);
      if (!writeTextOutput(options.optimizationReportPath, "optimization report",
                           result.optimizationReportText, result, diagnostics)) {
        return finish();
      }

      if (cachedArtifactKind == CachedArtifactKind::Nir) {
        result.nirText = std::move(cached->artifact);
        (void)writeTextOutput(options.outputPath, "NIR", result.nirText, result,
                              diagnostics);
        return finish();
      }

      result.llvmIr = std::move(cached->artifact);
      if (options.action == BuildAction::EmitLlvm ||
          options.action == BuildAction::Compile) {
        (void)writeTextOutput(options.outputPath, "LLVM IR", result.llvmIr, result,
                              diagnostics);
        return finish();
      }

      const std::filesystem::path outputPath =
          options.outputPath.empty()
              ? defaultNativeOutputPath(sources, source, options.action)
              : options.outputPath;
      (void)runCachedNativeToolchain(options.action, result.llvmIr, outputPath, options,
                                     *buildCache, result, diagnostics);
      return finish();
    }
  }

  std::vector<std::string> nativeCapabilityLog = result.phaseLog;
  frontend::CompilerPipeline pipeline;
  frontend::CompileResult compiled = pipeline.compile(sources, source, diagnostics);
  result.phaseLog = compiled.phaseLog;
  result.phaseLog.insert(result.phaseLog.end(), nativeCapabilityLog.begin(),
                         nativeCapabilityLog.end());
  result.phaseLog.push_back(buildConfigSummary(options));
  if (buildCache.has_value()) {
    result.phaseLog.push_back("cache: miss " + result.cacheKey + " (" +
                              cacheMissReason + ")");
  }
  if (!compiled.ok) {
    return finish();
  }

  auto storeCachedArtifact = [&](std::string_view artifact) {
    if (!buildCache.has_value()) {
      return;
    }
    std::string cacheError;
    if (buildCache->store(result.cacheKey, cachedArtifactKind, artifact,
                          result.optimizationReportText, cacheError)) {
      result.phaseLog.push_back("cache: stored " + result.cacheKey);
    } else {
      result.phaseLog.push_back("cache: write skipped: " + cacheError);
    }
  };

  nscplugin::NirEmitter emitter;
  nir::Module module = emitter.emit(compiled.typed);
  result.nirText = nir::toText(module);
  result.phaseLog.push_back("nscplugin: emitted NIR");

  checker::NirChecker checker;
  if (!checker.check(module, diagnostics)) {
    return finish();
  }
  result.phaseLog.push_back("checker: ok");

  if (options.action == BuildAction::Check) {
    if (!options.outputPath.empty()) {
      diagnostics.error(support::SourceSpan::none(),
                        "check action does not produce a file output");
    }
    return finish();
  }

  if (options.action == BuildAction::EmitNir && !optimize) {
    storeCachedArtifact(result.nirText);
    (void)writeTextOutput(options.outputPath, "NIR", result.nirText, result,
                          diagnostics);
    return finish();
  }

  linker::Linker linker;
  linker::LinkResult linked = linker.link({std::move(module)}, diagnostics);
  if (!linked.ok) {
    return finish();
  }
  result.phaseLog.push_back(
      "linker: " + std::to_string(linked.program.roots.size()) + " roots, " +
      std::to_string(linked.program.reachableGlobals.size()) + " reachable globals");

  linker::LinkedProgram program = std::move(linked.program);
  if (optimize) {
    interflow::InterflowOptimizer optimizer;
    interflow::InterflowResult optimized = optimizer.optimize(
        std::move(program),
        interflow::InterflowOptions{interflowTier(optimizationLevel)});
    for (const std::string& error : optimized.errors) {
      diagnostics.error(support::SourceSpan::none(), "interflow: " + error);
    }
    if (!optimized.ok) {
      return finish();
    }
    result.optimizationReportText =
        optimizationReportJson(optimized, optimizationLevel);
    if (!writeTextOutput(options.optimizationReportPath, "optimization report",
                         result.optimizationReportText, result, diagnostics)) {
      return finish();
    }
    program = std::move(optimized.program);
    result.phaseLog.push_back(
        "interflow: optimized, " + std::to_string(optimized.removedDefinitions) +
        " removed definitions, " + std::to_string(optimized.changedValues) +
        " folded values");
  } else {
    result.phaseLog.push_back("interflow: skipped");
  }

  if (options.action == BuildAction::EmitNir) {
    result.nirText = linkedProgramNirText(program);
    storeCachedArtifact(result.nirText);
    (void)writeTextOutput(options.outputPath, "NIR", result.nirText, result,
                          diagnostics);
    return finish();
  }

  codegen::LlvmCodegen codegen;
  codegen::CodegenResult generated = codegen.emit(
      program, codegen::CodegenOptions{&sources, optimize, options.debugInfo});
  for (const codegen::CodegenError& error : generated.errors) {
    diagnostics.error(error.span, "codegen: " + error.message);
  }
  if (!generated.ok) {
    return finish();
  }
  result.llvmIr = std::move(generated.llvmIr);
  result.phaseLog.push_back("codegen: emitted LLVM IR");
  storeCachedArtifact(result.llvmIr);

  if (options.action == BuildAction::EmitLlvm ||
      options.action == BuildAction::Compile) {
    (void)writeTextOutput(options.outputPath, "LLVM IR", result.llvmIr, result,
                          diagnostics);
    return finish();
  }

  if (options.action == BuildAction::BuildObject ||
      options.action == BuildAction::BuildBinary) {
    const std::filesystem::path outputPath =
        options.outputPath.empty()
            ? defaultNativeOutputPath(sources, source, options.action)
            : options.outputPath;
    if (buildCache.has_value()) {
      (void)runCachedNativeToolchain(options.action, result.llvmIr, outputPath, options,
                                     *buildCache, result, diagnostics);
    } else {
      const std::filesystem::path llvmIrPath = nativeIntermediateLlvmPath(outputPath);
      if (!writeTextOutput(llvmIrPath, "intermediate LLVM IR", result.llvmIr, result,
                           diagnostics, false)) {
        return finish();
      }
      (void)runNativeToolchain(options.action, llvmIrPath, outputPath, options, result,
                               diagnostics);
    }
    return finish();
  }

  return finish();
}

} // namespace scalanative::tools::build
