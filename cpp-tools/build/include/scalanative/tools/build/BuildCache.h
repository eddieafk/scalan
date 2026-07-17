#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scalanative::tools::build {

enum class CachedArtifactKind { Nir, Llvm, Object, Binary };

struct BuildCacheInputs {
  std::uint64_t sourceHash = 0;
  std::string sourcePath;
  CachedArtifactKind artifactKind = CachedArtifactKind::Llvm;
  std::string compilerVersion;
  std::string nirVersion;
  std::string runtimeAbi;
  std::string optimizationLevel;
  bool debugInfo = true;
  std::string targetTriple;
  std::string gcMode;
};

struct BuildCacheEntry {
  std::string artifact;
  std::string optimizationReport;
};

struct NativeObjectCacheInputs {
  std::string llvmHash;
  std::string toolchainFingerprint;
  std::string optimizationLevel;
  std::string targetTriple;
  std::string sysroot;
};

struct NativeLinkFingerprintInputs {
  std::string objectCacheKey;
  std::string toolchainFingerprint;
  std::string targetTriple;
  std::string sysroot;
  std::string linkMode;
  std::string linkerMode;
  std::vector<std::string> linkInputs;
};

class BuildCache {
public:
  explicit BuildCache(std::filesystem::path directory);

  [[nodiscard]] static std::string key(const BuildCacheInputs& inputs);
  [[nodiscard]] static std::string hash(std::string_view contents);
  [[nodiscard]] static std::string objectKey(const NativeObjectCacheInputs& inputs);
  [[nodiscard]] static std::string
  linkFingerprint(const NativeLinkFingerprintInputs& inputs);

  [[nodiscard]] std::optional<BuildCacheEntry> load(std::string_view key,
                                                    CachedArtifactKind kind,
                                                    bool requireOptimizationReport,
                                                    std::string& missReason) const;

  [[nodiscard]] bool store(std::string_view key, CachedArtifactKind kind,
                           std::string_view artifact,
                           std::string_view optimizationReport,
                           std::string& error) const;

private:
  std::filesystem::path directory_;
};

} // namespace scalanative::tools::build
