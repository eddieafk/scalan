#include "scalanative/nir/Verifier.h"

#include "scalanative/support/StdNames.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scalanative::nir {

namespace {

bool isBoxablePrimitiveType(const std::string& type) {
  return type == "Unit" || type == "Boolean" || type == "Byte" || type == "Short" ||
         type == "Int" || type == "Long" || type == "Float" || type == "Double" ||
         type == "Char" || type == "Symbol" || type == "String";
}

bool isFunction(DefinitionKind kind) {
  return kind == DefinitionKind::FunctionDecl || kind == DefinitionKind::FunctionDef;
}

bool isClassLikeDefinition(DefinitionKind kind) {
  return kind == DefinitionKind::Module || kind == DefinitionKind::Class ||
         kind == DefinitionKind::Trait;
}

bool isInheritableDefinition(DefinitionKind kind) {
  return kind == DefinitionKind::Class || kind == DefinitionKind::Trait;
}

bool isValidFunctionSignature(const std::string& signature) {
  if (signature.empty() || signature.front() != '(') {
    return false;
  }
  const std::size_t close = signature.find(')');
  return close != std::string::npos && close > 0 && close + 1 < signature.size();
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

std::string signatureReturnType(const std::string& signature) {
  const std::size_t close = signature.find(')');
  if (close == std::string::npos || close + 1 >= signature.size()) {
    return "Unknown";
  }
  return trim(std::string_view(signature).substr(close + 1));
}

bool isReferenceType(const std::string& type) {
  return type != "Unit" && type != "Boolean" && type != "Byte" && type != "Short" &&
         type != "Int" && type != "Long" && type != "Float" && type != "Double" &&
         type != "Char" && type != "Symbol" && type != "Nothing";
}

bool isZoneResultType(const std::string& type) {
  return type == "Nothing" || type == "Unit" || type == "Boolean" || type == "Byte" ||
         type == "Short" || type == "Int" || type == "Long" || type == "Float" ||
         type == "Double" || type == "Char";
}

bool isSizeOfPrimitiveType(const std::string& type) {
  return type == "Unit" || type == "Boolean" || type == "Byte" || type == "Short" ||
         type == "Int" || type == "Long" || type == "Float" || type == "Double" ||
         type == "Char";
}

bool isZoneByteArrayAccess(std::string_view target) {
  return target == support::StdNames::RuntimeByteArrayLength ||
         target == support::StdNames::RuntimeByteArrayApply ||
         target == support::StdNames::RuntimeByteArrayUpdate ||
         target == support::StdNames::RuntimeNativeBytesGetShortBe ||
         target == support::StdNames::RuntimeNativeBytesGetShortLe ||
         target == support::StdNames::RuntimeNativeBytesPutShortBe ||
         target == support::StdNames::RuntimeNativeBytesPutShortLe;
}

bool isByteBufferQuery(std::string_view target) {
  return target == support::StdNames::RuntimeByteBufferCapacity ||
         target == support::StdNames::RuntimeByteBufferPosition ||
         target == support::StdNames::RuntimeByteBufferLimit ||
         target == support::StdNames::RuntimeByteBufferRemaining ||
         target == support::StdNames::RuntimeByteBufferHasRemaining ||
         target == support::StdNames::RuntimeByteBufferGet;
}

bool isByteBufferMutation(std::string_view target) {
  return target == support::StdNames::RuntimeByteBufferSetPosition ||
         target == support::StdNames::RuntimeByteBufferSetLimit ||
         target == support::StdNames::RuntimeByteBufferPut ||
         target == support::StdNames::RuntimeByteBufferClear ||
         target == support::StdNames::RuntimeByteBufferFlip ||
         target == support::StdNames::RuntimeByteBufferRewind;
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

bool isScalarArrayElementTypeName(const std::string& typeName) {
  return typeName == "String" || typeName == "java.lang.String" || typeName == "Byte" ||
         typeName == "Short" || typeName == "Int" || typeName == "Boolean" ||
         typeName == "Long" || typeName == "Double" || typeName == "Float" ||
         typeName == "Char";
}

bool isTopObjectType(const std::string& type) {
  return type == "Object";
}

bool isCatchAllType(const std::string& type) {
  return isTopObjectType(type) || type == support::StdNames::JavaLangObject;
}

bool typesConform(const std::string& expected, const std::string& actual) {
  if (expected == "Unknown" || actual == "Unknown") {
    return true;
  }
  if (expected == actual) {
    return true;
  }
  if (actual == "Nothing") {
    return true;
  }
  if (isTopObjectType(expected) && isReferenceType(actual)) {
    return true;
  }
  if (expected == "Short" && actual == "Byte") {
    return true;
  }
  if (expected == "Int" && (actual == "Byte" || actual == "Short")) {
    return true;
  }
  if (expected == "Long" &&
      (actual == "Byte" || actual == "Short" || actual == "Int")) {
    return true;
  }
  if (expected == "Float" &&
      (actual == "Byte" || actual == "Short" || actual == "Int" || actual == "Long")) {
    return true;
  }
  if (expected == "Double" &&
      (actual == "Byte" || actual == "Short" || actual == "Int" || actual == "Long" ||
       actual == "Float")) {
    return true;
  }
  return actual == "Null" && isReferenceType(expected);
}

void verifyClassLikeDefinition(
    const Definition& definition,
    const std::unordered_map<std::string, const Definition*>& globals,
    VerifyResult& result) {
  if (definition.signature.empty()) {
    result.ok = false;
    result.errors.push_back("NIR " + std::string(definitionKindName(definition.kind)) +
                            " '" + definition.name + "' is missing parent metadata");
    return;
  }

  const std::vector<std::string> parentNames =
      metadataParentNames(definition.signature);
  std::unordered_set<std::string> seen;
  bool sawClassParent = false;
  for (std::size_t i = 0; i < parentNames.size(); ++i) {
    const std::string& parentName = parentNames[i];
    if (parentName.empty() || parentName == support::StdNames::JavaLangObject) {
      continue;
    }
    if (parentName == definition.name) {
      result.ok = false;
      result.errors.push_back("NIR " +
                              std::string(definitionKindName(definition.kind)) + " '" +
                              definition.name + "' extends itself");
      continue;
    }
    if (!seen.insert(parentName).second) {
      result.ok = false;
      result.errors.push_back(
          "NIR " + std::string(definitionKindName(definition.kind)) + " '" +
          definition.name + "' has duplicate parent: @" + parentName);
      continue;
    }
    auto parent = globals.find(parentName);
    if (parent == globals.end()) {
      result.ok = false;
      result.errors.push_back(
          "NIR " + std::string(definitionKindName(definition.kind)) + " '" +
          definition.name + "' extends unresolved parent: @" + parentName);
      continue;
    }
    if (!isInheritableDefinition(parent->second->kind)) {
      result.ok = false;
      result.errors.push_back(
          "NIR " + std::string(definitionKindName(definition.kind)) + " '" +
          definition.name + "' extends non-class or non-trait parent: @" + parentName);
      continue;
    }
    if (definition.kind == DefinitionKind::Trait &&
        parent->second->kind != DefinitionKind::Trait) {
      result.ok = false;
      result.errors.push_back("NIR trait '" + definition.name +
                              "' extends non-trait parent: @" + parentName);
    }
    if (parent->second->kind == DefinitionKind::Class) {
      if (sawClassParent || i != 0) {
        result.ok = false;
        result.errors.push_back(
            "NIR " + std::string(definitionKindName(definition.kind)) + " '" +
            definition.name + "' has invalid additional class parent: @" + parentName);
      }
      sawClassParent = true;
    }
  }
}

std::string ownerNameOf(const std::string& definitionName) {
  const std::size_t dot = definitionName.rfind('.');
  if (dot == std::string::npos) {
    return {};
  }
  return definitionName.substr(0, dot);
}

std::string memberNameOf(const std::string& definitionName) {
  const std::size_t dot = definitionName.rfind('.');
  if (dot == std::string::npos || dot + 1 >= definitionName.size()) {
    return {};
  }
  return definitionName.substr(dot + 1);
}

const Definition* resolveEffectMember(
    const std::string& receiverType, const std::string& memberName,
    const std::unordered_map<std::string, const Definition*>& globals,
    const std::unordered_map<std::string, std::vector<std::string>>& parentMap) {
  for (const std::string& current : linearizedTypeNames(receiverType, parentMap)) {
    if (current.empty() || current == support::StdNames::JavaLangObject) {
      continue;
    }
    auto member = globals.find(current + "." + memberName);
    if (member != globals.end()) {
      return member->second;
    }
  }
  return nullptr;
}

struct ReceiverEffectState {
  std::unordered_map<std::string, bool> receiverAliases;
  std::unordered_map<std::string, std::string> localTypes;
  std::unordered_set<std::string> dependencies;
  bool directlyEscapes = false;
};

std::string effectValueType(
    const Value& value, ReceiverEffectState& state,
    const std::unordered_map<std::string, const Definition*>& globals,
    const std::unordered_map<std::string, std::vector<std::string>>& parentMap) {
  switch (value.kind) {
  case ValueKind::Unit:
    return "Unit";
  case ValueKind::Literal:
    return value.type.empty() ? "Unknown" : value.type;
  case ValueKind::Local: {
    auto local = state.localTypes.find(value.text);
    if (local != state.localTypes.end()) {
      return local->second;
    }
    auto global = globals.find(value.text);
    if (global == globals.end()) {
      return "Unknown";
    }
    if (isFunction(global->second->kind)) {
      return signatureReturnType(global->second->signature);
    }
    return global->second->name;
  }
  case ValueKind::Super:
  case ValueKind::New:
    return value.type.empty() ? value.text : value.type;
  case ValueKind::SizeOf:
    return "Int";
  case ValueKind::Select: {
    if (value.operands.size() != 1) {
      return "Unknown";
    }
    const Definition* member = resolveEffectMember(
        effectValueType(value.operands.front(), state, globals, parentMap), value.text,
        globals, parentMap);
    if (member == nullptr) {
      return "Unknown";
    }
    return isFunction(member->kind) ? signatureReturnType(member->signature)
                                    : member->signature;
  }
  case ValueKind::Call:
    if (value.operands.empty()) {
      return "Unknown";
    }
    return effectValueType(value.operands.front(), state, globals, parentMap);
  case ValueKind::Unary:
    if (value.text == "!") {
      return "Boolean";
    }
    return value.operands.empty()
               ? "Unknown"
               : effectValueType(value.operands.front(), state, globals, parentMap);
  case ValueKind::Binary:
    if (value.text == "==" || value.text == "!=" || value.text == "<" ||
        value.text == ">" || value.text == "<=" || value.text == ">=" ||
        value.text == "&&" || value.text == "||") {
      return "Boolean";
    }
    if (value.text == "+" && value.operands.size() == 2) {
      const std::string lhs =
          effectValueType(value.operands[0], state, globals, parentMap);
      const std::string rhs =
          effectValueType(value.operands[1], state, globals, parentMap);
      if (lhs == "String" || rhs == "String") {
        return "String";
      }
    }
    return value.operands.empty()
               ? "Unknown"
               : effectValueType(value.operands.front(), state, globals, parentMap);
  case ValueKind::Assign:
  case ValueKind::LocalLet:
  case ValueKind::LocalVar:
  case ValueKind::While:
    return "Unit";
  case ValueKind::Throw:
    return "Nothing";
  case ValueKind::Try:
  case ValueKind::Catch:
    return value.type.empty() ? "Unknown" : value.type;
  case ValueKind::Finally:
    return "Unit";
  case ValueKind::Block: {
    ReceiverEffectState block = state;
    std::string result = "Unit";
    for (const Value& operand : value.operands) {
      if ((operand.kind == ValueKind::LocalLet ||
           operand.kind == ValueKind::LocalVar) &&
          operand.operands.size() == 1) {
        block.localTypes[operand.text] =
            operand.type.empty()
                ? effectValueType(operand.operands.front(), block, globals, parentMap)
                : operand.type;
        result = "Unit";
      } else {
        result = effectValueType(operand, block, globals, parentMap);
      }
    }
    return result;
  }
  case ValueKind::If:
    if (value.operands.size() != 3) {
      return "Unknown";
    }
    {
      const std::string thenType =
          effectValueType(value.operands[1], state, globals, parentMap);
      const std::string elseType =
          effectValueType(value.operands[2], state, globals, parentMap);
      if (thenType == "Nothing") {
        return elseType;
      }
      if (elseType == "Nothing" || thenType == elseType) {
        return thenType;
      }
      return "Unknown";
    }
  case ValueKind::ZoneScoped:
    return value.operands.size() == 1
               ? effectValueType(value.operands.front(), state, globals, parentMap)
               : "Unknown";
  case ValueKind::Box:
    return "Object";
  case ValueKind::Unbox:
    return value.text.empty() ? value.type : value.text;
  case ValueKind::IsInstanceOf:
    return "Boolean";
  case ValueKind::AsInstanceOf:
    return value.text.empty() ? value.type : value.text;
  case ValueKind::Unknown:
    return value.type.empty() ? "Unknown" : value.type;
  }
  return "Unknown";
}

bool scanReceiverEffect(
    const Value& value, ReceiverEffectState& state,
    const std::unordered_map<std::string, const Definition*>& globals,
    const std::unordered_map<std::string, std::vector<std::string>>& parentMap) {
  switch (value.kind) {
  case ValueKind::Unit:
  case ValueKind::Literal:
  case ValueKind::SizeOf:
  case ValueKind::Unknown:
    return false;
  case ValueKind::Super:
    return true;
  case ValueKind::Local: {
    auto alias = state.receiverAliases.find(value.text);
    return alias != state.receiverAliases.end() && alias->second;
  }
  case ValueKind::New:
    for (const Value& operand : value.operands) {
      (void)scanReceiverEffect(operand, state, globals, parentMap);
    }
    return false;
  case ValueKind::Block: {
    const auto savedAliases = state.receiverAliases;
    const auto savedTypes = state.localTypes;
    bool result = false;
    for (const Value& operand : value.operands) {
      result = scanReceiverEffect(operand, state, globals, parentMap);
    }
    state.receiverAliases = savedAliases;
    state.localTypes = savedTypes;
    return result;
  }
  case ValueKind::LocalLet:
  case ValueKind::LocalVar: {
    if (value.operands.size() != 1) {
      return false;
    }
    const bool initializer =
        scanReceiverEffect(value.operands.front(), state, globals, parentMap);
    state.receiverAliases[value.text] = initializer;
    state.localTypes[value.text] =
        value.type.empty()
            ? effectValueType(value.operands.front(), state, globals, parentMap)
            : value.type;
    return false;
  }
  case ValueKind::Select: {
    if (value.operands.size() != 1) {
      return false;
    }
    const bool receiver =
        scanReceiverEffect(value.operands.front(), state, globals, parentMap);
    if (receiver) {
      const Definition* member = resolveEffectMember(
          effectValueType(value.operands.front(), state, globals, parentMap),
          value.text, globals, parentMap);
      if (member != nullptr && isFunction(member->kind)) {
        state.dependencies.insert(member->name);
      }
    }
    return receiver &&
           isReferenceType(effectValueType(value, state, globals, parentMap));
  }
  case ValueKind::Call: {
    if (value.operands.empty()) {
      return false;
    }
    const bool callee =
        scanReceiverEffect(value.operands.front(), state, globals, parentMap);
    for (std::size_t i = 1; i < value.operands.size(); ++i) {
      if (scanReceiverEffect(value.operands[i], state, globals, parentMap)) {
        state.directlyEscapes = true;
      }
    }
    return callee && isReferenceType(effectValueType(value, state, globals, parentMap));
  }
  case ValueKind::Assign: {
    if (value.operands.size() != 2) {
      return false;
    }
    const bool assigned =
        scanReceiverEffect(value.operands.back(), state, globals, parentMap);
    const Value& target = value.operands.front();
    if (target.kind == ValueKind::Local) {
      state.receiverAliases[target.text] = assigned;
    } else if (target.kind == ValueKind::Select && !target.operands.empty()) {
      const bool receiver =
          scanReceiverEffect(target.operands.front(), state, globals, parentMap);
      if (assigned && !receiver) {
        state.directlyEscapes = true;
      }
    }
    return false;
  }
  case ValueKind::Try: {
    bool result = false;
    for (const Value& operand : value.operands) {
      if (operand.kind == ValueKind::Finally) {
        (void)scanReceiverEffect(operand, state, globals, parentMap);
      } else {
        result = scanReceiverEffect(operand, state, globals, parentMap) || result;
      }
    }
    return result;
  }
  case ValueKind::Catch: {
    if (value.operands.size() != 2 || value.operands.front().kind != ValueKind::Local) {
      return false;
    }
    const auto savedAliases = state.receiverAliases;
    const auto savedTypes = state.localTypes;
    state.receiverAliases[value.operands.front().text] = false;
    state.localTypes[value.operands.front().text] = value.operands.front().type;
    const bool result =
        scanReceiverEffect(value.operands.back(), state, globals, parentMap);
    state.receiverAliases = savedAliases;
    state.localTypes = savedTypes;
    return result;
  }
  case ValueKind::Finally:
    for (const Value& operand : value.operands) {
      (void)scanReceiverEffect(operand, state, globals, parentMap);
    }
    return false;
  case ValueKind::ZoneScoped:
  case ValueKind::Throw:
  case ValueKind::Box:
  case ValueKind::Unbox:
  case ValueKind::IsInstanceOf:
  case ValueKind::Unary:
  case ValueKind::Binary:
  case ValueKind::While:
    for (const Value& operand : value.operands) {
      (void)scanReceiverEffect(operand, state, globals, parentMap);
    }
    return false;
  case ValueKind::AsInstanceOf:
    return value.operands.size() == 1 &&
           scanReceiverEffect(value.operands.front(), state, globals, parentMap);
  case ValueKind::If: {
    bool result = false;
    for (const Value& operand : value.operands) {
      result = scanReceiverEffect(operand, state, globals, parentMap) || result;
    }
    return result;
  }
  }
  return false;
}

std::unordered_set<std::string> computeEscapingReceiverMethods(
    const Module& module,
    const std::unordered_map<std::string, const Definition*>& globals,
    const std::unordered_map<std::string, std::vector<std::string>>& parentMap) {
  std::unordered_map<std::string, std::unordered_set<std::string>> dependencies;
  std::vector<std::string> receiverMethods;
  std::unordered_set<std::string> receiverMethodNames;
  std::unordered_set<std::string> escaping;

  for (const Definition& definition : module.definitions) {
    if (definition.kind != DefinitionKind::FunctionDef) {
      continue;
    }
    const std::string owner = ownerNameOf(definition.name);
    auto ownerDefinition = globals.find(owner);
    const std::vector<std::string> parameters =
        signatureParameterTypes(definition.signature);
    if (ownerDefinition == globals.end() ||
        !isInheritableDefinition(ownerDefinition->second->kind) || parameters.empty() ||
        parameters.front() != owner) {
      continue;
    }

    ReceiverEffectState state;
    std::size_t parameterIndex = 0;
    for (const Instruction& instruction : definition.body.instructions) {
      if (instruction.kind == InstructionKind::Param) {
        state.localTypes[instruction.name] = instruction.type;
        state.receiverAliases[instruction.name] = parameterIndex == 0;
        ++parameterIndex;
      } else if (instruction.kind == InstructionKind::Let ||
                 instruction.kind == InstructionKind::Var) {
        const bool alias =
            scanReceiverEffect(instruction.value, state, globals, parentMap);
        state.receiverAliases[instruction.name] = alias;
        state.localTypes[instruction.name] =
            instruction.type.empty()
                ? effectValueType(instruction.value, state, globals, parentMap)
                : instruction.type;
      } else if (instruction.kind != InstructionKind::Unreachable) {
        (void)scanReceiverEffect(instruction.value, state, globals, parentMap);
      }
    }
    receiverMethodNames.insert(definition.name);
    dependencies[definition.name] = std::move(state.dependencies);
    if (state.directlyEscapes) {
      escaping.insert(definition.name);
    }
  }

  for (const Definition& definition : module.definitions) {
    if (!isFunction(definition.kind)) {
      continue;
    }
    const std::string owner = ownerNameOf(definition.name);
    auto ownerDefinition = globals.find(owner);
    const std::vector<std::string> parameters =
        signatureParameterTypes(definition.signature);
    if (ownerDefinition != globals.end() &&
        isInheritableDefinition(ownerDefinition->second->kind) && !parameters.empty() &&
        parameters.front() == owner) {
      receiverMethodNames.insert(definition.name);
    }
  }
  receiverMethods.assign(receiverMethodNames.begin(), receiverMethodNames.end());

  auto hasEscapingOverride = [&](const std::string& method) {
    const std::string owner = ownerNameOf(method);
    const std::string member = memberNameOf(method);
    if (owner.empty() || member.empty()) {
      return false;
    }
    for (const std::string& candidate : escaping) {
      const std::vector<std::string> candidateTypes =
          linearizedTypeNames(ownerNameOf(candidate), parentMap);
      if (memberNameOf(candidate) == member &&
          std::find(candidateTypes.begin(), candidateTypes.end(), owner) !=
              candidateTypes.end()) {
        return true;
      }
    }
    return false;
  };

  bool changed = true;
  while (changed) {
    changed = false;
    for (const std::string& method : receiverMethods) {
      if (!escaping.contains(method) && hasEscapingOverride(method)) {
        escaping.insert(method);
        changed = true;
      }
    }
    for (const auto& [method, callees] : dependencies) {
      if (escaping.contains(method)) {
        continue;
      }
      for (const std::string& callee : callees) {
        if (escaping.contains(callee) || hasEscapingOverride(callee)) {
          escaping.insert(method);
          changed = true;
          break;
        }
      }
    }
  }
  return escaping;
}

struct TypeMemberMetadata {
  bool valid = false;
  bool abstract = false;
  std::string target;
  std::string lowerBound;
  std::string upperBound;
};

TypeMemberMetadata typeMemberMetadata(const Definition& definition) {
  TypeMemberMetadata metadata;
  if (definition.kind != DefinitionKind::TypeMember || definition.signature.empty()) {
    return metadata;
  }
  if (definition.signature == "abstract") {
    metadata.valid = true;
    metadata.abstract = true;
  } else if (definition.signature.starts_with("abstract ")) {
    std::string_view bounds(definition.signature);
    bounds.remove_prefix(std::string_view("abstract ").size());
    if (bounds.starts_with(">: ")) {
      bounds.remove_prefix(3);
      const std::size_t upper = bounds.find(" <: ");
      metadata.lowerBound =
          trim(upper == std::string_view::npos ? bounds : bounds.substr(0, upper));
      if (upper != std::string_view::npos) {
        metadata.upperBound = trim(bounds.substr(upper + 4));
      }
    } else if (bounds.starts_with("<: ")) {
      bounds.remove_prefix(3);
      metadata.upperBound = trim(bounds);
    }
    metadata.valid = !metadata.lowerBound.empty() || !metadata.upperBound.empty();
    metadata.abstract = true;
  } else {
    metadata.valid = true;
    metadata.target = definition.signature;
  }
  return metadata;
}

bool metadataTypeIsSubtype(
    const std::string& actual, const std::string& expected,
    const std::unordered_map<std::string, std::vector<std::string>>& parentMap) {
  if (actual == expected) {
    return true;
  }
  const std::vector<std::string> types = linearizedTypeNames(actual, parentMap);
  return std::find(types.begin(), types.end(), expected) != types.end();
}

void verifyTypeMemberDefinition(
    const Definition& definition,
    const std::unordered_map<std::string, const Definition*>& globals,
    VerifyResult& result) {
  if (definition.signature.empty()) {
    result.ok = false;
    result.errors.push_back("NIR type member '" + definition.name +
                            "' is missing abstract or alias metadata");
  } else if (!typeMemberMetadata(definition).valid) {
    result.ok = false;
    result.errors.push_back("NIR type member '" + definition.name +
                            "' has malformed metadata: " + definition.signature);
  }
  if (!definition.body.empty()) {
    result.ok = false;
    result.errors.push_back("NIR type member '" + definition.name +
                            "' must not have a body");
  }
  const std::string owner = ownerNameOf(definition.name);
  auto ownerDefinition = globals.find(owner);
  if (owner.empty() || ownerDefinition == globals.end() ||
      !isClassLikeDefinition(ownerDefinition->second->kind)) {
    result.ok = false;
    result.errors.push_back("NIR type member '" + definition.name +
                            "' has no class-like owner");
  }
}

void verifyConcreteTypeMembers(
    const std::unordered_map<std::string, const Definition*>& globals,
    const std::unordered_map<std::string, std::vector<std::string>>& parentMap,
    VerifyResult& result) {
  std::unordered_map<std::string, std::unordered_map<std::string, const Definition*>>
      membersByOwner;
  for (const auto& [name, definition] : globals) {
    if (definition->kind != DefinitionKind::TypeMember) {
      continue;
    }
    membersByOwner[ownerNameOf(name)][memberNameOf(name)] = definition;
  }

  for (const auto& [name, definition] : globals) {
    if (definition->kind != DefinitionKind::Class &&
        definition->kind != DefinitionKind::Module) {
      continue;
    }
    std::unordered_map<std::string, const Definition*> effective;
    if (auto own = membersByOwner.find(name); own != membersByOwner.end()) {
      effective = own->second;
    }
    auto parents = parentMap.find(name);
    if (parents != parentMap.end()) {
      for (const std::string& parent :
           linearizedParentNames(parents->second, parentMap)) {
        auto inherited = membersByOwner.find(parent);
        if (inherited == membersByOwner.end()) {
          continue;
        }
        for (const auto& [memberName, member] : inherited->second) {
          effective.try_emplace(memberName, member);
        }
      }
    }
    for (const auto& [memberName, member] : effective) {
      if (typeMemberMetadata(*member).abstract) {
        result.ok = false;
        result.errors.push_back(
            "NIR " + std::string(definitionKindName(definition->kind)) + " '" + name +
            "' must implement abstract type member " + memberName);
      }
    }
  }
}

void verifyTypeMemberInheritance(
    const std::unordered_map<std::string, const Definition*>& globals,
    const std::unordered_map<std::string, std::vector<std::string>>& parentMap,
    VerifyResult& result) {
  std::unordered_map<std::string, std::unordered_map<std::string, const Definition*>>
      membersByOwner;
  for (const auto& [name, definition] : globals) {
    if (definition->kind == DefinitionKind::TypeMember) {
      membersByOwner[ownerNameOf(name)][memberNameOf(name)] = definition;
      const TypeMemberMetadata metadata = typeMemberMetadata(*definition);
      if (metadata.valid && metadata.abstract && !metadata.lowerBound.empty() &&
          !metadata.upperBound.empty() &&
          !metadataTypeIsSubtype(metadata.lowerBound, metadata.upperBound, parentMap)) {
        result.ok = false;
        result.errors.push_back(
            "NIR type member '" + name + "' lower bound " + metadata.lowerBound +
            " does not conform to upper bound " + metadata.upperBound);
      }
    }
  }

  for (const auto& [ownerName, owner] : globals) {
    if (!isClassLikeDefinition(owner->kind)) {
      continue;
    }
    const auto directParents = parentMap.find(ownerName);
    if (directParents == parentMap.end()) {
      continue;
    }
    std::unordered_map<std::string, const Definition*> effective;
    if (auto own = membersByOwner.find(ownerName); own != membersByOwner.end()) {
      effective = own->second;
    }
    const std::vector<std::string> parents =
        linearizedParentNames(directParents->second, parentMap);
    for (const std::string& parent : parents) {
      auto inherited = membersByOwner.find(parent);
      if (inherited == membersByOwner.end()) {
        continue;
      }
      for (const auto& [memberName, member] : inherited->second) {
        effective.try_emplace(memberName, member);
      }
    }

    for (const std::string& parent : parents) {
      auto inherited = membersByOwner.find(parent);
      if (inherited == membersByOwner.end()) {
        continue;
      }
      for (const auto& [memberName, requiredDefinition] : inherited->second) {
        auto selected = effective.find(memberName);
        if (selected == effective.end()) {
          continue;
        }
        const TypeMemberMetadata required = typeMemberMetadata(*requiredDefinition);
        const TypeMemberMetadata actual = typeMemberMetadata(*selected->second);
        if (!required.valid || !actual.valid ||
            selected->second == requiredDefinition) {
          continue;
        }
        if (!required.abstract) {
          if (actual.abstract || actual.target != required.target) {
            result.ok = false;
            result.errors.push_back(
                "NIR " + std::string(definitionKindName(owner->kind)) + " '" +
                ownerName + "' inherits incompatible type alias " + memberName);
          }
          continue;
        }
        if (required.upperBound.empty()) {
          if (required.lowerBound.empty()) {
            continue;
          }
        }
        const std::string actualLower =
            actual.abstract ? actual.lowerBound : actual.target;
        const std::string actualUpper =
            actual.abstract ? actual.upperBound : actual.target;
        if (!required.lowerBound.empty() &&
            (actualLower.empty() ||
             !metadataTypeIsSubtype(required.lowerBound, actualLower, parentMap))) {
          result.ok = false;
          result.errors.push_back(
              "NIR " + std::string(definitionKindName(owner->kind)) + " '" + ownerName +
              "' type member " + memberName + " target " +
              (actualLower.empty() ? "<unbounded>" : actualLower) +
              " does not preserve inherited lower bound " + required.lowerBound);
        }
        if (!required.upperBound.empty() &&
            (actualUpper.empty() ||
             !metadataTypeIsSubtype(actualUpper, required.upperBound, parentMap))) {
          result.ok = false;
          result.errors.push_back(
              "NIR " + std::string(definitionKindName(owner->kind)) + " '" + ownerName +
              "' type member " + memberName + " target " +
              (actualUpper.empty() ? "<unbounded>" : actualUpper) +
              " does not conform to inherited upper bound " + required.upperBound);
        }
      }
    }
  }
}

bool signatureUsesAbstractTypeMember(
    const std::string& signature,
    const std::unordered_set<std::string>& abstractTypeMembers) {
  if (abstractTypeMembers.contains(signatureReturnType(signature))) {
    return true;
  }
  const std::vector<std::string> parameters = signatureParameterTypes(signature);
  return std::any_of(
      parameters.begin(), parameters.end(),
      [&](const std::string& type) { return abstractTypeMembers.contains(type); });
}

void verifyConcreteTypeMemberRuntimeSignatures(
    const std::unordered_map<std::string, const Definition*>& globals,
    const std::unordered_map<std::string, std::vector<std::string>>& parentMap,
    VerifyResult& result) {
  std::unordered_set<std::string> abstractTypeMembers;
  std::unordered_map<std::string, std::unordered_map<std::string, const Definition*>>
      functionsByOwner;
  for (const auto& [name, definition] : globals) {
    if (definition->kind == DefinitionKind::TypeMember &&
        typeMemberMetadata(*definition).abstract) {
      abstractTypeMembers.insert(name);
    } else if (isFunction(definition->kind)) {
      functionsByOwner[ownerNameOf(name)][memberNameOf(name)] = definition;
    }
  }

  for (const auto& [name, definition] : globals) {
    if (definition->kind != DefinitionKind::Class &&
        definition->kind != DefinitionKind::Module) {
      continue;
    }
    std::unordered_set<std::string> ownMembers;
    if (auto own = functionsByOwner.find(name); own != functionsByOwner.end()) {
      for (const auto& [memberName, member] : own->second) {
        (void)member;
        ownMembers.insert(memberName);
      }
    }

    std::unordered_map<std::string, const Definition*> inherited;
    auto parents = parentMap.find(name);
    if (parents != parentMap.end()) {
      for (const std::string& parent :
           linearizedParentNames(parents->second, parentMap)) {
        auto parentFunctions = functionsByOwner.find(parent);
        if (parentFunctions == functionsByOwner.end()) {
          continue;
        }
        for (const auto& [memberName, member] : parentFunctions->second) {
          inherited.try_emplace(memberName, member);
        }
      }
    }

    for (const auto& [memberName, member] : inherited) {
      if (ownMembers.contains(memberName) ||
          member->kind != DefinitionKind::FunctionDef ||
          !signatureUsesAbstractTypeMember(member->signature, abstractTypeMembers)) {
        continue;
      }
      result.ok = false;
      result.errors.push_back(
          "NIR " + std::string(definitionKindName(definition->kind)) + " '" + name +
          "' must override inherited member " + memberName +
          " to specialize its abstract type-member runtime signature");
    }
  }
}

struct ValueInfo {
  bool resolved = true;
  std::string type = "Unknown";
  std::string globalName;
};

class BodyVerifier {
public:
  BodyVerifier(const Module& module,
               const std::unordered_map<std::string, const Definition*>& globals,
               const std::unordered_set<std::string>& escapingReceiverMethods,
               const Definition& definition, VerifyResult& result)
      : module_(module), globals_(globals), definition_(definition), result_(result),
        ownerName_(ownerNameOf(definition.name)),
        escapingReceiverMethods_(&escapingReceiverMethods) {}

  void verify() {
    const std::vector<std::string> parameterTypes =
        signatureParameterTypes(definition_.signature);
    const std::string returnType = signatureReturnType(definition_.signature);

    std::size_t parameterIndex = 0;
    bool sawNonParameter = false;
    bool sawTerminator = false;

    for (std::size_t i = 0; i < definition_.body.instructions.size(); ++i) {
      const Instruction& instruction = definition_.body.instructions[i];
      if (sawTerminator) {
        addError("NIR function definition '" + definition_.name +
                 "' has instructions after a terminator");
        break;
      }

      if (instruction.kind == InstructionKind::Param) {
        verifyParameter(instruction, parameterIndex, parameterTypes, sawNonParameter);
        ++parameterIndex;
        continue;
      }

      sawNonParameter = true;
      switch (instruction.kind) {
      case InstructionKind::Param:
        break;
      case InstructionKind::Let:
        verifyLet(instruction);
        break;
      case InstructionKind::Var:
        verifyVar(instruction);
        break;
      case InstructionKind::Eval:
        (void)verifyValue(instruction.value);
        break;
      case InstructionKind::Return:
        verifyReturn(instruction, returnType);
        break;
      case InstructionKind::Throw:
        verifyThrow(instruction);
        break;
      case InstructionKind::Unreachable:
        break;
      }

      if (isTerminator(instruction.kind)) {
        sawTerminator = true;
      }
    }

    if (parameterIndex != parameterTypes.size()) {
      addError("NIR function definition '" + definition_.name + "' has " +
               std::to_string(parameterIndex) + " parameter instructions but " +
               std::to_string(parameterTypes.size()) + " signature parameters");
    }
    if (!sawTerminator) {
      addError("NIR function definition '" + definition_.name +
               "' is missing a terminator");
    }
  }

private:
  struct ZoneEscapeState {
    std::unordered_map<std::string, bool> arenaReferences;
    std::unordered_map<std::string, std::string> localTypes;
    std::unordered_set<std::string> zoneLocals;
  };

  void addError(std::string message) {
    result_.ok = false;
    result_.errors.push_back(std::move(message));
  }

  const Definition* resolveMemberGlobal(const std::string& receiverGlobalName,
                                        const std::string& memberName) const {
    std::unordered_map<std::string, std::vector<std::string>> parentMap;
    for (const auto& [name, definition] : globals_) {
      if (definition != nullptr && isClassLikeDefinition(definition->kind)) {
        parentMap[name] = metadataParentNames(definition->signature);
      }
    }
    for (const std::string& current :
         linearizedTypeNames(receiverGlobalName, parentMap)) {
      if (current.empty() || current == support::StdNames::JavaLangObject) {
        continue;
      }
      const std::string candidate = current + "." + memberName;
      auto member = globals_.find(candidate);
      if (member != globals_.end()) {
        return member->second;
      }
    }
    return nullptr;
  }

  bool isSubtypeOf(const std::string& actual, const std::string& expected) const {
    if (actual == expected) {
      return true;
    }

    std::vector<std::string> worklist{actual};
    std::unordered_set<std::string> visited;
    while (!worklist.empty()) {
      std::string current = std::move(worklist.back());
      worklist.pop_back();
      if (current.empty() || current == support::StdNames::JavaLangObject ||
          !visited.insert(current).second) {
        continue;
      }
      auto definition = globals_.find(current);
      if (definition == globals_.end() ||
          !isClassLikeDefinition(definition->second->kind)) {
        continue;
      }
      for (const std::string& parent :
           metadataParentNames(definition->second->signature)) {
        if (parent == expected) {
          return true;
        }
        worklist.push_back(parent);
      }
    }
    return false;
  }

  bool isKnownCatchReferenceType(const std::string& type) const {
    if (isCatchAllType(type)) {
      return true;
    }
    const Definition* definition = resolveGlobal(type);
    return definition != nullptr && (definition->kind == DefinitionKind::Class ||
                                     definition->kind == DefinitionKind::Trait);
  }

  bool isValidCatchType(const std::string& type) const {
    if (isCatchAllType(type)) {
      return true;
    }
    const Definition* definition = resolveGlobal(type);
    return definition != nullptr &&
           (definition->kind == DefinitionKind::Trait ||
            (definition->kind == DefinitionKind::Class &&
             isSubtypeOf(definition->name,
                         std::string(support::StdNames::JavaLangThrowable))));
  }

  bool catchTypeShadows(const std::string& earlier, const std::string& later) const {
    return isCatchAllType(earlier) || isSubtypeOf(later, earlier) ||
           isSubtypeOf(std::string(support::StdNames::JavaLangThrowable), earlier);
  }

  bool typesConformTo(const std::string& expected, const std::string& actual) const {
    if (typesConform(expected, actual)) {
      return true;
    }
    if (!isReferenceType(expected) || !isReferenceType(actual)) {
      return false;
    }
    return isSubtypeOf(actual, expected);
  }

  bool isSupportedArrayElementType(const std::string& type) const {
    if (isScalarArrayElementTypeName(type) || isTopObjectType(type)) {
      return true;
    }
    if (const std::string nestedElement = arrayElementTypeName(type);
        !nestedElement.empty()) {
      return isSupportedArrayElementType(nestedElement);
    }
    const Definition* referenceElement = resolveGlobal(type);
    return referenceElement != nullptr && isClassLikeDefinition(referenceElement->kind);
  }

  void verifyParameter(const Instruction& instruction, std::size_t parameterIndex,
                       const std::vector<std::string>& parameterTypes,
                       bool sawNonParameter) {
    if (sawNonParameter) {
      addError("NIR function definition '" + definition_.name +
               "' has parameter instruction after non-parameter instructions");
    }
    if (instruction.name.empty()) {
      addError("NIR function definition '" + definition_.name +
               "' has an unnamed parameter");
      return;
    }
    if (!locals_.insert(instruction.name).second) {
      addError("NIR function definition '" + definition_.name +
               "' declares duplicate local: " + instruction.name);
    }
    if (parameterIndex >= parameterTypes.size()) {
      addError("NIR function definition '" + definition_.name +
               "' has too many parameter instructions");
    } else if (instruction.type != parameterTypes[parameterIndex]) {
      addError("NIR function definition '" + definition_.name + "' parameter %" +
               instruction.name + " type " + instruction.type +
               " does not match signature type " + parameterTypes[parameterIndex]);
    }
    localTypes_[instruction.name] = instruction.type;
  }

  void verifyLet(const Instruction& instruction) {
    if (instruction.name.empty()) {
      addError("NIR function definition '" + definition_.name +
               "' has an unnamed local");
      return;
    }
    ValueInfo value = verifyValue(instruction.value);
    if (!locals_.insert(instruction.name).second) {
      addError("NIR function definition '" + definition_.name +
               "' declares duplicate local: " + instruction.name);
    }
    if (!instruction.type.empty() && !typesConformTo(instruction.type, value.type)) {
      addError("NIR function definition '" + definition_.name + "' declares local " +
               instruction.name + " of type " + instruction.type +
               " from value of type " + value.type);
    }
    localTypes_[instruction.name] =
        instruction.type.empty() ? value.type : instruction.type;
  }

  void verifyVar(const Instruction& instruction) {
    if (instruction.name.empty()) {
      addError("NIR function definition '" + definition_.name +
               "' has an unnamed mutable local");
      return;
    }
    ValueInfo value = verifyValue(instruction.value);
    if (!locals_.insert(instruction.name).second) {
      addError("NIR function definition '" + definition_.name +
               "' declares duplicate local: " + instruction.name);
    }
    if (!instruction.type.empty() && !typesConformTo(instruction.type, value.type)) {
      addError("NIR function definition '" + definition_.name +
               "' declares mutable local " + instruction.name + " of type " +
               instruction.type + " from value of type " + value.type);
    }
    localTypes_[instruction.name] =
        instruction.type.empty() ? value.type : instruction.type;
    mutableLocals_.insert(instruction.name);
  }

  void verifyReturn(const Instruction& instruction,
                    const std::string& expectedReturnType) {
    if (instruction.type != expectedReturnType) {
      addError("NIR function definition '" + definition_.name + "' return type " +
               instruction.type + " does not match signature return type " +
               expectedReturnType);
    }
    ValueInfo value = verifyValue(instruction.value);
    if (!typesConformTo(instruction.type, value.type)) {
      addError("NIR function definition '" + definition_.name +
               "' returns value of type " + value.type + " as " + instruction.type);
    }
  }

  void verifyThrow(const Instruction& instruction) {
    if (instruction.type != "Nothing") {
      addError("NIR function definition '" + definition_.name +
               "' throw terminator must have type Nothing");
    }
    const ValueInfo value = verifyValue(instruction.value);
    const bool isThrowable =
        value.type == "Null" ||
        isSubtypeOf(value.type, std::string(support::StdNames::JavaLangThrowable));
    if (!isThrowable) {
      addError("NIR function definition '" + definition_.name +
               "' throws non-Throwable value of type " + value.type);
    }
  }

  const Definition* zoneSelectedDefinition(const Value& value,
                                           ZoneEscapeState& state) const {
    if (value.kind != ValueKind::Select || value.operands.size() != 1) {
      return nullptr;
    }
    const std::string receiverType = zoneValueType(value.operands.front(), state);
    const Definition* receiver = resolveGlobal(receiverType);
    if (receiver == nullptr || !isClassLikeDefinition(receiver->kind)) {
      return nullptr;
    }
    return resolveMemberGlobal(receiver->name, value.text);
  }

  std::string zoneValueType(const Value& value, ZoneEscapeState& state) const {
    switch (value.kind) {
    case ValueKind::Unit:
      return "Unit";
    case ValueKind::Literal:
      return value.type.empty() ? "Unknown" : value.type;
    case ValueKind::Local: {
      auto local = state.localTypes.find(value.text);
      if (local != state.localTypes.end()) {
        return local->second;
      }
      auto outer = localTypes_.find(value.text);
      if (outer != localTypes_.end()) {
        return outer->second;
      }
      const Definition* global = resolveGlobal(value.text);
      return global == nullptr ? "Unknown" : typeOfGlobal(*global);
    }
    case ValueKind::New:
      return value.type.empty() ? value.text : value.type;
    case ValueKind::SizeOf:
      return "Int";
    case ValueKind::Select: {
      const Definition* member = zoneSelectedDefinition(value, state);
      return member == nullptr ? "Unknown" : typeOfGlobal(*member);
    }
    case ValueKind::Call:
      if (value.operands.empty()) {
        return "Unknown";
      }
      if (value.operands.front().kind == ValueKind::Select) {
        const Definition* member =
            zoneSelectedDefinition(value.operands.front(), state);
        return member == nullptr ? "Unknown" : typeOfGlobal(*member);
      }
      if (value.operands.front().kind == ValueKind::Local) {
        const Definition* function = resolveGlobal(value.operands.front().text);
        return function == nullptr ? "Unknown" : typeOfGlobal(*function);
      }
      return "Unknown";
    case ValueKind::Unary:
      if (value.text == "!") {
        return "Boolean";
      }
      return value.operands.empty() ? "Unknown"
                                    : zoneValueType(value.operands.front(), state);
    case ValueKind::Binary:
      if (value.text == "==" || value.text == "!=" || value.text == "<" ||
          value.text == ">" || value.text == "<=" || value.text == ">=" ||
          value.text == "&&" || value.text == "||") {
        return "Boolean";
      }
      if (value.text == "+" && value.operands.size() == 2) {
        const std::string lhs = zoneValueType(value.operands[0], state);
        const std::string rhs = zoneValueType(value.operands[1], state);
        if (lhs == "String" || rhs == "String") {
          return "String";
        }
      }
      return value.operands.empty() ? "Unknown"
                                    : zoneValueType(value.operands.front(), state);
    case ValueKind::Assign:
    case ValueKind::LocalLet:
    case ValueKind::LocalVar:
    case ValueKind::While:
      return "Unit";
    case ValueKind::Throw:
      return "Nothing";
    case ValueKind::Try:
    case ValueKind::Catch:
      return value.type.empty() ? "Unknown" : value.type;
    case ValueKind::Finally:
      return "Unit";
    case ValueKind::Block: {
      ZoneEscapeState blockState = state;
      std::string result = "Unit";
      for (const Value& operand : value.operands) {
        if ((operand.kind == ValueKind::LocalLet ||
             operand.kind == ValueKind::LocalVar) &&
            operand.operands.size() == 1) {
          const std::string initializerType =
              zoneValueType(operand.operands.front(), blockState);
          blockState.localTypes[operand.text] =
              operand.type.empty() ? initializerType : operand.type;
          result = "Unit";
        } else {
          result = zoneValueType(operand, blockState);
        }
      }
      return result;
    }
    case ValueKind::If:
      if (value.operands.size() != 3) {
        return "Unknown";
      }
      {
        const std::string thenType = zoneValueType(value.operands[1], state);
        const std::string elseType = zoneValueType(value.operands[2], state);
        if (thenType == "Nothing") {
          return elseType;
        }
        if (elseType == "Nothing" || thenType == elseType) {
          return thenType;
        }
        return "Unknown";
      }
    case ValueKind::ZoneScoped:
      return value.operands.size() == 1 ? zoneValueType(value.operands.front(), state)
                                        : "Unknown";
    case ValueKind::Box:
      return "Object";
    case ValueKind::Unbox:
      return value.text.empty() ? value.type : value.text;
    case ValueKind::IsInstanceOf:
      return "Boolean";
    case ValueKind::AsInstanceOf:
      return value.text.empty() ? value.type : value.text;
    case ValueKind::Super:
      return value.type.empty() ? value.text : value.type;
    case ValueKind::Unknown:
      return value.type.empty() ? "Unknown" : value.type;
    }
    return "Unknown";
  }

  bool verifyZoneEscapeValue(const Value& value, ZoneEscapeState& state) {
    switch (value.kind) {
    case ValueKind::Unit:
    case ValueKind::Literal:
    case ValueKind::SizeOf:
    case ValueKind::Super:
    case ValueKind::Unknown:
      return false;
    case ValueKind::Local: {
      auto found = state.arenaReferences.find(value.text);
      return found != state.arenaReferences.end() && found->second;
    }
    case ValueKind::New:
      for (const Value& operand : value.operands) {
        (void)verifyZoneEscapeValue(operand, state);
      }
      return true;
    case ValueKind::Block: {
      const auto savedReferences = state.arenaReferences;
      const auto savedTypes = state.localTypes;
      const auto savedZoneLocals = state.zoneLocals;
      bool result = false;
      for (const Value& operand : value.operands) {
        result = verifyZoneEscapeValue(operand, state);
      }
      state.arenaReferences = savedReferences;
      state.localTypes = savedTypes;
      state.zoneLocals = savedZoneLocals;
      return result;
    }
    case ValueKind::LocalLet:
    case ValueKind::LocalVar: {
      if (value.operands.size() != 1) {
        return false;
      }
      const bool initializer = verifyZoneEscapeValue(value.operands.front(), state);
      state.arenaReferences[value.text] = initializer;
      state.localTypes[value.text] = value.type.empty()
                                         ? zoneValueType(value.operands.front(), state)
                                         : value.type;
      state.zoneLocals.insert(value.text);
      return false;
    }
    case ValueKind::Select:
      if (value.operands.size() != 1) {
        return false;
      }
      {
        const bool receiver = verifyZoneEscapeValue(value.operands.front(), state);
        const Definition* member =
            receiver ? zoneSelectedDefinition(value, state) : nullptr;
        if (member != nullptr && isFunction(member->kind) &&
            escapingReceiverMethods_ != nullptr &&
            escapingReceiverMethods_->contains(member->name)) {
          addError("NIR function definition '" + definition_.name +
                   "' invokes a method that may leak a scoped-zone receiver: " +
                   member->name);
        }
        return receiver && isReferenceType(zoneValueType(value, state));
      }
    case ValueKind::Call: {
      if (value.operands.empty()) {
        return false;
      }
      const std::string_view target =
          value.operands.front().kind == ValueKind::Local
              ? std::string_view(value.operands.front().text)
              : std::string_view{};
      if (target == support::StdNames::RuntimeZoneAllocBytes) {
        for (std::size_t i = 1; i < value.operands.size(); ++i) {
          (void)verifyZoneEscapeValue(value.operands[i], state);
        }
        return true;
      }
      if (target == support::StdNames::RuntimeByteBufferWrap) {
        for (std::size_t i = 1; i < value.operands.size(); ++i) {
          (void)verifyZoneEscapeValue(value.operands[i], state);
        }
        return true;
      }
      if (isByteBufferQuery(target) || isByteBufferMutation(target)) {
        bool receiver = false;
        for (std::size_t i = 1; i < value.operands.size(); ++i) {
          const bool argument = verifyZoneEscapeValue(value.operands[i], state);
          if (i == 1) {
            receiver = argument;
          } else if (argument) {
            addError("NIR function definition '" + definition_.name +
                     "' passes a scoped-zone reference to a ByteBuffer state "
                     "argument");
          }
        }
        return isByteBufferMutation(target) && receiver;
      }
      if (isZoneByteArrayAccess(target)) {
        for (std::size_t i = 1; i < value.operands.size(); ++i) {
          const bool argument = verifyZoneEscapeValue(value.operands[i], state);
          if (argument && i != 1) {
            addError("NIR function definition '" + definition_.name +
                     "' passes a scoped-zone reference to a byte-array value "
                     "argument");
          }
        }
        return false;
      }
      const bool callee = verifyZoneEscapeValue(value.operands.front(), state);
      for (std::size_t i = 1; i < value.operands.size(); ++i) {
        if (verifyZoneEscapeValue(value.operands[i], state)) {
          addError("NIR function definition '" + definition_.name +
                   "' passes a scoped-zone reference to an ordinary call");
        }
      }
      return callee && isReferenceType(zoneValueType(value, state));
    }
    case ValueKind::Assign: {
      if (value.operands.size() != 2) {
        return false;
      }
      const bool assigned = verifyZoneEscapeValue(value.operands.back(), state);
      const Value& target = value.operands.front();
      if (target.kind == ValueKind::Local) {
        if (assigned && !state.zoneLocals.contains(target.text)) {
          addError("NIR function definition '" + definition_.name +
                   "' assigns a scoped-zone reference to an outer local");
        }
        if (state.zoneLocals.contains(target.text)) {
          state.arenaReferences[target.text] = assigned;
        }
      } else if (target.kind == ValueKind::Select && !target.operands.empty()) {
        const bool receiver = verifyZoneEscapeValue(target.operands.front(), state);
        if (assigned && !receiver) {
          addError("NIR function definition '" + definition_.name +
                   "' stores a scoped-zone reference outside the zone");
        }
      }
      return false;
    }
    case ValueKind::Try: {
      bool result = false;
      for (const Value& operand : value.operands) {
        if (operand.kind == ValueKind::Finally) {
          (void)verifyZoneEscapeValue(operand, state);
        } else {
          result = verifyZoneEscapeValue(operand, state) || result;
        }
      }
      return result;
    }
    case ValueKind::Catch: {
      if (value.operands.size() != 2 ||
          value.operands.front().kind != ValueKind::Local) {
        return false;
      }
      const auto savedReferences = state.arenaReferences;
      const auto savedTypes = state.localTypes;
      const auto savedZoneLocals = state.zoneLocals;
      const std::string& bindingName = value.operands.front().text;
      state.arenaReferences[bindingName] = false;
      state.localTypes[bindingName] = value.operands.front().type;
      state.zoneLocals.insert(bindingName);
      const bool result = verifyZoneEscapeValue(value.operands.back(), state);
      state.arenaReferences = savedReferences;
      state.localTypes = savedTypes;
      state.zoneLocals = savedZoneLocals;
      return result;
    }
    case ValueKind::Finally:
      for (const Value& operand : value.operands) {
        (void)verifyZoneEscapeValue(operand, state);
      }
      return false;
    case ValueKind::ZoneScoped:
      return false;
    case ValueKind::Box:
      for (const Value& operand : value.operands) {
        (void)verifyZoneEscapeValue(operand, state);
      }
      return true;
    case ValueKind::Unbox:
    case ValueKind::IsInstanceOf:
      for (const Value& operand : value.operands) {
        (void)verifyZoneEscapeValue(operand, state);
      }
      return false;
    case ValueKind::AsInstanceOf:
      return value.operands.size() == 1 &&
             verifyZoneEscapeValue(value.operands.front(), state);
    case ValueKind::Throw:
      for (const Value& operand : value.operands) {
        (void)verifyZoneEscapeValue(operand, state);
      }
      return false;
    case ValueKind::Unary:
    case ValueKind::Binary:
    case ValueKind::While:
      for (const Value& operand : value.operands) {
        (void)verifyZoneEscapeValue(operand, state);
      }
      return false;
    case ValueKind::If: {
      bool result = false;
      for (const Value& operand : value.operands) {
        result = verifyZoneEscapeValue(operand, state) || result;
      }
      return result;
    }
    }
    return false;
  }

  ValueInfo verifyValue(const Value& value) {
    switch (value.kind) {
    case ValueKind::Unit:
      return ValueInfo{true, "Unit", {}};
    case ValueKind::Literal:
      return ValueInfo{true, value.type.empty() ? "Unknown" : value.type, {}};
    case ValueKind::Local:
      return verifyReference(value.text);
    case ValueKind::Super: {
      const std::string parentType = value.type.empty() ? value.text : value.type;
      const Definition* parent = resolveGlobal(parentType);
      if (parent == nullptr || !isClassLikeDefinition(parent->kind)) {
        addError("NIR function definition '" + definition_.name +
                 "' references unresolved super type: " + parentType);
        return ValueInfo{false, "Unknown", {}};
      }
      return ValueInfo{true, parent->name, parent->name};
    }
    case ValueKind::New: {
      if (value.text.empty()) {
        return ValueInfo{true, "Unknown", {}};
      }
      const std::string elementType = arrayElementTypeName(value.text);
      if (!elementType.empty()) {
        if (!isSupportedArrayElementType(elementType)) {
          addError("NIR function definition '" + definition_.name +
                   "' constructs Array with unsupported element type " + elementType);
          return ValueInfo{false, "Unknown", {}};
        }
        bool resolved = true;
        for (const Value& element : value.operands) {
          ValueInfo elementInfo = verifyValue(element);
          resolved = resolved && elementInfo.resolved;
          const bool conforms = isScalarArrayElementTypeName(elementType)
                                    ? typesConform(elementType, elementInfo.type)
                                    : typesConformTo(elementType, elementInfo.type);
          if (!conforms) {
            addError("NIR function definition '" + definition_.name +
                     "' constructs Array[" + elementType + "] with element type " +
                     elementInfo.type);
          }
        }
        return ValueInfo{resolved, "Array [ " + elementType + " ]", {}};
      }
      const Definition* global = resolveGlobal(value.text);
      if (global == nullptr || global->kind != DefinitionKind::Class) {
        addError("NIR function definition '" + definition_.name +
                 "' constructs undefined class: " + value.text);
        return ValueInfo{false, "Unknown", {}};
      }
      const std::vector<std::string> constructorParameterTypes =
          constructorParameterTypesOfClass(global->name);
      if (value.operands.size() != constructorParameterTypes.size()) {
        addError("NIR function definition '" + definition_.name + "' constructs " +
                 global->name + " with " + std::to_string(value.operands.size()) +
                 " arguments but expected " +
                 std::to_string(constructorParameterTypes.size()));
      }
      const std::size_t checkedArguments =
          std::min(value.operands.size(), constructorParameterTypes.size());
      for (std::size_t i = 0; i < checkedArguments; ++i) {
        ValueInfo argument = verifyValue(value.operands[i]);
        if (!typesConformTo(constructorParameterTypes[i], argument.type)) {
          addError("NIR function definition '" + definition_.name + "' constructs " +
                   global->name + " with argument " + std::to_string(i) + " of type " +
                   argument.type + " for field type " + constructorParameterTypes[i]);
        }
      }
      return ValueInfo{true, global->name, global->name};
    }
    case ValueKind::SizeOf: {
      if (value.text.empty() || !value.operands.empty()) {
        addError("NIR function definition '" + definition_.name +
                 "' has malformed sizeof value");
        return ValueInfo{false, "Unknown", {}};
      }
      if (isSizeOfPrimitiveType(value.text)) {
        return ValueInfo{true, "Int", {}};
      }
      const Definition* target = resolveGlobal(value.text);
      if (target == nullptr || target->kind != DefinitionKind::Class) {
        addError("NIR function definition '" + definition_.name +
                 "' applies sizeof to a non-concrete class type: " + value.text);
        return ValueInfo{false, "Unknown", {}};
      }
      return ValueInfo{true, "Int", {}};
    }
    case ValueKind::Throw: {
      if (value.operands.size() != 1 || value.type != "Nothing") {
        addError("NIR function definition '" + definition_.name +
                 "' has malformed throw value");
        return ValueInfo{false, "Nothing", {}};
      }
      const ValueInfo exception = verifyValue(value.operands.front());
      const bool isThrowable =
          exception.type == "Null" ||
          isSubtypeOf(exception.type,
                      std::string(support::StdNames::JavaLangThrowable));
      if (!isThrowable) {
        addError("NIR function definition '" + definition_.name +
                 "' throws non-Throwable value of type " + exception.type);
      }
      return ValueInfo{exception.resolved && isThrowable, "Nothing", {}};
    }
    case ValueKind::Try:
      return verifyTry(value);
    case ValueKind::Catch:
      addError("NIR function definition '" + definition_.name +
               "' has catch value outside try");
      return ValueInfo{false, value.type.empty() ? "Unknown" : value.type, {}};
    case ValueKind::Finally:
      addError("NIR function definition '" + definition_.name +
               "' has finally value outside try");
      return ValueInfo{false, "Unit", {}};
    case ValueKind::Block: {
      const std::unordered_set<std::string> savedLocals = locals_;
      const std::unordered_map<std::string, std::string> savedLocalTypes = localTypes_;
      const std::unordered_set<std::string> savedMutableLocals = mutableLocals_;
      std::unordered_set<std::string> declaredHere;
      ValueInfo result{true, "Unit", {}};
      for (const Value& operand : value.operands) {
        if ((operand.kind == ValueKind::LocalLet ||
             operand.kind == ValueKind::LocalVar) &&
            !declaredHere.insert(operand.text).second) {
          addError("NIR function definition '" + definition_.name +
                   "' declares duplicate block local: " + operand.text);
        }
        result = verifyValue(operand);
      }
      locals_ = savedLocals;
      localTypes_ = savedLocalTypes;
      mutableLocals_ = savedMutableLocals;
      return result;
    }
    case ValueKind::LocalLet:
    case ValueKind::LocalVar: {
      if (value.text.empty() || value.operands.size() != 1) {
        addError("NIR function definition '" + definition_.name +
                 "' has malformed block local binding");
        return ValueInfo{false, "Unit", {}};
      }
      ValueInfo initializer = verifyValue(value.operands.front());
      const std::string localType = value.type.empty() ? initializer.type : value.type;
      if (!value.type.empty() && !typesConformTo(value.type, initializer.type)) {
        addError("NIR function definition '" + definition_.name +
                 "' declares block local " + value.text + " of type " + value.type +
                 " from value of type " + initializer.type);
      }
      locals_.insert(value.text);
      localTypes_[value.text] = localType;
      if (value.kind == ValueKind::LocalVar) {
        mutableLocals_.insert(value.text);
      } else {
        mutableLocals_.erase(value.text);
      }
      return ValueInfo{initializer.resolved, "Unit", {}};
    }
    case ValueKind::ZoneScoped: {
      if (value.operands.size() != 1) {
        addError("NIR function definition '" + definition_.name +
                 "' has malformed zone-scoped value");
        return ValueInfo{false, "Unknown", {}};
      }
      ++zoneDepth_;
      ValueInfo result = verifyValue(value.operands.front());
      --zoneDepth_;
      ZoneEscapeState escapeState;
      escapeState.localTypes = localTypes_;
      (void)verifyZoneEscapeValue(value.operands.front(), escapeState);
      if (!isZoneResultType(result.type)) {
        addError("NIR function definition '" + definition_.name +
                 "' allows value of type " + result.type + " to escape a scoped zone");
        result.resolved = false;
      }
      return result;
    }
    case ValueKind::Box: {
      if (value.operands.size() != 1 || !isBoxablePrimitiveType(value.text)) {
        addError("NIR function definition '" + definition_.name +
                 "' has malformed or unsupported box value");
        return ValueInfo{false, "Unknown", {}};
      }
      ValueInfo operand = verifyValue(value.operands.front());
      if (!typesConformTo(value.text, operand.type)) {
        addError("NIR function definition '" + definition_.name +
                 "' boxes value of type " + operand.type + " as " + value.text);
        return ValueInfo{false, "Unknown", {}};
      }
      return ValueInfo{operand.resolved, "Object", {}};
    }
    case ValueKind::Unbox: {
      if (value.operands.size() != 1 || !isBoxablePrimitiveType(value.text)) {
        addError("NIR function definition '" + definition_.name +
                 "' has malformed or unsupported unbox value");
        return ValueInfo{false, "Unknown", {}};
      }
      ValueInfo operand = verifyValue(value.operands.front());
      if (!isReferenceType(operand.type)) {
        addError("NIR function definition '" + definition_.name +
                 "' unboxes non-reference value of type " + operand.type + " as " +
                 value.text);
        return ValueInfo{false, "Unknown", {}};
      }
      return ValueInfo{operand.resolved, value.text, {}};
    }
    case ValueKind::IsInstanceOf: {
      if (value.operands.size() != 1 || value.text.empty()) {
        addError("NIR function definition '" + definition_.name +
                 "' has malformed is-instance-of value");
        return ValueInfo{false, "Unknown", {}};
      }
      const bool targetsBoxedPrimitive = isBoxablePrimitiveType(value.text);
      const Definition* target =
          targetsBoxedPrimitive ? nullptr : resolveGlobal(value.text);
      if (!targetsBoxedPrimitive &&
          (target == nullptr || (target->kind != DefinitionKind::Class &&
                                 target->kind != DefinitionKind::Trait))) {
        addError("NIR function definition '" + definition_.name +
                 "' tests against undefined class or trait: " + value.text);
        return ValueInfo{false, "Unknown", {}};
      }
      ValueInfo operand = verifyValue(value.operands.front());
      if (!isReferenceType(operand.type)) {
        addError("NIR function definition '" + definition_.name +
                 "' applies is-instance-of to non-reference value of type " +
                 operand.type);
        return ValueInfo{false, "Unknown", {}};
      }
      return ValueInfo{operand.resolved, "Boolean", {}};
    }
    case ValueKind::AsInstanceOf: {
      if (value.operands.size() != 1 || value.text.empty()) {
        addError("NIR function definition '" + definition_.name +
                 "' has malformed as-instance-of value");
        return ValueInfo{false, "Unknown", {}};
      }
      const Definition* target = resolveGlobal(value.text);
      if (target == nullptr || (target->kind != DefinitionKind::Class &&
                                target->kind != DefinitionKind::Trait)) {
        addError("NIR function definition '" + definition_.name +
                 "' casts to undefined class or trait: " + value.text);
        return ValueInfo{false, "Unknown", {}};
      }
      ValueInfo operand = verifyValue(value.operands.front());
      if (!isReferenceType(operand.type)) {
        addError("NIR function definition '" + definition_.name +
                 "' applies as-instance-of to non-reference value of type " +
                 operand.type);
        return ValueInfo{false, "Unknown", {}};
      }
      return ValueInfo{operand.resolved, target->name, target->name};
    }
    case ValueKind::Select:
      return verifySelect(value);
    case ValueKind::Call:
      return verifyCall(value);
    case ValueKind::Unary:
      return verifyUnary(value);
    case ValueKind::Binary:
      return verifyBinary(value);
    case ValueKind::Assign:
      return verifyAssign(value);
    case ValueKind::If:
      return verifyIf(value);
    case ValueKind::While:
      return verifyWhile(value);
    case ValueKind::Unknown:
      return ValueInfo{true, value.type.empty() ? "Unknown" : value.type, {}};
    }
    return ValueInfo{true, "Unknown", {}};
  }

  ValueInfo verifyReference(const std::string& name) {
    auto local = localTypes_.find(name);
    if (local != localTypes_.end()) {
      return ValueInfo{true, local->second, {}};
    }

    if (const Definition* global = resolveGlobal(name)) {
      return ValueInfo{true, typeOfGlobal(*global), global->name};
    }

    addError("NIR function definition '" + definition_.name +
             "' references undefined local or global: " + name);
    return ValueInfo{false, "Unknown", {}};
  }

  ValueInfo verifySelect(const Value& value) {
    if (value.operands.size() != 1 || value.text.empty()) {
      addError("NIR function definition '" + definition_.name +
               "' has malformed select value");
      return ValueInfo{false, "Unknown", {}};
    }

    ValueInfo receiver = verifyValue(value.operands.front());
    if (!receiver.resolved) {
      return ValueInfo{receiver.resolved, "Unknown", {}};
    }

    std::string receiverGlobalName;
    if (!receiver.globalName.empty()) {
      const Definition* receiverDefinition = resolveGlobal(receiver.globalName);
      if (receiverDefinition != nullptr &&
          isClassLikeDefinition(receiverDefinition->kind)) {
        receiverGlobalName = receiverDefinition->name;
      }
    }
    if (receiverGlobalName.empty() && receiver.type != "Unknown") {
      if (const Definition* receiverType = resolveGlobal(receiver.type);
          receiverType != nullptr && isClassLikeDefinition(receiverType->kind)) {
        receiverGlobalName = receiverType->name;
      }
    }
    if (receiverGlobalName.empty()) {
      return ValueInfo{true, "Unknown", {}};
    }

    const Definition* member = resolveMemberGlobal(receiverGlobalName, value.text);
    if (member == nullptr) {
      addError("NIR function definition '" + definition_.name +
               "' references unresolved selected global: " + receiverGlobalName + "." +
               value.text);
      return ValueInfo{false, "Unknown", {}};
    }
    return ValueInfo{true, typeOfGlobal(*member), member->name};
  }

  ValueInfo verifyCall(const Value& value) {
    if (value.operands.empty()) {
      addError("NIR function definition '" + definition_.name +
               "' has malformed call value");
      return ValueInfo{false, "Unknown", {}};
    }

    ValueInfo callee = verifyValue(value.operands.front());
    if (!callee.resolved || callee.globalName.empty()) {
      return ValueInfo{callee.resolved, "Unknown", {}};
    }
    if (callee.globalName == support::StdNames::RuntimeZoneAllocBytes &&
        zoneDepth_ == 0) {
      addError("NIR function definition '" + definition_.name +
               "' calls zoneAllocBytes outside a zone-scoped value");
    }

    auto calleeDefinition = globals_.find(callee.globalName);
    if (calleeDefinition == globals_.end() ||
        !isFunction(calleeDefinition->second->kind)) {
      addError("NIR function definition '" + definition_.name +
               "' calls non-function global: " + callee.globalName);
      return ValueInfo{false, "Unknown", {}};
    }

    const std::vector<std::string> parameterTypes =
        signatureParameterTypes(calleeDefinition->second->signature);
    const std::string calleeOwner = ownerNameOf(callee.globalName);
    auto ownerDefinition = globals_.find(calleeOwner);
    const bool isModuleMember = ownerDefinition != globals_.end() &&
                                ownerDefinition->second->kind == DefinitionKind::Module;
    const bool hasImplicitReceiver = value.operands.front().kind == ValueKind::Select &&
                                     !parameterTypes.empty() && !isModuleMember;
    const std::size_t implicitReceiverCount = hasImplicitReceiver ? 1 : 0;
    const std::size_t argumentCount = value.operands.size() - 1 + implicitReceiverCount;
    if (argumentCount != parameterTypes.size()) {
      addError("NIR function definition '" + definition_.name + "' calls " +
               callee.globalName + " with " + std::to_string(argumentCount) +
               " arguments but expected " + std::to_string(parameterTypes.size()));
    }

    if (hasImplicitReceiver && !value.operands.front().operands.empty()) {
      ValueInfo receiver = verifyValue(value.operands.front().operands.front());
      if (!parameterTypes.empty() &&
          !typesConformTo(parameterTypes.front(), receiver.type)) {
        addError("NIR function definition '" + definition_.name +
                 "' passes receiver of type " + receiver.type + " to parameter type " +
                 parameterTypes.front());
      }
    }

    for (std::size_t i = 1; i < value.operands.size(); ++i) {
      ValueInfo argument = verifyValue(value.operands[i]);
      const std::size_t parameterIndex = i - 1 + implicitReceiverCount;
      if (parameterIndex < parameterTypes.size() &&
          !typesConformTo(parameterTypes[parameterIndex], argument.type)) {
        addError("NIR function definition '" + definition_.name + "' passes argument " +
                 std::to_string(parameterIndex) + " of type " + argument.type +
                 " to parameter type " + parameterTypes[parameterIndex]);
      }
    }

    return ValueInfo{true, signatureReturnType(calleeDefinition->second->signature),
                     callee.globalName};
  }

  ValueInfo verifyUnary(const Value& value) {
    if (value.operands.size() != 1 || value.text.empty()) {
      addError("NIR function definition '" + definition_.name +
               "' has malformed unary value");
      return ValueInfo{false, "Unknown", {}};
    }

    ValueInfo operand = verifyValue(value.operands.front());
    if (value.text == "!") {
      if (operand.type != "Boolean" && operand.type != "Unknown") {
        addError("NIR function definition '" + definition_.name +
                 "' applies ! to a non-Boolean operand");
      }
      return ValueInfo{operand.resolved, "Boolean", {}};
    }
    if (value.text == "+" || value.text == "-") {
      if (operand.type != "Byte" && operand.type != "Short" && operand.type != "Int" &&
          operand.type != "Long" && operand.type != "Float" &&
          operand.type != "Double" && operand.type != "Unknown") {
        addError("NIR function definition '" + definition_.name + "' applies " +
                 value.text + " to a non-numeric operand");
      }
      return ValueInfo{operand.resolved,
                       operand.type == "Byte" || operand.type == "Short" ? "Int"
                                                                         : operand.type,
                       {}};
    }

    addError("NIR function definition '" + definition_.name +
             "' has unsupported unary operator: " + value.text);
    return ValueInfo{false, "Unknown", {}};
  }

  ValueInfo verifyBinary(const Value& value) {
    if (value.operands.size() != 2 || value.text.empty()) {
      addError("NIR function definition '" + definition_.name +
               "' has malformed binary value");
      return ValueInfo{false, "Unknown", {}};
    }
    ValueInfo lhs = verifyValue(value.operands[0]);
    ValueInfo rhs = verifyValue(value.operands[1]);
    if (value.text == "&&" || value.text == "||") {
      if ((lhs.type != "Boolean" && lhs.type != "Unknown") ||
          (rhs.type != "Boolean" && rhs.type != "Unknown")) {
        addError("NIR function definition '" + definition_.name +
                 "' has logical operands that are not Boolean");
      }
      return ValueInfo{lhs.resolved && rhs.resolved, "Boolean", {}};
    }
    if (value.text == "&" || value.text == "|" || value.text == "^") {
      const auto isBitwise = [](const std::string& type) {
        return type == "Boolean" || type == "Byte" || type == "Short" ||
               type == "Int" || type == "Long" || type == "Unknown";
      };
      if (!isBitwise(lhs.type) || !isBitwise(rhs.type)) {
        addError("NIR function definition '" + definition_.name +
                 "' has bitwise operands that are not Boolean, Int, or Long");
      }
      if (lhs.type != "Unknown" && rhs.type != "Unknown" && lhs.type != rhs.type) {
        addError("NIR function definition '" + definition_.name +
                 "' has bitwise operands with mismatched types: " + lhs.type + " and " +
                 rhs.type);
      }
      return ValueInfo{lhs.resolved && rhs.resolved,
                       lhs.type == "Byte" || lhs.type == "Short" ? "Int" : lhs.type,
                       {}};
    }
    if (value.text == "<<" || value.text == ">>" || value.text == ">>>") {
      if (lhs.type != "Byte" && lhs.type != "Short" && lhs.type != "Int" &&
          lhs.type != "Long" && lhs.type != "Unknown") {
        addError("NIR function definition '" + definition_.name +
                 "' has a shift left operand that is not Int or Long");
      }
      if (rhs.type != "Int" && rhs.type != "Unknown") {
        addError("NIR function definition '" + definition_.name +
                 "' has a shift count that is not Int");
      }
      return ValueInfo{lhs.resolved && rhs.resolved,
                       lhs.type == "Byte" || lhs.type == "Short" ? "Int" : lhs.type,
                       {}};
    }
    if (value.text == "==" || value.text == "!=" || value.text == "<" ||
        value.text == ">" || value.text == "<=" || value.text == ">=") {
      return ValueInfo{lhs.resolved && rhs.resolved, "Boolean", {}};
    }
    if (value.text == "+" && (lhs.type == "String" || rhs.type == "String")) {
      const auto isStringConvertible = [](const std::string& type) {
        return type == "Unit" || type == "String" || type == "Boolean" ||
               type == "Byte" || type == "Short" || type == "Int" || type == "Long" ||
               type == "Float" || type == "Double" || type == "Char" ||
               type == "Symbol" || type == "Null" || isReferenceType(type) ||
               type == "Unknown";
      };
      if (!isStringConvertible(lhs.type) || !isStringConvertible(rhs.type)) {
        addError("NIR function definition '" + definition_.name +
                 "' has string concatenation operands that are not String, primitive, "
                 "Null, or object values");
      }
      return ValueInfo{lhs.resolved && rhs.resolved, "String", {}};
    }
    if (value.text == "%" &&
        ((lhs.type != "Byte" && lhs.type != "Short" && lhs.type != "Int" &&
          lhs.type != "Long" && lhs.type != "Float" && lhs.type != "Double" &&
          lhs.type != "Unknown") ||
         (rhs.type != "Byte" && rhs.type != "Short" && rhs.type != "Int" &&
          rhs.type != "Long" && rhs.type != "Float" && rhs.type != "Double" &&
          rhs.type != "Unknown"))) {
      addError("NIR function definition '" + definition_.name +
               "' has remainder operands that are not numeric");
    }
    if (lhs.type != "Unknown" && rhs.type != "Unknown" && lhs.type != rhs.type) {
      addError("NIR function definition '" + definition_.name +
               "' has binary operands with mismatched types: " + lhs.type + " and " +
               rhs.type);
    }
    return ValueInfo{lhs.resolved && rhs.resolved,
                     lhs.type == "Byte" || lhs.type == "Short" ? "Int" : lhs.type,
                     {}};
  }

  ValueInfo verifyAssign(const Value& value) {
    if (value.operands.size() != 2) {
      addError("NIR function definition '" + definition_.name +
               "' has malformed assign value");
      return ValueInfo{false, "Unknown", {}};
    }

    if (value.operands.front().kind == ValueKind::Local) {
      const std::string& localName = value.operands.front().text;
      auto localType = localTypes_.find(localName);
      if (localType == localTypes_.end()) {
        addError("NIR function definition '" + definition_.name +
                 "' assigns to undefined local: " + localName);
        (void)verifyValue(value.operands.back());
        return ValueInfo{false, "Unit", {}};
      }
      if (!mutableLocals_.contains(localName)) {
        addError("NIR function definition '" + definition_.name +
                 "' assigns to immutable local: " + localName);
      }
      ValueInfo assigned = verifyValue(value.operands.back());
      if (!typesConformTo(localType->second, assigned.type)) {
        addError("NIR function definition '" + definition_.name +
                 "' assigns value of type " + assigned.type + " to local type " +
                 localType->second);
      }
      return ValueInfo{assigned.resolved, "Unit", {}};
    }

    if (value.operands.front().kind != ValueKind::Select) {
      addError("NIR function definition '" + definition_.name +
               "' assigns to unsupported non-field target");
      (void)verifyValue(value.operands.back());
      return ValueInfo{false, "Unit", {}};
    }

    ValueInfo target = verifyValue(value.operands.front());
    ValueInfo assigned = verifyValue(value.operands.back());
    const Definition* targetDefinition = nullptr;
    if (!target.globalName.empty()) {
      auto found = globals_.find(target.globalName);
      if (found != globals_.end()) {
        targetDefinition = found->second;
      }
    }
    if (targetDefinition != nullptr &&
        targetDefinition->kind == DefinitionKind::Field) {
      if (!typesConformTo(target.type, assigned.type)) {
        addError("NIR function definition '" + definition_.name +
                 "' assigns value of type " + assigned.type + " to field type " +
                 target.type);
      }
      return ValueInfo{target.resolved && assigned.resolved, "Unit", {}};
    }

    const Definition* setterDefinition = nullptr;
    if (targetDefinition != nullptr && isFunction(targetDefinition->kind)) {
      auto setter = globals_.find(target.globalName + "_$eq");
      if (setter != globals_.end() && isFunction(setter->second->kind)) {
        setterDefinition = setter->second;
      }
    }
    if (setterDefinition == nullptr) {
      addError("NIR function definition '" + definition_.name +
               "' assigns to unresolved or non-field target");
      return ValueInfo{false, "Unit", {}};
    }

    const std::vector<std::string> setterParameters =
        signatureParameterTypes(setterDefinition->signature);
    auto setterOwner = globals_.find(ownerNameOf(setterDefinition->name));
    const bool moduleSetter = setterOwner != globals_.end() &&
                              setterOwner->second->kind == DefinitionKind::Module;
    const std::size_t expectedParameterCount = moduleSetter ? 1 : 2;
    if (setterParameters.size() != expectedParameterCount ||
        signatureReturnType(setterDefinition->signature) != "Unit") {
      addError("NIR function definition '" + definition_.name +
               "' resolves malformed setter: " + setterDefinition->name);
      return ValueInfo{false, "Unit", {}};
    }
    const std::size_t valueParameterIndex = moduleSetter ? 0 : 1;
    if (!typesConformTo(setterParameters[valueParameterIndex], assigned.type)) {
      addError("NIR function definition '" + definition_.name +
               "' assigns value of type " + assigned.type + " to setter type " +
               setterParameters[valueParameterIndex]);
    }
    return ValueInfo{target.resolved && assigned.resolved, "Unit", {}};
  }

  ValueInfo verifyIf(const Value& value) {
    if (value.operands.size() != 3) {
      addError("NIR function definition '" + definition_.name +
               "' has malformed if value");
      return ValueInfo{false, "Unknown", {}};
    }
    ValueInfo condition = verifyValue(value.operands[0]);
    if (condition.type != "Unknown" && condition.type != "Boolean") {
      addError("NIR function definition '" + definition_.name +
               "' has non-Boolean if condition: " + condition.type);
    }
    ValueInfo thenValue = verifyValue(value.operands[1]);
    ValueInfo elseValue = verifyValue(value.operands[2]);
    if (thenValue.type == elseValue.type) {
      return ValueInfo{condition.resolved && thenValue.resolved && elseValue.resolved,
                       thenValue.type,
                       {}};
    }
    if (thenValue.type == "Unknown") {
      return ValueInfo{condition.resolved && elseValue.resolved, elseValue.type, {}};
    }
    if (elseValue.type == "Unknown") {
      return ValueInfo{condition.resolved && thenValue.resolved, thenValue.type, {}};
    }
    if (thenValue.type == "Nothing") {
      return ValueInfo{condition.resolved && thenValue.resolved && elseValue.resolved,
                       elseValue.type,
                       {}};
    }
    if (elseValue.type == "Nothing") {
      return ValueInfo{condition.resolved && thenValue.resolved && elseValue.resolved,
                       thenValue.type,
                       {}};
    }
    addError("NIR function definition '" + definition_.name +
             "' has if branches with mismatched types: " + thenValue.type + " and " +
             elseValue.type);
    return ValueInfo{
        condition.resolved && thenValue.resolved && elseValue.resolved, "Unknown", {}};
  }

  ValueInfo verifyWhile(const Value& value) {
    if (value.operands.size() != 2) {
      addError("NIR function definition '" + definition_.name +
               "' has malformed while value");
      return ValueInfo{false, "Unknown", {}};
    }
    ValueInfo condition = verifyValue(value.operands[0]);
    if (condition.type != "Unknown" && condition.type != "Boolean") {
      addError("NIR function definition '" + definition_.name +
               "' has non-Boolean while condition: " + condition.type);
    }
    (void)verifyValue(value.operands[1]);
    return ValueInfo{condition.resolved, "Unit", {}};
  }

  ValueInfo verifyCatch(const Value& value) {
    if (value.operands.size() != 2 || value.type.empty() ||
        value.operands.front().kind != ValueKind::Local ||
        value.operands.front().text.empty() || value.operands.front().type.empty()) {
      addError("NIR function definition '" + definition_.name +
               "' has malformed catch value");
      return ValueInfo{false, value.type.empty() ? "Unknown" : value.type, {}};
    }

    const Value& binding = value.operands.front();
    const bool knownReferenceType = isKnownCatchReferenceType(binding.type);
    const bool validExceptionType = isValidCatchType(binding.type);
    if (!knownReferenceType) {
      addError("NIR function definition '" + definition_.name +
               "' catches non-reference or unresolved type " + binding.type);
    } else if (!validExceptionType) {
      addError("NIR function definition '" + definition_.name +
               "' catches class outside Throwable hierarchy: " + binding.type);
    }

    const std::unordered_set<std::string> savedLocals = locals_;
    const std::unordered_map<std::string, std::string> savedLocalTypes = localTypes_;
    const std::unordered_set<std::string> savedMutableLocals = mutableLocals_;
    locals_.insert(binding.text);
    localTypes_[binding.text] = binding.type;
    mutableLocals_.erase(binding.text);
    ValueInfo body = verifyValue(value.operands.back());
    locals_ = savedLocals;
    localTypes_ = savedLocalTypes;
    mutableLocals_ = savedMutableLocals;

    if (!typesConformTo(value.type, body.type)) {
      addError("NIR function definition '" + definition_.name +
               "' declares catch result " + value.type + " from body type " +
               body.type);
    }
    return ValueInfo{validExceptionType && body.resolved, value.type, {}};
  }

  ValueInfo verifyFinally(const Value& value) {
    if (value.operands.size() != 1 || value.type != "Unit") {
      addError("NIR function definition '" + definition_.name +
               "' has malformed finally value");
      return ValueInfo{false, "Unit", {}};
    }
    ValueInfo body = verifyValue(value.operands.front());
    return ValueInfo{body.resolved, "Unit", {}};
  }

  ValueInfo verifyTry(const Value& value) {
    if (value.operands.size() < 2 || value.type.empty()) {
      addError("NIR function definition '" + definition_.name +
               "' has malformed try value");
      return ValueInfo{false, value.type.empty() ? "Unknown" : value.type, {}};
    }

    ValueInfo body = verifyValue(value.operands.front());
    bool resolved = body.resolved;
    bool sawCatch = false;
    bool sawFinally = false;
    bool sawCatchAll = false;
    std::vector<std::string> earlierCatchTypes;
    if (!typesConformTo(value.type, body.type)) {
      addError("NIR function definition '" + definition_.name +
               "' declares try result " + value.type + " from body type " + body.type);
    }
    for (std::size_t index = 1; index < value.operands.size(); ++index) {
      const Value& operand = value.operands[index];
      if (operand.kind == ValueKind::Catch && !sawFinally) {
        sawCatch = true;
        const bool hasBinding = operand.operands.size() == 2 &&
                                operand.operands.front().kind == ValueKind::Local;
        const std::string catchType =
            hasBinding ? operand.operands.front().type : std::string{};
        if (sawCatchAll) {
          addError("NIR function definition '" + definition_.name +
                   "' has catch handler after catch-all");
          resolved = false;
        } else if (hasBinding && isValidCatchType(catchType)) {
          auto shadowing =
              std::find_if(earlierCatchTypes.begin(), earlierCatchTypes.end(),
                           [&](const std::string& earlier) {
                             return catchTypeShadows(earlier, catchType);
                           });
          if (shadowing != earlierCatchTypes.end()) {
            addError("NIR function definition '" + definition_.name +
                     "' has unreachable catch type " + catchType + " after " +
                     *shadowing);
            resolved = false;
          }
        }
        ValueInfo handler = verifyCatch(operand);
        resolved = resolved && handler.resolved;
        if (!typesConformTo(value.type, handler.type)) {
          addError("NIR function definition '" + definition_.name +
                   "' declares try result " + value.type + " from catch type " +
                   handler.type);
        }
        if (hasBinding && isValidCatchType(catchType)) {
          earlierCatchTypes.push_back(catchType);
        }
        if (hasBinding && isCatchAllType(catchType)) {
          sawCatchAll = true;
        }
        continue;
      }
      if (operand.kind == ValueKind::Finally && !sawFinally &&
          index + 1 == value.operands.size()) {
        sawFinally = true;
        resolved = verifyFinally(operand).resolved && resolved;
        continue;
      }
      addError("NIR function definition '" + definition_.name +
               "' requires ordered catches followed by at most one finalizer");
      resolved = false;
    }
    if (!sawCatch && !sawFinally) {
      addError("NIR function definition '" + definition_.name +
               "' try value has neither catch nor finally");
      resolved = false;
    }
    return ValueInfo{resolved, value.type, {}};
  }

  const Definition* resolveGlobal(const std::string& name) const {
    auto direct = globals_.find(name);
    if (direct != globals_.end()) {
      return direct->second;
    }
    if (!ownerName_.empty()) {
      auto ownerLocal = globals_.find(ownerName_ + "." + name);
      if (ownerLocal != globals_.end()) {
        return ownerLocal->second;
      }
    }
    if (!module_.name.empty()) {
      auto moduleLocal = globals_.find(module_.name + "." + name);
      if (moduleLocal != globals_.end()) {
        return moduleLocal->second;
      }
    }
    return nullptr;
  }

  std::vector<std::string>
  constructorParameterTypesOfClass(const std::string& className) const {
    std::vector<std::string> fieldTypes;
    for (const Definition& definition : module_.definitions) {
      if (definition.kind == DefinitionKind::Field &&
          ownerNameOf(definition.name) == className && definition.body.empty()) {
        fieldTypes.push_back(definition.signature.empty() ? "Unknown"
                                                          : definition.signature);
      }
    }
    return fieldTypes;
  }

  std::string typeOfGlobal(const Definition& definition) const {
    if (isFunction(definition.kind)) {
      return signatureReturnType(definition.signature);
    }
    if (definition.kind == DefinitionKind::Field) {
      return definition.signature.empty() ? "Unknown" : definition.signature;
    }
    return definition.name;
  }

  const Module& module_;
  const std::unordered_map<std::string, const Definition*>& globals_;
  const Definition& definition_;
  VerifyResult& result_;
  std::string ownerName_;
  std::unordered_set<std::string> locals_;
  std::unordered_set<std::string> mutableLocals_;
  std::unordered_map<std::string, std::string> localTypes_;
  const std::unordered_set<std::string>* escapingReceiverMethods_ = nullptr;
  std::size_t zoneDepth_ = 0;
};

} // namespace

VerifyResult Verifier::verify(const Module& module) const {
  VerifyResult result;
  if (module.name.empty()) {
    result.ok = false;
    result.errors.push_back("NIR module name is empty");
  }

  std::unordered_map<std::string, const Definition*> globals;
  std::unordered_set<std::string> names;
  for (const Definition& definition : module.definitions) {
    if (definition.name.empty()) {
      result.ok = false;
      result.errors.push_back("NIR definition name is empty");
      continue;
    }
    if (!definition.name.empty() && !names.insert(definition.name).second) {
      result.ok = false;
      result.errors.push_back("duplicate NIR definition: " + definition.name);
      continue;
    }
    globals[definition.name] = &definition;
  }

  std::unordered_map<std::string, std::vector<std::string>> parentMap;
  for (const Definition& definition : module.definitions) {
    if (isClassLikeDefinition(definition.kind)) {
      parentMap[definition.name] = metadataParentNames(definition.signature);
    }
  }
  for (const Definition& definition : module.definitions) {
    if (!isClassLikeDefinition(definition.kind)) {
      continue;
    }
    const LinearizationResult linearization =
        checkedLinearizedParentNames(parentMap[definition.name], parentMap);
    if (linearization.cyclic) {
      result.ok = false;
      result.errors.push_back("NIR " +
                              std::string(definitionKindName(definition.kind)) + " '" +
                              definition.name + "' has cyclic inheritance");
    } else if (!linearization.consistent) {
      result.ok = false;
      result.errors.push_back(
          "NIR " + std::string(definitionKindName(definition.kind)) + " '" +
          definition.name + "' has inconsistent parent linearization");
    }
  }

  const std::unordered_set<std::string> escapingReceiverMethods =
      computeEscapingReceiverMethods(module, globals, parentMap);

  for (const Definition& definition : module.definitions) {
    if (isClassLikeDefinition(definition.kind)) {
      verifyClassLikeDefinition(definition, globals, result);
    }
    if (isFunction(definition.kind)) {
      if (definition.signature.empty()) {
        result.ok = false;
        result.errors.push_back("NIR function '" + definition.name +
                                "' is missing a signature");
      } else if (!isValidFunctionSignature(definition.signature)) {
        result.ok = false;
        result.errors.push_back("invalid NIR function signature for '" +
                                definition.name + "': " + definition.signature);
      }
    }
    if (definition.kind == DefinitionKind::TypeMember) {
      verifyTypeMemberDefinition(definition, globals, result);
    }
    if (definition.kind == DefinitionKind::FunctionDecl && !definition.body.empty()) {
      result.ok = false;
      result.errors.push_back("NIR function declaration '" + definition.name +
                              "' must not have a body");
    }
    if (definition.kind == DefinitionKind::FunctionDef) {
      if (definition.body.empty()) {
        result.ok = false;
        result.errors.push_back("NIR function definition '" + definition.name +
                                "' is missing a body");
      } else {
        BodyVerifier bodyVerifier(module, globals, escapingReceiverMethods, definition,
                                  result);
        bodyVerifier.verify();
      }
    }
    if (definition.kind == DefinitionKind::Field && !definition.body.empty()) {
      const std::string ownerName = ownerNameOf(definition.name);
      Definition initializer = definition;
      initializer.kind = DefinitionKind::FunctionDef;
      initializer.name = definition.name + ".$init";
      initializer.signature =
          "(" + ownerName + ")" +
          (definition.signature.empty() ? "Unknown" : definition.signature);
      BodyVerifier bodyVerifier(module, globals, escapingReceiverMethods, initializer,
                                result);
      bodyVerifier.verify();
    }
  }

  verifyConcreteTypeMembers(globals, parentMap, result);
  verifyTypeMemberInheritance(globals, parentMap, result);
  verifyConcreteTypeMemberRuntimeSignatures(globals, parentMap, result);

  return result;
}

} // namespace scalanative::nir
