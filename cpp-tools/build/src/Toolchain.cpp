#include "scalanative/tools/build/Toolchain.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace scalanative::tools::build {

namespace {

std::string readTextFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string shellQuote(std::string_view text) {
  if (text.empty()) {
    return "''";
  }

  std::string quoted = "'";
  for (char ch : text) {
    if (ch == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(ch);
    }
  }
  quoted.push_back('\'');
  return quoted;
}

std::string renderCommand(const std::vector<std::string>& args) {
  std::ostringstream out;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i != 0) {
      out << ' ';
    }
    out << shellQuote(args[i]);
  }
  return out.str();
}

std::filesystem::path toolLogPath(const std::string& command) {
  const std::size_t hash = std::hash<std::string>{}(command);
  return std::filesystem::temp_directory_path() /
         ("cpp-scalanative-toolchain-" + std::to_string(hash) + ".log");
}

ToolchainInvocation runTool(const std::vector<std::string>& args) {
  ToolchainInvocation result;
  result.command = renderCommand(args);

  const std::filesystem::path logPath = toolLogPath(result.command);
  const std::string shellCommand =
      result.command + " > " + shellQuote(logPath.string()) + " 2>&1";
  result.exitCode = std::system(shellCommand.c_str());
  result.output = readTextFile(logPath);
  std::error_code ignored;
  std::filesystem::remove(logPath, ignored);
  result.ok = result.exitCode == 0;
  return result;
}

bool fileExists(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::exists(path, error) && !error;
}

std::string trim(std::string text) {
  std::size_t start = 0;
  while (start < text.size() &&
         std::isspace(static_cast<unsigned char>(text[start])) != 0) {
    ++start;
  }
  std::size_t end = text.size();
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return text.substr(start, end - start);
}

