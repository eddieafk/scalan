#include "scalanative/nscplugin/NirEmitter.h"

#include "scalanative/frontend/Ast.h"
#include "scalanative/nir/Builder.h"
#include "scalanative/support/StdNames.h"

#include <algorithm>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scalanative::nscplugin {

namespace {

struct ValueContext {
  std::string packageName;
  std::unordered_map<std::string, std::string> importAliases;
  std::unordered_set<std::string> implicitThisMembers;
  std::unordered_set<std::string> localNames;
  std::vector<support::SourceSpan> lexicalScopes;
  const std::vector<frontend::TypedDeclaration>* declarations = nullptr;
  const std::vector<frontend::TypedExpressionInfo>* expressionTypes = nullptr;
  const std::vector<frontend::TypedContextApplication>* contextApplications = nullptr;
  std::unordered_set<std::string>* referenceArrayElementTypes = nullptr;
  std::map<std::string, std::string>* runtimeArrayDeclarations = nullptr;
  std::vector<std::string> superTypes;
  std::string superType;
  std::string currentOwner;
  bool stackableSuper = false;
  bool hasImplicitReceiver = false;
};

struct MaterializedTraitValue {
  const frontend::TypedDeclaration* owner = nullptr;
  const frontend::TypedDeclaration* member = nullptr;
  bool effective = false;
};

bool isZoneScopedCall(const frontend::AstExpression& expression) {
  using frontend::AstExpressionKind;
  if (expression.kind != AstExpressionKind::Call || expression.children.empty()) {
    return false;
  }
  const frontend::AstExpression& callee = expression.children.front();
  return callee.kind == AstExpressionKind::Select && callee.children.size() == 1 &&
         callee.text == support::StdNames::ZoneScoped &&
         callee.children.front().kind == AstExpressionKind::Identifier &&
         callee.children.front().text == support::StdNames::Zone;
}

bool isZoneAllocBytesCall(const frontend::AstExpression& expression) {
  using frontend::AstExpressionKind;
  if (expression.kind != AstExpressionKind::Call || expression.children.empty()) {
    return false;
  }
  const frontend::AstExpression& callee = expression.children.front();
  return callee.kind == AstExpressionKind::Select && callee.children.size() == 1 &&
         callee.text == support::StdNames::ZoneAllocBytes &&
         callee.children.front().kind == AstExpressionKind::Identifier &&
         callee.children.front().text == support::StdNames::Zone;
}

bool isByteBufferWrapCall(const frontend::AstExpression& expression) {
  using frontend::AstExpressionKind;
  if (expression.kind != AstExpressionKind::Call || expression.children.empty()) {
    return false;
  }
  const frontend::AstExpression& callee = expression.children.front();
  return callee.kind == AstExpressionKind::Select && callee.children.size() == 1 &&
         callee.text == support::StdNames::ByteBufferWrap &&
         callee.children.front().kind == AstExpressionKind::Identifier &&
         callee.children.front().text == support::StdNames::ByteBuffer;
}

std::string_view nativeBytesOperation(const frontend::AstExpression& expression) {
  using frontend::AstExpressionKind;
  if (expression.kind != AstExpressionKind::Call || expression.children.empty()) {
    return {};
  }
  const frontend::AstExpression& callee = expression.children.front();
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

std::string nativeBytesRuntimeName(std::string_view operation) {
  if (operation == support::StdNames::NativeBytesGetShortBe) {
    return std::string(support::StdNames::RuntimeNativeBytesGetShortBe);
  }
  if (operation == support::StdNames::NativeBytesGetShortLe) {
    return std::string(support::StdNames::RuntimeNativeBytesGetShortLe);
  }
  if (operation == support::StdNames::NativeBytesPutShortBe) {
    return std::string(support::StdNames::RuntimeNativeBytesPutShortBe);
  }
  return std::string(support::StdNames::RuntimeNativeBytesPutShortLe);
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

std::string parameterName(const std::string& parameter) {
  const std::size_t colon = parameter.find(':');
  std::string name =
      trim(colon == std::string::npos ? std::string_view(parameter)
                                      : std::string_view(parameter).substr(0, colon));
  if (name.rfind("val ", 0) == 0 || name.rfind("var ", 0) == 0) {
    name.erase(0, 4);
  }
  return trim(name);
}

bool isAccessorParameter(const frontend::TypedDeclaration& declaration,
                         const std::string& name) {
  return std::find(declaration.accessorParameters.begin(),
                   declaration.accessorParameters.end(),
                   name) != declaration.accessorParameters.end();
}

std::string storedParameterFieldName(const frontend::TypedDeclaration& declaration,
                                     const std::string& parameter) {
  const std::string name = parameterName(parameter);
  return isAccessorParameter(declaration, name) ? name + "$field" : name;
}

std::string rawParameterTypeName(const std::string& parameter) {
  const std::size_t colon = parameter.find(':');
  if (colon == std::string::npos) {
    return "Unknown";
  }
  return trim(std::string_view(parameter).substr(colon + 1));
}

std::string compactTypeName(std::string_view typeName) {
  std::string compact;
  for (char ch : typeName) {
    if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
      compact.push_back(ch);
    }
  }
  return compact;
}

std::string arrayElementTypeName(const std::string& typeName) {
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

bool isStringArrayTypeName(const std::string& typeName) {
  const std::string elementType = arrayElementTypeName(typeName);
  return elementType == "String" || elementType == "java.lang.String";
}

bool isArrayTypeName(const std::string& typeName) {
  return !arrayElementTypeName(typeName).empty();
}

bool isReferenceArrayElementTypeName(const std::string& typeName) {
  return !typeName.empty() && typeName != "String" && typeName != "java.lang.String" &&
         typeName != "Byte" && typeName != "Short" && typeName != "Int" &&
         typeName != "Boolean" && typeName != "Long" && typeName != "Double" &&
         typeName != "Float" && typeName != "Char";
}

std::string referenceArrayRuntimeName(std::string_view runtimeBase,
                                      std::string_view elementType) {
  if (!isArrayTypeName(std::string(elementType))) {
    return std::string(runtimeBase) + "." + std::string(elementType);
  }

  constexpr char hex[] = "0123456789abcdef";
  const std::string compact = compactTypeName(elementType);
  std::string suffix = "nested$";
  for (const unsigned char ch : compact) {
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
  return std::string(runtimeBase) + "." + suffix;
}

std::string arrayOfDimRuntimeName(std::string_view elementType,
                                  std::size_t dimensions) {
  return referenceArrayRuntimeName(support::StdNames::RuntimeArrayOfDim, elementType) +
         "." + std::to_string(dimensions);
}

std::string arrayOfDimSignature(std::size_t dimensions, std::string_view resultType) {
  std::string signature = "(";
  for (std::size_t i = 0; i < dimensions; ++i) {
    if (i != 0) {
      signature.push_back(',');
    }
    signature += "Int";
  }
  signature += ")";
  signature += resultType;
  return signature;
}

std::string arrayCopyRuntimeName(std::string_view elementType) {
  return referenceArrayRuntimeName(support::StdNames::RuntimeArrayCopy, elementType);
}

std::string arrayFillRuntimeName(std::string_view elementType,
                                 std::size_t dimensions = 1) {
  std::string runtime =
      referenceArrayRuntimeName(support::StdNames::RuntimeArrayFill, elementType);
  if (dimensions > 1) {
    runtime += "." + std::to_string(dimensions);
  }
  return runtime;
}

std::string arrayFillSignature(std::string_view elementType, std::size_t dimensions = 1,
                               std::string_view resultType = {}) {
  std::string signature = "(";
  for (std::size_t i = 0; i < dimensions; ++i) {
    if (i != 0) {
      signature.push_back(',');
    }
    signature += "Int";
  }
  signature += ",";
  signature += elementType;
  signature += ")";
  if (resultType.empty()) {
    signature += "Array [ " + std::string(elementType) + " ]";
  } else {
    signature += resultType;
  }
  return signature;
}

std::string arrayConcatRuntimeName(std::string_view elementType,
                                   std::size_t arrayCount) {
  return referenceArrayRuntimeName(support::StdNames::RuntimeArrayConcat, elementType) +
         "." + std::to_string(arrayCount);
}

std::string arrayConcatSignature(std::string_view arrayType, std::size_t arrayCount) {
  std::string signature = "(";
  for (std::size_t i = 0; i < arrayCount; ++i) {
    if (i != 0) {
      signature.push_back(',');
    }
    signature += arrayType;
  }
  signature += ")";
  signature += arrayType;
  return signature;
}

std::string arrayCopyRuntimeName(std::string_view sourceElementType,
                                 std::string_view destinationElementType) {
  if (sourceElementType == destinationElementType) {
    return arrayCopyRuntimeName(sourceElementType);
  }
  return referenceArrayRuntimeName(
      referenceArrayRuntimeName(support::StdNames::RuntimeReferenceArrayCopy,
                                sourceElementType) +
          ".to",
      destinationElementType);
}

std::string arrayCopySignature(std::string_view sourceArrayType,
                               std::string_view destinationArrayType) {
  return "(" + std::string(sourceArrayType) + ",Int," +
         std::string(destinationArrayType) + ",Int,Int)Unit";
}

std::string arrayCopySignature(std::string_view arrayType) {
  return arrayCopySignature(arrayType, arrayType);
}

void recordReferenceArrayElementType(const std::string& elementType,
                                     const ValueContext& context) {
  if (context.referenceArrayElementTypes != nullptr &&
      isReferenceArrayElementTypeName(elementType)) {
    context.referenceArrayElementTypes->insert(elementType);
  }
}

std::string arrayAllocRuntimeName(const std::string& elementType,
                                  const ValueContext& context) {
  if (elementType == "String") {
    return std::string(support::StdNames::RuntimeArrayAlloc);
  }
  if (elementType == "Int") {
    return std::string(support::StdNames::RuntimeIntArrayAlloc);
  }
  if (elementType == "Byte") {
    return std::string(support::StdNames::RuntimeByteArrayAlloc);
  }
  if (elementType == "Short") {
    return std::string(support::StdNames::RuntimeShortArrayAlloc);
  }
  if (elementType == "Boolean") {
    return std::string(support::StdNames::RuntimeBooleanArrayAlloc);
  }
  if (elementType == "Long") {
    return std::string(support::StdNames::RuntimeLongArrayAlloc);
  }
  if (elementType == "Double") {
    return std::string(support::StdNames::RuntimeDoubleArrayAlloc);
  }
  if (elementType == "Float") {
    return std::string(support::StdNames::RuntimeFloatArrayAlloc);
  }
  if (elementType == "Char") {
    return std::string(support::StdNames::RuntimeCharArrayAlloc);
  }
  recordReferenceArrayElementType(elementType, context);
  return referenceArrayRuntimeName(support::StdNames::RuntimeReferenceArrayAlloc,
                                   elementType);
}

std::string arrayLengthRuntimeName(const std::string& elementType,
                                   const ValueContext& context) {
  if (elementType == "String") {
    return std::string(support::StdNames::RuntimeArrayLength);
  }
  if (elementType == "Int") {
    return std::string(support::StdNames::RuntimeIntArrayLength);
  }
  if (elementType == "Byte") {
    return std::string(support::StdNames::RuntimeByteArrayLength);
  }
  if (elementType == "Short") {
    return std::string(support::StdNames::RuntimeShortArrayLength);
  }
  if (elementType == "Boolean") {
    return std::string(support::StdNames::RuntimeBooleanArrayLength);
  }
  if (elementType == "Long") {
    return std::string(support::StdNames::RuntimeLongArrayLength);
  }
  if (elementType == "Double") {
    return std::string(support::StdNames::RuntimeDoubleArrayLength);
  }
  if (elementType == "Float") {
    return std::string(support::StdNames::RuntimeFloatArrayLength);
  }
  if (elementType == "Char") {
    return std::string(support::StdNames::RuntimeCharArrayLength);
  }
  recordReferenceArrayElementType(elementType, context);
  return referenceArrayRuntimeName(support::StdNames::RuntimeReferenceArrayLength,
                                   elementType);
}

std::string arrayApplyRuntimeName(const std::string& elementType,
                                  const ValueContext& context) {
  if (elementType == "String") {
    return std::string(support::StdNames::RuntimeArrayApply);
  }
  if (elementType == "Int") {
    return std::string(support::StdNames::RuntimeIntArrayApply);
  }
  if (elementType == "Byte") {
    return std::string(support::StdNames::RuntimeByteArrayApply);
  }
  if (elementType == "Short") {
    return std::string(support::StdNames::RuntimeShortArrayApply);
  }
  if (elementType == "Boolean") {
    return std::string(support::StdNames::RuntimeBooleanArrayApply);
  }
  if (elementType == "Long") {
    return std::string(support::StdNames::RuntimeLongArrayApply);
  }
  if (elementType == "Double") {
    return std::string(support::StdNames::RuntimeDoubleArrayApply);
  }
  if (elementType == "Float") {
    return std::string(support::StdNames::RuntimeFloatArrayApply);
  }
  if (elementType == "Char") {
    return std::string(support::StdNames::RuntimeCharArrayApply);
  }
  recordReferenceArrayElementType(elementType, context);
  return referenceArrayRuntimeName(support::StdNames::RuntimeReferenceArrayApply,
                                   elementType);
}

std::string arrayUpdateRuntimeName(const std::string& elementType,
                                   const ValueContext& context) {
  if (elementType == "String") {
    return std::string(support::StdNames::RuntimeArrayUpdate);
  }
  if (elementType == "Int") {
    return std::string(support::StdNames::RuntimeIntArrayUpdate);
  }
  if (elementType == "Byte") {
    return std::string(support::StdNames::RuntimeByteArrayUpdate);
  }
  if (elementType == "Short") {
    return std::string(support::StdNames::RuntimeShortArrayUpdate);
  }
  if (elementType == "Boolean") {
    return std::string(support::StdNames::RuntimeBooleanArrayUpdate);
  }
  if (elementType == "Long") {
    return std::string(support::StdNames::RuntimeLongArrayUpdate);
  }
  if (elementType == "Double") {
    return std::string(support::StdNames::RuntimeDoubleArrayUpdate);
  }
  if (elementType == "Float") {
    return std::string(support::StdNames::RuntimeFloatArrayUpdate);
  }
  if (elementType == "Char") {
    return std::string(support::StdNames::RuntimeCharArrayUpdate);
  }
  recordReferenceArrayElementType(elementType, context);
  return referenceArrayRuntimeName(support::StdNames::RuntimeReferenceArrayUpdate,
                                   elementType);
}

std::string arrayCloneRuntimeName(const std::string& elementType,
                                  const ValueContext& context) {
  if (elementType == "String") {
    return std::string(support::StdNames::RuntimeArrayClone);
  }
  if (elementType == "Int") {
    return std::string(support::StdNames::RuntimeIntArrayClone);
  }
  if (elementType == "Byte") {
    return std::string(support::StdNames::RuntimeByteArrayClone);
  }
  if (elementType == "Short") {
    return std::string(support::StdNames::RuntimeShortArrayClone);
  }
  if (elementType == "Boolean") {
    return std::string(support::StdNames::RuntimeBooleanArrayClone);
  }
  if (elementType == "Long") {
    return std::string(support::StdNames::RuntimeLongArrayClone);
  }
  if (elementType == "Double") {
    return std::string(support::StdNames::RuntimeDoubleArrayClone);
  }
  if (elementType == "Float") {
    return std::string(support::StdNames::RuntimeFloatArrayClone);
  }
  if (elementType == "Char") {
    return std::string(support::StdNames::RuntimeCharArrayClone);
  }
  recordReferenceArrayElementType(elementType, context);
  return referenceArrayRuntimeName(support::StdNames::RuntimeReferenceArrayClone,
                                   elementType);
}

bool isBuiltinTypeName(const std::string& name) {
  return name == "Unknown" || name == "Nothing" || name == "Unit" || name == "Byte" ||
         name == "Short" || name == "Int" || name == "Long" || name == "Float" ||
         name == "Double" || name == "Boolean" || name == "String" || name == "Char" ||
         name == "Symbol" || name == "Null" || name == "Object" ||
         isArrayTypeName(name);
}

std::string qualifyTypeName(const std::string& name, const ValueContext& context) {
  auto imported = context.importAliases.find(name);
  if (imported != context.importAliases.end()) {
    return imported->second;
  }
  if (isBuiltinTypeName(name)) {
    return name;
  }
  if (name == "Throwable") {
    return std::string(support::StdNames::JavaLangThrowable);
  }
  if (name == "Error") {
    return std::string(support::StdNames::JavaLangError);
  }
  if (name == "AssertionError") {
    return std::string(support::StdNames::JavaLangAssertionError);
  }
  if (name == "NotImplementedError") {
    return std::string(support::StdNames::ScalaNotImplementedError);
  }
  if (name == "Exception") {
    return std::string(support::StdNames::JavaLangException);
  }
  if (name == "RuntimeException") {
    return std::string(support::StdNames::JavaLangRuntimeException);
  }
  if (name == "ArithmeticException") {
    return std::string(support::StdNames::JavaLangArithmeticException);
  }
  if (name == "StackTraceElement") {
    return std::string(support::StdNames::JavaLangStackTraceElement);
  }
  if (name == "IllegalArgumentException") {
    return std::string(support::StdNames::JavaLangIllegalArgumentException);
  }
  if (name == "IllegalStateException") {
    return std::string(support::StdNames::JavaLangIllegalStateException);
  }
  if (name == "NullPointerException") {
    return std::string(support::StdNames::JavaLangNullPointerException);
  }
  if (name == "ClassCastException") {
    return std::string(support::StdNames::JavaLangClassCastException);
  }
  if (name == "ArrayStoreException") {
    return std::string(support::StdNames::JavaLangArrayStoreException);
  }
  if (name == "IndexOutOfBoundsException") {
    return std::string(support::StdNames::JavaLangIndexOutOfBoundsException);
  }
  if (name == "ArrayIndexOutOfBoundsException") {
    return std::string(support::StdNames::JavaLangArrayIndexOutOfBoundsException);
  }
  if (name == "NegativeArraySizeException") {
    return std::string(support::StdNames::JavaLangNegativeArraySizeException);
  }
  if (name == "BufferUnderflowException") {
    return std::string(support::StdNames::JavaNioBufferUnderflowException);
  }
  if (name == "BufferOverflowException") {
    return std::string(support::StdNames::JavaNioBufferOverflowException);
  }
  if (name == support::StdNames::ByteBuffer ||
      name == support::StdNames::JavaNioByteBuffer) {
    return std::string(support::StdNames::JavaNioByteBuffer);
  }
  if (name.empty() || name.find('.') != std::string::npos ||
      context.packageName.empty()) {
    return name;
  }
  return context.packageName + "." + name;
}

std::string runtimeTypeName(const frontend::TypeInfo& type) {
  return type.runtimeName.empty() ? type.name : type.runtimeName;
}

const frontend::TypeInfo* annotatedTypeFor(const frontend::AstExpression& expression,
                                           const ValueContext& context) {
  if (context.expressionTypes == nullptr || !expression.span.isValid()) {
    return nullptr;
  }
  for (auto info = context.expressionTypes->rbegin();
       info != context.expressionTypes->rend(); ++info) {
    if (info->span.source == expression.span.source &&
        info->span.start == expression.span.start &&
        info->span.length == expression.span.length) {
      return &info->type;
    }
  }
  return nullptr;
}

const frontend::TypedContextApplication*
contextApplicationFor(const frontend::AstExpression& expression,
                      const ValueContext& context) {
  if (context.contextApplications == nullptr || !expression.span.isValid()) {
    return nullptr;
  }
  auto application = std::find_if(
      context.contextApplications->rbegin(), context.contextApplications->rend(),
      [&](const frontend::TypedContextApplication& candidate) {
        return candidate.span.source == expression.span.source &&
               candidate.span.start == expression.span.start &&
               candidate.span.length == expression.span.length;
      });
  return application == context.contextApplications->rend() ? nullptr : &*application;
}

std::string byteBufferRuntimeName(const frontend::AstExpression& expression,
                                  const ValueContext& context) {
  using frontend::AstExpressionKind;
  if (expression.kind != AstExpressionKind::Call || expression.children.empty()) {
    return {};
  }
  const frontend::AstExpression& callee = expression.children.front();
  if (callee.kind != AstExpressionKind::Select || callee.children.size() != 1) {
    return {};
  }
  const frontend::TypeInfo* receiverType =
      annotatedTypeFor(callee.children.front(), context);
  if (receiverType == nullptr ||
      runtimeTypeName(*receiverType) != support::StdNames::JavaNioByteBuffer) {
    return {};
  }
  if (callee.text == support::StdNames::ByteBufferCapacity) {
    return std::string(support::StdNames::RuntimeByteBufferCapacity);
  }
  if (callee.text == support::StdNames::ByteBufferPosition) {
    return std::string(expression.children.size() == 1
                           ? support::StdNames::RuntimeByteBufferPosition
                           : support::StdNames::RuntimeByteBufferSetPosition);
  }
  if (callee.text == support::StdNames::ByteBufferLimit) {
    return std::string(expression.children.size() == 1
                           ? support::StdNames::RuntimeByteBufferLimit
                           : support::StdNames::RuntimeByteBufferSetLimit);
  }
  if (callee.text == support::StdNames::ByteBufferRemaining) {
    return std::string(support::StdNames::RuntimeByteBufferRemaining);
  }
  if (callee.text == support::StdNames::ByteBufferHasRemaining) {
    return std::string(support::StdNames::RuntimeByteBufferHasRemaining);
  }
  if (callee.text == support::StdNames::ByteBufferGet) {
    return std::string(support::StdNames::RuntimeByteBufferGet);
  }
  if (callee.text == support::StdNames::ByteBufferPut) {
    return std::string(support::StdNames::RuntimeByteBufferPut);
  }
  if (callee.text == support::StdNames::ByteBufferClear) {
    return std::string(support::StdNames::RuntimeByteBufferClear);
  }
  if (callee.text == support::StdNames::ByteBufferFlip) {
    return std::string(support::StdNames::RuntimeByteBufferFlip);
  }
  if (callee.text == support::StdNames::ByteBufferRewind) {
    return std::string(support::StdNames::RuntimeByteBufferRewind);
  }
  return {};
}

std::string primitiveTypeName(frontend::SimpleTypeKind kind) {
  switch (kind) {
  case frontend::SimpleTypeKind::Unit:
    return "Unit";
  case frontend::SimpleTypeKind::Boolean:
    return "Boolean";
  case frontend::SimpleTypeKind::Byte:
    return "Byte";
  case frontend::SimpleTypeKind::Short:
    return "Short";
  case frontend::SimpleTypeKind::Int:
    return "Int";
  case frontend::SimpleTypeKind::Long:
    return "Long";
  case frontend::SimpleTypeKind::Float:
    return "Float";
  case frontend::SimpleTypeKind::Double:
    return "Double";
  case frontend::SimpleTypeKind::Char:
    return "Char";
  case frontend::SimpleTypeKind::Symbol:
    return "Symbol";
  default:
    return {};
  }
}

std::string boxedObjectTypeName(frontend::SimpleTypeKind kind) {
  if (kind == frontend::SimpleTypeKind::String) {
    return "String";
  }
  return primitiveTypeName(kind);
}

std::string boxedPrimitiveType(const frontend::TypeInfo& type) {
  if (runtimeTypeName(type) != "Object") {
    return {};
  }
  return boxedObjectTypeName(type.kind);
}

std::string resolveIdentifierName(const std::string& name,
                                  const ValueContext& context) {
  auto imported = context.importAliases.find(name);
  if (imported != context.importAliases.end()) {
    return imported->second;
  }
  return name;
}

bool shouldUseImplicitThis(const std::string& name, const ValueContext& context) {
  return context.hasImplicitReceiver && context.implicitThisMembers.contains(name) &&
         !context.localNames.contains(name);
}

std::string parameterTypeName(const std::string& parameter,
                              const ValueContext& context) {
  const std::string typeName = rawParameterTypeName(parameter);
  return typeName == "Any" ? "Object" : qualifyTypeName(typeName, context);
}

std::string localDeclaredTypeName(const frontend::AstExpression& expression,
                                  const ValueContext& context) {
  if (expression.declaredType.empty()) {
    return {};
  }
  if (expression.declaredType == "Any") {
    return "Object";
  }
  const std::string compact = compactTypeName(expression.declaredType);
  const std::size_t typeArguments = compact.find('[');
  if (typeArguments != std::string::npos && arrayElementTypeName(compact).empty()) {
    return qualifyTypeName(compact.substr(0, typeArguments), context);
  }
  return qualifyTypeName(expression.declaredType, context);
}

std::string metadataParentNames(const frontend::TypedDeclaration& declaration) {
  const std::vector<std::string> parents =
      declaration.parentTypes.empty() && !declaration.declaredType.empty()
          ? std::vector<std::string>{declaration.declaredType}
          : declaration.parentTypes;
  if (parents.empty()) {
    return "@java.lang.Object";
  }
  std::ostringstream out;
  for (std::size_t i = 0; i < parents.size(); ++i) {
    if (i != 0) {
      out << " with ";
    }
    if (!parents[i].empty() && parents[i].front() == '@') {
      out << parents[i];
    } else {
      out << '@' << parents[i];
    }
  }
  return out.str();
}

std::string superTypeName(const frontend::TypedDeclaration& declaration) {
  if (!declaration.parentTypes.empty()) {
    return declaration.parentTypes.back();
  }
  return declaration.declaredType;
}

std::unordered_map<std::string, std::string>
importAliasesFor(const std::vector<frontend::TypedDeclaration>& declarations) {
  std::unordered_map<std::string, std::string> aliases;
  for (const frontend::TypedDeclaration& declaration : declarations) {
    if (declaration.kind != frontend::AstDeclarationKind::Import) {
      continue;
    }
    if (!declaration.importSelectors.empty()) {
      for (const frontend::AstImportSelector& selector : declaration.importSelectors) {
        if (!selector.name.empty() && !selector.alias.empty()) {
          aliases[selector.alias] = declaration.importPath + "." + selector.name;
        }
      }
    } else if (declaration.name == "_") {
      for (const frontend::TypedDeclaration& imported : declaration.members) {
        if (!imported.name.empty() && !imported.symbolName.empty()) {
          aliases[imported.name] = imported.symbolName;
        }
      }
    } else if (!declaration.name.empty() && !declaration.symbolName.empty()) {
      aliases[declaration.name] = declaration.symbolName;
    }
  }
  return aliases;
}

void collectImplicitThisMembers(
    const std::vector<frontend::TypedDeclaration>& declarations,
    const std::string& owner, std::unordered_set<std::string>& members,
    bool includeStoredFields = true) {
  for (const frontend::TypedDeclaration& declaration : declarations) {
    if (declaration.kind == frontend::AstDeclarationKind::Import) {
      continue;
    }
    if (declaration.symbolName == owner &&
        (declaration.kind == frontend::AstDeclarationKind::Class ||
         declaration.kind == frontend::AstDeclarationKind::Trait)) {
      if (includeStoredFields &&
          declaration.kind == frontend::AstDeclarationKind::Class) {
        for (const std::string& parameter : declaration.parameters) {
          const std::string name = parameterName(parameter);
          if (!name.empty()) {
            members.insert(name);
          }
        }
      }
      for (const frontend::TypedDeclaration& member : declaration.members) {
        if (!member.name.empty() &&
            member.kind != frontend::AstDeclarationKind::Package &&
            member.kind != frontend::AstDeclarationKind::Import &&
            (includeStoredFields || member.kind == frontend::AstDeclarationKind::Def)) {
          members.insert(member.name);
        }
      }
      const std::vector<std::string> parents =
          declaration.parentTypes.empty() && !declaration.declaredType.empty()
              ? std::vector<std::string>{declaration.declaredType}
              : declaration.parentTypes;
      for (const std::string& parent : parents) {
        collectImplicitThisMembers(declarations, parent, members, includeStoredFields);
      }
      return;
    }
    collectImplicitThisMembers(declaration.members, owner, members,
                               includeStoredFields);
  }
}

std::unordered_set<std::string>
implicitThisMembersFor(const std::vector<frontend::TypedDeclaration>& declarations,
                       const std::string& owner) {
  std::unordered_set<std::string> members;
  collectImplicitThisMembers(declarations, owner, members);
  return members;
}

std::string signatureFor(const frontend::TypedDeclaration& declaration,
                         const ValueContext& context,
                         const std::string& receiverType = {}) {
  if (declaration.kind == frontend::AstDeclarationKind::Def ||
      declaration.kind == frontend::AstDeclarationKind::Val ||
      declaration.kind == frontend::AstDeclarationKind::Var) {
    std::ostringstream signature;
    signature << '(';
    bool wroteParameter = false;
    if (!receiverType.empty()) {
      signature << receiverType;
      wroteParameter = true;
    }
    for (std::size_t i = 0; i < declaration.parameters.size(); ++i) {
      if (wroteParameter) {
        signature << ',';
      }
      signature << (i < declaration.parameterTypes.size()
                        ? runtimeTypeName(declaration.parameterTypes[i])
                        : parameterTypeName(declaration.parameters[i], context));
      wroteParameter = true;
    }
    signature << ')' << runtimeTypeName(declaration.inferredType);
    return signature.str();
  }

  return "()Unit";
}

bool isClassStoredField(const frontend::TypedDeclaration& declaration,
                        frontend::AstDeclarationKind ownerKind) {
  return ownerKind == frontend::AstDeclarationKind::Class &&
         (declaration.kind == frontend::AstDeclarationKind::Val ||
          declaration.kind == frontend::AstDeclarationKind::Var) &&
         declaration.hasInitializer;
}

bool isModuleStoredField(const frontend::TypedDeclaration& declaration,
                         frontend::AstDeclarationKind ownerKind) {
  return ownerKind == frontend::AstDeclarationKind::Object &&
         (declaration.kind == frontend::AstDeclarationKind::Val ||
          declaration.kind == frontend::AstDeclarationKind::Var) &&
         declaration.hasInitializer;
}

std::string storedFieldName(const frontend::TypedDeclaration& declaration) {
  return declaration.needsAccessor ? declaration.name + "$field" : declaration.name;
}

std::string moduleStoredFieldName(const frontend::TypedDeclaration& declaration) {
  return declaration.name + "$field";
}

std::string traitValueOwnerFragment(std::string_view ownerName) {
  std::string fragment;
  fragment.reserve(ownerName.size());
  for (char ch : ownerName) {
    const bool alphaNumeric = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                              (ch >= '0' && ch <= '9');
    fragment.push_back(alphaNumeric || ch == '_' ? ch : '$');
  }
  return fragment;
}

std::string traitValueFieldName(const frontend::TypedDeclaration& owner,
                                const frontend::TypedDeclaration& declaration) {
  return declaration.name + "$trait$" + traitValueOwnerFragment(owner.symbolName) +
         "$field";
}

std::string traitValueAccessorName(const frontend::TypedDeclaration& owner,
                                   const frontend::TypedDeclaration& declaration) {
  return "$trait-value$" + traitValueOwnerFragment(owner.symbolName) + "$" +
         declaration.name;
}

std::string setterName(const std::string& getterName) {
  return getterName + "_$eq";
}

nir::FunctionBody accessorBodyFor(const frontend::TypedDeclaration& declaration,
                                  const std::string& receiverType) {
  nir::FunctionBodyBuilder body;
  (void)body.addParameter("this", receiverType, declaration.span);
  (void)body.addReturn(runtimeTypeName(declaration.inferredType),
                       nir::selectValue(nir::localValue("this", declaration.span),
                                        storedFieldName(declaration), declaration.span),
                       declaration.span);
  return std::move(body).build();
}

nir::FunctionBody fieldAccessorBodyFor(const frontend::TypedDeclaration& declaration,
                                       const std::string& fieldName,
                                       const std::string& receiverType) {
  nir::FunctionBodyBuilder body;
  (void)body.addParameter("this", receiverType, declaration.span);
  (void)body.addReturn(runtimeTypeName(declaration.inferredType),
                       nir::selectValue(nir::localValue("this", declaration.span),
                                        fieldName, declaration.span),
                       declaration.span);
  return std::move(body).build();
}

nir::FunctionBody fieldSetterBodyFor(const frontend::TypedDeclaration& declaration,
                                     const std::string& fieldName,
                                     const std::string& receiverType) {
  nir::FunctionBodyBuilder body;
  (void)body.addParameter("this", receiverType, declaration.span);
  (void)body.addParameter("value", runtimeTypeName(declaration.inferredType),
                          declaration.span);
  (void)body.addEval(
      nir::assignValue(nir::selectValue(nir::localValue("this", declaration.span),
                                        fieldName, declaration.span),
                       nir::localValue("value", declaration.span), declaration.span),
      declaration.span);
  (void)body.addReturn("Unit", nir::unitValue(declaration.span), declaration.span);
  return std::move(body).build();
}

nir::FunctionBody moduleAccessorBodyFor(const frontend::TypedDeclaration& declaration,
                                        const std::string& moduleName) {
  nir::FunctionBodyBuilder body;
  (void)body.addReturn(runtimeTypeName(declaration.inferredType),
                       nir::selectValue(nir::localValue(moduleName, declaration.span),
                                        moduleStoredFieldName(declaration),
                                        declaration.span),
                       declaration.span);
  return std::move(body).build();
}

nir::FunctionBody moduleSetterBodyFor(const frontend::TypedDeclaration& declaration,
                                      const std::string& moduleName) {
  nir::FunctionBodyBuilder body;
  (void)body.addParameter("value", runtimeTypeName(declaration.inferredType),
                          declaration.span);
  (void)body.addEval(
      nir::assignValue(nir::selectValue(nir::localValue(moduleName, declaration.span),
                                        moduleStoredFieldName(declaration),
                                        declaration.span),
                       nir::localValue("value", declaration.span), declaration.span),
      declaration.span);
  (void)body.addReturn("Unit", nir::unitValue(declaration.span), declaration.span);
  return std::move(body).build();
}

nir::FunctionBody
parameterAccessorBodyFor(const frontend::TypedDeclaration& declaration,
                         const std::string& parameter, const ValueContext& context,
                         const std::string& receiverType) {
  nir::FunctionBodyBuilder body;
  const std::string name = parameterName(parameter);
  std::string type = parameterTypeName(parameter, context);
  for (std::size_t i = 0; i < declaration.parameters.size(); ++i) {
    if (parameterName(declaration.parameters[i]) == name &&
        i < declaration.parameterTypes.size()) {
      type = runtimeTypeName(declaration.parameterTypes[i]);
      break;
    }
  }
  (void)body.addParameter("this", receiverType, declaration.span);
  (void)body.addReturn(
      type,
      nir::selectValue(nir::localValue("this", declaration.span),
                       storedParameterFieldName(declaration, parameter),
                       declaration.span),
      declaration.span);
  return std::move(body).build();
}

bool isClassInitializerItem(const frontend::TypedDeclaration& declaration) {
  return isClassStoredField(declaration, frontend::AstDeclarationKind::Class);
}

bool classRequiresInitializerFunction(const frontend::TypedDeclaration& declaration) {
  if (!declaration.parentArguments.empty()) {
    return true;
  }
  for (const frontend::AstClassBodyItem& item : declaration.classBodyItems) {
    if (item.kind == frontend::AstClassBodyItemKind::Expression) {
      return true;
    }
    if (item.index < declaration.members.size() &&
        isClassInitializerItem(declaration.members[item.index])) {
      return true;
    }
  }
  return false;
}

bool moduleRequiresInitializerFunction(const frontend::TypedDeclaration& declaration) {
  for (const frontend::AstClassBodyItem& item : declaration.classBodyItems) {
    if (item.kind == frontend::AstClassBodyItemKind::Expression) {
      return true;
    }
    if (item.index < declaration.members.size() &&
        isModuleStoredField(declaration.members[item.index],
                            frontend::AstDeclarationKind::Object)) {
      return true;
    }
  }
  return false;
}

const frontend::TypedDeclaration*
findDeclarationBySymbol(const std::vector<frontend::TypedDeclaration>& declarations,
                        const std::string& symbolName) {
  for (const frontend::TypedDeclaration& declaration : declarations) {
    if (declaration.kind == frontend::AstDeclarationKind::Import) {
      continue;
    }
    if (declaration.symbolName == symbolName) {
      return &declaration;
    }
    if (const frontend::TypedDeclaration* nested =
            findDeclarationBySymbol(declaration.members, symbolName)) {
      return nested;
    }
  }
  return nullptr;
}

void collectParentMap(
    const std::vector<frontend::TypedDeclaration>& declarations,
    std::unordered_map<std::string, std::vector<std::string>>& parentMap);

const frontend::TypedDeclaration* findMemberDeclaration(const ValueContext& context,
                                                        const std::string& ownerName,
                                                        const std::string& memberName) {
  if (context.declarations == nullptr || ownerName.empty()) {
    return nullptr;
  }

  std::unordered_map<std::string, std::vector<std::string>> parentMap;
  collectParentMap(*context.declarations, parentMap);
  for (const std::string& candidateOwner :
       nir::linearizedTypeNames(ownerName, parentMap)) {
    const frontend::TypedDeclaration* owner =
        findDeclarationBySymbol(*context.declarations, candidateOwner);
    if (owner == nullptr) {
      continue;
    }
    for (const frontend::TypedDeclaration& member : owner->members) {
      if (member.name == memberName &&
          (member.kind == frontend::AstDeclarationKind::Def ||
           member.kind == frontend::AstDeclarationKind::Val ||
           member.kind == frontend::AstDeclarationKind::Var)) {
        return &member;
      }
    }
  }
  return nullptr;
}

const frontend::TypeInfo* findConstructorParameterType(const ValueContext& context,
                                                       const std::string& ownerName,
                                                       const std::string& memberName) {
  if (context.declarations == nullptr || ownerName.empty()) {
    return nullptr;
  }
  const frontend::TypedDeclaration* owner =
      findDeclarationBySymbol(*context.declarations, ownerName);
  if (owner == nullptr || owner->kind != frontend::AstDeclarationKind::Class) {
    return nullptr;
  }
  for (std::size_t i = 0; i < owner->parameters.size(); ++i) {
    if (parameterName(owner->parameters[i]) == memberName &&
        i < owner->parameterTypes.size()) {
      return &owner->parameterTypes[i];
    }
  }
  return nullptr;
}

const frontend::TypedDeclaration*
findTypeMemberDeclaration(const ValueContext& context, const std::string& ownerName,
                          const std::string& memberName) {
  if (context.declarations == nullptr || ownerName.empty()) {
    return nullptr;
  }

  std::unordered_map<std::string, std::vector<std::string>> parentMap;
  collectParentMap(*context.declarations, parentMap);
  for (const std::string& candidateOwner :
       nir::linearizedTypeNames(ownerName, parentMap)) {
    const frontend::TypedDeclaration* owner =
        findDeclarationBySymbol(*context.declarations, candidateOwner);
    if (owner == nullptr) {
      continue;
    }
    for (const frontend::TypedDeclaration& member : owner->members) {
      if (member.kind == frontend::AstDeclarationKind::Type &&
          member.name == memberName) {
        return &member;
      }
    }
  }
  return nullptr;
}

std::string receiverTypeFor(const frontend::AstExpression& expression,
                            const ValueContext& context) {
  using frontend::AstExpressionKind;

  if (const frontend::TypeInfo* type = annotatedTypeFor(expression, context);
      type != nullptr && type->kind == frontend::SimpleTypeKind::Object) {
    return runtimeTypeName(*type);
  }

  switch (expression.kind) {
  case AstExpressionKind::Identifier:
    return {};
  case AstExpressionKind::ModuleReference:
    return {};
  case AstExpressionKind::This:
    return context.currentOwner;
  case AstExpressionKind::Super:
    if (!expression.text.empty() && expression.text != "super") {
      return qualifyTypeName(expression.text, context);
    }
    return context.superType;
  case AstExpressionKind::New:
    return qualifyTypeName(expression.text, context);
  default:
    return {};
  }
}

const frontend::TypedDeclaration*
declarationForExpression(const frontend::AstExpression& expression,
                         const ValueContext& context) {
  using frontend::AstExpressionKind;

  if (expression.kind == AstExpressionKind::Call) {
    return expression.children.empty()
               ? nullptr
               : declarationForExpression(expression.children.front(), context);
  }
  if (expression.kind == AstExpressionKind::TypeApply) {
    return expression.children.empty()
               ? nullptr
               : declarationForExpression(expression.children.front(), context);
  }
  if (expression.kind == AstExpressionKind::Select) {
    if (expression.children.empty()) {
      return nullptr;
    }
    return findMemberDeclaration(context,
                                 receiverTypeFor(expression.children.front(), context),
                                 expression.text);
  }
  if (expression.kind == AstExpressionKind::Identifier &&
      !context.localNames.contains(expression.text)) {
    return findMemberDeclaration(context, context.currentOwner, expression.text);
  }
  return nullptr;
}

const frontend::TypeInfo*
arrayReceiverTypeFor(const frontend::AstExpression& expression,
                     const ValueContext& context) {
  using frontend::AstDeclarationKind;
  using frontend::AstExpressionKind;
  using frontend::SimpleTypeKind;

  if (expression.kind == AstExpressionKind::Identifier) {
    const frontend::TypedDeclaration* declaration =
        declarationForExpression(expression, context);
    if (declaration != nullptr && declaration->kind == AstDeclarationKind::Def) {
      return nullptr;
    }
  } else if (expression.kind != AstExpressionKind::Call) {
    return nullptr;
  }

  const frontend::TypeInfo* type = annotatedTypeFor(expression, context);
  if (type == nullptr || type->kind != SimpleTypeKind::Object ||
      arrayElementTypeName(type->name).empty()) {
    return nullptr;
  }
  return type;
}

std::string memberReceiverType(const frontend::AstExpression& expression,
                               const ValueContext& context) {
  if (expression.kind == frontend::AstExpressionKind::Call) {
    return expression.children.empty()
               ? std::string{}
               : memberReceiverType(expression.children.front(), context);
  }
  if (expression.kind == frontend::AstExpressionKind::TypeApply) {
    return expression.children.empty()
               ? std::string{}
               : memberReceiverType(expression.children.front(), context);
  }
  if (expression.kind == frontend::AstExpressionKind::Select) {
    return expression.children.empty()
               ? std::string{}
               : receiverTypeFor(expression.children.front(), context);
  }
  if (expression.kind == frontend::AstExpressionKind::Identifier) {
    return context.currentOwner;
  }
  return {};
}

std::string boxedPrimitiveTypeFor(const frontend::TypeInfo& type,
                                  const std::string& receiverType,
                                  const ValueContext& context) {
  if (const std::string primitive = boxedPrimitiveType(type); !primitive.empty()) {
    return primitive;
  }
  if (!type.abstractTypeMember || runtimeTypeName(type) != "Object" ||
      receiverType.empty()) {
    return {};
  }

  const std::size_t separator = type.name.find_last_of(".#");
  if (separator == std::string::npos || separator + 1 == type.name.size()) {
    return {};
  }
  const frontend::TypedDeclaration* effectiveType =
      findTypeMemberDeclaration(context, receiverType, type.name.substr(separator + 1));
  return effectiveType == nullptr
             ? std::string{}
             : boxedObjectTypeName(effectiveType->inferredType.kind);
}

std::string localTypeName(const frontend::AstExpression& expression,
                          const ValueContext& context) {
  const std::string declared = localDeclaredTypeName(expression, context);
  if (!declared.empty()) {
    return declared;
  }
  if (!expression.children.empty()) {
    if (const frontend::TypeInfo* type =
            annotatedTypeFor(expression.children.front(), context)) {
      return runtimeTypeName(*type);
    }
  }
  return {};
}

bool declarationDefinesConcreteMember(const frontend::TypedDeclaration& declaration,
                                      const std::string& memberName) {
  if (declaration.kind == frontend::AstDeclarationKind::Class) {
    for (const std::string& parameter : declaration.parameters) {
      if (parameterName(parameter) == memberName) {
        return true;
      }
    }
  }
  for (const frontend::TypedDeclaration& member : declaration.members) {
    if (member.name == memberName &&
        (member.kind != frontend::AstDeclarationKind::Def || member.hasInitializer)) {
      return true;
    }
  }
  return false;
}

void collectParentMap(
    const std::vector<frontend::TypedDeclaration>& declarations,
    std::unordered_map<std::string, std::vector<std::string>>& parentMap) {
  for (const frontend::TypedDeclaration& declaration : declarations) {
    if (declaration.kind == frontend::AstDeclarationKind::Class ||
        declaration.kind == frontend::AstDeclarationKind::Trait ||
        declaration.kind == frontend::AstDeclarationKind::Object) {
      parentMap[declaration.symbolName] = declaration.parentTypes;
    }
    if (declaration.kind == frontend::AstDeclarationKind::Import) {
      continue;
    }
    collectParentMap(declaration.members, parentMap);
  }
}

void addDeclaredMemberNames(const frontend::TypedDeclaration& declaration,
                            std::unordered_set<std::string>& names) {
  if (declaration.kind == frontend::AstDeclarationKind::Class) {
    for (const std::string& parameter : declaration.parameters) {
      const std::string name = parameterName(parameter);
      if (!name.empty()) {
        names.insert(name);
      }
    }
  }
  for (const frontend::TypedDeclaration& member : declaration.members) {
    if (member.kind == frontend::AstDeclarationKind::Def ||
        member.kind == frontend::AstDeclarationKind::Val ||
        member.kind == frontend::AstDeclarationKind::Var) {
      names.insert(member.name);
    }
  }
}

std::vector<MaterializedTraitValue> materializedTraitValuesForClass(
    const frontend::TypedModule& module, const frontend::TypedDeclaration& declaration,
    const std::unordered_map<std::string, std::vector<std::string>>& parentMap) {
  std::vector<MaterializedTraitValue> values;
  if (declaration.kind != frontend::AstDeclarationKind::Class) {
    return values;
  }

  std::unordered_set<std::string> resolvedNames;
  std::unordered_set<std::string> inheritedValueKeys;
  addDeclaredMemberNames(declaration, resolvedNames);
  for (const std::string& ownerName :
       nir::linearizedParentNames(declaration.parentTypes, parentMap)) {
    const frontend::TypedDeclaration* owner =
        findDeclarationBySymbol(module.declarations, ownerName);
    if (owner == nullptr) {
      continue;
    }
    if (owner->kind == frontend::AstDeclarationKind::Class) {
      for (const std::string& inheritedOwnerName :
           nir::linearizedTypeNames(owner->symbolName, parentMap)) {
        const frontend::TypedDeclaration* inheritedOwner =
            findDeclarationBySymbol(module.declarations, inheritedOwnerName);
        if (inheritedOwner == nullptr) {
          continue;
        }
        addDeclaredMemberNames(*inheritedOwner, resolvedNames);
        if (inheritedOwner->kind != frontend::AstDeclarationKind::Trait) {
          continue;
        }
        for (const frontend::TypedDeclaration& member : inheritedOwner->members) {
          if ((member.kind == frontend::AstDeclarationKind::Val ||
               member.kind == frontend::AstDeclarationKind::Var) &&
              member.hasInitializer) {
            inheritedValueKeys.insert(inheritedOwner->symbolName + "." + member.name);
          }
        }
      }
      continue;
    }
    if (owner->kind != frontend::AstDeclarationKind::Trait) {
      continue;
    }
    for (const frontend::TypedDeclaration& member : owner->members) {
      if (member.kind != frontend::AstDeclarationKind::Def &&
          member.kind != frontend::AstDeclarationKind::Val &&
          member.kind != frontend::AstDeclarationKind::Var) {
        continue;
      }
      const bool effective = resolvedNames.insert(member.name).second;
      if ((member.kind == frontend::AstDeclarationKind::Val ||
           member.kind == frontend::AstDeclarationKind::Var) &&
          member.hasInitializer &&
          !inheritedValueKeys.contains(owner->symbolName + "." + member.name)) {
        values.push_back(MaterializedTraitValue{owner, &member, effective});
      }
    }
  }

  return values;
}

std::vector<MaterializedTraitValue>
materializedTraitValuesForClass(const frontend::TypedModule& module,
                                const frontend::TypedDeclaration& declaration) {
  std::unordered_map<std::string, std::vector<std::string>> parentMap;
  collectParentMap(module.declarations, parentMap);
  return materializedTraitValuesForClass(module, declaration, parentMap);
}

const frontend::TypedDeclaration*
initializedTraitValueMember(const ValueContext& context, const std::string& ownerName,
                            const std::string& memberName) {
  if (context.declarations == nullptr) {
    return nullptr;
  }
  const frontend::TypedDeclaration* owner =
      findDeclarationBySymbol(*context.declarations, ownerName);
  if (owner == nullptr || owner->kind != frontend::AstDeclarationKind::Trait) {
    return nullptr;
  }
  for (const frontend::TypedDeclaration& member : owner->members) {
    if (member.name == memberName &&
        (member.kind == frontend::AstDeclarationKind::Val ||
         member.kind == frontend::AstDeclarationKind::Var) &&
        member.hasInitializer) {
      return &member;
    }
  }
  return nullptr;
}

std::string resolveSuperMemberOwner(const ValueContext& context,
                                    const std::string& memberName) {
  if (context.declarations == nullptr || context.superTypes.empty()) {
    return context.superType;
  }
  std::unordered_map<std::string, std::vector<std::string>> parentMap;
  collectParentMap(*context.declarations, parentMap);
  for (const std::string& parentType :
       nir::linearizedParentNames(context.superTypes, parentMap)) {
    const frontend::TypedDeclaration* parent =
        findDeclarationBySymbol(*context.declarations, parentType);
    if (parent != nullptr && declarationDefinesConcreteMember(*parent, memberName)) {
      return parent->symbolName;
    }
  }
  return context.superType;
}

bool classRequiresInitializerFunction(const frontend::TypedModule& module,
                                      const frontend::TypedDeclaration& declaration) {
  if (classRequiresInitializerFunction(declaration)) {
    return true;
  }
  if (!materializedTraitValuesForClass(module, declaration).empty()) {
    return true;
  }
  if (declaration.declaredType.empty()) {
    return false;
  }
  const frontend::TypedDeclaration* parent =
      findDeclarationBySymbol(module.declarations, declaration.declaredType);
  return parent != nullptr && parent->kind == frontend::AstDeclarationKind::Class &&
         classRequiresInitializerFunction(module, *parent);
}

std::string escapeBodyText(const std::string& text) {
  std::string escaped;
  for (char ch : text) {
    switch (ch) {
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped.push_back(ch);
      break;
    }
  }
  return escaped;
}

std::string literalTypeFor(const frontend::AstExpression& expression) {
  using frontend::AstExpressionKind;

  switch (expression.kind) {
  case AstExpressionKind::IntegerLiteral:
    return !expression.text.empty() &&
                   (expression.text.back() == 'l' || expression.text.back() == 'L')
               ? "Long"
               : "Int";
  case AstExpressionKind::FloatingLiteral:
    return !expression.text.empty() &&
                   (expression.text.back() == 'f' || expression.text.back() == 'F')
               ? "Float"
               : "Double";
  case AstExpressionKind::StringLiteral:
    return "String";
  case AstExpressionKind::CharLiteral:
    return "Char";
  case AstExpressionKind::SymbolLiteral:
    return "Symbol";
  case AstExpressionKind::BooleanLiteral:
    return "Boolean";
  case AstExpressionKind::NullLiteral:
    return "Null";
  default:
    return "Unknown";
  }
}

std::string runtimeToStringName(frontend::SimpleTypeKind kind) {
  switch (kind) {
  case frontend::SimpleTypeKind::Boolean:
    return std::string(support::StdNames::RuntimeBooleanToString);
  case frontend::SimpleTypeKind::Byte:
    return std::string(support::StdNames::RuntimeByteToString);
  case frontend::SimpleTypeKind::Short:
    return std::string(support::StdNames::RuntimeShortToString);
  case frontend::SimpleTypeKind::Int:
    return std::string(support::StdNames::RuntimeIntToString);
  case frontend::SimpleTypeKind::Long:
    return std::string(support::StdNames::RuntimeLongToString);
  case frontend::SimpleTypeKind::Float:
    return std::string(support::StdNames::RuntimeFloatToString);
  case frontend::SimpleTypeKind::Double:
    return std::string(support::StdNames::RuntimeDoubleToString);
  case frontend::SimpleTypeKind::Char:
    return std::string(support::StdNames::RuntimeCharToString);
  default:
    return {};
  }
}

std::string runtimeHashCodeName(frontend::SimpleTypeKind kind) {
  switch (kind) {
  case frontend::SimpleTypeKind::Boolean:
    return std::string(support::StdNames::RuntimeBooleanHashCode);
  case frontend::SimpleTypeKind::Byte:
    return std::string(support::StdNames::RuntimeByteHashCode);
  case frontend::SimpleTypeKind::Short:
    return std::string(support::StdNames::RuntimeShortHashCode);
  case frontend::SimpleTypeKind::Long:
    return std::string(support::StdNames::RuntimeLongHashCode);
  case frontend::SimpleTypeKind::Float:
    return std::string(support::StdNames::RuntimeFloatHashCode);
  case frontend::SimpleTypeKind::Double:
    return std::string(support::StdNames::RuntimeDoubleHashCode);
  case frontend::SimpleTypeKind::Char:
    return std::string(support::StdNames::RuntimeCharHashCode);
  case frontend::SimpleTypeKind::String:
    return std::string(support::StdNames::RuntimeStringHashCode);
  case frontend::SimpleTypeKind::Symbol:
    return std::string(support::StdNames::RuntimeSymbolHashCode);
  default:
    return {};
  }
}

std::string runtimeNumericConversionName(frontend::SimpleTypeKind source,
                                         std::string_view conversion) {
  if (conversion == support::StdNames::ToByte) {
    if (source == frontend::SimpleTypeKind::Short) {
      return std::string(support::StdNames::RuntimeShortToByte);
    }
    if (source == frontend::SimpleTypeKind::Int) {
      return std::string(support::StdNames::RuntimeIntToByte);
    }
  }
  if (conversion == support::StdNames::ToShort) {
    if (source == frontend::SimpleTypeKind::Byte) {
      return std::string(support::StdNames::RuntimeByteToShort);
    }
    if (source == frontend::SimpleTypeKind::Int) {
      return std::string(support::StdNames::RuntimeIntToShort);
    }
  }
  if (conversion == support::StdNames::ToInt) {
    if (source == frontend::SimpleTypeKind::Byte) {
      return std::string(support::StdNames::RuntimeByteToInt);
    }
    if (source == frontend::SimpleTypeKind::Short) {
      return std::string(support::StdNames::RuntimeShortToInt);
    }
  }
  return {};
}

nir::Value valueFor(const frontend::AstExpression& expression,
                    const ValueContext& context, bool preserveCallable = false);
nir::Value expressionValueFor(const frontend::AstExpression& expression,
                              const ValueContext& context,
                              bool preserveCallable = false);

nir::Value promoteNarrowIntegral(const frontend::AstExpression& expression,
                                 const ValueContext& context) {
  nir::Value value = expressionValueFor(expression, context);
  const frontend::TypeInfo* type = annotatedTypeFor(expression, context);
  if (type == nullptr) {
    return value;
  }
  const std::string_view runtime = type->kind == frontend::SimpleTypeKind::Byte
                                       ? support::StdNames::RuntimeByteToInt
                                   : type->kind == frontend::SimpleTypeKind::Short
                                       ? support::StdNames::RuntimeShortToInt
                                       : std::string_view{};
  if (runtime.empty()) {
    return value;
  }
  std::vector<nir::Value> arguments;
  arguments.push_back(std::move(value));
  return nir::callValue(nir::localValue(std::string(runtime), expression.span),
                        std::move(arguments), expression.span);
}

nir::Value boxForObjectStorage(nir::Value value, const std::string& targetType,
                               const frontend::AstExpression& source,
                               const ValueContext& context) {
  if (targetType != "Object") {
    return value;
  }
  const frontend::TypeInfo* sourceType = annotatedTypeFor(source, context);
  if (sourceType == nullptr) {
    return value;
  }
  const std::string primitive = boxedObjectTypeName(sourceType->kind);
  return primitive.empty() ? value
                           : nir::boxValue(primitive, std::move(value), source.span);
}

bool hasObjectRuntimeType(const frontend::AstExpression& expression,
                          const ValueContext& context) {
  const frontend::TypeInfo* type = annotatedTypeFor(expression, context);
  return type != nullptr && runtimeTypeName(*type) == "Object";
}

nir::Value runtimeAnyEqualsValue(const frontend::AstExpression& lhs,
                                 const frontend::AstExpression& rhs,
                                 support::SourceSpan span, const ValueContext& context,
                                 bool checkedReceiver = false) {
  std::vector<nir::Value> arguments;
  arguments.push_back(
      boxForObjectStorage(expressionValueFor(lhs, context), "Object", lhs, context));
  arguments.push_back(
      boxForObjectStorage(expressionValueFor(rhs, context), "Object", rhs, context));
  return nir::callValue(
      nir::localValue(std::string(checkedReceiver
                                      ? support::StdNames::RuntimeAnyReceiverEquals
                                      : support::StdNames::RuntimeAnyEquals),
                      span),
      std::move(arguments), span);
}

nir::Value scopedBodyValueFor(const frontend::AstExpression& expression,
                              const ValueContext& context) {
  using frontend::AstExpressionKind;
  if (expression.kind != AstExpressionKind::Block) {
    return valueFor(expression, context);
  }

  ValueContext blockContext = context;
  std::vector<nir::Value> values;
  values.reserve(expression.children.size());
  for (const frontend::AstExpression& child : expression.children) {
    if (child.kind != AstExpressionKind::LocalDeclaration) {
      values.push_back(scopedBodyValueFor(child, blockContext));
      continue;
    }

    nir::Value initializer =
        child.children.empty()
            ? nir::unitValue(child.span)
            : scopedBodyValueFor(child.children.front(), blockContext);
    const std::string type = localTypeName(child, blockContext);
    if (!child.children.empty()) {
      initializer = boxForObjectStorage(std::move(initializer), type,
                                        child.children.front(), blockContext);
    }
    if (child.mutableLocal) {
      values.push_back(
          nir::localVarValue(child.text, type, std::move(initializer), child.span));
    } else {
      values.push_back(
          nir::localLetValue(child.text, type, std::move(initializer), child.span));
    }
    blockContext.localNames.insert(child.text);
  }
  return nir::blockValue(std::move(values), expression.span);
}

nir::Value expressionValueFor(const frontend::AstExpression& expression,
                              const ValueContext& context, bool preserveCallable) {
  if (expression.kind == frontend::AstExpressionKind::Block) {
    return scopedBodyValueFor(expression, context);
  }
  return valueFor(expression, context, preserveCallable);
}

void appendExpressionSetup(const frontend::AstExpression& expression,
                           nir::FunctionBodyBuilder& body,
                           const ValueContext& context) {
  using frontend::AstExpressionKind;

  if (expression.kind == AstExpressionKind::Block) {
    ValueContext blockContext = context;
    blockContext.lexicalScopes.push_back(expression.span);
    for (std::size_t i = 0; i < expression.children.size(); ++i) {
      const frontend::AstExpression& child = expression.children[i];
      const bool isLast = i + 1 == expression.children.size();
      if (child.kind == AstExpressionKind::LocalDeclaration) {
        const std::string declaredType = localTypeName(child, blockContext);
        if (!child.children.empty()) {
          appendExpressionSetup(child.children.front(), body, blockContext);
          nir::Value initializer =
              boxForObjectStorage(valueFor(child.children.front(), blockContext),
                                  declaredType, child.children.front(), blockContext);
          if (child.mutableLocal) {
            (void)body.addVar(child.text, declaredType, std::move(initializer),
                              child.span, blockContext.lexicalScopes);
          } else {
            (void)body.addLet(child.text, declaredType, std::move(initializer),
                              child.span, blockContext.lexicalScopes);
          }
        } else {
          if (child.mutableLocal) {
            (void)body.addVar(child.text, declaredType, nir::unitValue(child.span),
                              child.span, blockContext.lexicalScopes);
          } else {
            (void)body.addLet(child.text, declaredType, nir::unitValue(child.span),
                              child.span, blockContext.lexicalScopes);
          }
        }
        blockContext.localNames.insert(child.text);
        continue;
      }
      appendExpressionSetup(child, body, blockContext);
      if (!isLast) {
        (void)body.addEval(valueFor(child, blockContext), child.span,
                           blockContext.lexicalScopes);
      }
    }
    return;
  }

  if (expression.kind == AstExpressionKind::LocalDeclaration) {
    const std::string declaredType = localTypeName(expression, context);
    if (!expression.children.empty()) {
      appendExpressionSetup(expression.children.front(), body, context);
      nir::Value initializer =
          boxForObjectStorage(valueFor(expression.children.front(), context),
                              declaredType, expression.children.front(), context);
      if (expression.mutableLocal) {
        (void)body.addVar(expression.text, declaredType, std::move(initializer),
                          expression.span, context.lexicalScopes);
      } else {
        (void)body.addLet(expression.text, declaredType, std::move(initializer),
                          expression.span, context.lexicalScopes);
      }
    } else {
      if (expression.mutableLocal) {
        (void)body.addVar(expression.text, declaredType,
                          nir::unitValue(expression.span), expression.span,
                          context.lexicalScopes);
      } else {
        (void)body.addLet(expression.text, declaredType,
                          nir::unitValue(expression.span), expression.span,
                          context.lexicalScopes);
      }
    }
  }
}

nir::Value valueFor(const frontend::AstExpression& expression,
                    const ValueContext& context, bool preserveCallable) {
  using frontend::AstExpressionKind;

  switch (expression.kind) {
  case AstExpressionKind::Empty:
    return nir::unitValue(expression.span);
  case AstExpressionKind::Identifier:
    if (expression.text == support::StdNames::NotImplemented) {
      return nir::throwValue(
          nir::newValue(std::string(support::StdNames::ScalaNotImplementedError),
                        {nir::literalValue("\"Implementation is missing\"", "String",
                                           expression.span)},
                        expression.span),
          expression.span);
    }
    if (expression.text == support::StdNames::Assert) {
      return nir::localValue(std::string(support::StdNames::RuntimeAssert),
                             expression.span);
    }
    if (expression.text == support::StdNames::Assume) {
      return nir::localValue(std::string(support::StdNames::RuntimeAssume),
                             expression.span);
    }
    if (expression.text == support::StdNames::Require) {
      return nir::localValue(std::string(support::StdNames::RuntimeRequire),
                             expression.span);
    }
    if (expression.text == support::StdNames::Println) {
      return nir::localValue(std::string(support::StdNames::RuntimePrintln),
                             expression.span);
    }
    if (expression.text == support::StdNames::GcCollect) {
      return nir::localValue(std::string(support::StdNames::RuntimeGcCollect),
                             expression.span);
    }
    if (expression.text == support::StdNames::GcLiveObjectCount) {
      return nir::localValue(std::string(support::StdNames::RuntimeGcLiveObjectCount),
                             expression.span);
    }
    if (expression.text == support::StdNames::GcCollectionCount) {
      return nir::localValue(std::string(support::StdNames::RuntimeGcCollectionCount),
                             expression.span);
    }
    if (expression.text == support::StdNames::GcSetCollectionThreshold) {
      return nir::localValue(
          std::string(support::StdNames::RuntimeGcSetCollectionThreshold),
          expression.span);
    }
    if (expression.text == support::StdNames::RuntimeFormat ||
        expression.text == support::StdNames::RuntimeFormatBoolean) {
      return nir::localValue(expression.text, expression.span);
    }
    if (shouldUseImplicitThis(expression.text, context)) {
      nir::Value selected = nir::selectValue(nir::localValue("this", expression.span),
                                             expression.text, expression.span);
      const frontend::TypedDeclaration* selectedDeclaration =
          declarationForExpression(expression, context);
      const std::string primitive =
          selectedDeclaration == nullptr
              ? std::string{}
              : boxedPrimitiveTypeFor(selectedDeclaration->inferredType,
                                      memberReceiverType(expression, context), context);
      if (!preserveCallable && !primitive.empty()) {
        return nir::unboxValue(primitive, std::move(selected), expression.span);
      }
      return selected;
    }
    if (!context.localNames.contains(expression.text)) {
      const frontend::TypedDeclaration* selectedDeclaration =
          declarationForExpression(expression, context);
      const frontend::TypedDeclaration* owner =
          context.declarations == nullptr
              ? nullptr
              : findDeclarationBySymbol(*context.declarations, context.currentOwner);
      if (selectedDeclaration != nullptr && owner != nullptr &&
          owner->kind == frontend::AstDeclarationKind::Object &&
          (selectedDeclaration->kind == frontend::AstDeclarationKind::Val ||
           selectedDeclaration->kind == frontend::AstDeclarationKind::Var)) {
        return nir::callValue(
            nir::localValue(context.currentOwner + "." + selectedDeclaration->name,
                            expression.span),
            {}, expression.span);
      }
    }
    return nir::localValue(resolveIdentifierName(expression.text, context),
                           expression.span);
  case AstExpressionKind::ModuleReference:
    return nir::localValue(resolveIdentifierName(expression.text, context),
                           expression.span);
  case AstExpressionKind::IntegerLiteral:
  case AstExpressionKind::FloatingLiteral:
  case AstExpressionKind::CharLiteral:
  case AstExpressionKind::SymbolLiteral:
  case AstExpressionKind::BooleanLiteral:
  case AstExpressionKind::NullLiteral:
    return nir::literalValue(escapeBodyText(expression.text),
                             literalTypeFor(expression), expression.span);
  case AstExpressionKind::StringLiteral:
    return nir::literalValue(expression.text, literalTypeFor(expression),
                             expression.span);
  case AstExpressionKind::This:
    return nir::localValue("this", expression.span);
  case AstExpressionKind::Super:
    if (!expression.text.empty() && expression.text != "super") {
      return nir::qualifiedSuperValue(qualifyTypeName(expression.text, context),
                                      expression.span);
    }
    if (context.superType.empty()) {
      return nir::unknownValue("super", expression.span);
    }
    if (context.stackableSuper && !context.currentOwner.empty()) {
      return nir::stackSuperValue(context.currentOwner, expression.span);
    }
    return nir::superValue(context.superType, expression.span);
  case AstExpressionKind::New:
    return nir::newValue(qualifyTypeName(expression.text, context), expression.span);
  case AstExpressionKind::LocalDeclaration:
    return nir::unitValue(expression.span);
  case AstExpressionKind::Return:
    if (expression.children.empty()) {
      return nir::unitValue(expression.span);
    }
    return expressionValueFor(expression.children.front(), context);
  case AstExpressionKind::Throw:
    if (expression.children.size() != 1) {
      return nir::unknownValue("<malformed-throw>", expression.span);
    }
    return nir::throwValue(expressionValueFor(expression.children.front(), context),
                           expression.span);
  case AstExpressionKind::Try: {
    if (expression.children.size() < 2) {
      return nir::unknownValue("<malformed-try>", expression.span);
    }
    const frontend::TypeInfo* semanticType = annotatedTypeFor(expression, context);
    const std::string resultType =
        semanticType == nullptr ? "Unknown" : runtimeTypeName(*semanticType);
    nir::Value body = scopedBodyValueFor(expression.children.front(), context);
    std::vector<nir::Value> catches;
    nir::Value finalizer;
    bool hasFinalizer = false;
    for (std::size_t index = 1; index < expression.children.size(); ++index) {
      const frontend::AstExpression& child = expression.children[index];
      if (child.kind == AstExpressionKind::Finally && child.children.size() == 1) {
        finalizer = nir::finallyValue(
            scopedBodyValueFor(child.children.front(), context), child.span);
        hasFinalizer = true;
        continue;
      }
      if (child.kind != AstExpressionKind::Catch || child.children.size() != 1) {
        return nir::unknownValue("<malformed-try-child>", child.span);
      }
      ValueContext handlerContext = context;
      handlerContext.localNames.insert(child.text);
      const frontend::TypeInfo* handlerSemanticType = annotatedTypeFor(child, context);
      const std::string handlerResultType = handlerSemanticType == nullptr
                                                ? resultType
                                                : runtimeTypeName(*handlerSemanticType);
      catches.push_back(
          nir::catchValue(child.text, qualifyTypeName(child.declaredType, context),
                          scopedBodyValueFor(child.children.front(), handlerContext),
                          handlerResultType, child.span));
    }
    return hasFinalizer
               ? nir::tryValue(std::move(body), std::move(catches),
                               std::move(finalizer), resultType, expression.span)
               : nir::tryValue(std::move(body), std::move(catches), resultType,
                               expression.span);
  }
  case AstExpressionKind::Catch:
    return nir::unknownValue("<catch-outside-try>", expression.span);
  case AstExpressionKind::Finally:
    return nir::unknownValue("<finally-outside-try>", expression.span);
  case AstExpressionKind::Block:
    if (expression.children.empty()) {
      return nir::unitValue(expression.span);
    }
    {
      ValueContext blockContext = context;
      for (std::size_t i = 0; i + 1 < expression.children.size(); ++i) {
        const frontend::AstExpression& child = expression.children[i];
        if (child.kind == AstExpressionKind::LocalDeclaration) {
          blockContext.localNames.insert(child.text);
        }
      }
      return valueFor(expression.children.back(), blockContext);
    }
  case AstExpressionKind::Select:
    if (expression.children.empty()) {
      return nir::unknownValue("." + expression.text, expression.span);
    }
    {
      if (expression.text == support::StdNames::ToByte ||
          expression.text == support::StdNames::ToShort ||
          expression.text == support::StdNames::ToInt) {
        const frontend::AstExpression& receiver = expression.children.front();
        const frontend::TypeInfo* receiverType = annotatedTypeFor(receiver, context);
        if (receiverType != nullptr) {
          nir::Value receiverValue = expressionValueFor(receiver, context);
          const std::string runtime =
              runtimeNumericConversionName(receiverType->kind, expression.text);
          if (runtime.empty()) {
            return receiverValue;
          }
          std::vector<nir::Value> arguments;
          arguments.push_back(std::move(receiverValue));
          return nir::callValue(nir::localValue(runtime, expression.span),
                                std::move(arguments), expression.span);
        }
      }
      if (expression.text == support::StdNames::StringLength) {
        const frontend::TypeInfo* receiverType =
            annotatedTypeFor(expression.children.front(), context);
        if (receiverType != nullptr &&
            receiverType->kind == frontend::SimpleTypeKind::String) {
          std::vector<nir::Value> arguments;
          arguments.push_back(expressionValueFor(expression.children.front(), context));
          return nir::callValue(
              nir::localValue(std::string(support::StdNames::RuntimeStringLength),
                              expression.span),
              std::move(arguments), expression.span);
        }
        if (receiverType != nullptr &&
            receiverType->kind == frontend::SimpleTypeKind::Object) {
          const std::string elementType = arrayElementTypeName(receiverType->name);
          if (!elementType.empty()) {
            std::vector<nir::Value> arguments;
            arguments.push_back(
                expressionValueFor(expression.children.front(), context));
            const std::string runtime = arrayLengthRuntimeName(elementType, context);
            return nir::callValue(nir::localValue(runtime, expression.span),
                                  std::move(arguments), expression.span);
          }
        }
      }
      const frontend::TypedDeclaration* selectedDeclaration =
          declarationForExpression(expression, context);
      if (expression.text == support::StdNames::HashCode) {
        const frontend::TypeInfo* receiverType =
            annotatedTypeFor(expression.children.front(), context);
        if (receiverType != nullptr && selectedDeclaration == nullptr) {
          if (receiverType->kind == frontend::SimpleTypeKind::Unit) {
            return nir::literalValue("0", "Int", expression.span);
          }
          if (receiverType->kind == frontend::SimpleTypeKind::Int) {
            return expressionValueFor(expression.children.front(), context);
          }
          const std::string runtimeName = runtimeHashCodeName(receiverType->kind);
          if (!runtimeName.empty()) {
            std::vector<nir::Value> arguments;
            arguments.push_back(
                expressionValueFor(expression.children.front(), context));
            return nir::callValue(nir::localValue(runtimeName, expression.span),
                                  std::move(arguments), expression.span);
          }
          if (receiverType->kind == frontend::SimpleTypeKind::Object) {
            std::vector<nir::Value> arguments;
            arguments.push_back(
                expressionValueFor(expression.children.front(), context));
            return nir::callValue(
                nir::localValue(
                    std::string(support::StdNames::RuntimeAnyReceiverHashCode),
                    expression.span),
                std::move(arguments), expression.span);
          }
          if (receiverType->kind == frontend::SimpleTypeKind::Null) {
            std::vector<nir::Value> arguments;
            arguments.push_back(
                expressionValueFor(expression.children.front(), context));
            return nir::callValue(
                nir::localValue(
                    std::string(support::StdNames::RuntimeAnyReceiverHashCode),
                    expression.span),
                std::move(arguments), expression.span);
          }
        }
      }
      if (expression.text == support::StdNames::ToString) {
        const frontend::TypeInfo* receiverType =
            annotatedTypeFor(expression.children.front(), context);
        if (receiverType != nullptr && selectedDeclaration == nullptr) {
          if (receiverType->kind == frontend::SimpleTypeKind::String) {
            std::vector<nir::Value> arguments;
            arguments.push_back(
                expressionValueFor(expression.children.front(), context));
            return nir::callValue(
                nir::localValue(std::string(support::StdNames::RuntimeStringToString),
                                expression.span),
                std::move(arguments), expression.span);
          }
          const std::string runtimeName = runtimeToStringName(receiverType->kind);
          if (!runtimeName.empty()) {
            std::vector<nir::Value> arguments;
            arguments.push_back(
                expressionValueFor(expression.children.front(), context));
            return nir::callValue(nir::localValue(runtimeName, expression.span),
                                  std::move(arguments), expression.span);
          }
          if (receiverType->kind == frontend::SimpleTypeKind::Unit ||
              receiverType->kind == frontend::SimpleTypeKind::Symbol ||
              receiverType->kind == frontend::SimpleTypeKind::Null ||
              receiverType->kind == frontend::SimpleTypeKind::Object) {
            std::vector<nir::Value> arguments;
            nir::Value receiver =
                expressionValueFor(expression.children.front(), context);
            receiver = boxForObjectStorage(std::move(receiver), "Object",
                                           expression.children.front(), context);
            arguments.push_back(std::move(receiver));
            const std::string_view runtime =
                receiverType->kind == frontend::SimpleTypeKind::Null ||
                        receiverType->kind == frontend::SimpleTypeKind::Object
                    ? support::StdNames::RuntimeAnyReceiverToString
                    : support::StdNames::RuntimeAnyToString;
            return nir::callValue(
                nir::localValue(std::string(runtime), expression.span),
                std::move(arguments), expression.span);
          }
        }
      }
      const std::string selectedReceiver = memberReceiverType(expression, context);
      auto adaptSelectedValue = [&](nir::Value selected) {
        const frontend::TypeInfo* erasedSelectedType =
            selectedDeclaration == nullptr
                ? findConstructorParameterType(context, selectedReceiver,
                                               expression.text)
                : &selectedDeclaration->inferredType;
        const std::string primitive =
            erasedSelectedType == nullptr
                ? std::string{}
                : boxedPrimitiveTypeFor(*erasedSelectedType, selectedReceiver, context);
        if (!preserveCallable && !primitive.empty()) {
          return nir::unboxValue(primitive, std::move(selected), expression.span);
        }
        if (!preserveCallable && erasedSelectedType != nullptr &&
            (selectedDeclaration == nullptr ||
             selectedDeclaration->kind != frontend::AstDeclarationKind::Def)) {
          const frontend::TypeInfo* selectedType =
              annotatedTypeFor(expression, context);
          const std::string erasedType = runtimeTypeName(*erasedSelectedType);
          const std::string staticType =
              selectedType == nullptr ? erasedType : runtimeTypeName(*selectedType);
          if (erasedType != staticType && staticType != "Object") {
            const std::string boxedType = selectedType == nullptr
                                              ? std::string{}
                                              : boxedObjectTypeName(selectedType->kind);
            if (!boxedType.empty()) {
              return nir::unboxValue(boxedType, std::move(selected), expression.span);
            }
            return nir::asInstanceOfValue(staticType, std::move(selected),
                                          expression.span);
          }
        }
        return selected;
      };
      if (expression.children.front().kind == AstExpressionKind::Super &&
          !expression.children.front().text.empty() &&
          expression.children.front().text != "super" &&
          !context.currentOwner.empty() && context.declarations != nullptr) {
        const std::string traitOwner =
            qualifyTypeName(expression.children.front().text, context);
        const frontend::TypedDeclaration* traitValue =
            initializedTraitValueMember(context, traitOwner, expression.text);
        const frontend::TypedDeclaration* currentOwner =
            findDeclarationBySymbol(*context.declarations, context.currentOwner);
        const frontend::TypedDeclaration* traitDeclaration =
            findDeclarationBySymbol(*context.declarations, traitOwner);
        if (traitValue != nullptr && traitDeclaration != nullptr &&
            currentOwner != nullptr &&
            currentOwner->kind == frontend::AstDeclarationKind::Class) {
          return adaptSelectedValue(nir::selectValue(
              nir::superValue(context.currentOwner, expression.span),
              traitValueAccessorName(*traitDeclaration, *traitValue), expression.span));
        }
      }
      if (expression.children.front().kind == AstExpressionKind::Super &&
          (expression.children.front().text.empty() ||
           expression.children.front().text == "super")) {
        if (context.stackableSuper && !context.currentOwner.empty()) {
          return adaptSelectedValue(nir::selectValue(
              nir::stackSuperValue(context.currentOwner, expression.span),
              expression.text, expression.span));
        }
        const std::string owner = resolveSuperMemberOwner(context, expression.text);
        if (owner.empty()) {
          return nir::unknownValue("super." + expression.text, expression.span);
        }
        return adaptSelectedValue(nir::selectValue(
            nir::superValue(owner, expression.span), expression.text, expression.span));
      }
      return adaptSelectedValue(
          nir::selectValue(expressionValueFor(expression.children.front(), context),
                           expression.text, expression.span));
    }
  case AstExpressionKind::TypeApply: {
    if (expression.children.size() != 1) {
      return nir::unknownValue("type-apply <malformed>", expression.span);
    }
    const frontend::AstExpression& callee = expression.children.front();
    const bool isArrayEmpty =
        callee.kind == AstExpressionKind::Select && callee.children.size() == 1 &&
        callee.text == support::StdNames::ArrayEmpty &&
        callee.children.front().kind == AstExpressionKind::Identifier &&
        callee.children.front().text == "Array";
    if (isArrayEmpty) {
      const frontend::TypeInfo* resultType = annotatedTypeFor(expression, context);
      const std::string elementType = resultType == nullptr
                                          ? std::string{}
                                          : arrayElementTypeName(resultType->name);
      if (!elementType.empty()) {
        recordReferenceArrayElementType(elementType, context);
        std::vector<nir::Value> arguments;
        arguments.push_back(nir::literalValue("0", "Int", expression.span));
        return nir::callValue(
            nir::localValue(arrayAllocRuntimeName(elementType, context),
                            expression.span),
            std::move(arguments), expression.span);
      }
    }
    if (callee.kind == AstExpressionKind::Identifier &&
        callee.text == support::StdNames::SizeOf) {
      return nir::sizeOfValue(qualifyTypeName(expression.declaredType, context),
                              expression.span);
    }
    if (callee.kind != AstExpressionKind::Select || callee.children.size() != 1) {
      return expressionValueFor(callee, context, preserveCallable);
    }
    const std::string targetType = qualifyTypeName(expression.declaredType, context);
    nir::Value receiver = expressionValueFor(callee.children.front(), context);
    if (callee.text == support::StdNames::IsInstanceOf) {
      return nir::isInstanceOfValue(targetType, std::move(receiver), expression.span);
    }
    if (callee.text == support::StdNames::AsInstanceOf) {
      const frontend::TypeInfo* receiverType =
          annotatedTypeFor(callee.children.front(), context);
      if (targetType == "String" && receiverType != nullptr &&
          runtimeTypeName(*receiverType) == "Object") {
        return nir::unboxValue("String", std::move(receiver), expression.span);
      }
      if (const frontend::TypeInfo* target = annotatedTypeFor(expression, context)) {
        if (const std::string primitive = primitiveTypeName(target->kind);
            !primitive.empty()) {
          return nir::unboxValue(primitive, std::move(receiver), expression.span);
        }
      }
      return nir::asInstanceOfValue(targetType, std::move(receiver), expression.span);
    }
    return expressionValueFor(callee, context, preserveCallable);
  }
  case AstExpressionKind::Call: {
    if (expression.children.empty()) {
      return nir::unknownValue("call <empty>", expression.span);
    }
    if (expression.children.size() == 6 &&
        expression.children.front().kind == AstExpressionKind::Select &&
        expression.children.front().children.size() == 1 &&
        expression.children.front().text == support::StdNames::ArrayCopy &&
        expression.children.front().children.front().kind ==
            AstExpressionKind::Identifier &&
        expression.children.front().children.front().text == "Array") {
      const frontend::TypeInfo* sourceType =
          annotatedTypeFor(expression.children[1], context);
      const frontend::TypeInfo* destinationType =
          annotatedTypeFor(expression.children[3], context);
      const std::string sourceElementType =
          sourceType == nullptr ? std::string{}
                                : arrayElementTypeName(sourceType->name);
      const std::string destinationElementType =
          destinationType == nullptr ? std::string{}
                                     : arrayElementTypeName(destinationType->name);
      if (!sourceElementType.empty() && !destinationElementType.empty()) {
        recordReferenceArrayElementType(sourceElementType, context);
        recordReferenceArrayElementType(destinationElementType, context);
        const std::string runtimeName =
            arrayCopyRuntimeName(sourceElementType, destinationElementType);
        if (sourceType->name != destinationType->name &&
            context.runtimeArrayDeclarations != nullptr) {
          (*context.runtimeArrayDeclarations)[runtimeName] =
              arrayCopySignature(sourceType->name, destinationType->name);
        }
        std::vector<nir::Value> arguments;
        for (std::size_t i = 1; i < expression.children.size(); ++i) {
          arguments.push_back(expressionValueFor(expression.children[i], context));
        }
        return nir::callValue(nir::localValue(runtimeName, expression.span),
                              std::move(arguments), expression.span);
      }
    }
    if (expression.children.size() == 1 &&
        expression.children.front().kind == AstExpressionKind::Select &&
        expression.children.front().children.size() == 1 &&
        expression.children.front().text == support::StdNames::ArrayClone) {
      const frontend::AstExpression& receiver =
          expression.children.front().children.front();
      const frontend::TypeInfo* receiverType = annotatedTypeFor(receiver, context);
      const std::string elementType = receiverType == nullptr
                                          ? std::string{}
                                          : arrayElementTypeName(receiverType->name);
      if (!elementType.empty()) {
        std::vector<nir::Value> arguments;
        arguments.push_back(expressionValueFor(receiver, context));
        return nir::callValue(
            nir::localValue(arrayCloneRuntimeName(elementType, context),
                            expression.span),
            std::move(arguments), expression.span);
      }
    }
    if (expression.children.size() == 2 &&
        expression.children.front().kind == AstExpressionKind::Select &&
        expression.children.front().text == support::StdNames::Equals) {
      const frontend::AstExpression& callee = expression.children.front();
      const frontend::TypedDeclaration* selectedDeclaration =
          declarationForExpression(callee, context);
      if (selectedDeclaration == nullptr && callee.children.size() == 1) {
        const frontend::TypeInfo* receiverType =
            annotatedTypeFor(callee.children.front(), context);
        const frontend::TypeInfo* argumentType =
            annotatedTypeFor(expression.children[1], context);
        if (receiverType != nullptr &&
            receiverType->kind == frontend::SimpleTypeKind::String) {
          std::vector<nir::Value> arguments;
          arguments.push_back(expressionValueFor(callee.children.front(), context));
          arguments.push_back(
              boxForObjectStorage(expressionValueFor(expression.children[1], context),
                                  "Object", expression.children[1], context));
          return nir::callValue(
              nir::localValue(std::string(support::StdNames::RuntimeStringEquals),
                              expression.span),
              std::move(arguments), expression.span);
        }
        if (receiverType != nullptr &&
            receiverType->kind == frontend::SimpleTypeKind::Unit) {
          if (argumentType != nullptr && runtimeTypeName(*argumentType) == "Object") {
            return runtimeAnyEqualsValue(callee.children.front(),
                                         expression.children[1], expression.span,
                                         context);
          }
          if (argumentType != nullptr &&
              argumentType->kind == frontend::SimpleTypeKind::Unit) {
            return nir::literalValue("true", "Boolean", expression.span);
          }
          return nir::literalValue("false", "Boolean", expression.span);
        }
        if (receiverType != nullptr &&
            (receiverType->kind == frontend::SimpleTypeKind::Object ||
             receiverType->kind == frontend::SimpleTypeKind::Null)) {
          return runtimeAnyEqualsValue(callee.children.front(), expression.children[1],
                                       expression.span, context, true);
        }
        if (hasObjectRuntimeType(expression.children[1], context)) {
          return runtimeAnyEqualsValue(callee.children.front(), expression.children[1],
                                       expression.span, context);
        }
        return nir::binaryValue(
            "==", expressionValueFor(callee.children.front(), context),
            expressionValueFor(expression.children[1], context), expression.span);
      }
    }
    if (expression.children.size() == 1 &&
        expression.children.front().kind == AstExpressionKind::Select &&
        expression.children.front().text == support::StdNames::HashCode) {
      return valueFor(expression.children.front(), context);
    }
    if (expression.children.size() == 1 &&
        expression.children.front().kind == AstExpressionKind::Select &&
        (expression.children.front().text == support::StdNames::ToByte ||
         expression.children.front().text == support::StdNames::ToShort ||
         expression.children.front().text == support::StdNames::ToInt)) {
      return valueFor(expression.children.front(), context);
    }
    if (expression.children.size() == 1 &&
        expression.children.front().kind == AstExpressionKind::Select &&
        expression.children.front().text == support::StdNames::ToString) {
      return valueFor(expression.children.front(), context);
    }
    if (isZoneScopedCall(expression)) {
      if (expression.children.size() != 2) {
        return nir::unknownValue("malformed Zone.scoped", expression.span);
      }
      return nir::zoneScopedValue(scopedBodyValueFor(expression.children[1], context),
                                  expression.span);
    }
    if (isZoneAllocBytesCall(expression)) {
      if (expression.children.size() != 2) {
        return nir::unknownValue("malformed Zone.allocBytes", expression.span);
      }
      return nir::callValue(
          nir::localValue(std::string(support::StdNames::RuntimeZoneAllocBytes),
                          expression.span),
          {expressionValueFor(expression.children[1], context)}, expression.span);
    }
    if (isByteBufferWrapCall(expression)) {
      if (expression.children.size() != 2) {
        return nir::unknownValue("malformed ByteBuffer.wrap", expression.span);
      }
      return nir::callValue(
          nir::localValue(std::string(support::StdNames::RuntimeByteBufferWrap),
                          expression.span),
          {expressionValueFor(expression.children[1], context)}, expression.span);
    }
    if (const std::string runtimeName = byteBufferRuntimeName(expression, context);
        !runtimeName.empty()) {
      const frontend::AstExpression& callee = expression.children.front();
      std::vector<nir::Value> arguments{
          expressionValueFor(callee.children.front(), context)};
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        arguments.push_back(expressionValueFor(expression.children[i], context));
      }
      return nir::callValue(nir::localValue(runtimeName, expression.span),
                            std::move(arguments), expression.span);
    }
    if (const std::string_view operation = nativeBytesOperation(expression);
        !operation.empty()) {
      std::vector<nir::Value> arguments;
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        arguments.push_back(expressionValueFor(expression.children[i], context));
      }
      return nir::callValue(
          nir::localValue(nativeBytesRuntimeName(operation), expression.span),
          std::move(arguments), expression.span);
    }
    const frontend::AstExpression& arrayCallee = expression.children.front();
    const bool inferredArrayLiteral =
        arrayCallee.kind == AstExpressionKind::Identifier &&
        arrayCallee.text == "Array";
    const bool explicitArrayLiteral =
        arrayCallee.kind == AstExpressionKind::TypeApply &&
        arrayCallee.children.size() == 1 &&
        arrayCallee.children.front().kind == AstExpressionKind::Identifier &&
        arrayCallee.children.front().text == "Array";
    const bool dynamicArrayConstructor =
        arrayCallee.kind == AstExpressionKind::TypeApply &&
        arrayCallee.children.size() == 1 &&
        arrayCallee.children.front().kind == AstExpressionKind::New &&
        arrayCallee.children.front().text == "Array";
    const bool arrayRange =
        arrayCallee.kind == AstExpressionKind::Select &&
        arrayCallee.children.size() == 1 &&
        arrayCallee.text == support::StdNames::ArrayRange &&
        arrayCallee.children.front().kind == AstExpressionKind::Identifier &&
        arrayCallee.children.front().text == "Array";
    const bool arrayConcat =
        arrayCallee.kind == AstExpressionKind::TypeApply &&
        arrayCallee.children.size() == 1 &&
        arrayCallee.children.front().kind == AstExpressionKind::Select &&
        arrayCallee.children.front().children.size() == 1 &&
        arrayCallee.children.front().text == support::StdNames::ArrayConcat &&
        arrayCallee.children.front().children.front().kind ==
            AstExpressionKind::Identifier &&
        arrayCallee.children.front().children.front().text == "Array";
    const bool arrayOfDim =
        arrayCallee.kind == AstExpressionKind::TypeApply &&
        arrayCallee.children.size() == 1 &&
        arrayCallee.children.front().kind == AstExpressionKind::Select &&
        arrayCallee.children.front().children.size() == 1 &&
        arrayCallee.children.front().text == support::StdNames::ArrayOfDim &&
        arrayCallee.children.front().children.front().kind ==
            AstExpressionKind::Identifier &&
        arrayCallee.children.front().children.front().text == "Array";
    const frontend::AstExpression* arrayFillTypeApply = nullptr;
    if (arrayCallee.kind == AstExpressionKind::Call && !arrayCallee.children.empty() &&
        arrayCallee.children.front().kind == AstExpressionKind::TypeApply &&
        arrayCallee.children.front().children.size() == 1) {
      const frontend::AstExpression& candidate = arrayCallee.children.front();
      const frontend::AstExpression& selected = candidate.children.front();
      if (selected.kind == AstExpressionKind::Select && selected.children.size() == 1 &&
          selected.text == support::StdNames::ArrayFill &&
          selected.children.front().kind == AstExpressionKind::Identifier &&
          selected.children.front().text == "Array") {
        arrayFillTypeApply = &candidate;
      }
    }
    if (arrayFillTypeApply != nullptr && arrayCallee.children.size() >= 2 &&
        expression.children.size() == 2) {
      const frontend::TypeInfo* arrayType = annotatedTypeFor(expression, context);
      const std::size_t dimensions = arrayCallee.children.size() - 1;
      std::string elementType = arrayType == nullptr ? std::string{} : arrayType->name;
      for (std::size_t i = 0; i < dimensions && !elementType.empty(); ++i) {
        elementType = arrayElementTypeName(elementType);
      }
      if (arrayType == nullptr || elementType.empty() || dimensions == 0) {
        return nir::unknownValue("malformed Array.fill", expression.span);
      }
      recordReferenceArrayElementType(elementType, context);
      nir::Value element = expressionValueFor(expression.children[1], context);
      if (elementType == "Object") {
        if (const frontend::TypeInfo* type =
                annotatedTypeFor(expression.children[1], context)) {
          if (const std::string primitive = boxedObjectTypeName(type->kind);
              !primitive.empty()) {
            element = nir::boxValue(primitive, std::move(element),
                                    expression.children[1].span);
          }
        }
      }
      std::vector<nir::Value> arguments;
      for (std::size_t i = 1; i < arrayCallee.children.size(); ++i) {
        arguments.push_back(expressionValueFor(arrayCallee.children[i], context));
      }
      arguments.push_back(std::move(element));
      const std::string runtime = arrayFillRuntimeName(elementType, dimensions);
      if (dimensions > 1 && context.runtimeArrayDeclarations != nullptr) {
        context.runtimeArrayDeclarations->emplace(
            runtime, arrayFillSignature(elementType, dimensions, arrayType->name));
      }
      return nir::callValue(nir::localValue(runtime, expression.span),
                            std::move(arguments), expression.span);
    }
    if (arrayRange &&
        (expression.children.size() == 3 || expression.children.size() == 4)) {
      std::vector<nir::Value> arguments;
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        arguments.push_back(expressionValueFor(expression.children[i], context));
      }
      if (arguments.size() == 2) {
        arguments.push_back(nir::literalValue("1", "Int", expression.span));
      }
      return nir::callValue(
          nir::localValue(std::string(support::StdNames::RuntimeArrayRange),
                          expression.span),
          std::move(arguments), expression.span);
    }
    if (arrayConcat) {
      const frontend::TypeInfo* arrayType = annotatedTypeFor(expression, context);
      const std::string elementType =
          arrayType == nullptr ? std::string{} : arrayElementTypeName(arrayType->name);
      if (arrayType == nullptr || elementType.empty()) {
        return nir::unknownValue("malformed Array.concat", expression.span);
      }
      recordReferenceArrayElementType(elementType, context);
      std::vector<nir::Value> arguments;
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        arguments.push_back(expressionValueFor(expression.children[i], context));
      }
      const std::string runtime = arrayConcatRuntimeName(elementType, arguments.size());
      if (context.runtimeArrayDeclarations != nullptr) {
        context.runtimeArrayDeclarations->emplace(
            runtime, arrayConcatSignature(arrayType->name, arguments.size()));
      }
      return nir::callValue(nir::localValue(runtime, expression.span),
                            std::move(arguments), expression.span);
    }
    if (dynamicArrayConstructor) {
      const frontend::TypeInfo* arrayType = annotatedTypeFor(expression, context);
      const std::string elementType =
          arrayType == nullptr ? std::string{} : arrayElementTypeName(arrayType->name);
      if (elementType.empty() || expression.children.size() != 2) {
        return nir::unknownValue("malformed Array constructor", expression.span);
      }
      std::vector<nir::Value> arguments;
      arguments.push_back(expressionValueFor(expression.children[1], context));
      return nir::callValue(
          nir::localValue(arrayAllocRuntimeName(elementType, context), expression.span),
          std::move(arguments), expression.span);
    }
    if (arrayOfDim) {
      const frontend::TypeInfo* arrayType = annotatedTypeFor(expression, context);
      const std::size_t dimensions = expression.children.size() - 1;
      std::string elementType = arrayType == nullptr ? std::string{} : arrayType->name;
      for (std::size_t i = 0; i < dimensions && !elementType.empty(); ++i) {
        elementType = arrayElementTypeName(elementType);
      }
      if (arrayType == nullptr || elementType.empty() || dimensions == 0) {
        return nir::unknownValue("malformed Array.ofDim", expression.span);
      }

      std::vector<nir::Value> arguments;
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        arguments.push_back(expressionValueFor(expression.children[i], context));
      }
      if (dimensions == 1) {
        return nir::callValue(
            nir::localValue(arrayAllocRuntimeName(elementType, context),
                            expression.span),
            std::move(arguments), expression.span);
      }

      const std::string runtime = arrayOfDimRuntimeName(elementType, dimensions);
      if (context.runtimeArrayDeclarations != nullptr) {
        context.runtimeArrayDeclarations->emplace(
            runtime, arrayOfDimSignature(dimensions, arrayType->name));
      }
      return nir::callValue(nir::localValue(runtime, expression.span),
                            std::move(arguments), expression.span);
    }
    if (inferredArrayLiteral || explicitArrayLiteral) {
      const frontend::TypeInfo* arrayType = annotatedTypeFor(expression, context);
      const std::string elementType =
          arrayType == nullptr ? std::string{} : arrayElementTypeName(arrayType->name);
      if (elementType.empty()) {
        return nir::unknownValue("unsupported array element type", expression.span);
      }
      std::vector<nir::Value> elements;
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        nir::Value element = expressionValueFor(expression.children[i], context);
        if (elementType == "Object") {
          if (const frontend::TypeInfo* type =
                  annotatedTypeFor(expression.children[i], context)) {
            if (const std::string primitive = boxedObjectTypeName(type->kind);
                !primitive.empty()) {
              element = nir::boxValue(primitive, std::move(element),
                                      expression.children[i].span);
            }
          }
        }
        elements.push_back(std::move(element));
      }
      return nir::newValue("Array [ " + elementType + " ]", std::move(elements),
                           expression.span);
    }
    const frontend::AstExpression* constructor =
        expression.children.front().kind == AstExpressionKind::New
            ? &expression.children.front()
            : nullptr;
    if (expression.children.front().kind == AstExpressionKind::TypeApply &&
        expression.children.front().children.size() == 1 &&
        expression.children.front().children.front().kind == AstExpressionKind::New) {
      constructor = &expression.children.front().children.front();
    }
    if (constructor != nullptr) {
      const frontend::TypedDeclaration* classDeclaration =
          context.declarations == nullptr
              ? nullptr
              : findDeclarationBySymbol(*context.declarations,
                                        qualifyTypeName(constructor->text, context));
      std::vector<nir::Value> arguments;
      for (std::size_t i = 1; i < expression.children.size(); ++i) {
        nir::Value argument = expressionValueFor(expression.children[i], context);
        const std::size_t parameterIndex = i - 1;
        if (classDeclaration != nullptr &&
            parameterIndex < classDeclaration->parameterTypes.size()) {
          argument = boxForObjectStorage(
              std::move(argument),
              runtimeTypeName(classDeclaration->parameterTypes[parameterIndex]),
              expression.children[i], context);
        }
        arguments.push_back(std::move(argument));
      }
      return nir::newValue(qualifyTypeName(constructor->text, context),
                           std::move(arguments), expression.span);
    }
    if (expression.children.size() == 2) {
      if (const frontend::TypeInfo* arrayType =
              arrayReceiverTypeFor(expression.children.front(), context)) {
        const std::string elementType = arrayElementTypeName(arrayType->name);
        std::vector<nir::Value> arguments;
        arguments.push_back(expressionValueFor(expression.children.front(), context));
        arguments.push_back(expressionValueFor(expression.children[1], context));
        const std::string runtime = arrayApplyRuntimeName(elementType, context);
        return nir::callValue(nir::localValue(runtime, expression.span),
                              std::move(arguments), expression.span);
      }
    }
    const frontend::TypedDeclaration* target =
        declarationForExpression(expression.children.front(), context);
    const std::string targetReceiver =
        memberReceiverType(expression.children.front(), context);
    std::vector<nir::Value> arguments;
    for (std::size_t i = 1; i < expression.children.size(); ++i) {
      nir::Value argument = expressionValueFor(expression.children[i], context);
      const std::size_t parameterIndex = i - 1;
      const std::string primitive =
          target != nullptr && parameterIndex < target->parameterTypes.size()
              ? boxedPrimitiveTypeFor(target->parameterTypes[parameterIndex],
                                      targetReceiver, context)
              : std::string{};
      std::string boxedPrimitive = primitive;
      if (boxedPrimitive.empty() && target != nullptr &&
          parameterIndex < target->parameterTypes.size() &&
          runtimeTypeName(target->parameterTypes[parameterIndex]) == "Object") {
        if (const frontend::TypeInfo* argumentType =
                annotatedTypeFor(expression.children[i], context)) {
          boxedPrimitive = boxedObjectTypeName(argumentType->kind);
        }
      }
      if (!boxedPrimitive.empty()) {
        argument = nir::boxValue(boxedPrimitive, std::move(argument),
                                 expression.children[i].span);
      }
      arguments.push_back(std::move(argument));
    }
    if (const frontend::TypedContextApplication* application =
            contextApplicationFor(expression, context)) {
      for (const frontend::TypedContextArgument& contextual : application->arguments) {
        const std::size_t parameterIndex = arguments.size();
        frontend::AstExpression argumentExpression;
        argumentExpression.kind = AstExpressionKind::Identifier;
        argumentExpression.text = contextual.name;
        argumentExpression.span = expression.span;
        nir::Value argument;
        if (contextual.name.empty()) {
          argument = nir::unknownValue("<missing-given>", expression.span);
        } else if (contextual.requiresAccessor) {
          argument =
              nir::callValue(nir::localValue(contextual.symbolName, expression.span),
                             {}, expression.span);
        } else {
          argument = expressionValueFor(argumentExpression, context);
        }
        if (target != nullptr && parameterIndex < target->parameterTypes.size()) {
          const std::string targetType =
              runtimeTypeName(target->parameterTypes[parameterIndex]);
          if (targetType == "Object") {
            if (const std::string primitive = boxedObjectTypeName(contextual.type.kind);
                !primitive.empty()) {
              argument = nir::boxValue(primitive, std::move(argument), expression.span);
            }
          }
        }
        arguments.push_back(std::move(argument));
      }
    }
    nir::Value call =
        nir::callValue(expressionValueFor(expression.children.front(), context, true),
                       std::move(arguments), expression.span);
    const std::string returnPrimitive =
        target == nullptr
            ? std::string{}
            : boxedPrimitiveTypeFor(target->inferredType, targetReceiver, context);
    if (!returnPrimitive.empty()) {
      return nir::unboxValue(returnPrimitive, std::move(call), expression.span);
    }
    if (target != nullptr) {
      const frontend::TypeInfo* staticResult = annotatedTypeFor(expression, context);
      const std::string erasedResult = runtimeTypeName(target->inferredType);
      const std::string staticRuntime =
          staticResult == nullptr ? erasedResult : runtimeTypeName(*staticResult);
      if (erasedResult != staticRuntime && staticRuntime != "Object") {
        const std::string boxedType = staticResult == nullptr
                                          ? std::string{}
                                          : boxedObjectTypeName(staticResult->kind);
        if (!boxedType.empty()) {
          return nir::unboxValue(boxedType, std::move(call), expression.span);
        }
        return nir::asInstanceOfValue(staticRuntime, std::move(call), expression.span);
      }
    }
    return call;
  }
  case AstExpressionKind::Unary:
    if (expression.children.size() != 1) {
      return nir::unknownValue("<malformed-unary>", expression.span);
    }
    return nir::unaryValue(
        expression.text,
        expression.text == "+" || expression.text == "-"
            ? promoteNarrowIntegral(expression.children.front(), context)
            : expressionValueFor(expression.children.front(), context),
        expression.span);
  case AstExpressionKind::Binary:
    if (expression.children.size() != 2) {
      return nir::unknownValue("<malformed-binary>", expression.span);
    }
    if (expression.text == "==" || expression.text == "!=") {
      if (hasObjectRuntimeType(expression.children[0], context) ||
          hasObjectRuntimeType(expression.children[1], context)) {
        nir::Value equals = runtimeAnyEqualsValue(
            expression.children[0], expression.children[1], expression.span, context);
        if (expression.text == "!=") {
          return nir::unaryValue("!", std::move(equals), expression.span);
        }
        return equals;
      }
      const frontend::TypeInfo* lhsType =
          annotatedTypeFor(expression.children[0], context);
      const frontend::TypeInfo* rhsType =
          annotatedTypeFor(expression.children[1], context);
      if (lhsType != nullptr && rhsType != nullptr &&
          (lhsType->kind == frontend::SimpleTypeKind::Unit ||
           rhsType->kind == frontend::SimpleTypeKind::Unit)) {
        const bool equal = lhsType->kind == frontend::SimpleTypeKind::Unit &&
                           rhsType->kind == frontend::SimpleTypeKind::Unit;
        const bool result = expression.text == "==" ? equal : !equal;
        return nir::literalValue(result ? "true" : "false", "Boolean", expression.span);
      }
    }
    return nir::binaryValue(
        expression.text, promoteNarrowIntegral(expression.children[0], context),
        promoteNarrowIntegral(expression.children[1], context), expression.span);
  case AstExpressionKind::Assign: {
    if (expression.children.size() != 2) {
      return nir::unknownValue("<malformed-assign>", expression.span);
    }
    if (expression.children.front().kind == AstExpressionKind::Call &&
        expression.children.front().children.size() == 2) {
      const frontend::AstExpression& target = expression.children.front();
      const frontend::AstExpression& arrayExpression = target.children.front();
      if (const frontend::TypeInfo* arrayType =
              arrayReceiverTypeFor(arrayExpression, context)) {
        const std::string elementType = arrayElementTypeName(arrayType->name);
        std::vector<nir::Value> arguments;
        arguments.push_back(expressionValueFor(arrayExpression, context));
        arguments.push_back(expressionValueFor(target.children[1], context));
        nir::Value assignedValue = expressionValueFor(expression.children[1], context);
        if (elementType == "Object") {
          if (const frontend::TypeInfo* type =
                  annotatedTypeFor(expression.children[1], context)) {
            if (const std::string primitive = boxedObjectTypeName(type->kind);
                !primitive.empty()) {
              assignedValue = nir::boxValue(primitive, std::move(assignedValue),
                                            expression.children[1].span);
            }
          }
        }
        arguments.push_back(std::move(assignedValue));
        const std::string runtime = arrayUpdateRuntimeName(elementType, context);
        return nir::callValue(nir::localValue(runtime, expression.span),
                              std::move(arguments), expression.span);
      }
    }
    nir::Value assignedValue = expressionValueFor(expression.children[1], context);
    std::string targetRuntimeType;
    if (const frontend::TypeInfo* targetType =
            annotatedTypeFor(expression.children[0], context)) {
      targetRuntimeType = runtimeTypeName(*targetType);
    }
    if (expression.children[0].kind == AstExpressionKind::Select) {
      const frontend::TypedDeclaration* selectedDeclaration =
          declarationForExpression(expression.children[0], context);
      const std::string selectedReceiver =
          memberReceiverType(expression.children[0], context);
      const frontend::TypeInfo* erasedSelectedType =
          selectedDeclaration == nullptr
              ? findConstructorParameterType(context, selectedReceiver,
                                             expression.children[0].text)
              : &selectedDeclaration->inferredType;
      if (erasedSelectedType != nullptr) {
        targetRuntimeType = runtimeTypeName(*erasedSelectedType);
      }
    }
    if (!targetRuntimeType.empty()) {
      assignedValue = boxForObjectStorage(std::move(assignedValue), targetRuntimeType,
                                          expression.children[1], context);
    }
    if (expression.children[0].kind == AstExpressionKind::Identifier &&
        !context.localNames.contains(expression.children[0].text)) {
      const frontend::TypedDeclaration* target =
          declarationForExpression(expression.children[0], context);
      const frontend::TypedDeclaration* owner =
          context.declarations == nullptr
              ? nullptr
              : findDeclarationBySymbol(*context.declarations, context.currentOwner);
      if (target != nullptr && owner != nullptr &&
          owner->kind == frontend::AstDeclarationKind::Object &&
          target->kind == frontend::AstDeclarationKind::Var &&
          runtimeTypeName(target->inferredType) == "Object") {
        std::vector<nir::Value> arguments;
        arguments.push_back(std::move(assignedValue));
        return nir::callValue(
            nir::localValue(context.currentOwner + "." + setterName(target->name),
                            expression.span),
            std::move(arguments), expression.span);
      }
    }
    return nir::assignValue(expressionValueFor(expression.children[0], context, true),
                            std::move(assignedValue), expression.span);
  }
  case AstExpressionKind::If:
    if (expression.children.size() < 2) {
      return nir::unknownValue("<malformed-if>", expression.span);
    }
    if (expression.children.size() == 2) {
      return nir::ifValue(expressionValueFor(expression.children[0], context),
                          scopedBodyValueFor(expression.children[1], context),
                          nir::unitValue(expression.span), expression.span);
    }
    return nir::ifValue(expressionValueFor(expression.children[0], context),
                        scopedBodyValueFor(expression.children[1], context),
                        scopedBodyValueFor(expression.children[2], context),
                        expression.span);
  case AstExpressionKind::While:
    if (expression.children.size() != 2) {
      return nir::unknownValue("<malformed-while>", expression.span);
    }
    return nir::whileValue(expressionValueFor(expression.children[0], context),
                           scopedBodyValueFor(expression.children[1], context),
                           expression.span);
  }
  return nir::unknownValue("<unknown-expression>", expression.span);
}

