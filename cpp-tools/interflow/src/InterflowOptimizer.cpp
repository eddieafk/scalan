#include "scalanative/tools/interflow/InterflowOptimizer.h"

#include "scalanative/nir/Builder.h"
#include "scalanative/nir/Verifier.h"
#include "scalanative/support/StdNames.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scalanative::tools::interflow {

namespace {

struct PassEffect {
  std::size_t removedDefinitions = 0;
  std::size_t changedValues = 0;
};

struct InlineReturnSummary {
  std::vector<std::string> parameters;
  nir::Value value;
};

using InlineReturnMap = std::unordered_map<std::string, InlineReturnSummary>;
using DefinitionIndex = std::unordered_map<std::string, const nir::Definition*>;
using ParentMap = std::unordered_map<std::string, std::vector<std::string>>;
using ConstructorFieldMap = std::unordered_map<std::string, std::vector<std::string>>;
using FieldSnapshotMap = std::unordered_map<std::string, std::vector<std::string>>;

std::string ownerNameOf(const std::string& definitionName) {
  const std::size_t dot = definitionName.rfind('.');
  if (dot == std::string::npos) {
    return {};
  }
  return definitionName.substr(0, dot);
}

std::size_t definitionCount(const linker::LinkedProgram& program) {
  std::size_t count = 0;
  for (const nir::Module& module : program.modules) {
    count += module.definitions.size();
  }
  return count;
}

bool isFunctionDefinitionKind(nir::DefinitionKind kind) {
  return kind == nir::DefinitionKind::FunctionDecl ||
         kind == nir::DefinitionKind::FunctionDef;
}

bool isClassLikeDefinitionKind(nir::DefinitionKind kind) {
  return kind == nir::DefinitionKind::Module || kind == nir::DefinitionKind::Class ||
         kind == nir::DefinitionKind::Trait;
}

bool isConcreteDispatchDefinitionKind(nir::DefinitionKind kind) {
  return kind == nir::DefinitionKind::Module || kind == nir::DefinitionKind::Class;
}

DefinitionIndex definitionIndexFor(const linker::LinkedProgram& program) {
  DefinitionIndex definitions;
  for (const nir::Module& module : program.modules) {
    for (const nir::Definition& definition : module.definitions) {
      if (!definition.name.empty()) {
        definitions.emplace(definition.name, &definition);
      }
    }
  }
  return definitions;
}

ParentMap parentMapFor(const DefinitionIndex& definitions) {
  ParentMap parents;
  for (const auto& [name, definition] : definitions) {
    if (definition != nullptr && isClassLikeDefinitionKind(definition->kind)) {
      parents[name] = nir::metadataParentNames(definition->signature);
    }
  }
  return parents;
}

ConstructorFieldMap constructorFieldMapFor(const linker::LinkedProgram& program) {
  ConstructorFieldMap fields;
  for (const nir::Module& module : program.modules) {
    for (const nir::Definition& definition : module.definitions) {
      if (definition.kind == nir::DefinitionKind::Field && definition.body.empty()) {
        fields[ownerNameOf(definition.name)].push_back(definition.name);
      }
    }
  }
  return fields;
}

std::vector<std::string> validateProgram(const linker::LinkedProgram& program) {
  std::vector<std::string> errors;
  nir::Verifier verifier;
  for (const nir::Module& module : program.modules) {
    nir::VerifyResult result = verifier.verify(module);
    for (const std::string& error : result.errors) {
      errors.push_back(module.name + ": " + error);
    }
  }
  return errors;
}

bool isFoldableIntegerType(std::string_view type) {
  return type == "Int" || type == "Long";
}

bool isFoldableFloatingType(std::string_view type) {
  return type == "Float" || type == "Double";
}

bool isUnaryPlusIdentityType(std::string_view type) {
  return type == "Int" || type == "Long" || type == "Float" || type == "Double";
}

bool isSameLocalComparisonType(std::string_view type) {
  return type == "Boolean" || type == "Int" || type == "Long" || type == "Char";
}

bool isOrderedNonFloatingComparisonType(std::string_view type) {
  return type == "Int" || type == "Long" || type == "Char";
}

bool isSameLocalEqualityType(std::string_view type) {
  return !type.empty() && type != "Unknown" && type != "Float" && type != "Double";
}

std::optional<long long> integerLiteralValue(const nir::Value& value) {
  if (value.kind != nir::ValueKind::Literal || !isFoldableIntegerType(value.type)) {
    return std::nullopt;
  }
  try {
    std::string text = value.text;
    if (value.type == "Long" && !text.empty() &&
        (text.back() == 'L' || text.back() == 'l')) {
      text.pop_back();
    }
    std::size_t parsed = 0;
    long long result = std::stoll(text, &parsed, 10);
    if (parsed != text.size()) {
      return std::nullopt;
    }
    return result;
  } catch (...) {
    return std::nullopt;
  }
}

std::string integerLiteralText(long long value, std::string_view type) {
  std::string text = std::to_string(value);
  if (type == "Long") {
    text += 'L';
  }
  return text;
}

std::int64_t signedInt32Value(std::uint32_t bits);

std::int64_t signedInt64Value(std::uint64_t bits) {
  std::int64_t value = 0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

std::uint32_t arithmeticShiftRight32(std::uint32_t bits, std::uint32_t count) {
  if (count == 0) {
    return bits;
  }
  const std::uint32_t shifted = bits >> count;
  return (bits & 0x80000000u) == 0 ? shifted
                                   : shifted | (~std::uint32_t{0} << (32u - count));
}

std::uint64_t arithmeticShiftRight64(std::uint64_t bits, std::uint32_t count) {
  if (count == 0) {
    return bits;
  }
  const std::uint64_t shifted = bits >> count;
  return (bits & 0x8000000000000000ULL) == 0
             ? shifted
             : shifted | (~std::uint64_t{0} << (64u - count));
}

std::optional<long long> foldIntegralBitwiseOrShift(std::string_view operation,
                                                    long long lhs, long long rhs,
                                                    std::string_view lhsType,
                                                    std::string_view rhsType) {
  const bool bitwise = operation == "&" || operation == "|" || operation == "^";
  const bool shift = operation == "<<" || operation == ">>" || operation == ">>>";
  if ((!bitwise && !shift) || (lhsType != "Int" && lhsType != "Long")) {
    return std::nullopt;
  }
  if ((bitwise && rhsType != lhsType) || (shift && rhsType != "Int")) {
    return std::nullopt;
  }

  if (lhsType == "Int") {
    if (lhs < std::numeric_limits<std::int32_t>::min() ||
        lhs > std::numeric_limits<std::int32_t>::max() ||
        rhs < std::numeric_limits<std::int32_t>::min() ||
        rhs > std::numeric_limits<std::int32_t>::max()) {
      return std::nullopt;
    }
    const std::uint32_t lhsBits = static_cast<std::uint32_t>(lhs);
    const std::uint32_t rhsBits = static_cast<std::uint32_t>(rhs);
    std::uint32_t result = 0;
    if (operation == "&") {
      result = lhsBits & rhsBits;
    } else if (operation == "|") {
      result = lhsBits | rhsBits;
    } else if (operation == "^") {
      result = lhsBits ^ rhsBits;
    } else {
      const std::uint32_t count = rhsBits & 31u;
      if (operation == "<<") {
        result = lhsBits << count;
      } else if (operation == ">>") {
        result = arithmeticShiftRight32(lhsBits, count);
      } else {
        result = lhsBits >> count;
      }
    }
    return signedInt32Value(result);
  }

  if (bitwise && rhsType != "Long") {
    return std::nullopt;
  }
  if (shift && (rhs < std::numeric_limits<std::int32_t>::min() ||
                rhs > std::numeric_limits<std::int32_t>::max())) {
    return std::nullopt;
  }
  const std::uint64_t lhsBits = static_cast<std::uint64_t>(lhs);
  const std::uint64_t rhsBits = static_cast<std::uint64_t>(rhs);
  std::uint64_t result = 0;
  if (operation == "&") {
    result = lhsBits & rhsBits;
  } else if (operation == "|") {
    result = lhsBits | rhsBits;
  } else if (operation == "^") {
    result = lhsBits ^ rhsBits;
  } else {
    const std::uint32_t count = static_cast<std::uint32_t>(rhs) & 63u;
    if (operation == "<<") {
      result = lhsBits << count;
    } else if (operation == ">>") {
      result = arithmeticShiftRight64(lhsBits, count);
    } else {
      result = lhsBits >> count;
    }
  }
  return signedInt64Value(result);
}

std::optional<long long> foldIntegralDivisionOrRemainder(std::string_view operation,
                                                         long long lhs, long long rhs,
                                                         std::string_view lhsType,
                                                         std::string_view rhsType) {
  if ((operation != "/" && operation != "%") || lhsType != rhsType || rhs == 0 ||
      (lhsType != "Int" && lhsType != "Long")) {
    return std::nullopt;
  }

  if (lhsType == "Int") {
    if (lhs < std::numeric_limits<std::int32_t>::min() ||
        lhs > std::numeric_limits<std::int32_t>::max() ||
        rhs < std::numeric_limits<std::int32_t>::min() ||
        rhs > std::numeric_limits<std::int32_t>::max()) {
      return std::nullopt;
    }
    const std::int32_t narrowedLhs = static_cast<std::int32_t>(lhs);
    const std::int32_t narrowedRhs = static_cast<std::int32_t>(rhs);
    if (narrowedLhs == std::numeric_limits<std::int32_t>::min() && narrowedRhs == -1) {
      return operation == "/" ? std::numeric_limits<std::int32_t>::min() : 0;
    }
    return operation == "/" ? narrowedLhs / narrowedRhs : narrowedLhs % narrowedRhs;
  }

  const std::int64_t narrowedLhs = static_cast<std::int64_t>(lhs);
  const std::int64_t narrowedRhs = static_cast<std::int64_t>(rhs);
  if (narrowedLhs == std::numeric_limits<std::int64_t>::min() && narrowedRhs == -1) {
    return operation == "/" ? std::numeric_limits<std::int64_t>::min() : 0;
  }
  return operation == "/" ? narrowedLhs / narrowedRhs : narrowedLhs % narrowedRhs;
}

std::optional<double> floatingLiteralValue(const nir::Value& value) {
  if (value.kind != nir::ValueKind::Literal || !isFoldableFloatingType(value.type)) {
    return std::nullopt;
  }
  try {
    std::string text = value.text;
    if (value.type == "Float" && !text.empty() &&
        (text.back() == 'F' || text.back() == 'f')) {
      text.pop_back();
    }
    if (value.type == "Double" && !text.empty() &&
        (text.back() == 'D' || text.back() == 'd')) {
      text.pop_back();
    }
    std::size_t parsed = 0;
    const double result = std::stod(text, &parsed);
    if (parsed != text.size()) {
      return std::nullopt;
    }
    return result;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::uint32_t> floatLiteralBits(const nir::Value& value) {
  if (value.kind != nir::ValueKind::Literal || value.type != "Float") {
    return std::nullopt;
  }
  try {
    std::string text = value.text;
    if (!text.empty() && (text.back() == 'F' || text.back() == 'f')) {
      text.pop_back();
    }
    std::size_t parsed = 0;
    const double widened = std::stod(text, &parsed);
    if (parsed != text.size()) {
      return std::nullopt;
    }
    const float narrowed = static_cast<float>(widened);
    std::uint32_t bits = 0;
    std::memcpy(&bits, &narrowed, sizeof(bits));
    return bits;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::uint64_t> doubleLiteralBits(const nir::Value& value) {
  if (value.kind != nir::ValueKind::Literal || value.type != "Double") {
    return std::nullopt;
  }
  try {
    std::string text = value.text;
    if (!text.empty() && (text.back() == 'D' || text.back() == 'd')) {
      text.pop_back();
    }
    std::size_t parsed = 0;
    const double parsedValue = std::stod(text, &parsed);
    if (parsed != text.size()) {
      return std::nullopt;
    }
    std::uint64_t bits = 0;
    std::memcpy(&bits, &parsedValue, sizeof(bits));
    return bits;
  } catch (...) {
    return std::nullopt;
  }
}

bool isFloatingOneLiteral(const nir::Value& value, std::string_view expectedType) {
  if (value.type != expectedType || !isFoldableFloatingType(expectedType)) {
    return false;
  }
  const std::optional<double> literal = floatingLiteralValue(value);
  return literal && *literal == 1.0;
}

std::string floatingUnaryLiteralText(std::string_view op, const nir::Value& operand) {
  std::string text = operand.text;
  if (op == "+") {
    return text;
  }
  if (!text.empty() && text.front() == '-') {
    text.erase(text.begin());
    return text;
  }
  if (!text.empty() && text.front() == '+') {
    text.front() = '-';
    return text;
  }
  return "-" + text;
}

std::optional<bool> booleanLiteralValue(const nir::Value& value) {
  if (value.kind != nir::ValueKind::Literal || value.type != "Boolean") {
    return std::nullopt;
  }
  if (value.text == "true") {
    return true;
  }
  if (value.text == "false") {
    return false;
  }
  return std::nullopt;
}

std::optional<int> charLiteralValue(const nir::Value& value) {
  if (value.kind != nir::ValueKind::Literal || value.type != "Char" ||
      value.text.size() < 3 || value.text.front() != '\'' ||
      value.text.back() != '\'') {
    return std::nullopt;
  }
  std::string_view text(value.text);
  text.remove_prefix(1);
  text.remove_suffix(1);
  if (text.size() == 1) {
    return static_cast<unsigned char>(text.front());
  }
  if (text.size() != 2 || text.front() != '\\') {
    return std::nullopt;
  }
  switch (text.back()) {
  case 'n':
    return '\n';
  case 'r':
    return '\r';
  case 't':
    return '\t';
  case '\\':
    return '\\';
  case '\'':
    return '\'';
  default:
    return std::nullopt;
  }
}

std::optional<std::string> stringLiteralValue(const nir::Value& value) {
  if (value.kind != nir::ValueKind::Literal || value.type != "String") {
    return std::nullopt;
  }
  std::string_view text(value.text);
  if (text.starts_with("\"\"\"") && text.ends_with("\"\"\"") && text.size() >= 6) {
    return std::string(text.substr(3, text.size() - 6));
  }
  if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
    return std::nullopt;
  }

  std::string decoded;
  for (std::size_t i = 1; i + 1 < text.size(); ++i) {
    char ch = text[i];
    if (ch != '\\' || i + 2 >= text.size()) {
      decoded.push_back(ch);
      continue;
    }

    ++i;
    switch (text[i]) {
    case 'n':
      decoded.push_back('\n');
      break;
    case 'r':
      decoded.push_back('\r');
      break;
    case 't':
      decoded.push_back('\t');
      break;
    case '"':
      decoded.push_back('"');
      break;
    case '\\':
      decoded.push_back('\\');
      break;
    default:
      decoded.push_back(text[i]);
      break;
    }
  }
  return decoded;
}

std::string escapedStringLiteralText(std::string_view value) {
  std::string text;
  text.push_back('"');
  for (char ch : value) {
    switch (ch) {
    case '\n':
      text += "\\n";
      break;
    case '\r':
      text += "\\r";
      break;
    case '\t':
      text += "\\t";
      break;
    case '"':
      text += "\\\"";
      break;
    case '\\':
      text += "\\\\";
      break;
    default:
      text.push_back(ch);
      break;
    }
  }
  text.push_back('"');
  return text;
}

std::optional<std::string> symbolLiteralText(const nir::Value& value) {
  if (value.kind != nir::ValueKind::Literal || value.type != "Symbol" ||
      value.text.empty()) {
    return std::nullopt;
  }
  return value.text;
}

nir::Value booleanLiteral(bool value, support::SourceSpan span) {
  return nir::Value{
      nir::ValueKind::Literal, "Boolean", value ? "true" : "false", {}, span};
}

std::int64_t signedInt32Value(std::uint32_t bits) {
  return bits <= 0x7fffffffu ? static_cast<std::int64_t>(bits)
                             : static_cast<std::int64_t>(bits) - 4294967296LL;
}

nir::Value intLiteral(std::int64_t value, support::SourceSpan span) {
  return nir::Value{nir::ValueKind::Literal, "Int", std::to_string(value), {}, span};
}

nir::Value stringLiteral(std::string_view value, support::SourceSpan span) {
  return nir::Value{
      nir::ValueKind::Literal, "String", escapedStringLiteralText(value), {}, span};
}

void replaceValue(nir::Value& value, const nir::Value& replacement) {
  nir::Value copy = replacement;
  value = std::move(copy);
}

bool isPureLetValue(const nir::Value& value);
bool isLiteralLikePropagatableValue(const nir::Value& value);
bool isUnusedAllocationDiscardable(const nir::Value& value,
                                   const DefinitionIndex& definitions,
                                   const ParentMap& parentMap,
                                   const ConstructorFieldMap& constructorFields,
                                   const FieldSnapshotMap& fieldSnapshots);
bool isUnusedValueDiscardable(const nir::Value& value,
                              const DefinitionIndex& definitions,
                              const ParentMap& parentMap,
                              const ConstructorFieldMap& constructorFields,
                              const FieldSnapshotMap& fieldSnapshots);
std::size_t discardUnusedAllocationEffects(nir::Value& value,
                                           const DefinitionIndex& definitions,
                                           const ParentMap& parentMap,
                                           const ConstructorFieldMap& constructorFields,
                                           const FieldSnapshotMap& fieldSnapshots);
std::size_t collectVisibleUses(const nir::Value& value, const std::string& name);

bool usesOnlyAllowedLocals(const nir::Value& value,
                           const std::unordered_set<std::string>& allowed) {
  if (value.kind == nir::ValueKind::Local) {
    return allowed.contains(value.text);
  }
  return std::all_of(value.operands.begin(), value.operands.end(),
                     [&](const nir::Value& operand) {
                       return usesOnlyAllowedLocals(operand, allowed);
                     });
}

void collectLocalNames(const nir::Value& value,
                       std::unordered_set<std::string>& names) {
  if (value.kind == nir::ValueKind::Local) {
    names.insert(value.text);
  }
  for (const nir::Value& operand : value.operands) {
    collectLocalNames(operand, names);
  }
}

bool valuesEqual(const nir::Value& left, const nir::Value& right) {
  if (left.kind != right.kind || left.type != right.type || left.text != right.text ||
      left.operands.size() != right.operands.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.operands.size(); ++i) {
    if (!valuesEqual(left.operands[i], right.operands[i])) {
      return false;
    }
  }
  return true;
}

std::string
knownValueType(const nir::Value& value,
               const std::unordered_map<std::string, std::string>& localTypes) {
  if (!value.type.empty()) {
    return value.type;
  }
  switch (value.kind) {
  case nir::ValueKind::Unit:
    return "Unit";
  case nir::ValueKind::Local: {
    auto local = localTypes.find(value.text);
    return local == localTypes.end() ? "Unknown" : local->second;
  }
  case nir::ValueKind::Unary:
    if (value.text == "!") {
      return "Boolean";
    }
    return value.operands.empty() ? "Unknown"
                                  : knownValueType(value.operands.front(), localTypes);
  case nir::ValueKind::Binary:
    if (value.text == "==" || value.text == "!=" || value.text == "<" ||
        value.text == ">" || value.text == "<=" || value.text == ">=" ||
        value.text == "&&" || value.text == "||") {
      return "Boolean";
    }
    if (value.text == "+" && value.operands.size() == 2 &&
        (knownValueType(value.operands[0], localTypes) == "String" ||
         knownValueType(value.operands[1], localTypes) == "String")) {
      return "String";
    }
    return value.operands.empty() ? "Unknown"
                                  : knownValueType(value.operands.front(), localTypes);
  case nir::ValueKind::If:
    if (value.operands.size() == 3) {
      const std::string thenType = knownValueType(value.operands[1], localTypes);
      const std::string elseType = knownValueType(value.operands[2], localTypes);
      if (thenType == "Nothing") {
        return elseType;
      }
      if (elseType == "Nothing") {
        return thenType;
      }
      if (thenType == elseType) {
        return thenType;
      }
    }
    return "Unknown";
  case nir::ValueKind::Block:
    return value.operands.empty() ? "Unit"
                                  : knownValueType(value.operands.back(), localTypes);
  case nir::ValueKind::SizeOf:
    return "Int";
  case nir::ValueKind::ZoneScoped:
    return value.operands.empty() ? "Unknown"
                                  : knownValueType(value.operands.front(), localTypes);
  case nir::ValueKind::Box:
    return "Object";
  case nir::ValueKind::Unbox:
  case nir::ValueKind::AsInstanceOf:
  case nir::ValueKind::New:
  case nir::ValueKind::Super:
    return value.text.empty() ? "Unknown" : value.text;
  case nir::ValueKind::IsInstanceOf:
    return "Boolean";
  case nir::ValueKind::Literal:
  case nir::ValueKind::Assign:
  case nir::ValueKind::Call:
  case nir::ValueKind::Select:
  case nir::ValueKind::While:
  case nir::ValueKind::LocalLet:
  case nir::ValueKind::LocalVar:
  case nir::ValueKind::Unknown:
    return "Unknown";
  case nir::ValueKind::Throw:
    return "Nothing";
  case nir::ValueKind::Try:
  case nir::ValueKind::Catch:
    return value.type.empty() ? "Unknown" : value.type;
  case nir::ValueKind::Finally:
    return "Unit";
  }
  return "Unknown";
}

bool isKnownValueType(const nir::Value& value, std::string_view type,
                      const std::unordered_map<std::string, std::string>& localTypes) {
  return knownValueType(value, localTypes) == type;
}

bool isNegatedSamePureBooleanValue(
    const nir::Value& maybeNegation, const nir::Value& value,
    const std::unordered_map<std::string, std::string>& localTypes) {
  return maybeNegation.kind == nir::ValueKind::Unary && maybeNegation.text == "!" &&
         maybeNegation.operands.size() == 1 &&
         valuesEqual(maybeNegation.operands.front(), value) && isPureLetValue(value) &&
         isKnownValueType(value, "Boolean", localTypes);
}

bool isPureKnownBooleanValue(
    const nir::Value& value,
    const std::unordered_map<std::string, std::string>& localTypes) {
  return isPureLetValue(value) && isKnownValueType(value, "Boolean", localTypes);
}

bool isBooleanAbsorptionOperand(
    const nir::Value& nested, std::string_view nestedOp, const nir::Value& retained,
    const std::unordered_map<std::string, std::string>& localTypes) {
  return nested.kind == nir::ValueKind::Binary && nested.text == nestedOp &&
         nested.operands.size() == 2 && isPureKnownBooleanValue(retained, localTypes) &&
         isPureKnownBooleanValue(nested.operands[0], localTypes) &&
         isPureKnownBooleanValue(nested.operands[1], localTypes) &&
         (valuesEqual(nested.operands[0], retained) ||
          valuesEqual(nested.operands[1], retained));
}

const nir::Value* booleanComplementAbsorptionRemainder(
    const nir::Value& nested, std::string_view nestedOp, const nir::Value& retained,
    const std::unordered_map<std::string, std::string>& localTypes) {
  if (nested.kind != nir::ValueKind::Binary || nested.text != nestedOp ||
      nested.operands.size() != 2 || !isPureKnownBooleanValue(retained, localTypes)) {
    return nullptr;
  }
  if (isNegatedSamePureBooleanValue(nested.operands[0], retained, localTypes) &&
      isPureKnownBooleanValue(nested.operands[1], localTypes)) {
    return &nested.operands[1];
  }
  if (isNegatedSamePureBooleanValue(nested.operands[1], retained, localTypes) &&
      isPureKnownBooleanValue(nested.operands[0], localTypes)) {
    return &nested.operands[0];
  }
  return nullptr;
}

nir::Value
booleanWithPolarity(const nir::Value& operand, bool preserveOperand,
                    support::SourceSpan span,
                    const std::unordered_map<std::string, std::string>& localTypes) {
  if (preserveOperand) {
    return operand;
  }
  if (operand.kind == nir::ValueKind::Unary && operand.text == "!" &&
      operand.operands.size() == 1 &&
      isKnownValueType(operand.operands.front(), "Boolean", localTypes)) {
    return operand.operands.front();
  }
  return nir::unaryValue("!", operand, span);
}

void replaceWithBooleanPolarity(
    nir::Value& target, const nir::Value& operand, bool preserveOperand,
    support::SourceSpan span,
    const std::unordered_map<std::string, std::string>& localTypes) {
  target = booleanWithPolarity(operand, preserveOperand, span, localTypes);
}

std::optional<bool> booleanValueWhenConditionIs(
    const nir::Value& value, const nir::Value& condition, bool conditionValue,
    const std::unordered_map<std::string, std::string>& localTypes) {
  if (std::optional<bool> literal = booleanLiteralValue(value)) {
    return literal;
  }
  if (valuesEqual(value, condition) &&
      isKnownValueType(condition, "Boolean", localTypes)) {
    return conditionValue;
  }
  if (value.kind == nir::ValueKind::Unary && value.text == "!" &&
      value.operands.size() == 1 && valuesEqual(value.operands.front(), condition) &&
      isKnownValueType(condition, "Boolean", localTypes)) {
    return !conditionValue;
  }
  if (condition.kind == nir::ValueKind::Unary && condition.text == "!" &&
      condition.operands.size() == 1 &&
      valuesEqual(value, condition.operands.front()) &&
      isKnownValueType(condition.operands.front(), "Boolean", localTypes)) {
    return !conditionValue;
  }
  return std::nullopt;
}

bool canonicalizeBooleanIfAsLogical(
    nir::Value& value, const std::unordered_map<std::string, std::string>& localTypes) {
  if (value.kind != nir::ValueKind::If || value.operands.size() != 3 ||
      !isKnownValueType(value.operands[0], "Boolean", localTypes)) {
    return false;
  }

  const nir::Value& condition = value.operands[0];
  const nir::Value& thenValue = value.operands[1];
  const nir::Value& elseValue = value.operands[2];
  const std::optional<bool> thenBoolean = booleanLiteralValue(thenValue);
  const std::optional<bool> elseBoolean = booleanLiteralValue(elseValue);

  if (elseBoolean && !thenBoolean &&
      isKnownValueType(thenValue, "Boolean", localTypes)) {
    value = nir::binaryValue(
        *elseBoolean ? "||" : "&&",
        booleanWithPolarity(condition, !*elseBoolean, value.span, localTypes),
        thenValue, value.span);
    return true;
  }

  if (thenBoolean && !elseBoolean &&
      isKnownValueType(elseValue, "Boolean", localTypes)) {
    value = nir::binaryValue(
        *thenBoolean ? "||" : "&&",
        booleanWithPolarity(condition, *thenBoolean, value.span, localTypes), elseValue,
        value.span);
    return true;
  }

  return false;
}

bool isIntegerLiteral(const nir::Value& value, long long expected) {
  const std::optional<long long> literal = integerLiteralValue(value);
  return literal && *literal == expected;
}

bool isIntegerLiteralOfType(const nir::Value& value, long long expected,
                            std::string_view type) {
  return value.type == type && isIntegerLiteral(value, expected);
}

bool isNullLiteral(const nir::Value& value) {
  return value.kind == nir::ValueKind::Literal && value.text == "null";
}

bool isTypedNullReferenceValue(const nir::Value& value, std::string_view declaredType) {
  return isNullLiteral(value) && !declaredType.empty() && declaredType != "Null" &&
         declaredType != "Unknown";
}

bool isBoxedAbiType(std::string_view type) {
  return type == "Unit" || type == "Boolean" || type == "Byte" || type == "Short" ||
         type == "Int" || type == "Long" || type == "Float" || type == "Double" ||
         type == "Char" || type == "Symbol" || type == "String";
}

bool isTopObjectTarget(std::string_view type) {
  return type == "Object" || type == "java.lang.Object";
}

bool isKnownClassLikeType(std::string_view type, const DefinitionIndex& definitions) {
  auto definition = definitions.find(std::string(type));
  return definition != definitions.end() && definition->second != nullptr &&
         isClassLikeDefinitionKind(definition->second->kind);
}

std::optional<std::string> arrayElementTypeName(std::string_view type);

bool knownReferenceTypeConformsTo(std::string_view sourceType,
                                  std::string_view targetType,
                                  const DefinitionIndex& definitions,
                                  const ParentMap& parentMap) {
  if (sourceType.empty() || sourceType == "Unknown" || targetType.empty()) {
    return false;
  }
  if (sourceType == targetType) {
    return true;
  }
  if (arrayElementTypeName(sourceType) && isTopObjectTarget(targetType)) {
    return true;
  }
  if (!isKnownClassLikeType(sourceType, definitions)) {
    return false;
  }
  if (isTopObjectTarget(targetType)) {
    return true;
  }
  for (const std::string& current :
       nir::linearizedTypeNames(std::string(sourceType), parentMap)) {
    if (current == targetType) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> arrayElementTypeName(std::string_view type) {
  constexpr std::string_view prefix = "Array [ ";
  constexpr std::string_view suffix = " ]";
  if (!type.starts_with(prefix) || !type.ends_with(suffix) ||
      type.size() <= prefix.size() + suffix.size()) {
    return std::nullopt;
  }
  type.remove_prefix(prefix.size());
  type.remove_suffix(suffix.size());
  return std::string(type);
}

std::string referenceArrayRuntimeSuffix(std::string_view elementType) {
  if (!arrayElementTypeName(elementType)) {
    return std::string(elementType);
  }

  constexpr char hex[] = "0123456789abcdef";
  std::string suffix = "nested$";
  for (const unsigned char ch : elementType) {
    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
      continue;
    }
    const bool unescaped = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                           (ch >= '0' && ch <= '9') || ch == '.' || ch == '_';
    if (unescaped) {
      suffix.push_back(static_cast<char>(ch));
    } else {
      suffix.push_back('$');
      suffix.push_back(hex[ch >> 4]);
      suffix.push_back(hex[ch & 0x0f]);
    }
  }
  return suffix;
}

bool isRuntimeArrayLengthHelper(std::string_view target, std::string_view elementType) {
  if (elementType == "String") {
    return target == support::StdNames::RuntimeArrayLength;
  }
  if (elementType == "Int") {
    return target == support::StdNames::RuntimeIntArrayLength;
  }
  if (elementType == "Byte") {
    return target == support::StdNames::RuntimeByteArrayLength;
  }
  if (elementType == "Short") {
    return target == support::StdNames::RuntimeShortArrayLength;
  }
  if (elementType == "Boolean") {
    return target == support::StdNames::RuntimeBooleanArrayLength;
  }
  if (elementType == "Long") {
    return target == support::StdNames::RuntimeLongArrayLength;
  }
  if (elementType == "Double") {
    return target == support::StdNames::RuntimeDoubleArrayLength;
  }
  if (elementType == "Float") {
    return target == support::StdNames::RuntimeFloatArrayLength;
  }
  if (elementType == "Char") {
    return target == support::StdNames::RuntimeCharArrayLength;
  }
  const std::string referenceHelper =
      std::string(support::StdNames::RuntimeReferenceArrayLength) + "." +
      referenceArrayRuntimeSuffix(elementType);
  return target == referenceHelper;
}

bool isRuntimeArrayApplyHelper(std::string_view target, std::string_view elementType) {
  if (elementType == "String") {
    return target == support::StdNames::RuntimeArrayApply;
  }
  if (elementType == "Int") {
    return target == support::StdNames::RuntimeIntArrayApply;
  }
  if (elementType == "Byte") {
    return target == support::StdNames::RuntimeByteArrayApply;
  }
  if (elementType == "Short") {
    return target == support::StdNames::RuntimeShortArrayApply;
  }
  if (elementType == "Boolean") {
    return target == support::StdNames::RuntimeBooleanArrayApply;
  }
  if (elementType == "Long") {
    return target == support::StdNames::RuntimeLongArrayApply;
  }
  if (elementType == "Double") {
    return target == support::StdNames::RuntimeDoubleArrayApply;
  }
  if (elementType == "Float") {
    return target == support::StdNames::RuntimeFloatArrayApply;
  }
  if (elementType == "Char") {
    return target == support::StdNames::RuntimeCharArrayApply;
  }
  const std::string referenceHelper =
      std::string(support::StdNames::RuntimeReferenceArrayApply) + "." +
      referenceArrayRuntimeSuffix(elementType);
  return target == referenceHelper;
}

bool isRuntimeArrayUpdateTarget(std::string_view target) {
  return target == support::StdNames::RuntimeArrayUpdate ||
         target == support::StdNames::RuntimeIntArrayUpdate ||
         target == support::StdNames::RuntimeByteArrayUpdate ||
         target == support::StdNames::RuntimeShortArrayUpdate ||
         target == support::StdNames::RuntimeBooleanArrayUpdate ||
         target == support::StdNames::RuntimeLongArrayUpdate ||
         target == support::StdNames::RuntimeDoubleArrayUpdate ||
         target == support::StdNames::RuntimeFloatArrayUpdate ||
         target == support::StdNames::RuntimeCharArrayUpdate ||
         target.starts_with(
             std::string(support::StdNames::RuntimeReferenceArrayUpdate) + ".");
}

bool isRuntimeArrayUpdateHelper(std::string_view target, std::string_view elementType) {
  if (elementType == "String") {
    return target == support::StdNames::RuntimeArrayUpdate;
  }
  if (elementType == "Int") {
    return target == support::StdNames::RuntimeIntArrayUpdate;
  }
  if (elementType == "Byte") {
    return target == support::StdNames::RuntimeByteArrayUpdate;
  }
  if (elementType == "Short") {
    return target == support::StdNames::RuntimeShortArrayUpdate;
  }
  if (elementType == "Boolean") {
    return target == support::StdNames::RuntimeBooleanArrayUpdate;
  }
  if (elementType == "Long") {
    return target == support::StdNames::RuntimeLongArrayUpdate;
  }
  if (elementType == "Double") {
    return target == support::StdNames::RuntimeDoubleArrayUpdate;
  }
  if (elementType == "Float") {
    return target == support::StdNames::RuntimeFloatArrayUpdate;
  }
  if (elementType == "Char") {
    return target == support::StdNames::RuntimeCharArrayUpdate;
  }
  const std::string referenceHelper =
      std::string(support::StdNames::RuntimeReferenceArrayUpdate) + "." +
      referenceArrayRuntimeSuffix(elementType);
  return target == referenceHelper;
}

bool isRuntimeArrayReadTarget(std::string_view target) {
  return target == support::StdNames::RuntimeArrayLength ||
         target == support::StdNames::RuntimeIntArrayLength ||
         target == support::StdNames::RuntimeByteArrayLength ||
         target == support::StdNames::RuntimeShortArrayLength ||
         target == support::StdNames::RuntimeBooleanArrayLength ||
         target == support::StdNames::RuntimeLongArrayLength ||
         target == support::StdNames::RuntimeDoubleArrayLength ||
         target == support::StdNames::RuntimeFloatArrayLength ||
         target == support::StdNames::RuntimeCharArrayLength ||
         target == support::StdNames::RuntimeArrayApply ||
         target == support::StdNames::RuntimeIntArrayApply ||
         target == support::StdNames::RuntimeByteArrayApply ||
         target == support::StdNames::RuntimeShortArrayApply ||
         target == support::StdNames::RuntimeBooleanArrayApply ||
         target == support::StdNames::RuntimeLongArrayApply ||
         target == support::StdNames::RuntimeDoubleArrayApply ||
         target == support::StdNames::RuntimeFloatArrayApply ||
         target == support::StdNames::RuntimeCharArrayApply ||
         target.starts_with(
             std::string(support::StdNames::RuntimeReferenceArrayLength) + ".") ||
         target.starts_with(std::string(support::StdNames::RuntimeReferenceArrayApply) +
                            ".");
}

bool isRuntimeHashCodeTarget(std::string_view target) {
  return target == support::StdNames::RuntimeByteHashCode ||
         target == support::StdNames::RuntimeShortHashCode ||
         target == support::StdNames::RuntimeBooleanHashCode ||
         target == support::StdNames::RuntimeLongHashCode ||
         target == support::StdNames::RuntimeFloatHashCode ||
         target == support::StdNames::RuntimeDoubleHashCode ||
         target == support::StdNames::RuntimeCharHashCode ||
         target == support::StdNames::RuntimeStringHashCode ||
         target == support::StdNames::RuntimeSymbolHashCode ||
         target == support::StdNames::RuntimeAnyHashCode ||
         target == support::StdNames::RuntimeAnyReceiverHashCode;
}

std::optional<std::string> exactBoxedAbiOperandType(const nir::Value& operand) {
  if (operand.kind == nir::ValueKind::Box && operand.operands.size() == 1 &&
      isBoxedAbiType(operand.text) && isPureLetValue(operand.operands.front())) {
    return operand.text;
  }
  return std::nullopt;
}

bool isStringLiteralValue(const nir::Value& value) {
  return value.kind == nir::ValueKind::Literal && value.type == "String" &&
         stringLiteralValue(value).has_value();
}

std::optional<std::string> exactAbiOperandType(const nir::Value& operand) {
  if (std::optional<std::string> boxedType = exactBoxedAbiOperandType(operand)) {
    return boxedType;
  }
  if (isStringLiteralValue(operand)) {
    return "String";
  }
  return std::nullopt;
}

const nir::Value* exactBoxedAbiPayload(const nir::Value& operand,
                                       std::string_view targetType) {
  if (operand.kind == nir::ValueKind::Box && operand.text == targetType &&
      operand.operands.size() == 1 && isBoxedAbiType(targetType)) {
    return &operand.operands.front();
  }
  return nullptr;
}

struct ExactBoxedAbiValue {
  std::string type;
  const nir::Value* payload = nullptr;
};

const nir::Value*
exactImmutableValue(const nir::Value& value,
                    const std::unordered_map<std::string, nir::Value>& localValues) {
  const nir::Value* candidate = &value;
  std::unordered_set<std::string> seen;
  while (candidate->kind == nir::ValueKind::Local) {
    if (!seen.insert(candidate->text).second) {
      return nullptr;
    }
    auto local = localValues.find(candidate->text);
    if (local == localValues.end()) {
      return nullptr;
    }
    candidate = &local->second;
  }
  return candidate;
}

std::optional<ExactBoxedAbiValue>
exactPureBoxedAbiValue(const nir::Value& value,
                       const std::unordered_map<std::string, nir::Value>& localValues) {
  const nir::Value* candidate = exactImmutableValue(value, localValues);
  if (candidate == nullptr || candidate->kind != nir::ValueKind::Box ||
      candidate->operands.size() != 1 || !isBoxedAbiType(candidate->text) ||
      !isPureLetValue(candidate->operands.front())) {
    return std::nullopt;
  }
  return ExactBoxedAbiValue{candidate->text, &candidate->operands.front()};
}

bool isExactNullValue(const nir::Value& value,
                      const std::unordered_map<std::string, nir::Value>& localValues) {
  const nir::Value* candidate = exactImmutableValue(value, localValues);
  return candidate != nullptr && isNullLiteral(*candidate);
}

bool isExactPureNonNullReferenceValue(
    const nir::Value& value,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  const nir::Value* candidate = exactImmutableValue(value, localValues);
  if (candidate == nullptr) {
    return false;
  }

  if (candidate->kind == nir::ValueKind::Box && candidate->operands.size() == 1 &&
      isBoxedAbiType(candidate->text) && isPureLetValue(candidate->operands.front())) {
    return true;
  }
  return isStringLiteralValue(*candidate);
}

bool isExactEvaluatedBoxValue(const nir::Value& value) {
  return value.kind == nir::ValueKind::Box && value.operands.size() == 1 &&
         isBoxedAbiType(value.text);
}

bool isExactNonNullReferenceValue(const nir::Value& value) {
  if (value.kind == nir::ValueKind::New) {
    return true;
  }
  if (isExactEvaluatedBoxValue(value)) {
    return true;
  }
  return isStringLiteralValue(value);
}

std::optional<std::string> exactImmutableNonNullReferenceRootName(
    const nir::Value& value,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  if (value.kind != nir::ValueKind::Local) {
    return std::nullopt;
  }

  std::string current = value.text;
  std::unordered_set<std::string> seen;
  while (seen.insert(current).second) {
    auto local = localValues.find(current);
    if (local == localValues.end()) {
      return std::nullopt;
    }
    if (local->second.kind == nir::ValueKind::Local) {
      current = local->second.text;
      continue;
    }
    return isExactNonNullReferenceValue(local->second)
               ? std::optional<std::string>(current)
               : std::nullopt;
  }
  return std::nullopt;
}

bool isProvenNonNullReferenceValue(
    const nir::Value& value,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  return isExactNonNullReferenceValue(value) ||
         exactImmutableNonNullReferenceRootName(value, localValues).has_value();
}

std::optional<bool> foldExactSameReferenceComparison(
    std::string_view op, const nir::Value& left, const nir::Value& right,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  if (op != "==" && op != "!=") {
    return std::nullopt;
  }
  const std::optional<std::string> lhsRoot =
      exactImmutableNonNullReferenceRootName(left, localValues);
  const std::optional<std::string> rhsRoot =
      exactImmutableNonNullReferenceRootName(right, localValues);
  if (!lhsRoot || !rhsRoot || *lhsRoot != *rhsRoot) {
    return std::nullopt;
  }
  return op == "==";
}

std::optional<std::string> exactImmutableFreshNewRootName(
    const nir::Value& value,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  if (value.kind != nir::ValueKind::Local) {
    return std::nullopt;
  }

  std::string current = value.text;
  std::unordered_set<std::string> seen;
  while (seen.insert(current).second) {
    auto local = localValues.find(current);
    if (local == localValues.end()) {
      return std::nullopt;
    }
    if (local->second.kind == nir::ValueKind::Local) {
      current = local->second.text;
      continue;
    }
    return local->second.kind == nir::ValueKind::New
               ? std::optional<std::string>(current)
               : std::nullopt;
  }
  return std::nullopt;
}

std::optional<bool> foldExactDistinctFreshReferenceComparison(
    std::string_view op, const nir::Value& left, const nir::Value& right,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  if (op != "==" && op != "!=") {
    return std::nullopt;
  }
  const std::optional<std::string> lhsRoot =
      exactImmutableFreshNewRootName(left, localValues);
  const std::optional<std::string> rhsRoot =
      exactImmutableFreshNewRootName(right, localValues);
  if (!lhsRoot || !rhsRoot || *lhsRoot == *rhsRoot) {
    return std::nullopt;
  }
  return op == "!=";
}

std::optional<nir::Value>
foldDirectDistinctFreshReferenceComparison(std::string_view op, const nir::Value& left,
                                           const nir::Value& right,
                                           support::SourceSpan span) {
  if ((op != "==" && op != "!=") || left.kind != nir::ValueKind::New ||
      right.kind != nir::ValueKind::New) {
    return std::nullopt;
  }
  return nir::blockValue({left, right, booleanLiteral(op == "!=", span)}, span);
}

std::optional<std::string> exactRuntimeHashCallReferenceRootName(
    const nir::Value& value,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  if (value.kind != nir::ValueKind::Call || value.operands.size() != 2 ||
      value.operands.front().kind != nir::ValueKind::Local ||
      !isRuntimeHashCodeTarget(value.operands.front().text)) {
    return std::nullopt;
  }
  return exactImmutableNonNullReferenceRootName(value.operands[1], localValues);
}

std::optional<bool> foldExactSameRuntimeHashComparison(
    std::string_view op, const nir::Value& left, const nir::Value& right,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  if ((op != "==" && op != "!=") || left.kind != nir::ValueKind::Call ||
      right.kind != nir::ValueKind::Call || left.operands.empty() ||
      right.operands.empty() || left.operands.front().kind != nir::ValueKind::Local ||
      right.operands.front().kind != nir::ValueKind::Local ||
      left.operands.front().text != right.operands.front().text) {
    return std::nullopt;
  }
  const std::optional<std::string> lhsRoot =
      exactRuntimeHashCallReferenceRootName(left, localValues);
  const std::optional<std::string> rhsRoot =
      exactRuntimeHashCallReferenceRootName(right, localValues);
  if (!lhsRoot || !rhsRoot || *lhsRoot != *rhsRoot) {
    return std::nullopt;
  }
  return op == "==";
}

const nir::Value* exactPureStringLiteralValue(
    const nir::Value& value,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  const nir::Value* candidate = exactImmutableValue(value, localValues);
  if (candidate == nullptr) {
    return nullptr;
  }
  return isStringLiteralValue(*candidate) ? candidate : nullptr;
}

const nir::Value* exactLocalArrayAllocationValue(
    const nir::Value& value,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  if (value.kind != nir::ValueKind::Local) {
    return nullptr;
  }

  std::string current = value.text;
  std::unordered_set<std::string> seen;
  while (seen.insert(current).second) {
    auto local = localValues.find(current);
    if (local == localValues.end()) {
      return nullptr;
    }
    if (local->second.kind != nir::ValueKind::Local) {
      return &local->second;
    }
    current = local->second.text;
  }
  return nullptr;
}

std::optional<std::string> exactLocalArrayRootName(
    const nir::Value& value,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  if (value.kind != nir::ValueKind::Local) {
    return std::nullopt;
  }

  std::string current = value.text;
  std::unordered_set<std::string> seen;
  while (seen.insert(current).second) {
    auto local = localValues.find(current);
    if (local == localValues.end()) {
      return std::nullopt;
    }
    if (local->second.kind == nir::ValueKind::Local) {
      current = local->second.text;
      continue;
    }
    if (local->second.kind == nir::ValueKind::New &&
        arrayElementTypeName(local->second.type)) {
      return current;
    }
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<std::string> exactLocalObjectRootName(
    const nir::Value& value,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  if (value.kind != nir::ValueKind::Local) {
    return std::nullopt;
  }

  std::string current = value.text;
  std::unordered_set<std::string> seen;
  while (seen.insert(current).second) {
    auto local = localValues.find(current);
    if (local == localValues.end()) {
      return std::nullopt;
    }
    if (local->second.kind == nir::ValueKind::Local) {
      current = local->second.text;
      continue;
    }
    if (local->second.kind == nir::ValueKind::New &&
        !arrayElementTypeName(local->second.type)) {
      return current;
    }
    return std::nullopt;
  }
  return std::nullopt;
}

void forgetArrayContents(nir::Value& array) {
  if (array.kind != nir::ValueKind::New || !arrayElementTypeName(array.type)) {
    return;
  }
  for (nir::Value& element : array.operands) {
    element = nir::unknownValue("<unknown-array-element>", array.span);
  }
}

void forgetArrayLocalContents(
    std::string_view root, std::unordered_map<std::string, nir::Value>& localValues) {
  auto array = localValues.find(std::string(root));
  if (array != localValues.end()) {
    forgetArrayContents(array->second);
  }
}

void forgetAllArrayLocalContents(
    std::unordered_map<std::string, nir::Value>& localValues) {
  for (auto& [name, value] : localValues) {
    (void)name;
    forgetArrayContents(value);
  }
}

void forgetObjectFieldContents(nir::Value& object) {
  if (object.kind != nir::ValueKind::New || arrayElementTypeName(object.type)) {
    return;
  }
  for (nir::Value& field : object.operands) {
    field = nir::unknownValue("<unknown-field>", object.span);
  }
}

void forgetObjectLocalFieldContents(
    std::string_view root, std::unordered_map<std::string, nir::Value>& localValues) {
  auto object = localValues.find(std::string(root));
  if (object != localValues.end()) {
    forgetObjectFieldContents(object->second);
  }
}

void collectExactLocalObjectRootNames(
    const nir::Value& value,
    const std::unordered_map<std::string, nir::Value>& localValues,
    std::unordered_set<std::string>& roots,
    std::unordered_set<std::string>& seenLocals) {
  if (value.kind == nir::ValueKind::Local) {
    if (!seenLocals.insert(value.text).second) {
      return;
    }

    if (std::optional<std::string> root =
            exactLocalObjectRootName(value, localValues)) {
      if (roots.insert(*root).second) {
        auto object = localValues.find(*root);
        if (object != localValues.end()) {
          for (const nir::Value& field : object->second.operands) {
            collectExactLocalObjectRootNames(field, localValues, roots, seenLocals);
          }
        }
      }
      return;
    }

    auto local = localValues.find(value.text);
    if (local != localValues.end()) {
      collectExactLocalObjectRootNames(local->second, localValues, roots, seenLocals);
    }
    return;
  }

  for (const nir::Value& operand : value.operands) {
    collectExactLocalObjectRootNames(operand, localValues, roots, seenLocals);
  }
}

std::unordered_set<std::string> exactLocalObjectRootNamesInValue(
    const nir::Value& value,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  std::unordered_set<std::string> roots;
  std::unordered_set<std::string> seenLocals;
  collectExactLocalObjectRootNames(value, localValues, roots, seenLocals);
  return roots;
}

bool forgetObjectLocalFieldContentsForRootsInValue(
    const nir::Value& value, std::unordered_map<std::string, nir::Value>& localValues) {
  const std::unordered_set<std::string> roots =
      exactLocalObjectRootNamesInValue(value, localValues);
  for (const std::string& root : roots) {
    forgetObjectLocalFieldContents(root, localValues);
  }
  return !roots.empty();
}

bool updateExactArrayLocalValue(
    std::string_view target, const nir::Value& arrayOperand, const nir::Value& index,
    const nir::Value& assignedValue,
    std::unordered_map<std::string, nir::Value>& localValues) {
  const std::optional<std::string> root =
      exactLocalArrayRootName(arrayOperand, localValues);
  if (!root) {
    return false;
  }

  auto array = localValues.find(*root);
  if (array == localValues.end() || array->second.kind != nir::ValueKind::New) {
    return false;
  }

  const std::optional<std::string> elementType =
      arrayElementTypeName(array->second.type);
  if (!elementType || array->second.text != array->second.type ||
      !isRuntimeArrayUpdateHelper(target, *elementType)) {
    forgetArrayLocalContents(*root, localValues);
    return true;
  }

  const nir::Value* exactIndex = exactImmutableValue(index, localValues);
  const nir::Value* exactAssignedValue =
      exactImmutableValue(assignedValue, localValues);
  if (exactIndex == nullptr || exactIndex->type != "Int" ||
      exactAssignedValue == nullptr || !isPureLetValue(*exactAssignedValue)) {
    forgetArrayLocalContents(*root, localValues);
    return true;
  }

  const std::optional<long long> offset = integerLiteralValue(*exactIndex);
  if (!offset || *offset < 0 ||
      static_cast<std::size_t>(*offset) >= array->second.operands.size()) {
    forgetArrayLocalContents(*root, localValues);
    return true;
  }

  array->second.operands[static_cast<std::size_t>(*offset)] = *exactAssignedValue;
  return true;
}

bool isKnownArrayValue(const nir::Value& value,
                       const std::unordered_map<std::string, std::string>& localTypes,
                       const std::unordered_map<std::string, nir::Value>& localValues) {
  if (value.kind == nir::ValueKind::New && arrayElementTypeName(value.type)) {
    return true;
  }
  if (exactLocalArrayAllocationValue(value, localValues) != nullptr) {
    return true;
  }
  return arrayElementTypeName(knownValueType(value, localTypes)).has_value();
}

std::optional<std::string> callTargetName(const nir::Value& value) {
  if (value.kind != nir::ValueKind::Call || value.operands.empty() ||
      value.operands.front().kind != nir::ValueKind::Local) {
    return std::nullopt;
  }
  return value.operands.front().text;
}

void invalidateArrayLocalValuesAfterPotentialMutation(
    const nir::Value& value,
    const std::unordered_map<std::string, std::string>& localTypes,
    std::unordered_map<std::string, nir::Value>& localValues) {
  for (const nir::Value& operand : value.operands) {
    invalidateArrayLocalValuesAfterPotentialMutation(operand, localTypes, localValues);
  }

  const std::optional<std::string> target = callTargetName(value);
  if (!target) {
    return;
  }

  if (isRuntimeArrayUpdateTarget(*target)) {
    if (value.operands.size() >= 2) {
      if (value.operands.size() >= 4 &&
          updateExactArrayLocalValue(*target, value.operands[1], value.operands[2],
                                     value.operands[3], localValues)) {
        return;
      }
      if (std::optional<std::string> root =
              exactLocalArrayRootName(value.operands[1], localValues)) {
        forgetArrayLocalContents(*root, localValues);
        return;
      }
      if (isKnownArrayValue(value.operands[1], localTypes, localValues)) {
        forgetAllArrayLocalContents(localValues);
      }
    } else {
      forgetAllArrayLocalContents(localValues);
    }
    return;
  }

  if (isRuntimeArrayReadTarget(*target)) {
    return;
  }

  for (std::size_t i = 1; i < value.operands.size(); ++i) {
    if (isKnownArrayValue(value.operands[i], localTypes, localValues)) {
      forgetAllArrayLocalContents(localValues);
      return;
    }
  }
}

const nir::Value* exactPureLocalBoxedAbiPayload(
    const nir::Value& operand, std::string_view targetType,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  const nir::Value* exact = exactImmutableValue(operand, localValues);
  if (exact == nullptr) {
    return nullptr;
  }
  const nir::Value* payload = exactBoxedAbiPayload(*exact, targetType);
  return payload != nullptr && isPureLetValue(*payload) ? payload : nullptr;
}

std::optional<bool> boxedPayloadsEqual(std::string_view type, const nir::Value& left,
                                       const nir::Value& right) {
  if (type == "Unit") {
    return left.kind == nir::ValueKind::Unit && right.kind == nir::ValueKind::Unit;
  }
  if (type == "Boolean") {
    const std::optional<bool> lhs = booleanLiteralValue(left);
    const std::optional<bool> rhs = booleanLiteralValue(right);
    return lhs && rhs ? std::optional<bool>(*lhs == *rhs) : std::nullopt;
  }
  if (type == "Int" || type == "Long") {
    const std::optional<long long> lhs = integerLiteralValue(left);
    const std::optional<long long> rhs = integerLiteralValue(right);
    return lhs && rhs ? std::optional<bool>(*lhs == *rhs) : std::nullopt;
  }
  if (type == "Float") {
    const std::optional<double> lhs = floatingLiteralValue(left);
    const std::optional<double> rhs = floatingLiteralValue(right);
    return lhs && rhs ? std::optional<bool>(static_cast<float>(*lhs) ==
                                            static_cast<float>(*rhs))
                      : std::nullopt;
  }
  if (type == "Double") {
    const std::optional<double> lhs = floatingLiteralValue(left);
    const std::optional<double> rhs = floatingLiteralValue(right);
    return lhs && rhs ? std::optional<bool>(*lhs == *rhs) : std::nullopt;
  }
  if (type == "Char") {
    const std::optional<int> lhs = charLiteralValue(left);
    const std::optional<int> rhs = charLiteralValue(right);
    return lhs && rhs ? std::optional<bool>(*lhs == *rhs) : std::nullopt;
  }
  if (type == "String") {
    const std::optional<std::string> lhs = stringLiteralValue(left);
    const std::optional<std::string> rhs = stringLiteralValue(right);
    return lhs && rhs ? std::optional<bool>(*lhs == *rhs) : std::nullopt;
  }
  if (type == "Symbol") {
    const std::optional<std::string> lhs = symbolLiteralText(left);
    const std::optional<std::string> rhs = symbolLiteralText(right);
    return lhs && rhs ? std::optional<bool>(*lhs == *rhs) : std::nullopt;
  }
  return std::nullopt;
}

std::int64_t longHashCodeBits(std::uint64_t bits) {
  return signedInt32Value(static_cast<std::uint32_t>(bits ^ (bits >> 32)));
}

std::int64_t longHashCode(long long value) {
  return longHashCodeBits(static_cast<std::uint64_t>(value));
}

std::int64_t stringHashCode(std::string_view value) {
  std::uint32_t hash = 0;
  for (unsigned char byte : value) {
    hash = hash * 31u + static_cast<std::uint32_t>(byte);
  }
  return signedInt32Value(hash);
}

std::optional<std::int64_t> foldedBoxedHashCode(const ExactBoxedAbiValue& boxed) {
  if (boxed.type == "Unit") {
    return boxed.payload->kind == nir::ValueKind::Unit ? std::optional<std::int64_t>(0)
                                                       : std::nullopt;
  }
  if (boxed.type == "Boolean") {
    const std::optional<bool> value = booleanLiteralValue(*boxed.payload);
    return value ? std::optional<std::int64_t>(*value ? 1231 : 1237) : std::nullopt;
  }
  if (boxed.type == "Int") {
    const std::optional<long long> value = integerLiteralValue(*boxed.payload);
    return value ? std::optional<std::int64_t>(
                       signedInt32Value(static_cast<std::uint32_t>(*value)))
                 : std::nullopt;
  }
  if (boxed.type == "Long") {
    const std::optional<long long> value = integerLiteralValue(*boxed.payload);
    return value ? std::optional<std::int64_t>(longHashCode(*value)) : std::nullopt;
  }
  if (boxed.type == "Float") {
    const std::optional<std::uint32_t> bits = floatLiteralBits(*boxed.payload);
    return bits ? std::optional<std::int64_t>(signedInt32Value(*bits)) : std::nullopt;
  }
  if (boxed.type == "Double") {
    const std::optional<std::uint64_t> bits = doubleLiteralBits(*boxed.payload);
    return bits ? std::optional<std::int64_t>(longHashCodeBits(*bits)) : std::nullopt;
  }
  if (boxed.type == "Char") {
    const std::optional<int> value = charLiteralValue(*boxed.payload);
    return value ? std::optional<std::int64_t>(*value) : std::nullopt;
  }
  if (boxed.type == "String") {
    const std::optional<std::string> value = stringLiteralValue(*boxed.payload);
    return value ? std::optional<std::int64_t>(stringHashCode(*value)) : std::nullopt;
  }
  if (boxed.type == "Symbol") {
    const std::optional<std::string> value = symbolLiteralText(*boxed.payload);
    return value ? std::optional<std::int64_t>(stringHashCode(*value)) : std::nullopt;
  }
  return std::nullopt;
}

std::optional<std::string> foldedBoxedToString(const ExactBoxedAbiValue& boxed) {
  if (boxed.type == "Unit") {
    return boxed.payload->kind == nir::ValueKind::Unit
               ? std::optional<std::string>("()")
               : std::nullopt;
  }
  if (boxed.type == "Boolean") {
    const std::optional<bool> value = booleanLiteralValue(*boxed.payload);
    return value ? std::optional<std::string>(*value ? "true" : "false") : std::nullopt;
  }
  if (boxed.type == "Int") {
    const std::optional<long long> value = integerLiteralValue(*boxed.payload);
    if (!value || *value < std::numeric_limits<std::int32_t>::min() ||
        *value > std::numeric_limits<std::int32_t>::max()) {
      return std::nullopt;
    }
    return std::to_string(*value);
  }
  if (boxed.type == "Long") {
    const std::optional<long long> value = integerLiteralValue(*boxed.payload);
    return value ? std::optional<std::string>(std::to_string(*value)) : std::nullopt;
  }
  if (boxed.type == "Char") {
    const std::optional<int> value = charLiteralValue(*boxed.payload);
    if (!value) {
      return std::nullopt;
    }
    return std::string(1, static_cast<char>(*value));
  }
  if (boxed.type == "String") {
    return stringLiteralValue(*boxed.payload);
  }
  if (boxed.type == "Symbol") {
    return symbolLiteralText(*boxed.payload);
  }
  return std::nullopt;
}

std::optional<std::string> exactStringifiedAbiValue(
    const nir::Value& value,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  const nir::Value* exact = exactImmutableValue(value, localValues);
  if (exact == nullptr) {
    return std::nullopt;
  }
  if (isNullLiteral(*exact)) {
    return "null";
  }
  if (exact->kind == nir::ValueKind::Unit) {
    return "()";
  }
  if (const std::optional<std::string> text = stringLiteralValue(*exact)) {
    return text;
  }
  if (const std::optional<bool> boolean = booleanLiteralValue(*exact)) {
    return *boolean ? "true" : "false";
  }
  if (exact->type == "Int") {
    const std::optional<long long> integer = integerLiteralValue(*exact);
    if (!integer || *integer < std::numeric_limits<std::int32_t>::min() ||
        *integer > std::numeric_limits<std::int32_t>::max()) {
      return std::nullopt;
    }
    return std::to_string(*integer);
  }
  if (exact->type == "Long") {
    const std::optional<long long> integer = integerLiteralValue(*exact);
    return integer ? std::optional<std::string>(std::to_string(*integer))
                   : std::nullopt;
  }
  if (exact->type == "Char") {
    const std::optional<int> character = charLiteralValue(*exact);
    if (!character) {
      return std::nullopt;
    }
    return std::string(1, static_cast<char>(*character));
  }
  if (exact->type == "Symbol") {
    return symbolLiteralText(*exact);
  }
  if (value.kind == nir::ValueKind::Local && isExactEvaluatedBoxValue(*exact) &&
      exact->text == "Unit") {
    return "()";
  }
  if (exact->kind == nir::ValueKind::Box && exact->operands.size() == 1 &&
      isBoxedAbiType(exact->text) && isPureLetValue(exact->operands.front())) {
    return foldedBoxedToString(
        ExactBoxedAbiValue{exact->text, &exact->operands.front()});
  }
  return std::nullopt;
}

std::optional<std::string>
foldExactStringConcat(const nir::Value& left, const nir::Value& right,
                      const std::unordered_map<std::string, std::string>& localTypes,
                      const std::unordered_map<std::string, nir::Value>& localValues) {
  if (knownValueType(left, localTypes) != "String" &&
      knownValueType(right, localTypes) != "String") {
    return std::nullopt;
  }
  const std::optional<std::string> lhs = exactStringifiedAbiValue(left, localValues);
  const std::optional<std::string> rhs = exactStringifiedAbiValue(right, localValues);
  if (!lhs || !rhs) {
    return std::nullopt;
  }
  return *lhs + *rhs;
}

std::optional<nir::Value> foldDirectEvaluatedUnitBoxStringConcat(
    const nir::Value& left, const nir::Value& right,
    const std::unordered_map<std::string, std::string>& localTypes,
    const std::unordered_map<std::string, nir::Value>& localValues,
    support::SourceSpan span) {
  if (knownValueType(left, localTypes) != "String" &&
      knownValueType(right, localTypes) != "String") {
    return std::nullopt;
  }
  const auto isEffectfulUnitBox = [](const nir::Value& value) {
    return isExactEvaluatedBoxValue(value) && value.text == "Unit" &&
           !isPureLetValue(value.operands.front());
  };

  std::optional<std::string> folded;
  if (isEffectfulUnitBox(left)) {
    if (const std::optional<std::string> rhs =
            exactStringifiedAbiValue(right, localValues)) {
      folded = "()" + *rhs;
    }
  } else if (isEffectfulUnitBox(right)) {
    if (const std::optional<std::string> lhs =
            exactStringifiedAbiValue(left, localValues)) {
      folded = *lhs + "()";
    }
  }
  if (!folded) {
    return std::nullopt;
  }
  return nir::blockValue({left, right, stringLiteral(*folded, span)}, span);
}

std::optional<nir::Value> foldDirectEvaluatedUnitBoxRuntimeCall(
    std::string_view target, const nir::Value& operand, support::SourceSpan span) {
  if (!isExactEvaluatedBoxValue(operand) || operand.text != "Unit" ||
      isPureLetValue(operand.operands.front())) {
    return std::nullopt;
  }
  if (target == support::StdNames::RuntimeAnyHashCode ||
      target == support::StdNames::RuntimeAnyReceiverHashCode) {
    return nir::blockValue({operand, intLiteral(0, span)}, span);
  }
  if (target == support::StdNames::RuntimeAnyToString ||
      target == support::StdNames::RuntimeAnyReceiverToString) {
    return nir::blockValue({operand, stringLiteral("()", span)}, span);
  }
  return std::nullopt;
}

std::optional<std::int64_t> foldExactRuntimeHashCode(
    std::string_view target, const nir::Value& operand,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  const nir::Value* exact = exactImmutableValue(operand, localValues);
  if (target == support::StdNames::RuntimeBooleanHashCode) {
    if (exact == nullptr) {
      return std::nullopt;
    }
    const std::optional<bool> value = booleanLiteralValue(*exact);
    return value ? std::optional<std::int64_t>(*value ? 1231 : 1237) : std::nullopt;
  }
  if (target == support::StdNames::RuntimeLongHashCode) {
    if (exact == nullptr || exact->type != "Long") {
      return std::nullopt;
    }
    const std::optional<long long> value = integerLiteralValue(*exact);
    return value ? std::optional<std::int64_t>(longHashCode(*value)) : std::nullopt;
  }
  if (target == support::StdNames::RuntimeFloatHashCode) {
    if (exact == nullptr) {
      return std::nullopt;
    }
    const std::optional<std::uint32_t> bits = floatLiteralBits(*exact);
    return bits ? std::optional<std::int64_t>(signedInt32Value(*bits)) : std::nullopt;
  }
  if (target == support::StdNames::RuntimeDoubleHashCode) {
    if (exact == nullptr) {
      return std::nullopt;
    }
    const std::optional<std::uint64_t> bits = doubleLiteralBits(*exact);
    return bits ? std::optional<std::int64_t>(longHashCodeBits(*bits)) : std::nullopt;
  }
  if (target == support::StdNames::RuntimeCharHashCode) {
    if (exact == nullptr) {
      return std::nullopt;
    }
    const std::optional<int> value = charLiteralValue(*exact);
    return value ? std::optional<std::int64_t>(*value) : std::nullopt;
  }
  if (target == support::StdNames::RuntimeStringHashCode) {
    const nir::Value* value = exactPureStringLiteralValue(operand, localValues);
    if (value == nullptr) {
      return std::nullopt;
    }
    const std::optional<std::string> text = stringLiteralValue(*value);
    return text ? std::optional<std::int64_t>(stringHashCode(*text)) : std::nullopt;
  }
  if (target == support::StdNames::RuntimeSymbolHashCode) {
    if (exact == nullptr) {
      return std::nullopt;
    }
    const std::optional<std::string> value = symbolLiteralText(*exact);
    return value ? std::optional<std::int64_t>(stringHashCode(*value)) : std::nullopt;
  }
  if (target == support::StdNames::RuntimeAnyHashCode ||
      target == support::StdNames::RuntimeAnyReceiverHashCode) {
    if (target == support::StdNames::RuntimeAnyReceiverHashCode &&
        !isProvenNonNullReferenceValue(operand, localValues)) {
      return std::nullopt;
    }
    if (target == support::StdNames::RuntimeAnyHashCode &&
        isExactNullValue(operand, localValues)) {
      return 0;
    }
    if (exact != nullptr && isExactEvaluatedBoxValue(*exact) && exact->text == "Unit") {
      return 0;
    }
    const std::optional<ExactBoxedAbiValue> boxed =
        exactPureBoxedAbiValue(operand, localValues);
    return boxed ? foldedBoxedHashCode(*boxed) : std::nullopt;
  }
  return std::nullopt;
}

std::optional<std::int64_t> foldExactRuntimeStringLength(
    std::string_view target, const nir::Value& operand,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  if (target != support::StdNames::RuntimeStringLength) {
    return std::nullopt;
  }
  const nir::Value* value = exactPureStringLiteralValue(operand, localValues);
  if (value == nullptr) {
    return std::nullopt;
  }
  const std::optional<std::string> text = stringLiteralValue(*value);
  return text ? std::optional<std::int64_t>(text->size()) : std::nullopt;
}

std::optional<std::int64_t> foldExactRuntimeArrayLength(
    std::string_view target, const nir::Value& operand,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  const nir::Value* array = exactLocalArrayAllocationValue(operand, localValues);
  if (array == nullptr || array->kind != nir::ValueKind::New) {
    return std::nullopt;
  }
  const std::optional<std::string> elementType = arrayElementTypeName(array->type);
  if (!elementType || array->text != array->type ||
      !isRuntimeArrayLengthHelper(target, *elementType) ||
      array->operands.size() >
          static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(array->operands.size());
}

std::string freshLocalName(std::string_view base,
                           const std::unordered_set<std::string>& localNames) {
  std::string name(base);
  for (std::size_t suffix = 1; localNames.contains(name); ++suffix) {
    name = std::string(base) + std::to_string(suffix);
  }
  return name;
}

std::optional<nir::Value> foldDirectRuntimeArrayLength(std::string_view target,
                                                       const nir::Value& operand,
                                                       support::SourceSpan span) {
  if (operand.kind != nir::ValueKind::New) {
    return std::nullopt;
  }
  const std::optional<std::string> elementType = arrayElementTypeName(operand.type);
  if (!elementType || operand.text != operand.type ||
      !isRuntimeArrayLengthHelper(target, *elementType) ||
      operand.operands.size() >
          static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
    return std::nullopt;
  }

  nir::Value length =
      intLiteral(static_cast<std::int64_t>(operand.operands.size()), span);
  if (operand.operands.empty()) {
    return length;
  }
  std::vector<nir::Value> effects = operand.operands;
  effects.push_back(std::move(length));
  return nir::blockValue(std::move(effects), span);
}

std::optional<nir::Value> foldDirectRuntimeArrayApply(
    std::string_view target, const nir::Value& arrayOperand,
    const nir::Value& indexOperand,
    const std::unordered_map<std::string, nir::Value>& localValues,
    const std::unordered_set<std::string>& localNames, support::SourceSpan span) {
  if (arrayOperand.kind != nir::ValueKind::New) {
    return std::nullopt;
  }
  const std::optional<std::string> elementType =
      arrayElementTypeName(arrayOperand.type);
  if (!elementType || arrayOperand.text != arrayOperand.type ||
      !isRuntimeArrayApplyHelper(target, *elementType)) {
    return std::nullopt;
  }

  const nir::Value* exactIndex = exactImmutableValue(indexOperand, localValues);
  if (exactIndex == nullptr || exactIndex->type != "Int") {
    return std::nullopt;
  }
  const std::optional<long long> index = integerLiteralValue(*exactIndex);
  if (!index || *index < 0 ||
      static_cast<std::size_t>(*index) >= arrayOperand.operands.size()) {
    return std::nullopt;
  }

  const std::string resultName = freshLocalName("directArrayApplyResult", localNames);

  const std::size_t selectedIndex = static_cast<std::size_t>(*index);
  std::vector<nir::Value> evaluation;
  evaluation.reserve(arrayOperand.operands.size() + 1);
  for (std::size_t i = 0; i < arrayOperand.operands.size(); ++i) {
    if (i == selectedIndex) {
      evaluation.push_back(
          nir::localLetValue(resultName, *elementType, arrayOperand.operands[i], span));
    } else {
      evaluation.push_back(arrayOperand.operands[i]);
    }
  }
  nir::Value result = nir::localValue(resultName, span);
  result.type = *elementType;
  evaluation.push_back(std::move(result));
  return nir::blockValue(std::move(evaluation), span);
}

std::optional<nir::Value> foldDirectRuntimeArrayUpdate(
    std::string_view target, const nir::Value& arrayOperand,
    const nir::Value& indexOperand, const nir::Value& assignedValue,
    const std::unordered_map<std::string, nir::Value>& localValues,
    support::SourceSpan span) {
  if (arrayOperand.kind != nir::ValueKind::New) {
    return std::nullopt;
  }
  const std::optional<std::string> elementType =
      arrayElementTypeName(arrayOperand.type);
  if (!elementType || arrayOperand.text != arrayOperand.type ||
      !isRuntimeArrayUpdateHelper(target, *elementType)) {
    return std::nullopt;
  }

  const nir::Value* exactIndex = exactImmutableValue(indexOperand, localValues);
  if (exactIndex == nullptr || exactIndex->type != "Int") {
    return std::nullopt;
  }
  const std::optional<long long> index = integerLiteralValue(*exactIndex);
  if (!index || *index < 0 ||
      static_cast<std::size_t>(*index) >= arrayOperand.operands.size()) {
    return std::nullopt;
  }

  std::vector<nir::Value> evaluation = arrayOperand.operands;
  evaluation.reserve(evaluation.size() + 3);
  evaluation.push_back(indexOperand);
  evaluation.push_back(assignedValue);
  evaluation.push_back(nir::unitValue(span));
  return nir::blockValue(std::move(evaluation), span);
}

std::optional<nir::Value> foldExactRuntimeArrayApply(
    std::string_view target, const nir::Value& arrayOperand,
    const nir::Value& indexOperand,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  const nir::Value* array = exactLocalArrayAllocationValue(arrayOperand, localValues);
  if (array == nullptr || array->kind != nir::ValueKind::New) {
    return std::nullopt;
  }
  const std::optional<std::string> elementType = arrayElementTypeName(array->type);
  if (!elementType || array->text != array->type ||
      !isRuntimeArrayApplyHelper(target, *elementType)) {
    return std::nullopt;
  }

  const nir::Value* exactIndex = exactImmutableValue(indexOperand, localValues);
  if (exactIndex == nullptr || exactIndex->type != "Int") {
    return std::nullopt;
  }
  const std::optional<long long> index = integerLiteralValue(*exactIndex);
  if (!index || *index < 0 ||
      static_cast<std::size_t>(*index) >= array->operands.size()) {
    return std::nullopt;
  }

  const nir::Value& element = array->operands[static_cast<std::size_t>(*index)];
  if (!isPureLetValue(element)) {
    return std::nullopt;
  }
  return element;
}

std::optional<std::string> foldExactRuntimeToString(
    std::string_view target, const nir::Value& operand,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  const nir::Value* exact = exactImmutableValue(operand, localValues);
  if (target == support::StdNames::RuntimeIntToString) {
    if (exact == nullptr || exact->type != "Int") {
      return std::nullopt;
    }
    const std::optional<long long> value = integerLiteralValue(*exact);
    if (!value || *value < std::numeric_limits<std::int32_t>::min() ||
        *value > std::numeric_limits<std::int32_t>::max()) {
      return std::nullopt;
    }
    return std::to_string(*value);
  }
  if (target == support::StdNames::RuntimeLongToString) {
    if (exact == nullptr || exact->type != "Long") {
      return std::nullopt;
    }
    const std::optional<long long> value = integerLiteralValue(*exact);
    return value ? std::optional<std::string>(std::to_string(*value)) : std::nullopt;
  }
  if (target == support::StdNames::RuntimeBooleanToString) {
    if (exact == nullptr) {
      return std::nullopt;
    }
    const std::optional<bool> value = booleanLiteralValue(*exact);
    return value ? std::optional<std::string>(*value ? "true" : "false") : std::nullopt;
  }
  if (target == support::StdNames::RuntimeCharToString) {
    if (exact == nullptr) {
      return std::nullopt;
    }
    const std::optional<int> value = charLiteralValue(*exact);
    if (!value) {
      return std::nullopt;
    }
    return std::string(1, static_cast<char>(*value));
  }
  if (target == support::StdNames::RuntimeStringToString) {
    const nir::Value* value = exactPureStringLiteralValue(operand, localValues);
    if (value == nullptr) {
      return std::nullopt;
    }
    return stringLiteralValue(*value);
  }
  if (target != support::StdNames::RuntimeAnyToString &&
      target != support::StdNames::RuntimeAnyReceiverToString) {
    return std::nullopt;
  }
  if (target == support::StdNames::RuntimeAnyReceiverToString &&
      !isProvenNonNullReferenceValue(operand, localValues)) {
    return std::nullopt;
  }
  if (operand.kind == nir::ValueKind::Local) {
    if (exact != nullptr && isExactEvaluatedBoxValue(*exact) && exact->text == "Unit") {
      return "()";
    }
  }
  if (const std::optional<std::string> exactStringified =
          exactStringifiedAbiValue(operand, localValues)) {
    return exactStringified;
  }
  return std::nullopt;
}

std::optional<bool> foldExactRuntimeStringEquals(
    std::string_view target, const nir::Value& receiver, const nir::Value& argument,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  if (target != support::StdNames::RuntimeStringEquals ||
      isExactNullValue(receiver, localValues)) {
    return std::nullopt;
  }
  const nir::Value* left = exactPureStringLiteralValue(receiver, localValues);
  if (left == nullptr) {
    return std::nullopt;
  }
  if (isExactNullValue(argument, localValues)) {
    return false;
  }
  const std::optional<ExactBoxedAbiValue> boxed =
      exactPureBoxedAbiValue(argument, localValues);
  if (!boxed) {
    return std::nullopt;
  }
  if (boxed->type != "String") {
    return false;
  }
  const nir::Value* right = exactPureStringLiteralValue(*boxed->payload, localValues);
  if (right == nullptr) {
    return std::nullopt;
  }
  return stringLiteralValue(*left) == stringLiteralValue(*right);
}

std::optional<std::string>
foldExactRuntimeFormat(std::string_view target, const nir::Value& format,
                       const nir::Value& value,
                       const std::unordered_map<std::string, nir::Value>& localValues) {
  const nir::Value* exactFormat = exactPureStringLiteralValue(format, localValues);
  if (exactFormat == nullptr) {
    return std::nullopt;
  }
  const std::optional<std::string> formatText = stringLiteralValue(*exactFormat);
  if (!formatText) {
    return std::nullopt;
  }

  if (target == support::StdNames::RuntimeFormat && *formatText == "%s") {
    const nir::Value* exactString = exactPureStringLiteralValue(value, localValues);
    if (exactString == nullptr) {
      return std::nullopt;
    }
    return stringLiteralValue(*exactString);
  }
  if (target == support::StdNames::RuntimeFormat && *formatText == "%c") {
    const nir::Value* exact = exactImmutableValue(value, localValues);
    if (exact == nullptr) {
      return std::nullopt;
    }
    const std::optional<int> character = charLiteralValue(*exact);
    if (!character) {
      return std::nullopt;
    }
    return std::string(1, static_cast<char>(*character));
  }
  if (target == support::StdNames::RuntimeFormatBoolean && *formatText == "%s") {
    const nir::Value* exact = exactImmutableValue(value, localValues);
    if (exact == nullptr) {
      return std::nullopt;
    }
    const std::optional<bool> boolean = booleanLiteralValue(*exact);
    return boolean ? std::optional<std::string>(*boolean ? "true" : "false")
                   : std::nullopt;
  }
  return std::nullopt;
}

std::optional<bool> exactEvaluatedBoxAnyEqualsResult(const nir::Value& left,
                                                     const nir::Value& right) {
  if (!isExactEvaluatedBoxValue(left) || !isExactEvaluatedBoxValue(right)) {
    return std::nullopt;
  }
  if (left.text != right.text) {
    return false;
  }
  return left.text == "Unit" ? std::optional<bool>(true) : std::nullopt;
}

std::optional<nir::Value> foldDirectEvaluatedBoxAnyEquals(const nir::Value& left,
                                                          const nir::Value& right,
                                                          support::SourceSpan span) {
  const std::optional<bool> result = exactEvaluatedBoxAnyEqualsResult(left, right);
  if (!result) {
    return std::nullopt;
  }
  return nir::blockValue({left, right, booleanLiteral(*result, span)}, span);
}

std::optional<bool> foldExactLocalEvaluatedBoxAnyEquals(
    const nir::Value& left, const nir::Value& right,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  if (left.kind != nir::ValueKind::Local || right.kind != nir::ValueKind::Local) {
    return std::nullopt;
  }
  const nir::Value* lhs = exactImmutableValue(left, localValues);
  const nir::Value* rhs = exactImmutableValue(right, localValues);
  return lhs == nullptr || rhs == nullptr
             ? std::nullopt
             : exactEvaluatedBoxAnyEqualsResult(*lhs, *rhs);
}

std::optional<bool> foldExactBoxedAnyEquals(
    const nir::Value& left, const nir::Value& right,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  if (std::optional<bool> sameReference =
          foldExactSameReferenceComparison("==", left, right, localValues)) {
    return sameReference;
  }
  if (std::optional<bool> distinctFresh =
          foldExactDistinctFreshReferenceComparison("==", left, right, localValues)) {
    return distinctFresh;
  }

  const bool lhsNull = isExactNullValue(left, localValues);
  const bool rhsNull = isExactNullValue(right, localValues);
  if (lhsNull || rhsNull) {
    if (lhsNull && rhsNull) {
      return true;
    }
    const nir::Value& nonNullCandidate = lhsNull ? right : left;
    if (isExactPureNonNullReferenceValue(nonNullCandidate, localValues) ||
        exactImmutableNonNullReferenceRootName(nonNullCandidate, localValues)) {
      return false;
    }
    return std::nullopt;
  }

  const nir::Value* lhsString = exactPureStringLiteralValue(left, localValues);
  const nir::Value* rhsString = exactPureStringLiteralValue(right, localValues);
  if (lhsString != nullptr || rhsString != nullptr) {
    if (lhsString == nullptr || rhsString == nullptr) {
      return std::nullopt;
    }
    if (lhsString->text == rhsString->text) {
      return true;
    }
    const std::optional<std::string> lhsText = stringLiteralValue(*lhsString);
    const std::optional<std::string> rhsText = stringLiteralValue(*rhsString);
    return lhsText && rhsText && *lhsText != *rhsText ? std::optional<bool>(false)
                                                      : std::nullopt;
  }

  const std::optional<ExactBoxedAbiValue> lhs =
      exactPureBoxedAbiValue(left, localValues);
  const std::optional<ExactBoxedAbiValue> rhs =
      exactPureBoxedAbiValue(right, localValues);
  if (!lhs || !rhs) {
    return std::nullopt;
  }
  if (lhs->type != rhs->type) {
    return false;
  }
  return boxedPayloadsEqual(lhs->type, *lhs->payload, *rhs->payload);
}

std::optional<bool> foldExactNullComparison(
    std::string_view op, const nir::Value& left, const nir::Value& right,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  if (op != "==" && op != "!=") {
    return std::nullopt;
  }
  const bool lhsNull = isExactNullValue(left, localValues);
  const bool rhsNull = isExactNullValue(right, localValues);
  if (!lhsNull && !rhsNull) {
    return std::nullopt;
  }

  bool equal = false;
  if (lhsNull && rhsNull) {
    equal = true;
  } else {
    const nir::Value& nonNullCandidate = lhsNull ? right : left;
    if (!isExactPureNonNullReferenceValue(nonNullCandidate, localValues) &&
        !exactImmutableNonNullReferenceRootName(nonNullCandidate, localValues)) {
      return std::nullopt;
    }
  }
  return op == "==" ? equal : !equal;
}

std::optional<nir::Value> foldDirectEvaluatedNonNullNullComparison(
    std::string_view op, const nir::Value& left, const nir::Value& right,
    const std::unordered_map<std::string, nir::Value>& localValues,
    support::SourceSpan span) {
  if (op != "==" && op != "!=") {
    return std::nullopt;
  }
  const bool lhsNull = isExactNullValue(left, localValues);
  const bool rhsNull = isExactNullValue(right, localValues);
  if (lhsNull == rhsNull) {
    return std::nullopt;
  }
  const nir::Value& nonNull = lhsNull ? right : left;
  const bool exactEvaluatedNonNull =
      nonNull.kind == nir::ValueKind::New || isExactEvaluatedBoxValue(nonNull);
  if (!exactEvaluatedNonNull) {
    return std::nullopt;
  }
  return nir::blockValue({left, right, booleanLiteral(op == "!=", span)}, span);
}

bool isExactNonNullInstanceOfOperand(const nir::Value& operand,
                                     std::string_view targetType) {
  const std::optional<std::string> exactType = exactAbiOperandType(operand);
  return exactType && *exactType == targetType;
}

bool isDisjointExactAbiInstanceOfOperand(const nir::Value& operand,
                                         std::string_view targetType) {
  const std::optional<std::string> exactType = exactAbiOperandType(operand);
  return exactType && isBoxedAbiType(targetType) && *exactType != targetType;
}

bool isExactNonNullInstanceOfLocal(
    const nir::Value& operand, std::string_view targetType,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  const nir::Value* exact = exactImmutableValue(operand, localValues);
  return exact != nullptr && isExactNonNullInstanceOfOperand(*exact, targetType);
}

bool isDisjointExactAbiInstanceOfLocal(
    const nir::Value& operand, std::string_view targetType,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  const nir::Value* exact = exactImmutableValue(operand, localValues);
  return exact != nullptr && isDisjointExactAbiInstanceOfOperand(*exact, targetType);
}

std::optional<bool> exactEvaluatedBoxInstanceOfResult(const nir::Value& operand,
                                                      std::string_view targetType) {
  if (!isExactEvaluatedBoxValue(operand)) {
    return std::nullopt;
  }
  if (targetType == operand.text || isTopObjectTarget(targetType)) {
    return true;
  }
  return isBoxedAbiType(targetType) ? std::optional<bool>(false) : std::nullopt;
}

std::optional<nir::Value> foldExactEvaluatedBoxInstanceOfOperand(
    const nir::Value& operand, std::string_view targetType, support::SourceSpan span) {
  const std::optional<bool> result =
      exactEvaluatedBoxInstanceOfResult(operand, targetType);
  if (!result) {
    return std::nullopt;
  }
  return nir::blockValue({operand, booleanLiteral(*result, span)}, span);
}

std::optional<bool> foldExactEvaluatedBoxInstanceOfLocal(
    const nir::Value& operand, std::string_view targetType,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  if (operand.kind != nir::ValueKind::Local) {
    return std::nullopt;
  }
  const nir::Value* exact = exactImmutableValue(operand, localValues);
  return exact == nullptr ? std::nullopt
                          : exactEvaluatedBoxInstanceOfResult(*exact, targetType);
}

std::optional<bool> exactNewInstanceOfResult(std::string_view sourceType,
                                             std::string_view targetType,
                                             const DefinitionIndex& definitions,
                                             const ParentMap& parentMap) {
  if (sourceType.empty()) {
    return std::nullopt;
  }
  if (arrayElementTypeName(sourceType)) {
    return isTopObjectTarget(targetType) ? std::optional<bool>(true) : std::nullopt;
  }
  if (!isKnownClassLikeType(sourceType, definitions)) {
    return std::nullopt;
  }
  if (isTopObjectTarget(targetType)) {
    return true;
  }
  if (!isKnownClassLikeType(targetType, definitions)) {
    return std::nullopt;
  }
  return knownReferenceTypeConformsTo(sourceType, targetType, definitions, parentMap);
}

std::optional<nir::Value> foldExactEvaluatedNewInstanceOfOperand(
    const nir::Value& operand, std::string_view targetType,
    const DefinitionIndex& definitions, const ParentMap& parentMap,
    support::SourceSpan span) {
  if (operand.kind != nir::ValueKind::New) {
    return std::nullopt;
  }

  const std::string sourceType = operand.text.empty() ? operand.type : operand.text;
  const std::optional<bool> result =
      exactNewInstanceOfResult(sourceType, targetType, definitions, parentMap);
  if (!result) {
    return std::nullopt;
  }
  return nir::blockValue({operand, booleanLiteral(*result, span)}, span);
}

std::optional<bool> foldExactEvaluatedNewInstanceOfLocal(
    const nir::Value& operand, std::string_view targetType,
    const std::unordered_map<std::string, nir::Value>& localValues,
    const DefinitionIndex& definitions, const ParentMap& parentMap) {
  if (operand.kind != nir::ValueKind::Local) {
    return std::nullopt;
  }
  const nir::Value* exact = exactImmutableValue(operand, localValues);
  if (exact == nullptr || exact->kind != nir::ValueKind::New) {
    return std::nullopt;
  }
  const std::string sourceType = exact->text.empty() ? exact->type : exact->text;
  return exactNewInstanceOfResult(sourceType, targetType, definitions, parentMap);
}

std::optional<bool> sameLocalComparisonResult(std::string_view op) {
  if (op == "==" || op == "<=" || op == ">=") {
    return true;
  }
  if (op == "!=" || op == "<" || op == ">") {
    return false;
  }
  return std::nullopt;
}

std::optional<std::string> invertedOrderedComparison(std::string_view op) {
  if (op == "<") {
    return ">=";
  }
  if (op == ">") {
    return "<=";
  }
  if (op == "<=") {
    return ">";
  }
  if (op == ">=") {
    return "<";
  }
  return std::nullopt;
}

std::optional<std::string> canonicalOrderedComparison(std::string_view op) {
  if (op == ">") {
    return "<";
  }
  if (op == ">=") {
    return "<=";
  }
  return std::nullopt;
}

bool canonicalizeOrderedComparisonDirection(
    nir::Value& value, const std::unordered_map<std::string, std::string>& localTypes) {
  if (value.kind != nir::ValueKind::Binary || value.operands.size() != 2) {
    return false;
  }
  const std::optional<std::string> canonical = canonicalOrderedComparison(value.text);
  if (!canonical) {
    return false;
  }
  const std::string lhsType = knownValueType(value.operands[0], localTypes);
  const std::string rhsType = knownValueType(value.operands[1], localTypes);
  if (lhsType != rhsType || !isOrderedNonFloatingComparisonType(lhsType) ||
      !isPureLetValue(value.operands[0]) || !isPureLetValue(value.operands[1])) {
    return false;
  }
  value.text = *canonical;
  std::swap(value.operands[0], value.operands[1]);
  return true;
}

void substituteInlineParameters(
    nir::Value& value, const std::unordered_map<std::string, nir::Value>& replacements);

std::optional<InlineReturnSummary>
inlineableReturnSummary(const nir::Definition& definition) {
  if (definition.kind != nir::DefinitionKind::FunctionDef ||
      definition.body.instructions.empty()) {
    return std::nullopt;
  }

  InlineReturnSummary summary;
  std::unordered_set<std::string> parameterNames;
  std::unordered_set<std::string> allowedLocals;
  std::unordered_map<std::string, nir::Value> pureLocalValues;
  std::size_t instructionIndex = 0;
  while (instructionIndex < definition.body.instructions.size() &&
         definition.body.instructions[instructionIndex].kind ==
             nir::InstructionKind::Param) {
    const std::string& name = definition.body.instructions[instructionIndex].name;
    if (name.empty() || allowedLocals.contains(name)) {
      return std::nullopt;
    }
    summary.parameters.push_back(name);
    parameterNames.insert(name);
    allowedLocals.insert(name);
    ++instructionIndex;
  }

  while (instructionIndex < definition.body.instructions.size() &&
         definition.body.instructions[instructionIndex].kind ==
             nir::InstructionKind::Let) {
    const nir::Instruction& instruction =
        definition.body.instructions[instructionIndex];
    if (instruction.name.empty() || allowedLocals.contains(instruction.name) ||
        !isPureLetValue(instruction.value) ||
        !usesOnlyAllowedLocals(instruction.value, allowedLocals)) {
      return std::nullopt;
    }

    nir::Value value = instruction.value;
    substituteInlineParameters(value, pureLocalValues);
    if (!isPureLetValue(value) || !usesOnlyAllowedLocals(value, parameterNames)) {
      return std::nullopt;
    }
    pureLocalValues.emplace(instruction.name, std::move(value));
    allowedLocals.insert(instruction.name);
    ++instructionIndex;
  }

  if (instructionIndex + 1 != definition.body.instructions.size()) {
    return std::nullopt;
  }

  const nir::Instruction& instruction = definition.body.instructions[instructionIndex];
  if (instruction.kind != nir::InstructionKind::Return ||
      !isPureLetValue(instruction.value) ||
      !usesOnlyAllowedLocals(instruction.value, allowedLocals)) {
    return std::nullopt;
  }

  nir::Value value = instruction.value;
  substituteInlineParameters(value, pureLocalValues);
  if (!isPureLetValue(value) || !usesOnlyAllowedLocals(value, parameterNames)) {
    return std::nullopt;
  }
  summary.value = std::move(value);
  return summary;
}

InlineReturnMap collectInlineableReturnSummaries(const linker::LinkedProgram& program) {
  InlineReturnMap values;
  for (const nir::Module& module : program.modules) {
    for (const nir::Definition& definition : module.definitions) {
      if (std::optional<InlineReturnSummary> summary =
              inlineableReturnSummary(definition)) {
        values.emplace(definition.name, std::move(*summary));
      }
    }
  }
  return values;
}

void substituteInlineParameters(
    nir::Value& value,
    const std::unordered_map<std::string, nir::Value>& replacements) {
  if (value.kind == nir::ValueKind::Local) {
    auto replacement = replacements.find(value.text);
    if (replacement != replacements.end()) {
      replaceValue(value, replacement->second);
    }
    return;
  }

  for (nir::Value& operand : value.operands) {
    substituteInlineParameters(operand, replacements);
  }
}

const InlineReturnSummary*
inlineSummaryForReference(const std::string& reference, const std::string& currentOwner,
                          const std::string& currentModule,
                          const InlineReturnMap& inlineValues) {
  if (!currentOwner.empty()) {
    const std::string candidate = currentOwner + "." + reference;
    auto found = inlineValues.find(candidate);
    if (found != inlineValues.end()) {
      return &found->second;
    }
  }
  if (!currentModule.empty()) {
    const std::string candidate = currentModule + "." + reference;
    auto found = inlineValues.find(candidate);
    if (found != inlineValues.end()) {
      return &found->second;
    }
  }
  auto found = inlineValues.find(reference);
  return found == inlineValues.end() ? nullptr : &found->second;
}

const nir::Definition* definitionForReference(const std::string& reference,
                                              const std::string& currentOwner,
                                              const std::string& currentModule,
                                              const DefinitionIndex& definitions) {
  if (!currentOwner.empty()) {
    auto found = definitions.find(currentOwner + "." + reference);
    if (found != definitions.end()) {
      return found->second;
    }
  }
  if (!currentModule.empty()) {
    auto found = definitions.find(currentModule + "." + reference);
    if (found != definitions.end()) {
      return found->second;
    }
  }
  auto found = definitions.find(reference);
  return found == definitions.end() ? nullptr : found->second;
}

bool requiresNonNullReceiverForInlining(const std::string& reference,
                                        const std::string& currentOwner,
                                        const std::string& currentModule,
                                        const DefinitionIndex& definitions) {
  const nir::Definition* definition =
      definitionForReference(reference, currentOwner, currentModule, definitions);
  if (definition == nullptr) {
    return false;
  }
  auto owner = definitions.find(ownerNameOf(definition->name));
  return owner != definitions.end() && owner->second != nullptr &&
         (owner->second->kind == nir::DefinitionKind::Class ||
          owner->second->kind == nir::DefinitionKind::Trait);
}

std::optional<nir::Value> inlineZeroArgLocalValue(const nir::Value& value,
                                                  const std::string& currentOwner,
                                                  const std::string& currentModule,
                                                  const InlineReturnMap& inlineValues) {
  if (value.kind != nir::ValueKind::Local) {
    return std::nullopt;
  }

  const InlineReturnSummary* summary =
      inlineSummaryForReference(value.text, currentOwner, currentModule, inlineValues);
  if (summary == nullptr || !summary->parameters.empty()) {
    return std::nullopt;
  }
  return summary->value;
}

std::optional<nir::Value>
inlineCallValue(const nir::Value& value, const std::string& currentOwner,
                const std::string& currentModule, const InlineReturnMap& inlineValues,
                const DefinitionIndex& definitions, const ParentMap& parentMap,
                const ConstructorFieldMap& constructorFields,
                const FieldSnapshotMap& fieldSnapshots,
                const std::unordered_map<std::string, nir::Value>& localValues) {
  if (value.kind != nir::ValueKind::Call || value.operands.empty() ||
      value.operands.front().kind != nir::ValueKind::Local) {
    return std::nullopt;
  }

  const InlineReturnSummary* summary = inlineSummaryForReference(
      value.operands.front().text, currentOwner, currentModule, inlineValues);
  if (summary == nullptr) {
    return std::nullopt;
  }

  if (value.operands.size() != summary->parameters.size() + 1) {
    return std::nullopt;
  }

  if (requiresNonNullReceiverForInlining(value.operands.front().text, currentOwner,
                                         currentModule, definitions)) {
    if (summary->parameters.empty()) {
      return std::nullopt;
    }
    const nir::Value& receiver = value.operands[1];
    if (!isProvenNonNullReferenceValue(receiver, localValues)) {
      return std::nullopt;
    }
  }

  std::unordered_map<std::string, nir::Value> replacements;
  replacements.reserve(summary->parameters.size());
  for (std::size_t i = 0; i < summary->parameters.size(); ++i) {
    const nir::Value& argument = value.operands[i + 1];
    const std::string& parameter = summary->parameters[i];
    const bool parameterUsed = collectVisibleUses(summary->value, parameter) != 0;
    const bool parameterForwarded = summary->value.kind == nir::ValueKind::Local &&
                                    summary->value.text == parameter;
    if (!isPureLetValue(argument)) {
      if (parameterUsed && !parameterForwarded) {
        return std::nullopt;
      }
      if (!parameterUsed &&
          !isUnusedAllocationDiscardable(argument, definitions, parentMap,
                                         constructorFields, fieldSnapshots)) {
        return std::nullopt;
      }
      if (parameterForwarded) {
        replacements.emplace(parameter, argument);
      }
      continue;
    }
    if (parameterUsed) {
      replacements.emplace(parameter, argument);
    }
  }

  nir::Value inlined = summary->value;
  substituteInlineParameters(inlined, replacements);
  return inlined;
}

std::string trim(std::string_view text) {
  while (!text.empty() && text.front() == ' ') {
    text.remove_prefix(1);
  }
  while (!text.empty() && text.back() == ' ') {
    text.remove_suffix(1);
  }
  return std::string(text);
}

std::vector<std::string> signatureParameterTypes(const std::string& signature) {
  std::vector<std::string> types;
  const std::size_t open = signature.find('(');
  const std::size_t close = signature.find(')');
  if (open != 0 || close == std::string::npos || close <= open + 1) {
    return types;
  }

  std::string_view parameters(signature.data() + open + 1, close - open - 1);
  std::size_t start = 0;
  for (std::size_t i = 0; i < parameters.size(); ++i) {
    if (parameters[i] == ',') {
      types.push_back(trim(parameters.substr(start, i - start)));
      start = i + 1;
    }
  }
  types.push_back(trim(parameters.substr(start)));
  return types;
}

std::optional<std::string>
exactNewReceiverType(const nir::Value& receiver,
                     const std::unordered_map<std::string, nir::Value>& localValues) {
  const nir::Value* current = &receiver;
  std::unordered_set<std::string> seen;
  while (current != nullptr) {
    if (current->kind == nir::ValueKind::New && !arrayElementTypeName(current->type)) {
      return current->text.empty() ? current->type : current->text;
    }
    if (current->kind != nir::ValueKind::Local || !seen.insert(current->text).second) {
      return std::nullopt;
    }
    auto local = localValues.find(current->text);
    if (local == localValues.end()) {
      return std::nullopt;
    }
    current = &local->second;
  }
  return std::nullopt;
}

bool hasKnownConcreteSubtype(const std::string& typeName,
                             const DefinitionIndex& definitions,
                             const ParentMap& parentMap) {
  for (const auto& [candidateName, definition] : definitions) {
    if (candidateName == typeName || definition == nullptr ||
        !isConcreteDispatchDefinitionKind(definition->kind)) {
      continue;
    }
    const std::vector<std::string> linearized =
        nir::linearizedTypeNames(candidateName, parentMap);
    if (std::find(linearized.begin(), linearized.end(), typeName) != linearized.end()) {
      return true;
    }
  }
  return false;
}

std::optional<std::string>
closedStaticReceiverType(const nir::Value& receiver,
                         const std::unordered_map<std::string, std::string>& localTypes,
                         const DefinitionIndex& definitions,
                         const ParentMap& parentMap) {
  if (receiver.kind != nir::ValueKind::Local) {
    return std::nullopt;
  }
  auto localType = localTypes.find(receiver.text);
  if (localType == localTypes.end()) {
    return std::nullopt;
  }

  auto definition = definitions.find(localType->second);
  if (definition == definitions.end() || definition->second == nullptr ||
      !isConcreteDispatchDefinitionKind(definition->second->kind) ||
      hasKnownConcreteSubtype(localType->second, definitions, parentMap)) {
    return std::nullopt;
  }
  return localType->second;
}

std::optional<std::string>
monomorphicReceiverType(const nir::Value& receiver,
                        const std::unordered_map<std::string, std::string>& localTypes,
                        const std::unordered_map<std::string, nir::Value>& localValues,
                        const DefinitionIndex& definitions,
                        const ParentMap& parentMap) {
  if (std::optional<std::string> exact = exactNewReceiverType(receiver, localValues)) {
    return exact;
  }
  return closedStaticReceiverType(receiver, localTypes, definitions, parentMap);
}

std::optional<std::string> resolveExactFunctionMember(
    const std::string& receiverType, const std::string& memberName,
    const DefinitionIndex& definitions, const ParentMap& parentMap) {
  auto receiverDefinition = definitions.find(receiverType);
  if (receiverDefinition == definitions.end() ||
      receiverDefinition->second == nullptr ||
      !isClassLikeDefinitionKind(receiverDefinition->second->kind)) {
    return std::nullopt;
  }

  for (const std::string& current : nir::linearizedTypeNames(receiverType, parentMap)) {
    const std::string candidate = current + "." + memberName;
    auto definition = definitions.find(candidate);
    if (definition == definitions.end()) {
      continue;
    }
    if (definition->second != nullptr &&
        isFunctionDefinitionKind(definition->second->kind)) {
      return candidate;
    }
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<std::string> resolveExactFieldMember(const std::string& receiverType,
                                                   const std::string& memberName,
                                                   const DefinitionIndex& definitions,
                                                   const ParentMap& parentMap) {
  auto receiverDefinition = definitions.find(receiverType);
  if (receiverDefinition == definitions.end() ||
      receiverDefinition->second == nullptr ||
      !isClassLikeDefinitionKind(receiverDefinition->second->kind)) {
    return std::nullopt;
  }

  for (const std::string& current : nir::linearizedTypeNames(receiverType, parentMap)) {
    const std::string candidate = current + "." + memberName;
    auto definition = definitions.find(candidate);
    if (definition == definitions.end()) {
      continue;
    }
    if (definition->second != nullptr &&
        definition->second->kind == nir::DefinitionKind::Field) {
      return candidate;
    }
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<std::size_t>
constructorFieldIndex(const std::string& receiverType, const std::string& fieldName,
                      const ConstructorFieldMap& constructorFields) {
  auto fields = constructorFields.find(receiverType);
  if (fields == constructorFields.end()) {
    return std::nullopt;
  }
  auto found = std::find(fields->second.begin(), fields->second.end(), fieldName);
  if (found == fields->second.end()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(std::distance(fields->second.begin(), found));
}

std::optional<std::size_t> exactConstructorFieldIndex(
    const std::string& receiverType, const std::string& memberName,
    const DefinitionIndex& definitions, const ParentMap& parentMap,
    const ConstructorFieldMap& constructorFields) {
  const std::optional<std::string> field =
      resolveExactFieldMember(receiverType, memberName, definitions, parentMap);
  if (!field || ownerNameOf(*field) != receiverType) {
    return std::nullopt;
  }
  return constructorFieldIndex(receiverType, *field, constructorFields);
}

std::optional<std::string> resolveInitializerTargetField(
    const nir::Value& target, const std::string& initializerOwner,
    const DefinitionIndex& definitions, const ParentMap& parentMap) {
  if (target.kind != nir::ValueKind::Select || target.operands.size() != 1 ||
      target.text.empty()) {
    return std::nullopt;
  }

  const nir::Value& receiver = target.operands.front();
  std::optional<std::string> receiverType;
  if (receiver.kind == nir::ValueKind::Local && receiver.text == "this") {
    receiverType = initializerOwner;
  } else if (receiver.kind == nir::ValueKind::Super && !receiver.type.empty()) {
    receiverType = receiver.type;
  }
  if (!receiverType) {
    return std::nullopt;
  }
  return resolveExactFieldMember(*receiverType, target.text, definitions, parentMap);
}

bool isUnitReturn(const nir::Instruction& instruction) {
  return instruction.kind == nir::InstructionKind::Return &&
         instruction.type == "Unit" && instruction.value.kind == nir::ValueKind::Unit;
}

std::optional<std::vector<std::string>>
simpleInitializerAssignedFields(const std::string& typeName,
                                const DefinitionIndex& definitions,
                                const ParentMap& parentMap) {
  const std::string initializerName =
      typeName + "." + std::string(support::StdNames::Constructor);
  auto initializer = definitions.find(initializerName);
  if (initializer == definitions.end()) {
    return std::vector<std::string>{};
  }
  if (initializer->second == nullptr ||
      initializer->second->kind != nir::DefinitionKind::FunctionDef) {
    return std::nullopt;
  }

  std::vector<std::string> fields;
  std::unordered_set<std::string> seenFields;
  bool sawNonParam = false;
  for (std::size_t index = 0; index < initializer->second->body.instructions.size();
       ++index) {
    const nir::Instruction& instruction = initializer->second->body.instructions[index];
    if (instruction.kind == nir::InstructionKind::Param) {
      if (sawNonParam || instruction.name != "this" || instruction.type != typeName) {
        return std::nullopt;
      }
      continue;
    }

    sawNonParam = true;
    if (isUnitReturn(instruction)) {
      return index + 1 == initializer->second->body.instructions.size()
                 ? std::optional<std::vector<std::string>>(fields)
                 : std::nullopt;
    }

    if (instruction.kind != nir::InstructionKind::Eval ||
        instruction.value.kind != nir::ValueKind::Assign ||
        instruction.value.operands.size() != 2) {
      return std::nullopt;
    }

    const std::optional<std::string> field = resolveInitializerTargetField(
        instruction.value.operands.front(), typeName, definitions, parentMap);
    if (!field) {
      return std::nullopt;
    }
    if (seenFields.insert(*field).second) {
      fields.push_back(*field);
    }
  }
  return std::nullopt;
}

FieldSnapshotMap fieldSnapshotMapFor(const DefinitionIndex& definitions,
                                     const ParentMap& parentMap,
                                     const ConstructorFieldMap& constructorFields) {
  FieldSnapshotMap snapshots(constructorFields.begin(), constructorFields.end());
  for (const auto& [name, definition] : definitions) {
    if (definition == nullptr || definition->kind != nir::DefinitionKind::Class) {
      continue;
    }

    auto assignedFields = simpleInitializerAssignedFields(name, definitions, parentMap);
    if (!assignedFields) {
      continue;
    }

    std::vector<std::string>& fields = snapshots[name];
    std::unordered_set<std::string> seen(fields.begin(), fields.end());
    for (const std::string& field : *assignedFields) {
      if (seen.insert(field).second) {
        fields.push_back(field);
      }
    }
  }
  return snapshots;
}

std::optional<std::size_t>
fieldSnapshotIndex(const std::string& objectType, const std::string& receiverType,
                   const std::string& memberName, const DefinitionIndex& definitions,
                   const ParentMap& parentMap, const FieldSnapshotMap& fieldSnapshots) {
  const std::optional<std::string> field =
      resolveExactFieldMember(receiverType, memberName, definitions, parentMap);
  if (!field) {
    return std::nullopt;
  }

  auto fields = fieldSnapshots.find(objectType);
  if (fields == fieldSnapshots.end()) {
    return std::nullopt;
  }
  auto found = std::find(fields->second.begin(), fields->second.end(), *field);
  if (found == fields->second.end()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(std::distance(fields->second.begin(), found));
}

bool substituteExactSnapshotFields(nir::Value& value, const nir::Value& object,
                                   const std::string& objectType,
                                   const DefinitionIndex& definitions,
                                   const ParentMap& parentMap,
                                   const FieldSnapshotMap& fieldSnapshots) {
  bool changed = false;
  for (nir::Value& operand : value.operands) {
    changed = substituteExactSnapshotFields(operand, object, objectType, definitions,
                                            parentMap, fieldSnapshots) ||
              changed;
  }

  if (value.kind != nir::ValueKind::Select || value.operands.size() != 1 ||
      value.text.empty()) {
    return changed;
  }

  const nir::Value& receiver = value.operands.front();
  std::optional<std::string> receiverType;
  if (receiver.kind == nir::ValueKind::Local && receiver.text == "this") {
    receiverType = objectType;
  } else if (receiver.kind == nir::ValueKind::Super && !receiver.type.empty()) {
    receiverType = receiver.type;
  }
  if (!receiverType) {
    return changed;
  }

  const std::optional<std::size_t> index = fieldSnapshotIndex(
      objectType, *receiverType, value.text, definitions, parentMap, fieldSnapshots);
  if (!index || *index >= object.operands.size()) {
    return changed;
  }
  const nir::Value& replacement = object.operands[*index];
  if (!isPureLetValue(replacement)) {
    return changed;
  }

  replaceValue(value, replacement);
  return true;
}

nir::Value extendExactNewFieldSnapshot(const nir::Value& value,
                                       const DefinitionIndex& definitions,
                                       const ParentMap& parentMap,
                                       const ConstructorFieldMap& constructorFields,
                                       const FieldSnapshotMap& fieldSnapshots) {
  nir::Value object = value;
  if (object.kind != nir::ValueKind::New || arrayElementTypeName(object.type)) {
    return object;
  }

  const std::string objectType = object.text.empty() ? object.type : object.text;
  auto constructorFieldNames = constructorFields.find(objectType);
  if (constructorFieldNames == constructorFields.end() ||
      object.operands.size() != constructorFieldNames->second.size()) {
    return object;
  }

  auto snapshotFieldNames = fieldSnapshots.find(objectType);
  if (snapshotFieldNames == fieldSnapshots.end() ||
      snapshotFieldNames->second.size() <= object.operands.size()) {
    return object;
  }

  const std::string initializerName =
      objectType + "." + std::string(support::StdNames::Constructor);
  auto initializer = definitions.find(initializerName);
  if (initializer == definitions.end() || initializer->second == nullptr) {
    return object;
  }

  std::unordered_set<std::string> allowedLocals;
  for (const nir::Value& operand : object.operands) {
    collectLocalNames(operand, allowedLocals);
  }

  for (const nir::Instruction& instruction : initializer->second->body.instructions) {
    if (instruction.kind == nir::InstructionKind::Param || isUnitReturn(instruction)) {
      continue;
    }

    if (instruction.kind != nir::InstructionKind::Eval ||
        instruction.value.kind != nir::ValueKind::Assign ||
        instruction.value.operands.size() != 2) {
      return value;
    }

    const std::optional<std::string> field = resolveInitializerTargetField(
        instruction.value.operands.front(), objectType, definitions, parentMap);
    if (!field) {
      return value;
    }
    auto fieldName = std::find(snapshotFieldNames->second.begin(),
                               snapshotFieldNames->second.end(), *field);
    if (fieldName == snapshotFieldNames->second.end()) {
      return value;
    }
    const std::size_t index = static_cast<std::size_t>(
        std::distance(snapshotFieldNames->second.begin(), fieldName));

    nir::Value assigned = instruction.value.operands.back();
    (void)substituteExactSnapshotFields(assigned, object, objectType, definitions,
                                        parentMap, fieldSnapshots);
    if (!isPureLetValue(assigned) || !usesOnlyAllowedLocals(assigned, allowedLocals)) {
      return value;
    }

    while (object.operands.size() <= index) {
      object.operands.push_back(
          nir::unknownValue("<unknown-field>", instruction.value.span));
    }
    object.operands[index] = std::move(assigned);
  }

  return object;
}

bool isTrivialConstructorInitializer(const nir::Definition& definition,
                                     std::string_view receiverType) {
  if (definition.kind != nir::DefinitionKind::FunctionDef ||
      definition.body.instructions.size() != 2) {
    return false;
  }

  const nir::Instruction& parameter = definition.body.instructions[0];
  const nir::Instruction& result = definition.body.instructions[1];
  return parameter.kind == nir::InstructionKind::Param && parameter.name == "this" &&
         parameter.type == receiverType &&
         result.kind == nir::InstructionKind::Return && result.type == "Unit" &&
         result.value.kind == nir::ValueKind::Unit;
}

bool hasOnlyFoldableDirectConstructorFieldStorage(
    const std::string& typeName, const DefinitionIndex& definitions,
    const ParentMap& parentMap, const ConstructorFieldMap& constructorFields) {
  auto fields = constructorFields.find(typeName);
  if (fields == constructorFields.end()) {
    return false;
  }

  const std::unordered_set<std::string> ownConstructorFields(fields->second.begin(),
                                                             fields->second.end());
  for (const std::string& current : nir::linearizedTypeNames(typeName, parentMap)) {
    const std::string initializerName =
        current + "." + std::string(support::StdNames::Constructor);
    auto initializer = definitions.find(initializerName);
    if (initializer != definitions.end() &&
        (initializer->second == nullptr ||
         !isTrivialConstructorInitializer(*initializer->second, current))) {
      return false;
    }

    for (const auto& [name, definition] : definitions) {
      if (definition == nullptr || definition->kind != nir::DefinitionKind::Field ||
          ownerNameOf(name) != current) {
        continue;
      }
      if (current != typeName || !definition->body.empty() ||
          !ownConstructorFields.contains(name)) {
        return false;
      }
    }
  }
  return true;
}

bool isDirectConstructorFieldAllocationShellFoldable(
    const nir::Value& object, const std::string& receiverType,
    const DefinitionIndex& definitions, const ParentMap& parentMap,
    const ConstructorFieldMap& constructorFields) {
  if (object.kind != nir::ValueKind::New || arrayElementTypeName(object.type)) {
    return false;
  }

  auto definition = definitions.find(receiverType);
  if (definition == definitions.end() || definition->second == nullptr ||
      definition->second->kind != nir::DefinitionKind::Class) {
    return false;
  }

  auto fields = constructorFields.find(receiverType);
  if (fields == constructorFields.end() ||
      object.operands.size() != fields->second.size()) {
    return false;
  }

  return hasOnlyFoldableDirectConstructorFieldStorage(receiverType, definitions,
                                                      parentMap, constructorFields);
}

bool foldExactConstructorFieldSelect(
    nir::Value& value, const std::unordered_map<std::string, nir::Value>& localValues,
    const std::unordered_set<std::string>& localNames,
    const DefinitionIndex& definitions, const ParentMap& parentMap,
    const ConstructorFieldMap& constructorFields,
    const FieldSnapshotMap& fieldSnapshots) {
  if (value.kind != nir::ValueKind::Select || value.operands.size() != 1 ||
      value.text.empty()) {
    return false;
  }

  if (value.operands.front().kind == nir::ValueKind::New) {
    const nir::Value& object = value.operands.front();
    const std::string receiverType = object.text.empty() ? object.type : object.text;
    const std::optional<std::size_t> index = exactConstructorFieldIndex(
        receiverType, value.text, definitions, parentMap, constructorFields);
    if (!index || *index >= object.operands.size() ||
        !isDirectConstructorFieldAllocationShellFoldable(
            object, receiverType, definitions, parentMap, constructorFields)) {
      return false;
    }

    auto fields = constructorFields.find(receiverType);
    if (fields == constructorFields.end() || *index >= fields->second.size()) {
      return false;
    }
    auto field = definitions.find(fields->second[*index]);
    if (field == definitions.end() || field->second == nullptr ||
        field->second->kind != nir::DefinitionKind::Field ||
        field->second->signature.empty()) {
      return false;
    }

    const std::string resultName =
        freshLocalName("directFieldSelectResult", localNames);
    std::vector<nir::Value> evaluation;
    evaluation.reserve(object.operands.size() + 1);
    for (std::size_t i = 0; i < object.operands.size(); ++i) {
      if (i == *index) {
        evaluation.push_back(nir::localLetValue(resultName, field->second->signature,
                                                object.operands[i], value.span));
      } else {
        evaluation.push_back(object.operands[i]);
      }
    }
    nir::Value result = nir::localValue(resultName, value.span);
    result.type = field->second->signature;
    evaluation.push_back(std::move(result));
    value = nir::blockValue(std::move(evaluation), value.span);
    return true;
  }

  const std::optional<std::string> root =
      exactLocalObjectRootName(value.operands.front(), localValues);
  if (!root) {
    return false;
  }
  auto object = localValues.find(*root);
  if (object == localValues.end() || object->second.kind != nir::ValueKind::New) {
    return false;
  }

  const std::string receiverType =
      object->second.text.empty() ? object->second.type : object->second.text;
  const std::optional<std::size_t> index = fieldSnapshotIndex(
      receiverType, receiverType, value.text, definitions, parentMap, fieldSnapshots);
  if (!index || *index >= object->second.operands.size()) {
    return false;
  }

  const nir::Value* exactField =
      exactImmutableValue(object->second.operands[*index], localValues);
  if (exactField == nullptr || !isPureLetValue(*exactField)) {
    return false;
  }

  replaceValue(value, *exactField);
  return true;
}

bool updateExactObjectFieldLocalValue(
    const nir::Value& target, const nir::Value& assignedValue,
    std::unordered_map<std::string, nir::Value>& localValues,
    const DefinitionIndex& definitions, const ParentMap& parentMap,
    const FieldSnapshotMap& fieldSnapshots) {
  if (target.kind != nir::ValueKind::Select || target.operands.size() != 1 ||
      target.text.empty()) {
    return false;
  }

  const std::optional<std::string> root =
      exactLocalObjectRootName(target.operands.front(), localValues);
  if (!root) {
    return false;
  }
  auto object = localValues.find(*root);
  if (object == localValues.end() || object->second.kind != nir::ValueKind::New) {
    return false;
  }

  const std::string receiverType =
      object->second.text.empty() ? object->second.type : object->second.text;
  const std::optional<std::size_t> index = fieldSnapshotIndex(
      receiverType, receiverType, target.text, definitions, parentMap, fieldSnapshots);
  if (!index || *index >= object->second.operands.size()) {
    forgetObjectLocalFieldContents(*root, localValues);
    return true;
  }

  const nir::Value* exactAssignedValue =
      exactImmutableValue(assignedValue, localValues);
  if (exactAssignedValue == nullptr || !isPureLetValue(*exactAssignedValue)) {
    object->second.operands[*index] =
        nir::unknownValue("<unknown-field>", assignedValue.span);
    return true;
  }

  object->second.operands[*index] = *exactAssignedValue;
  return true;
}

void invalidateObjectFieldLocalValuesAfterPotentialMutation(
    const nir::Value& value, std::unordered_map<std::string, nir::Value>& localValues,
    const DefinitionIndex& definitions, const ParentMap& parentMap,
    const FieldSnapshotMap& fieldSnapshots) {
  for (const nir::Value& operand : value.operands) {
    invalidateObjectFieldLocalValuesAfterPotentialMutation(
        operand, localValues, definitions, parentMap, fieldSnapshots);
  }

  if (value.kind == nir::ValueKind::Assign && value.operands.size() == 2) {
    const std::unordered_set<std::string> assignedRoots =
        exactLocalObjectRootNamesInValue(value.operands.back(), localValues);
    if (updateExactObjectFieldLocalValue(value.operands.front(), value.operands.back(),
                                         localValues, definitions, parentMap,
                                         fieldSnapshots)) {
      for (const std::string& root : assignedRoots) {
        forgetObjectLocalFieldContents(root, localValues);
      }
      return;
    }
    const std::unordered_set<std::string> targetRoots =
        exactLocalObjectRootNamesInValue(value.operands.front(), localValues);
    for (const std::string& root : assignedRoots) {
      forgetObjectLocalFieldContents(root, localValues);
    }
    for (const std::string& root : targetRoots) {
      forgetObjectLocalFieldContents(root, localValues);
    }
    return;
  }

  if (value.kind == nir::ValueKind::Call) {
    (void)forgetObjectLocalFieldContentsForRootsInValue(value, localValues);
  }
}

std::optional<std::string>
devirtualizedTarget(const nir::Value& callee, std::size_t argumentCount,
                    const std::unordered_map<std::string, std::string>& localTypes,
                    const std::unordered_map<std::string, nir::Value>& localValues,
                    const DefinitionIndex& definitions, const ParentMap& parentMap) {
  if (callee.kind != nir::ValueKind::Select || callee.operands.size() != 1 ||
      callee.text.empty()) {
    return std::nullopt;
  }

  const std::optional<std::string> receiverType = monomorphicReceiverType(
      callee.operands.front(), localTypes, localValues, definitions, parentMap);
  if (!receiverType) {
    return std::nullopt;
  }

  const std::optional<std::string> target =
      resolveExactFunctionMember(*receiverType, callee.text, definitions, parentMap);
  if (!target) {
    return std::nullopt;
  }

  auto definition = definitions.find(*target);
  if (definition == definitions.end() || definition->second == nullptr ||
      signatureParameterTypes(definition->second->signature).size() != argumentCount) {
    return std::nullopt;
  }
  return target;
}

bool devirtualizeExactReceiverCall(
    nir::Value& value, const std::unordered_map<std::string, std::string>& localTypes,
    const std::unordered_map<std::string, nir::Value>& localValues,
    const DefinitionIndex& definitions, const ParentMap& parentMap) {
  if (value.kind == nir::ValueKind::Select) {
    std::optional<std::string> target =
        devirtualizedTarget(value, 1, localTypes, localValues, definitions, parentMap);
    if (!target) {
      return false;
    }

    std::vector<nir::Value> arguments;
    arguments.push_back(std::move(value.operands.front()));
    value = nir::callValue(nir::localValue(*target, value.span), std::move(arguments),
                           value.span);
    return true;
  }

  if (value.kind != nir::ValueKind::Call || value.operands.empty() ||
      value.operands.front().kind != nir::ValueKind::Select ||
      value.operands.front().operands.size() != 1) {
    return false;
  }

  std::optional<std::string> target =
      devirtualizedTarget(value.operands.front(), value.operands.size(), localTypes,
                          localValues, definitions, parentMap);
  if (!target) {
    return false;
  }

  std::vector<nir::Value> arguments;
  arguments.reserve(value.operands.size());
  arguments.push_back(std::move(value.operands.front().operands.front()));
  for (std::size_t i = 1; i < value.operands.size(); ++i) {
    arguments.push_back(std::move(value.operands[i]));
  }
  value = nir::callValue(nir::localValue(*target, value.span), std::move(arguments),
                         value.span);
  return true;
}

bool foldValue(nir::Value& value,
               const std::unordered_map<std::string, std::string>& localTypes,
               const std::unordered_map<std::string, nir::Value>& localValues,
               const std::unordered_set<std::string>& localNames,
               const std::string& currentOwner, const std::string& currentModule,
               const InlineReturnMap& inlineValues, const DefinitionIndex& definitions,
               const ParentMap& parentMap, const ConstructorFieldMap& constructorFields,
               const FieldSnapshotMap& fieldSnapshots) {
  if (value.kind == nir::ValueKind::Catch && value.operands.size() == 2 &&
      value.operands.front().kind == nir::ValueKind::Local) {
    std::unordered_map<std::string, std::string> scopedTypes = localTypes;
    std::unordered_map<std::string, nir::Value> scopedValues = localValues;
    std::unordered_set<std::string> scopedNames = localNames;
    const nir::Value& binding = value.operands.front();
    scopedTypes[binding.text] = binding.type;
    scopedValues.erase(binding.text);
    scopedNames.insert(binding.text);
    return foldValue(value.operands.back(), scopedTypes, scopedValues, scopedNames,
                     currentOwner, currentModule, inlineValues, definitions, parentMap,
                     constructorFields, fieldSnapshots);
  }

  if (value.kind == nir::ValueKind::Block) {
    bool changed = false;
    std::unordered_map<std::string, std::string> scopedTypes = localTypes;
    std::unordered_map<std::string, nir::Value> scopedValues = localValues;
    std::unordered_set<std::string> scopedNames = localNames;
    for (nir::Value& operand : value.operands) {
      if (operand.kind == nir::ValueKind::LocalLet ||
          operand.kind == nir::ValueKind::LocalVar) {
        if (!operand.operands.empty()) {
          changed =
              foldValue(operand.operands.front(), scopedTypes, scopedValues,
                        scopedNames, currentOwner, currentModule, inlineValues,
                        definitions, parentMap, constructorFields, fieldSnapshots) ||
              changed;
          invalidateArrayLocalValuesAfterPotentialMutation(operand.operands.front(),
                                                           scopedTypes, scopedValues);
          invalidateObjectFieldLocalValuesAfterPotentialMutation(
              operand.operands.front(), scopedValues, definitions, parentMap,
              fieldSnapshots);
          if (operand.kind == nir::ValueKind::LocalVar) {
            (void)forgetObjectLocalFieldContentsForRootsInValue(
                operand.operands.front(), scopedValues);
          }
          const std::string localType =
              operand.type.empty()
                  ? knownValueType(operand.operands.front(), scopedTypes)
                  : operand.type;
          if (!operand.text.empty() && localType != "Unknown") {
            scopedTypes[operand.text] = localType;
          } else {
            scopedTypes.erase(operand.text);
          }
        }
        if (operand.kind == nir::ValueKind::LocalLet && !operand.text.empty() &&
            operand.operands.size() == 1) {
          scopedValues[operand.text] =
              extendExactNewFieldSnapshot(operand.operands.front(), definitions,
                                          parentMap, constructorFields, fieldSnapshots);
        } else {
          scopedValues.erase(operand.text);
        }
        if (!operand.text.empty()) {
          scopedNames.insert(operand.text);
        }
        continue;
      }
      changed = foldValue(operand, scopedTypes, scopedValues, scopedNames, currentOwner,
                          currentModule, inlineValues, definitions, parentMap,
                          constructorFields, fieldSnapshots) ||
                changed;
      invalidateArrayLocalValuesAfterPotentialMutation(operand, scopedTypes,
                                                       scopedValues);
      invalidateObjectFieldLocalValuesAfterPotentialMutation(
          operand, scopedValues, definitions, parentMap, fieldSnapshots);
    }
    return changed;
  }

  if (value.kind == nir::ValueKind::Local) {
    auto local = localValues.find(value.text);
    if (local != localValues.end() && isLiteralLikePropagatableValue(local->second)) {
      auto localType = localTypes.find(value.text);
      const bool preservesReferenceOwner =
          localType != localTypes.end() &&
          isTypedNullReferenceValue(local->second, localType->second);
      if (preservesReferenceOwner) {
        return false;
      }
      replaceValue(value, local->second);
      return true;
    }
    if (!localNames.contains(value.text)) {
      if (std::optional<nir::Value> inlined = inlineZeroArgLocalValue(
              value, currentOwner, currentModule, inlineValues)) {
        replaceValue(value, *inlined);
        (void)foldValue(value, localTypes, localValues, localNames, currentOwner,
                        currentModule, inlineValues, definitions, parentMap,
                        constructorFields, fieldSnapshots);
        return true;
      }
    }
    return false;
  }

  bool changed = false;
  for (std::size_t i = 0; i < value.operands.size(); ++i) {
    if (value.kind == nir::ValueKind::Assign && i == 0) {
      continue;
    }
    if (value.kind == nir::ValueKind::Call && i == 0 &&
        (value.operands[i].kind == nir::ValueKind::Local ||
         value.operands[i].kind == nir::ValueKind::Select)) {
      continue;
    }
    changed = foldValue(value.operands[i], localTypes, localValues, localNames,
                        currentOwner, currentModule, inlineValues, definitions,
                        parentMap, constructorFields, fieldSnapshots) ||
              changed;
  }

  if (foldExactConstructorFieldSelect(value, localValues, localNames, definitions,
                                      parentMap, constructorFields, fieldSnapshots)) {
    return true;
  }

  if (devirtualizeExactReceiverCall(value, localTypes, localValues, definitions,
                                    parentMap)) {
    (void)foldValue(value, localTypes, localValues, localNames, currentOwner,
                    currentModule, inlineValues, definitions, parentMap,
                    constructorFields, fieldSnapshots);
    return true;
  }

  if (std::optional<nir::Value> inlined =
          inlineCallValue(value, currentOwner, currentModule, inlineValues, definitions,
                          parentMap, constructorFields, fieldSnapshots, localValues)) {
    replaceValue(value, *inlined);
    (void)foldValue(value, localTypes, localValues, localNames, currentOwner,
                    currentModule, inlineValues, definitions, parentMap,
                    constructorFields, fieldSnapshots);
    return true;
  }

  if (value.kind == nir::ValueKind::Call && value.operands.size() == 4 &&
      value.operands.front().kind == nir::ValueKind::Local) {
    if (std::optional<nir::Value> folded = foldDirectRuntimeArrayUpdate(
            value.operands.front().text, value.operands[1], value.operands[2],
            value.operands[3], localValues, value.span)) {
      replaceValue(value, *folded);
      return true;
    }
  }

  if (value.kind == nir::ValueKind::Call && value.operands.size() == 3 &&
      value.operands.front().kind == nir::ValueKind::Local &&
      (value.operands.front().text == support::StdNames::RuntimeAnyEquals ||
       value.operands.front().text == support::StdNames::RuntimeAnyReceiverEquals) &&
      (value.operands.front().text != support::StdNames::RuntimeAnyReceiverEquals ||
       isProvenNonNullReferenceValue(value.operands[1], localValues))) {
    if (std::optional<nir::Value> folded = foldDirectEvaluatedNonNullNullComparison(
            "==", value.operands[1], value.operands[2], localValues, value.span)) {
      replaceValue(value, *folded);
      return true;
    }
    if (std::optional<nir::Value> folded = foldDirectEvaluatedBoxAnyEquals(
            value.operands[1], value.operands[2], value.span)) {
      replaceValue(value, *folded);
      return true;
    }
    if (std::optional<bool> folded = foldExactLocalEvaluatedBoxAnyEquals(
            value.operands[1], value.operands[2], localValues)) {
      value = booleanLiteral(*folded, value.span);
      return true;
    }
    if (std::optional<nir::Value> folded = foldDirectDistinctFreshReferenceComparison(
            "==", value.operands[1], value.operands[2], value.span)) {
      replaceValue(value, *folded);
      return true;
    }
    if (std::optional<bool> folded = foldExactBoxedAnyEquals(
            value.operands[1], value.operands[2], localValues)) {
      value = booleanLiteral(*folded, value.span);
      return true;
    }
  }

  if (value.kind == nir::ValueKind::Call && value.operands.size() == 3 &&
      value.operands.front().kind == nir::ValueKind::Local) {
    if (std::optional<bool> folded =
            foldExactRuntimeStringEquals(value.operands.front().text, value.operands[1],
                                         value.operands[2], localValues)) {
      value = booleanLiteral(*folded, value.span);
      return true;
    }
    if (std::optional<nir::Value> folded = foldDirectRuntimeArrayApply(
            value.operands.front().text, value.operands[1], value.operands[2],
            localValues, localNames, value.span)) {
      replaceValue(value, *folded);
      return true;
    }
    if (std::optional<nir::Value> folded =
            foldExactRuntimeArrayApply(value.operands.front().text, value.operands[1],
                                       value.operands[2], localValues)) {
      replaceValue(value, *folded);
      return true;
    }
    if (std::optional<std::string> folded =
            foldExactRuntimeFormat(value.operands.front().text, value.operands[1],
                                   value.operands[2], localValues)) {
      value = stringLiteral(*folded, value.span);
      return true;
    }
  }

  if (value.kind == nir::ValueKind::Call && value.operands.size() == 2 &&
      value.operands.front().kind == nir::ValueKind::Local) {
    if (std::optional<nir::Value> folded = foldDirectRuntimeArrayLength(
            value.operands.front().text, value.operands[1], value.span)) {
      replaceValue(value, *folded);
      return true;
    }
    if (std::optional<std::string> folded = foldExactRuntimeToString(
            value.operands.front().text, value.operands[1], localValues)) {
      value = stringLiteral(*folded, value.span);
      return true;
    }
    if (std::optional<std::int64_t> folded = foldExactRuntimeStringLength(
            value.operands.front().text, value.operands[1], localValues)) {
      value = intLiteral(*folded, value.span);
      return true;
    }
    if (std::optional<std::int64_t> folded = foldExactRuntimeArrayLength(
            value.operands.front().text, value.operands[1], localValues)) {
      value = intLiteral(*folded, value.span);
      return true;
    }
    if (std::optional<nir::Value> folded = foldDirectEvaluatedUnitBoxRuntimeCall(
            value.operands.front().text, value.operands[1], value.span)) {
      replaceValue(value, *folded);
      return true;
    }
    if (std::optional<std::int64_t> folded = foldExactRuntimeHashCode(
            value.operands.front().text, value.operands[1], localValues)) {
      value = intLiteral(*folded, value.span);
      return true;
    }
  }

  if (value.kind == nir::ValueKind::Binary && value.operands.size() == 2) {
    nir::Value& lhsValue = value.operands[0];
    nir::Value& rhsValue = value.operands[1];
    if (value.text == "+") {
      if (std::optional<std::string> folded =
              foldExactStringConcat(lhsValue, rhsValue, localTypes, localValues)) {
        value = stringLiteral(*folded, value.span);
        return true;
      }
      if (std::optional<nir::Value> folded = foldDirectEvaluatedUnitBoxStringConcat(
              lhsValue, rhsValue, localTypes, localValues, value.span)) {
        replaceValue(value, *folded);
        return true;
      }
    }
    if (std::optional<nir::Value> folded = foldDirectEvaluatedNonNullNullComparison(
            value.text, lhsValue, rhsValue, localValues, value.span)) {
      replaceValue(value, *folded);
      return true;
    }
    if (std::optional<bool> folded =
            foldExactNullComparison(value.text, lhsValue, rhsValue, localValues)) {
      value = booleanLiteral(*folded, value.span);
      return true;
    }
    if (std::optional<bool> folded = foldExactSameReferenceComparison(
            value.text, lhsValue, rhsValue, localValues)) {
      value = booleanLiteral(*folded, value.span);
      return true;
    }
    if (std::optional<nir::Value> folded = foldDirectDistinctFreshReferenceComparison(
            value.text, lhsValue, rhsValue, value.span)) {
      replaceValue(value, *folded);
      return true;
    }
    if (std::optional<bool> folded = foldExactDistinctFreshReferenceComparison(
            value.text, lhsValue, rhsValue, localValues)) {
      value = booleanLiteral(*folded, value.span);
      return true;
    }
    if (std::optional<bool> folded = foldExactSameRuntimeHashComparison(
            value.text, lhsValue, rhsValue, localValues)) {
      value = booleanLiteral(*folded, value.span);
      return true;
    }

    std::optional<long long> lhs = integerLiteralValue(value.operands[0]);
    std::optional<long long> rhs = integerLiteralValue(value.operands[1]);
    if (lhs && rhs) {
      if (std::optional<long long> folded = foldIntegralBitwiseOrShift(
              value.text, *lhs, *rhs, lhsValue.type, rhsValue.type)) {
        value = nir::Value{nir::ValueKind::Literal,
                           lhsValue.type,
                           integerLiteralText(*folded, lhsValue.type),
                           {},
                           value.span};
        return true;
      }
      if (std::optional<long long> folded = foldIntegralDivisionOrRemainder(
              value.text, *lhs, *rhs, lhsValue.type, rhsValue.type)) {
        value = nir::Value{nir::ValueKind::Literal,
                           lhsValue.type,
                           integerLiteralText(*folded, lhsValue.type),
                           {},
                           value.span};
        return true;
      }
      std::optional<long long> folded;
      if (value.text == "+") {
        folded = *lhs + *rhs;
      } else if (value.text == "-") {
        folded = *lhs - *rhs;
      } else if (value.text == "*") {
        folded = *lhs * *rhs;
      }
      if (folded) {
        value = nir::Value{nir::ValueKind::Literal,
                           value.operands[0].type,
                           integerLiteralText(*folded, value.operands[0].type),
                           {},
                           value.span};
        return true;
      }
      std::optional<bool> foldedComparison;
      if (value.text == "==") {
        foldedComparison = *lhs == *rhs;
      } else if (value.text == "!=") {
        foldedComparison = *lhs != *rhs;
      } else if (value.text == "<") {
        foldedComparison = *lhs < *rhs;
      } else if (value.text == ">") {
        foldedComparison = *lhs > *rhs;
      } else if (value.text == "<=") {
        foldedComparison = *lhs <= *rhs;
      } else if (value.text == ">=") {
        foldedComparison = *lhs >= *rhs;
      }
      if (foldedComparison) {
        value = booleanLiteral(*foldedComparison, value.span);
        return true;
      }
    }

    std::optional<double> lhsFloating = floatingLiteralValue(value.operands[0]);
    std::optional<double> rhsFloating = floatingLiteralValue(value.operands[1]);
    if (lhsFloating && rhsFloating && lhsValue.type == rhsValue.type) {
      std::optional<bool> foldedComparison;
      if (value.text == "==") {
        foldedComparison = *lhsFloating == *rhsFloating;
      } else if (value.text == "!=") {
        foldedComparison = *lhsFloating != *rhsFloating;
      } else if (value.text == "<") {
        foldedComparison = *lhsFloating < *rhsFloating;
      } else if (value.text == ">") {
        foldedComparison = *lhsFloating > *rhsFloating;
      } else if (value.text == "<=") {
        foldedComparison = *lhsFloating <= *rhsFloating;
      } else if (value.text == ">=") {
        foldedComparison = *lhsFloating >= *rhsFloating;
      }
      if (foldedComparison) {
        value = booleanLiteral(*foldedComparison, value.span);
        return true;
      }
    }

    std::optional<int> lhsChar = charLiteralValue(value.operands[0]);
    std::optional<int> rhsChar = charLiteralValue(value.operands[1]);
    if (lhsChar && rhsChar) {
      std::optional<bool> foldedComparison;
      if (value.text == "==") {
        foldedComparison = *lhsChar == *rhsChar;
      } else if (value.text == "!=") {
        foldedComparison = *lhsChar != *rhsChar;
      } else if (value.text == "<") {
        foldedComparison = *lhsChar < *rhsChar;
      } else if (value.text == ">") {
        foldedComparison = *lhsChar > *rhsChar;
      } else if (value.text == "<=") {
        foldedComparison = *lhsChar <= *rhsChar;
      } else if (value.text == ">=") {
        foldedComparison = *lhsChar >= *rhsChar;
      }
      if (foldedComparison) {
        value = booleanLiteral(*foldedComparison, value.span);
        return true;
      }
    }

    std::optional<std::string> lhsString = stringLiteralValue(value.operands[0]);
    std::optional<std::string> rhsString = stringLiteralValue(value.operands[1]);
    if (lhsString && rhsString) {
      if (value.text == "==" || value.text == "!=") {
        const bool equal = *lhsString == *rhsString;
        value = booleanLiteral(value.text == "==" ? equal : !equal, value.span);
        return true;
      }
    }

    if (lhsString && rhsString && value.text == "+") {
      std::string concatenated = *lhsString;
      concatenated += *rhsString;
      value = nir::Value{nir::ValueKind::Literal,
                         "String",
                         escapedStringLiteralText(concatenated),
                         {},
                         value.span};
      return true;
    }

    std::optional<std::string> lhsSymbol = symbolLiteralText(value.operands[0]);
    std::optional<std::string> rhsSymbol = symbolLiteralText(value.operands[1]);
    if (lhsSymbol && rhsSymbol) {
      if (value.text == "==" || value.text == "!=") {
        const bool equal = *lhsSymbol == *rhsSymbol;
        value = booleanLiteral(value.text == "==" ? equal : !equal, value.span);
        return true;
      }
    }

    if (lhsValue.kind == nir::ValueKind::Unit &&
        rhsValue.kind == nir::ValueKind::Unit) {
      if (value.text == "==" || value.text == "!=") {
        value = booleanLiteral(value.text == "==", value.span);
        return true;
      }
    }

    std::optional<bool> lhsBoolean = booleanLiteralValue(value.operands[0]);
    std::optional<bool> rhsBoolean = booleanLiteralValue(value.operands[1]);
    if (lhsBoolean && rhsBoolean) {
      std::optional<bool> folded;
      if (value.text == "==") {
        folded = *lhsBoolean == *rhsBoolean;
      } else if (value.text == "!=") {
        folded = *lhsBoolean != *rhsBoolean;
      } else if (value.text == "&&") {
        folded = *lhsBoolean && *rhsBoolean;
      } else if (value.text == "||") {
        folded = *lhsBoolean || *rhsBoolean;
      } else if (value.text == "&") {
        folded = *lhsBoolean & *rhsBoolean;
      } else if (value.text == "|") {
        folded = *lhsBoolean | *rhsBoolean;
      } else if (value.text == "^") {
        folded = *lhsBoolean ^ *rhsBoolean;
      }
      if (folded) {
        value = booleanLiteral(*folded, value.span);
        return true;
      }
    }

    if (lhsValue.kind == nir::ValueKind::SizeOf &&
        rhsValue.kind == nir::ValueKind::SizeOf && lhsValue.text == rhsValue.text) {
      if (std::optional<bool> folded = sameLocalComparisonResult(value.text)) {
        value = booleanLiteral(*folded, value.span);
        return true;
      }
      if (value.text == "-") {
        value = nir::Value{nir::ValueKind::Literal, "Int", "0", {}, value.span};
        return true;
      }
    }

    const bool sameLocalOperands = lhsValue.kind == nir::ValueKind::Local &&
                                   rhsValue.kind == nir::ValueKind::Local &&
                                   lhsValue.text == rhsValue.text;
    const bool samePureOperands =
        valuesEqual(lhsValue, rhsValue) && isPureLetValue(lhsValue);
    const std::string lhsKnownType = knownValueType(lhsValue, localTypes);
    if (samePureOperands && isSameLocalComparisonType(lhsKnownType)) {
      if (std::optional<bool> folded = sameLocalComparisonResult(value.text)) {
        value = booleanLiteral(*folded, value.span);
        return true;
      }
    }
    if (sameLocalOperands && isSameLocalEqualityType(lhsKnownType)) {
      if (value.text == "==" || value.text == "!=") {
        value = booleanLiteral(value.text == "==", value.span);
        return true;
      }
    }
    if (samePureOperands && value.text == "-" && isFoldableIntegerType(lhsKnownType)) {
      value = nir::Value{nir::ValueKind::Literal,
                         lhsKnownType,
                         integerLiteralText(0, lhsKnownType),
                         {},
                         value.span};
      return true;
    }
    if (samePureOperands && (value.text == "&&" || value.text == "||") &&
        lhsKnownType == "Boolean") {
      replaceValue(value, lhsValue);
      return true;
    }

    if (value.text == "+" || value.text == "-" || value.text == "*" ||
        value.text == "/" || value.text == "%") {
      const std::string rhsKnownType = knownValueType(rhsValue, localTypes);
      if ((value.text == "+" || value.text == "-") && isIntegerLiteral(rhsValue, 0) &&
          isFoldableIntegerType(lhsKnownType)) {
        replaceValue(value, lhsValue);
        return true;
      }
      if (value.text == "+" && isIntegerLiteral(lhsValue, 0) &&
          isFoldableIntegerType(rhsKnownType)) {
        replaceValue(value, rhsValue);
        return true;
      }
      if (value.text == "-" && isFoldableIntegerType(rhsKnownType) &&
          isIntegerLiteralOfType(lhsValue, 0, rhsKnownType)) {
        value = nir::unaryValue("-", rhsValue, value.span);
        return true;
      }
      if ((value.text == "*" || value.text == "/") && isIntegerLiteral(rhsValue, 1) &&
          isFoldableIntegerType(lhsKnownType)) {
        replaceValue(value, lhsValue);
        return true;
      }
      if (value.text == "*" && isIntegerLiteral(lhsValue, 1) &&
          isFoldableIntegerType(rhsKnownType)) {
        replaceValue(value, rhsValue);
        return true;
      }
      if (value.text == "*" && isFoldableIntegerType(lhsKnownType) &&
          isIntegerLiteralOfType(rhsValue, -1, lhsKnownType)) {
        value = nir::unaryValue("-", lhsValue, value.span);
        return true;
      }
      if (value.text == "*" && isFoldableIntegerType(rhsKnownType) &&
          isIntegerLiteralOfType(lhsValue, -1, rhsKnownType)) {
        value = nir::unaryValue("-", rhsValue, value.span);
        return true;
      }
      if ((value.text == "*" || value.text == "/") &&
          isFloatingOneLiteral(rhsValue, lhsKnownType)) {
        replaceValue(value, lhsValue);
        return true;
      }
      if (value.text == "*" && isFloatingOneLiteral(lhsValue, rhsKnownType)) {
        replaceValue(value, rhsValue);
        return true;
      }
      if (value.text == "*" && isIntegerLiteral(rhsValue, 0) &&
          isFoldableIntegerType(lhsKnownType) && isPureLetValue(lhsValue)) {
        value = nir::Value{nir::ValueKind::Literal,
                           lhsKnownType,
                           integerLiteralText(0, lhsKnownType),
                           {},
                           value.span};
        return true;
      }
      if (value.text == "*" && isIntegerLiteral(lhsValue, 0) &&
          isFoldableIntegerType(rhsKnownType) && isPureLetValue(rhsValue)) {
        value = nir::Value{nir::ValueKind::Literal,
                           rhsKnownType,
                           integerLiteralText(0, rhsKnownType),
                           {},
                           value.span};
        return true;
      }
      if (value.text == "%" && isIntegerLiteral(rhsValue, 1) &&
          isFoldableIntegerType(lhsKnownType) && isPureLetValue(lhsValue)) {
        value = nir::Value{nir::ValueKind::Literal,
                           lhsKnownType,
                           integerLiteralText(0, lhsKnownType),
                           {},
                           value.span};
        return true;
      }
    }

    if (canonicalizeOrderedComparisonDirection(value, localTypes)) {
      return true;
    }

    const bool lhsKnownBoolean = isKnownValueType(lhsValue, "Boolean", localTypes);
    const bool rhsKnownBoolean = isKnownValueType(rhsValue, "Boolean", localTypes);
    const bool rhsNegatesLhs =
        isNegatedSamePureBooleanValue(rhsValue, lhsValue, localTypes);
    const bool lhsNegatesRhs =
        isNegatedSamePureBooleanValue(lhsValue, rhsValue, localTypes);
    if (value.text == "==" || value.text == "!=") {
      const bool equality = value.text == "==";
      if (rhsNegatesLhs || lhsNegatesRhs) {
        value = booleanLiteral(!equality, value.span);
        return true;
      }
      if (rhsBoolean && lhsKnownBoolean) {
        replaceWithBooleanPolarity(value, lhsValue, *rhsBoolean == equality, value.span,
                                   localTypes);
        return true;
      }
      if (lhsBoolean && rhsKnownBoolean) {
        replaceWithBooleanPolarity(value, rhsValue, *lhsBoolean == equality, value.span,
                                   localTypes);
        return true;
      }
    }

    if (value.text == "&&") {
      if (rhsNegatesLhs || lhsNegatesRhs) {
        value = booleanLiteral(false, value.span);
        return true;
      }
      if (rhsBoolean && *rhsBoolean && lhsKnownBoolean) {
        replaceValue(value, lhsValue);
        return true;
      }
      if (lhsBoolean && *lhsBoolean && rhsKnownBoolean) {
        replaceValue(value, rhsValue);
        return true;
      }
      if (rhsBoolean && !*rhsBoolean && lhsKnownBoolean && isPureLetValue(lhsValue)) {
        value = booleanLiteral(false, value.span);
        return true;
      }
      if (lhsBoolean && !*lhsBoolean && rhsKnownBoolean && isPureLetValue(rhsValue)) {
        value = booleanLiteral(false, value.span);
        return true;
      }
    } else if (value.text == "||") {
      if (rhsNegatesLhs || lhsNegatesRhs) {
        value = booleanLiteral(true, value.span);
        return true;
      }
      if (rhsBoolean && !*rhsBoolean && lhsKnownBoolean) {
        replaceValue(value, lhsValue);
        return true;
      }
      if (lhsBoolean && !*lhsBoolean && rhsKnownBoolean) {
        replaceValue(value, rhsValue);
        return true;
      }
      if (rhsBoolean && *rhsBoolean && lhsKnownBoolean && isPureLetValue(lhsValue)) {
        value = booleanLiteral(true, value.span);
        return true;
      }
      if (lhsBoolean && *lhsBoolean && rhsKnownBoolean && isPureLetValue(rhsValue)) {
        value = booleanLiteral(true, value.span);
        return true;
      }
    }

    if (value.text == "&&" || value.text == "||") {
      const std::string nestedOp = value.text == "&&" ? "||" : "&&";
      if (isBooleanAbsorptionOperand(lhsValue, nestedOp, rhsValue, localTypes)) {
        replaceValue(value, rhsValue);
        return true;
      }
      if (isBooleanAbsorptionOperand(rhsValue, nestedOp, lhsValue, localTypes)) {
        replaceValue(value, lhsValue);
        return true;
      }
      if (const nir::Value* remainder = booleanComplementAbsorptionRemainder(
              rhsValue, nestedOp, lhsValue, localTypes);
          remainder != nullptr) {
        value = nir::binaryValue(value.text, lhsValue, *remainder, value.span);
        return true;
      }
      if (const nir::Value* remainder = booleanComplementAbsorptionRemainder(
              lhsValue, nestedOp, rhsValue, localTypes);
          remainder != nullptr) {
        value = nir::binaryValue(value.text, rhsValue, *remainder, value.span);
        return true;
      }
    }
  }

  if (value.kind == nir::ValueKind::Unary && value.operands.size() == 1) {
    const nir::Value& operand = value.operands.front();
    if ((value.text == "+" || value.text == "-") &&
        operand.kind == nir::ValueKind::Literal &&
        isFoldableIntegerType(operand.type)) {
      std::optional<long long> integer = integerLiteralValue(operand);
      if (integer) {
        const long long result = value.text == "-" ? -*integer : *integer;
        value = nir::Value{nir::ValueKind::Literal,
                           operand.type,
                           integerLiteralText(result, operand.type),
                           {},
                           value.span};
        return true;
      }
    }
    if ((value.text == "+" || value.text == "-") &&
        operand.kind == nir::ValueKind::Literal &&
        isFoldableFloatingType(operand.type) && floatingLiteralValue(operand)) {
      value = nir::Value{nir::ValueKind::Literal,
                         operand.type,
                         floatingUnaryLiteralText(value.text, operand),
                         {},
                         value.span};
      return true;
    }
    if (value.text == "+" &&
        isUnaryPlusIdentityType(knownValueType(operand, localTypes))) {
      replaceValue(value, operand);
      return true;
    }
    if (value.text == "-" && operand.kind == nir::ValueKind::Unary &&
        operand.text == "-" && operand.operands.size() == 1 &&
        isFoldableIntegerType(knownValueType(operand.operands.front(), localTypes))) {
      replaceValue(value, operand.operands.front());
      return true;
    }
    if (value.text == "!" && operand.kind == nir::ValueKind::Literal &&
        operand.type == "Boolean") {
      const std::optional<bool> boolean = booleanLiteralValue(operand);
      if (boolean) {
        value = booleanLiteral(!*boolean, value.span);
        return true;
      }
    }
    if (value.text == "!" && operand.kind == nir::ValueKind::Binary &&
        operand.operands.size() == 2 &&
        (operand.text == "&&" || operand.text == "||") &&
        isPureLetValue(operand.operands[0]) && isPureLetValue(operand.operands[1]) &&
        isKnownValueType(operand.operands[0], "Boolean", localTypes) &&
        isKnownValueType(operand.operands[1], "Boolean", localTypes)) {
      const std::string inverted = operand.text == "&&" ? "||" : "&&";
      value = nir::binaryValue(
          inverted, nir::unaryValue("!", operand.operands[0], value.span),
          nir::unaryValue("!", operand.operands[1], value.span), value.span);
      return true;
    }
    if (value.text == "!" && operand.kind == nir::ValueKind::Binary &&
        operand.operands.size() == 2 &&
        (operand.text == "==" || operand.text == "!=")) {
      const std::string inverted = operand.text == "==" ? "!=" : "==";
      value = nir::binaryValue(inverted, operand.operands[0], operand.operands[1],
                               value.span);
      return true;
    }
    if (value.text == "!" && operand.kind == nir::ValueKind::Binary &&
        operand.operands.size() == 2) {
      const std::string lhsType = knownValueType(operand.operands[0], localTypes);
      const std::string rhsType = knownValueType(operand.operands[1], localTypes);
      const std::optional<std::string> inverted =
          invertedOrderedComparison(operand.text);
      if (inverted && lhsType == rhsType &&
          isOrderedNonFloatingComparisonType(lhsType)) {
        value = nir::binaryValue(*inverted, operand.operands[0], operand.operands[1],
                                 value.span);
        (void)canonicalizeOrderedComparisonDirection(value, localTypes);
        return true;
      }
    }
    if (value.text == "!" && operand.kind == nir::ValueKind::Unary &&
        operand.text == "!" && operand.operands.size() == 1 &&
        isKnownValueType(operand.operands.front(), "Boolean", localTypes)) {
      replaceValue(value, operand.operands.front());
      return true;
    }
  }

  if (value.kind == nir::ValueKind::If && value.operands.size() == 3) {
    std::optional<bool> condition = booleanLiteralValue(value.operands[0]);
    if (condition) {
      replaceValue(value, *condition ? value.operands[1] : value.operands[2]);
      return true;
    }
    const std::optional<bool> thenBoolean = booleanLiteralValue(value.operands[1]);
    const std::optional<bool> elseBoolean = booleanLiteralValue(value.operands[2]);
    if (thenBoolean && elseBoolean) {
      if (*thenBoolean && !*elseBoolean) {
        replaceWithBooleanPolarity(value, value.operands[0], true, value.span,
                                   localTypes);
        return true;
      }
      if (!*thenBoolean && *elseBoolean) {
        replaceWithBooleanPolarity(value, value.operands[0], false, value.span,
                                   localTypes);
        return true;
      }
    }
    if (isPureLetValue(value.operands[0]) &&
        isKnownValueType(value.operands[0], "Boolean", localTypes)) {
      const std::optional<bool> whenTrue = booleanValueWhenConditionIs(
          value.operands[1], value.operands[0], true, localTypes);
      const std::optional<bool> whenFalse = booleanValueWhenConditionIs(
          value.operands[2], value.operands[0], false, localTypes);
      if (whenTrue && whenFalse) {
        if (*whenTrue == *whenFalse) {
          value = booleanLiteral(*whenTrue, value.span);
          return true;
        }
        replaceWithBooleanPolarity(value, value.operands[0], *whenTrue, value.span,
                                   localTypes);
        return true;
      }
    }
    if (valuesEqual(value.operands[1], value.operands[2])) {
      if (isPureLetValue(value.operands[0])) {
        replaceValue(value, value.operands[1]);
      } else {
        value = nir::blockValue({value.operands[0], value.operands[1]}, value.span);
      }
      return true;
    }
    if (canonicalizeBooleanIfAsLogical(value, localTypes)) {
      return true;
    }
  }

  if (value.kind == nir::ValueKind::While && value.operands.size() == 2) {
    const std::optional<bool> condition = booleanLiteralValue(value.operands[0]);
    if (condition && !*condition) {
      value = nir::unitValue(value.span);
      return true;
    }
  }

  if (value.kind == nir::ValueKind::IsInstanceOf && value.operands.size() == 1) {
    if (isNullLiteral(value.operands.front())) {
      value = booleanLiteral(false, value.span);
      return true;
    }
    if (isTopObjectTarget(value.text) &&
        isExactPureNonNullReferenceValue(value.operands.front(), localValues)) {
      value = booleanLiteral(true, value.span);
      return true;
    }
    if (isExactNonNullInstanceOfOperand(value.operands.front(), value.text)) {
      value = booleanLiteral(true, value.span);
      return true;
    }
    if (isExactNonNullInstanceOfLocal(value.operands.front(), value.text,
                                      localValues)) {
      value = booleanLiteral(true, value.span);
      return true;
    }
    if (std::optional<nir::Value> folded = foldExactEvaluatedNewInstanceOfOperand(
            value.operands.front(), value.text, definitions, parentMap, value.span)) {
      replaceValue(value, *folded);
      return true;
    }
    if (std::optional<bool> folded = foldExactEvaluatedNewInstanceOfLocal(
            value.operands.front(), value.text, localValues, definitions, parentMap)) {
      value = booleanLiteral(*folded, value.span);
      return true;
    }
    if (isDisjointExactAbiInstanceOfOperand(value.operands.front(), value.text) ||
        isDisjointExactAbiInstanceOfLocal(value.operands.front(), value.text,
                                          localValues)) {
      value = booleanLiteral(false, value.span);
      return true;
    }
    if (std::optional<nir::Value> folded = foldExactEvaluatedBoxInstanceOfOperand(
            value.operands.front(), value.text, value.span)) {
      replaceValue(value, *folded);
      return true;
    }
    if (std::optional<bool> folded = foldExactEvaluatedBoxInstanceOfLocal(
            value.operands.front(), value.text, localValues)) {
      value = booleanLiteral(*folded, value.span);
      return true;
    }
  }

  if (value.kind == nir::ValueKind::Unbox && value.operands.size() == 1) {
    const nir::Value& operand = value.operands.front();
    if (operand.kind == nir::ValueKind::Box && operand.text == value.text &&
        operand.operands.size() == 1) {
      replaceValue(value, operand.operands.front());
      return true;
    }
    if (const nir::Value* payload =
            exactPureLocalBoxedAbiPayload(operand, value.text, localValues);
        payload != nullptr) {
      replaceValue(value, *payload);
      return true;
    }
  }

  if (value.kind == nir::ValueKind::AsInstanceOf && value.operands.size() == 1) {
    const nir::Value& operand = value.operands.front();
    if (isNullLiteral(operand)) {
      value = nir::literalValue("null", value.text, value.span);
      return true;
    }
    if (isTopObjectTarget(value.text) &&
        isExactPureNonNullReferenceValue(operand, localValues)) {
      replaceValue(value, operand);
      return true;
    }
    if (operand.kind == nir::ValueKind::AsInstanceOf && operand.text == value.text &&
        operand.operands.size() == 1) {
      replaceValue(value, operand);
      return true;
    }
    const std::string operandType = knownValueType(operand, localTypes);
    if (operandType == value.text ||
        knownReferenceTypeConformsTo(operandType, value.text, definitions, parentMap)) {
      replaceValue(value, operand);
      return true;
    }
  }

  return changed;
}

PassEffect foldConstants(linker::LinkedProgram& program) {
  PassEffect effect;
  bool changed = true;
  while (changed) {
    changed = false;
    const InlineReturnMap inlineValues = collectInlineableReturnSummaries(program);
    const DefinitionIndex definitions = definitionIndexFor(program);
    const ParentMap parentMap = parentMapFor(definitions);
    const ConstructorFieldMap constructorFields = constructorFieldMapFor(program);
    const FieldSnapshotMap fieldSnapshots =
        fieldSnapshotMapFor(definitions, parentMap, constructorFields);
    for (nir::Module& module : program.modules) {
      for (nir::Definition& definition : module.definitions) {
        if (definition.kind != nir::DefinitionKind::FunctionDef) {
          continue;
        }
        std::unordered_map<std::string, std::string> localTypes;
        std::unordered_map<std::string, nir::Value> localValues;
        std::unordered_set<std::string> localNames;
        const std::string currentOwner = ownerNameOf(definition.name);
        for (nir::Instruction& instruction : definition.body.instructions) {
          if (instruction.kind == nir::InstructionKind::Param) {
            if (!instruction.name.empty()) {
              localNames.insert(instruction.name);
            }
            if (!instruction.name.empty() && !instruction.type.empty()) {
              localTypes[instruction.name] = instruction.type;
            } else {
              localTypes.erase(instruction.name);
            }
            localValues.erase(instruction.name);
            continue;
          }
          if (instruction.kind == nir::InstructionKind::Unreachable) {
            continue;
          }
          if (foldValue(instruction.value, localTypes, localValues, localNames,
                        currentOwner, module.name, inlineValues, definitions, parentMap,
                        constructorFields, fieldSnapshots)) {
            ++effect.changedValues;
            changed = true;
          }
          invalidateArrayLocalValuesAfterPotentialMutation(instruction.value,
                                                           localTypes, localValues);
          invalidateObjectFieldLocalValuesAfterPotentialMutation(
              instruction.value, localValues, definitions, parentMap, fieldSnapshots);
          if (instruction.kind == nir::InstructionKind::Var) {
            (void)forgetObjectLocalFieldContentsForRootsInValue(instruction.value,
                                                                localValues);
          }
          if (instruction.kind == nir::InstructionKind::Let ||
              instruction.kind == nir::InstructionKind::Var) {
            const std::string localType =
                instruction.type.empty() ? knownValueType(instruction.value, localTypes)
                                         : instruction.type;
            if (!instruction.name.empty() && localType != "Unknown") {
              localTypes[instruction.name] = localType;
            } else {
              localTypes.erase(instruction.name);
            }
            if (instruction.kind == nir::InstructionKind::Let &&
                !instruction.name.empty()) {
              localValues[instruction.name] =
                  extendExactNewFieldSnapshot(instruction.value, definitions, parentMap,
                                              constructorFields, fieldSnapshots);
            } else {
              localValues.erase(instruction.name);
            }
            if (!instruction.name.empty()) {
              localNames.insert(instruction.name);
            }
          }
        }
      }
    }
  }
  return effect;
}

bool isLiteralLikePropagatableValue(const nir::Value& value) {
  if (value.kind == nir::ValueKind::Literal || value.kind == nir::ValueKind::Unit) {
    return true;
  }
  if (value.kind == nir::ValueKind::SizeOf) {
    return true;
  }
  if (value.kind == nir::ValueKind::Unary || value.kind == nir::ValueKind::Binary) {
    return std::all_of(value.operands.begin(), value.operands.end(),
                       isLiteralLikePropagatableValue);
  }
  if (value.kind == nir::ValueKind::If && value.operands.size() == 3) {
    return std::all_of(value.operands.begin(), value.operands.end(),
                       isLiteralLikePropagatableValue);
  }
  if ((value.kind == nir::ValueKind::AsInstanceOf ||
       value.kind == nir::ValueKind::IsInstanceOf) &&
      value.operands.size() == 1 && isNullLiteral(value.operands.front())) {
    return true;
  }
  return false;
}

bool isPropagatableValue(const nir::Value& value,
                         const std::unordered_set<std::string>& immutableLocals) {
  if (isLiteralLikePropagatableValue(value)) {
    return true;
  }
  return value.kind == nir::ValueKind::Local && immutableLocals.contains(value.text);
}

void collectLocalUses(const nir::Value& value,
                      std::unordered_map<std::string, std::size_t>& uses,
                      const std::unordered_set<std::string>& shadowed);

void collectLocalUsesInBlock(const std::vector<nir::Value>& operands,
                             std::unordered_map<std::string, std::size_t>& uses,
                             std::unordered_set<std::string> shadowed) {
  for (const nir::Value& operand : operands) {
    if (operand.kind == nir::ValueKind::LocalLet ||
        operand.kind == nir::ValueKind::LocalVar) {
      if (!operand.operands.empty()) {
        collectLocalUses(operand.operands.front(), uses, shadowed);
      }
      if (!operand.text.empty()) {
        shadowed.insert(operand.text);
      }
      continue;
    }
    collectLocalUses(operand, uses, shadowed);
  }
}

void collectLocalUses(const nir::Value& value,
                      std::unordered_map<std::string, std::size_t>& uses,
                      const std::unordered_set<std::string>& shadowed) {
  if (value.kind == nir::ValueKind::Local) {
    if (!shadowed.contains(value.text)) {
      ++uses[value.text];
    }
    return;
  }
  if (value.kind == nir::ValueKind::Block) {
    collectLocalUsesInBlock(value.operands, uses, shadowed);
    return;
  }
  if (value.kind == nir::ValueKind::LocalLet ||
      value.kind == nir::ValueKind::LocalVar) {
    if (!value.operands.empty()) {
      collectLocalUses(value.operands.front(), uses, shadowed);
    }
    return;
  }
  for (const nir::Value& operand : value.operands) {
    collectLocalUses(operand, uses, shadowed);
  }
}

std::unordered_map<std::string, std::size_t> localUses(const nir::FunctionBody& body) {
  std::unordered_map<std::string, std::size_t> uses;
  const std::unordered_set<std::string> shadowed;
  for (const nir::Instruction& instruction : body.instructions) {
    if (instruction.kind == nir::InstructionKind::Param ||
        instruction.kind == nir::InstructionKind::Unreachable) {
      continue;
    }
    collectLocalUses(instruction.value, uses, shadowed);
  }
  return uses;
}

std::unordered_map<std::string, std::size_t>
localUsesAfterInstruction(const nir::FunctionBody& body, std::size_t index) {
  std::unordered_map<std::string, std::size_t> uses;
  const std::unordered_set<std::string> shadowed;
  for (std::size_t i = index + 1; i < body.instructions.size(); ++i) {
    const nir::Instruction& instruction = body.instructions[i];
    if (instruction.kind == nir::InstructionKind::Param ||
        instruction.kind == nir::InstructionKind::Unreachable) {
      continue;
    }
    collectLocalUses(instruction.value, uses, shadowed);
  }
  return uses;
}

struct LocalAnalysisState {
  std::unordered_map<std::string, std::string> types;
  std::unordered_map<std::string, nir::Value> values;
};

LocalAnalysisState
localStateBeforeInstruction(const nir::FunctionBody& body, std::size_t endIndex,
                            const DefinitionIndex& definitions,
                            const ParentMap& parentMap,
                            const ConstructorFieldMap& constructorFields,
                            const FieldSnapshotMap& fieldSnapshots) {
  LocalAnalysisState state;
  for (std::size_t i = 0; i < endIndex && i < body.instructions.size(); ++i) {
    const nir::Instruction& instruction = body.instructions[i];
    switch (instruction.kind) {
    case nir::InstructionKind::Param:
      if (!instruction.name.empty() && !instruction.type.empty()) {
        state.types[instruction.name] = instruction.type;
      } else {
        state.types.erase(instruction.name);
      }
      state.values.erase(instruction.name);
      break;
    case nir::InstructionKind::Let: {
      invalidateArrayLocalValuesAfterPotentialMutation(instruction.value, state.types,
                                                       state.values);
      invalidateObjectFieldLocalValuesAfterPotentialMutation(
          instruction.value, state.values, definitions, parentMap, fieldSnapshots);
      const std::string localType = instruction.type.empty()
                                        ? knownValueType(instruction.value, state.types)
                                        : instruction.type;
      if (!instruction.name.empty() && localType != "Unknown") {
        state.types[instruction.name] = localType;
      } else {
        state.types.erase(instruction.name);
      }
      if (!instruction.name.empty()) {
        state.values[instruction.name] =
            extendExactNewFieldSnapshot(instruction.value, definitions, parentMap,
                                        constructorFields, fieldSnapshots);
      }
      break;
    }
    case nir::InstructionKind::Var: {
      invalidateArrayLocalValuesAfterPotentialMutation(instruction.value, state.types,
                                                       state.values);
      invalidateObjectFieldLocalValuesAfterPotentialMutation(
          instruction.value, state.values, definitions, parentMap, fieldSnapshots);
      (void)forgetObjectLocalFieldContentsForRootsInValue(instruction.value,
                                                          state.values);
      const std::string localType = instruction.type.empty()
                                        ? knownValueType(instruction.value, state.types)
                                        : instruction.type;
      if (!instruction.name.empty() && localType != "Unknown") {
        state.types[instruction.name] = localType;
      } else {
        state.types.erase(instruction.name);
      }
      state.values.erase(instruction.name);
      break;
    }
    case nir::InstructionKind::Eval:
    case nir::InstructionKind::Return:
    case nir::InstructionKind::Throw:
      invalidateArrayLocalValuesAfterPotentialMutation(instruction.value, state.types,
                                                       state.values);
      invalidateObjectFieldLocalValuesAfterPotentialMutation(
          instruction.value, state.values, definitions, parentMap, fieldSnapshots);
      break;
    case nir::InstructionKind::Unreachable:
      break;
    }
  }
  return state;
}

bool hasClassOrInheritedFields(const std::string& typeName,
                               const DefinitionIndex& definitions,
                               const ParentMap& parentMap) {
  for (const std::string& current : nir::linearizedTypeNames(typeName, parentMap)) {
    for (const auto& [name, definition] : definitions) {
      if (definition != nullptr && definition->kind == nir::DefinitionKind::Field &&
          ownerNameOf(name) == current) {
        return true;
      }
    }
  }
  return false;
}

bool hasClassOrInheritedInitializer(const std::string& typeName,
                                    const DefinitionIndex& definitions,
                                    const ParentMap& parentMap) {
  for (const std::string& current : nir::linearizedTypeNames(typeName, parentMap)) {
    if (definitions.contains(current + "." +
                             std::string(support::StdNames::Constructor))) {
      return true;
    }
  }
  return false;
}

bool hasExpectedAllocationSnapshotOperands(const nir::Value& value,
                                           const std::string& typeName,
                                           const ConstructorFieldMap& constructorFields,
                                           const FieldSnapshotMap& fieldSnapshots) {
  auto fields = constructorFields.find(typeName);
  const std::size_t expectedConstructorOperands =
      fields == constructorFields.end() ? 0 : fields->second.size();
  if (value.operands.size() == expectedConstructorOperands) {
    return true;
  }

  auto snapshots = fieldSnapshots.find(typeName);
  return snapshots != fieldSnapshots.end() &&
         value.operands.size() == snapshots->second.size();
}

bool isSimplePureInitializerAllocationDiscardable(
    const nir::Value& value, const std::string& typeName,
    const DefinitionIndex& definitions, const ParentMap& parentMap,
    const FieldSnapshotMap& fieldSnapshots) {
  const std::string initializerName =
      typeName + "." + std::string(support::StdNames::Constructor);
  auto initializer = definitions.find(initializerName);
  if (initializer == definitions.end() || initializer->second == nullptr ||
      initializer->second->kind != nir::DefinitionKind::FunctionDef) {
    return false;
  }

  static const std::vector<std::string> emptyFields;
  auto snapshotFields = fieldSnapshots.find(typeName);
  const std::vector<std::string>& fields =
      snapshotFields == fieldSnapshots.end() ? emptyFields : snapshotFields->second;

  nir::Value object = value;
  while (object.operands.size() < fields.size()) {
    object.operands.push_back(nir::unknownValue("<unknown-field>", value.span));
  }

  std::unordered_set<std::string> allowedLocals;
  for (const nir::Value& operand : value.operands) {
    collectLocalNames(operand, allowedLocals);
  }

  bool sawNonParam = false;
  for (const nir::Instruction& instruction : initializer->second->body.instructions) {
    if (instruction.kind == nir::InstructionKind::Param) {
      if (sawNonParam || instruction.name != "this" || instruction.type != typeName) {
        return false;
      }
      continue;
    }

    sawNonParam = true;
    if (isUnitReturn(instruction)) {
      return true;
    }

    if (instruction.kind != nir::InstructionKind::Eval ||
        instruction.value.kind != nir::ValueKind::Assign ||
        instruction.value.operands.size() != 2) {
      return false;
    }

    const std::optional<std::string> field = resolveInitializerTargetField(
        instruction.value.operands.front(), typeName, definitions, parentMap);
    if (!field) {
      return false;
    }
    auto fieldName = std::find(fields.begin(), fields.end(), *field);
    if (fieldName == fields.end()) {
      return false;
    }

    const std::size_t index =
        static_cast<std::size_t>(std::distance(fields.begin(), fieldName));
    nir::Value assigned = instruction.value.operands.back();
    (void)substituteExactSnapshotFields(assigned, object, typeName, definitions,
                                        parentMap, fieldSnapshots);
    if (!isPureLetValue(assigned) || !usesOnlyAllowedLocals(assigned, allowedLocals)) {
      return false;
    }
    object.operands[index] = std::move(assigned);
  }
  return false;
}

bool isSimplePureInitializerShellDiscardable(const nir::Value& value,
                                             const std::string& typeName,
                                             const DefinitionIndex& definitions,
                                             const ParentMap& parentMap,
                                             const FieldSnapshotMap& fieldSnapshots) {
  nir::Value probe = value;
  for (std::size_t i = 0; i < probe.operands.size(); ++i) {
    probe.operands[i] =
        nir::localValue("$allocation.arg." + std::to_string(i), probe.operands[i].span);
  }
  return isSimplePureInitializerAllocationDiscardable(probe, typeName, definitions,
                                                      parentMap, fieldSnapshots);
}

bool isUnusedObjectAllocationShellDiscardable(
    const nir::Value& value, const DefinitionIndex& definitions,
    const ParentMap& parentMap, const ConstructorFieldMap& constructorFields,
    const FieldSnapshotMap& fieldSnapshots) {
  if (value.kind != nir::ValueKind::New || arrayElementTypeName(value.type)) {
    return false;
  }

  const std::string typeName = value.text.empty() ? value.type : value.text;
  auto definition = definitions.find(typeName);
  if (definition == definitions.end() || definition->second == nullptr ||
      definition->second->kind != nir::DefinitionKind::Class ||
      !hasExpectedAllocationSnapshotOperands(value, typeName, constructorFields,
                                             fieldSnapshots)) {
    return false;
  }

  if (hasOnlyFoldableDirectConstructorFieldStorage(typeName, definitions, parentMap,
                                                   constructorFields)) {
    return true;
  }
  if (!hasClassOrInheritedInitializer(typeName, definitions, parentMap)) {
    return !hasClassOrInheritedFields(typeName, definitions, parentMap);
  }
  return isSimplePureInitializerShellDiscardable(value, typeName, definitions,
                                                 parentMap, fieldSnapshots);
}

bool isUnusedAllocationDiscardable(const nir::Value& value,
                                   const DefinitionIndex& definitions,
                                   const ParentMap& parentMap,
                                   const ConstructorFieldMap& constructorFields,
                                   const FieldSnapshotMap& fieldSnapshots) {
  if (value.kind != nir::ValueKind::New) {
    return false;
  }

  const auto isDiscardableOperand = [&](const nir::Value& operand) {
    return isPureLetValue(operand) ||
           isUnusedAllocationDiscardable(operand, definitions, parentMap,
                                         constructorFields, fieldSnapshots);
  };

  if (arrayElementTypeName(value.type)) {
    return value.text == value.type &&
           std::all_of(value.operands.begin(), value.operands.end(),
                       isDiscardableOperand);
  }

  const std::string typeName = value.text.empty() ? value.type : value.text;
  auto definition = definitions.find(typeName);
  if (definition == definitions.end() || definition->second == nullptr ||
      definition->second->kind != nir::DefinitionKind::Class) {
    return false;
  }

  if (!hasExpectedAllocationSnapshotOperands(value, typeName, constructorFields,
                                             fieldSnapshots) ||
      !std::all_of(value.operands.begin(), value.operands.end(),
                   isDiscardableOperand)) {
    return false;
  }

  if (!hasClassOrInheritedInitializer(typeName, definitions, parentMap)) {
    return !hasClassOrInheritedFields(typeName, definitions, parentMap) ||
           hasOnlyFoldableDirectConstructorFieldStorage(typeName, definitions,
                                                        parentMap, constructorFields);
  }

  return isSimplePureInitializerAllocationDiscardable(value, typeName, definitions,
                                                      parentMap, fieldSnapshots);
}

bool isPureLetValue(const nir::Value& value) {
  switch (value.kind) {
  case nir::ValueKind::Unit:
  case nir::ValueKind::Local:
  case nir::ValueKind::Literal:
  case nir::ValueKind::SizeOf:
    return true;
  case nir::ValueKind::Unary:
    return std::all_of(value.operands.begin(), value.operands.end(), isPureLetValue);
  case nir::ValueKind::Binary:
    return value.text != "/" && value.text != "%" &&
           std::all_of(value.operands.begin(), value.operands.end(), isPureLetValue);
  case nir::ValueKind::If:
    return value.operands.size() == 3 &&
           std::all_of(value.operands.begin(), value.operands.end(), isPureLetValue);
  case nir::ValueKind::IsInstanceOf:
    return value.operands.size() == 1 && isPureLetValue(value.operands.front());
  case nir::ValueKind::Box:
    return value.operands.size() == 1 && isPureLetValue(value.operands.front());
  case nir::ValueKind::Assign:
  case nir::ValueKind::Throw:
  case nir::ValueKind::Try:
  case nir::ValueKind::Catch:
  case nir::ValueKind::Finally:
  case nir::ValueKind::Call:
  case nir::ValueKind::Select:
  case nir::ValueKind::While:
  case nir::ValueKind::Block:
  case nir::ValueKind::LocalLet:
  case nir::ValueKind::LocalVar:
  case nir::ValueKind::New:
  case nir::ValueKind::ZoneScoped:
  case nir::ValueKind::Unbox:
  case nir::ValueKind::AsInstanceOf:
  case nir::ValueKind::Super:
  case nir::ValueKind::Unknown:
    return false;
  }
  return false;
}

bool isUnusedValueDiscardable(const nir::Value& value,
                              const DefinitionIndex& definitions,
                              const ParentMap& parentMap,
                              const ConstructorFieldMap& constructorFields,
                              const FieldSnapshotMap& fieldSnapshots) {
  if (isPureLetValue(value) ||
      isUnusedAllocationDiscardable(value, definitions, parentMap, constructorFields,
                                    fieldSnapshots)) {
    return true;
  }
  return value.kind == nir::ValueKind::If && value.operands.size() == 3 &&
         isPureLetValue(value.operands[0]) &&
         isUnusedValueDiscardable(value.operands[1], definitions, parentMap,
                                  constructorFields, fieldSnapshots) &&
         isUnusedValueDiscardable(value.operands[2], definitions, parentMap,
                                  constructorFields, fieldSnapshots);
}

bool isTriviallyDiscardableValue(const nir::Value& value) {
  switch (value.kind) {
  case nir::ValueKind::Unit:
  case nir::ValueKind::Local:
  case nir::ValueKind::Literal:
  case nir::ValueKind::SizeOf:
    return true;
  case nir::ValueKind::Unary:
  case nir::ValueKind::Binary:
  case nir::ValueKind::If:
  case nir::ValueKind::Box:
  case nir::ValueKind::IsInstanceOf:
    return isPureLetValue(value);
  case nir::ValueKind::Assign:
  case nir::ValueKind::Throw:
  case nir::ValueKind::Try:
  case nir::ValueKind::Catch:
  case nir::ValueKind::Finally:
  case nir::ValueKind::Call:
  case nir::ValueKind::Select:
  case nir::ValueKind::While:
  case nir::ValueKind::Block:
  case nir::ValueKind::LocalLet:
  case nir::ValueKind::LocalVar:
  case nir::ValueKind::New:
  case nir::ValueKind::ZoneScoped:
  case nir::ValueKind::Unbox:
  case nir::ValueKind::AsInstanceOf:
  case nir::ValueKind::Super:
  case nir::ValueKind::Unknown:
    return false;
  }
  return false;
}

std::string blockResultType(const std::vector<nir::Value>& operands) {
  if (operands.empty() || operands.back().kind == nir::ValueKind::LocalLet ||
      operands.back().kind == nir::ValueKind::LocalVar) {
    return "Unit";
  }
  return operands.back().type;
}

std::size_t collectVisibleUses(const nir::Value& value, const std::string& name);

std::size_t collectVisibleUsesInBlock(const std::vector<nir::Value>& operands,
                                      const std::string& name,
                                      std::size_t startIndex = 0) {
  std::size_t uses = 0;
  for (std::size_t i = startIndex; i < operands.size(); ++i) {
    const nir::Value& operand = operands[i];
    if (operand.kind == nir::ValueKind::LocalLet ||
        operand.kind == nir::ValueKind::LocalVar) {
      if (!operand.operands.empty()) {
        uses += collectVisibleUses(operand.operands.front(), name);
      }
      if (operand.text == name) {
        break;
      }
      continue;
    }
    uses += collectVisibleUses(operand, name);
  }
  return uses;
}

std::size_t collectVisibleUses(const nir::Value& value, const std::string& name) {
  if (value.kind == nir::ValueKind::Local) {
    return value.text == name ? 1 : 0;
  }
  if (value.kind == nir::ValueKind::Block) {
    return collectVisibleUsesInBlock(value.operands, name);
  }
  if (value.kind == nir::ValueKind::LocalLet ||
      value.kind == nir::ValueKind::LocalVar) {
    return value.operands.empty() ? 0
                                  : collectVisibleUses(value.operands.front(), name);
  }

  std::size_t uses = 0;
  for (const nir::Value& operand : value.operands) {
    uses += collectVisibleUses(operand, name);
  }
  return uses;
}

bool replaceLocalUse(nir::Value& value, const std::string& name,
                     const nir::Value& replacement);

bool replaceLocalUseInBlock(std::vector<nir::Value>& operands, const std::string& name,
                            const nir::Value& replacement) {
  for (nir::Value& operand : operands) {
    if (operand.kind == nir::ValueKind::LocalLet ||
        operand.kind == nir::ValueKind::LocalVar) {
      if (!operand.operands.empty() &&
          replaceLocalUse(operand.operands.front(), name, replacement)) {
        return true;
      }
      if (operand.text == name) {
        return false;
      }
      continue;
    }
    if (replaceLocalUse(operand, name, replacement)) {
      return true;
    }
  }
  return false;
}

bool replaceLocalUse(nir::Value& value, const std::string& name,
                     const nir::Value& replacement) {
  if (value.kind == nir::ValueKind::Local) {
    if (value.text != name) {
      return false;
    }
    replaceValue(value, replacement);
    return true;
  }
  if (value.kind == nir::ValueKind::Block) {
    return replaceLocalUseInBlock(value.operands, name, replacement);
  }
  if (value.kind == nir::ValueKind::LocalLet ||
      value.kind == nir::ValueKind::LocalVar) {
    return !value.operands.empty() &&
           replaceLocalUse(value.operands.front(), name, replacement);
  }

  for (nir::Value& operand : value.operands) {
    if (replaceLocalUse(operand, name, replacement)) {
      return true;
    }
  }
  return false;
}

std::size_t eliminateDeadLocalLetsInValue(nir::Value& value,
                                          const DefinitionIndex& definitions,
                                          const ParentMap& parentMap,
                                          const ConstructorFieldMap& constructorFields,
                                          const FieldSnapshotMap& fieldSnapshots) {
  std::size_t changedValues = 0;
  for (nir::Value& operand : value.operands) {
    changedValues += eliminateDeadLocalLetsInValue(operand, definitions, parentMap,
                                                   constructorFields, fieldSnapshots);
  }

  if (value.kind != nir::ValueKind::Block) {
    return changedValues;
  }

  bool removed = true;
  while (removed) {
    removed = false;
    for (std::size_t i = 0; i < value.operands.size(); ++i) {
      nir::Value& operand = value.operands[i];
      const bool isFinalOperand = i + 1 == value.operands.size();
      if (!isFinalOperand && operand.kind != nir::ValueKind::LocalLet &&
          operand.kind != nir::ValueKind::LocalVar) {
        const std::size_t discarded = discardUnusedAllocationEffects(
            operand, definitions, parentMap, constructorFields, fieldSnapshots);
        changedValues += discarded;
        removed = removed || discarded != 0;
      }
      const bool unusedLocalBinding =
          (operand.kind == nir::ValueKind::LocalLet ||
           operand.kind == nir::ValueKind::LocalVar) &&
          !isFinalOperand && !operand.text.empty() && operand.operands.size() == 1 &&
          collectVisibleUsesInBlock(value.operands, operand.text, i + 1) == 0;
      const bool deadLocalBinding =
          unusedLocalBinding &&
          isUnusedValueDiscardable(operand.operands.front(), definitions, parentMap,
                                   constructorFields, fieldSnapshots);
      const bool effectOnlyLocalBinding = unusedLocalBinding && !deadLocalBinding;
      const bool deadPureOperand =
          !isFinalOperand && operand.kind != nir::ValueKind::LocalLet &&
          operand.kind != nir::ValueKind::LocalVar &&
          isUnusedValueDiscardable(operand, definitions, parentMap, constructorFields,
                                   fieldSnapshots);
      if (effectOnlyLocalBinding) {
        replaceValue(operand, operand.operands.front());
        ++changedValues;
        removed = true;
        break;
      }
      if (!deadLocalBinding && !deadPureOperand) {
        continue;
      }
      value.operands.erase(value.operands.begin() + static_cast<std::ptrdiff_t>(i));
      ++changedValues;
      removed = true;
      break;
    }
  }
  value.type = blockResultType(value.operands);
  return changedValues;
}

std::size_t discardUnusedValueResult(nir::Value& value);
bool isUnitResultValue(const nir::Value& value);
nir::Value effectOnlyUnitValue(nir::Value value, support::SourceSpan span);

bool hasVisibleAssignmentToLocal(const nir::Value& value, const std::string& name);

bool hasVisibleAssignmentToLocalInBlock(const std::vector<nir::Value>& operands,
                                        const std::string& name,
                                        std::size_t startIndex = 0) {
  for (std::size_t i = startIndex; i < operands.size(); ++i) {
    const nir::Value& operand = operands[i];
    if (operand.kind == nir::ValueKind::LocalLet ||
        operand.kind == nir::ValueKind::LocalVar) {
      if (!operand.operands.empty() &&
          hasVisibleAssignmentToLocal(operand.operands.front(), name)) {
        return true;
      }
      if (operand.text == name) {
        return false;
      }
      continue;
    }
    if (hasVisibleAssignmentToLocal(operand, name)) {
      return true;
    }
  }
  return false;
}

bool hasVisibleAssignmentToLocal(const nir::Value& value, const std::string& name) {
  if (value.kind == nir::ValueKind::Assign && value.operands.size() == 2) {
    const nir::Value& target = value.operands.front();
    if (target.kind == nir::ValueKind::Local && target.text == name) {
      return true;
    }
    return (target.kind != nir::ValueKind::Local &&
            hasVisibleAssignmentToLocal(target, name)) ||
           hasVisibleAssignmentToLocal(value.operands.back(), name);
  }
  if (value.kind == nir::ValueKind::Block) {
    return hasVisibleAssignmentToLocalInBlock(value.operands, name);
  }
  if (value.kind == nir::ValueKind::LocalLet ||
      value.kind == nir::ValueKind::LocalVar) {
    return !value.operands.empty() &&
           hasVisibleAssignmentToLocal(value.operands.front(), name);
  }

  return std::any_of(value.operands.begin(), value.operands.end(),
                     [&](const nir::Value& operand) {
                       return hasVisibleAssignmentToLocal(operand, name);
                     });
}

std::size_t promoteUnassignedLocalVarsInBlock(nir::Value& value) {
  if (value.kind != nir::ValueKind::Block) {
    return 0;
  }

  std::size_t changedValues = 0;
  for (std::size_t i = 0; i < value.operands.size(); ++i) {
    nir::Value& operand = value.operands[i];
    if (operand.kind == nir::ValueKind::LocalVar && !operand.text.empty() &&
        !hasVisibleAssignmentToLocalInBlock(value.operands, operand.text, i + 1)) {
      operand.kind = nir::ValueKind::LocalLet;
      ++changedValues;
    }
  }
  return changedValues;
}

bool hasVisibleAssignmentToTopLevelLocalAfterInstruction(const nir::FunctionBody& body,
                                                         const std::string& name,
                                                         std::size_t index) {
  for (std::size_t i = index + 1; i < body.instructions.size(); ++i) {
    const nir::Instruction& instruction = body.instructions[i];
    if (instruction.kind == nir::InstructionKind::Param ||
        instruction.kind == nir::InstructionKind::Unreachable) {
      continue;
    }
    if (hasVisibleAssignmentToLocal(instruction.value, name)) {
      return true;
    }
    if ((instruction.kind == nir::InstructionKind::Let ||
         instruction.kind == nir::InstructionKind::Var) &&
        instruction.name == name) {
      return false;
    }
  }
  return false;
}

bool hasTopLevelLocalBinding(const nir::Value& value) {
  return std::any_of(value.operands.begin(), value.operands.end(),
                     [](const nir::Value& operand) {
                       return operand.kind == nir::ValueKind::LocalLet ||
                              operand.kind == nir::ValueKind::LocalVar;
                     });
}

std::size_t simplifyBlocksInValue(nir::Value& value) {
  std::size_t changedValues = 0;
  for (nir::Value& operand : value.operands) {
    changedValues += simplifyBlocksInValue(operand);
  }

  if (value.kind == nir::ValueKind::While && value.operands.size() == 2 &&
      !isUnitResultValue(value.operands[1])) {
    value.operands[1] = effectOnlyUnitValue(std::move(value.operands[1]), value.span);
    value.type = "Unit";
    return changedValues + 1;
  }

  if (value.kind != nir::ValueKind::Block) {
    return changedValues;
  }

  changedValues += promoteUnassignedLocalVarsInBlock(value);

  for (std::size_t i = 0; i + 1 < value.operands.size();) {
    changedValues += discardUnusedValueResult(value.operands[i]);
    if (value.operands[i].kind != nir::ValueKind::LocalLet &&
        value.operands[i].kind != nir::ValueKind::LocalVar &&
        isTriviallyDiscardableValue(value.operands[i])) {
      value.operands.erase(value.operands.begin() + static_cast<std::ptrdiff_t>(i));
      ++changedValues;
      continue;
    }
    ++i;
  }

  while (!value.operands.empty() &&
         (value.operands.back().kind == nir::ValueKind::LocalLet ||
          value.operands.back().kind == nir::ValueKind::LocalVar) &&
         value.operands.back().operands.size() == 1) {
    nir::Value& binding = value.operands.back();
    binding = effectOnlyUnitValue(std::move(binding.operands.front()), binding.span);
    ++changedValues;
  }

  bool flattenedNestedEffectBlock = true;
  while (flattenedNestedEffectBlock) {
    flattenedNestedEffectBlock = false;
    for (std::size_t i = 0; i + 1 < value.operands.size(); ++i) {
      nir::Value& operand = value.operands[i];
      if (operand.kind != nir::ValueKind::Block || hasTopLevelLocalBinding(operand)) {
        continue;
      }

      std::vector<nir::Value> nestedOperands = std::move(operand.operands);
      auto position =
          value.operands.erase(value.operands.begin() + static_cast<std::ptrdiff_t>(i));
      value.operands.insert(position, std::make_move_iterator(nestedOperands.begin()),
                            std::make_move_iterator(nestedOperands.end()));
      ++changedValues;
      flattenedNestedEffectBlock = true;
      break;
    }
  }

  while (!value.operands.empty() &&
         value.operands.back().kind == nir::ValueKind::Block) {
    std::vector<nir::Value> nestedOperands = std::move(value.operands.back().operands);
    value.operands.pop_back();
    value.operands.insert(value.operands.end(),
                          std::make_move_iterator(nestedOperands.begin()),
                          std::make_move_iterator(nestedOperands.end()));
    ++changedValues;
  }

  bool inlinedAdjacentBinding = true;
  while (inlinedAdjacentBinding) {
    inlinedAdjacentBinding = false;
    for (std::size_t i = 0; i + 1 < value.operands.size(); ++i) {
      nir::Value& binding = value.operands[i];
      nir::Value& nextBinding = value.operands[i + 1];
      if (binding.kind != nir::ValueKind::LocalLet || binding.text.empty() ||
          binding.operands.size() != 1 || !isPureLetValue(binding.operands.front())) {
        continue;
      }
      if ((nextBinding.kind != nir::ValueKind::LocalLet &&
           nextBinding.kind != nir::ValueKind::LocalVar) ||
          nextBinding.operands.size() != 1 ||
          !isPureLetValue(nextBinding.operands.front()) ||
          collectVisibleUses(nextBinding.operands.front(), binding.text) != 1) {
        continue;
      }

      nir::Value nextValue = nextBinding.operands.front();
      if (!replaceLocalUse(nextValue, binding.text, binding.operands.front())) {
        continue;
      }
      nextBinding.operands.front() = std::move(nextValue);
      value.operands.erase(value.operands.begin() + static_cast<std::ptrdiff_t>(i));
      ++changedValues;
      inlinedAdjacentBinding = true;
      break;
    }
  }

  bool inlinedLiteralBindingIntoOnlyUse = true;
  while (inlinedLiteralBindingIntoOnlyUse) {
    inlinedLiteralBindingIntoOnlyUse = false;
    for (std::size_t i = 0; i + 1 < value.operands.size(); ++i) {
      nir::Value& binding = value.operands[i];
      if (binding.kind != nir::ValueKind::LocalLet || binding.text.empty() ||
          binding.operands.size() != 1 ||
          !isLiteralLikePropagatableValue(binding.operands.front()) ||
          collectVisibleUsesInBlock(value.operands, binding.text, i + 1) != 1 ||
          hasVisibleAssignmentToLocalInBlock(value.operands, binding.text, i + 1)) {
        continue;
      }

      for (std::size_t j = i + 1; j < value.operands.size(); ++j) {
        nir::Value& use = value.operands[j];
        if (collectVisibleUses(use, binding.text) == 1) {
          nir::Value useValue = use;
          if (replaceLocalUse(useValue, binding.text, binding.operands.front())) {
            use = std::move(useValue);
            value.operands.erase(value.operands.begin() +
                                 static_cast<std::ptrdiff_t>(i));
            ++changedValues;
            inlinedLiteralBindingIntoOnlyUse = true;
          }
          break;
        }
        if ((use.kind == nir::ValueKind::LocalLet ||
             use.kind == nir::ValueKind::LocalVar) &&
            use.text == binding.text) {
          break;
        }
      }
      if (inlinedLiteralBindingIntoOnlyUse) {
        break;
      }
    }
  }

  value.type = blockResultType(value.operands);

  if (value.operands.empty()) {
    value = nir::unitValue(value.span);
    return changedValues + 1;
  }

  if (value.operands.size() == 1 &&
      value.operands.front().kind != nir::ValueKind::LocalLet &&
      value.operands.front().kind != nir::ValueKind::LocalVar) {
    replaceValue(value, value.operands.front());
    return changedValues + 1;
  }

  if (value.operands.size() == 2 &&
      value.operands[0].kind == nir::ValueKind::LocalLet &&
      !value.operands[0].text.empty() && value.operands[0].operands.size() == 1 &&
      isPureLetValue(value.operands[0].operands.front()) &&
      isPureLetValue(value.operands[1]) &&
      collectVisibleUses(value.operands[1], value.operands[0].text) == 1) {
    nir::Value inlined = value.operands[1];
    if (replaceLocalUse(inlined, value.operands[0].text,
                        value.operands[0].operands.front())) {
      replaceValue(value, inlined);
      return changedValues + 1;
    }
  }

  if (value.operands.size() == 2 &&
      (value.operands[0].kind == nir::ValueKind::LocalLet ||
       value.operands[0].kind == nir::ValueKind::LocalVar) &&
      value.operands[1].kind == nir::ValueKind::Local &&
      value.operands[0].text == value.operands[1].text &&
      value.operands[0].operands.size() == 1 &&
      isPureLetValue(value.operands[0].operands.front())) {
    replaceValue(value, value.operands[0].operands.front());
    return changedValues + 1;
  }

  return changedValues;
}

std::size_t promoteUnassignedTopLevelVars(nir::FunctionBody& body) {
  std::size_t changedValues = 0;
  for (std::size_t i = 0; i < body.instructions.size(); ++i) {
    nir::Instruction& instruction = body.instructions[i];
    if (instruction.kind == nir::InstructionKind::Var && !instruction.name.empty() &&
        !hasVisibleAssignmentToTopLevelLocalAfterInstruction(body, instruction.name,
                                                             i)) {
      instruction.kind = nir::InstructionKind::Let;
      ++changedValues;
    }
  }
  return changedValues;
}

std::size_t collectVisibleTopLevelUsesAfterInstruction(const nir::FunctionBody& body,
                                                       const std::string& name,
                                                       std::size_t index) {
  std::size_t uses = 0;
  for (std::size_t i = index + 1; i < body.instructions.size(); ++i) {
    const nir::Instruction& instruction = body.instructions[i];
    if (instruction.kind == nir::InstructionKind::Param ||
        instruction.kind == nir::InstructionKind::Unreachable) {
      continue;
    }
    uses += collectVisibleUses(instruction.value, name);
    if ((instruction.kind == nir::InstructionKind::Let ||
         instruction.kind == nir::InstructionKind::Var) &&
        instruction.name == name) {
      break;
    }
  }
  return uses;
}

bool containsLocalBindingForAnyName(const nir::Value& value,
                                    const std::unordered_set<std::string>& names) {
  if (names.empty()) {
    return false;
  }
  if ((value.kind == nir::ValueKind::LocalLet ||
       value.kind == nir::ValueKind::LocalVar) &&
      names.contains(value.text)) {
    return true;
  }
  return std::any_of(value.operands.begin(), value.operands.end(),
                     [&](const nir::Value& operand) {
                       return containsLocalBindingForAnyName(operand, names);
                     });
}

bool localNamesAreImmutableBeforeInstruction(
    const nir::FunctionBody& body, const std::unordered_set<std::string>& names,
    std::size_t index) {
  std::unordered_map<std::string, bool> immutableLocals;
  for (std::size_t i = 0; i < index && i < body.instructions.size(); ++i) {
    const nir::Instruction& instruction = body.instructions[i];
    if (instruction.name.empty()) {
      continue;
    }
    if (instruction.kind == nir::InstructionKind::Param ||
        instruction.kind == nir::InstructionKind::Let) {
      immutableLocals[instruction.name] = true;
    } else if (instruction.kind == nir::InstructionKind::Var) {
      immutableLocals[instruction.name] = false;
    }
  }

  return std::all_of(names.begin(), names.end(), [&](const std::string& name) {
    auto local = immutableLocals.find(name);
    return local != immutableLocals.end() && local->second;
  });
}

std::size_t inlineAdjacentSingleUseTopLevelLets(nir::FunctionBody& body) {
  std::size_t changedValues = 0;
  bool inlined = true;
  while (inlined) {
    inlined = false;
    for (std::size_t i = 0; i + 1 < body.instructions.size(); ++i) {
      nir::Instruction& binding = body.instructions[i];
      nir::Instruction& next = body.instructions[i + 1];
      if (binding.kind != nir::InstructionKind::Let || binding.name.empty() ||
          !isPureLetValue(binding.value) || !isPureLetValue(next.value) ||
          collectVisibleTopLevelUsesAfterInstruction(body, binding.name, i) != 1 ||
          collectVisibleUses(next.value, binding.name) != 1 ||
          hasVisibleAssignmentToTopLevelLocalAfterInstruction(body, binding.name, i)) {
        continue;
      }

      std::unordered_set<std::string> replacementLocals;
      collectLocalNames(binding.value, replacementLocals);
      if (!localNamesAreImmutableBeforeInstruction(body, replacementLocals, i) ||
          containsLocalBindingForAnyName(next.value, replacementLocals)) {
        continue;
      }

      nir::Value nextValue = next.value;
      if (!replaceLocalUse(nextValue, binding.name, binding.value)) {
        continue;
      }
      next.value = std::move(nextValue);
      ++changedValues;
      inlined = true;
      break;
    }
  }
  return changedValues;
}

std::size_t inlineSingleUseLiteralLikeTopLevelLets(nir::FunctionBody& body) {
  std::size_t changedValues = 0;
  bool inlined = true;
  while (inlined) {
    inlined = false;
    for (std::size_t i = 0; i + 1 < body.instructions.size(); ++i) {
      const nir::Instruction& binding = body.instructions[i];
      if (binding.kind != nir::InstructionKind::Let || binding.name.empty() ||
          !isLiteralLikePropagatableValue(binding.value) ||
          collectVisibleTopLevelUsesAfterInstruction(body, binding.name, i) != 1 ||
          hasVisibleAssignmentToTopLevelLocalAfterInstruction(body, binding.name, i)) {
        continue;
      }

      for (std::size_t j = i + 1; j < body.instructions.size(); ++j) {
        nir::Instruction& use = body.instructions[j];
        if (collectVisibleUses(use.value, binding.name) == 1) {
          nir::Value useValue = use.value;
          if (replaceLocalUse(useValue, binding.name, binding.value)) {
            use.value = std::move(useValue);
            ++changedValues;
            inlined = true;
          }
          break;
        }
        if ((use.kind == nir::InstructionKind::Let ||
             use.kind == nir::InstructionKind::Var) &&
            use.name == binding.name) {
          break;
        }
      }
      if (inlined) {
        break;
      }
    }
  }
  return changedValues;
}

std::size_t normalizeSimplifiedBlock(nir::Value& value) {
  if (value.kind != nir::ValueKind::Block) {
    return 0;
  }

  value.type = blockResultType(value.operands);
  if (value.operands.empty()) {
    value = nir::unitValue(value.span);
    return 1;
  }
  if (value.operands.size() == 1 &&
      value.operands.front().kind != nir::ValueKind::LocalLet &&
      value.operands.front().kind != nir::ValueKind::LocalVar) {
    replaceValue(value, value.operands.front());
    return 1;
  }
  return 0;
}

bool isEffectTransparentDiscardWrapper(const nir::Value& value) {
  switch (value.kind) {
  case nir::ValueKind::Unary:
  case nir::ValueKind::Box:
  case nir::ValueKind::IsInstanceOf:
    return value.operands.size() == 1;
  case nir::ValueKind::Unit:
  case nir::ValueKind::Local:
  case nir::ValueKind::Literal:
  case nir::ValueKind::Binary:
  case nir::ValueKind::Assign:
  case nir::ValueKind::Throw:
  case nir::ValueKind::Try:
  case nir::ValueKind::Catch:
  case nir::ValueKind::Finally:
  case nir::ValueKind::Call:
  case nir::ValueKind::Select:
  case nir::ValueKind::If:
  case nir::ValueKind::While:
  case nir::ValueKind::Block:
  case nir::ValueKind::LocalLet:
  case nir::ValueKind::LocalVar:
  case nir::ValueKind::New:
  case nir::ValueKind::SizeOf:
  case nir::ValueKind::ZoneScoped:
  case nir::ValueKind::Unbox:
  case nir::ValueKind::AsInstanceOf:
  case nir::ValueKind::Super:
  case nir::ValueKind::Unknown:
    return false;
  }
  return false;
}

bool isEffectTransparentDiscardBinaryComparison(const nir::Value& value) {
  return value.kind == nir::ValueKind::Binary && value.operands.size() == 2 &&
         (value.text == "==" || value.text == "!=" || value.text == "<" ||
          value.text == ">" || value.text == "<=" || value.text == ">=");
}

bool isDiscardedShortCircuitLogicalBinary(const nir::Value& value) {
  return value.kind == nir::ValueKind::Binary && value.operands.size() == 2 &&
         (value.text == "&&" || value.text == "||");
}

bool isUnitResultValue(const nir::Value& value) {
  return value.kind == nir::ValueKind::Unit || value.type == "Unit";
}

nir::Value effectOnlyUnitValue(nir::Value value, support::SourceSpan span) {
  while ((value.kind == nir::ValueKind::LocalLet ||
          value.kind == nir::ValueKind::LocalVar) &&
         value.operands.size() == 1) {
    replaceValue(value, value.operands.front());
  }

  (void)discardUnusedValueResult(value);
  if (isTriviallyDiscardableValue(value)) {
    return nir::unitValue(span);
  }
  if (isUnitResultValue(value)) {
    return value;
  }

  std::vector<nir::Value> operands;
  if (value.kind == nir::ValueKind::Block) {
    operands = std::move(value.operands);
  } else {
    operands.push_back(std::move(value));
  }
  operands.push_back(nir::unitValue(span));
  return nir::blockValue(std::move(operands), span);
}

std::size_t discardUnusedValueResult(nir::Value& value) {
  if (isEffectTransparentDiscardWrapper(value)) {
    replaceValue(value, value.operands.front());
    return discardUnusedValueResult(value) + 1;
  }

  if (value.kind == nir::ValueKind::If && value.operands.size() == 3 &&
      isTriviallyDiscardableValue(value.operands[1]) &&
      isTriviallyDiscardableValue(value.operands[2])) {
    replaceValue(value, value.operands[0]);
    return discardUnusedValueResult(value) + 1;
  }

  if (value.kind == nir::ValueKind::If && value.operands.size() == 3 &&
      (!isUnitResultValue(value.operands[1]) ||
       !isUnitResultValue(value.operands[2]))) {
    value.operands[1] = effectOnlyUnitValue(std::move(value.operands[1]), value.span);
    value.operands[2] = effectOnlyUnitValue(std::move(value.operands[2]), value.span);
    value.type = "Unit";
    return 1;
  }

  if (value.kind == nir::ValueKind::While && value.operands.size() == 2 &&
      !isUnitResultValue(value.operands[1])) {
    value.operands[1] = effectOnlyUnitValue(std::move(value.operands[1]), value.span);
    value.type = "Unit";
    return 1;
  }

  if (value.kind == nir::ValueKind::ZoneScoped && value.operands.size() == 1 &&
      !isUnitResultValue(value.operands.front())) {
    value.operands.front() =
        effectOnlyUnitValue(std::move(value.operands.front()), value.span);
    value.type = "Unit";
    return 1;
  }

  if (isDiscardedShortCircuitLogicalBinary(value)) {
    const bool isAnd = value.text == "&&";
    std::vector<nir::Value> operands = std::move(value.operands);
    nir::Value condition = std::move(operands.front());
    nir::Value rhs = effectOnlyUnitValue(std::move(operands.back()), value.span);
    nir::Value skipped = nir::unitValue(value.span);
    value = isAnd ? nir::ifValue(std::move(condition), std::move(rhs),
                                 std::move(skipped), value.span)
                  : nir::ifValue(std::move(condition), std::move(skipped),
                                 std::move(rhs), value.span);
    value.type = "Unit";
    return discardUnusedValueResult(value) + 1;
  }

  if (isEffectTransparentDiscardBinaryComparison(value)) {
    std::vector<nir::Value> operands = std::move(value.operands);
    value = nir::blockValue(std::move(operands), value.span);
    return discardUnusedValueResult(value) + 1;
  }

  if (value.kind != nir::ValueKind::Block) {
    return 0;
  }

  std::size_t changedValues = 0;
  while (!value.operands.empty()) {
    nir::Value& result = value.operands.back();
    if ((result.kind == nir::ValueKind::LocalLet ||
         result.kind == nir::ValueKind::LocalVar) &&
        result.operands.size() == 1) {
      replaceValue(result, result.operands.front());
      ++changedValues;
      continue;
    }
    changedValues += discardUnusedValueResult(result);
    if (!isTriviallyDiscardableValue(result)) {
      break;
    }
    value.operands.pop_back();
    ++changedValues;
  }
  changedValues += normalizeSimplifiedBlock(value);
  return changedValues;
}

std::size_t discardUnusedAllocationEffects(nir::Value& value,
                                           const DefinitionIndex& definitions,
                                           const ParentMap& parentMap,
                                           const ConstructorFieldMap& constructorFields,
                                           const FieldSnapshotMap& fieldSnapshots) {
  const bool discardableArrayShell = value.kind == nir::ValueKind::New &&
                                     arrayElementTypeName(value.type) &&
                                     value.text == value.type;
  const bool discardableObjectShell =
      value.kind == nir::ValueKind::New && !arrayElementTypeName(value.type) &&
      isUnusedObjectAllocationShellDiscardable(value, definitions, parentMap,
                                               constructorFields, fieldSnapshots);
  if ((discardableArrayShell || discardableObjectShell) &&
      !isUnusedAllocationDiscardable(value, definitions, parentMap, constructorFields,
                                     fieldSnapshots)) {
    std::size_t changedValues = 1;
    std::vector<nir::Value> effects;
    effects.reserve(value.operands.size());
    for (nir::Value& operand : value.operands) {
      if (isUnusedValueDiscardable(operand, definitions, parentMap, constructorFields,
                                   fieldSnapshots)) {
        continue;
      }
      const support::SourceSpan span = operand.span;
      changedValues += discardUnusedAllocationEffects(
          operand, definitions, parentMap, constructorFields, fieldSnapshots);
      effects.push_back(effectOnlyUnitValue(std::move(operand), span));
    }
    if (effects.empty()) {
      value = nir::unitValue(value.span);
      return changedValues;
    }
    const support::SourceSpan span = value.span;
    if (effects.size() == 1) {
      replaceValue(value, effects.front());
    } else {
      value = nir::blockValue(std::move(effects), span);
    }
    return changedValues;
  }

  if (value.kind != nir::ValueKind::If || value.operands.size() != 3) {
    return 0;
  }

  const bool discardThen = isUnusedValueDiscardable(
      value.operands[1], definitions, parentMap, constructorFields, fieldSnapshots);
  const bool discardElse = isUnusedValueDiscardable(
      value.operands[2], definitions, parentMap, constructorFields, fieldSnapshots);
  if (discardThen && discardElse) {
    replaceValue(value, value.operands[0]);
    return discardUnusedValueResult(value) + 1;
  }
  if (!discardThen && !discardElse) {
    return 0;
  }

  std::size_t changedValues = 0;
  for (std::size_t i = 1; i < value.operands.size(); ++i) {
    nir::Value& branch = value.operands[i];
    const bool discardBranch = i == 1 ? discardThen : discardElse;
    if (discardBranch && !isUnitResultValue(branch)) {
      branch = nir::unitValue(branch.span);
      ++changedValues;
    } else if (!discardBranch) {
      changedValues += discardUnusedAllocationEffects(
          branch, definitions, parentMap, constructorFields, fieldSnapshots);
      if (!isUnitResultValue(branch)) {
        const support::SourceSpan span = branch.span;
        branch = effectOnlyUnitValue(std::move(branch), span);
        ++changedValues;
      }
    }
  }
  if (value.type != "Unit") {
    value.type = "Unit";
    ++changedValues;
  }
  return changedValues;
}

std::size_t removeDeadPureTopLevelInstructions(nir::FunctionBody& body) {
  std::size_t changedValues = 0;
  bool removed = true;
  while (removed) {
    removed = false;
    const std::unordered_map<std::string, std::size_t> uses = localUses(body);
    auto write = body.instructions.begin();
    for (auto read = body.instructions.begin(); read != body.instructions.end();
         ++read) {
      if (read->kind == nir::InstructionKind::Eval) {
        const std::size_t discarded = discardUnusedValueResult(read->value);
        changedValues += discarded;
        removed = removed || discarded != 0;
      }
      const bool unusedBinding = (read->kind == nir::InstructionKind::Let ||
                                  read->kind == nir::InstructionKind::Var) &&
                                 !uses.contains(read->name);
      const bool deadLet = read->kind == nir::InstructionKind::Let && unusedBinding &&
                           isPureLetValue(read->value);
      const bool deadVar = read->kind == nir::InstructionKind::Var && unusedBinding &&
                           isPureLetValue(read->value);
      const bool deadEval = read->kind == nir::InstructionKind::Eval &&
                            isTriviallyDiscardableValue(read->value);
      if (deadLet || deadVar || deadEval) {
        ++changedValues;
        removed = true;
        continue;
      }
      if (unusedBinding) {
        read->kind = nir::InstructionKind::Eval;
        read->name.clear();
        read->type.clear();
        ++changedValues;
        removed = true;
      }
      if (write != read) {
        *write = std::move(*read);
      }
      ++write;
    }
    body.instructions.erase(write, body.instructions.end());
  }
  return changedValues;
}

bool isRemovableExactArrayUpdateEval(const nir::FunctionBody& body, std::size_t index,
                                     const DefinitionIndex& definitions,
                                     const ParentMap& parentMap,
                                     const ConstructorFieldMap& constructorFields,
                                     const FieldSnapshotMap& fieldSnapshots) {
  if (index >= body.instructions.size()) {
    return false;
  }
  const nir::Instruction& instruction = body.instructions[index];
  if (instruction.kind != nir::InstructionKind::Eval ||
      instruction.value.kind != nir::ValueKind::Call ||
      instruction.value.operands.size() != 4 ||
      instruction.value.operands.front().kind != nir::ValueKind::Local ||
      instruction.value.operands[1].kind != nir::ValueKind::Local) {
    return false;
  }

  const std::string& target = instruction.value.operands.front().text;
  if (!isRuntimeArrayUpdateTarget(target)) {
    return false;
  }

  const LocalAnalysisState state = localStateBeforeInstruction(
      body, index, definitions, parentMap, constructorFields, fieldSnapshots);
  const std::optional<std::string> root =
      exactLocalArrayRootName(instruction.value.operands[1], state.values);
  if (!root || *root != instruction.value.operands[1].text) {
    return false;
  }

  const std::unordered_map<std::string, std::size_t> usesAfter =
      localUsesAfterInstruction(body, index);
  if (usesAfter.contains(*root)) {
    return false;
  }

  auto array = state.values.find(*root);
  if (array == state.values.end() ||
      !isUnusedAllocationDiscardable(array->second, definitions, parentMap,
                                     constructorFields, fieldSnapshots)) {
    return false;
  }
  const std::optional<std::string> elementType =
      arrayElementTypeName(array->second.type);
  if (!elementType || array->second.text != array->second.type ||
      !isRuntimeArrayUpdateHelper(target, *elementType)) {
    return false;
  }

  const nir::Value* exactIndex =
      exactImmutableValue(instruction.value.operands[2], state.values);
  if (exactIndex == nullptr || exactIndex->type != "Int") {
    return false;
  }
  const std::optional<long long> offset = integerLiteralValue(*exactIndex);
  if (!offset || *offset < 0 ||
      static_cast<std::size_t>(*offset) >= array->second.operands.size()) {
    return false;
  }

  const nir::Value* exactAssignedValue =
      exactImmutableValue(instruction.value.operands[3], state.values);
  return exactAssignedValue != nullptr && isPureLetValue(*exactAssignedValue);
}

bool hasExactLocalObjectAlias(
    std::string_view root,
    const std::unordered_map<std::string, nir::Value>& localValues) {
  const std::string rootName(root);
  for (const auto& [name, value] : localValues) {
    (void)value;
    if (name == rootName) {
      continue;
    }
    const std::optional<std::string> aliasRoot =
        exactLocalObjectRootName(nir::localValue(name), localValues);
    if (aliasRoot && *aliasRoot == rootName) {
      return true;
    }
  }
  return false;
}

bool isRemovableExactObjectFieldAssignEval(const nir::FunctionBody& body,
                                           std::size_t index,
                                           const DefinitionIndex& definitions,
                                           const ParentMap& parentMap,
                                           const ConstructorFieldMap& constructorFields,
                                           const FieldSnapshotMap& fieldSnapshots) {
  if (index >= body.instructions.size()) {
    return false;
  }
  const nir::Instruction& instruction = body.instructions[index];
  if (instruction.kind != nir::InstructionKind::Eval ||
      instruction.value.kind != nir::ValueKind::Assign ||
      instruction.value.operands.size() != 2) {
    return false;
  }

  const nir::Value& target = instruction.value.operands.front();
  if (target.kind != nir::ValueKind::Select || target.operands.size() != 1 ||
      target.operands.front().kind != nir::ValueKind::Local ||
      target.operands.front().text.empty()) {
    return false;
  }

  const LocalAnalysisState state = localStateBeforeInstruction(
      body, index, definitions, parentMap, constructorFields, fieldSnapshots);
  const std::optional<std::string> root =
      exactLocalObjectRootName(target.operands.front(), state.values);
  if (!root || *root != target.operands.front().text ||
      hasExactLocalObjectAlias(*root, state.values)) {
    return false;
  }

  const std::unordered_map<std::string, std::size_t> usesAfter =
      localUsesAfterInstruction(body, index);
  if (usesAfter.contains(*root)) {
    return false;
  }

  auto object = state.values.find(*root);
  if (object == state.values.end() ||
      !isUnusedAllocationDiscardable(object->second, definitions, parentMap,
                                     constructorFields, fieldSnapshots)) {
    return false;
  }

  const nir::Value* exactAssignedValue =
      exactImmutableValue(instruction.value.operands.back(), state.values);
  return exactAssignedValue != nullptr && isPureLetValue(*exactAssignedValue);
}

bool propagateValue(nir::Value& value,
                    const std::unordered_map<std::string, nir::Value>& constants,
                    const std::unordered_set<std::string>& immutableLocals) {
  if (value.kind == nir::ValueKind::Local) {
    auto constant = constants.find(value.text);
    if (constant != constants.end()) {
      value = constant->second;
      return true;
    }
    return false;
  }

  if (value.kind == nir::ValueKind::Block) {
    bool changed = false;
    std::unordered_map<std::string, nir::Value> scopedConstants = constants;
    std::unordered_set<std::string> scopedImmutableLocals = immutableLocals;
    for (nir::Value& operand : value.operands) {
      if (operand.kind == nir::ValueKind::LocalLet ||
          operand.kind == nir::ValueKind::LocalVar) {
        if (!operand.operands.empty()) {
          changed = propagateValue(operand.operands.front(), scopedConstants,
                                   scopedImmutableLocals) ||
                    changed;
        }
        if (operand.kind == nir::ValueKind::LocalLet && !operand.text.empty() &&
            operand.operands.size() == 1 &&
            isPropagatableValue(operand.operands.front(), scopedImmutableLocals) &&
            !isTypedNullReferenceValue(operand.operands.front(), operand.type)) {
          scopedConstants[operand.text] = operand.operands.front();
        } else {
          scopedConstants.erase(operand.text);
        }
        if (operand.kind == nir::ValueKind::LocalLet && !operand.text.empty()) {
          scopedImmutableLocals.insert(operand.text);
        } else {
          scopedImmutableLocals.erase(operand.text);
        }
        continue;
      }
      changed =
          propagateValue(operand, scopedConstants, scopedImmutableLocals) || changed;
    }
    return changed;
  }

  bool changed = false;
  for (nir::Value& operand : value.operands) {
    changed = propagateValue(operand, constants, immutableLocals) || changed;
  }
  return changed;
}

PassEffect propagateLocalConstants(linker::LinkedProgram& program) {
  PassEffect effect;
  for (nir::Module& module : program.modules) {
    for (nir::Definition& definition : module.definitions) {
      if (definition.kind != nir::DefinitionKind::FunctionDef) {
        continue;
      }

      std::unordered_map<std::string, nir::Value> constants;
      std::unordered_set<std::string> immutableLocals;
      for (nir::Instruction& instruction : definition.body.instructions) {
        switch (instruction.kind) {
        case nir::InstructionKind::Param:
          constants.erase(instruction.name);
          if (!instruction.name.empty()) {
            immutableLocals.insert(instruction.name);
          }
          break;
        case nir::InstructionKind::Let:
          if (propagateValue(instruction.value, constants, immutableLocals)) {
            ++effect.changedValues;
          }
          if (isPropagatableValue(instruction.value, immutableLocals) &&
              !isTypedNullReferenceValue(instruction.value, instruction.type)) {
            constants[instruction.name] = instruction.value;
          } else {
            constants.erase(instruction.name);
          }
          if (!instruction.name.empty()) {
            immutableLocals.insert(instruction.name);
          }
          break;
        case nir::InstructionKind::Var:
          if (propagateValue(instruction.value, constants, immutableLocals)) {
            ++effect.changedValues;
          }
          constants.erase(instruction.name);
          immutableLocals.erase(instruction.name);
          break;
        case nir::InstructionKind::Eval:
        case nir::InstructionKind::Return:
        case nir::InstructionKind::Throw:
          if (propagateValue(instruction.value, constants, immutableLocals)) {
            ++effect.changedValues;
          }
          break;
        case nir::InstructionKind::Unreachable:
          break;
        }
      }
    }
  }
  return effect;
}

PassEffect eliminateDeadLocalLets(linker::LinkedProgram& program) {
  PassEffect effect;
  const DefinitionIndex definitions = definitionIndexFor(program);
  const ParentMap parentMap = parentMapFor(definitions);
  const ConstructorFieldMap constructorFields = constructorFieldMapFor(program);
  const FieldSnapshotMap fieldSnapshots =
      fieldSnapshotMapFor(definitions, parentMap, constructorFields);
  for (nir::Module& module : program.modules) {
    for (nir::Definition& definition : module.definitions) {
      if (definition.kind != nir::DefinitionKind::FunctionDef) {
        continue;
      }

      for (nir::Instruction& instruction : definition.body.instructions) {
        if (instruction.kind == nir::InstructionKind::Param ||
            instruction.kind == nir::InstructionKind::Unreachable) {
          continue;
        }
        effect.changedValues +=
            eliminateDeadLocalLetsInValue(instruction.value, definitions, parentMap,
                                          constructorFields, fieldSnapshots);
      }

      bool removed = true;
      while (removed) {
        removed = false;
        const std::unordered_map<std::string, std::size_t> uses =
            localUses(definition.body);
        auto& instructions = definition.body.instructions;
        auto write = instructions.begin();
        std::size_t readIndex = 0;
        for (auto read = instructions.begin(); read != instructions.end();
             ++read, ++readIndex) {
          if (read->kind == nir::InstructionKind::Eval) {
            const std::size_t discarded = discardUnusedAllocationEffects(
                read->value, definitions, parentMap, constructorFields, fieldSnapshots);
            effect.changedValues += discarded;
            removed = removed || discarded != 0;
          }
          const bool unusedBinding = (read->kind == nir::InstructionKind::Let ||
                                      read->kind == nir::InstructionKind::Var) &&
                                     !uses.contains(read->name);
          const bool removableBindingValue =
              unusedBinding &&
              isUnusedValueDiscardable(read->value, definitions, parentMap,
                                       constructorFields, fieldSnapshots);
          const bool deadLet =
              read->kind == nir::InstructionKind::Let && removableBindingValue;
          const bool deadVar =
              read->kind == nir::InstructionKind::Var && removableBindingValue;
          const bool deadEval =
              read->kind == nir::InstructionKind::Eval &&
              isUnusedValueDiscardable(read->value, definitions, parentMap,
                                       constructorFields, fieldSnapshots);
          const bool deadExactArrayUpdate = isRemovableExactArrayUpdateEval(
              definition.body, readIndex, definitions, parentMap, constructorFields,
              fieldSnapshots);
          const bool deadExactObjectFieldAssign = isRemovableExactObjectFieldAssignEval(
              definition.body, readIndex, definitions, parentMap, constructorFields,
              fieldSnapshots);
          if (deadLet || deadVar || deadEval || deadExactArrayUpdate ||
              deadExactObjectFieldAssign) {
            ++effect.changedValues;
            removed = true;
            continue;
          }
          if (unusedBinding) {
            read->kind = nir::InstructionKind::Eval;
            read->name.clear();
            read->type.clear();
            ++effect.changedValues;
            removed = true;
          }
          if (write != read) {
            *write = std::move(*read);
          }
          ++write;
        }
        instructions.erase(write, instructions.end());
      }
    }
  }
  return effect;
}

PassEffect simplifyBlocks(linker::LinkedProgram& program) {
  PassEffect effect;
  for (nir::Module& module : program.modules) {
    for (nir::Definition& definition : module.definitions) {
      if (definition.kind != nir::DefinitionKind::FunctionDef) {
        continue;
      }

      for (nir::Instruction& instruction : definition.body.instructions) {
        if (instruction.kind == nir::InstructionKind::Param ||
            instruction.kind == nir::InstructionKind::Unreachable) {
          continue;
        }
        effect.changedValues += simplifyBlocksInValue(instruction.value);
      }
      effect.changedValues += promoteUnassignedTopLevelVars(definition.body);
      effect.changedValues += inlineAdjacentSingleUseTopLevelLets(definition.body);
      effect.changedValues += inlineSingleUseLiteralLikeTopLevelLets(definition.body);
      effect.changedValues += removeDeadPureTopLevelInstructions(definition.body);
    }
  }
  return effect;
}

bool canPruneUnreachable(const nir::Definition& definition) {
  return definition.kind == nir::DefinitionKind::FunctionDecl ||
         definition.kind == nir::DefinitionKind::FunctionDef;
}

bool isDispatchOwner(nir::DefinitionKind kind) {
  return kind == nir::DefinitionKind::Class || kind == nir::DefinitionKind::Trait;
}

struct DirectReferenceContext {
  std::string moduleName;
  std::string ownerName;
  const std::unordered_map<std::string, const nir::Definition*>* definitions = nullptr;
  std::unordered_set<std::string> localNames;
};

std::unordered_map<std::string, std::string>
moduleIndexFor(const linker::LinkedProgram& program) {
  std::unordered_map<std::string, std::string> modules;
  for (const nir::Module& module : program.modules) {
    for (const nir::Definition& definition : module.definitions) {
      if (!definition.name.empty()) {
        modules.emplace(definition.name, module.name);
      }
    }
  }
  return modules;
}

std::optional<std::string>
resolveDirectReference(const std::string& reference,
                       const DirectReferenceContext& context) {
  if (context.definitions == nullptr || reference.empty()) {
    return std::nullopt;
  }

  auto direct = context.definitions->find(reference);
  if (direct != context.definitions->end()) {
    return direct->first;
  }
  if (!context.ownerName.empty()) {
    const std::string ownerRelative = context.ownerName + "." + reference;
    auto ownerLocal = context.definitions->find(ownerRelative);
    if (ownerLocal != context.definitions->end()) {
      return ownerLocal->first;
    }
  }
  if (!context.moduleName.empty()) {
    const std::string moduleRelative = context.moduleName + "." + reference;
    auto moduleLocal = context.definitions->find(moduleRelative);
    if (moduleLocal != context.definitions->end()) {
      return moduleLocal->first;
    }
  }
  return std::nullopt;
}

bool isDispatchOwnedFunction(
    const nir::Definition& definition,
    const std::unordered_map<std::string, const nir::Definition*>& definitions) {
  if (!canPruneUnreachable(definition)) {
    return false;
  }

  const std::string owner = ownerNameOf(definition.name);
  auto ownerDefinition = definitions.find(owner);
  return ownerDefinition != definitions.end() &&
         isDispatchOwner(ownerDefinition->second->kind);
}

bool shouldRetainOriginalReachableDefinition(
    const nir::Definition& definition,
    const std::unordered_map<std::string, const nir::Definition*>& definitions) {
  if (!canPruneUnreachable(definition)) {
    return true;
  }
  if (isDispatchOwnedFunction(definition, definitions)) {
    return true;
  }
  if (definition.kind == nir::DefinitionKind::FunctionDecl) {
    return false;
  }
  return !inlineableReturnSummary(definition);
}

void collectDirectValueReferences(const nir::Value& value,
                                  DirectReferenceContext& context,
                                  std::vector<std::string>& references);

void collectDirectBindingReference(const nir::Value& binding,
                                   DirectReferenceContext& context,
                                   std::vector<std::string>& references) {
  if (!binding.operands.empty()) {
    collectDirectValueReferences(binding.operands.front(), context, references);
  }
  if (!binding.text.empty()) {
    context.localNames.insert(binding.text);
  }
}

void collectDirectValueReferences(const nir::Value& value,
                                  DirectReferenceContext& context,
                                  std::vector<std::string>& references) {
  switch (value.kind) {
  case nir::ValueKind::Local:
    if (!context.localNames.contains(value.text)) {
      if (std::optional<std::string> target =
              resolveDirectReference(value.text, context)) {
        references.push_back(*target);
      }
    }
    return;
  case nir::ValueKind::Call:
    if (!value.operands.empty()) {
      if (value.operands.front().kind == nir::ValueKind::Local &&
          !context.localNames.contains(value.operands.front().text)) {
        if (std::optional<std::string> target =
                resolveDirectReference(value.operands.front().text, context)) {
          references.push_back(*target);
        }
      } else {
        collectDirectValueReferences(value.operands.front(), context, references);
      }
      for (std::size_t i = 1; i < value.operands.size(); ++i) {
        collectDirectValueReferences(value.operands[i], context, references);
      }
    }
    return;
  case nir::ValueKind::Block: {
    DirectReferenceContext blockContext = context;
    for (const nir::Value& operand : value.operands) {
      if (operand.kind == nir::ValueKind::LocalLet ||
          operand.kind == nir::ValueKind::LocalVar) {
        collectDirectBindingReference(operand, blockContext, references);
        continue;
      }
      collectDirectValueReferences(operand, blockContext, references);
    }
    return;
  }
  case nir::ValueKind::LocalLet:
  case nir::ValueKind::LocalVar:
    collectDirectBindingReference(value, context, references);
    return;
  case nir::ValueKind::Catch: {
    if (value.operands.size() != 2 ||
        value.operands.front().kind != nir::ValueKind::Local) {
      for (const nir::Value& operand : value.operands) {
        collectDirectValueReferences(operand, context, references);
      }
      return;
    }
    DirectReferenceContext handlerContext = context;
    handlerContext.localNames.insert(value.operands.front().text);
    collectDirectValueReferences(value.operands.back(), handlerContext, references);
    return;
  }
  case nir::ValueKind::Select:
    if (value.operands.size() == 1 &&
        value.operands.front().kind == nir::ValueKind::Local &&
        !context.localNames.contains(value.operands.front().text)) {
      if (std::optional<std::string> receiver =
              resolveDirectReference(value.operands.front().text, context)) {
        const std::string selected = *receiver + "." + value.text;
        if (context.definitions != nullptr && context.definitions->contains(selected)) {
          references.push_back(selected);
        }
      }
    }
    for (const nir::Value& operand : value.operands) {
      collectDirectValueReferences(operand, context, references);
    }
    return;
  case nir::ValueKind::New:
  case nir::ValueKind::Assign:
  case nir::ValueKind::Throw:
  case nir::ValueKind::Try:
  case nir::ValueKind::Finally:
  case nir::ValueKind::Unary:
  case nir::ValueKind::Binary:
  case nir::ValueKind::If:
  case nir::ValueKind::While:
  case nir::ValueKind::SizeOf:
  case nir::ValueKind::ZoneScoped:
  case nir::ValueKind::Box:
  case nir::ValueKind::Unbox:
  case nir::ValueKind::IsInstanceOf:
  case nir::ValueKind::AsInstanceOf:
  case nir::ValueKind::Super:
    for (const nir::Value& operand : value.operands) {
      collectDirectValueReferences(operand, context, references);
    }
    return;
  case nir::ValueKind::Unit:
  case nir::ValueKind::Literal:
  case nir::ValueKind::Unknown:
    return;
  }
}

std::vector<std::string> collectDirectDefinitionReferences(
    const nir::Definition& definition, const std::string& moduleName,
    const std::unordered_map<std::string, const nir::Definition*>& definitions) {
  std::vector<std::string> references;
  if (definition.kind != nir::DefinitionKind::FunctionDef &&
      !(definition.kind == nir::DefinitionKind::Field && !definition.body.empty())) {
    return references;
  }

  DirectReferenceContext context;
  context.moduleName = moduleName;
  context.ownerName = ownerNameOf(definition.name);
  context.definitions = &definitions;
  for (const nir::Instruction& instruction : definition.body.instructions) {
    if (instruction.kind == nir::InstructionKind::Param) {
      if (!instruction.name.empty()) {
        context.localNames.insert(instruction.name);
      }
      continue;
    }
    if (instruction.kind == nir::InstructionKind::Let ||
        instruction.kind == nir::InstructionKind::Var) {
      collectDirectValueReferences(instruction.value, context, references);
      if (!instruction.name.empty()) {
        context.localNames.insert(instruction.name);
      }
      continue;
    }
    if (instruction.kind != nir::InstructionKind::Unreachable) {
      collectDirectValueReferences(instruction.value, context, references);
    }
  }
  return references;
}

std::unordered_set<std::string>
recomputedReachableAfterInlining(const linker::LinkedProgram& program) {
  const std::unordered_map<std::string, const nir::Definition*> definitions =
      definitionIndexFor(program);
  const std::unordered_map<std::string, std::string> modules = moduleIndexFor(program);
  std::unordered_set<std::string> reachable;
  std::vector<std::string> worklist;

  auto addReachable = [&](const std::string& name) {
    if (definitions.contains(name) && reachable.insert(name).second) {
      worklist.push_back(name);
    }
  };

  auto drainWorklist = [&] {
    while (!worklist.empty()) {
      std::string name = std::move(worklist.back());
      worklist.pop_back();
      auto definition = definitions.find(name);
      if (definition == definitions.end()) {
        continue;
      }
      std::string moduleName;
      auto module = modules.find(name);
      if (module != modules.end()) {
        moduleName = module->second;
      }
      for (const std::string& reference : collectDirectDefinitionReferences(
               *definition->second, moduleName, definitions)) {
        addReachable(reference);
      }
    }
  };

  for (const std::string& root : program.roots) {
    addReachable(root);
  }
  drainWorklist();

  for (const std::string& name : program.reachableGlobals) {
    auto definition = definitions.find(name);
    if (definition == definitions.end()) {
      continue;
    }
    if (shouldRetainOriginalReachableDefinition(*definition->second, definitions)) {
      addReachable(name);
    }
  }
  drainWorklist();

  return reachable;
}

PassEffect pruneUnreachableFunctions(linker::LinkedProgram& program) {
  PassEffect effect;
  if (program.reachableGlobals.empty()) {
    return effect;
  }

  const std::unordered_set<std::string> reachable =
      recomputedReachableAfterInlining(program);
  for (nir::Module& module : program.modules) {
    const std::size_t before = module.definitions.size();
    module.definitions.erase(
        std::remove_if(module.definitions.begin(), module.definitions.end(),
                       [&](const nir::Definition& definition) {
                         return canPruneUnreachable(definition) &&
                                !reachable.contains(definition.name);
                       }),
        module.definitions.end());
    effect.removedDefinitions += before - module.definitions.size();
  }

  program.reachableGlobals.assign(reachable.begin(), reachable.end());
  std::sort(program.reachableGlobals.begin(), program.reachableGlobals.end());
  return effect;
}

template <typename Fn>
bool runPass(linker::LinkedProgram& program, std::string name,
             std::vector<PassReport>& reports, InterflowResult& result, Fn&& pass) {
  PassReport report;
  report.name = std::move(name);
  report.definitionsBefore = definitionCount(program);

  std::vector<std::string> beforeErrors = validateProgram(program);
  report.validationErrorsBefore = beforeErrors.size();
  if (!beforeErrors.empty()) {
    result.ok = false;
    result.errors = std::move(beforeErrors);
    report.definitionsAfter = report.definitionsBefore;
    reports.push_back(std::move(report));
    return false;
  }

  auto start = std::chrono::steady_clock::now();
  PassEffect effect = pass(program);
  auto end = std::chrono::steady_clock::now();

  report.durationMicros = static_cast<std::size_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
  report.removedDefinitions = effect.removedDefinitions;
  report.changedValues = effect.changedValues;
  report.definitionsAfter = definitionCount(program);

  std::vector<std::string> afterErrors = validateProgram(program);
  report.validationErrorsAfter = afterErrors.size();
  if (!afterErrors.empty()) {
    result.ok = false;
    result.errors = std::move(afterErrors);
    reports.push_back(std::move(report));
    return false;
  }

  result.removedDefinitions += effect.removedDefinitions;
  result.changedValues += effect.changedValues;
  reports.push_back(std::move(report));
  return true;
}

} // namespace

InterflowResult InterflowOptimizer::optimize(linker::LinkedProgram program,
                                             const InterflowOptions& options) const {
  InterflowResult result;
  result.program = std::move(program);

  if (!runPass(result.program, "propagate-local-constants", result.reports, result,
               propagateLocalConstants)) {
    return result;
  }
  if (!runPass(result.program, "fold-constants", result.reports, result,
               foldConstants)) {
    return result;
  }
  if (!runPass(result.program, "eliminate-dead-local-lets", result.reports, result,
               eliminateDeadLocalLets)) {
    return result;
  }
  if (!runPass(result.program, "simplify-blocks", result.reports, result,
               simplifyBlocks)) {
    return result;
  }
  if (options.tier == OptimizationTier::Basic) {
    (void)runPass(result.program, "prune-unreachable-functions", result.reports, result,
                  pruneUnreachableFunctions);
    return result;
  }
  if (!runPass(result.program, "fold-cleaned-constants", result.reports, result,
               foldConstants)) {
    return result;
  }
  if (!runPass(result.program, "eliminate-cleaned-dead-local-lets", result.reports,
               result, eliminateDeadLocalLets)) {
    return result;
  }
  if (!runPass(result.program, "simplify-cleaned-blocks", result.reports, result,
               simplifyBlocks)) {
    return result;
  }
  if (options.tier == OptimizationTier::Aggressive) {
    if (!runPass(result.program, "propagate-aggressive-local-constants", result.reports,
                 result, propagateLocalConstants)) {
      return result;
    }
    if (!runPass(result.program, "fold-aggressive-constants", result.reports, result,
                 foldConstants)) {
      return result;
    }
    if (!runPass(result.program, "eliminate-aggressive-dead-local-lets", result.reports,
                 result, eliminateDeadLocalLets)) {
      return result;
    }
    if (!runPass(result.program, "simplify-aggressive-blocks", result.reports, result,
                 simplifyBlocks)) {
      return result;
    }
  }
  (void)runPass(result.program, "prune-unreachable-functions", result.reports, result,
                pruneUnreachableFunctions);
  return result;
}

} // namespace scalanative::tools::interflow
