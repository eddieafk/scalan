#include "scalanative/tools/build/BuildCache.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

namespace scalanative::tools::build {

namespace {

constexpr std::string_view CacheFormat = "cpp-scalanative-build-cache-v1";

std::string_view artifactKindName(CachedArtifactKind kind) {
  switch (kind) {
  case CachedArtifactKind::Nir:
    return "nir";
  case CachedArtifactKind::Llvm:
    return "llvm";
  case CachedArtifactKind::Object:
    return "object";
  case CachedArtifactKind::Binary:
    return "binary";
  }
  return "llvm";
}

std::string_view artifactFilename(CachedArtifactKind kind) {
  switch (kind) {
  case CachedArtifactKind::Nir:
    return "artifact.nir";
  case CachedArtifactKind::Llvm:
    return "artifact.ll";
  case CachedArtifactKind::Object:
    return "artifact.o";
  case CachedArtifactKind::Binary:
    return "artifact.bin";
  }
  return "artifact.ll";
}

class StableHasher {
public:
  void add(std::string_view value) {
    addSize(value.size());
    for (const char byte : value) {
      hash_ ^= static_cast<unsigned char>(byte);
      hash_ *= Prime;
    }
  }

  void add(std::uint64_t value) {
    for (std::size_t shift = 0; shift < 64; shift += 8) {
      hash_ ^= static_cast<unsigned char>((value >> shift) & 0xffU);
      hash_ *= Prime;
    }
  }

  [[nodiscard]] std::string finish() const {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash_;
    return out.str();
  }

private:
  void addSize(std::size_t size) {
    add(static_cast<std::uint64_t>(size));
  }

  static constexpr std::uint64_t OffsetBasis = 14695981039346656037ULL;
  static constexpr std::uint64_t Prime = 1099511628211ULL;
  std::uint64_t hash_ = OffsetBasis;
};

void addList(StableHasher& hasher, const std::vector<std::string>& values) {
  hasher.add(static_cast<std::uint64_t>(values.size()));
  for (const std::string& value : values) {
    hasher.add(value);
  }
}

std::optional<std::string> readText(const std::filesystem::path& path) {
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

bool writeAtomically(const std::filesystem::path& path, std::string_view contents,
                     std::string& error) {
  static std::atomic<std::uint64_t> sequence{0};
  const std::uint64_t nonce =
      static_cast<std::uint64_t>(
          std::chrono::steady_clock::now().time_since_epoch().count()) ^
      sequence.fetch_add(1, std::memory_order_relaxed);
  const std::filesystem::path temporary =
      path.string() + ".tmp." + std::to_string(nonce);
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      error = "could not open temporary cache file '" + temporary.string() + "'";
      return false;
    }
    output << contents;
    if (!output) {
      error = "could not write temporary cache file '" + temporary.string() + "'";
      return false;
    }
  }

  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::error_code renameError;
  std::filesystem::rename(temporary, path, renameError);
  if (renameError) {
    std::filesystem::remove(temporary, ignored);
    error = "could not publish cache file '" + path.string() +
            "': " + renameError.message();
    return false;
  }
  return true;
}

std::string manifest(std::string_view key, CachedArtifactKind kind,
                     bool hasOptimizationReport) {
  std::ostringstream out;
  out << "format=" << CacheFormat << '\n';
  out << "key=" << key << '\n';
  out << "kind=" << artifactKindName(kind) << '\n';
  out << "optimization-report=" << (hasOptimizationReport ? "yes" : "no") << '\n';
  return out.str();
}

} // namespace

BuildCache::BuildCache(std::filesystem::path directory)
    : directory_(std::move(directory)) {}

std::string BuildCache::key(const BuildCacheInputs& inputs) {
  StableHasher hasher;
  hasher.add(CacheFormat);
  hasher.add(inputs.sourceHash);
  hasher.add(inputs.sourcePath);
  hasher.add(artifactKindName(inputs.artifactKind));
  hasher.add(inputs.compilerVersion);
  hasher.add(inputs.nirVersion);
  hasher.add(inputs.runtimeAbi);
  hasher.add(inputs.optimizationLevel);
  hasher.add(inputs.debugInfo ? "debug-info" : "no-debug-info");
  hasher.add(inputs.targetTriple);
  hasher.add(inputs.gcMode);
  return hasher.finish();
}