nir::FunctionBody bodyFor(const frontend::TypedDeclaration& declaration,
                          const ValueContext& context,
                          const std::string& receiverType = {}) {
  nir::FunctionBodyBuilder body;
  ValueContext bodyContext = context;
  if (!receiverType.empty()) {
    (void)body.addParameter("this", receiverType, declaration.span);
    bodyContext.localNames.insert("this");
  }
  for (std::size_t i = 0; i < declaration.parameters.size(); ++i) {
    const std::string& parameter = declaration.parameters[i];
    const std::string name = parameterName(parameter);
    const frontend::TypeInfo* semanticType = i < declaration.parameterTypes.size()
                                                 ? &declaration.parameterTypes[i]
                                                 : nullptr;
    const std::string type = semanticType != nullptr
                                 ? runtimeTypeName(*semanticType)
                                 : parameterTypeName(parameter, context);
    const std::string primitive =
        semanticType == nullptr ? std::string{} : boxedPrimitiveType(*semanticType);
    if (!primitive.empty()) {
      const std::string abiName = name + "$boxed";
      (void)body.addParameter(abiName, type, declaration.span);
      (void)body.addLet(name, primitive,
                        nir::unboxValue(primitive,
                                        nir::localValue(abiName, declaration.span),
                                        declaration.span),
                        declaration.span);
    } else if (semanticType != nullptr &&
               semanticType->kind == frontend::SimpleTypeKind::Object &&
               !semanticType->typeParameter && !semanticType->abstractTypeMember &&
               semanticType->typeConstructorName.empty() &&
               runtimeTypeName(*semanticType) != semanticType->name &&
               semanticType->name != "Object") {
      const std::string abiName = name + "$erased";
      (void)body.addParameter(abiName, type, declaration.span);
      (void)body.addLet(
          name, semanticType->name,
          nir::asInstanceOfValue(semanticType->name,
                                 nir::localValue(abiName, declaration.span),
                                 declaration.span),
          declaration.span);
    } else {
      (void)body.addParameter(name, type, declaration.span);
    }
    bodyContext.localNames.insert(name);
  }
  appendExpressionSetup(declaration.initializer, body, bodyContext);
  ValueContext resultContext = bodyContext;
  const frontend::AstExpression* resultExpression = &declaration.initializer;
  while (resultExpression->kind == frontend::AstExpressionKind::Block) {
    resultContext.lexicalScopes.push_back(resultExpression->span);
    if (resultExpression->children.empty()) {
      break;
    }
    for (std::size_t i = 0; i + 1 < resultExpression->children.size(); ++i) {
      const frontend::AstExpression& child = resultExpression->children[i];
      if (child.kind == frontend::AstExpressionKind::LocalDeclaration) {
        resultContext.localNames.insert(child.text);
      }
    }
    resultExpression = &resultExpression->children.back();
  }
  if (resultExpression->kind == frontend::AstExpressionKind::Throw &&
      resultExpression->children.size() == 1) {
    (void)body.addThrow(valueFor(resultExpression->children.front(), resultContext),
                        resultExpression->span, std::move(resultContext.lexicalScopes));
    return std::move(body).build();
  }

  nir::Value result = valueFor(declaration.initializer, bodyContext);
  if (const std::string primitive = boxedPrimitiveType(declaration.inferredType);
      !primitive.empty()) {
    result = nir::boxValue(primitive, std::move(result), declaration.span);
  } else {
    result = boxForObjectStorage(std::move(result),
                                 runtimeTypeName(declaration.inferredType),
                                 declaration.initializer, bodyContext);
  }
  (void)body.addReturn(runtimeTypeName(declaration.inferredType), std::move(result),
                       declaration.span, std::move(resultContext.lexicalScopes));
  return std::move(body).build();
}

