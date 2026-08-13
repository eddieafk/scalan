#pragma once

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#ifndef SCALANATIVE_TEST_ROOT
#error "SCALANATIVE_TEST_ROOT must identify the repository tests directory"
#endif

namespace scalanative::testing {

[[nodiscard]] inline std::string readTextFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

struct TestResource {
  std::string_view version;
  std::string_view kind;
  std::string_view fileName;

  [[nodiscard]] std::filesystem::path path() const {
    return std::filesystem::path(SCALANATIVE_TEST_ROOT) / std::string(version) /
           std::string(kind) / std::string(fileName);
  }

  [[nodiscard]] std::string readText() const {
    return readTextFile(path());
  }
};

[[nodiscard]] inline bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

[[nodiscard]] inline std::size_t countOccurrences(std::string_view haystack,
                                                  std::string_view needle) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while ((offset = haystack.find(needle, offset)) != std::string_view::npos) {
    ++count;
    offset += needle.size();
  }
  return count;
}

inline int fail(std::string_view testName, const std::string& message) {
  std::cerr << testName << ": " << message << '\n';
  return 1;
}

} // namespace scalanative::testing
