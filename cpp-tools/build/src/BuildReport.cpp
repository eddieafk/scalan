#include "scalanative/tools/build/BuildReport.h"

#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string_view>

namespace scalanative::tools::build {

namespace {

void writeJsonString(std::ostream& out, std::string_view text) {
  out << '"';
  for (const char ch : text) {
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
        constexpr char digits[] = "0123456789abcdef";
        const auto byte = static_cast<unsigned char>(ch);
        out << "\\u00" << digits[(byte >> 4U) & 0xfU] << digits[byte & 0xfU];
      } else {
        out << ch;
      }
      break;
    }
  }
  out << '"';
}

void writeNullableString(std::ostream& out, std::string_view text) {
  if (text.empty()) {
    out << "null";
  } else {
    writeJsonString(out, text);
  }
}

void writeStringArray(std::ostream& out, const std::vector<std::string>& values,
                      std::string_view indent) {
  out << "[";
  if (!values.empty()) {
    out << '\n';
  }
  for (std::size_t index = 0; index < values.size(); ++index) {
    out << indent;
    writeJsonString(out, values[index]);
    out << (index + 1 == values.size() ? "\n" : ",\n");
  }
  if (!values.empty()) {
    out << std::string(indent.size() >= 2 ? indent.size() - 2 : 0, ' ');
  }
  out << ']';
}

void writePathArray(std::ostream& out, const std::vector<std::filesystem::path>& paths,
                    std::string_view indent) {
  out << "[";
  if (!paths.empty()) {
    out << '\n';
  }
  for (std::size_t index = 0; index < paths.size(); ++index) {
    out << indent;
    writeJsonString(out, paths[index].string());
    out << (index + 1 == paths.size() ? "\n" : ",\n");
  }
  if (!paths.empty()) {
    out << std::string(indent.size() >= 2 ? indent.size() - 2 : 0, ' ');
  }
  out << ']';
}

void writeRange(std::ostream& out, const BuildSourceRange& range,
                std::string_view indent) {
  if (!range.valid) {
    out << "null";
    return;
  }
  out << "{\n" << indent << "  \"path\": ";
  writeJsonString(out, range.path);
  out << ",\n"
      << indent << "  \"start\": {\"offset\": " << range.startOffset
      << ", \"line\": " << range.startLine << ", \"column\": " << range.startColumn
      << "},\n"
      << indent << "  \"end\": {\"offset\": " << range.endOffset
      << ", \"line\": " << range.endLine << ", \"column\": " << range.endColumn << "}\n"
      << indent << '}';
}

void writeCacheEntry(std::ostream& out, bool applicable, bool enabled, bool hit,
                     std::string_view key) {
  out << "{\"applicable\": " << (applicable ? "true" : "false")
      << ", \"enabled\": " << (enabled ? "true" : "false")
      << ", \"hit\": " << (hit ? "true" : "false") << ", \"key\": ";
  writeNullableString(out, key);
  out << '}';
}

} // namespace