std::optional<std::filesystem::path> findOnPath(std::string_view executable) {
  if (executable.find('/') != std::string_view::npos) {
    std::filesystem::path path(executable);
    if (fileExists(path)) {
      return path;
    }
    return std::nullopt;
  }

  const char* rawPath = std::getenv("PATH");
  if (rawPath == nullptr) {
    return std::nullopt;
  }

#ifdef _WIN32
  constexpr char separator = ';';
#else
  constexpr char separator = ':';
#endif

  std::string_view path(rawPath);
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t end = path.find(separator, start);
    const std::string_view entry = end == std::string_view::npos
                                       ? path.substr(start)
                                       : path.substr(start, end - start);
    if (!entry.empty()) {
      std::filesystem::path candidate = std::filesystem::path(entry) / executable;
      if (fileExists(candidate)) {
        return candidate;
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }

  return std::nullopt;
}

std::optional<std::filesystem::path> discoverLld() {
  if (const char* explicitLld = std::getenv("CPP_SCALANATIVE_LLD")) {
    if (std::optional<std::filesystem::path> lld = findOnPath(explicitLld)) {
      return lld;
    }
  }

#ifdef _WIN32
  constexpr std::string_view candidates[] = {"lld-link", "ld.lld"};
#elif __APPLE__
  constexpr std::string_view candidates[] = {"ld64.lld", "ld.lld", "lld"};
#else
  constexpr std::string_view candidates[] = {"ld.lld", "lld"};
#endif
  for (const std::string_view candidate : candidates) {
    if (std::optional<std::filesystem::path> lld = findOnPath(candidate)) {
      return lld;
    }
  }
  return std::nullopt;
}

std::vector<std::string> clangBaseArgs(const NativeToolchain& toolchain,
                                       const BuildOptions& options) {
  std::vector<std::string> args;
  args.push_back(toolchain.clang.string());
  args.push_back("-Wno-override-module");
  args.push_back(
      "-" + std::string(optimizationLevelName(effectiveOptimizationLevel(options))));
  if (!options.targetTriple.empty()) {
    args.push_back("--target=" + options.targetTriple);
  }
  if (!options.sysroot.empty()) {
    args.push_back("--sysroot=" + options.sysroot.string());
  }
  return args;
}

void addLinkMode(std::vector<std::string>& args, const BuildOptions& options) {
  if (options.linkMode == LinkMode::Static) {
    args.push_back("-static");
  }
}

std::vector<std::string> libraryFilenames(std::string_view name, LinkMode mode) {
  if (mode == LinkMode::Static) {
    return {"lib" + std::string(name) + ".a"};
  }
#ifdef _WIN32
  return {std::string(name) + ".lib", "lib" + std::string(name) + ".a"};
#elif __APPLE__
  return {"lib" + std::string(name) + ".dylib", "lib" + std::string(name) + ".a"};
#else
  return {"lib" + std::string(name) + ".so", "lib" + std::string(name) + ".a"};
#endif
}

std::optional<std::filesystem::path> resolveLibrary(const NativeToolchain& toolchain,
                                                    const BuildOptions& options,
                                                    std::string_view name) {
  for (const std::string& filename : libraryFilenames(name, options.linkMode)) {
    std::vector<std::string> args{toolchain.clang.string()};
    if (!options.targetTriple.empty()) {
      args.push_back("--target=" + options.targetTriple);
    }
    if (!options.sysroot.empty()) {
      args.push_back("--sysroot=" + options.sysroot.string());
    }
    args.push_back("--print-file-name=" + filename);
    ToolchainInvocation query = runTool(args);
    if (!query.ok) {
      continue;
    }
    const std::string resolved = trim(std::move(query.output));
    if (resolved.empty() || resolved == filename) {
      continue;
    }
    const std::filesystem::path path(resolved);
    std::error_code regularError;
    if (!std::filesystem::is_regular_file(path, regularError) || regularError) {
      continue;
    }
    std::error_code canonicalError;
    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(path, canonicalError);
    return canonicalError ? path : canonical;
  }
  return std::nullopt;
}

void addNativeLinkInputs(std::vector<std::string>& args, const BuildOptions& options) {
  for (const std::string& runtimeLibrary : options.runtimeLibraries) {
    args.push_back(runtimeLibrary);
  }

  for (const std::string& linkLibrary : options.linkLibraries) {
    if (linkLibrary.empty()) {
      continue;
    }
    const std::filesystem::path asPath(linkLibrary);
    if (linkLibrary.front() == '-' || asPath.has_parent_path()) {
      args.push_back(linkLibrary);
    } else {
      args.push_back("-l" + linkLibrary);
    }
  }
}

std::vector<std::string> nativeBinaryLinkArgs(const NativeToolchain& toolchain,
                                              const std::filesystem::path& inputPath,
                                              const std::filesystem::path& binaryPath,
                                              const BuildOptions& options) {
  std::vector<std::string> args = clangBaseArgs(toolchain, options);
  if (options.linkerMode == LinkerMode::Lld && toolchain.lld.has_value()) {
    args.push_back("-fuse-ld=" + toolchain.lld->string());
  }
  addLinkMode(args, options);
  args.push_back(inputPath.string());
#ifndef _WIN32
  args.push_back("-lm");
#endif
  addNativeLinkInputs(args, options);
  args.push_back("-o");
  args.push_back(binaryPath.string());
  return args;
}

std::vector<std::string> parseQuotedCommand(std::string_view line) {
  std::vector<std::string> arguments;
  std::string current;
  char quote = '\0';
  bool escaped = false;
  bool active = false;
  for (const char ch : line) {
    if (escaped) {
      current.push_back(ch);
      escaped = false;
      active = true;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      active = true;
      continue;
    }
    if (quote != '\0') {
      if (ch == quote) {
        quote = '\0';
      } else {
        current.push_back(ch);
      }
      active = true;
      continue;
    }
    if (ch == '\'' || ch == '"') {
      quote = ch;
      active = true;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
      if (active) {
        arguments.push_back(std::move(current));
        current.clear();
        active = false;
      }
      continue;
    }
    current.push_back(ch);
    active = true;
  }
  if (escaped) {
    current.push_back('\\');
  }
  if (active) {
    arguments.push_back(std::move(current));
  }
  return arguments;
}

std::optional<std::filesystem::path>
canonicalRegularFile(const std::filesystem::path& path) {
  std::error_code regularError;
  if (!std::filesystem::is_regular_file(path, regularError) || regularError) {
    return std::nullopt;
  }
  std::error_code canonicalError;
  const std::filesystem::path canonical =
      std::filesystem::weakly_canonical(path, canonicalError);
  return canonicalError ? path : canonical;
}

std::optional<std::filesystem::path>
resolvePlanFile(const std::filesystem::path& path,
                const std::vector<std::filesystem::path>& searchDirectories,
                const BuildOptions& options, bool requireSysroot = false) {
  if (!options.sysroot.empty() && path.is_absolute()) {
    if (std::optional<std::filesystem::path> resolved =
            canonicalRegularFile(options.sysroot / path.relative_path())) {
      return resolved;
    }
  }
  if (!requireSysroot) {
    if (std::optional<std::filesystem::path> resolved = canonicalRegularFile(path)) {
      return resolved;
    }
  }
  if (!path.is_absolute()) {
    for (const std::filesystem::path& directory : searchDirectories) {
      if (std::optional<std::filesystem::path> resolved =
              canonicalRegularFile(directory / path)) {
        return resolved;
      }
    }
  }
  return std::nullopt;
}

bool isRequiredPlanFile(std::string_view argument) {
  const std::string extension = std::filesystem::path(argument).extension().string();
  return extension == ".o" || extension == ".obj" || extension == ".a" ||
         extension == ".lib" || extension == ".so" || extension == ".dylib";
}

void addSysrootSearchDirectories(std::vector<std::filesystem::path>& searchDirectories,
                                 const BuildOptions& options) {
  if (options.sysroot.empty()) {
    return;
  }
  const std::filesystem::path& root = options.sysroot;
  searchDirectories.push_back(root / "lib");
  searchDirectories.push_back(root / "lib64");
  searchDirectories.push_back(root / "usr/lib");
  searchDirectories.push_back(root / "usr/lib64");
  if (!options.targetTriple.empty()) {
    searchDirectories.push_back(root / "lib" / options.targetTriple);
    searchDirectories.push_back(root / "usr/lib" / options.targetTriple);
  }
}

std::optional<std::filesystem::path>
resolvePlanLibrary(std::string_view name,
                   const std::vector<std::filesystem::path>& searchDirectories,
                   const NativeToolchain& toolchain, const BuildOptions& options) {
  for (const std::filesystem::path& directory : searchDirectories) {
    for (const std::string& filename : libraryFilenames(name, options.linkMode)) {
      if (std::optional<std::filesystem::path> resolved =
              canonicalRegularFile(directory / filename)) {
        return resolved;
      }
    }
  }
  if (!options.sysroot.empty()) {
    return std::nullopt;
  }
  return resolveLibrary(toolchain, options, name);
}

bool samePath(const std::filesystem::path& left, const std::filesystem::path& right) {
  std::error_code leftError;
  const std::filesystem::path canonicalLeft =
      std::filesystem::weakly_canonical(left, leftError);
  std::error_code rightError;
  const std::filesystem::path canonicalRight =
      std::filesystem::weakly_canonical(right, rightError);
  return (leftError ? left : canonicalLeft) == (rightError ? right : canonicalRight);
}

} // namespace

