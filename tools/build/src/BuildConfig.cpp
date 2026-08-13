#include "scalanative/tools/build/BuildConfig.h"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace scalanative::tools::build {

namespace {

enum class JsonKind { Null, Boolean, Number, String, Array, Object };

struct JsonValue {
  JsonKind kind = JsonKind::Null;
  bool boolean = false;
  std::string text;
  std::vector<JsonValue> array;
  std::map<std::string, JsonValue> object;
};

class JsonParser {
public:
  JsonParser(std::string_view input, std::string path)
      : input_(input), path_(std::move(path)) {}

  std::optional<JsonValue> parse() {
    skipWhitespace();
    std::optional<JsonValue> value = parseValue(0);
    if (!value.has_value()) {
      return std::nullopt;
    }
    skipWhitespace();
    if (offset_ != input_.size()) {
      fail("unexpected trailing JSON input");
      return std::nullopt;
    }
    return value;
  }

  [[nodiscard]] const std::string& error() const {
    return error_;
  }

private:
  std::optional<JsonValue> parseValue(std::size_t depth) {
    if (depth > 64) {
      fail("JSON nesting exceeds 64 levels");
      return std::nullopt;
    }
    skipWhitespace();
    if (offset_ >= input_.size()) {
      fail("expected a JSON value");
      return std::nullopt;
    }
    switch (input_[offset_]) {
    case 'n':
      return parseLiteral("null", JsonValue{});
    case 't': {
      JsonValue value;
      value.kind = JsonKind::Boolean;
      value.boolean = true;
      return parseLiteral("true", std::move(value));
    }
    case 'f': {
      JsonValue value;
      value.kind = JsonKind::Boolean;
      value.boolean = false;
      return parseLiteral("false", std::move(value));
    }
    case '"': {
      std::optional<std::string> string = parseString();
      if (!string.has_value()) {
        return std::nullopt;
      }
      JsonValue value;
      value.kind = JsonKind::String;
      value.text = std::move(*string);
      return value;
    }
    case '[':
      return parseArray(depth + 1);
    case '{':
      return parseObject(depth + 1);
    default:
      if (input_[offset_] == '-' ||
          (input_[offset_] >= '0' && input_[offset_] <= '9')) {
        return parseNumber();
      }
      fail("expected a JSON value");
      return std::nullopt;
    }
  }

  std::optional<JsonValue> parseLiteral(std::string_view literal, JsonValue value) {
    if (input_.substr(offset_, literal.size()) != literal) {
      fail("invalid JSON literal");
      return std::nullopt;
    }
    offset_ += literal.size();
    return value;
  }

  std::optional<JsonValue> parseNumber() {
    const std::size_t start = offset_;
    if (input_[offset_] == '-') {
      ++offset_;
      if (offset_ >= input_.size()) {
        fail("incomplete JSON number");
        return std::nullopt;
      }
    }
    if (input_[offset_] == '0') {
      ++offset_;
    } else if (input_[offset_] >= '1' && input_[offset_] <= '9') {
      while (offset_ < input_.size() && input_[offset_] >= '0' &&
             input_[offset_] <= '9') {
        ++offset_;
      }
    } else {
      fail("invalid JSON number");
      return std::nullopt;
    }
    if (offset_ < input_.size() && input_[offset_] == '.') {
      ++offset_;
      const std::size_t fractionStart = offset_;
      while (offset_ < input_.size() && input_[offset_] >= '0' &&
             input_[offset_] <= '9') {
        ++offset_;
      }
      if (offset_ == fractionStart) {
        fail("invalid JSON number fraction");
        return std::nullopt;
      }
    }
    if (offset_ < input_.size() && (input_[offset_] == 'e' || input_[offset_] == 'E')) {
      ++offset_;
      if (offset_ < input_.size() &&
          (input_[offset_] == '+' || input_[offset_] == '-')) {
        ++offset_;
      }
      const std::size_t exponentStart = offset_;
      while (offset_ < input_.size() && input_[offset_] >= '0' &&
             input_[offset_] <= '9') {
        ++offset_;
      }
      if (offset_ == exponentStart) {
        fail("invalid JSON number exponent");
        return std::nullopt;
      }
    }
    JsonValue value;
    value.kind = JsonKind::Number;
    value.text = std::string(input_.substr(start, offset_ - start));
    return value;
  }