void appendClassInitializerItems(const frontend::TypedDeclaration& declaration,
                                 nir::FunctionBodyBuilder& body,
                                 ValueContext& bodyContext) {
  for (const frontend::AstClassBodyItem& item : declaration.classBodyItems) {
    if (item.kind == frontend::AstClassBodyItemKind::Declaration) {
      if (item.index >= declaration.members.size()) {
        continue;
      }
      const frontend::TypedDeclaration& member = declaration.members[item.index];
      if (!isClassInitializerItem(member)) {
        continue;
      }
      appendExpressionSetup(member.initializer, body, bodyContext);
      nir::Value initializer = valueFor(member.initializer, bodyContext);
      if (const std::string primitive = boxedPrimitiveType(member.inferredType);
          !primitive.empty()) {
        initializer =
            nir::boxValue(primitive, std::move(initializer), member.initializer.span);
      } else {
        initializer = boxForObjectStorage(std::move(initializer),
                                          runtimeTypeName(member.inferredType),
                                          member.initializer, bodyContext);
      }
      (void)body.addEval(
          nir::assignValue(nir::selectValue(nir::localValue("this", member.span),
                                            storedFieldName(member), member.span),
                           std::move(initializer), member.span),
          member.span);
      continue;
    }

    if (item.index >= declaration.constructorBody.size()) {
      continue;
    }
    const frontend::AstExpression& expression = declaration.constructorBody[item.index];
    appendExpressionSetup(expression, body, bodyContext);
    (void)body.addEval(valueFor(expression, bodyContext), expression.span);
  }
}