std::optional<NativeToolchain> discoverNativeToolchain() {
  if (const char* explicitClang = std::getenv("CPP_SCALANATIVE_CLANG")) {
    if (std::optional<std::filesystem::path> clang = findOnPath(explicitClang)) {
      return NativeToolchain{*clang, discoverLld()};
    }
  }

  if (std::optional<std::filesystem::path> clang = findOnPath("clang")) {
    return NativeToolchain{*clang, discoverLld()};
  }
  return std::nullopt;
}

std::string nativeToolchainFingerprint(const NativeToolchain& toolchain) {
  std::ostringstream identity;
  std::error_code canonicalError;
  const std::filesystem::path canonical =
      std::filesystem::weakly_canonical(toolchain.clang, canonicalError);
  identity << (canonicalError ? toolchain.clang : canonical).string() << '\n';

  ToolchainInvocation version = runTool({toolchain.clang.string(), "--version"});
  if (version.ok) {
    identity << version.output;
  } else {
    std::error_code sizeError;
    const std::uintmax_t size = std::filesystem::file_size(toolchain.clang, sizeError);
    std::error_code timeError;
    const auto modified = std::filesystem::last_write_time(toolchain.clang, timeError);
    identity << "size=" << (sizeError ? 0 : size) << '\n';
    identity << "modified=" << (timeError ? 0 : modified.time_since_epoch().count())
             << '\n';
  }
  return identity.str();
}