  static int hexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
      return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
      return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
      return ch - 'A' + 10;
    }
    return -1;
  }

  std::optional<std::uint32_t> parseUnicodeEscape() {
    if (offset_ + 4 > input_.size()) {
      fail("incomplete JSON Unicode escape");
      return std::nullopt;
    }
    std::uint32_t codepoint = 0;
    for (std::size_t index = 0; index < 4; ++index) {
      const int digit = hexValue(input_[offset_ + index]);
      if (digit < 0) {
        fail("invalid JSON Unicode escape");
        return std::nullopt;
      }
      codepoint = (codepoint << 4U) | static_cast<std::uint32_t>(digit);
    }
    offset_ += 4;
    return codepoint;
  }

  static void appendUtf8(std::string& output, std::uint32_t codepoint) {
    if (codepoint <= 0x7fU) {
      output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffU) {
      output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
      output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0xffffU) {
      output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
      output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
      output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else {
      output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
      output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
      output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
      output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
  }

  std::optional<std::string> parseString() {
    ++offset_;
    std::string result;
    while (offset_ < input_.size()) {
      const char ch = input_[offset_++];
      if (ch == '"') {
        return result;
      }
      if (static_cast<unsigned char>(ch) < 0x20U) {
        fail("unescaped control character in JSON string");
        return std::nullopt;
      }
      if (ch != '\\') {
        result.push_back(ch);
        continue;
      }
      if (offset_ >= input_.size()) {
        fail("incomplete JSON string escape");
        return std::nullopt;
      }
      const char escaped = input_[offset_++];
      switch (escaped) {
      case '"':
      case '\\':
      case '/':
        result.push_back(escaped);
        break;
      case 'b':
        result.push_back('\b');
        break;
      case 'f':
        result.push_back('\f');
        break;
      case 'n':
        result.push_back('\n');
        break;
      case 'r':
        result.push_back('\r');
        break;
      case 't':
        result.push_back('\t');
        break;
      case 'u': {
        std::optional<std::uint32_t> codepoint = parseUnicodeEscape();
        if (!codepoint.has_value()) {
          return std::nullopt;
        }
        if (*codepoint >= 0xd800U && *codepoint <= 0xdbffU) {
          if (offset_ + 2 > input_.size() || input_[offset_] != '\\' ||
              input_[offset_ + 1] != 'u') {
            fail("high surrogate must be followed by a low surrogate");
            return std::nullopt;
          }
          offset_ += 2;
          std::optional<std::uint32_t> low = parseUnicodeEscape();
          if (!low.has_value() || *low < 0xdc00U || *low > 0xdfffU) {
            fail("invalid low surrogate in JSON string");
            return std::nullopt;
          }
          *codepoint = 0x10000U + ((*codepoint - 0xd800U) << 10U) + (*low - 0xdc00U);
        } else if (*codepoint >= 0xdc00U && *codepoint <= 0xdfffU) {
          fail("unexpected low surrogate in JSON string");
          return std::nullopt;
        }
        appendUtf8(result, *codepoint);
        break;
      }
      default:
        fail("invalid JSON string escape");
        return std::nullopt;
      }
    }
    fail("unterminated JSON string");
    return std::nullopt;
  }

  std::optional<JsonValue> parseArray(std::size_t depth) {
    ++offset_;
    JsonValue result;
    result.kind = JsonKind::Array;
    skipWhitespace();
    if (consume(']')) {
      return result;
    }
    while (true) {
      std::optional<JsonValue> value = parseValue(depth);
      if (!value.has_value()) {
        return std::nullopt;
      }
      result.array.push_back(std::move(*value));
      skipWhitespace();
      if (consume(']')) {
        return result;
      }
      if (!consume(',')) {
        fail("expected ',' or ']' in JSON array");
        return std::nullopt;
      }
    }
  }

  std::optional<JsonValue> parseObject(std::size_t depth) {
    ++offset_;
    JsonValue result;
    result.kind = JsonKind::Object;
    skipWhitespace();
    if (consume('}')) {
      return result;
    }
    while (true) {
      skipWhitespace();
      if (offset_ >= input_.size() || input_[offset_] != '"') {
        fail("expected a string key in JSON object");
        return std::nullopt;
      }
      std::optional<std::string> key = parseString();
      if (!key.has_value()) {
        return std::nullopt;
      }
      skipWhitespace();
      if (!consume(':')) {
        fail("expected ':' after JSON object key");
        return std::nullopt;
      }
      std::optional<JsonValue> value = parseValue(depth);
      if (!value.has_value()) {
        return std::nullopt;
      }
      if (!result.object.emplace(*key, std::move(*value)).second) {
        fail("duplicate JSON object key '" + *key + "'");
        return std::nullopt;
      }
      skipWhitespace();
      if (consume('}')) {
        return result;
      }
      if (!consume(',')) {
        fail("expected ',' or '}' in JSON object");
        return std::nullopt;
      }
    }
  }

  bool consume(char expected) {
    if (offset_ < input_.size() && input_[offset_] == expected) {
      ++offset_;
      return true;
    }
    return false;
  }

  void skipWhitespace() {
    while (offset_ < input_.size() &&
           (input_[offset_] == ' ' || input_[offset_] == '\t' ||
            input_[offset_] == '\r' || input_[offset_] == '\n')) {
      ++offset_;
    }
  }

  void fail(std::string message) {
    if (!error_.empty()) {
      return;
    }
    std::size_t line = 1;
    std::size_t column = 1;
    for (std::size_t index = 0; index < offset_ && index < input_.size(); ++index) {
      if (input_[index] == '\n') {
        ++line;
        column = 1;
      } else {
        ++column;
      }
    }
    error_ = path_ + ':' + std::to_string(line) + ':' + std::to_string(column) + ": " +
             std::move(message);
  }

  std::string_view input_;
  std::string path_;
  std::size_t offset_ = 0;
  std::string error_;
};

std::optional<std::string> readFile(const std::filesystem::path& path) {
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

std::filesystem::path absoluteNormalized(const std::filesystem::path& path) {
  std::error_code absoluteError;
  const std::filesystem::path absolute = std::filesystem::absolute(path, absoluteError);
  if (absoluteError) {
    return path.lexically_normal();
  }
  std::error_code canonicalError;
  const std::filesystem::path canonical =
      std::filesystem::weakly_canonical(absolute, canonicalError);
  return (canonicalError ? absolute : canonical).lexically_normal();
}

std::filesystem::path resolvePath(const std::filesystem::path& base,
                                  std::string_view value) {
  if (value.empty()) {
    return {};
  }
  const std::filesystem::path path(value);
  return absoluteNormalized(path.is_absolute() ? path : base / path);
}

const JsonValue* find(const JsonValue& object, std::string_view key) {
  const auto found = object.object.find(std::string(key));
  return found == object.object.end() ? nullptr : &found->second;
}

bool readString(const JsonValue& root, std::string_view key, std::string& output,
                std::string& error) {
  const JsonValue* value = find(root, key);
  if (value == nullptr) {
    return true;
  }
  if (value->kind != JsonKind::String) {
    error = "configuration key '" + std::string(key) + "' must be a string";
    return false;
  }
  output = value->text;
  return true;
}

bool readBoolean(const JsonValue& root, std::string_view key, bool& output,
                 std::string& error) {
  const JsonValue* value = find(root, key);
  if (value == nullptr) {
    return true;
  }
  if (value->kind != JsonKind::Boolean) {
    error = "configuration key '" + std::string(key) + "' must be a boolean";
    return false;
  }
  output = value->boolean;
  return true;
}

bool readInteger(const JsonValue& root, std::string_view key, std::int64_t& output,
                 std::string& error, bool required = false) {
  const JsonValue* value = find(root, key);
  if (value == nullptr) {
    if (required) {
      error = "configuration key '" + std::string(key) + "' is required";
      return false;
    }
    return true;
  }
  if (value->kind != JsonKind::Number ||
      value->text.find_first_of(".eE") != std::string::npos) {
    error = "configuration key '" + std::string(key) + "' must be an integer";
    return false;
  }
  const char* begin = value->text.data();
  const char* end = begin + value->text.size();
  const auto parsed = std::from_chars(begin, end, output);
  if (parsed.ec != std::errc{} || parsed.ptr != end) {
    error = "configuration key '" + std::string(key) + "' is out of range";
    return false;
  }
  return true;
}

bool readStringArray(const JsonValue& root, std::string_view key,
                     std::vector<std::string>& output, std::string& error) {
  const JsonValue* value = find(root, key);
  if (value == nullptr) {
    return true;
  }
  if (value->kind != JsonKind::Array) {
    error = "configuration key '" + std::string(key) + "' must be an array";
    return false;
  }
  for (const JsonValue& element : value->array) {
    if (element.kind != JsonKind::String) {
      error = "configuration key '" + std::string(key) + "' must contain only strings";
      return false;
    }
    output.push_back(element.text);
  }
  return true;
}

bool looksLikeLinkPath(std::string_view value) {
  const std::filesystem::path path(value);
  const std::string extension = path.extension().string();
  return path.has_parent_path() || extension == ".a" || extension == ".o" ||
         extension == ".obj" || extension == ".so" || extension == ".dylib" ||
         extension == ".lib";
}

std::string schemaError(const std::filesystem::path& path, std::string message) {
  return path.string() + ": " + std::move(message);
}

} // namespace

BuildConfigLoadResult loadBuildConfiguration(const std::filesystem::path& path) {
  const std::filesystem::path configPath = absoluteNormalized(path);
  const std::optional<std::string> contents = readFile(configPath);
  if (!contents.has_value()) {
    return {std::nullopt,
            "could not read build configuration '" + configPath.string() + "'"};
  }

  JsonParser parser(*contents, configPath.string());
  std::optional<JsonValue> root = parser.parse();
  if (!root.has_value()) {
    return {std::nullopt, parser.error()};
  }
  if (root->kind != JsonKind::Object) {
    return {std::nullopt,
            schemaError(configPath, "build configuration must be a JSON object")};
  }

  static const std::set<std::string, std::less<>> knownKeys = {"action",
                                                               "buildReport",
                                                               "cacheDirectory",
                                                               "debugInfo",
                                                               "gc",
                                                               "linkLibraries",
                                                               "linkMode",
                                                               "linker",
                                                               "optimizationLevel",
                                                               "optimizationReport",
                                                               "output",
                                                               "runtimeLibraries",
                                                               "schemaVersion",
                                                               "source",
                                                               "sysroot",
                                                               "target"};
  for (const auto& [key, value] : root->object) {
    (void)value;
    if (!knownKeys.contains(key)) {
      return {std::nullopt,
              schemaError(configPath, "unknown configuration key '" + key + "'")};
    }
  }

  std::string error;
  std::int64_t schemaVersion = 0;
  if (!readInteger(*root, "schemaVersion", schemaVersion, error, true) ||
      schemaVersion != 1) {
    if (error.empty()) {
      error =
          "unsupported schemaVersion " + std::to_string(schemaVersion) + "; expected 1";
    }
    return {std::nullopt, schemaError(configPath, std::move(error))};
  }

  BuildConfiguration configuration;
  configuration.options.configurationPath = configPath;
  const std::filesystem::path base = configPath.parent_path();
  std::string value;

  if (!readString(*root, "action", value, error)) {
    return {std::nullopt, schemaError(configPath, std::move(error))};
  }
  if (!value.empty()) {
    if (value == "compile") {
      configuration.options.action = BuildAction::Compile;
    } else if (value == "check") {
      configuration.options.action = BuildAction::Check;
    } else if (value == "emit-nir") {
      configuration.options.action = BuildAction::EmitNir;
    } else if (value == "emit-llvm") {
      configuration.options.action = BuildAction::EmitLlvm;
    } else if (value == "build-object") {
      configuration.options.action = BuildAction::BuildObject;
    } else if (value == "build-binary") {
      configuration.options.action = BuildAction::BuildBinary;
    } else {
      return {std::nullopt, schemaError(configPath, "invalid action '" + value + "'")};
    }
  }

  std::int64_t optimizationLevel = 0;
  if (!readInteger(*root, "optimizationLevel", optimizationLevel, error)) {
    return {std::nullopt, schemaError(configPath, std::move(error))};
  }
  if (find(*root, "optimizationLevel") != nullptr) {
    if (optimizationLevel < 0 || optimizationLevel > 3) {
      return {std::nullopt,
              schemaError(configPath, "optimizationLevel must be 0, 1, 2, or 3")};
    }
    configuration.options.optimizationLevel =
        static_cast<OptimizationLevel>(optimizationLevel);
    configuration.options.optimize = optimizationLevel != 0;
  }
  if (!readBoolean(*root, "debugInfo", configuration.options.debugInfo, error)) {
    return {std::nullopt, schemaError(configPath, std::move(error))};
  }

  value.clear();
  if (!readString(*root, "linkMode", value, error)) {
    return {std::nullopt, schemaError(configPath, std::move(error))};
  }
  if (!value.empty()) {
    if (value == "default" || value == "dynamic") {
      configuration.options.linkMode = LinkMode::Default;
    } else if (value == "static") {
      configuration.options.linkMode = LinkMode::Static;
    } else {
      return {std::nullopt,
              schemaError(configPath, "invalid linkMode '" + value + "'")};
    }
  }

  value.clear();
  if (!readString(*root, "linker", value, error)) {
    return {std::nullopt, schemaError(configPath, std::move(error))};
  }
  if (!value.empty()) {
    if (value == "default" || value == "platform") {
      configuration.options.linkerMode = LinkerMode::Default;
    } else if (value == "lld") {
      configuration.options.linkerMode = LinkerMode::Lld;
    } else {
      return {std::nullopt, schemaError(configPath, "invalid linker '" + value + "'")};
    }
  }

  auto readPlainString = [&](std::string_view key, std::string& target) {
    std::string parsed;
    if (!readString(*root, key, parsed, error)) {
      return false;
    }
    if (find(*root, key) != nullptr) {
      target = std::move(parsed);
    }
    return true;
  };
  if (!readPlainString("target", configuration.options.targetTriple) ||
      !readPlainString("gc", configuration.options.gcMode)) {
    return {std::nullopt, schemaError(configPath, std::move(error))};
  }

  auto readPath = [&](std::string_view key, std::filesystem::path& target) {
    std::string parsed;
    if (!readString(*root, key, parsed, error)) {
      return false;
    }
    if (find(*root, key) != nullptr) {
      target = resolvePath(base, parsed);
    }
    return true;
  };
  if (!readPath("source", configuration.sourcePath) ||
      !readPath("output", configuration.options.outputPath) ||
      !readPath("sysroot", configuration.options.sysroot) ||
      !readPath("cacheDirectory", configuration.options.cacheDirectory) ||
      !readPath("optimizationReport", configuration.options.optimizationReportPath) ||
      !readPath("buildReport", configuration.buildReportPath)) {
    return {std::nullopt, schemaError(configPath, std::move(error))};
  }

  std::vector<std::string> runtimeLibraries;
  if (!readStringArray(*root, "runtimeLibraries", runtimeLibraries, error)) {
    return {std::nullopt, schemaError(configPath, std::move(error))};
  }
  for (const std::string& library : runtimeLibraries) {
    configuration.options.runtimeLibraries.push_back(
        resolvePath(base, library).string());
  }

  std::vector<std::string> linkLibraries;
  if (!readStringArray(*root, "linkLibraries", linkLibraries, error)) {
    return {std::nullopt, schemaError(configPath, std::move(error))};
  }
  for (const std::string& library : linkLibraries) {
    configuration.options.linkLibraries.push_back(
        looksLikeLinkPath(library) ? resolvePath(base, library).string() : library);
  }
  return {std::move(configuration), {}};
}

} // namespace scalanative::tools::build