void appendModuleInitializerItems(const frontend::TypedDeclaration& declaration,
                                  const std::string& moduleName,
                                  nir::FunctionBodyBuilder& body,
                                  ValueContext& bodyContext) {
  for (const frontend::AstClassBodyItem& item : declaration.classBodyItems) {
    if (item.kind == frontend::AstClassBodyItemKind::Declaration) {
      if (item.index >= declaration.members.size()) {
        continue;
      }
      const frontend::TypedDeclaration& member = declaration.members[item.index];
      if (!isModuleStoredField(member, frontend::AstDeclarationKind::Object)) {
        continue;
      }
      appendExpressionSetup(member.initializer, body, bodyContext);
      nir::Value initializer = valueFor(member.initializer, bodyContext);
      if (const std::string primitive = boxedPrimitiveType(member.inferredType);
          !primitive.empty()) {
        initializer =
            nir::boxValue(primitive, std::move(initializer), member.initializer.span);
      } else {
        initializer = boxForObjectStorage(std::move(initializer),
                                          runtimeTypeName(member.inferredType),
                                          member.initializer, bodyContext);
      }
      (void)body.addEval(
          nir::assignValue(nir::selectValue(nir::localValue(moduleName, member.span),
                                            moduleStoredFieldName(member), member.span),
                           std::move(initializer), member.span),
          member.span);
      continue;
    }

    if (item.index >= declaration.constructorBody.size()) {
      continue;
    }
    const frontend::AstExpression& expression = declaration.constructorBody[item.index];
    appendExpressionSetup(expression, body, bodyContext);
    (void)body.addEval(valueFor(expression, bodyContext), expression.span);
  }
}