NativeLinkResolution resolveNativeLinkInputs(const NativeToolchain& toolchain,
                                             const BuildOptions& options) {
  NativeLinkResolution result;
  auto addLibrary = [&](std::string category, std::string argument, std::string name) {
    std::optional<std::filesystem::path> resolved =
        resolveLibrary(toolchain, options, name);
    result.inputs.push_back(ResolvedNativeLinkInput{
        std::move(category), std::move(argument), std::move(resolved)});
    if (options.linkMode == LinkMode::Static &&
        !result.inputs.back().resolvedPath.has_value()) {
      result.missingStaticLibraries.push_back("lib" + name + ".a");
    }
  };
  auto addPath = [&](std::string category, const std::string& argument) {
    const std::filesystem::path path(argument);
    std::error_code regularError;
    const bool regular =
        std::filesystem::is_regular_file(path, regularError) && !regularError;
    std::optional<std::filesystem::path> resolved;
    if (regular) {
      std::error_code canonicalError;
      const std::filesystem::path canonical =
          std::filesystem::weakly_canonical(path, canonicalError);
      resolved = canonicalError ? path : canonical;
    }
    result.inputs.push_back(
        ResolvedNativeLinkInput{std::move(category), argument, std::move(resolved)});
    const std::string extension = path.extension().string();
    const bool staticLinkFile = regular && (extension == ".a" || extension == ".o" ||
                                            extension == ".obj" || extension == ".lib");
    if (options.linkMode == LinkMode::Static && !staticLinkFile) {
      result.missingStaticLibraries.push_back(argument);
    }
  };

  // Clang supplies libc implicitly; libm is explicit in the generated link.
  addLibrary("system", "<implicit libc>", "c");
#ifndef _WIN32
  addLibrary("implicit", "-lm", "m");
#endif
  for (const std::string& runtimeLibrary : options.runtimeLibraries) {
    addPath("runtime", runtimeLibrary);
  }
  for (const std::string& linkLibrary : options.linkLibraries) {
    if (linkLibrary.empty()) {
      continue;
    }
    if (linkLibrary.starts_with("-l") && linkLibrary.size() > 2) {
      addLibrary("link", linkLibrary, linkLibrary.substr(2));
      continue;
    }
    const std::filesystem::path asPath(linkLibrary);
    if (linkLibrary.front() != '-' && !asPath.has_parent_path()) {
      addLibrary("link", "-l" + linkLibrary, linkLibrary);
      continue;
    }
    if (linkLibrary.front() == '-') {
      result.inputs.push_back(
          ResolvedNativeLinkInput{"argument", linkLibrary, std::nullopt});
    } else {
      addPath("link", linkLibrary);
    }
  }
  return result;
}

