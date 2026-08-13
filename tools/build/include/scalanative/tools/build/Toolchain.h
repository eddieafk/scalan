#pragma once

#include "scalanative/tools/build/BuildDriver.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace scalanative::tools::build {

struct NativeToolchain {
  std::filesystem::path clang;
  std::optional<std::filesystem::path> lld;
};

struct ToolchainInvocation {
  bool ok = false;
  int exitCode = -1;
  std::string command;
  std::string output;
};

struct ResolvedNativeLinkInput {
  std::string category;
  std::string argument;
  std::optional<std::filesystem::path> resolvedPath;
};

struct NativeLinkResolution {
  std::vector<ResolvedNativeLinkInput> inputs;
  std::vector<std::string> missingStaticLibraries;
};

struct NativeLinkPlan {
  bool ok = false;
  std::vector<std::string> normalizedArguments;
  std::vector<ResolvedNativeLinkInput> resolvedInputs;
  std::vector<std::string> unresolvedLibraries;
  std::vector<std::string> unresolvedFiles;
  std::string error;
};

struct NativeTargetProbe {
  bool ok = false;
  std::string targetTriple;
  std::string error;
};

[[nodiscard]] std::optional<NativeToolchain> discoverNativeToolchain();
[[nodiscard]] std::string nativeToolchainFingerprint(const NativeToolchain& toolchain);
[[nodiscard]] NativeLinkResolution
resolveNativeLinkInputs(const NativeToolchain& toolchain, const BuildOptions& options);
[[nodiscard]] NativeLinkPlan inspectNativeLinkPlan(
    const NativeToolchain& toolchain, const std::filesystem::path& objectPath,
    const std::filesystem::path& binaryPath, const BuildOptions& options);
[[nodiscard]] NativeTargetProbe probeNativeTarget(const NativeToolchain& toolchain,
                                                  const BuildOptions& options);
[[nodiscard]] NativeLinkPlan probeNativeLinkPlan(const NativeToolchain& toolchain,
                                                 const BuildOptions& options);

[[nodiscard]] ToolchainInvocation compileLlvmToObject(
    const NativeToolchain& toolchain, const std::filesystem::path& llvmIrPath,
    const std::filesystem::path& objectPath, const BuildOptions& options);

[[nodiscard]] ToolchainInvocation
linkLlvmToBinary(const NativeToolchain& toolchain,
                 const std::filesystem::path& llvmIrPath,
                 const std::filesystem::path& binaryPath, const BuildOptions& options);

[[nodiscard]] ToolchainInvocation linkObjectToBinary(
    const NativeToolchain& toolchain, const std::filesystem::path& objectPath,
    const std::filesystem::path& binaryPath, const BuildOptions& options);

} // namespace scalanative::tools::build
