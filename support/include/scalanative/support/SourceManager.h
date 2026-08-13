#pragma once

#include "scalanative/support/Ids.h"
#include "scalanative/support/SourceSpan.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scalanative::support {

class DiagnosticEngine;

struct SourceFile {
  SourceId id;
  std::string path;
  std::string contents;
  std::vector<std::size_t> lineOffsets;
  std::uint64_t contentHash = 0;
  bool hadByteOrderMark = false;
  bool normalizedNewlines = false;
};

class SourceManager {
public:
  [[nodiscard]] SourceId addVirtualFile(std::string path, std::string contents);
  [[nodiscard]] SourceId addVirtualFile(std::string path, std::string contents,
                                        DiagnosticEngine& diagnostics);
  [[nodiscard]] std::optional<SourceId>
  addFile(const std::filesystem::path& path, DiagnosticEngine& diagnostics);

  [[nodiscard]] const SourceFile* get(SourceId id) const;
  [[nodiscard]] std::string_view text(SourceId id) const;
  [[nodiscard]] std::pair<std::size_t, std::size_t>
  lineColumn(SourceSpan span) const;
  [[nodiscard]] std::string lineText(SourceId id, std::size_t line) const;
  [[nodiscard]] std::string snippet(SourceSpan span) const;
  [[nodiscard]] std::size_t lineCount(SourceId id) const;
  [[nodiscard]] std::uint64_t contentHash(SourceId id) const;

private:
  struct Utf8Error {
    std::size_t offset = 0;
    std::size_t length = 1;
    std::string message;
  };

  struct PreparedSource {
    std::string contents;
    std::vector<std::size_t> lineOffsets;
    std::vector<Utf8Error> utf8Errors;
    std::uint64_t contentHash = 0;
    bool hadByteOrderMark = false;
    bool normalizedNewlines = false;
  };

  [[nodiscard]] SourceId addPreparedSource(std::string path,
                                           PreparedSource source,
                                           DiagnosticEngine* diagnostics);
  [[nodiscard]] static PreparedSource prepare(std::string contents);
  static std::vector<std::size_t> buildLineOffsets(std::string_view text);
  [[nodiscard]] static std::vector<Utf8Error>
  validateUtf8(std::string_view text);
  [[nodiscard]] static std::uint64_t hashContents(std::string_view text);

  std::vector<SourceFile> files_;
};

} // namespace scalanative::support