NativeLinkPlan inspectNativeLinkPlan(const NativeToolchain& toolchain,
                                     const std::filesystem::path& objectPath,
                                     const std::filesystem::path& binaryPath,
                                     const BuildOptions& options) {
  std::vector<std::string> probe =
      nativeBinaryLinkArgs(toolchain, objectPath, binaryPath, options);
  probe.insert(probe.begin() + 1, "-###");
  const ToolchainInvocation invocation = runTool(probe);
  if (!invocation.ok) {
    return NativeLinkPlan{false, {}, {}, {}, {}, invocation.output};
  }

  std::vector<std::string> linkerArguments;
  std::istringstream lines(invocation.output);
  std::string line;
  while (std::getline(lines, line)) {
    const std::string candidate = trim(line);
    if (candidate.empty() || (candidate.front() != '"' && candidate.front() != '\'')) {
      continue;
    }
    std::vector<std::string> parsed = parseQuotedCommand(candidate);
    if (parsed.size() > 1) {
      linkerArguments = std::move(parsed);
    }
  }
  if (linkerArguments.empty()) {
    return NativeLinkPlan{false, {}, {},
                          {},    {}, "clang did not report an implicit linker command"};
  }

  std::vector<std::filesystem::path> searchDirectories;
  addSysrootSearchDirectories(searchDirectories, options);
  for (const std::string& argument : linkerArguments) {
    if (argument.starts_with("-L") && argument.size() > 2) {
      std::error_code canonicalError;
      const std::filesystem::path directory(argument.substr(2));
      const std::filesystem::path canonical =
          std::filesystem::weakly_canonical(directory, canonicalError);
      searchDirectories.push_back(canonicalError ? directory : canonical);
    }
  }

  NativeLinkPlan result;
  for (std::size_t index = 0; index < linkerArguments.size(); ++index) {
    const std::string& argument = linkerArguments[index];
    if (argument == "-o" && index + 1 < linkerArguments.size()) {
      ++index;
      continue;
    }
    if (samePath(argument, objectPath)) {
      result.normalizedArguments.push_back("<cached-object>");
      continue;
    }
    if (argument.starts_with("-L") && argument.size() > 2) {
      const std::filesystem::path directory(argument.substr(2));
      std::error_code canonicalError;
      const std::filesystem::path canonical =
          std::filesystem::weakly_canonical(directory, canonicalError);
      result.normalizedArguments.push_back(
          "-L" + (canonicalError ? directory : canonical).string());
      continue;
    }
    if (argument.starts_with("-l") && argument.size() > 2) {
      result.normalizedArguments.push_back(argument);
      std::optional<std::filesystem::path> resolved = resolvePlanLibrary(
          std::string_view(argument).substr(2), searchDirectories, toolchain, options);
      result.resolvedInputs.push_back(
          ResolvedNativeLinkInput{"link-plan-library", argument, resolved});
      if (!resolved.has_value()) {
        result.unresolvedLibraries.push_back(argument);
      }
      continue;
    }
    const bool dynamicLoader =
        index > 0 && (linkerArguments[index - 1] == "-dynamic-linker" ||
                      linkerArguments[index - 1] == "--dynamic-linker");
    const bool requiredFile =
        index == 0 || dynamicLoader || isRequiredPlanFile(argument);
    if (std::optional<std::filesystem::path> resolved =
            resolvePlanFile(argument, searchDirectories, options,
                            dynamicLoader && !options.sysroot.empty())) {
      result.normalizedArguments.push_back("<resolved-file>");
      result.resolvedInputs.push_back(ResolvedNativeLinkInput{
          dynamicLoader ? "link-plan-loader"
                        : (index == 0 ? "link-plan-linker" : "link-plan-file"),
          argument, resolved});
      continue;
    }
    if (requiredFile) {
      result.unresolvedFiles.push_back(argument);
    }
    result.normalizedArguments.push_back(argument);
  }
  result.ok = result.unresolvedLibraries.empty() && result.unresolvedFiles.empty();
  if (!result.ok) {
    std::ostringstream error;
    error << "could not resolve linker plan inputs";
    if (!result.unresolvedFiles.empty()) {
      error << "; files:";
      for (const std::string& unresolved : result.unresolvedFiles) {
        error << ' ' << unresolved;
      }
    }
    if (!result.unresolvedLibraries.empty()) {
      error << "; libraries:";
      for (const std::string& unresolved : result.unresolvedLibraries) {
        error << ' ' << unresolved;
      }
    }
    result.error = error.str();
  }
  return result;
}

