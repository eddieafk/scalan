#include "scalanative/support/SourceManager.h"

#include "scalanative/support/Diagnostics.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <string_view>
#include <utility>

namespace scalanative::support {

SourceId SourceManager::addVirtualFile(std::string path, std::string contents) {
  return addPreparedSource(std::move(path), prepare(std::move(contents)), nullptr);
}

SourceId SourceManager::addVirtualFile(std::string path, std::string contents,
                                       DiagnosticEngine& diagnostics) {
  return addPreparedSource(std::move(path), prepare(std::move(contents)),
                           &diagnostics);
}

std::optional<SourceId>
SourceManager::addFile(const std::filesystem::path& path,
                       DiagnosticEngine& diagnostics) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    diagnostics.error(SourceSpan::none(),
                      "unable to open source file: " + path.string());
    return std::nullopt;
  }

  std::string contents((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
  return addVirtualFile(path.string(), std::move(contents), diagnostics);
}

const SourceFile* SourceManager::get(SourceId id) const {
  if (!id.isValid() || id.value() >= files_.size()) {
    return nullptr;
  }
  return &files_[id.value()];
}

std::string_view SourceManager::text(SourceId id) const {
  const SourceFile* file = get(id);
  if (file == nullptr) {
    return {};
  }
  return file->contents;
}

std::pair<std::size_t, std::size_t>
SourceManager::lineColumn(SourceSpan span) const {
  const SourceFile* file = get(span.source);
  if (file == nullptr || file->lineOffsets.empty()) {
    return {0, 0};
  }

  const std::size_t clampedStart = std::min(span.start, file->contents.size());
  auto it = std::upper_bound(file->lineOffsets.begin(), file->lineOffsets.end(),
                             clampedStart);
  std::size_t line = static_cast<std::size_t>(
      std::distance(file->lineOffsets.begin(), it == file->lineOffsets.begin()
                                                  ? it
                                                  : std::prev(it)));
  std::size_t column = clampedStart - file->lineOffsets[line];
  return {line + 1, column + 1};
}

std::string SourceManager::lineText(SourceId id, std::size_t line) const {
  const SourceFile* file = get(id);
  if (file == nullptr || line == 0 || line > file->lineOffsets.size()) {
    return {};
  }

  std::size_t start = file->lineOffsets[line - 1];
  std::size_t end = line < file->lineOffsets.size()
                        ? file->lineOffsets[line] - 1
                        : file->contents.size();
  return file->contents.substr(start, end - start);
}

std::string SourceManager::snippet(SourceSpan span) const {
  const SourceFile* file = get(span.source);
  if (file == nullptr || span.start >= file->contents.size()) {
    return {};
  }

  const std::size_t end =
      std::min(file->contents.size(), span.start + span.length);
  return file->contents.substr(span.start, end - span.start);
}

std::size_t SourceManager::lineCount(SourceId id) const {
  const SourceFile* file = get(id);
  if (file == nullptr) {
    return 0;
  }
  return file->lineOffsets.size();
}

std::uint64_t SourceManager::contentHash(SourceId id) const {
  const SourceFile* file = get(id);
  if (file == nullptr) {
    return 0;
  }
  return file->contentHash;
}

SourceId SourceManager::addPreparedSource(std::string path, PreparedSource source,
                                          DiagnosticEngine* diagnostics) {
  SourceId id(static_cast<SourceId::Value>(files_.size()));
  files_.push_back(SourceFile{id,
                              std::move(path),
                              std::move(source.contents),
                              std::move(source.lineOffsets),
                              source.contentHash,
                              source.hadByteOrderMark,
                              source.normalizedNewlines});

  if (diagnostics != nullptr) {
    for (const Utf8Error& error : source.utf8Errors) {
      diagnostics->error({id, error.offset, error.length}, error.message);
    }
  }

  return id;
}

SourceManager::PreparedSource SourceManager::prepare(std::string contents) {
  PreparedSource prepared;
  std::size_t index = 0;

  if (contents.size() >= 3 &&
      static_cast<unsigned char>(contents[0]) == 0xEF &&
      static_cast<unsigned char>(contents[1]) == 0xBB &&
      static_cast<unsigned char>(contents[2]) == 0xBF) {
    prepared.hadByteOrderMark = true;
    index = 3;
  }

  prepared.contents.reserve(contents.size() - index);
  for (; index < contents.size(); ++index) {
    const char ch = contents[index];
    if (ch == '\r') {
      prepared.normalizedNewlines = true;
      if (index + 1 < contents.size() && contents[index + 1] == '\n') {
        ++index;
      }
      prepared.contents.push_back('\n');
      continue;
    }
    prepared.contents.push_back(ch);
  }

  prepared.lineOffsets = buildLineOffsets(prepared.contents);
  prepared.utf8Errors = validateUtf8(prepared.contents);
  prepared.contentHash = hashContents(prepared.contents);
  return prepared;
}

std::vector<std::size_t>
SourceManager::buildLineOffsets(std::string_view text) {
  std::vector<std::size_t> offsets;
  offsets.push_back(0);
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '\n') {
      offsets.push_back(index + 1);
    }
  }
  return offsets;
}

