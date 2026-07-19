#include "scalanative/tools/linker/Linker.h"

#include "scalanative/support/StdNames.h"

#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scalanative::tools::linker {

namespace {

constexpr const char* RuntimeMain = support::StdNames::RuntimeMain.data();

struct GlobalInfo {
  const nir::Definition* definition = nullptr;
  std::string moduleName;
};

struct ReferenceContext {
  std::string moduleName;
  std::string ownerName;
  const std::unordered_map<std::string, GlobalInfo>* globals = nullptr;
  std::unordered_set<std::string> locals;
  std::unordered_map<std::string, std::string> localTypes;
};

bool isFunction(nir::DefinitionKind kind) {
  return kind == nir::DefinitionKind::FunctionDecl ||
         kind == nir::DefinitionKind::FunctionDef;
}

bool isClassLikeDefinition(nir::DefinitionKind kind) {
  return kind == nir::DefinitionKind::Module || kind == nir::DefinitionKind::Class ||
         kind == nir::DefinitionKind::Trait;
}

bool isDispatchTypeDefinition(nir::DefinitionKind kind) {
  return kind == nir::DefinitionKind::Class || kind == nir::DefinitionKind::Trait;
}

bool isBoxablePrimitiveType(const std::string& type) {
  return type == "Unit" || type == "Boolean" || type == "Byte" || type == "Short" ||
         type == "Int" || type == "Long" || type == "Float" || type == "Double" ||
         type == "Char" || type == "Symbol" || type == "String";
}

bool isRuntimeReferenceArrayCopyOperation(std::string_view name) {
  const std::string prefix =
      std::string(support::StdNames::RuntimeReferenceArrayCopy) + ".";
  return name.starts_with(prefix);
}

bool isRuntimeArrayOperation(std::string_view name) {
  const std::string referenceLengthPrefix =
      std::string(support::StdNames::RuntimeReferenceArrayLength) + ".";
  const std::string referenceApplyPrefix =
      std::string(support::StdNames::RuntimeReferenceArrayApply) + ".";
  const std::string referenceUpdatePrefix =
      std::string(support::StdNames::RuntimeReferenceArrayUpdate) + ".";
  const std::string referenceClonePrefix =
      std::string(support::StdNames::RuntimeReferenceArrayClone) + ".";
  const std::string copyPrefix = std::string(support::StdNames::RuntimeArrayCopy) + ".";
  const std::string concatPrefix =
      std::string(support::StdNames::RuntimeArrayConcat) + ".";
  return name == support::StdNames::RuntimeArrayLength ||
         name == support::StdNames::RuntimeArrayApply ||
         name == support::StdNames::RuntimeArrayUpdate ||
         name == support::StdNames::RuntimeArrayClone ||
         name == support::StdNames::RuntimeIntArrayLength ||
         name == support::StdNames::RuntimeIntArrayApply ||
         name == support::StdNames::RuntimeIntArrayUpdate ||
         name == support::StdNames::RuntimeIntArrayClone ||
         name == support::StdNames::RuntimeByteArrayLength ||
         name == support::StdNames::RuntimeByteArrayApply ||
         name == support::StdNames::RuntimeByteArrayUpdate ||
         name == support::StdNames::RuntimeByteArrayClone ||
         name == support::StdNames::RuntimeShortArrayLength ||
         name == support::StdNames::RuntimeShortArrayApply ||
         name == support::StdNames::RuntimeShortArrayUpdate ||
         name == support::StdNames::RuntimeShortArrayClone ||
         name == support::StdNames::RuntimeBooleanArrayLength ||
         name == support::StdNames::RuntimeBooleanArrayApply ||
         name == support::StdNames::RuntimeBooleanArrayUpdate ||
         name == support::StdNames::RuntimeBooleanArrayClone ||
         name == support::StdNames::RuntimeLongArrayLength ||
         name == support::StdNames::RuntimeLongArrayApply ||
         name == support::StdNames::RuntimeLongArrayUpdate ||
         name == support::StdNames::RuntimeLongArrayClone ||
         name == support::StdNames::RuntimeDoubleArrayLength ||
         name == support::StdNames::RuntimeDoubleArrayApply ||
         name == support::StdNames::RuntimeDoubleArrayUpdate ||
         name == support::StdNames::RuntimeDoubleArrayClone ||
         name == support::StdNames::RuntimeFloatArrayLength ||
         name == support::StdNames::RuntimeFloatArrayApply ||
         name == support::StdNames::RuntimeFloatArrayUpdate ||
         name == support::StdNames::RuntimeFloatArrayClone ||
         name == support::StdNames::RuntimeCharArrayLength ||
         name == support::StdNames::RuntimeCharArrayApply ||
         name == support::StdNames::RuntimeCharArrayUpdate ||
         name == support::StdNames::RuntimeCharArrayClone ||
         name.starts_with(referenceLengthPrefix) ||
         name.starts_with(referenceApplyPrefix) ||
         name.starts_with(referenceUpdatePrefix) ||
         name.starts_with(referenceClonePrefix) || name.starts_with(copyPrefix) ||
         name.starts_with(concatPrefix) || isRuntimeReferenceArrayCopyOperation(name);
}

bool isRuntimeArrayAllocationOperation(std::string_view name) {
  const std::string referenceAllocPrefix =
      std::string(support::StdNames::RuntimeReferenceArrayAlloc) + ".";
  const std::string ofDimPrefix =
      std::string(support::StdNames::RuntimeArrayOfDim) + ".";
  const std::string fillPrefix = std::string(support::StdNames::RuntimeArrayFill) + ".";
  const std::string concatPrefix =
      std::string(support::StdNames::RuntimeArrayConcat) + ".";
  return name == support::StdNames::RuntimeArrayAlloc ||
         name == support::StdNames::RuntimeIntArrayAlloc ||
         name == support::StdNames::RuntimeByteArrayAlloc ||
         name == support::StdNames::RuntimeShortArrayAlloc ||
         name == support::StdNames::RuntimeBooleanArrayAlloc ||
         name == support::StdNames::RuntimeLongArrayAlloc ||
         name == support::StdNames::RuntimeDoubleArrayAlloc ||
         name == support::StdNames::RuntimeFloatArrayAlloc ||
         name == support::StdNames::RuntimeCharArrayAlloc ||
         name == support::StdNames::RuntimeArrayRange ||
         name.starts_with(referenceAllocPrefix) || name.starts_with(ofDimPrefix) ||
         name.starts_with(fillPrefix) || name.starts_with(concatPrefix);
}

bool isRuntimeAssertOperation(std::string_view name) {
  return name == support::StdNames::RuntimeAssert;
}

bool isRuntimeAssumeOperation(std::string_view name) {
  return name == support::StdNames::RuntimeAssume;
}

bool isRuntimeRequireOperation(std::string_view name) {
  return name == support::StdNames::RuntimeRequire;
}

bool isRuntimeIllegalArgumentOperation(std::string_view name) {
  const std::string concatPrefix =
      std::string(support::StdNames::RuntimeArrayConcat) + ".";
  return isRuntimeRequireOperation(name) ||
         name == support::StdNames::RuntimeArrayRange || name.starts_with(concatPrefix);
}

bool isRuntimeNullReceiverOperation(std::string_view name) {
  return name == support::StdNames::RuntimeStringLength ||
         name == support::StdNames::RuntimeStringToString ||
         name == support::StdNames::RuntimeStringEquals ||
         name == support::StdNames::RuntimeStringHashCode ||
         name == support::StdNames::RuntimeAnyReceiverToString ||
         name == support::StdNames::RuntimeAnyReceiverEquals ||
         name == support::StdNames::RuntimeAnyReceiverHashCode;
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

bool isMainCandidate(const nir::Definition& definition) {
  if (!isFunction(definition.kind)) {
    return false;
  }
  return definition.name == "main" || definition.name.ends_with(".main");
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

std::string signatureReturnType(const std::string& signature) {
  const std::size_t close = signature.find(')');
  if (close == std::string::npos || close + 1 >= signature.size()) {
    return "Unknown";
  }
  return trim(std::string_view(signature).substr(close + 1));
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

bool isSubtypeOf(const std::string& actual, const std::string& expected,
                 const std::unordered_map<std::string, GlobalInfo>& globals) {
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
    auto definition = globals.find(current);
    if (definition == globals.end() ||
        !isClassLikeDefinition(definition->second.definition->kind)) {
      continue;
    }
    for (const std::string& parent :
         nir::metadataParentNames(definition->second.definition->signature)) {
      if (parent == expected) {
        return true;
      }
      worklist.push_back(parent);
    }
  }
  return false;
}

bool isVirtualMethodDefinition(
    const GlobalInfo& global,
    const std::unordered_map<std::string, GlobalInfo>& globals) {
  if (global.definition == nullptr || !isFunction(global.definition->kind)) {
    return false;
  }

  const std::string owner = ownerNameOf(global.definition->name);
  auto ownerDefinition = globals.find(owner);
  if (owner.empty() || ownerDefinition == globals.end() ||
      !isDispatchTypeDefinition(ownerDefinition->second.definition->kind)) {
    return false;
  }

  const std::vector<std::string> parameters =
      signatureParameterTypes(global.definition->signature);
  return !parameters.empty() && parameters.front() == owner;
}

bool virtualSignaturesMatch(
    const nir::Definition& expected, const nir::Definition& candidate,
    const std::unordered_map<std::string, GlobalInfo>& globals) {
  const std::vector<std::string> expectedParameters =
      signatureParameterTypes(expected.signature);
  const std::vector<std::string> candidateParameters =
      signatureParameterTypes(candidate.signature);
  if (expectedParameters.empty() ||
      expectedParameters.size() != candidateParameters.size()) {
    return false;
  }

  const std::string expectedReturn = signatureReturnType(expected.signature);
  const std::string candidateReturn = signatureReturnType(candidate.signature);
  if (expectedReturn != candidateReturn &&
      !isSubtypeOf(candidateReturn, expectedReturn, globals)) {
    return false;
  }

  for (std::size_t i = 1; i < expectedParameters.size(); ++i) {
    if (expectedParameters[i] != candidateParameters[i]) {
      return false;
    }
  }
  return true;
}

void collectVirtualOverrideReferences(
    const std::string& reference,
    const std::unordered_map<std::string, GlobalInfo>& globals,
    std::vector<std::string>& references) {
  auto target = globals.find(reference);
  if (target == globals.end() || !isVirtualMethodDefinition(target->second, globals)) {
    return;
  }

  const std::string targetOwner = ownerNameOf(reference);
  const std::string targetMember = memberNameOf(reference);
  for (const auto& [candidateName, candidate] : globals) {
    if (candidateName == reference || memberNameOf(candidateName) != targetMember ||
        !isVirtualMethodDefinition(candidate, globals)) {
      continue;
    }

    const std::string candidateOwner = ownerNameOf(candidateName);
    if (isSubtypeOf(candidateOwner, targetOwner, globals) &&
        virtualSignaturesMatch(*target->second.definition, *candidate.definition,
                               globals)) {
      references.push_back(candidateName);
    }
  }
}

const GlobalInfo* resolveGlobal(const std::string& name,
                                const ReferenceContext& context) {
  if (context.globals == nullptr) {
    return nullptr;
  }

  auto direct = context.globals->find(name);
  if (direct != context.globals->end()) {
    return &direct->second;
  }
  if (!context.ownerName.empty()) {
    auto ownerLocal = context.globals->find(context.ownerName + "." + name);
    if (ownerLocal != context.globals->end()) {
      return &ownerLocal->second;
    }
  }
  if (!context.moduleName.empty()) {
    auto moduleLocal = context.globals->find(context.moduleName + "." + name);
    if (moduleLocal != context.globals->end()) {
      return &moduleLocal->second;
    }
  }
  return nullptr;
}

const GlobalInfo* resolveValueGlobal(const nir::Value& value,
                                     const ReferenceContext& context);

std::unordered_map<std::string, std::vector<std::string>>
parentMapFor(const std::unordered_map<std::string, GlobalInfo>& globals) {
  std::unordered_map<std::string, std::vector<std::string>> parentMap;
  for (const auto& [name, global] : globals) {
    if (global.definition != nullptr &&
        isClassLikeDefinition(global.definition->kind)) {
      parentMap[name] = nir::metadataParentNames(global.definition->signature);
    }
  }
  return parentMap;
}

const GlobalInfo* resolveMemberGlobal(const std::string& receiverGlobalName,
                                      const std::string& memberName,
                                      const ReferenceContext& context) {
  if (context.globals == nullptr) {
    return nullptr;
  }

  const auto parentMap = parentMapFor(*context.globals);
  for (const std::string& current :
       nir::linearizedTypeNames(receiverGlobalName, parentMap)) {
    if (current.empty() || current == support::StdNames::JavaLangObject) {
      continue;
    }
    auto member = context.globals->find(current + "." + memberName);
    if (member != context.globals->end()) {
      return &member->second;
    }
  }
  return nullptr;
}

void collectClassFieldReferences(const std::string& className,
                                 const ReferenceContext& context,
                                 std::vector<std::string>& references,
                                 std::unordered_set<std::string>& visited) {
  if (context.globals == nullptr) {
    return;
  }
  if (!visited.insert(className).second ||
      className == support::StdNames::JavaLangObject) {
    return;
  }
  auto classDefinition = context.globals->find(className);
  if (classDefinition != context.globals->end() &&
      isClassLikeDefinition(classDefinition->second.definition->kind)) {
    for (const std::string& parent :
         nir::metadataParentNames(classDefinition->second.definition->signature)) {
      collectClassFieldReferences(parent, context, references, visited);
    }
  }
  for (const auto& [name, global] : *context.globals) {
    if (global.definition->kind == nir::DefinitionKind::Field &&
        ownerNameOf(name) == className) {
      references.push_back(name);
    }
  }
}

void collectClassFieldReferences(const std::string& className,
                                 const ReferenceContext& context,
                                 std::vector<std::string>& references) {
  std::unordered_set<std::string> visited;
  collectClassFieldReferences(className, context, references, visited);
}

void collectClassInitializerReference(const std::string& className,
                                      const ReferenceContext& context,
                                      std::vector<std::string>& references) {
  if (context.globals == nullptr) {
    return;
  }
  const std::string initializerName = className + ".$init";
  if (context.globals->contains(initializerName)) {
    references.push_back(initializerName);
  }
}

std::string valueSimpleType(const nir::Value& value, const ReferenceContext& context);

const GlobalInfo* receiverGlobalForSelect(const nir::Value& receiver,
                                          const ReferenceContext& context) {
  if (receiver.kind == nir::ValueKind::New) {
    return resolveGlobal(receiver.text, context);
  }
  if (receiver.kind == nir::ValueKind::Super) {
    return resolveGlobal(receiver.type.empty() ? receiver.text : receiver.type,
                         context);
  }
  if (receiver.kind == nir::ValueKind::Local &&
      context.locals.contains(receiver.text)) {
    auto localType = context.localTypes.find(receiver.text);
    if (localType == context.localTypes.end()) {
      return nullptr;
    }
    const GlobalInfo* receiverType = resolveGlobal(localType->second, context);
    if (receiverType != nullptr &&
        isClassLikeDefinition(receiverType->definition->kind)) {
      return receiverType;
    }
    return nullptr;
  }
  const std::string receiverTypeName = valueSimpleType(receiver, context);
  if (const GlobalInfo* receiverType = resolveGlobal(receiverTypeName, context);
      receiverType != nullptr &&
      isClassLikeDefinition(receiverType->definition->kind)) {
    return receiverType;
  }
  return resolveValueGlobal(receiver, context);
}

std::string valueSimpleType(const nir::Value& value, const ReferenceContext& context) {
  switch (value.kind) {
  case nir::ValueKind::Unit:
    return "Unit";
  case nir::ValueKind::Literal:
    return value.type.empty() ? "Unknown" : value.type;
  case nir::ValueKind::New:
    if (const GlobalInfo* global = resolveValueGlobal(value, context)) {
      return global->definition->name;
    }
    return value.type.empty() ? value.text : value.type;
  case nir::ValueKind::SizeOf:
    return "Int";
  case nir::ValueKind::ZoneScoped:
    if (value.operands.size() == 1) {
      return valueSimpleType(value.operands.front(), context);
    }
    return "Unknown";
  case nir::ValueKind::Box:
    return "Object";
  case nir::ValueKind::Unbox:
    return value.text.empty() ? value.type : value.text;
  case nir::ValueKind::IsInstanceOf:
    return "Boolean";
  case nir::ValueKind::AsInstanceOf:
    return value.text.empty() ? value.type : value.text;
  case nir::ValueKind::Super:
    if (const GlobalInfo* global = resolveValueGlobal(value, context)) {
      return global->definition->name;
    }
    return value.type.empty() ? value.text : value.type;
  case nir::ValueKind::Local: {
    auto localType = context.localTypes.find(value.text);
    if (localType != context.localTypes.end()) {
      return localType->second;
    }
    if (const GlobalInfo* global = resolveValueGlobal(value, context)) {
      if (isFunction(global->definition->kind)) {
        return signatureReturnType(global->definition->signature);
      }
      return global->definition->name;
    }
    return "Unknown";
  }
  case nir::ValueKind::Select:
  case nir::ValueKind::Call: {
    const nir::Value* callee = &value;
    if (value.kind == nir::ValueKind::Call) {
      if (value.operands.empty()) {
        return "Unknown";
      }
      callee = &value.operands.front();
    }
    if (const GlobalInfo* global = resolveValueGlobal(*callee, context);
        global != nullptr && isFunction(global->definition->kind)) {
      return signatureReturnType(global->definition->signature);
    }
    return "Unknown";
  }
  case nir::ValueKind::Unary:
    if (value.text == "!") {
      return "Boolean";
    }
    if (!value.operands.empty()) {
      return valueSimpleType(value.operands.front(), context);
    }
    return "Unknown";
  case nir::ValueKind::Binary:
    if (value.text == "==" || value.text == "!=" || value.text == "<" ||
        value.text == ">" || value.text == "<=" || value.text == ">=" ||
        value.text == "&&" || value.text == "||") {
      return "Boolean";
    }
    if (value.text == "+" && value.operands.size() == 2) {
      const std::string lhs = valueSimpleType(value.operands[0], context);
      const std::string rhs = valueSimpleType(value.operands[1], context);
      if (lhs == "String" || rhs == "String") {
        return "String";
      }
    }
    if (!value.operands.empty()) {
      return valueSimpleType(value.operands.front(), context);
    }
    return "Unknown";
  case nir::ValueKind::Assign:
    return "Unit";
  case nir::ValueKind::Throw:
    return "Nothing";
  case nir::ValueKind::Try:
  case nir::ValueKind::Catch:
    return value.type.empty() ? "Unknown" : value.type;
  case nir::ValueKind::Finally:
    return "Unit";
  case nir::ValueKind::Block: {
    if (value.operands.empty()) {
      return "Unit";
    }
    ReferenceContext blockContext = context;
    std::string resultType = "Unit";
    for (const nir::Value& operand : value.operands) {
      if ((operand.kind == nir::ValueKind::LocalLet ||
           operand.kind == nir::ValueKind::LocalVar) &&
          operand.operands.size() == 1) {
        const std::string initializerType =
            valueSimpleType(operand.operands.front(), blockContext);
        blockContext.locals.insert(operand.text);
        blockContext.localTypes[operand.text] =
            operand.type.empty() ? initializerType : operand.type;
        resultType = "Unit";
      } else {
        resultType = valueSimpleType(operand, blockContext);
      }
    }
    return resultType;
  }
  case nir::ValueKind::LocalLet:
  case nir::ValueKind::LocalVar:
    return "Unit";
  case nir::ValueKind::If:
    if (value.operands.size() == 3) {
      const std::string thenType = valueSimpleType(value.operands[1], context);
      const std::string elseType = valueSimpleType(value.operands[2], context);
      if (thenType == "Nothing") {
        return elseType;
      }
      if (elseType == "Nothing" || thenType == elseType) {
        return thenType;
      }
    }
    return "Unknown";
  case nir::ValueKind::While:
    return "Unit";
  case nir::ValueKind::Unknown:
    return value.type.empty() ? "Unknown" : value.type;
  }
  return "Unknown";
}

void collectExceptionDescriptionReference(const nir::Value& exception,
                                          const ReferenceContext& context,
                                          std::vector<std::string>& references) {
  const std::string exceptionType = valueSimpleType(exception, context);
  const GlobalInfo* exceptionClass = resolveGlobal(exceptionType, context);
  bool foundDescription = false;
  if (exceptionClass != nullptr) {
    if (const GlobalInfo* toString =
            resolveMemberGlobal(exceptionClass->definition->name,
                                std::string(support::StdNames::ToString), context)) {
      references.push_back(toString->definition->name);
      foundDescription = true;
    }
  }
  if (foundDescription || context.globals == nullptr) {
    return;
  }
  for (const auto& [name, candidate] : *context.globals) {
    if (memberNameOf(name) != support::StdNames::ToString ||
        !isVirtualMethodDefinition(candidate, *context.globals)) {
      continue;
    }
    if (exceptionClass == nullptr ||
        isSubtypeOf(ownerNameOf(name), exceptionClass->definition->name,
                    *context.globals)) {
      references.push_back(name);
    }
  }
}

void collectClassCastFailureReference(const ReferenceContext& context,
                                      std::vector<std::string>& references) {
  if (context.globals == nullptr) {
    return;
  }
  const std::string classCastName(support::StdNames::JavaLangClassCastException);
  if (context.globals->contains(classCastName)) {
    references.push_back(classCastName);
  }
}

void collectNullThrowFailureReference(const ReferenceContext& context,
                                      std::vector<std::string>& references) {
  if (context.globals == nullptr) {
    return;
  }
  const std::string nullPointerName(support::StdNames::JavaLangNullPointerException);
  if (context.globals->contains(nullPointerName)) {
    references.push_back(nullPointerName);
  }
}

void collectArithmeticFailureReference(const nir::Value& value,
                                       const ReferenceContext& context,
                                       std::vector<std::string>& references) {
  if (value.kind != nir::ValueKind::Binary || value.operands.size() != 2 ||
      (value.text != "/" && value.text != "%") || context.globals == nullptr) {
    return;
  }
  const std::string operandType = valueSimpleType(value.operands.front(), context);
  if (operandType != "Int" && operandType != "Long") {
    return;
  }
  const std::string arithmeticName(support::StdNames::JavaLangArithmeticException);
  if (context.globals->contains(arithmeticName)) {
    references.push_back(arithmeticName);
  }
}

bool isInstanceMemberSelect(const nir::Value& value, const ReferenceContext& context) {
  if (value.kind != nir::ValueKind::Select || value.operands.size() != 1 ||
      value.operands.front().kind == nir::ValueKind::Super ||
      context.globals == nullptr) {
    return false;
  }
  const GlobalInfo* member = resolveValueGlobal(value, context);
  if (member == nullptr) {
    return false;
  }
  auto owner = context.globals->find(ownerNameOf(member->definition->name));
  return owner != context.globals->end() && owner->second.definition != nullptr &&
         owner->second.definition->kind != nir::DefinitionKind::Module;
}

void collectNullReceiverFailureReference(const nir::Value& value,
                                         const ReferenceContext& context,
                                         std::vector<std::string>& references) {
  if (!isInstanceMemberSelect(value, context) || context.globals == nullptr) {
    return;
  }
  const std::string nullPointerName(support::StdNames::JavaLangNullPointerException);
  if (context.globals->contains(nullPointerName)) {
    references.push_back(nullPointerName);
  }
}

void collectValueReferences(const nir::Value& value, ReferenceContext& context,
                            std::vector<std::string>& references,
                            std::vector<std::string>& unresolved) {
  switch (value.kind) {
  case nir::ValueKind::Unit:
  case nir::ValueKind::Literal:
  case nir::ValueKind::Super:
  case nir::ValueKind::Unknown:
    return;
  case nir::ValueKind::New:
    if (const GlobalInfo* global = resolveValueGlobal(value, context)) {
      references.push_back(global->definition->name);
      collectClassFieldReferences(global->definition->name, context, references);
      collectClassInitializerReference(global->definition->name, context, references);
    }
    for (const nir::Value& operand : value.operands) {
      collectValueReferences(operand, context, references, unresolved);
    }
    return;
  case nir::ValueKind::SizeOf:
    if (const GlobalInfo* global = resolveGlobal(value.text, context)) {
      references.push_back(global->definition->name);
    } else if (value.text != "Unit" && value.text != "Boolean" &&
               value.text != "Byte" && value.text != "Short" && value.text != "Int" &&
               value.text != "Long" && value.text != "Float" &&
               value.text != "Double" && value.text != "Char") {
      unresolved.push_back(value.text);
    }
    return;
  case nir::ValueKind::Local:
    if (context.locals.contains(value.text)) {
      return;
    }
    if (const GlobalInfo* global = resolveGlobal(value.text, context)) {
      references.push_back(global->definition->name);
      return;
    }
    unresolved.push_back(value.text);
    return;
  case nir::ValueKind::Select:
    collectNullReceiverFailureReference(value, context, references);
    if (const GlobalInfo* global = resolveValueGlobal(value, context)) {
      references.push_back(global->definition->name);
    }
    for (const nir::Value& operand : value.operands) {
      collectValueReferences(operand, context, references, unresolved);
    }
    return;
  case nir::ValueKind::Call:
    if (!value.operands.empty()) {
      collectNullReceiverFailureReference(value.operands.front(), context, references);
      const GlobalInfo* callee = resolveValueGlobal(value.operands.front(), context);
      if (callee != nullptr) {
        references.push_back(callee->definition->name);
      } else {
        collectValueReferences(value.operands.front(), context, references, unresolved);
      }
      if (callee != nullptr && value.operands.size() >= 2 &&
          memberNameOf(callee->definition->name) == support::StdNames::InitCause) {
        collectExceptionDescriptionReference(value.operands[1], context, references);
      }
      if (callee != nullptr && value.operands.size() >= 2 &&
          callee->definition->name == support::StdNames::RuntimePrintStackTrace) {
        collectExceptionDescriptionReference(value.operands[1], context, references);
      }
      for (std::size_t i = 1; i < value.operands.size(); ++i) {
        collectValueReferences(value.operands[i], context, references, unresolved);
      }
    }
    return;
  case nir::ValueKind::Block: {
    const std::unordered_set<std::string> savedLocals = context.locals;
    const std::unordered_map<std::string, std::string> savedLocalTypes =
        context.localTypes;
    for (const nir::Value& operand : value.operands) {
      if ((operand.kind == nir::ValueKind::LocalLet ||
           operand.kind == nir::ValueKind::LocalVar) &&
          operand.operands.size() == 1) {
        collectValueReferences(operand.operands.front(), context, references,
                               unresolved);
        const std::string initializerType =
            valueSimpleType(operand.operands.front(), context);
        context.locals.insert(operand.text);
        context.localTypes[operand.text] =
            operand.type.empty() ? initializerType : operand.type;
      } else {
        collectValueReferences(operand, context, references, unresolved);
      }
    }
    context.locals = savedLocals;
    context.localTypes = savedLocalTypes;
    return;
  }
  case nir::ValueKind::LocalLet:
  case nir::ValueKind::LocalVar:
    if (!value.operands.empty()) {
      collectValueReferences(value.operands.front(), context, references, unresolved);
      const std::string initializerType =
          valueSimpleType(value.operands.front(), context);
      context.locals.insert(value.text);
      context.localTypes[value.text] =
          value.type.empty() ? initializerType : value.type;
    }
    return;
  case nir::ValueKind::Catch: {
    if (value.operands.size() != 2 ||
        value.operands.front().kind != nir::ValueKind::Local) {
      for (const nir::Value& operand : value.operands) {
        collectValueReferences(operand, context, references, unresolved);
      }
      return;
    }
    const nir::Value& binding = value.operands.front();
    if (binding.type != "Object") {
      if (const GlobalInfo* type = resolveGlobal(binding.type, context)) {
        references.push_back(type->definition->name);
      } else {
        unresolved.push_back(binding.type);
      }
    }
    const std::unordered_set<std::string> savedLocals = context.locals;
    const std::unordered_map<std::string, std::string> savedLocalTypes =
        context.localTypes;
    context.locals.insert(binding.text);
    context.localTypes[binding.text] = binding.type;
    collectValueReferences(value.operands.back(), context, references, unresolved);
    context.locals = savedLocals;
    context.localTypes = savedLocalTypes;
    return;
  }
  case nir::ValueKind::Unary:
  case nir::ValueKind::Binary:
  case nir::ValueKind::Try:
  case nir::ValueKind::Finally:
  case nir::ValueKind::ZoneScoped:
  case nir::ValueKind::Box:
  case nir::ValueKind::If:
  case nir::ValueKind::While:
    collectArithmeticFailureReference(value, context, references);
    for (const nir::Value& operand : value.operands) {
      collectValueReferences(operand, context, references, unresolved);
    }
    return;
  case nir::ValueKind::Unbox:
    collectClassCastFailureReference(context, references);
    for (const nir::Value& operand : value.operands) {
      collectValueReferences(operand, context, references, unresolved);
    }
    return;
  case nir::ValueKind::Throw:
    if (value.operands.size() == 1) {
      collectNullThrowFailureReference(context, references);
      collectExceptionDescriptionReference(value.operands.front(), context, references);
      collectValueReferences(value.operands.front(), context, references, unresolved);
    }
    return;
  case nir::ValueKind::IsInstanceOf:
    if (!isBoxablePrimitiveType(value.text)) {
      if (const GlobalInfo* target = resolveGlobal(value.text, context)) {
        references.push_back(target->definition->name);
      } else {
        unresolved.push_back(value.text);
      }
    }
    for (const nir::Value& operand : value.operands) {
      collectValueReferences(operand, context, references, unresolved);
    }
    return;
  case nir::ValueKind::AsInstanceOf:
    collectClassCastFailureReference(context, references);
    if (const GlobalInfo* target = resolveGlobal(value.text, context)) {
      references.push_back(target->definition->name);
    } else {
      unresolved.push_back(value.text);
    }
    for (const nir::Value& operand : value.operands) {
      collectValueReferences(operand, context, references, unresolved);
    }
    return;
  case nir::ValueKind::Assign:
    if (!value.operands.empty()) {
      collectNullReceiverFailureReference(value.operands.front(), context, references);
      if (const GlobalInfo* target =
              resolveValueGlobal(value.operands.front(), context)) {
        references.push_back(target->definition->name);
        const std::string setterName = target->definition->name + "_$eq";
        if (context.globals != nullptr && context.globals->contains(setterName)) {
          references.push_back(setterName);
        }
      } else {
        collectValueReferences(value.operands.front(), context, references, unresolved);
      }
      for (std::size_t i = 1; i < value.operands.size(); ++i) {
        collectValueReferences(value.operands[i], context, references, unresolved);
      }
    }
    return;
  }
}

const GlobalInfo* resolveValueGlobal(const nir::Value& value,
                                     const ReferenceContext& context) {
  if (value.kind == nir::ValueKind::Local && !context.locals.contains(value.text)) {
    return resolveGlobal(value.text, context);
  }
  if (value.kind == nir::ValueKind::New) {
    return resolveGlobal(value.text, context);
  }
  if (value.kind == nir::ValueKind::Super) {
    return resolveGlobal(value.type.empty() ? value.text : value.type, context);
  }
  if (value.kind == nir::ValueKind::Select && value.operands.size() == 1 &&
      !value.text.empty()) {
    const GlobalInfo* receiver =
        receiverGlobalForSelect(value.operands.front(), context);
    if (receiver == nullptr) {
      return nullptr;
    }
    return resolveMemberGlobal(receiver->definition->name, value.text, context);
  }
  return nullptr;
}

std::vector<std::string>
collectDefinitionReferences(const nir::Definition& definition,
                            const std::string& moduleName,
                            const std::unordered_map<std::string, GlobalInfo>& globals,
                            std::vector<std::string>& unresolved) {
  std::vector<std::string> references;
  if (isClassLikeDefinition(definition.kind)) {
    for (const std::string& parent : nir::metadataParentNames(definition.signature)) {
      if (parent == support::StdNames::JavaLangObject) {
        continue;
      }
      if (globals.contains(parent)) {
        references.push_back(parent);
      }
      for (const auto& [name, global] : globals) {
        if (ownerNameOf(name) == parent && isFunction(global.definition->kind)) {
          references.push_back(name);
        }
      }
    }
    ReferenceContext context;
    context.moduleName = moduleName;
    context.ownerName = definition.name;
    context.globals = &globals;
    collectClassFieldReferences(definition.name, context, references);
    return references;
  }
  if (definition.kind == nir::DefinitionKind::Field &&
      globals.contains(definition.signature)) {
    references.push_back(definition.signature);
  }
  if (isFunction(definition.kind) && (isRuntimeAssertOperation(definition.name) ||
                                      isRuntimeAssumeOperation(definition.name))) {
    const std::string assertionErrorName(support::StdNames::JavaLangAssertionError);
    if (globals.contains(assertionErrorName)) {
      references.push_back(assertionErrorName);
    }
  }
  if (isFunction(definition.kind) &&
      isRuntimeIllegalArgumentOperation(definition.name)) {
    const std::string illegalArgumentName(
        support::StdNames::JavaLangIllegalArgumentException);
    if (globals.contains(illegalArgumentName)) {
      references.push_back(illegalArgumentName);
    }
  }
  if (isFunction(definition.kind) && isRuntimeArrayOperation(definition.name)) {
    const std::string nullPointerName(support::StdNames::JavaLangNullPointerException);
    const std::string arrayBoundsName(
        support::StdNames::JavaLangArrayIndexOutOfBoundsException);
    if (globals.contains(nullPointerName)) {
      references.push_back(nullPointerName);
    }
    if (globals.contains(arrayBoundsName)) {
      references.push_back(arrayBoundsName);
    }
  }
  if (isFunction(definition.kind) &&
      isRuntimeReferenceArrayCopyOperation(definition.name)) {
    const std::string arrayStoreName(support::StdNames::JavaLangArrayStoreException);
    if (globals.contains(arrayStoreName)) {
      references.push_back(arrayStoreName);
    }
  }
  if (isFunction(definition.kind) &&
      isRuntimeArrayAllocationOperation(definition.name)) {
    const std::string negativeSizeName(
        support::StdNames::JavaLangNegativeArraySizeException);
    if (globals.contains(negativeSizeName)) {
      references.push_back(negativeSizeName);
    }
  }
  if (isFunction(definition.kind) && isRuntimeNullReceiverOperation(definition.name)) {
    const std::string nullPointerName(support::StdNames::JavaLangNullPointerException);
    if (globals.contains(nullPointerName)) {
      references.push_back(nullPointerName);
    }
  }
  if (definition.kind != nir::DefinitionKind::FunctionDef &&
      !(definition.kind == nir::DefinitionKind::Field && !definition.body.empty())) {
    return references;
  }

  ReferenceContext context;
  context.moduleName = moduleName;
  context.ownerName = ownerNameOf(definition.name);
  context.globals = &globals;

  auto owner = globals.find(context.ownerName);
  if (owner != globals.end() &&
      owner->second.definition->kind == nir::DefinitionKind::Module) {
    references.push_back(owner->second.definition->name);
    collectClassFieldReferences(context.ownerName, context, references);
    collectClassInitializerReference(context.ownerName, context, references);
  }

  for (const nir::Instruction& instruction : definition.body.instructions) {
    if (instruction.kind == nir::InstructionKind::Param) {
      context.locals.insert(instruction.name);
      context.localTypes[instruction.name] = instruction.type;
      continue;
    }
    if (instruction.kind == nir::InstructionKind::Let ||
        instruction.kind == nir::InstructionKind::Var) {
      collectValueReferences(instruction.value, context, references, unresolved);
      context.locals.insert(instruction.name);
      context.localTypes[instruction.name] =
          instruction.type.empty() ? valueSimpleType(instruction.value, context)
                                   : instruction.type;
      continue;
    }
    if (instruction.kind == nir::InstructionKind::Throw) {
      collectNullThrowFailureReference(context, references);
      collectExceptionDescriptionReference(instruction.value, context, references);
    }
    collectValueReferences(instruction.value, context, references, unresolved);
  }
  return references;
}

} // namespace

LinkResult Linker::link(std::vector<nir::Module> modules,
                        support::DiagnosticEngine& diagnostics) const {
  LinkResult result;
  if (modules.empty()) {
    diagnostics.error(support::SourceSpan::none(), "linker: no NIR modules");
    return result;
  }

  result.program.modules = std::move(modules);

  std::unordered_map<std::string, GlobalInfo> globals;
  for (const nir::Module& module : result.program.modules) {
    for (const nir::Definition& definition : module.definitions) {
      if (definition.name.empty()) {
        continue;
      }
      if (globals.contains(definition.name)) {
        diagnostics.error(support::SourceSpan::none(),
                          "linker: duplicate global: " + definition.name);
        continue;
      }
      globals[definition.name] = GlobalInfo{&definition, module.name};
    }
  }

  if (globals.contains(RuntimeMain)) {
    result.program.roots.push_back(RuntimeMain);
  } else {
    for (const auto& [name, global] : globals) {
      if (isMainCandidate(*global.definition)) {
        result.program.roots.push_back(name);
      }
    }
  }

  if (result.program.roots.empty()) {
    diagnostics.error(support::SourceSpan::none(),
                      "linker: no reachable main entry point");
    return result;
  }

  std::deque<std::string> worklist;
  std::unordered_set<std::string> reachable;
  for (const std::string& root : result.program.roots) {
    if (!globals.contains(root)) {
      diagnostics.error(support::SourceSpan::none(),
                        "linker: unresolved root: " + root);
      continue;
    }
    worklist.push_back(root);
  }

  while (!worklist.empty()) {
    std::string current = std::move(worklist.front());
    worklist.pop_front();
    if (!reachable.insert(current).second) {
      continue;
    }

    auto found = globals.find(current);
    if (found == globals.end()) {
      diagnostics.error(support::SourceSpan::none(),
                        "linker: unresolved global: " + current);
      continue;
    }

    std::vector<std::string> unresolved;
    std::vector<std::string> references = collectDefinitionReferences(
        *found->second.definition, found->second.moduleName, globals, unresolved);
    std::vector<std::string> virtualReferences;
    for (const std::string& reference : references) {
      collectVirtualOverrideReferences(reference, globals, virtualReferences);
    }
    references.insert(references.end(), virtualReferences.begin(),
                      virtualReferences.end());
    for (const std::string& unresolvedName : unresolved) {
      diagnostics.error(support::SourceSpan::none(),
                        "linker: unresolved reference from " + current + ": " +
                            unresolvedName);
    }
    for (const std::string& reference : references) {
      if (!reachable.contains(reference)) {
        worklist.push_back(reference);
      }
    }
  }

  for (const std::string& root : result.program.roots) {
    if (!reachable.contains(root)) {
      diagnostics.error(support::SourceSpan::none(),
                        "linker: root was not reachable: " + root);
    }
  }

  result.program.reachableGlobals.assign(reachable.begin(), reachable.end());
  result.ok = !diagnostics.hasErrors();
  return result;
}

} // namespace scalanative::tools::linker