nir::FunctionBody
moduleInitializerBodyFor(const frontend::TypedDeclaration& declaration,
                         const ValueContext& context, const std::string& moduleName) {
  nir::FunctionBodyBuilder body;
  ValueContext bodyContext = context;
  bodyContext.currentOwner = moduleName;
  appendModuleInitializerItems(declaration, moduleName, body, bodyContext);
  (void)body.addReturn("Unit", nir::unitValue(declaration.span), declaration.span);
  return std::move(body).build();
}

void appendMaterializedTraitInitializerItems(
    const frontend::TypedModule& module, const frontend::TypedDeclaration& declaration,
    nir::FunctionBodyBuilder& body, const ValueContext& classContext) {
  std::vector<MaterializedTraitValue> values =
      materializedTraitValuesForClass(module, declaration);
  for (auto value = values.rbegin(); value != values.rend(); ++value) {
    if (value->owner == nullptr || value->member == nullptr) {
      continue;
    }
    ValueContext traitContext = classContext;
    traitContext.hasImplicitReceiver = true;
    traitContext.currentOwner = value->owner->symbolName;
    traitContext.stackableSuper = true;
    traitContext.implicitThisMembers =
        implicitThisMembersFor(module.declarations, value->owner->symbolName);
    traitContext.superTypes = value->owner->parentTypes;
    traitContext.superType = superTypeName(*value->owner);
    traitContext.localNames.insert("this");
    appendExpressionSetup(value->member->initializer, body, traitContext);
    nir::Value initializer = valueFor(value->member->initializer, traitContext);
    if (const std::string primitive = boxedPrimitiveType(value->member->inferredType);
        !primitive.empty()) {
      initializer = nir::boxValue(primitive, std::move(initializer),
                                  value->member->initializer.span);
    } else {
      initializer = boxForObjectStorage(std::move(initializer),
                                        runtimeTypeName(value->member->inferredType),
                                        value->member->initializer, traitContext);
    }
    (void)body.addEval(
        nir::assignValue(
            nir::selectValue(nir::localValue("this", value->member->span),
                             traitValueFieldName(*value->owner, *value->member),
                             value->member->span),
            std::move(initializer), value->member->span),
        value->member->span);
  }
}