std::vector<SourceManager::Utf8Error>
SourceManager::validateUtf8(std::string_view text) {
  auto byte = [&](std::size_t index) {
    return static_cast<unsigned char>(text[index]);
  };
  auto isContinuation = [&](std::size_t index) {
    return index < text.size() && (byte(index) & 0xC0U) == 0x80U;
  };
  auto truncated = [&](std::size_t index, std::size_t length) {
    return index + length > text.size();
  };

  std::vector<Utf8Error> errors;
  std::size_t index = 0;
  while (index < text.size()) {
    const unsigned char first = byte(index);
    if (first <= 0x7FU) {
      ++index;
      continue;
    }

    std::size_t length = 0;
    bool valid = false;
    if (first >= 0xC2U && first <= 0xDFU) {
      length = 2;
      valid = !truncated(index, length) && isContinuation(index + 1);
    } else if (first == 0xE0U) {
      length = 3;
      valid = !truncated(index, length) && byte(index + 1) >= 0xA0U &&
              byte(index + 1) <= 0xBFU && isContinuation(index + 2);
    } else if (first >= 0xE1U && first <= 0xECU) {
      length = 3;
      valid = !truncated(index, length) && isContinuation(index + 1) &&
              isContinuation(index + 2);
    } else if (first == 0xEDU) {
      length = 3;
      valid = !truncated(index, length) && byte(index + 1) >= 0x80U &&
              byte(index + 1) <= 0x9FU && isContinuation(index + 2);
    } else if (first >= 0xEEU && first <= 0xEFU) {
      length = 3;
      valid = !truncated(index, length) && isContinuation(index + 1) &&
              isContinuation(index + 2);
    } else if (first == 0xF0U) {
      length = 4;
      valid = !truncated(index, length) && byte(index + 1) >= 0x90U &&
              byte(index + 1) <= 0xBFU && isContinuation(index + 2) &&
              isContinuation(index + 3);
    } else if (first >= 0xF1U && first <= 0xF3U) {
      length = 4;
      valid = !truncated(index, length) && isContinuation(index + 1) &&
              isContinuation(index + 2) && isContinuation(index + 3);
    } else if (first == 0xF4U) {
      length = 4;
      valid = !truncated(index, length) && byte(index + 1) >= 0x80U &&
              byte(index + 1) <= 0x8FU && isContinuation(index + 2) &&
              isContinuation(index + 3);
    }

    if (valid) {
      index += length;
      continue;
    }

    errors.push_back(
        Utf8Error{index, 1, "invalid UTF-8 sequence in source file"});
    ++index;
  }

  return errors;
}

std::uint64_t SourceManager::hashContents(std::string_view text) {
  constexpr std::uint64_t offsetBasis = 14695981039346656037ULL;
  constexpr std::uint64_t prime = 1099511628211ULL;

  std::uint64_t hash = offsetBasis;
  for (const char ch : text) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= prime;
  }
  return hash;
}

} // namespace scalanative::support
