#pragma once

#include "scalanative/support/Diagnostics.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace scalanative::tools::build {

enum class BuildAction { Compile, Check, EmitNir, EmitLlvm, BuildObject, BuildBinary };
enum class OptimizationLevel { O0, O1, O2, O3 };
enum class LinkMode { Default, Static };
enum class LinkerMode { Default, Lld };

struct BuildOptions {
  BuildAction action = BuildAction::Compile;
  bool optimize = false;
  OptimizationLevel optimizationLevel = OptimizationLevel::O0;
  LinkMode linkMode = LinkMode::Default;
  LinkerMode linkerMode = LinkerMode::Default;
  bool debugInfo = true;
  std::string targetTriple;
  std::filesystem::path sysroot;
  std::string gcMode = "hybrid";
  std::filesystem::path outputPath;
  std::filesystem::path optimizationReportPath;
  std::filesystem::path cacheDirectory;
  std::filesystem::path configurationPath;
  std::vector<std::string> runtimeLibraries;
  std::vector<std::string> linkLibraries;
};

struct BuildSourceRange {
  bool valid = false;
  std::string path;
  std::size_t startOffset = 0;
  std::size_t endOffset = 0;
  std::size_t startLine = 0;
  std::size_t startColumn = 0;
  std::size_t endLine = 0;
  std::size_t endColumn = 0;
};

struct BuildFixIt {
  BuildSourceRange range;
  std::string replacement;
};

struct BuildDiagnostic {
  std::string severity;
  std::string message;
  BuildSourceRange range;
  std::vector<BuildFixIt> fixIts;
};

struct BuildResult {
  bool ok = false;
  bool cacheHit = false;
  bool objectCacheHit = false;
  bool binaryCacheHit = false;
  std::string cacheKey;
  std::string sourcePath;
  std::string objectCacheKey;
  std::string linkFingerprint;
  std::string nirText;
  std::string llvmIr;
  std::string optimizationReportText;
  std::string diagnosticsText;
  std::vector<BuildDiagnostic> diagnostics;
  std::vector<std::string> phaseLog;
  std::vector<std::filesystem::path> producedArtifacts;
};

[[nodiscard]] std::string_view buildActionName(BuildAction action);
[[nodiscard]] std::string_view optimizationLevelName(OptimizationLevel level);
[[nodiscard]] std::string_view linkModeName(LinkMode mode);
[[nodiscard]] std::string_view linkerModeName(LinkerMode mode);
[[nodiscard]] OptimizationLevel effectiveOptimizationLevel(const BuildOptions& options);

class BuildDriver {
public:
  [[nodiscard]] BuildResult buildFile(const std::filesystem::path& path,
                                      const BuildOptions& options,
                                      support::DiagnosticEngine& diagnostics) const;

  [[nodiscard]] BuildResult buildSource(std::string name, std::string source,
                                        const BuildOptions& options,
                                        support::DiagnosticEngine& diagnostics) const;

private:
  [[nodiscard]] BuildResult
  buildLoadedSource(support::SourceManager& sources, support::SourceId source,
                    const BuildOptions& options,
                    support::DiagnosticEngine& diagnostics) const;
};

} // namespace scalanative::tools::build