void appendParentInitialization(const frontend::TypedModule& module,
                                const frontend::TypedDeclaration& declaration,
                                nir::FunctionBodyBuilder& body,
                                ValueContext& bodyContext) {
  if (declaration.declaredType.empty()) {
    return;
  }

  const frontend::TypedDeclaration* parent =
      findDeclarationBySymbol(module.declarations, declaration.declaredType);
  if (parent == nullptr || parent->kind != frontend::AstDeclarationKind::Class) {
    return;
  }

  const std::size_t checkedArguments =
      std::min(declaration.parentArguments.size(), parent->parameters.size());
  for (std::size_t i = 0; i < checkedArguments; ++i) {
    const frontend::AstExpression& argument = declaration.parentArguments[i];
    const std::string name = storedParameterFieldName(*parent, parent->parameters[i]);
    if (name.empty()) {
      continue;
    }
    appendExpressionSetup(argument, body, bodyContext);
    nir::Value initializedValue = valueFor(argument, bodyContext);
    if (i < parent->parameterTypes.size()) {
      initializedValue = boxForObjectStorage(std::move(initializedValue),
                                             runtimeTypeName(parent->parameterTypes[i]),
                                             argument, bodyContext);
    }
    (void)body.addEval(
        nir::assignValue(
            nir::selectValue(nir::superValue(parent->symbolName, argument.span), name,
                             argument.span),
            std::move(initializedValue), argument.span),
        argument.span);
  }

  appendParentInitialization(module, *parent, body, bodyContext);
  appendMaterializedTraitInitializerItems(module, *parent, body, bodyContext);
  appendClassInitializerItems(*parent, body, bodyContext);
}

nir::FunctionBody constructorBodyFor(const frontend::TypedModule& module,
                                     const frontend::TypedDeclaration& declaration,
                                     const ValueContext& context,
                                     const std::string& receiverType) {
  nir::FunctionBodyBuilder body;
  ValueContext bodyContext = context;
  (void)body.addParameter("this", receiverType, declaration.span);
  bodyContext.localNames.insert("this");
  appendParentInitialization(module, declaration, body, bodyContext);
  appendMaterializedTraitInitializerItems(module, declaration, body, bodyContext);
  appendClassInitializerItems(declaration, body, bodyContext);
  (void)body.addReturn("Unit", nir::unitValue(declaration.span), declaration.span);
  return std::move(body).build();
}

const frontend::TypedDeclaration* findMainDeclaration(
    const std::vector<frontend::TypedDeclaration>& declarations,
    frontend::AstDeclarationKind ownerKind = frontend::AstDeclarationKind::Package) {
  for (const frontend::TypedDeclaration& declaration : declarations) {
    if (declaration.kind == frontend::AstDeclarationKind::Def &&
        declaration.name == "main" &&
        ownerKind == frontend::AstDeclarationKind::Object) {
      if (declaration.parameters.empty()) {
        return &declaration;
      }
      if (declaration.parameters.size() == 1 &&
          isStringArrayTypeName(rawParameterTypeName(declaration.parameters.front()))) {
        return &declaration;
      }
    }
    if (const frontend::TypedDeclaration* nested =
            findMainDeclaration(declaration.members, declaration.kind)) {
      return nested;
    }
  }
  return nullptr;
}

nir::FunctionBody
runtimeMainBodyFor(const frontend::TypedDeclaration& mainDeclaration) {
  nir::FunctionBodyBuilder body;
  std::vector<nir::Value> arguments;
  if (mainDeclaration.parameters.size() == 1) {
    const std::string argsType =
        mainDeclaration.parameterTypes.empty()
            ? rawParameterTypeName(mainDeclaration.parameters.front())
            : runtimeTypeName(mainDeclaration.parameterTypes.front());
    (void)body.addParameter("args", argsType, mainDeclaration.span);
    arguments.push_back(nir::localValue("args", mainDeclaration.span));
  }
  nir::Value call =
      nir::callValue(nir::localValue(mainDeclaration.symbolName, mainDeclaration.span),
                     std::move(arguments), mainDeclaration.span);
  if (mainDeclaration.inferredType.name == "Int") {
    (void)body.addReturn("Int", std::move(call), mainDeclaration.span);
  } else {
    (void)body.addEval(std::move(call), mainDeclaration.span);
    (void)body.addReturn("Int", nir::literalValue("0", "Int", mainDeclaration.span),
                         mainDeclaration.span);
  }
  return std::move(body).build();
}

std::string runtimeMainSignatureFor(const frontend::TypedDeclaration& mainDeclaration) {
  if (mainDeclaration.parameters.size() != 1) {
    return "()Int";
  }
  const std::string argsType =
      mainDeclaration.parameterTypes.empty()
          ? rawParameterTypeName(mainDeclaration.parameters.front())
          : runtimeTypeName(mainDeclaration.parameterTypes.front());
  return "(" + argsType + ")Int";
}

nir::FunctionBody standardThrowableMessageBody() {
  const support::SourceSpan noSpan = support::SourceSpan::none();
  nir::FunctionBodyBuilder body;
  (void)body.addParameter("this", std::string(support::StdNames::JavaLangThrowable),
                          noSpan);
  (void)body.addReturn(
      "String",
      nir::selectValue(nir::localValue("this", noSpan), "message$field", noSpan),
      noSpan);
  return std::move(body).build();
}

nir::FunctionBody standardThrowableTraceInitializerBody() {
  const support::SourceSpan noSpan = support::SourceSpan::none();
  nir::FunctionBodyBuilder body;
  (void)body.addParameter("this", std::string(support::StdNames::JavaLangThrowable),
                          noSpan);
  (void)body.addReturn("Long", nir::literalValue("0L", "Long", noSpan), noSpan);
  return std::move(body).build();
}

nir::FunctionBody standardLiteralFieldInitializerBody(std::string receiverType,
                                                      std::string fieldType,
                                                      std::string literal) {
  const support::SourceSpan noSpan = support::SourceSpan::none();
  nir::FunctionBodyBuilder body;
  (void)body.addParameter("this", std::move(receiverType), noSpan);
  (void)body.addReturn(
      fieldType, nir::literalValue(std::move(literal), fieldType, noSpan), noSpan);
  return std::move(body).build();
}

nir::FunctionBody standardThrowableCauseBody() {
  const support::SourceSpan noSpan = support::SourceSpan::none();
  const std::string throwableName(support::StdNames::JavaLangThrowable);
  nir::FunctionBodyBuilder body;
  (void)body.addParameter("this", throwableName, noSpan);
  nir::Value uninitialized = nir::binaryValue(
      "==", nir::selectValue(nir::localValue("this", noSpan), "cause", noSpan),
      nir::localValue("this", noSpan), noSpan);
  uninitialized.type = "Boolean";
  nir::Value visibleCause = nir::ifValue(
      std::move(uninitialized), nir::literalValue("null", throwableName, noSpan),
      nir::selectValue(nir::localValue("this", noSpan), "cause", noSpan), noSpan);
  visibleCause.type = throwableName;
  (void)body.addReturn(throwableName, std::move(visibleCause), noSpan);
  return std::move(body).build();
}

nir::FunctionBody standardThrowableInitCauseBody() {
  const support::SourceSpan noSpan = support::SourceSpan::none();
  const std::string throwableName(support::StdNames::JavaLangThrowable);
  nir::FunctionBodyBuilder body;
  (void)body.addParameter("this", throwableName, noSpan);
  (void)body.addParameter("cause", throwableName, noSpan);

  nir::Value initialized = nir::binaryValue(
      "!=", nir::selectValue(nir::localValue("this", noSpan), "cause", noSpan),
      nir::localValue("this", noSpan), noSpan);
  initialized.type = "Boolean";
  nir::Value repeatedFailure = nir::throwValue(
      nir::newValue(
          std::string(support::StdNames::JavaLangIllegalStateException),
          {nir::literalValue("\"Cause already initialized\"", "String", noSpan)},
          noSpan),
      noSpan);

  nir::Value selfCause = nir::binaryValue("==", nir::localValue("cause", noSpan),
                                          nir::localValue("this", noSpan), noSpan);
  selfCause.type = "Boolean";
  nir::Value selfFailure = nir::throwValue(
      nir::newValue(
          std::string(support::StdNames::JavaLangIllegalArgumentException),
          {nir::literalValue("\"An exception cannot cause itself\"", "String", noSpan)},
          noSpan),
      noSpan);

  nir::Value returnedThis = nir::localValue("this", noSpan);
  returnedThis.type = throwableName;
  nir::Value initializedCause = nir::blockValue(
      {nir::assignValue(
           nir::selectValue(nir::localValue("this", noSpan), "cause", noSpan),
           nir::localValue("cause", noSpan), noSpan),
       std::move(returnedThis)},
      noSpan);
  initializedCause.type = throwableName;
  nir::Value checkedSelf = nir::ifValue(std::move(selfCause), std::move(selfFailure),
                                        std::move(initializedCause), noSpan);
  checkedSelf.type = throwableName;
  nir::Value checkedInitialization =
      nir::ifValue(std::move(initialized), std::move(repeatedFailure),
                   std::move(checkedSelf), noSpan);
  checkedInitialization.type = throwableName;
  (void)body.addReturn(throwableName, std::move(checkedInitialization), noSpan);
  return std::move(body).build();
}

