#include "scalanative/frontend/Typechecker.h"

#include "scalanative/support/SourceManager.h"
#include "scalanative/support/StdNames.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_set>

namespace scalanative::frontend {

namespace {

constexpr std::string_view SummonName = "summon";
constexpr std::string_view ImplicitlyName = "implicitly";
constexpr std::size_t MaxInlineExpansionDepth = 32;

bool isIntegerConstantType(SimpleTypeKind kind) {
  return kind == SimpleTypeKind::Byte || kind == SimpleTypeKind::Short ||
         kind == SimpleTypeKind::Int || kind == SimpleTypeKind::Long;
}

bool isFloatingConstantType(SimpleTypeKind kind) {
  return kind == SimpleTypeKind::Float || kind == SimpleTypeKind::Double;
}

SimpleTypeKind floatingConstantType(std::string_view text) {
  return !text.empty() && (text.back() == 'f' || text.back() == 'F')
             ? SimpleTypeKind::Float
             : SimpleTypeKind::Double;
}

bool hasFloatingConstantSyntax(std::string_view text) {
  return text.find('.') != std::string_view::npos ||
         text.find('e') != std::string_view::npos ||
         text.find('E') != std::string_view::npos ||
         (!text.empty() &&
          (text.back() == 'f' || text.back() == 'F' || text.back() == 'd' ||
           text.back() == 'D'));
}

std::optional<double> parseFloatingConstant(std::string_view text,
                                            SimpleTypeKind kind) {
  if (!isFloatingConstantType(kind)) {
    return std::nullopt;
  }
  std::string normalized;
  normalized.reserve(text.size());
  for (char ch : text) {
    if (ch != '_') {
      normalized.push_back(ch);
    }
  }
  if (!normalized.empty() &&
      (normalized.back() == 'f' || normalized.back() == 'F' ||
       normalized.back() == 'd' || normalized.back() == 'D')) {
    normalized.pop_back();
  }
  try {
    std::size_t parsed = 0;
    const double value = std::stod(normalized, &parsed);
    if (parsed != normalized.size() || !std::isfinite(value)) {
      return std::nullopt;
    }
    if (kind == SimpleTypeKind::Float) {
      const float narrowed = static_cast<float>(value);
      return std::isfinite(narrowed)
                 ? std::optional<double>{static_cast<double>(narrowed)}
                 : std::nullopt;
    }
    return value;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::int64_t> parseIntegerConstant(std::string_view text) {
  std::string normalized;
  normalized.reserve(text.size());
  for (char ch : text) {
    if (ch != '_') {
      normalized.push_back(ch);
    }
  }
  if (!normalized.empty() &&
      (normalized.back() == 'l' || normalized.back() == 'L')) {
    normalized.pop_back();
  }

  int base = 10;
  std::size_t firstDigit = 0;
  if (normalized.size() > 2 && normalized[0] == '0' &&
      (normalized[1] == 'x' || normalized[1] == 'X')) {
    base = 16;
    firstDigit = 2;
  }
  if (firstDigit == normalized.size()) {
    return std::nullopt;
  }

  std::int64_t value = 0;
  const char* begin = normalized.data() + firstDigit;
  const char* end = normalized.data() + normalized.size();
  const auto [parsed, error] = std::from_chars(begin, end, value, base);
  return error == std::errc{} && parsed == end
             ? std::optional<std::int64_t>{value}
             : std::nullopt;
}

std::optional<std::int64_t> checkedIntegerAdd(std::int64_t left,
                                              std::int64_t right) {
  constexpr std::int64_t minimum = std::numeric_limits<std::int64_t>::min();
  constexpr std::int64_t maximum = std::numeric_limits<std::int64_t>::max();
  if ((right > 0 && left > maximum - right) ||
      (right < 0 && left < minimum - right)) {
    return std::nullopt;
  }
  return left + right;
}

std::optional<std::int64_t> checkedIntegerSubtract(std::int64_t left,
                                                   std::int64_t right) {
  constexpr std::int64_t minimum = std::numeric_limits<std::int64_t>::min();
  constexpr std::int64_t maximum = std::numeric_limits<std::int64_t>::max();
  if ((right < 0 && left > maximum + right) ||
      (right > 0 && left < minimum + right)) {
    return std::nullopt;
  }
  return left - right;
}

std::optional<std::int64_t> checkedIntegerMultiply(std::int64_t left,
                                                   std::int64_t right) {
  constexpr std::uint64_t positiveLimit =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  constexpr std::uint64_t negativeLimit = positiveLimit + 1;
  const auto magnitude = [](std::int64_t value) {
    return value < 0
               ? static_cast<std::uint64_t>(-(value + 1)) + std::uint64_t{1}
               : static_cast<std::uint64_t>(value);
  };

  const bool negative = (left < 0) != (right < 0);
  const std::uint64_t leftMagnitude = magnitude(left);
  const std::uint64_t rightMagnitude = magnitude(right);
  const std::uint64_t limit = negative ? negativeLimit : positiveLimit;
  if (rightMagnitude != 0 && leftMagnitude > limit / rightMagnitude) {
    return std::nullopt;
  }

  const std::uint64_t resultMagnitude = leftMagnitude * rightMagnitude;
  if (!negative) {
    return static_cast<std::int64_t>(resultMagnitude);
  }
  if (resultMagnitude == negativeLimit) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return -static_cast<std::int64_t>(resultMagnitude);
}

bool isClassLikeDeclaration(AstDeclarationKind kind) {
  return kind == AstDeclarationKind::Object || kind == AstDeclarationKind::Class ||
         kind == AstDeclarationKind::Trait;
}

bool isInheritableDeclaration(AstDeclarationKind kind) {
  return kind == AstDeclarationKind::Class || kind == AstDeclarationKind::Trait;
}

std::string derivedInstanceName(std::string_view typeclassSymbolName,
                                bool derivingObject) {
  std::string name = "$derived$";
  name.reserve(name.size() + typeclassSymbolName.size());
  for (char ch : typeclassSymbolName) {
    const bool alphaNumeric = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                              (ch >= '0' && ch <= '9');
    name.push_back(alphaNumeric || ch == '_' ? ch : '$');
  }
  name += derivingObject ? "$object" : "$type";
  return name;
}

std::string productMirrorImplementationName(std::string_view targetSymbolName) {
  std::string name = "$mirror$Product$";
  name.reserve(name.size() + targetSymbolName.size());
  for (char ch : targetSymbolName) {
    const bool alphaNumeric = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                              (ch >= '0' && ch <= '9');
    name.push_back(alphaNumeric || ch == '_' ? ch : '$');
  }
  return name;
}

std::string sumMirrorImplementationName(std::string_view targetSymbolName) {
  std::string name = "$mirror$Sum$";
  name.reserve(name.size() + targetSymbolName.size());
  for (char ch : targetSymbolName) {
    const bool alphaNumeric = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                              (ch >= '0' && ch <= '9');
    name.push_back(alphaNumeric || ch == '_' ? ch : '$');
  }
  return name;
}

std::string stringSingletonType(std::string_view value) {
  std::string literal{"\""};
  literal.reserve(value.size() + 2);
  for (char ch : value) {
    if (ch == '\n') {
      literal += "\\n";
      continue;
    }
    if (ch == '\r') {
      literal += "\\r";
      continue;
    }
    if (ch == '\t') {
      literal += "\\t";
      continue;
    }
    if (ch == '"' || ch == '\\') {
      literal.push_back('\\');
    }
    literal.push_back(ch);
  }
  literal.push_back('"');
  return literal;
}

std::string decodeStringLiteral(std::string_view text) {
  if (text.starts_with("\"\"\"") && text.ends_with("\"\"\"") &&
      text.size() >= 6) {
    return std::string(text.substr(3, text.size() - 6));
  }
  if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
    return std::string(text);
  }

  std::string decoded;
  for (std::size_t i = 1; i + 1 < text.size(); ++i) {
    const char ch = text[i];
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

std::optional<std::uint32_t> decodeCharLiteral(std::string_view text) {
  if (text.size() < 3 || text.front() != '\'' || text.back() != '\'') {
    return std::nullopt;
  }
  text.remove_prefix(1);
  text.remove_suffix(1);
  if (text.size() == 1) {
    return static_cast<unsigned char>(text.front());
  }
  if (text.size() != 2 || text.front() != '\\') {
    return std::nullopt;
  }
  switch (text.back()) {
  case 'b':
    return '\b';
  case 't':
    return '\t';
  case 'n':
    return '\n';
  case 'f':
    return '\f';
  case 'r':
    return '\r';
  case '"':
    return '"';
  case '\'':
    return '\'';
  case '\\':
    return '\\';
  default:
    return std::nullopt;
  }
}

std::string tupleTypeName(const std::vector<std::string>& elements) {
  if (elements.empty()) {
    return "scala.EmptyTuple";
  }
  std::string type = "scala.Tuple" + std::to_string(elements.size()) + "[";
  for (std::size_t i = 0; i < elements.size(); ++i) {
    if (i != 0) {
      type += ",";
    }
    type += elements[i];
  }
  type += "]";
  return type;
}

bool typesMatchForOverride(const TypeInfo& expected, const TypeInfo& actual) {
  if (expected.kind == SimpleTypeKind::Unknown ||
      actual.kind == SimpleTypeKind::Unknown) {
    return true;
  }
  return expected.name == actual.name;
}

TypeInfo staticExpressionType(TypeInfo type) {
  if (!type.typeParameter && !type.abstractTypeMember &&
      type.compositeKind == CompositeTypeKind::None &&
      type.typeConstructorName.empty() && type.singletonLiteral.empty() &&
      !type.runtimeName.empty() &&
      type.runtimeName != type.name) {
    type.runtimeName.clear();
  }
  return type;
}

bool declarationHasImplementation(AstDeclarationKind kind, bool hasInitializer) {
  return kind != AstDeclarationKind::Def && kind != AstDeclarationKind::Val &&
                 kind != AstDeclarationKind::Var && kind != AstDeclarationKind::Type
             ? true
             : hasInitializer;
}

bool isExplicitValParameter(const std::string& parameter) {
  return parameter.rfind("val ", 0) == 0;
}

bool isExplicitVarParameter(const std::string& parameter) {
  return parameter.rfind("var ", 0) == 0;
}

bool isValueAccessor(AstDeclarationKind kind) {
  return kind == AstDeclarationKind::Val || kind == AstDeclarationKind::Var;
}

bool isReferenceType(const TypeInfo& type) {
  return type.kind == SimpleTypeKind::Object || type.kind == SimpleTypeKind::String;
}

bool isReferenceOrNullType(const TypeInfo& type) {
  return isReferenceType(type) || type.kind == SimpleTypeKind::Null;
}

bool isBoxablePrimitiveType(SimpleTypeKind kind) {
  return kind == SimpleTypeKind::Unit || kind == SimpleTypeKind::Boolean ||
         kind == SimpleTypeKind::Byte || kind == SimpleTypeKind::Short ||
         kind == SimpleTypeKind::Int || kind == SimpleTypeKind::Long ||
         kind == SimpleTypeKind::Float || kind == SimpleTypeKind::Double ||
         kind == SimpleTypeKind::Char || kind == SimpleTypeKind::Symbol ||
         kind == SimpleTypeKind::String;
}

bool isCompilerKnownEqualsReceiver(const TypeInfo& type) {
  return type.kind == SimpleTypeKind::Unit || type.kind == SimpleTypeKind::String ||
         type.kind == SimpleTypeKind::Boolean || type.kind == SimpleTypeKind::Byte ||
         type.kind == SimpleTypeKind::Short || type.kind == SimpleTypeKind::Int ||
         type.kind == SimpleTypeKind::Long || type.kind == SimpleTypeKind::Float ||
         type.kind == SimpleTypeKind::Double || type.kind == SimpleTypeKind::Char ||
         type.kind == SimpleTypeKind::Symbol || type.kind == SimpleTypeKind::Object ||
         type.kind == SimpleTypeKind::Unknown;
}

bool isCompilerKnownEqualsArgumentCompatible(const TypeInfo& receiver,
                                             const TypeInfo& argument) {
  if (receiver.kind == SimpleTypeKind::Unknown ||
      argument.kind == SimpleTypeKind::Unknown) {
    return true;
  }
  if (receiver.kind == SimpleTypeKind::Unit) {
    return argument.kind == SimpleTypeKind::Unit ||
           argument.kind == SimpleTypeKind::Null ||
           argument.kind == SimpleTypeKind::Object ||
           isBoxablePrimitiveType(argument.kind);
  }
  if (receiver.kind == argument.kind) {
    return receiver.kind != SimpleTypeKind::Unit &&
           receiver.kind != SimpleTypeKind::Null;
  }
  if (receiver.kind == SimpleTypeKind::Object) {
    return isReferenceOrNullType(argument) || isBoxablePrimitiveType(argument.kind);
  }
  return isReferenceType(receiver) && isReferenceOrNullType(argument);
}

bool isCompilerKnownHashCodeReceiver(const TypeInfo& type) {
  return type.kind == SimpleTypeKind::Unit || type.kind == SimpleTypeKind::String ||
         type.kind == SimpleTypeKind::Boolean || type.kind == SimpleTypeKind::Byte ||
         type.kind == SimpleTypeKind::Short || type.kind == SimpleTypeKind::Int ||
         type.kind == SimpleTypeKind::Long || type.kind == SimpleTypeKind::Float ||
         type.kind == SimpleTypeKind::Double || type.kind == SimpleTypeKind::Char ||
         type.kind == SimpleTypeKind::Symbol || type.kind == SimpleTypeKind::Null ||
         type.kind == SimpleTypeKind::Object || type.kind == SimpleTypeKind::Unknown;
}

bool isZoneScopedCall(const AstExpression& expression) {
  if (expression.kind != AstExpressionKind::Call || expression.children.empty()) {
    return false;
  }
  const AstExpression& callee = expression.children.front();
  return callee.kind == AstExpressionKind::Select && callee.children.size() == 1 &&
         callee.text == support::StdNames::ZoneScoped &&
         callee.children.front().kind == AstExpressionKind::Identifier &&
         callee.children.front().text == support::StdNames::Zone;
}

bool isZoneAllocBytesCall(const AstExpression& expression) {
  if (expression.kind != AstExpressionKind::Call || expression.children.empty()) {
    return false;
  }
  const AstExpression& callee = expression.children.front();
  return callee.kind == AstExpressionKind::Select && callee.children.size() == 1 &&
         callee.text == support::StdNames::ZoneAllocBytes &&
         callee.children.front().kind == AstExpressionKind::Identifier &&
         callee.children.front().text == support::StdNames::Zone;
}

std::string_view nativeBytesOperation(const AstExpression& expression) {
  if (expression.kind != AstExpressionKind::Call || expression.children.empty()) {
    return {};
  }
  const AstExpression& callee = expression.children.front();
  if (callee.kind != AstExpressionKind::Select || callee.children.size() != 1 ||
      callee.children.front().kind != AstExpressionKind::Identifier ||
      callee.children.front().text != support::StdNames::NativeBytes) {
    return {};
  }
  if (callee.text == support::StdNames::NativeBytesGetShortBe ||
      callee.text == support::StdNames::NativeBytesGetShortLe ||
      callee.text == support::StdNames::NativeBytesPutShortBe ||
      callee.text == support::StdNames::NativeBytesPutShortLe) {
    return callee.text;
  }
  return {};
}

bool isByteBufferWrapCall(const AstExpression& expression) {
  if (expression.kind != AstExpressionKind::Call || expression.children.empty()) {
    return false;
  }
  const AstExpression& callee = expression.children.front();
  return callee.kind == AstExpressionKind::Select && callee.children.size() == 1 &&
         callee.text == support::StdNames::ByteBufferWrap &&
         callee.children.front().kind == AstExpressionKind::Identifier &&
         callee.children.front().text == support::StdNames::ByteBuffer;
}

bool isByteBufferOperationName(std::string_view operation) {
  return operation == support::StdNames::ByteBufferCapacity ||
         operation == support::StdNames::ByteBufferPosition ||
         operation == support::StdNames::ByteBufferLimit ||
         operation == support::StdNames::ByteBufferRemaining ||
         operation == support::StdNames::ByteBufferHasRemaining ||
         operation == support::StdNames::ByteBufferGet ||
         operation == support::StdNames::ByteBufferPut ||
         operation == support::StdNames::ByteBufferGetShort ||
         operation == support::StdNames::ByteBufferPutShort ||
         operation == support::StdNames::ByteBufferClear ||
         operation == support::StdNames::ByteBufferFlip ||
         operation == support::StdNames::ByteBufferRewind ||
         operation == support::StdNames::ByteBufferMark ||
         operation == support::StdNames::ByteBufferReset;
}

bool isByteBufferType(const TypeInfo& type) {
  const std::string& name = type.runtimeName.empty() ? type.name : type.runtimeName;
  return type.kind == SimpleTypeKind::Object &&
         name == support::StdNames::JavaNioByteBuffer;
}

bool canEscapeZone(SimpleTypeKind kind) {
  return kind != SimpleTypeKind::Unknown && kind != SimpleTypeKind::Unit &&
         kind != SimpleTypeKind::Boolean && kind != SimpleTypeKind::Byte &&
         kind != SimpleTypeKind::Short && kind != SimpleTypeKind::Int &&
         kind != SimpleTypeKind::Long && kind != SimpleTypeKind::Float &&
         kind != SimpleTypeKind::Double && kind != SimpleTypeKind::Char;
}

bool hasCompileTimeSize(SimpleTypeKind kind) {
  return kind == SimpleTypeKind::Unit || kind == SimpleTypeKind::Boolean ||
         kind == SimpleTypeKind::Byte || kind == SimpleTypeKind::Short ||
         kind == SimpleTypeKind::Int || kind == SimpleTypeKind::Long ||
         kind == SimpleTypeKind::Float || kind == SimpleTypeKind::Double ||
         kind == SimpleTypeKind::Char;
}

std::string compactTypeName(std::string_view typeName) {
  std::string compact;
  compact.reserve(typeName.size());
  for (char ch : typeName) {
    if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
      compact.push_back(ch);
    }
  }
  return compact;
}

struct PolymorphicFunctionTypeSyntax {
  std::string typeParameter;
  std::string parameterType;
  std::string resultType;
  bool polymorphic = false;
  bool malformed = false;
};

PolymorphicFunctionTypeSyntax
parsePolymorphicFunctionTypeSyntax(std::string_view typeName) {
  const std::string compact = compactTypeName(typeName);
  PolymorphicFunctionTypeSyntax parsed;
  if (compact.empty() || compact.front() != '[' ||
      compact.find("=>") == std::string::npos) {
    return parsed;
  }

  parsed.polymorphic = true;
  const std::size_t close = compact.find(']');
  if (close == std::string::npos || close <= 1 ||
      compact.substr(close + 1, 2) != "=>") {
    parsed.malformed = true;
    return parsed;
  }
  parsed.typeParameter = compact.substr(1, close - 1);
  const auto isIdentifier = [](std::string_view name) {
    if (name.empty()) {
      return false;
    }
    const auto isStart = [](char ch) {
      return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
    };
    const auto isPart = [&](char ch) {
      return isStart(ch) || (ch >= '0' && ch <= '9');
    };
    return isStart(name.front()) && std::all_of(name.begin() + 1, name.end(), isPart);
  };
  if (!isIdentifier(parsed.typeParameter)) {
    parsed.malformed = true;
    return parsed;
  }

  const std::size_t parameterStart = close + 3;
  const std::size_t resultArrow = compact.find("=>", parameterStart);
  if (resultArrow == std::string::npos) {
    parsed.malformed = true;
    return parsed;
  }
  parsed.parameterType = compact.substr(parameterStart, resultArrow - parameterStart);
  if (parsed.parameterType.size() >= 2 && parsed.parameterType.front() == '(' &&
      parsed.parameterType.back() == ')') {
    parsed.parameterType =
        parsed.parameterType.substr(1, parsed.parameterType.size() - 2);
  }
  parsed.resultType = compact.substr(resultArrow + 2);
  parsed.malformed =
      parsed.parameterType != parsed.typeParameter || parsed.resultType.empty();
  return parsed;
}

std::string arrayElementTypeName(std::string_view typeName) {
  const std::string compact = compactTypeName(typeName);
  constexpr std::string_view arrayPrefix = "Array[";
  constexpr std::string_view scalaArrayPrefix = "scala.Array[";
  const std::string_view prefix = compact.starts_with(arrayPrefix) ? arrayPrefix
                                  : compact.starts_with(scalaArrayPrefix)
                                      ? scalaArrayPrefix
                                      : std::string_view{};
  if (prefix.empty() || compact.size() <= prefix.size() || compact.back() != ']') {
    return {};
  }
  const std::string element =
      compact.substr(prefix.size(), compact.size() - prefix.size() - 1);
  if (const std::string nestedElement = arrayElementTypeName(element);
      !nestedElement.empty()) {
    return "Array [ " + nestedElement + " ]";
  }
  return element;
}

bool isBuiltinArrayElementKind(SimpleTypeKind kind) {
  return kind == SimpleTypeKind::String || kind == SimpleTypeKind::Byte ||
         kind == SimpleTypeKind::Short || kind == SimpleTypeKind::Int ||
         kind == SimpleTypeKind::Boolean || kind == SimpleTypeKind::Long ||
         kind == SimpleTypeKind::Double || kind == SimpleTypeKind::Float ||
         kind == SimpleTypeKind::Char;
}

bool isAnyArrayElementType(const TypeInfo& type) {
  return type.kind == SimpleTypeKind::Object && type.name == "Object";
}

bool isSupportedAnyArrayValueType(const TypeInfo& type) {
  return type.kind == SimpleTypeKind::Unit || isBuiltinArrayElementKind(type.kind) ||
         type.kind == SimpleTypeKind::Object || type.kind == SimpleTypeKind::Symbol ||
         type.kind == SimpleTypeKind::Null;
}

std::string arrayTypeName(const TypeInfo& elementType) {
  return "Array [ " + elementType.name + " ]";
}

std::string ownerNameOf(std::string_view symbolName) {
  const std::size_t separator = symbolName.rfind('.');
  return separator == std::string_view::npos
             ? std::string{}
             : std::string(symbolName.substr(0, separator));
}

std::string memberNameOf(std::string_view symbolName) {
  const std::size_t separator = symbolName.rfind('.');
  return separator == std::string_view::npos
             ? std::string(symbolName)
             : std::string(symbolName.substr(separator + 1));
}

std::string wildcardImportOwner(std::string_view importPath) {
  return importPath.ends_with("._")
             ? std::string(importPath.substr(0, importPath.size() - 2))
             : std::string{};
}

bool isDirectMemberOf(std::string_view symbolName, std::string_view owner) {
  if (owner.empty() || !symbolName.starts_with(owner) ||
      symbolName.size() <= owner.size() || symbolName[owner.size()] != '.') {
    return false;
  }
  const std::string_view member = symbolName.substr(owner.size() + 1);
  return !member.empty() && member.find('.') == std::string_view::npos;
}

const char* memberKindName(AstDeclarationKind kind) {
  switch (kind) {
  case AstDeclarationKind::Package:
    return "package";
  case AstDeclarationKind::Def:
    return "method";
  case AstDeclarationKind::Val:
    return "value";
  case AstDeclarationKind::Var:
    return "variable";
  case AstDeclarationKind::Import:
    return "import";
  case AstDeclarationKind::Object:
    return "object";
  case AstDeclarationKind::Class:
    return "class";
  case AstDeclarationKind::Trait:
    return "trait";
  case AstDeclarationKind::Type:
    return "type member";
  }
  return "member";
}

bool inheritedContractSatisfiedBy(const SymbolInfo& effective,
                                  const SymbolInfo& required) {
  if (required.kind == AstDeclarationKind::Type) {
    if (effective.kind != AstDeclarationKind::Type) {
      return false;
    }
    if (!required.hasImplementation) {
      return true;
    }
    return effective.hasImplementation &&
           typesMatchForOverride(required.type, effective.type);
  }
  if (effective.kind == AstDeclarationKind::Var &&
      required.kind == AstDeclarationKind::Val) {
    return typesMatchForOverride(required.type, effective.type);
  }
  if (effective.kind != required.kind) {
    return false;
  }
  if (isValueAccessor(effective.kind)) {
    return typesMatchForOverride(required.type, effective.type);
  }
  if (effective.kind != AstDeclarationKind::Def ||
      effective.parameterTypes.size() != required.parameterTypes.size()) {
    return false;
  }
  for (std::size_t i = 0; i < effective.parameterTypes.size(); ++i) {
    if (!typesMatchForOverride(required.parameterTypes[i],
                               effective.parameterTypes[i])) {
      return false;
    }
  }
  return typesMatchForOverride(required.type, effective.type);
}

bool memberShapesSupportResultMeet(const SymbolInfo& lhs,
                                   const SymbolInfo& rhs) {
  if (lhs.kind != rhs.kind) {
    return false;
  }
  if (lhs.kind == AstDeclarationKind::Def) {
    if (lhs.typeParameters.size() != rhs.typeParameters.size() ||
        lhs.parameterTypes.size() != rhs.parameterTypes.size() ||
        lhs.contextualParameters != rhs.contextualParameters) {
      return false;
    }
    for (std::size_t i = 0; i < lhs.parameterTypes.size(); ++i) {
      if (!typesMatchForOverride(lhs.parameterTypes[i], rhs.parameterTypes[i])) {
        return false;
      }
    }
    return true;
  }
  if (lhs.kind == AstDeclarationKind::Val) {
    return true;
  }
  if (lhs.kind == AstDeclarationKind::Var) {
    return typesMatchForOverride(lhs.type, rhs.type);
  }
  return lhs.symbolName == rhs.symbolName;
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

struct AppliedTypeSyntax {
  std::string constructor;
  std::vector<std::string> arguments;
  bool applied = false;
  bool malformed = false;
};

struct WildcardTypeSyntax {
  std::string lowerBound;
  std::string upperBound;
  bool wildcard = false;
  bool malformed = false;
};

enum class CompositeTypeSyntaxKind { None, Intersection, Union };

struct CompositeTypeSyntax {
  CompositeTypeSyntaxKind kind = CompositeTypeSyntaxKind::None;
  std::vector<std::string> operands;
  bool malformed = false;
};

std::string normalizeGivenImportType(std::string_view typeName, bool* malformed) {
  std::string normalized = compactTypeName(typeName);
  std::vector<char> delimiters;
  for (char ch : normalized) {
    if (ch == '[' || ch == '(') {
      delimiters.push_back(ch);
      continue;
    }
    if (ch != ']' && ch != ')') {
      continue;
    }
    const char expected = ch == ']' ? '[' : '(';
    if (delimiters.empty() || delimiters.back() != expected) {
      *malformed = true;
      return normalized;
    }
    delimiters.pop_back();
  }
  if (!delimiters.empty()) {
    *malformed = true;
    return normalized;
  }

  while (normalized.size() >= 2 && normalized.front() == '(') {
    std::size_t depth = 0;
    std::size_t closing = std::string::npos;
    for (std::size_t i = 0; i < normalized.size(); ++i) {
      if (normalized[i] == '(') {
        ++depth;
      } else if (normalized[i] == ')') {
        --depth;
        if (depth == 0) {
          closing = i;
          break;
        }
      }
    }
    if (closing == std::string::npos || closing + 1 != normalized.size()) {
      break;
    }
    normalized = normalized.substr(1, normalized.size() - 2);
  }
  if (normalized.empty()) {
    *malformed = true;
  }
  return normalized;
}

CompositeTypeSyntax parseCompositeTypeSyntax(std::string_view typeName) {
  const auto split = [&](char operation) {
    CompositeTypeSyntax parsed;
    parsed.kind = operation == '|'
                      ? CompositeTypeSyntaxKind::Union
                      : CompositeTypeSyntaxKind::Intersection;
    std::size_t bracketDepth = 0;
    std::size_t parenthesisDepth = 0;
    std::size_t operandStart = 0;
    for (std::size_t i = 0; i <= typeName.size(); ++i) {
      if (i != typeName.size()) {
        if (typeName[i] == '[') {
          ++bracketDepth;
        } else if (typeName[i] == ']') {
          --bracketDepth;
        } else if (typeName[i] == '(') {
          ++parenthesisDepth;
        } else if (typeName[i] == ')') {
          --parenthesisDepth;
        }
      }
      if (i != typeName.size() &&
          (typeName[i] != operation || bracketDepth != 0 ||
           parenthesisDepth != 0)) {
        continue;
      }
      if (i == operandStart) {
        parsed.malformed = true;
        return parsed;
      }
      parsed.operands.emplace_back(typeName.substr(operandStart, i - operandStart));
      operandStart = i + 1;
    }
    if (parsed.operands.size() == 1) {
      parsed.kind = CompositeTypeSyntaxKind::None;
      parsed.operands.clear();
    }
    return parsed;
  };

  CompositeTypeSyntax unionType = split('|');
  if (unionType.malformed ||
      unionType.kind != CompositeTypeSyntaxKind::None) {
    return unionType;
  }
  return split('&');
}

std::string compositeTypeName(CompositeTypeKind kind,
                              const std::vector<TypeInfo>& operands) {
  const std::string_view operation =
      kind == CompositeTypeKind::Union ? " | " : " & ";
  const int precedence = kind == CompositeTypeKind::Union ? 1 : 2;
  std::string name;
  for (std::size_t i = 0; i < operands.size(); ++i) {
    if (i != 0) {
      name += operation;
    }
    const TypeInfo& operand = operands[i];
    const int operandPrecedence =
        operand.compositeKind == CompositeTypeKind::Union
            ? 1
            : operand.compositeKind == CompositeTypeKind::Intersection ? 2 : 3;
    const bool parenthesize = operandPrecedence < precedence;
    if (parenthesize) {
      name += '(';
    }
    name += operand.name;
    if (parenthesize) {
      name += ')';
    }
  }
  return name;
}

TypeInfo makeCompositeType(CompositeTypeKind kind,
                           std::vector<TypeInfo> operands) {
  std::vector<TypeInfo> flattened;
  for (TypeInfo& operand : operands) {
    if (operand.compositeKind == kind) {
      flattened.insert(flattened.end(),
                       std::make_move_iterator(operand.compositeTypes.begin()),
                       std::make_move_iterator(operand.compositeTypes.end()));
    } else {
      flattened.push_back(std::move(operand));
    }
  }

  if (kind == CompositeTypeKind::Intersection) {
    auto unionOperand =
        std::find_if(flattened.begin(), flattened.end(), [](const TypeInfo& operand) {
          return operand.compositeKind == CompositeTypeKind::Union;
        });
    if (unionOperand != flattened.end()) {
      std::vector<TypeInfo> alternatives;
      alternatives.reserve(unionOperand->compositeTypes.size());
      const std::size_t unionIndex =
          static_cast<std::size_t>(std::distance(flattened.begin(), unionOperand));
      for (const TypeInfo& alternative : unionOperand->compositeTypes) {
        std::vector<TypeInfo> branch = flattened;
        branch[unionIndex] = alternative;
        alternatives.push_back(
            makeCompositeType(CompositeTypeKind::Intersection, std::move(branch)));
      }
      return makeCompositeType(CompositeTypeKind::Union,
                               std::move(alternatives));
    }
  }

  if (flattened.size() == 1) {
    return std::move(flattened.front());
  }
  TypeInfo composite{SimpleTypeKind::Object, ""};
  composite.runtimeName = "Object";
  composite.compositeKind = kind;
  composite.compositeTypes = std::move(flattened);
  composite.name =
      compositeTypeName(composite.compositeKind, composite.compositeTypes);
  return composite;
}

std::size_t findTopLevelTypeBound(std::string_view typeName,
                                  std::string_view marker,
                                  std::size_t start) {
  std::size_t bracketDepth = 0;
  for (std::size_t i = start; i + marker.size() <= typeName.size(); ++i) {
    if (typeName[i] == '[') {
      ++bracketDepth;
      continue;
    }
    if (typeName[i] == ']') {
      if (bracketDepth == 0) {
        return std::string_view::npos;
      }
      --bracketDepth;
      continue;
    }
    if (bracketDepth == 0 && typeName.substr(i, marker.size()) == marker) {
      return i;
    }
  }
  return std::string_view::npos;
}

WildcardTypeSyntax parseWildcardTypeSyntax(std::string_view typeName) {
  const std::string compact = compactTypeName(typeName);
  WildcardTypeSyntax parsed;
  if (compact.empty() || compact.front() != '?') {
    return parsed;
  }

  parsed.wildcard = true;
  if (compact.size() == 1) {
    return parsed;
  }

  if (compact.starts_with("?<:")) {
    parsed.upperBound = compact.substr(3);
    parsed.malformed =
        parsed.upperBound.empty() ||
        findTopLevelTypeBound(parsed.upperBound, "<:", 0) !=
            std::string_view::npos ||
        findTopLevelTypeBound(parsed.upperBound, ">:", 0) !=
            std::string_view::npos;
    return parsed;
  }
  if (!compact.starts_with("?>:")) {
    parsed.malformed = true;
    return parsed;
  }

  const std::size_t upperMarker = findTopLevelTypeBound(compact, "<:", 3);
  parsed.lowerBound =
      compact.substr(3, upperMarker == std::string::npos
                            ? std::string::npos
                            : upperMarker - 3);
  if (upperMarker != std::string::npos) {
    parsed.upperBound = compact.substr(upperMarker + 2);
  }
  parsed.malformed =
      parsed.lowerBound.empty() ||
      (upperMarker != std::string::npos && parsed.upperBound.empty()) ||
      findTopLevelTypeBound(parsed.lowerBound, "<:", 0) !=
          std::string_view::npos ||
      findTopLevelTypeBound(parsed.lowerBound, ">:", 0) !=
          std::string_view::npos ||
      findTopLevelTypeBound(parsed.upperBound, "<:", 0) !=
          std::string_view::npos ||
      findTopLevelTypeBound(parsed.upperBound, ">:", 0) !=
          std::string_view::npos;
  return parsed;
}

bool isUniversalWildcardUpperBound(std::string_view typeName) {
  const std::string compact = compactTypeName(typeName);
  return compact == "Any" || compact == "scala.Any";
}

AppliedTypeSyntax parseAppliedTypeSyntax(std::string_view typeName) {
  const std::string compact = compactTypeName(typeName);
  const std::size_t open = compact.find('[');
  if (open == std::string::npos) {
    return AppliedTypeSyntax{compact, {}, false, false};
  }

  AppliedTypeSyntax parsed;
  parsed.constructor = compact.substr(0, open);
  parsed.applied = true;
  if (parsed.constructor.empty() || compact.back() != ']') {
    parsed.malformed = true;
    return parsed;
  }

  std::size_t depth = 0;
  std::size_t argumentStart = open + 1;
  for (std::size_t i = open; i < compact.size(); ++i) {
    if (compact[i] == '[') {
      ++depth;
      continue;
    }
    if (compact[i] == ']') {
      if (depth == 0) {
        parsed.malformed = true;
        return parsed;
      }
      --depth;
      if (depth == 0) {
        if (i + 1 != compact.size()) {
          parsed.malformed = true;
          return parsed;
        }
        if (i == argumentStart) {
          parsed.malformed = true;
          return parsed;
        }
        parsed.arguments.push_back(compact.substr(argumentStart, i - argumentStart));
      }
      continue;
    }
    if (compact[i] == ',' && depth == 1) {
      if (i == argumentStart) {
        parsed.malformed = true;
        return parsed;
      }
      parsed.arguments.push_back(compact.substr(argumentStart, i - argumentStart));
      argumentStart = i + 1;
    }
  }
  if (depth != 0 || parsed.arguments.empty()) {
    parsed.malformed = true;
  }
  return parsed;
}

std::optional<std::size_t> tupleArityForConstructor(std::string_view constructor) {
  constexpr std::string_view scalaPrefix = "scala.";
  if (constructor.starts_with(scalaPrefix)) {
    constructor.remove_prefix(scalaPrefix.size());
  }
  constexpr std::string_view tuplePrefix = "Tuple";
  if (!constructor.starts_with(tuplePrefix)) {
    return std::nullopt;
  }
  constructor.remove_prefix(tuplePrefix.size());
  if (constructor.empty()) {
    return std::nullopt;
  }
  std::size_t arity = 0;
  for (char ch : constructor) {
    if (ch < '0' || ch > '9') {
      return std::nullopt;
    }
    arity = arity * 10 + static_cast<std::size_t>(ch - '0');
  }
  return arity >= 1 && arity <= 22 ? std::optional<std::size_t>{arity}
                                   : std::nullopt;
}

std::optional<std::vector<std::string>>
parenthesizedTupleTypeElements(std::string_view typeName) {
  const std::string normalized = trim(typeName);
  if (normalized.size() < 5 || normalized.front() != '(' ||
      normalized.back() != ')') {
    return std::nullopt;
  }

  const std::string_view elements(normalized.data() + 1, normalized.size() - 2);
  std::vector<std::string> result;
  std::size_t elementStart = 0;
  std::size_t bracketDepth = 0;
  std::size_t parenthesisDepth = 0;
  char quote = '\0';
  bool escaped = false;
  bool sawComma = false;
  for (std::size_t i = 0; i < elements.size(); ++i) {
    const char ch = elements[i];
    if (quote != '\0') {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == quote) {
        quote = '\0';
      }
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (ch == '[') {
      ++bracketDepth;
      continue;
    }
    if (ch == ']') {
      if (bracketDepth == 0) {
        return std::nullopt;
      }
      --bracketDepth;
      continue;
    }
    if (ch == '(') {
      ++parenthesisDepth;
      continue;
    }
    if (ch == ')') {
      if (parenthesisDepth == 0) {
        return std::nullopt;
      }
      --parenthesisDepth;
      continue;
    }
    if (ch == ',' && bracketDepth == 0 && parenthesisDepth == 0) {
      const std::string element = trim(elements.substr(elementStart, i - elementStart));
      if (element.empty()) {
        return std::nullopt;
      }
      result.push_back(element);
      elementStart = i + 1;
      sawComma = true;
    }
  }
  if (!sawComma || bracketDepth != 0 || parenthesisDepth != 0 || quote != '\0') {
    return std::nullopt;
  }
  const std::string last = trim(elements.substr(elementStart));
  if (last.empty()) {
    return std::nullopt;
  }
  result.push_back(last);
  return result;
}

std::optional<std::vector<std::string>>
consTupleTypeElements(std::string_view typeName) {
  const std::string normalized = trim(typeName);
  std::vector<std::string> heads;
  std::size_t elementStart = 0;
  std::size_t bracketDepth = 0;
  std::size_t parenthesisDepth = 0;
  char quote = '\0';
  bool escaped = false;
  for (std::size_t i = 0; i < normalized.size(); ++i) {
    const char ch = normalized[i];
    if (quote != '\0') {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == quote) {
        quote = '\0';
      }
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (ch == '[') {
      ++bracketDepth;
      continue;
    }
    if (ch == ']') {
      if (bracketDepth == 0) {
        return std::nullopt;
      }
      --bracketDepth;
      continue;
    }
    if (ch == '(') {
      ++parenthesisDepth;
      continue;
    }
    if (ch == ')') {
      if (parenthesisDepth == 0) {
        return std::nullopt;
      }
      --parenthesisDepth;
      continue;
    }
    if (ch != '*' || i + 1 >= normalized.size() || normalized[i + 1] != ':' ||
        bracketDepth != 0 ||
        parenthesisDepth != 0) {
      continue;
    }
    const std::string head = trim(
        std::string_view(normalized).substr(elementStart, i - elementStart));
    if (head.empty()) {
      return std::nullopt;
    }
    heads.push_back(head);
    elementStart = i + 2;
    ++i;
  }
  if (heads.empty() || bracketDepth != 0 || parenthesisDepth != 0 ||
      quote != '\0') {
    return std::nullopt;
  }

  const std::string tail =
      trim(std::string_view(normalized).substr(elementStart));
  if (tail.empty()) {
    return std::nullopt;
  }
  const std::string compactTail = compactTypeName(tail);
  if (compactTail == "EmptyTuple" ||
      compactTail == support::StdNames::ScalaEmptyTuple) {
    return heads;
  }
  if (const auto elements = parenthesizedTupleTypeElements(tail);
      elements.has_value()) {
    heads.insert(heads.end(), elements->begin(), elements->end());
    return heads;
  }
  if (tail.size() >= 2 && tail.front() == '(' && tail.back() == ')') {
    if (const auto elements = consTupleTypeElements(
            std::string_view(tail).substr(1, tail.size() - 2));
        elements.has_value()) {
      heads.insert(heads.end(), elements->begin(), elements->end());
      return heads;
    }
  }
  const AppliedTypeSyntax applied = parseAppliedTypeSyntax(tail);
  const std::optional<std::size_t> arity =
      applied.malformed ? std::nullopt
                        : tupleArityForConstructor(applied.constructor);
  if (!arity.has_value() || applied.arguments.size() != *arity) {
    return std::nullopt;
  }
  heads.insert(heads.end(), applied.arguments.begin(), applied.arguments.end());
  return heads;
}

std::optional<std::vector<std::string>> tupleTypeElements(std::string_view typeName) {
  const std::string compact = compactTypeName(typeName);
  if (compact == "EmptyTuple" || compact == support::StdNames::ScalaEmptyTuple) {
    return std::vector<std::string>{};
  }
  if (const auto cons = consTupleTypeElements(typeName); cons.has_value()) {
    return cons;
  }
  if (const auto parenthesized = parenthesizedTupleTypeElements(typeName);
      parenthesized.has_value()) {
    return parenthesized;
  }
  const AppliedTypeSyntax applied = parseAppliedTypeSyntax(typeName);
  const std::optional<std::size_t> arity =
      applied.malformed ? std::nullopt
                        : tupleArityForConstructor(applied.constructor);
  if (!arity.has_value() || applied.arguments.size() != *arity) {
    return std::nullopt;
  }
  return applied.arguments;
}

std::vector<std::string> typeArgumentsFor(const AstExpression& expression) {
  if (!expression.typeArguments.empty()) {
    return expression.typeArguments;
  }
  return expression.declaredType.empty()
             ? std::vector<std::string>{}
             : std::vector<std::string>{expression.declaredType};
}

std::vector<std::string> sourceParentTypes(const AstDeclaration& declaration) {
  if (!declaration.parentTypes.empty()) {
    return declaration.parentTypes;
  }
  if (!declaration.declaredType.empty()) {
    return {declaration.declaredType};
  }
  return {};
}

std::vector<std::string>
linearizedParentsFor(const std::vector<std::string>& directParents,
                     const std::unordered_map<std::string, SymbolInfo>& symbols,
                     bool* cyclic = nullptr, bool* consistent = nullptr) {
  using Sequence = std::vector<std::string>;
  if (cyclic != nullptr) {
    *cyclic = false;
  }
  if (consistent != nullptr) {
    *consistent = true;
  }
  std::unordered_map<std::string, Sequence> cache;
  std::unordered_set<std::string> visiting;

  auto merge = [&](std::vector<Sequence> sequences) {
    Sequence result;
    std::unordered_set<std::string> emitted;
    while (true) {
      sequences.erase(
          std::remove_if(sequences.begin(), sequences.end(),
                         [](const Sequence& sequence) { return sequence.empty(); }),
          sequences.end());
      if (sequences.empty()) {
        break;
      }

      std::string candidate;
      for (const Sequence& sequence : sequences) {
        const std::string& head = sequence.front();
        bool appearsInTail = false;
        for (const Sequence& other : sequences) {
          if (std::find(std::next(other.begin()), other.end(), head) != other.end()) {
            appearsInTail = true;
            break;
          }
        }
        if (!appearsInTail) {
          candidate = head;
          break;
        }
      }
      if (candidate.empty()) {
        if (consistent != nullptr) {
          *consistent = false;
        }
        return Sequence{};
      }
      if (emitted.insert(candidate).second) {
        result.push_back(candidate);
      }
      for (Sequence& sequence : sequences) {
        if (!sequence.empty() && sequence.front() == candidate) {
          sequence.erase(sequence.begin());
        }
      }
    }
    return result;
  };

  std::function<Sequence(const std::string&)> linearize =
      [&](const std::string& typeName) -> Sequence {
    if (auto cached = cache.find(typeName); cached != cache.end()) {
      return cached->second;
    }
    if (!visiting.insert(typeName).second) {
      if (cyclic != nullptr) {
        *cyclic = true;
      }
      return {};
    }

    std::vector<std::string> parents;
    if (auto symbol = symbols.find(typeName); symbol != symbols.end()) {
      parents = symbol->second.parentSymbolNames;
    }
    std::vector<Sequence> sequences;
    Sequence priority(parents.rbegin(), parents.rend());
    for (const std::string& parent : priority) {
      sequences.push_back(linearize(parent));
    }
    if (!priority.empty()) {
      sequences.push_back(priority);
    }
    Sequence result{typeName};
    Sequence merged = merge(std::move(sequences));
    result.insert(result.end(), merged.begin(), merged.end());
    visiting.erase(typeName);
    cache[typeName] = result;
    return result;
  };

  std::vector<Sequence> sequences;
  Sequence priority(directParents.rbegin(), directParents.rend());
  for (const std::string& parent : priority) {
    sequences.push_back(linearize(parent));
  }
  if (!priority.empty()) {
    sequences.push_back(priority);
  }
  return merge(std::move(sequences));
}

bool inheritanceReaches(const std::string& start, const std::string& target,
                        const std::unordered_map<std::string, SymbolInfo>& symbols,
                        std::unordered_set<std::string>& visited) {
  if (start == target) {
    return true;
  }
  if (!visited.insert(start).second) {
    return false;
  }
  auto symbol = symbols.find(start);
  if (symbol == symbols.end()) {
    return false;
  }
  for (const std::string& parent : symbol->second.parentSymbolNames) {
    if (inheritanceReaches(parent, target, symbols, visited)) {
      return true;
    }
  }
  return false;
}

std::vector<TypedDeclaration> standardExceptionDeclarations() {
  const support::SourceSpan noSpan = support::SourceSpan::none();

  TypedDeclaration throwable;
  throwable.kind = AstDeclarationKind::Class;
  throwable.name = "Throwable";
  throwable.symbolName = std::string(support::StdNames::JavaLangThrowable);
  throwable.span = noSpan;
  throwable.parameters = {"val message: String", "cause: java.lang.Throwable"};
  throwable.parameterTypes = {TypeInfo{SimpleTypeKind::String, "String"},
                              TypeInfo{SimpleTypeKind::Object, throwable.symbolName}};
  throwable.accessorParameters = {std::string(support::StdNames::ThrowableMessage)};
  throwable.inferredType = TypeInfo{SimpleTypeKind::Object, throwable.symbolName};

  TypedDeclaration getMessage;
  getMessage.kind = AstDeclarationKind::Def;
  getMessage.name = std::string(support::StdNames::GetMessage);
  getMessage.symbolName = throwable.symbolName + "." + getMessage.name;
  getMessage.span = noSpan;
  getMessage.declaredType = "String";
  getMessage.inferredType = TypeInfo{SimpleTypeKind::String, "String"};
  getMessage.hasInitializer = true;

  TypedDeclaration getCause;
  getCause.kind = AstDeclarationKind::Def;
  getCause.name = std::string(support::StdNames::GetCause);
  getCause.symbolName = throwable.symbolName + "." + getCause.name;
  getCause.span = noSpan;
  getCause.declaredType = throwable.symbolName;
  getCause.inferredType = TypeInfo{SimpleTypeKind::Object, throwable.symbolName};
  getCause.hasInitializer = true;

  TypedDeclaration initCause;
  initCause.kind = AstDeclarationKind::Def;
  initCause.name = std::string(support::StdNames::InitCause);
  initCause.symbolName = throwable.symbolName + "." + initCause.name;
  initCause.span = noSpan;
  initCause.parameters = {"cause: java.lang.Throwable"};
  initCause.parameterTypes = {TypeInfo{SimpleTypeKind::Object, throwable.symbolName}};
  initCause.declaredType = throwable.symbolName;
  initCause.inferredType = TypeInfo{SimpleTypeKind::Object, throwable.symbolName};
  initCause.hasInitializer = true;

  TypedDeclaration fillInStackTrace;
  fillInStackTrace.kind = AstDeclarationKind::Def;
  fillInStackTrace.name = std::string(support::StdNames::FillInStackTrace);
  fillInStackTrace.symbolName = throwable.symbolName + "." + fillInStackTrace.name;
  fillInStackTrace.span = noSpan;
  fillInStackTrace.declaredType = throwable.symbolName;
  fillInStackTrace.inferredType =
      TypeInfo{SimpleTypeKind::Object, throwable.symbolName};
  fillInStackTrace.hasInitializer = true;

  TypedDeclaration getStackTrace;
  getStackTrace.kind = AstDeclarationKind::Def;
  getStackTrace.name = std::string(support::StdNames::GetStackTrace);
  getStackTrace.symbolName = throwable.symbolName + "." + getStackTrace.name;
  getStackTrace.span = noSpan;
  getStackTrace.declaredType =
      "Array[" + std::string(support::StdNames::JavaLangStackTraceElement) + "]";
  getStackTrace.inferredType = TypeInfo{
      SimpleTypeKind::Object,
      "Array [ " + std::string(support::StdNames::JavaLangStackTraceElement) + " ]"};
  getStackTrace.hasInitializer = true;

  TypedDeclaration setStackTrace;
  setStackTrace.kind = AstDeclarationKind::Def;
  setStackTrace.name = std::string(support::StdNames::SetStackTrace);
  setStackTrace.symbolName = throwable.symbolName + "." + setStackTrace.name;
  setStackTrace.span = noSpan;
  setStackTrace.parameters = {
      "stackTrace: Array[" + std::string(support::StdNames::JavaLangStackTraceElement) +
      "]"};
  setStackTrace.parameterTypes = {TypeInfo{
      SimpleTypeKind::Object,
      "Array [ " + std::string(support::StdNames::JavaLangStackTraceElement) + " ]"}};
  setStackTrace.declaredType = "Unit";
  setStackTrace.inferredType = TypeInfo{SimpleTypeKind::Unit, "Unit"};
  setStackTrace.hasInitializer = true;

  TypedDeclaration addSuppressed;
  addSuppressed.kind = AstDeclarationKind::Def;
  addSuppressed.name = std::string(support::StdNames::AddSuppressed);
  addSuppressed.symbolName = throwable.symbolName + "." + addSuppressed.name;
  addSuppressed.span = noSpan;
  addSuppressed.parameters = {"exception: java.lang.Throwable"};
  addSuppressed.parameterTypes = {
      TypeInfo{SimpleTypeKind::Object, throwable.symbolName}};
  addSuppressed.declaredType = "Unit";
  addSuppressed.inferredType = TypeInfo{SimpleTypeKind::Unit, "Unit"};
  addSuppressed.hasInitializer = true;

  TypedDeclaration getSuppressed;
  getSuppressed.kind = AstDeclarationKind::Def;
  getSuppressed.name = std::string(support::StdNames::GetSuppressed);
  getSuppressed.symbolName = throwable.symbolName + "." + getSuppressed.name;
  getSuppressed.span = noSpan;
  getSuppressed.declaredType = "Array[java.lang.Throwable]";
  getSuppressed.inferredType =
      TypeInfo{SimpleTypeKind::Object, "Array [ java.lang.Throwable ]"};
  getSuppressed.hasInitializer = true;

  TypedDeclaration printStackTrace;
  printStackTrace.kind = AstDeclarationKind::Def;
  printStackTrace.name = std::string(support::StdNames::PrintStackTrace);
  printStackTrace.symbolName = throwable.symbolName + "." + printStackTrace.name;
  printStackTrace.span = noSpan;
  printStackTrace.declaredType = "Unit";
  printStackTrace.inferredType = TypeInfo{SimpleTypeKind::Unit, "Unit"};
  printStackTrace.hasInitializer = true;

  TypedDeclaration throwableToString;
  throwableToString.kind = AstDeclarationKind::Def;
  throwableToString.name = std::string(support::StdNames::ToString);
  throwableToString.symbolName = throwable.symbolName + "." + throwableToString.name;
  throwableToString.span = noSpan;
  throwableToString.declaredType = "String";
  throwableToString.inferredType = TypeInfo{SimpleTypeKind::String, "String"};
  throwableToString.hasInitializer = true;
  throwable.members = {std::move(getMessage),      std::move(getCause),
                       std::move(initCause),       std::move(fillInStackTrace),
                       std::move(getStackTrace),   std::move(setStackTrace),
                       std::move(addSuppressed),   std::move(getSuppressed),
                       std::move(printStackTrace), std::move(throwableToString)};

  TypedDeclaration exception;
  exception.kind = AstDeclarationKind::Class;
  exception.name = "Exception";
  exception.symbolName = std::string(support::StdNames::JavaLangException);
  exception.span = noSpan;
  exception.parameters = {"val message: String"};
  exception.parameterTypes = {TypeInfo{SimpleTypeKind::String, "String"}};
  exception.accessorParameters = {std::string(support::StdNames::ThrowableMessage)};
  exception.declaredType = throwable.symbolName;
  exception.parentTypes = {throwable.symbolName};
  AstExpression parentMessage;
  parentMessage.kind = AstExpressionKind::Identifier;
  parentMessage.text = std::string(support::StdNames::ThrowableMessage);
  parentMessage.span = noSpan;
  AstExpression uninitializedCause;
  uninitializedCause.kind = AstExpressionKind::This;
  uninitializedCause.text = "this";
  uninitializedCause.span = noSpan;
  exception.parentArguments = {std::move(parentMessage), std::move(uninitializedCause)};
  exception.inferredType = TypeInfo{SimpleTypeKind::Object, exception.symbolName};

  TypedDeclaration toString;
  toString.kind = AstDeclarationKind::Def;
  toString.name = std::string(support::StdNames::ToString);
  toString.symbolName = exception.symbolName + "." + toString.name;
  toString.span = noSpan;
  toString.declaredType = "String";
  toString.inferredType = TypeInfo{SimpleTypeKind::String, "String"};
  toString.hasInitializer = true;
  toString.isOverride = true;
  exception.members = {std::move(toString)};

  TypedDeclaration error;
  error.kind = AstDeclarationKind::Class;
  error.name = "Error";
  error.symbolName = std::string(support::StdNames::JavaLangError);
  error.span = noSpan;
  error.parameters = {"val message: String"};
  error.parameterTypes = {TypeInfo{SimpleTypeKind::String, "String"}};
  error.accessorParameters = {std::string(support::StdNames::ThrowableMessage)};
  error.declaredType = throwable.symbolName;
  error.parentTypes = {throwable.symbolName};
  AstExpression errorParentMessage;
  errorParentMessage.kind = AstExpressionKind::Identifier;
  errorParentMessage.text = std::string(support::StdNames::ThrowableMessage);
  errorParentMessage.span = noSpan;
  AstExpression errorUninitializedCause;
  errorUninitializedCause.kind = AstExpressionKind::This;
  errorUninitializedCause.text = "this";
  errorUninitializedCause.span = noSpan;
  error.parentArguments = {std::move(errorParentMessage),
                           std::move(errorUninitializedCause)};
  error.inferredType = TypeInfo{SimpleTypeKind::Object, error.symbolName};

  const auto exceptionSubclass = [&](std::string name, std::string symbolName,
                                     const std::string& parentSymbolName) {
    TypedDeclaration subclass;
    subclass.kind = AstDeclarationKind::Class;
    subclass.name = std::move(name);
    subclass.symbolName = std::move(symbolName);
    subclass.span = noSpan;
    subclass.parameters = {"val message: String"};
    subclass.parameterTypes = {TypeInfo{SimpleTypeKind::String, "String"}};
    subclass.accessorParameters = {std::string(support::StdNames::ThrowableMessage)};
    subclass.declaredType = parentSymbolName;
    subclass.parentTypes = {parentSymbolName};
    AstExpression message;
    message.kind = AstExpressionKind::Identifier;
    message.text = std::string(support::StdNames::ThrowableMessage);
    message.span = noSpan;
    subclass.parentArguments = {std::move(message)};
    subclass.inferredType = TypeInfo{SimpleTypeKind::Object, subclass.symbolName};
    return subclass;
  };

  TypedDeclaration assertionError = exceptionSubclass(
      "AssertionError", std::string(support::StdNames::JavaLangAssertionError),
      error.symbolName);
  TypedDeclaration notImplementedError = exceptionSubclass(
      "NotImplementedError", std::string(support::StdNames::ScalaNotImplementedError),
      error.symbolName);
  TypedDeclaration runtimeException = exceptionSubclass(
      "RuntimeException", std::string(support::StdNames::JavaLangRuntimeException),
      exception.symbolName);
  TypedDeclaration arithmeticException =
      exceptionSubclass("ArithmeticException",
                        std::string(support::StdNames::JavaLangArithmeticException),
                        runtimeException.symbolName);
  TypedDeclaration illegalArgument = exceptionSubclass(
      "IllegalArgumentException",
      std::string(support::StdNames::JavaLangIllegalArgumentException),
      runtimeException.symbolName);
  TypedDeclaration illegalState =
      exceptionSubclass("IllegalStateException",
                        std::string(support::StdNames::JavaLangIllegalStateException),
                        runtimeException.symbolName);
  TypedDeclaration nullPointer =
      exceptionSubclass("NullPointerException",
                        std::string(support::StdNames::JavaLangNullPointerException),
                        runtimeException.symbolName);
  TypedDeclaration classCast = exceptionSubclass(
      "ClassCastException", std::string(support::StdNames::JavaLangClassCastException),
      runtimeException.symbolName);
  TypedDeclaration arrayStore =
      exceptionSubclass("ArrayStoreException",
                        std::string(support::StdNames::JavaLangArrayStoreException),
                        runtimeException.symbolName);
  TypedDeclaration indexOutOfBounds = exceptionSubclass(
      "IndexOutOfBoundsException",
      std::string(support::StdNames::JavaLangIndexOutOfBoundsException),
      runtimeException.symbolName);
  TypedDeclaration arrayIndexOutOfBounds = exceptionSubclass(
      "ArrayIndexOutOfBoundsException",
      std::string(support::StdNames::JavaLangArrayIndexOutOfBoundsException),
      indexOutOfBounds.symbolName);
  TypedDeclaration negativeArraySize = exceptionSubclass(
      "NegativeArraySizeException",
      std::string(support::StdNames::JavaLangNegativeArraySizeException),
      runtimeException.symbolName);
  TypedDeclaration bufferUnderflow =
      exceptionSubclass("BufferUnderflowException",
                        std::string(support::StdNames::JavaNioBufferUnderflowException),
                        runtimeException.symbolName);
  TypedDeclaration bufferOverflow =
      exceptionSubclass("BufferOverflowException",
                        std::string(support::StdNames::JavaNioBufferOverflowException),
                        runtimeException.symbolName);
  TypedDeclaration invalidMark =
      exceptionSubclass("InvalidMarkException",
                        std::string(support::StdNames::JavaNioInvalidMarkException),
                        illegalState.symbolName);
  TypedDeclaration stackTraceElement;
  stackTraceElement.kind = AstDeclarationKind::Class;
  stackTraceElement.name = "StackTraceElement";
  stackTraceElement.symbolName =
      std::string(support::StdNames::JavaLangStackTraceElement);
  stackTraceElement.span = noSpan;
  stackTraceElement.parameters = {"val functionName: String", "val fileName: String",
                                  "val lineNumber: Int", "val columnNumber: Int"};
  stackTraceElement.parameterTypes = {TypeInfo{SimpleTypeKind::String, "String"},
                                      TypeInfo{SimpleTypeKind::String, "String"},
                                      TypeInfo{SimpleTypeKind::Int, "Int"},
                                      TypeInfo{SimpleTypeKind::Int, "Int"}};
  stackTraceElement.accessorParameters = {
      std::string(support::StdNames::StackTraceFunctionName),
      std::string(support::StdNames::StackTraceFileName),
      std::string(support::StdNames::StackTraceLineNumber),
      std::string(support::StdNames::StackTraceColumnNumber)};
  stackTraceElement.inferredType =
      TypeInfo{SimpleTypeKind::Object, stackTraceElement.symbolName};

  TypedDeclaration stackTraceToString;
  stackTraceToString.kind = AstDeclarationKind::Def;
  stackTraceToString.name = std::string(support::StdNames::ToString);
  stackTraceToString.symbolName =
      stackTraceElement.symbolName + "." + stackTraceToString.name;
  stackTraceToString.span = noSpan;
  stackTraceToString.declaredType = "String";
  stackTraceToString.inferredType = TypeInfo{SimpleTypeKind::String, "String"};
  stackTraceToString.hasInitializer = true;
  stackTraceToString.isOverride = true;
  stackTraceElement.members = {std::move(stackTraceToString)};
  return {std::move(throwable),           std::move(error),
          std::move(assertionError),      std::move(notImplementedError),
          std::move(exception),           std::move(runtimeException),
          std::move(arithmeticException), std::move(illegalArgument),
          std::move(illegalState),        std::move(nullPointer),
          std::move(classCast),           std::move(arrayStore),
          std::move(indexOutOfBounds),    std::move(arrayIndexOutOfBounds),
          std::move(negativeArraySize),   std::move(bufferUnderflow),
          std::move(bufferOverflow),      std::move(invalidMark),
          std::move(stackTraceElement)};
}

std::vector<std::pair<std::string, AstDeclaration>>
standardDerivationDeclarations(const AstModule& module) {
  const support::SourceSpan noSpan = support::SourceSpan::none();

  AstDeclaration tuple;
  tuple.kind = AstDeclarationKind::Trait;
  tuple.name = "Tuple";
  tuple.span = noSpan;

  AstDeclaration emptyTuple;
  emptyTuple.kind = AstDeclarationKind::Object;
  emptyTuple.name = "EmptyTuple";
  emptyTuple.span = noSpan;
  emptyTuple.parentTypes = {"scala.Tuple"};

  AstDeclaration polyFunction;
  polyFunction.kind = AstDeclarationKind::Trait;
  polyFunction.name = "PolyFunction";
  polyFunction.span = noSpan;

  bool needsPolyFunction = false;
  std::vector<std::size_t> tupleArities;
  std::size_t maximumUnknownTupleConsDepth = 0;
  std::size_t maximumTupleShrinkDepth = 0;
  const auto addTupleArity = [&](std::size_t arity) {
    if (arity >= 1 && arity <= 22 &&
        std::find(tupleArities.begin(), tupleArities.end(), arity) ==
            tupleArities.end()) {
      tupleArities.push_back(arity);
    }
  };
  std::unordered_map<std::string, std::size_t> declaredTupleArities;
  const std::function<void(const std::vector<AstDeclaration>&)>
      collectDeclaredTupleArities =
          [&](const std::vector<AstDeclaration>& declarations) {
            for (const AstDeclaration& declaration : declarations) {
              const auto mentionsPolyFunction = [](std::string_view typeName) {
                return typeName.find("PolyFunction") != std::string_view::npos ||
                       parsePolymorphicFunctionTypeSyntax(typeName).polymorphic;
              };
              needsPolyFunction =
                  needsPolyFunction ||
                  mentionsPolyFunction(declaration.declaredType) ||
                  std::any_of(declaration.parentTypes.begin(),
                              declaration.parentTypes.end(),
                              mentionsPolyFunction) ||
                  std::any_of(declaration.parameters.begin(),
                              declaration.parameters.end(),
                              mentionsPolyFunction);
              if (const auto elements = tupleTypeElements(declaration.declaredType);
                  elements.has_value() && elements->size() <= 22) {
                declaredTupleArities[declaration.name] = elements->size();
              }
              collectDeclaredTupleArities(declaration.members);
            }
          };
  collectDeclaredTupleArities(module.declarations);
  const std::function<void(std::string_view)> collectTupleType =
      [&](std::string_view typeName) {
        const std::optional<std::vector<std::string>> elements =
            tupleTypeElements(typeName);
        if (!elements.has_value()) {
          return;
        }
        addTupleArity(elements->size());
        for (const std::string& element : *elements) {
          collectTupleType(element);
        }
      };
  const std::function<std::size_t(const AstExpression&)> tupleConsDepth =
      [&](const AstExpression& expression) {
        return expression.kind == AstExpressionKind::Binary &&
                       expression.text == "*:" && expression.children.size() == 2
                   ? 1 + tupleConsDepth(expression.children[1])
                   : 0;
      };
  const std::function<std::size_t(const AstExpression&)> tupleShrinkDepth =
      [&](const AstExpression& expression) {
        return expression.kind == AstExpressionKind::Select &&
                       (expression.text == support::StdNames::TupleTail ||
                        expression.text == support::StdNames::TupleInit) &&
                       expression.children.size() == 1
                   ? 1 + tupleShrinkDepth(expression.children.front())
                   : 0;
      };
  const std::function<std::optional<std::size_t>(const AstExpression&)>
      collectTupleExpressions =
          [&](const AstExpression& expression) -> std::optional<std::size_t> {
        needsPolyFunction =
            needsPolyFunction ||
            expression.kind == AstExpressionKind::PolymorphicFunction ||
            (expression.kind == AstExpressionKind::Select &&
             expression.text == support::StdNames::TupleMap);
        maximumTupleShrinkDepth =
            std::max(maximumTupleShrinkDepth, tupleShrinkDepth(expression));
        std::optional<std::size_t> knownArity;
        if (expression.kind == AstExpressionKind::TupleLiteral &&
            expression.children.size() >= 2 && expression.children.size() <= 22) {
          addTupleArity(expression.children.size());
          knownArity = expression.children.size();
        } else if (expression.kind == AstExpressionKind::Identifier &&
                   expression.text == "EmptyTuple") {
          knownArity = 0;
        } else if (expression.kind == AstExpressionKind::Identifier) {
          if (auto declared = declaredTupleArities.find(expression.text);
              declared != declaredTupleArities.end()) {
            knownArity = declared->second;
          }
        }
        for (const std::string& typeArgument : expression.typeArguments) {
          collectTupleType(typeArgument);
        }
        collectTupleType(expression.declaredType);
        std::optional<std::size_t> leftArity;
        std::optional<std::size_t> rightArity;
        for (std::size_t index = 0; index < expression.children.size(); ++index) {
          const std::optional<std::size_t> childArity =
              collectTupleExpressions(expression.children[index]);
          if (index == 0) {
            leftArity = childArity;
          }
          if (index == 1) {
            rightArity = childArity;
          }
        }
        if (expression.kind == AstExpressionKind::Binary &&
            expression.text == "*:" && expression.children.size() == 2) {
          if (rightArity.has_value() && *rightArity < 22) {
            addTupleArity(*rightArity + 1);
            return *rightArity + 1;
          }
          const std::size_t unknownDepth = tupleConsDepth(expression);
          maximumUnknownTupleConsDepth =
              std::max(maximumUnknownTupleConsDepth, unknownDepth);
          return std::nullopt;
        }
        if (expression.kind == AstExpressionKind::Binary &&
            expression.text == support::StdNames::TupleConcat &&
            expression.children.size() == 2 && leftArity.has_value() &&
            rightArity.has_value()) {
          const std::size_t resultArity = *leftArity + *rightArity;
          addTupleArity(resultArity);
          return resultArity <= 22
                     ? std::optional<std::size_t>{resultArity}
                     : std::nullopt;
        }
        return knownArity;
      };
  const std::function<void(const std::vector<AstDeclaration>&)>
      collectTupleDeclarations =
          [&](const std::vector<AstDeclaration>& declarations) {
            for (const AstDeclaration& declaration : declarations) {
              collectTupleType(declaration.declaredType);
              if (declaration.hasInitializer) {
                collectTupleExpressions(declaration.initializer);
              }
              for (const AstExpression& argument : declaration.parentArguments) {
                collectTupleExpressions(argument);
              }
              for (const AstExpression& expression : declaration.constructorBody) {
                collectTupleExpressions(expression);
              }
              collectTupleDeclarations(declaration.members);
            }
          };
  collectTupleDeclarations(module.declarations);
  std::vector<const AstDeclaration*> derivationCandidates;
  const std::function<void(const std::vector<AstDeclaration>&)>
      collectDerivationCandidates =
          [&](const std::vector<AstDeclaration>& declarations) {
            for (const AstDeclaration& declaration : declarations) {
              derivationCandidates.push_back(&declaration);
              if (declaration.kind == AstDeclarationKind::Object) {
                collectDerivationCandidates(declaration.members);
              }
            }
          };
  collectDerivationCandidates(module.declarations);
  for (const AstDeclaration* declaration : derivationCandidates) {
    if (declaration->kind == AstDeclarationKind::Class &&
        !declaration->derivedTypes.empty()) {
      addTupleArity(declaration->parameters.size());
    }
  }
  for (const AstDeclaration* declarationPointer : derivationCandidates) {
    const AstDeclaration& declaration = *declarationPointer;
    if (declaration.kind != AstDeclarationKind::Trait || !declaration.isSealed ||
        declaration.derivedTypes.empty()) {
      continue;
    }
    const std::string qualifiedName = module.packageName.empty()
                                          ? declaration.name
                                          : module.packageName + "." + declaration.name;
    std::size_t childCount = 0;
    const auto countDirectChild = [&](const AstDeclaration& candidate) {
      if (candidate.kind != AstDeclarationKind::Class &&
          candidate.kind != AstDeclarationKind::Object) {
        return;
      }
      const bool directChild = std::any_of(
          candidate.parentTypes.begin(), candidate.parentTypes.end(),
          [&](const std::string& parentType) {
            const std::string constructor =
                parseAppliedTypeSyntax(parentType).constructor;
            return constructor == declaration.name || constructor == qualifiedName;
          });
      childCount += directChild ? 1 : 0;
    };
    std::function<void(const AstDeclaration&)> countNestedDirectChildren =
        [&](const AstDeclaration& nestedCandidate) {
          countDirectChild(nestedCandidate);
          if (nestedCandidate.kind != AstDeclarationKind::Class &&
              nestedCandidate.kind != AstDeclarationKind::Trait &&
              nestedCandidate.kind != AstDeclarationKind::Object) {
            return;
          }
          for (const AstDeclaration& member : nestedCandidate.members) {
            countNestedDirectChildren(member);
          }
        };
    for (const AstDeclaration& candidate : module.declarations) {
      countNestedDirectChildren(candidate);
    }
    addTupleArity(childCount);
  }
  if (maximumUnknownTupleConsDepth != 0) {
    std::vector<std::size_t> baseArities = tupleArities;
    baseArities.push_back(0);
    for (std::size_t base : baseArities) {
      for (std::size_t offset = 1; offset <= maximumUnknownTupleConsDepth &&
                                   base + offset <= 22;
           ++offset) {
        addTupleArity(base + offset);
      }
    }
  }
  if (maximumTupleShrinkDepth != 0) {
    const std::vector<std::size_t> baseArities = tupleArities;
    for (std::size_t base : baseArities) {
      for (std::size_t offset = 1;
           offset <= maximumTupleShrinkDepth && offset < base; ++offset) {
        addTupleArity(base - offset);
      }
    }
  }
  std::sort(tupleArities.begin(), tupleArities.end());

  std::vector<AstDeclaration> tupleTypes;
  tupleTypes.reserve(tupleArities.size());
  for (std::size_t arity : tupleArities) {
    AstDeclaration tupleType;
    tupleType.kind = AstDeclarationKind::Class;
    tupleType.name = "Tuple" + std::to_string(arity);
    tupleType.span = noSpan;
    tupleType.parentTypes = {"scala.Tuple"};
    for (std::size_t index = 1; index <= arity; ++index) {
      AstTypeParameter element;
      element.name = "T" + std::to_string(index);
      element.span = noSpan;
      element.variance = TypeVariance::Covariant;
      tupleType.typeParameters.push_back(std::move(element));
      tupleType.parameters.push_back("val _" + std::to_string(index) + ": T" +
                                     std::to_string(index));
    }
    tupleTypes.push_back(std::move(tupleType));
  }

  AstDeclaration product;
  product.kind = AstDeclarationKind::Trait;
  product.name = "Product";
  product.span = noSpan;

  AstDeclaration productArity;
  productArity.kind = AstDeclarationKind::Def;
  productArity.name = "productArity";
  productArity.span = noSpan;
  productArity.declaredType = "Int";

  AstDeclaration productElement;
  productElement.kind = AstDeclarationKind::Def;
  productElement.name = "productElement";
  productElement.span = noSpan;
  productElement.parameters = {"index: Int"};
  productElement.contextualParameters = {false};
  productElement.declaredType = "Object";
  product.members = {std::move(productArity), std::move(productElement)};

  AstDeclaration mirror;
  mirror.kind = AstDeclarationKind::Trait;
  mirror.name = "Mirror";
  mirror.span = noSpan;

  AstDeclaration mirroredTypeMember;
  mirroredTypeMember.kind = AstDeclarationKind::Type;
  mirroredTypeMember.name = "MirroredType";
  mirroredTypeMember.span = noSpan;

  AstDeclaration mirroredMonoType;
  mirroredMonoType.kind = AstDeclarationKind::Type;
  mirroredMonoType.name = "MirroredMonoType";
  mirroredMonoType.span = noSpan;

  AstDeclaration mirroredElemTypes;
  mirroredElemTypes.kind = AstDeclarationKind::Type;
  mirroredElemTypes.name = "MirroredElemTypes";
  mirroredElemTypes.span = noSpan;

  AstDeclaration mirroredLabel;
  mirroredLabel.kind = AstDeclarationKind::Type;
  mirroredLabel.name = "MirroredLabel";
  mirroredLabel.span = noSpan;
  mirroredLabel.upperBound = "String";

  AstDeclaration mirroredElemLabels;
  mirroredElemLabels.kind = AstDeclarationKind::Type;
  mirroredElemLabels.name = "MirroredElemLabels";
  mirroredElemLabels.span = noSpan;
  mirroredElemLabels.upperBound = "scala.Tuple";

  mirror.members = {std::move(mirroredTypeMember), std::move(mirroredMonoType),
                    std::move(mirroredElemTypes), std::move(mirroredLabel),
                    std::move(mirroredElemLabels)};

  AstDeclaration mirrorOf;
  mirrorOf.kind = AstDeclarationKind::Trait;
  mirrorOf.name = "Of";
  mirrorOf.span = noSpan;
  mirrorOf.parentTypes = {"scala.deriving.Mirror"};
  AstTypeParameter ofType;
  ofType.name = "T";
  ofType.span = noSpan;
  mirrorOf.typeParameters.push_back(std::move(ofType));

  AstDeclaration ofMirroredType;
  ofMirroredType.kind = AstDeclarationKind::Type;
  ofMirroredType.name = "MirroredType";
  ofMirroredType.span = noSpan;
  ofMirroredType.declaredType = "T";
  ofMirroredType.hasInitializer = true;

  AstDeclaration ofMirroredMonoType;
  ofMirroredMonoType.kind = AstDeclarationKind::Type;
  ofMirroredMonoType.name = "MirroredMonoType";
  ofMirroredMonoType.span = noSpan;
  ofMirroredMonoType.declaredType = "T";
  ofMirroredMonoType.hasInitializer = true;
  mirrorOf.members = {std::move(ofMirroredType), std::move(ofMirroredMonoType)};

  AstDeclaration mirrorProduct;
  mirrorProduct.kind = AstDeclarationKind::Trait;
  mirrorProduct.name = "Product";
  mirrorProduct.span = noSpan;
  mirrorProduct.parentTypes = {"scala.deriving.Mirror"};

  AstDeclaration productOf;
  productOf.kind = AstDeclarationKind::Trait;
  productOf.name = "ProductOf";
  productOf.span = noSpan;
  productOf.parentTypes = {"scala.deriving.Mirror.Product",
                           "scala.deriving.Mirror.Of[T]"};
  AstTypeParameter mirroredType;
  mirroredType.name = "T";
  mirroredType.span = noSpan;
  productOf.typeParameters.push_back(std::move(mirroredType));

  AstDeclaration productOfMirroredType;
  productOfMirroredType.kind = AstDeclarationKind::Type;
  productOfMirroredType.name = "MirroredType";
  productOfMirroredType.span = noSpan;
  productOfMirroredType.declaredType = "T";
  productOfMirroredType.isOverride = true;
  productOfMirroredType.hasInitializer = true;

  AstDeclaration productOfMirroredMonoType;
  productOfMirroredMonoType.kind = AstDeclarationKind::Type;
  productOfMirroredMonoType.name = "MirroredMonoType";
  productOfMirroredMonoType.span = noSpan;
  productOfMirroredMonoType.declaredType = "T";
  productOfMirroredMonoType.isOverride = true;
  productOfMirroredMonoType.hasInitializer = true;

  AstDeclaration fromProduct;
  fromProduct.kind = AstDeclarationKind::Def;
  fromProduct.name = "fromProduct";
  fromProduct.span = noSpan;
  fromProduct.parameters = {"product: scala.Product"};
  fromProduct.contextualParameters = {false};
  fromProduct.declaredType = "T";
  productOf.members = {std::move(productOfMirroredType),
                       std::move(productOfMirroredMonoType), std::move(fromProduct)};

  AstDeclaration mirrorSum;
  mirrorSum.kind = AstDeclarationKind::Trait;
  mirrorSum.name = "Sum";
  mirrorSum.span = noSpan;
  mirrorSum.parentTypes = {"scala.deriving.Mirror"};

  AstDeclaration sumOf;
  sumOf.kind = AstDeclarationKind::Trait;
  sumOf.name = "SumOf";
  sumOf.span = noSpan;
  sumOf.parentTypes = {"scala.deriving.Mirror.Sum", "scala.deriving.Mirror.Of[T]"};
  AstTypeParameter sumType;
  sumType.name = "T";
  sumType.span = noSpan;
  sumOf.typeParameters.push_back(std::move(sumType));

  AstDeclaration sumOfMirroredType;
  sumOfMirroredType.kind = AstDeclarationKind::Type;
  sumOfMirroredType.name = "MirroredType";
  sumOfMirroredType.span = noSpan;
  sumOfMirroredType.declaredType = "T";
  sumOfMirroredType.isOverride = true;
  sumOfMirroredType.hasInitializer = true;

  AstDeclaration sumOfMirroredMonoType;
  sumOfMirroredMonoType.kind = AstDeclarationKind::Type;
  sumOfMirroredMonoType.name = "MirroredMonoType";
  sumOfMirroredMonoType.span = noSpan;
  sumOfMirroredMonoType.declaredType = "T";
  sumOfMirroredMonoType.isOverride = true;
  sumOfMirroredMonoType.hasInitializer = true;

  AstDeclaration ordinal;
  ordinal.kind = AstDeclarationKind::Def;
  ordinal.name = "ordinal";
  ordinal.span = noSpan;
  ordinal.parameters = {"value: T"};
  ordinal.contextualParameters = {false};
  ordinal.declaredType = "Int";
  sumOf.members = {std::move(sumOfMirroredType), std::move(sumOfMirroredMonoType),
                   std::move(ordinal)};

  std::vector<std::pair<std::string, AstDeclaration>> declarations;
  declarations.emplace_back("scala", std::move(tuple));
  declarations.emplace_back("scala", std::move(emptyTuple));
  if (needsPolyFunction) {
    declarations.emplace_back("scala", std::move(polyFunction));
  }
  for (AstDeclaration& tupleType : tupleTypes) {
    declarations.emplace_back("scala", std::move(tupleType));
  }
  declarations.emplace_back("scala", std::move(product));
  declarations.emplace_back("scala.deriving", std::move(mirror));
  declarations.emplace_back("scala.deriving.Mirror", std::move(mirrorOf));
  declarations.emplace_back("scala.deriving.Mirror", std::move(mirrorProduct));
  declarations.emplace_back("scala.deriving.Mirror", std::move(productOf));
  declarations.emplace_back("scala.deriving.Mirror", std::move(mirrorSum));
  declarations.emplace_back("scala.deriving.Mirror", std::move(sumOf));
  return declarations;
}

} // namespace

Typechecker::Typechecker(support::DiagnosticEngine& diagnostics,
                         const support::SourceManager* sources)
    : diagnostics_(diagnostics), sources_(sources) {}

TypedModule Typechecker::typecheck(const AstModule& module) {
  declaredMemberScopes_.clear();
  memberScopes_.clear();
  globalSymbols_.clear();
  companionTypeNames_.clear();
  derivedGivens_.clear();
  derivedInstances_.clear();
  mirrorDeclarations_.clear();
  localFactoryDeclarations_.clear();
  expressionTypes_.clear();
  contextApplications_.clear();
  inlineApplications_.clear();
  polymorphicFunctionApplications_.clear();
  polymorphicFunctionValueDeclarations_.clear();
  polymorphicFunctionClosures_.clear();
  polymorphicFunctionClosureDeclarations_.clear();
  validatedInlineValueSymbols_.clear();
  directZoneReceiverEscapes_.clear();
  receiverMethodCallSites_.clear();
  implicitReceiverMethodNames_.clear();
  zoneBodiesToAnalyze_.clear();
  currentPackageName_ = module.packageName;
  zoneInferenceDepth_ = 0;
  inlineExpansionDepth_ = 0;
  erasedValueSelectorDepth_ = 0;
  inlineDefinitionDepth_ = 0;
  allowedUninitializedExpression_ = nullptr;
  allowedUninitializedType_ = TypeInfo{};
  TypedModule typed;
  typed.packageName = module.packageName;
  Scope scope;
  addRuntimeBuiltins(scope);
  typed.declarations = standardExceptionDeclarations();
  std::vector<std::pair<std::string, AstDeclaration>> derivationDeclarations =
      standardDerivationDeclarations(module);
  for (const auto& [owner, declaration] : derivationDeclarations) {
    collectDeclaration(declaration, owner, scope);
  }
  const std::function<void(const std::vector<AstDeclaration>&, const std::string&)>
      collectCompanionNames = [&](const std::vector<AstDeclaration>& declarations,
                                  const std::string& owner) {
        std::unordered_map<std::string, unsigned> companionKinds;
        for (const AstDeclaration& declaration : declarations) {
          if (declaration.name.empty()) {
            continue;
          }
          const std::string name = qualify(owner, declaration.name);
          if (declaration.kind == AstDeclarationKind::Class ||
              declaration.kind == AstDeclarationKind::Trait) {
            companionKinds[name] |= 1U;
          } else if (declaration.kind == AstDeclarationKind::Object) {
            companionKinds[name] |= 2U;
          }
        }
        for (const auto& [name, kinds] : companionKinds) {
          if (kinds == 3U) {
            companionTypeNames_.insert(name);
          }
        }
        for (const AstDeclaration& declaration : declarations) {
          if (declaration.kind == AstDeclarationKind::Class ||
              declaration.kind == AstDeclarationKind::Trait ||
              declaration.kind == AstDeclarationKind::Object) {
            collectCompanionNames(declaration.members,
                                  declarationSymbolName(declaration, owner));
          }
        }
      };
  collectCompanionNames(module.declarations, module.packageName);
  for (const AstDeclaration& declaration : module.declarations) {
    if (declaration.kind == AstDeclarationKind::Class ||
        declaration.kind == AstDeclarationKind::Trait) {
      collectDeclaration(declaration, module.packageName, scope);
    }
  }
  for (const AstDeclaration& declaration : module.declarations) {
    if (declaration.kind != AstDeclarationKind::Class &&
        declaration.kind != AstDeclarationKind::Trait) {
      collectDeclaration(declaration, module.packageName, scope);
    }
  }
  for (const AstDeclaration& declaration : module.declarations) {
    applyImport(declaration, scope);
  }
  collectProductMirrors(module.declarations, module.packageName, scope);
  collectSumMirrors(module.declarations, module.packageName, scope);
  collectDerivedGivens(module.declarations, module.packageName, scope);
  for (const auto& [owner, declaration] : derivationDeclarations) {
    typed.declarations.push_back(typecheckDeclaration(declaration, owner, scope));
  }
  for (const AstDeclaration& declaration : module.declarations) {
    typed.declarations.push_back(
        typecheckDeclaration(declaration, module.packageName, scope));
  }
  for (const AstDeclaration& declaration : mirrorDeclarations_) {
    typed.declarations.push_back(
        typecheckDeclaration(declaration, module.packageName, scope));
  }
  for (TypedDeclaration& declaration : localFactoryDeclarations_) {
    typed.declarations.push_back(std::move(declaration));
  }
  for (TypedDeclaration& declaration : polymorphicFunctionClosureDeclarations_) {
    typed.declarations.push_back(std::move(declaration));
  }
  attachDerivedInstances(typed.declarations);
  propagateZoneReceiverEffects();
  for (const AstExpression& body : zoneBodiesToAnalyze_) {
    std::unordered_map<std::string, bool> arenaReferences;
    std::unordered_set<std::string> zoneLocals;
    (void)analyzeZoneExpression(body, arenaReferences, zoneLocals);
  }
  typed.expressionTypes = expressionTypes_;
  typed.contextApplications = contextApplications_;
  typed.inlineApplications = inlineApplications_;
  typed.polymorphicFunctionApplications = polymorphicFunctionApplications_;
  typed.polymorphicFunctionValueDeclarations = polymorphicFunctionValueDeclarations_;
  typed.polymorphicFunctionClosures = polymorphicFunctionClosures_;
  return typed;
}

TypedDeclaration Typechecker::typecheckDeclaration(const AstDeclaration& declaration,
                                                   const std::string& owner,
                                                   Scope& scope) {
  if (declaration.name.empty() && declaration.kind != AstDeclarationKind::Import) {
    diagnostics_.error(declaration.span, "declaration has no name");
  }

  TypedDeclaration typed;
  typed.isTransparent = declaration.isTransparent;
  typed.isInline = declaration.isInline;
  typed.kind = declaration.kind;
  typed.name = declaration.name;
  typed.symbolName = declaration.kind == AstDeclarationKind::Import
                         ? importSymbolName(declaration, scope)
                         : declarationSymbolName(declaration, owner);
  typed.span = declaration.span;
  typed.importPath = declaration.importPath;
  typed.importSelectors = declaration.importSelectors;
  typed.importGivenTypes = declaration.importGivenTypes;
  typed.importsGivens = declaration.importsGivens;
  typed.importsWildcard = declaration.importsWildcard;
  typed.parentArguments = declaration.parentArguments;
  typed.derivedTypes = declaration.derivedTypes;
  typed.isOverride = declaration.isOverride;
  typed.isGiven = declaration.isGiven;
  typed.isAnonymousGiven = declaration.isAnonymousGiven;
  typed.hasInitializer = declaration.hasInitializer;
  typed.initializer = declaration.initializer;
  typed.constructorBody = declaration.constructorBody;
  typed.classBodyItems = declaration.classBodyItems;

  Scope signatureScope = scope;
  typed.typeParameters = resolvedTypeParameters(declaration.typeParameters,
                                                typed.symbolName, signatureScope);
  typed.parameters = resolvedParameters(declaration.parameters, signatureScope,
                                        &typed.parameterTypes, &declaration.span);
  typed.contextualParameters = declaration.contextualParameters;
  typed.contextualParameters.resize(typed.parameters.size(), false);
  typed.inlineParameters = declaration.inlineParameters;
  typed.inlineParameters.resize(typed.parameters.size(), false);
  typed.parameterClauseSizes = declaration.parameterClauseSizes;
  typed.contextualParameterClauses =
      declaration.contextualParameterClauses;
  bool inlineValueInitializerSupported = true;
  if (typed.isInline) {
    if (typed.kind == AstDeclarationKind::Def && !typed.hasInitializer) {
      diagnostics_.error(declaration.span,
                         "inline method requires an implementation");
    } else if (typed.kind == AstDeclarationKind::Val) {
      if (!typed.hasInitializer) {
        diagnostics_.error(declaration.span,
                           "inline value requires an initializer");
        inlineValueInitializerSupported = false;
      } else if (!isSupportedInlineValueInitializer(declaration.initializer,
                                                     signatureScope)) {
        diagnostics_.error(
            declaration.initializer.span,
            "inline value initializer must use literals, operators, and "
            "previously defined inline values");
        inlineValueInitializerSupported = false;
      }
    }
  }
  for (std::size_t i = 0; i < typed.parameters.size(); ++i) {
    const std::string name = parameterName(typed.parameters[i]);
    if (name.empty()) {
      continue;
    }
    SymbolInfo parameter;
    parameter.kind = parameterDeclarationKind(typed.parameters[i]);
    parameter.name = name;
    parameter.symbolName = qualify(typed.symbolName, name);
    parameter.type = i < typed.parameterTypes.size()
                         ? typed.parameterTypes[i]
                         : TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
    parameter.isContextParameter = typed.contextualParameters[i];
    parameter.isInlineParameter = typed.inlineParameters[i];
    parameter.isLexicalValue = true;
    signatureScope[name] = std::move(parameter);
  }

  TypeInfo declared = typeFromDeclaredName(declaration.declaredType, &signatureScope,
                                           &declaration.span);
  TypeInfo lowerBound =
      typeFromDeclaredName(declaration.lowerBound, &signatureScope, &declaration.span);
  TypeInfo upperBound =
      typeFromDeclaredName(declaration.upperBound, &signatureScope, &declaration.span);
  typed.declaredType = declared.kind == SimpleTypeKind::Unknown
                           ? declaration.declaredType
                           : declared.name;
  typed.lowerBound = lowerBound.kind == SimpleTypeKind::Unknown ? declaration.lowerBound
                                                                : lowerBound.name;
  typed.upperBound = upperBound.kind == SimpleTypeKind::Unknown ? declaration.upperBound
                                                                : upperBound.name;
  const bool hasUnsupportedAbstractDependentSignature =
      ((declared.pathDependent || declared.typeProjection) &&
       declared.abstractTypeMember && declared.runtimeName.empty()) ||
      std::any_of(typed.parameterTypes.begin(), typed.parameterTypes.end(),
                  [](const TypeInfo& type) {
                    return (type.pathDependent || type.typeProjection) &&
                           type.abstractTypeMember && type.runtimeName.empty();
                  });
  if ((declaration.kind == AstDeclarationKind::Def ||
       declaration.kind == AstDeclarationKind::Val ||
       declaration.kind == AstDeclarationKind::Var) &&
      hasUnsupportedAbstractDependentSignature) {
    diagnostics_.error(
        declaration.span,
        "abstract dependent runtime type requires a concrete reference upper "
        "bound");
  }
  if (isClassLikeDeclaration(declaration.kind)) {
    validateInheritance(declaration, typed, signatureScope);
  }
  if (declaration.kind == AstDeclarationKind::Class) {
    Scope parentArgumentScope = signatureScope;
    addParametersToScope(declaration, parentArgumentScope);
    validateParentConstructorArguments(declaration, typed, parentArgumentScope);
  } else if (!declaration.parentArguments.empty()) {
    diagnostics_.error(declaration.span,
                       "parent constructor arguments are only supported for "
                       "classes in this MVP");
  }
  TypeInfo inferred;
  std::optional<TypedPolymorphicFunctionApplication> polymorphicFunctionValue;
  switch (declaration.kind) {
  case AstDeclarationKind::Package:
  case AstDeclarationKind::Import:
    inferred = TypeInfo{SimpleTypeKind::Unit, "Unit"};
    break;
  case AstDeclarationKind::Object:
  case AstDeclarationKind::Class:
  case AstDeclarationKind::Trait:
    inferred = TypeInfo{SimpleTypeKind::Object, typed.symbolName};
    break;
  case AstDeclarationKind::Type:
    inferred = declaration.hasInitializer
                   ? declared
                   : TypeInfo{SimpleTypeKind::Object, typed.symbolName};
    break;
  case AstDeclarationKind::Def:
  case AstDeclarationKind::Val:
  case AstDeclarationKind::Var:
    if (declaration.hasInitializer) {
      Scope expressionScope = signatureScope;
      if (declaration.kind == AstDeclarationKind::Def) {
        addParametersToScope(declaration, expressionScope);
      }
      const bool inlineDefinition =
          declaration.kind == AstDeclarationKind::Def && declaration.isInline;
      if (inlineDefinition) {
        ++inlineDefinitionDepth_;
      }
      const AstExpression* previousUninitializedExpression =
          allowedUninitializedExpression_;
      TypeInfo previousUninitializedType = allowedUninitializedType_;
      const bool uninitializedInitializer =
          isUninitializedExpression(declaration.initializer, expressionScope);
      if (uninitializedInitializer) {
        const auto enclosing = globalSymbols_.find(owner);
        const bool mutableField =
            declaration.kind == AstDeclarationKind::Var &&
            enclosing != globalSymbols_.end() &&
            (enclosing->second.kind == AstDeclarationKind::Class ||
             enclosing->second.kind == AstDeclarationKind::Object);
        if (mutableField) {
          allowedUninitializedExpression_ = &declaration.initializer;
          allowedUninitializedType_ = declared;
          if (declared.kind == SimpleTypeKind::Unknown) {
            diagnostics_.error(
                declaration.initializer.span,
                "uninitialized field requires an explicit declared type");
          }
        }
      }
      if (declaration.kind == AstDeclarationKind::Val) {
        if (declaration.initializer.kind == AstExpressionKind::PolymorphicFunction) {
          polymorphicFunctionValue = typecheckPolymorphicFunctionLiteral(
              declaration.initializer, {}, declaration.initializer.span,
              expressionScope,
              "stored polymorphic function literal must have the form "
              "[A] => (value: A) => body");
          if (polymorphicFunctionValue.has_value()) {
            polymorphicFunctionValue->definitionOwnerName = owner;
          }
        } else {
          polymorphicFunctionValue =
              polymorphicFunctionAlias(declaration.initializer, expressionScope);
        }
        if (polymorphicFunctionValue.has_value()) {
          const SymbolInfo* polyFunction = typeSymbolForDeclaredName(
              std::string(support::StdNames::ScalaPolyFunction), &expressionScope);
          inferred = polyFunction == nullptr
                         ? TypeInfo{SimpleTypeKind::Object,
                                    std::string(support::StdNames::ScalaPolyFunction)}
                         : polyFunction->type;
          if (declared.polymorphicFunctionType) {
            inferred = declared;
          }
        } else {
          inferred = inferExpressionType(
              declaration.initializer, expressionScope,
              declared.kind == SimpleTypeKind::Unknown ? nullptr : &declared);
        }
      } else {
        inferred = inferExpressionType(
            declaration.initializer, expressionScope,
            declared.kind == SimpleTypeKind::Unknown ? nullptr : &declared);
      }
      allowedUninitializedExpression_ = previousUninitializedExpression;
      allowedUninitializedType_ = std::move(previousUninitializedType);
      if (inlineDefinition) {
        --inlineDefinitionDepth_;
      }
    } else {
      inferred = TypeInfo{SimpleTypeKind::Unit, "Unit"};
    }
    break;
  }

  typed.isCompilerKnownPolymorphicFunctionValue = polymorphicFunctionValue.has_value();
  if (polymorphicFunctionValue.has_value() && declared.polymorphicFunctionType &&
      !polymorphicFunctionMatchesDeclaredType(declared, *polymorphicFunctionValue)) {
    const std::string expectedResult =
        declared.typeArguments.size() == 2 ? declared.typeArguments[1].name : "Unknown";
    diagnostics_.error(declaration.span,
                       "polymorphic function result type " +
                           polymorphicFunctionValue->resultType.name +
                           " does not conform to declared result type " +
                           expectedResult);
  }

  const bool declaredIsValueType = declaration.kind == AstDeclarationKind::Def ||
                                   declaration.kind == AstDeclarationKind::Val ||
                                   declaration.kind == AstDeclarationKind::Var;
  if (declaredIsValueType && declared.kind == SimpleTypeKind::Unknown) {
    inferred = widenSoftUnion(inferred);
  }
  if (declaredIsValueType && declared.kind != SimpleTypeKind::Unknown) {
    const bool storedAny = (declaration.kind == AstDeclarationKind::Val ||
                            declaration.kind == AstDeclarationKind::Var) &&
                           isAnyArrayElementType(declared);
    const bool initializerConforms = storedAny ? isSupportedAnyArrayValueType(inferred)
                                               : isAssignable(declared, inferred);
    if (declaration.hasInitializer && !initializerConforms) {
      diagnostics_.error(declaration.span, "initializer type " + inferred.name +
                                               " does not conform to declared type " +
                                               declared.name);
    }
    typed.inferredType = declared;
  } else {
    typed.inferredType = inferred;
  }
  if (declaration.kind == AstDeclarationKind::Import &&
      (declaration.name == "_" || declaration.importsWildcard ||
       declaration.importsGivens || !declaration.importGivenTypes.empty())) {
    const std::string& importOwner = typed.symbolName;
    for (const auto& [symbolName, symbol] : globalSymbols_) {
      if (!isDirectMemberOf(symbolName, importOwner)) {
        continue;
      }
      const bool wildcardMatch =
          (declaration.name == "_" || declaration.importsWildcard) &&
          !symbol.isGiven;
      const bool givenMatch =
          givenImportMatches(declaration, symbol, signatureScope);
      if (!wildcardMatch && !givenMatch) {
        continue;
      }
      TypedDeclaration imported;
      imported.kind = AstDeclarationKind::Import;
      imported.name = memberNameOf(symbolName);
      imported.symbolName = symbol.symbolName;
      imported.span = declaration.span;
      imported.declaredType = symbol.type.name;
      imported.inferredType = symbol.type;
      imported.typeParameters = symbol.typeParameters;
      imported.parameters = symbol.parameters;
      imported.parameterTypes = symbol.parameterTypes;
      imported.contextualParameters = symbol.contextualParameters;
      imported.inlineParameters = symbol.inlineParameters;
      imported.parameterClauseSizes = symbol.parameterClauseSizes;
      imported.contextualParameterClauses =
          symbol.contextualParameterClauses;
      imported.isTransparent = symbol.isTransparent;
      imported.isGiven = symbol.isGiven;
      imported.isAnonymousGiven = symbol.isAnonymousGiven;
      typed.members.push_back(std::move(imported));
    }
  }
  if (declaration.kind == AstDeclarationKind::Type &&
      lowerBound.kind != SimpleTypeKind::Unknown &&
      upperBound.kind != SimpleTypeKind::Unknown &&
      !isAssignable(upperBound, lowerBound)) {
    diagnostics_.error(declaration.span, "type member " + declaration.name +
                                             " lower bound " + lowerBound.name +
                                             " does not conform to upper bound " +
                                             upperBound.name);
  }

  if (!declaration.name.empty() && declaration.kind != AstDeclarationKind::Package &&
      declaration.kind != AstDeclarationKind::Import) {
    SymbolInfo symbol;
    symbol.kind = declaration.kind;
    symbol.name = declaration.name;
    symbol.symbolName = typed.symbolName;
    symbol.parentSymbolName =
        isClassLikeDeclaration(declaration.kind) ? typed.declaredType : std::string{};
    symbol.parentSymbolNames = isClassLikeDeclaration(declaration.kind)
                                   ? typed.parentTypes
                                   : std::vector<std::string>{};
    symbol.parentTypes = isClassLikeDeclaration(declaration.kind)
                             ? typed.parentTypeInfos
                             : std::vector<TypeInfo>{};
    symbol.type = typed.inferredType;
    symbol.lowerBound = lowerBound;
    symbol.upperBound = upperBound;
    symbol.typeParameters = typed.typeParameters;
    symbol.parameters = typed.parameters;
    symbol.parameterTypes = typed.parameterTypes;
    symbol.contextualParameters = typed.contextualParameters;
    symbol.inlineParameters = typed.inlineParameters;
    symbol.parameterClauseSizes = declaration.parameterClauseSizes;
    symbol.contextualParameterClauses = declaration.contextualParameterClauses;
    symbol.isGiven = typed.isGiven;
    symbol.isAnonymousGiven = typed.isAnonymousGiven;
    symbol.isTransparent = declaration.isTransparent;
    symbol.isInline = declaration.isInline;
    if (declaration.isInline) {
      symbol.inlineBody = declaration.initializer;
    }
    if (polymorphicFunctionValue.has_value()) {
      symbol.polymorphicFunctionValue =
          std::make_shared<TypedPolymorphicFunctionApplication>(
              std::move(*polymorphicFunctionValue));
    }
    if (declaration.isInline && declaration.kind == AstDeclarationKind::Val &&
        typed.inferredType.kind == SimpleTypeKind::Boolean) {
      symbol.specializedBooleanValue =
          constantBooleanValue(declaration.initializer, signatureScope);
    }
    if (declaration.isInline && declaration.kind == AstDeclarationKind::Val &&
        isIntegerConstantType(typed.inferredType.kind)) {
      symbol.specializedIntegerValue =
          constantIntegerValue(declaration.initializer, signatureScope);
    }
    if (declaration.isInline && declaration.kind == AstDeclarationKind::Val &&
        isFloatingConstantType(typed.inferredType.kind)) {
      symbol.specializedFloatingValue =
          constantFloatingValue(declaration.initializer, signatureScope);
    }
    if (declaration.isInline && declaration.kind == AstDeclarationKind::Val &&
        typed.inferredType.kind == SimpleTypeKind::String) {
      symbol.specializedStringValue =
          constantStringValue(declaration.initializer, signatureScope);
    }
    if (declaration.isInline && declaration.kind == AstDeclarationKind::Val &&
        typed.inferredType.kind == SimpleTypeKind::Char) {
      symbol.specializedCharValue =
          constantCharValue(declaration.initializer, signatureScope);
    }
    if (auto enclosing = globalSymbols_.find(owner);
        enclosing != globalSymbols_.end() &&
        enclosing->second.kind == AstDeclarationKind::Object) {
      symbol.isModuleMember = declaration.kind == AstDeclarationKind::Val ||
                              declaration.kind == AstDeclarationKind::Var;
    }
    symbol.hasImplementation =
        declarationHasImplementation(declaration.kind, declaration.hasInitializer);
    globalSymbols_[typed.symbolName] = symbol;
    const bool companionObject =
        declaration.kind == AstDeclarationKind::Object &&
        companionTypeNames_.contains(qualify(owner, declaration.name));
    if (!companionObject) {
      scope[declaration.name] = std::move(symbol);
    }
    if (declaration.isInline && declaration.kind == AstDeclarationKind::Val &&
        declaration.hasInitializer && inlineValueInitializerSupported) {
      validatedInlineValueSymbols_.insert(typed.symbolName);
    }
  }

  Scope ownMemberScope;
  const auto collectMember = [&](const AstDeclaration& member) {
    Scope memberResolutionScope = signatureScope;
    mergeScope(memberResolutionScope, ownMemberScope);
    collectDeclaration(member, typed.symbolName, memberResolutionScope);
    if (auto collected = memberResolutionScope.find(member.name);
        collected != memberResolutionScope.end()) {
      ownMemberScope[member.name] = std::move(collected->second);
    }
  };
  for (const AstDeclaration& member : declaration.members) {
    if (member.kind == AstDeclarationKind::Type) {
      collectMember(member);
    }
  }
  for (const AstDeclaration& member : declaration.members) {
    if (member.kind != AstDeclarationKind::Type) {
      collectMember(member);
    }
  }
  mergeInheritedMembers(ownMemberScope, typed.parentTypes, typed.parentTypeInfos);
  validateInheritedMemberCompatibility(declaration, typed.parentTypes,
                                       typed.parentTypeInfos, ownMemberScope);
  if (declaration.kind == AstDeclarationKind::Class) {
    for (std::size_t parameterIndex = 0; parameterIndex < typed.parameters.size();
         ++parameterIndex) {
      const std::string& parameter = typed.parameters[parameterIndex];
      const std::string name = parameterName(parameter);
      if (name.empty()) {
        continue;
      }
      SymbolInfo field;
      field.kind = parameterDeclarationKind(parameter);
      field.name = name;
      field.symbolName = qualify(typed.symbolName, name);
      field.type = parameterType(parameter, &signatureScope);
      field.isContextParameter = parameterIndex < typed.contextualParameters.size() &&
                                 typed.contextualParameters[parameterIndex];
      field.isInstanceMember = true;
      ownMemberScope[name] = std::move(field);

      const std::vector<SymbolInfo> inherited = specializedInheritedMembers(
          typed.parentTypes, typed.parentTypeInfos, name, ownMemberScope);
      const bool hasConcreteInheritedMember =
          std::any_of(inherited.begin(), inherited.end(), [](const SymbolInfo& member) {
            return member.hasImplementation;
          });
      if (hasConcreteInheritedMember) {
        continue;
      }
      for (const SymbolInfo& specialized : inherited) {
        if (specialized.kind != AstDeclarationKind::Val &&
            specialized.kind != AstDeclarationKind::Var) {
          diagnostics_.error(declaration.span,
                             "constructor parameter " + name +
                                 " cannot implement inherited non-value member");
          continue;
        }
        const bool validParameterKind = specialized.kind == AstDeclarationKind::Var
                                            ? isExplicitVarParameter(parameter)
                                            : isExplicitValParameter(parameter) ||
                                                  isExplicitVarParameter(parameter);
        if (!validParameterKind) {
          diagnostics_.error(
              declaration.span,
              "class " + declaration.name + " must implement abstract " +
                  std::string(specialized.kind == AstDeclarationKind::Var ? "variable "
                                                                          : "value ") +
                  name + " with a " +
                  (specialized.kind == AstDeclarationKind::Var ? "var" : "val or var") +
                  " constructor parameter or class member");
          continue;
        }
        const TypeInfo actual = parameterType(parameter, &signatureScope);
        if (!typesMatchForOverride(specialized.type, actual)) {
          const std::string memberKind =
              specialized.kind == AstDeclarationKind::Var ? "variable" : "value";
          diagnostics_.error(declaration.span,
                             "constructor " + memberKind + " " + name + " type " +
                                 actual.name + " does not match inherited " +
                                 memberKind + " type " + specialized.type.name);
          continue;
        }
        if (std::find(typed.accessorParameters.begin(), typed.accessorParameters.end(),
                      name) == typed.accessorParameters.end()) {
          typed.accessorParameters.push_back(name);
        }
      }
    }
  }
  Scope memberScope = signatureScope;
  mergeScope(memberScope, ownMemberScope);
  for (const AstDeclaration& member : declaration.members) {
    applyImport(member, memberScope);
  }
  if (declaration.kind == AstDeclarationKind::Class ||
      declaration.kind == AstDeclarationKind::Trait) {
    SymbolInfo self;
    self.kind = AstDeclarationKind::Val;
    self.name = "this";
    self.symbolName = typed.symbolName + ".this";
    self.type = TypeInfo{SimpleTypeKind::Object, typed.symbolName};
    memberScope[self.name] = std::move(self);
    if (!typed.declaredType.empty()) {
      SymbolInfo super;
      super.kind = AstDeclarationKind::Val;
      super.name = "super";
      super.symbolName = typed.symbolName + ".super";
      super.type = typed.parentTypeInfos.empty()
                       ? TypeInfo{SimpleTypeKind::Object, typed.parentTypes.back()}
                       : typed.parentTypeInfos.back();
      super.parentSymbolNames = typed.parentTypes;
      super.parentTypes = typed.parentTypeInfos;
      memberScope[super.name] = std::move(super);
    }
    for (std::size_t i = 0; i < typed.parentTypes.size(); ++i) {
      const std::string& parentType = typed.parentTypes[i];
      SymbolInfo qualifiedSuper;
      qualifiedSuper.kind = AstDeclarationKind::Val;
      qualifiedSuper.name = "super:" + parentType;
      qualifiedSuper.symbolName = typed.symbolName + ".super[" + parentType + "]";
      qualifiedSuper.type = i < typed.parentTypeInfos.size()
                                ? typed.parentTypeInfos[i]
                                : TypeInfo{SimpleTypeKind::Object, parentType};
      memberScope[qualifiedSuper.name] = std::move(qualifiedSuper);
    }
  }
  for (const AstDeclaration& member : declaration.members) {
    if (declaration.kind == AstDeclarationKind::Trait &&
        member.kind == AstDeclarationKind::Def && !member.hasInitializer &&
        member.declaredType.empty()) {
      diagnostics_.error(member.span, "abstract trait method " + member.name +
                                          " requires an explicit return type");
    }
    if (declaration.kind == AstDeclarationKind::Trait &&
        member.kind == AstDeclarationKind::Val && !member.hasInitializer &&
        member.declaredType.empty()) {
      diagnostics_.error(member.span, "abstract trait value " + member.name +
                                          " requires an explicit type");
    }
    if (declaration.kind == AstDeclarationKind::Trait &&
        member.kind == AstDeclarationKind::Var && !member.hasInitializer &&
        member.declaredType.empty()) {
      diagnostics_.error(member.span, "abstract trait variable " + member.name +
                                          " requires an explicit type");
    }
    TypedDeclaration typedMember =
        typecheckDeclaration(member, typed.symbolName, memberScope);
    if (auto resolved = memberScope.find(typedMember.name);
        resolved != memberScope.end()) {
      ownMemberScope[typedMember.name] = resolved->second;
      globalSymbols_[resolved->second.symbolName] = resolved->second;
    }
    const std::vector<SymbolInfo> inherited = specializedInheritedMembers(
        typed.parentTypes, typed.parentTypeInfos, typedMember.name, memberScope);
    if (!inherited.empty()) {
      for (const SymbolInfo& specialized : inherited) {
        const bool matchingMethod = typedMember.kind == AstDeclarationKind::Def &&
                                    specialized.kind == AstDeclarationKind::Def;
        const bool matchingValue = (typedMember.kind == AstDeclarationKind::Val ||
                                    typedMember.kind == AstDeclarationKind::Var) &&
                                   (specialized.kind == AstDeclarationKind::Val ||
                                    specialized.kind == AstDeclarationKind::Var);
        if (matchingMethod || matchingValue) {
          if (!specialized.type.runtimeName.empty()) {
            typedMember.inferredType.runtimeName = specialized.type.runtimeName;
          }
          if (matchingMethod) {
            const std::size_t bridgedParameters = std::min(
                typedMember.parameterTypes.size(), specialized.parameterTypes.size());
            for (std::size_t i = 0; i < bridgedParameters; ++i) {
              if (!specialized.parameterTypes[i].runtimeName.empty()) {
                typedMember.parameterTypes[i].runtimeName =
                    specialized.parameterTypes[i].runtimeName;
              }
            }
          }
          auto unsupportedBridge = [](const TypeInfo& type) {
            return !type.runtimeName.empty() && type.runtimeName != type.name &&
                   !isBoxablePrimitiveType(type.kind) &&
                   type.kind != SimpleTypeKind::Object &&
                   type.kind != SimpleTypeKind::String;
          };
          if (unsupportedBridge(typedMember.inferredType) ||
              std::any_of(typedMember.parameterTypes.begin(),
                          typedMember.parameterTypes.end(), unsupportedBridge)) {
            diagnostics_.error(
                typedMember.span,
                "dependent ABI boxing is currently supported only for scalar and "
                "reference types");
          }
        }
        if (typedMember.kind == AstDeclarationKind::Type &&
            specialized.kind == AstDeclarationKind::Type &&
            !typedMember.hasInitializer && !specialized.hasImplementation) {
          if (typedMember.lowerBound.empty() &&
              specialized.lowerBound.kind != SimpleTypeKind::Unknown) {
            typedMember.lowerBound = specialized.lowerBound.name;
          }
          if (typedMember.upperBound.empty() &&
              specialized.upperBound.kind != SimpleTypeKind::Unknown) {
            typedMember.upperBound = specialized.upperBound.name;
          }
        }
        validateOverride(typedMember, specialized);
        if ((specialized.kind == AstDeclarationKind::Val ||
             specialized.kind == AstDeclarationKind::Var) &&
            (typedMember.kind == AstDeclarationKind::Val ||
             typedMember.kind == AstDeclarationKind::Var) &&
            typedMember.hasInitializer) {
          typedMember.needsAccessor = true;
        }
      }
    } else if (typedMember.isOverride) {
      diagnostics_.error(typedMember.span,
                         "override " + typedMember.name + " overrides nothing");
    }
    SymbolInfo updated;
    updated.kind = typedMember.kind;
    updated.name = typedMember.name;
    updated.symbolName = typedMember.symbolName;
    updated.parentSymbolName = isClassLikeDeclaration(typedMember.kind)
                                   ? typedMember.declaredType
                                   : std::string{};
    updated.parentSymbolNames = isClassLikeDeclaration(typedMember.kind)
                                    ? typedMember.parentTypes
                                    : std::vector<std::string>{};
    updated.parentTypes = isClassLikeDeclaration(typedMember.kind)
                              ? typedMember.parentTypeInfos
                              : std::vector<TypeInfo>{};
    updated.type = typedMember.inferredType;
    updated.lowerBound = typeFromDeclaredName(typedMember.lowerBound, &memberScope);
    updated.upperBound = typeFromDeclaredName(typedMember.upperBound, &memberScope);
    updated.typeParameters = typedMember.typeParameters;
    updated.parameters = typedMember.parameters;
    updated.parameterTypes = typedMember.parameterTypes;
    updated.contextualParameters = typedMember.contextualParameters;
    updated.inlineParameters = typedMember.inlineParameters;
    updated.parameterClauseSizes = typedMember.parameterClauseSizes;
    updated.contextualParameterClauses =
        typedMember.contextualParameterClauses;
    updated.isGiven = typedMember.isGiven;
    updated.isTransparent = typedMember.isTransparent;
    updated.isInline = typedMember.isInline;
    if (auto typedSymbol = memberScope.find(typedMember.name);
        typedSymbol != memberScope.end()) {
      updated.polymorphicFunctionValue = typedSymbol->second.polymorphicFunctionValue;
    }
    if (typedMember.isInline) {
      updated.inlineBody = member.initializer;
    }
    if (typedMember.isInline && typedMember.kind == AstDeclarationKind::Val &&
        typedMember.inferredType.kind == SimpleTypeKind::Boolean) {
      updated.specializedBooleanValue =
          constantBooleanValue(member.initializer, memberScope);
    }
    if (typedMember.isInline && typedMember.kind == AstDeclarationKind::Val &&
        isIntegerConstantType(typedMember.inferredType.kind)) {
      updated.specializedIntegerValue =
          constantIntegerValue(member.initializer, memberScope);
    }
    if (typedMember.isInline && typedMember.kind == AstDeclarationKind::Val &&
        isFloatingConstantType(typedMember.inferredType.kind)) {
      updated.specializedFloatingValue =
          constantFloatingValue(member.initializer, memberScope);
    }
    if (typedMember.isInline && typedMember.kind == AstDeclarationKind::Val &&
        typedMember.inferredType.kind == SimpleTypeKind::String) {
      updated.specializedStringValue =
          constantStringValue(member.initializer, memberScope);
    }
    if (typedMember.isInline && typedMember.kind == AstDeclarationKind::Val &&
        typedMember.inferredType.kind == SimpleTypeKind::Char) {
      updated.specializedCharValue =
          constantCharValue(member.initializer, memberScope);
    }
    updated.isAnonymousGiven = typedMember.isAnonymousGiven;
    updated.isModuleMember = declaration.kind == AstDeclarationKind::Object &&
                             (typedMember.kind == AstDeclarationKind::Val ||
                              typedMember.kind == AstDeclarationKind::Var);
    updated.isInstanceMember = (declaration.kind == AstDeclarationKind::Class ||
                                declaration.kind == AstDeclarationKind::Trait) &&
                               (typedMember.kind == AstDeclarationKind::Def ||
                                typedMember.kind == AstDeclarationKind::Val ||
                                typedMember.kind == AstDeclarationKind::Var);
    updated.hasImplementation =
        declarationHasImplementation(typedMember.kind, typedMember.hasInitializer);
    if ((declaration.kind == AstDeclarationKind::Class ||
         declaration.kind == AstDeclarationKind::Object) &&
        (updated.kind == AstDeclarationKind::Def ||
         updated.kind == AstDeclarationKind::Val ||
         updated.kind == AstDeclarationKind::Var) &&
        runtimeSignatureUsesAbstractType(updated)) {
      diagnostics_.error(
          typedMember.span,
          std::string(declaration.kind == AstDeclarationKind::Class ? "class "
                                                                    : "object ") +
              declaration.name + " member " + typedMember.name +
              " uses an unresolved abstract type in its runtime signature");
    }
    const bool companionObjectMember =
        typedMember.kind == AstDeclarationKind::Object &&
        companionTypeNames_.contains(qualify(typed.symbolName, typedMember.name));
    if (companionObjectMember) {
      globalSymbols_[typedMember.symbolName] = std::move(updated);
    } else {
      memberScope[member.name] = updated;
      ownMemberScope[member.name] = std::move(updated);
      globalSymbols_[typedMember.symbolName] = memberScope[member.name];
    }
    if ((declaration.kind == AstDeclarationKind::Class ||
         declaration.kind == AstDeclarationKind::Trait) &&
        typedMember.kind == AstDeclarationKind::Def && typedMember.hasInitializer) {
      recordDirectZoneReceiverEscape(member, typedMember);
    }
    typed.members.push_back(std::move(typedMember));
  }

  if (declaration.kind == AstDeclarationKind::Class ||
      declaration.kind == AstDeclarationKind::Object) {
    std::unordered_set<std::string> declaredRuntimeMembers;
    for (const AstDeclaration& member : declaration.members) {
      if (member.kind == AstDeclarationKind::Def ||
          member.kind == AstDeclarationKind::Val ||
          member.kind == AstDeclarationKind::Var) {
        declaredRuntimeMembers.insert(member.name);
      }
    }
    if (declaration.kind == AstDeclarationKind::Class) {
      for (const std::string& parameter : declaration.parameters) {
        declaredRuntimeMembers.insert(parameterName(parameter));
      }
    }
    for (const auto& [name, member] : ownMemberScope) {
      if ((member.kind == AstDeclarationKind::Type ||
           member.kind == AstDeclarationKind::Def ||
           member.kind == AstDeclarationKind::Val ||
           member.kind == AstDeclarationKind::Var) &&
          !member.hasImplementation) {
        diagnostics_.error(declaration.span,
                           std::string(declaration.kind == AstDeclarationKind::Class
                                           ? "class "
                                           : "object ") +
                               declaration.name + " must implement abstract " +
                               (member.kind == AstDeclarationKind::Type ? "type member "
                                : member.kind == AstDeclarationKind::Val ? "value "
                                : member.kind == AstDeclarationKind::Var ? "variable "
                                                                         : "method ") +
                               name);
      }
      if ((member.kind == AstDeclarationKind::Def ||
           member.kind == AstDeclarationKind::Val ||
           member.kind == AstDeclarationKind::Var) &&
          member.hasImplementation && !declaredRuntimeMembers.contains(name)) {
        auto original = globalSymbols_.find(member.symbolName);
        if (original != globalSymbols_.end() &&
            runtimeSignatureUsesAbstractType(original->second)) {
          diagnostics_.error(
              declaration.span,
              std::string(declaration.kind == AstDeclarationKind::Class ? "class "
                                                                        : "object ") +
                  declaration.name + " must override inherited member " + name +
                  " to specialize its abstract type-member runtime signature");
        }
      }
    }
  }

  if (declaration.kind == AstDeclarationKind::Class ||
      declaration.kind == AstDeclarationKind::Object) {
    Scope constructorScope = memberScope;
    for (const AstExpression& expression : declaration.constructorBody) {
      (void)inferExpressionType(expression, constructorScope);
    }
  }

  if (declaration.kind == AstDeclarationKind::Class ||
      declaration.kind == AstDeclarationKind::Trait) {
    validateVariance(declaration, typed);
  }

  if (declaration.kind == AstDeclarationKind::Object ||
      declaration.kind == AstDeclarationKind::Class ||
      declaration.kind == AstDeclarationKind::Trait) {
    declaredMemberScopes_[typed.symbolName] = ownMemberScope;
    memberScopes_[typed.symbolName] = std::move(ownMemberScope);
  }

  return typed;
}

void Typechecker::addRuntimeBuiltins(Scope& scope) {
  SymbolInfo option;
  option.kind = AstDeclarationKind::Trait;
  option.name = "Option";
  option.symbolName = std::string(support::StdNames::ScalaOption);
  option.type = TypeInfo{SimpleTypeKind::Object, option.symbolName};
  option.parentSymbolName = std::string(support::StdNames::JavaLangObject);
  option.parentSymbolNames = {option.parentSymbolName};
  TypeParameterInfo optionParameter;
  optionParameter.name = "A";
  optionParameter.symbolName = option.symbolName + ".A";
  optionParameter.lowerBound = TypeInfo{SimpleTypeKind::Nothing, "Nothing"};
  optionParameter.upperBound = TypeInfo{SimpleTypeKind::Object, "Object"};
  optionParameter.variance = TypeVariance::Covariant;
  option.typeParameters.push_back(optionParameter);
  scope[option.name] = option;
  globalSymbols_[option.symbolName] = option;
  declaredMemberScopes_[option.symbolName] = {};
  memberScopes_[option.symbolName] = {};

  SymbolInfo some;
  some.kind = AstDeclarationKind::Class;
  some.name = "Some";
  some.symbolName = std::string(support::StdNames::ScalaSome);
  some.type = TypeInfo{SimpleTypeKind::Object, some.symbolName};
  some.parentSymbolName = option.symbolName;
  some.parentSymbolNames = {option.symbolName};
  TypeParameterInfo someParameter = optionParameter;
  someParameter.symbolName = some.symbolName + ".A";
  some.typeParameters.push_back(someParameter);
  TypeInfo someValueType{SimpleTypeKind::Object, "A"};
  someValueType.runtimeName = "Object";
  someValueType.typeParameter = true;
  someValueType.typeParameterSymbolName = someParameter.symbolName;
  some.parameters = {"value: A"};
  some.parameterTypes = {someValueType};
  some.parameterClauseSizes = {1};
  some.contextualParameterClauses = {false};
  TypeInfo optionParent{SimpleTypeKind::Object,
                        option.symbolName + " [ A ]"};
  optionParent.runtimeName = option.symbolName;
  optionParent.typeConstructorName = option.symbolName;
  optionParent.typeArguments = {someValueType};
  some.parentTypes = {optionParent};
  scope[some.name] = some;
  globalSymbols_[some.symbolName] = some;

  Scope someMembers;
  SymbolInfo someValue;
  someValue.kind = AstDeclarationKind::Val;
  someValue.name = std::string(support::StdNames::OptionValue);
  someValue.symbolName = some.symbolName + "." + someValue.name;
  someValue.type = someValueType;
  someValue.hasImplementation = true;
  someValue.isInstanceMember = true;
  someMembers[someValue.name] = someValue;
  globalSymbols_[someValue.symbolName] = someValue;
  declaredMemberScopes_[some.symbolName] = someMembers;
  memberScopes_[some.symbolName] = someMembers;

  SymbolInfo none;
  none.kind = AstDeclarationKind::Object;
  none.name = "None";
  none.symbolName = std::string(support::StdNames::ScalaNone);
  none.type = TypeInfo{SimpleTypeKind::Object, none.symbolName};
  none.type.runtimeName = none.symbolName;
  none.parentSymbolName = option.symbolName;
  none.parentSymbolNames = {option.symbolName};
  TypeInfo nothingType{SimpleTypeKind::Nothing, "Nothing"};
  TypeInfo noneParent{SimpleTypeKind::Object,
                      option.symbolName + " [ Nothing ]"};
  noneParent.runtimeName = option.symbolName;
  noneParent.typeConstructorName = option.symbolName;
  noneParent.typeArguments = {nothingType};
  none.parentTypes = {noneParent};
  scope[none.name] = none;
  globalSymbols_[none.symbolName] = none;
  declaredMemberScopes_[none.symbolName] = {};
  memberScopes_[none.symbolName] = {};

  SymbolInfo compiletime;
  compiletime.kind = AstDeclarationKind::Object;
  compiletime.name = "compiletime";
  compiletime.symbolName = std::string(support::StdNames::ScalaCompiletime);
  compiletime.type =
      TypeInfo{SimpleTypeKind::Object, compiletime.symbolName};
  globalSymbols_[compiletime.symbolName] = compiletime;

  SymbolInfo compiletimeError;
  compiletimeError.kind = AstDeclarationKind::Def;
  compiletimeError.name = std::string(support::StdNames::CompiletimeError);
  compiletimeError.symbolName =
      std::string(support::StdNames::ScalaCompiletimeError);
  compiletimeError.type = TypeInfo{SimpleTypeKind::Nothing, "Nothing"};
  compiletimeError.parameters = {"msg: String"};
  compiletimeError.parameterTypes = {
      TypeInfo{SimpleTypeKind::String, "String"}};
  compiletimeError.inlineParameters = {true};
  compiletimeError.parameterClauseSizes = {1};
  compiletimeError.contextualParameterClauses = {false};
  globalSymbols_[compiletimeError.symbolName] = std::move(compiletimeError);

  SymbolInfo requireConst;
  requireConst.kind = AstDeclarationKind::Def;
  requireConst.name = std::string(support::StdNames::RequireConst);
  requireConst.symbolName =
      std::string(support::StdNames::ScalaCompiletimeRequireConst);
  requireConst.type = TypeInfo{SimpleTypeKind::Unit, "Unit"};
  requireConst.parameters = {"x: Object"};
  requireConst.parameterTypes = {TypeInfo{SimpleTypeKind::Object, "Object"}};
  requireConst.inlineParameters = {true};
  requireConst.parameterClauseSizes = {1};
  requireConst.contextualParameterClauses = {false};
  globalSymbols_[requireConst.symbolName] = std::move(requireConst);

  SymbolInfo codeOf;
  codeOf.kind = AstDeclarationKind::Def;
  codeOf.name = std::string(support::StdNames::CodeOf);
  codeOf.symbolName = std::string(support::StdNames::ScalaCompiletimeCodeOf);
  codeOf.type = TypeInfo{SimpleTypeKind::String, "String"};
  codeOf.parameters = {"arg: Object"};
  codeOf.parameterTypes = {TypeInfo{SimpleTypeKind::Object, "Object"}};
  codeOf.inlineParameters = {true};
  codeOf.parameterClauseSizes = {1};
  codeOf.contextualParameterClauses = {false};
  codeOf.isTransparent = true;
  globalSymbols_[codeOf.symbolName] = std::move(codeOf);

  SymbolInfo uninitialized;
  uninitialized.kind = AstDeclarationKind::Def;
  uninitialized.name = std::string(support::StdNames::Uninitialized);
  uninitialized.symbolName =
      std::string(support::StdNames::ScalaCompiletimeUninitialized);
  uninitialized.type = TypeInfo{SimpleTypeKind::Nothing, "Nothing"};
  globalSymbols_[uninitialized.symbolName] = std::move(uninitialized);

  SymbolInfo constValue;
  constValue.kind = AstDeclarationKind::Def;
  constValue.name = std::string(support::StdNames::ConstValue);
  constValue.symbolName =
      std::string(support::StdNames::ScalaCompiletimeConstValue);
  constValue.type = TypeInfo{SimpleTypeKind::Object, "Object"};
  TypeParameterInfo constantType;
  constantType.name = "T";
  constantType.symbolName = constValue.symbolName + ".T";
  constantType.lowerBound = TypeInfo{SimpleTypeKind::Nothing, "Nothing"};
  constantType.upperBound = TypeInfo{SimpleTypeKind::Object, "Object"};
  constValue.typeParameters.push_back(std::move(constantType));
  globalSymbols_[constValue.symbolName] = std::move(constValue);

  SymbolInfo constValueOpt;
  constValueOpt.kind = AstDeclarationKind::Def;
  constValueOpt.name = std::string(support::StdNames::ConstValueOpt);
  constValueOpt.symbolName =
      std::string(support::StdNames::ScalaCompiletimeConstValueOpt);
  TypeParameterInfo optionalType;
  optionalType.name = "T";
  optionalType.symbolName = constValueOpt.symbolName + ".T";
  optionalType.lowerBound = TypeInfo{SimpleTypeKind::Nothing, "Nothing"};
  optionalType.upperBound = TypeInfo{SimpleTypeKind::Object, "Object"};
  constValueOpt.typeParameters.push_back(optionalType);
  TypeInfo optionalArgument{SimpleTypeKind::Object, "T"};
  optionalArgument.runtimeName = "Object";
  optionalArgument.typeParameter = true;
  optionalArgument.typeParameterSymbolName = optionalType.symbolName;
  constValueOpt.type = TypeInfo{
      SimpleTypeKind::Object,
      std::string(support::StdNames::ScalaOption) + " [ T ]"};
  constValueOpt.type.runtimeName = std::string(support::StdNames::ScalaOption);
  constValueOpt.type.typeConstructorName =
      std::string(support::StdNames::ScalaOption);
  constValueOpt.type.typeArguments = {optionalArgument};
  constValueOpt.isTransparent = true;
  globalSymbols_[constValueOpt.symbolName] = std::move(constValueOpt);

  SymbolInfo constValueTuple;
  constValueTuple.kind = AstDeclarationKind::Def;
  constValueTuple.name = std::string(support::StdNames::ConstValueTuple);
  constValueTuple.symbolName =
      std::string(support::StdNames::ScalaCompiletimeConstValueTuple);
  TypeParameterInfo tupleType;
  tupleType.name = "T";
  tupleType.symbolName = constValueTuple.symbolName + ".T";
  tupleType.lowerBound = TypeInfo{SimpleTypeKind::Nothing, "Nothing"};
  tupleType.upperBound = TypeInfo{SimpleTypeKind::Object,
                                  std::string(support::StdNames::ScalaTuple)};
  tupleType.upperBound.runtimeName = std::string(support::StdNames::ScalaTuple);
  constValueTuple.typeParameters.push_back(tupleType);
  constValueTuple.type = TypeInfo{SimpleTypeKind::Object, "T"};
  constValueTuple.type.runtimeName = "Object";
  constValueTuple.type.typeParameter = true;
  constValueTuple.type.typeParameterSymbolName = tupleType.symbolName;
  constValueTuple.isTransparent = true;
  globalSymbols_[constValueTuple.symbolName] = std::move(constValueTuple);

  SymbolInfo erasedValue;
  erasedValue.kind = AstDeclarationKind::Def;
  erasedValue.name = std::string(support::StdNames::ErasedValue);
  erasedValue.symbolName =
      std::string(support::StdNames::ScalaCompiletimeErasedValue);
  erasedValue.type = TypeInfo{SimpleTypeKind::Object, "Object"};
  TypeParameterInfo erasedType;
  erasedType.name = "T";
  erasedType.symbolName = erasedValue.symbolName + ".T";
  erasedType.lowerBound = TypeInfo{SimpleTypeKind::Nothing, "Nothing"};
  erasedType.upperBound = TypeInfo{SimpleTypeKind::Object, "Object"};
  erasedValue.typeParameters.push_back(std::move(erasedType));
  globalSymbols_[erasedValue.symbolName] = std::move(erasedValue);

  SymbolInfo summonInline;
  summonInline.kind = AstDeclarationKind::Def;
  summonInline.name = std::string(support::StdNames::SummonInline);
  summonInline.symbolName =
      std::string(support::StdNames::ScalaCompiletimeSummonInline);
  summonInline.type = TypeInfo{SimpleTypeKind::Object, "Object"};
  TypeParameterInfo summonedType;
  summonedType.name = "T";
  summonedType.symbolName = summonInline.symbolName + ".T";
  summonedType.lowerBound = TypeInfo{SimpleTypeKind::Nothing, "Nothing"};
  summonedType.upperBound = TypeInfo{SimpleTypeKind::Object, "Object"};
  summonInline.typeParameters.push_back(std::move(summonedType));
  globalSymbols_[summonInline.symbolName] = std::move(summonInline);

  SymbolInfo summonAll;
  summonAll.kind = AstDeclarationKind::Def;
  summonAll.name = std::string(support::StdNames::SummonAll);
  summonAll.symbolName =
      std::string(support::StdNames::ScalaCompiletimeSummonAll);
  TypeParameterInfo summonedTupleType;
  summonedTupleType.name = "T";
  summonedTupleType.symbolName = summonAll.symbolName + ".T";
  summonedTupleType.lowerBound = TypeInfo{SimpleTypeKind::Nothing, "Nothing"};
  summonedTupleType.upperBound = TypeInfo{
      SimpleTypeKind::Object, std::string(support::StdNames::ScalaTuple)};
  summonedTupleType.upperBound.runtimeName =
      std::string(support::StdNames::ScalaTuple);
  summonAll.typeParameters.push_back(summonedTupleType);
  summonAll.type = TypeInfo{SimpleTypeKind::Object, "T"};
  summonAll.type.runtimeName = "Object";
  summonAll.type.typeParameter = true;
  summonAll.type.typeParameterSymbolName = summonedTupleType.symbolName;
  globalSymbols_[summonAll.symbolName] = std::move(summonAll);

  SymbolInfo summonFrom;
  summonFrom.kind = AstDeclarationKind::Def;
  summonFrom.name = std::string(support::StdNames::SummonFrom);
  summonFrom.symbolName =
      std::string(support::StdNames::ScalaCompiletimeSummonFrom);
  summonFrom.type = TypeInfo{SimpleTypeKind::Object, "Object"};
  globalSymbols_[summonFrom.symbolName] = std::move(summonFrom);

  SymbolInfo notImplemented;
  notImplemented.kind = AstDeclarationKind::Def;
  notImplemented.name = std::string(support::StdNames::NotImplemented);
  notImplemented.symbolName = notImplemented.name;
  notImplemented.type = TypeInfo{SimpleTypeKind::Nothing, "Nothing"};
  scope[notImplemented.name] = std::move(notImplemented);

  SymbolInfo assert;
  assert.kind = AstDeclarationKind::Def;
  assert.name = std::string(support::StdNames::Assert);
  assert.symbolName = std::string(support::StdNames::RuntimeAssert);
  assert.type = TypeInfo{SimpleTypeKind::Unit, "Unit"};
  assert.parameterTypes.push_back(TypeInfo{SimpleTypeKind::Boolean, "Boolean"});
  scope[assert.name] = std::move(assert);

  SymbolInfo assume;
  assume.kind = AstDeclarationKind::Def;
  assume.name = std::string(support::StdNames::Assume);
  assume.symbolName = std::string(support::StdNames::RuntimeAssume);
  assume.type = TypeInfo{SimpleTypeKind::Unit, "Unit"};
  assume.parameterTypes.push_back(TypeInfo{SimpleTypeKind::Boolean, "Boolean"});
  scope[assume.name] = std::move(assume);

  SymbolInfo require;
  require.kind = AstDeclarationKind::Def;
  require.name = std::string(support::StdNames::Require);
  require.symbolName = std::string(support::StdNames::RuntimeRequire);
  require.type = TypeInfo{SimpleTypeKind::Unit, "Unit"};
  require.parameterTypes.push_back(TypeInfo{SimpleTypeKind::Boolean, "Boolean"});
  scope[require.name] = std::move(require);

  SymbolInfo println;
  println.kind = AstDeclarationKind::Def;
  println.name = std::string(support::StdNames::Println);
  println.symbolName = std::string(support::StdNames::RuntimePrintln);
  println.type = TypeInfo{SimpleTypeKind::Unit, "Unit"};
  println.parameterTypes.push_back(TypeInfo{SimpleTypeKind::Unknown, "Unknown"});
  scope[println.name] = std::move(println);

  SymbolInfo gcCollect;
  gcCollect.kind = AstDeclarationKind::Def;
  gcCollect.name = std::string(support::StdNames::GcCollect);
  gcCollect.symbolName = std::string(support::StdNames::RuntimeGcCollect);
  gcCollect.type = TypeInfo{SimpleTypeKind::Unit, "Unit"};
  scope[gcCollect.name] = std::move(gcCollect);

  SymbolInfo gcLiveObjectCount;
  gcLiveObjectCount.kind = AstDeclarationKind::Def;
  gcLiveObjectCount.name = std::string(support::StdNames::GcLiveObjectCount);
  gcLiveObjectCount.symbolName =
      std::string(support::StdNames::RuntimeGcLiveObjectCount);
  gcLiveObjectCount.type = TypeInfo{SimpleTypeKind::Long, "Long"};
  scope[gcLiveObjectCount.name] = std::move(gcLiveObjectCount);

  SymbolInfo gcCollectionCount;
  gcCollectionCount.kind = AstDeclarationKind::Def;
  gcCollectionCount.name = std::string(support::StdNames::GcCollectionCount);
  gcCollectionCount.symbolName =
      std::string(support::StdNames::RuntimeGcCollectionCount);
  gcCollectionCount.type = TypeInfo{SimpleTypeKind::Long, "Long"};
  scope[gcCollectionCount.name] = std::move(gcCollectionCount);

  SymbolInfo gcSetCollectionThreshold;
  gcSetCollectionThreshold.kind = AstDeclarationKind::Def;
  gcSetCollectionThreshold.name =
      std::string(support::StdNames::GcSetCollectionThreshold);
  gcSetCollectionThreshold.symbolName =
      std::string(support::StdNames::RuntimeGcSetCollectionThreshold);
  gcSetCollectionThreshold.type = TypeInfo{SimpleTypeKind::Unit, "Unit"};
  gcSetCollectionThreshold.parameterTypes.push_back(
      TypeInfo{SimpleTypeKind::Long, "Long"});
  scope[gcSetCollectionThreshold.name] = std::move(gcSetCollectionThreshold);

  SymbolInfo throwable;
  throwable.kind = AstDeclarationKind::Class;
  throwable.name = "Throwable";
  throwable.symbolName = std::string(support::StdNames::JavaLangThrowable);
  throwable.type = TypeInfo{SimpleTypeKind::Object, throwable.symbolName};
  throwable.parentSymbolName = std::string(support::StdNames::JavaLangObject);
  throwable.parentSymbolNames = {throwable.parentSymbolName};
  throwable.parameterTypes = {TypeInfo{SimpleTypeKind::String, "String"},
                              TypeInfo{SimpleTypeKind::Object, throwable.symbolName}};
  throwable.hasImplementation = true;
  scope[throwable.name] = throwable;
  globalSymbols_[throwable.symbolName] = throwable;

  Scope throwableMembers;
  SymbolInfo throwableMessage;
  throwableMessage.kind = AstDeclarationKind::Val;
  throwableMessage.name = std::string(support::StdNames::ThrowableMessage);
  throwableMessage.symbolName = throwable.symbolName + "." + throwableMessage.name;
  throwableMessage.type = TypeInfo{SimpleTypeKind::String, "String"};
  throwableMessage.hasImplementation = true;
  throwableMembers[throwableMessage.name] = throwableMessage;

  SymbolInfo getMessage;
  getMessage.kind = AstDeclarationKind::Def;
  getMessage.name = std::string(support::StdNames::GetMessage);
  getMessage.symbolName = throwable.symbolName + "." + getMessage.name;
  getMessage.type = TypeInfo{SimpleTypeKind::String, "String"};
  getMessage.hasImplementation = true;
  throwableMembers[getMessage.name] = getMessage;

  SymbolInfo getCause;
  getCause.kind = AstDeclarationKind::Def;
  getCause.name = std::string(support::StdNames::GetCause);
  getCause.symbolName = throwable.symbolName + "." + getCause.name;
  getCause.type = TypeInfo{SimpleTypeKind::Object, throwable.symbolName};
  getCause.hasImplementation = true;
  throwableMembers[getCause.name] = getCause;

  SymbolInfo initCause;
  initCause.kind = AstDeclarationKind::Def;
  initCause.name = std::string(support::StdNames::InitCause);
  initCause.symbolName = throwable.symbolName + "." + initCause.name;
  initCause.type = TypeInfo{SimpleTypeKind::Object, throwable.symbolName};
  initCause.parameterTypes = {TypeInfo{SimpleTypeKind::Object, throwable.symbolName}};
  initCause.hasImplementation = true;
  throwableMembers[initCause.name] = initCause;

  SymbolInfo fillInStackTrace;
  fillInStackTrace.kind = AstDeclarationKind::Def;
  fillInStackTrace.name = std::string(support::StdNames::FillInStackTrace);
  fillInStackTrace.symbolName = throwable.symbolName + "." + fillInStackTrace.name;
  fillInStackTrace.type = TypeInfo{SimpleTypeKind::Object, throwable.symbolName};
  fillInStackTrace.hasImplementation = true;
  throwableMembers[fillInStackTrace.name] = fillInStackTrace;

  SymbolInfo getStackTrace;
  getStackTrace.kind = AstDeclarationKind::Def;
  getStackTrace.name = std::string(support::StdNames::GetStackTrace);
  getStackTrace.symbolName = throwable.symbolName + "." + getStackTrace.name;
  getStackTrace.type = TypeInfo{
      SimpleTypeKind::Object,
      "Array [ " + std::string(support::StdNames::JavaLangStackTraceElement) + " ]"};
  getStackTrace.hasImplementation = true;
  throwableMembers[getStackTrace.name] = getStackTrace;

  SymbolInfo setStackTrace;
  setStackTrace.kind = AstDeclarationKind::Def;
  setStackTrace.name = std::string(support::StdNames::SetStackTrace);
  setStackTrace.symbolName = throwable.symbolName + "." + setStackTrace.name;
  setStackTrace.type = TypeInfo{SimpleTypeKind::Unit, "Unit"};
  setStackTrace.parameterTypes = {TypeInfo{
      SimpleTypeKind::Object,
      "Array [ " + std::string(support::StdNames::JavaLangStackTraceElement) + " ]"}};
  setStackTrace.hasImplementation = true;
  throwableMembers[setStackTrace.name] = setStackTrace;

  SymbolInfo addSuppressed;
  addSuppressed.kind = AstDeclarationKind::Def;
  addSuppressed.name = std::string(support::StdNames::AddSuppressed);
  addSuppressed.symbolName = throwable.symbolName + "." + addSuppressed.name;
  addSuppressed.type = TypeInfo{SimpleTypeKind::Unit, "Unit"};
  addSuppressed.parameterTypes = {
      TypeInfo{SimpleTypeKind::Object, throwable.symbolName}};
  addSuppressed.hasImplementation = true;
  throwableMembers[addSuppressed.name] = addSuppressed;

  SymbolInfo getSuppressed;
  getSuppressed.kind = AstDeclarationKind::Def;
  getSuppressed.name = std::string(support::StdNames::GetSuppressed);
  getSuppressed.symbolName = throwable.symbolName + "." + getSuppressed.name;
  getSuppressed.type =
      TypeInfo{SimpleTypeKind::Object, "Array [ java.lang.Throwable ]"};
  getSuppressed.hasImplementation = true;
  throwableMembers[getSuppressed.name] = getSuppressed;

  SymbolInfo printStackTrace;
  printStackTrace.kind = AstDeclarationKind::Def;
  printStackTrace.name = std::string(support::StdNames::PrintStackTrace);
  printStackTrace.symbolName = throwable.symbolName + "." + printStackTrace.name;
  printStackTrace.type = TypeInfo{SimpleTypeKind::Unit, "Unit"};
  printStackTrace.hasImplementation = true;
  throwableMembers[printStackTrace.name] = printStackTrace;

  SymbolInfo throwableToString;
  throwableToString.kind = AstDeclarationKind::Def;
  throwableToString.name = std::string(support::StdNames::ToString);
  throwableToString.symbolName = throwable.symbolName + "." + throwableToString.name;
  throwableToString.type = TypeInfo{SimpleTypeKind::String, "String"};
  throwableToString.hasImplementation = true;
  throwableMembers[throwableToString.name] = throwableToString;
  declaredMemberScopes_[throwable.symbolName] = throwableMembers;
  memberScopes_[throwable.symbolName] = throwableMembers;

  SymbolInfo exception;
  exception.kind = AstDeclarationKind::Class;
  exception.name = "Exception";
  exception.symbolName = std::string(support::StdNames::JavaLangException);
  exception.type = TypeInfo{SimpleTypeKind::Object, exception.symbolName};
  exception.parentSymbolName = throwable.symbolName;
  exception.parentSymbolNames = {throwable.symbolName};
  exception.parameterTypes = {TypeInfo{SimpleTypeKind::String, "String"}};
  exception.hasImplementation = true;
  scope[exception.name] = exception;
  globalSymbols_[exception.symbolName] = exception;

  Scope exceptionDeclaredMembers;
  SymbolInfo message;
  message.kind = AstDeclarationKind::Val;
  message.name = std::string(support::StdNames::ThrowableMessage);
  message.symbolName = exception.symbolName + "." + message.name;
  message.type = TypeInfo{SimpleTypeKind::String, "String"};
  message.hasImplementation = true;
  exceptionDeclaredMembers[message.name] = message;

  SymbolInfo toString;
  toString.kind = AstDeclarationKind::Def;
  toString.name = std::string(support::StdNames::ToString);
  toString.symbolName = exception.symbolName + "." + toString.name;
  toString.type = TypeInfo{SimpleTypeKind::String, "String"};
  toString.hasImplementation = true;
  exceptionDeclaredMembers[toString.name] = toString;
  Scope exceptionMembers = throwableMembers;
  exceptionMembers[message.name] = std::move(message);
  exceptionMembers[toString.name] = toString;
  declaredMemberScopes_[exception.symbolName] = std::move(exceptionDeclaredMembers);
  memberScopes_[exception.symbolName] = std::move(exceptionMembers);

  SymbolInfo error;
  error.kind = AstDeclarationKind::Class;
  error.name = "Error";
  error.symbolName = std::string(support::StdNames::JavaLangError);
  error.type = TypeInfo{SimpleTypeKind::Object, error.symbolName};
  error.parentSymbolName = throwable.symbolName;
  error.parentSymbolNames = {throwable.symbolName};
  error.parameterTypes = {TypeInfo{SimpleTypeKind::String, "String"}};
  error.hasImplementation = true;
  scope[error.name] = error;
  globalSymbols_[error.symbolName] = error;

  SymbolInfo errorMessage;
  errorMessage.kind = AstDeclarationKind::Val;
  errorMessage.name = std::string(support::StdNames::ThrowableMessage);
  errorMessage.symbolName = error.symbolName + "." + errorMessage.name;
  errorMessage.type = TypeInfo{SimpleTypeKind::String, "String"};
  errorMessage.hasImplementation = true;
  Scope errorDeclaredMembers;
  errorDeclaredMembers[errorMessage.name] = errorMessage;
  Scope errorMembers = throwableMembers;
  errorMembers[errorMessage.name] = std::move(errorMessage);
  declaredMemberScopes_[error.symbolName] = std::move(errorDeclaredMembers);
  memberScopes_[error.symbolName] = std::move(errorMembers);

  const auto addExceptionSubclass = [&](std::string name, std::string symbolName,
                                        const std::string& parentSymbolName) {
    SymbolInfo subclass;
    subclass.kind = AstDeclarationKind::Class;
    subclass.name = std::move(name);
    subclass.symbolName = std::move(symbolName);
    subclass.type = TypeInfo{SimpleTypeKind::Object, subclass.symbolName};
    subclass.parentSymbolName = parentSymbolName;
    subclass.parentSymbolNames = {parentSymbolName};
    subclass.parameterTypes = {TypeInfo{SimpleTypeKind::String, "String"}};
    subclass.hasImplementation = true;
    scope[subclass.name] = subclass;
    globalSymbols_[subclass.symbolName] = subclass;

    SymbolInfo subclassMessage;
    subclassMessage.kind = AstDeclarationKind::Val;
    subclassMessage.name = std::string(support::StdNames::ThrowableMessage);
    subclassMessage.symbolName = subclass.symbolName + "." + subclassMessage.name;
    subclassMessage.type = TypeInfo{SimpleTypeKind::String, "String"};
    subclassMessage.hasImplementation = true;
    Scope declaredMembers;
    declaredMembers[subclassMessage.name] = subclassMessage;
    Scope members = memberScopes_.at(parentSymbolName);
    members[subclassMessage.name] = std::move(subclassMessage);
    declaredMemberScopes_[subclass.symbolName] = std::move(declaredMembers);
    memberScopes_[subclass.symbolName] = std::move(members);
  };
  addExceptionSubclass("AssertionError",
                       std::string(support::StdNames::JavaLangAssertionError),
                       error.symbolName);
  addExceptionSubclass("NotImplementedError",
                       std::string(support::StdNames::ScalaNotImplementedError),
                       error.symbolName);
  addExceptionSubclass("RuntimeException",
                       std::string(support::StdNames::JavaLangRuntimeException),
                       exception.symbolName);
  addExceptionSubclass("ArithmeticException",
                       std::string(support::StdNames::JavaLangArithmeticException),
                       std::string(support::StdNames::JavaLangRuntimeException));
  addExceptionSubclass("IllegalArgumentException",
                       std::string(support::StdNames::JavaLangIllegalArgumentException),
                       std::string(support::StdNames::JavaLangRuntimeException));
  addExceptionSubclass("IllegalStateException",
                       std::string(support::StdNames::JavaLangIllegalStateException),
                       std::string(support::StdNames::JavaLangRuntimeException));
  addExceptionSubclass("NullPointerException",
                       std::string(support::StdNames::JavaLangNullPointerException),
                       std::string(support::StdNames::JavaLangRuntimeException));
  addExceptionSubclass("ClassCastException",
                       std::string(support::StdNames::JavaLangClassCastException),
                       std::string(support::StdNames::JavaLangRuntimeException));
  addExceptionSubclass("ArrayStoreException",
                       std::string(support::StdNames::JavaLangArrayStoreException),
                       std::string(support::StdNames::JavaLangRuntimeException));
  addExceptionSubclass(
      "IndexOutOfBoundsException",
      std::string(support::StdNames::JavaLangIndexOutOfBoundsException),
      std::string(support::StdNames::JavaLangRuntimeException));
  addExceptionSubclass(
      "ArrayIndexOutOfBoundsException",
      std::string(support::StdNames::JavaLangArrayIndexOutOfBoundsException),
      std::string(support::StdNames::JavaLangIndexOutOfBoundsException));
  addExceptionSubclass(
      "NegativeArraySizeException",
      std::string(support::StdNames::JavaLangNegativeArraySizeException),
      std::string(support::StdNames::JavaLangRuntimeException));
  addExceptionSubclass("BufferUnderflowException",
                       std::string(support::StdNames::JavaNioBufferUnderflowException),
                       std::string(support::StdNames::JavaLangRuntimeException));
  addExceptionSubclass("BufferOverflowException",
                       std::string(support::StdNames::JavaNioBufferOverflowException),
                       std::string(support::StdNames::JavaLangRuntimeException));
  addExceptionSubclass("InvalidMarkException",
                       std::string(support::StdNames::JavaNioInvalidMarkException),
                       std::string(support::StdNames::JavaLangIllegalStateException));

  SymbolInfo stackTraceElement;
  stackTraceElement.kind = AstDeclarationKind::Class;
  stackTraceElement.name = "StackTraceElement";
  stackTraceElement.symbolName =
      std::string(support::StdNames::JavaLangStackTraceElement);
  stackTraceElement.type =
      TypeInfo{SimpleTypeKind::Object, stackTraceElement.symbolName};
  stackTraceElement.parameterTypes = {TypeInfo{SimpleTypeKind::String, "String"},
                                      TypeInfo{SimpleTypeKind::String, "String"},
                                      TypeInfo{SimpleTypeKind::Int, "Int"},
                                      TypeInfo{SimpleTypeKind::Int, "Int"}};
  stackTraceElement.hasImplementation = true;
  scope[stackTraceElement.name] = stackTraceElement;
  globalSymbols_[stackTraceElement.symbolName] = stackTraceElement;

  Scope stackTraceMembers;
  const auto addStackTraceMember = [&](std::string name, TypeInfo type) {
    SymbolInfo member;
    member.kind = AstDeclarationKind::Val;
    member.name = std::move(name);
    member.symbolName = stackTraceElement.symbolName + "." + member.name;
    member.type = std::move(type);
    member.hasImplementation = true;
    stackTraceMembers[member.name] = std::move(member);
  };
  addStackTraceMember(std::string(support::StdNames::StackTraceFunctionName),
                      TypeInfo{SimpleTypeKind::String, "String"});
  addStackTraceMember(std::string(support::StdNames::StackTraceFileName),
                      TypeInfo{SimpleTypeKind::String, "String"});
  addStackTraceMember(std::string(support::StdNames::StackTraceLineNumber),
                      TypeInfo{SimpleTypeKind::Int, "Int"});
  addStackTraceMember(std::string(support::StdNames::StackTraceColumnNumber),
                      TypeInfo{SimpleTypeKind::Int, "Int"});
  SymbolInfo stackTraceToString;
  stackTraceToString.kind = AstDeclarationKind::Def;
  stackTraceToString.name = std::string(support::StdNames::ToString);
  stackTraceToString.symbolName =
      stackTraceElement.symbolName + "." + stackTraceToString.name;
  stackTraceToString.type = TypeInfo{SimpleTypeKind::String, "String"};
  stackTraceToString.hasImplementation = true;
  stackTraceMembers[stackTraceToString.name] = std::move(stackTraceToString);
  declaredMemberScopes_[stackTraceElement.symbolName] = stackTraceMembers;
  memberScopes_[stackTraceElement.symbolName] = std::move(stackTraceMembers);
}

std::string Typechecker::declarationSymbolName(const AstDeclaration& declaration,
                                               const std::string& owner) const {
  std::string name = qualify(owner, declaration.name);
  if (declaration.kind == AstDeclarationKind::Object &&
      companionTypeNames_.contains(name)) {
    name += '$';
  }
  return name;
}

std::string Typechecker::importSymbolName(const AstDeclaration& declaration,
                                          const Scope& scope) const {
  std::string path = declaration.importPath;
  if (declaration.name == "_") {
    path = wildcardImportOwner(path);
  }

  const auto resolveOwner = [&](const std::string& owner) {
    if (companionTypeNames_.contains(owner)) {
      return owner + '$';
    }
    if (auto visible = scope.find(owner); visible != scope.end()) {
      if (companionTypeNames_.contains(visible->second.symbolName)) {
        return visible->second.symbolName + '$';
      }
      if (visible->second.kind == AstDeclarationKind::Object) {
        return visible->second.symbolName;
      }
    }
    return owner;
  };

  if (!declaration.importSelectors.empty() || declaration.name == "_" ||
      declaration.importsWildcard || declaration.importsGivens ||
      !declaration.importGivenTypes.empty()) {
    return resolveOwner(path);
  }
  if (globalSymbols_.contains(path)) {
    return path;
  }
  const std::size_t separator = path.rfind('.');
  if (separator == std::string::npos) {
    return resolveOwner(path);
  }
  const std::string owner = resolveOwner(path.substr(0, separator));
  const std::string resolved = owner + path.substr(separator);
  return globalSymbols_.contains(resolved) ? resolved : path;
}

void Typechecker::collectDeclaration(const AstDeclaration& declaration,
                                     const std::string& owner, Scope& scope) {
  if (declaration.name.empty() || declaration.kind == AstDeclarationKind::Package ||
      declaration.kind == AstDeclarationKind::Import) {
    return;
  }

  SymbolInfo symbol;
  symbol.kind = declaration.kind;
  symbol.name = declaration.name;
  symbol.symbolName = declarationSymbolName(declaration, owner);
  Scope declarationScope = scope;
  symbol.typeParameters = resolvedTypeParameters(declaration.typeParameters,
                                                 symbol.symbolName, declarationScope);
  symbol.parameters = declaration.parameters;
  symbol.contextualParameters = declaration.contextualParameters;
  symbol.contextualParameters.resize(declaration.parameters.size(), false);
  symbol.inlineParameters = declaration.inlineParameters;
  symbol.inlineParameters.resize(declaration.parameters.size(), false);
  symbol.parameterClauseSizes = declaration.parameterClauseSizes;
  symbol.contextualParameterClauses = declaration.contextualParameterClauses;
  symbol.isGiven = declaration.isGiven;
  symbol.isAnonymousGiven = declaration.isAnonymousGiven;
  symbol.isTransparent = declaration.isTransparent;
  symbol.isInline = declaration.isInline;
  if (declaration.isInline) {
    symbol.inlineBody = declaration.initializer;
  }
  if (auto enclosing = globalSymbols_.find(owner);
      enclosing != globalSymbols_.end() &&
      enclosing->second.kind == AstDeclarationKind::Object) {
    symbol.isModuleMember = declaration.kind == AstDeclarationKind::Val ||
                            declaration.kind == AstDeclarationKind::Var;
  } else if (enclosing != globalSymbols_.end() &&
             (enclosing->second.kind == AstDeclarationKind::Class ||
              enclosing->second.kind == AstDeclarationKind::Trait)) {
    symbol.isInstanceMember = declaration.kind == AstDeclarationKind::Def ||
                              declaration.kind == AstDeclarationKind::Val ||
                              declaration.kind == AstDeclarationKind::Var;
  }
  if (isClassLikeDeclaration(declaration.kind)) {
    for (const std::string& parentName : sourceParentTypes(declaration)) {
      if (const SymbolInfo* parent =
              typeSymbolForDeclaredName(parentName, &declarationScope);
          parent != nullptr && isInheritableDeclaration(parent->kind)) {
        symbol.parentSymbolNames.push_back(parent->symbolName);
        const AppliedTypeSyntax applied = parseAppliedTypeSyntax(parentName);
        symbol.parentTypes.push_back(
            applied.applied ? typeFromDeclaredName(parentName, &declarationScope)
                            : parent->type);
      }
    }
    if (!symbol.parentSymbolNames.empty()) {
      symbol.parentSymbolName = symbol.parentSymbolNames.front();
    }
  }
  symbol.type = preliminaryDeclarationType(declaration, &declarationScope);
  symbol.lowerBound = typeFromDeclaredName(declaration.lowerBound, &declarationScope);
  symbol.upperBound = typeFromDeclaredName(declaration.upperBound, &declarationScope);
  if (declaration.kind == AstDeclarationKind::Type && !declaration.hasInitializer) {
    symbol.type = TypeInfo{SimpleTypeKind::Object, symbol.symbolName};
  }
  symbol.hasImplementation =
      declarationHasImplementation(declaration.kind, declaration.hasInitializer);
  if (declaration.kind == AstDeclarationKind::Object ||
      declaration.kind == AstDeclarationKind::Class ||
      declaration.kind == AstDeclarationKind::Trait) {
    symbol.type = TypeInfo{SimpleTypeKind::Object, symbol.symbolName};
  }
  for (const std::string& parameter : declaration.parameters) {
    symbol.parameterTypes.push_back(parameterType(parameter, &declarationScope));
  }
  std::string symbolName = symbol.symbolName;
  globalSymbols_[symbol.symbolName] = symbol;
  const bool companionObject =
      declaration.kind == AstDeclarationKind::Object &&
      companionTypeNames_.contains(qualify(owner, declaration.name));
  if (!companionObject) {
    scope[declaration.name] = symbol;
    declarationScope[declaration.name] = symbol;
  }

  if (declaration.kind == AstDeclarationKind::Object ||
      declaration.kind == AstDeclarationKind::Class ||
      declaration.kind == AstDeclarationKind::Trait) {
    Scope ownMembers;
    if (declaration.kind == AstDeclarationKind::Class) {
      for (std::size_t parameterIndex = 0;
           parameterIndex < declaration.parameters.size(); ++parameterIndex) {
        const std::string& parameter = declaration.parameters[parameterIndex];
        const std::string name = parameterName(parameter);
        if (name.empty()) {
          continue;
        }
        SymbolInfo field;
        field.kind = parameterDeclarationKind(parameter);
        field.name = name;
        field.symbolName = qualify(symbolName, name);
        field.type = parameterType(parameter, &declarationScope);
        field.isContextParameter =
            parameterIndex < declaration.contextualParameters.size() &&
            declaration.contextualParameters[parameterIndex];
        ownMembers[name] = std::move(field);
      }
    }
    const auto collectMember = [&](const AstDeclaration& member) {
      Scope memberResolutionScope = declarationScope;
      mergeScope(memberResolutionScope, ownMembers);
      collectDeclaration(member, symbolName, memberResolutionScope);
      if (auto collected = memberResolutionScope.find(member.name);
          collected != memberResolutionScope.end()) {
        ownMembers[member.name] = std::move(collected->second);
      }
    };
    for (const AstDeclaration& member : declaration.members) {
      if (member.kind == AstDeclarationKind::Type) {
        collectMember(member);
      }
    }
    for (const AstDeclaration& member : declaration.members) {
      if (member.kind != AstDeclarationKind::Type) {
        collectMember(member);
      }
    }
    declaredMemberScopes_[symbolName] = ownMembers;
    mergeInheritedMembers(ownMembers, symbol.parentSymbolNames, symbol.parentTypes);
    memberScopes_[symbolName] = std::move(ownMembers);
  }
}

void Typechecker::collectProductMirrors(const std::vector<AstDeclaration>& declarations,
                                        const std::string& owner, Scope& scope) {
  const auto productOfSymbol = globalSymbols_.find("scala.deriving.Mirror.ProductOf");
  const auto mirrorOfSymbol = globalSymbols_.find("scala.deriving.Mirror.Of");
  if (productOfSymbol == globalSymbols_.end() ||
      mirrorOfSymbol == globalSymbols_.end()) {
    return;
  }
  const SymbolInfo productOf = productOfSymbol->second;
  const SymbolInfo mirrorOf = mirrorOfSymbol->second;

  for (const AstDeclaration& declaration : declarations) {
    if (declaration.kind == AstDeclarationKind::Object) {
      Scope nestedScope = scope;
      const std::string nestedOwner = declarationSymbolName(declaration, owner);
      if (auto members = memberScopes_.find(nestedOwner);
          members != memberScopes_.end()) {
        mergeScope(nestedScope, members->second);
      }
      collectProductMirrors(declaration.members, nestedOwner, nestedScope);
    }
    if (declaration.kind != AstDeclarationKind::Class ||
        declaration.derivedTypes.empty()) {
      continue;
    }

    Scope declarationScope = scope;
    const std::string targetName = declarationSymbolName(declaration, owner);
    if (auto members = memberScopes_.find(targetName); members != memberScopes_.end()) {
      mergeScope(declarationScope, members->second);
    }
    const bool requiresProductMirror = std::any_of(
        declaration.derivedTypes.begin(), declaration.derivedTypes.end(),
        [&](const std::string& derivedTypeName) {
          const SymbolInfo* typeclass =
              typeSymbolForDeclaredName(derivedTypeName, &declarationScope);
          if (typeclass == nullptr) {
            return false;
          }
          auto companionMembers = memberScopes_.find(typeclass->symbolName + '$');
          if (companionMembers == memberScopes_.end()) {
            return false;
          }
          auto derived = companionMembers->second.find("derived");
          if (derived == companionMembers->second.end()) {
            return false;
          }
          for (std::size_t parameterIndex = 0;
               parameterIndex < derived->second.parameterTypes.size();
               ++parameterIndex) {
            const bool contextual =
                parameterIndex < derived->second.contextualParameters.size() &&
                derived->second.contextualParameters[parameterIndex];
            const std::string& constructor =
                derived->second.parameterTypes[parameterIndex].typeConstructorName;
            if (contextual && (constructor == productOf.symbolName ||
                               constructor == mirrorOf.symbolName)) {
              return true;
            }
          }
          return false;
        });
    if (!requiresProductMirror) {
      continue;
    }

    auto targetSymbol = globalSymbols_.find(targetName);
    if (targetSymbol == globalSymbols_.end() ||
        std::any_of(targetSymbol->second.contextualParameters.begin(),
                    targetSymbol->second.contextualParameters.end(),
                    [](bool contextual) { return contextual; })) {
      continue;
    }
    const SymbolInfo target = targetSymbol->second;

    std::vector<TypeInfo> derivingTypeArguments;
    derivingTypeArguments.reserve(target.typeParameters.size());
    for (const TypeParameterInfo& parameter : target.typeParameters) {
      TypeInfo parameterType{SimpleTypeKind::Object, parameter.name};
      parameterType.runtimeName = parameter.upperBound.runtimeName.empty()
                                      ? parameter.upperBound.name
                                      : parameter.upperBound.runtimeName;
      if (parameterType.runtimeName.empty() || parameterType.runtimeName == "Unknown") {
        parameterType.runtimeName = "Object";
      }
      parameterType.typeParameterSymbolName = parameter.symbolName;
      parameterType.typeParameter = true;
      derivingTypeArguments.push_back(std::move(parameterType));
    }
    const bool generic = !derivingTypeArguments.empty();
    const TypeInfo derivingType =
        generic ? specializeResolvedTypeApplication(target, derivingTypeArguments,
                                                    declaration.span, false)
                      .type
                : target.type;

    std::size_t syntheticSpanOffset = 1000000 + mirrorDeclarations_.size() * 1000;
    const auto nextSpan = [&]() {
      support::SourceSpan span = declaration.span;
      span.length += syntheticSpanOffset++;
      return span;
    };

    AstDeclaration implementation;
    implementation.kind = AstDeclarationKind::Class;
    implementation.name = productMirrorImplementationName(targetName);
    implementation.span = nextSpan();
    implementation.typeParameters = declaration.typeParameters;
    implementation.parentTypes = {"scala.deriving.Mirror.ProductOf[" +
                                  derivingType.name + "]"};

    const auto addTypeAlias = [&](std::string name, std::string aliasTarget) {
      AstDeclaration alias;
      alias.kind = AstDeclarationKind::Type;
      alias.name = std::move(name);
      alias.span = nextSpan();
      alias.declaredType = std::move(aliasTarget);
      alias.isOverride = true;
      alias.hasInitializer = true;
      implementation.members.push_back(std::move(alias));
    };

    std::vector<std::string> elementTypes;
    std::vector<std::string> elementLabels;
    elementTypes.reserve(target.parameterTypes.size());
    elementLabels.reserve(target.parameters.size());
    for (const TypeInfo& parameterType : target.parameterTypes) {
      elementTypes.push_back(parameterType.name);
    }
    for (const std::string& parameter : target.parameters) {
      elementLabels.push_back(stringSingletonType(parameterName(parameter)));
    }
    addTypeAlias("MirroredElemTypes", tupleTypeName(elementTypes));
    addTypeAlias("MirroredLabel", stringSingletonType(declaration.name));
    addTypeAlias("MirroredElemLabels", tupleTypeName(elementLabels));

    AstDeclaration fromProduct;
    fromProduct.kind = AstDeclarationKind::Def;
    fromProduct.name = "fromProduct";
    fromProduct.span = nextSpan();
    fromProduct.parameters = {"product: scala.Product"};
    fromProduct.contextualParameters = {false};
    fromProduct.declaredType = derivingType.name;
    fromProduct.isOverride = true;
    fromProduct.hasInitializer = true;

    AstExpression constructor;
    constructor.kind = AstExpressionKind::New;
    constructor.text = targetName;
    constructor.span = nextSpan();
    AstExpression constructorTarget;
    if (generic) {
      constructorTarget.kind = AstExpressionKind::TypeApply;
      constructorTarget.span = nextSpan();
      for (const TypeParameterInfo& parameter : target.typeParameters) {
        constructorTarget.typeArguments.push_back(parameter.name);
      }
      constructorTarget.declaredType = constructorTarget.typeArguments.front();
      constructorTarget.children.push_back(std::move(constructor));
    } else {
      constructorTarget = std::move(constructor);
    }
    AstExpression construction;
    construction.kind = AstExpressionKind::Call;
    construction.span = nextSpan();
    construction.children.push_back(std::move(constructorTarget));

    for (std::size_t parameterIndex = 0; parameterIndex < target.parameterTypes.size();
         ++parameterIndex) {
      AstExpression productReference;
      productReference.kind = AstExpressionKind::Identifier;
      productReference.text = "product";
      productReference.span = nextSpan();

      AstExpression productElement;
      productElement.kind = AstExpressionKind::Select;
      productElement.text = "productElement";
      productElement.span = nextSpan();
      productElement.children.push_back(std::move(productReference));

      AstExpression index;
      index.kind = AstExpressionKind::IntegerLiteral;
      index.text = std::to_string(parameterIndex);
      index.span = nextSpan();

      AstExpression elementCall;
      elementCall.kind = AstExpressionKind::Call;
      elementCall.span = nextSpan();
      elementCall.children.push_back(std::move(productElement));
      elementCall.children.push_back(std::move(index));

      AstExpression castSelection;
      castSelection.kind = AstExpressionKind::Select;
      castSelection.text = std::string(support::StdNames::AsInstanceOf);
      castSelection.span = nextSpan();
      castSelection.children.push_back(std::move(elementCall));

      const std::string parameterTypeName = target.parameterTypes[parameterIndex].name;
      AstExpression cast;
      cast.kind = AstExpressionKind::TypeApply;
      cast.declaredType = parameterTypeName;
      cast.typeArguments = {parameterTypeName};
      cast.span = nextSpan();
      cast.children.push_back(std::move(castSelection));
      construction.children.push_back(std::move(cast));
    }
    fromProduct.initializer = std::move(construction);
    implementation.members.push_back(std::move(fromProduct));

    mirrorDeclarations_.push_back(std::move(implementation));
    AstDeclaration& storedImplementation = mirrorDeclarations_.back();
    collectDeclaration(storedImplementation, currentPackageName_, scope);
    const std::string implementationName =
        declarationSymbolName(storedImplementation, currentPackageName_);

    const TypeInfo mirrorType = specializeResolvedTypeApplication(
                                    productOf, {derivingType}, declaration.span, false)
                                    .type;
    const std::string instanceOwner = targetName + '$';
    const std::string instanceName = "$mirror$Product$type";

    TypedDeclaration member;
    member.kind = generic ? AstDeclarationKind::Def : AstDeclarationKind::Val;
    member.name = instanceName;
    member.symbolName = qualify(instanceOwner, instanceName);
    member.span = declaration.span;
    member.typeParameters =
        generic ? target.typeParameters : std::vector<TypeParameterInfo>{};
    member.declaredType = mirrorType.name;
    member.inferredType = mirrorType;
    member.isGiven = true;
    member.hasInitializer = true;
    AstExpression mirrorConstructor;
    mirrorConstructor.kind = AstExpressionKind::New;
    mirrorConstructor.text = implementationName;
    mirrorConstructor.span = nextSpan();
    if (generic) {
      member.initializer.kind = AstExpressionKind::TypeApply;
      member.initializer.span = nextSpan();
      for (const TypeParameterInfo& parameter : target.typeParameters) {
        member.initializer.typeArguments.push_back(parameter.name);
      }
      member.initializer.declaredType = member.initializer.typeArguments.front();
      member.initializer.children.push_back(std::move(mirrorConstructor));
    } else {
      member.initializer = std::move(mirrorConstructor);
    }
    derivedInstances_.push_back(
        DerivedInstanceInfo{instanceOwner, std::move(member), {}});

    SymbolInfo candidate;
    candidate.kind = generic ? AstDeclarationKind::Def : AstDeclarationKind::Val;
    candidate.name = "derived$Mirror$Product";
    candidate.symbolName = qualify(instanceOwner, instanceName);
    candidate.type = mirrorType;
    candidate.typeParameters =
        generic ? target.typeParameters : std::vector<TypeParameterInfo>{};
    candidate.hasImplementation = true;
    candidate.isGiven = true;
    candidate.isModuleMember = !generic;
    derivedGivens_[targetName].push_back(std::move(candidate));
  }
}

void Typechecker::collectSumMirrors(const std::vector<AstDeclaration>& declarations,
                                    const std::string& owner, Scope& scope) {
  collectSumMirrorsRecursive(declarations, owner, scope, declarations, owner);
}

void Typechecker::collectSumMirrorsRecursive(
    const std::vector<AstDeclaration>& declarations, const std::string& owner,
    Scope& scope, const std::vector<AstDeclaration>& allDeclarations,
    const std::string& allDeclarationsOwner) {
  const auto sumOfSymbol = globalSymbols_.find("scala.deriving.Mirror.SumOf");
  const auto mirrorOfSymbol = globalSymbols_.find("scala.deriving.Mirror.Of");
  if (sumOfSymbol == globalSymbols_.end() || mirrorOfSymbol == globalSymbols_.end()) {
    return;
  }
  const SymbolInfo sumOf = sumOfSymbol->second;
  const SymbolInfo mirrorOf = mirrorOfSymbol->second;

  for (const AstDeclaration& declaration : declarations) {
    if (declaration.kind == AstDeclarationKind::Object) {
      Scope nestedScope = scope;
      const std::string nestedOwner = declarationSymbolName(declaration, owner);
      if (auto members = memberScopes_.find(nestedOwner);
          members != memberScopes_.end()) {
        mergeScope(nestedScope, members->second);
      }
      collectSumMirrorsRecursive(declaration.members, nestedOwner, nestedScope,
                                 allDeclarations, allDeclarationsOwner);
    }
    if (declaration.kind != AstDeclarationKind::Trait ||
        declaration.derivedTypes.empty()) {
      continue;
    }

    Scope declarationScope = scope;
    const std::string targetName = declarationSymbolName(declaration, owner);
    if (auto members = memberScopes_.find(targetName); members != memberScopes_.end()) {
      mergeScope(declarationScope, members->second);
    }
    const bool requiresSumMirror = std::any_of(
        declaration.derivedTypes.begin(), declaration.derivedTypes.end(),
        [&](const std::string& derivedTypeName) {
          const SymbolInfo* typeclass =
              typeSymbolForDeclaredName(derivedTypeName, &declarationScope);
          if (typeclass == nullptr) {
            return false;
          }
          auto companionMembers = memberScopes_.find(typeclass->symbolName + '$');
          if (companionMembers == memberScopes_.end()) {
            return false;
          }
          auto derived = companionMembers->second.find("derived");
          if (derived == companionMembers->second.end()) {
            return false;
          }
          for (std::size_t parameterIndex = 0;
               parameterIndex < derived->second.parameterTypes.size();
               ++parameterIndex) {
            const bool contextual =
                parameterIndex < derived->second.contextualParameters.size() &&
                derived->second.contextualParameters[parameterIndex];
            const std::string& constructor =
                derived->second.parameterTypes[parameterIndex].typeConstructorName;
            if (contextual && (constructor == sumOf.symbolName ||
                               constructor == mirrorOf.symbolName)) {
              return true;
            }
          }
          return false;
        });
    if (!requiresSumMirror) {
      continue;
    }

    auto targetSymbol = globalSymbols_.find(targetName);
    if (targetSymbol == globalSymbols_.end()) {
      continue;
    }
    if (!declaration.isSealed) {
      diagnostics_.error(declaration.span,
                         "sum mirror derivation requires a sealed trait: " +
                             targetName);
      continue;
    }
    const SymbolInfo target = targetSymbol->second;
    std::vector<TypeInfo> derivingTypeArguments;
    derivingTypeArguments.reserve(target.typeParameters.size());
    for (const TypeParameterInfo& parameter : target.typeParameters) {
      TypeInfo parameterType{SimpleTypeKind::Object, parameter.name};
      parameterType.runtimeName = parameter.upperBound.runtimeName.empty()
                                      ? parameter.upperBound.name
                                      : parameter.upperBound.runtimeName;
      if (parameterType.runtimeName.empty() || parameterType.runtimeName == "Unknown") {
        parameterType.runtimeName = "Object";
      }
      parameterType.typeParameterSymbolName = parameter.symbolName;
      parameterType.typeParameter = true;
      derivingTypeArguments.push_back(std::move(parameterType));
    }
    const bool generic = !derivingTypeArguments.empty();
    const TypeInfo derivingType =
        generic ? specializeResolvedTypeApplication(target, derivingTypeArguments,
                                                    declaration.span, false)
                      .type
                : target.type;

    struct SumChild {
      const AstDeclaration* declaration = nullptr;
      SymbolInfo symbol;
      std::string elementType;
    };
    const auto fixedParentArgumentAppliesToEveryTarget =
        [&](const TypeParameterInfo& parameter, const TypeInfo& argument) {
          if (argument.kind == SimpleTypeKind::Unknown) {
            return false;
          }
          switch (parameter.variance) {
          case TypeVariance::Covariant:
            return isAssignable(parameter.lowerBound, argument);
          case TypeVariance::Contravariant:
            return isAssignable(argument, parameter.upperBound);
          case TypeVariance::Invariant:
            return parameter.lowerBound.kind != SimpleTypeKind::Unknown &&
                   parameter.upperBound.kind != SimpleTypeKind::Unknown &&
                   parameter.lowerBound.name == parameter.upperBound.name &&
                   argument.name == parameter.lowerBound.name;
          }
          return false;
        };
    std::vector<SumChild> children;
    bool unsupportedChild = false;
    struct SumCandidate {
      const AstDeclaration* declaration = nullptr;
      std::string owner;
    };
    std::vector<SumCandidate> candidates;
    const std::function<void(const std::vector<AstDeclaration>&, const std::string&)>
        collectCandidates = [&](const std::vector<AstDeclaration>& nestedDeclarations,
                                const std::string& nestedOwner) {
          for (const AstDeclaration& candidateDeclaration : nestedDeclarations) {
            candidates.push_back(SumCandidate{&candidateDeclaration, nestedOwner});
            if (candidateDeclaration.kind == AstDeclarationKind::Class ||
                candidateDeclaration.kind == AstDeclarationKind::Trait ||
                candidateDeclaration.kind == AstDeclarationKind::Object) {
              collectCandidates(
                  candidateDeclaration.members,
                  declarationSymbolName(candidateDeclaration, nestedOwner));
            }
          }
        };
    collectCandidates(allDeclarations, allDeclarationsOwner);
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const SumCandidate& lhs, const SumCandidate& rhs) {
                       return lhs.declaration->span.start < rhs.declaration->span.start;
                     });

    for (const SumCandidate& sumCandidate : candidates) {
      const AstDeclaration& candidateDeclaration = *sumCandidate.declaration;
      if (!isClassLikeDeclaration(candidateDeclaration.kind)) {
        continue;
      }
      const std::string candidateName =
          declarationSymbolName(candidateDeclaration, sumCandidate.owner);
      auto candidate = globalSymbols_.find(candidateName);
      if (candidate == globalSymbols_.end() ||
          std::find(candidate->second.parentSymbolNames.begin(),
                    candidate->second.parentSymbolNames.end(),
                    targetName) == candidate->second.parentSymbolNames.end()) {
        continue;
      }
      if (candidateDeclaration.kind != AstDeclarationKind::Class &&
          candidateDeclaration.kind != AstDeclarationKind::Object) {
        diagnostics_.error(candidateDeclaration.span,
                           "sum mirror child must be a concrete class or object: " +
                               candidateName);
        unsupportedChild = true;
        continue;
      }
      if (!generic && !candidate->second.typeParameters.empty()) {
        diagnostics_.error(
            candidateDeclaration.span,
            "sum mirror child of a monomorphic sealed trait must be non-generic: " +
                candidateName);
        unsupportedChild = true;
        continue;
      }

      std::vector<TypeInfo> childTypeArguments;
      if (generic) {
        const auto directParent =
            std::find(candidate->second.parentSymbolNames.begin(),
                      candidate->second.parentSymbolNames.end(), targetName);
        const std::size_t parentIndex = static_cast<std::size_t>(
            std::distance(candidate->second.parentSymbolNames.begin(), directParent));
        const TypeInfo* parentType = parentIndex < candidate->second.parentTypes.size()
                                         ? &candidate->second.parentTypes[parentIndex]
                                         : nullptr;
        bool applicable = parentType != nullptr && parentType->typeArguments.size() ==
                                                       target.typeParameters.size();
        std::vector<std::size_t> childParameterMappings(
            candidate->second.typeParameters.size(), target.typeParameters.size());
        for (std::size_t targetIndex = 0;
             applicable && targetIndex < target.typeParameters.size(); ++targetIndex) {
          const TypeInfo& parentArgument = parentType->typeArguments[targetIndex];
          if (!parentArgument.typeParameter) {
            applicable = fixedParentArgumentAppliesToEveryTarget(
                target.typeParameters[targetIndex], parentArgument);
            continue;
          }

          const auto childParameter = std::find_if(
              candidate->second.typeParameters.begin(),
              candidate->second.typeParameters.end(),
              [&](const TypeParameterInfo& parameter) {
                return parameter.symbolName == parentArgument.typeParameterSymbolName;
              });
          if (childParameter == candidate->second.typeParameters.end()) {
            applicable = false;
            continue;
          }
          const std::size_t childIndex = static_cast<std::size_t>(
              std::distance(candidate->second.typeParameters.begin(), childParameter));
          if (childParameterMappings[childIndex] != target.typeParameters.size() &&
              childParameterMappings[childIndex] != targetIndex) {
            applicable = false;
            continue;
          }
          childParameterMappings[childIndex] = targetIndex;
        }
        applicable =
            applicable &&
            std::none_of(childParameterMappings.begin(), childParameterMappings.end(),
                         [&](std::size_t targetIndex) {
                           return targetIndex == target.typeParameters.size();
                         });

        std::unordered_map<std::string, TypeInfo> childSubstitutions;
        childTypeArguments.reserve(candidate->second.typeParameters.size());
        for (std::size_t childIndex = 0;
             applicable && childIndex < candidate->second.typeParameters.size();
             ++childIndex) {
          const std::size_t targetIndex = childParameterMappings[childIndex];
          childTypeArguments.push_back(derivingTypeArguments[targetIndex]);
          childSubstitutions[candidate->second.typeParameters[childIndex].symbolName] =
              derivingTypeArguments[targetIndex];
        }
        for (std::size_t childIndex = 0;
             applicable && childIndex < candidate->second.typeParameters.size();
             ++childIndex) {
          const TypeParameterInfo& childParameter =
              candidate->second.typeParameters[childIndex];
          const TypeParameterInfo& targetParameter =
              target.typeParameters[childParameterMappings[childIndex]];
          const TypeInfo childLower =
              substituteTypeParameters(childParameter.lowerBound, childSubstitutions);
          const TypeInfo childUpper =
              substituteTypeParameters(childParameter.upperBound, childSubstitutions);
          applicable = isAssignable(targetParameter.lowerBound, childLower) &&
                       isAssignable(childUpper, targetParameter.upperBound);
        }
        if (!applicable) {
          diagnostics_.error(
              candidateDeclaration.span,
              "generic sum mirror child parent type must be applicable to every "
              "sealed trait type argument combination: " +
                  candidateName);
          unsupportedChild = true;
          continue;
        }
      }

      const std::string elementType =
          generic && !childTypeArguments.empty()
              ? specializeResolvedTypeApplication(candidate->second, childTypeArguments,
                                                  candidateDeclaration.span, false)
                    .type.name
              : candidate->second.type.name;
      children.push_back(
          SumChild{&candidateDeclaration, candidate->second, elementType});
    }
    if (unsupportedChild) {
      continue;
    }
    if (children.empty()) {
      diagnostics_.error(declaration.span,
                         "sum mirror derivation requires at least one direct child: " +
                             targetName);
      continue;
    }

    std::size_t syntheticSpanOffset = 1000000 + mirrorDeclarations_.size() * 1000;
    const auto nextSpan = [&]() {
      support::SourceSpan span = declaration.span;
      span.length += syntheticSpanOffset++;
      return span;
    };

    AstDeclaration implementation;
    implementation.kind = AstDeclarationKind::Class;
    implementation.name = sumMirrorImplementationName(targetName);
    implementation.span = nextSpan();
    implementation.typeParameters = declaration.typeParameters;
    for (AstTypeParameter& parameter : implementation.typeParameters) {
      parameter.variance = TypeVariance::Invariant;
    }
    implementation.parentTypes = {"scala.deriving.Mirror.SumOf[" + derivingType.name +
                                  "]"};

    const auto addTypeAlias = [&](std::string name, std::string aliasTarget) {
      AstDeclaration alias;
      alias.kind = AstDeclarationKind::Type;
      alias.name = std::move(name);
      alias.span = nextSpan();
      alias.declaredType = std::move(aliasTarget);
      alias.isOverride = true;
      alias.hasInitializer = true;
      implementation.members.push_back(std::move(alias));
    };

    std::vector<std::string> elementTypes;
    std::vector<std::string> elementLabels;
    elementTypes.reserve(children.size());
    elementLabels.reserve(children.size());
    for (const SumChild& child : children) {
      elementTypes.push_back(child.elementType);
      elementLabels.push_back(stringSingletonType(child.declaration->name));
    }
    addTypeAlias("MirroredElemTypes", tupleTypeName(elementTypes));
    addTypeAlias("MirroredLabel", stringSingletonType(declaration.name));
    addTypeAlias("MirroredElemLabels", tupleTypeName(elementLabels));

    AstExpression fallback;
    fallback.kind = AstExpressionKind::IntegerLiteral;
    fallback.text = std::to_string(children.size() - 1);
    fallback.span = nextSpan();
    for (std::size_t reverseIndex = children.size() - 1; reverseIndex > 0;
         --reverseIndex) {
      const std::size_t childIndex = reverseIndex - 1;

      AstExpression value;
      value.kind = AstExpressionKind::Identifier;
      value.text = "value";
      value.span = nextSpan();

      AstExpression typeTestMember;
      typeTestMember.kind = AstExpressionKind::Select;
      typeTestMember.text = support::StdNames::IsInstanceOf;
      typeTestMember.span = nextSpan();
      typeTestMember.children.push_back(std::move(value));

      AstExpression typeTest;
      typeTest.kind = AstExpressionKind::TypeApply;
      typeTest.declaredType = children[childIndex].elementType;
      typeTest.typeArguments = {children[childIndex].elementType};
      typeTest.span = nextSpan();
      typeTest.children.push_back(std::move(typeTestMember));

      AstExpression ordinalValue;
      ordinalValue.kind = AstExpressionKind::IntegerLiteral;
      ordinalValue.text = std::to_string(childIndex);
      ordinalValue.span = nextSpan();

      AstExpression branch;
      branch.kind = AstExpressionKind::If;
      branch.span = nextSpan();
      branch.children.push_back(std::move(typeTest));
      branch.children.push_back(std::move(ordinalValue));
      branch.children.push_back(std::move(fallback));
      fallback = std::move(branch);
    }

    AstDeclaration ordinal;
    ordinal.kind = AstDeclarationKind::Def;
    ordinal.name = "ordinal";
    ordinal.span = nextSpan();
    ordinal.parameters = {"value: " + derivingType.name};
    ordinal.contextualParameters = {false};
    ordinal.declaredType = "Int";
    ordinal.isOverride = true;
    ordinal.hasInitializer = true;
    ordinal.initializer = std::move(fallback);
    implementation.members.push_back(std::move(ordinal));

    mirrorDeclarations_.push_back(std::move(implementation));
    AstDeclaration& storedImplementation = mirrorDeclarations_.back();
    collectDeclaration(storedImplementation, currentPackageName_, scope);
    const std::string implementationName =
        declarationSymbolName(storedImplementation, currentPackageName_);

    const TypeInfo mirrorType = specializeResolvedTypeApplication(
                                    sumOf, {derivingType}, declaration.span, false)
                                    .type;
    const std::string instanceOwner = targetName + '$';
    const std::string instanceName = "$mirror$Sum$type";

    TypedDeclaration member;
    member.kind = generic ? AstDeclarationKind::Def : AstDeclarationKind::Val;
    member.name = instanceName;
    member.symbolName = qualify(instanceOwner, instanceName);
    member.span = declaration.span;
    member.typeParameters =
        generic ? target.typeParameters : std::vector<TypeParameterInfo>{};
    member.declaredType = mirrorType.name;
    member.inferredType = mirrorType;
    member.isGiven = true;
    member.hasInitializer = true;
    AstExpression mirrorConstructor;
    mirrorConstructor.kind = AstExpressionKind::New;
    mirrorConstructor.text = implementationName;
    mirrorConstructor.span = nextSpan();
    if (generic) {
      member.initializer.kind = AstExpressionKind::TypeApply;
      member.initializer.span = nextSpan();
      for (const TypeParameterInfo& parameter : target.typeParameters) {
        member.initializer.typeArguments.push_back(parameter.name);
      }
      member.initializer.declaredType = member.initializer.typeArguments.front();
      member.initializer.children.push_back(std::move(mirrorConstructor));
    } else {
      member.initializer = std::move(mirrorConstructor);
    }
    derivedInstances_.push_back(
        DerivedInstanceInfo{instanceOwner, std::move(member), {}});

    SymbolInfo candidate;
    candidate.kind = generic ? AstDeclarationKind::Def : AstDeclarationKind::Val;
    candidate.name = "derived$Mirror$Sum";
    candidate.symbolName = qualify(instanceOwner, instanceName);
    candidate.type = mirrorType;
    candidate.typeParameters =
        generic ? target.typeParameters : std::vector<TypeParameterInfo>{};
    candidate.hasImplementation = true;
    candidate.isGiven = true;
    candidate.isModuleMember = !generic;
    derivedGivens_[targetName].push_back(std::move(candidate));
  }
}

void Typechecker::collectDerivedGivens(const std::vector<AstDeclaration>& declarations,
                                       const std::string& owner, const Scope& scope) {
  for (const AstDeclaration& declaration : declarations) {
    if (declaration.name.empty() || (declaration.kind != AstDeclarationKind::Class &&
                                     declaration.kind != AstDeclarationKind::Trait &&
                                     declaration.kind != AstDeclarationKind::Object)) {
      continue;
    }

    const std::string targetName = declarationSymbolName(declaration, owner);
    auto target = globalSymbols_.find(targetName);
    if (target == globalSymbols_.end()) {
      continue;
    }

    Scope declarationScope = scope;
    if (auto members = memberScopes_.find(targetName); members != memberScopes_.end()) {
      mergeScope(declarationScope, members->second);
    }

    for (const std::string& derivedTypeName : declaration.derivedTypes) {
      const SymbolInfo* typeclass =
          typeSymbolForDeclaredName(derivedTypeName, &declarationScope);
      if (typeclass == nullptr || (typeclass->kind != AstDeclarationKind::Class &&
                                   typeclass->kind != AstDeclarationKind::Trait)) {
        diagnostics_.error(declaration.span,
                           "unresolved derived type class: " + derivedTypeName);
        continue;
      }
      if (typeclass->typeParameters.size() != 1) {
        diagnostics_.error(
            declaration.span,
            "derived type class " + derivedTypeName +
                " must have exactly one type parameter in this derives milestone");
        continue;
      }

      std::vector<TypeInfo> derivingTypeArguments;
      derivingTypeArguments.reserve(target->second.typeParameters.size());
      for (const TypeParameterInfo& parameter : target->second.typeParameters) {
        TypeInfo parameterType{SimpleTypeKind::Object, parameter.name};
        parameterType.runtimeName = parameter.upperBound.runtimeName.empty()
                                        ? parameter.upperBound.name
                                        : parameter.upperBound.runtimeName;
        if (parameterType.runtimeName.empty() ||
            parameterType.runtimeName == "Unknown") {
          parameterType.runtimeName = "Object";
        }
        parameterType.typeParameterSymbolName = parameter.symbolName;
        parameterType.typeParameter = true;
        derivingTypeArguments.push_back(std::move(parameterType));
      }
      const TypeInfo derivingType =
          derivingTypeArguments.empty()
              ? target->second.type
              : specializeResolvedTypeApplication(target->second, derivingTypeArguments,
                                                  declaration.span, false)
                    .type;
      const TypeInfo expected = specializeResolvedTypeApplication(
                                    *typeclass, {derivingType}, declaration.span, false)
                                    .type;
      auto companionMembers = memberScopes_.find(typeclass->symbolName + '$');
      if (companionMembers == memberScopes_.end()) {
        diagnostics_.error(declaration.span,
                           "derived type class " + derivedTypeName +
                               " requires a companion method named derived");
        continue;
      }
      auto method = companionMembers->second.find("derived");
      if (method == companionMembers->second.end() ||
          method->second.kind != AstDeclarationKind::Def) {
        diagnostics_.error(declaration.span,
                           "derived type class " + derivedTypeName +
                               " requires a companion method named derived");
        continue;
      }

      SymbolInfo candidate =
          inferTypeApplication(method->second, {}, declaration.span, &expected, false);
      if (!candidate.typeParameters.empty() ||
          candidate.type.kind == SimpleTypeKind::Unknown ||
          !isAssignable(expected, candidate.type)) {
        diagnostics_.error(declaration.span, "method " + typeclass->name +
                                                 ".derived cannot produce " +
                                                 expected.name);
        continue;
      }
      const bool allContextual =
          candidate.contextualParameters.size() == candidate.parameterTypes.size() &&
          std::all_of(candidate.contextualParameters.begin(),
                      candidate.contextualParameters.end(),
                      [](bool contextual) { return contextual; });
      if (!allContextual) {
        diagnostics_.error(declaration.span,
                           "method " + typeclass->name +
                               ".derived may only have using parameters");
        continue;
      }

      std::vector<std::string> prerequisiteParameters;
      std::vector<TypeInfo> prerequisiteTypes;
      prerequisiteParameters.reserve(derivingTypeArguments.size());
      prerequisiteTypes.reserve(derivingTypeArguments.size());
      for (const TypeInfo& argument : derivingTypeArguments) {
        TypeInfo prerequisite = specializeResolvedTypeApplication(
                                    *typeclass, {argument}, declaration.span, false)
                                    .type;
        prerequisiteParameters.push_back("derived$" + argument.name + ": " +
                                         prerequisite.name);
        prerequisiteTypes.push_back(std::move(prerequisite));
      }
      candidate.parameters.insert(candidate.parameters.begin(),
                                  prerequisiteParameters.begin(),
                                  prerequisiteParameters.end());
      candidate.parameterTypes.insert(candidate.parameterTypes.begin(),
                                      prerequisiteTypes.begin(),
                                      prerequisiteTypes.end());
      candidate.contextualParameters.insert(candidate.contextualParameters.begin(),
                                            derivingTypeArguments.size(), true);
      candidate.contextPrerequisiteCount = derivingTypeArguments.size();
      candidate.typeParameters = target->second.typeParameters;

      candidate.name = "derived$" + typeclass->name;
      candidate.isGiven = true;
      if (derivingTypeArguments.empty()) {
        Scope initializerScope = scope;
        std::vector<TypedContextArgument> factoryArguments =
            resolveContextArguments(candidate, 0, initializerScope, declaration.span);
        const std::string instanceName = derivedInstanceName(
            typeclass->symbolName, declaration.kind == AstDeclarationKind::Object);
        const std::string instanceOwner = declaration.kind == AstDeclarationKind::Object
                                              ? targetName
                                              : targetName + '$';
        support::SourceSpan initializerSpan = declaration.span;
        initializerSpan.length += derivedInstances_.size() + 1;

        TypedDeclaration member;
        member.kind = AstDeclarationKind::Val;
        member.name = instanceName;
        member.symbolName = qualify(instanceOwner, instanceName);
        member.span = declaration.span;
        member.declaredType = expected.name;
        member.inferredType = expected;
        member.isGiven = true;
        member.hasInitializer = true;
        member.initializer.kind = AstExpressionKind::Call;
        member.initializer.span = initializerSpan;
        AstExpression factory;
        factory.kind = AstExpressionKind::Identifier;
        factory.text = candidate.symbolName;
        factory.span = initializerSpan;
        member.initializer.children.push_back(std::move(factory));
        derivedInstances_.push_back(DerivedInstanceInfo{
            instanceOwner, std::move(member), std::move(factoryArguments)});

        candidate.kind = AstDeclarationKind::Val;
        candidate.symbolName = qualify(instanceOwner, instanceName);
        candidate.parameters.clear();
        candidate.parameterTypes.clear();
        candidate.contextualParameters.clear();
        candidate.isModuleMember = true;
        candidate.contextPrerequisiteCount = 0;
      }
      derivedGivens_[targetName].push_back(std::move(candidate));
    }

    if (declaration.kind == AstDeclarationKind::Object) {
      collectDerivedGivens(declaration.members, targetName, declarationScope);
    }
  }
}

void Typechecker::attachDerivedInstances(std::vector<TypedDeclaration>& declarations) {
  const auto findDeclaration = [&](const auto& self,
                                   std::vector<TypedDeclaration>& candidates,
                                   const std::string& symbolName) -> TypedDeclaration* {
    for (TypedDeclaration& candidate : candidates) {
      if (candidate.symbolName == symbolName) {
        return &candidate;
      }
      if (TypedDeclaration* nested = self(self, candidate.members, symbolName);
          nested != nullptr) {
        return nested;
      }
    }
    return nullptr;
  };

  for (DerivedInstanceInfo& instance : derivedInstances_) {
    TypedDeclaration* owner =
        findDeclaration(findDeclaration, declarations, instance.ownerSymbolName);
    if (owner == nullptr) {
      TypedDeclaration companion;
      companion.kind = AstDeclarationKind::Object;
      const std::size_t separator = instance.ownerSymbolName.rfind('.');
      companion.name = separator == std::string::npos
                           ? instance.ownerSymbolName
                           : instance.ownerSymbolName.substr(separator + 1);
      if (companion.name.ends_with('$')) {
        companion.name.pop_back();
      }
      companion.symbolName = instance.ownerSymbolName;
      companion.span = instance.member.span;
      companion.inferredType =
          TypeInfo{SimpleTypeKind::Object, instance.ownerSymbolName};
      const std::size_t enclosingSeparator = instance.ownerSymbolName.rfind('.');
      TypedDeclaration* enclosing =
          enclosingSeparator == std::string::npos
              ? nullptr
              : findDeclaration(findDeclaration, declarations,
                                instance.ownerSymbolName.substr(0, enclosingSeparator));
      if (enclosing != nullptr && enclosing->kind == AstDeclarationKind::Object) {
        const std::size_t companionIndex = enclosing->members.size();
        enclosing->members.push_back(std::move(companion));
        enclosing->classBodyItems.push_back(
            AstClassBodyItem{AstClassBodyItemKind::Declaration, companionIndex});
        owner = &enclosing->members.back();
      } else {
        declarations.push_back(std::move(companion));
        owner = &declarations.back();
      }
    }

    const std::size_t memberIndex = owner->members.size();
    recordContextApplication(instance.member.initializer.span,
                             std::move(instance.factoryArguments));
    owner->members.push_back(std::move(instance.member));
    owner->classBodyItems.push_back(
        AstClassBodyItem{AstClassBodyItemKind::Declaration, memberIndex});
  }
}

std::string Typechecker::desugarGivenImportUserInfix(
    const std::string& filter, const Scope& scope, bool* malformed) const {
  struct OperatorToken {
    std::size_t start = 0;
    std::size_t end = 0;
    std::string name;
    int precedence = 0;
    bool rightAssociative = false;
    bool builtIn = false;
  };

  const auto isIdentifierStart = [](char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           ch == '_' || ch == '$';
  };
  const auto isIdentifierPart = [&](char ch) {
    return isIdentifierStart(ch) || (ch >= '0' && ch <= '9');
  };
  const auto nextNonSpace = [](std::string_view text, std::size_t start) {
    while (start < text.size() &&
           (text[start] == ' ' || text[start] == '\t' ||
            text[start] == '\n' || text[start] == '\r')) {
      ++start;
    }
    return start;
  };
  const auto isSymbolicPart = [](char ch) {
    switch (ch) {
    case '!':
    case '#':
    case '%':
    case '&':
    case '*':
    case '+':
    case '-':
    case '/':
    case ':':
    case '<':
    case '=':
    case '>':
    case '?':
    case '\\':
    case '^':
    case '|':
    case '~':
      return true;
    default:
      return false;
    }
  };
  const auto precedence = [&](std::string_view name) {
    if (name.size() > 1 && name.back() == '=' && name.front() != '=' &&
        name != "<=" && name != ">=" && name != "!=") {
      return 0;
    }
    const char first = name.front();
    if (isIdentifierStart(first)) {
      return 1;
    }
    switch (first) {
    case '|':
      return 2;
    case '^':
      return 3;
    case '&':
      return 4;
    case '=':
    case '!':
      return 5;
    case '<':
    case '>':
      return 6;
    case ':':
      return 7;
    case '+':
    case '-':
      return 8;
    case '*':
    case '/':
    case '%':
      return 9;
    default:
      return 10;
    }
  };
  const auto isBinaryTypeConstructor = [&](const std::string& name) {
    const SymbolInfo* symbol = typeSymbolForDeclaredName(name, &scope);
    return symbol != nullptr &&
           (symbol->kind == AstDeclarationKind::Class ||
            symbol->kind == AstDeclarationKind::Trait ||
            (symbol->kind == AstDeclarationKind::Type &&
             symbol->hasImplementation)) &&
           symbol->typeParameters.size() == 2;
  };

  std::function<std::string(
      const TypeInfo&, const std::unordered_map<std::string, std::string>&)>
      renderAliasType;
  renderAliasType =
      [&](const TypeInfo& type,
          const std::unordered_map<std::string, std::string>& substitutions) {
        if (type.typeParameter) {
          auto replacement =
              substitutions.find(type.typeParameterSymbolName);
          if (replacement != substitutions.end()) {
            return replacement->second;
          }
        }
        if (type.compositeKind != CompositeTypeKind::None) {
          const char operation =
              type.compositeKind == CompositeTypeKind::Union ? '|' : '&';
          std::string rendered{"("};
          for (std::size_t i = 0; i < type.compositeTypes.size(); ++i) {
            if (i != 0) {
              rendered += operation;
            }
            rendered +=
                renderAliasType(type.compositeTypes[i], substitutions);
          }
          rendered += ")";
          return rendered;
        }
        const std::string& constructorName =
            !type.typeConstructorName.empty() ? type.typeConstructorName
                                              : type.runtimeName;
        if (!constructorName.empty() && !type.typeArguments.empty()) {
          std::string rendered = constructorName + "[";
          for (std::size_t i = 0; i < type.typeArguments.size(); ++i) {
            if (i != 0) {
              rendered += ",";
            }
            rendered += renderAliasType(type.typeArguments[i], substitutions);
          }
          rendered += "]";
          return rendered;
        }
        return type.name;
      };

  std::unordered_set<std::string> expandingAliases;
  std::function<std::string(std::string)> desugar;
  desugar = [&](std::string source) -> std::string {
    source = trim(source);
    if (source.empty()) {
      *malformed = true;
      return source;
    }

    std::vector<char> delimiters;
    for (char ch : source) {
      if (ch == '[' || ch == '(') {
        delimiters.push_back(ch);
      } else if (ch == ']' || ch == ')') {
        const char expected = ch == ']' ? '[' : '(';
        if (delimiters.empty() || delimiters.back() != expected) {
          *malformed = true;
          return source;
        }
        delimiters.pop_back();
      }
    }
    if (!delimiters.empty()) {
      *malformed = true;
      return source;
    }

    if (source.front() == '(') {
      std::size_t depth = 0;
      std::size_t closing = std::string::npos;
      for (std::size_t i = 0; i < source.size(); ++i) {
        if (source[i] == '(') {
          ++depth;
        } else if (source[i] == ')') {
          --depth;
          if (depth == 0) {
            closing = i;
            break;
          }
        }
      }
      if (closing + 1 == source.size()) {
        return "(" + desugar(source.substr(1, source.size() - 2)) + ")";
      }
    }

    std::vector<OperatorToken> operators;
    std::size_t bracketDepth = 0;
    std::size_t parenthesisDepth = 0;
    for (std::size_t i = 0; i < source.size();) {
      if (source[i] == '[') {
        ++bracketDepth;
        ++i;
        continue;
      }
      if (source[i] == ']') {
        --bracketDepth;
        ++i;
        continue;
      }
      if (source[i] == '(') {
        ++parenthesisDepth;
        ++i;
        continue;
      }
      if (source[i] == ')') {
        --parenthesisDepth;
        ++i;
        continue;
      }
      if (bracketDepth != 0 || parenthesisDepth != 0) {
        ++i;
        continue;
      }

      const bool alphanumeric = isIdentifierStart(source[i]);
      const bool symbolic = isSymbolicPart(source[i]);
      if (!alphanumeric && !symbolic) {
        ++i;
        continue;
      }
      const std::size_t start = i++;
      if (alphanumeric) {
        while (i < source.size() && isIdentifierPart(source[i])) {
          ++i;
        }
      } else {
        while (i < source.size() && isSymbolicPart(source[i])) {
          ++i;
        }
      }
      const std::string name = source.substr(start, i - start);
      if (alphanumeric &&
          ((start != 0 && source[start - 1] == '.') ||
           (i < source.size() && source[i] == '.'))) {
        continue;
      }
      const bool builtIn = name == "|" || name == "&";
      if (!builtIn && !isBinaryTypeConstructor(name)) {
        continue;
      }
      const std::size_t after = nextNonSpace(source, i);
      if (after < source.size() && source[after] == '[') {
        continue;
      }
      operators.push_back(OperatorToken{start, i, name, precedence(name),
                                        name.ends_with(':'), builtIn});
    }

    if (!operators.empty()) {
      int lowestPrecedence = operators.front().precedence;
      for (const OperatorToken& operation : operators) {
        lowestPrecedence = std::min(lowestPrecedence, operation.precedence);
      }
      bool rightAssociative = false;
      for (const OperatorToken& operation : operators) {
        if (operation.precedence == lowestPrecedence) {
          rightAssociative = operation.rightAssociative;
          break;
        }
      }
      for (const OperatorToken& operation : operators) {
        if (operation.precedence == lowestPrecedence &&
            operation.rightAssociative != rightAssociative) {
          *malformed = true;
          return source;
        }
      }

      const OperatorToken* selected = nullptr;
      for (const OperatorToken& operation : operators) {
        if (operation.precedence != lowestPrecedence) {
          continue;
        }
        if (selected == nullptr || !rightAssociative) {
          selected = &operation;
        }
      }
      const OperatorToken& operation = *selected;
      const std::string left = trim(source.substr(0, operation.start));
      const std::string right = trim(source.substr(operation.end));
      if (left.empty() || right.empty()) {
        if (!operation.builtIn) {
          *malformed = true;
        }
        return source;
      }
      if (operation.builtIn) {
        return desugar(left) + operation.name + desugar(right);
      }
      return desugar(operation.name + "[" + desugar(left) + "," +
                     desugar(right) + "]");
    }

    std::size_t open = std::string::npos;
    std::size_t roundDepth = 0;
    for (std::size_t i = 0; i < source.size(); ++i) {
      if (source[i] == '(') {
        ++roundDepth;
      } else if (source[i] == ')') {
        --roundDepth;
      } else if (source[i] == '[' && roundDepth == 0) {
        open = i;
        break;
      }
    }
    if (open == std::string::npos || source.back() != ']') {
      return source;
    }

    std::vector<std::string> arguments;
    std::size_t squareDepth = 0;
    std::size_t nestedRoundDepth = 0;
    std::size_t argumentStart = open + 1;
    for (std::size_t i = open + 1; i + 1 < source.size(); ++i) {
      if (source[i] == '[') {
        ++squareDepth;
      } else if (source[i] == ']') {
        --squareDepth;
      } else if (source[i] == '(') {
        ++nestedRoundDepth;
      } else if (source[i] == ')') {
        --nestedRoundDepth;
      } else if (source[i] == ',' && squareDepth == 0 &&
                 nestedRoundDepth == 0) {
        arguments.push_back(source.substr(argumentStart, i - argumentStart));
        argumentStart = i + 1;
      }
    }
    arguments.push_back(
        source.substr(argumentStart, source.size() - argumentStart - 1));
    std::vector<std::string> rewrittenArguments;
    rewrittenArguments.reserve(arguments.size());
    for (const std::string& argument : arguments) {
      rewrittenArguments.push_back(desugar(argument));
    }

    const std::string constructorName = trim(source.substr(0, open));
    const SymbolInfo* constructor =
        typeSymbolForDeclaredName(constructorName, &scope);
    if (constructor != nullptr &&
        constructor->kind == AstDeclarationKind::Type &&
        constructor->hasImplementation &&
        constructor->typeParameters.size() == rewrittenArguments.size()) {
      if (!expandingAliases.insert(constructor->symbolName).second) {
        *malformed = true;
        return source;
      }
      std::unordered_map<std::string, std::string> substitutions;
      for (std::size_t i = 0; i < rewrittenArguments.size(); ++i) {
        substitutions[constructor->typeParameters[i].symbolName] =
            rewrittenArguments[i];
      }
      const std::string expanded =
          renderAliasType(constructor->type, substitutions);
      const std::string rewritten = desugar(expanded);
      expandingAliases.erase(constructor->symbolName);
      return rewritten;
    }

    std::string rewritten = constructorName + "[";
    for (std::size_t i = 0; i < rewrittenArguments.size(); ++i) {
      if (i != 0) {
        rewritten += ",";
      }
      rewritten += rewrittenArguments[i];
    }
    rewritten += "]";
    return rewritten;
  };

  return desugar(filter);
}

bool Typechecker::givenImportTypeMatches(const std::string& filter,
                                         const SymbolInfo& candidate,
                                         const Scope& scope) const {
  bool malformedUserInfix = false;
  const std::string desugaredFilter =
      desugarGivenImportUserInfix(filter, scope, &malformedUserInfix);
  if (malformedUserInfix) {
    return false;
  }
  std::unordered_set<std::string> visiting;
  std::function<bool(const std::string&, const TypeInfo&)> matches;
  std::function<bool(const std::string&, const TypeInfo&)> patternConformsTo;
  patternConformsTo = [&](const std::string& patternName,
                          const TypeInfo& target) {
    bool malformed = false;
    const std::string pattern =
        normalizeGivenImportType(patternName, &malformed);
    if (malformed) {
      return false;
    }
    const CompositeTypeSyntax infix =
        parseCompositeTypeSyntax(pattern);
    if (infix.malformed) {
      return false;
    }
    if (infix.kind == CompositeTypeSyntaxKind::Union) {
      return std::all_of(
          infix.operands.begin(), infix.operands.end(),
          [&](const std::string& operand) {
            return patternConformsTo(operand, target);
          });
    }
    if (infix.kind == CompositeTypeSyntaxKind::Intersection) {
      return std::any_of(
          infix.operands.begin(), infix.operands.end(),
          [&](const std::string& operand) {
            return patternConformsTo(operand, target);
          });
    }
    const TypeInfo source = typeFromDeclaredName(pattern, &scope);
    return source.kind != SimpleTypeKind::Unknown &&
           isAssignable(target, source);
  };
  matches = [&](const std::string& patternName, const TypeInfo& value) {
    bool malformed = false;
    const std::string pattern =
        normalizeGivenImportType(patternName, &malformed);
    if (malformed) {
      return false;
    }
    const WildcardTypeSyntax wildcard = parseWildcardTypeSyntax(pattern);
    if (wildcard.wildcard) {
      if (wildcard.malformed) {
        return false;
      }

      if (value.typeParameter) {
        const auto parameter = std::find_if(
            candidate.typeParameters.begin(), candidate.typeParameters.end(),
            [&](const TypeParameterInfo& info) {
              return info.symbolName == value.typeParameterSymbolName;
            });
        if (parameter == candidate.typeParameters.end()) {
          return false;
        }
        return (wildcard.lowerBound.empty() ||
                patternConformsTo(wildcard.lowerBound,
                                  parameter->lowerBound)) &&
               (wildcard.upperBound.empty() ||
                isUniversalWildcardUpperBound(wildcard.upperBound) ||
                matches(wildcard.upperBound, parameter->upperBound));
      }
      return (wildcard.lowerBound.empty() ||
              patternConformsTo(wildcard.lowerBound, value)) &&
             (wildcard.upperBound.empty() ||
              isUniversalWildcardUpperBound(wildcard.upperBound) ||
              matches(wildcard.upperBound, value));
    }

    const CompositeTypeSyntax infix =
        parseCompositeTypeSyntax(pattern);
    if (infix.malformed) {
      return false;
    }
    if (infix.kind == CompositeTypeSyntaxKind::Union) {
      return std::any_of(
          infix.operands.begin(), infix.operands.end(),
          [&](const std::string& operand) { return matches(operand, value); });
    }
    if (infix.kind == CompositeTypeSyntaxKind::Intersection) {
      return std::all_of(
          infix.operands.begin(), infix.operands.end(),
          [&](const std::string& operand) { return matches(operand, value); });
    }

    const AppliedTypeSyntax applied = parseAppliedTypeSyntax(pattern);
    if (!applied.applied || applied.malformed) {
      const TypeInfo expected = typeFromDeclaredName(pattern, &scope);
      return expected.kind != SimpleTypeKind::Unknown &&
             isAssignable(expected, value);
    }

    const SymbolInfo* constructor =
        typeSymbolForDeclaredName(applied.constructor, &scope);
    if (constructor != nullptr &&
        constructor->kind == AstDeclarationKind::Type &&
        constructor->hasImplementation) {
      const TypeInfo expected = typeFromDeclaredName(pattern, &scope);
      return expected.kind != SimpleTypeKind::Unknown &&
             isAssignable(expected, value);
    }
    if (constructor == nullptr ||
        (constructor->kind != AstDeclarationKind::Class &&
         constructor->kind != AstDeclarationKind::Trait) ||
        constructor->typeParameters.size() != applied.arguments.size()) {
      return false;
    }

    std::function<bool(const TypeInfo&)> matchesConstructor;
    matchesConstructor = [&](const TypeInfo& current) {
      if (current.typeConstructorName == constructor->symbolName &&
          current.typeArguments.size() == applied.arguments.size()) {
        for (std::size_t i = 0; i < applied.arguments.size(); ++i) {
          const TypeVariance variance =
              constructor->typeParameters[i].variance;
          const bool covariantMatch =
              matches(applied.arguments[i], current.typeArguments[i]);
          const bool contravariantMatch =
              patternConformsTo(applied.arguments[i],
                                current.typeArguments[i]);
          const bool argumentMatches =
              variance == TypeVariance::Covariant
                  ? covariantMatch
                  : variance == TypeVariance::Contravariant
                        ? contravariantMatch
                        : covariantMatch &&
                              (applied.arguments[i].find('?') !=
                                   std::string::npos ||
                               contravariantMatch);
          if (!argumentMatches) {
            return false;
          }
        }
        return true;
      }

      const std::string currentName =
          current.typeConstructorName.empty()
              ? (current.runtimeName.empty() ? current.name : current.runtimeName)
              : current.typeConstructorName;
      const std::string visitKey = pattern + " <- " + currentName;
      if (!visiting.insert(visitKey).second) {
        return false;
      }
      auto currentSymbol = globalSymbols_.find(currentName);
      if (currentSymbol != globalSymbols_.end()) {
        for (const TypeInfo& parentPattern : currentSymbol->second.parentTypes) {
          const TypeInfo parent =
              specializeTypeForReceiver(parentPattern, current);
          if (matchesConstructor(parent)) {
            visiting.erase(visitKey);
            return true;
          }
        }
      }
      visiting.erase(visitKey);
      return false;
    };
    return matchesConstructor(value);
  };
  return matches(desugaredFilter, candidate.type);
}

bool Typechecker::givenImportMatches(const AstDeclaration& declaration,
                                     const SymbolInfo& symbol,
                                     const Scope& scope) const {
  if (!symbol.isGiven) {
    return false;
  }
  if (declaration.importsGivens) {
    return true;
  }
  return std::any_of(
      declaration.importGivenTypes.begin(), declaration.importGivenTypes.end(),
      [&](const std::string& filter) {
        return givenImportTypeMatches(filter, symbol, scope);
      });
}

bool Typechecker::validateGivenImportFilter(
    const std::string& filter, const Scope& scope,
    const support::SourceSpan& span) const {
  bool malformedUserInfix = false;
  const std::string desugaredFilter =
      desugarGivenImportUserInfix(filter, scope, &malformedUserInfix);
  if (malformedUserInfix) {
    diagnostics_.error(span, "malformed user-defined infix type in given import "
                             "filter: " +
                                 filter);
    return false;
  }
  std::function<bool(const std::string&)> validate;
  validate = [&](const std::string& patternName) {
    bool malformed = false;
    const std::string pattern =
        normalizeGivenImportType(patternName, &malformed);
    if (malformed) {
      diagnostics_.error(
          span, "malformed parenthesized type in given import filter: " +
                    patternName);
      return false;
    }
    const WildcardTypeSyntax wildcard = parseWildcardTypeSyntax(pattern);
    if (wildcard.wildcard) {
      if (wildcard.malformed) {
        diagnostics_.error(span, "malformed wildcard bounds in given import filter: " +
                                     patternName);
        return false;
      }
      if ((!wildcard.lowerBound.empty() && !validate(wildcard.lowerBound)) ||
          (!wildcard.upperBound.empty() && !validate(wildcard.upperBound))) {
        return false;
      }
      const auto containsInfix = [](const std::string& bound) {
        bool malformedBound = false;
        const std::string normalized =
            normalizeGivenImportType(bound, &malformedBound);
        return !malformedBound &&
               parseCompositeTypeSyntax(normalized).kind !=
                   CompositeTypeSyntaxKind::None;
      };
      if (containsInfix(wildcard.lowerBound) ||
          containsInfix(wildcard.upperBound)) {
        return true;
      }
      bool malformedLower = false;
      bool malformedUpper = false;
      const std::string normalizedLower =
          normalizeGivenImportType(wildcard.lowerBound, &malformedLower);
      const std::string normalizedUpper =
          normalizeGivenImportType(wildcard.upperBound, &malformedUpper);
      const TypeInfo lower =
          wildcard.lowerBound.empty()
              ? TypeInfo{SimpleTypeKind::Nothing, "Nothing"}
              : typeFromDeclaredName(normalizedLower, &scope, &span);
      const TypeInfo upper =
          wildcard.upperBound.empty()
              ? TypeInfo{SimpleTypeKind::Object, "Object"}
              : typeFromDeclaredName(normalizedUpper, &scope, &span);
      if (!isAssignable(upper, lower)) {
        diagnostics_.error(
            span, "wildcard lower bound " + lower.name +
                      " does not conform to upper bound " + upper.name +
                      " in given import filter");
        return false;
      }
      return true;
    }

    const CompositeTypeSyntax infix =
        parseCompositeTypeSyntax(pattern);
    if (infix.malformed) {
      diagnostics_.error(span, "malformed intersection or union type in given "
                               "import filter: " +
                                   patternName);
      return false;
    }
    if (infix.kind != CompositeTypeSyntaxKind::None) {
      return std::all_of(infix.operands.begin(), infix.operands.end(),
                         validate);
    }

    const AppliedTypeSyntax applied = parseAppliedTypeSyntax(pattern);
    if (applied.applied && applied.malformed) {
      diagnostics_.error(span, "malformed applied type in given import filter: " +
                                   patternName);
      return false;
    }
    if (applied.applied && !applied.malformed) {
      const SymbolInfo* constructor =
          typeSymbolForDeclaredName(applied.constructor, &scope);
      if (constructor == nullptr ||
          (constructor->kind != AstDeclarationKind::Class &&
           constructor->kind != AstDeclarationKind::Trait &&
           !(constructor->kind == AstDeclarationKind::Type &&
             constructor->hasImplementation))) {
        diagnostics_.error(span, "unresolved generic type constructor: " +
                                     applied.constructor);
        return false;
      }
      if (constructor->typeParameters.size() != applied.arguments.size()) {
        diagnostics_.error(
            span, "type application to " + constructor->name + " has " +
                      std::to_string(applied.arguments.size()) +
                      " arguments but expected " +
                      std::to_string(constructor->typeParameters.size()));
        return false;
      }
      return std::all_of(applied.arguments.begin(), applied.arguments.end(),
                         validate);
    }

    if (pattern.find('?') != std::string::npos) {
      diagnostics_.error(span, "malformed wildcard in given import filter: " +
                                   patternName);
      return false;
    }
    return typeFromDeclaredName(pattern, &scope, &span).kind !=
           SimpleTypeKind::Unknown;
  };
  return validate(desugaredFilter);
}

void Typechecker::applyImport(const AstDeclaration& declaration, Scope& scope) {
  if (declaration.kind != AstDeclarationKind::Import) {
    return;
  }
  if (declaration.importPath.empty()) {
    return;
  }

  const bool wildcardImport =
      declaration.name == "_" || declaration.importsWildcard;
  const bool bulkImport = wildcardImport || declaration.importsGivens ||
                          !declaration.importGivenTypes.empty();
  if (!declaration.importSelectors.empty() || bulkImport) {
    const std::string importOwner = importSymbolName(declaration, scope);
    if (!globalSymbols_.contains(importOwner)) {
      diagnostics_.error(declaration.span,
                         "unresolved import owner: " + declaration.importPath);
      return;
    }
    for (const std::string& filter : declaration.importGivenTypes) {
      (void)validateGivenImportFilter(filter, scope, declaration.span);
    }
    for (const AstImportSelector& selector : declaration.importSelectors) {
      const std::string importedName = importOwner + "." + selector.name;
      auto imported = globalSymbols_.find(importedName);
      if (imported == globalSymbols_.end()) {
        diagnostics_.error(selector.span,
                           "unresolved import selector: " + selector.name);
        continue;
      }
      SymbolInfo alias = imported->second;
      alias.name = selector.alias;
      scope[alias.name] = std::move(alias);
    }
    if (!bulkImport) {
      return;
    }
    for (const auto& [symbolName, symbol] : globalSymbols_) {
      if (!isDirectMemberOf(symbolName, importOwner)) {
        continue;
      }
      const bool wildcardMatch = wildcardImport && !symbol.isGiven;
      const bool givenMatch = givenImportMatches(declaration, symbol, scope);
      if (!wildcardMatch && !givenMatch) {
        continue;
      }
      SymbolInfo alias = symbol;
      alias.name = memberNameOf(symbolName);
      scope[alias.name] = std::move(alias);
    }
    return;
  }

  if (declaration.name.empty()) {
    return;
  }

  const std::string importedName = importSymbolName(declaration, scope);
  auto imported = globalSymbols_.find(importedName);
  if (imported == globalSymbols_.end()) {
    diagnostics_.error(declaration.span,
                       "unresolved import: " + declaration.importPath);
    return;
  }

  SymbolInfo alias = imported->second;
  alias.name = declaration.name;
  scope[declaration.name] = std::move(alias);
}

void Typechecker::mergeScope(Scope& destination, const Scope& source) const {
  for (const auto& [name, symbol] : source) {
    destination[name] = symbol;
  }
}

TypeInfo Typechecker::inferExpressionType(const AstExpression& expression, Scope& scope,
                                          const TypeInfo* expectedType) {
  TypeInfo type = inferExpressionTypeImpl(expression, scope, expectedType);
  if (expression.span.isValid()) {
    auto sameSpan = [&](const TypedExpressionInfo& info) {
      return info.span.source == expression.span.source &&
             info.span.start == expression.span.start &&
             info.span.length == expression.span.length;
    };
    auto existing =
        std::find_if(expressionTypes_.rbegin(), expressionTypes_.rend(), sameSpan);
    if (existing == expressionTypes_.rend()) {
      expressionTypes_.push_back(TypedExpressionInfo{expression.span, type});
    } else {
      existing->type = type;
    }
  }
  return type;
}

bool Typechecker::isSupportedInlineValueInitializer(
    const AstExpression& expression, const Scope& scope) const {
  const auto isValidatedInlineValue = [&](const SymbolInfo* symbol) {
    return symbol != nullptr && symbol->kind == AstDeclarationKind::Val &&
           symbol->isInline &&
           validatedInlineValueSymbols_.contains(symbol->symbolName);
  };

  switch (expression.kind) {
  case AstExpressionKind::IntegerLiteral:
  case AstExpressionKind::FloatingLiteral:
  case AstExpressionKind::StringLiteral:
  case AstExpressionKind::CharLiteral:
  case AstExpressionKind::BooleanLiteral:
  case AstExpressionKind::NullLiteral:
    return true;
  case AstExpressionKind::Identifier: {
    const auto symbol = scope.find(expression.text);
    return symbol != scope.end() && isValidatedInlineValue(&symbol->second);
  }
  case AstExpressionKind::Select:
    if (expression.children.size() == 1 &&
        expression.children.front().kind == AstExpressionKind::Identifier) {
      const auto receiver = scope.find(expression.children.front().text);
      if (receiver != scope.end()) {
        const std::optional<SymbolInfo> member = resolvedMemberForReceiverType(
            receiver->second.type, expression.text);
        return member.has_value() && isValidatedInlineValue(&*member);
      }
    }
    return false;
  case AstExpressionKind::Unary:
    return expression.children.size() == 1 &&
           isSupportedInlineValueInitializer(expression.children.front(), scope);
  case AstExpressionKind::Binary:
    return expression.children.size() == 2 &&
           isSupportedInlineValueInitializer(expression.children.front(), scope) &&
           isSupportedInlineValueInitializer(expression.children.back(), scope);
  default:
    return false;
  }
}

bool Typechecker::isErasedValueCallee(const AstExpression& expression,
                                      const Scope& scope) const {
  if (expression.kind == AstExpressionKind::Identifier) {
    const auto symbol = scope.find(expression.text);
    return symbol != scope.end() &&
           symbol->second.symbolName ==
               support::StdNames::ScalaCompiletimeErasedValue;
  }

  std::function<std::optional<std::string>(const AstExpression&)> qualifiedPath;
  qualifiedPath = [&](const AstExpression& candidate)
      -> std::optional<std::string> {
    if (candidate.kind == AstExpressionKind::Identifier) {
      return candidate.text;
    }
    if (candidate.kind != AstExpressionKind::Select ||
        candidate.children.size() != 1) {
      return std::nullopt;
    }
    std::optional<std::string> receiver =
        qualifiedPath(candidate.children.front());
    return receiver.has_value()
               ? std::optional<std::string>(*receiver + "." + candidate.text)
               : std::nullopt;
  };
  const std::optional<std::string> path = qualifiedPath(expression);
  return path.has_value() &&
         *path == support::StdNames::ScalaCompiletimeErasedValue;
}

bool Typechecker::isErasedValueExpression(const AstExpression& expression,
                                          const Scope& scope) const {
  return expression.kind == AstExpressionKind::TypeApply &&
         expression.children.size() == 1 &&
         isErasedValueCallee(expression.children.front(), scope);
}

bool Typechecker::isConstValueCallee(const AstExpression& expression,
                                     const Scope& scope) const {
  if (expression.kind == AstExpressionKind::Identifier) {
    const auto symbol = scope.find(expression.text);
    return symbol != scope.end() &&
           symbol->second.symbolName ==
               support::StdNames::ScalaCompiletimeConstValue;
  }

  std::function<std::optional<std::string>(const AstExpression&)> qualifiedPath;
  qualifiedPath = [&](const AstExpression& candidate)
      -> std::optional<std::string> {
    if (candidate.kind == AstExpressionKind::Identifier) {
      return candidate.text;
    }
    if (candidate.kind != AstExpressionKind::Select ||
        candidate.children.size() != 1) {
      return std::nullopt;
    }
    std::optional<std::string> receiver =
        qualifiedPath(candidate.children.front());
    return receiver.has_value()
               ? std::optional<std::string>(*receiver + "." + candidate.text)
               : std::nullopt;
  };
  const std::optional<std::string> path = qualifiedPath(expression);
  return path.has_value() &&
         *path == support::StdNames::ScalaCompiletimeConstValue;
}

bool Typechecker::isConstValueExpression(const AstExpression& expression,
                                         const Scope& scope) const {
  return expression.kind == AstExpressionKind::TypeApply &&
         expression.children.size() == 1 &&
         isConstValueCallee(expression.children.front(), scope);
}

bool Typechecker::isConstValueOptCallee(const AstExpression& expression,
                                        const Scope& scope) const {
  if (expression.kind == AstExpressionKind::Identifier) {
    const auto symbol = scope.find(expression.text);
    return symbol != scope.end() &&
           symbol->second.symbolName ==
               support::StdNames::ScalaCompiletimeConstValueOpt;
  }

  std::function<std::optional<std::string>(const AstExpression&)> qualifiedPath;
  qualifiedPath = [&](const AstExpression& candidate)
      -> std::optional<std::string> {
    if (candidate.kind == AstExpressionKind::Identifier) {
      return candidate.text;
    }
    if (candidate.kind != AstExpressionKind::Select ||
        candidate.children.size() != 1) {
      return std::nullopt;
    }
    std::optional<std::string> receiver =
        qualifiedPath(candidate.children.front());
    return receiver.has_value()
               ? std::optional<std::string>(*receiver + "." + candidate.text)
               : std::nullopt;
  };
  const std::optional<std::string> path = qualifiedPath(expression);
  return path.has_value() &&
         *path == support::StdNames::ScalaCompiletimeConstValueOpt;
}

bool Typechecker::isConstValueTupleCallee(const AstExpression& expression,
                                          const Scope& scope) const {
  if (expression.kind == AstExpressionKind::Identifier) {
    const auto symbol = scope.find(expression.text);
    return symbol != scope.end() &&
           symbol->second.symbolName ==
               support::StdNames::ScalaCompiletimeConstValueTuple;
  }

  std::function<std::optional<std::string>(const AstExpression&)> qualifiedPath;
  qualifiedPath = [&](const AstExpression& candidate)
      -> std::optional<std::string> {
    if (candidate.kind == AstExpressionKind::Identifier) {
      return candidate.text;
    }
    if (candidate.kind != AstExpressionKind::Select ||
        candidate.children.size() != 1) {
      return std::nullopt;
    }
    std::optional<std::string> receiver =
        qualifiedPath(candidate.children.front());
    return receiver.has_value()
               ? std::optional<std::string>(*receiver + "." + candidate.text)
               : std::nullopt;
  };
  const std::optional<std::string> path = qualifiedPath(expression);
  return path.has_value() &&
         *path == support::StdNames::ScalaCompiletimeConstValueTuple;
}

bool Typechecker::isCompiletimeErrorCallee(const AstExpression& expression,
                                           const Scope& scope) const {
  if (expression.kind == AstExpressionKind::Identifier) {
    const auto symbol = scope.find(expression.text);
    return symbol != scope.end() &&
           symbol->second.symbolName ==
               support::StdNames::ScalaCompiletimeError;
  }

  std::function<std::optional<std::string>(const AstExpression&)> qualifiedPath;
  qualifiedPath = [&](const AstExpression& candidate)
      -> std::optional<std::string> {
    if (candidate.kind == AstExpressionKind::Identifier) {
      return candidate.text;
    }
    if (candidate.kind != AstExpressionKind::Select ||
        candidate.children.size() != 1) {
      return std::nullopt;
    }
    std::optional<std::string> receiver =
        qualifiedPath(candidate.children.front());
    return receiver.has_value()
               ? std::optional<std::string>(*receiver + "." + candidate.text)
               : std::nullopt;
  };
  const std::optional<std::string> path = qualifiedPath(expression);
  return path.has_value() &&
         *path == support::StdNames::ScalaCompiletimeError;
}

bool Typechecker::isCodeOfCallee(const AstExpression& expression,
                                 const Scope& scope) const {
  if (expression.kind == AstExpressionKind::Identifier) {
    const auto symbol = scope.find(expression.text);
    return symbol != scope.end() &&
           symbol->second.symbolName == support::StdNames::ScalaCompiletimeCodeOf;
  }

  std::function<std::optional<std::string>(const AstExpression&)> qualifiedPath;
  qualifiedPath = [&](const AstExpression& candidate)
      -> std::optional<std::string> {
    if (candidate.kind == AstExpressionKind::Identifier) {
      return candidate.text;
    }
    if (candidate.kind != AstExpressionKind::Select ||
        candidate.children.size() != 1) {
      return std::nullopt;
    }
    std::optional<std::string> receiver =
        qualifiedPath(candidate.children.front());
    return receiver.has_value()
               ? std::optional<std::string>(*receiver + "." + candidate.text)
               : std::nullopt;
  };
  const std::optional<std::string> path = qualifiedPath(expression);
  return path.has_value() && *path == support::StdNames::ScalaCompiletimeCodeOf;
}

bool Typechecker::isUninitializedExpression(const AstExpression& expression,
                                            const Scope& scope) const {
  if (expression.kind == AstExpressionKind::Identifier) {
    const auto symbol = scope.find(expression.text);
    return symbol != scope.end() &&
           symbol->second.symbolName ==
               support::StdNames::ScalaCompiletimeUninitialized;
  }

  std::function<std::optional<std::string>(const AstExpression&)> qualifiedPath;
  qualifiedPath = [&](const AstExpression& candidate)
      -> std::optional<std::string> {
    if (candidate.kind == AstExpressionKind::Identifier) {
      return candidate.text;
    }
    if (candidate.kind != AstExpressionKind::Select ||
        candidate.children.size() != 1) {
      return std::nullopt;
    }
    std::optional<std::string> receiver =
        qualifiedPath(candidate.children.front());
    return receiver.has_value()
               ? std::optional<std::string>(*receiver + "." + candidate.text)
               : std::nullopt;
  };
  const std::optional<std::string> path = qualifiedPath(expression);
  return path.has_value() &&
         *path == support::StdNames::ScalaCompiletimeUninitialized;
}

bool Typechecker::isRequireConstCallee(const AstExpression& expression,
                                       const Scope& scope) const {
  if (expression.kind == AstExpressionKind::Identifier) {
    const auto symbol = scope.find(expression.text);
    return symbol != scope.end() &&
           symbol->second.symbolName ==
               support::StdNames::ScalaCompiletimeRequireConst;
  }

  std::function<std::optional<std::string>(const AstExpression&)> qualifiedPath;
  qualifiedPath = [&](const AstExpression& candidate)
      -> std::optional<std::string> {
    if (candidate.kind == AstExpressionKind::Identifier) {
      return candidate.text;
    }
    if (candidate.kind != AstExpressionKind::Select ||
        candidate.children.size() != 1) {
      return std::nullopt;
    }
    std::optional<std::string> receiver =
        qualifiedPath(candidate.children.front());
    return receiver.has_value()
               ? std::optional<std::string>(*receiver + "." + candidate.text)
               : std::nullopt;
  };
  const std::optional<std::string> path = qualifiedPath(expression);
  return path.has_value() &&
         *path == support::StdNames::ScalaCompiletimeRequireConst;
}

bool Typechecker::isSummonInlineCallee(const AstExpression& expression,
                                       const Scope& scope) const {
  if (expression.kind == AstExpressionKind::Identifier) {
    const auto symbol = scope.find(expression.text);
    return symbol != scope.end() &&
           symbol->second.symbolName ==
               support::StdNames::ScalaCompiletimeSummonInline;
  }

  std::function<std::optional<std::string>(const AstExpression&)> qualifiedPath;
  qualifiedPath = [&](const AstExpression& candidate)
      -> std::optional<std::string> {
    if (candidate.kind == AstExpressionKind::Identifier) {
      return candidate.text;
    }
    if (candidate.kind != AstExpressionKind::Select ||
        candidate.children.size() != 1) {
      return std::nullopt;
    }
    std::optional<std::string> receiver =
        qualifiedPath(candidate.children.front());
    return receiver.has_value()
               ? std::optional<std::string>(*receiver + "." + candidate.text)
               : std::nullopt;
  };
  const std::optional<std::string> path = qualifiedPath(expression);
  return path.has_value() &&
         *path == support::StdNames::ScalaCompiletimeSummonInline;
}

bool Typechecker::isSummonAllCallee(const AstExpression& expression,
                                    const Scope& scope) const {
  if (expression.kind == AstExpressionKind::Identifier) {
    const auto symbol = scope.find(expression.text);
    return symbol != scope.end() &&
           symbol->second.symbolName ==
               support::StdNames::ScalaCompiletimeSummonAll;
  }

  std::function<std::optional<std::string>(const AstExpression&)> qualifiedPath;
  qualifiedPath = [&](const AstExpression& candidate)
      -> std::optional<std::string> {
    if (candidate.kind == AstExpressionKind::Identifier) {
      return candidate.text;
    }
    if (candidate.kind != AstExpressionKind::Select ||
        candidate.children.size() != 1) {
      return std::nullopt;
    }
    std::optional<std::string> receiver =
        qualifiedPath(candidate.children.front());
    return receiver.has_value()
               ? std::optional<std::string>(*receiver + "." + candidate.text)
               : std::nullopt;
  };
  const std::optional<std::string> path = qualifiedPath(expression);
  return path.has_value() &&
         *path == support::StdNames::ScalaCompiletimeSummonAll;
}

std::optional<bool>
Typechecker::constantBooleanValue(const AstExpression& expression,
                                  const Scope& scope) const {
  switch (expression.kind) {
  case AstExpressionKind::BooleanLiteral:
    if (expression.text == "true") {
      return true;
    }
    if (expression.text == "false") {
      return false;
    }
    return std::nullopt;
  case AstExpressionKind::Block: {
    if (expression.children.size() != 2) {
      return std::nullopt;
    }
    const AstExpression& bindingDeclaration = expression.children.front();
    if (bindingDeclaration.kind != AstExpressionKind::LocalDeclaration ||
        bindingDeclaration.mutableLocal || bindingDeclaration.text.empty() ||
        bindingDeclaration.children.size() != 1) {
      return std::nullopt;
    }

    const AstExpression* selectorReference =
        &bindingDeclaration.children.front();
    if (selectorReference->kind == AstExpressionKind::TypeApply &&
        selectorReference->children.size() == 1 &&
        selectorReference->children.front().kind == AstExpressionKind::Select &&
        selectorReference->children.front().text ==
            support::StdNames::AsInstanceOf &&
        selectorReference->children.front().children.size() == 1) {
      selectorReference =
          &selectorReference->children.front().children.front();
    }
    if (selectorReference->kind != AstExpressionKind::Identifier ||
        !selectorReference->text.starts_with("$match")) {
      return std::nullopt;
    }
    const auto selector = scope.find(selectorReference->text);
    if (selector == scope.end()) {
      return std::nullopt;
    }

    SymbolInfo binding;
    binding.kind = AstDeclarationKind::Val;
    binding.name = bindingDeclaration.text;
    binding.symbolName = bindingDeclaration.text;
    binding.type = selector->second.type;
    binding.isLexicalValue = true;
    binding.specializedBooleanValue =
        selector->second.specializedBooleanValue;
    binding.specializedIntegerValue =
        selector->second.specializedIntegerValue;
    binding.specializedFloatingValue =
        selector->second.specializedFloatingValue;
    binding.specializedStringValue = selector->second.specializedStringValue;
    binding.specializedCharValue = selector->second.specializedCharValue;
    binding.specializedNullValue = selector->second.specializedNullValue;
    binding.specializedStaticType = selector->second.specializedStaticType;

    Scope bindingScope = scope;
    bindingScope[bindingDeclaration.text] = std::move(binding);
    return constantBooleanValue(expression.children.back(), bindingScope);
  }
  case AstExpressionKind::Identifier:
    if (auto symbol = scope.find(expression.text); symbol != scope.end()) {
      return symbol->second.specializedBooleanValue;
    }
    return std::nullopt;
  case AstExpressionKind::Select:
    if (expression.children.size() == 1 &&
        expression.children.front().kind == AstExpressionKind::Identifier) {
      if (auto receiver = scope.find(expression.children.front().text);
          receiver != scope.end()) {
        if (std::optional<SymbolInfo> member =
                resolvedMemberForReceiverType(receiver->second.type,
                                              expression.text)) {
          return member->specializedBooleanValue;
        }
      }
    }
    return std::nullopt;
  case AstExpressionKind::TypeApply:
    if (isConstValueExpression(expression, scope) &&
        typeArgumentsFor(expression).size() == 1) {
      const TypeInfo constant =
          typeFromDeclaredName(expression.declaredType, &scope, &expression.span);
      if (constant.singletonLiteral == "true") {
        return true;
      }
      if (constant.singletonLiteral == "false") {
        return false;
      }
      return std::nullopt;
    }
    if (expression.children.size() == 1 &&
        expression.children.front().kind == AstExpressionKind::Select &&
        expression.children.front().text == support::StdNames::IsInstanceOf &&
        expression.children.front().children.size() == 1) {
      const std::optional<TypeInfo> actual = specializedStaticType(
          expression.children.front().children.front(), scope);
      if (!actual.has_value()) {
        return std::nullopt;
      }
      const TypeInfo target =
          typeFromDeclaredName(expression.declaredType, &scope, &expression.span);
      return staticTypeTestValue(*actual, target);
    }
    return std::nullopt;
  case AstExpressionKind::Unary:
    if (expression.text == "!" && expression.children.size() == 1) {
      if (std::optional<bool> operand =
              constantBooleanValue(expression.children.front(), scope)) {
        return !*operand;
      }
    }
    return std::nullopt;
  case AstExpressionKind::Binary:
    if (expression.children.size() != 2) {
      return std::nullopt;
    }
    {
      if (expression.text == "&&" || expression.text == "||") {
        const std::optional<bool> left =
            constantBooleanValue(expression.children.front(), scope);
        if (!left.has_value()) {
          return std::nullopt;
        }
        if ((expression.text == "&&" && !*left) ||
            (expression.text == "||" && *left)) {
          return *left;
        }
        return constantBooleanValue(expression.children.back(), scope);
      }
      if (expression.text == "==" || expression.text == "!=") {
        const std::optional<bool> left =
            constantBooleanValue(expression.children.front(), scope);
        const std::optional<bool> right =
            constantBooleanValue(expression.children.back(), scope);
        if (left.has_value() && right.has_value()) {
          return expression.text == "==" ? *left == *right : *left != *right;
        }

        const std::optional<bool> leftNull =
            constantNullValue(expression.children.front(), scope);
        const std::optional<bool> rightNull =
            constantNullValue(expression.children.back(), scope);
        if (leftNull.has_value() && rightNull.has_value() &&
            (*leftNull || *rightNull)) {
          return expression.text == "==" ? *leftNull == *rightNull
                                         : *leftNull != *rightNull;
        }

        const auto singletonComparison =
            [&](const AstExpression& pattern,
                const AstExpression& value) -> std::optional<bool> {
          if (pattern.kind != AstExpressionKind::ModuleReference) {
            return std::nullopt;
          }
          const std::optional<TypeInfo> singleton =
              specializedStaticType(pattern, scope);
          const std::optional<TypeInfo> actual =
              specializedStaticType(value, scope);
          if (!singleton.has_value() || !actual.has_value()) {
            return std::nullopt;
          }
          const std::string singletonName =
              singleton->runtimeName.empty() ? singleton->name
                                             : singleton->runtimeName;
          auto symbol = globalSymbols_.find(singletonName);
          if (symbol == globalSymbols_.end() ||
              symbol->second.kind != AstDeclarationKind::Object) {
            return std::nullopt;
          }
          if (actual->kind == SimpleTypeKind::Null ||
              isBoxablePrimitiveType(actual->kind)) {
            return false;
          }
          return staticTypeTestValue(*actual, *singleton);
        };
        std::optional<bool> singleton = singletonComparison(
            expression.children.front(), expression.children.back());
        if (!singleton.has_value()) {
          singleton = singletonComparison(expression.children.back(),
                                            expression.children.front());
        }
        if (singleton.has_value()) {
          return expression.text == "==" ? *singleton : !*singleton;
        }
      }

      if (expression.text == "==" || expression.text == "!=" ||
          expression.text == "<" || expression.text == ">" ||
          expression.text == "<=" || expression.text == ">=") {
        if (expression.text == "==" || expression.text == "!=") {
          const std::optional<std::string> leftString =
              constantStringValue(expression.children.front(), scope);
          const std::optional<std::string> rightString =
              constantStringValue(expression.children.back(), scope);
          if (leftString.has_value() && rightString.has_value()) {
            return expression.text == "==" ? *leftString == *rightString
                                           : *leftString != *rightString;
          }

          const std::optional<std::uint32_t> leftChar =
              constantCharValue(expression.children.front(), scope);
          const std::optional<std::uint32_t> rightChar =
              constantCharValue(expression.children.back(), scope);
          if (leftChar.has_value() && rightChar.has_value()) {
            return expression.text == "==" ? *leftChar == *rightChar
                                           : *leftChar != *rightChar;
          }
        }

        const std::optional<double> leftFloating =
            constantFloatingValue(expression.children.front(), scope);
        const std::optional<double> rightFloating =
            constantFloatingValue(expression.children.back(), scope);
        if (leftFloating.has_value() && rightFloating.has_value()) {
          if (expression.text == "==") {
            return *leftFloating == *rightFloating;
          }
          if (expression.text == "!=") {
            return *leftFloating != *rightFloating;
          }
          if (expression.text == "<") {
            return *leftFloating < *rightFloating;
          }
          if (expression.text == ">") {
            return *leftFloating > *rightFloating;
          }
          if (expression.text == "<=") {
            return *leftFloating <= *rightFloating;
          }
          return *leftFloating >= *rightFloating;
        }

        const std::optional<std::int64_t> left =
            constantIntegerValue(expression.children.front(), scope);
        const std::optional<std::int64_t> right =
            constantIntegerValue(expression.children.back(), scope);
        if (!left.has_value() || !right.has_value()) {
          return std::nullopt;
        }
        if (expression.text == "==") {
          return *left == *right;
        }
        if (expression.text == "!=") {
          return *left != *right;
        }
        if (expression.text == "<") {
          return *left < *right;
        }
        if (expression.text == ">") {
          return *left > *right;
        }
        if (expression.text == "<=") {
          return *left <= *right;
        }
        return *left >= *right;
      }
    }
    return std::nullopt;
  default:
    return std::nullopt;
  }
}

std::optional<std::string>
Typechecker::constantStringValue(const AstExpression& expression,
                                 const Scope& scope) const {
  const auto symbolValue = [&](const SymbolInfo& symbol)
      -> std::optional<std::string> {
    if (symbol.specializedStringValue.has_value()) {
      return symbol.specializedStringValue;
    }
    if (symbol.kind == AstDeclarationKind::Val && symbol.isInline &&
        validatedInlineValueSymbols_.contains(symbol.symbolName) &&
        symbol.inlineBody.kind != AstExpressionKind::Empty) {
      return constantStringValue(symbol.inlineBody, scope);
    }
    return std::nullopt;
  };
  switch (expression.kind) {
  case AstExpressionKind::StringLiteral:
    return decodeStringLiteral(expression.text);
  case AstExpressionKind::Identifier:
    if (auto symbol = scope.find(expression.text); symbol != scope.end()) {
      return symbolValue(symbol->second);
    }
    return std::nullopt;
  case AstExpressionKind::Select:
    if (expression.children.size() == 1 &&
        expression.children.front().kind == AstExpressionKind::Identifier) {
      if (auto receiver = scope.find(expression.children.front().text);
          receiver != scope.end()) {
        if (std::optional<SymbolInfo> member = resolvedMemberForReceiverType(
                receiver->second.type, expression.text)) {
          return symbolValue(*member);
        }
      }
    }
    return std::nullopt;
  case AstExpressionKind::TypeApply:
    if (isConstValueExpression(expression, scope) &&
        typeArgumentsFor(expression).size() == 1) {
      const TypeInfo constant =
          typeFromDeclaredName(expression.declaredType, &scope, &expression.span);
      if (constant.kind == SimpleTypeKind::String &&
          !constant.singletonLiteral.empty()) {
        return decodeStringLiteral(constant.singletonLiteral);
      }
    }
    return std::nullopt;
  case AstExpressionKind::Call:
    if (expression.children.size() == 2 &&
        isCodeOfCallee(expression.children.front(), scope)) {
      return sourceCodeForExpression(expression.children.back(), scope);
    }
    return std::nullopt;
  case AstExpressionKind::Binary:
    if (expression.text == "+" && expression.children.size() == 2) {
      const std::optional<std::string> left =
          constantStringValue(expression.children.front(), scope);
      const std::optional<std::string> right =
          constantStringValue(expression.children.back(), scope);
      if (left.has_value() && right.has_value()) {
        return *left + *right;
      }
    }
    return std::nullopt;
  default:
    return std::nullopt;
  }
}

std::optional<std::string>
Typechecker::sourceCodeForExpression(const AstExpression& expression,
                                     const Scope& scope) const {
  if (expression.kind == AstExpressionKind::Identifier) {
    if (auto symbol = scope.find(expression.text);
        symbol != scope.end() && symbol->second.specializedCode.has_value()) {
      return symbol->second.specializedCode;
    }
  }

  const auto joinedChildren = [&](std::size_t first,
                                  std::string_view separator)
      -> std::optional<std::string> {
    std::string result;
    for (std::size_t index = first; index < expression.children.size(); ++index) {
      const std::optional<std::string> child =
          sourceCodeForExpression(expression.children[index], scope);
      if (!child.has_value()) {
        return std::nullopt;
      }
      if (index != first) {
        result += separator;
      }
      result += *child;
    }
    return result;
  };

  switch (expression.kind) {
  case AstExpressionKind::Identifier:
  case AstExpressionKind::ModuleReference:
  case AstExpressionKind::IntegerLiteral:
  case AstExpressionKind::FloatingLiteral:
  case AstExpressionKind::StringLiteral:
  case AstExpressionKind::CharLiteral:
  case AstExpressionKind::SymbolLiteral:
  case AstExpressionKind::BooleanLiteral:
  case AstExpressionKind::NullLiteral:
    return expression.text;
  case AstExpressionKind::This:
    return expression.text.empty() ? std::optional<std::string>{"this"}
                                   : expression.text;
  case AstExpressionKind::Super:
    return expression.text.empty() ? std::optional<std::string>{"super"}
                                   : expression.text;
  case AstExpressionKind::TupleLiteral:
    if (const std::optional<std::string> elements = joinedChildren(0, ", ")) {
      return "(" + *elements + ")";
    }
    break;
  case AstExpressionKind::PolymorphicFunction:
    break;
  case AstExpressionKind::Call:
    if (!expression.children.empty()) {
      const std::optional<std::string> callee =
          sourceCodeForExpression(expression.children.front(), scope);
      const std::optional<std::string> arguments = joinedChildren(1, ", ");
      if (callee.has_value() && arguments.has_value()) {
        return *callee + "(" + *arguments + ")";
      }
    }
    break;
  case AstExpressionKind::Select:
    if (expression.children.size() == 1) {
      if (const std::optional<std::string> receiver =
              sourceCodeForExpression(expression.children.front(), scope)) {
        return *receiver + "." + expression.text;
      }
    }
    break;
  case AstExpressionKind::TypeApply:
    if (expression.children.size() == 1) {
      if (const std::optional<std::string> callee =
              sourceCodeForExpression(expression.children.front(), scope)) {
        return *callee + "[" + expression.declaredType + "]";
      }
    }
    break;
  case AstExpressionKind::Unary:
    if (expression.children.size() == 1) {
      if (const std::optional<std::string> operand =
              sourceCodeForExpression(expression.children.front(), scope)) {
        return expression.text + *operand;
      }
    }
    break;
  case AstExpressionKind::Binary:
    if (expression.children.size() == 2) {
      const std::optional<std::string> left =
          sourceCodeForExpression(expression.children.front(), scope);
      const std::optional<std::string> right =
          sourceCodeForExpression(expression.children.back(), scope);
      if (left.has_value() && right.has_value()) {
        return *left + " " + expression.text + " " + *right;
      }
    }
    break;
  case AstExpressionKind::Assign:
    if (expression.children.size() == 2) {
      const std::optional<std::string> target =
          sourceCodeForExpression(expression.children.front(), scope);
      const std::optional<std::string> value =
          sourceCodeForExpression(expression.children.back(), scope);
      if (target.has_value() && value.has_value()) {
        return *target + " = " + *value;
      }
    }
    break;
  case AstExpressionKind::New: {
    std::string result = "new " + expression.text;
    if (!expression.children.empty()) {
      if (const std::optional<std::string> arguments = joinedChildren(0, ", ")) {
        result += "(" + *arguments + ")";
      } else {
        break;
      }
    }
    return result;
  }
  default:
    break;
  }

  if (sources_ == nullptr || !expression.span.isValid()) {
    return std::nullopt;
  }
  std::string code = sources_->snippet(expression.span);
  return code.empty() ? std::nullopt
                      : std::optional<std::string>{std::move(code)};
}

std::optional<std::uint32_t>
Typechecker::constantCharValue(const AstExpression& expression,
                               const Scope& scope) const {
  switch (expression.kind) {
  case AstExpressionKind::CharLiteral:
    return decodeCharLiteral(expression.text);
  case AstExpressionKind::Identifier:
    if (auto symbol = scope.find(expression.text); symbol != scope.end()) {
      return symbol->second.specializedCharValue;
    }
    return std::nullopt;
  case AstExpressionKind::Select:
    if (expression.children.size() == 1 &&
        expression.children.front().kind == AstExpressionKind::Identifier) {
      if (auto receiver = scope.find(expression.children.front().text);
          receiver != scope.end()) {
        if (std::optional<SymbolInfo> member = resolvedMemberForReceiverType(
                receiver->second.type, expression.text)) {
          return member->specializedCharValue;
        }
      }
    }
    return std::nullopt;
  case AstExpressionKind::TypeApply:
    if (isConstValueExpression(expression, scope) &&
        typeArgumentsFor(expression).size() == 1) {
      const TypeInfo constant =
          typeFromDeclaredName(expression.declaredType, &scope, &expression.span);
      if (constant.kind == SimpleTypeKind::Char &&
          !constant.singletonLiteral.empty()) {
        return decodeCharLiteral(constant.singletonLiteral);
      }
    }
    return std::nullopt;
  default:
    return std::nullopt;
  }
}

std::optional<bool>
Typechecker::constantNullValue(const AstExpression& expression,
                               const Scope& scope) const {
  const auto symbolValue = [&](const SymbolInfo& symbol)
      -> std::optional<bool> {
    if (symbol.specializedNullValue.has_value()) {
      return symbol.specializedNullValue;
    }
    if (symbol.kind == AstDeclarationKind::Object) {
      return false;
    }
    if (symbol.type.kind == SimpleTypeKind::Null) {
      return true;
    }
    if (hasCompileTimeSize(symbol.type.kind)) {
      return false;
    }
    if (symbol.kind == AstDeclarationKind::Val && symbol.isInline &&
        validatedInlineValueSymbols_.contains(symbol.symbolName) &&
        symbol.inlineBody.kind != AstExpressionKind::Empty) {
      return constantNullValue(symbol.inlineBody, scope);
    }
    return std::nullopt;
  };

  switch (expression.kind) {
  case AstExpressionKind::NullLiteral:
    return true;
  case AstExpressionKind::IntegerLiteral:
  case AstExpressionKind::FloatingLiteral:
  case AstExpressionKind::StringLiteral:
  case AstExpressionKind::CharLiteral:
  case AstExpressionKind::SymbolLiteral:
  case AstExpressionKind::BooleanLiteral:
  case AstExpressionKind::This:
  case AstExpressionKind::Super:
  case AstExpressionKind::ModuleReference:
  case AstExpressionKind::New:
    return false;
  case AstExpressionKind::Identifier:
    if (auto symbol = scope.find(expression.text); symbol != scope.end()) {
      return symbolValue(symbol->second);
    }
    return std::nullopt;
  case AstExpressionKind::Select:
    if (expression.children.size() == 1 &&
        expression.children.front().kind == AstExpressionKind::Identifier) {
      if (auto receiver = scope.find(expression.children.front().text);
          receiver != scope.end()) {
        if (std::optional<SymbolInfo> member = resolvedMemberForReceiverType(
                receiver->second.type, expression.text)) {
          return symbolValue(*member);
        }
      }
    }
    return std::nullopt;
  case AstExpressionKind::TypeApply:
    if (expression.children.size() == 1 &&
        expression.children.front().kind == AstExpressionKind::Select &&
        expression.children.front().text == support::StdNames::AsInstanceOf &&
        expression.children.front().children.size() == 1) {
      const TypeInfo target =
          typeFromDeclaredName(expression.declaredType, &scope, &expression.span);
      if (hasCompileTimeSize(target.kind)) {
        return false;
      }
      if (!isReferenceType(target) && target.kind != SimpleTypeKind::Null) {
        return std::nullopt;
      }
      return constantNullValue(
          expression.children.front().children.front(), scope);
    }
    return std::nullopt;
  default:
    if (const std::optional<TypeInfo> type =
            specializedStaticType(expression, scope)) {
      if (type->kind == SimpleTypeKind::Null) {
        return true;
      }
      if (hasCompileTimeSize(type->kind)) {
        return false;
      }
    }
    return std::nullopt;
  }
}

std::optional<double>
Typechecker::constantFloatingValue(const AstExpression& expression,
                                   const Scope& scope) const {
  switch (expression.kind) {
  case AstExpressionKind::FloatingLiteral:
    return parseFloatingConstant(expression.text,
                                 floatingConstantType(expression.text));
  case AstExpressionKind::Identifier:
    if (auto symbol = scope.find(expression.text); symbol != scope.end()) {
      return symbol->second.specializedFloatingValue;
    }
    return std::nullopt;
  case AstExpressionKind::Select:
    if (expression.children.size() == 1 &&
        expression.children.front().kind == AstExpressionKind::Identifier) {
      if (auto receiver = scope.find(expression.children.front().text);
          receiver != scope.end()) {
        if (std::optional<SymbolInfo> member = resolvedMemberForReceiverType(
                receiver->second.type, expression.text)) {
          return member->specializedFloatingValue;
        }
      }
    }
    return std::nullopt;
  case AstExpressionKind::TypeApply:
    if (isConstValueExpression(expression, scope) &&
        typeArgumentsFor(expression).size() == 1) {
      const TypeInfo constant =
          typeFromDeclaredName(expression.declaredType, &scope, &expression.span);
      if (isFloatingConstantType(constant.kind) &&
          !constant.singletonLiteral.empty()) {
        return parseFloatingConstant(constant.singletonLiteral, constant.kind);
      }
    }
    return std::nullopt;
  case AstExpressionKind::Unary:
    if (expression.children.size() == 1 &&
        (expression.text == "+" || expression.text == "-")) {
      if (const std::optional<double> operand =
              constantFloatingValue(expression.children.front(), scope)) {
        return expression.text == "-" ? -*operand : *operand;
      }
    }
    return std::nullopt;
  default:
    return std::nullopt;
  }
}

std::optional<TypeInfo>
Typechecker::specializedStaticType(const AstExpression& expression,
                                   const Scope& scope) const {
  if (isErasedValueExpression(expression, scope) &&
      typeArgumentsFor(expression).size() == 1) {
    return staticExpressionType(
        typeFromDeclaredName(expression.declaredType, &scope, &expression.span));
  }
  if (expression.kind == AstExpressionKind::Identifier) {
    if (auto symbol = scope.find(expression.text);
        symbol != scope.end() && symbol->second.specializedStaticType.has_value()) {
      return symbol->second.specializedStaticType;
    }
  }
  if (!expression.span.isValid()) {
    return std::nullopt;
  }
  for (auto info = expressionTypes_.rbegin(); info != expressionTypes_.rend(); ++info) {
    if (info->span.source == expression.span.source &&
        info->span.start == expression.span.start &&
        info->span.length == expression.span.length) {
      return staticExpressionType(info->type);
    }
  }
  return std::nullopt;
}

std::optional<bool>
Typechecker::staticTypeTestValue(const TypeInfo& actual,
                                 const TypeInfo& target) const {
  if (actual.kind == SimpleTypeKind::Unknown ||
      target.kind == SimpleTypeKind::Unknown || actual.typeParameter ||
      target.typeParameter || actual.abstractTypeMember ||
      target.abstractTypeMember ||
      actual.compositeKind != CompositeTypeKind::None ||
      target.compositeKind != CompositeTypeKind::None) {
    return std::nullopt;
  }
  if (actual.kind == SimpleTypeKind::Null) {
    return false;
  }
  const bool actualScalar = isBoxablePrimitiveType(actual.kind) &&
                            actual.kind != SimpleTypeKind::String;
  const bool targetScalar = isBoxablePrimitiveType(target.kind) &&
                            target.kind != SimpleTypeKind::String;
  if ((actualScalar || actual.kind == SimpleTypeKind::String) &&
      (targetScalar || target.kind == SimpleTypeKind::String)) {
    return actual.kind == target.kind;
  }
  if (isAssignable(target, actual)) {
    return true;
  }

  const auto runtimeName = [](const TypeInfo& type) {
    return type.typeConstructorName.empty()
               ? (type.runtimeName.empty() ? type.name : type.runtimeName)
               : type.typeConstructorName;
  };
  const std::string actualName = runtimeName(actual);
  const std::string targetName = runtimeName(target);
  if (actual.kind == SimpleTypeKind::Object && actualName == "Object") {
    return std::nullopt;
  }
  if (actual.kind == SimpleTypeKind::String &&
      target.kind == SimpleTypeKind::Object) {
    return std::nullopt;
  }
  if (actual.kind == SimpleTypeKind::Object &&
      (targetScalar || target.kind == SimpleTypeKind::String)) {
    return false;
  }

  const auto actualSymbol = globalSymbols_.find(actualName);
  const auto targetSymbol = globalSymbols_.find(targetName);
  if (actualSymbol == globalSymbols_.end() ||
      targetSymbol == globalSymbols_.end()) {
    return std::nullopt;
  }
  if (actualSymbol->second.kind == AstDeclarationKind::Object) {
    return false;
  }
  const bool actualClass = actualSymbol->second.kind == AstDeclarationKind::Class;
  const bool targetClass = targetSymbol->second.kind == AstDeclarationKind::Class;
  const bool targetObject = targetSymbol->second.kind == AstDeclarationKind::Object;
  if (actualClass && (targetClass || targetObject) &&
      !isSubtypeOf(targetName, actualName)) {
    return false;
  }
  return std::nullopt;
}

std::optional<std::int64_t>
Typechecker::constantIntegerValue(const AstExpression& expression,
                                  const Scope& scope) const {
  const auto fitsType = [&](std::int64_t value) {
    const SimpleTypeKind kind = constantIntegerType(expression, scope);
    return kind == SimpleTypeKind::Long ||
           (kind == SimpleTypeKind::Int &&
            value >= std::numeric_limits<std::int32_t>::min() &&
            value <= std::numeric_limits<std::int32_t>::max()) ||
           (kind == SimpleTypeKind::Short &&
            value >= std::numeric_limits<std::int16_t>::min() &&
            value <= std::numeric_limits<std::int16_t>::max()) ||
           (kind == SimpleTypeKind::Byte &&
            value >= std::numeric_limits<std::int8_t>::min() &&
            value <= std::numeric_limits<std::int8_t>::max());
  };

  switch (expression.kind) {
  case AstExpressionKind::IntegerLiteral: {
    const std::optional<std::int64_t> value =
        parseIntegerConstant(expression.text);
    return value.has_value() && fitsType(*value) ? value : std::nullopt;
  }
  case AstExpressionKind::Identifier:
    if (auto symbol = scope.find(expression.text); symbol != scope.end()) {
      if (symbol->second.specializedIntegerValue.has_value()) {
        return symbol->second.specializedIntegerValue;
      }
      const std::optional<std::int64_t> singleton =
          parseIntegerConstant(symbol->second.type.singletonLiteral);
      return singleton.has_value() && fitsType(*singleton) ? singleton
                                                           : std::nullopt;
    }
    return std::nullopt;
  case AstExpressionKind::Select:
    if (expression.children.size() == 1 &&
        (expression.text == support::StdNames::ToByte ||
         expression.text == support::StdNames::ToShort ||
         expression.text == support::StdNames::ToInt)) {
      const std::optional<std::int64_t> operand =
          constantIntegerValue(expression.children.front(), scope);
      if (!operand.has_value()) {
        return std::nullopt;
      }
      if (expression.text == support::StdNames::ToInt) {
        return static_cast<std::int32_t>(*operand);
      }
      const std::int64_t modulus =
          expression.text == support::StdNames::ToByte ? 256 : 65536;
      const std::int64_t signBit = modulus / 2;
      std::int64_t narrowed = *operand % modulus;
      if (narrowed < 0) {
        narrowed += modulus;
      }
      return narrowed >= signBit ? narrowed - modulus : narrowed;
    }
    if (expression.children.size() == 1 &&
        expression.children.front().kind == AstExpressionKind::Identifier) {
      if (auto receiver = scope.find(expression.children.front().text);
          receiver != scope.end()) {
        if (std::optional<SymbolInfo> member = resolvedMemberForReceiverType(
                receiver->second.type, expression.text)) {
          return member->specializedIntegerValue;
        }
      }
    }
    return std::nullopt;
  case AstExpressionKind::TypeApply:
    if (isConstValueExpression(expression, scope) &&
        typeArgumentsFor(expression).size() == 1) {
      const TypeInfo constant =
          typeFromDeclaredName(expression.declaredType, &scope, &expression.span);
      const std::optional<std::int64_t> value =
          parseIntegerConstant(constant.singletonLiteral);
      return value.has_value() && fitsType(*value) ? value : std::nullopt;
    }
    return std::nullopt;
  case AstExpressionKind::Unary:
    if (expression.children.size() == 1 &&
        (expression.text == "+" || expression.text == "-")) {
      const std::optional<std::int64_t> operand =
          constantIntegerValue(expression.children.front(), scope);
      if (!operand.has_value()) {
        return std::nullopt;
      }
      if (expression.text == "+") {
        return operand;
      }
      if (*operand != std::numeric_limits<std::int64_t>::min()) {
        const std::int64_t result = -*operand;
        return fitsType(result) ? std::optional<std::int64_t>{result}
                                : std::nullopt;
      }
    }
    return std::nullopt;
  case AstExpressionKind::Binary:
    if (expression.children.size() == 2) {
      const std::optional<std::int64_t> left =
          constantIntegerValue(expression.children.front(), scope);
      const std::optional<std::int64_t> right =
          constantIntegerValue(expression.children.back(), scope);
      if (!left.has_value() || !right.has_value()) {
        return std::nullopt;
      }
      std::optional<std::int64_t> result;
      if (expression.text == "+") {
        result = checkedIntegerAdd(*left, *right);
      }
      if (expression.text == "-") {
        result = checkedIntegerSubtract(*left, *right);
      }
      if (expression.text == "*") {
        result = checkedIntegerMultiply(*left, *right);
      }
      if (expression.text == "/" && *right != 0 &&
          !(*left == std::numeric_limits<std::int64_t>::min() && *right == -1)) {
        result = *left / *right;
      }
      if (expression.text == "%" && *right != 0) {
        if (*left == std::numeric_limits<std::int64_t>::min() && *right == -1) {
          return std::int64_t{0};
        }
        result = *left % *right;
      }
      if (result.has_value()) {
        return fitsType(*result) ? result : std::nullopt;
      }
    }
    return std::nullopt;
  default:
    return std::nullopt;
  }
}

SimpleTypeKind
Typechecker::constantIntegerType(const AstExpression& expression,
                                 const Scope& scope) const {
  switch (expression.kind) {
  case AstExpressionKind::IntegerLiteral:
    return !expression.text.empty() &&
                   (expression.text.back() == 'l' || expression.text.back() == 'L')
               ? SimpleTypeKind::Long
               : SimpleTypeKind::Int;
  case AstExpressionKind::Identifier:
    if (auto symbol = scope.find(expression.text);
        symbol != scope.end() && isIntegerConstantType(symbol->second.type.kind)) {
      return symbol->second.type.kind;
    }
    return SimpleTypeKind::Unknown;
  case AstExpressionKind::Select:
    if (expression.children.size() == 1) {
      if (expression.text == support::StdNames::ToByte) {
        return SimpleTypeKind::Byte;
      }
      if (expression.text == support::StdNames::ToShort) {
        return SimpleTypeKind::Short;
      }
      if (expression.text == support::StdNames::ToInt) {
        return SimpleTypeKind::Int;
      }
    }
    if (expression.children.size() == 1 &&
        expression.children.front().kind == AstExpressionKind::Identifier) {
      if (auto receiver = scope.find(expression.children.front().text);
          receiver != scope.end()) {
        if (std::optional<SymbolInfo> member = resolvedMemberForReceiverType(
                receiver->second.type, expression.text);
            member.has_value() && isIntegerConstantType(member->type.kind)) {
          return member->type.kind;
        }
      }
    }
    return SimpleTypeKind::Unknown;
  case AstExpressionKind::TypeApply:
    if (isConstValueExpression(expression, scope) &&
        typeArgumentsFor(expression).size() == 1) {
      const TypeInfo constant =
          typeFromDeclaredName(expression.declaredType, &scope, &expression.span);
      return isIntegerConstantType(constant.kind) &&
                     !constant.singletonLiteral.empty()
                 ? constant.kind
                 : SimpleTypeKind::Unknown;
    }
    return SimpleTypeKind::Unknown;
  case AstExpressionKind::Unary:
    if (expression.children.size() == 1) {
      const SimpleTypeKind operand =
          constantIntegerType(expression.children.front(), scope);
      return operand == SimpleTypeKind::Byte || operand == SimpleTypeKind::Short
                 ? SimpleTypeKind::Int
                 : operand;
    }
    return SimpleTypeKind::Unknown;
  case AstExpressionKind::Binary:
    if (expression.children.size() == 2) {
      const SimpleTypeKind left =
          constantIntegerType(expression.children.front(), scope);
      const SimpleTypeKind right =
          constantIntegerType(expression.children.back(), scope);
      if (!isIntegerConstantType(left) || !isIntegerConstantType(right)) {
        return SimpleTypeKind::Unknown;
      }
      return left == SimpleTypeKind::Long || right == SimpleTypeKind::Long
                 ? SimpleTypeKind::Long
                 : SimpleTypeKind::Int;
    }
    return SimpleTypeKind::Unknown;
  default:
    return SimpleTypeKind::Unknown;
  }
}

std::optional<TypeInfo> Typechecker::recordInlineApplication(
    const AstExpression& expression, const SymbolInfo& symbol,
    const std::vector<TypeInfo>& typeArguments,
    const std::vector<AstExpression>& arguments,
    const std::vector<TypedContextArgument>& contextualArguments,
    const AstExpression* receiver, const TypeInfo* receiverType, Scope& scope,
    const TypeInfo* expectedType) {
  const std::size_t contextualParameterCount = static_cast<std::size_t>(
      std::count(symbol.contextualParameters.begin(),
                 symbol.contextualParameters.end(), true));
  const bool hasExplicitContextArguments =
      contextualArguments.empty() && arguments.size() == symbol.parameters.size();
  const bool hasMaterializedContextArguments =
      contextualArguments.size() == contextualParameterCount &&
      arguments.size() + contextualArguments.size() == symbol.parameters.size() &&
      std::all_of(contextualArguments.begin(), contextualArguments.end(),
                  [&](const TypedContextArgument& argument) {
                    return argument.parameterIndex <
                               symbol.contextualParameters.size() &&
                           symbol.contextualParameters[argument.parameterIndex];
                  });
  if (!symbol.isInline || !symbol.hasImplementation ||
      symbol.inlineBody.kind == AstExpressionKind::Empty ||
      symbol.typeParameters.size() != typeArguments.size() ||
      (!hasExplicitContextArguments && !hasMaterializedContextArguments)) {
    return std::nullopt;
  }
  if (symbol.isInstanceMember &&
      (receiver == nullptr || receiverType == nullptr)) {
    return std::nullopt;
  }
  {
    std::size_t sourceArgumentIndex = 0;
    for (std::size_t index = 0; index < symbol.parameters.size(); ++index) {
      const bool contextual = index < symbol.contextualParameters.size() &&
                              symbol.contextualParameters[index];
      if (contextual && !contextualArguments.empty()) {
        continue;
      }
      const AstExpression* sourceArgument =
          sourceArgumentIndex < arguments.size()
              ? &arguments[sourceArgumentIndex++]
              : nullptr;
      const bool inlineParameter = index < symbol.inlineParameters.size() &&
                                   symbol.inlineParameters[index];
      const SimpleTypeKind parameterKind =
          index < symbol.parameterTypes.size()
              ? symbol.parameterTypes[index].kind
              : SimpleTypeKind::Unknown;
      const bool booleanInlineParameter =
          inlineParameter && parameterKind == SimpleTypeKind::Boolean;
      const bool integerInlineParameter =
          inlineParameter && isIntegerConstantType(parameterKind);
      const bool floatingInlineParameter =
          inlineParameter && isFloatingConstantType(parameterKind);
      const bool stringInlineParameter =
          inlineParameter && parameterKind == SimpleTypeKind::String;
      const bool charInlineParameter =
          inlineParameter && parameterKind == SimpleTypeKind::Char;
      const bool hasConstantArgument =
          sourceArgument != nullptr &&
          (booleanInlineParameter
               ? constantBooleanValue(*sourceArgument, scope).has_value()
           : integerInlineParameter
               ? constantIntegerValue(*sourceArgument, scope).has_value()
           : floatingInlineParameter
               ? constantFloatingValue(*sourceArgument, scope).has_value()
           : stringInlineParameter
               ? constantStringValue(*sourceArgument, scope).has_value()
           : charInlineParameter
               ? constantCharValue(*sourceArgument, scope).has_value()
               : false);
      if ((!booleanInlineParameter && !integerInlineParameter &&
           !floatingInlineParameter && !stringInlineParameter &&
           !charInlineParameter) ||
          sourceArgument == nullptr || hasConstantArgument) {
        continue;
      }
      const std::function<bool(const AstExpression&)>
          containsUnresolvedInlineParameter = [&](const AstExpression& candidate) {
            if (candidate.kind == AstExpressionKind::Identifier) {
              if (auto parameter = scope.find(candidate.text);
                  parameter != scope.end() &&
                  parameter->second.isInlineParameter) {
                return (booleanInlineParameter &&
                        !parameter->second.specializedBooleanValue.has_value()) ||
                       (integerInlineParameter &&
                        !parameter->second.specializedIntegerValue.has_value()) ||
                       (floatingInlineParameter &&
                        !parameter->second.specializedFloatingValue.has_value()) ||
                       (stringInlineParameter &&
                        !parameter->second.specializedStringValue.has_value()) ||
                       (charInlineParameter &&
                        !parameter->second.specializedCharValue.has_value());
              }
            }
            return std::any_of(candidate.children.begin(), candidate.children.end(),
                               containsUnresolvedInlineParameter);
          };
      if (containsUnresolvedInlineParameter(*sourceArgument)) {
        return std::nullopt;
      }
    }
  }
  const std::string definitionOwner = ownerNameOf(symbol.symbolName);
  std::optional<TypeInfo> ownerReceiverType;
  if (symbol.isInstanceMember) {
    for (const TypeInfo& base : baseTypesFor(*receiverType)) {
      const std::string baseName =
          base.typeConstructorName.empty()
              ? (base.runtimeName.empty() ? base.name : base.runtimeName)
              : base.typeConstructorName;
      if (baseName == definitionOwner) {
        ownerReceiverType = base;
        break;
      }
    }
    if (auto owner = globalSymbols_.find(definitionOwner);
        owner != globalSymbols_.end() && !owner->second.typeParameters.empty() &&
        (!ownerReceiverType.has_value() ||
         ownerReceiverType->typeArguments.size() !=
             owner->second.typeParameters.size())) {
      diagnostics_.error(
          expression.span,
          "inline instance method specialization requires a fully applied "
          "generic owner receiver");
      return std::nullopt;
    }
  }
  if (inlineExpansionDepth_ >= MaxInlineExpansionDepth) {
    diagnostics_.error(expression.span,
                       "maximum inline expansion depth of " +
                           std::to_string(MaxInlineExpansionDepth) +
                           " exceeded while expanding " + symbol.name);
    return std::nullopt;
  }

  Scope inlineScope = scope;
  std::unordered_map<std::string, TypeInfo> substitutions;
  if (symbol.isInstanceMember && ownerReceiverType.has_value()) {
    auto owner = globalSymbols_.find(definitionOwner);
    if (owner != globalSymbols_.end()) {
      const std::size_t ownerArgumentCount =
          std::min(owner->second.typeParameters.size(),
                   ownerReceiverType->typeArguments.size());
      for (std::size_t index = 0; index < ownerArgumentCount; ++index) {
        const TypeParameterInfo& parameter = owner->second.typeParameters[index];
        const TypeInfo& argument = ownerReceiverType->typeArguments[index];
        substitutions[parameter.symbolName] = argument;
        SymbolInfo concreteType;
        concreteType.kind = AstDeclarationKind::Type;
        concreteType.name = parameter.name;
        concreteType.symbolName = parameter.symbolName;
        concreteType.type = argument;
        concreteType.lowerBound = argument;
        concreteType.upperBound = argument;
        concreteType.hasImplementation = true;
        inlineScope[parameter.name] = std::move(concreteType);
      }
    }
  }
  if (auto members = memberScopes_.find(definitionOwner);
      members != memberScopes_.end()) {
    for (const auto& [name, member] : members->second) {
      inlineScope[name] =
          ownerReceiverType.has_value()
              ? specializeMemberForReceiver(member, *ownerReceiverType)
              : member;
    }
  }
  if (symbol.isInstanceMember) {
    SymbolInfo thisSymbol;
    thisSymbol.kind = AstDeclarationKind::Val;
    thisSymbol.name = "this";
    thisSymbol.symbolName = "this";
    thisSymbol.type = *receiverType;
    thisSymbol.isLexicalValue = true;
    inlineScope["this"] = std::move(thisSymbol);
  }
  for (std::size_t index = 0; index < typeArguments.size(); ++index) {
    const TypeParameterInfo& parameter = symbol.typeParameters[index];
    substitutions[parameter.symbolName] = typeArguments[index];
    SymbolInfo concreteType;
    concreteType.kind = AstDeclarationKind::Type;
    concreteType.name = parameter.name;
    concreteType.symbolName = parameter.symbolName;
    concreteType.type = typeArguments[index];
    concreteType.lowerBound = typeArguments[index];
    concreteType.upperBound = typeArguments[index];
    concreteType.hasImplementation = true;
    inlineScope[parameter.name] = std::move(concreteType);
  }

  std::vector<std::string> parameterNames;
  std::vector<TypeInfo> parameterTypes;
  parameterNames.reserve(symbol.parameters.size());
  parameterTypes.reserve(symbol.parameters.size());
  std::size_t sourceArgumentIndex = 0;
  for (std::size_t index = 0; index < symbol.parameters.size(); ++index) {
    const std::string name = parameterName(symbol.parameters[index]);
    TypeInfo type =
        index < symbol.parameterTypes.size()
            ? substituteTypeParameters(symbol.parameterTypes[index], substitutions)
            : TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
    SymbolInfo parameter;
    parameter.kind = parameterDeclarationKind(symbol.parameters[index]);
    parameter.name = name;
    parameter.symbolName = qualify(symbol.symbolName, name);
    parameter.type = type;
    parameter.isContextParameter =
        index < symbol.contextualParameters.size() &&
        symbol.contextualParameters[index];
    parameter.isInlineParameter =
        index < symbol.inlineParameters.size() && symbol.inlineParameters[index];
    const bool materializedContextParameter =
        parameter.isContextParameter && !contextualArguments.empty();
    const AstExpression* sourceArgument = nullptr;
    if (!materializedContextParameter && sourceArgumentIndex < arguments.size()) {
      sourceArgument = &arguments[sourceArgumentIndex++];
    }
    if (sourceArgument != nullptr) {
      parameter.specializedStaticType =
          specializedStaticType(*sourceArgument, scope);
      parameter.specializedBooleanValue =
          constantBooleanValue(*sourceArgument, scope);
      parameter.specializedIntegerValue =
          constantIntegerValue(*sourceArgument, scope);
      parameter.specializedFloatingValue =
          constantFloatingValue(*sourceArgument, scope);
      parameter.specializedStringValue =
          constantStringValue(*sourceArgument, scope);
      parameter.specializedCharValue = constantCharValue(*sourceArgument, scope);
      parameter.specializedNullValue = constantNullValue(*sourceArgument, scope);
      parameter.specializedCode =
          sourceCodeForExpression(*sourceArgument, scope);
    } else if (materializedContextParameter) {
      auto contextual = std::find_if(
          contextualArguments.begin(), contextualArguments.end(),
          [&](const TypedContextArgument& argument) {
            return argument.parameterIndex == index;
          });
      if (contextual != contextualArguments.end()) {
        parameter.specializedStaticType = contextual->type;
      }
    }
    parameter.isLexicalValue = true;
    inlineScope[name] = std::move(parameter);
    parameterNames.push_back(name);
    parameterTypes.push_back(std::move(type));
  }

  std::vector<TypedExpressionInfo> outerExpressionTypes =
      std::move(expressionTypes_);
  std::vector<TypedContextApplication> outerContextApplications =
      std::move(contextApplications_);
  std::vector<TypedInlineApplication> outerInlineApplications =
      std::move(inlineApplications_);
  expressionTypes_.clear();
  contextApplications_.clear();
  inlineApplications_.clear();

  ++inlineExpansionDepth_;
  TypeInfo resultType = inferExpressionType(
      symbol.inlineBody, inlineScope,
      symbol.isTransparent ? nullptr : expectedType);
  --inlineExpansionDepth_;

  TypedInlineApplication application;
  application.span = expression.span;
  application.expressionKind = expression.kind;
  application.symbolName = symbol.symbolName;
  application.ownerName = definitionOwner;
  application.body = symbol.inlineBody;
  application.parameterNames = std::move(parameterNames);
  application.parameterTypes = std::move(parameterTypes);
  application.inlineParameters = symbol.inlineParameters;
  application.arguments = arguments;
  application.contextualArguments = contextualArguments;
  application.resultType = resultType;
  application.hasReceiver = symbol.isInstanceMember;
  if (application.hasReceiver) {
    application.receiver = *receiver;
    application.receiverType = *receiverType;
  }
  application.expressionTypes = std::move(expressionTypes_);
  application.contextApplications = std::move(contextApplications_);
  application.inlineApplications = std::move(inlineApplications_);

  expressionTypes_ = std::move(outerExpressionTypes);
  contextApplications_ = std::move(outerContextApplications);
  inlineApplications_ = std::move(outerInlineApplications);

  const auto sameSpan = [&](const TypedInlineApplication& candidate) {
    return candidate.expressionKind == expression.kind &&
           candidate.span.source == expression.span.source &&
           candidate.span.start == expression.span.start &&
           candidate.span.length == expression.span.length;
  };
  auto existing =
      std::find_if(inlineApplications_.begin(), inlineApplications_.end(), sameSpan);
  if (existing == inlineApplications_.end()) {
    inlineApplications_.push_back(std::move(application));
  } else {
    *existing = std::move(application);
  }
  return symbol.isTransparent
             ? std::optional<TypeInfo>(staticExpressionType(std::move(resultType)))
             : std::nullopt;
}

bool Typechecker::isSupportedArrayElementType(const TypeInfo& candidate,
                                              const Scope& scope,
                                              const support::SourceSpan& span) const {
  if (isBuiltinArrayElementKind(candidate.kind) || isAnyArrayElementType(candidate)) {
    return true;
  }
  if (candidate.kind != SimpleTypeKind::Object) {
    return false;
  }
  if (const std::string nestedElement = arrayElementTypeName(candidate.name);
      !nestedElement.empty()) {
    return isSupportedArrayElementType(
        typeFromDeclaredName(nestedElement, &scope, &span), scope, span);
  }
  const SymbolInfo* symbol = typeSymbolForDeclaredName(candidate.name, &scope);
  return symbol != nullptr && (symbol->kind == AstDeclarationKind::Class ||
                               symbol->kind == AstDeclarationKind::Trait ||
                               symbol->kind == AstDeclarationKind::Object);
}

bool Typechecker::arrayElementConforms(const TypeInfo& expected,
                                       const TypeInfo& actual) const {
  if (actual.kind == SimpleTypeKind::Unknown) {
    return true;
  }
  if (isAnyArrayElementType(expected)) {
    return isSupportedAnyArrayValueType(actual);
  }
  return expected.kind == SimpleTypeKind::Object ? isAssignable(expected, actual)
                                                 : expected.kind == actual.kind;
}

std::optional<TypedPolymorphicFunctionApplication>
Typechecker::typecheckPolymorphicFunctionLiteral(
    const AstExpression& function, const std::vector<TypeInfo>& typeArguments,
    const support::SourceSpan& applicationSpan, Scope& scope,
    const std::string& shapeDiagnostic) {
  const AstLocalMethod* method = function.localMethod.get();
  const bool basicShape =
      method != nullptr && method->typeParameters.size() == 1 &&
      method->parameters.size() == 1 && method->contextualParameters.size() == 1 &&
      !method->contextualParameters.front() && function.children.size() == 1 &&
      method->typeParameters.front().variance == TypeVariance::Invariant &&
      method->typeParameters.front().lowerBound.empty() &&
      method->typeParameters.front().upperBound.empty() &&
      method->typeParameters.front().contextBounds.empty();
  if (!basicShape) {
    diagnostics_.error(function.span, shapeDiagnostic);
    return std::nullopt;
  }

  Scope functionScope = scope;
  const std::string functionOwner =
      qualify(currentPackageName_, "$poly$" + std::to_string(function.span.start));
  const std::vector<TypeParameterInfo> typeParameters =
      resolvedTypeParameters(method->typeParameters, functionOwner, functionScope);
  const TypeInfo parameter =
      parameterType(method->parameters.front(), &functionScope, &function.span);
  const bool exactParameter =
      typeParameters.size() == 1 && parameter.typeParameter &&
      parameter.typeParameterSymbolName == typeParameters.front().symbolName;
  if (!exactParameter) {
    diagnostics_.error(function.span, shapeDiagnostic);
    return std::nullopt;
  }

  SymbolInfo valueParameter;
  valueParameter.kind = AstDeclarationKind::Val;
  valueParameter.name = parameterName(method->parameters.front());
  valueParameter.symbolName = qualify(functionOwner, valueParameter.name);
  valueParameter.type = parameter;
  valueParameter.isLexicalValue = true;
  functionScope[valueParameter.name] = valueParameter;

  std::vector<TypedExpressionInfo> outerExpressionTypes = std::move(expressionTypes_);
  std::vector<TypedContextApplication> outerContextApplications =
      std::move(contextApplications_);
  std::vector<TypedInlineApplication> outerInlineApplications =
      std::move(inlineApplications_);
  expressionTypes_.clear();
  contextApplications_.clear();
  inlineApplications_.clear();

  const TypeInfo functionResult =
      inferExpressionType(function.children.front(), functionScope);

  TypedPolymorphicFunctionApplication application;
  application.span = applicationSpan;
  application.parameterName = valueParameter.name;
  application.parameterType = parameter;
  application.resultType = functionResult;
  application.typeArguments = typeArguments;
  application.body = function.children.front();
  application.expressionTypes = std::move(expressionTypes_);
  application.contextApplications = std::move(contextApplications_);
  application.inlineApplications = std::move(inlineApplications_);

  expressionTypes_ = std::move(outerExpressionTypes);
  contextApplications_ = std::move(outerContextApplications);
  inlineApplications_ = std::move(outerInlineApplications);

  const SymbolInfo* polyFunction = typeSymbolForDeclaredName(
      std::string(support::StdNames::ScalaPolyFunction), &scope);
  const TypeInfo literalType =
      polyFunction == nullptr
          ? TypeInfo{SimpleTypeKind::Object,
                     std::string(support::StdNames::ScalaPolyFunction)}
          : polyFunction->type;
  if (function.span.isValid()) {
    const auto sameSpan = [&](const TypedExpressionInfo& info) {
      return info.span.source == function.span.source &&
             info.span.start == function.span.start &&
             info.span.length == function.span.length;
    };
    auto existing =
        std::find_if(expressionTypes_.rbegin(), expressionTypes_.rend(), sameSpan);
    if (existing == expressionTypes_.rend()) {
      expressionTypes_.push_back(TypedExpressionInfo{function.span, literalType});
    } else {
      existing->type = literalType;
    }
  }

  application.mappedResultTypes.reserve(typeArguments.size());
  std::unordered_map<std::string, TypeInfo> substitutions;
  for (const TypeInfo& typeArgument : typeArguments) {
    substitutions[typeParameters.front().symbolName] = typeArgument;
    application.mappedResultTypes.push_back(
        substituteTypeParameters(functionResult, substitutions));
  }
  return application;
}

std::optional<TypedPolymorphicFunctionApplication>
Typechecker::polymorphicFunctionAlias(const AstExpression& expression,
                                      const Scope& scope) const {
  if (expression.kind != AstExpressionKind::Identifier) {
    return std::nullopt;
  }
  auto source = scope.find(expression.text);
  if (source == scope.end() || source->second.polymorphicFunctionValue == nullptr) {
    return std::nullopt;
  }

  TypedPolymorphicFunctionApplication alias = *source->second.polymorphicFunctionValue;
  if (source->second.isInstanceMember && !alias.hasReceiver) {
    auto self = scope.find("this");
    if (self == scope.end()) {
      return std::nullopt;
    }
    alias.hasReceiver = true;
    alias.receiver.kind = AstExpressionKind::This;
    alias.receiver.span = expression.span;
    alias.receiverType = self->second.type;
  }
  return alias;
}

bool Typechecker::polymorphicFunctionMatchesDeclaredType(
    const TypeInfo& declared,
    const TypedPolymorphicFunctionApplication& function) const {
  if (!declared.polymorphicFunctionType) {
    return true;
  }
  if (declared.typeArguments.size() != 2 ||
      !declared.typeArguments.front().typeParameter ||
      !function.parameterType.typeParameter) {
    return false;
  }

  TypeInfo canonicalParameter{SimpleTypeKind::Object, "$polytype"};
  canonicalParameter.runtimeName = "Object";
  std::unordered_map<std::string, TypeInfo> declaredSubstitution;
  declaredSubstitution[declared.typeArguments.front().typeParameterSymbolName] =
      canonicalParameter;
  std::unordered_map<std::string, TypeInfo> functionSubstitution;
  functionSubstitution[function.parameterType.typeParameterSymbolName] =
      canonicalParameter;
  const TypeInfo declaredResult =
      substituteTypeParameters(declared.typeArguments[1], declaredSubstitution);
  const TypeInfo functionResult =
      substituteTypeParameters(function.resultType, functionSubstitution);
  return typesMatchForOverride(declaredResult, functionResult);
}

TypeInfo Typechecker::typecheckRuntimePolymorphicFunctionLiteral(
    const AstExpression& function, const TypeInfo& expectedType, Scope& scope) {
  std::optional<TypedPolymorphicFunctionApplication> application =
      typecheckPolymorphicFunctionLiteral(
          function, {}, function.span, scope,
          "runtime polymorphic function literal must have the form "
          "[A] => (value: A) => body");
  if (!application.has_value()) {
    return expectedType;
  }
  if (!polymorphicFunctionMatchesDeclaredType(expectedType, *application)) {
    const std::string expectedResult = expectedType.typeArguments.size() == 2
                                           ? expectedType.typeArguments[1].name
                                           : "Unknown";
    diagnostics_.error(function.span, "polymorphic function result type " +
                                          application->resultType.name +
                                          " does not conform to declared result type " +
                                          expectedResult);
    return expectedType;
  }

  std::vector<TypedPolymorphicFunctionCapture> captures;
  std::vector<std::string> capturedMemberNames;
  std::vector<TypedPolymorphicFunctionModuleMember> moduleMembers;
  std::unordered_set<std::string> capturedNames;
  std::unordered_set<std::string> capturedMembers;
  std::unordered_set<std::string> capturedModuleMembers;
  std::vector<std::string> mutableCaptures;
  std::vector<std::string> erasedCaptures;
  bool capturesSuper = false;
  bool missingOuterReceiver = false;
  const auto captureOuterReceiver = [&](const support::SourceSpan& span) {
    if (!capturedNames.insert("$outer$this").second) {
      return;
    }
    auto self = scope.find("this");
    if (self == scope.end()) {
      missingOuterReceiver = true;
      return;
    }
    AstExpression value;
    value.kind = AstExpressionKind::This;
    value.text = "this";
    value.span = span;
    captures.push_back(TypedPolymorphicFunctionCapture{
        "this", "$capture$this", self->second.type, std::move(value), true});
  };
  std::function<void(const AstExpression&, std::unordered_set<std::string>&)>
      collectCaptures;
  collectCaptures = [&](const AstExpression& expression,
                        std::unordered_set<std::string>& boundNames) {
    if (expression.kind == AstExpressionKind::Identifier &&
        !boundNames.contains(expression.text)) {
      auto symbol = scope.find(expression.text);
      if (symbol == scope.end()) {
        return;
      }
      const std::string memberOwner = ownerNameOf(symbol->second.symbolName);
      auto owner = globalSymbols_.find(memberOwner);
      if (owner != globalSymbols_.end() &&
          owner->second.kind == AstDeclarationKind::Object &&
          (symbol->second.kind == AstDeclarationKind::Def ||
           symbol->second.kind == AstDeclarationKind::Val ||
           symbol->second.kind == AstDeclarationKind::Var)) {
        if (capturedModuleMembers.insert(expression.text).second) {
          moduleMembers.push_back(TypedPolymorphicFunctionModuleMember{
              expression.text, symbol->second.symbolName,
              symbol->second.kind == AstDeclarationKind::Val ||
                  symbol->second.kind == AstDeclarationKind::Var});
        }
        return;
      }
      if (symbol->second.isInstanceMember) {
        captureOuterReceiver(expression.span);
        if (capturedMembers.insert(expression.text).second) {
          capturedMemberNames.push_back(expression.text);
        }
        return;
      }
      if (!symbol->second.isLexicalValue ||
          !capturedNames.insert(expression.text).second) {
        return;
      }
      if (symbol->second.kind == AstDeclarationKind::Var) {
        mutableCaptures.push_back(expression.text);
        return;
      }
      if (symbol->second.isErasedCompileTimeValue ||
          symbol->second.polymorphicFunctionValue != nullptr) {
        erasedCaptures.push_back(expression.text);
        return;
      }
      AstExpression value;
      value.kind = AstExpressionKind::Identifier;
      value.text = expression.text;
      value.span = expression.span;
      captures.push_back(TypedPolymorphicFunctionCapture{
          expression.text, "$capture$" + std::to_string(captures.size()),
          symbol->second.type, std::move(value), false});
      return;
    }
    if (expression.kind == AstExpressionKind::This) {
      captureOuterReceiver(expression.span);
      return;
    }
    if (expression.kind == AstExpressionKind::Super) {
      capturesSuper = true;
      return;
    }
    if (expression.kind == AstExpressionKind::Block) {
      std::unordered_set<std::string> blockNames = boundNames;
      for (const AstExpression& child : expression.children) {
        if (child.kind == AstExpressionKind::LocalDeclaration) {
          for (const AstExpression& initializer : child.children) {
            collectCaptures(initializer, blockNames);
          }
          if (!child.text.empty()) {
            blockNames.insert(child.text);
          }
        } else {
          collectCaptures(child, blockNames);
        }
      }
      return;
    }
    for (const AstExpression& child : expression.children) {
      collectCaptures(child, boundNames);
    }
  };
  std::unordered_set<std::string> boundNames{application->parameterName};
  collectCaptures(application->body, boundNames);
  const auto joinedNames = [](const std::vector<std::string>& names) {
    std::string joined;
    for (std::size_t index = 0; index < names.size(); ++index) {
      if (index != 0) {
        joined += ", ";
      }
      joined += names[index];
    }
    return joined;
  };
  if (!mutableCaptures.empty()) {
    diagnostics_.error(
        function.span,
        "runtime polymorphic function closures cannot capture mutable local "
        "values yet: " +
            joinedNames(mutableCaptures));
  }
  if (!erasedCaptures.empty()) {
    diagnostics_.error(function.span,
                       "runtime polymorphic function closures cannot capture erased "
                       "compiler-known values yet: " +
                           joinedNames(erasedCaptures));
  }
  if (capturesSuper) {
    diagnostics_.error(
        function.span,
        "runtime polymorphic function closures cannot capture super yet");
  }
  if (missingOuterReceiver) {
    diagnostics_.error(
        function.span,
        "runtime polymorphic function closure has no outer receiver to capture");
  }
  if (!mutableCaptures.empty() || !erasedCaptures.empty() || capturesSuper ||
      missingOuterReceiver) {
    return expectedType;
  }

  const auto sameSpan = [&](const TypedPolymorphicFunctionClosure& closure) {
    return closure.span.source == function.span.source &&
           closure.span.start == function.span.start &&
           closure.span.length == function.span.length;
  };
  if (std::none_of(polymorphicFunctionClosures_.begin(),
                   polymorphicFunctionClosures_.end(), sameSpan)) {
    const std::string simpleName = "$polyclosure$" +
                                   std::to_string(function.span.source.value()) + "$" +
                                   std::to_string(function.span.start);
    const std::string className = qualify(currentPackageName_, simpleName);

    TypedDeclaration closure;
    closure.kind = AstDeclarationKind::Class;
    closure.name = simpleName;
    closure.symbolName = className;
    closure.span = function.span;
    closure.declaredType = std::string(support::StdNames::ScalaPolyFunction);
    closure.parentTypes = {std::string(support::StdNames::ScalaPolyFunction)};
    closure.parentTypeInfos = {TypeInfo{
        SimpleTypeKind::Object, std::string(support::StdNames::ScalaPolyFunction)}};
    closure.inferredType = TypeInfo{SimpleTypeKind::Object, className};
    closure.inferredType.runtimeName = className;
    for (const TypedPolymorphicFunctionCapture& capture : captures) {
      closure.parameters.push_back(capture.fieldName + ": " + capture.type.name);
      closure.parameterTypes.push_back(capture.type);
      closure.contextualParameters.push_back(false);
      closure.inlineParameters.push_back(false);
    }
    if (!captures.empty()) {
      closure.parameterClauseSizes = {captures.size()};
      closure.contextualParameterClauses = {false};
    }

    TypeParameterInfo typeParameter;
    typeParameter.name = application->parameterType.name;
    typeParameter.symbolName = application->parameterType.typeParameterSymbolName;
    typeParameter.lowerBound = TypeInfo{SimpleTypeKind::Nothing, "Nothing"};
    typeParameter.upperBound = TypeInfo{SimpleTypeKind::Object, "Object"};

    TypedDeclaration apply;
    apply.kind = AstDeclarationKind::Def;
    apply.name = std::string(support::StdNames::TupleApply);
    apply.symbolName = className + "." + apply.name;
    apply.span = function.span;
    apply.typeParameters = {std::move(typeParameter)};
    apply.parameters = {application->parameterName + ": " +
                        application->parameterType.name};
    apply.parameterTypes = {application->parameterType};
    apply.contextualParameters = {false};
    apply.inlineParameters = {false};
    apply.parameterClauseSizes = {1};
    apply.contextualParameterClauses = {false};
    apply.declaredType = application->resultType.name;
    apply.inferredType = application->resultType;
    apply.inferredType.runtimeName = "Object";
    apply.hasInitializer = true;
    apply.initializer = application->body;
    closure.members.push_back(std::move(apply));

    expressionTypes_.insert(expressionTypes_.end(),
                            application->expressionTypes.begin(),
                            application->expressionTypes.end());
    contextApplications_.insert(contextApplications_.end(),
                                application->contextApplications.begin(),
                                application->contextApplications.end());
    inlineApplications_.insert(inlineApplications_.end(),
                               application->inlineApplications.begin(),
                               application->inlineApplications.end());
    polymorphicFunctionClosures_.push_back(TypedPolymorphicFunctionClosure{
        function.span, className, std::move(captures), std::move(capturedMemberNames),
        std::move(moduleMembers)});
    polymorphicFunctionClosureDeclarations_.push_back(std::move(closure));
  }
  return expectedType;
}

void Typechecker::recordPolymorphicFunctionApplication(
    TypedPolymorphicFunctionApplication application) {
  const auto sameSpan = [&](const TypedPolymorphicFunctionApplication& candidate) {
    return candidate.span.source == application.span.source &&
           candidate.span.start == application.span.start &&
           candidate.span.length == application.span.length;
  };
  auto existing = std::find_if(polymorphicFunctionApplications_.begin(),
                               polymorphicFunctionApplications_.end(), sameSpan);
  if (existing == polymorphicFunctionApplications_.end()) {
    polymorphicFunctionApplications_.push_back(std::move(application));
  } else {
    *existing = std::move(application);
  }
}

TypeInfo Typechecker::inferExpressionTypeImpl(const AstExpression& expression,
                                              Scope& scope,
                                              const TypeInfo* expectedType) {
  if (isUninitializedExpression(expression, scope)) {
    if (allowedUninitializedExpression_ != &expression) {
      diagnostics_.error(
          expression.span,
          "uninitialized may only initialize a mutable class or object field");
      return TypeInfo{SimpleTypeKind::Nothing, "Nothing"};
    }
    return allowedUninitializedType_;
  }

  switch (expression.kind) {
  case AstExpressionKind::Empty:
    return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
  case AstExpressionKind::IntegerLiteral: {
    const SimpleTypeKind kind =
        !expression.text.empty() &&
                (expression.text.back() == 'l' || expression.text.back() == 'L')
            ? SimpleTypeKind::Long
            : SimpleTypeKind::Int;
    if (expectedType != nullptr && expectedType->kind == kind &&
        !expectedType->singletonLiteral.empty()) {
      const std::optional<std::int64_t> literal =
          parseIntegerConstant(expression.text);
      const std::optional<std::int64_t> expectedLiteral =
          parseIntegerConstant(expectedType->singletonLiteral);
      if (literal.has_value() && expectedLiteral.has_value() &&
          *literal == *expectedLiteral) {
        return *expectedType;
      }
    }
    return TypeInfo{kind, kind == SimpleTypeKind::Long ? "Long" : "Int"};
  }
  case AstExpressionKind::FloatingLiteral:
    if (!expression.text.empty() &&
        (expression.text.back() == 'f' || expression.text.back() == 'F')) {
      return TypeInfo{SimpleTypeKind::Float, "Float"};
    }
    return TypeInfo{SimpleTypeKind::Double, "Double"};
  case AstExpressionKind::StringLiteral:
    return TypeInfo{SimpleTypeKind::String, "String"};
  case AstExpressionKind::CharLiteral:
    return TypeInfo{SimpleTypeKind::Char, "Char"};
  case AstExpressionKind::SymbolLiteral:
    return TypeInfo{SimpleTypeKind::Symbol, "Symbol"};
  case AstExpressionKind::BooleanLiteral:
    return TypeInfo{SimpleTypeKind::Boolean, "Boolean"};
  case AstExpressionKind::NullLiteral:
    return TypeInfo{SimpleTypeKind::Null, "Null"};
  case AstExpressionKind::This:
    if (auto found = scope.find("this"); found != scope.end()) {
      return found->second.type;
    }
    diagnostics_.error(expression.span,
                       "this is only available in class or trait members");
    return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
  case AstExpressionKind::Super: {
    if (expression.text.empty() || expression.text == "super") {
      if (auto found = scope.find("super"); found != scope.end()) {
        return found->second.type;
      }
      diagnostics_.error(expression.span,
                         "super is only available in classes with a parent");
      return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
    }

    const SymbolInfo* parent = typeSymbolForDeclaredName(expression.text, &scope);
    if (parent == nullptr || !isInheritableDeclaration(parent->kind)) {
      diagnostics_.error(expression.span,
                         "unresolved qualified super type: " + expression.text);
      return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
    }
    const std::string key = "super:" + parent->symbolName;
    if (auto found = scope.find(key); found != scope.end()) {
      return found->second.type;
    }
    diagnostics_.error(expression.span,
                       "qualified super type is not a direct parent: " +
                           expression.text);
    return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
  }
  case AstExpressionKind::New:
    return inferNewType(expression, scope);
  case AstExpressionKind::TupleLiteral: {
    if (expression.children.size() < 2 || expression.children.size() > 22) {
      return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
    }
    const std::string constructorName =
        std::string(support::StdNames::ScalaTuple) +
        std::to_string(expression.children.size());
    auto constructor = globalSymbols_.find(constructorName);
    if (constructor == globalSymbols_.end()) {
      diagnostics_.error(expression.span,
                         "unresolved tuple value constructor: " + constructorName);
      return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
    }
    std::vector<TypeInfo> elementTypes;
    elementTypes.reserve(expression.children.size());
    for (const AstExpression& element : expression.children) {
      elementTypes.push_back(inferExpressionType(element, scope));
    }
    return specializeResolvedTypeApplication(
               constructor->second, elementTypes, expression.span, true)
        .type;
  }
  case AstExpressionKind::PolymorphicFunction: {
    if (expectedType != nullptr && expectedType->polymorphicFunctionType) {
      return typecheckRuntimePolymorphicFunctionLiteral(expression, *expectedType,
                                                        scope);
    }
    diagnostics_.error(
        expression.span,
        "polymorphic function literals are currently supported only as direct "
        "Tuple.map arguments, direct invocations, or immutable val initializers; "
        "they are also supported as runtime values with an explicit polymorphic "
        "function type");
    const SymbolInfo* polyFunction = typeSymbolForDeclaredName(
        std::string(support::StdNames::ScalaPolyFunction), &scope);
    return polyFunction == nullptr
               ? TypeInfo{SimpleTypeKind::Object,
                          std::string(support::StdNames::ScalaPolyFunction)}
               : polyFunction->type;
  }
  case AstExpressionKind::LocalDeclaration:
    if (expression.text.empty()) {
      diagnostics_.error(expression.span, "local declaration has no name");
      return TypeInfo{SimpleTypeKind::Unit, "Unit"};
    }
    return TypeInfo{SimpleTypeKind::Unit, "Unit"};
  case AstExpressionKind::Identifier: {
    auto found = scope.find(expression.text);
    if (found == scope.end()) {
      diagnostics_.error(expression.span, "unresolved identifier: " + expression.text);
      return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
    }
    if (found->second.isErasedCompileTimeValue) {
      diagnostics_.error(
          expression.span,
          "erasedValue inline match patterns cannot bind a runtime value");
      return TypeInfo{SimpleTypeKind::Object, "Object"};
    }
    if (found->second.polymorphicFunctionValue != nullptr) {
      diagnostics_.error(
          expression.span,
          "polymorphic function value " + found->second.name +
              " must be invoked directly with one explicit type argument and one "
              "value argument");
      return found->second.type;
    }
    if ((found->second.kind == AstDeclarationKind::Class ||
         found->second.kind == AstDeclarationKind::Trait) &&
        companionTypeNames_.contains(found->second.symbolName)) {
      auto companion = globalSymbols_.find(found->second.symbolName + '$');
      if (companion != globalSymbols_.end()) {
        return companion->second.type;
      }
    }
    if (found->second.kind == AstDeclarationKind::Def &&
        !found->second.typeParameters.empty()) {
      diagnostics_.error(expression.span,
                         "generic method " + found->second.name + " requires " +
                             std::to_string(found->second.typeParameters.size()) +
                             " explicit type arguments");
    }
    if (found->second.kind == AstDeclarationKind::Def &&
        found->second.isInline && found->second.typeParameters.empty() &&
        found->second.parameters.empty() &&
        found->second.parameterClauseSizes.empty()) {
      std::optional<AstExpression> receiver;
      std::optional<TypeInfo> receiverType;
      if (found->second.isInstanceMember) {
        if (auto thisSymbol = scope.find("this"); thisSymbol != scope.end()) {
          AstExpression implicitReceiver;
          implicitReceiver.kind = AstExpressionKind::This;
          implicitReceiver.span = expression.span;
          receiver = std::move(implicitReceiver);
          receiverType = thisSymbol->second.type;
        }
      }
      if (std::optional<TypeInfo> transparentResult = recordInlineApplication(
              expression, found->second, {}, {}, {},
              receiver.has_value() ? &*receiver : nullptr,
              receiverType.has_value() ? &*receiverType : nullptr, scope,
              expectedType)) {
        return *transparentResult;
      }
    }
    return found->second.type;
  }
  case AstExpressionKind::ModuleReference: {
    auto found = scope.find(expression.text);
    if (found != scope.end() && found->second.kind == AstDeclarationKind::Object) {
      return found->second.type;
    }

    SymbolInfo resolved;
    bool hasResolved = false;
    const std::size_t firstDot = expression.text.find('.');
    if (firstDot != std::string::npos) {
      auto root = scope.find(expression.text.substr(0, firstDot));
      if (root != scope.end() && root->second.kind == AstDeclarationKind::Object) {
        resolved = root->second;
        hasResolved = true;
        std::size_t segmentStart = firstDot + 1;
        while (segmentStart < expression.text.size()) {
          const std::size_t nextDot = expression.text.find('.', segmentStart);
          const std::string segment = expression.text.substr(
              segmentStart, nextDot == std::string::npos ? std::string::npos
                                                         : nextDot - segmentStart);
          auto members = memberScopes_.find(resolved.type.name);
          if (members == memberScopes_.end()) {
            hasResolved = false;
            break;
          }
          auto member = members->second.find(segment);
          if (member == members->second.end() ||
              member->second.kind != AstDeclarationKind::Object) {
            hasResolved = false;
            break;
          }
          resolved = member->second;
          segmentStart =
              nextDot == std::string::npos ? expression.text.size() : nextDot + 1;
        }
      }
    }
    if (!hasResolved || resolved.kind != AstDeclarationKind::Object) {
      diagnostics_.error(expression.span,
                         "singleton match pattern must name an object: " +
                             expression.text);
      return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
    }
    return resolved.type;
  }
  case AstExpressionKind::Select:
    return inferSelectType(expression, scope);
  case AstExpressionKind::TypeApply: {
    if (expression.children.size() != 1) {
      diagnostics_.error(expression.span, "type application requires one target");
      return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
    }
    const std::vector<std::string> typeArguments = typeArgumentsFor(expression);
    const AstExpression& callee = expression.children.front();
    const bool isSizeOf = callee.kind == AstExpressionKind::Identifier &&
                          callee.text == support::StdNames::SizeOf;
    const bool isConstValue = isConstValueCallee(callee, scope);
    const bool isConstValueOpt = isConstValueOptCallee(callee, scope);
    const bool isConstValueTuple = isConstValueTupleCallee(callee, scope);
    const bool isErasedValue = isErasedValueCallee(callee, scope);
    const bool isSummonInline = isSummonInlineCallee(callee, scope);
    const bool isSummonAll = isSummonAllCallee(callee, scope);
    const bool isSummon =
        callee.kind == AstExpressionKind::Identifier &&
        (callee.text == SummonName || callee.text == ImplicitlyName);
    const bool isTypeTest = callee.kind == AstExpressionKind::Select &&
                            callee.children.size() == 1 &&
                            callee.text == support::StdNames::IsInstanceOf;
    const bool isCast = callee.kind == AstExpressionKind::Select &&
                        callee.children.size() == 1 &&
                        callee.text == support::StdNames::AsInstanceOf;
    const bool isArrayEmpty =
        callee.kind == AstExpressionKind::Select && callee.children.size() == 1 &&
        callee.text == support::StdNames::ArrayEmpty &&
        callee.children.front().kind == AstExpressionKind::Identifier &&
        callee.children.front().text == "Array";
    if (isConstValue) {
      if (typeArguments.size() != 1) {
        diagnostics_.error(expression.span,
                           "constValue requires exactly one type argument");
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }
      const TypeInfo constant =
          typeFromDeclaredName(expression.declaredType, &scope, &expression.span);
      if (!constant.singletonLiteral.empty()) {
        return constant;
      }
      if (constant.typeParameter && inlineDefinitionDepth_ != 0) {
        return constant;
      }
      diagnostics_.error(expression.span,
                         "constValue requires a constant singleton type: " +
                             expression.declaredType);
      return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
    }
    if (isConstValueOpt) {
      if (typeArguments.size() != 1) {
        diagnostics_.error(expression.span,
                           "constValueOpt requires exactly one type argument");
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }
      const TypeInfo argument =
          typeFromDeclaredName(expression.declaredType, &scope, &expression.span);
      TypeInfo optionType{
          SimpleTypeKind::Object,
          std::string(support::StdNames::ScalaOption) + " [ " + argument.name +
              " ]"};
      optionType.runtimeName = std::string(support::StdNames::ScalaOption);
      optionType.typeConstructorName = std::string(support::StdNames::ScalaOption);
      optionType.typeArguments = {argument};
      return optionType;
    }
    if (isConstValueTuple) {
      if (typeArguments.size() != 1) {
        diagnostics_.error(expression.span,
                           "constValueTuple requires exactly one type argument");
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }
      const TypeInfo tuple =
          typeFromDeclaredName(expression.declaredType, &scope, &expression.span);
      if (tuple.typeParameter) {
        if (inlineDefinitionDepth_ != 0 &&
            tuple.runtimeName == support::StdNames::ScalaTuple) {
          return tuple;
        }
        diagnostics_.error(expression.span,
                           "constValueTuple requires a concrete tuple type");
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }
      const std::string constructor =
          tuple.typeConstructorName.empty() ? tuple.runtimeName
                                            : tuple.typeConstructorName;
      const bool emptyTuple =
          tuple.name == support::StdNames::ScalaEmptyTuple ||
          tuple.runtimeName == support::StdNames::ScalaEmptyTuple;
      const std::optional<std::size_t> tupleArity =
          tupleArityForConstructor(constructor);
      if (!emptyTuple &&
          (!tupleArity.has_value() || tuple.typeArguments.size() != *tupleArity)) {
        diagnostics_.error(expression.span,
                           "constValueTuple requires a tuple type");
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }
      const bool constantElements = std::all_of(
          tuple.typeArguments.begin(), tuple.typeArguments.end(),
          [](const TypeInfo& element) { return !element.singletonLiteral.empty(); });
      if (!constantElements) {
        diagnostics_.error(
            expression.span,
            "constValueTuple requires constant singleton element types");
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }
      return tuple;
    }
    if (isErasedValue) {
      if (typeArguments.size() != 1) {
        diagnostics_.error(expression.span,
                           "erasedValue requires exactly one type argument");
        return TypeInfo{SimpleTypeKind::Object, "Object"};
      }
      (void)typeFromDeclaredName(expression.declaredType, &scope,
                                 &expression.span);
      if (erasedValueSelectorDepth_ == 0) {
        diagnostics_.error(
            expression.span,
            "erasedValue may only be used as the selector of an inline match");
      }
      return TypeInfo{SimpleTypeKind::Object, "Object"};
    }
    if (isSummonInline) {
      if (typeArguments.size() != 1) {
        diagnostics_.error(expression.span,
                           "summonInline requires exactly one type argument");
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }
      const TypeInfo requested =
          typeFromDeclaredName(expression.declaredType, &scope, &expression.span);
      if (inlineDefinitionDepth_ != 0) {
        return requested;
      }
      SymbolInfo request;
      request.kind = AstDeclarationKind::Def;
      request.name = std::string(support::StdNames::SummonInline);
      request.type = requested;
      request.parameters = {"evidence: " + requested.name};
      request.parameterTypes = {requested};
      request.contextualParameters = {true};
      std::vector<TypedContextArgument> arguments =
          resolveContextArguments(request, 0, scope, expression.span);
      recordContextApplication(expression.span, std::move(arguments));
      return requested;
    }
    if (isSummonAll) {
      if (typeArguments.size() != 1) {
        diagnostics_.error(expression.span,
                           "summonAll requires exactly one type argument");
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }
      const TypeInfo tuple =
          typeFromDeclaredName(expression.declaredType, &scope, &expression.span);
      if (tuple.typeParameter) {
        if (inlineDefinitionDepth_ != 0 &&
            tuple.runtimeName == support::StdNames::ScalaTuple) {
          return tuple;
        }
        diagnostics_.error(expression.span,
                           "summonAll requires a concrete tuple type");
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }
      const std::string constructor =
          tuple.typeConstructorName.empty() ? tuple.runtimeName
                                            : tuple.typeConstructorName;
      const bool emptyTuple =
          tuple.name == support::StdNames::ScalaEmptyTuple ||
          tuple.runtimeName == support::StdNames::ScalaEmptyTuple;
      const std::optional<std::size_t> tupleArity =
          tupleArityForConstructor(constructor);
      if (!emptyTuple &&
          (!tupleArity.has_value() || tuple.typeArguments.size() != *tupleArity)) {
        diagnostics_.error(expression.span, "summonAll requires a tuple type");
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }

      SymbolInfo request;
      request.kind = AstDeclarationKind::Def;
      request.name = std::string(support::StdNames::SummonAll);
      request.type = tuple;
      for (std::size_t index = 0; index < tuple.typeArguments.size(); ++index) {
        const TypeInfo& element = tuple.typeArguments[index];
        request.parameters.push_back("element" + std::to_string(index + 1) +
                                     ": " + element.name);
        request.parameterTypes.push_back(element);
        request.contextualParameters.push_back(true);
      }
      std::vector<TypedContextArgument> arguments =
          resolveContextArguments(request, 0, scope, expression.span);
      recordContextApplication(expression.span, std::move(arguments));
      return tuple;
    }
    if (isSizeOf) {
      if (typeArguments.size() != 1) {
        diagnostics_.error(expression.span,
                           "sizeof requires exactly one type argument");
        return TypeInfo{SimpleTypeKind::Int, "Int"};
      }
      const TypeInfo target =
          typeFromDeclaredName(expression.declaredType, &scope, &expression.span);
      if (hasCompileTimeSize(target.kind)) {
        return TypeInfo{SimpleTypeKind::Int, "Int"};
      }
      const SymbolInfo* targetSymbol =
          typeSymbolForDeclaredName(expression.declaredType, &scope);
      if (targetSymbol == nullptr || targetSymbol->kind != AstDeclarationKind::Class) {
        diagnostics_.error(expression.span,
                           "sizeof[T] requires a primitive or known concrete class: " +
                               expression.declaredType);
      }
      return TypeInfo{SimpleTypeKind::Int, "Int"};
    }
    if (isSummon) {
      if (typeArguments.size() != 1) {
        diagnostics_.error(expression.span, callee.text +
                                                " requires exactly one type argument");
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }
      const TypeInfo requested =
          typeFromDeclaredName(expression.declaredType, &scope, &expression.span);
      SymbolInfo request;
      request.kind = AstDeclarationKind::Def;
      request.name = callee.text;
      request.type = requested;
      request.parameters = {"evidence: " + requested.name};
      request.parameterTypes = {requested};
      request.contextualParameters = {true};
      std::vector<TypedContextArgument> arguments =
          resolveContextArguments(request, 0, scope, expression.span);
      recordContextApplication(expression.span, std::move(arguments));
      return requested;
    }
    if (isArrayEmpty) {
      if (typeArguments.size() != 1) {
        diagnostics_.error(expression.span,
                           "Array.empty requires exactly one type argument");
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }
      const TypeInfo elementType =
          typeFromDeclaredName(expression.declaredType, &scope, &expression.span);
      if (!isSupportedArrayElementType(elementType, scope, expression.span)) {
        diagnostics_.error(
            expression.span,
            "Array.empty type argument must be a supported scalar, reference, or "
            "nested array type in this subset");
      }
      return TypeInfo{SimpleTypeKind::Object, arrayTypeName(elementType)};
    }
    if (!isTypeTest && !isCast) {
      SymbolInfo target;
      bool foundTarget = false;
      std::optional<AstExpression> inlineReceiver;
      std::optional<TypeInfo> inlineReceiverType;
      if (callee.kind == AstExpressionKind::New) {
        if (const SymbolInfo* constructor =
                typeSymbolForDeclaredName(callee.text, &scope)) {
          target = *constructor;
          foundTarget = true;
        }
      } else if (callee.kind == AstExpressionKind::Identifier) {
        auto found = scope.find(callee.text);
        if (found != scope.end()) {
          target = found->second;
          foundTarget = true;
        }
      } else if (callee.kind == AstExpressionKind::Select &&
                 callee.children.size() == 1) {
        const TypeInfo receiver = inferExpressionType(callee.children.front(), scope);
        if (std::optional<SymbolInfo> member =
                resolvedMemberForReceiverType(receiver, callee.text)) {
          target = std::move(*member);
          foundTarget = true;
          inlineReceiver = callee.children.front();
          inlineReceiverType = receiver;
        }
      }

      if (!foundTarget || (target.kind != AstDeclarationKind::Def &&
                           target.kind != AstDeclarationKind::Class)) {
        diagnostics_.error(
            expression.span,
            "type application target must be a generic method or constructor");
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }
      if (target.typeParameters.empty()) {
        diagnostics_.error(expression.span,
                           target.name + " does not declare type parameters");
        return target.type;
      }
      SymbolInfo specialized =
          specializeTypeApplication(target, typeArguments, scope, expression.span);
      if (target.isInline && target.kind == AstDeclarationKind::Def &&
          target.parameterTypes.empty() && target.hasImplementation) {
        if (target.isInstanceMember && !inlineReceiver.has_value()) {
          if (auto thisSymbol = scope.find("this"); thisSymbol != scope.end()) {
            AstExpression implicitReceiver;
            implicitReceiver.kind = AstExpressionKind::This;
            implicitReceiver.span = callee.span;
            inlineReceiver = std::move(implicitReceiver);
            inlineReceiverType = thisSymbol->second.type;
          }
        }
        std::vector<TypeInfo> resolvedArguments;
        resolvedArguments.reserve(typeArguments.size());
        for (const std::string& argument : typeArguments) {
          resolvedArguments.push_back(
              typeFromDeclaredName(argument, &scope));
        }
        if (std::optional<TypeInfo> transparentResult = recordInlineApplication(
                expression, target, resolvedArguments, {}, {},
                inlineReceiver.has_value() ? &*inlineReceiver : nullptr,
                inlineReceiverType.has_value() ? &*inlineReceiverType : nullptr,
                scope, expectedType)) {
          return *transparentResult;
        }
      }
      return specialized.type;
    }

    if (typeArguments.size() != 1) {
      diagnostics_.error(expression.span,
                         std::string(isTypeTest ? "isInstanceOf" : "asInstanceOf") +
                             " requires exactly one type argument");
      return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
    }

    const SymbolInfo* erasedReceiver = nullptr;
    if (callee.children.front().kind == AstExpressionKind::Identifier) {
      const auto receiver = scope.find(callee.children.front().text);
      if (receiver != scope.end() &&
          receiver->second.isErasedCompileTimeValue) {
        erasedReceiver = &receiver->second;
      }
    }
    if (erasedReceiver != nullptr && isCast) {
      diagnostics_.error(
          callee.children.front().span,
          "erasedValue inline match patterns cannot bind a runtime value");
    }
    const TypeInfo receiverType =
        erasedReceiver == nullptr
            ? inferExpressionType(callee.children.front(), scope)
            : TypeInfo{SimpleTypeKind::Object, "Object"};
    if (receiverType.kind != SimpleTypeKind::Object &&
        receiverType.kind != SimpleTypeKind::Null &&
        receiverType.kind != SimpleTypeKind::Unknown) {
      diagnostics_.error(
          callee.children.front().span,
          std::string(isTypeTest ? "isInstanceOf[T]" : "asInstanceOf[T]") +
              " receiver must be a reference value");
    }

    const TypeInfo targetType =
        typeFromDeclaredName(expression.declaredType, &scope, &expression.span);
    const bool targetsBoxedPrimitive = isBoxablePrimitiveType(targetType.kind);
    const bool targetsUnionCast =
        !isTypeTest && targetType.compositeKind == CompositeTypeKind::Union;
    const SymbolInfo* target =
        targetsBoxedPrimitive
            ? nullptr
            : typeSymbolForDeclaredName(expression.declaredType, &scope);
    if (!targetsBoxedPrimitive && !targetsUnionCast &&
        !targetType.typeParameter &&
        (target == nullptr || (target->kind != AstDeclarationKind::Class &&
                               target->kind != AstDeclarationKind::Trait &&
                               target->kind != AstDeclarationKind::Object))) {
      diagnostics_.error(expression.span,
                         std::string(isTypeTest ? "isInstanceOf" : "asInstanceOf") +
                             " target must be a known class, trait, or object: " +
                             expression.declaredType);
    }
    if (isTypeTest) {
      return TypeInfo{SimpleTypeKind::Boolean, "Boolean"};
    }
    return targetType;
  }
  case AstExpressionKind::Assign:
    return inferAssignType(expression, scope);
  case AstExpressionKind::Call: {
    if (expression.children.empty()) {
      return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
    }
    if (isUninitializedExpression(expression.children.front(), scope)) {
      for (std::size_t index = 1; index < expression.children.size(); ++index) {
        (void)inferExpressionType(expression.children[index], scope);
      }
      diagnostics_.error(expression.span,
                         "uninitialized must be used without an argument list");
      return TypeInfo{SimpleTypeKind::Nothing, "Nothing"};
    }
    const AstExpression& callCallee = expression.children.front();
    const AstExpression* invokedPolymorphicFunction = nullptr;
    const AstExpression* polymorphicTypeApplication = nullptr;
    if (callCallee.kind == AstExpressionKind::PolymorphicFunction) {
      invokedPolymorphicFunction = &callCallee;
    } else if (callCallee.kind == AstExpressionKind::TypeApply &&
               callCallee.children.size() == 1 &&
               callCallee.children.front().kind ==
                   AstExpressionKind::PolymorphicFunction) {
      invokedPolymorphicFunction = &callCallee.children.front();
      polymorphicTypeApplication = &callCallee;
    }
    if (invokedPolymorphicFunction != nullptr) {
      if (polymorphicTypeApplication == nullptr ||
          polymorphicTypeApplication->typeArguments.size() != 1 ||
          expression.children.size() != 2) {
        for (std::size_t index = 1; index < expression.children.size(); ++index) {
          (void)inferExpressionType(expression.children[index], scope);
        }
        diagnostics_.error(
            expression.span,
            "polymorphic function invocation requires exactly one explicit type "
            "argument and one value argument");
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }

      const std::string& typeArgumentName =
          polymorphicTypeApplication->typeArguments.front();
      const TypeInfo typeArgument = typeFromDeclaredName(
          typeArgumentName, &scope, &polymorphicTypeApplication->span);
      if (typeArgument.kind == SimpleTypeKind::Unknown) {
        diagnostics_.error(polymorphicTypeApplication->span,
                           "unresolved polymorphic function type argument: " +
                               typeArgumentName);
      }
      const TypeInfo argument = inferExpressionType(
          expression.children[1], scope,
          typeArgument.kind == SimpleTypeKind::Unknown ? nullptr : &typeArgument);
      if (typeArgument.kind != SimpleTypeKind::Unknown &&
          argument.kind != SimpleTypeKind::Unknown &&
          !isAssignable(typeArgument, argument)) {
        diagnostics_.error(expression.children[1].span,
                           "argument type " + argument.name +
                               " does not conform to polymorphic function parameter " +
                               typeArgument.name);
      }

      std::optional<TypedPolymorphicFunctionApplication> literalApplication =
          typecheckPolymorphicFunctionLiteral(
              *invokedPolymorphicFunction, {typeArgument}, expression.span, scope,
              "polymorphic function literal must have the form "
              "[A] => (value: A) => body");
      if (!literalApplication.has_value() ||
          literalApplication->mappedResultTypes.size() != 1) {
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }
      const TypeInfo result = literalApplication->mappedResultTypes.front();
      literalApplication->isInvocation = true;
      recordPolymorphicFunctionApplication(std::move(*literalApplication));
      return result;
    }

    const AstExpression* polymorphicValueTarget = &callCallee;
    const AstExpression* polymorphicValueTypeApplication = nullptr;
    if (callCallee.kind == AstExpressionKind::TypeApply &&
        callCallee.children.size() == 1) {
      polymorphicValueTarget = &callCallee.children.front();
      polymorphicValueTypeApplication = &callCallee;
    }
    SymbolInfo polymorphicValue;
    bool foundPolymorphicValue = false;
    std::optional<AstExpression> polymorphicValueReceiver;
    std::optional<TypeInfo> polymorphicValueReceiverType;
    if (polymorphicValueTarget->kind == AstExpressionKind::Identifier) {
      auto target = scope.find(polymorphicValueTarget->text);
      if (target != scope.end() && target->second.polymorphicFunctionValue != nullptr) {
        polymorphicValue = target->second;
        foundPolymorphicValue = true;
      }
    } else if (polymorphicValueTarget->kind == AstExpressionKind::Select &&
               polymorphicValueTarget->children.size() == 1) {
      const bool knownPolymorphicMember = std::any_of(
          globalSymbols_.begin(), globalSymbols_.end(), [&](const auto& candidate) {
            return candidate.second.name == polymorphicValueTarget->text &&
                   candidate.second.polymorphicFunctionValue != nullptr;
          });
      if (knownPolymorphicMember) {
        const TypeInfo receiver =
            inferExpressionType(polymorphicValueTarget->children.front(), scope);
        if (std::optional<SymbolInfo> member =
                resolvedMemberForReceiverType(receiver, polymorphicValueTarget->text);
            member.has_value() && member->polymorphicFunctionValue != nullptr) {
          polymorphicValue = std::move(*member);
          foundPolymorphicValue = true;
          polymorphicValueReceiver = polymorphicValueTarget->children.front();
          polymorphicValueReceiverType = receiver;
        }
      }
    }
    if (foundPolymorphicValue) {
      if (polymorphicValueTypeApplication == nullptr ||
          polymorphicValueTypeApplication->typeArguments.size() != 1 ||
          expression.children.size() != 2) {
        for (std::size_t index = 1; index < expression.children.size(); ++index) {
          (void)inferExpressionType(expression.children[index], scope);
        }
        diagnostics_.error(
            expression.span,
            "stored polymorphic function invocation requires exactly one explicit "
            "type argument and one value argument");
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }

      const std::string& typeArgumentName =
          polymorphicValueTypeApplication->typeArguments.front();
      const TypeInfo typeArgument = typeFromDeclaredName(
          typeArgumentName, &scope, &polymorphicValueTypeApplication->span);
      if (typeArgument.kind == SimpleTypeKind::Unknown) {
        diagnostics_.error(polymorphicValueTypeApplication->span,
                           "unresolved polymorphic function type argument: " +
                               typeArgumentName);
      }
      const TypeInfo argument = inferExpressionType(
          expression.children[1], scope,
          typeArgument.kind == SimpleTypeKind::Unknown ? nullptr : &typeArgument);
      if (typeArgument.kind != SimpleTypeKind::Unknown &&
          argument.kind != SimpleTypeKind::Unknown &&
          !isAssignable(typeArgument, argument)) {
        diagnostics_.error(expression.children[1].span,
                           "argument type " + argument.name +
                               " does not conform to polymorphic function parameter " +
                               typeArgument.name);
      }

      TypedPolymorphicFunctionApplication application =
          *polymorphicValue.polymorphicFunctionValue;
      application.span = expression.span;
      application.typeArguments = {typeArgument};
      application.mappedResultTypes.clear();
      std::unordered_map<std::string, TypeInfo> substitutions;
      substitutions[application.parameterType.typeParameterSymbolName] = typeArgument;
      application.mappedResultTypes.push_back(
          substituteTypeParameters(application.resultType, substitutions));
      application.isInvocation = true;
      if (polymorphicValue.isInstanceMember) {
        if (!polymorphicValueReceiver.has_value()) {
          if (scope.contains("this")) {
            AstExpression implicitReceiver;
            implicitReceiver.kind = AstExpressionKind::This;
            implicitReceiver.span = polymorphicValueTarget->span;
            polymorphicValueReceiver = std::move(implicitReceiver);
            polymorphicValueReceiverType = scope.at("this").type;
          }
        }
        if (polymorphicValueReceiver.has_value() &&
            polymorphicValueReceiverType.has_value()) {
          application.hasReceiver = true;
          application.receiver = std::move(*polymorphicValueReceiver);
          application.receiverType = std::move(*polymorphicValueReceiverType);
        }
      }
      const TypeInfo result = application.mappedResultTypes.front();
      recordPolymorphicFunctionApplication(std::move(application));
      return result;
    }
    std::optional<TypeInfo> runtimePolymorphicFunctionType;
    if (polymorphicValueTarget->kind == AstExpressionKind::Identifier) {
      auto target = scope.find(polymorphicValueTarget->text);
      if (target != scope.end() && target->second.polymorphicFunctionValue == nullptr &&
          target->second.kind != AstDeclarationKind::Def &&
          target->second.type.polymorphicFunctionType) {
        runtimePolymorphicFunctionType = target->second.type;
      }
    } else if (polymorphicValueTypeApplication != nullptr &&
               polymorphicValueTarget->kind == AstExpressionKind::Select &&
               polymorphicValueTarget->children.size() == 1 &&
               (polymorphicValueTarget->children.front().kind !=
                    AstExpressionKind::Identifier ||
                scope.contains(polymorphicValueTarget->children.front().text))) {
      const TypeInfo receiver =
          inferExpressionType(polymorphicValueTarget->children.front(), scope);
      if (std::optional<SymbolInfo> member =
              resolvedMemberForReceiverType(receiver, polymorphicValueTarget->text);
          member.has_value() && member->kind != AstDeclarationKind::Def &&
          member->polymorphicFunctionValue == nullptr &&
          member->type.polymorphicFunctionType) {
        runtimePolymorphicFunctionType = member->type;
      }
    } else if (polymorphicValueTypeApplication != nullptr &&
               polymorphicValueTarget->kind == AstExpressionKind::Call) {
      const TypeInfo targetType = inferExpressionType(*polymorphicValueTarget, scope);
      if (targetType.polymorphicFunctionType) {
        runtimePolymorphicFunctionType = targetType;
      }
    }
    if (runtimePolymorphicFunctionType.has_value()) {
      if (polymorphicValueTypeApplication == nullptr ||
          polymorphicValueTypeApplication->typeArguments.size() != 1 ||
          expression.children.size() != 2) {
        for (std::size_t index = 1; index < expression.children.size(); ++index) {
          (void)inferExpressionType(expression.children[index], scope);
        }
        diagnostics_.error(
            expression.span,
            "runtime polymorphic function invocation requires exactly one "
            "explicit type argument and one value argument");
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }

      const std::string& typeArgumentName =
          polymorphicValueTypeApplication->typeArguments.front();
      const TypeInfo typeArgument = typeFromDeclaredName(
          typeArgumentName, &scope, &polymorphicValueTypeApplication->span);
      if (typeArgument.kind == SimpleTypeKind::Unknown) {
        diagnostics_.error(polymorphicValueTypeApplication->span,
                           "unresolved polymorphic function type argument: " +
                               typeArgumentName);
      }

      const TypeInfo& runtimeType = *runtimePolymorphicFunctionType;
      if (runtimeType.typeArguments.size() != 2 ||
          !runtimeType.typeArguments.front().typeParameter) {
        diagnostics_.error(expression.span,
                           "malformed runtime polymorphic function type");
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }
      std::unordered_map<std::string, TypeInfo> substitutions;
      substitutions[runtimeType.typeArguments.front().typeParameterSymbolName] =
          typeArgument;
      const TypeInfo specializedParameter =
          substituteTypeParameters(runtimeType.typeArguments.front(), substitutions);
      const TypeInfo specializedResult =
          substituteTypeParameters(runtimeType.typeArguments[1], substitutions);
      const TypeInfo argument = inferExpressionType(
          expression.children[1], scope,
          typeArgument.kind == SimpleTypeKind::Unknown ? nullptr
                                                       : &specializedParameter);
      if (typeArgument.kind != SimpleTypeKind::Unknown &&
          argument.kind != SimpleTypeKind::Unknown &&
          !isAssignable(specializedParameter, argument)) {
        diagnostics_.error(expression.children[1].span,
                           "argument type " + argument.name +
                               " does not conform to polymorphic function parameter " +
                               specializedParameter.name);
      }

      TypedPolymorphicFunctionApplication application;
      application.span = expression.span;
      application.parameterType = runtimeType.typeArguments.front();
      application.resultType = runtimeType.typeArguments[1];
      application.typeArguments = {typeArgument};
      application.mappedResultTypes = {specializedResult};
      application.isRuntimeInvocation = true;
      application.runtimeFunction = *polymorphicValueTarget;
      application.runtimeFunctionType = runtimeType;
      recordPolymorphicFunctionApplication(std::move(application));
      return specializedResult;
    }
    if (callCallee.kind == AstExpressionKind::Select &&
        callCallee.children.size() == 1 &&
        callCallee.text == support::StdNames::TupleMap) {
      const TypeInfo receiver =
          inferExpressionType(callCallee.children.front(), scope);
      const bool emptyTuple =
          receiver.name == support::StdNames::ScalaEmptyTuple ||
          receiver.runtimeName == support::StdNames::ScalaEmptyTuple;
      const std::string receiverConstructor =
          receiver.typeConstructorName.empty() ? receiver.runtimeName
                                               : receiver.typeConstructorName;
      const std::optional<std::size_t> arity =
          tupleArityForConstructor(receiverConstructor);
      const bool abstractTuple =
          receiver.name == support::StdNames::ScalaTuple ||
          receiver.runtimeName == support::StdNames::ScalaTuple;
      if (emptyTuple || arity.has_value() || abstractTuple) {
        if (expression.children.size() != 2) {
          for (std::size_t index = 1; index < expression.children.size(); ++index) {
            if (expression.children[index].kind !=
                AstExpressionKind::PolymorphicFunction) {
              (void)inferExpressionType(expression.children[index], scope);
            }
          }
          diagnostics_.error(
              expression.span,
              "tuple map requires exactly one polymorphic function");
          return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
        }
        if (abstractTuple ||
            (!emptyTuple &&
             (!arity.has_value() || receiver.typeArguments.size() != *arity))) {
          diagnostics_.error(expression.span,
                             "map requires a concrete tuple receiver");
          return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
        }

        const AstExpression& function = expression.children[1];
        std::vector<TypeInfo> mappedElements;
        if (function.kind == AstExpressionKind::PolymorphicFunction) {
          std::optional<TypedPolymorphicFunctionApplication> literalApplication =
              typecheckPolymorphicFunctionLiteral(
                  function, receiver.typeArguments, expression.span, scope,
                  "tuple map polymorphic function literal must have the form "
                  "[A] => (value: A) => body");
          if (!literalApplication.has_value()) {
            return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
          }
          mappedElements = literalApplication->mappedResultTypes;
          literalApplication->isTupleMap = true;
          recordPolymorphicFunctionApplication(std::move(*literalApplication));
        } else {
          SymbolInfo storedFunction;
          bool foundStoredFunction = false;
          std::optional<AstExpression> storedFunctionReceiver;
          std::optional<TypeInfo> storedFunctionReceiverType;
          if (function.kind == AstExpressionKind::Identifier) {
            auto target = scope.find(function.text);
            if (target != scope.end() &&
                target->second.polymorphicFunctionValue != nullptr) {
              storedFunction = target->second;
              foundStoredFunction = true;
            }
          } else if (function.kind == AstExpressionKind::Select &&
                     function.children.size() == 1) {
            const bool knownPolymorphicMember = std::any_of(
                globalSymbols_.begin(), globalSymbols_.end(),
                [&](const auto& candidate) {
                  return candidate.second.name == function.text &&
                         candidate.second.polymorphicFunctionValue != nullptr;
                });
            if (knownPolymorphicMember) {
              const TypeInfo selectedReceiver =
                  inferExpressionType(function.children.front(), scope);
              if (std::optional<SymbolInfo> member =
                      resolvedMemberForReceiverType(selectedReceiver, function.text);
                  member.has_value() && member->polymorphicFunctionValue != nullptr) {
                storedFunction = std::move(*member);
                foundStoredFunction = true;
                storedFunctionReceiver = function.children.front();
                storedFunctionReceiverType = selectedReceiver;
              }
            }
          }

          if (foundStoredFunction) {
            TypedPolymorphicFunctionApplication application =
                *storedFunction.polymorphicFunctionValue;
            application.span = expression.span;
            application.typeArguments = receiver.typeArguments;
            application.mappedResultTypes.clear();
            std::unordered_map<std::string, TypeInfo> substitutions;
            for (const TypeInfo& element : receiver.typeArguments) {
              substitutions[application.parameterType.typeParameterSymbolName] =
                  element;
              application.mappedResultTypes.push_back(
                  substituteTypeParameters(application.resultType, substitutions));
            }
            application.isInvocation = false;
            application.isTupleMap = true;
            if (storedFunction.isInstanceMember) {
              if (!storedFunctionReceiver.has_value() && scope.contains("this")) {
                AstExpression implicitReceiver;
                implicitReceiver.kind = AstExpressionKind::This;
                implicitReceiver.span = function.span;
                storedFunctionReceiver = std::move(implicitReceiver);
                storedFunctionReceiverType = scope.at("this").type;
              }
              if (storedFunctionReceiver.has_value() &&
                  storedFunctionReceiverType.has_value()) {
                application.hasReceiver = true;
                application.receiver = std::move(*storedFunctionReceiver);
                application.receiverType = std::move(*storedFunctionReceiverType);
              }
            }
            mappedElements = application.mappedResultTypes;
            recordPolymorphicFunctionApplication(std::move(application));
          } else {
            const TypeInfo argumentType = inferExpressionType(function, scope);
            if (argumentType.polymorphicFunctionType) {
              if (argumentType.typeArguments.size() != 2 ||
                  !argumentType.typeArguments.front().typeParameter) {
                diagnostics_.error(function.span,
                                   "malformed runtime polymorphic function type");
                return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
              }
              TypedPolymorphicFunctionApplication application;
              application.span = expression.span;
              application.parameterType = argumentType.typeArguments.front();
              application.resultType = argumentType.typeArguments[1];
              application.typeArguments = receiver.typeArguments;
              application.isRuntimeTupleMap = true;
              application.runtimeFunction = function;
              application.runtimeFunctionType = argumentType;
              std::unordered_map<std::string, TypeInfo> substitutions;
              for (const TypeInfo& element : receiver.typeArguments) {
                substitutions[application.parameterType.typeParameterSymbolName] =
                    element;
                application.mappedResultTypes.push_back(
                    substituteTypeParameters(application.resultType, substitutions));
              }
              mappedElements = application.mappedResultTypes;
              recordPolymorphicFunctionApplication(std::move(application));
            } else {
              const SymbolInfo* application = knownMemberForReceiverType(
                  argumentType, std::string(support::StdNames::TupleApply));
              const SymbolInfo* polyFunction = typeSymbolForDeclaredName(
                  std::string(support::StdNames::ScalaPolyFunction), &scope);
              const bool polymorphicUnaryApplication =
                  polyFunction != nullptr &&
                  isAssignable(polyFunction->type, argumentType) &&
                  application != nullptr &&
                  application->kind == AstDeclarationKind::Def &&
                  application->typeParameters.size() == 1 &&
                  application->parameterTypes.size() == 1 &&
                  application->parameterClauseSizes.size() == 1 &&
                  application->parameterClauseSizes.front() == 1 &&
                  application->contextualParameters.size() == 1 &&
                  !application->contextualParameters.front() &&
                  application->typeParameters.front().upperBound.kind ==
                      SimpleTypeKind::Object &&
                  application->typeParameters.front().upperBound.name == "Object" &&
                  application->typeParameters.front().lowerBound.kind ==
                      SimpleTypeKind::Nothing &&
                  application->parameterTypes.front().typeParameter &&
                  application->parameterTypes.front().typeParameterSymbolName ==
                      application->typeParameters.front().symbolName;
              if (!polymorphicUnaryApplication) {
                diagnostics_.error(function.span,
                                   "tuple map function must define apply[A](value: A)");
                return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
              }
              mappedElements.reserve(receiver.typeArguments.size());
              for (const TypeInfo& element : receiver.typeArguments) {
                mappedElements.push_back(
                    specializeResolvedTypeApplication(*application, {element},
                                                      expression.span, true)
                        .type);
              }
            }
          }
        }

        if (emptyTuple) {
          auto empty = globalSymbols_.find(
              std::string(support::StdNames::ScalaEmptyTuple));
          if (empty == globalSymbols_.end()) {
            diagnostics_.error(
                expression.span,
                "unresolved tuple map result: scala.EmptyTuple");
            return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
          }
          return empty->second.type;
        }

        const std::string resultConstructor =
            std::string(support::StdNames::ScalaTuple) +
            std::to_string(mappedElements.size());
        auto result = globalSymbols_.find(resultConstructor);
        if (result == globalSymbols_.end()) {
          diagnostics_.error(expression.span,
                             "unresolved tuple map constructor: " +
                                 resultConstructor);
          return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
        }
        return specializeResolvedTypeApplication(
                   result->second, mappedElements, expression.span, true)
            .type;
      }
    }
    const AstExpression* tupleApplyReceiver = nullptr;
    std::optional<TypeInfo> tupleApplyReceiverType;
    if (callCallee.kind == AstExpressionKind::Select &&
        callCallee.children.size() == 1 &&
        callCallee.text == support::StdNames::TupleApply) {
      tupleApplyReceiver = &callCallee.children.front();
    } else {
      bool callableDeclaration = false;
      if (callCallee.kind == AstExpressionKind::Identifier) {
        if (auto symbol = scope.find(callCallee.text); symbol != scope.end()) {
          callableDeclaration =
              (symbol->second.kind == AstDeclarationKind::Def &&
               (!symbol->second.typeParameters.empty() ||
                !symbol->second.parameterTypes.empty() ||
                !symbol->second.parameterClauseSizes.empty())) ||
              symbol->second.kind == AstDeclarationKind::Class;
        } else {
          callableDeclaration = true;
        }
      } else if (callCallee.kind == AstExpressionKind::Select &&
                 callCallee.children.size() == 1) {
        callableDeclaration = true;
      } else if (callCallee.kind == AstExpressionKind::Call ||
                 callCallee.kind == AstExpressionKind::New ||
                 callCallee.kind == AstExpressionKind::TypeApply) {
        callableDeclaration = true;
      }
      if (!callableDeclaration) {
        tupleApplyReceiver = &callCallee;
      }
    }
    if (tupleApplyReceiver != nullptr) {
      tupleApplyReceiverType =
          inferExpressionType(*tupleApplyReceiver, scope);
      const bool emptyTuple =
          tupleApplyReceiverType->name == support::StdNames::ScalaEmptyTuple ||
          tupleApplyReceiverType->runtimeName ==
              support::StdNames::ScalaEmptyTuple;
      const std::string constructor =
          tupleApplyReceiverType->typeConstructorName.empty()
              ? tupleApplyReceiverType->runtimeName
              : tupleApplyReceiverType->typeConstructorName;
      const std::optional<std::size_t> arity =
          tupleArityForConstructor(constructor);
      const bool abstractTuple =
          tupleApplyReceiverType->name == support::StdNames::ScalaTuple ||
          tupleApplyReceiverType->runtimeName == support::StdNames::ScalaTuple;
      if (emptyTuple || arity.has_value() || abstractTuple) {
        std::vector<TypeInfo> argumentTypes;
        argumentTypes.reserve(expression.children.size() - 1);
        for (std::size_t index = 1; index < expression.children.size(); ++index) {
          argumentTypes.push_back(
              inferExpressionType(expression.children[index], scope));
        }
        if (expression.children.size() != 2) {
          diagnostics_.error(expression.span,
                             "tuple apply requires exactly one Int index");
          return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
        }
        if (abstractTuple ||
            (!emptyTuple &&
             (!arity.has_value() ||
              tupleApplyReceiverType->typeArguments.size() != *arity))) {
          diagnostics_.error(expression.span,
                             "apply requires a concrete tuple receiver");
          return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
        }
        const TypeInfo& indexType = argumentTypes.front();
        if (indexType.kind != SimpleTypeKind::Int &&
            indexType.kind != SimpleTypeKind::Unknown) {
          diagnostics_.error(expression.children[1].span,
                             "tuple apply index must have type Int");
          return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
        }
        const std::optional<std::int64_t> index =
            constantIntegerValue(expression.children[1], scope);
        if (!index.has_value()) {
          diagnostics_.error(
              expression.children[1].span,
              "tuple apply index must be a compile-time constant Int in this "
              "subset");
          return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
        }
        const std::size_t tupleSize =
            emptyTuple ? 0 : tupleApplyReceiverType->typeArguments.size();
        if (*index < 0 || static_cast<std::uint64_t>(*index) >= tupleSize) {
          diagnostics_.error(
              expression.children[1].span,
              "tuple apply index " + std::to_string(*index) +
                  " is outside tuple bounds [0, " +
                  std::to_string(tupleSize) + ")");
          return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
        }

        const std::string indexLiteral = std::to_string(*index);
        for (auto info = expressionTypes_.rbegin();
             info != expressionTypes_.rend(); ++info) {
          if (info->span.source == expression.children[1].span.source &&
              info->span.start == expression.children[1].span.start &&
              info->span.length == expression.children[1].span.length) {
            info->type.singletonLiteral = indexLiteral;
            break;
          }
        }
        return tupleApplyReceiverType->typeArguments[
            static_cast<std::size_t>(*index)];
      }
    }
    if (isCodeOfCallee(expression.children.front(), scope)) {
      TypeInfo result{SimpleTypeKind::String, "String"};
      if (expression.children.size() != 2) {
        diagnostics_.error(expression.span,
                           "codeOf requires exactly one argument");
        for (std::size_t index = 1; index < expression.children.size(); ++index) {
          (void)inferExpressionType(expression.children[index], scope);
        }
        return result;
      }

      const AstExpression& argument = expression.children.back();
      (void)inferExpressionType(argument, scope);
      if (inlineDefinitionDepth_ == 0) {
        if (const std::optional<std::string> code =
                sourceCodeForExpression(argument, scope)) {
          result.singletonLiteral = stringSingletonType(*code);
          result.stringSingleton = true;
        } else {
          diagnostics_.error(argument.span,
                             "codeOf could not recover argument source code");
        }
      }
      return result;
    }
    if (isRequireConstCallee(expression.children.front(), scope)) {
      const TypeInfo unit{SimpleTypeKind::Unit, "Unit"};
      if (expression.children.size() != 2) {
        diagnostics_.error(expression.span,
                           "requireConst requires exactly one argument");
        for (std::size_t index = 1; index < expression.children.size(); ++index) {
          (void)inferExpressionType(expression.children[index], scope);
        }
        return unit;
      }

      const AstExpression& argument = expression.children.back();
      const TypeInfo argumentType = inferExpressionType(argument, scope);
      const bool supportedType =
          argumentType.kind == SimpleTypeKind::Boolean ||
          argumentType.kind == SimpleTypeKind::Byte ||
          argumentType.kind == SimpleTypeKind::Short ||
          argumentType.kind == SimpleTypeKind::Int ||
          argumentType.kind == SimpleTypeKind::Long ||
          argumentType.kind == SimpleTypeKind::Float ||
          argumentType.kind == SimpleTypeKind::Double ||
          argumentType.kind == SimpleTypeKind::Char ||
          argumentType.kind == SimpleTypeKind::String;
      if (argumentType.kind != SimpleTypeKind::Unknown && !supportedType) {
        diagnostics_.error(
            argument.span,
            "requireConst argument must have type Boolean, Byte, Short, Int, "
            "Long, Float, Double, Char, or String");
        return unit;
      }

      if (inlineDefinitionDepth_ == 0 && supportedType) {
        bool isConstant = false;
        switch (argumentType.kind) {
        case SimpleTypeKind::Boolean:
          isConstant = constantBooleanValue(argument, scope).has_value();
          break;
        case SimpleTypeKind::Byte:
        case SimpleTypeKind::Short:
        case SimpleTypeKind::Int:
        case SimpleTypeKind::Long:
          isConstant = constantIntegerValue(argument, scope).has_value();
          break;
        case SimpleTypeKind::Float:
        case SimpleTypeKind::Double:
          isConstant = constantFloatingValue(argument, scope).has_value();
          break;
        case SimpleTypeKind::Char:
          isConstant = constantCharValue(argument, scope).has_value();
          break;
        case SimpleTypeKind::String:
          isConstant = constantStringValue(argument, scope).has_value();
          break;
        default:
          break;
        }
        if (!isConstant) {
          diagnostics_.error(
              argument.span,
              "requireConst requires a compile-time constant value");
        }
      }
      return unit;
    }
    if (isCompiletimeErrorCallee(expression.children.front(), scope)) {
      if (expression.children.size() != 2) {
        diagnostics_.error(
            expression.span,
            "compiletime.error requires exactly one String argument");
        for (std::size_t index = 1; index < expression.children.size(); ++index) {
          (void)inferExpressionType(expression.children[index], scope);
        }
        return TypeInfo{SimpleTypeKind::Nothing, "Nothing"};
      }

      const AstExpression& messageExpression = expression.children.back();
      const TypeInfo messageType = inferExpressionType(messageExpression, scope);
      if (messageType.kind != SimpleTypeKind::String &&
          messageType.kind != SimpleTypeKind::Unknown) {
        diagnostics_.error(messageExpression.span,
                           "compiletime.error message must have type String");
        return TypeInfo{SimpleTypeKind::Nothing, "Nothing"};
      }
      if (inlineDefinitionDepth_ == 0 &&
          messageType.kind != SimpleTypeKind::Unknown) {
        if (const std::optional<std::string> message =
                constantStringValue(messageExpression, scope)) {
          diagnostics_.error(expression.span, *message);
        } else {
          diagnostics_.error(
              messageExpression.span,
              "compiletime.error requires a compile-time constant String message");
        }
      }
      return TypeInfo{SimpleTypeKind::Nothing, "Nothing"};
    }
    std::vector<const AstExpression*> callClauses;
    const AstExpression* rootCallee = &expression;
    while (rootCallee->kind == AstExpressionKind::Call &&
           !rootCallee->children.empty()) {
      callClauses.push_back(rootCallee);
      rootCallee = &rootCallee->children.front();
    }
    const AstExpression* targetExpression = rootCallee;
    if (targetExpression->kind == AstExpressionKind::TypeApply &&
        targetExpression->children.size() == 1) {
      targetExpression = &targetExpression->children.front();
    }

    const SymbolInfo* multiClauseTarget = nullptr;
    if (targetExpression->kind == AstExpressionKind::Identifier) {
      if (auto found = scope.find(targetExpression->text); found != scope.end()) {
        multiClauseTarget = &found->second;
      }
    } else if (targetExpression->kind == AstExpressionKind::Select &&
               targetExpression->children.size() == 1) {
      const AstExpression& receiverExpression =
          targetExpression->children.front();
      if (receiverExpression.kind == AstExpressionKind::Identifier) {
        if (auto found = scope.find(receiverExpression.text);
            found != scope.end()) {
          multiClauseTarget =
              knownMemberForReceiverType(found->second.type,
                                         targetExpression->text);
        }
      } else {
        const TypeInfo receiver =
            inferExpressionType(receiverExpression, scope);
        multiClauseTarget =
            knownMemberForReceiverType(receiver, targetExpression->text);
      }
    } else if (targetExpression->kind == AstExpressionKind::New) {
      multiClauseTarget = qualifiedPathSymbol(targetExpression->text, &scope);
    }

    std::vector<std::size_t> sourceClauseSizes;
    sourceClauseSizes.reserve(callClauses.size());
    for (auto clause = callClauses.rbegin(); clause != callClauses.rend();
         ++clause) {
      sourceClauseSizes.push_back((*clause)->children.size() - 1);
    }
    const auto matchesDeclaredClauses = [&] {
      if (multiClauseTarget == nullptr ||
          multiClauseTarget->parameterClauseSizes.empty() ||
          multiClauseTarget->parameterClauseSizes.size() !=
              multiClauseTarget->contextualParameterClauses.size()) {
        return true;
      }

      std::vector<std::size_t> ordinaryClauseSizes;
      std::size_t contextualArgumentCount = 0;
      for (std::size_t index = 0;
           index < multiClauseTarget->parameterClauseSizes.size(); ++index) {
        if (multiClauseTarget->contextualParameterClauses[index]) {
          contextualArgumentCount +=
              multiClauseTarget->parameterClauseSizes[index];
        } else {
          ordinaryClauseSizes.push_back(
              multiClauseTarget->parameterClauseSizes[index]);
        }
      }

      if (sourceClauseSizes == ordinaryClauseSizes ||
          sourceClauseSizes == multiClauseTarget->parameterClauseSizes) {
        return true;
      }
      if (contextualArgumentCount != 0 && !ordinaryClauseSizes.empty() &&
          sourceClauseSizes.size() == ordinaryClauseSizes.size()) {
        for (std::size_t index = 0;
             index + 1 < ordinaryClauseSizes.size(); ++index) {
          if (sourceClauseSizes[index] != ordinaryClauseSizes[index]) {
            return false;
          }
        }
        return sourceClauseSizes.back() ==
               ordinaryClauseSizes.back() + contextualArgumentCount;
      }
      return false;
    };

    if (!expression.isFlattenedCall && multiClauseTarget != nullptr &&
        multiClauseTarget->isInline &&
        !matchesDeclaredClauses()) {
      const auto clauseShape = [](const std::vector<std::size_t>& sizes) {
        std::string result;
        for (std::size_t size : sizes) {
          result += "(" + std::to_string(size) + ")";
        }
        return result;
      };
      diagnostics_.error(
          expression.span,
          "inline call to " + multiClauseTarget->name +
              " must preserve its declared ordinary argument clauses; found " +
              clauseShape(sourceClauseSizes) + " but expected " +
              clauseShape(multiClauseTarget->parameterClauseSizes));
      return multiClauseTarget->type;
    }

    if (callClauses.size() > 1 && multiClauseTarget != nullptr) {
      const auto firstContext =
          std::find(multiClauseTarget->contextualParameters.begin(),
                    multiClauseTarget->contextualParameters.end(), true);
      const std::size_t firstContextIndex = static_cast<std::size_t>(
          std::distance(multiClauseTarget->contextualParameters.begin(),
                        firstContext));
      const bool hasTrailingContextClause =
          firstContext != multiClauseTarget->contextualParameters.end() &&
          std::all_of(firstContext,
                      multiClauseTarget->contextualParameters.end(),
                      [](bool contextual) { return contextual; });

      std::size_t sourceArgumentCount = 0;
      for (const AstExpression* clause : callClauses) {
        sourceArgumentCount += clause->children.size() - 1;
      }
      const std::size_t parameterCount = multiClauseTarget->parameterTypes.size();
      const bool completeInlineApplication =
          multiClauseTarget->isInline &&
          (sourceArgumentCount == parameterCount ||
           (hasTrailingContextClause &&
            sourceArgumentCount == firstContextIndex));
      const AstExpression& firstClause = *callClauses.back();
      const bool explicitTrailingContextApplication =
          callClauses.size() == 2 && hasTrailingContextClause &&
          firstClause.children.size() - 1 == firstContextIndex &&
          expression.children.size() - 1 ==
              parameterCount - firstContextIndex;
      if (completeInlineApplication ||
          explicitTrailingContextApplication) {
        AstExpression flattened;
        flattened.kind = AstExpressionKind::Call;
        flattened.span = expression.span;
        flattened.isFlattenedCall = true;
        flattened.children.push_back(*rootCallee);
        for (auto clause = callClauses.rbegin(); clause != callClauses.rend();
             ++clause) {
          flattened.children.insert(flattened.children.end(),
                                    (*clause)->children.begin() + 1,
                                    (*clause)->children.end());
        }
        return inferExpressionType(flattened, scope, expectedType);
      }
    }
    if (isZoneScopedCall(expression)) {
      if (expression.children.size() != 2) {
        diagnostics_.error(expression.span,
                           "Zone.scoped requires exactly one block argument");
        for (std::size_t i = 1; i < expression.children.size(); ++i) {
          (void)inferExpressionType(expression.children[i], scope);
        }
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }
      const AstExpression& body = expression.children[1];
      if (body.kind != AstExpressionKind::Block) {
        diagnostics_.error(body.span, "Zone.scoped requires a block argument");
      }
      ++zoneInferenceDepth_;
      TypeInfo result = inferExpressionType(body, scope);
      --zoneInferenceDepth_;
      zoneBodiesToAnalyze_.push_back(body);
      if (canEscapeZone(result.kind)) {
        diagnostics_.error(
            body.span,
            "Zone.scoped result must be Unit or a primitive value; references "
            "cannot escape the zone");
      }
      return result;
    }
    if (isZoneAllocBytesCall(expression)) {
      if (zoneInferenceDepth_ == 0) {
        diagnostics_.error(expression.span,
                           "Zone.allocBytes is only valid inside a Zone.scoped body");
      }
      if (expression.children.size() != 2) {
        diagnostics_.error(expression.span,
                           "Zone.allocBytes requires exactly one Int length");
        for (std::size_t i = 1; i < expression.children.size(); ++i) {
          (void)inferExpressionType(expression.children[i], scope);
        }
        return TypeInfo{SimpleTypeKind::Object, "Array [ Byte ]"};
      }
      const TypeInfo length = inferExpressionType(expression.children[1], scope);
      if (length.kind != SimpleTypeKind::Int &&
          length.kind != SimpleTypeKind::Unknown) {
        diagnostics_.error(expression.children[1].span,
                           "Zone.allocBytes length must have type Int");
      }
      return TypeInfo{SimpleTypeKind::Object, "Array [ Byte ]"};
    }
    if (isByteBufferWrapCall(expression)) {
      if (expression.children.size() != 2) {
        diagnostics_.error(expression.span,
                           "ByteBuffer.wrap requires exactly one Array[Byte]");
        for (std::size_t i = 1; i < expression.children.size(); ++i) {
          (void)inferExpressionType(expression.children[i], scope);
        }
        return TypeInfo{SimpleTypeKind::Object,
                        std::string(support::StdNames::JavaNioByteBuffer)};
      }
      const TypeInfo bytes = inferExpressionType(expression.children[1], scope);
      if (bytes.kind != SimpleTypeKind::Unknown &&
          (bytes.kind != SimpleTypeKind::Object ||
           arrayElementTypeName(bytes.name) != "Byte")) {
        diagnostics_.error(expression.children[1].span,
                           "ByteBuffer.wrap storage must have type Array[Byte]");
      }
      return TypeInfo{SimpleTypeKind::Object,
                      std::string(support::StdNames::JavaNioByteBuffer)};
    }
    {
      const AstExpression& selected = expression.children.front();
      if (selected.kind == AstExpressionKind::Select && selected.children.size() == 1 &&
          isByteBufferOperationName(selected.text)) {
        const TypeInfo receiver = inferExpressionType(selected.children.front(), scope);
        if (isByteBufferType(receiver)) {
          const std::size_t argumentCount = expression.children.size() - 1;
          const bool positionOrLimit =
              selected.text == support::StdNames::ByteBufferPosition ||
              selected.text == support::StdNames::ByteBufferLimit;
          const bool get = selected.text == support::StdNames::ByteBufferGet;
          const bool put = selected.text == support::StdNames::ByteBufferPut;
          const bool getShort = selected.text == support::StdNames::ByteBufferGetShort;
          const bool putShort = selected.text == support::StdNames::ByteBufferPutShort;
          const bool validArity =
              (positionOrLimit && argumentCount <= 1) || (get && argumentCount <= 1) ||
              (put && (argumentCount == 1 || argumentCount == 2)) ||
              (getShort && argumentCount <= 1) ||
              (putShort && (argumentCount == 1 || argumentCount == 2)) ||
              (!positionOrLimit && !get && !put && !getShort && !putShort &&
               argumentCount == 0);
          if (!validArity) {
            std::string argumentContract;
            if (positionOrLimit || get) {
              argumentContract = " accepts zero or one Int argument";
            } else if (put) {
              argumentContract =
                  " requires one Byte argument or an Int index and Byte value";
            } else if (getShort) {
              argumentContract = " accepts zero or one Int argument";
            } else if (putShort) {
              argumentContract =
                  " requires one Short argument or an Int index and Short value";
            } else {
              argumentContract = " does not accept arguments";
            }
            diagnostics_.error(expression.span, selected.text + argumentContract);
            for (std::size_t i = 1; i < expression.children.size(); ++i) {
              (void)inferExpressionType(expression.children[i], scope);
            }
          } else if (positionOrLimit && argumentCount == 1) {
            const TypeInfo value = inferExpressionType(expression.children[1], scope);
            if (value.kind != SimpleTypeKind::Int &&
                value.kind != SimpleTypeKind::Unknown) {
              diagnostics_.error(expression.children[1].span,
                                 selected.text + " value must have type Int");
            }
          } else if (get && argumentCount == 1) {
            const TypeInfo index = inferExpressionType(expression.children[1], scope);
            if (index.kind != SimpleTypeKind::Int &&
                index.kind != SimpleTypeKind::Unknown) {
              diagnostics_.error(expression.children[1].span,
                                 "get index must have type Int");
            }
          } else if (put) {
            const std::size_t valueIndex = argumentCount == 1 ? 1 : 2;
            if (argumentCount == 2) {
              const TypeInfo index = inferExpressionType(expression.children[1], scope);
              if (index.kind != SimpleTypeKind::Int &&
                  index.kind != SimpleTypeKind::Unknown) {
                diagnostics_.error(expression.children[1].span,
                                   "put index must have type Int");
              }
            }
            const TypeInfo value =
                inferExpressionType(expression.children[valueIndex], scope);
            if (value.kind != SimpleTypeKind::Byte &&
                value.kind != SimpleTypeKind::Unknown) {
              diagnostics_.error(expression.children[valueIndex].span,
                                 "put value must have type Byte");
            }
          } else if (getShort && argumentCount == 1) {
            const TypeInfo index = inferExpressionType(expression.children[1], scope);
            if (index.kind != SimpleTypeKind::Int &&
                index.kind != SimpleTypeKind::Unknown) {
              diagnostics_.error(expression.children[1].span,
                                 "getShort index must have type Int");
            }
          } else if (putShort) {
            const std::size_t valueIndex = argumentCount == 1 ? 1 : 2;
            if (argumentCount == 2) {
              const TypeInfo index = inferExpressionType(expression.children[1], scope);
              if (index.kind != SimpleTypeKind::Int &&
                  index.kind != SimpleTypeKind::Unknown) {
                diagnostics_.error(expression.children[1].span,
                                   "putShort index must have type Int");
              }
            }
            const TypeInfo value =
                inferExpressionType(expression.children[valueIndex], scope);
            if (value.kind != SimpleTypeKind::Short &&
                value.kind != SimpleTypeKind::Unknown) {
              diagnostics_.error(expression.children[valueIndex].span,
                                 "putShort value must have type Short");
            }
          }

          const bool returnsInt =
              selected.text == support::StdNames::ByteBufferCapacity ||
              selected.text == support::StdNames::ByteBufferRemaining ||
              ((selected.text == support::StdNames::ByteBufferPosition ||
                selected.text == support::StdNames::ByteBufferLimit) &&
               argumentCount == 0);
          if (returnsInt) {
            return TypeInfo{SimpleTypeKind::Int, "Int"};
          }
          if (selected.text == support::StdNames::ByteBufferHasRemaining) {
            return TypeInfo{SimpleTypeKind::Boolean, "Boolean"};
          }
          if (selected.text == support::StdNames::ByteBufferGet) {
            return TypeInfo{SimpleTypeKind::Byte, "Byte"};
          }
          if (selected.text == support::StdNames::ByteBufferGetShort) {
            return TypeInfo{SimpleTypeKind::Short, "Short"};
          }
          return TypeInfo{SimpleTypeKind::Object,
                          std::string(support::StdNames::JavaNioByteBuffer)};
        }
      }
    }
    if (const std::string_view operation = nativeBytesOperation(expression);
        !operation.empty()) {
      const bool isGet = operation == support::StdNames::NativeBytesGetShortBe ||
                         operation == support::StdNames::NativeBytesGetShortLe;
      const std::size_t expectedChildren = isGet ? 3 : 4;
      if (expression.children.size() != expectedChildren) {
        diagnostics_.error(
            expression.span,
            std::string(operation) +
                (isGet ? " requires an Array[Byte] and Int index"
                       : " requires an Array[Byte], Int index, and Short value"));
        for (std::size_t i = 1; i < expression.children.size(); ++i) {
          (void)inferExpressionType(expression.children[i], scope);
        }
        return TypeInfo{isGet ? SimpleTypeKind::Short : SimpleTypeKind::Unit,
                        isGet ? "Short" : "Unit"};
      }
      const TypeInfo bytes = inferExpressionType(expression.children[1], scope);
      const TypeInfo index = inferExpressionType(expression.children[2], scope);
      if (bytes.kind != SimpleTypeKind::Unknown &&
          (bytes.kind != SimpleTypeKind::Object ||
           arrayElementTypeName(bytes.name) != "Byte")) {
        diagnostics_.error(expression.children[1].span,
                           std::string(operation) +
                               " storage must have type Array[Byte]");
      }
      if (index.kind != SimpleTypeKind::Int && index.kind != SimpleTypeKind::Unknown) {
        diagnostics_.error(expression.children[2].span,
                           std::string(operation) + " index must have type Int");
      }
      if (!isGet) {
        const TypeInfo stored = inferExpressionType(expression.children[3], scope);
        if (stored.kind != SimpleTypeKind::Short &&
            stored.kind != SimpleTypeKind::Unknown) {
          diagnostics_.error(expression.children[3].span,
                             std::string(operation) + " value must have type Short");
        }
      }
      return TypeInfo{isGet ? SimpleTypeKind::Short : SimpleTypeKind::Unit,
                      isGet ? "Short" : "Unit"};
    }
    const AstExpression& callee = expression.children.front();
    const bool inferredArrayLiteral =
        callee.kind == AstExpressionKind::Identifier && callee.text == "Array";
    const bool explicitArrayLiteral =
        callee.kind == AstExpressionKind::TypeApply && callee.children.size() == 1 &&
        callee.children.front().kind == AstExpressionKind::Identifier &&
        callee.children.front().text == "Array";
    const bool dynamicArrayConstructor =
        callee.kind == AstExpressionKind::TypeApply && callee.children.size() == 1 &&
        callee.children.front().kind == AstExpressionKind::New &&
        callee.children.front().text == "Array";
    const bool arrayOfDim =
        callee.kind == AstExpressionKind::TypeApply && callee.children.size() == 1 &&
        callee.children.front().kind == AstExpressionKind::Select &&
        callee.children.front().children.size() == 1 &&
        callee.children.front().text == support::StdNames::ArrayOfDim &&
        callee.children.front().children.front().kind ==
            AstExpressionKind::Identifier &&
        callee.children.front().children.front().text == "Array";
    const bool arrayCopy =
        callee.kind == AstExpressionKind::Select && callee.children.size() == 1 &&
        callee.text == support::StdNames::ArrayCopy &&
        callee.children.front().kind == AstExpressionKind::Identifier &&
        callee.children.front().text == "Array";
    const bool arrayRange =
        callee.kind == AstExpressionKind::Select && callee.children.size() == 1 &&
        callee.text == support::StdNames::ArrayRange &&
        callee.children.front().kind == AstExpressionKind::Identifier &&
        callee.children.front().text == "Array";
    const AstExpression* arrayConcatTypeApply = nullptr;
    if (callee.kind == AstExpressionKind::TypeApply && callee.children.size() == 1) {
      const AstExpression& selected = callee.children.front();
      if (selected.kind == AstExpressionKind::Select && selected.children.size() == 1 &&
          selected.text == support::StdNames::ArrayConcat &&
          selected.children.front().kind == AstExpressionKind::Identifier &&
          selected.children.front().text == "Array") {
        arrayConcatTypeApply = &callee;
      }
    }
    const AstExpression* arrayFillTypeApply = nullptr;
    if (callee.kind == AstExpressionKind::Call && !callee.children.empty() &&
        callee.children.front().kind == AstExpressionKind::TypeApply &&
        callee.children.front().children.size() == 1) {
      const AstExpression& candidate = callee.children.front();
      const AstExpression& selected = candidate.children.front();
      if (selected.kind == AstExpressionKind::Select && selected.children.size() == 1 &&
          selected.text == support::StdNames::ArrayFill &&
          selected.children.front().kind == AstExpressionKind::Identifier &&
          selected.children.front().text == "Array") {
        arrayFillTypeApply = &candidate;
      }
    }
    if (arrayFillTypeApply != nullptr) {
      const TypeInfo elementType = typeFromDeclaredName(
          arrayFillTypeApply->declaredType, &scope, &arrayFillTypeApply->span);
      if (!isSupportedArrayElementType(elementType, scope, expression.span)) {
        diagnostics_.error(
            arrayFillTypeApply->span,
            "Array.fill type argument must be a supported scalar, reference, or "
            "nested array type in this subset");
      }

      std::vector<TypeInfo> lengthTypes;
      for (std::size_t i = 1; i < callee.children.size(); ++i) {
        lengthTypes.push_back(inferExpressionType(callee.children[i], scope));
      }
      if (lengthTypes.empty()) {
        diagnostics_.error(callee.span,
                           "Array.fill requires at least one Int dimension");
      }
      for (std::size_t i = 0; i < lengthTypes.size(); ++i) {
        if (lengthTypes[i].kind != SimpleTypeKind::Int &&
            lengthTypes[i].kind != SimpleTypeKind::Unknown) {
          diagnostics_.error(callee.children[i + 1].span,
                             "Array.fill dimensions must have type Int");
        }
      }

      std::vector<TypeInfo> valueTypes;
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        valueTypes.push_back(inferExpressionType(expression.children[i], scope));
      }
      if (valueTypes.size() != 1) {
        diagnostics_.error(expression.span,
                           "Array.fill requires exactly one element expression");
      } else if (!arrayElementConforms(elementType, valueTypes.front())) {
        diagnostics_.error(expression.children[1].span,
                           "Array.fill element does not conform to its declared type");
      }
      TypeInfo resultType = elementType;
      const std::size_t dimensions = std::max<std::size_t>(1, lengthTypes.size());
      for (std::size_t i = 0; i < dimensions; ++i) {
        resultType = TypeInfo{SimpleTypeKind::Object, arrayTypeName(resultType)};
      }
      return resultType;
    }
    if (arrayConcatTypeApply != nullptr) {
      const TypeInfo elementType = typeFromDeclaredName(
          arrayConcatTypeApply->declaredType, &scope, &arrayConcatTypeApply->span);
      if (!isSupportedArrayElementType(elementType, scope, expression.span)) {
        diagnostics_.error(
            arrayConcatTypeApply->span,
            "Array.concat type argument must be a supported scalar, reference, or "
            "nested array type in this subset");
      }

      const std::string expectedArrayType = arrayTypeName(elementType);
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        const TypeInfo argumentType =
            inferExpressionType(expression.children[i], scope);
        if (argumentType.kind != SimpleTypeKind::Unknown &&
            (argumentType.kind != SimpleTypeKind::Object ||
             argumentType.name != expectedArrayType)) {
          diagnostics_.error(
              expression.children[i].span,
              "Array.concat arguments must match its declared array type");
        }
      }
      return TypeInfo{SimpleTypeKind::Object, expectedArrayType};
    }
    if (arrayRange) {
      std::vector<TypeInfo> argumentTypes;
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        argumentTypes.push_back(inferExpressionType(expression.children[i], scope));
      }
      if (argumentTypes.size() != 2 && argumentTypes.size() != 3) {
        diagnostics_.error(expression.span,
                           "Array.range requires start, end, and an optional step");
      }
      for (std::size_t i = 0; i < argumentTypes.size(); ++i) {
        if (argumentTypes[i].kind != SimpleTypeKind::Int &&
            argumentTypes[i].kind != SimpleTypeKind::Unknown) {
          diagnostics_.error(expression.children[i + 1].span,
                             "Array.range arguments must have type Int");
        }
      }
      return TypeInfo{SimpleTypeKind::Object,
                      arrayTypeName(TypeInfo{SimpleTypeKind::Int, "Int"})};
    }
    if (arrayCopy) {
      std::vector<TypeInfo> argumentTypes;
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        argumentTypes.push_back(inferExpressionType(expression.children[i], scope));
      }
      if (argumentTypes.size() != 5) {
        diagnostics_.error(expression.span,
                           "Array.copy requires source, source position, destination, "
                           "destination position, and length");
        return TypeInfo{SimpleTypeKind::Unit, "Unit"};
      }

      const auto isKnownArray = [](const TypeInfo& type) {
        return type.kind == SimpleTypeKind::Object &&
               !arrayElementTypeName(type.name).empty();
      };
      const bool sourceIsArray = isKnownArray(argumentTypes[0]);
      const bool destinationIsArray = isKnownArray(argumentTypes[2]);
      if (!sourceIsArray && argumentTypes[0].kind != SimpleTypeKind::Unknown) {
        diagnostics_.error(expression.children[1].span,
                           "Array.copy source must have a statically known array type");
      }
      if (!destinationIsArray && argumentTypes[2].kind != SimpleTypeKind::Unknown) {
        diagnostics_.error(
            expression.children[3].span,
            "Array.copy destination must have a statically known array type");
      }
      if (sourceIsArray && destinationIsArray &&
          argumentTypes[0].name != argumentTypes[2].name) {
        const TypeInfo sourceElement = typeFromDeclaredName(
            arrayElementTypeName(argumentTypes[0].name), &scope, &expression.span);
        const TypeInfo destinationElement = typeFromDeclaredName(
            arrayElementTypeName(argumentTypes[2].name), &scope, &expression.span);
        const auto isDescriptorBackedReference = [](const TypeInfo& element) {
          return element.kind == SimpleTypeKind::Object &&
                 arrayElementTypeName(element.name).empty();
        };
        if (!isDescriptorBackedReference(sourceElement) ||
            !isDescriptorBackedReference(destinationElement)) {
          diagnostics_.error(
              expression.children[3].span,
              "Array.copy requires identical primitive, String, or nested array "
              "element types; only class, trait, object, and Any arrays may differ");
        }
      }
      for (const std::size_t index : {std::size_t{1}, std::size_t{3}, std::size_t{4}}) {
        if (argumentTypes[index].kind != SimpleTypeKind::Int &&
            argumentTypes[index].kind != SimpleTypeKind::Unknown) {
          diagnostics_.error(expression.children[index + 1].span,
                             "Array.copy positions and length must have type Int");
        }
      }
      return TypeInfo{SimpleTypeKind::Unit, "Unit"};
    }
    if (dynamicArrayConstructor || arrayOfDim) {
      TypeInfo elementType =
          typeFromDeclaredName(callee.declaredType, &scope, &callee.span);
      if (!isSupportedArrayElementType(elementType, scope, expression.span)) {
        diagnostics_.error(
            callee.span,
            std::string(arrayOfDim ? "Array.ofDim" : "Array constructor") +
                " type argument must be a supported scalar, reference, or nested "
                "array type in this subset");
      }
      std::vector<TypeInfo> argumentTypes;
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        argumentTypes.push_back(inferExpressionType(expression.children[i], scope));
      }
      if (dynamicArrayConstructor) {
        if (argumentTypes.size() != 1) {
          diagnostics_.error(expression.span,
                             "Array constructor requires exactly one Int length");
        } else if (argumentTypes.front().kind != SimpleTypeKind::Int &&
                   argumentTypes.front().kind != SimpleTypeKind::Unknown) {
          diagnostics_.error(expression.children[1].span,
                             "Array constructor length must have type Int");
        }
        return TypeInfo{SimpleTypeKind::Object, arrayTypeName(elementType)};
      }

      if (argumentTypes.empty()) {
        diagnostics_.error(expression.span,
                           "Array.ofDim requires at least one Int dimension");
      }
      for (std::size_t i = 0; i < argumentTypes.size(); ++i) {
        if (argumentTypes[i].kind != SimpleTypeKind::Int &&
            argumentTypes[i].kind != SimpleTypeKind::Unknown) {
          diagnostics_.error(expression.children[i + 1].span,
                             "Array.ofDim dimensions must have type Int");
        }
      }
      TypeInfo resultType = elementType;
      const std::size_t dimensions = std::max<std::size_t>(1, argumentTypes.size());
      for (std::size_t i = 0; i < dimensions; ++i) {
        resultType = TypeInfo{SimpleTypeKind::Object, arrayTypeName(resultType)};
      }
      return resultType;
    }
    if (inferredArrayLiteral || explicitArrayLiteral) {
      TypeInfo elementType{SimpleTypeKind::String, "String"};
      if (explicitArrayLiteral) {
        elementType = typeFromDeclaredName(callee.declaredType, &scope, &callee.span);
        if (!isSupportedArrayElementType(elementType, scope, expression.span)) {
          diagnostics_.error(
              callee.span,
              "Array literal type argument must be a supported scalar, reference, "
              "or nested array type in this subset");
        }
      }
      std::vector<TypeInfo> elementTypes;
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        elementTypes.push_back(inferExpressionType(expression.children[i], scope));
      }
      if (inferredArrayLiteral && elementTypes.empty()) {
        diagnostics_.error(expression.span, "Array literals require at least one "
                                            "supported scalar or reference element");
      }
      if (inferredArrayLiteral && !elementTypes.empty()) {
        elementType = elementTypes.front();
        const bool allElementsSupportObjectSlots = std::all_of(
            elementTypes.begin(), elementTypes.end(), [&](const TypeInfo& candidate) {
              return candidate.kind == SimpleTypeKind::Unknown ||
                     isSupportedAnyArrayValueType(candidate);
            });
        const bool needsObjectSlots = std::any_of(
            elementTypes.begin(), elementTypes.end(), [&](const TypeInfo& candidate) {
              return !arrayElementConforms(elementType, candidate);
            });
        if (allElementsSupportObjectSlots &&
            (!isSupportedArrayElementType(elementType, scope, expression.span) ||
             needsObjectSlots)) {
          elementType = TypeInfo{SimpleTypeKind::Object, "Object"};
        } else if (elementType.kind != SimpleTypeKind::Unknown &&
                   !isSupportedArrayElementType(elementType, scope, expression.span)) {
          diagnostics_.error(
              expression.children[1].span,
              "Array literal element type is unsupported in this subset");
          elementType = TypeInfo{SimpleTypeKind::String, "String"};
        }
      }
      for (std::size_t i = 0; i < elementTypes.size(); ++i) {
        if (!arrayElementConforms(elementType, elementTypes[i])) {
          diagnostics_.error(
              expression.children[i + 1].span,
              "Array literal elements must conform to the declared type");
        }
      }
      return TypeInfo{SimpleTypeKind::Object, arrayTypeName(elementType)};
    }
    if (const std::string elementTypeName = inferArrayElementTypeName(callee, scope);
        !elementTypeName.empty()) {
      std::vector<TypeInfo> argumentTypes;
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        argumentTypes.push_back(inferExpressionType(expression.children[i], scope));
      }
      if (argumentTypes.size() != 1) {
        diagnostics_.error(expression.span,
                           "array indexing requires exactly one Int index");
      } else if (argumentTypes.front().kind != SimpleTypeKind::Int &&
                 argumentTypes.front().kind != SimpleTypeKind::Unknown) {
        diagnostics_.error(expression.children[1].span,
                           "array index must have type Int");
      }
      return typeFromDeclaredName(elementTypeName, &scope, &expression.span);
    }
    const bool isArrayCloneCall = callee.kind == AstExpressionKind::Select &&
                                  callee.children.size() == 1 &&
                                  callee.text == support::StdNames::ArrayClone;
    if (isArrayCloneCall) {
      const TypeInfo receiverType = inferExpressionType(callee.children.front(), scope);
      if (receiverType.kind == SimpleTypeKind::Object &&
          !arrayElementTypeName(receiverType.name).empty()) {
        for (std::size_t i = 1; i < expression.children.size(); ++i) {
          (void)inferExpressionType(expression.children[i], scope);
        }
        if (expression.children.size() != 1) {
          diagnostics_.error(expression.span, "array clone does not accept arguments");
        }
        return receiverType;
      }
    }
    const bool isNumericConversionCall = callee.kind == AstExpressionKind::Select &&
                                         callee.children.size() == 1 &&
                                         (callee.text == support::StdNames::ToByte ||
                                          callee.text == support::StdNames::ToShort ||
                                          callee.text == support::StdNames::ToInt);
    if (isNumericConversionCall) {
      TypeInfo calleeType = inferExpressionType(callee, scope);
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        (void)inferExpressionType(expression.children[i], scope);
      }
      if (expression.children.size() != 1) {
        diagnostics_.error(expression.span, callee.text + " does not accept arguments");
      }
      return calleeType;
    }
    const bool isEqualsCall = callee.kind == AstExpressionKind::Select &&
                              callee.children.size() == 1 &&
                              callee.text == support::StdNames::Equals;
    bool isKnownMemberEqualsCall = false;
    TypeInfo equalsReceiverType{SimpleTypeKind::Unknown, "Unknown"};
    if (isEqualsCall) {
      equalsReceiverType = inferExpressionType(callee.children.front(), scope);
      isKnownMemberEqualsCall =
          knownMemberForReceiverType(equalsReceiverType, callee.text) != nullptr;
    }
    if (isEqualsCall && !isKnownMemberEqualsCall) {
      std::vector<TypeInfo> argumentTypes;
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        argumentTypes.push_back(inferExpressionType(expression.children[i], scope));
      }
      if (expression.children.size() != 2) {
        diagnostics_.error(expression.span, "equals requires exactly one argument");
      } else if (!isCompilerKnownEqualsReceiver(equalsReceiverType)) {
        diagnostics_.error(callee.children.front().span,
                           "equals receiver must be Unit, a primitive, String, "
                           "Symbol, or object value in this subset");
      } else if (!argumentTypes.empty() &&
                 !isCompilerKnownEqualsArgumentCompatible(equalsReceiverType,
                                                          argumentTypes.front())) {
        diagnostics_.error(expression.children[1].span,
                           "equals argument type " + argumentTypes.front().name +
                               " is not comparable with receiver type " +
                               equalsReceiverType.name + " in this subset");
      }
      return TypeInfo{SimpleTypeKind::Boolean, "Boolean"};
    }
    const bool isHashCodeCall = callee.kind == AstExpressionKind::Select &&
                                callee.children.size() == 1 &&
                                callee.text == support::StdNames::HashCode;
    bool isKnownMemberHashCodeCall = false;
    if (isHashCodeCall) {
      const TypeInfo receiver = inferExpressionType(callee.children.front(), scope);
      isKnownMemberHashCodeCall =
          knownMemberForReceiverType(receiver, callee.text) != nullptr;
    }
    if (isHashCodeCall && !isKnownMemberHashCodeCall) {
      TypeInfo calleeType = inferExpressionType(callee, scope);
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        (void)inferExpressionType(expression.children[i], scope);
      }
      if (expression.children.size() != 1) {
        diagnostics_.error(expression.span, "hashCode does not accept arguments");
      }
      return calleeType;
    }
    const bool isToStringCall = callee.kind == AstExpressionKind::Select &&
                                callee.children.size() == 1 &&
                                callee.text == support::StdNames::ToString;
    bool isKnownMemberToStringCall = false;
    if (isToStringCall) {
      const TypeInfo receiver = inferExpressionType(callee.children.front(), scope);
      isKnownMemberToStringCall =
          knownMemberForReceiverType(receiver, callee.text) != nullptr;
    }
    if (isToStringCall && !isKnownMemberToStringCall) {
      TypeInfo calleeType = inferExpressionType(callee, scope);
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        (void)inferExpressionType(expression.children[i], scope);
      }
      if (expression.children.size() != 1) {
        diagnostics_.error(expression.span, "toString does not accept arguments");
      }
      return calleeType;
    }
    const bool isRuntimeFormat = callee.kind == AstExpressionKind::Identifier &&
                                 callee.text == support::StdNames::RuntimeFormat;
    const bool isRuntimeBooleanFormat =
        callee.kind == AstExpressionKind::Identifier &&
        callee.text == support::StdNames::RuntimeFormatBoolean;
    if (isRuntimeFormat || isRuntimeBooleanFormat) {
      std::vector<TypeInfo> argumentTypes;
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        argumentTypes.push_back(inferExpressionType(expression.children[i], scope));
      }
      if (argumentTypes.size() != 2) {
        diagnostics_.error(
            expression.span,
            "f-interpolation format intrinsic requires a format string and value");
      }
      if (!argumentTypes.empty() &&
          argumentTypes.front().kind != SimpleTypeKind::String &&
          argumentTypes.front().kind != SimpleTypeKind::Unknown) {
        diagnostics_.error(expression.children[1].span,
                           "f-interpolation format specifier must be a String");
      }
      const char conversion =
          expression.children.size() >= 2 && expression.children[1].text.size() >= 2
              ? expression.children[1].text[expression.children[1].text.size() - 2]
              : '\0';
      if (argumentTypes.size() >= 2 &&
          argumentTypes[1].kind != SimpleTypeKind::Unknown) {
        if (conversion == 'f' && argumentTypes[1].kind != SimpleTypeKind::Float &&
            argumentTypes[1].kind != SimpleTypeKind::Double) {
          diagnostics_.error(
              expression.children[2].span,
              "f-interpolation %...f specifiers require Float or Double values");
        }
        if (conversion == 'd' && argumentTypes[1].kind != SimpleTypeKind::Byte &&
            argumentTypes[1].kind != SimpleTypeKind::Short &&
            argumentTypes[1].kind != SimpleTypeKind::Int &&
            argumentTypes[1].kind != SimpleTypeKind::Long) {
          diagnostics_.error(
              expression.children[2].span,
              "f-interpolation %...d specifiers require Int or Long values");
        }
        if (conversion == 'c' && argumentTypes[1].kind != SimpleTypeKind::Char) {
          diagnostics_.error(expression.children[2].span,
                             "f-interpolation %...c specifiers require Char values");
        }
        if (isRuntimeFormat && conversion == 's' &&
            argumentTypes[1].kind != SimpleTypeKind::String) {
          diagnostics_.error(expression.children[2].span,
                             "f-interpolation %...s specifiers require String values");
        }
        if (isRuntimeBooleanFormat &&
            argumentTypes[1].kind != SimpleTypeKind::Boolean) {
          diagnostics_.error(expression.children[2].span,
                             "f-interpolation %...b specifiers require Boolean values");
        }
      }
      return TypeInfo{SimpleTypeKind::String, "String"};
    }
    const AstExpression* constructor =
        callee.kind == AstExpressionKind::New ? &callee : nullptr;
    if (callee.kind == AstExpressionKind::TypeApply && callee.children.size() == 1 &&
        callee.children.front().kind == AstExpressionKind::New) {
      constructor = &callee.children.front();
    }
    if (constructor != nullptr) {
      std::vector<TypeInfo> argumentTypes;
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        argumentTypes.push_back(inferExpressionType(expression.children[i], scope));
      }

      const SymbolInfo* classSymbol = nullptr;
      SymbolInfo specializedClass;
      if (const SymbolInfo* resolved = qualifiedPathSymbol(constructor->text, &scope);
          resolved != nullptr && resolved->kind == AstDeclarationKind::Class) {
        classSymbol = resolved;
      }
      TypeInfo constructed{SimpleTypeKind::Unknown, "Unknown"};
      if (classSymbol != nullptr) {
        if (callee.kind == AstExpressionKind::TypeApply) {
          constructed = inferExpressionType(callee, scope);
          specializedClass = specializeTypeApplication(
              *classSymbol, typeArgumentsFor(callee), scope, callee.span, false);
          classSymbol = &specializedClass;
        } else if (!classSymbol->typeParameters.empty()) {
          specializedClass = inferTypeApplication(*classSymbol, argumentTypes,
                                                  expression.span, expectedType);
          classSymbol = &specializedClass;
          constructed = specializedClass.type;
        } else {
          constructed = inferExpressionType(callee, scope);
        }
      } else {
        constructed = inferExpressionType(callee, scope);
      }

      if (classSymbol != nullptr && classSymbol->typeParameters.empty()) {
        const std::size_t contextualParameterCount = static_cast<std::size_t>(
            std::count(classSymbol->contextualParameters.begin(),
                       classSymbol->contextualParameters.end(), true));
        const std::size_t ordinaryParameterCount =
            classSymbol->parameterTypes.size() - contextualParameterCount;
        bool materializedContextArguments = false;
        if (contextualParameterCount != 0 &&
            argumentTypes.size() == ordinaryParameterCount) {
          std::vector<TypedContextArgument> contextArguments =
              resolveContextArguments(*classSymbol, 0, scope, expression.span);
          for (const TypedContextArgument& argument : contextArguments) {
            const std::size_t insertionIndex =
                std::min(argument.parameterIndex, argumentTypes.size());
            argumentTypes.insert(argumentTypes.begin() + insertionIndex, argument.type);
          }
          recordContextApplication(expression.span, std::move(contextArguments));
          materializedContextArguments = true;
        }
        if (argumentTypes.size() != classSymbol->parameterTypes.size()) {
          diagnostics_.error(expression.span,
                             "constructor for " + classSymbol->name + " has " +
                                 std::to_string(argumentTypes.size()) +
                                 " arguments but expected " +
                                 std::to_string(classSymbol->parameterTypes.size()));
        }
        const std::size_t checkedArguments =
            std::min(argumentTypes.size(), classSymbol->parameterTypes.size());
        std::size_t sourceArgumentIndex = 0;
        for (std::size_t i = 0; i < checkedArguments; ++i) {
          const bool targetsAny = isAnyArrayElementType(classSymbol->parameterTypes[i]);
          const bool argumentConforms =
              targetsAny
                  ? isSupportedAnyArrayValueType(argumentTypes[i])
                  : isAssignable(classSymbol->parameterTypes[i], argumentTypes[i]);
          if (!argumentConforms) {
            support::SourceSpan argumentSpan = expression.span;
            const bool synthesized = materializedContextArguments &&
                                     i < classSymbol->contextualParameters.size() &&
                                     classSymbol->contextualParameters[i];
            if (!synthesized) {
              const std::size_t childIndex =
                  materializedContextArguments ? sourceArgumentIndex : i;
              if (childIndex + 1 < expression.children.size()) {
                argumentSpan = expression.children[childIndex + 1].span;
              }
            }
            diagnostics_.error(argumentSpan, "constructor argument " +
                                                 std::to_string(i) + " of type " +
                                                 argumentTypes[i].name +
                                                 " does not conform to field type " +
                                                 classSymbol->parameterTypes[i].name);
          }
          if (!materializedContextArguments ||
              i >= classSymbol->contextualParameters.size() ||
              !classSymbol->contextualParameters[i]) {
            ++sourceArgumentIndex;
          }
        }
      }
      return constructed;
    }

    const SymbolInfo* calleeSymbol = nullptr;
    SymbolInfo specializedCallee;
    std::optional<SymbolInfo> explicitInlineTarget;
    std::vector<TypeInfo> explicitInlineTypeArguments;
    std::optional<SymbolInfo> inferredInlineTarget;
    std::vector<TypeInfo> inferredInlineTypeArguments;
    std::optional<SymbolInfo> directInlineTarget;
    std::optional<AstExpression> inlineReceiver;
    std::optional<TypeInfo> inlineReceiverType;
    if (callee.kind == AstExpressionKind::Identifier) {
      auto found = scope.find(callee.text);
      if (found != scope.end()) {
        calleeSymbol = &found->second;
        if (found->second.isInstanceMember) {
          if (auto thisSymbol = scope.find("this"); thisSymbol != scope.end()) {
            AstExpression implicitReceiver;
            implicitReceiver.kind = AstExpressionKind::This;
            implicitReceiver.span = callee.span;
            inlineReceiver = std::move(implicitReceiver);
            inlineReceiverType = thisSymbol->second.type;
          }
        }
      }
    } else if (callee.kind == AstExpressionKind::Select &&
               callee.children.size() == 1) {
      const TypeInfo receiver = inferExpressionType(callee.children.front(), scope);
      if (std::optional<SymbolInfo> member =
              resolvedMemberForReceiverType(receiver, callee.text)) {
        specializedCallee = std::move(*member);
        calleeSymbol = &specializedCallee;
        inlineReceiver = callee.children.front();
        inlineReceiverType = receiver;
      }
    } else if (callee.kind == AstExpressionKind::TypeApply &&
               callee.children.size() == 1) {
      const AstExpression& genericTarget = callee.children.front();
      const SymbolInfo* rawTarget = nullptr;
      SymbolInfo receiverSpecialized;
      if (genericTarget.kind == AstExpressionKind::Identifier) {
        auto found = scope.find(genericTarget.text);
        if (found != scope.end()) {
          rawTarget = &found->second;
          if (found->second.isInstanceMember) {
            if (auto thisSymbol = scope.find("this"); thisSymbol != scope.end()) {
              AstExpression implicitReceiver;
              implicitReceiver.kind = AstExpressionKind::This;
              implicitReceiver.span = genericTarget.span;
              inlineReceiver = std::move(implicitReceiver);
              inlineReceiverType = thisSymbol->second.type;
            }
          }
        }
      } else if (genericTarget.kind == AstExpressionKind::Select &&
                 genericTarget.children.size() == 1) {
        const TypeInfo receiver =
            inferExpressionType(genericTarget.children.front(), scope);
        if (std::optional<SymbolInfo> member =
                resolvedMemberForReceiverType(receiver, genericTarget.text)) {
          receiverSpecialized = std::move(*member);
          rawTarget = &receiverSpecialized;
          inlineReceiver = genericTarget.children.front();
          inlineReceiverType = receiver;
        }
      }
      if (rawTarget != nullptr) {
        if (rawTarget->isInline && rawTarget->kind == AstDeclarationKind::Def) {
          explicitInlineTarget = *rawTarget;
          const std::vector<std::string> explicitArguments =
              typeArgumentsFor(callee);
          explicitInlineTypeArguments.reserve(explicitArguments.size());
          for (const std::string& argument : explicitArguments) {
            explicitInlineTypeArguments.push_back(
                typeFromDeclaredName(argument, &scope));
          }
        }
        specializedCallee = specializeTypeApplication(
            *rawTarget, typeArgumentsFor(callee), scope, callee.span, false);
        calleeSymbol = &specializedCallee;
      }
    }

    if (callee.kind != AstExpressionKind::TypeApply && calleeSymbol != nullptr &&
        calleeSymbol->kind == AstDeclarationKind::Def &&
        calleeSymbol->isInline && calleeSymbol->typeParameters.empty()) {
      directInlineTarget = *calleeSymbol;
    }

    std::vector<TypeInfo> argumentTypes;
    for (std::size_t i = 1; i < expression.children.size(); ++i) {
      const std::size_t parameterIndex = i - 1;
      const TypeInfo* argumentExpectedType =
          calleeSymbol != nullptr && calleeSymbol->typeParameters.empty() &&
                  parameterIndex < calleeSymbol->parameterTypes.size() &&
                  calleeSymbol->parameterTypes[parameterIndex].polymorphicFunctionType
              ? &calleeSymbol->parameterTypes[parameterIndex]
              : nullptr;
      argumentTypes.push_back(
          inferExpressionType(expression.children[i], scope, argumentExpectedType));
    }
    TypeInfo calleeType{SimpleTypeKind::Unknown, "Unknown"};
    if (calleeSymbol != nullptr && callee.kind != AstExpressionKind::TypeApply &&
        !calleeSymbol->typeParameters.empty()) {
      const SymbolInfo inferenceTarget = *calleeSymbol;
      std::vector<TypeInfo> inferredTypeArguments;
      bool inferenceConflict = false;
      specializedCallee = inferTypeApplication(
          inferenceTarget, argumentTypes, expression.span, expectedType, false,
          &inferredTypeArguments, &inferenceConflict);
      if (!specializedCallee.typeParameters.empty()) {
        std::size_t firstContextParameter = inferenceTarget.parameterTypes.size();
        for (std::size_t parameterIndex = 0;
             parameterIndex < inferenceTarget.contextualParameters.size();
             ++parameterIndex) {
          if (inferenceTarget.contextualParameters[parameterIndex]) {
            firstContextParameter = parameterIndex;
            break;
          }
        }
        const std::size_t contextualParameterCount = static_cast<std::size_t>(
            std::count(inferenceTarget.contextualParameters.begin(),
                       inferenceTarget.contextualParameters.end(), true));
        const std::size_t ordinaryParameterCount =
            inferenceTarget.parameterTypes.size() - contextualParameterCount;
        std::vector<SymbolInfo> contextualApplications;
        std::vector<TypeInfo> contextuallyInferredTypeArguments;
        if (!inferenceConflict &&
            firstContextParameter < inferenceTarget.parameterTypes.size() &&
            argumentTypes.size() == ordinaryParameterCount) {
          contextualApplications = inferContextualTypeApplications(
              inferenceTarget, inferredTypeArguments, firstContextParameter, scope,
              expression.span, true, nullptr,
              &contextuallyInferredTypeArguments);
        }
        if (contextualApplications.size() == 1) {
          specializedCallee = std::move(contextualApplications.front());
          inferredTypeArguments =
              std::move(contextuallyInferredTypeArguments);
        } else if (contextualApplications.size() > 1) {
          diagnostics_.error(expression.span,
                             "ambiguous contextual type inference for " +
                                 inferenceTarget.name +
                                 "; use explicit type arguments");
        } else {
          specializedCallee = inferTypeApplication(inferenceTarget, argumentTypes,
                                                   expression.span, expectedType);
        }
      } else {
        specializedCallee = inferTypeApplication(inferenceTarget, argumentTypes,
                                                 expression.span, expectedType);
      }
      if (inferenceTarget.isInline && !inferenceConflict &&
          specializedCallee.typeParameters.empty() &&
          inferredTypeArguments.size() == inferenceTarget.typeParameters.size() &&
          std::none_of(inferredTypeArguments.begin(), inferredTypeArguments.end(),
                       [](const TypeInfo& type) {
                         return type.kind == SimpleTypeKind::Unknown;
                       })) {
        inferredInlineTarget = inferenceTarget;
        inferredInlineTypeArguments = inferredTypeArguments;
      }
      calleeSymbol = &specializedCallee;
      calleeType = specializedCallee.type;
    } else {
      calleeType = inferExpressionType(callee, scope);
    }

    std::vector<TypedContextArgument> inlineContextArguments;
    if (calleeSymbol != nullptr && calleeSymbol->typeParameters.empty()) {
      const std::size_t contextualParameterCount = static_cast<std::size_t>(
          std::count(calleeSymbol->contextualParameters.begin(),
                     calleeSymbol->contextualParameters.end(), true));
      const std::size_t ordinaryParameterCount =
          calleeSymbol->parameterTypes.size() - contextualParameterCount;
      bool materializedContextArguments = false;
      if (contextualParameterCount != 0 &&
          argumentTypes.size() == ordinaryParameterCount) {
        std::vector<TypedContextArgument> contextArguments =
            resolveContextArguments(*calleeSymbol, 0, scope, expression.span);
        for (const TypedContextArgument& argument : contextArguments) {
          const std::size_t insertionIndex =
              std::min(argument.parameterIndex, argumentTypes.size());
          argumentTypes.insert(argumentTypes.begin() + insertionIndex, argument.type);
        }
        inlineContextArguments = contextArguments;
        recordContextApplication(expression.span, std::move(contextArguments));
        materializedContextArguments = true;
      }
      if (argumentTypes.size() != calleeSymbol->parameterTypes.size()) {
        diagnostics_.error(expression.span,
                           "call to " + calleeSymbol->name + " has " +
                               std::to_string(argumentTypes.size()) +
                               " arguments but expected " +
                               std::to_string(calleeSymbol->parameterTypes.size()));
      }
      const std::size_t checkedArguments =
          std::min(argumentTypes.size(), calleeSymbol->parameterTypes.size());
      std::size_t sourceArgumentIndex = 0;
      for (std::size_t i = 0; i < checkedArguments; ++i) {
        const bool targetsAny = isAnyArrayElementType(calleeSymbol->parameterTypes[i]);
        const bool argumentConforms =
            targetsAny
                ? isSupportedAnyArrayValueType(argumentTypes[i])
                : isAssignable(calleeSymbol->parameterTypes[i], argumentTypes[i]);
        if (!argumentConforms) {
          support::SourceSpan argumentSpan = expression.span;
          const bool synthesized = materializedContextArguments &&
                                   i < calleeSymbol->contextualParameters.size() &&
                                   calleeSymbol->contextualParameters[i];
          if (!synthesized) {
            const std::size_t childIndex =
                materializedContextArguments ? sourceArgumentIndex : i;
            if (childIndex + 1 < expression.children.size()) {
              argumentSpan = expression.children[childIndex + 1].span;
            }
          }
          if (argumentTypes[i].polymorphicFunctionType &&
              calleeSymbol->parameterTypes[i].polymorphicFunctionType) {
            diagnostics_.error(
                argumentSpan,
                "polymorphic function argument signature does not conform to "
                "parameter signature");
          } else {
            diagnostics_.error(argumentSpan,
                               "argument " + std::to_string(i) + " of type " +
                                   argumentTypes[i].name +
                                   " does not conform to parameter type " +
                                   calleeSymbol->parameterTypes[i].name);
          }
        }
        if (!materializedContextArguments ||
            i >= calleeSymbol->contextualParameters.size() ||
            !calleeSymbol->contextualParameters[i]) {
          ++sourceArgumentIndex;
        }
      }
    }
    const auto hasSupportedInlineArguments =
        [&](const SymbolInfo& target) {
          const std::size_t sourceArgumentCount =
              expression.children.size() - 1;
          if (inlineContextArguments.empty()) {
            return sourceArgumentCount == target.parameters.size();
          }
          const std::size_t contextualParameterCount =
              static_cast<std::size_t>(std::count(
                  target.contextualParameters.begin(),
                  target.contextualParameters.end(), true));
          return inlineContextArguments.size() == contextualParameterCount &&
                 sourceArgumentCount + contextualParameterCount ==
                     target.parameters.size();
        };
    std::optional<TypeInfo> transparentInlineResult;
    if (directInlineTarget.has_value() && calleeSymbol != nullptr &&
        calleeSymbol->typeParameters.empty() &&
        hasSupportedInlineArguments(*directInlineTarget)) {
      std::vector<AstExpression> inlineArguments(expression.children.begin() + 1,
                                                 expression.children.end());
      transparentInlineResult = recordInlineApplication(
          expression, *directInlineTarget, {}, inlineArguments,
          inlineContextArguments,
          inlineReceiver.has_value() ? &*inlineReceiver : nullptr,
          inlineReceiverType.has_value() ? &*inlineReceiverType : nullptr, scope,
          expectedType);
    }
    if (explicitInlineTarget.has_value() && calleeSymbol != nullptr &&
        calleeSymbol->typeParameters.empty() &&
        hasSupportedInlineArguments(*explicitInlineTarget)) {
      std::vector<AstExpression> inlineArguments(expression.children.begin() + 1,
                                                 expression.children.end());
      transparentInlineResult = recordInlineApplication(
          expression, *explicitInlineTarget, explicitInlineTypeArguments,
          inlineArguments, inlineContextArguments,
          inlineReceiver.has_value() ? &*inlineReceiver : nullptr,
          inlineReceiverType.has_value() ? &*inlineReceiverType : nullptr, scope,
          expectedType);
    }
    if (inferredInlineTarget.has_value() && calleeSymbol != nullptr &&
        calleeSymbol->typeParameters.empty() &&
        hasSupportedInlineArguments(*inferredInlineTarget)) {
      std::vector<AstExpression> inlineArguments(expression.children.begin() + 1,
                                                 expression.children.end());
      transparentInlineResult = recordInlineApplication(
          expression, *inferredInlineTarget, inferredInlineTypeArguments,
          inlineArguments, inlineContextArguments,
          inlineReceiver.has_value() ? &*inlineReceiver : nullptr,
          inlineReceiverType.has_value() ? &*inlineReceiverType : nullptr, scope,
          expectedType);
    }
    if (transparentInlineResult.has_value()) {
      return *transparentInlineResult;
    }
    return calleeSymbol == nullptr ? calleeType
                                   : staticExpressionType(std::move(calleeType));
  }
  case AstExpressionKind::Block:
    if (expression.children.empty()) {
      return TypeInfo{SimpleTypeKind::Unit, "Unit"};
    }
    {
      Scope blockScope = scope;
      const bool isInlineMatchBlock =
          !expression.children.empty() &&
          expression.children.back().kind == AstExpressionKind::If &&
          expression.children.back().isInline &&
          expression.children.back().text == "match";
      std::size_t contextualNestingDepth = 1;
      for (const auto& [name, candidate] : blockScope) {
        (void)name;
        if (candidate.isGiven) {
          contextualNestingDepth =
              std::max(contextualNestingDepth, candidate.contextualNestingDepth + 1);
        }
      }
      auto localDeclarationType = [&](const AstExpression& local) {
        TypeInfo declared{SimpleTypeKind::Unknown, "Unknown"};
        if (!local.declaredType.empty()) {
          declared = typeFromDeclaredName(local.declaredType, &blockScope, &local.span);
        }
        TypeInfo initializerType =
            local.children.empty()
                ? TypeInfo{SimpleTypeKind::Unit, "Unit"}
                : inferExpressionType(
                      local.children.front(), blockScope,
                      declared.kind == SimpleTypeKind::Unknown ? nullptr : &declared);
        if (local.declaredType.empty()) {
          return widenSoftUnion(initializerType);
        }

        const bool targetsAny = isAnyArrayElementType(declared);
        const bool initializerConforms =
            targetsAny ? isSupportedAnyArrayValueType(initializerType)
                       : isAssignable(declared, initializerType);
        if (!local.children.empty() && !initializerConforms) {
          diagnostics_.error(local.children.front().span,
                             "initializer type " + initializerType.name +
                                 " does not conform to declared type " + declared.name);
        }
        return declared.kind == SimpleTypeKind::Unknown ? initializerType : declared;
      };
      auto addLocalDeclaration = [&](const AstExpression& local) {
        if (local.localMethod != nullptr) {
          const AstLocalMethod& method = *local.localMethod;
          std::unordered_set<std::string> factoryParameters;
          for (const std::string& parameter : method.parameters) {
            const std::string name = parameterName(parameter);
            if (!name.empty()) {
              factoryParameters.insert(name);
            }
          }
          std::vector<std::pair<std::string, TypeInfo>> captures;
          std::unordered_set<std::string> capturedNames;
          std::unordered_set<std::string> reportedReceiverCaptures;
          std::function<void(const AstExpression&,
                             const std::unordered_set<std::string>&)>
              collectCaptures;
          collectCaptures =
              [&](const AstExpression& candidate,
                  const std::unordered_set<std::string>& locallyBoundNames) {
                if (candidate.kind == AstExpressionKind::Block) {
                  std::unordered_set<std::string> blockBindings = locallyBoundNames;
                  for (const AstExpression& child : candidate.children) {
                    collectCaptures(child, blockBindings);
                    if (child.kind == AstExpressionKind::LocalDeclaration &&
                        !child.text.empty()) {
                      blockBindings.insert(child.text);
                    }
                  }
                  return;
                }
                if (candidate.kind == AstExpressionKind::Catch) {
                  std::unordered_set<std::string> catchBindings = locallyBoundNames;
                  if (!candidate.text.empty()) {
                    catchBindings.insert(candidate.text);
                  }
                  for (const AstExpression& child : candidate.children) {
                    collectCaptures(child, catchBindings);
                  }
                  return;
                }
                if (candidate.kind == AstExpressionKind::SummonFromCase) {
                  std::unordered_set<std::string> caseBindings = locallyBoundNames;
                  if (!candidate.text.empty() && candidate.text != "_") {
                    caseBindings.insert(candidate.text);
                  }
                  for (const AstExpression& child : candidate.children) {
                    collectCaptures(child, caseBindings);
                  }
                  return;
                }
                if (candidate.kind == AstExpressionKind::LocalDeclaration &&
                    candidate.localMethod != nullptr) {
                  std::unordered_set<std::string> methodBindings = locallyBoundNames;
                  methodBindings.insert(candidate.text);
                  for (const std::string& parameter :
                       candidate.localMethod->parameters) {
                    const std::string name = parameterName(parameter);
                    if (!name.empty()) {
                      methodBindings.insert(name);
                    }
                  }
                  for (const AstExpression& child : candidate.children) {
                    collectCaptures(child, methodBindings);
                  }
                  return;
                }
                if (candidate.kind == AstExpressionKind::This ||
                    candidate.kind == AstExpressionKind::Super) {
                  if (reportedReceiverCaptures.insert(candidate.text).second) {
                    diagnostics_.error(
                        candidate.span,
                        "capturing this or super in a local parameterized given is "
                        "not supported yet");
                  }
                } else if (candidate.kind == AstExpressionKind::Identifier &&
                           !locallyBoundNames.contains(candidate.text)) {
                  auto captured = blockScope.find(candidate.text);
                  if (captured != blockScope.end() &&
                      captured->second.isInstanceMember) {
                    if (reportedReceiverCaptures.insert(candidate.text).second) {
                      diagnostics_.error(
                          candidate.span,
                          "capturing this or super in a local parameterized given is "
                          "not supported yet");
                    }
                  } else if (captured != blockScope.end() &&
                             captured->second.isLexicalValue &&
                             capturedNames.insert(candidate.text).second) {
                    captures.emplace_back(candidate.text, captured->second.type);
                  }
                }
                for (const AstExpression& child : candidate.children) {
                  collectCaptures(child, locallyBoundNames);
                }
              };
          if (!local.children.empty()) {
            collectCaptures(local.children.front(), factoryParameters);
          }

          AstDeclaration factory;
          factory.kind = AstDeclarationKind::Def;
          factory.name = local.text;
          factory.span = local.span;
          factory.typeParameters = method.typeParameters;
          for (const auto& [name, type] : captures) {
            factory.parameters.push_back(name + ": " + type.name);
            factory.contextualParameters.push_back(false);
          }
          factory.parameters.insert(factory.parameters.end(), method.parameters.begin(),
                                    method.parameters.end());
          factory.contextualParameters.insert(factory.contextualParameters.end(),
                                              method.contextualParameters.begin(),
                                              method.contextualParameters.end());
          factory.declaredType = local.declaredType;
          factory.isGiven = local.isGiven;
          factory.isAnonymousGiven = local.isAnonymousGiven;
          factory.hasInitializer = !local.children.empty();
          if (factory.hasInitializer) {
            factory.initializer = local.children.front();
          }
          const std::string owner = qualify(
              currentPackageName_, "$local$" + std::to_string(local.span.start));
          TypedDeclaration typedFactory =
              typecheckDeclaration(factory, owner, blockScope);
          if (auto inserted = blockScope.find(local.text);
              inserted != blockScope.end()) {
            inserted->second.contextualNestingDepth = contextualNestingDepth;
            inserted->second.captureParameterCount = captures.size();
            if (auto global = globalSymbols_.find(inserted->second.symbolName);
                global != globalSymbols_.end()) {
              global->second.contextualNestingDepth = contextualNestingDepth;
              global->second.captureParameterCount = captures.size();
            }
          }
          localFactoryDeclarations_.push_back(std::move(typedFactory));
          return;
        }

        const bool erasedInlineMatchSelector =
            isInlineMatchBlock && !local.mutableLocal &&
            local.text.starts_with("$match") && local.children.size() == 1 &&
            isErasedValueExpression(local.children.front(), blockScope);
        if (erasedInlineMatchSelector) {
          ++erasedValueSelectorDepth_;
        }
        std::optional<TypedPolymorphicFunctionApplication> polymorphicFunctionValue;
        TypeInfo localType;
        if (!local.mutableLocal && local.children.size() == 1) {
          if (local.children.front().kind == AstExpressionKind::PolymorphicFunction) {
            polymorphicFunctionValue = typecheckPolymorphicFunctionLiteral(
                local.children.front(), {}, local.children.front().span, blockScope,
                "stored polymorphic function literal must have the form "
                "[A] => (value: A) => body");
          } else {
            polymorphicFunctionValue =
                polymorphicFunctionAlias(local.children.front(), blockScope);
          }
        }
        if (polymorphicFunctionValue.has_value()) {
          const TypeInfo declaredLocal =
              typeFromDeclaredName(local.declaredType, &blockScope, &local.span);
          if (declaredLocal.polymorphicFunctionType) {
            localType = declaredLocal;
            if (!polymorphicFunctionMatchesDeclaredType(declaredLocal,
                                                        *polymorphicFunctionValue)) {
              const std::string expectedResult =
                  declaredLocal.typeArguments.size() == 2
                      ? declaredLocal.typeArguments[1].name
                      : "Unknown";
              diagnostics_.error(local.span,
                                 "polymorphic function result type " +
                                     polymorphicFunctionValue->resultType.name +
                                     " does not conform to declared result type " +
                                     expectedResult);
            }
          } else {
            const SymbolInfo* polyFunction = typeSymbolForDeclaredName(
                std::string(support::StdNames::ScalaPolyFunction), &blockScope);
            localType =
                polyFunction == nullptr
                    ? TypeInfo{SimpleTypeKind::Object,
                               std::string(support::StdNames::ScalaPolyFunction)}
                    : polyFunction->type;
          }
          polymorphicFunctionValueDeclarations_.push_back(local.span);
        } else {
          localType = localDeclarationType(local);
        }
        if (erasedInlineMatchSelector) {
          --erasedValueSelectorDepth_;
        }
        if (local.span.isValid()) {
          auto sameSpan = [&](const TypedExpressionInfo& info) {
            return info.span.source == local.span.source &&
                   info.span.start == local.span.start &&
                   info.span.length == local.span.length;
          };
          auto existing =
              std::find_if(expressionTypes_.rbegin(), expressionTypes_.rend(),
                           sameSpan);
          if (existing == expressionTypes_.rend()) {
            expressionTypes_.push_back(TypedExpressionInfo{local.span, localType});
          } else {
            existing->type = localType;
          }
        }
        SymbolInfo symbol;
        symbol.kind =
            local.mutableLocal ? AstDeclarationKind::Var : AstDeclarationKind::Val;
        symbol.name = local.text;
        symbol.symbolName = local.text;
        symbol.type = localType;
        symbol.isGiven = local.isGiven;
        symbol.isAnonymousGiven = local.isAnonymousGiven;
        symbol.contextualNestingDepth = contextualNestingDepth;
        symbol.isLexicalValue = true;
        symbol.isErasedCompileTimeValue = erasedInlineMatchSelector;
        if (polymorphicFunctionValue.has_value()) {
          symbol.polymorphicFunctionValue =
              std::make_shared<TypedPolymorphicFunctionApplication>(
                  std::move(*polymorphicFunctionValue));
        }
        if (!local.mutableLocal && !local.children.empty() &&
            local.text.starts_with("$match")) {
          symbol.specializedBooleanValue =
              constantBooleanValue(local.children.front(), blockScope);
          symbol.specializedIntegerValue =
              constantIntegerValue(local.children.front(), blockScope);
          symbol.specializedFloatingValue =
              constantFloatingValue(local.children.front(), blockScope);
          symbol.specializedStringValue =
              constantStringValue(local.children.front(), blockScope);
          symbol.specializedCharValue =
              constantCharValue(local.children.front(), blockScope);
          symbol.specializedNullValue =
              constantNullValue(local.children.front(), blockScope);
          symbol.specializedStaticType =
              specializedStaticType(local.children.front(), blockScope);
        }
        blockScope[local.text] = std::move(symbol);
      };
      for (std::size_t i = 0; i + 1 < expression.children.size(); ++i) {
        const AstExpression& child = expression.children[i];
        if (child.kind == AstExpressionKind::LocalDeclaration) {
          addLocalDeclaration(child);
        } else {
          (void)inferExpressionType(child, blockScope);
        }
      }
      const AstExpression& last = expression.children.back();
      if (last.kind == AstExpressionKind::LocalDeclaration) {
        addLocalDeclaration(last);
        return TypeInfo{SimpleTypeKind::Unit, "Unit"};
      }
      return inferExpressionType(last, blockScope, expectedType);
    }
  case AstExpressionKind::Return:
    if (expression.children.empty()) {
      return TypeInfo{SimpleTypeKind::Unit, "Unit"};
    }
    return inferExpressionType(expression.children.front(), scope, expectedType);
  case AstExpressionKind::Throw: {
    if (expression.children.size() != 1) {
      return TypeInfo{SimpleTypeKind::Nothing, "Nothing"};
    }
    const TypeInfo exception = inferExpressionType(expression.children.front(), scope);
    const std::string exceptionName =
        exception.runtimeName.empty() ? exception.name : exception.runtimeName;
    const bool validException =
        exception.kind == SimpleTypeKind::Null ||
        exception.kind == SimpleTypeKind::Unknown ||
        (exception.kind == SimpleTypeKind::Object &&
         isSubtypeOf(exceptionName, std::string(support::StdNames::JavaLangThrowable)));
    if (!validException) {
      diagnostics_.error(expression.children.front().span,
                         "throw operand must conform to Throwable or be null");
    }
    return TypeInfo{SimpleTypeKind::Nothing, "Nothing"};
  }
  case AstExpressionKind::Try: {
    if (expression.children.size() < 2) {
      return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
    }
    TypeInfo result =
        inferExpressionType(expression.children.front(), scope, expectedType);
    bool sawCatchAll = false;
    std::vector<std::pair<std::string, std::string>> earlierCatchTypes;
    for (std::size_t index = 1; index < expression.children.size(); ++index) {
      const AstExpression& child = expression.children[index];
      if (child.kind == AstExpressionKind::Finally) {
        (void)inferExpressionType(child, scope);
        continue;
      }
      if (child.kind != AstExpressionKind::Catch || child.children.size() != 1) {
        continue;
      }

      TypeInfo exceptionType =
          typeFromDeclaredName(child.declaredType, &scope, &child.span);
      const SymbolInfo* exceptionSymbol =
          exceptionType.name == "Object"
              ? nullptr
              : typeSymbolForDeclaredName(child.declaredType, &scope);
      const std::string exceptionName = exceptionType.runtimeName.empty()
                                            ? exceptionType.name
                                            : exceptionType.runtimeName;
      const bool catchAll = exceptionType.kind == SimpleTypeKind::Object &&
                            (exceptionType.name == "Object" ||
                             exceptionName == support::StdNames::JavaLangObject);
      const bool knownReference =
          exceptionType.kind == SimpleTypeKind::Object &&
          (catchAll || (exceptionSymbol != nullptr &&
                        (exceptionSymbol->kind == AstDeclarationKind::Class ||
                         exceptionSymbol->kind == AstDeclarationKind::Trait)));
      const bool throwableClass =
          exceptionSymbol != nullptr &&
          exceptionSymbol->kind == AstDeclarationKind::Class &&
          isSubtypeOf(exceptionName, std::string(support::StdNames::JavaLangThrowable));
      const bool validExceptionType =
          catchAll ||
          (exceptionSymbol != nullptr &&
           (exceptionSymbol->kind == AstDeclarationKind::Trait || throwableClass));
      if (!knownReference) {
        diagnostics_.error(child.span,
                           "catch type must be a known class, trait, or Object: " +
                               child.declaredType);
        exceptionType = TypeInfo{SimpleTypeKind::Object, "Object"};
      } else if (exceptionSymbol != nullptr &&
                 exceptionSymbol->kind == AstDeclarationKind::Class &&
                 !throwableClass) {
        diagnostics_.error(child.span, "catch class must conform to Throwable: " +
                                           child.declaredType);
      }
      if (sawCatchAll) {
        diagnostics_.error(child.span, "catch-all handler must be last");
      } else if (validExceptionType) {
        const auto shadowing = std::find_if(
            earlierCatchTypes.begin(), earlierCatchTypes.end(),
            [&](const auto& earlier) {
              return isSubtypeOf(exceptionName, earlier.first) ||
                     isSubtypeOf(std::string(support::StdNames::JavaLangThrowable),
                                 earlier.first);
            });
        if (shadowing != earlierCatchTypes.end()) {
          diagnostics_.error(child.span, "catch handler for " + child.declaredType +
                                             " is unreachable after " +
                                             shadowing->second);
        }
      }
      if (validExceptionType) {
        earlierCatchTypes.emplace_back(exceptionName, child.declaredType);
      }
      sawCatchAll = sawCatchAll || catchAll;

      Scope handlerScope = scope;
      SymbolInfo binding;
      binding.kind = AstDeclarationKind::Val;
      binding.name = child.text;
      binding.symbolName = child.text;
      binding.type = exceptionType;
      handlerScope[child.text] = std::move(binding);
      result =
          commonType(result, inferExpressionType(child, handlerScope, expectedType));
    }
    return result;
  }
  case AstExpressionKind::Catch:
    return expression.children.size() == 1
               ? inferExpressionType(expression.children.front(), scope, expectedType)
               : TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
  case AstExpressionKind::Finally:
    if (expression.children.size() == 1) {
      (void)inferExpressionType(expression.children.front(), scope);
    }
    return TypeInfo{SimpleTypeKind::Unit, "Unit"};
  case AstExpressionKind::SummonFrom: {
    for (std::size_t branchIndex = 0; branchIndex < expression.children.size();
         ++branchIndex) {
      const AstExpression& branch = expression.children[branchIndex];
      if (branch.kind != AstExpressionKind::SummonFromCase ||
          branch.children.size() != 1) {
        continue;
      }

      Scope branchScope = scope;
      std::vector<TypedContextArgument> arguments;
      const bool catchAll = branch.text == "_" && branch.declaredType.empty();
      if (!catchAll) {
        const TypeInfo requested =
            typeFromDeclaredName(branch.declaredType, &scope, &branch.span);
        SymbolInfo request;
        request.kind = AstDeclarationKind::Def;
        request.name = "summonFrom";
        request.type = requested;
        request.parameters = {"evidence: " + requested.name};
        request.parameterTypes = {requested};
        request.contextualParameters = {true};

        ContextResolutionFailure failure = ContextResolutionFailure::None;
        arguments = resolveContextArguments(request, 0, scope, branch.span, nullptr,
                                            false, &failure);
        const bool matched = arguments.size() == 1 &&
                             arguments.front().type.kind != SimpleTypeKind::Unknown;
        if (!matched && (failure == ContextResolutionFailure::None ||
                         failure == ContextResolutionFailure::Missing)) {
          continue;
        }
        if (!matched) {
          (void)resolveContextArguments(request, 0, scope, branch.span);
        }

        if (branch.text != "_") {
          SymbolInfo binding;
          binding.kind = AstDeclarationKind::Val;
          binding.name = branch.text;
          binding.symbolName = branch.text;
          binding.type = requested;
          binding.isGiven = branch.isGiven;
          binding.isContextParameter = branch.isGiven;
          binding.isLexicalValue = true;
          branchScope[branch.text] = std::move(binding);
        }
      }

      recordContextApplication(expression.span, std::move(arguments), true,
                               branchIndex);
      return inferExpressionType(branch.children.front(), branchScope, expectedType);
    }
    diagnostics_.error(expression.span,
                       "no summonFrom case matched a contextual value");
    return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
  }
  case AstExpressionKind::SummonFromCase:
    return expression.children.size() == 1
               ? inferExpressionType(expression.children.front(), scope, expectedType)
               : TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
  case AstExpressionKind::If:
    if (expression.children.size() < 2) {
      return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
    }
    {
      const TypeInfo condition = inferExpressionType(expression.children[0], scope);
      if (condition.kind != SimpleTypeKind::Boolean &&
          condition.kind != SimpleTypeKind::Unknown) {
        diagnostics_.error(expression.children[0].span,
                           "if condition requires a Boolean value");
      }
    }
    if (inlineExpansionDepth_ != 0) {
      if (std::optional<bool> condition =
              constantBooleanValue(expression.children[0], scope)) {
        const std::size_t selectedBranch = *condition ? 1 : 2;
        recordContextApplication(expression.span, {}, true, selectedBranch);
        if (selectedBranch >= expression.children.size()) {
          return TypeInfo{SimpleTypeKind::Unit, "Unit"};
        }
        return inferExpressionType(expression.children[selectedBranch], scope,
                                   expectedType);
      }
      if (expression.isInline) {
        diagnostics_.error(expression.children[0].span,
                           expression.text == "match"
                               ? "inline match selector must be reducible from a "
                                 "compile-time value or static type"
                               : "inline if condition must be a compile-time "
                                 "Boolean constant");
      }
    }
    if (expression.children.size() == 2) {
      (void)inferExpressionType(expression.children[1], scope, expectedType);
      return TypeInfo{SimpleTypeKind::Unit, "Unit"};
    }
    return commonType(inferExpressionType(expression.children[1], scope, expectedType),
                      inferExpressionType(expression.children[2], scope, expectedType));
  case AstExpressionKind::While:
    if (expression.children.empty()) {
      return TypeInfo{SimpleTypeKind::Unit, "Unit"};
    }
    {
      const TypeInfo condition =
          inferExpressionType(expression.children.front(), scope);
      if (condition.kind != SimpleTypeKind::Boolean &&
          condition.kind != SimpleTypeKind::Unknown) {
        diagnostics_.error(expression.children.front().span,
                           "while condition requires a Boolean value");
      }
    }
    for (std::size_t i = 1; i < expression.children.size(); ++i) {
      (void)inferExpressionType(expression.children[i], scope);
    }
    return TypeInfo{SimpleTypeKind::Unit, "Unit"};
  case AstExpressionKind::Unary: {
    if (expression.children.size() != 1) {
      return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
    }
    const TypeInfo operand = inferExpressionType(expression.children.front(), scope);
    if (expression.text == "!") {
      if (operand.kind != SimpleTypeKind::Boolean &&
          operand.kind != SimpleTypeKind::Unknown) {
        diagnostics_.error(expression.children.front().span,
                           "logical negation operator ! requires a Boolean operand");
      }
      return TypeInfo{SimpleTypeKind::Boolean, "Boolean"};
    }
    if (expression.text == "+" || expression.text == "-") {
      if (operand.kind != SimpleTypeKind::Byte &&
          operand.kind != SimpleTypeKind::Short &&
          operand.kind != SimpleTypeKind::Int && operand.kind != SimpleTypeKind::Long &&
          operand.kind != SimpleTypeKind::Float &&
          operand.kind != SimpleTypeKind::Double &&
          operand.kind != SimpleTypeKind::Unknown) {
        diagnostics_.error(expression.children.front().span,
                           "unary operator " + expression.text +
                               " requires a numeric operand");
      }
      return operand.kind == SimpleTypeKind::Byte ||
                     operand.kind == SimpleTypeKind::Short
                 ? TypeInfo{SimpleTypeKind::Int, "Int"}
                 : operand;
    }
    diagnostics_.error(expression.span,
                       "unsupported unary operator: " + expression.text);
    return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
  }
  case AstExpressionKind::Binary: {
    if (expression.children.size() != 2) {
      return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
    }
    TypeInfo lhs = inferExpressionType(expression.children[0], scope);
    TypeInfo rhs = inferExpressionType(expression.children[1], scope);
    if (expression.text == support::StdNames::TupleConcat) {
      const bool emptyLeft =
          lhs.name == support::StdNames::ScalaEmptyTuple ||
          lhs.runtimeName == support::StdNames::ScalaEmptyTuple;
      const bool emptyRight =
          rhs.name == support::StdNames::ScalaEmptyTuple ||
          rhs.runtimeName == support::StdNames::ScalaEmptyTuple;
      const std::string leftConstructor =
          lhs.typeConstructorName.empty() ? lhs.runtimeName
                                          : lhs.typeConstructorName;
      const std::string rightConstructor =
          rhs.typeConstructorName.empty() ? rhs.runtimeName
                                          : rhs.typeConstructorName;
      const std::optional<std::size_t> leftArity =
          tupleArityForConstructor(leftConstructor);
      const std::optional<std::size_t> rightArity =
          tupleArityForConstructor(rightConstructor);
      const bool validLeft =
          emptyLeft ||
          (leftArity.has_value() && lhs.typeArguments.size() == *leftArity);
      const bool validRight =
          emptyRight ||
          (rightArity.has_value() && rhs.typeArguments.size() == *rightArity);
      if (!validLeft) {
        diagnostics_.error(expression.children[0].span,
                           "++ left operand must be a concrete tuple");
      }
      if (!validRight) {
        diagnostics_.error(expression.children[1].span,
                           "++ right operand must be a concrete tuple");
      }
      if (!validLeft || !validRight) {
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }

      const std::size_t resultArity =
          lhs.typeArguments.size() + rhs.typeArguments.size();
      if (resultArity > 22) {
        diagnostics_.error(expression.span,
                           "++ tuple concatenation supports at most 22 elements "
                           "in this subset");
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }
      if (resultArity == 0) {
        auto empty = globalSymbols_.find(
            std::string(support::StdNames::ScalaEmptyTuple));
        if (empty == globalSymbols_.end()) {
          diagnostics_.error(
              expression.span,
              "unresolved tuple ++ result: scala.EmptyTuple");
          return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
        }
        return empty->second.type;
      }

      const std::string constructorName =
          std::string(support::StdNames::ScalaTuple) +
          std::to_string(resultArity);
      auto constructor = globalSymbols_.find(constructorName);
      if (constructor == globalSymbols_.end()) {
        diagnostics_.error(expression.span,
                           "unresolved tuple ++ constructor: " +
                               constructorName);
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }
      std::vector<TypeInfo> elementTypes;
      elementTypes.reserve(resultArity);
      elementTypes.insert(elementTypes.end(), lhs.typeArguments.begin(),
                          lhs.typeArguments.end());
      elementTypes.insert(elementTypes.end(), rhs.typeArguments.begin(),
                          rhs.typeArguments.end());
      return specializeResolvedTypeApplication(
                 constructor->second, elementTypes, expression.span, true)
          .type;
    }
    if (expression.text == "*:") {
      const bool emptyTail =
          rhs.name == support::StdNames::ScalaEmptyTuple ||
          rhs.runtimeName == support::StdNames::ScalaEmptyTuple;
      const std::string tailConstructor =
          rhs.typeConstructorName.empty() ? rhs.runtimeName
                                          : rhs.typeConstructorName;
      const std::optional<std::size_t> tailArity =
          tupleArityForConstructor(tailConstructor);
      if (!emptyTail &&
          (!tailArity.has_value() || rhs.typeArguments.size() != *tailArity)) {
        diagnostics_.error(expression.children[1].span,
                           "*: right operand must be a concrete tuple");
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }
      const std::size_t resultArity = rhs.typeArguments.size() + 1;
      if (resultArity > 22) {
        diagnostics_.error(expression.span,
                           "*: tuple construction supports at most 22 elements in "
                           "this subset");
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }
      const std::string constructorName =
          std::string(support::StdNames::ScalaTuple) +
          std::to_string(resultArity);
      auto constructor = globalSymbols_.find(constructorName);
      if (constructor == globalSymbols_.end()) {
        diagnostics_.error(expression.span,
                           "unresolved tuple value constructor: " + constructorName);
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }
      std::vector<TypeInfo> elementTypes;
      elementTypes.reserve(resultArity);
      elementTypes.push_back(std::move(lhs));
      elementTypes.insert(elementTypes.end(), rhs.typeArguments.begin(),
                          rhs.typeArguments.end());
      return specializeResolvedTypeApplication(
                 constructor->second, elementTypes, expression.span, true)
          .type;
    }
    if (expression.text == "==" || expression.text == "!=" || expression.text == "<" ||
        expression.text == ">" || expression.text == "<=" || expression.text == ">=") {
      return TypeInfo{SimpleTypeKind::Boolean, "Boolean"};
    }
    if (expression.text == "&&" || expression.text == "||") {
      if (lhs.kind != SimpleTypeKind::Boolean && lhs.kind != SimpleTypeKind::Unknown) {
        diagnostics_.error(expression.children[0].span,
                           "logical operator " + expression.text +
                               " requires a Boolean left operand");
      }
      if (rhs.kind != SimpleTypeKind::Boolean && rhs.kind != SimpleTypeKind::Unknown) {
        diagnostics_.error(expression.children[1].span,
                           "logical operator " + expression.text +
                               " requires a Boolean right operand");
      }
      return TypeInfo{SimpleTypeKind::Boolean, "Boolean"};
    }
    if (expression.text == "&" || expression.text == "|" || expression.text == "^") {
      const auto isBitwise = [](SimpleTypeKind kind) {
        return kind == SimpleTypeKind::Boolean || kind == SimpleTypeKind::Byte ||
               kind == SimpleTypeKind::Short || kind == SimpleTypeKind::Int ||
               kind == SimpleTypeKind::Long || kind == SimpleTypeKind::Unknown;
      };
      if (!isBitwise(lhs.kind) || !isBitwise(rhs.kind)) {
        diagnostics_.error(expression.span,
                           "bitwise operator " + expression.text +
                               " requires Boolean, Int, or Long operands");
      }
      if (lhs.kind != SimpleTypeKind::Unknown && rhs.kind != SimpleTypeKind::Unknown &&
          lhs.kind != rhs.kind) {
        diagnostics_.error(expression.span, "bitwise operands must have the same type");
      }
      const TypeInfo result = commonType(lhs, rhs);
      return result.kind == SimpleTypeKind::Byte || result.kind == SimpleTypeKind::Short
                 ? TypeInfo{SimpleTypeKind::Int, "Int"}
                 : result;
    }
    if (expression.text == "<<" || expression.text == ">>" ||
        expression.text == ">>>") {
      if (lhs.kind != SimpleTypeKind::Byte && lhs.kind != SimpleTypeKind::Short &&
          lhs.kind != SimpleTypeKind::Int && lhs.kind != SimpleTypeKind::Long &&
          lhs.kind != SimpleTypeKind::Unknown) {
        diagnostics_.error(expression.children[0].span,
                           "shift left operand must have type Int or Long");
      }
      if (rhs.kind != SimpleTypeKind::Int && rhs.kind != SimpleTypeKind::Unknown) {
        diagnostics_.error(expression.children[1].span,
                           "shift count must have type Int");
      }
      return lhs.kind == SimpleTypeKind::Byte || lhs.kind == SimpleTypeKind::Short
                 ? TypeInfo{SimpleTypeKind::Int, "Int"}
                 : lhs;
    }
    if (expression.text == "+" &&
        (lhs.kind == SimpleTypeKind::String || rhs.kind == SimpleTypeKind::String)) {
      const auto isStringConvertible = [](SimpleTypeKind kind) {
        return kind == SimpleTypeKind::Unit || kind == SimpleTypeKind::String ||
               kind == SimpleTypeKind::Boolean || kind == SimpleTypeKind::Byte ||
               kind == SimpleTypeKind::Short || kind == SimpleTypeKind::Int ||
               kind == SimpleTypeKind::Long || kind == SimpleTypeKind::Float ||
               kind == SimpleTypeKind::Double || kind == SimpleTypeKind::Char ||
               kind == SimpleTypeKind::Symbol || kind == SimpleTypeKind::Null ||
               kind == SimpleTypeKind::Object || kind == SimpleTypeKind::Unknown;
      };
      if (!isStringConvertible(lhs.kind) || !isStringConvertible(rhs.kind)) {
        diagnostics_.error(expression.span, "string concatenation supports String, "
                                            "primitive, Null, and object operands "
                                            "in this subset");
      }
      return TypeInfo{SimpleTypeKind::String, "String"};
    }
    if (expression.text == "%") {
      const auto isNumeric = [](SimpleTypeKind kind) {
        return kind == SimpleTypeKind::Byte || kind == SimpleTypeKind::Short ||
               kind == SimpleTypeKind::Int || kind == SimpleTypeKind::Long ||
               kind == SimpleTypeKind::Float || kind == SimpleTypeKind::Double ||
               kind == SimpleTypeKind::Unknown;
      };
      if (!isNumeric(lhs.kind) || !isNumeric(rhs.kind)) {
        diagnostics_.error(expression.span,
                           "remainder operator % requires numeric operands");
      }
    }
    const TypeInfo result = commonType(lhs, rhs);
    return result.kind == SimpleTypeKind::Byte || result.kind == SimpleTypeKind::Short
               ? TypeInfo{SimpleTypeKind::Int, "Int"}
               : result;
  }
  }
  return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
}

bool Typechecker::expressionHasReferenceType(const AstExpression& expression) const {
  if (expression.kind == AstExpressionKind::New) {
    return true;
  }
  for (auto info = expressionTypes_.rbegin(); info != expressionTypes_.rend(); ++info) {
    if (info->span.source == expression.span.source &&
        info->span.start == expression.span.start &&
        info->span.length == expression.span.length) {
      return isReferenceType(info->type);
    }
  }
  return false;
}

bool Typechecker::selectedMethodMayEscapeZone(const AstExpression& expression) const {
  if (expression.kind != AstExpressionKind::Select || expression.children.size() != 1) {
    return false;
  }
  const AstExpression& receiver = expression.children.front();
  for (auto info = expressionTypes_.rbegin(); info != expressionTypes_.rend(); ++info) {
    if (info->span.source != receiver.span.source ||
        info->span.start != receiver.span.start ||
        info->span.length != receiver.span.length) {
      continue;
    }
    const std::string owner =
        info->type.runtimeName.empty() ? info->type.name : info->type.runtimeName;
    auto members = memberScopes_.find(owner);
    if (members == memberScopes_.end()) {
      return false;
    }
    auto member = members->second.find(expression.text);
    return member != members->second.end() &&
           member->second.kind == AstDeclarationKind::Def &&
           directZoneReceiverEscapes_.contains(member->second.symbolName);
  }
  return false;
}

bool Typechecker::expressionDirectlyEscapesReceiver(
    const AstExpression& expression,
    std::unordered_map<std::string, bool>& receiverAliases,
    std::unordered_set<std::string>& localNames,
    std::vector<AstExpression>* receiverMethodCallSites) const {
  switch (expression.kind) {
  case AstExpressionKind::Empty:
  case AstExpressionKind::IntegerLiteral:
  case AstExpressionKind::FloatingLiteral:
  case AstExpressionKind::StringLiteral:
  case AstExpressionKind::CharLiteral:
  case AstExpressionKind::SymbolLiteral:
  case AstExpressionKind::BooleanLiteral:
  case AstExpressionKind::NullLiteral:
  case AstExpressionKind::Super:
  case AstExpressionKind::ModuleReference:
    return false;
  case AstExpressionKind::This:
    return true;
  case AstExpressionKind::Identifier: {
    auto found = receiverAliases.find(expression.text);
    return found != receiverAliases.end() && found->second;
  }
  case AstExpressionKind::New:
    for (const AstExpression& child : expression.children) {
      (void)expressionDirectlyEscapesReceiver(child, receiverAliases, localNames,
                                              receiverMethodCallSites);
    }
    return false;
  case AstExpressionKind::LocalDeclaration: {
    const bool initializerEscapes =
        !expression.children.empty() &&
        expressionDirectlyEscapesReceiver(expression.children.front(), receiverAliases,
                                          localNames, receiverMethodCallSites);
    localNames.insert(expression.text);
    receiverAliases[expression.text] = initializerEscapes;
    return false;
  }
  case AstExpressionKind::Block: {
    struct SavedLocal {
      std::string name;
      bool hadAlias = false;
      bool alias = false;
      bool wasLocal = false;
    };
    std::vector<SavedLocal> savedLocals;
    bool result = false;
    for (const AstExpression& child : expression.children) {
      if (child.kind == AstExpressionKind::LocalDeclaration) {
        auto alias = receiverAliases.find(child.text);
        savedLocals.push_back(
            SavedLocal{child.text, alias != receiverAliases.end(),
                       alias == receiverAliases.end() ? false : alias->second,
                       localNames.contains(child.text)});
      }
      result = expressionDirectlyEscapesReceiver(child, receiverAliases, localNames,
                                                 receiverMethodCallSites);
    }
    for (auto saved = savedLocals.rbegin(); saved != savedLocals.rend(); ++saved) {
      if (saved->hadAlias) {
        receiverAliases[saved->name] = saved->alias;
      } else {
        receiverAliases.erase(saved->name);
      }
      if (saved->wasLocal) {
        localNames.insert(saved->name);
      } else {
        localNames.erase(saved->name);
      }
    }
    return result;
  }
  case AstExpressionKind::Select: {
    if (expression.children.size() != 1) {
      return false;
    }
    const bool receiver =
        expressionDirectlyEscapesReceiver(expression.children.front(), receiverAliases,
                                          localNames, receiverMethodCallSites);
    if (receiver && receiverMethodCallSites != nullptr) {
      receiverMethodCallSites->push_back(expression);
    }
    return receiver && expressionHasReferenceType(expression);
  }
  case AstExpressionKind::TypeApply:
    if (expression.children.empty()) {
      return false;
    }
    if (expression.children.front().kind == AstExpressionKind::Select &&
        expression.children.front().text == support::StdNames::AsInstanceOf &&
        !expression.children.front().children.empty()) {
      return expressionDirectlyEscapesReceiver(
          expression.children.front().children.front(), receiverAliases, localNames,
          receiverMethodCallSites);
    }
    (void)expressionDirectlyEscapesReceiver(expression.children.front(),
                                            receiverAliases, localNames,
                                            receiverMethodCallSites);
    return false;
  case AstExpressionKind::Call:
    if (expression.children.empty()) {
      return false;
    }
    if (expression.children.front().kind == AstExpressionKind::New) {
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        (void)expressionDirectlyEscapesReceiver(expression.children[i], receiverAliases,
                                                localNames, receiverMethodCallSites);
      }
      return false;
    }
    (void)expressionDirectlyEscapesReceiver(expression.children.front(),
                                            receiverAliases, localNames,
                                            receiverMethodCallSites);
    for (std::size_t i = 1; i < expression.children.size(); ++i) {
      if (expressionDirectlyEscapesReceiver(expression.children[i], receiverAliases,
                                            localNames, receiverMethodCallSites)) {
        return true;
      }
    }
    return false;
  case AstExpressionKind::Assign: {
    if (expression.children.size() != 2) {
      return false;
    }
    const bool assigned =
        expressionDirectlyEscapesReceiver(expression.children.back(), receiverAliases,
                                          localNames, receiverMethodCallSites);
    const AstExpression& target = expression.children.front();
    if (target.kind == AstExpressionKind::Identifier) {
      if (localNames.contains(target.text)) {
        receiverAliases[target.text] = assigned;
        return false;
      }
      return assigned;
    }
    if (target.kind == AstExpressionKind::Select && !target.children.empty()) {
      const bool receiver =
          expressionDirectlyEscapesReceiver(target.children.front(), receiverAliases,
                                            localNames, receiverMethodCallSites);
      return assigned && !receiver;
    }
    return assigned;
  }
  case AstExpressionKind::Return:
  case AstExpressionKind::Throw:
    for (const AstExpression& child : expression.children) {
      (void)expressionDirectlyEscapesReceiver(child, receiverAliases, localNames,
                                              receiverMethodCallSites);
    }
    return false;
  case AstExpressionKind::Try: {
    bool result = false;
    for (const AstExpression& child : expression.children) {
      if (child.kind == AstExpressionKind::Finally) {
        (void)expressionDirectlyEscapesReceiver(child, receiverAliases, localNames,
                                                receiverMethodCallSites);
      } else {
        result = expressionDirectlyEscapesReceiver(child, receiverAliases, localNames,
                                                   receiverMethodCallSites) ||
                 result;
      }
    }
    return result;
  }
  case AstExpressionKind::Catch: {
    const auto savedAliases = receiverAliases;
    const auto savedLocals = localNames;
    receiverAliases[expression.text] = false;
    localNames.insert(expression.text);
    const bool result =
        !expression.children.empty() &&
        expressionDirectlyEscapesReceiver(expression.children.front(), receiverAliases,
                                          localNames, receiverMethodCallSites);
    receiverAliases = savedAliases;
    localNames = savedLocals;
    return result;
  }
  case AstExpressionKind::Finally:
    for (const AstExpression& child : expression.children) {
      (void)expressionDirectlyEscapesReceiver(child, receiverAliases, localNames,
                                              receiverMethodCallSites);
    }
    return false;
  case AstExpressionKind::SummonFrom: {
    bool result = false;
    for (const AstExpression& child : expression.children) {
      result = expressionDirectlyEscapesReceiver(child, receiverAliases, localNames,
                                                 receiverMethodCallSites) ||
               result;
    }
    return result;
  }
  case AstExpressionKind::SummonFromCase: {
    const auto savedAliases = receiverAliases;
    const auto savedLocals = localNames;
    if (!expression.text.empty() && expression.text != "_") {
      receiverAliases[expression.text] = false;
      localNames.insert(expression.text);
    }
    const bool result =
        !expression.children.empty() &&
        expressionDirectlyEscapesReceiver(expression.children.front(), receiverAliases,
                                          localNames, receiverMethodCallSites);
    receiverAliases = savedAliases;
    localNames = savedLocals;
    return result;
  }
  case AstExpressionKind::If:
  case AstExpressionKind::While:
  case AstExpressionKind::Unary:
  case AstExpressionKind::Binary:
  case AstExpressionKind::PolymorphicFunction:
  case AstExpressionKind::TupleLiteral: {
    bool result = false;
    for (const AstExpression& child : expression.children) {
      result = expressionDirectlyEscapesReceiver(child, receiverAliases, localNames,
                                                 receiverMethodCallSites) ||
               result;
    }
    return result;
  }
  }
  return false;
}

void Typechecker::recordDirectZoneReceiverEscape(const AstDeclaration& declaration,
                                                 const TypedDeclaration& typed) {
  std::unordered_map<std::string, bool> receiverAliases;
  std::unordered_set<std::string> localNames;
  for (const std::string& parameter : declaration.parameters) {
    const std::string name = parameterName(parameter);
    if (!name.empty()) {
      localNames.insert(name);
      receiverAliases[name] = false;
    }
  }
  std::vector<AstExpression> receiverMethodCallSites;
  if (expressionDirectlyEscapesReceiver(declaration.initializer, receiverAliases,
                                        localNames, &receiverMethodCallSites)) {
    directZoneReceiverEscapes_.insert(typed.symbolName);
  }
  receiverMethodCallSites_[typed.symbolName] = std::move(receiverMethodCallSites);
  std::unordered_set<std::string> implicitLocals;
  for (const std::string& parameter : declaration.parameters) {
    const std::string name = parameterName(parameter);
    if (!name.empty()) {
      implicitLocals.insert(name);
    }
  }
  collectImplicitReceiverMethodNames(declaration.initializer, implicitLocals,
                                     implicitReceiverMethodNames_[typed.symbolName]);
}

void Typechecker::collectImplicitReceiverMethodNames(
    const AstExpression& expression, std::unordered_set<std::string>& localNames,
    std::unordered_set<std::string>& methodNames) const {
  if (expression.kind == AstExpressionKind::Identifier) {
    if (!localNames.contains(expression.text)) {
      methodNames.insert(expression.text);
    }
    return;
  }
  if (expression.kind == AstExpressionKind::Block) {
    std::vector<std::string> introduced;
    for (const AstExpression& child : expression.children) {
      if (child.kind == AstExpressionKind::LocalDeclaration) {
        if (!child.children.empty()) {
          collectImplicitReceiverMethodNames(child.children.front(), localNames,
                                             methodNames);
        }
        if (localNames.insert(child.text).second) {
          introduced.push_back(child.text);
        }
        continue;
      }
      collectImplicitReceiverMethodNames(child, localNames, methodNames);
    }
    for (const std::string& name : introduced) {
      localNames.erase(name);
    }
    return;
  }
  if (expression.kind == AstExpressionKind::SummonFromCase) {
    const bool introduced = !expression.text.empty() && expression.text != "_" &&
                            localNames.insert(expression.text).second;
    for (const AstExpression& child : expression.children) {
      collectImplicitReceiverMethodNames(child, localNames, methodNames);
    }
    if (introduced) {
      localNames.erase(expression.text);
    }
    return;
  }
  for (const AstExpression& child : expression.children) {
    collectImplicitReceiverMethodNames(child, localNames, methodNames);
  }
}

void Typechecker::propagateZoneReceiverEffects() {
  std::unordered_map<std::string, std::unordered_set<std::string>> dependencies;
  for (const auto& [method, callSites] : receiverMethodCallSites_) {
    for (const AstExpression& callSite : callSites) {
      if (callSite.children.size() != 1) {
        continue;
      }
      const AstExpression& receiver = callSite.children.front();
      std::string owner;
      for (auto info = expressionTypes_.rbegin(); info != expressionTypes_.rend();
           ++info) {
        if (info->span.source == receiver.span.source &&
            info->span.start == receiver.span.start &&
            info->span.length == receiver.span.length) {
          owner =
              info->type.runtimeName.empty() ? info->type.name : info->type.runtimeName;
          break;
        }
      }
      auto members = memberScopes_.find(owner);
      if (members == memberScopes_.end()) {
        continue;
      }
      auto member = members->second.find(callSite.text);
      if (member != members->second.end() &&
          member->second.kind == AstDeclarationKind::Def) {
        dependencies[method].insert(member->second.symbolName);
      }
    }
  }
  for (const auto& [method, methodNames] : implicitReceiverMethodNames_) {
    auto members = memberScopes_.find(ownerNameOf(method));
    if (members == memberScopes_.end()) {
      continue;
    }
    for (const std::string& name : methodNames) {
      auto member = members->second.find(name);
      if (member != members->second.end() &&
          member->second.kind == AstDeclarationKind::Def) {
        dependencies[method].insert(member->second.symbolName);
      }
    }
  }

  auto hasUnsafeOverride = [&](const std::string& method) {
    const std::string owner = ownerNameOf(method);
    const std::string member = memberNameOf(method);
    if (owner.empty() || member.empty()) {
      return false;
    }
    for (const std::string& candidate : directZoneReceiverEscapes_) {
      if (memberNameOf(candidate) == member &&
          isSubtypeOf(ownerNameOf(candidate), owner)) {
        return true;
      }
    }
    return false;
  };

  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& [symbolName, symbol] : globalSymbols_) {
      if (symbol.kind != AstDeclarationKind::Def ||
          directZoneReceiverEscapes_.contains(symbolName)) {
        continue;
      }
      auto owner = globalSymbols_.find(ownerNameOf(symbolName));
      if (owner != globalSymbols_.end() &&
          (owner->second.kind == AstDeclarationKind::Class ||
           owner->second.kind == AstDeclarationKind::Trait) &&
          hasUnsafeOverride(symbolName)) {
        directZoneReceiverEscapes_.insert(symbolName);
        changed = true;
      }
    }
    for (const auto& [method, callees] : dependencies) {
      if (directZoneReceiverEscapes_.contains(method)) {
        continue;
      }
      for (const std::string& callee : callees) {
        if (directZoneReceiverEscapes_.contains(callee) || hasUnsafeOverride(callee)) {
          directZoneReceiverEscapes_.insert(method);
          changed = true;
          break;
        }
      }
    }
  }
}

bool Typechecker::analyzeZoneExpression(
    const AstExpression& expression,
    std::unordered_map<std::string, bool>& arenaReferences,
    std::unordered_set<std::string>& zoneLocals) {
  switch (expression.kind) {
  case AstExpressionKind::Empty:
  case AstExpressionKind::IntegerLiteral:
  case AstExpressionKind::FloatingLiteral:
  case AstExpressionKind::StringLiteral:
  case AstExpressionKind::CharLiteral:
  case AstExpressionKind::SymbolLiteral:
  case AstExpressionKind::BooleanLiteral:
  case AstExpressionKind::NullLiteral:
  case AstExpressionKind::This:
  case AstExpressionKind::Super:
  case AstExpressionKind::ModuleReference:
    return false;
  case AstExpressionKind::Identifier: {
    auto found = arenaReferences.find(expression.text);
    return found != arenaReferences.end() && found->second;
  }
  case AstExpressionKind::New:
    return true;
  case AstExpressionKind::LocalDeclaration: {
    const bool initializerIsArenaReference =
        !expression.children.empty() &&
        analyzeZoneExpression(expression.children.front(), arenaReferences, zoneLocals);
    zoneLocals.insert(expression.text);
    arenaReferences[expression.text] = initializerIsArenaReference;
    return false;
  }
  case AstExpressionKind::Block: {
    struct SavedLocal {
      std::string name;
      bool hadArenaReference = false;
      bool arenaReference = false;
      bool wasZoneLocal = false;
    };
    std::vector<SavedLocal> savedLocals;
    bool resultIsArenaReference = false;
    for (const AstExpression& child : expression.children) {
      if (child.kind == AstExpressionKind::LocalDeclaration) {
        auto existing = arenaReferences.find(child.text);
        savedLocals.push_back(
            SavedLocal{child.text, existing != arenaReferences.end(),
                       existing == arenaReferences.end() ? false : existing->second,
                       zoneLocals.contains(child.text)});
      }
      resultIsArenaReference =
          analyzeZoneExpression(child, arenaReferences, zoneLocals);
    }
    for (auto saved = savedLocals.rbegin(); saved != savedLocals.rend(); ++saved) {
      if (saved->hadArenaReference) {
        arenaReferences[saved->name] = saved->arenaReference;
      } else {
        arenaReferences.erase(saved->name);
      }
      if (saved->wasZoneLocal) {
        zoneLocals.insert(saved->name);
      } else {
        zoneLocals.erase(saved->name);
      }
    }
    return resultIsArenaReference;
  }
  case AstExpressionKind::Select:
    if (expression.children.size() != 1) {
      return false;
    }
    {
      const bool receiverIsArenaReference = analyzeZoneExpression(
          expression.children.front(), arenaReferences, zoneLocals);
      if (receiverIsArenaReference && selectedMethodMayEscapeZone(expression)) {
        diagnostics_.error(expression.span,
                           "Zone.scoped receiver may escape through method " +
                               expression.text);
      }
      return receiverIsArenaReference && expressionHasReferenceType(expression);
    }
  case AstExpressionKind::TypeApply:
    if (expression.children.size() != 1) {
      return false;
    }
    if (expression.children.front().kind == AstExpressionKind::Select &&
        expression.children.front().text == support::StdNames::AsInstanceOf &&
        !expression.children.front().children.empty()) {
      return analyzeZoneExpression(expression.children.front().children.front(),
                                   arenaReferences, zoneLocals);
    }
    (void)analyzeZoneExpression(expression.children.front(), arenaReferences,
                                zoneLocals);
    return false;
  case AstExpressionKind::Call: {
    if (expression.children.empty()) {
      return false;
    }
    if (isZoneScopedCall(expression)) {
      return false;
    }
    if (isZoneAllocBytesCall(expression)) {
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        if (analyzeZoneExpression(expression.children[i], arenaReferences,
                                  zoneLocals)) {
          diagnostics_.error(expression.children[i].span,
                             "Zone.allocBytes length cannot reference zone-owned "
                             "storage");
        }
      }
      return true;
    }
    if (isByteBufferWrapCall(expression)) {
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        (void)analyzeZoneExpression(expression.children[i], arenaReferences,
                                    zoneLocals);
      }
      return true;
    }
    const AstExpression& selected = expression.children.front();
    if (selected.kind == AstExpressionKind::Select && selected.children.size() == 1 &&
        isByteBufferOperationName(selected.text)) {
      bool byteBufferReceiver = false;
      const AstExpression& receiver = selected.children.front();
      for (auto info = expressionTypes_.rbegin(); info != expressionTypes_.rend();
           ++info) {
        if (info->span.source == receiver.span.source &&
            info->span.start == receiver.span.start &&
            info->span.length == receiver.span.length) {
          byteBufferReceiver = isByteBufferType(info->type);
          break;
        }
      }
      if (byteBufferReceiver) {
        const bool receiverIsArenaReference =
            analyzeZoneExpression(receiver, arenaReferences, zoneLocals);
        for (std::size_t i = 1; i < expression.children.size(); ++i) {
          if (analyzeZoneExpression(expression.children[i], arenaReferences,
                                    zoneLocals)) {
            diagnostics_.error(
                expression.children[i].span,
                "ByteBuffer state arguments cannot reference zone-owned storage");
          }
        }
        const bool returnsBuffer =
            selected.text == support::StdNames::ByteBufferPut ||
            selected.text == support::StdNames::ByteBufferPutShort ||
            selected.text == support::StdNames::ByteBufferClear ||
            selected.text == support::StdNames::ByteBufferFlip ||
            selected.text == support::StdNames::ByteBufferRewind ||
            selected.text == support::StdNames::ByteBufferMark ||
            selected.text == support::StdNames::ByteBufferReset ||
            ((selected.text == support::StdNames::ByteBufferPosition ||
              selected.text == support::StdNames::ByteBufferLimit) &&
             expression.children.size() == 2);
        return returnsBuffer && receiverIsArenaReference;
      }
    }
    if (!nativeBytesOperation(expression).empty()) {
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        const bool argumentIsArenaReference =
            analyzeZoneExpression(expression.children[i], arenaReferences, zoneLocals);
        if (argumentIsArenaReference && i != 1) {
          diagnostics_.error(
              expression.children[i].span,
              "NativeBytes value arguments cannot reference zone-owned storage");
        }
      }
      return false;
    }
    const AstExpression& callee = expression.children.front();
    if (callee.kind == AstExpressionKind::New) {
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        (void)analyzeZoneExpression(expression.children[i], arenaReferences,
                                    zoneLocals);
      }
      return true;
    }

    const bool calleeIsArenaReference =
        analyzeZoneExpression(callee, arenaReferences, zoneLocals);
    for (std::size_t i = 1; i < expression.children.size(); ++i) {
      if (analyzeZoneExpression(expression.children[i], arenaReferences, zoneLocals)) {
        diagnostics_.error(
            expression.children[i].span,
            "Zone.scoped reference cannot be passed to an ordinary call; it "
            "may escape the zone");
      }
    }
    return calleeIsArenaReference && expressionHasReferenceType(expression);
  }
  case AstExpressionKind::Assign: {
    if (expression.children.size() != 2) {
      return false;
    }
    const AstExpression& target = expression.children.front();
    const bool assignedIsArenaReference =
        analyzeZoneExpression(expression.children.back(), arenaReferences, zoneLocals);
    if (target.kind == AstExpressionKind::Identifier) {
      if (assignedIsArenaReference && !zoneLocals.contains(target.text)) {
        diagnostics_.error(
            expression.children.back().span,
            "Zone.scoped reference cannot be assigned to an outer variable");
      }
      if (zoneLocals.contains(target.text)) {
        arenaReferences[target.text] = assignedIsArenaReference;
      }
      return false;
    }
    if (target.kind == AstExpressionKind::Select && !target.children.empty()) {
      const bool receiverIsArenaReference =
          analyzeZoneExpression(target.children.front(), arenaReferences, zoneLocals);
      if (assignedIsArenaReference && !receiverIsArenaReference) {
        diagnostics_.error(
            expression.children.back().span,
            "Zone.scoped reference cannot be stored in an object outside the zone");
      }
    }
    return false;
  }
  case AstExpressionKind::Return:
    for (const AstExpression& child : expression.children) {
      (void)analyzeZoneExpression(child, arenaReferences, zoneLocals);
    }
    diagnostics_.error(
        expression.span,
        "return inside Zone.scoped is not supported until abnormal-exit cleanup "
        "is available");
    return false;
  case AstExpressionKind::Throw:
    for (const AstExpression& child : expression.children) {
      (void)analyzeZoneExpression(child, arenaReferences, zoneLocals);
    }
    return false;
  case AstExpressionKind::Try: {
    bool resultIsArenaReference = false;
    for (const AstExpression& child : expression.children) {
      if (child.kind == AstExpressionKind::Finally) {
        (void)analyzeZoneExpression(child, arenaReferences, zoneLocals);
      } else {
        resultIsArenaReference =
            analyzeZoneExpression(child, arenaReferences, zoneLocals) ||
            resultIsArenaReference;
      }
    }
    return resultIsArenaReference;
  }
  case AstExpressionKind::Catch: {
    const auto savedReferences = arenaReferences;
    const auto savedZoneLocals = zoneLocals;
    arenaReferences[expression.text] = false;
    zoneLocals.insert(expression.text);
    const bool result =
        !expression.children.empty() &&
        analyzeZoneExpression(expression.children.front(), arenaReferences, zoneLocals);
    arenaReferences = savedReferences;
    zoneLocals = savedZoneLocals;
    return result;
  }
  case AstExpressionKind::Finally:
    for (const AstExpression& child : expression.children) {
      (void)analyzeZoneExpression(child, arenaReferences, zoneLocals);
    }
    return false;
  case AstExpressionKind::SummonFrom: {
    bool resultIsArenaReference = false;
    for (const AstExpression& child : expression.children) {
      resultIsArenaReference =
          analyzeZoneExpression(child, arenaReferences, zoneLocals) ||
          resultIsArenaReference;
    }
    return resultIsArenaReference;
  }
  case AstExpressionKind::SummonFromCase: {
    const auto savedReferences = arenaReferences;
    const auto savedZoneLocals = zoneLocals;
    if (!expression.text.empty() && expression.text != "_") {
      arenaReferences[expression.text] = false;
      zoneLocals.insert(expression.text);
    }
    const bool result =
        !expression.children.empty() &&
        analyzeZoneExpression(expression.children.front(), arenaReferences, zoneLocals);
    arenaReferences = savedReferences;
    zoneLocals = savedZoneLocals;
    return result;
  }
  case AstExpressionKind::If: {
    if (expression.children.empty()) {
      return false;
    }
    (void)analyzeZoneExpression(expression.children.front(), arenaReferences,
                                zoneLocals);
    bool resultIsArenaReference = false;
    for (std::size_t i = 1; i < expression.children.size(); ++i) {
      resultIsArenaReference =
          analyzeZoneExpression(expression.children[i], arenaReferences, zoneLocals) ||
          resultIsArenaReference;
    }
    return resultIsArenaReference;
  }
  case AstExpressionKind::While:
    for (const AstExpression& child : expression.children) {
      (void)analyzeZoneExpression(child, arenaReferences, zoneLocals);
    }
    return false;
  case AstExpressionKind::Unary:
    for (const AstExpression& child : expression.children) {
      (void)analyzeZoneExpression(child, arenaReferences, zoneLocals);
    }
    return false;
  case AstExpressionKind::Binary:
    for (const AstExpression& child : expression.children) {
      const bool arenaReference =
          analyzeZoneExpression(child, arenaReferences, zoneLocals);
      if (expression.text == "*:" && arenaReference) {
        diagnostics_.error(child.span,
                           "Zone.scoped reference cannot be stored in a tuple");
      }
    }
    return false;
  case AstExpressionKind::TupleLiteral:
    for (const AstExpression& child : expression.children) {
      if (analyzeZoneExpression(child, arenaReferences, zoneLocals)) {
        diagnostics_.error(child.span,
                           "Zone.scoped reference cannot be stored in a tuple");
      }
    }
    return false;
  case AstExpressionKind::PolymorphicFunction:
    for (const AstExpression& child : expression.children) {
      (void)analyzeZoneExpression(child, arenaReferences, zoneLocals);
    }
    return false;
  }
  return false;
}

TypeInfo Typechecker::inferNewType(const AstExpression& expression, Scope& scope) {
  if (expression.text.empty()) {
    return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
  }

  const SymbolInfo* constructor = qualifiedPathSymbol(expression.text, &scope);
  if (constructor != nullptr && constructor->kind == AstDeclarationKind::Class) {
    if (!constructor->typeParameters.empty()) {
      diagnostics_.error(expression.span,
                         "generic class " + constructor->name + " requires " +
                             std::to_string(constructor->typeParameters.size()) +
                             " explicit type arguments");
    }
    return constructor->type;
  }

  if (constructor != nullptr) {
    diagnostics_.error(expression.span,
                       "constructor target is not a class: " + expression.text);
  } else {
    diagnostics_.error(expression.span, "unresolved class: " + expression.text);
  }
  return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
}

TypeInfo Typechecker::inferSelectType(const AstExpression& expression, Scope& scope) {
  if (expression.text == support::StdNames::StringLength &&
      expression.children.size() == 1) {
    const TypeInfo receiver = inferExpressionType(expression.children.front(), scope);
    if (receiver.kind == SimpleTypeKind::String) {
      return TypeInfo{SimpleTypeKind::Int, "Int"};
    }
    if (receiver.kind == SimpleTypeKind::Object &&
        !arrayElementTypeName(receiver.name).empty()) {
      return TypeInfo{SimpleTypeKind::Int, "Int"};
    }
  }
  if ((expression.text == support::StdNames::ToByte ||
       expression.text == support::StdNames::ToShort ||
       expression.text == support::StdNames::ToInt) &&
      expression.children.size() == 1) {
    const TypeInfo receiver = inferExpressionType(expression.children.front(), scope);
    const bool supportedReceiver = receiver.kind == SimpleTypeKind::Byte ||
                                   receiver.kind == SimpleTypeKind::Short ||
                                   receiver.kind == SimpleTypeKind::Int ||
                                   receiver.kind == SimpleTypeKind::Unknown;
    if (!supportedReceiver) {
      diagnostics_.error(expression.span,
                         expression.text +
                             " is currently supported for Byte, Short, and Int");
      return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
    }
    if (expression.text == support::StdNames::ToByte) {
      return TypeInfo{SimpleTypeKind::Byte, "Byte"};
    }
    if (expression.text == support::StdNames::ToShort) {
      return TypeInfo{SimpleTypeKind::Short, "Short"};
    }
    return TypeInfo{SimpleTypeKind::Int, "Int"};
  }
  if (expression.text == support::StdNames::ToString &&
      expression.children.size() == 1) {
    const TypeInfo receiver = inferExpressionType(expression.children.front(), scope);
    if (const SymbolInfo* member =
            knownMemberForReceiverType(receiver, expression.text)) {
      return member->type;
    }
    switch (receiver.kind) {
    case SimpleTypeKind::String:
    case SimpleTypeKind::Unit:
    case SimpleTypeKind::Boolean:
    case SimpleTypeKind::Byte:
    case SimpleTypeKind::Short:
    case SimpleTypeKind::Int:
    case SimpleTypeKind::Long:
    case SimpleTypeKind::Float:
    case SimpleTypeKind::Double:
    case SimpleTypeKind::Char:
    case SimpleTypeKind::Symbol:
    case SimpleTypeKind::Null:
    case SimpleTypeKind::Object:
      return TypeInfo{SimpleTypeKind::String, "String"};
    case SimpleTypeKind::Unknown:
      return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
    default:
      diagnostics_.error(expression.span,
                         "toString is currently supported for String, primitive, "
                         "Null, and object receivers");
      return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
    }
  }
  if (expression.text == support::StdNames::HashCode &&
      expression.children.size() == 1) {
    const TypeInfo receiver = inferExpressionType(expression.children.front(), scope);
    if (const SymbolInfo* member =
            knownMemberForReceiverType(receiver, expression.text)) {
      return member->type;
    }
    if (isCompilerKnownHashCodeReceiver(receiver)) {
      return TypeInfo{SimpleTypeKind::Int, "Int"};
    }
    diagnostics_.error(expression.span,
                       "hashCode is currently supported for Unit, primitive, "
                       "String, Symbol, Null, and object receivers");
    return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
  }

  const bool tupleOperation =
      expression.text == support::StdNames::TupleHead ||
      expression.text == support::StdNames::TupleInit ||
      expression.text == support::StdNames::TupleLast ||
      expression.text == support::StdNames::TupleTail ||
      expression.text == support::StdNames::TupleSize;
  if (tupleOperation && expression.children.size() == 1) {
    const TypeInfo receiver =
        inferExpressionType(expression.children.front(), scope);
    const bool emptyTuple =
        receiver.name == support::StdNames::ScalaEmptyTuple ||
        receiver.runtimeName == support::StdNames::ScalaEmptyTuple;
    const std::string constructor =
        receiver.typeConstructorName.empty() ? receiver.runtimeName
                                             : receiver.typeConstructorName;
    const std::optional<std::size_t> arity =
        tupleArityForConstructor(constructor);
    const bool abstractTuple =
        receiver.name == support::StdNames::ScalaTuple ||
        receiver.runtimeName == support::StdNames::ScalaTuple;
    if (emptyTuple || arity.has_value() || abstractTuple) {
      if (!emptyTuple &&
          (!arity.has_value() || receiver.typeArguments.size() != *arity)) {
        diagnostics_.error(expression.span, expression.text +
                                                " requires a concrete tuple receiver");
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }
      if (expression.text == support::StdNames::TupleSize) {
        const std::string literal =
            std::to_string(emptyTuple ? 0 : receiver.typeArguments.size());
        TypeInfo size{SimpleTypeKind::Int, literal};
        size.runtimeName = "Int";
        size.singletonLiteral = literal;
        return size;
      }
      if (emptyTuple) {
        diagnostics_.error(expression.span, expression.text +
                                                " is not available on EmptyTuple");
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }
      if (expression.text == support::StdNames::TupleHead) {
        return receiver.typeArguments.front();
      }
      if (expression.text == support::StdNames::TupleLast) {
        return receiver.typeArguments.back();
      }
      if (receiver.typeArguments.size() == 1) {
        auto empty = globalSymbols_.find(
            std::string(support::StdNames::ScalaEmptyTuple));
        if (empty == globalSymbols_.end()) {
          diagnostics_.error(
              expression.span,
              "unresolved tuple " + expression.text +
                  " result: scala.EmptyTuple");
          return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
        }
        return empty->second.type;
      }
      const std::string resultConstructor =
          std::string(support::StdNames::ScalaTuple) +
          std::to_string(receiver.typeArguments.size() - 1);
      auto result = globalSymbols_.find(resultConstructor);
      if (result == globalSymbols_.end()) {
        diagnostics_.error(
            expression.span, "unresolved tuple " + expression.text +
                                 " constructor: " + resultConstructor);
        return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
      }
      const bool removesHead =
          expression.text == support::StdNames::TupleTail;
      std::vector<TypeInfo> resultElements(
          removesHead ? std::next(receiver.typeArguments.begin())
                      : receiver.typeArguments.begin(),
          removesHead ? receiver.typeArguments.end()
                      : std::prev(receiver.typeArguments.end()));
      return specializeResolvedTypeApplication(
                 result->second, resultElements, expression.span, true)
          .type;
    }
  }

  std::optional<SymbolInfo> resolvedMember;
  std::optional<TypeInfo> selectedReceiverType;
  if (expression.children.size() == 1) {
    selectedReceiverType =
        inferExpressionType(expression.children.front(), scope);
    resolvedMember =
        resolvedMemberForReceiverType(*selectedReceiverType, expression.text);
  }
  const SymbolInfo* member =
      resolvedMember.has_value() ? &*resolvedMember
                                 : selectedMember(expression, scope);
  if (member == nullptr) {
    return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
  }
  SymbolInfo specializedMember =
      resolvedMember.has_value() ? *resolvedMember : *member;
  if (!resolvedMember.has_value() && expression.children.size() == 1) {
    if (!selectedReceiverType.has_value()) {
      selectedReceiverType =
          inferExpressionType(expression.children.front(), scope);
    }
    specializedMember =
        specializeMemberForReceiver(*member, *selectedReceiverType);
  }
  if (specializedMember.polymorphicFunctionValue != nullptr) {
    diagnostics_.error(
        expression.span,
        "polymorphic function value " + specializedMember.name +
            " must be invoked directly with one explicit type argument and one "
            "value argument");
    return specializedMember.type;
  }
  if ((specializedMember.kind == AstDeclarationKind::Class ||
       specializedMember.kind == AstDeclarationKind::Trait) &&
      companionTypeNames_.contains(specializedMember.symbolName)) {
    auto companion = globalSymbols_.find(specializedMember.symbolName + '$');
    if (companion != globalSymbols_.end()) {
      return companion->second.type;
    }
  }
  if (!specializedMember.typeParameters.empty()) {
    diagnostics_.error(expression.span,
                       "generic method " + specializedMember.name + " requires " +
                           std::to_string(specializedMember.typeParameters.size()) +
                           " explicit type arguments");
  }
  if (specializedMember.kind == AstDeclarationKind::Def &&
      specializedMember.isInline && specializedMember.typeParameters.empty() &&
      specializedMember.parameters.empty() &&
      specializedMember.parameterClauseSizes.empty() &&
      expression.children.size() == 1 && selectedReceiverType.has_value()) {
    if (std::optional<TypeInfo> transparentResult = recordInlineApplication(
            expression, specializedMember, {}, {}, {},
            &expression.children.front(), &*selectedReceiverType, scope,
            nullptr)) {
      return *transparentResult;
    }
  }
  TypeInfo selected = staticExpressionType(specializedMember.type);
  if (expression.children.size() != 1 ||
      (!isAbstractTypeMember(selected) && selected.dependentMemberName.empty())) {
    return selected;
  }

  const AstExpression& receiver = expression.children.front();
  if (receiver.kind != AstExpressionKind::Identifier) {
    return selected;
  }
  auto receiverSymbol = scope.find(receiver.text);
  if (receiverSymbol == scope.end() ||
      (receiverSymbol->second.kind != AstDeclarationKind::Val &&
       receiverSymbol->second.kind != AstDeclarationKind::Object)) {
    return selected;
  }

  const std::size_t dot = selected.name.rfind('.');
  const std::string memberName =
      !selected.dependentMemberName.empty() ? selected.dependentMemberName
      : dot == std::string::npos            ? std::string{}
                                            : selected.name.substr(dot + 1);
  if (memberName.empty()) {
    return selected;
  }
  if (isAbstractTypeMember(selected)) {
    selected.name = receiver.text + "." + memberName;
  }
  selected.dependentOwnerName = receiverSymbol->second.type.name;
  selected.dependentMemberName = memberName;
  selected.dependentPathName = receiver.text + "." + memberName;
  selected.pathDependent = true;
  selected.typeProjection = false;
  return selected;
}

TypeInfo Typechecker::inferAssignType(const AstExpression& expression, Scope& scope) {
  if (expression.children.size() != 2) {
    return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
  }

  const AstExpression& target = expression.children.front();
  const AstExpression& assignedValue = expression.children.back();
  TypeInfo assignedType = inferExpressionType(assignedValue, scope);

  if (target.kind == AstExpressionKind::Call && !target.children.empty()) {
    const AstExpression& arrayExpression = target.children.front();
    if (const std::string elementTypeName =
            inferArrayElementTypeName(arrayExpression, scope);
        !elementTypeName.empty()) {
      if (target.children.size() != 2) {
        diagnostics_.error(target.span,
                           "array assignment requires exactly one Int index");
      } else {
        const TypeInfo indexType = inferExpressionType(target.children[1], scope);
        if (indexType.kind != SimpleTypeKind::Int &&
            indexType.kind != SimpleTypeKind::Unknown) {
          diagnostics_.error(target.children[1].span, "array index must have type Int");
        }
      }
      const TypeInfo elementType =
          typeFromDeclaredName(elementTypeName, &scope, &arrayExpression.span);
      const bool arrayElementConforms =
          assignedType.kind == SimpleTypeKind::Unknown ||
          (isAnyArrayElementType(elementType)
               ? isSupportedAnyArrayValueType(assignedType)
           : elementType.kind == SimpleTypeKind::Object
               ? isAssignable(elementType, assignedType)
               : elementType.kind == assignedType.kind);
      if (!arrayElementConforms) {
        diagnostics_.error(
            assignedValue.span,
            "array assignment value does not conform to the element type");
      }
      return TypeInfo{SimpleTypeKind::Unit, "Unit"};
    }
  }

  const SymbolInfo* targetSymbol = nullptr;
  SymbolInfo specializedTarget;
  if (target.kind == AstExpressionKind::Identifier ||
      target.kind == AstExpressionKind::Select) {
    (void)inferExpressionType(target, scope);
    targetSymbol = selectedMember(target, scope);
    if (targetSymbol != nullptr && target.kind == AstExpressionKind::Select &&
        target.children.size() == 1) {
      const TypeInfo receiver = inferExpressionType(target.children.front(), scope);
      specializedTarget = specializeMemberForReceiver(*targetSymbol, receiver);
      targetSymbol = &specializedTarget;
    }
  } else {
    diagnostics_.error(target.span,
                       "assignment target must be an identifier or member selection");
  }

  if (targetSymbol == nullptr) {
    return TypeInfo{SimpleTypeKind::Unit, "Unit"};
  }
  if (targetSymbol->kind != AstDeclarationKind::Var) {
    diagnostics_.error(target.span,
                       "cannot assign to immutable value: " + targetSymbol->name);
  } else {
    const bool targetsAny = isAnyArrayElementType(targetSymbol->type);
    const bool assignmentConforms =
        targetsAny ? isSupportedAnyArrayValueType(assignedType)
                   : isAssignable(targetSymbol->type, assignedType);
    if (!assignmentConforms) {
      diagnostics_.error(assignedValue.span, "assignment value type " +
                                                 assignedType.name +
                                                 " does not conform to target type " +
                                                 targetSymbol->type.name);
    }
  }
  return TypeInfo{SimpleTypeKind::Unit, "Unit"};
}

std::string Typechecker::inferArrayElementTypeName(const AstExpression& expression,
                                                   Scope& scope) {
  if (expression.kind == AstExpressionKind::Identifier) {
    const auto declaration = scope.find(expression.text);
    if (declaration == scope.end() ||
        declaration->second.kind == AstDeclarationKind::Def) {
      return {};
    }
  } else if (expression.kind != AstExpressionKind::Call) {
    return {};
  }

  const TypeInfo receiverType = inferExpressionType(expression, scope);
  if (receiverType.kind != SimpleTypeKind::Object) {
    return {};
  }
  return arrayElementTypeName(receiverType.name);
}

const SymbolInfo* Typechecker::selectedMember(const AstExpression& expression,
                                              Scope& scope) {
  if (expression.kind == AstExpressionKind::Identifier) {
    auto found = scope.find(expression.text);
    if (found == scope.end()) {
      diagnostics_.error(expression.span, "unresolved identifier: " + expression.text);
      return nullptr;
    }
    return &found->second;
  }

  if (expression.children.size() != 1 || expression.text.empty()) {
    return nullptr;
  }

  const AstExpression& receiverExpression = expression.children.front();
  if (receiverExpression.kind == AstExpressionKind::Super &&
      (receiverExpression.text.empty() || receiverExpression.text == "super")) {
    auto super = scope.find("super");
    if (super == scope.end()) {
      diagnostics_.error(expression.span,
                         "super is only available in classes with a parent");
      return nullptr;
    }
    const std::vector<const SymbolInfo*> candidates =
        inheritedMembers(super->second.parentSymbolNames, expression.text);
    for (const SymbolInfo* candidate : candidates) {
      if (candidate->kind != AstDeclarationKind::Def || candidate->hasImplementation) {
        return candidate;
      }
    }
    if (!candidates.empty()) {
      return candidates.front();
    }
    diagnostics_.error(expression.span, "unresolved super member: " + expression.text);
    return nullptr;
  }

  TypeInfo receiver = inferExpressionType(receiverExpression, scope);
  if (receiver.kind == SimpleTypeKind::Unknown) {
    return nullptr;
  }
  if (receiver.kind != SimpleTypeKind::Object) {
    diagnostics_.error(expression.span, "cannot select member " + expression.text +
                                            " from non-object type " + receiver.name);
    return nullptr;
  }

  if (const SymbolInfo* member =
          knownMemberForReceiverType(receiver, expression.text)) {
    return member;
  }
  if (receiver.compositeKind == CompositeTypeKind::Union) {
    diagnostics_.error(expression.span,
                       "unresolved member: " + expression.text + " on union type " +
                           receiver.name +
                           "; union members must come from a common base type");
  } else if (receiver.compositeKind == CompositeTypeKind::Intersection) {
    diagnostics_.error(expression.span,
                       "unresolved or incompatible member: " + expression.text +
                           " on intersection type " + receiver.name);
  } else {
    diagnostics_.error(expression.span, "unresolved member: " + expression.text +
                                            " on " + receiver.name);
  }
  return nullptr;
}

const SymbolInfo*
Typechecker::knownMemberForReceiverType(const TypeInfo& receiver,
                                        const std::string& memberName) const {
  if (receiver.kind != SimpleTypeKind::Object) {
    return nullptr;
  }

  const auto memberForOwner = [&](const std::string& owner) -> const SymbolInfo* {
    auto members = memberScopes_.find(owner);
    if (members == memberScopes_.end()) {
      return nullptr;
    }
    auto member = members->second.find(memberName);
    return member == members->second.end() ? nullptr : &member->second;
  };
  if (receiver.compositeKind == CompositeTypeKind::Union) {
    for (const std::string& commonBase : baseTypeNamesFor(receiver)) {
      if (const SymbolInfo* member = memberForOwner(commonBase)) {
        return member;
      }
    }
    return nullptr;
  }

  if (receiver.compositeKind == CompositeTypeKind::Intersection) {
    if (!resolvedMemberForReceiverType(receiver, memberName).has_value()) {
      return nullptr;
    }
    for (const TypeInfo& operand : receiver.compositeTypes) {
      const SymbolInfo* candidate = knownMemberForReceiverType(operand, memberName);
      if (candidate != nullptr) {
        return candidate;
      }
    }
    return nullptr;
  }

  std::string memberOwner = receiver.typeConstructorName.empty()
                                ? receiver.name
                                : receiver.typeConstructorName;
  if (const SymbolInfo* member = memberForOwner(memberOwner)) {
    return member;
  }
  if (!receiver.runtimeName.empty() && receiver.runtimeName != memberOwner) {
    return memberForOwner(receiver.runtimeName);
  }
  return nullptr;
}

std::optional<SymbolInfo> Typechecker::resolvedMemberForReceiverType(
    const TypeInfo& receiver, const std::string& memberName) const {
  if (receiver.kind != SimpleTypeKind::Object) {
    return std::nullopt;
  }
  if (receiver.compositeKind != CompositeTypeKind::Intersection) {
    const SymbolInfo* member =
        knownMemberForReceiverType(receiver, memberName);
    if (member == nullptr) {
      return std::nullopt;
    }
    return specializeMemberForReceiver(*member, receiver);
  }

  std::optional<SymbolInfo> merged;
  for (const TypeInfo& operand : receiver.compositeTypes) {
    std::optional<SymbolInfo> candidate =
        resolvedMemberForReceiverType(operand, memberName);
    if (!candidate.has_value()) {
      continue;
    }
    if (!merged.has_value()) {
      merged = std::move(candidate);
      continue;
    }
    if (!memberShapesSupportResultMeet(*merged, *candidate)) {
      return std::nullopt;
    }
    if (merged->type.name == candidate->type.name &&
        merged->type.compositeKind == candidate->type.compositeKind) {
      continue;
    }
    if (isAssignable(merged->type, candidate->type)) {
      merged->type = candidate->type;
    } else if (!isAssignable(candidate->type, merged->type)) {
      merged->type = makeCompositeType(
          CompositeTypeKind::Intersection,
          {std::move(merged->type), std::move(candidate->type)});
    }
  }
  return merged;
}

const SymbolInfo* Typechecker::qualifiedPathSymbol(const std::string& name,
                                                   const Scope* scope) const {
  const std::string normalized = trim(name);
  if (normalized.empty()) {
    return nullptr;
  }

  if (normalized.find('.') == std::string::npos) {
    if (scope != nullptr) {
      auto visible = scope->find(normalized);
      if (visible != scope->end()) {
        return &visible->second;
      }
    }
    auto global = globalSymbols_.find(normalized);
    return global == globalSymbols_.end() ? nullptr : &global->second;
  }

  if (auto global = globalSymbols_.find(normalized); global != globalSymbols_.end()) {
    return &global->second;
  }

  const SymbolInfo* resolved = nullptr;
  std::size_t segmentStart = 0;
  const std::size_t firstDot = normalized.find('.');
  if (scope != nullptr) {
    auto visible = scope->find(normalized.substr(0, firstDot));
    if (visible != scope->end()) {
      resolved = &visible->second;
      segmentStart = firstDot + 1;
    }
  }

  if (resolved == nullptr) {
    std::size_t separator = normalized.rfind('.');
    while (separator != std::string::npos) {
      auto global = globalSymbols_.find(normalized.substr(0, separator));
      if (global != globalSymbols_.end()) {
        resolved = &global->second;
        segmentStart = separator + 1;
        break;
      }
      if (separator == 0) {
        break;
      }
      separator = normalized.rfind('.', separator - 1);
    }
  }
  if (resolved == nullptr) {
    return nullptr;
  }

  if ((resolved->kind == AstDeclarationKind::Class ||
       resolved->kind == AstDeclarationKind::Trait) &&
      companionTypeNames_.contains(resolved->symbolName)) {
    auto companion = globalSymbols_.find(resolved->symbolName + '$');
    if (companion == globalSymbols_.end()) {
      return nullptr;
    }
    resolved = &companion->second;
  } else if (resolved->kind != AstDeclarationKind::Object) {
    return nullptr;
  }

  while (segmentStart < normalized.size()) {
    const std::size_t nextDot = normalized.find('.', segmentStart);
    const std::string segment = normalized.substr(
        segmentStart,
        nextDot == std::string::npos ? std::string::npos : nextDot - segmentStart);
    const std::string memberOwner = resolved->type.typeConstructorName.empty()
                                        ? resolved->type.name
                                        : resolved->type.typeConstructorName;
    auto members = memberScopes_.find(memberOwner);
    if (members == memberScopes_.end()) {
      return nullptr;
    }
    auto member = members->second.find(segment);
    if (member == members->second.end()) {
      return nullptr;
    }
    resolved = &member->second;
    if (nextDot == std::string::npos) {
      return resolved;
    }
    if ((resolved->kind == AstDeclarationKind::Class ||
         resolved->kind == AstDeclarationKind::Trait) &&
        companionTypeNames_.contains(resolved->symbolName)) {
      auto companion = globalSymbols_.find(resolved->symbolName + '$');
      if (companion == globalSymbols_.end()) {
        return nullptr;
      }
      resolved = &companion->second;
    } else if (resolved->kind != AstDeclarationKind::Object) {
      return nullptr;
    }
    segmentStart = nextDot + 1;
  }
  return resolved;
}

const SymbolInfo* Typechecker::typeSymbolForDeclaredName(const std::string& name,
                                                         const Scope* scope) const {
  const AppliedTypeSyntax applied = parseAppliedTypeSyntax(name);
  const std::string normalized = applied.applied && !applied.constructor.empty()
                                     ? applied.constructor
                                     : trim(name);
  return qualifiedPathSymbol(normalized, scope);
}

void Typechecker::validateInheritance(const AstDeclaration& declaration,
                                      TypedDeclaration& typed,
                                      const Scope& scope) const {
  const std::vector<std::string> parentNames = sourceParentTypes(declaration);
  if (parentNames.empty()) {
    typed.declaredType.clear();
    typed.parentTypes.clear();
    typed.parentTypeInfos.clear();
    return;
  }

  typed.parentTypes.clear();
  typed.parentTypeInfos.clear();
  std::unordered_set<std::string> seen;
  bool sawClassParent = false;
  for (std::size_t i = 0; i < parentNames.size(); ++i) {
    const SymbolInfo* parent = typeSymbolForDeclaredName(parentNames[i], &scope);
    if (parent == nullptr) {
      diagnostics_.error(declaration.span, "unresolved parent type: " + parentNames[i]);
      continue;
    }
    const AppliedTypeSyntax appliedParent = parseAppliedTypeSyntax(parentNames[i]);
    TypeInfo resolvedParent = parent->type;
    if (!parent->typeParameters.empty()) {
      if (!appliedParent.applied) {
        diagnostics_.error(declaration.span, "generic parent " + parent->name +
                                                 " requires explicit type arguments");
        continue;
      }
      resolvedParent = typeFromDeclaredName(parentNames[i], &scope, &declaration.span);
    }
    if (parent->typeParameters.empty() && appliedParent.applied) {
      (void)typeFromDeclaredName(parentNames[i], &scope, &declaration.span);
      continue;
    }
    if (!isInheritableDeclaration(parent->kind)) {
      diagnostics_.error(declaration.span,
                         "cannot extend non-class or non-trait: " + parent->name);
      continue;
    }
    if (parent->symbolName == typed.symbolName) {
      diagnostics_.error(declaration.span,
                         "declaration cannot extend itself: " + declaration.name);
      continue;
    }
    if (!seen.insert(parent->symbolName).second) {
      diagnostics_.error(declaration.span, "duplicate parent type: " + parent->name);
      continue;
    }
    if (declaration.kind == AstDeclarationKind::Trait &&
        parent->kind != AstDeclarationKind::Trait) {
      diagnostics_.error(declaration.span,
                         "trait " + declaration.name +
                             " can only extend another trait in this MVP");
      continue;
    }
    if (parent->kind == AstDeclarationKind::Class) {
      if (sawClassParent) {
        diagnostics_.error(declaration.span,
                           "multiple class parents are not supported: " +
                               declaration.name);
        continue;
      }
      if (i != 0) {
        diagnostics_.error(declaration.span,
                           "class parent must be the first extends parent: " +
                               parent->name);
        continue;
      }
      sawClassParent = true;
    }
    typed.parentTypes.push_back(parent->symbolName);
    typed.parentTypeInfos.push_back(std::move(resolvedParent));
  }
  typed.declaredType =
      typed.parentTypes.empty() ? std::string{} : typed.parentTypes.front();

  bool hasCycle = false;
  for (const std::string& parent : typed.parentTypes) {
    std::unordered_set<std::string> visited;
    if (inheritanceReaches(parent, typed.symbolName, globalSymbols_, visited)) {
      hasCycle = true;
      break;
    }
  }
  bool linearizationCycle = false;
  bool consistent = true;
  (void)linearizedParentsFor(typed.parentTypes, globalSymbols_, &linearizationCycle,
                             &consistent);
  if (hasCycle || linearizationCycle) {
    diagnostics_.error(declaration.span,
                       "cyclic inheritance involving " + declaration.name);
  } else if (!consistent) {
    diagnostics_.error(declaration.span,
                       "inconsistent parent linearization for " + declaration.name);
  }
}

void Typechecker::validateParentConstructorArguments(const AstDeclaration& declaration,
                                                     const TypedDeclaration& typed,
                                                     Scope& scope) {
  if (typed.declaredType.empty()) {
    if (!declaration.parentArguments.empty()) {
      diagnostics_.error(declaration.span,
                         "parent constructor arguments require an extends parent");
    }
    return;
  }

  auto parent = globalSymbols_.find(typed.declaredType);
  if (parent == globalSymbols_.end()) {
    return;
  }
  if (parent->second.kind != AstDeclarationKind::Class) {
    if (!declaration.parentArguments.empty()) {
      diagnostics_.error(declaration.span,
                         "parent constructor arguments require a class parent");
    }
    return;
  }

  SymbolInfo specializedParent = parent->second;
  if (!typed.parentTypeInfos.empty()) {
    specializedParent =
        specializeMemberForReceiver(parent->second, typed.parentTypeInfos.front());
  }
  const std::vector<TypeInfo>& expected = specializedParent.parameterTypes;
  if (declaration.parentArguments.size() != expected.size()) {
    diagnostics_.error(declaration.span,
                       "parent constructor for " + parent->second.name + " has " +
                           std::to_string(declaration.parentArguments.size()) +
                           " arguments but expected " +
                           std::to_string(expected.size()));
  }

  const std::size_t checkedArguments =
      std::min(declaration.parentArguments.size(), expected.size());
  for (std::size_t i = 0; i < checkedArguments; ++i) {
    TypeInfo actual = inferExpressionType(declaration.parentArguments[i], scope);
    const bool targetsAny = isAnyArrayElementType(expected[i]);
    const bool argumentConforms = targetsAny ? isSupportedAnyArrayValueType(actual)
                                             : isAssignable(expected[i], actual);
    if (!argumentConforms) {
      diagnostics_.error(declaration.parentArguments[i].span,
                         "parent constructor argument " + std::to_string(i) +
                             " of type " + actual.name +
                             " does not conform to field type " + expected[i].name);
    }
  }
}

std::vector<const SymbolInfo*>
Typechecker::inheritedMembers(const std::vector<std::string>& parentSymbolNames,
                              const std::string& memberName) const {
  std::vector<const SymbolInfo*> result;
  if (parentSymbolNames.empty() || memberName.empty()) {
    return result;
  }

  std::unordered_set<std::string> seen;
  for (const std::string& parentName :
       linearizedParentsFor(parentSymbolNames, globalSymbols_)) {
    auto parentScope = declaredMemberScopes_.find(parentName);
    if (parentScope != declaredMemberScopes_.end()) {
      auto member = parentScope->second.find(memberName);
      if (member != parentScope->second.end() &&
          seen.insert(member->second.symbolName).second) {
        result.push_back(&member->second);
      }
    }
  }
  return result;
}

std::vector<SymbolInfo> Typechecker::specializedInheritedMembers(
    const std::vector<std::string>& parentSymbolNames,
    const std::vector<TypeInfo>& parentTypes, const std::string& memberName,
    const Scope& scope) const {
  std::vector<SymbolInfo> result;
  if (parentSymbolNames.empty() || memberName.empty()) {
    return result;
  }

  const std::unordered_map<std::string, TypeInfo> effectiveParents =
      effectiveParentTypes(parentTypes);
  std::unordered_set<std::string> seen;
  for (const std::string& parentName :
       linearizedParentsFor(parentSymbolNames, globalSymbols_)) {
    auto parentScope = declaredMemberScopes_.find(parentName);
    if (parentScope == declaredMemberScopes_.end()) {
      continue;
    }
    auto member = parentScope->second.find(memberName);
    if (member == parentScope->second.end() ||
        !seen.insert(member->second.symbolName).second) {
      continue;
    }
    auto appliedParent = effectiveParents.find(parentName);
    result.push_back(specializeInheritedMember(
        member->second, scope,
        appliedParent == effectiveParents.end() ? nullptr : &appliedParent->second));
  }
  return result;
}

void Typechecker::validateInheritedMemberCompatibility(
    const AstDeclaration& declaration,
    const std::vector<std::string>& parentSymbolNames,
    const std::vector<TypeInfo>& parentTypes, const Scope& effectiveScope) const {
  const std::unordered_map<std::string, TypeInfo> effectiveParents =
      effectiveParentTypes(parentTypes);
  std::unordered_map<std::string, std::vector<SymbolInfo>> members;
  for (const std::string& parentName :
       linearizedParentsFor(parentSymbolNames, globalSymbols_)) {
    auto parentScope = declaredMemberScopes_.find(parentName);
    if (parentScope == declaredMemberScopes_.end()) {
      continue;
    }
    for (const auto& [name, symbol] : parentScope->second) {
      if (symbol.kind != AstDeclarationKind::Type &&
          symbol.kind != AstDeclarationKind::Def &&
          symbol.kind != AstDeclarationKind::Val &&
          symbol.kind != AstDeclarationKind::Var) {
        continue;
      }
      auto& candidates = members[name];
      const bool duplicate = std::any_of(
          candidates.begin(), candidates.end(), [&](const SymbolInfo& candidate) {
            return candidate.symbolName == symbol.symbolName;
          });
      if (!duplicate) {
        auto appliedParent = effectiveParents.find(parentName);
        candidates.push_back(specializeInheritedMember(
            symbol, effectiveScope,
            appliedParent == effectiveParents.end() ? nullptr
                                                    : &appliedParent->second));
      }
    }
  }

  const std::string owner =
      std::string(declaration.kind == AstDeclarationKind::Trait    ? "trait "
                  : declaration.kind == AstDeclarationKind::Object ? "object "
                                                                   : "class ") +
      declaration.name;
  for (const auto& [name, candidates] : members) {
    if (candidates.size() < 2) {
      continue;
    }
    const SymbolInfo& effective = candidates.front();
    for (std::size_t i = 1; i < candidates.size(); ++i) {
      const SymbolInfo& required = candidates[i];
      if (effective.kind == AstDeclarationKind::Type &&
          required.kind == AstDeclarationKind::Type) {
        if (required.hasImplementation) {
          if (effective.hasImplementation &&
              typesMatchForOverride(required.type, effective.type)) {
            continue;
          }
          diagnostics_.error(declaration.span,
                             owner + " inherits incompatible type aliases for " + name +
                                 ": " + effective.type.name + " from " +
                                 effective.symbolName + " and " + required.type.name +
                                 " from " + required.symbolName);
          break;
        }

        const TypeInfo effectiveLower =
            effective.hasImplementation ? effective.type : effective.lowerBound;
        const TypeInfo effectiveUpper =
            effective.hasImplementation ? effective.type : effective.upperBound;
        if (required.lowerBound.kind != SimpleTypeKind::Unknown &&
            (effectiveLower.kind == SimpleTypeKind::Unknown ||
             !isAssignable(effectiveLower, required.lowerBound))) {
          diagnostics_.error(
              declaration.span,
              owner + " inherits type member " + name + " from " +
                  effective.symbolName + " whose " +
                  (effective.hasImplementation ? "alias target " : "lower bound ") +
                  effectiveLower.name + " does not preserve inherited lower bound " +
                  required.lowerBound.name + " required by " + required.symbolName);
          break;
        }
        if (required.upperBound.kind != SimpleTypeKind::Unknown &&
            (effectiveUpper.kind == SimpleTypeKind::Unknown ||
             !isAssignable(required.upperBound, effectiveUpper))) {
          diagnostics_.error(
              declaration.span,
              owner + " inherits type member " + name + " from " +
                  effective.symbolName + " whose " +
                  (effective.hasImplementation ? "alias target " : "upper bound ") +
                  effectiveUpper.name + " does not conform to inherited upper bound " +
                  required.upperBound.name + " required by " + required.symbolName);
          break;
        }
        continue;
      }
      if (inheritedContractSatisfiedBy(effective, required)) {
        continue;
      }
      if (!effective.hasImplementation && !required.hasImplementation &&
          memberShapesSupportResultMeet(effective, required)) {
        continue;
      }
      if (effective.kind == AstDeclarationKind::Def &&
          required.kind == AstDeclarationKind::Def) {
        diagnostics_.error(declaration.span,
                           owner + " inherits incompatible method " + name + " from " +
                               effective.symbolName + " and " + required.symbolName);
      } else if (isValueAccessor(effective.kind) && isValueAccessor(required.kind) &&
                 !typesMatchForOverride(required.type, effective.type)) {
        diagnostics_.error(declaration.span,
                           owner + " inherits incompatible accessor types for " + name +
                               ": " + effective.type.name + " from " +
                               effective.symbolName + " and " + required.type.name +
                               " from " + required.symbolName);
      } else if (effective.kind == AstDeclarationKind::Val &&
                 required.kind == AstDeclarationKind::Var) {
        diagnostics_.error(declaration.span,
                           owner + " inherits incompatible accessor shape for " + name +
                               ": effective value " + effective.symbolName +
                               " does not provide the setter required by variable " +
                               required.symbolName);
      } else {
        diagnostics_.error(declaration.span,
                           owner + " inherits incompatible member shape for " + name +
                               ": " + memberKindName(effective.kind) + " " +
                               effective.symbolName + " conflicts with " +
                               memberKindName(required.kind) + " " +
                               required.symbolName);
      }
      break;
    }
  }
}

void Typechecker::validateOverride(const TypedDeclaration& overriding,
                                   const SymbolInfo& inherited) const {
  if (inherited.kind == AstDeclarationKind::Type) {
    if (overriding.kind != AstDeclarationKind::Type) {
      diagnostics_.error(overriding.span, "cannot implement inherited type member " +
                                              inherited.name +
                                              " with a non-type declaration");
      return;
    }
    if (inherited.hasImplementation &&
        (!overriding.hasInitializer ||
         !typesMatchForOverride(inherited.type, overriding.inferredType))) {
      diagnostics_.error(
          overriding.span,
          "type alias " + overriding.name + " target " + overriding.inferredType.name +
              " does not match inherited alias target " + inherited.type.name);
    } else if (!inherited.hasImplementation) {
      const TypeInfo overridingLower =
          overriding.hasInitializer
              ? overriding.inferredType
              : typeFromDeclaredName(overriding.lowerBound, nullptr);
      const TypeInfo overridingUpper =
          overriding.hasInitializer
              ? overriding.inferredType
              : typeFromDeclaredName(overriding.upperBound, nullptr);
      if (inherited.lowerBound.kind != SimpleTypeKind::Unknown &&
          (overridingLower.kind == SimpleTypeKind::Unknown ||
           !isAssignable(overridingLower, inherited.lowerBound))) {
        diagnostics_.error(
            overriding.span,
            "type member " + overriding.name +
                (overriding.hasInitializer ? " alias target " : " lower bound ") +
                overridingLower.name + " does not preserve inherited lower bound " +
                inherited.lowerBound.name);
      }
      if (inherited.upperBound.kind != SimpleTypeKind::Unknown &&
          (overridingUpper.kind == SimpleTypeKind::Unknown ||
           !isAssignable(inherited.upperBound, overridingUpper))) {
        diagnostics_.error(
            overriding.span,
            "type member " + overriding.name +
                (overriding.hasInitializer ? " alias target " : " upper bound ") +
                overridingUpper.name +
                " does not conform to inherited upper "
                "bound " +
                inherited.upperBound.name);
      }
    }
    return;
  }
  if (inherited.kind == AstDeclarationKind::Var) {
    if (overriding.kind != AstDeclarationKind::Var) {
      diagnostics_.error(overriding.span, "cannot implement inherited variable " +
                                              inherited.name +
                                              " with a non-variable declaration");
      return;
    }
    if (!typesMatchForOverride(inherited.type, overriding.inferredType)) {
      diagnostics_.error(overriding.span,
                         "override " + overriding.name + " variable type " +
                             overriding.inferredType.name +
                             " does not match inherited variable type " +
                             inherited.type.name);
    }
    return;
  }
  if (inherited.kind == AstDeclarationKind::Val) {
    if (overriding.kind != AstDeclarationKind::Val &&
        overriding.kind != AstDeclarationKind::Var) {
      diagnostics_.error(overriding.span, "cannot implement inherited value " +
                                              inherited.name +
                                              " with a non-value declaration");
      return;
    }
    if (!isAssignable(inherited.type, overriding.inferredType)) {
      diagnostics_.error(overriding.span, "override " + overriding.name +
                                              " value type " +
                                              overriding.inferredType.name +
                                              " does not match inherited value type " +
                                              inherited.type.name);
    }
    return;
  }
  if (inherited.kind != AstDeclarationKind::Def) {
    if (overriding.isOverride) {
      diagnostics_.error(overriding.span,
                         "override " + overriding.name +
                             " can only target inherited methods in this MVP");
    }
    return;
  }
  if (overriding.kind != AstDeclarationKind::Def) {
    diagnostics_.error(overriding.span, "cannot override inherited method " +
                                            inherited.name +
                                            " with non-method declaration");
    return;
  }

  if (overriding.parameters.size() != inherited.parameterTypes.size()) {
    diagnostics_.error(overriding.span,
                       "override " + overriding.name + " has " +
                           std::to_string(overriding.parameters.size()) +
                           " parameters but inherited method expects " +
                           std::to_string(inherited.parameterTypes.size()));
    return;
  }

  for (std::size_t i = 0; i < overriding.parameters.size(); ++i) {
    TypeInfo actual = parameterType(overriding.parameters[i], nullptr);
    if (!typesMatchForOverride(inherited.parameterTypes[i], actual)) {
      diagnostics_.error(overriding.span,
                         "override " + overriding.name + " parameter " +
                             std::to_string(i) + " type " + actual.name +
                             " does not match inherited parameter type " +
                             inherited.parameterTypes[i].name);
    }
  }

  if (!isAssignable(inherited.type, overriding.inferredType)) {
    diagnostics_.error(
        overriding.span,
        "override " + overriding.name + " return type " + overriding.inferredType.name +
            " does not match inherited return type " + inherited.type.name);
  }
}

void Typechecker::mergeInheritedMembers(
    Scope& destination, const std::vector<std::string>& parentSymbolNames,
    const std::vector<TypeInfo>& parentTypes) const {
  if (parentSymbolNames.empty()) {
    return;
  }

  const std::vector<std::string> parents =
      linearizedParentsFor(parentSymbolNames, globalSymbols_);
  const std::unordered_map<std::string, TypeInfo> effectiveParents =
      effectiveParentTypes(parentTypes);
  for (const std::string& parentName : parents) {
    auto parentScope = declaredMemberScopes_.find(parentName);
    if (parentScope == declaredMemberScopes_.end()) {
      continue;
    }
    for (const auto& [name, symbol] : parentScope->second) {
      if (symbol.kind == AstDeclarationKind::Type && !destination.contains(name)) {
        auto appliedParent = effectiveParents.find(parentName);
        destination[name] = specializeInheritedMember(
            symbol, destination,
            appliedParent == effectiveParents.end() ? nullptr : &appliedParent->second);
      }
    }
  }

  for (const std::string& parentName : parents) {
    auto parentScope = declaredMemberScopes_.find(parentName);
    if (parentScope == declaredMemberScopes_.end()) {
      continue;
    }
    for (const auto& [name, symbol] : parentScope->second) {
      if ((symbol.kind == AstDeclarationKind::Def ||
           symbol.kind == AstDeclarationKind::Val ||
           symbol.kind == AstDeclarationKind::Var) &&
          !destination.contains(name)) {
        auto appliedParent = effectiveParents.find(parentName);
        destination[name] = specializeInheritedMember(
            symbol, destination,
            appliedParent == effectiveParents.end() ? nullptr : &appliedParent->second);
      }
    }
  }
}

TypeInfo Typechecker::substituteTypeMembers(const TypeInfo& type,
                                            const Scope& scope) const {
  if (type.name.empty()) {
    return type;
  }
  const std::size_t dot = type.name.rfind('.');
  const std::string memberName =
      dot == std::string::npos ? type.name : type.name.substr(dot + 1);
  auto alias = scope.find(memberName);
  if (alias == scope.end() || alias->second.kind != AstDeclarationKind::Type ||
      !alias->second.hasImplementation || alias->second.type.name == type.name) {
    return type;
  }
  TypeInfo substituted = alias->second.type;
  substituted.runtimeName = type.runtimeName;
  substituted.resolvedAliasName = alias->second.symbolName;
  substituted.dependentOwnerName = type.dependentOwnerName;
  substituted.dependentMemberName = type.dependentMemberName;
  substituted.dependentPathName = type.dependentPathName;
  substituted.pathDependent = type.pathDependent;
  substituted.typeProjection = type.typeProjection;
  if (substituted.dependentMemberName.empty()) {
    const std::size_t separator = type.name.find_last_of(".#");
    if (separator != std::string::npos && separator + 1 < type.name.size()) {
      substituted.dependentOwnerName = type.name.substr(0, separator);
      substituted.dependentMemberName = type.name.substr(separator + 1);
    }
  }
  return substituted;
}

TypeInfo Typechecker::specializeTypeForReceiver(const TypeInfo& type,
                                                const TypeInfo& receiver) const {
  if (receiver.typeConstructorName.empty() || receiver.typeArguments.empty()) {
    return type;
  }
  auto constructor = globalSymbols_.find(receiver.typeConstructorName);
  if (constructor == globalSymbols_.end() ||
      constructor->second.typeParameters.size() != receiver.typeArguments.size()) {
    return type;
  }
  std::unordered_map<std::string, TypeInfo> substitutions;
  for (std::size_t i = 0; i < receiver.typeArguments.size(); ++i) {
    substitutions[constructor->second.typeParameters[i].symbolName] =
        receiver.typeArguments[i];
  }
  return substituteTypeParameters(type, substitutions);
}

std::unordered_map<std::string, TypeInfo>
Typechecker::effectiveParentTypes(const std::vector<TypeInfo>& directParents) const {
  std::unordered_map<std::string, TypeInfo> result;
  std::unordered_set<std::string> visiting;
  std::function<void(const TypeInfo&)> visit = [&](const TypeInfo& parent) {
    const std::string parentName =
        parent.typeConstructorName.empty()
            ? (parent.runtimeName.empty() ? parent.name : parent.runtimeName)
            : parent.typeConstructorName;
    if (parentName.empty() || result.contains(parentName) ||
        !visiting.insert(parentName).second) {
      return;
    }
    result[parentName] = parent;
    auto symbol = globalSymbols_.find(parentName);
    if (symbol != globalSymbols_.end()) {
      for (const TypeInfo& inherited : symbol->second.parentTypes) {
        visit(specializeTypeForReceiver(inherited, parent));
      }
    }
    visiting.erase(parentName);
  };
  for (const TypeInfo& parent : directParents) {
    visit(parent);
  }
  return result;
}

SymbolInfo Typechecker::specializeInheritedMember(const SymbolInfo& member,
                                                  const Scope& scope,
                                                  const TypeInfo* appliedParent) const {
  SymbolInfo specialized = member;
  const auto specialize = [&](const TypeInfo& type) {
    TypeInfo result = substituteTypeMembers(type, scope);
    if (appliedParent != nullptr) {
      result = specializeTypeForReceiver(result, *appliedParent);
    }
    if (!type.runtimeName.empty() && result.name != type.name) {
      result.runtimeName = type.runtimeName;
    }
    return result;
  };
  specialized.type = specialize(member.type);
  for (TypeInfo& parameterType : specialized.parameterTypes) {
    parameterType = specialize(parameterType);
  }
  specialized.lowerBound = specialize(member.lowerBound);
  specialized.upperBound = specialize(member.upperBound);
  for (TypeParameterInfo& typeParameter : specialized.typeParameters) {
    typeParameter.lowerBound = specialize(typeParameter.lowerBound);
    typeParameter.upperBound = specialize(typeParameter.upperBound);
  }
  return specialized;
}

void Typechecker::validateVariance(const AstDeclaration& declaration,
                                   const TypedDeclaration& typed) const {
  if (typed.typeParameters.empty()) {
    return;
  }

  const auto parameterFor = [&](const TypeInfo& type) -> const TypeParameterInfo* {
    if (!type.typeParameter) {
      return nullptr;
    }
    auto found =
        std::find_if(typed.typeParameters.begin(), typed.typeParameters.end(),
                     [&](const TypeParameterInfo& parameter) {
                       return parameter.symbolName == type.typeParameterSymbolName;
                     });
    return found == typed.typeParameters.end() ? nullptr : &*found;
  };
  const auto parameterSpan = [&](const std::string& name) {
    auto found = std::find_if(
        declaration.typeParameters.begin(), declaration.typeParameters.end(),
        [&](const AstTypeParameter& parameter) { return parameter.name == name; });
    return found == declaration.typeParameters.end() ? declaration.span : found->span;
  };

  std::function<void(const TypeInfo&, int, const std::string&)> inspect;
  inspect = [&](const TypeInfo& type, int position, const std::string& context) {
    if (const TypeParameterInfo* parameter = parameterFor(type);
        parameter != nullptr && parameter->variance != TypeVariance::Invariant) {
      const bool invalid =
          (parameter->variance == TypeVariance::Covariant && position != 1) ||
          (parameter->variance == TypeVariance::Contravariant && position != -1);
      if (invalid) {
        const std::string declaredVariance =
            parameter->variance == TypeVariance::Covariant ? "covariant"
                                                           : "contravariant";
        const std::string positionName = position == 0  ? "invariant"
                                         : position > 0 ? "covariant"
                                                        : "contravariant";
        diagnostics_.error(parameterSpan(parameter->name),
                           declaredVariance + " type parameter " + parameter->name +
                               " occurs in " + positionName + " position in " +
                               context);
      }
      return;
    }

    if (type.compositeKind != CompositeTypeKind::None) {
      for (const TypeInfo& operand : type.compositeTypes) {
        inspect(operand, position, context);
      }
      return;
    }

    if (type.typeConstructorName.empty() || type.typeArguments.empty()) {
      return;
    }
    auto constructor = globalSymbols_.find(type.typeConstructorName);
    for (std::size_t i = 0; i < type.typeArguments.size(); ++i) {
      TypeVariance argumentVariance = TypeVariance::Invariant;
      if (constructor != globalSymbols_.end() &&
          i < constructor->second.typeParameters.size()) {
        argumentVariance = constructor->second.typeParameters[i].variance;
      }
      int argumentPosition = 0;
      if (position != 0 && argumentVariance != TypeVariance::Invariant) {
        argumentPosition =
            argumentVariance == TypeVariance::Covariant ? position : -position;
      }
      inspect(type.typeArguments[i], argumentPosition, context);
    }
  };

  for (const TypeInfo& parent : typed.parentTypeInfos) {
    inspect(parent, 1, "parent type " + parent.name);
  }

  if (declaration.kind == AstDeclarationKind::Class) {
    const std::size_t parameterCount =
        std::min(typed.parameters.size(), typed.parameterTypes.size());
    for (std::size_t i = 0; i < parameterCount; ++i) {
      if (!isExplicitValParameter(typed.parameters[i]) &&
          !isExplicitVarParameter(typed.parameters[i])) {
        continue;
      }
      const std::string name = parameterName(typed.parameters[i]);
      inspect(typed.parameterTypes[i],
              isExplicitVarParameter(typed.parameters[i]) ? 0 : 1,
              "constructor parameter " + name);
    }
  }

  for (const TypedDeclaration& member : typed.members) {
    if (member.kind == AstDeclarationKind::Def) {
      inspect(member.inferredType, 1, "return type of method " + member.name);
      for (std::size_t i = 0; i < member.parameterTypes.size(); ++i) {
        const std::string name = i < member.parameters.size()
                                     ? parameterName(member.parameters[i])
                                     : std::to_string(i);
        inspect(member.parameterTypes[i], -1,
                "parameter " + name + " of method " + member.name);
      }
    } else if (member.kind == AstDeclarationKind::Val) {
      inspect(member.inferredType, 1, "value " + member.name);
    } else if (member.kind == AstDeclarationKind::Var) {
      inspect(member.inferredType, 0, "variable " + member.name);
    }
  }
}

std::vector<TypeParameterInfo>
Typechecker::resolvedTypeParameters(const std::vector<AstTypeParameter>& parameters,
                                    const std::string& owner, Scope& scope) const {
  std::vector<TypeParameterInfo> resolved;
  resolved.reserve(parameters.size());
  for (const AstTypeParameter& parameter : parameters) {
    TypeParameterInfo info;
    info.name = parameter.name;
    info.symbolName = qualify(owner, parameter.name);
    info.variance = parameter.variance;
    info.lowerBound =
        parameter.lowerBound.empty()
            ? TypeInfo{SimpleTypeKind::Nothing, "Nothing"}
            : typeFromDeclaredName(parameter.lowerBound, &scope, &parameter.span);
    info.upperBound =
        parameter.upperBound.empty()
            ? TypeInfo{SimpleTypeKind::Object, "Object"}
            : typeFromDeclaredName(parameter.upperBound, &scope, &parameter.span);

    if (info.upperBound.kind != SimpleTypeKind::Unknown &&
        !isReferenceType(info.upperBound)) {
      diagnostics_.error(parameter.span,
                         "type parameter " + parameter.name +
                             " requires a reference upper bound in this generics "
                             "milestone");
      info.upperBound = TypeInfo{SimpleTypeKind::Object, "Object"};
    }
    if (info.lowerBound.kind != SimpleTypeKind::Unknown &&
        info.lowerBound.kind != SimpleTypeKind::Nothing &&
        !isReferenceType(info.lowerBound)) {
      diagnostics_.error(parameter.span,
                         "type parameter " + parameter.name +
                             " requires a reference lower bound in this generics "
                             "milestone");
      info.lowerBound = TypeInfo{SimpleTypeKind::Nothing, "Nothing"};
    }
    if (info.lowerBound.kind != SimpleTypeKind::Unknown &&
        info.upperBound.kind != SimpleTypeKind::Unknown &&
        !isAssignable(info.upperBound, info.lowerBound)) {
      diagnostics_.error(parameter.span, "type parameter " + parameter.name +
                                             " lower bound " + info.lowerBound.name +
                                             " does not conform to upper bound " +
                                             info.upperBound.name);
    }

    TypeInfo parameterType{SimpleTypeKind::Object, parameter.name};
    parameterType.runtimeName = info.upperBound.runtimeName.empty()
                                    ? info.upperBound.name
                                    : info.upperBound.runtimeName;
    if (parameterType.runtimeName.empty() || parameterType.runtimeName == "Unknown") {
      parameterType.runtimeName = "Object";
    }
    parameterType.typeParameterSymbolName = info.symbolName;
    parameterType.typeParameter = true;

    SymbolInfo symbol;
    symbol.kind = AstDeclarationKind::Type;
    symbol.name = parameter.name;
    symbol.symbolName = info.symbolName;
    symbol.type = std::move(parameterType);
    symbol.lowerBound = info.lowerBound;
    symbol.upperBound = info.upperBound;
    symbol.hasImplementation = true;
    scope[parameter.name] = std::move(symbol);
    resolved.push_back(std::move(info));
  }
  return resolved;
}

TypeInfo Typechecker::substituteTypeParameters(
    const TypeInfo& type,
    const std::unordered_map<std::string, TypeInfo>& substitutions) const {
  if (type.typeParameter) {
    auto replacement = substitutions.find(type.typeParameterSymbolName);
    if (replacement != substitutions.end()) {
      return replacement->second;
    }
  }

  if (type.typeArguments.empty() && type.compositeTypes.empty()) {
    return type;
  }
  TypeInfo substituted = type;
  for (TypeInfo& operand : substituted.compositeTypes) {
    operand = substituteTypeParameters(operand, substitutions);
  }
  if (substituted.compositeKind != CompositeTypeKind::None) {
    TypeInfo normalized = makeCompositeType(substituted.compositeKind,
                                            std::move(substituted.compositeTypes));
    normalized.dependentOwnerName = std::move(substituted.dependentOwnerName);
    normalized.dependentMemberName = std::move(substituted.dependentMemberName);
    normalized.dependentPathName = std::move(substituted.dependentPathName);
    normalized.resolvedAliasName = std::move(substituted.resolvedAliasName);
    normalized.abstractTypeMember = substituted.abstractTypeMember;
    normalized.pathDependent = substituted.pathDependent;
    normalized.typeProjection = substituted.typeProjection;
    normalized.softUnion =
        substituted.softUnion && normalized.compositeKind == CompositeTypeKind::Union;
    return normalized;
  }
  for (TypeInfo& argument : substituted.typeArguments) {
    argument = substituteTypeParameters(argument, substitutions);
  }
  if (!substituted.typeConstructorName.empty()) {
    substituted.name = substituted.typeConstructorName + " [ ";
    for (std::size_t i = 0; i < substituted.typeArguments.size(); ++i) {
      if (i != 0) {
        substituted.name += ", ";
      }
      substituted.name += substituted.typeArguments[i].name;
    }
    substituted.name += " ]";
  }
  return substituted;
}

SymbolInfo Typechecker::specializeMemberForReceiver(const SymbolInfo& member,
                                                    const TypeInfo& receiver) const {
  if (receiver.compositeKind != CompositeTypeKind::None) {
    for (const TypeInfo& operand : receiver.compositeTypes) {
      if (const SymbolInfo* candidate =
              knownMemberForReceiverType(operand, member.name);
          candidate != nullptr && candidate->symbolName == member.symbolName) {
        return specializeMemberForReceiver(member, operand);
      }
    }
    return member;
  }
  if (receiver.typeConstructorName.empty() || receiver.typeArguments.empty()) {
    return member;
  }
  auto constructor = globalSymbols_.find(receiver.typeConstructorName);
  if (constructor == globalSymbols_.end() ||
      constructor->second.typeParameters.size() != receiver.typeArguments.size()) {
    return member;
  }

  std::unordered_map<std::string, TypeInfo> substitutions;
  for (std::size_t i = 0; i < receiver.typeArguments.size(); ++i) {
    substitutions[constructor->second.typeParameters[i].symbolName] =
        receiver.typeArguments[i];
  }
  SymbolInfo specialized = member;
  specialized.type = substituteTypeParameters(member.type, substitutions);
  for (TypeInfo& parameterType : specialized.parameterTypes) {
    parameterType = substituteTypeParameters(parameterType, substitutions);
  }
  for (TypeParameterInfo& typeParameter : specialized.typeParameters) {
    typeParameter.lowerBound =
        substituteTypeParameters(typeParameter.lowerBound, substitutions);
    typeParameter.upperBound =
        substituteTypeParameters(typeParameter.upperBound, substitutions);
  }
  return specialized;
}

SymbolInfo Typechecker::specializeTypeApplication(
    const SymbolInfo& symbol, const std::vector<std::string>& typeArguments,
    const Scope& scope, const support::SourceSpan& span, bool reportDiagnostics) const {
  if (typeArguments.size() != symbol.typeParameters.size()) {
    if (reportDiagnostics) {
      diagnostics_.error(span, "type application to " + symbol.name + " has " +
                                   std::to_string(typeArguments.size()) +
                                   " arguments but expected " +
                                   std::to_string(symbol.typeParameters.size()));
    }
    return symbol;
  }

  std::vector<TypeInfo> resolvedArguments;
  resolvedArguments.reserve(typeArguments.size());
  for (const std::string& typeArgument : typeArguments) {
    resolvedArguments.push_back(typeFromDeclaredName(
        typeArgument, &scope, reportDiagnostics ? &span : nullptr));
  }
  return specializeResolvedTypeApplication(symbol, resolvedArguments, span,
                                           reportDiagnostics);
}

SymbolInfo Typechecker::specializeResolvedTypeApplication(
    const SymbolInfo& symbol, const std::vector<TypeInfo>& typeArguments,
    const support::SourceSpan& span, bool reportDiagnostics) const {
  SymbolInfo specialized = symbol;
  if (typeArguments.size() != symbol.typeParameters.size()) {
    if (reportDiagnostics) {
      diagnostics_.error(span, "type application to " + symbol.name + " has " +
                                   std::to_string(typeArguments.size()) +
                                   " arguments but expected " +
                                   std::to_string(symbol.typeParameters.size()));
    }
    return specialized;
  }

  std::unordered_map<std::string, TypeInfo> substitutions;
  for (std::size_t i = 0; i < typeArguments.size(); ++i) {
    const TypeInfo& argument = typeArguments[i];
    if (argument.kind != SimpleTypeKind::Unknown &&
        argument.kind != SimpleTypeKind::Nothing && !isReferenceType(argument) &&
        !isBoxablePrimitiveType(argument.kind)) {
      if (reportDiagnostics) {
        diagnostics_.error(span,
                           "type argument " + argument.name + " for " + symbol.name +
                               " must be a supported primitive or reference type");
      }
    }
    const TypeInfo upper =
        substituteTypeParameters(symbol.typeParameters[i].upperBound, substitutions);
    const TypeInfo lower =
        substituteTypeParameters(symbol.typeParameters[i].lowerBound, substitutions);
    if (argument.kind != SimpleTypeKind::Unknown &&
        upper.kind != SimpleTypeKind::Unknown && upper.name != "Object" &&
        !isAssignable(upper, argument) && reportDiagnostics) {
      diagnostics_.error(span, "type argument " + argument.name + " for " +
                                   symbol.typeParameters[i].name +
                                   " does not conform to upper bound " + upper.name);
    }
    if (argument.kind != SimpleTypeKind::Unknown &&
        lower.kind != SimpleTypeKind::Unknown &&
        lower.kind != SimpleTypeKind::Nothing && !isAssignable(argument, lower) &&
        reportDiagnostics) {
      diagnostics_.error(span, "type argument " + argument.name + " for " +
                                   symbol.typeParameters[i].name +
                                   " does not conform to lower bound " + lower.name);
    }
    substitutions[symbol.typeParameters[i].symbolName] = argument;
  }

  specialized.type = substituteTypeParameters(symbol.type, substitutions);
  for (TypeInfo& parameterType : specialized.parameterTypes) {
    parameterType = substituteTypeParameters(parameterType, substitutions);
  }
  if (symbol.kind == AstDeclarationKind::Class ||
      symbol.kind == AstDeclarationKind::Trait) {
    specialized.type = TypeInfo{SimpleTypeKind::Object, symbol.symbolName + " [ "};
    for (std::size_t i = 0; i < typeArguments.size(); ++i) {
      if (i != 0) {
        specialized.type.name += ", ";
      }
      specialized.type.name += typeArguments[i].name;
    }
    specialized.type.name += " ]";
    specialized.type.runtimeName = symbol.symbolName;
    specialized.type.typeConstructorName = symbol.symbolName;
    specialized.type.typeArguments = typeArguments;
  }
  specialized.typeParameters.clear();
  return specialized;
}

SymbolInfo Typechecker::inferTypeApplication(
    const SymbolInfo& symbol, const std::vector<TypeInfo>& argumentTypes,
    const support::SourceSpan& span, const TypeInfo* expectedResultType,
    bool reportDiagnostics, std::vector<TypeInfo>* inferredTypeArguments,
    bool* inferenceConflict) const {
  std::unordered_map<std::string, TypeInfo> substitutions;
  std::unordered_set<std::string> conflictingParameters;
  bool hasConflict = false;

  const auto isApplicationTypeParameter = [&](const TypeInfo& type) {
    return type.typeParameter &&
           std::any_of(symbol.typeParameters.begin(), symbol.typeParameters.end(),
                       [&](const TypeParameterInfo& parameter) {
                         return parameter.symbolName == type.typeParameterSymbolName;
                       });
  };
  const auto sameType = [](const TypeInfo& lhs, const TypeInfo& rhs) {
    return lhs.kind == rhs.kind && lhs.name == rhs.name;
  };
  const auto mergeCandidate = [&](const TypeInfo& current, const TypeInfo& candidate) {
    if (sameType(current, candidate)) {
      return current;
    }
    if (isAssignable(current, candidate)) {
      return current;
    }
    if (isAssignable(candidate, current)) {
      return candidate;
    }
    const TypeInfo merged = commonType(current, candidate);
    if (!merged.softUnion) {
      return merged;
    }
    return widenSoftUnion(merged);
  };

  std::function<void(const TypeInfo&, const TypeInfo&, bool)> collectInference;
  collectInference = [&](const TypeInfo& parameterType, const TypeInfo& argumentType,
                         bool onlyIfMissing) {
    if (argumentType.kind == SimpleTypeKind::Unknown) {
      return;
    }
    if (isApplicationTypeParameter(parameterType)) {
      auto inferred = substitutions.find(parameterType.typeParameterSymbolName);
      if (onlyIfMissing && inferred != substitutions.end()) {
        return;
      }
      if (inferred == substitutions.end()) {
        substitutions.emplace(parameterType.typeParameterSymbolName, argumentType);
        return;
      }
      const TypeInfo merged = mergeCandidate(inferred->second, argumentType);
      if (merged.kind == SimpleTypeKind::Unknown) {
        hasConflict = true;
        if (reportDiagnostics &&
            conflictingParameters.insert(parameterType.typeParameterSymbolName)
                .second) {
          diagnostics_.error(span, "conflicting inferred types " +
                                       inferred->second.name + " and " +
                                       argumentType.name + " for type parameter " +
                                       parameterType.name + " of " + symbol.name);
        }
        return;
      }
      inferred->second = merged;
      return;
    }
    if (!parameterType.typeConstructorName.empty() &&
        parameterType.typeConstructorName == argumentType.typeConstructorName &&
        parameterType.typeArguments.size() == argumentType.typeArguments.size()) {
      for (std::size_t i = 0; i < parameterType.typeArguments.size(); ++i) {
        collectInference(parameterType.typeArguments[i], argumentType.typeArguments[i],
                         onlyIfMissing);
      }
      return;
    }
    if (!parameterType.typeConstructorName.empty() &&
        !argumentType.typeConstructorName.empty()) {
      auto constructor = globalSymbols_.find(parameterType.typeConstructorName);
      if (constructor == globalSymbols_.end()) {
        return;
      }
      for (const TypeInfo& parentPattern : constructor->second.parentTypes) {
        const TypeInfo parent = specializeTypeForReceiver(parentPattern, parameterType);
        if (parent.typeConstructorName == argumentType.typeConstructorName) {
          collectInference(parent, argumentType, onlyIfMissing);
        }
      }
    }
  };

  const std::size_t contextualParameterCount = static_cast<std::size_t>(std::count(
      symbol.contextualParameters.begin(), symbol.contextualParameters.end(), true));
  const bool contextsAreOmitted =
      contextualParameterCount != 0 &&
      argumentTypes.size() + contextualParameterCount == symbol.parameterTypes.size();
  if (contextsAreOmitted) {
    std::size_t argumentIndex = 0;
    for (std::size_t parameterIndex = 0;
         parameterIndex < symbol.parameterTypes.size() &&
         argumentIndex < argumentTypes.size();
         ++parameterIndex) {
      if (parameterIndex < symbol.contextualParameters.size() &&
          symbol.contextualParameters[parameterIndex]) {
        continue;
      }
      collectInference(symbol.parameterTypes[parameterIndex],
                       argumentTypes[argumentIndex], false);
      ++argumentIndex;
    }
  } else {
    const std::size_t checkedArguments =
        std::min(symbol.parameterTypes.size(), argumentTypes.size());
    for (std::size_t i = 0; i < checkedArguments; ++i) {
      collectInference(symbol.parameterTypes[i], argumentTypes[i], false);
    }
  }

  if (expectedResultType != nullptr &&
      expectedResultType->kind != SimpleTypeKind::Unknown) {
    if (symbol.kind == AstDeclarationKind::Class &&
        expectedResultType->typeConstructorName == symbol.symbolName &&
        expectedResultType->typeArguments.size() == symbol.typeParameters.size()) {
      for (std::size_t i = 0; i < symbol.typeParameters.size(); ++i) {
        TypeInfo parameterType{SimpleTypeKind::Object, symbol.typeParameters[i].name};
        parameterType.typeParameter = true;
        parameterType.typeParameterSymbolName = symbol.typeParameters[i].symbolName;
        collectInference(parameterType, expectedResultType->typeArguments[i], true);
      }
    } else if (symbol.kind == AstDeclarationKind::Def) {
      collectInference(symbol.type, *expectedResultType, true);
    }
  }

  std::vector<TypeInfo> inferredArguments;
  inferredArguments.reserve(symbol.typeParameters.size());
  bool complete = true;
  for (const TypeParameterInfo& parameter : symbol.typeParameters) {
    auto inferred = substitutions.find(parameter.symbolName);
    if (inferred == substitutions.end() ||
        inferred->second.kind == SimpleTypeKind::Unknown) {
      complete = false;
      inferredArguments.push_back(TypeInfo{SimpleTypeKind::Unknown, "Unknown"});
      if (reportDiagnostics) {
        diagnostics_.error(span, "cannot infer type argument " + parameter.name +
                                     " for " + symbol.name +
                                     (expectedResultType == nullptr
                                          ? " from value arguments"
                                          : " from value arguments or expected "
                                            "result type") +
                                     "; use explicit type arguments");
      }
      continue;
    }
    TypeInfo inferredArgument =
        parameter.upperBound.compositeKind == CompositeTypeKind::Union
            ? inferred->second
            : widenSoftUnion(inferred->second);
    inferredArguments.push_back(std::move(inferredArgument));
  }
  if (inferredTypeArguments != nullptr) {
    *inferredTypeArguments = inferredArguments;
  }
  if (inferenceConflict != nullptr) {
    *inferenceConflict = hasConflict;
  }
  if (!complete || hasConflict) {
    return symbol;
  }
  return specializeResolvedTypeApplication(symbol, inferredArguments, span,
                                           reportDiagnostics);
}

std::vector<SymbolInfo> Typechecker::inferContextualTypeApplications(
    const SymbolInfo& symbol, const std::vector<TypeInfo>& inferredTypeArguments,
    std::size_t firstContextParameter, Scope& scope, const support::SourceSpan& span,
    bool reportDiagnostics,
    std::unordered_set<std::string>* expandingGenericEvidence,
    std::vector<TypeInfo>* resolvedTypeArguments) const {
  using InferenceState = std::unordered_map<std::string, TypeInfo>;

  const auto isApplicationTypeParameter = [&](const TypeInfo& type) {
    return type.typeParameter &&
           std::any_of(symbol.typeParameters.begin(), symbol.typeParameters.end(),
                       [&](const TypeParameterInfo& parameter) {
                         return parameter.symbolName == type.typeParameterSymbolName;
                       });
  };
  std::function<bool(const TypeInfo&)> mentionsApplicationTypeParameter;
  mentionsApplicationTypeParameter = [&](const TypeInfo& type) {
    return isApplicationTypeParameter(type) ||
           std::any_of(type.typeArguments.begin(), type.typeArguments.end(),
                       mentionsApplicationTypeParameter) ||
           std::any_of(type.compositeTypes.begin(), type.compositeTypes.end(),
                       mentionsApplicationTypeParameter);
  };

  std::unordered_set<std::string> visiting;
  std::function<bool(const TypeInfo&, const TypeInfo&, InferenceState&)>
      collectInference;
  collectInference = [&](const TypeInfo& pattern, const TypeInfo& evidence,
                         InferenceState& state) {
    if (evidence.kind == SimpleTypeKind::Unknown) {
      return false;
    }
    if (isApplicationTypeParameter(pattern)) {
      auto inferred = state.find(pattern.typeParameterSymbolName);
      if (inferred == state.end()) {
        state.emplace(pattern.typeParameterSymbolName, evidence);
        return true;
      }
      return inferred->second.kind == evidence.kind &&
             inferred->second.name == evidence.name;
    }
    if (!pattern.typeConstructorName.empty() &&
        pattern.typeConstructorName == evidence.typeConstructorName &&
        pattern.typeArguments.size() == evidence.typeArguments.size()) {
      for (std::size_t index = 0; index < pattern.typeArguments.size(); ++index) {
        if (!collectInference(pattern.typeArguments[index],
                              evidence.typeArguments[index], state)) {
          return false;
        }
      }
      return true;
    }
    if (!mentionsApplicationTypeParameter(pattern)) {
      return isAssignable(pattern, evidence);
    }

    const std::string evidenceName =
        evidence.typeConstructorName.empty()
            ? (evidence.runtimeName.empty() ? evidence.name : evidence.runtimeName)
            : evidence.typeConstructorName;
    const std::string visitKey = pattern.name + " <- " + evidenceName;
    if (!visiting.insert(visitKey).second) {
      return false;
    }
    auto evidenceSymbol = globalSymbols_.find(evidenceName);
    if (evidenceSymbol != globalSymbols_.end()) {
      for (const TypeInfo& parentPattern : evidenceSymbol->second.parentTypes) {
        const TypeInfo parent = specializeTypeForReceiver(parentPattern, evidence);
        InferenceState parentState = state;
        if (collectInference(pattern, parent, parentState)) {
          visiting.erase(visitKey);
          state = std::move(parentState);
          return true;
        }
      }
    }
    visiting.erase(visitKey);
    return false;
  };

  std::vector<const SymbolInfo*> contextParameterEvidence;
  std::vector<const SymbolInfo*> givenEvidence;
  std::vector<const SymbolInfo*> genericGivenEvidence;
  std::unordered_set<std::string> seenEvidence;
  for (const auto& [name, candidate] : scope) {
    (void)name;
    if ((!candidate.isGiven && !candidate.isContextParameter) ||
        candidate.type.kind == SimpleTypeKind::Unknown) {
      continue;
    }
    const std::string key = candidate.symbolName + " as " + candidate.type.name;
    if (!seenEvidence.insert(key).second) {
      continue;
    }
    if (!candidate.typeParameters.empty()) {
      if (candidate.isGiven) {
        genericGivenEvidence.push_back(&candidate);
      }
    } else {
      (candidate.isContextParameter ? contextParameterEvidence : givenEvidence)
          .push_back(&candidate);
    }
  }

  std::unordered_set<std::string> localExpandingGenericEvidence;
  if (expandingGenericEvidence == nullptr) {
    expandingGenericEvidence = &localExpandingGenericEvidence;
  }
  std::vector<SymbolInfo> expandedGenericEvidence;
  for (const SymbolInfo* candidate : genericGivenEvidence) {
    bool materializableParameters =
        candidate->captureParameterCount <= candidate->parameterTypes.size() &&
        candidate->contextualParameters.size() == candidate->parameterTypes.size();
    for (std::size_t index = 0;
         materializableParameters && index < candidate->parameterTypes.size();
         ++index) {
      materializableParameters = candidate->contextualParameters[index] ==
                                 (index >= candidate->captureParameterCount);
    }
    if (!materializableParameters || candidate->parameterTypes.empty() ||
        !expandingGenericEvidence->insert(candidate->symbolName).second) {
      continue;
    }
    std::vector<TypeInfo> candidateArguments(
        candidate->typeParameters.size(), TypeInfo{SimpleTypeKind::Unknown, "Unknown"});
    std::vector<SymbolInfo> applications =
        inferContextualTypeApplications(*candidate, candidateArguments, 0, scope, span,
                                        false, expandingGenericEvidence);
    expandingGenericEvidence->erase(candidate->symbolName);
    for (SymbolInfo& application : applications) {
      if (application.type.kind != SimpleTypeKind::Unknown) {
        expandedGenericEvidence.push_back(std::move(application));
      }
    }
  }
  for (const SymbolInfo& application : expandedGenericEvidence) {
    givenEvidence.push_back(&application);
  }

  const auto stateKey = [&](const InferenceState& state) {
    std::string key;
    for (const TypeParameterInfo& parameter : symbol.typeParameters) {
      key += parameter.symbolName + "=";
      if (auto inferred = state.find(parameter.symbolName); inferred != state.end()) {
        key += inferred->second.name;
      }
      key += ";";
    }
    return key;
  };
  const auto deduplicateStates = [&](std::vector<InferenceState>& states) {
    std::unordered_set<std::string> seen;
    std::erase_if(states, [&](const InferenceState& state) {
      return !seen.insert(stateKey(state)).second;
    });
  };

  InferenceState initialState;
  const std::size_t seededArguments =
      std::min(symbol.typeParameters.size(), inferredTypeArguments.size());
  for (std::size_t argumentIndex = 0; argumentIndex < seededArguments;
       ++argumentIndex) {
    if (inferredTypeArguments[argumentIndex].kind != SimpleTypeKind::Unknown) {
      initialState.emplace(symbol.typeParameters[argumentIndex].symbolName,
                           inferredTypeArguments[argumentIndex]);
    }
  }
  std::vector<InferenceState> states;
  states.push_back(std::move(initialState));
  for (std::size_t parameterIndex = firstContextParameter;
       parameterIndex < symbol.parameterTypes.size(); ++parameterIndex) {
    if (parameterIndex >= symbol.contextualParameters.size() ||
        !symbol.contextualParameters[parameterIndex]) {
      continue;
    }
    const TypeInfo& pattern = symbol.parameterTypes[parameterIndex];
    if (!mentionsApplicationTypeParameter(pattern)) {
      continue;
    }

    std::vector<InferenceState> combined;
    for (const InferenceState& existing : states) {
      const auto inferFromEvidence =
          [&](const std::vector<const SymbolInfo*>& evidenceCandidates) {
            std::vector<InferenceState> inferredStates;
            for (const SymbolInfo* evidence : evidenceCandidates) {
              InferenceState inferred = existing;
              if (collectInference(pattern, evidence->type, inferred)) {
                inferredStates.push_back(std::move(inferred));
              }
            }
            return inferredStates;
          };
      std::vector<InferenceState> parameterStates =
          inferFromEvidence(contextParameterEvidence);
      if (parameterStates.empty()) {
        parameterStates = inferFromEvidence(givenEvidence);
      }
      for (InferenceState& parameterState : parameterStates) {
        combined.push_back(std::move(parameterState));
      }
    }
    deduplicateStates(combined);
    states = std::move(combined);
    if (states.empty()) {
      return {};
    }
  }

  std::vector<SymbolInfo> viable;
  std::vector<std::vector<TypeInfo>> viableArguments;
  std::unordered_set<std::string> seenApplications;
  std::function<bool(const TypedContextArgument&)> isMaterialized;
  isMaterialized = [&](const TypedContextArgument& argument) {
    return argument.type.kind != SimpleTypeKind::Unknown &&
           std::all_of(argument.arguments.begin(), argument.arguments.end(),
                       isMaterialized);
  };
  for (const InferenceState& state : states) {
    std::vector<TypeInfo> arguments;
    std::string applicationKey;
    bool complete = true;
    for (const TypeParameterInfo& parameter : symbol.typeParameters) {
      auto inferred = state.find(parameter.symbolName);
      if (inferred == state.end() || inferred->second.kind == SimpleTypeKind::Unknown) {
        complete = false;
        break;
      }
      arguments.push_back(inferred->second);
      applicationKey += inferred->second.name + ";";
    }
    if (!complete || !seenApplications.insert(applicationKey).second) {
      continue;
    }

    SymbolInfo specialized =
        specializeResolvedTypeApplication(symbol, arguments, span, false);
    const std::vector<TypedContextArgument> resolved = resolveContextArguments(
        specialized, firstContextParameter, scope, span, nullptr, false);
    const std::size_t expectedContextArguments = static_cast<std::size_t>(std::count(
        specialized.contextualParameters.begin() +
            static_cast<std::ptrdiff_t>(std::min(
                firstContextParameter, specialized.contextualParameters.size())),
        specialized.contextualParameters.end(), true));
    if (resolved.size() != expectedContextArguments ||
        !std::all_of(resolved.begin(), resolved.end(), isMaterialized)) {
      continue;
    }
    viable.push_back(std::move(specialized));
    viableArguments.push_back(arguments);
  }
  if (reportDiagnostics && viable.size() == 1) {
    viable.front() =
        specializeResolvedTypeApplication(symbol, viableArguments.front(), span, true);
  }
  if (resolvedTypeArguments != nullptr) {
    if (viable.size() == 1) {
      *resolvedTypeArguments = viableArguments.front();
    } else {
      resolvedTypeArguments->clear();
    }
  }
  return viable;
}

bool Typechecker::isAbstractTypeMember(const TypeInfo& type) const {
  if (type.abstractTypeMember) {
    return true;
  }
  auto symbol = globalSymbols_.find(type.name);
  return symbol != globalSymbols_.end() &&
         symbol->second.kind == AstDeclarationKind::Type &&
         !symbol->second.hasImplementation;
}

bool Typechecker::runtimeSignatureUsesAbstractType(const SymbolInfo& member) const {
  if (isAbstractTypeMember(member.type) && member.type.runtimeName.empty()) {
    return true;
  }
  return std::any_of(member.parameterTypes.begin(), member.parameterTypes.end(),
                     [&](const TypeInfo& type) {
                       return isAbstractTypeMember(type) && type.runtimeName.empty();
                     });
}

TypeInfo Typechecker::typeFromDeclaredName(const std::string& name, const Scope* scope,
                                           const support::SourceSpan* span) const {
  const std::string normalized = trim(name);
  if (normalized.empty()) {
    return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
  }
  const std::string compact = compactTypeName(normalized);
  const PolymorphicFunctionTypeSyntax polymorphicFunctionType =
      parsePolymorphicFunctionTypeSyntax(normalized);
  if (polymorphicFunctionType.polymorphic) {
    if (polymorphicFunctionType.malformed) {
      if (span != nullptr) {
        diagnostics_.error(
            *span, "unary polymorphic function type must have the form [A] => A => R");
      }
      return TypeInfo{SimpleTypeKind::Unknown, normalized};
    }
    Scope polymorphicScope = scope == nullptr ? Scope{} : *scope;
    TypeInfo typeParameter{SimpleTypeKind::Object,
                           polymorphicFunctionType.typeParameter};
    typeParameter.runtimeName = "Object";
    typeParameter.typeParameter = true;
    typeParameter.typeParameterSymbolName =
        "$declaredPoly$" + polymorphicFunctionType.typeParameter;
    SymbolInfo typeParameterSymbol;
    typeParameterSymbol.kind = AstDeclarationKind::Type;
    typeParameterSymbol.name = polymorphicFunctionType.typeParameter;
    typeParameterSymbol.symbolName = typeParameter.typeParameterSymbolName;
    typeParameterSymbol.type = typeParameter;
    typeParameterSymbol.lowerBound = TypeInfo{SimpleTypeKind::Nothing, "Nothing"};
    typeParameterSymbol.upperBound = TypeInfo{SimpleTypeKind::Object, "Object"};
    polymorphicScope[polymorphicFunctionType.typeParameter] =
        std::move(typeParameterSymbol);
    const TypeInfo resultType = typeFromDeclaredName(polymorphicFunctionType.resultType,
                                                     &polymorphicScope, span);
    if (resultType.kind == SimpleTypeKind::Unknown) {
      return TypeInfo{SimpleTypeKind::Unknown, normalized};
    }
    const SymbolInfo* polyFunction = typeSymbolForDeclaredName(
        std::string(support::StdNames::ScalaPolyFunction), scope);
    TypeInfo type;
    if (polyFunction != nullptr) {
      type = polyFunction->type;
    } else {
      type = TypeInfo{SimpleTypeKind::Object,
                      std::string(support::StdNames::ScalaPolyFunction)};
      type.runtimeName = std::string(support::StdNames::ScalaPolyFunction);
    }
    type.polymorphicFunctionType = true;
    type.typeArguments = {std::move(typeParameter), resultType};
    return type;
  }
  if (compact == "true" || compact == "false") {
    TypeInfo literal{SimpleTypeKind::Boolean, compact};
    literal.runtimeName = "Boolean";
    literal.singletonLiteral = compact;
    return literal;
  }
  if (!compact.empty() &&
      ((compact.front() >= '0' && compact.front() <= '9') ||
       compact.front() == '-')) {
    if (const std::optional<std::int64_t> value = parseIntegerConstant(compact)) {
      const bool isLong = compact.back() == 'l' || compact.back() == 'L';
      if (isLong || (*value >= std::numeric_limits<std::int32_t>::min() &&
                     *value <= std::numeric_limits<std::int32_t>::max())) {
        TypeInfo literal{isLong ? SimpleTypeKind::Long : SimpleTypeKind::Int,
                         compact};
        literal.runtimeName = isLong ? "Long" : "Int";
        literal.singletonLiteral = compact;
        return literal;
      }
      if (span != nullptr) {
        diagnostics_.error(*span,
                           "integer singleton type is outside the Int range: " +
                               compact);
      }
      return TypeInfo{SimpleTypeKind::Unknown, compact};
    }
    if (hasFloatingConstantSyntax(compact)) {
      const SimpleTypeKind kind = floatingConstantType(compact);
      if (parseFloatingConstant(compact, kind).has_value()) {
        TypeInfo literal{kind, compact};
        literal.runtimeName = kind == SimpleTypeKind::Float ? "Float" : "Double";
        literal.singletonLiteral.reserve(compact.size());
        std::copy_if(compact.begin(), compact.end(),
                     std::back_inserter(literal.singletonLiteral),
                     [](char ch) { return ch != '_'; });
        return literal;
      }
    }
  }
  if (normalized.size() >= 3 && normalized.front() == '\'' &&
      normalized.back() == '\'') {
    TypeInfo literal{SimpleTypeKind::Char, normalized};
    literal.runtimeName = "Char";
    literal.singletonLiteral = normalized;
    return literal;
  }
  if (normalized == "Nothing") {
    return TypeInfo{SimpleTypeKind::Nothing, "Nothing"};
  }
  if (normalized == "Unit") {
    return TypeInfo{SimpleTypeKind::Unit, "Unit"};
  }
  if (normalized == "Byte") {
    return TypeInfo{SimpleTypeKind::Byte, "Byte"};
  }
  if (normalized == "Short") {
    return TypeInfo{SimpleTypeKind::Short, "Short"};
  }
  if (normalized == "Int") {
    return TypeInfo{SimpleTypeKind::Int, "Int"};
  }
  if (normalized == "Long") {
    return TypeInfo{SimpleTypeKind::Long, "Long"};
  }
  if (normalized == "Float") {
    return TypeInfo{SimpleTypeKind::Float, "Float"};
  }
  if (normalized == "Double") {
    return TypeInfo{SimpleTypeKind::Double, "Double"};
  }
  if (normalized == "Boolean") {
    return TypeInfo{SimpleTypeKind::Boolean, "Boolean"};
  }
  if (normalized.size() >= 2 && normalized.front() == '"' && normalized.back() == '"') {
    TypeInfo literal{SimpleTypeKind::String, normalized};
    literal.runtimeName = "String";
    literal.singletonLiteral = normalized;
    literal.stringSingleton = true;
    return literal;
  }
  if (normalized == "String" || normalized == "java.lang.String" ||
      normalized == "scala.Predef.String") {
    return TypeInfo{SimpleTypeKind::String, "String"};
  }
  if (normalized == support::StdNames::ByteBuffer ||
      normalized == support::StdNames::JavaNioByteBuffer) {
    return TypeInfo{SimpleTypeKind::Object,
                    std::string(support::StdNames::JavaNioByteBuffer)};
  }
  if (normalized == "Char") {
    return TypeInfo{SimpleTypeKind::Char, "Char"};
  }
  if (normalized == "Symbol") {
    return TypeInfo{SimpleTypeKind::Symbol, "Symbol"};
  }
  if (normalized == "Any") {
    return TypeInfo{SimpleTypeKind::Object, "Object"};
  }
  if (normalized == "Null") {
    return TypeInfo{SimpleTypeKind::Null, "Null"};
  }
  if (const auto tupleElements = consTupleTypeElements(normalized);
      tupleElements.has_value()) {
    if (tupleElements->size() > 22) {
      if (span != nullptr) {
        diagnostics_.error(*span,
                           "*: tuple types support at most 22 elements in this "
                           "subset");
      }
      return TypeInfo{SimpleTypeKind::Unknown, normalized};
    }
    const std::string constructorName =
        std::string(support::StdNames::ScalaTuple) +
        std::to_string(tupleElements->size());
    const SymbolInfo* constructor =
        typeSymbolForDeclaredName(constructorName, scope);
    if (constructor == nullptr) {
      if (span != nullptr) {
        diagnostics_.error(*span,
                           "unresolved tuple type constructor: " + constructorName);
      }
      return TypeInfo{SimpleTypeKind::Unknown, normalized};
    }
    std::vector<TypeInfo> elementTypes;
    elementTypes.reserve(tupleElements->size());
    for (const std::string& element : *tupleElements) {
      elementTypes.push_back(typeFromDeclaredName(element, scope, span));
    }
    return specializeResolvedTypeApplication(
               *constructor, elementTypes,
               span == nullptr ? support::SourceSpan::none() : *span,
               span != nullptr)
        .type;
  }
  if (const auto tupleElements = parenthesizedTupleTypeElements(normalized);
      tupleElements.has_value()) {
    const std::string constructorName =
        std::string(support::StdNames::ScalaTuple) +
        std::to_string(tupleElements->size());
    const SymbolInfo* constructor =
        typeSymbolForDeclaredName(constructorName, scope);
    if (constructor == nullptr) {
      if (span != nullptr) {
        diagnostics_.error(*span,
                           "unresolved tuple type constructor: " + constructorName);
      }
      return TypeInfo{SimpleTypeKind::Unknown, normalized};
    }
    std::vector<TypeInfo> elementTypes;
    elementTypes.reserve(tupleElements->size());
    for (const std::string& element : *tupleElements) {
      elementTypes.push_back(typeFromDeclaredName(element, scope, span));
    }
    return specializeResolvedTypeApplication(
               *constructor, elementTypes,
               span == nullptr ? support::SourceSpan::none() : *span,
               span != nullptr)
        .type;
  }

  bool malformedComposite = false;
  const std::string normalizedComposite =
      normalizeGivenImportType(normalized, &malformedComposite);
  const CompositeTypeSyntax compositeSyntax =
      malformedComposite ? CompositeTypeSyntax{}
                         : parseCompositeTypeSyntax(normalizedComposite);
  const AppliedTypeSyntax directApplied =
      parseAppliedTypeSyntax(normalizedComposite);
  const bool resolvedDirectApplication =
      directApplied.applied && !directApplied.malformed &&
      typeSymbolForDeclaredName(directApplied.constructor, scope) != nullptr;
  const bool symbolicCompositeApplication =
      normalizedComposite.starts_with("|[") ||
      normalizedComposite.starts_with("&[") ||
      (resolvedDirectApplication &&
       (directApplied.constructor.find('|') != std::string::npos ||
        directApplied.constructor.find('&') != std::string::npos));
  if (!symbolicCompositeApplication &&
      (malformedComposite || compositeSyntax.malformed)) {
    if (span != nullptr) {
      diagnostics_.error(*span,
                         "malformed intersection or union type: " + normalized);
    }
    return TypeInfo{SimpleTypeKind::Unknown, normalized};
  }
  if (!symbolicCompositeApplication &&
      compositeSyntax.kind != CompositeTypeSyntaxKind::None) {
    const CompositeTypeKind kind =
        compositeSyntax.kind == CompositeTypeSyntaxKind::Union
            ? CompositeTypeKind::Union
            : CompositeTypeKind::Intersection;
    std::vector<TypeInfo> operands;
    operands.reserve(compositeSyntax.operands.size());
    for (const std::string& operand : compositeSyntax.operands) {
      operands.push_back(typeFromDeclaredName(operand, scope, span));
    }
    return makeCompositeType(kind, std::move(operands));
  }
  if (normalizedComposite != compactTypeName(normalized)) {
    return typeFromDeclaredName(normalizedComposite, scope, span);
  }

  if (const std::string elementName = arrayElementTypeName(normalized);
      !elementName.empty()) {
    const TypeInfo elementType = typeFromDeclaredName(elementName, scope, span);
    if (elementType.kind == SimpleTypeKind::Unknown) {
      return TypeInfo{SimpleTypeKind::Unknown, normalized};
    }
    return TypeInfo{SimpleTypeKind::Object, arrayTypeName(elementType)};
  }

  const AppliedTypeSyntax applied = parseAppliedTypeSyntax(normalized);
  if (applied.applied) {
    if (applied.malformed) {
      if (span != nullptr) {
        diagnostics_.error(*span, "malformed applied type: " + normalized);
      }
      return TypeInfo{SimpleTypeKind::Unknown, normalized};
    }
    const SymbolInfo* constructor =
        typeSymbolForDeclaredName(applied.constructor, scope);
    const bool supportedConstructor =
        constructor != nullptr &&
        (constructor->kind == AstDeclarationKind::Class ||
         constructor->kind == AstDeclarationKind::Trait ||
         (constructor->kind == AstDeclarationKind::Type &&
          constructor->hasImplementation));
    if (!supportedConstructor) {
      if (span != nullptr) {
        diagnostics_.error(*span, "unresolved generic type constructor: " +
                                      applied.constructor);
      }
      return TypeInfo{SimpleTypeKind::Unknown, normalized};
    }
    const Scope emptyScope;
    const SymbolInfo specialized = specializeTypeApplication(
        *constructor, applied.arguments, scope == nullptr ? emptyScope : *scope,
        span == nullptr ? support::SourceSpan::none() : *span, span != nullptr);
    return specialized.type;
  }

  auto resolvedTypeSymbol = [](const SymbolInfo& symbol) {
    if (symbol.type.typeParameter) {
      return symbol.type;
    }
    if (symbol.kind == AstDeclarationKind::Type && !symbol.hasImplementation) {
      TypeInfo type = symbol.type;
      type.abstractTypeMember = true;
      if (isReferenceType(symbol.upperBound) && !symbol.upperBound.abstractTypeMember) {
        type.runtimeName = symbol.upperBound.name;
      } else if (symbol.upperBound.kind == SimpleTypeKind::Unknown ||
                 isBoxablePrimitiveType(symbol.upperBound.kind)) {
        type.runtimeName = "Object";
      }
      return type;
    }
    if (symbol.kind == AstDeclarationKind::Type && symbol.hasImplementation) {
      TypeInfo type = symbol.type;
      const std::size_t separator = symbol.symbolName.rfind('.');
      if (separator != std::string::npos && separator + 1 < symbol.symbolName.size()) {
        type.dependentOwnerName = symbol.symbolName.substr(0, separator);
        type.dependentMemberName = symbol.symbolName.substr(separator + 1);
      }
      type.resolvedAliasName = symbol.symbolName;
      return type;
    }
    return symbol.type;
  };

  const std::size_t projection = normalized.find('#');
  if (projection != std::string::npos) {
    if (projection == 0 || projection + 1 >= normalized.size() ||
        normalized.find('#', projection + 1) != std::string::npos) {
      if (span != nullptr) {
        diagnostics_.error(*span, "malformed type projection: " + normalized);
      }
      return TypeInfo{SimpleTypeKind::Unknown, normalized};
    }

    const std::string ownerName = normalized.substr(0, projection);
    const std::string memberName = normalized.substr(projection + 1);
    const SymbolInfo* owner = typeSymbolForDeclaredName(ownerName, scope);
    if (owner == nullptr || (owner->kind != AstDeclarationKind::Class &&
                             owner->kind != AstDeclarationKind::Trait &&
                             owner->kind != AstDeclarationKind::Type)) {
      if (span != nullptr) {
        diagnostics_.error(*span, "unresolved type projection owner: " + ownerName);
      }
      return TypeInfo{SimpleTypeKind::Unknown, normalized};
    }

    const TypeInfo ownerType = resolvedTypeSymbol(*owner);
    auto members = memberScopes_.find(ownerType.name);
    if (members == memberScopes_.end()) {
      if (span != nullptr) {
        diagnostics_.error(*span, "type projection owner " + ownerType.name +
                                      " has no known members");
      }
      return TypeInfo{SimpleTypeKind::Unknown, normalized};
    }
    auto member = members->second.find(memberName);
    if (member == members->second.end()) {
      if (span != nullptr) {
        diagnostics_.error(*span, "unresolved projected type member " + memberName +
                                      " on " + ownerType.name);
      }
      return TypeInfo{SimpleTypeKind::Unknown, normalized};
    }
    if (member->second.kind != AstDeclarationKind::Type) {
      if (span != nullptr) {
        diagnostics_.error(*span, "projected member " + memberName + " on " +
                                      ownerType.name + " is not a type member");
      }
      return TypeInfo{SimpleTypeKind::Unknown, normalized};
    }
    if (member->second.hasImplementation) {
      TypeInfo selected = member->second.type;
      selected.dependentOwnerName = ownerType.name;
      selected.dependentMemberName = memberName;
      selected.resolvedAliasName = member->second.symbolName;
      selected.typeProjection = true;
      return selected;
    }

    TypeInfo selected = member->second.type;
    selected.name = ownerType.name + "#" + memberName;
    selected.dependentOwnerName = ownerType.name;
    selected.dependentMemberName = memberName;
    selected.abstractTypeMember = true;
    selected.typeProjection = true;
    if (isReferenceType(member->second.upperBound) &&
        !member->second.upperBound.abstractTypeMember) {
      selected.runtimeName = member->second.upperBound.name;
    } else if (member->second.upperBound.kind == SimpleTypeKind::Unknown ||
               isBoxablePrimitiveType(member->second.upperBound.kind)) {
      selected.runtimeName = "Object";
    }
    return selected;
  }

  if (scope != nullptr && normalized.find('.') == std::string::npos) {
    auto found = scope->find(normalized);
    if (found != scope->end() && (found->second.kind == AstDeclarationKind::Type ||
                                  found->second.kind == AstDeclarationKind::Object ||
                                  found->second.kind == AstDeclarationKind::Class ||
                                  found->second.kind == AstDeclarationKind::Trait)) {
      if (span != nullptr && !found->second.typeParameters.empty()) {
        diagnostics_.error(*span,
                           "generic type " + normalized + " requires " +
                               std::to_string(found->second.typeParameters.size()) +
                               " explicit type arguments");
      }
      return resolvedTypeSymbol(found->second);
    }
  }

  auto global = globalSymbols_.find(normalized);
  if (global != globalSymbols_.end() &&
      (global->second.kind == AstDeclarationKind::Type ||
       global->second.kind == AstDeclarationKind::Object ||
       global->second.kind == AstDeclarationKind::Class ||
       global->second.kind == AstDeclarationKind::Trait)) {
    if (span != nullptr && !global->second.typeParameters.empty()) {
      diagnostics_.error(*span,
                         "generic type " + normalized + " requires " +
                             std::to_string(global->second.typeParameters.size()) +
                             " explicit type arguments");
    }
    return resolvedTypeSymbol(global->second);
  }

  if (const SymbolInfo* qualified = qualifiedPathSymbol(normalized, scope);
      qualified != nullptr && (qualified->kind == AstDeclarationKind::Object ||
                               qualified->kind == AstDeclarationKind::Class ||
                               qualified->kind == AstDeclarationKind::Trait)) {
    if (span != nullptr && !qualified->typeParameters.empty()) {
      diagnostics_.error(*span, "generic type " + normalized + " requires " +
                                    std::to_string(qualified->typeParameters.size()) +
                                    " explicit type arguments");
    }
    return resolvedTypeSymbol(*qualified);
  }

  const std::size_t firstDot = normalized.find('.');
  if (scope != nullptr && firstDot != std::string::npos) {
    const std::string rootName = normalized.substr(0, firstDot);
    auto root = scope->find(rootName);
    if (root != scope->end() && (root->second.kind == AstDeclarationKind::Val ||
                                 root->second.kind == AstDeclarationKind::Var ||
                                 root->second.kind == AstDeclarationKind::Def ||
                                 root->second.kind == AstDeclarationKind::Object)) {
      if (root->second.kind == AstDeclarationKind::Var ||
          root->second.kind == AstDeclarationKind::Def) {
        if (span != nullptr) {
          diagnostics_.error(*span, "unstable path-dependent type prefix: " + rootName +
                                        (root->second.kind == AstDeclarationKind::Var
                                             ? " is a variable"
                                             : " is a method"));
        }
        return TypeInfo{SimpleTypeKind::Unknown, normalized};
      }

      TypeInfo receiverType = root->second.type;
      std::size_t segmentStart = firstDot + 1;
      while (segmentStart < normalized.size()) {
        const std::size_t nextDot = normalized.find('.', segmentStart);
        const std::string segment = normalized.substr(
            segmentStart,
            nextDot == std::string::npos ? std::string::npos : nextDot - segmentStart);
        std::string memberOwner = receiverType.typeConstructorName.empty()
                                      ? receiverType.name
                                      : receiverType.typeConstructorName;
        auto members = memberScopes_.find(memberOwner);
        if (members == memberScopes_.end() && !receiverType.runtimeName.empty() &&
            receiverType.runtimeName != memberOwner) {
          memberOwner = receiverType.runtimeName;
          members = memberScopes_.find(memberOwner);
        }
        if (receiverType.kind != SimpleTypeKind::Object ||
            members == memberScopes_.end()) {
          if (span != nullptr) {
            diagnostics_.error(*span, "stable path " + rootName + " has type " +
                                          receiverType.name +
                                          " with no selectable type members");
          }
          return TypeInfo{SimpleTypeKind::Unknown, normalized};
        }
        auto member = members->second.find(segment);
        if (member == members->second.end()) {
          if (span != nullptr) {
            diagnostics_.error(*span, "unresolved path-dependent type member " +
                                          segment + " on " + receiverType.name);
          }
          return TypeInfo{SimpleTypeKind::Unknown, normalized};
        }
        const SymbolInfo specializedMember =
            specializeMemberForReceiver(member->second, receiverType);

        const bool isFinal = nextDot == std::string::npos;
        if (isFinal) {
          if (specializedMember.kind != AstDeclarationKind::Type) {
            if (span != nullptr) {
              diagnostics_.error(*span, "selected path member " + segment + " on " +
                                            receiverType.name +
                                            " is not a type member");
            }
            return TypeInfo{SimpleTypeKind::Unknown, normalized};
          }
          if (specializedMember.hasImplementation) {
            TypeInfo selected = specializedMember.type;
            selected.dependentOwnerName = receiverType.name;
            selected.dependentMemberName = segment;
            selected.dependentPathName = normalized;
            selected.resolvedAliasName = specializedMember.symbolName;
            selected.pathDependent = true;
            return selected;
          }
          TypeInfo selected = specializedMember.type;
          selected.name = normalized;
          selected.dependentOwnerName = receiverType.name;
          selected.dependentMemberName = segment;
          selected.dependentPathName = normalized;
          selected.abstractTypeMember = true;
          selected.pathDependent = true;
          if (isReferenceType(specializedMember.upperBound) &&
              !specializedMember.upperBound.abstractTypeMember) {
            selected.runtimeName = specializedMember.upperBound.name;
          } else if (specializedMember.upperBound.kind == SimpleTypeKind::Unknown ||
                     isBoxablePrimitiveType(specializedMember.upperBound.kind)) {
            selected.runtimeName = "Object";
          }
          return selected;
        }

        if (specializedMember.kind != AstDeclarationKind::Val &&
            specializedMember.kind != AstDeclarationKind::Object) {
          if (span != nullptr) {
            diagnostics_.error(*span, "unstable nested path-dependent type prefix: " +
                                          segment);
          }
          return TypeInfo{SimpleTypeKind::Unknown, normalized};
        }
        receiverType = specializedMember.type;
        segmentStart = nextDot + 1;
      }
    }
  }
  return TypeInfo{SimpleTypeKind::Object, normalized};
}

TypeInfo Typechecker::preliminaryDeclarationType(const AstDeclaration& declaration,
                                                 const Scope* scope) const {
  TypeInfo declared = typeFromDeclaredName(declaration.declaredType, scope);
  if (declared.kind != SimpleTypeKind::Unknown) {
    return declared;
  }
  switch (declaration.kind) {
  case AstDeclarationKind::Package:
  case AstDeclarationKind::Import:
    return TypeInfo{SimpleTypeKind::Unit, "Unit"};
  case AstDeclarationKind::Object:
  case AstDeclarationKind::Class:
  case AstDeclarationKind::Trait:
    return TypeInfo{SimpleTypeKind::Object, declaration.name};
  case AstDeclarationKind::Type:
    return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
  case AstDeclarationKind::Def:
  case AstDeclarationKind::Val:
  case AstDeclarationKind::Var:
    return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
  }
  return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
}

TypeInfo Typechecker::commonType(const TypeInfo& lhs, const TypeInfo& rhs) const {
  if (lhs.kind == rhs.kind && lhs.name == rhs.name &&
      lhs.compositeKind == rhs.compositeKind) {
    return lhs;
  }
  if (lhs.kind == SimpleTypeKind::Unknown) {
    return rhs;
  }
  if (rhs.kind == SimpleTypeKind::Unknown) {
    return lhs;
  }
  if (lhs.kind == SimpleTypeKind::Nothing) {
    return rhs;
  }
  if (rhs.kind == SimpleTypeKind::Nothing) {
    return lhs;
  }
  if (isAssignable(lhs, rhs)) {
    return lhs;
  }
  if (isAssignable(rhs, lhs)) {
    return rhs;
  }

  auto rank = [](SimpleTypeKind kind) {
    switch (kind) {
    case SimpleTypeKind::Byte:
      return 1;
    case SimpleTypeKind::Short:
      return 2;
    case SimpleTypeKind::Int:
      return 3;
    case SimpleTypeKind::Long:
      return 4;
    case SimpleTypeKind::Float:
      return 5;
    case SimpleTypeKind::Double:
      return 6;
    default:
      return 0;
    }
  };

  const int lhsRank = rank(lhs.kind);
  const int rhsRank = rank(rhs.kind);
  if (lhsRank != 0 && rhsRank != 0) {
    return lhsRank >= rhsRank ? lhs : rhs;
  }

  const auto supportsSoftUnion = [](const TypeInfo& type) {
    return type.kind == SimpleTypeKind::Unit || type.kind == SimpleTypeKind::Boolean ||
           type.kind == SimpleTypeKind::Byte || type.kind == SimpleTypeKind::Short ||
           type.kind == SimpleTypeKind::Int || type.kind == SimpleTypeKind::Long ||
           type.kind == SimpleTypeKind::Float || type.kind == SimpleTypeKind::Double ||
           type.kind == SimpleTypeKind::Char || type.kind == SimpleTypeKind::String ||
           type.kind == SimpleTypeKind::Symbol || type.kind == SimpleTypeKind::Null ||
           type.kind == SimpleTypeKind::Object;
  };
  if (supportsSoftUnion(lhs) && supportsSoftUnion(rhs)) {
    TypeInfo result = makeCompositeType(CompositeTypeKind::Union, {lhs, rhs});
    result.softUnion = result.compositeKind == CompositeTypeKind::Union;
    return result;
  }
  return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
}

std::vector<TypeInfo> Typechecker::baseTypesFor(const TypeInfo& type) const {
  const auto baseName = [](const TypeInfo& base) {
    return base.typeConstructorName.empty()
               ? (base.runtimeName.empty() ? base.name : base.runtimeName)
               : base.typeConstructorName;
  };
  const auto mergeAppliedBase =
      [&](const TypeInfo& lhs, const TypeInfo& rhs) -> std::optional<TypeInfo> {
    const std::string lhsName = baseName(lhs);
    if (lhsName.empty() || lhsName != baseName(rhs)) {
      return std::nullopt;
    }
    if (lhs.name == rhs.name && lhs.compositeKind == rhs.compositeKind) {
      return lhs;
    }

    auto symbol = globalSymbols_.find(lhsName);
    if (symbol == globalSymbols_.end() ||
        symbol->second.typeParameters.empty() ||
        lhs.typeConstructorName != lhsName || rhs.typeConstructorName != lhsName ||
        lhs.typeArguments.size() != symbol->second.typeParameters.size() ||
        rhs.typeArguments.size() != symbol->second.typeParameters.size()) {
      return std::nullopt;
    }

    std::vector<TypeInfo> arguments;
    arguments.reserve(lhs.typeArguments.size());
    for (std::size_t i = 0; i < lhs.typeArguments.size(); ++i) {
      const TypeInfo& left = lhs.typeArguments[i];
      const TypeInfo& right = rhs.typeArguments[i];
      switch (symbol->second.typeParameters[i].variance) {
      case TypeVariance::Invariant:
        if (left.name != right.name ||
            left.compositeKind != right.compositeKind) {
          return std::nullopt;
        }
        arguments.push_back(left);
        break;
      case TypeVariance::Covariant: {
        TypeInfo merged = commonType(left, right);
        if (merged.kind == SimpleTypeKind::Unknown) {
          return std::nullopt;
        }
        arguments.push_back(widenSoftUnion(merged));
        break;
      }
      case TypeVariance::Contravariant:
        if (isAssignable(left, right)) {
          arguments.push_back(right);
        } else if (isAssignable(right, left)) {
          arguments.push_back(left);
        } else {
          arguments.push_back(
              makeCompositeType(CompositeTypeKind::Intersection, {left, right}));
        }
        break;
      }
    }
    return specializeResolvedTypeApplication(
               symbol->second, arguments, support::SourceSpan::none(), false)
        .type;
  };

  if (type.compositeKind == CompositeTypeKind::Intersection) {
    std::vector<TypeInfo> result;
    for (const TypeInfo& operand : type.compositeTypes) {
      for (TypeInfo base : baseTypesFor(operand)) {
        const std::string name = baseName(base);
        const bool present =
            std::any_of(result.begin(), result.end(), [&](const TypeInfo& candidate) {
              return baseName(candidate) == name;
            });
        if (!present) {
          result.push_back(std::move(base));
        }
      }
    }
    return result;
  }
  if (type.compositeKind == CompositeTypeKind::Union) {
    if (type.compositeTypes.empty()) {
      return {};
    }
    std::vector<TypeInfo> common = baseTypesFor(type.compositeTypes.front());
    for (std::size_t i = 1; i < type.compositeTypes.size(); ++i) {
      const std::vector<TypeInfo> alternatives =
          baseTypesFor(type.compositeTypes[i]);
      for (auto candidate = common.begin(); candidate != common.end();) {
        auto alternative =
            std::find_if(alternatives.begin(), alternatives.end(),
                         [&](const TypeInfo& other) {
                           return baseName(*candidate) == baseName(other);
                         });
        if (alternative == alternatives.end()) {
          candidate = common.erase(candidate);
          continue;
        }
        std::optional<TypeInfo> merged =
            mergeAppliedBase(*candidate, *alternative);
        if (!merged.has_value()) {
          candidate = common.erase(candidate);
          continue;
        }
        *candidate = std::move(*merged);
        ++candidate;
      }
    }
    return common;
  }

  std::string owner =
      type.typeConstructorName.empty() ? type.name : type.typeConstructorName;
  if ((!globalSymbols_.contains(owner) || !memberScopes_.contains(owner)) &&
      !type.runtimeName.empty()) {
    owner = type.runtimeName;
  }
  if (owner.empty()) {
    return {};
  }

  std::vector<TypeInfo> directParents;
  if (auto symbol = globalSymbols_.find(owner); symbol != globalSymbols_.end()) {
    directParents.reserve(symbol->second.parentTypes.size());
    for (const TypeInfo& parent : symbol->second.parentTypes) {
      directParents.push_back(specializeTypeForReceiver(parent, type));
    }
  }
  const std::unordered_map<std::string, TypeInfo> effectiveParents =
      effectiveParentTypes(directParents);

  std::vector<TypeInfo> result;
  for (const std::string& name :
       linearizedParentsFor({owner}, globalSymbols_)) {
    if (name == owner) {
      result.push_back(type);
      continue;
    }
    if (auto parent = effectiveParents.find(name);
        parent != effectiveParents.end()) {
      result.push_back(parent->second);
      continue;
    }
    if (auto symbol = globalSymbols_.find(name);
        symbol != globalSymbols_.end()) {
      result.push_back(symbol->second.type);
    }
  }
  return result;
}

std::vector<std::string> Typechecker::baseTypeNamesFor(const TypeInfo& type) const {
  std::vector<std::string> result;
  for (const TypeInfo& base : baseTypesFor(type)) {
    std::string name =
        base.typeConstructorName.empty()
            ? (base.runtimeName.empty() ? base.name : base.runtimeName)
            : base.typeConstructorName;
    if (!name.empty() &&
        std::find(result.begin(), result.end(), name) == result.end()) {
      result.push_back(std::move(name));
    }
  }
  return result;
}

TypeInfo Typechecker::widenSoftUnion(const TypeInfo& type) const {
  if (!type.softUnion || type.compositeKind != CompositeTypeKind::Union) {
    return type;
  }

  const auto isTransparentBase = [&](const std::string& name) {
    static const std::unordered_set<std::string> transparent{
        "Any",
        "scala.Any",
        "AnyVal",
        "scala.AnyVal",
        "Object",
        std::string(support::StdNames::JavaLangObject),
        "Matchable",
        "scala.Matchable",
        "scala.Product",
        "java.lang.Comparable",
        "java.io.Serializable",
    };
    if (transparent.contains(name)) {
      return true;
    }
    auto symbol = globalSymbols_.find(name);
    return symbol != globalSymbols_.end() && symbol->second.isTransparent;
  };

  const auto baseName = [](const TypeInfo& base) {
    return base.typeConstructorName.empty()
               ? (base.runtimeName.empty() ? base.name : base.runtimeName)
               : base.typeConstructorName;
  };
  const std::vector<TypeInfo> commonBases = baseTypesFor(type);
  std::vector<TypeInfo> visibleBases;
  for (const TypeInfo& base : commonBases) {
    const std::string name = baseName(base);
    auto symbol = globalSymbols_.find(name);
    if (symbol == globalSymbols_.end() ||
        !isInheritableDeclaration(symbol->second.kind) ||
        (!symbol->second.typeParameters.empty() &&
         (base.typeConstructorName != name ||
          base.typeArguments.size() != symbol->second.typeParameters.size())) ||
        isTransparentBase(name)) {
      continue;
    }
    const bool hasMoreSpecificCommonBase = std::any_of(
        commonBases.begin(), commonBases.end(), [&](const TypeInfo& candidate) {
          const std::string candidateName = baseName(candidate);
          return candidateName != name && !isTransparentBase(candidateName) &&
                 isSubtypeOf(candidateName, name);
        });
    if (!hasMoreSpecificCommonBase) {
      visibleBases.push_back(base);
    }
  }

  return visibleBases.empty() ? type
                              : makeCompositeType(CompositeTypeKind::Intersection,
                                                  std::move(visibleBases));
}

bool Typechecker::isAssignable(const TypeInfo& expected, const TypeInfo& actual) const {
  std::unordered_set<std::string> visiting;
  std::function<bool(const TypeInfo&, const TypeInfo&, bool)> conforms;
  conforms = [&](const TypeInfo& target, const TypeInfo& value,
                 bool allowNumericWidening) {
    if (target.kind == SimpleTypeKind::Unknown ||
        value.kind == SimpleTypeKind::Unknown) {
      return true;
    }
    if (value.kind == SimpleTypeKind::Nothing) {
      return true;
    }
    if (value.compositeKind == CompositeTypeKind::Union) {
      return std::all_of(value.compositeTypes.begin(), value.compositeTypes.end(),
                         [&](const TypeInfo& operand) {
                           return conforms(target, operand, allowNumericWidening);
                         });
    }
    if (target.compositeKind == CompositeTypeKind::Intersection) {
      return std::all_of(
          target.compositeTypes.begin(), target.compositeTypes.end(),
          [&](const TypeInfo& operand) {
            return conforms(operand, value, allowNumericWidening);
          });
    }
    if (target.compositeKind == CompositeTypeKind::Union) {
      return std::any_of(
          target.compositeTypes.begin(), target.compositeTypes.end(),
          [&](const TypeInfo& operand) {
            return conforms(operand, value, allowNumericWidening);
          });
    }
    if (value.compositeKind == CompositeTypeKind::Intersection) {
      return std::any_of(
          value.compositeTypes.begin(), value.compositeTypes.end(),
          [&](const TypeInfo& operand) {
            return conforms(target, operand, allowNumericWidening);
          });
    }
    if (!target.singletonLiteral.empty()) {
      return value.singletonLiteral == target.singletonLiteral &&
             target.kind == value.kind;
    }
    if (target.stringSingleton) {
      return value.stringSingleton && target.name == value.name;
    }
    if (value.kind == SimpleTypeKind::Null && isReferenceType(target)) {
      return true;
    }
    if (target.polymorphicFunctionType) {
      if (!value.polymorphicFunctionType || target.typeArguments.size() != 2 ||
          value.typeArguments.size() != 2 ||
          !target.typeArguments.front().typeParameter ||
          !value.typeArguments.front().typeParameter) {
        return false;
      }
      TypeInfo canonicalParameter{SimpleTypeKind::Object, "$polytype"};
      canonicalParameter.runtimeName = "Object";
      std::unordered_map<std::string, TypeInfo> targetSubstitution;
      targetSubstitution[target.typeArguments.front().typeParameterSymbolName] =
          canonicalParameter;
      std::unordered_map<std::string, TypeInfo> valueSubstitution;
      valueSubstitution[value.typeArguments.front().typeParameterSymbolName] =
          canonicalParameter;
      const TypeInfo targetResult =
          substituteTypeParameters(target.typeArguments[1], targetSubstitution);
      const TypeInfo valueResult =
          substituteTypeParameters(value.typeArguments[1], valueSubstitution);
      return typesMatchForOverride(targetResult, valueResult);
    }

    const std::string targetConstructor = target.typeConstructorName;
    const std::string valueConstructor = value.typeConstructorName;
    if (!targetConstructor.empty()) {
      const std::string visitKey = target.name + " <- " + value.name;
      if (!visiting.insert(visitKey).second) {
        return false;
      }

      if (targetConstructor == valueConstructor &&
          target.typeArguments.size() == value.typeArguments.size()) {
        auto constructor = globalSymbols_.find(targetConstructor);
        if (constructor == globalSymbols_.end() ||
            constructor->second.typeParameters.size() != target.typeArguments.size()) {
          visiting.erase(visitKey);
          return false;
        }
        for (std::size_t i = 0; i < target.typeArguments.size(); ++i) {
          const TypeInfo& targetArgument = target.typeArguments[i];
          const TypeInfo& valueArgument = value.typeArguments[i];
          const TypeVariance variance = constructor->second.typeParameters[i].variance;
          bool argumentConforms = false;
          if (variance == TypeVariance::Covariant) {
            argumentConforms = conforms(targetArgument, valueArgument, false);
          } else if (variance == TypeVariance::Contravariant) {
            argumentConforms = conforms(valueArgument, targetArgument, false);
          } else {
            argumentConforms = targetArgument.name == valueArgument.name;
          }
          if (!argumentConforms) {
            visiting.erase(visitKey);
            return false;
          }
        }
        visiting.erase(visitKey);
        return true;
      }

      const std::string valueName =
          valueConstructor.empty()
              ? (value.runtimeName.empty() ? value.name : value.runtimeName)
              : valueConstructor;
      auto valueSymbol = globalSymbols_.find(valueName);
      if (valueSymbol != globalSymbols_.end()) {
        for (const TypeInfo& parentPattern : valueSymbol->second.parentTypes) {
          const TypeInfo parent = specializeTypeForReceiver(parentPattern, value);
          if (conforms(target, parent, false)) {
            visiting.erase(visitKey);
            return true;
          }
        }
      }
      visiting.erase(visitKey);
      return false;
    }

    if (!valueConstructor.empty()) {
      TypeInfo erasedValue{SimpleTypeKind::Object, value.runtimeName.empty()
                                                       ? valueConstructor
                                                       : value.runtimeName};
      if (!conforms(target, erasedValue, allowNumericWidening)) {
        return false;
      }
      return true;
    }

    if (target.kind == SimpleTypeKind::Object) {
      if (target.name == "Object") {
        return value.kind == SimpleTypeKind::Object ||
               value.kind == SimpleTypeKind::String;
      }
      if (value.kind != SimpleTypeKind::Object) {
        return false;
      }
      if (target.typeProjection && value.pathDependent &&
          !target.dependentOwnerName.empty() &&
          target.dependentMemberName == value.dependentMemberName &&
          (value.dependentOwnerName == target.dependentOwnerName ||
           isSubtypeOf(value.dependentOwnerName, target.dependentOwnerName))) {
        return true;
      }
      const std::string& valueName =
          value.runtimeName.empty() ? value.name : value.runtimeName;
      return target.name == value.name || target.name == valueName ||
             isSubtypeOf(valueName, target.name);
    }
    if (target.kind == value.kind) {
      return true;
    }
    if (!allowNumericWidening) {
      return false;
    }
    if (target.kind == SimpleTypeKind::Double) {
      return value.kind == SimpleTypeKind::Float ||
             value.kind == SimpleTypeKind::Long || value.kind == SimpleTypeKind::Int ||
             value.kind == SimpleTypeKind::Short || value.kind == SimpleTypeKind::Byte;
    }
    if (target.kind == SimpleTypeKind::Float) {
      return value.kind == SimpleTypeKind::Long || value.kind == SimpleTypeKind::Int ||
             value.kind == SimpleTypeKind::Short || value.kind == SimpleTypeKind::Byte;
    }
    if (target.kind == SimpleTypeKind::Long) {
      return value.kind == SimpleTypeKind::Int || value.kind == SimpleTypeKind::Short ||
             value.kind == SimpleTypeKind::Byte;
    }
    if (target.kind == SimpleTypeKind::Int) {
      return value.kind == SimpleTypeKind::Short || value.kind == SimpleTypeKind::Byte;
    }
    if (target.kind == SimpleTypeKind::Short) {
      return value.kind == SimpleTypeKind::Byte;
    }
    return false;
  };
  return conforms(expected, actual, true);
}

std::vector<TypedContextArgument> Typechecker::resolveContextArguments(
    const SymbolInfo& callee, std::size_t firstContextParameter, Scope& scope,
    const support::SourceSpan& span, std::unordered_set<std::string>* resolving,
    bool reportDiagnostics, ContextResolutionFailure* failure) const {
  struct ContextCandidate {
    std::string referenceName;
    SymbolInfo symbol;
    std::optional<TypedContextArgument> materializedArgument;
    ContextResolutionFailure failure = ContextResolutionFailure::None;
  };
  const auto sortCandidates = [](std::vector<ContextCandidate>& candidates) {
    std::sort(candidates.begin(), candidates.end(),
              [](const ContextCandidate& lhs, const ContextCandidate& rhs) {
                return lhs.symbol.symbolName < rhs.symbol.symbolName;
              });
  };
  std::unordered_set<std::string> localResolving;
  if (resolving == nullptr) {
    resolving = &localResolving;
  }
  const auto unknownArgument = [](std::size_t parameterIndex) {
    TypedContextArgument argument;
    argument.type = TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
    argument.parameterIndex = parameterIndex;
    return argument;
  };
  const auto mergeFailure = [&](ContextResolutionFailure candidate) {
    if (failure == nullptr) {
      return;
    }
    const auto priority = [](ContextResolutionFailure value) {
      switch (value) {
      case ContextResolutionFailure::None:
        return 0;
      case ContextResolutionFailure::Missing:
        return 1;
      case ContextResolutionFailure::Unsupported:
        return 2;
      case ContextResolutionFailure::Diverging:
        return 3;
      case ContextResolutionFailure::Ambiguous:
        return 4;
      }
      return 0;
    };
    if (priority(candidate) > priority(*failure)) {
      *failure = candidate;
    }
  };

  std::vector<TypedContextArgument> result;
  for (std::size_t i = firstContextParameter; i < callee.parameterTypes.size(); ++i) {
    if (i >= callee.contextualParameters.size() || !callee.contextualParameters[i]) {
      continue;
    }
    const TypeInfo& expected = callee.parameterTypes[i];
    std::vector<ContextCandidate> parameterCandidates;
    std::vector<ContextCandidate> givenCandidates;
    const auto addCandidate = [&](std::vector<ContextCandidate>& candidates,
                                  const std::string& referenceName,
                                  const SymbolInfo& rawCandidate) {
      SymbolInfo candidate = rawCandidate;
      if (!candidate.typeParameters.empty()) {
        candidate = inferTypeApplication(candidate, {}, span, &expected, false);
        if (!candidate.typeParameters.empty()) {
          return;
        }
      }
      if (candidate.type.kind == SimpleTypeKind::Unknown ||
          !isAssignable(expected, candidate.type)) {
        return;
      }
      candidates.push_back(ContextCandidate{referenceName, std::move(candidate),
                                            std::nullopt,
                                            ContextResolutionFailure::None});
    };
    for (const auto& [name, candidate] : scope) {
      if (!candidate.isGiven && !candidate.isContextParameter) {
        continue;
      }
      addCandidate(candidate.isContextParameter ? parameterCandidates : givenCandidates,
                   name, candidate);
    }

    if (!givenCandidates.empty()) {
      const std::size_t innermostDepth =
          std::max_element(
              givenCandidates.begin(), givenCandidates.end(),
              [](const ContextCandidate& lhs, const ContextCandidate& rhs) {
                return lhs.symbol.contextualNestingDepth <
                       rhs.symbol.contextualNestingDepth;
              })
              ->symbol.contextualNestingDepth;
      std::erase_if(givenCandidates, [&](const ContextCandidate& candidate) {
        return candidate.symbol.contextualNestingDepth != innermostDepth;
      });
    }

    std::vector<ContextCandidate> companionCandidates;
    if (parameterCandidates.empty() && givenCandidates.empty()) {
      std::unordered_set<std::string> associatedTypes;
      std::function<void(const TypeInfo&)> collectAssociatedTypes;
      collectAssociatedTypes = [&](const TypeInfo& type) {
        if (!type.typeParameter && !type.abstractTypeMember &&
            type.kind == SimpleTypeKind::Object) {
          const std::string name =
              type.typeConstructorName.empty() ? type.name : type.typeConstructorName;
          if (!name.empty() && name != "Object") {
            associatedTypes.insert(name);
          }
        }
        for (const TypeInfo& argument : type.typeArguments) {
          collectAssociatedTypes(argument);
        }
        for (const TypeInfo& operand : type.compositeTypes) {
          collectAssociatedTypes(operand);
        }
      };
      collectAssociatedTypes(expected);

      std::unordered_set<std::string> seenSymbols;
      for (const std::string& associatedType : associatedTypes) {
        if (companionTypeNames_.contains(associatedType)) {
          auto members = memberScopes_.find(associatedType + '$');
          if (members != memberScopes_.end()) {
            for (const auto& [name, candidate] : members->second) {
              if (!candidate.isGiven ||
                  !seenSymbols
                       .insert(candidate.symbolName + " as " + candidate.type.name)
                       .second) {
                continue;
              }
              addCandidate(companionCandidates, name, candidate);
            }
          }
        }
        if (auto derived = derivedGivens_.find(associatedType);
            derived != derivedGivens_.end()) {
          for (const SymbolInfo& candidate : derived->second) {
            if (!seenSymbols.insert(candidate.symbolName + " as " + candidate.type.name)
                     .second) {
              continue;
            }
            addCandidate(companionCandidates, candidate.name, candidate);
          }
        }
      }
    }

    std::vector<ContextCandidate>& candidates =
        !parameterCandidates.empty()
            ? parameterCandidates
            : (!givenCandidates.empty() ? givenCandidates : companionCandidates);
    sortCandidates(candidates);
    const auto retainUndominated = [&](std::vector<ContextCandidate>& ranked,
                                       const auto& dominates) {
      if (ranked.size() < 2) {
        return;
      }
      std::vector<bool> dominated(ranked.size(), false);
      for (std::size_t candidateIndex = 0; candidateIndex < ranked.size();
           ++candidateIndex) {
        for (std::size_t alternativeIndex = 0; alternativeIndex < ranked.size();
             ++alternativeIndex) {
          if (candidateIndex != alternativeIndex &&
              dominates(ranked[alternativeIndex], ranked[candidateIndex])) {
            dominated[candidateIndex] = true;
            break;
          }
        }
      }
      std::vector<ContextCandidate> preferred;
      preferred.reserve(ranked.size());
      for (std::size_t candidateIndex = 0; candidateIndex < ranked.size();
           ++candidateIndex) {
        if (!dominated[candidateIndex]) {
          preferred.push_back(std::move(ranked[candidateIndex]));
        }
      }
      ranked = std::move(preferred);
    };
    const auto companionClassOfObject = [&](const std::string& owner) {
      auto symbol = globalSymbols_.find(owner);
      if (symbol == globalSymbols_.end() ||
          symbol->second.kind != AstDeclarationKind::Object || !owner.ends_with('$')) {
        return std::string{};
      }
      const std::string companionClass = owner.substr(0, owner.size() - 1);
      auto companion = globalSymbols_.find(companionClass);
      return companion != globalSymbols_.end() &&
                     (companion->second.kind == AstDeclarationKind::Class ||
                      companion->second.kind == AstDeclarationKind::Trait)
                 ? companionClass
                 : std::string{};
    };
    const auto inheritsGivens = [&](const std::string& owner) {
      auto members = memberScopes_.find(owner);
      if (members == memberScopes_.end()) {
        return false;
      }
      return std::any_of(
          members->second.begin(), members->second.end(), [&](const auto& entry) {
            const SymbolInfo& member = entry.second;
            return member.isGiven && ownerNameOf(member.symbolName) != owner;
          });
    };
    const auto hasMoreSpecificOwner = [&](const ContextCandidate& lhs,
                                          const ContextCandidate& rhs) {
      const std::string lhsOwner = ownerNameOf(lhs.symbol.symbolName);
      const std::string rhsOwner = ownerNameOf(rhs.symbol.symbolName);
      if (lhsOwner.empty() || rhsOwner.empty() || lhsOwner == rhsOwner) {
        return false;
      }
      if (isSubtypeOf(lhsOwner, rhsOwner)) {
        return true;
      }
      const std::string lhsCompanion = companionClassOfObject(lhsOwner);
      if (!lhsCompanion.empty() && isSubtypeOf(lhsCompanion, rhsOwner)) {
        return true;
      }
      const std::string rhsCompanion = companionClassOfObject(rhsOwner);
      return !lhsCompanion.empty() && !rhsCompanion.empty() &&
             !inheritsGivens(rhsOwner) && isSubtypeOf(lhsCompanion, rhsCompanion);
    };

    const auto contextualParameterTypes = [](const SymbolInfo& symbol) {
      std::vector<const TypeInfo*> types;
      for (std::size_t parameterIndex = 0;
           parameterIndex < symbol.parameterTypes.size(); ++parameterIndex) {
        if (parameterIndex < symbol.contextualParameters.size() &&
            symbol.contextualParameters[parameterIndex]) {
          types.push_back(&symbol.parameterTypes[parameterIndex]);
        }
      }
      return types;
    };
    const auto rankCandidates = [&](std::vector<ContextCandidate>& ranked) {
      // Owner inheritance is the explicit Scala priority mechanism and precedes
      // the Scala 3.7+ preference for a uniquely most-general result type.
      retainUndominated(ranked, hasMoreSpecificOwner);
      retainUndominated(ranked,
                        [&](const ContextCandidate& lhs, const ContextCandidate& rhs) {
                          return isAssignable(lhs.symbol.type, rhs.symbol.type) &&
                                 !isAssignable(rhs.symbol.type, lhs.symbol.type);
                        });
      retainUndominated(
          ranked, [&](const ContextCandidate& lhs, const ContextCandidate& rhs) {
            const std::vector<const TypeInfo*> lhsParameters =
                contextualParameterTypes(lhs.symbol);
            const std::vector<const TypeInfo*> rhsParameters =
                contextualParameterTypes(rhs.symbol);
            if (lhsParameters.empty() != rhsParameters.empty()) {
              return lhsParameters.empty();
            }
            if (lhsParameters.empty() || lhsParameters.size() != rhsParameters.size()) {
              return false;
            }
            bool strictlyMoreSpecific = false;
            for (std::size_t parameterIndex = 0; parameterIndex < lhsParameters.size();
                 ++parameterIndex) {
              const TypeInfo& lhsParameter = *lhsParameters[parameterIndex];
              const TypeInfo& rhsParameter = *rhsParameters[parameterIndex];
              if (!isAssignable(rhsParameter, lhsParameter)) {
                return false;
              }
              strictlyMoreSpecific =
                  strictlyMoreSpecific || !isAssignable(lhsParameter, rhsParameter);
            }
            return strictlyMoreSpecific;
          });
    };
    std::function<bool(const TypedContextArgument&)> isMaterialized;
    isMaterialized = [&](const TypedContextArgument& argument) {
      return argument.type.kind != SimpleTypeKind::Unknown &&
             std::all_of(argument.arguments.begin(), argument.arguments.end(),
                         isMaterialized);
    };
    const auto materializeCandidate = [&](const ContextCandidate& candidate,
                                          bool emitDiagnostics,
                                          ContextResolutionFailure* candidateFailure)
        -> std::optional<TypedContextArgument> {
      const SymbolInfo& selected = candidate.symbol;
      TypedContextArgument argument;
      argument.name = candidate.referenceName;
      argument.symbolName = selected.symbolName;
      argument.type = selected.type;
      argument.parameterIndex = i;
      argument.requiresAccessor = selected.isModuleMember;
      argument.isCall = selected.kind == AstDeclarationKind::Def;
      argument.prerequisiteArgumentCount = selected.contextPrerequisiteCount;
      if (!argument.isCall) {
        return argument;
      }

      bool materializableParameters =
          selected.captureParameterCount <= selected.parameterTypes.size() &&
          selected.contextualParameters.size() == selected.parameterTypes.size();
      for (std::size_t index = 0;
           materializableParameters && index < selected.parameterTypes.size();
           ++index) {
        materializableParameters = selected.contextualParameters[index] ==
                                   (index >= selected.captureParameterCount);
      }
      if (!materializableParameters) {
        if (candidateFailure != nullptr) {
          *candidateFailure = ContextResolutionFailure::Unsupported;
        }
        if (emitDiagnostics) {
          diagnostics_.error(span, "given method " + selected.name +
                                       " cannot be materialized because it has "
                                       "ordinary parameters");
        }
        return std::nullopt;
      }
      for (std::size_t index = 0;
           index < selected.captureParameterCount && index < selected.parameters.size();
           ++index) {
        argument.captureArgumentNames.push_back(
            parameterName(selected.parameters[index]));
      }

      const std::string expansionKey =
          selected.symbolName + " as " + selected.type.name;
      if (!resolving->insert(expansionKey).second) {
        if (candidateFailure != nullptr) {
          *candidateFailure = ContextResolutionFailure::Diverging;
        }
        if (emitDiagnostics) {
          diagnostics_.error(span, "diverging given expansion for type " +
                                       selected.type.name + " via " + selected.name);
        }
        return std::nullopt;
      }
      argument.arguments = resolveContextArguments(selected, 0, scope, span, resolving,
                                                   emitDiagnostics, candidateFailure);
      resolving->erase(expansionKey);
      return isMaterialized(argument)
                 ? std::optional<TypedContextArgument>{std::move(argument)}
                 : std::nullopt;
    };

    for (ContextCandidate& candidate : candidates) {
      candidate.materializedArgument =
          materializeCandidate(candidate, false, &candidate.failure);
    }
    std::vector<ContextCandidate> allCandidates = candidates;
    std::erase_if(candidates, [](const ContextCandidate& candidate) {
      return !candidate.materializedArgument.has_value();
    });
    rankCandidates(candidates);

    const std::string parameterNameText =
        i < callee.parameters.size() ? parameterName(callee.parameters[i])
                                     : std::to_string(i - firstContextParameter);
    if (candidates.empty()) {
      if (allCandidates.empty()) {
        mergeFailure(ContextResolutionFailure::Missing);
      } else {
        rankCandidates(allCandidates);
        mergeFailure(allCandidates.front().failure == ContextResolutionFailure::None
                         ? ContextResolutionFailure::Unsupported
                         : allCandidates.front().failure);
      }
      if (reportDiagnostics) {
        if (allCandidates.empty()) {
          diagnostics_.error(span, "no given value found for context parameter " +
                                       parameterNameText + " of type " + expected.name +
                                       " required by " + callee.name);
        } else {
          (void)materializeCandidate(allCandidates.front(), true, nullptr);
        }
      }
      result.push_back(unknownArgument(i));
      continue;
    }
    if (candidates.size() > 1) {
      mergeFailure(ContextResolutionFailure::Ambiguous);
      if (reportDiagnostics) {
        std::string message = "ambiguous given values for context parameter " +
                              parameterNameText + " of type " + expected.name +
                              " required by " + callee.name + ": ";
        for (std::size_t candidateIndex = 0; candidateIndex < candidates.size();
             ++candidateIndex) {
          if (candidateIndex != 0) {
            message += ", ";
          }
          const SymbolInfo& candidate = candidates[candidateIndex].symbol;
          message += candidate.isAnonymousGiven ? candidate.type.name : candidate.name;
        }
        diagnostics_.error(span, std::move(message));
      }
      result.push_back(unknownArgument(i));
      continue;
    }
    result.push_back(std::move(*candidates.front().materializedArgument));
  }
  return result;
}

void Typechecker::recordContextApplication(const support::SourceSpan& span,
                                           std::vector<TypedContextArgument> arguments,
                                           bool hasSelectedBranch,
                                           std::size_t selectedBranch) {
  auto sameSpan = [&](const TypedContextApplication& application) {
    return application.span.source == span.source &&
           application.span.start == span.start &&
           application.span.length == span.length;
  };
  auto existing =
      std::find_if(contextApplications_.begin(), contextApplications_.end(), sameSpan);
  if (existing == contextApplications_.end()) {
    contextApplications_.push_back(TypedContextApplication{
        span, std::move(arguments), hasSelectedBranch, selectedBranch});
  } else {
    existing->arguments = std::move(arguments);
    existing->hasSelectedBranch = hasSelectedBranch;
    existing->selectedBranch = selectedBranch;
  }
}

bool Typechecker::isSubtypeOf(const std::string& actual,
                              const std::string& expected) const {
  if (actual == expected) {
    return true;
  }

  std::vector<std::string> worklist{actual};
  std::unordered_set<std::string> visited;
  while (!worklist.empty()) {
    std::string current = std::move(worklist.back());
    worklist.pop_back();
    if (!visited.insert(current).second) {
      continue;
    }
    auto found = globalSymbols_.find(current);
    if (found == globalSymbols_.end()) {
      continue;
    }
    for (const std::string& parent : found->second.parentSymbolNames) {
      if (parent == expected) {
        return true;
      }
      worklist.push_back(parent);
    }
  }
  return false;
}

void Typechecker::addParametersToScope(const AstDeclaration& declaration,
                                       Scope& scope) const {
  for (std::size_t i = 0; i < declaration.parameters.size(); ++i) {
    const std::string& parameter = declaration.parameters[i];
    std::string name = parameterName(parameter);
    if (name.empty()) {
      continue;
    }
    SymbolInfo symbol;
    symbol.kind = parameterDeclarationKind(parameter);
    symbol.name = name;
    symbol.symbolName = qualify(declaration.name, name);
    symbol.type = parameterType(parameter, &scope, &declaration.span);
    symbol.isContextParameter = i < declaration.contextualParameters.size() &&
                                declaration.contextualParameters[i];
    symbol.isInlineParameter = i < declaration.inlineParameters.size() &&
                               declaration.inlineParameters[i];
    symbol.isLexicalValue = true;
    scope[name] = std::move(symbol);
  }
}

std::vector<std::string> Typechecker::resolvedParameters(
    const std::vector<std::string>& parameters, const Scope& scope,
    std::vector<TypeInfo>* parameterTypes, const support::SourceSpan* span) const {
  std::vector<std::string> resolved;
  resolved.reserve(parameters.size());
  if (parameterTypes != nullptr) {
    parameterTypes->clear();
    parameterTypes->reserve(parameters.size());
  }
  Scope parameterScope = scope;
  for (const std::string& parameter : parameters) {
    std::string name = parameterName(parameter);
    if (name.empty()) {
      resolved.push_back(parameter);
      if (parameterTypes != nullptr) {
        parameterTypes->push_back(TypeInfo{SimpleTypeKind::Unknown, "Unknown"});
      }
      continue;
    }
    const std::size_t colon = parameter.find(':');
    if (colon == std::string::npos) {
      resolved.push_back(std::move(name));
      if (parameterTypes != nullptr) {
        parameterTypes->push_back(TypeInfo{SimpleTypeKind::Unknown, "Unknown"});
      }
      continue;
    }
    TypeInfo type = parameterType(parameter, &parameterScope, span);
    if (parameterTypes != nullptr) {
      parameterTypes->push_back(type);
    }
    if (parameterDeclarationKind(parameter) == AstDeclarationKind::Var) {
      resolved.push_back("var " + name + ": " + type.name);
    } else if (parameter.rfind("val ", 0) == 0) {
      resolved.push_back("val " + name + ": " + type.name);
    } else {
      resolved.push_back(name + ": " + type.name);
    }
    SymbolInfo symbol;
    symbol.kind = parameterDeclarationKind(parameter);
    symbol.name = name;
    symbol.symbolName = name;
    symbol.type = type;
    parameterScope[name] = std::move(symbol);
  }
  return resolved;
}

std::string Typechecker::parameterName(const std::string& parameter) {
  const std::size_t colon = parameter.find(':');
  std::string name =
      colon == std::string::npos ? parameter : parameter.substr(0, colon);
  while (!name.empty() && name.back() == ' ') {
    name.pop_back();
  }
  if (name.rfind("val ", 0) == 0) {
    name.erase(0, 4);
  } else if (name.rfind("var ", 0) == 0) {
    name.erase(0, 4);
  }
  while (!name.empty() && name.front() == ' ') {
    name.erase(name.begin());
  }
  return name;
}

AstDeclarationKind Typechecker::parameterDeclarationKind(const std::string& parameter) {
  if (parameter.rfind("var ", 0) == 0) {
    return AstDeclarationKind::Var;
  }
  return AstDeclarationKind::Val;
}

TypeInfo Typechecker::parameterType(const std::string& parameter, const Scope* scope,
                                    const support::SourceSpan* span) const {
  const std::size_t colon = parameter.find(':');
  if (colon == std::string::npos) {
    return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
  }
  std::string type = parameter.substr(colon + 1);
  while (!type.empty() && type.front() == ' ') {
    type.erase(type.begin());
  }
  return typeFromDeclaredName(type, scope, span);
}

std::string Typechecker::qualify(const std::string& owner, const std::string& name) {
  if (owner.empty() || name.empty()) {
    return name;
  }
  return owner + "." + name;
}

const char* simpleTypeKindName(SimpleTypeKind kind) {
  switch (kind) {
  case SimpleTypeKind::Unknown:
    return "Unknown";
  case SimpleTypeKind::Nothing:
    return "Nothing";
  case SimpleTypeKind::Unit:
    return "Unit";
  case SimpleTypeKind::Byte:
    return "Byte";
  case SimpleTypeKind::Short:
    return "Short";
  case SimpleTypeKind::Int:
    return "Int";
  case SimpleTypeKind::Long:
    return "Long";
  case SimpleTypeKind::Float:
    return "Float";
  case SimpleTypeKind::Double:
    return "Double";
  case SimpleTypeKind::Boolean:
    return "Boolean";
  case SimpleTypeKind::String:
    return "String";
  case SimpleTypeKind::Char:
    return "Char";
  case SimpleTypeKind::Symbol:
    return "Symbol";
  case SimpleTypeKind::Null:
    return "Null";
  case SimpleTypeKind::Object:
    return "Object";
  }
  return "Unknown";
}

} // namespace scalanative::frontend
