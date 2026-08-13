#pragma once

#include "scalanative/tools/build/BuildDriver.h"

#include <filesystem>
#include <optional>
#include <string>

namespace scalanative::tools::build {

struct BuildConfiguration {
  BuildOptions options;
  std::filesystem::path sourcePath;
  std::filesystem::path buildReportPath;
};

struct BuildConfigLoadResult {
  std::optional<BuildConfiguration> configuration;
  std::string error;
};

[[nodiscard]] BuildConfigLoadResult
loadBuildConfiguration(const std::filesystem::path& path);

} // namespace scalanative::tools::build