nir::FunctionBody standardThrowableToStringBody() {
  const support::SourceSpan noSpan = support::SourceSpan::none();
  nir::FunctionBodyBuilder body;
  (void)body.addParameter("this", std::string(support::StdNames::JavaLangThrowable),
                          noSpan);
  std::vector<nir::Value> arguments;
  arguments.push_back(nir::localValue("this", noSpan));
  (void)body.addReturn(
      "String",
      nir::callValue(
          nir::localValue(std::string(support::StdNames::RuntimeThrowableToString),
                          noSpan),
          std::move(arguments), noSpan),
      noSpan);
  return std::move(body).build();
}

nir::FunctionBody standardThrowablePrintStackTraceBody() {
  const support::SourceSpan noSpan = support::SourceSpan::none();
  nir::FunctionBodyBuilder body;
  (void)body.addParameter("this", std::string(support::StdNames::JavaLangThrowable),
                          noSpan);
  std::vector<nir::Value> arguments;
  arguments.push_back(nir::localValue("this", noSpan));
  nir::Value printed = nir::callValue(
      nir::localValue(std::string(support::StdNames::RuntimePrintStackTrace), noSpan),
      std::move(arguments), noSpan);
  printed.type = "Unit";
  (void)body.addReturn("Unit", std::move(printed), noSpan);
  return std::move(body).build();
}

nir::FunctionBody standardThrowableFillInStackTraceBody() {
  const support::SourceSpan noSpan = support::SourceSpan::none();
  const std::string throwableName(support::StdNames::JavaLangThrowable);
  nir::FunctionBodyBuilder body;
  (void)body.addParameter("this", throwableName, noSpan);
  std::vector<nir::Value> arguments;
  arguments.push_back(nir::localValue("this", noSpan));
  (void)body.addReturn(
      throwableName,
      nir::callValue(
          nir::localValue(std::string(support::StdNames::RuntimeFillInStackTrace),
                          noSpan),
          std::move(arguments), noSpan),
      noSpan);
  return std::move(body).build();
}

nir::FunctionBody standardThrowableGetStackTraceBody() {
  const support::SourceSpan noSpan = support::SourceSpan::none();
  const std::string throwableName(support::StdNames::JavaLangThrowable);
  const std::string arrayType =
      "Array [ " + std::string(support::StdNames::JavaLangStackTraceElement) + " ]";
  nir::FunctionBodyBuilder body;
  (void)body.addParameter("this", throwableName, noSpan);
  std::vector<nir::Value> arguments;
  arguments.push_back(nir::localValue("this", noSpan));
  (void)body.addReturn(
      arrayType,
      nir::callValue(
          nir::localValue(std::string(support::StdNames::RuntimeGetStackTrace), noSpan),
          std::move(arguments), noSpan),
      noSpan);
  return std::move(body).build();
}

nir::FunctionBody standardThrowableSetStackTraceBody() {
  const support::SourceSpan noSpan = support::SourceSpan::none();
  const std::string throwableName(support::StdNames::JavaLangThrowable);
  const std::string arrayType =
      "Array [ " + std::string(support::StdNames::JavaLangStackTraceElement) + " ]";
  nir::FunctionBodyBuilder body;
  (void)body.addParameter("this", throwableName, noSpan);
  (void)body.addParameter("stackTrace", arrayType, noSpan);
  std::vector<nir::Value> arguments;
  arguments.push_back(nir::localValue("this", noSpan));
  arguments.push_back(nir::localValue("stackTrace", noSpan));
  nir::Value status = nir::callValue(
      nir::localValue(std::string(support::StdNames::RuntimeSetStackTrace), noSpan),
      std::move(arguments), noSpan);
  status.type = "Int";
  nir::Value nullArray =
      nir::binaryValue("==", status, nir::literalValue("1", "Int", noSpan), noSpan);
  nullArray.type = "Boolean";
  nir::Value nullArrayFailure = nir::throwValue(
      nir::newValue(
          std::string(support::StdNames::JavaLangNullPointerException),
          {nir::literalValue("\"Stack trace cannot be null\"", "String", noSpan)},
          noSpan),
      noSpan);

  nir::Value nullFrame =
      nir::binaryValue("==", status, nir::literalValue("2", "Int", noSpan), noSpan);
  nullFrame.type = "Boolean";
  nir::Value nullFrameFailure = nir::throwValue(
      nir::newValue(std::string(support::StdNames::JavaLangNullPointerException),
                    {nir::literalValue("\"Stack trace cannot contain null elements\"",
                                       "String", noSpan)},
                    noSpan),
      noSpan);
  nir::Value checkedFrame =
      nir::ifValue(std::move(nullFrame), std::move(nullFrameFailure),
                   nir::unitValue(noSpan), noSpan);
  checkedFrame.type = "Unit";
  nir::Value checkedArray =
      nir::ifValue(std::move(nullArray), std::move(nullArrayFailure),
                   std::move(checkedFrame), noSpan);
  checkedArray.type = "Unit";
  (void)body.addReturn("Unit", std::move(checkedArray), noSpan);
  return std::move(body).build();
}

nir::FunctionBody standardThrowableAddSuppressedBody() {
  const support::SourceSpan noSpan = support::SourceSpan::none();
  const std::string throwableName(support::StdNames::JavaLangThrowable);
  nir::FunctionBodyBuilder body;
  (void)body.addParameter("this", throwableName, noSpan);
  (void)body.addParameter("exception", throwableName, noSpan);

  nir::Value isNull =
      nir::binaryValue("==", nir::localValue("exception", noSpan),
                       nir::literalValue("null", throwableName, noSpan), noSpan);
  isNull.type = "Boolean";
  nir::Value nullFailure = nir::throwValue(
      nir::newValue(std::string(support::StdNames::JavaLangIllegalArgumentException),
                    {nir::literalValue("\"Suppressed exception cannot be null\"",
                                       "String", noSpan)},
                    noSpan),
      noSpan);

  nir::Value isSelf = nir::binaryValue("==", nir::localValue("exception", noSpan),
                                       nir::localValue("this", noSpan), noSpan);
  isSelf.type = "Boolean";
  nir::Value selfFailure = nir::throwValue(
      nir::newValue(std::string(support::StdNames::JavaLangIllegalArgumentException),
                    {nir::literalValue("\"An exception cannot suppress itself\"",
                                       "String", noSpan)},
                    noSpan),
      noSpan);

  std::vector<nir::Value> arguments;
  arguments.push_back(nir::localValue("this", noSpan));
  arguments.push_back(nir::localValue("exception", noSpan));
  nir::Value addition = nir::callValue(
      nir::localValue(std::string(support::StdNames::RuntimeAddSuppressed), noSpan),
      std::move(arguments), noSpan);
  addition.type = "Unit";
  nir::Value added =
      nir::blockValue({std::move(addition), nir::unitValue(noSpan)}, noSpan);
  added.type = "Unit";
  nir::Value checkedSelf =
      nir::ifValue(std::move(isSelf), std::move(selfFailure), std::move(added), noSpan);
  checkedSelf.type = "Unit";
  nir::Value checkedNull = nir::ifValue(std::move(isNull), std::move(nullFailure),
                                        std::move(checkedSelf), noSpan);
  checkedNull.type = "Unit";
  (void)body.addReturn("Unit", std::move(checkedNull), noSpan);
  return std::move(body).build();
}

nir::FunctionBody standardThrowableGetSuppressedBody() {
  const support::SourceSpan noSpan = support::SourceSpan::none();
  const std::string throwableName(support::StdNames::JavaLangThrowable);
  const std::string arrayType = "Array [ " + throwableName + " ]";
  nir::FunctionBodyBuilder body;
  (void)body.addParameter("this", throwableName, noSpan);
  std::vector<nir::Value> arguments;
  arguments.push_back(nir::localValue("this", noSpan));
  (void)body.addReturn(
      arrayType,
      nir::callValue(
          nir::localValue(std::string(support::StdNames::RuntimeGetSuppressed), noSpan),
          std::move(arguments), noSpan),
      noSpan);
  return std::move(body).build();
}

nir::FunctionBody standardStackTraceElementToStringBody() {
  const support::SourceSpan noSpan = support::SourceSpan::none();
  nir::FunctionBodyBuilder body;
  (void)body.addParameter(
      "this", std::string(support::StdNames::JavaLangStackTraceElement), noSpan);
  std::vector<nir::Value> arguments;
  arguments.push_back(nir::localValue("this", noSpan));
  (void)body.addReturn(
      "String",
      nir::callValue(
          nir::localValue(
              std::string(support::StdNames::RuntimeStackTraceElementToString), noSpan),
          std::move(arguments), noSpan),
      noSpan);
  return std::move(body).build();
}

nir::FunctionBody standardExceptionToStringBody() {
  const support::SourceSpan noSpan = support::SourceSpan::none();
  nir::FunctionBodyBuilder body;
  (void)body.addParameter("this", std::string(support::StdNames::JavaLangException),
                          noSpan);
  std::vector<nir::Value> arguments;
  arguments.push_back(nir::localValue("this", noSpan));
  (void)body.addReturn(
      "String",
      nir::callValue(
          nir::localValue(std::string(support::StdNames::RuntimeThrowableToString),
                          noSpan),
          std::move(arguments), noSpan),
      noSpan);
  return std::move(body).build();
}

} // namespace

nir::Module NirEmitter::emit(const frontend::TypedModule& module) const {
  std::string moduleName = module.packageName.empty() ? "root" : module.packageName;
  nir::ModuleBuilder builder(moduleName);
  std::unordered_set<std::string> referenceArrayElementTypes;
  std::map<std::string, std::string> runtimeArrayDeclarations;

  for (const frontend::TypedDeclaration& declaration : module.declarations) {
    emitDeclaration(builder, module, declaration, "",
                    frontend::AstDeclarationKind::Package, &referenceArrayElementTypes,
                    &runtimeArrayDeclarations);
  }

  const support::SourceSpan noSpan = support::SourceSpan::none();
  const std::string throwableName(support::StdNames::JavaLangThrowable);
  const std::string suppressedNodeName(
      support::StdNames::RuntimeSuppressedExceptionNode);
  builder.addDefinition(
      nir::Definition{nir::DefinitionKind::Class,
                      suppressedNodeName,
                      "@" + std::string(support::StdNames::JavaLangObject),
                      {},
                      noSpan});
  builder.addDefinition(nir::Definition{
      nir::DefinitionKind::Field,
      suppressedNodeName + "." +
          std::string(support::StdNames::SuppressedExceptionValue),
      throwableName,
      standardLiteralFieldInitializerBody(suppressedNodeName, throwableName, "null"),
      noSpan});
  builder.addDefinition(
      nir::Definition{nir::DefinitionKind::Field,
                      suppressedNodeName + "." +
                          std::string(support::StdNames::SuppressedExceptionNext),
                      suppressedNodeName,
                      standardLiteralFieldInitializerBody(suppressedNodeName,
                                                          suppressedNodeName, "null"),
                      noSpan});
  builder.addDefinition(nir::Definition{
      nir::DefinitionKind::Field,
      throwableName + "." + std::string(support::StdNames::ThrowableSuppressedHead),
      suppressedNodeName,
      standardLiteralFieldInitializerBody(throwableName, suppressedNodeName, "null"),
      noSpan});
  builder.addDefinition(nir::Definition{
      nir::DefinitionKind::Field,
      throwableName + "." + std::string(support::StdNames::ThrowableSuppressedCount),
      "Int", standardLiteralFieldInitializerBody(throwableName, "Int", "0"), noSpan});

  builder.addDefinition(nir::Definition{
      nir::DefinitionKind::Field,
      std::string(support::StdNames::JavaLangThrowable) + "." +
          std::string(support::StdNames::ThrowableTrace),
      "Long", standardThrowableTraceInitializerBody(), support::SourceSpan::none()});

  builder.addFunctionDef(std::string(support::StdNames::JavaLangThrowable) + "." +
                             std::string(support::StdNames::GetMessage),
                         "(" + std::string(support::StdNames::JavaLangThrowable) +
                             ")String",
                         standardThrowableMessageBody(), support::SourceSpan::none());
  builder.addFunctionDef(std::string(support::StdNames::JavaLangThrowable) + "." +
                             std::string(support::StdNames::GetCause),
                         "(" + std::string(support::StdNames::JavaLangThrowable) + ")" +
                             std::string(support::StdNames::JavaLangThrowable),
                         standardThrowableCauseBody(), support::SourceSpan::none());
  builder.addFunctionDef(std::string(support::StdNames::JavaLangThrowable) + "." +
                             std::string(support::StdNames::InitCause),
                         "(" + std::string(support::StdNames::JavaLangThrowable) + "," +
                             std::string(support::StdNames::JavaLangThrowable) + ")" +
                             std::string(support::StdNames::JavaLangThrowable),
                         standardThrowableInitCauseBody(), support::SourceSpan::none());
  builder.addFunctionDef(std::string(support::StdNames::JavaLangThrowable) + "." +
                             std::string(support::StdNames::FillInStackTrace),
                         "(" + std::string(support::StdNames::JavaLangThrowable) + ")" +
                             std::string(support::StdNames::JavaLangThrowable),
                         standardThrowableFillInStackTraceBody(),
                         support::SourceSpan::none());
  builder.addFunctionDef(
      std::string(support::StdNames::JavaLangThrowable) + "." +
          std::string(support::StdNames::GetStackTrace),
      "(" + std::string(support::StdNames::JavaLangThrowable) + ")Array [ " +
          std::string(support::StdNames::JavaLangStackTraceElement) + " ]",
      standardThrowableGetStackTraceBody(), support::SourceSpan::none());
  builder.addFunctionDef(
      std::string(support::StdNames::JavaLangThrowable) + "." +
          std::string(support::StdNames::SetStackTrace),
      "(" + std::string(support::StdNames::JavaLangThrowable) + ",Array [ " +
          std::string(support::StdNames::JavaLangStackTraceElement) + " ])Unit",
      standardThrowableSetStackTraceBody(), support::SourceSpan::none());
  builder.addFunctionDef(throwableName + "." +
                             std::string(support::StdNames::AddSuppressed),
                         "(" + throwableName + "," + throwableName + ")Unit",
                         standardThrowableAddSuppressedBody(), noSpan);
  builder.addFunctionDef(throwableName + "." +
                             std::string(support::StdNames::GetSuppressed),
                         "(" + throwableName + ")Array [ " + throwableName + " ]",
                         standardThrowableGetSuppressedBody(), noSpan);
  builder.addFunctionDef(
      throwableName + "." + std::string(support::StdNames::PrintStackTrace),
      "(" + throwableName + ")Unit", standardThrowablePrintStackTraceBody(), noSpan);
  builder.addFunctionDef(std::string(support::StdNames::JavaLangThrowable) + "." +
                             std::string(support::StdNames::ToString),
                         "(" + std::string(support::StdNames::JavaLangThrowable) +
                             ")String",
                         standardThrowableToStringBody(), support::SourceSpan::none());
  builder.addFunctionDef(std::string(support::StdNames::JavaLangException) + "." +
                             std::string(support::StdNames::ToString),
                         "(" + std::string(support::StdNames::JavaLangException) +
                             ")String",
                         standardExceptionToStringBody(), support::SourceSpan::none());
  builder.addFunctionDef(
      std::string(support::StdNames::JavaLangStackTraceElement) + "." +
          std::string(support::StdNames::ToString),
      "(" + std::string(support::StdNames::JavaLangStackTraceElement) + ")String",
      standardStackTraceElementToStringBody(), support::SourceSpan::none());

  const bool hasUserDeclarations = std::any_of(
      module.declarations.begin(), module.declarations.end(),
      [](const frontend::TypedDeclaration& declaration) {
        return declaration.symbolName != support::StdNames::JavaLangThrowable &&
               declaration.symbolName != support::StdNames::JavaLangError &&
               declaration.symbolName != support::StdNames::JavaLangAssertionError &&
               declaration.symbolName != support::StdNames::ScalaNotImplementedError &&
               declaration.symbolName != support::StdNames::JavaLangException &&
               declaration.symbolName != support::StdNames::JavaLangRuntimeException &&
               declaration.symbolName !=
                   support::StdNames::JavaLangArithmeticException &&
               declaration.symbolName !=
                   support::StdNames::JavaLangIllegalArgumentException &&
               declaration.symbolName !=
                   support::StdNames::JavaLangIllegalStateException &&
               declaration.symbolName !=
                   support::StdNames::JavaLangNullPointerException &&
               declaration.symbolName !=
                   support::StdNames::JavaLangClassCastException &&
               declaration.symbolName !=
                   support::StdNames::JavaLangIndexOutOfBoundsException &&
               declaration.symbolName !=
                   support::StdNames::JavaLangArrayIndexOutOfBoundsException &&
               declaration.symbolName !=
                   support::StdNames::JavaLangNegativeArraySizeException &&
               declaration.symbolName !=
                   support::StdNames::JavaNioBufferUnderflowException &&
               declaration.symbolName !=
                   support::StdNames::JavaNioBufferOverflowException &&
               declaration.symbolName != support::StdNames::JavaLangStackTraceElement;
      });
  if (!hasUserDeclarations) {
    builder.addModule("root.Empty", support::SourceSpan::none());
  }

  if (const frontend::TypedDeclaration* mainDeclaration =
          findMainDeclaration(module.declarations)) {
    builder.addFunctionDef(std::string(support::StdNames::RuntimeMain),
                           runtimeMainSignatureFor(*mainDeclaration),
                           runtimeMainBodyFor(*mainDeclaration), mainDeclaration->span);
  } else {
    builder.addFunctionDecl(std::string(support::StdNames::RuntimeMain), "()Int",
                            support::SourceSpan::none());
  }
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeAssert),
                          "(Boolean)Unit", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeAssume),
                          "(Boolean)Unit", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeRequire),
                          "(Boolean)Unit", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimePrintln),
                          "(Unknown)Unit", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeGcCollect), "()Unit",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeGcLiveObjectCount),
                          "()Long", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeGcCollectionCount),
                          "()Long", support::SourceSpan::none());
  builder.addFunctionDecl(
      std::string(support::StdNames::RuntimeGcSetCollectionThreshold), "(Long)Unit",
      support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeZoneAllocBytes),
                          "(Int)Array [ Byte ]", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeNativeBytesGetShortBe),
                          "(Array [ Byte ],Int)Short", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeNativeBytesGetShortLe),
                          "(Array [ Byte ],Int)Short", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeNativeBytesPutShortBe),
                          "(Array [ Byte ],Int,Short)Unit",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeNativeBytesPutShortLe),
                          "(Array [ Byte ],Int,Short)Unit",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeByteBufferWrap),
                          "(Array [ Byte ])java.nio.ByteBuffer",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeByteBufferCapacity),
                          "(java.nio.ByteBuffer)Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeByteBufferPosition),
                          "(java.nio.ByteBuffer)Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeByteBufferSetPosition),
                          "(java.nio.ByteBuffer,Int)java.nio.ByteBuffer",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeByteBufferLimit),
                          "(java.nio.ByteBuffer)Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeByteBufferSetLimit),
                          "(java.nio.ByteBuffer,Int)java.nio.ByteBuffer",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeByteBufferRemaining),
                          "(java.nio.ByteBuffer)Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeByteBufferHasRemaining),
                          "(java.nio.ByteBuffer)Boolean", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeByteBufferGet),
                          "(java.nio.ByteBuffer)Byte", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeByteBufferPut),
                          "(java.nio.ByteBuffer,Byte)java.nio.ByteBuffer",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeByteBufferClear),
                          "(java.nio.ByteBuffer)java.nio.ByteBuffer",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeByteBufferFlip),
                          "(java.nio.ByteBuffer)java.nio.ByteBuffer",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeByteBufferRewind),
                          "(java.nio.ByteBuffer)java.nio.ByteBuffer",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeStringLength),
                          "(String)Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeStringToString),
                          "(String)String", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeStringEquals),
                          "(String,Object)Boolean", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeArrayAlloc),
                          "(Int)Array [ String ]", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeArrayLength),
                          "(Array [ String ])Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeArrayApply),
                          "(Array [ String ],Int)String", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeArrayUpdate),
                          "(Array [ String ],Int,String)Unit",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeArrayClone),
                          "(Array [ String ])Array [ String ]",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeIntArrayAlloc),
                          "(Int)Array [ Int ]", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeIntArrayLength),
                          "(Array [ Int ])Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeIntArrayApply),
                          "(Array [ Int ],Int)Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeIntArrayUpdate),
                          "(Array [ Int ],Int,Int)Unit", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeIntArrayClone),
                          "(Array [ Int ])Array [ Int ]", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeByteArrayAlloc),
                          "(Int)Array [ Byte ]", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeByteArrayLength),
                          "(Array [ Byte ])Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeByteArrayApply),
                          "(Array [ Byte ],Int)Byte", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeByteArrayUpdate),
                          "(Array [ Byte ],Int,Byte)Unit", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeByteArrayClone),
                          "(Array [ Byte ])Array [ Byte ]",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeShortArrayAlloc),
                          "(Int)Array [ Short ]", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeShortArrayLength),
                          "(Array [ Short ])Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeShortArrayApply),
                          "(Array [ Short ],Int)Short", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeShortArrayUpdate),
                          "(Array [ Short ],Int,Short)Unit",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeShortArrayClone),
                          "(Array [ Short ])Array [ Short ]",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeArrayRange),
                          "(Int,Int,Int)Array [ Int ]", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeBooleanArrayAlloc),
                          "(Int)Array [ Boolean ]", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeBooleanArrayLength),
                          "(Array [ Boolean ])Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeBooleanArrayApply),
                          "(Array [ Boolean ],Int)Boolean",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeBooleanArrayUpdate),
                          "(Array [ Boolean ],Int,Boolean)Unit",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeBooleanArrayClone),
                          "(Array [ Boolean ])Array [ Boolean ]",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeLongArrayAlloc),
                          "(Int)Array [ Long ]", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeLongArrayLength),
                          "(Array [ Long ])Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeLongArrayApply),
                          "(Array [ Long ],Int)Long", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeLongArrayUpdate),
                          "(Array [ Long ],Int,Long)Unit", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeLongArrayClone),
                          "(Array [ Long ])Array [ Long ]",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeDoubleArrayAlloc),
                          "(Int)Array [ Double ]", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeDoubleArrayLength),
                          "(Array [ Double ])Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeDoubleArrayApply),
                          "(Array [ Double ],Int)Double", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeDoubleArrayUpdate),
                          "(Array [ Double ],Int,Double)Unit",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeDoubleArrayClone),
                          "(Array [ Double ])Array [ Double ]",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeFloatArrayAlloc),
                          "(Int)Array [ Float ]", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeFloatArrayLength),
                          "(Array [ Float ])Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeFloatArrayApply),
                          "(Array [ Float ],Int)Float", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeFloatArrayUpdate),
                          "(Array [ Float ],Int,Float)Unit",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeFloatArrayClone),
                          "(Array [ Float ])Array [ Float ]",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeCharArrayAlloc),
                          "(Int)Array [ Char ]", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeCharArrayLength),
                          "(Array [ Char ])Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeCharArrayApply),
                          "(Array [ Char ],Int)Char", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeCharArrayUpdate),
                          "(Array [ Char ],Int,Char)Unit", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeCharArrayClone),
                          "(Array [ Char ])Array [ Char ]",
                          support::SourceSpan::none());
  for (const std::string_view elementType :
       {"String", "Byte", "Short", "Int", "Boolean", "Long", "Double", "Float",
        "Char"}) {
    const std::string arrayType = "Array [ " + std::string(elementType) + " ]";
    builder.addFunctionDecl(arrayFillRuntimeName(elementType),
                            arrayFillSignature(elementType),
                            support::SourceSpan::none());
    builder.addFunctionDecl(arrayCopyRuntimeName(elementType),
                            arrayCopySignature(arrayType), support::SourceSpan::none());
  }
  std::vector<std::string> sortedReferenceArrayElementTypes(
      referenceArrayElementTypes.begin(), referenceArrayElementTypes.end());
  std::sort(sortedReferenceArrayElementTypes.begin(),
            sortedReferenceArrayElementTypes.end());
  for (const std::string& elementType : sortedReferenceArrayElementTypes) {
    const std::string arrayType = "Array [ " + elementType + " ]";
    builder.addFunctionDecl(
        referenceArrayRuntimeName(support::StdNames::RuntimeReferenceArrayAlloc,
                                  elementType),
        "(Int)" + arrayType, support::SourceSpan::none());
    builder.addFunctionDecl(
        referenceArrayRuntimeName(support::StdNames::RuntimeReferenceArrayLength,
                                  elementType),
        "(" + arrayType + ")Int", support::SourceSpan::none());
    builder.addFunctionDecl(
        referenceArrayRuntimeName(support::StdNames::RuntimeReferenceArrayApply,
                                  elementType),
        "(" + arrayType + ",Int)" + elementType, support::SourceSpan::none());
    builder.addFunctionDecl(
        referenceArrayRuntimeName(support::StdNames::RuntimeReferenceArrayUpdate,
                                  elementType),
        "(" + arrayType + ",Int," + elementType + ")Unit", support::SourceSpan::none());
    builder.addFunctionDecl(
        referenceArrayRuntimeName(support::StdNames::RuntimeReferenceArrayClone,
                                  elementType),
        "(" + arrayType + ")" + arrayType, support::SourceSpan::none());
    builder.addFunctionDecl(arrayFillRuntimeName(elementType),
                            arrayFillSignature(elementType),
                            support::SourceSpan::none());
    builder.addFunctionDecl(arrayCopyRuntimeName(elementType),
                            arrayCopySignature(arrayType), support::SourceSpan::none());
  }
  for (const auto& [name, signature] : runtimeArrayDeclarations) {
    builder.addFunctionDecl(name, signature, support::SourceSpan::none());
  }
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeBooleanToString),
                          "(Boolean)String", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeByteToString),
                          "(Byte)String", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeShortToString),
                          "(Short)String", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeIntToString),
                          "(Int)String", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeLongToString),
                          "(Long)String", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeFloatToString),
                          "(Float)String", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeDoubleToString),
                          "(Double)String", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeCharToString),
                          "(Char)String", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeAnyToString),
                          "(Object)String", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeAnyReceiverToString),
                          "(Object)String", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeThrowableToString),
                          "(" + std::string(support::StdNames::JavaLangThrowable) +
                              ")String",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimePrintStackTrace),
                          "(" + throwableName + ")Unit", noSpan);
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeFillInStackTrace),
                          "(" + std::string(support::StdNames::JavaLangThrowable) +
                              ")" + std::string(support::StdNames::JavaLangThrowable),
                          support::SourceSpan::none());
  builder.addFunctionDecl(
      std::string(support::StdNames::RuntimeGetStackTrace),
      "(" + std::string(support::StdNames::JavaLangThrowable) + ")Array [ " +
          std::string(support::StdNames::JavaLangStackTraceElement) + " ]",
      support::SourceSpan::none());
  builder.addFunctionDecl(
      std::string(support::StdNames::RuntimeSetStackTrace),
      "(" + std::string(support::StdNames::JavaLangThrowable) + ",Array [ " +
          std::string(support::StdNames::JavaLangStackTraceElement) + " ])Int",
      support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeAddSuppressed),
                          "(" + throwableName + "," + throwableName + ")Unit", noSpan);
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeGetSuppressed),
                          "(" + throwableName + ")Array [ " + throwableName + " ]",
                          noSpan);
  builder.addFunctionDecl(
      std::string(support::StdNames::RuntimeStackTraceElementToString),
      "(" + std::string(support::StdNames::JavaLangStackTraceElement) + ")String",
      support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeAnyEquals),
                          "(Object,Object)Boolean", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeAnyReceiverEquals),
                          "(Object,Object)Boolean", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeIntToByte), "(Int)Byte",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeIntToShort),
                          "(Int)Short", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeShortToByte),
                          "(Short)Byte", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeByteToShort),
                          "(Byte)Short", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeByteToInt), "(Byte)Int",
                          support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeShortToInt),
                          "(Short)Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeByteHashCode),
                          "(Byte)Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeShortHashCode),
                          "(Short)Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeBooleanHashCode),
                          "(Boolean)Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeLongHashCode),
                          "(Long)Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeFloatHashCode),
                          "(Float)Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeDoubleHashCode),
                          "(Double)Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeCharHashCode),
                          "(Char)Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeStringHashCode),
                          "(String)Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeSymbolHashCode),
                          "(Symbol)Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeAnyHashCode),
                          "(Object)Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeAnyReceiverHashCode),
                          "(Object)Int", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeFormat),
                          "(String,Unknown)String", support::SourceSpan::none());
  builder.addFunctionDecl(std::string(support::StdNames::RuntimeFormatBoolean),
                          "(String,Unknown)String", support::SourceSpan::none());
  return std::move(builder).build();
}