NativeTargetProbe probeNativeTarget(const NativeToolchain& toolchain,
                                    const BuildOptions& options) {
  std::vector<std::string> args{toolchain.clang.string()};
  if (!options.targetTriple.empty()) {
    args.push_back("--target=" + options.targetTriple);
  }
  if (!options.sysroot.empty()) {
    args.push_back("--sysroot=" + options.sysroot.string());
  }
  args.push_back("--print-target-triple");
  const ToolchainInvocation invocation = runTool(args);
  if (!invocation.ok) {
    return NativeTargetProbe{false, {}, trim(invocation.output)};
  }
  const std::string triple = trim(invocation.output);
  if (triple.empty()) {
    return NativeTargetProbe{false, {}, "clang reported an empty target triple"};
  }
  return NativeTargetProbe{true, triple, {}};
}

NativeLinkPlan probeNativeLinkPlan(const NativeToolchain& toolchain,
                                   const BuildOptions& options) {
  static std::atomic<std::uint64_t> sequence{0};
  const std::uint64_t nonce =
      static_cast<std::uint64_t>(
          std::chrono::steady_clock::now().time_since_epoch().count()) ^
      sequence.fetch_add(1, std::memory_order_relaxed);
  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path objectPath =
      temporary / ("cpp-scalanative-link-probe-" + std::to_string(nonce) + ".o");
  const std::filesystem::path binaryPath =
      temporary / ("cpp-scalanative-link-probe-" + std::to_string(nonce));
  {
    std::ofstream object(objectPath, std::ios::binary | std::ios::trunc);
    if (!object) {
      return NativeLinkPlan{false, {}, {},
                            {},    {}, "could not create temporary linker probe input"};
    }
  }
  NativeLinkPlan plan =
      inspectNativeLinkPlan(toolchain, objectPath, binaryPath, options);
  std::error_code ignored;
  std::filesystem::remove(objectPath, ignored);
  std::filesystem::remove(binaryPath, ignored);
  std::optional<std::filesystem::path> linker;
  for (const ResolvedNativeLinkInput& input : plan.resolvedInputs) {
    if (input.category == "link-plan-linker" && input.resolvedPath.has_value()) {
      // Preserve argv[0]: multicall linkers such as LLD select their mode from it.
      linker = input.argument;
      break;
    }
  }
  std::vector<std::string> compatibilityArgs;
  for (std::size_t index = 0; index + 1 < plan.normalizedArguments.size(); ++index) {
    if (plan.normalizedArguments[index] == "-m") {
      compatibilityArgs = {"-m", plan.normalizedArguments[index + 1]};
      break;
    }
  }
  if (!linker.has_value() || compatibilityArgs.empty()) {
    return plan;
  }

  std::vector<std::string> linkerProbe{linker->string(), "--version"};
  linkerProbe.insert(linkerProbe.end(), compatibilityArgs.begin(),
                     compatibilityArgs.end());
  const ToolchainInvocation linkerInvocation = runTool(linkerProbe);
  if (!linkerInvocation.ok) {
    plan.ok = false;
    plan.error = "selected linker '" + linker->string() +
                 "' does not support target emulation '" + compatibilityArgs[1] +
                 "': " + trim(linkerInvocation.output);
  }
  return plan;
}

ToolchainInvocation compileLlvmToObject(const NativeToolchain& toolchain,
                                        const std::filesystem::path& llvmIrPath,
                                        const std::filesystem::path& objectPath,
                                        const BuildOptions& options) {
  std::vector<std::string> args = clangBaseArgs(toolchain, options);
  args.push_back("-c");
  args.push_back(llvmIrPath.string());
  args.push_back("-o");
  args.push_back(objectPath.string());
  return runTool(args);
}

ToolchainInvocation linkLlvmToBinary(const NativeToolchain& toolchain,
                                     const std::filesystem::path& llvmIrPath,
                                     const std::filesystem::path& binaryPath,
                                     const BuildOptions& options) {
  return runTool(nativeBinaryLinkArgs(toolchain, llvmIrPath, binaryPath, options));
}

ToolchainInvocation linkObjectToBinary(const NativeToolchain& toolchain,
                                       const std::filesystem::path& objectPath,
                                       const std::filesystem::path& binaryPath,
                                       const BuildOptions& options) {
  return runTool(nativeBinaryLinkArgs(toolchain, objectPath, binaryPath, options));
}

} // namespace scalanative::tools::build