std::string buildReportJson(const BuildResult& result, const BuildOptions& options) {
  std::size_t errors = 0;
  std::size_t warnings = 0;
  for (const BuildDiagnostic& diagnostic : result.diagnostics) {
    errors += diagnostic.severity == "error" ? 1 : 0;
    warnings += diagnostic.severity == "warning" ? 1 : 0;
  }

  const bool frontendCacheEnabled =
      !options.cacheDirectory.empty() && options.action != BuildAction::Check;
  const bool objectApplicable = options.action == BuildAction::BuildObject ||
                                options.action == BuildAction::BuildBinary;
  const bool binaryApplicable = options.action == BuildAction::BuildBinary;

  std::ostringstream out;
  out << "{\n"
      << "  \"schemaVersion\": 1,\n"
      << "  \"compiler\": \"cpp-scalanative\",\n"
      << "  \"ok\": " << (result.ok ? "true" : "false") << ",\n"
      << "  \"action\": ";
  writeJsonString(out, buildActionName(options.action));
  out << ",\n  \"sourcePath\": ";
  writeNullableString(out, result.sourcePath);
  out << ",\n  \"configuration\": {\n"
      << "    \"optimizationLevel\": ";
  writeJsonString(out, optimizationLevelName(effectiveOptimizationLevel(options)));
  out << ",\n    \"debugInfo\": " << (options.debugInfo ? "true" : "false")
      << ",\n    \"targetTriple\": ";
  writeNullableString(out, options.targetTriple);
  out << ",\n    \"sysroot\": ";
  writeNullableString(out, options.sysroot.string());
  out << ",\n    \"gc\": ";
  writeJsonString(out, options.gcMode.empty() ? "hybrid" : options.gcMode);
  out << ",\n    \"linkMode\": ";
  writeJsonString(out, linkModeName(options.linkMode));
  out << ",\n    \"linker\": ";
  writeJsonString(out, linkerModeName(options.linkerMode));
  out << ",\n    \"cacheDirectory\": ";
  writeNullableString(out, options.cacheDirectory.string());
  out << ",\n    \"configurationPath\": ";
  writeNullableString(out, options.configurationPath.string());
  out << ",\n    \"outputPath\": ";
  writeNullableString(out, options.outputPath.string());
  out << ",\n    \"optimizationReportPath\": ";
  writeNullableString(out, options.optimizationReportPath.string());
  out << ",\n    \"runtimeLibraries\": ";
  writeStringArray(out, options.runtimeLibraries, "      ");
  out << ",\n    \"linkLibraries\": ";
  writeStringArray(out, options.linkLibraries, "      ");
  out << "\n  },\n  \"cache\": {\n    \"frontend\": ";
  writeCacheEntry(out, true, frontendCacheEnabled, result.cacheHit, result.cacheKey);
  out << ",\n    \"object\": ";
  writeCacheEntry(out, objectApplicable, objectApplicable && frontendCacheEnabled,
                  result.objectCacheHit, result.objectCacheKey);
  out << ",\n    \"binary\": ";
  writeCacheEntry(out, binaryApplicable, binaryApplicable && frontendCacheEnabled,
                  result.binaryCacheHit, result.linkFingerprint);
  out << "\n  },\n  \"diagnosticCounts\": {\"errors\": " << errors
      << ", \"warnings\": " << warnings << "},\n"
      << "  \"diagnostics\": [";
  if (!result.diagnostics.empty()) {
    out << '\n';
  }
  for (std::size_t index = 0; index < result.diagnostics.size(); ++index) {
    const BuildDiagnostic& diagnostic = result.diagnostics[index];
    out << "    {\n      \"severity\": ";
    writeJsonString(out, diagnostic.severity);
    out << ",\n      \"message\": ";
    writeJsonString(out, diagnostic.message);
    out << ",\n      \"location\": ";
    writeRange(out, diagnostic.range, "      ");
    out << ",\n      \"fixIts\": [";
    if (!diagnostic.fixIts.empty()) {
      out << '\n';
    }
    for (std::size_t fixIndex = 0; fixIndex < diagnostic.fixIts.size(); ++fixIndex) {
      const BuildFixIt& fixIt = diagnostic.fixIts[fixIndex];
      out << "        {\"location\": ";
      writeRange(out, fixIt.range, "        ");
      out << ", \"replacement\": ";
      writeJsonString(out, fixIt.replacement);
      out << '}' << (fixIndex + 1 == diagnostic.fixIts.size() ? "\n" : ",\n");
    }
    if (!diagnostic.fixIts.empty()) {
      out << "      ";
    }
    out << "]\n    }" << (index + 1 == result.diagnostics.size() ? "\n" : ",\n");
  }
  out << "  ],\n  \"phases\": ";
  writeStringArray(out, result.phaseLog, "    ");
  out << ",\n  \"artifacts\": ";
  writePathArray(out, result.producedArtifacts, "    ");
  out << "\n}\n";
  return out.str();
}

} // namespace scalanative::tools::build