void NirEmitter::emitDeclaration(
    nir::ModuleBuilder& builder, const frontend::TypedModule& module,
    const frontend::TypedDeclaration& declaration, const std::string& owner,
    frontend::AstDeclarationKind ownerKind,
    std::unordered_set<std::string>* referenceArrayElementTypes,
    std::map<std::string, std::string>* runtimeArrayDeclarations) {
  if (declaration.kind == frontend::AstDeclarationKind::Package ||
      declaration.kind == frontend::AstDeclarationKind::Import ||
      declaration.name.empty()) {
    return;
  }

  std::string globalName = owner.empty() ? (declaration.symbolName.empty()
                                                ? qualify(module, declaration.name)
                                                : declaration.symbolName)
                                         : qualifyMember(owner, declaration.name);
  const bool hasImplicitReceiver = ownerKind == frontend::AstDeclarationKind::Class ||
                                   ownerKind == frontend::AstDeclarationKind::Trait;
  const std::string receiverType = hasImplicitReceiver ? owner : std::string{};
  ValueContext context;
  context.packageName = module.packageName;
  context.importAliases = importAliasesFor(module.declarations);
  context.declarations = &module.declarations;
  context.expressionTypes = &module.expressionTypes;
  context.contextApplications = &module.contextApplications;
  context.referenceArrayElementTypes = referenceArrayElementTypes;
  context.runtimeArrayDeclarations = runtimeArrayDeclarations;
  context.hasImplicitReceiver = hasImplicitReceiver;
  context.currentOwner = owner;
  context.stackableSuper = ownerKind == frontend::AstDeclarationKind::Trait;
  if (hasImplicitReceiver) {
    context.implicitThisMembers = implicitThisMembersFor(module.declarations, owner);
    if (const frontend::TypedDeclaration* ownerDeclaration =
            findDeclarationBySymbol(module.declarations, owner)) {
      context.superTypes = ownerDeclaration->parentTypes;
      context.superType = superTypeName(*ownerDeclaration);
    }
  }

  switch (declaration.kind) {
  case frontend::AstDeclarationKind::Object:
    builder.addDefinition(nir::Definition{nir::DefinitionKind::Module,
                                          globalName,
                                          metadataParentNames(declaration),
                                          {},
                                          declaration.span});
    for (const frontend::TypedDeclaration& member : declaration.members) {
      if (!isModuleStoredField(member, declaration.kind)) {
        continue;
      }
      builder.addDefinition(
          nir::Definition{nir::DefinitionKind::Field,
                          qualifyMember(globalName, moduleStoredFieldName(member)),
                          runtimeTypeName(member.inferredType),
                          {},
                          member.span});
      builder.addFunctionDef(qualifyMember(globalName, member.name),
                             "()" + runtimeTypeName(member.inferredType),
                             moduleAccessorBodyFor(member, globalName), member.span);
      if (member.kind == frontend::AstDeclarationKind::Var) {
        builder.addFunctionDef(qualifyMember(globalName, setterName(member.name)),
                               "(" + runtimeTypeName(member.inferredType) + ")Unit",
                               moduleSetterBodyFor(member, globalName), member.span);
      }
    }
    if (moduleRequiresInitializerFunction(declaration)) {
      ValueContext moduleContext = context;
      moduleContext.currentOwner = globalName;
      builder.addFunctionDef(
          qualifyMember(globalName, std::string(support::StdNames::Constructor)),
          "()Unit", moduleInitializerBodyFor(declaration, moduleContext, globalName),
          declaration.span);
    }
    break;
  case frontend::AstDeclarationKind::Class:
    builder.addDefinition(nir::Definition{nir::DefinitionKind::Class,
                                          globalName,
                                          metadataParentNames(declaration),
                                          {},
                                          declaration.span});
    for (std::size_t parameterIndex = 0; parameterIndex < declaration.parameters.size();
         ++parameterIndex) {
      const std::string& parameter = declaration.parameters[parameterIndex];
      const std::string name = parameterName(parameter);
      if (name.empty()) {
        continue;
      }
      const std::string parameterType =
          parameterIndex < declaration.parameterTypes.size()
              ? runtimeTypeName(declaration.parameterTypes[parameterIndex])
              : parameterTypeName(parameter, context);
      builder.addDefinition(nir::Definition{
          nir::DefinitionKind::Field,
          qualifyMember(globalName, storedParameterFieldName(declaration, parameter)),
          parameterType,
          {},
          declaration.span});
      if (isAccessorParameter(declaration, name)) {
        builder.addFunctionDef(
            qualifyMember(globalName, name), "(" + globalName + ")" + parameterType,
            parameterAccessorBodyFor(declaration, parameter, context, globalName),
            declaration.span);
        if (parameter.rfind("var ", 0) == 0) {
          frontend::TypedDeclaration parameterDeclaration;
          parameterDeclaration.name = name;
          parameterDeclaration.inferredType =
              frontend::TypeInfo{frontend::SimpleTypeKind::Unknown, parameterType};
          parameterDeclaration.span = declaration.span;
          builder.addFunctionDef(
              qualifyMember(globalName, setterName(name)),
              "(" + globalName + "," + parameterType + ")Unit",
              fieldSetterBodyFor(parameterDeclaration,
                                 storedParameterFieldName(declaration, parameter),
                                 globalName),
              declaration.span);
        }
      }
    }
    {
      ValueContext memberContext = context;
      memberContext.hasImplicitReceiver = true;
      memberContext.currentOwner = globalName;
      memberContext.stackableSuper = false;
      memberContext.implicitThisMembers =
          implicitThisMembersFor(module.declarations, globalName);
      memberContext.superTypes = declaration.parentTypes;
      memberContext.superType = superTypeName(declaration);
      for (const frontend::TypedDeclaration& member : declaration.members) {
        if (!isClassStoredField(member, declaration.kind)) {
          continue;
        }
        builder.addDefinition(
            nir::Definition{nir::DefinitionKind::Field,
                            qualifyMember(globalName, storedFieldName(member)),
                            runtimeTypeName(member.inferredType),
                            bodyFor(member, memberContext, globalName), member.span});
        if (member.needsAccessor) {
          builder.addFunctionDef(qualifyMember(globalName, member.name),
                                 "(" + globalName + ")" +
                                     runtimeTypeName(member.inferredType),
                                 accessorBodyFor(member, globalName), member.span);
          if (member.kind == frontend::AstDeclarationKind::Var) {
            builder.addFunctionDef(
                qualifyMember(globalName, setterName(member.name)),
                "(" + globalName + "," + runtimeTypeName(member.inferredType) + ")Unit",
                fieldSetterBodyFor(member, storedFieldName(member), globalName),
                member.span);
          }
        }
      }
      for (const MaterializedTraitValue& value :
           materializedTraitValuesForClass(module, declaration)) {
        if (value.owner == nullptr || value.member == nullptr) {
          continue;
        }
        ValueContext traitContext = memberContext;
        traitContext.currentOwner = value.owner->symbolName;
        traitContext.stackableSuper = true;
        traitContext.implicitThisMembers =
            implicitThisMembersFor(module.declarations, value.owner->symbolName);
        traitContext.superTypes = value.owner->parentTypes;
        traitContext.superType = superTypeName(*value.owner);
        builder.addDefinition(nir::Definition{
            nir::DefinitionKind::Field,
            qualifyMember(globalName, traitValueFieldName(*value.owner, *value.member)),
            runtimeTypeName(value.member->inferredType),
            bodyFor(*value.member, traitContext, globalName), value.member->span});
        builder.addFunctionDef(
            qualifyMember(globalName,
                          traitValueAccessorName(*value.owner, *value.member)),
            "(" + globalName + ")" + runtimeTypeName(value.member->inferredType),
            fieldAccessorBodyFor(*value.member,
                                 traitValueFieldName(*value.owner, *value.member),
                                 globalName),
            value.member->span);
        if (value.member->kind == frontend::AstDeclarationKind::Var) {
          builder.addFunctionDef(
              qualifyMember(globalName, setterName(traitValueAccessorName(
                                            *value.owner, *value.member))),
              "(" + globalName + "," + runtimeTypeName(value.member->inferredType) +
                  ")Unit",
              fieldSetterBodyFor(*value.member,
                                 traitValueFieldName(*value.owner, *value.member),
                                 globalName),
              value.member->span);
        }
        if (value.effective) {
          builder.addFunctionDef(
              qualifyMember(globalName, value.member->name),
              "(" + globalName + ")" + runtimeTypeName(value.member->inferredType),
              fieldAccessorBodyFor(*value.member,
                                   traitValueFieldName(*value.owner, *value.member),
                                   globalName),
              value.member->span);
          if (value.member->kind == frontend::AstDeclarationKind::Var) {
            builder.addFunctionDef(
                qualifyMember(globalName, setterName(value.member->name)),
                "(" + globalName + "," + runtimeTypeName(value.member->inferredType) +
                    ")Unit",
                fieldSetterBodyFor(*value.member,
                                   traitValueFieldName(*value.owner, *value.member),
                                   globalName),
                value.member->span);
          }
        }
      }
    }
    if (classRequiresInitializerFunction(module, declaration)) {
      ValueContext memberContext = context;
      memberContext.hasImplicitReceiver = true;
      memberContext.currentOwner = globalName;
      memberContext.stackableSuper = false;
      memberContext.implicitThisMembers =
          implicitThisMembersFor(module.declarations, globalName);
      memberContext.superTypes = declaration.parentTypes;
      memberContext.superType = superTypeName(declaration);
      builder.addFunctionDef(
          qualifyMember(globalName, std::string(support::StdNames::Constructor)),
          "(" + globalName + ")Unit",
          constructorBodyFor(module, declaration, memberContext, globalName),
          declaration.span);
    }
    break;
  case frontend::AstDeclarationKind::Trait:
    builder.addDefinition(nir::Definition{nir::DefinitionKind::Trait,
                                          globalName,
                                          metadataParentNames(declaration),
                                          {},
                                          declaration.span});
    break;
  case frontend::AstDeclarationKind::Type: {
    std::string metadata;
    if (declaration.hasInitializer) {
      metadata = declaration.inferredType.name;
    } else {
      metadata = "abstract";
      if (!declaration.lowerBound.empty()) {
        metadata += " >: " + declaration.lowerBound;
      }
      if (!declaration.upperBound.empty()) {
        metadata += " <: " + declaration.upperBound;
      }
    }
    builder.addDefinition(nir::Definition{nir::DefinitionKind::TypeMember,
                                          globalName,
                                          std::move(metadata),
                                          {},
                                          declaration.span});
  } break;
  case frontend::AstDeclarationKind::Def:
  case frontend::AstDeclarationKind::Val:
  case frontend::AstDeclarationKind::Var:
    if (ownerKind == frontend::AstDeclarationKind::Trait &&
        (declaration.kind == frontend::AstDeclarationKind::Val ||
         declaration.kind == frontend::AstDeclarationKind::Var)) {
      builder.addFunctionDecl(globalName,
                              signatureFor(declaration, context, receiverType),
                              declaration.span);
      if (declaration.kind == frontend::AstDeclarationKind::Var) {
        builder.addFunctionDecl(setterName(globalName),
                                "(" + receiverType + "," +
                                    runtimeTypeName(declaration.inferredType) + ")Unit",
                                declaration.span);
      }
    } else if (declaration.hasInitializer) {
      builder.addFunctionDef(
          globalName, signatureFor(declaration, context, receiverType),
          bodyFor(declaration, context, receiverType), declaration.span);
    } else {
      builder.addFunctionDecl(globalName,
                              signatureFor(declaration, context, receiverType),
                              declaration.span);
    }
    break;
  case frontend::AstDeclarationKind::Package:
  case frontend::AstDeclarationKind::Import:
    break;
  }

  for (const frontend::TypedDeclaration& member : declaration.members) {
    const bool generatedThrowableMember =
        declaration.symbolName == support::StdNames::JavaLangThrowable &&
        (member.name == support::StdNames::GetMessage ||
         member.name == support::StdNames::GetCause ||
         member.name == support::StdNames::InitCause ||
         member.name == support::StdNames::FillInStackTrace ||
         member.name == support::StdNames::GetStackTrace ||
         member.name == support::StdNames::SetStackTrace ||
         member.name == support::StdNames::AddSuppressed ||
         member.name == support::StdNames::GetSuppressed ||
         member.name == support::StdNames::PrintStackTrace ||
         member.name == support::StdNames::ToString);
    const bool generatedExceptionMember =
        declaration.symbolName == support::StdNames::JavaLangException &&
        member.name == support::StdNames::ToString;
    const bool generatedStackTraceElementMember =
        declaration.symbolName == support::StdNames::JavaLangStackTraceElement &&
        member.name == support::StdNames::ToString;
    if (generatedThrowableMember || generatedExceptionMember ||
        generatedStackTraceElementMember) {
      continue;
    }
    if (isClassStoredField(member, declaration.kind) ||
        isModuleStoredField(member, declaration.kind)) {
      continue;
    }
    emitDeclaration(builder, module, member, globalName, declaration.kind,
                    referenceArrayElementTypes, runtimeArrayDeclarations);
  }
}

std::string NirEmitter::qualify(const frontend::TypedModule& module,
                                const std::string& name) {
  if (module.packageName.empty()) {
    return name;
  }
  return module.packageName + "." + name;
}

std::string NirEmitter::qualifyMember(const std::string& owner,
                                      const std::string& name) {
  return owner + "." + name;
}

} // namespace scalanative::nscplugin
