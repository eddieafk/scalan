#include "scalanative/frontend/Typechecker.h"

#include "scalanative/support/StdNames.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <optional>
#include <string_view>
#include <unordered_set>

namespace scalanative::frontend {

namespace {

bool isClassLikeDeclaration(AstDeclarationKind kind) {
  return kind == AstDeclarationKind::Object || kind == AstDeclarationKind::Class ||
         kind == AstDeclarationKind::Trait;
}

bool isInheritableDeclaration(AstDeclarationKind kind) {
  return kind == AstDeclarationKind::Class || kind == AstDeclarationKind::Trait;
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
      type.typeConstructorName.empty() && !type.runtimeName.empty() &&
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
         operation == support::StdNames::ByteBufferClear ||
         operation == support::StdNames::ByteBufferFlip ||
         operation == support::StdNames::ByteBufferRewind;
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
          std::move(bufferOverflow),      std::move(stackTraceElement)};
}

} // namespace

Typechecker::Typechecker(support::DiagnosticEngine& diagnostics)
    : diagnostics_(diagnostics) {}

TypedModule Typechecker::typecheck(const AstModule& module) {
  declaredMemberScopes_.clear();
  memberScopes_.clear();
  globalSymbols_.clear();
  companionTypeNames_.clear();
  expressionTypes_.clear();
  contextApplications_.clear();
  directZoneReceiverEscapes_.clear();
  receiverMethodCallSites_.clear();
  implicitReceiverMethodNames_.clear();
  zoneBodiesToAnalyze_.clear();
  zoneInferenceDepth_ = 0;
  TypedModule typed;
  typed.packageName = module.packageName;
  Scope scope;
  addRuntimeBuiltins(scope);
  typed.declarations = standardExceptionDeclarations();
  std::unordered_map<std::string, unsigned> companionKinds;
  for (const AstDeclaration& declaration : module.declarations) {
    if (declaration.name.empty()) {
      continue;
    }
    const std::string name = qualify(module.packageName, declaration.name);
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
  for (const AstDeclaration& declaration : module.declarations) {
    typed.declarations.push_back(
        typecheckDeclaration(declaration, module.packageName, scope));
  }
  propagateZoneReceiverEffects();
  for (const AstExpression& body : zoneBodiesToAnalyze_) {
    std::unordered_map<std::string, bool> arenaReferences;
    std::unordered_set<std::string> zoneLocals;
    (void)analyzeZoneExpression(body, arenaReferences, zoneLocals);
  }
  typed.expressionTypes = expressionTypes_;
  typed.contextApplications = contextApplications_;
  return typed;
}

TypedDeclaration Typechecker::typecheckDeclaration(const AstDeclaration& declaration,
                                                   const std::string& owner,
                                                   Scope& scope) {
  if (declaration.name.empty() && declaration.kind != AstDeclarationKind::Import) {
    diagnostics_.error(declaration.span, "declaration has no name");
  }

  TypedDeclaration typed;
  typed.kind = declaration.kind;
  typed.name = declaration.name;
  typed.symbolName = declaration.kind == AstDeclarationKind::Import
                         ? importSymbolName(declaration, scope)
                         : declarationSymbolName(declaration, owner);
  typed.span = declaration.span;
  typed.importPath = declaration.importPath;
  typed.importSelectors = declaration.importSelectors;
  typed.parentArguments = declaration.parentArguments;
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
      inferred = inferExpressionType(
          declaration.initializer, expressionScope,
          declared.kind == SimpleTypeKind::Unknown ? nullptr : &declared);
    } else {
      inferred = TypeInfo{SimpleTypeKind::Unit, "Unit"};
    }
    break;
  }

  const bool declaredIsValueType = declaration.kind == AstDeclarationKind::Def ||
                                   declaration.kind == AstDeclarationKind::Val ||
                                   declaration.kind == AstDeclarationKind::Var;
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
  if (declaration.kind == AstDeclarationKind::Import && declaration.name == "_") {
    const std::string& importOwner = typed.symbolName;
    for (const auto& [symbolName, symbol] : globalSymbols_) {
      if (!isDirectMemberOf(symbolName, importOwner)) {
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
    symbol.isGiven = typed.isGiven;
    symbol.isAnonymousGiven = typed.isAnonymousGiven;
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
    for (const std::string& parameter : typed.parameters) {
      const std::string name = parameterName(parameter);
      if (name.empty()) {
        continue;
      }
      SymbolInfo field;
      field.kind = parameterDeclarationKind(parameter);
      field.name = name;
      field.symbolName = qualify(typed.symbolName, name);
      field.type = parameterType(parameter, &signatureScope);
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
    updated.isGiven = typedMember.isGiven;
    updated.isAnonymousGiven = typedMember.isAnonymousGiven;
    updated.isModuleMember = declaration.kind == AstDeclarationKind::Object &&
                             (typedMember.kind == AstDeclarationKind::Val ||
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
    memberScope[member.name] = updated;
    ownMemberScope[member.name] = std::move(updated);
    globalSymbols_[typedMember.symbolName] = memberScope[member.name];
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

  if (!declaration.importSelectors.empty() || declaration.name == "_") {
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
  symbol.isGiven = declaration.isGiven;
  symbol.isAnonymousGiven = declaration.isAnonymousGiven;
  if (auto enclosing = globalSymbols_.find(owner);
      enclosing != globalSymbols_.end() &&
      enclosing->second.kind == AstDeclarationKind::Object) {
    symbol.isModuleMember = declaration.kind == AstDeclarationKind::Val ||
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
      for (const std::string& parameter : declaration.parameters) {
        const std::string name = parameterName(parameter);
        if (name.empty()) {
          continue;
        }
        SymbolInfo field;
        field.kind = parameterDeclarationKind(parameter);
        field.name = name;
        field.symbolName = qualify(symbolName, name);
        field.type = parameterType(parameter, &declarationScope);
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

void Typechecker::applyImport(const AstDeclaration& declaration, Scope& scope) {
  if (declaration.kind != AstDeclarationKind::Import) {
    return;
  }
  if (declaration.importPath.empty()) {
    return;
  }

  if (!declaration.importSelectors.empty()) {
    const std::string importOwner = importSymbolName(declaration, scope);
    if (!globalSymbols_.contains(importOwner)) {
      diagnostics_.error(declaration.span,
                         "unresolved import owner: " + declaration.importPath);
      return;
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
    return;
  }

  if (declaration.name.empty()) {
    return;
  }

  if (declaration.name == "_") {
    const std::string owner = importSymbolName(declaration, scope);
    if (owner.empty() || !globalSymbols_.contains(owner)) {
      diagnostics_.error(declaration.span,
                         "unresolved wildcard import: " + declaration.importPath);
      return;
    }
    for (const auto& [symbolName, symbol] : globalSymbols_) {
      if (!isDirectMemberOf(symbolName, owner)) {
        continue;
      }
      SymbolInfo alias = symbol;
      alias.name = memberNameOf(symbolName);
      scope[alias.name] = std::move(alias);
    }
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

TypeInfo Typechecker::inferExpressionTypeImpl(const AstExpression& expression,
                                              Scope& scope,
                                              const TypeInfo* expectedType) {
  switch (expression.kind) {
  case AstExpressionKind::Empty:
    return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
  case AstExpressionKind::IntegerLiteral:
    if (!expression.text.empty() &&
        (expression.text.back() == 'l' || expression.text.back() == 'L')) {
      return TypeInfo{SimpleTypeKind::Long, "Long"};
    }
    return TypeInfo{SimpleTypeKind::Int, "Int"};
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
        if (const SymbolInfo* member =
                knownMemberForReceiverType(receiver, callee.text)) {
          target = specializeMemberForReceiver(*member, receiver);
          foundTarget = true;
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
      return specializeTypeApplication(target, typeArguments, scope, expression.span)
          .type;
    }

    if (typeArguments.size() != 1) {
      diagnostics_.error(expression.span,
                         std::string(isTypeTest ? "isInstanceOf" : "asInstanceOf") +
                             " requires exactly one type argument");
      return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
    }

    const TypeInfo receiverType = inferExpressionType(callee.children.front(), scope);
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
    const SymbolInfo* target =
        targetsBoxedPrimitive
            ? nullptr
            : typeSymbolForDeclaredName(expression.declaredType, &scope);
    if (!targetsBoxedPrimitive &&
        (target == nullptr || (target->kind != AstDeclarationKind::Class &&
                               target->kind != AstDeclarationKind::Trait))) {
      diagnostics_.error(
          expression.span,
          std::string(isTypeTest ? "isInstanceOf" : "asInstanceOf") +
              " target must be a known class or trait: " + expression.declaredType);
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
          const bool put = selected.text == support::StdNames::ByteBufferPut;
          if ((positionOrLimit && argumentCount > 1) || (put && argumentCount != 1) ||
              (!positionOrLimit && !put && argumentCount != 0)) {
            const std::string argumentContract =
                positionOrLimit ? " accepts zero or one Int argument"
                                : (put ? " requires exactly one Byte argument"
                                       : " does not accept arguments");
            diagnostics_.error(expression.span, selected.text + argumentContract);
            for (std::size_t i = 1; i < expression.children.size(); ++i) {
              (void)inferExpressionType(expression.children[i], scope);
            }
          } else if (argumentCount == 1) {
            const TypeInfo value = inferExpressionType(expression.children[1], scope);
            const SimpleTypeKind expectedKind =
                put ? SimpleTypeKind::Byte : SimpleTypeKind::Int;
            if (value.kind != expectedKind && value.kind != SimpleTypeKind::Unknown) {
              diagnostics_.error(expression.children[1].span,
                                 selected.text + " value must have type " +
                                     (put ? "Byte" : "Int"));
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
      auto found = scope.find(constructor->text);
      if (found != scope.end() && found->second.kind == AstDeclarationKind::Class) {
        classSymbol = &found->second;
      } else if (auto global = globalSymbols_.find(constructor->text);
                 global != globalSymbols_.end() &&
                 global->second.kind == AstDeclarationKind::Class) {
        classSymbol = &global->second;
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
        if (argumentTypes.size() != classSymbol->parameterTypes.size()) {
          diagnostics_.error(expression.span,
                             "constructor for " + classSymbol->name + " has " +
                                 std::to_string(argumentTypes.size()) +
                                 " arguments but expected " +
                                 std::to_string(classSymbol->parameterTypes.size()));
        }
        const std::size_t checkedArguments =
            std::min(argumentTypes.size(), classSymbol->parameterTypes.size());
        for (std::size_t i = 0; i < checkedArguments; ++i) {
          const bool targetsAny = isAnyArrayElementType(classSymbol->parameterTypes[i]);
          const bool argumentConforms =
              targetsAny
                  ? isSupportedAnyArrayValueType(argumentTypes[i])
                  : isAssignable(classSymbol->parameterTypes[i], argumentTypes[i]);
          if (!argumentConforms) {
            diagnostics_.error(expression.children[i + 1].span,
                               "constructor argument " + std::to_string(i) +
                                   " of type " + argumentTypes[i].name +
                                   " does not conform to field type " +
                                   classSymbol->parameterTypes[i].name);
          }
        }
      }
      return constructed;
    }

    const SymbolInfo* calleeSymbol = nullptr;
    SymbolInfo specializedCallee;
    if (callee.kind == AstExpressionKind::Identifier) {
      auto found = scope.find(callee.text);
      if (found != scope.end()) {
        calleeSymbol = &found->second;
      }
    } else if (callee.kind == AstExpressionKind::Select &&
               callee.children.size() == 1) {
      const TypeInfo receiver = inferExpressionType(callee.children.front(), scope);
      if (const SymbolInfo* member =
              knownMemberForReceiverType(receiver, callee.text)) {
        specializedCallee = specializeMemberForReceiver(*member, receiver);
        calleeSymbol = &specializedCallee;
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
        }
      } else if (genericTarget.kind == AstExpressionKind::Select &&
                 genericTarget.children.size() == 1) {
        const TypeInfo receiver =
            inferExpressionType(genericTarget.children.front(), scope);
        if (const SymbolInfo* member =
                knownMemberForReceiverType(receiver, genericTarget.text)) {
          receiverSpecialized = specializeMemberForReceiver(*member, receiver);
          rawTarget = &receiverSpecialized;
        }
      }
      if (rawTarget != nullptr) {
        specializedCallee = specializeTypeApplication(
            *rawTarget, typeArgumentsFor(callee), scope, callee.span, false);
        calleeSymbol = &specializedCallee;
      }
    }

    std::vector<TypeInfo> argumentTypes;
    for (std::size_t i = 1; i < expression.children.size(); ++i) {
      argumentTypes.push_back(inferExpressionType(expression.children[i], scope));
    }
    TypeInfo calleeType{SimpleTypeKind::Unknown, "Unknown"};
    if (calleeSymbol != nullptr && callee.kind != AstExpressionKind::TypeApply &&
        !calleeSymbol->typeParameters.empty()) {
      const SymbolInfo inferenceTarget = *calleeSymbol;
      specializedCallee = inferTypeApplication(inferenceTarget, argumentTypes,
                                               expression.span, expectedType);
      calleeSymbol = &specializedCallee;
      calleeType = specializedCallee.type;
    } else {
      calleeType = inferExpressionType(callee, scope);
    }

    if (calleeSymbol != nullptr && calleeSymbol->typeParameters.empty()) {
      std::size_t firstContextParameter = calleeSymbol->parameterTypes.size();
      for (std::size_t i = 0; i < calleeSymbol->contextualParameters.size(); ++i) {
        if (calleeSymbol->contextualParameters[i]) {
          firstContextParameter = i;
          break;
        }
      }
      if (firstContextParameter < calleeSymbol->parameterTypes.size() &&
          argumentTypes.size() == firstContextParameter) {
        std::vector<TypedContextArgument> contextArguments = resolveContextArguments(
            *calleeSymbol, firstContextParameter, scope, expression.span);
        for (const TypedContextArgument& argument : contextArguments) {
          argumentTypes.push_back(argument.type);
        }
        recordContextApplication(expression.span, std::move(contextArguments));
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
      for (std::size_t i = 0; i < checkedArguments; ++i) {
        const bool targetsAny = isAnyArrayElementType(calleeSymbol->parameterTypes[i]);
        const bool argumentConforms =
            targetsAny
                ? isSupportedAnyArrayValueType(argumentTypes[i])
                : isAssignable(calleeSymbol->parameterTypes[i], argumentTypes[i]);
        if (!argumentConforms) {
          diagnostics_.error(expression.children[i + 1].span,
                             "argument " + std::to_string(i) + " of type " +
                                 argumentTypes[i].name +
                                 " does not conform to parameter type " +
                                 calleeSymbol->parameterTypes[i].name);
        }
      }
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
          return initializerType;
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
      for (std::size_t i = 0; i + 1 < expression.children.size(); ++i) {
        const AstExpression& child = expression.children[i];
        if (child.kind == AstExpressionKind::LocalDeclaration) {
          TypeInfo localType = localDeclarationType(child);
          SymbolInfo symbol;
          symbol.kind =
              child.mutableLocal ? AstDeclarationKind::Var : AstDeclarationKind::Val;
          symbol.name = child.text;
          symbol.symbolName = child.text;
          symbol.type = localType;
          symbol.isGiven = child.isGiven;
          symbol.isAnonymousGiven = child.isAnonymousGiven;
          symbol.contextualNestingDepth = contextualNestingDepth;
          blockScope[child.text] = std::move(symbol);
        } else {
          (void)inferExpressionType(child, blockScope);
        }
      }
      const AstExpression& last = expression.children.back();
      if (last.kind == AstExpressionKind::LocalDeclaration) {
        SymbolInfo symbol;
        symbol.kind =
            last.mutableLocal ? AstDeclarationKind::Var : AstDeclarationKind::Val;
        symbol.name = last.text;
        symbol.symbolName = last.text;
        symbol.type = localDeclarationType(last);
        symbol.isGiven = last.isGiven;
        symbol.isAnonymousGiven = last.isAnonymousGiven;
        symbol.contextualNestingDepth = contextualNestingDepth;
        blockScope[last.text] = std::move(symbol);
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
  case AstExpressionKind::If:
  case AstExpressionKind::While:
  case AstExpressionKind::Unary:
  case AstExpressionKind::Binary: {
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
            selected.text == support::StdNames::ByteBufferClear ||
            selected.text == support::StdNames::ByteBufferFlip ||
            selected.text == support::StdNames::ByteBufferRewind ||
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
  case AstExpressionKind::Binary:
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

  auto found = scope.find(expression.text);
  if (found != scope.end() && found->second.kind == AstDeclarationKind::Class) {
    if (!found->second.typeParameters.empty()) {
      diagnostics_.error(expression.span,
                         "generic class " + found->second.name + " requires " +
                             std::to_string(found->second.typeParameters.size()) +
                             " explicit type arguments");
    }
    return found->second.type;
  }

  if (expression.text.find('.') != std::string::npos) {
    return TypeInfo{SimpleTypeKind::Object, expression.text};
  }

  diagnostics_.error(expression.span, "unresolved class: " + expression.text);
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

  const SymbolInfo* member = selectedMember(expression, scope);
  if (member == nullptr) {
    return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
  }
  SymbolInfo specializedMember = *member;
  if (expression.children.size() == 1) {
    const TypeInfo receiver = inferExpressionType(expression.children.front(), scope);
    specializedMember = specializeMemberForReceiver(*member, receiver);
  }
  if (!specializedMember.typeParameters.empty()) {
    diagnostics_.error(expression.span,
                       "generic method " + specializedMember.name + " requires " +
                           std::to_string(specializedMember.typeParameters.size()) +
                           " explicit type arguments");
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

  std::string memberOwner = receiver.typeConstructorName.empty()
                                ? receiver.name
                                : receiver.typeConstructorName;
  auto members = memberScopes_.find(memberOwner);
  if (members == memberScopes_.end() && !receiver.runtimeName.empty() &&
      receiver.runtimeName != memberOwner) {
    memberOwner = receiver.runtimeName;
    members = memberScopes_.find(memberOwner);
  }
  if (members == memberScopes_.end()) {
    diagnostics_.error(expression.span,
                       "no known members for receiver: " + receiver.name);
    return nullptr;
  }

  auto member = members->second.find(expression.text);
  if (member == members->second.end()) {
    diagnostics_.error(expression.span,
                       "unresolved member: " + expression.text + " on " + memberOwner);
    return nullptr;
  }

  return &member->second;
}

const SymbolInfo*
Typechecker::knownMemberForReceiverType(const TypeInfo& receiver,
                                        const std::string& memberName) const {
  if (receiver.kind != SimpleTypeKind::Object) {
    return nullptr;
  }

  std::string memberOwner = receiver.typeConstructorName.empty()
                                ? receiver.name
                                : receiver.typeConstructorName;
  auto members = memberScopes_.find(memberOwner);
  if (members == memberScopes_.end() && !receiver.runtimeName.empty() &&
      receiver.runtimeName != memberOwner) {
    memberOwner = receiver.runtimeName;
    members = memberScopes_.find(memberOwner);
  }
  if (members == memberScopes_.end()) {
    return nullptr;
  }

  auto member = members->second.find(memberName);
  if (member == members->second.end()) {
    return nullptr;
  }
  return &member->second;
}

const SymbolInfo* Typechecker::typeSymbolForDeclaredName(const std::string& name,
                                                         const Scope* scope) const {
  const AppliedTypeSyntax applied = parseAppliedTypeSyntax(name);
  const std::string normalized = applied.applied && !applied.constructor.empty()
                                     ? applied.constructor
                                     : trim(name);
  if (normalized.empty()) {
    return nullptr;
  }
  if (scope != nullptr && normalized.find('.') == std::string::npos) {
    auto found = scope->find(normalized);
    if (found != scope->end()) {
      return &found->second;
    }
  }
  auto global = globalSymbols_.find(normalized);
  if (global != globalSymbols_.end()) {
    return &global->second;
  }
  return nullptr;
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
    if (!typesMatchForOverride(inherited.type, overriding.inferredType)) {
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

  if (!typesMatchForOverride(inherited.type, overriding.inferredType)) {
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

  if (type.typeArguments.empty()) {
    return type;
  }
  TypeInfo substituted = type;
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
    if (argument.kind != SimpleTypeKind::Unknown && !isReferenceType(argument) &&
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

SymbolInfo Typechecker::inferTypeApplication(const SymbolInfo& symbol,
                                             const std::vector<TypeInfo>& argumentTypes,
                                             const support::SourceSpan& span,
                                             const TypeInfo* expectedResultType,
                                             bool reportDiagnostics) const {
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
    if (current.kind == candidate.kind) {
      return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
    }
    return commonType(current, candidate);
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
    }
  };

  const std::size_t checkedArguments =
      std::min(symbol.parameterTypes.size(), argumentTypes.size());
  for (std::size_t i = 0; i < checkedArguments; ++i) {
    collectInference(symbol.parameterTypes[i], argumentTypes[i], false);
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
    inferredArguments.push_back(inferred->second);
  }
  if (!complete || hasConflict) {
    return symbol;
  }
  return specializeResolvedTypeApplication(symbol, inferredArguments, span,
                                           reportDiagnostics);
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
    if (constructor == nullptr || (constructor->kind != AstDeclarationKind::Class &&
                                   constructor->kind != AstDeclarationKind::Trait)) {
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
        auto members = memberScopes_.find(receiverType.name);
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

        const bool isFinal = nextDot == std::string::npos;
        if (isFinal) {
          if (member->second.kind != AstDeclarationKind::Type) {
            if (span != nullptr) {
              diagnostics_.error(*span, "selected path member " + segment + " on " +
                                            receiverType.name +
                                            " is not a type member");
            }
            return TypeInfo{SimpleTypeKind::Unknown, normalized};
          }
          if (member->second.hasImplementation) {
            TypeInfo selected = member->second.type;
            selected.dependentOwnerName = receiverType.name;
            selected.dependentMemberName = segment;
            selected.dependentPathName = normalized;
            selected.resolvedAliasName = member->second.symbolName;
            selected.pathDependent = true;
            return selected;
          }
          TypeInfo selected = member->second.type;
          selected.name = normalized;
          selected.dependentOwnerName = receiverType.name;
          selected.dependentMemberName = segment;
          selected.dependentPathName = normalized;
          selected.abstractTypeMember = true;
          selected.pathDependent = true;
          if (isReferenceType(member->second.upperBound) &&
              !member->second.upperBound.abstractTypeMember) {
            selected.runtimeName = member->second.upperBound.name;
          } else if (member->second.upperBound.kind == SimpleTypeKind::Unknown ||
                     isBoxablePrimitiveType(member->second.upperBound.kind)) {
            selected.runtimeName = "Object";
          }
          return selected;
        }

        if (member->second.kind != AstDeclarationKind::Val &&
            member->second.kind != AstDeclarationKind::Object) {
          if (span != nullptr) {
            diagnostics_.error(*span, "unstable nested path-dependent type prefix: " +
                                          segment);
          }
          return TypeInfo{SimpleTypeKind::Unknown, normalized};
        }
        receiverType = member->second.type;
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
  if (lhs.kind == rhs.kind) {
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
  return TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
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
    if (value.kind == SimpleTypeKind::Null && isReferenceType(target)) {
      return true;
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
    bool reportDiagnostics) const {
  struct ContextCandidate {
    std::string referenceName;
    SymbolInfo symbol;
    std::optional<TypedContextArgument> materializedArgument;
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
  const auto unknownArgument = [] {
    TypedContextArgument argument;
    argument.type = TypeInfo{SimpleTypeKind::Unknown, "Unknown"};
    return argument;
  };

  std::vector<TypedContextArgument> result;
  for (std::size_t i = firstContextParameter; i < callee.parameterTypes.size(); ++i) {
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
      candidates.push_back(
          ContextCandidate{referenceName, std::move(candidate), std::nullopt});
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
      };
      collectAssociatedTypes(expected);

      std::unordered_set<std::string> seenSymbols;
      for (const std::string& associatedType : associatedTypes) {
        if (!companionTypeNames_.contains(associatedType)) {
          continue;
        }
        auto members = memberScopes_.find(associatedType + '$');
        if (members == memberScopes_.end()) {
          continue;
        }
        for (const auto& [name, candidate] : members->second) {
          if (!candidate.isGiven || !seenSymbols.insert(candidate.symbolName).second) {
            continue;
          }
          addCandidate(companionCandidates, name, candidate);
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
        for (std::size_t alternativeIndex = 0;
             alternativeIndex < ranked.size(); ++alternativeIndex) {
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
          symbol->second.kind != AstDeclarationKind::Object ||
          !owner.ends_with('$')) {
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
          members->second.begin(), members->second.end(),
          [&](const auto& entry) {
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
             !inheritsGivens(rhsOwner) &&
             isSubtypeOf(lhsCompanion, rhsCompanion);
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
      retainUndominated(
          ranked, [&](const ContextCandidate& lhs, const ContextCandidate& rhs) {
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
            if (lhsParameters.empty() ||
                lhsParameters.size() != rhsParameters.size()) {
              return false;
            }
            bool strictlyMoreSpecific = false;
            for (std::size_t parameterIndex = 0;
                 parameterIndex < lhsParameters.size(); ++parameterIndex) {
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
                                          bool emitDiagnostics)
        -> std::optional<TypedContextArgument> {
      const SymbolInfo& selected = candidate.symbol;
      TypedContextArgument argument;
      argument.name = candidate.referenceName;
      argument.symbolName = selected.symbolName;
      argument.type = selected.type;
      argument.requiresAccessor = selected.isModuleMember;
      argument.isCall = selected.kind == AstDeclarationKind::Def;
      if (!argument.isCall) {
        return argument;
      }

      const bool allContextual =
          selected.contextualParameters.size() == selected.parameterTypes.size() &&
          std::all_of(selected.contextualParameters.begin(),
                      selected.contextualParameters.end(),
                      [](bool contextual) { return contextual; });
      if (!allContextual) {
        if (emitDiagnostics) {
          diagnostics_.error(span, "given method " + selected.name +
                                       " cannot be materialized because it has "
                                       "ordinary parameters");
        }
        return std::nullopt;
      }

      const std::string expansionKey =
          selected.symbolName + " as " + selected.type.name;
      if (!resolving->insert(expansionKey).second) {
        if (emitDiagnostics) {
          diagnostics_.error(span, "diverging given expansion for type " +
                                       selected.type.name + " via " + selected.name);
        }
        return std::nullopt;
      }
      argument.arguments = resolveContextArguments(selected, 0, scope, span, resolving,
                                                    emitDiagnostics);
      resolving->erase(expansionKey);
      return isMaterialized(argument)
                 ? std::optional<TypedContextArgument>{std::move(argument)}
                 : std::nullopt;
    };

    std::vector<ContextCandidate> allCandidates = candidates;
    for (ContextCandidate& candidate : candidates) {
      candidate.materializedArgument = materializeCandidate(candidate, false);
    }
    std::erase_if(candidates, [](const ContextCandidate& candidate) {
      return !candidate.materializedArgument.has_value();
    });
    rankCandidates(candidates);

    const std::string parameterNameText =
        i < callee.parameters.size() ? parameterName(callee.parameters[i])
                                     : std::to_string(i - firstContextParameter);
    if (candidates.empty()) {
      if (reportDiagnostics) {
        if (allCandidates.empty()) {
          diagnostics_.error(span, "no given value found for context parameter " +
                                       parameterNameText + " of type " + expected.name +
                                       " required by " + callee.name);
        } else {
          rankCandidates(allCandidates);
          (void)materializeCandidate(allCandidates.front(), true);
        }
      }
      result.push_back(unknownArgument());
      continue;
    }
    if (candidates.size() > 1) {
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
      result.push_back(unknownArgument());
      continue;
    }
    result.push_back(std::move(*candidates.front().materializedArgument));
  }
  return result;
}

void Typechecker::recordContextApplication(
    const support::SourceSpan& span, std::vector<TypedContextArgument> arguments) {
  auto sameSpan = [&](const TypedContextApplication& application) {
    return application.span.source == span.source &&
           application.span.start == span.start &&
           application.span.length == span.length;
  };
  auto existing =
      std::find_if(contextApplications_.begin(), contextApplications_.end(), sameSpan);
  if (existing == contextApplications_.end()) {
    contextApplications_.push_back(TypedContextApplication{span, std::move(arguments)});
  } else {
    existing->arguments = std::move(arguments);
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