std::string BuildCache::hash(std::string_view contents) {
  StableHasher hasher;
  hasher.add(contents);
  return hasher.finish();
}

std::string BuildCache::objectKey(const NativeObjectCacheInputs& inputs) {
  StableHasher hasher;
  hasher.add(CacheFormat);
  hasher.add("native-object");
  hasher.add(inputs.llvmHash);
  hasher.add(inputs.toolchainFingerprint);
  hasher.add(inputs.optimizationLevel);
  hasher.add(inputs.targetTriple);
  hasher.add(inputs.sysroot);
  return hasher.finish();
}

std::string BuildCache::linkFingerprint(const NativeLinkFingerprintInputs& inputs) {
  StableHasher hasher;
  hasher.add(CacheFormat);
  hasher.add("native-link");
  hasher.add(inputs.objectCacheKey);
  hasher.add(inputs.toolchainFingerprint);
  hasher.add(inputs.targetTriple);
  hasher.add(inputs.sysroot);
  hasher.add(inputs.linkMode);
  hasher.add(inputs.linkerMode);
  addList(hasher, inputs.linkInputs);
  return hasher.finish();
}

std::optional<BuildCacheEntry> BuildCache::load(std::string_view key,
                                                CachedArtifactKind kind,
                                                bool requireOptimizationReport,
                                                std::string& missReason) const {
  const std::filesystem::path entryDirectory = directory_ / std::string(key);
  const std::filesystem::path manifestPath = entryDirectory / "manifest.txt";
  const std::optional<std::string> storedManifest = readText(manifestPath);
  if (!storedManifest.has_value()) {
    missReason = "entry not found";
    return std::nullopt;
  }

  const std::string expectedWithoutReport = manifest(key, kind, false);
  const std::string expectedWithReport = manifest(key, kind, true);
  const bool hasOptimizationReport = *storedManifest == expectedWithReport;
  if (*storedManifest != expectedWithoutReport && !hasOptimizationReport) {
    missReason = "manifest mismatch";
    return std::nullopt;
  }
  if (requireOptimizationReport && !hasOptimizationReport) {
    missReason = "optimization report missing";
    return std::nullopt;
  }

  const std::filesystem::path artifactPath = entryDirectory / artifactFilename(kind);
  std::optional<std::string> artifact = readText(artifactPath);
  if (!artifact.has_value()) {
    missReason = "artifact missing";
    return std::nullopt;
  }

  std::string optimizationReport;
  if (hasOptimizationReport) {
    std::optional<std::string> report =
        readText(entryDirectory / "optimization-report.json");
    if (!report.has_value()) {
      missReason = "optimization report missing";
      return std::nullopt;
    }
    optimizationReport = std::move(*report);
  }
  missReason.clear();
  return BuildCacheEntry{std::move(*artifact), std::move(optimizationReport)};
}

bool BuildCache::store(std::string_view key, CachedArtifactKind kind,
                       std::string_view artifact, std::string_view optimizationReport,
                       std::string& error) const {
  const std::filesystem::path entryDirectory = directory_ / std::string(key);
  std::error_code directoryError;
  std::filesystem::create_directories(entryDirectory, directoryError);
  if (directoryError) {
    error = "could not create cache directory '" + entryDirectory.string() +
            "': " + directoryError.message();
    return false;
  }

  const std::filesystem::path artifactPath = entryDirectory / artifactFilename(kind);
  if (!writeAtomically(artifactPath, artifact, error)) {
    return false;
  }
  if (!optimizationReport.empty() &&
      !writeAtomically(entryDirectory / "optimization-report.json", optimizationReport,
                       error)) {
    return false;
  }

  return writeAtomically(entryDirectory / "manifest.txt",
                         manifest(key, kind, !optimizationReport.empty()), error);
}

} // namespace scalanative::tools::build
