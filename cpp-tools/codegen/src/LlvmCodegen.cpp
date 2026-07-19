#include "scalanative/tools/codegen/LlvmCodegen.h"

#include "scalanative/nir/Nir.h"
#include "scalanative/runtime/RuntimeConfig.h"
#include "scalanative/support/SourceManager.h"
#include "scalanative/support/StdNames.h"
#include "scalanative/tools/codegen/RuntimeLlvmResources.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace scalanative::tools::codegen {

namespace {

struct Signature {
  std::vector<std::string> parameterTypes;
  std::string returnType = "Unit";
};

struct StringConstant {
  std::string name;
  std::string literalText;
  std::string contents;
};

struct SourceFrameInfo {
  support::SourceId source;
  std::string functionName;
  std::string fileName;
  std::string functionConstant;
  std::string fileConstant;
  std::size_t line = 0;
  std::size_t column = 0;
};

struct FieldInfo {
  std::string className;
  std::string simpleType;
  std::size_t offset = 0;
  nir::FunctionBody initializer;
  bool constructorParameter = true;
  support::SourceSpan span;
};

struct VirtualSlot {
  std::string memberName;
  std::string implementationName;
};

struct StackSuperSite {
  std::string ownerName;
  std::string memberName;
};

struct ClassLayout {
  std::vector<std::string> fields;
  std::vector<std::string> constructorFields;
  std::vector<std::size_t> traceOffsets;
  std::vector<VirtualSlot> virtualSlots;
  std::size_t size = 0;
  bool hasHeader = false;
  bool hasChildren = false;
  bool isTrait = false;
  bool isModule = false;
  std::uint32_t typeId = 0;
  std::string descriptorName;
  std::string vtableName;
};

struct LoweringState {
  std::unordered_map<std::string, std::string> values;
  std::unordered_map<std::string, std::string> localValueNames;
  std::unordered_map<std::string, std::string> mutableLocalSlots;
  std::unordered_set<std::string> mutableUnitLocals;
  std::unordered_map<std::string, std::string> shadowRootSlots;
  std::vector<std::string> bindingRootSlots;
  std::vector<std::string> temporaryRootSlots;
  std::unordered_map<std::string, std::string> simpleTypes;
  std::unordered_set<std::string> exactLocals;
  std::unordered_set<std::string> usedLocalValueNames;
  std::string moduleName;
  std::string ownerName;
  const std::unordered_set<std::string>* globals = nullptr;
  const std::unordered_map<std::string, Signature>* functionSignatures = nullptr;
  const std::unordered_map<std::string, FieldInfo>* fields = nullptr;
  const std::unordered_map<std::string, ClassLayout>* classLayouts = nullptr;
  const std::unordered_map<std::string, std::vector<std::string>>* classParents =
      nullptr;
  const std::unordered_map<std::string, StringConstant>* stringConstants = nullptr;
  const support::SourceManager* sources = nullptr;
  std::vector<CodegenError>* errors = nullptr;
  std::string functionName;
  support::SourceId source;
  std::string currentBlockLabel = "entry";
  int nextTemporary = 0;
  std::size_t nextBindingRootSlot = 0;
  std::size_t nextTemporaryRootSlot = 0;
  bool hasShadowFrame = false;
  bool hasSourceFrame = false;
  bool preserveMutableLocalsAcrossHandlers = false;
};

void reportUnsupported(LoweringState& state, support::SourceSpan span,
                       std::string detail) {
  if (state.errors != nullptr) {
    state.errors->push_back(CodegenError{span, "function '" + state.functionName +
                                                   "': " + std::move(detail)});
  }
}

support::SourceSpan diagnosticSpan(const nir::Instruction& instruction) {
  return instruction.value.span.isValid() ? instruction.value.span : instruction.span;
}

std::uint32_t sourceCoordinate(std::size_t value) {
  return static_cast<std::uint32_t>(std::min(
      value, static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
}

std::string sourceCStringPointer(const std::string& constant,
                                 const std::string& contents) {
  return "getelementptr inbounds ([" + std::to_string(contents.size() + 1) +
         " x i8], ptr @" + constant + ", i64 0, i64 0)";
}

void emitSourceLocation(support::SourceSpan span, const LoweringState& state,
                        std::ostringstream& out) {
  if (!state.hasSourceFrame || state.sources == nullptr || !span.isValid() ||
      span.source != state.source || state.sources->get(span.source) == nullptr) {
    return;
  }
  const auto [line, column] = state.sources->lineColumn(span);
  if (line == 0) {
    return;
  }
  out << "  store i32 " << sourceCoordinate(line) << ", ptr %__source_line_field\n";
  out << "  store i32 " << sourceCoordinate(column) << ", ptr %__source_column_field\n";
}

constexpr std::size_t ObjectHeaderSize = 8;
constexpr std::size_t ByteBufferSize = 32;
constexpr std::size_t ByteBufferBackingOffset = 8;
constexpr std::size_t ByteBufferCapacityOffset = 16;
constexpr std::size_t ByteBufferPositionOffset = 20;
constexpr std::size_t ByteBufferLimitOffset = 24;
constexpr std::size_t ByteBufferMarkOffset = 28;
constexpr std::size_t SuppressedExceptionCapacity = 64;
constexpr std::size_t SuppressedArrayOwnerOffset =
    ObjectHeaderSize + 8 + SuppressedExceptionCapacity * 8;
constexpr std::size_t SuppressedArraySize = SuppressedArrayOwnerOffset + 8;
constexpr std::size_t SuppressedArrayTraceCount = SuppressedExceptionCapacity + 1;
constexpr std::size_t TypeDescriptorKindIndex = 0;
constexpr std::size_t TypeDescriptorTypeIdIndex = 1;
constexpr std::size_t TypeDescriptorTypeNameIndex = 7;
constexpr std::size_t TypeDescriptorVtableIndex = 8;
constexpr std::size_t TypeDescriptorVtableSlotCountIndex = 9;
constexpr std::size_t TypeDescriptorAncestorsIndex = 10;
constexpr std::size_t TypeDescriptorAncestorCountIndex = 11;
constexpr std::size_t TypeDescriptorTraceOffsetsIndex = 12;
constexpr std::size_t TypeDescriptorTraceCountIndex = 13;
constexpr std::size_t TypeDescriptorDefaultOwnershipIndex = 14;
constexpr std::size_t NoVirtualSlotIndex = std::numeric_limits<std::size_t>::max();
constexpr std::uint32_t FirstClassLikeTypeId = 1024;

std::string stackSuperSlotName(const std::string& ownerName,
                               const std::string& memberName) {
  return "$stack-super$" + ownerName + "." + memberName;
}

bool isHiddenTraitValueAccessor(const std::string& memberName) {
  return memberName.rfind("$trait-value$", 0) == 0;
}

bool isStackSuperSelect(const nir::Value& value) {
  return value.kind == nir::ValueKind::Select && value.operands.size() == 1 &&
         nir::isStackSuper(value.operands.front());
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

std::vector<std::string> split(std::string_view text, char delimiter) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == delimiter) {
      parts.push_back(trim(text.substr(start, i - start)));
      start = i + 1;
    }
  }
  parts.push_back(trim(text.substr(start)));
  return parts;
}

Signature parseSignature(const std::string& signature) {
  Signature parsed;
  const std::size_t open = signature.find('(');
  const std::size_t close = signature.find(')');
  if (open != 0 || close == std::string::npos || close + 1 >= signature.size()) {
    return parsed;
  }

  const std::string parameters = signature.substr(open + 1, close - open - 1);
  if (!parameters.empty()) {
    parsed.parameterTypes = split(parameters, ',');
  }
  parsed.returnType = trim(std::string_view(signature).substr(close + 1));
  return parsed;
}

std::string llvmType(const std::string& simpleType) {
  if (simpleType == "Unit") {
    return "void";
  }
  if (simpleType == "Boolean") {
    return "i1";
  }
  if (simpleType == "Byte") {
    return "i8";
  }
  if (simpleType == "Short") {
    return "i16";
  }
  if (simpleType == "Int" || simpleType == "Char") {
    return "i32";
  }
  if (simpleType == "Long") {
    return "i64";
  }
  if (simpleType == "Float") {
    return "float";
  }
  if (simpleType == "Double") {
    return "double";
  }
  return "ptr";
}

std::size_t typeSize(const std::string& simpleType) {
  const std::string lowered = llvmType(simpleType);
  if (lowered == "i1") {
    return 1;
  }
  if (lowered == "i8") {
    return 1;
  }
  if (lowered == "i16") {
    return 2;
  }
  if (lowered == "i32" || lowered == "float") {
    return 4;
  }
  if (lowered == "i64" || lowered == "double" || lowered == "ptr") {
    return 8;
  }
  return 1;
}

std::size_t typeAlign(const std::string& simpleType) {
  const std::size_t size = typeSize(simpleType);
  return size > 8 ? 8 : size;
}

std::size_t alignTo(std::size_t offset, std::size_t alignment) {
  if (alignment <= 1) {
    return offset;
  }
  const std::size_t remainder = offset % alignment;
  if (remainder == 0) {
    return offset;
  }
  return offset + alignment - remainder;
}

std::string simpleTypeFromLlvm(const std::string& type) {
  if (type == "void") {
    return "Unit";
  }
  if (type == "i1") {
    return "Boolean";
  }
  if (type == "i8") {
    return "Byte";
  }
  if (type == "i16") {
    return "Short";
  }
  if (type == "i32") {
    return "Int";
  }
  if (type == "i64") {
    return "Long";
  }
  if (type == "float") {
    return "Float";
  }
  if (type == "double") {
    return "Double";
  }
  return "Object";
}

std::string defaultValue(const std::string& type) {
  const std::string lowered = llvmType(type);
  if (lowered == "i1" || lowered == "i8" || lowered == "i16" || lowered == "i32" ||
      lowered == "i64") {
    return "0";
  }
  if (lowered == "float") {
    return "0.0";
  }
  if (lowered == "double") {
    return "0.000000e+00";
  }
  return "null";
}

std::string sanitizeIdentifier(std::string_view text) {
  std::string sanitized;
  for (char ch : text) {
    const unsigned char byte = static_cast<unsigned char>(ch);
    if (std::isalnum(byte) != 0 || ch == '_') {
      sanitized.push_back(ch);
    } else {
      sanitized.push_back('_');
    }
  }
  if (sanitized.empty()) {
    return "_";
  }
  if (std::isdigit(static_cast<unsigned char>(sanitized.front())) != 0) {
    sanitized.insert(sanitized.begin(), '_');
  }
  return sanitized;
}

bool isIntegerLiteral(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  if (text.front() == '-' || text.front() == '+') {
    text.remove_prefix(1);
  }
  if (text.empty()) {
    return false;
  }
  for (char ch : text) {
    if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
      return false;
    }
  }
  return true;
}

bool isBoxablePrimitiveType(const std::string& type) {
  return type == "Unit" || type == "Boolean" || type == "Byte" || type == "Short" ||
         type == "Int" || type == "Long" || type == "Float" || type == "Double" ||
         type == "Char" || type == "Symbol" || type == "String";
}

bool isReferenceType(const std::string& simpleType) {
  return simpleType != "Unit" && simpleType != "String" && simpleType != "Unknown" &&
         simpleType != "Nothing" && !isBoxablePrimitiveType(simpleType);
}

std::string compactTypeName(std::string_view typeName) {
  std::string compact;
  compact.reserve(typeName.size());
  for (char ch : typeName) {
    if (std::isspace(static_cast<unsigned char>(ch)) == 0) {
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

bool isStringArrayType(std::string_view typeName) {
  const std::string elementType = arrayElementTypeName(typeName);
  return elementType == "String" || elementType == "java.lang.String";
}

bool isIntArrayType(std::string_view typeName) {
  return arrayElementTypeName(typeName) == "Int";
}

bool isByteArrayType(std::string_view typeName) {
  return arrayElementTypeName(typeName) == "Byte";
}

bool isShortArrayType(std::string_view typeName) {
  return arrayElementTypeName(typeName) == "Short";
}

bool isBooleanArrayType(std::string_view typeName) {
  return arrayElementTypeName(typeName) == "Boolean";
}

bool isLongArrayType(std::string_view typeName) {
  return arrayElementTypeName(typeName) == "Long";
}

bool isDoubleArrayType(std::string_view typeName) {
  return arrayElementTypeName(typeName) == "Double";
}

bool isFloatArrayType(std::string_view typeName) {
  return arrayElementTypeName(typeName) == "Float";
}

bool isCharArrayType(std::string_view typeName) {
  return arrayElementTypeName(typeName) == "Char";
}

bool isReferenceArrayType(std::string_view typeName) {
  const std::string elementType = arrayElementTypeName(typeName);
  return !elementType.empty() && elementType != "String" &&
         elementType != "java.lang.String" && elementType != "Byte" &&
         elementType != "Short" && elementType != "Int" && elementType != "Boolean" &&
         elementType != "Long" && elementType != "Double" && elementType != "Float" &&
         elementType != "Char" && llvmType(elementType) == "ptr";
}

constexpr std::array<std::string_view, 11> BoxedPrimitiveTypes = {
    "Unit",  "Boolean", "Byte", "Short",  "Int",   "Long",
    "Float", "Double",  "Char", "Symbol", "String"};

std::string boxedPrimitiveDescriptorName(std::string_view type) {
  return "__scalanative_boxed_" + sanitizeIdentifier(type);
}

std::string boxedPrimitiveUnboxName(std::string_view type) {
  return "__scalanative_unbox_" + sanitizeIdentifier(type);
}

std::string classTypeDescriptorName(std::string_view className) {
  return "__type_" + sanitizeIdentifier(className);
}

std::string classTypeNameConstantName(std::string_view className) {
  return "__typename_" + sanitizeIdentifier(className);
}

std::string classTraceOffsetsName(std::string_view className) {
  return "__trace_offsets_" + sanitizeIdentifier(className);
}

std::string moduleInstanceName(std::string_view moduleName) {
  return "__module_instance_" + sanitizeIdentifier(moduleName);
}

std::string moduleAccessorName(std::string_view moduleName) {
  return "__scalanative_module_" + sanitizeIdentifier(moduleName);
}

std::string boxedTypeNameConstantName(std::string_view type) {
  return "__typename_boxed_" + sanitizeIdentifier(type);
}

std::string llvmCString(std::string_view text);

void emitStringFormatHelper(std::ostringstream& out, std::string_view name,
                            std::string_view valueType, std::size_t payloadSize,
                            std::string_view formatName, std::size_t formatSize,
                            bool promoteFloat = false) {
  out << "define internal ptr @" << name << "(" << valueType << " %value) {\n";
  out << "entry:\n";
  out << "  %storage = call ptr @__scalanative_program_arena_alloc(i64 "
      << payloadSize + ObjectHeaderSize << ", ptr null)\n";
  out << "  %result = getelementptr i8, ptr %storage, i64 " << ObjectHeaderSize << "\n";
  std::string argument = std::string(valueType) + " %value";
  if (promoteFloat) {
    out << "  %promoted = fpext float %value to double\n";
    argument = "double %promoted";
  }
  out << "  %written = call i32 (ptr, i64, ptr, ...) @snprintf(ptr %result, i64 "
      << payloadSize << ", ptr getelementptr inbounds ([" << formatSize
      << " x i8], ptr @" << formatName << ", i64 0, i64 0), " << argument << ")\n";
  out << "  ret ptr %result\n";
  out << "}\n\n";
}

void emitRuntimeTypeHelpers(std::ostringstream& out,
                            std::size_t dynamicToStringSlotIndex) {
  out << "define internal void @__scalanative_gc_register(ptr %object) {\n";
  out << "entry:\n";
  out << "  %node = call ptr @malloc(i64 24)\n";
  out << "  %object_field = getelementptr %scalanative.gc_node, ptr %node, i32 0, "
         "i32 0\n";
  out << "  store ptr %object, ptr %object_field\n";
  out << "  %marked_field = getelementptr %scalanative.gc_node, ptr %node, i32 0, "
         "i32 1\n";
  out << "  store i1 false, ptr %marked_field\n";
  out << "  %next_field = getelementptr %scalanative.gc_node, ptr %node, i32 0, i32 "
         "2\n";
  out << "  %head = load ptr, ptr @__scalanative_gc_head\n";
  out << "  store ptr %head, ptr %next_field\n";
  out << "  store ptr %node, ptr @__scalanative_gc_head\n";
  out << "  %count = load i64, ptr @__scalanative_gc_allocation_count\n";
  out << "  %next_count = add i64 %count, 1\n";
  out << "  store i64 %next_count, ptr @__scalanative_gc_allocation_count\n";
  out << "  ret void\n";
  out << "}\n\n";

  out << "define internal void @__scalanative_tag_object(ptr %object, ptr "
         "%descriptor, i32 %ownership) {\n";
  out << "entry:\n";
  out << "  %descriptor_bits = ptrtoint ptr %descriptor to i64\n";
  out << "  %ownership_bits = zext i32 %ownership to i64\n";
  out << "  %tagged_bits = or i64 %descriptor_bits, %ownership_bits\n";
  out << "  %tagged = inttoptr i64 %tagged_bits to ptr\n";
  out << "  store ptr %tagged, ptr %object\n";
  out << "  ret void\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_object_descriptor(ptr %object) {\n";
  out << "entry:\n";
  out << "  %tagged = load ptr, ptr %object\n";
  out << "  %tagged_bits = ptrtoint ptr %tagged to i64\n";
  out << "  %descriptor_bits = and i64 %tagged_bits, -4\n";
  out << "  %descriptor = inttoptr i64 %descriptor_bits to ptr\n";
  out << "  ret ptr %descriptor\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_descriptor_type_name(ptr "
         "%descriptor) {\n";
  out << "entry:\n";
  out << "  %type_name_field = getelementptr %scalanative.type_descriptor, ptr "
         "%descriptor, i32 0, i32 "
      << TypeDescriptorTypeNameIndex << "\n";
  out << "  %type_name = load ptr, ptr %type_name_field\n";
  out << "  ret ptr %type_name\n";
  out << "}\n\n";

  out << "define internal i32 @__scalanative_object_ownership(ptr %object) {\n";
  out << "entry:\n";
  out << "  %tagged = load ptr, ptr %object\n";
  out << "  %tagged_bits = ptrtoint ptr %tagged to i64\n";
  out << "  %ownership_bits = and i64 %tagged_bits, 3\n";
  out << "  %ownership = trunc i64 %ownership_bits to i32\n";
  out << "  ret i32 %ownership\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_alloc(i64 %size, ptr %descriptor, "
         "i32 %ownership) {\n";
  out << "entry:\n";
  out << "  %object = call ptr @malloc(i64 %size)\n";
  out << "  call void @llvm.memset.p0.i64(ptr %object, i8 0, i64 %size, i1 false)\n";
  out << "  call void @__scalanative_tag_object(ptr %object, ptr %descriptor, i32 "
         "%ownership)\n";
  out << "  %gc_owned = icmp eq i32 %ownership, "
      << runtime::objectOwnershipTag(runtime::ObjectOwnership::Gc) << "\n";
  out << "  br i1 %gc_owned, label %register, label %ready\n";
  out << "register:\n";
  out << "  call void @__scalanative_gc_register(ptr %object)\n";
  out << "  br label %ready\n";
  out << "ready:\n";
  out << "  ret ptr %object\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_arena_new_block(i64 %capacity, ptr "
         "%previous) {\n";
  out << "entry:\n";
  out << "  %block = call ptr @malloc(i64 32)\n";
  out << "  %buffer = call ptr @malloc(i64 %capacity)\n";
  out << "  %buffer_field = getelementptr %scalanative.arena_block, ptr %block, i32 "
         "0, i32 0\n";
  out << "  store ptr %buffer, ptr %buffer_field\n";
  out << "  %capacity_field = getelementptr %scalanative.arena_block, ptr %block, "
         "i32 0, i32 1\n";
  out << "  store i64 %capacity, ptr %capacity_field\n";
  out << "  %offset_field = getelementptr %scalanative.arena_block, ptr %block, i32 "
         "0, i32 2\n";
  out << "  store i64 0, ptr %offset_field\n";
  out << "  %previous_field = getelementptr %scalanative.arena_block, ptr %block, "
         "i32 0, i32 3\n";
  out << "  store ptr %previous, ptr %previous_field\n";
  out << "  ret ptr %block\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_arena_create(i64 %capacity) {\n";
  out << "entry:\n";
  out << "  %arena = call ptr @malloc(i64 24)\n";
  out << "  %block = call ptr @__scalanative_arena_new_block(i64 %capacity, ptr "
         "null)\n";
  out << "  %current_field = getelementptr %scalanative.arena, ptr %arena, i32 0, "
         "i32 0\n";
  out << "  store ptr %block, ptr %current_field\n";
  out << "  %capacity_field = getelementptr %scalanative.arena, ptr %arena, i32 0, "
         "i32 1\n";
  out << "  store i64 %capacity, ptr %capacity_field\n";
  out << "  %previous_zone_field = getelementptr %scalanative.arena, ptr %arena, "
         "i32 0, i32 2\n";
  out << "  store ptr null, ptr %previous_zone_field\n";
  out << "  ret ptr %arena\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_arena_alloc(ptr %arena, i64 %size, ptr "
         "%descriptor) {\n";
  out << "entry:\n";
  out << "  %current_field = getelementptr %scalanative.arena, ptr %arena, i32 0, "
         "i32 0\n";
  out << "  %block = load ptr, ptr %current_field\n";
  out << "  %offset_field = getelementptr %scalanative.arena_block, ptr %block, i32 "
         "0, i32 2\n";
  out << "  %offset = load i64, ptr %offset_field\n";
  out << "  %padded = add i64 %offset, 7\n";
  out << "  %aligned = and i64 %padded, -8\n";
  out << "  %next = add i64 %aligned, %size\n";
  out << "  %capacity_field = getelementptr %scalanative.arena_block, ptr %block, "
         "i32 0, i32 1\n";
  out << "  %capacity = load i64, ptr %capacity_field\n";
  out << "  %fits = icmp ule i64 %next, %capacity\n";
  out << "  br i1 %fits, label %allocate, label %grow\n";
  out << "grow:\n";
  out << "  %default_capacity_field = getelementptr %scalanative.arena, ptr %arena, "
         "i32 0, i32 1\n";
  out << "  %default_capacity = load i64, ptr %default_capacity_field\n";
  out << "  %doubled_capacity = shl i64 %capacity, 1\n";
  out << "  %below_default = icmp ult i64 %doubled_capacity, %default_capacity\n";
  out << "  %growth_capacity = select i1 %below_default, i64 %default_capacity, i64 "
         "%doubled_capacity\n";
  out << "  %below_size = icmp ult i64 %growth_capacity, %size\n";
  out << "  %new_capacity = select i1 %below_size, i64 %size, i64 "
         "%growth_capacity\n";
  out << "  %new_block = call ptr @__scalanative_arena_new_block(i64 %new_capacity, "
         "ptr %block)\n";
  out << "  store ptr %new_block, ptr %current_field\n";
  out << "  %new_buffer_field = getelementptr %scalanative.arena_block, ptr "
         "%new_block, i32 0, i32 0\n";
  out << "  %new_buffer = load ptr, ptr %new_buffer_field\n";
  out << "  %new_offset_field = getelementptr %scalanative.arena_block, ptr "
         "%new_block, i32 0, i32 2\n";
  out << "  store i64 %size, ptr %new_offset_field\n";
  out << "  call void @llvm.memset.p0.i64(ptr %new_buffer, i8 0, i64 %size, i1 "
         "false)\n";
  out << "  call void @__scalanative_tag_object(ptr %new_buffer, ptr %descriptor, "
         "i32 "
      << runtime::objectOwnershipTag(runtime::ObjectOwnership::Arena) << ")\n";
  out << "  ret ptr %new_buffer\n";
  out << "allocate:\n";
  out << "  %buffer_field = getelementptr %scalanative.arena_block, ptr %block, i32 "
         "0, i32 0\n";
  out << "  %buffer = load ptr, ptr %buffer_field\n";
  out << "  %object = getelementptr i8, ptr %buffer, i64 %aligned\n";
  out << "  store i64 %next, ptr %offset_field\n";
  out << "  call void @llvm.memset.p0.i64(ptr %object, i8 0, i64 %size, i1 false)\n";
  out << "  call void @__scalanative_tag_object(ptr %object, ptr %descriptor, i32 "
      << runtime::objectOwnershipTag(runtime::ObjectOwnership::Arena) << ")\n";
  out << "  ret ptr %object\n";
  out << "}\n\n";

  out << "define internal void @__scalanative_arena_destroy(ptr %arena) {\n";
  out << "entry:\n";
  out << "  %current_field = getelementptr %scalanative.arena, ptr %arena, i32 0, "
         "i32 0\n";
  out << "  %current = load ptr, ptr %current_field\n";
  out << "  br label %loop\n";
  out << "loop:\n";
  out << "  %block = phi ptr [ %current, %entry ], [ %previous, %release ]\n";
  out << "  %exists = icmp ne ptr %block, null\n";
  out << "  br i1 %exists, label %release, label %done\n";
  out << "release:\n";
  out << "  %buffer_field = getelementptr %scalanative.arena_block, ptr %block, i32 "
         "0, i32 0\n";
  out << "  %buffer = load ptr, ptr %buffer_field\n";
  out << "  %previous_field = getelementptr %scalanative.arena_block, ptr %block, "
         "i32 0, i32 3\n";
  out << "  %previous = load ptr, ptr %previous_field\n";
  out << "  call void @free(ptr %buffer)\n";
  out << "  call void @free(ptr %block)\n";
  out << "  br label %loop\n";
  out << "done:\n";
  out << "  call void @free(ptr %arena)\n";
  out << "  ret void\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_program_arena_alloc(i64 %size, ptr "
         "%descriptor) {\n";
  out << "entry:\n";
  out << "  %current = load ptr, ptr @__scalanative_program_arena\n";
  out << "  %missing = icmp eq ptr %current, null\n";
  out << "  br i1 %missing, label %create, label %allocate\n";
  out << "create:\n";
  out << "  %created = call ptr @__scalanative_arena_create(i64 1048576)\n";
  out << "  store ptr %created, ptr @__scalanative_program_arena\n";
  out << "  br label %allocate\n";
  out << "allocate:\n";
  out << "  %arena = phi ptr [ %current, %entry ], [ %created, %create ]\n";
  out << "  %object = call ptr @__scalanative_arena_alloc(ptr %arena, i64 %size, ptr "
         "%descriptor)\n";
  out << "  ret ptr %object\n";
  out << "}\n\n";

  out << "define internal void @__scalanative_program_arena_destroy() {\n";
  out << "entry:\n";
  out << "  %arena = load ptr, ptr @__scalanative_program_arena\n";
  out << "  %exists = icmp ne ptr %arena, null\n";
  out << "  br i1 %exists, label %destroy, label %done\n";
  out << "destroy:\n";
  out << "  call void @__scalanative_arena_destroy(ptr %arena)\n";
  out << "  store ptr null, ptr @__scalanative_program_arena\n";
  out << "  br label %done\n";
  out << "done:\n";
  out << "  ret void\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_args_from_argv(i32 %argc, ptr %argv) {\n";
  out << "entry:\n";
  out << "  %has_arguments = icmp sgt i32 %argc, 1\n";
  out << "  %candidate_count = sub i32 %argc, 1\n";
  out << "  %argument_count_i32 = select i1 %has_arguments, i32 %candidate_count, "
         "i32 0\n";
  out << "  %argument_count = zext i32 %argument_count_i32 to i64\n";
  out << "  %elements_size = mul i64 %argument_count, 8\n";
  out << "  %allocation_size = add i64 %elements_size, 16\n";
  out << "  %array = call ptr @__scalanative_program_arena_alloc(i64 %allocation_size, "
         "ptr null)\n";
  out << "  %length_slot = getelementptr i8, ptr %array, i64 8\n";
  out << "  store i64 %argument_count, ptr %length_slot\n";
  out << "  br label %copy_loop\n";
  out << "copy_loop:\n";
  out << "  %index = phi i64 [ 0, %entry ], [ %next_index, %copy ]\n";
  out << "  %done = icmp uge i64 %index, %argument_count\n";
  out << "  br i1 %done, label %ready, label %copy\n";
  out << "copy:\n";
  out << "  %argv_index = add i64 %index, 1\n";
  out << "  %source_slot = getelementptr ptr, ptr %argv, i64 %argv_index\n";
  out << "  %argument = load ptr, ptr %source_slot\n";
  out << "  %elements = getelementptr i8, ptr %array, i64 16\n";
  out << "  %destination_slot = getelementptr ptr, ptr %elements, i64 %index\n";
  out << "  store ptr %argument, ptr %destination_slot\n";
  out << "  %next_index = add i64 %index, 1\n";
  out << "  br label %copy_loop\n";
  out << "ready:\n";
  out << "  ret ptr %array\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_require_non_null_receiver(ptr "
         "%receiver) {\n";
  out << "entry:\n";
  out << "  %is_null = icmp eq ptr %receiver, null\n";
  out << "  br i1 %is_null, label %null_receiver, label %ready\n";
  out << "null_receiver:\n";
  out << "  call void @__scalanative_throw_null_receiver()\n";
  out << "  unreachable\n";
  out << "ready:\n";
  out << "  ret ptr %receiver\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_require_non_null_thrown_exception(ptr "
         "%exception) {\n";
  out << "entry:\n";
  out << "  %is_null = icmp eq ptr %exception, null\n";
  out << "  br i1 %is_null, label %null_exception, label %ready\n";
  out << "null_exception:\n";
  out << "  call void @__scalanative_throw_null_exception()\n";
  out << "  unreachable\n";
  out << "ready:\n";
  out << "  ret ptr %exception\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_array_alloc(i32 %length, i64 "
         "%element_size) {\n";
  out << "entry:\n";
  out << "  %is_negative = icmp slt i32 %length, 0\n";
  out << "  br i1 %is_negative, label %negative, label %allocate\n";
  out << "negative:\n";
  out << "  call void @__scalanative_throw_negative_array_size()\n";
  out << "  unreachable\n";
  out << "allocate:\n";
  out << "  %wide_length = zext i32 %length to i64\n";
  out << "  %elements_size = mul i64 %wide_length, %element_size\n";
  out << "  %allocation_size = add i64 %elements_size, " << ObjectHeaderSize + 8
      << "\n";
  out << "  %array = call ptr @__scalanative_program_arena_alloc(i64 "
         "%allocation_size, ptr null)\n";
  out << "  %length_slot = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize
      << "\n";
  out << "  store i64 %wide_length, ptr %length_slot\n";
  out << "  ret ptr %array\n";
  out << "}\n\n";

  out << "define internal i64 @__scalanative_array_length(ptr %array) {\n";
  out << "entry:\n";
  out << "  %is_null = icmp eq ptr %array, null\n";
  out << "  br i1 %is_null, label %null_array, label %read\n";
  out << "read:\n";
  out << "  %length_slot = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %length = load i64, ptr %length_slot\n";
  out << "  ret i64 %length\n";
  out << "null_array:\n";
  out << "  call void @__scalanative_throw_null_array()\n";
  out << "  unreachable\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_array_clone(ptr %array, i64 "
         "%element_size) {\n";
  out << "entry:\n";
  out << "  %length = call i64 @__scalanative_array_length(ptr %array)\n";
  out << "  %narrow_length = trunc i64 %length to i32\n";
  out << "  %clone = call ptr @__scalanative_array_alloc(i32 %narrow_length, i64 "
         "%element_size)\n";
  out << "  %byte_count = mul i64 %length, %element_size\n";
  out << "  %source = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize + 8
      << "\n";
  out << "  %destination = getelementptr i8, ptr %clone, i64 " << ObjectHeaderSize + 8
      << "\n";
  out << "  call void @llvm.memcpy.p0.p0.i64(ptr %destination, ptr %source, i64 "
         "%byte_count, i1 false)\n";
  out << "  ret ptr %clone\n";
  out << "}\n\n";

  out << "define internal void @__scalanative_array_copy(ptr %source, i32 "
         "%source_position, ptr %destination, i32 %destination_position, i32 "
         "%length, i64 %element_size, ptr %target_descriptor, i1 "
         "%check_elements) {\n";
  out << "entry:\n";
  out << "  %source_length = call i64 @__scalanative_array_length(ptr %source)\n";
  out << "  %destination_length = call i64 @__scalanative_array_length(ptr "
         "%destination)\n";
  out << "  %source_position_valid = icmp sge i32 %source_position, 0\n";
  out << "  %destination_position_valid = icmp sge i32 %destination_position, 0\n";
  out << "  %length_valid = icmp sge i32 %length, 0\n";
  out << "  %positions_valid = and i1 %source_position_valid, "
         "%destination_position_valid\n";
  out << "  %arguments_valid = and i1 %positions_valid, %length_valid\n";
  out << "  br i1 %arguments_valid, label %check_ranges, label %out_of_bounds\n";
  out << "check_ranges:\n";
  out << "  %wide_source_position = zext i32 %source_position to i64\n";
  out << "  %wide_destination_position = zext i32 %destination_position to i64\n";
  out << "  %wide_length = zext i32 %length to i64\n";
  out << "  %source_end = add i64 %wide_source_position, %wide_length\n";
  out << "  %destination_end = add i64 %wide_destination_position, %wide_length\n";
  out << "  %source_range_valid = icmp ule i64 %source_end, %source_length\n";
  out << "  %destination_range_valid = icmp ule i64 %destination_end, "
         "%destination_length\n";
  out << "  %ranges_valid = and i1 %source_range_valid, %destination_range_valid\n";
  out << "  br i1 %ranges_valid, label %prepare_copy, label %out_of_bounds\n";
  out << "prepare_copy:\n";
  out << "  %source_byte_offset = mul i64 %wide_source_position, %element_size\n";
  out << "  %destination_byte_offset = mul i64 %wide_destination_position, "
         "%element_size\n";
  out << "  %byte_count = mul i64 %wide_length, %element_size\n";
  out << "  %source_elements = getelementptr i8, ptr %source, i64 "
      << ObjectHeaderSize + 8 << "\n";
  out << "  %destination_elements = getelementptr i8, ptr %destination, i64 "
      << ObjectHeaderSize + 8 << "\n";
  out << "  %source_start = getelementptr i8, ptr %source_elements, i64 "
         "%source_byte_offset\n";
  out << "  %destination_start = getelementptr i8, ptr %destination_elements, i64 "
         "%destination_byte_offset\n";
  out << "  br i1 %check_elements, label %checked_prepare, label %copy\n";
  out << "copy:\n";
  out << "  call void @llvm.memmove.p0.p0.i64(ptr %destination_start, ptr "
         "%source_start, i64 %byte_count, i1 false)\n";
  out << "  ret void\n";
  out << "checked_prepare:\n";
  out << "  %same_array = icmp eq ptr %source, %destination\n";
  out << "  %destination_after_source = icmp ugt i64 "
         "%wide_destination_position, %wide_source_position\n";
  out << "  %destination_inside_source = icmp ult i64 "
         "%wide_destination_position, %source_end\n";
  out << "  %overlap_after = and i1 %destination_after_source, "
         "%destination_inside_source\n";
  out << "  %copy_backwards = and i1 %same_array, %overlap_after\n";
  out << "  br label %checked_loop\n";
  out << "checked_loop:\n";
  out << "  %iteration = phi i64 [ 0, %checked_prepare ], [ %next_iteration, "
         "%checked_store ]\n";
  out << "  %checked_done = icmp uge i64 %iteration, %wide_length\n";
  out << "  br i1 %checked_done, label %checked_complete, label %checked_load\n";
  out << "checked_load:\n";
  out << "  %reverse_remaining = sub i64 %wide_length, %iteration\n";
  out << "  %reverse_index = sub i64 %reverse_remaining, 1\n";
  out << "  %element_index = select i1 %copy_backwards, i64 %reverse_index, i64 "
         "%iteration\n";
  out << "  %source_element_index = add i64 %wide_source_position, "
         "%element_index\n";
  out << "  %destination_element_index = add i64 %wide_destination_position, "
         "%element_index\n";
  out << "  %source_slot = getelementptr ptr, ptr %source_elements, i64 "
         "%source_element_index\n";
  out << "  %destination_slot = getelementptr ptr, ptr %destination_elements, i64 "
         "%destination_element_index\n";
  out << "  %element = load ptr, ptr %source_slot\n";
  out << "  %element_is_null = icmp eq ptr %element, null\n";
  out << "  %element_matches = call i1 @__scalanative_is_instance_of(ptr "
         "%element, ptr %target_descriptor)\n";
  out << "  %element_compatible = or i1 %element_is_null, %element_matches\n";
  out << "  br i1 %element_compatible, label %checked_store, label "
         "%incompatible_element\n";
  out << "checked_store:\n";
  out << "  store ptr %element, ptr %destination_slot\n";
  out << "  %next_iteration = add i64 %iteration, 1\n";
  out << "  br label %checked_loop\n";
  out << "checked_complete:\n";
  out << "  ret void\n";
  out << "incompatible_element:\n";
  out << "  call void @__scalanative_throw_array_store()\n";
  out << "  unreachable\n";
  out << "out_of_bounds:\n";
  out << "  call void @__scalanative_throw_array_index_out_of_bounds()\n";
  out << "  unreachable\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_array_string_slot(ptr %array, i32 %index) "
         "{\n";
  out << "entry:\n";
  out << "  %is_null = icmp eq ptr %array, null\n";
  out << "  br i1 %is_null, label %null_array, label %check_index\n";
  out << "check_index:\n";
  out << "  %is_nonnegative = icmp sge i32 %index, 0\n";
  out << "  br i1 %is_nonnegative, label %check_bounds, label %trap\n";
  out << "check_bounds:\n";
  out << "  %length_slot = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %length = load i64, ptr %length_slot\n";
  out << "  %wide_index = sext i32 %index to i64\n";
  out << "  %in_bounds = icmp ult i64 %wide_index, %length\n";
  out << "  br i1 %in_bounds, label %ready, label %trap\n";
  out << "ready:\n";
  out << "  %elements = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize + 8
      << "\n";
  out << "  %element_slot = getelementptr ptr, ptr %elements, i64 %wide_index\n";
  out << "  ret ptr %element_slot\n";
  out << "trap:\n";
  out << "  call void @__scalanative_throw_array_index_out_of_bounds()\n";
  out << "  unreachable\n";
  out << "null_array:\n";
  out << "  call void @__scalanative_throw_null_array()\n";
  out << "  unreachable\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_array_string_at(ptr %array, i32 %index) "
         "{\n";
  out << "entry:\n";
  out << "  %element_slot = call ptr @__scalanative_array_string_slot(ptr %array, i32 "
         "%index)\n";
  out << "  %element = load ptr, ptr %element_slot\n";
  out << "  ret ptr %element\n";
  out << "}\n\n";

  out << "define internal void @__scalanative_array_string_set(ptr %array, i32 %index, "
         "ptr %value) {\n";
  out << "entry:\n";
  out << "  %element_slot = call ptr @__scalanative_array_string_slot(ptr %array, i32 "
         "%index)\n";
  out << "  store ptr %value, ptr %element_slot\n";
  out << "  ret void\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_array_reference_slot(ptr %array, i32 "
         "%index) "
         "{\n";
  out << "entry:\n";
  out << "  %is_null = icmp eq ptr %array, null\n";
  out << "  br i1 %is_null, label %null_array, label %check_index\n";
  out << "check_index:\n";
  out << "  %is_nonnegative = icmp sge i32 %index, 0\n";
  out << "  br i1 %is_nonnegative, label %check_bounds, label %trap\n";
  out << "check_bounds:\n";
  out << "  %length_slot = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %length = load i64, ptr %length_slot\n";
  out << "  %wide_index = sext i32 %index to i64\n";
  out << "  %in_bounds = icmp ult i64 %wide_index, %length\n";
  out << "  br i1 %in_bounds, label %ready, label %trap\n";
  out << "ready:\n";
  out << "  %elements = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize + 8
      << "\n";
  out << "  %element_slot = getelementptr ptr, ptr %elements, i64 %wide_index\n";
  out << "  ret ptr %element_slot\n";
  out << "trap:\n";
  out << "  call void @__scalanative_throw_array_index_out_of_bounds()\n";
  out << "  unreachable\n";
  out << "null_array:\n";
  out << "  call void @__scalanative_throw_null_array()\n";
  out << "  unreachable\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_array_reference_at(ptr %array, i32 "
         "%index) "
         "{\n";
  out << "entry:\n";
  out << "  %element_slot = call ptr @__scalanative_array_reference_slot(ptr %array, "
         "i32 %index)\n";
  out << "  %element = load ptr, ptr %element_slot\n";
  out << "  ret ptr %element\n";
  out << "}\n\n";

  out << "define internal void @__scalanative_array_reference_set(ptr %array, i32 "
         "%index, "
         "ptr %value) {\n";
  out << "entry:\n";
  out << "  %element_slot = call ptr @__scalanative_array_reference_slot(ptr %array, "
         "i32 %index)\n";
  out << "  store ptr %value, ptr %element_slot\n";
  out << "  ret void\n";
  out << "}\n\n";

  const auto emitNarrowArrayAccessors = [&](std::string_view kind,
                                            std::string_view elementType) {
    out << "define internal ptr @__scalanative_array_" << kind
        << "_slot(ptr %array, i32 %index) {\n";
    out << "entry:\n";
    out << "  %is_null = icmp eq ptr %array, null\n";
    out << "  br i1 %is_null, label %null_array, label %check_index\n";
    out << "check_index:\n";
    out << "  %is_nonnegative = icmp sge i32 %index, 0\n";
    out << "  br i1 %is_nonnegative, label %check_bounds, label %trap\n";
    out << "check_bounds:\n";
    out << "  %length_slot = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize
        << "\n";
    out << "  %length = load i64, ptr %length_slot\n";
    out << "  %wide_index = sext i32 %index to i64\n";
    out << "  %in_bounds = icmp ult i64 %wide_index, %length\n";
    out << "  br i1 %in_bounds, label %ready, label %trap\n";
    out << "ready:\n";
    out << "  %elements = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize + 8
        << "\n";
    out << "  %element_slot = getelementptr " << elementType
        << ", ptr %elements, i64 %wide_index\n";
    out << "  ret ptr %element_slot\n";
    out << "trap:\n";
    out << "  call void @__scalanative_throw_array_index_out_of_bounds()\n";
    out << "  unreachable\n";
    out << "null_array:\n";
    out << "  call void @__scalanative_throw_null_array()\n";
    out << "  unreachable\n";
    out << "}\n\n";

    out << "define internal " << elementType << " @__scalanative_array_" << kind
        << "_at(ptr %array, i32 %index) {\n";
    out << "entry:\n";
    out << "  %element_slot = call ptr @__scalanative_array_" << kind
        << "_slot(ptr %array, i32 %index)\n";
    out << "  %element = load " << elementType << ", ptr %element_slot\n";
    out << "  ret " << elementType << " %element\n";
    out << "}\n\n";

    out << "define internal void @__scalanative_array_" << kind
        << "_set(ptr %array, i32 %index, " << elementType << " %value) {\n";
    out << "entry:\n";
    out << "  %element_slot = call ptr @__scalanative_array_" << kind
        << "_slot(ptr %array, i32 %index)\n";
    out << "  store " << elementType << " %value, ptr %element_slot\n";
    out << "  ret void\n";
    out << "}\n\n";
  };
  emitNarrowArrayAccessors("byte", "i8");
  emitNarrowArrayAccessors("short", "i16");

  out << "define internal ptr @__scalanative_native_bytes_short_slot(ptr %array, "
         "i32 %index) {\n";
  out << "entry:\n";
  out << "  %is_null = icmp eq ptr %array, null\n";
  out << "  br i1 %is_null, label %null_array, label %check_index\n";
  out << "check_index:\n";
  out << "  %wide_index = sext i32 %index to i64\n";
  out << "  %is_nonnegative = icmp sge i64 %wide_index, 0\n";
  out << "  %end = add i64 %wide_index, 2\n";
  out << "  %length_slot = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %length = load i64, ptr %length_slot\n";
  out << "  %range_fits = icmp ule i64 %end, %length\n";
  out << "  %in_bounds = and i1 %is_nonnegative, %range_fits\n";
  out << "  br i1 %in_bounds, label %ready, label %trap\n";
  out << "ready:\n";
  out << "  %elements = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize + 8
      << "\n";
  out << "  %slot = getelementptr i8, ptr %elements, i64 %wide_index\n";
  out << "  ret ptr %slot\n";
  out << "trap:\n";
  out << "  call void @__scalanative_throw_array_index_out_of_bounds()\n";
  out << "  unreachable\n";
  out << "null_array:\n";
  out << "  call void @__scalanative_throw_null_array()\n";
  out << "  unreachable\n";
  out << "}\n\n";

  out << "define internal i16 @__scalanative_native_bytes_get_short(ptr %array, "
         "i32 %index, i1 %little_endian) {\n";
  out << "entry:\n";
  out << "  %slot = call ptr @__scalanative_native_bytes_short_slot(ptr %array, "
         "i32 %index)\n";
  out << "  %second_slot = getelementptr i8, ptr %slot, i64 1\n";
  out << "  %first = load i8, ptr %slot\n";
  out << "  %second = load i8, ptr %second_slot\n";
  out << "  %wide_first = zext i8 %first to i16\n";
  out << "  %wide_second = zext i8 %second to i16\n";
  out << "  %big_high = shl i16 %wide_first, 8\n";
  out << "  %big = or i16 %big_high, %wide_second\n";
  out << "  %little_high = shl i16 %wide_second, 8\n";
  out << "  %little = or i16 %little_high, %wide_first\n";
  out << "  %result = select i1 %little_endian, i16 %little, i16 %big\n";
  out << "  ret i16 %result\n";
  out << "}\n\n";

  out << "define internal void @__scalanative_native_bytes_put_short(ptr %array, "
         "i32 %index, i16 %value, i1 %little_endian) {\n";
  out << "entry:\n";
  out << "  %slot = call ptr @__scalanative_native_bytes_short_slot(ptr %array, "
         "i32 %index)\n";
  out << "  %second_slot = getelementptr i8, ptr %slot, i64 1\n";
  out << "  %low = trunc i16 %value to i8\n";
  out << "  %shifted = lshr i16 %value, 8\n";
  out << "  %high = trunc i16 %shifted to i8\n";
  out << "  %first = select i1 %little_endian, i8 %low, i8 %high\n";
  out << "  %second = select i1 %little_endian, i8 %high, i8 %low\n";
  out << "  store i8 %first, ptr %slot\n";
  out << "  store i8 %second, ptr %second_slot\n";
  out << "  ret void\n";
  out << "}\n\n";

  out << "define internal void @__scalanative_byte_buffer_require(ptr %buffer) {\n";
  out << "entry:\n";
  out << "  %is_null = icmp eq ptr %buffer, null\n";
  out << "  br i1 %is_null, label %null_buffer, label %ready\n";
  out << "ready:\n";
  out << "  ret void\n";
  out << "null_buffer:\n";
  out << "  call void @__scalanative_throw_null_receiver()\n";
  out << "  unreachable\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_byte_buffer_wrap(ptr %array) {\n";
  out << "entry:\n";
  out << "  %is_null = icmp eq ptr %array, null\n";
  out << "  br i1 %is_null, label %null_array, label %allocate\n";
  out << "allocate:\n";
  out << "  %length_slot = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %wide_length = load i64, ptr %length_slot\n";
  out << "  %length = trunc i64 %wide_length to i32\n";
  out << "  %buffer = call ptr @__scalanative_box_alloc(i64 " << ByteBufferSize
      << ", ptr null)\n";
  out << "  %backing_slot = getelementptr i8, ptr %buffer, i64 "
      << ByteBufferBackingOffset << "\n";
  out << "  %capacity_slot = getelementptr i8, ptr %buffer, i64 "
      << ByteBufferCapacityOffset << "\n";
  out << "  %position_slot = getelementptr i8, ptr %buffer, i64 "
      << ByteBufferPositionOffset << "\n";
  out << "  %limit_slot = getelementptr i8, ptr %buffer, i64 " << ByteBufferLimitOffset
      << "\n";
  out << "  %mark_slot = getelementptr i8, ptr %buffer, i64 " << ByteBufferMarkOffset
      << "\n";
  out << "  store ptr %array, ptr %backing_slot\n";
  out << "  store i32 %length, ptr %capacity_slot\n";
  out << "  store i32 0, ptr %position_slot\n";
  out << "  store i32 %length, ptr %limit_slot\n";
  out << "  store i32 -1, ptr %mark_slot\n";
  out << "  ret ptr %buffer\n";
  out << "null_array:\n";
  out << "  call void @__scalanative_throw_null_array()\n";
  out << "  unreachable\n";
  out << "}\n\n";

  const auto emitByteBufferIntQuery = [&](std::string_view name, std::size_t offset) {
    out << "define internal i32 @__scalanative_byte_buffer_" << name
        << "(ptr %buffer) {\n";
    out << "entry:\n";
    out << "  call void @__scalanative_byte_buffer_require(ptr %buffer)\n";
    out << "  %slot = getelementptr i8, ptr %buffer, i64 " << offset << "\n";
    out << "  %value = load i32, ptr %slot\n";
    out << "  ret i32 %value\n";
    out << "}\n\n";
  };
  emitByteBufferIntQuery("capacity", ByteBufferCapacityOffset);
  emitByteBufferIntQuery("position", ByteBufferPositionOffset);
  emitByteBufferIntQuery("limit", ByteBufferLimitOffset);

  out << "define internal i32 @__scalanative_byte_buffer_remaining(ptr %buffer) {\n";
  out << "entry:\n";
  out << "  call void @__scalanative_byte_buffer_require(ptr %buffer)\n";
  out << "  %position_slot = getelementptr i8, ptr %buffer, i64 "
      << ByteBufferPositionOffset << "\n";
  out << "  %limit_slot = getelementptr i8, ptr %buffer, i64 " << ByteBufferLimitOffset
      << "\n";
  out << "  %position = load i32, ptr %position_slot\n";
  out << "  %limit = load i32, ptr %limit_slot\n";
  out << "  %remaining = sub i32 %limit, %position\n";
  out << "  ret i32 %remaining\n";
  out << "}\n\n";

  out << "define internal i1 @__scalanative_byte_buffer_has_remaining(ptr %buffer) "
         "{\n";
  out << "entry:\n";
  out << "  %remaining = call i32 @__scalanative_byte_buffer_remaining(ptr %buffer)\n";
  out << "  %has_remaining = icmp sgt i32 %remaining, 0\n";
  out << "  ret i1 %has_remaining\n";
  out << "}\n\n";

  out << "define internal i8 @__scalanative_byte_buffer_get(ptr %buffer) {\n";
  out << "entry:\n";
  out << "  call void @__scalanative_byte_buffer_require(ptr %buffer)\n";
  out << "  %position_slot = getelementptr i8, ptr %buffer, i64 "
      << ByteBufferPositionOffset << "\n";
  out << "  %limit_slot = getelementptr i8, ptr %buffer, i64 " << ByteBufferLimitOffset
      << "\n";
  out << "  %position = load i32, ptr %position_slot\n";
  out << "  %limit = load i32, ptr %limit_slot\n";
  out << "  %has_remaining = icmp slt i32 %position, %limit\n";
  out << "  br i1 %has_remaining, label %read, label %underflow\n";
  out << "read:\n";
  out << "  %backing_slot = getelementptr i8, ptr %buffer, i64 "
      << ByteBufferBackingOffset << "\n";
  out << "  %array = load ptr, ptr %backing_slot\n";
  out << "  %elements = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize + 8
      << "\n";
  out << "  %wide_position = sext i32 %position to i64\n";
  out << "  %element_slot = getelementptr i8, ptr %elements, i64 %wide_position\n";
  out << "  %value = load i8, ptr %element_slot\n";
  out << "  %next_position = add i32 %position, 1\n";
  out << "  store i32 %next_position, ptr %position_slot\n";
  out << "  ret i8 %value\n";
  out << "underflow:\n";
  out << "  call void @__scalanative_throw_byte_buffer_underflow()\n";
  out << "  unreachable\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_byte_buffer_put(ptr %buffer, i8 %value) "
         "{\n";
  out << "entry:\n";
  out << "  call void @__scalanative_byte_buffer_require(ptr %buffer)\n";
  out << "  %position_slot = getelementptr i8, ptr %buffer, i64 "
      << ByteBufferPositionOffset << "\n";
  out << "  %limit_slot = getelementptr i8, ptr %buffer, i64 " << ByteBufferLimitOffset
      << "\n";
  out << "  %position = load i32, ptr %position_slot\n";
  out << "  %limit = load i32, ptr %limit_slot\n";
  out << "  %has_remaining = icmp slt i32 %position, %limit\n";
  out << "  br i1 %has_remaining, label %write, label %overflow\n";
  out << "write:\n";
  out << "  %backing_slot = getelementptr i8, ptr %buffer, i64 "
      << ByteBufferBackingOffset << "\n";
  out << "  %array = load ptr, ptr %backing_slot\n";
  out << "  %elements = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize + 8
      << "\n";
  out << "  %wide_position = sext i32 %position to i64\n";
  out << "  %element_slot = getelementptr i8, ptr %elements, i64 %wide_position\n";
  out << "  store i8 %value, ptr %element_slot\n";
  out << "  %next_position = add i32 %position, 1\n";
  out << "  store i32 %next_position, ptr %position_slot\n";
  out << "  ret ptr %buffer\n";
  out << "overflow:\n";
  out << "  call void @__scalanative_throw_byte_buffer_overflow()\n";
  out << "  unreachable\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_byte_buffer_set_position(ptr %buffer, "
         "i32 %value) {\n";
  out << "entry:\n";
  out << "  call void @__scalanative_byte_buffer_require(ptr %buffer)\n";
  out << "  %limit_slot = getelementptr i8, ptr %buffer, i64 " << ByteBufferLimitOffset
      << "\n";
  out << "  %limit = load i32, ptr %limit_slot\n";
  out << "  %nonnegative = icmp sge i32 %value, 0\n";
  out << "  %within_limit = icmp sle i32 %value, %limit\n";
  out << "  %valid = and i1 %nonnegative, %within_limit\n";
  out << "  br i1 %valid, label %update, label %invalid\n";
  out << "update:\n";
  out << "  %position_slot = getelementptr i8, ptr %buffer, i64 "
      << ByteBufferPositionOffset << "\n";
  out << "  %mark_slot = getelementptr i8, ptr %buffer, i64 " << ByteBufferMarkOffset
      << "\n";
  out << "  %mark = load i32, ptr %mark_slot\n";
  out << "  %invalidate_mark = icmp sgt i32 %mark, %value\n";
  out << "  %next_mark = select i1 %invalidate_mark, i32 -1, i32 %mark\n";
  out << "  store i32 %value, ptr %position_slot\n";
  out << "  store i32 %next_mark, ptr %mark_slot\n";
  out << "  ret ptr %buffer\n";
  out << "invalid:\n";
  out << "  call void @__scalanative_throw_byte_buffer_position()\n";
  out << "  unreachable\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_byte_buffer_set_limit(ptr %buffer, "
         "i32 %value) {\n";
  out << "entry:\n";
  out << "  call void @__scalanative_byte_buffer_require(ptr %buffer)\n";
  out << "  %capacity_slot = getelementptr i8, ptr %buffer, i64 "
      << ByteBufferCapacityOffset << "\n";
  out << "  %capacity = load i32, ptr %capacity_slot\n";
  out << "  %nonnegative = icmp sge i32 %value, 0\n";
  out << "  %within_capacity = icmp sle i32 %value, %capacity\n";
  out << "  %valid = and i1 %nonnegative, %within_capacity\n";
  out << "  br i1 %valid, label %update, label %invalid\n";
  out << "update:\n";
  out << "  %position_slot = getelementptr i8, ptr %buffer, i64 "
      << ByteBufferPositionOffset << "\n";
  out << "  %mark_slot = getelementptr i8, ptr %buffer, i64 " << ByteBufferMarkOffset
      << "\n";
  out << "  %position = load i32, ptr %position_slot\n";
  out << "  %mark = load i32, ptr %mark_slot\n";
  out << "  %clamp_position = icmp sgt i32 %position, %value\n";
  out << "  %next_position = select i1 %clamp_position, i32 %value, i32 %position\n";
  out << "  %invalidate_mark = icmp sgt i32 %mark, %value\n";
  out << "  %next_mark = select i1 %invalidate_mark, i32 -1, i32 %mark\n";
  out << "  %limit_slot = getelementptr i8, ptr %buffer, i64 " << ByteBufferLimitOffset
      << "\n";
  out << "  store i32 %value, ptr %limit_slot\n";
  out << "  store i32 %next_position, ptr %position_slot\n";
  out << "  store i32 %next_mark, ptr %mark_slot\n";
  out << "  ret ptr %buffer\n";
  out << "invalid:\n";
  out << "  call void @__scalanative_throw_byte_buffer_limit()\n";
  out << "  unreachable\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_byte_buffer_clear(ptr %buffer) {\n";
  out << "entry:\n";
  out << "  call void @__scalanative_byte_buffer_require(ptr %buffer)\n";
  out << "  %capacity_slot = getelementptr i8, ptr %buffer, i64 "
      << ByteBufferCapacityOffset << "\n";
  out << "  %position_slot = getelementptr i8, ptr %buffer, i64 "
      << ByteBufferPositionOffset << "\n";
  out << "  %limit_slot = getelementptr i8, ptr %buffer, i64 " << ByteBufferLimitOffset
      << "\n";
  out << "  %mark_slot = getelementptr i8, ptr %buffer, i64 " << ByteBufferMarkOffset
      << "\n";
  out << "  %capacity = load i32, ptr %capacity_slot\n";
  out << "  store i32 0, ptr %position_slot\n";
  out << "  store i32 %capacity, ptr %limit_slot\n";
  out << "  store i32 -1, ptr %mark_slot\n";
  out << "  ret ptr %buffer\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_byte_buffer_flip(ptr %buffer) {\n";
  out << "entry:\n";
  out << "  call void @__scalanative_byte_buffer_require(ptr %buffer)\n";
  out << "  %position_slot = getelementptr i8, ptr %buffer, i64 "
      << ByteBufferPositionOffset << "\n";
  out << "  %limit_slot = getelementptr i8, ptr %buffer, i64 " << ByteBufferLimitOffset
      << "\n";
  out << "  %mark_slot = getelementptr i8, ptr %buffer, i64 " << ByteBufferMarkOffset
      << "\n";
  out << "  %position = load i32, ptr %position_slot\n";
  out << "  store i32 %position, ptr %limit_slot\n";
  out << "  store i32 0, ptr %position_slot\n";
  out << "  store i32 -1, ptr %mark_slot\n";
  out << "  ret ptr %buffer\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_byte_buffer_rewind(ptr %buffer) {\n";
  out << "entry:\n";
  out << "  call void @__scalanative_byte_buffer_require(ptr %buffer)\n";
  out << "  %position_slot = getelementptr i8, ptr %buffer, i64 "
      << ByteBufferPositionOffset << "\n";
  out << "  %mark_slot = getelementptr i8, ptr %buffer, i64 " << ByteBufferMarkOffset
      << "\n";
  out << "  store i32 0, ptr %position_slot\n";
  out << "  store i32 -1, ptr %mark_slot\n";
  out << "  ret ptr %buffer\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_array_int_slot(ptr %array, i32 %index) "
         "{\n";
  out << "entry:\n";
  out << "  %is_null = icmp eq ptr %array, null\n";
  out << "  br i1 %is_null, label %null_array, label %check_index\n";
  out << "check_index:\n";
  out << "  %is_nonnegative = icmp sge i32 %index, 0\n";
  out << "  br i1 %is_nonnegative, label %check_bounds, label %trap\n";
  out << "check_bounds:\n";
  out << "  %length_slot = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %length = load i64, ptr %length_slot\n";
  out << "  %wide_index = sext i32 %index to i64\n";
  out << "  %in_bounds = icmp ult i64 %wide_index, %length\n";
  out << "  br i1 %in_bounds, label %ready, label %trap\n";
  out << "ready:\n";
  out << "  %elements = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize + 8
      << "\n";
  out << "  %element_slot = getelementptr i32, ptr %elements, i64 %wide_index\n";
  out << "  ret ptr %element_slot\n";
  out << "trap:\n";
  out << "  call void @__scalanative_throw_array_index_out_of_bounds()\n";
  out << "  unreachable\n";
  out << "null_array:\n";
  out << "  call void @__scalanative_throw_null_array()\n";
  out << "  unreachable\n";
  out << "}\n\n";

  out << "define internal i32 @__scalanative_array_int_at(ptr %array, i32 %index) {\n";
  out << "entry:\n";
  out << "  %element_slot = call ptr @__scalanative_array_int_slot(ptr %array, i32 "
         "%index)\n";
  out << "  %element = load i32, ptr %element_slot\n";
  out << "  ret i32 %element\n";
  out << "}\n\n";

  out << "define internal void @__scalanative_array_int_set(ptr %array, i32 %index, "
         "i32 %value) {\n";
  out << "entry:\n";
  out << "  %element_slot = call ptr @__scalanative_array_int_slot(ptr %array, i32 "
         "%index)\n";
  out << "  store i32 %value, ptr %element_slot\n";
  out << "  ret void\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_array_boolean_slot(ptr %array, i32 "
         "%index) "
         "{\n";
  out << "entry:\n";
  out << "  %is_null = icmp eq ptr %array, null\n";
  out << "  br i1 %is_null, label %null_array, label %check_index\n";
  out << "check_index:\n";
  out << "  %is_nonnegative = icmp sge i32 %index, 0\n";
  out << "  br i1 %is_nonnegative, label %check_bounds, label %trap\n";
  out << "check_bounds:\n";
  out << "  %length_slot = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %length = load i64, ptr %length_slot\n";
  out << "  %wide_index = sext i32 %index to i64\n";
  out << "  %in_bounds = icmp ult i64 %wide_index, %length\n";
  out << "  br i1 %in_bounds, label %ready, label %trap\n";
  out << "ready:\n";
  out << "  %elements = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize + 8
      << "\n";
  out << "  %element_slot = getelementptr i8, ptr %elements, i64 %wide_index\n";
  out << "  ret ptr %element_slot\n";
  out << "trap:\n";
  out << "  call void @__scalanative_throw_array_index_out_of_bounds()\n";
  out << "  unreachable\n";
  out << "null_array:\n";
  out << "  call void @__scalanative_throw_null_array()\n";
  out << "  unreachable\n";
  out << "}\n\n";

  out << "define internal i1 @__scalanative_array_boolean_at(ptr %array, i32 %index) "
         "{\n";
  out << "entry:\n";
  out << "  %element_slot = call ptr @__scalanative_array_boolean_slot(ptr %array, "
         "i32 %index)\n";
  out << "  %element = load i1, ptr %element_slot\n";
  out << "  ret i1 %element\n";
  out << "}\n\n";

  out << "define internal void @__scalanative_array_boolean_set(ptr %array, i32 "
         "%index, "
         "i1 %value) {\n";
  out << "entry:\n";
  out << "  %element_slot = call ptr @__scalanative_array_boolean_slot(ptr %array, "
         "i32 %index)\n";
  out << "  store i1 %value, ptr %element_slot\n";
  out << "  ret void\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_array_long_slot(ptr %array, i32 %index) "
         "{\n";
  out << "entry:\n";
  out << "  %is_null = icmp eq ptr %array, null\n";
  out << "  br i1 %is_null, label %null_array, label %check_index\n";
  out << "check_index:\n";
  out << "  %is_nonnegative = icmp sge i32 %index, 0\n";
  out << "  br i1 %is_nonnegative, label %check_bounds, label %trap\n";
  out << "check_bounds:\n";
  out << "  %length_slot = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %length = load i64, ptr %length_slot\n";
  out << "  %wide_index = sext i32 %index to i64\n";
  out << "  %in_bounds = icmp ult i64 %wide_index, %length\n";
  out << "  br i1 %in_bounds, label %ready, label %trap\n";
  out << "ready:\n";
  out << "  %elements = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize + 8
      << "\n";
  out << "  %element_slot = getelementptr i64, ptr %elements, i64 %wide_index\n";
  out << "  ret ptr %element_slot\n";
  out << "trap:\n";
  out << "  call void @__scalanative_throw_array_index_out_of_bounds()\n";
  out << "  unreachable\n";
  out << "null_array:\n";
  out << "  call void @__scalanative_throw_null_array()\n";
  out << "  unreachable\n";
  out << "}\n\n";

  out << "define internal i64 @__scalanative_array_long_at(ptr %array, i32 %index) {\n";
  out << "entry:\n";
  out << "  %element_slot = call ptr @__scalanative_array_long_slot(ptr %array, i32 "
         "%index)\n";
  out << "  %element = load i64, ptr %element_slot\n";
  out << "  ret i64 %element\n";
  out << "}\n\n";

  out << "define internal void @__scalanative_array_long_set(ptr %array, i32 %index, "
         "i64 %value) {\n";
  out << "entry:\n";
  out << "  %element_slot = call ptr @__scalanative_array_long_slot(ptr %array, i32 "
         "%index)\n";
  out << "  store i64 %value, ptr %element_slot\n";
  out << "  ret void\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_array_double_slot(ptr %array, i32 %index) "
         "{\n";
  out << "entry:\n";
  out << "  %is_null = icmp eq ptr %array, null\n";
  out << "  br i1 %is_null, label %null_array, label %check_index\n";
  out << "check_index:\n";
  out << "  %is_nonnegative = icmp sge i32 %index, 0\n";
  out << "  br i1 %is_nonnegative, label %check_bounds, label %trap\n";
  out << "check_bounds:\n";
  out << "  %length_slot = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %length = load i64, ptr %length_slot\n";
  out << "  %wide_index = sext i32 %index to i64\n";
  out << "  %in_bounds = icmp ult i64 %wide_index, %length\n";
  out << "  br i1 %in_bounds, label %ready, label %trap\n";
  out << "ready:\n";
  out << "  %elements = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize + 8
      << "\n";
  out << "  %element_slot = getelementptr double, ptr %elements, i64 %wide_index\n";
  out << "  ret ptr %element_slot\n";
  out << "trap:\n";
  out << "  call void @__scalanative_throw_array_index_out_of_bounds()\n";
  out << "  unreachable\n";
  out << "null_array:\n";
  out << "  call void @__scalanative_throw_null_array()\n";
  out << "  unreachable\n";
  out << "}\n\n";

  out << "define internal double @__scalanative_array_double_at(ptr %array, i32 "
         "%index) "
         "{\n";
  out << "entry:\n";
  out << "  %element_slot = call ptr @__scalanative_array_double_slot(ptr %array, "
         "i32 %index)\n";
  out << "  %element = load double, ptr %element_slot\n";
  out << "  ret double %element\n";
  out << "}\n\n";

  out << "define internal void @__scalanative_array_double_set(ptr %array, i32 %index, "
         "double %value) {\n";
  out << "entry:\n";
  out << "  %element_slot = call ptr @__scalanative_array_double_slot(ptr %array, "
         "i32 %index)\n";
  out << "  store double %value, ptr %element_slot\n";
  out << "  ret void\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_array_float_slot(ptr %array, i32 %index) "
         "{\n";
  out << "entry:\n";
  out << "  %is_null = icmp eq ptr %array, null\n";
  out << "  br i1 %is_null, label %null_array, label %check_index\n";
  out << "check_index:\n";
  out << "  %is_nonnegative = icmp sge i32 %index, 0\n";
  out << "  br i1 %is_nonnegative, label %check_bounds, label %trap\n";
  out << "check_bounds:\n";
  out << "  %length_slot = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %length = load i64, ptr %length_slot\n";
  out << "  %wide_index = sext i32 %index to i64\n";
  out << "  %in_bounds = icmp ult i64 %wide_index, %length\n";
  out << "  br i1 %in_bounds, label %ready, label %trap\n";
  out << "ready:\n";
  out << "  %elements = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize + 8
      << "\n";
  out << "  %element_slot = getelementptr float, ptr %elements, i64 %wide_index\n";
  out << "  ret ptr %element_slot\n";
  out << "trap:\n";
  out << "  call void @__scalanative_throw_array_index_out_of_bounds()\n";
  out << "  unreachable\n";
  out << "null_array:\n";
  out << "  call void @__scalanative_throw_null_array()\n";
  out << "  unreachable\n";
  out << "}\n\n";

  out << "define internal float @__scalanative_array_float_at(ptr %array, i32 %index) "
         "{\n";
  out << "entry:\n";
  out << "  %element_slot = call ptr @__scalanative_array_float_slot(ptr %array, "
         "i32 %index)\n";
  out << "  %element = load float, ptr %element_slot\n";
  out << "  ret float %element\n";
  out << "}\n\n";

  out << "define internal void @__scalanative_array_float_set(ptr %array, i32 %index, "
         "float %value) {\n";
  out << "entry:\n";
  out << "  %element_slot = call ptr @__scalanative_array_float_slot(ptr %array, "
         "i32 %index)\n";
  out << "  store float %value, ptr %element_slot\n";
  out << "  ret void\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_array_char_slot(ptr %array, i32 %index) "
         "{\n";
  out << "entry:\n";
  out << "  %is_null = icmp eq ptr %array, null\n";
  out << "  br i1 %is_null, label %null_array, label %check_index\n";
  out << "check_index:\n";
  out << "  %is_nonnegative = icmp sge i32 %index, 0\n";
  out << "  br i1 %is_nonnegative, label %check_bounds, label %trap\n";
  out << "check_bounds:\n";
  out << "  %length_slot = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %length = load i64, ptr %length_slot\n";
  out << "  %wide_index = sext i32 %index to i64\n";
  out << "  %in_bounds = icmp ult i64 %wide_index, %length\n";
  out << "  br i1 %in_bounds, label %ready, label %trap\n";
  out << "ready:\n";
  out << "  %elements = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize + 8
      << "\n";
  out << "  %element_slot = getelementptr i32, ptr %elements, i64 %wide_index\n";
  out << "  ret ptr %element_slot\n";
  out << "trap:\n";
  out << "  call void @__scalanative_throw_array_index_out_of_bounds()\n";
  out << "  unreachable\n";
  out << "null_array:\n";
  out << "  call void @__scalanative_throw_null_array()\n";
  out << "  unreachable\n";
  out << "}\n\n";

  out << "define internal i32 @__scalanative_array_char_at(ptr %array, i32 %index) {\n";
  out << "entry:\n";
  out << "  %element_slot = call ptr @__scalanative_array_char_slot(ptr %array, i32 "
         "%index)\n";
  out << "  %element = load i32, ptr %element_slot\n";
  out << "  ret i32 %element\n";
  out << "}\n\n";

  out << "define internal void @__scalanative_array_char_set(ptr %array, i32 %index, "
         "i32 %value) {\n";
  out << "entry:\n";
  out << "  %element_slot = call ptr @__scalanative_array_char_slot(ptr %array, i32 "
         "%index)\n";
  out << "  store i32 %value, ptr %element_slot\n";
  out << "  ret void\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_string_concat(ptr %left, ptr %right) {\n";
  out << "entry:\n";
  out << "  %left_length = call i64 @strlen(ptr %left)\n";
  out << "  %right_length = call i64 @strlen(ptr %right)\n";
  out << "  %combined_length = add i64 %left_length, %right_length\n";
  out << "  %payload_size = add i64 %combined_length, 1\n";
  out << "  %allocation_size = add i64 %payload_size, 8\n";
  out << "  %storage = call ptr @__scalanative_program_arena_alloc(i64 "
         "%allocation_size, ptr null)\n";
  out << "  %result = getelementptr i8, ptr %storage, i64 8\n";
  out << "  call ptr @strcpy(ptr %result, ptr %left)\n";
  out << "  call ptr @strcat(ptr %result, ptr %right)\n";
  out << "  ret ptr %result\n";
  out << "}\n\n";

  emitStringFormatHelper(out, "__scalanative_string_from_int", "i32", 12,
                         ".fmt.string.int", 3);
  emitStringFormatHelper(out, "__scalanative_string_from_long", "i64", 21,
                         ".fmt.string.long", 5);
  emitStringFormatHelper(out, "__scalanative_string_from_float", "float", 512,
                         ".fmt.string.float", 3, true);
  emitStringFormatHelper(out, "__scalanative_string_from_double", "double", 512,
                         ".fmt.string.float", 3);
  emitStringFormatHelper(out, "__scalanative_string_from_char", "i32", 8,
                         ".fmt.string.char", 3);

  out << "define internal ptr @__scalanative_string_from_boolean(i1 %value) {\n";
  out << "entry:\n";
  out << "  %payload_size = select i1 %value, i64 5, i64 6\n";
  out << "  %allocation_size = add i64 %payload_size, " << ObjectHeaderSize << "\n";
  out << "  %storage = call ptr @__scalanative_program_arena_alloc(i64 "
         "%allocation_size, ptr null)\n";
  out << "  %result = getelementptr i8, ptr %storage, i64 " << ObjectHeaderSize << "\n";
  out << "  br i1 %value, label %is_true, label %is_false\n";
  out << "is_true:\n";
  out << "  call ptr @strcpy(ptr %result, ptr getelementptr inbounds ([5 x i8], ptr "
         "@.str.boolean.true, i64 0, i64 0))\n";
  out << "  br label %done\n";
  out << "is_false:\n";
  out << "  call ptr @strcpy(ptr %result, ptr getelementptr inbounds ([6 x i8], ptr "
         "@.str.boolean.false, i64 0, i64 0))\n";
  out << "  br label %done\n";
  out << "done:\n";
  out << "  ret ptr %result\n";
  out << "}\n\n";

  out << "define internal i32 @__scalanative_boolean_hash_code(i1 %value) {\n";
  out << "entry:\n";
  out << "  %hash = select i1 %value, i32 1231, i32 1237\n";
  out << "  ret i32 %hash\n";
  out << "}\n\n";

  out << "define internal i32 @__scalanative_long_hash_code(i64 %value) {\n";
  out << "entry:\n";
  out << "  %shifted = lshr i64 %value, 32\n";
  out << "  %folded = xor i64 %value, %shifted\n";
  out << "  %hash = trunc i64 %folded to i32\n";
  out << "  ret i32 %hash\n";
  out << "}\n\n";

  out << "define internal i32 @__scalanative_float_hash_code(float %value) {\n";
  out << "entry:\n";
  out << "  %hash = bitcast float %value to i32\n";
  out << "  ret i32 %hash\n";
  out << "}\n\n";

  out << "define internal i32 @__scalanative_char_hash_code(i32 %value) {\n";
  out << "entry:\n";
  out << "  ret i32 %value\n";
  out << "}\n\n";

  out << "define internal i32 @__scalanative_double_hash_code(double %value) {\n";
  out << "entry:\n";
  out << "  %bits = bitcast double %value to i64\n";
  out << "  %hash = call i32 @__scalanative_long_hash_code(i64 %bits)\n";
  out << "  ret i32 %hash\n";
  out << "}\n\n";

  out << "define internal i32 @__scalanative_string_hash_code(ptr %string) {\n";
  out << "entry:\n";
  out << "  %is_null = icmp eq ptr %string, null\n";
  out << "  br i1 %is_null, label %null_value, label %loop\n";
  out << "null_value:\n";
  out << "  ret i32 0\n";
  out << "loop:\n";
  out << "  %index = phi i64 [ 0, %entry ], [ %next_index, %advance ]\n";
  out << "  %hash = phi i32 [ 0, %entry ], [ %next_hash, %advance ]\n";
  out << "  %char_ptr = getelementptr i8, ptr %string, i64 %index\n";
  out << "  %byte = load i8, ptr %char_ptr\n";
  out << "  %is_end = icmp eq i8 %byte, 0\n";
  out << "  br i1 %is_end, label %done, label %advance\n";
  out << "advance:\n";
  out << "  %wide = zext i8 %byte to i32\n";
  out << "  %scaled = mul i32 %hash, 31\n";
  out << "  %next_hash = add i32 %scaled, %wide\n";
  out << "  %next_index = add i64 %index, 1\n";
  out << "  br label %loop\n";
  out << "done:\n";
  out << "  ret i32 %hash\n";
  out << "}\n\n";

  out << "define internal i32 @__scalanative_object_identity_hash_code(ptr %object) "
         "{\n";
  out << "entry:\n";
  out << "  %address = ptrtoint ptr %object to i64\n";
  out << "  %shifted = lshr i64 %address, 32\n";
  out << "  %folded = xor i64 %address, %shifted\n";
  out << "  %hash = trunc i64 %folded to i32\n";
  out << "  ret i32 %hash\n";
  out << "}\n\n";

  out << "define internal i32 @__scalanative_any_hash_code(ptr %object) {\n";
  out << "entry:\n";
  out << "  %is_null = icmp eq ptr %object, null\n";
  out << "  br i1 %is_null, label %null_value, label %load_descriptor\n";
  out << "null_value:\n";
  out << "  ret i32 0\n";
  out << "load_descriptor:\n";
  out << "  %descriptor = call ptr @__scalanative_object_descriptor(ptr %object)\n";
  out << "  %kind_field = getelementptr %scalanative.type_descriptor, ptr "
         "%descriptor, i32 0, i32 "
      << TypeDescriptorKindIndex << "\n";
  out << "  %kind = load i32, ptr %kind_field\n";
  out << "  %is_boxed = icmp eq i32 %kind, "
      << static_cast<std::uint32_t>(runtime::RuntimeTypeKind::BoxedPrimitive) << "\n";
  out << "  br i1 %is_boxed, label %boxed, label %object_hash\n";
  out << "boxed:\n";
  out << "  %type_id_field = getelementptr %scalanative.type_descriptor, ptr "
         "%descriptor, i32 0, i32 "
      << TypeDescriptorTypeIdIndex << "\n";
  out << "  %type_id = load i32, ptr %type_id_field\n";
  out << "  switch i32 %type_id, label %object_hash [\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Unit)
      << ", label %boxed_unit\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Boolean)
      << ", label %boxed_boolean\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Byte)
      << ", label %boxed_byte\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Short)
      << ", label %boxed_short\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Int)
      << ", label %boxed_int\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Long)
      << ", label %boxed_long\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Float)
      << ", label %boxed_float\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Double)
      << ", label %boxed_double\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Char)
      << ", label %boxed_char\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Symbol)
      << ", label %boxed_symbol\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::String)
      << ", label %boxed_string\n";
  out << "  ]\n";
  out << "boxed_unit:\n";
  out << "  ret i32 0\n";
  out << "boxed_boolean:\n";
  out << "  %boolean_payload = getelementptr i8, ptr %object, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %boolean_value = load i1, ptr %boolean_payload\n";
  out << "  %boolean_hash = call i32 @__scalanative_boolean_hash_code(i1 "
         "%boolean_value)\n";
  out << "  ret i32 %boolean_hash\n";
  out << "boxed_byte:\n";
  out << "  %byte_payload = getelementptr i8, ptr %object, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %byte_value = load i8, ptr %byte_payload\n";
  out << "  %byte_hash = sext i8 %byte_value to i32\n";
  out << "  ret i32 %byte_hash\n";
  out << "boxed_short:\n";
  out << "  %short_payload = getelementptr i8, ptr %object, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %short_value = load i16, ptr %short_payload\n";
  out << "  %short_hash = sext i16 %short_value to i32\n";
  out << "  ret i32 %short_hash\n";
  out << "boxed_int:\n";
  out << "  %int_payload = getelementptr i8, ptr %object, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %int_value = load i32, ptr %int_payload\n";
  out << "  ret i32 %int_value\n";
  out << "boxed_long:\n";
  out << "  %long_payload = getelementptr i8, ptr %object, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %long_value = load i64, ptr %long_payload\n";
  out << "  %long_hash = call i32 @__scalanative_long_hash_code(i64 %long_value)\n";
  out << "  ret i32 %long_hash\n";
  out << "boxed_float:\n";
  out << "  %float_payload = getelementptr i8, ptr %object, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %float_value = load float, ptr %float_payload\n";
  out << "  %float_hash = call i32 @__scalanative_float_hash_code(float "
         "%float_value)\n";
  out << "  ret i32 %float_hash\n";
  out << "boxed_double:\n";
  out << "  %double_payload = getelementptr i8, ptr %object, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %double_value = load double, ptr %double_payload\n";
  out << "  %double_hash = call i32 @__scalanative_double_hash_code(double "
         "%double_value)\n";
  out << "  ret i32 %double_hash\n";
  out << "boxed_char:\n";
  out << "  %char_payload = getelementptr i8, ptr %object, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %char_value = load i32, ptr %char_payload\n";
  out << "  ret i32 %char_value\n";
  out << "boxed_symbol:\n";
  out << "  %symbol_payload = getelementptr i8, ptr %object, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %symbol_value = load ptr, ptr %symbol_payload\n";
  out << "  %symbol_hash = call i32 @__scalanative_string_hash_code(ptr "
         "%symbol_value)\n";
  out << "  ret i32 %symbol_hash\n";
  out << "boxed_string:\n";
  out << "  %string_payload = getelementptr i8, ptr %object, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %string_value = load ptr, ptr %string_payload\n";
  out << "  %string_hash = call i32 @__scalanative_string_hash_code(ptr "
         "%string_value)\n";
  out << "  ret i32 %string_hash\n";
  out << "object_hash:\n";
  out << "  %object_hash_value = call i32 @__scalanative_object_identity_hash_code("
         "ptr %object)\n";
  out << "  ret i32 %object_hash_value\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_any_to_string(ptr %object) {\n";
  out << "entry:\n";
  out << "  %is_null = icmp eq ptr %object, null\n";
  out << "  br i1 %is_null, label %null_value, label %load_descriptor\n";
  out << "null_value:\n";
  out << "  ret ptr getelementptr inbounds ([5 x i8], ptr @.str.null, i64 0, i64 "
         "0)\n";
  out << "load_descriptor:\n";
  out << "  %descriptor = call ptr @__scalanative_object_descriptor(ptr %object)\n";
  out << "  %kind_field = getelementptr %scalanative.type_descriptor, ptr "
         "%descriptor, i32 0, i32 "
      << TypeDescriptorKindIndex << "\n";
  out << "  %kind = load i32, ptr %kind_field\n";
  out << "  %is_boxed = icmp eq i32 %kind, "
      << static_cast<std::uint32_t>(runtime::RuntimeTypeKind::BoxedPrimitive) << "\n";
  out << "  br i1 %is_boxed, label %boxed, label %object_to_string\n";
  out << "boxed:\n";
  out << "  %type_id_field = getelementptr %scalanative.type_descriptor, ptr "
         "%descriptor, i32 0, i32 "
      << TypeDescriptorTypeIdIndex << "\n";
  out << "  %type_id = load i32, ptr %type_id_field\n";
  out << "  switch i32 %type_id, label %object_to_string [\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Unit)
      << ", label %boxed_unit\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Boolean)
      << ", label %boxed_boolean\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Byte)
      << ", label %boxed_byte\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Short)
      << ", label %boxed_short\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Int)
      << ", label %boxed_int\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Long)
      << ", label %boxed_long\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Float)
      << ", label %boxed_float\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Double)
      << ", label %boxed_double\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Char)
      << ", label %boxed_char\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Symbol)
      << ", label %boxed_symbol\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::String)
      << ", label %boxed_string\n";
  out << "  ]\n";
  out << "boxed_unit:\n";
  out << "  ret ptr getelementptr inbounds ([3 x i8], ptr @.str.unit, i64 0, i64 "
         "0)\n";
  out << "boxed_boolean:\n";
  out << "  %boolean_payload = getelementptr i8, ptr %object, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %boolean_value = load i1, ptr %boolean_payload\n";
  out << "  %boolean_string = call ptr @__scalanative_string_from_boolean(i1 "
         "%boolean_value)\n";
  out << "  ret ptr %boolean_string\n";
  out << "boxed_byte:\n";
  out << "  %byte_payload = getelementptr i8, ptr %object, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %byte_value = load i8, ptr %byte_payload\n";
  out << "  %wide_byte = sext i8 %byte_value to i32\n";
  out << "  %byte_string = call ptr @__scalanative_string_from_int(i32 %wide_byte)\n";
  out << "  ret ptr %byte_string\n";
  out << "boxed_short:\n";
  out << "  %short_payload = getelementptr i8, ptr %object, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %short_value = load i16, ptr %short_payload\n";
  out << "  %wide_short = sext i16 %short_value to i32\n";
  out << "  %short_string = call ptr @__scalanative_string_from_int(i32 %wide_short)\n";
  out << "  ret ptr %short_string\n";
  out << "boxed_int:\n";
  out << "  %int_payload = getelementptr i8, ptr %object, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %int_value = load i32, ptr %int_payload\n";
  out << "  %int_string = call ptr @__scalanative_string_from_int(i32 %int_value)\n";
  out << "  ret ptr %int_string\n";
  out << "boxed_long:\n";
  out << "  %long_payload = getelementptr i8, ptr %object, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %long_value = load i64, ptr %long_payload\n";
  out << "  %long_string = call ptr @__scalanative_string_from_long(i64 "
         "%long_value)\n";
  out << "  ret ptr %long_string\n";
  out << "boxed_float:\n";
  out << "  %float_payload = getelementptr i8, ptr %object, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %float_value = load float, ptr %float_payload\n";
  out << "  %float_string = call ptr @__scalanative_string_from_float(float "
         "%float_value)\n";
  out << "  ret ptr %float_string\n";
  out << "boxed_double:\n";
  out << "  %double_payload = getelementptr i8, ptr %object, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %double_value = load double, ptr %double_payload\n";
  out << "  %double_string = call ptr @__scalanative_string_from_double(double "
         "%double_value)\n";
  out << "  ret ptr %double_string\n";
  out << "boxed_char:\n";
  out << "  %char_payload = getelementptr i8, ptr %object, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %char_value = load i32, ptr %char_payload\n";
  out << "  %char_string = call ptr @__scalanative_string_from_char(i32 "
         "%char_value)\n";
  out << "  ret ptr %char_string\n";
  out << "boxed_symbol:\n";
  out << "  %symbol_payload = getelementptr i8, ptr %object, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %symbol_value = load ptr, ptr %symbol_payload\n";
  out << "  ret ptr %symbol_value\n";
  out << "boxed_string:\n";
  out << "  %string_payload = getelementptr i8, ptr %object, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %string_value = load ptr, ptr %string_payload\n";
  out << "  ret ptr %string_value\n";
  out << "object_to_string:\n";
  if (dynamicToStringSlotIndex != NoVirtualSlotIndex) {
    out << "  %vtable_field = getelementptr %scalanative.type_descriptor, ptr "
           "%descriptor, i32 0, i32 "
        << TypeDescriptorVtableIndex << "\n";
    out << "  %vtable = load ptr, ptr %vtable_field\n";
    out << "  %has_vtable = icmp ne ptr %vtable, null\n";
    out << "  br i1 %has_vtable, label %check_to_string_slot_count, label "
           "%object_type_name\n";
    out << "check_to_string_slot_count:\n";
    out << "  %slot_count_field = getelementptr %scalanative.type_descriptor, ptr "
           "%descriptor, i32 0, i32 "
        << TypeDescriptorVtableSlotCountIndex << "\n";
    out << "  %slot_count = load i32, ptr %slot_count_field\n";
    out << "  %has_to_string_slot = icmp ugt i32 %slot_count, "
        << dynamicToStringSlotIndex << "\n";
    out << "  br i1 %has_to_string_slot, label %load_to_string_slot, label "
           "%object_type_name\n";
    out << "load_to_string_slot:\n";
    out << "  %to_string_slot_pointer = getelementptr i8, ptr %vtable, i64 "
        << dynamicToStringSlotIndex * 8 << "\n";
    out << "  %to_string_function = load ptr, ptr %to_string_slot_pointer\n";
    out << "  %has_to_string_function = icmp ne ptr %to_string_function, null\n";
    out << "  br i1 %has_to_string_function, label %call_to_string, label "
           "%object_type_name\n";
    out << "call_to_string:\n";
    out << "  %custom_string = call ptr %to_string_function(ptr %object)\n";
    out << "  ret ptr %custom_string\n";
  } else {
    out << "  br label %object_type_name\n";
  }
  out << "object_type_name:\n";
  out << "  %type_name_value = call ptr "
         "@__scalanative_descriptor_type_name(ptr %descriptor)\n";
  out << "  ret ptr %type_name_value\n";
  out << "}\n\n";

  out << "define internal i1 @__scalanative_string_equals(ptr %left, ptr %right) {\n";
  out << "entry:\n";
  out << "  %identical = icmp eq ptr %left, %right\n";
  out << "  br i1 %identical, label %equal, label %check_left\n";
  out << "check_left:\n";
  out << "  %left_null = icmp eq ptr %left, null\n";
  out << "  br i1 %left_null, label %different, label %check_right\n";
  out << "check_right:\n";
  out << "  %right_null = icmp eq ptr %right, null\n";
  out << "  br i1 %right_null, label %different, label %compare\n";
  out << "compare:\n";
  out << "  %comparison = call i32 @strcmp(ptr %left, ptr %right)\n";
  out << "  %contents_equal = icmp eq i32 %comparison, 0\n";
  out << "  ret i1 %contents_equal\n";
  out << "equal:\n";
  out << "  ret i1 true\n";
  out << "different:\n";
  out << "  ret i1 false\n";
  out << "}\n\n";

  out << "define internal i1 @__scalanative_string_equals_object(ptr %left, ptr "
         "%right) {\n";
  out << "entry:\n";
  out << "  %right_null = icmp eq ptr %right, null\n";
  out << "  br i1 %right_null, label %different, label %load_descriptor\n";
  out << "load_descriptor:\n";
  out << "  %descriptor = call ptr @__scalanative_object_descriptor(ptr %right)\n";
  out << "  %is_string = icmp eq ptr %descriptor, @__scalanative_boxed_String\n";
  out << "  br i1 %is_string, label %compare, label %different\n";
  out << "compare:\n";
  out << "  %right_payload = getelementptr i8, ptr %right, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %right_string = load ptr, ptr %right_payload\n";
  out << "  %equal = call i1 @__scalanative_string_equals(ptr %left, ptr "
         "%right_string)\n";
  out << "  ret i1 %equal\n";
  out << "different:\n";
  out << "  ret i1 false\n";
  out << "}\n\n";

  out << "define internal i1 @__scalanative_any_equals(ptr %left, ptr %right) {\n";
  out << "entry:\n";
  out << "  %identical = icmp eq ptr %left, %right\n";
  out << "  br i1 %identical, label %equal, label %check_left\n";
  out << "check_left:\n";
  out << "  %left_null = icmp eq ptr %left, null\n";
  out << "  br i1 %left_null, label %different, label %check_right\n";
  out << "check_right:\n";
  out << "  %right_null = icmp eq ptr %right, null\n";
  out << "  br i1 %right_null, label %different, label %load_descriptors\n";
  out << "load_descriptors:\n";
  out << "  %left_descriptor = call ptr @__scalanative_object_descriptor(ptr %left)\n";
  out << "  %right_descriptor = call ptr @__scalanative_object_descriptor(ptr "
         "%right)\n";
  out << "  %left_kind_field = getelementptr %scalanative.type_descriptor, ptr "
         "%left_descriptor, i32 0, i32 "
      << TypeDescriptorKindIndex << "\n";
  out << "  %right_kind_field = getelementptr %scalanative.type_descriptor, ptr "
         "%right_descriptor, i32 0, i32 "
      << TypeDescriptorKindIndex << "\n";
  out << "  %left_kind = load i32, ptr %left_kind_field\n";
  out << "  %right_kind = load i32, ptr %right_kind_field\n";
  out << "  %left_boxed = icmp eq i32 %left_kind, "
      << static_cast<std::uint32_t>(runtime::RuntimeTypeKind::BoxedPrimitive) << "\n";
  out << "  %right_boxed = icmp eq i32 %right_kind, "
      << static_cast<std::uint32_t>(runtime::RuntimeTypeKind::BoxedPrimitive) << "\n";
  out << "  %both_boxed = and i1 %left_boxed, %right_boxed\n";
  out << "  br i1 %both_boxed, label %boxed_types, label %different\n";
  out << "boxed_types:\n";
  out << "  %left_type_id_field = getelementptr %scalanative.type_descriptor, ptr "
         "%left_descriptor, i32 0, i32 "
      << TypeDescriptorTypeIdIndex << "\n";
  out << "  %right_type_id_field = getelementptr %scalanative.type_descriptor, ptr "
         "%right_descriptor, i32 0, i32 "
      << TypeDescriptorTypeIdIndex << "\n";
  out << "  %left_type_id = load i32, ptr %left_type_id_field\n";
  out << "  %right_type_id = load i32, ptr %right_type_id_field\n";
  out << "  %same_boxed_type = icmp eq i32 %left_type_id, %right_type_id\n";
  out << "  br i1 %same_boxed_type, label %boxed_payload, label %different\n";
  out << "boxed_payload:\n";
  out << "  switch i32 %left_type_id, label %different [\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Unit)
      << ", label %equal\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Boolean)
      << ", label %boxed_boolean\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Byte)
      << ", label %boxed_byte\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Short)
      << ", label %boxed_short\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Int)
      << ", label %boxed_int\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Long)
      << ", label %boxed_long\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Float)
      << ", label %boxed_float\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Double)
      << ", label %boxed_double\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Char)
      << ", label %boxed_char\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::Symbol)
      << ", label %boxed_symbol\n";
  out << "    i32 " << static_cast<std::uint32_t>(runtime::BoxedPrimitiveKind::String)
      << ", label %boxed_string\n";
  out << "  ]\n";
  out << "boxed_boolean:\n";
  out << "  %left_boolean_payload = getelementptr i8, ptr %left, i64 "
      << ObjectHeaderSize << "\n";
  out << "  %right_boolean_payload = getelementptr i8, ptr %right, i64 "
      << ObjectHeaderSize << "\n";
  out << "  %left_boolean = load i1, ptr %left_boolean_payload\n";
  out << "  %right_boolean = load i1, ptr %right_boolean_payload\n";
  out << "  %boolean_equal = icmp eq i1 %left_boolean, %right_boolean\n";
  out << "  ret i1 %boolean_equal\n";
  out << "boxed_byte:\n";
  out << "  %left_byte_payload = getelementptr i8, ptr %left, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %right_byte_payload = getelementptr i8, ptr %right, i64 "
      << ObjectHeaderSize << "\n";
  out << "  %left_byte = load i8, ptr %left_byte_payload\n";
  out << "  %right_byte = load i8, ptr %right_byte_payload\n";
  out << "  %byte_equal = icmp eq i8 %left_byte, %right_byte\n";
  out << "  ret i1 %byte_equal\n";
  out << "boxed_short:\n";
  out << "  %left_short_payload = getelementptr i8, ptr %left, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %right_short_payload = getelementptr i8, ptr %right, i64 "
      << ObjectHeaderSize << "\n";
  out << "  %left_short = load i16, ptr %left_short_payload\n";
  out << "  %right_short = load i16, ptr %right_short_payload\n";
  out << "  %short_equal = icmp eq i16 %left_short, %right_short\n";
  out << "  ret i1 %short_equal\n";
  out << "boxed_int:\n";
  out << "  %left_int_payload = getelementptr i8, ptr %left, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %right_int_payload = getelementptr i8, ptr %right, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %left_int = load i32, ptr %left_int_payload\n";
  out << "  %right_int = load i32, ptr %right_int_payload\n";
  out << "  %int_equal = icmp eq i32 %left_int, %right_int\n";
  out << "  ret i1 %int_equal\n";
  out << "boxed_long:\n";
  out << "  %left_long_payload = getelementptr i8, ptr %left, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %right_long_payload = getelementptr i8, ptr %right, i64 "
      << ObjectHeaderSize << "\n";
  out << "  %left_long = load i64, ptr %left_long_payload\n";
  out << "  %right_long = load i64, ptr %right_long_payload\n";
  out << "  %long_equal = icmp eq i64 %left_long, %right_long\n";
  out << "  ret i1 %long_equal\n";
  out << "boxed_float:\n";
  out << "  %left_float_payload = getelementptr i8, ptr %left, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %right_float_payload = getelementptr i8, ptr %right, i64 "
      << ObjectHeaderSize << "\n";
  out << "  %left_float = load float, ptr %left_float_payload\n";
  out << "  %right_float = load float, ptr %right_float_payload\n";
  out << "  %float_equal = fcmp oeq float %left_float, %right_float\n";
  out << "  ret i1 %float_equal\n";
  out << "boxed_double:\n";
  out << "  %left_double_payload = getelementptr i8, ptr %left, i64 "
      << ObjectHeaderSize << "\n";
  out << "  %right_double_payload = getelementptr i8, ptr %right, i64 "
      << ObjectHeaderSize << "\n";
  out << "  %left_double = load double, ptr %left_double_payload\n";
  out << "  %right_double = load double, ptr %right_double_payload\n";
  out << "  %double_equal = fcmp oeq double %left_double, %right_double\n";
  out << "  ret i1 %double_equal\n";
  out << "boxed_char:\n";
  out << "  %left_char_payload = getelementptr i8, ptr %left, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %right_char_payload = getelementptr i8, ptr %right, i64 "
      << ObjectHeaderSize << "\n";
  out << "  %left_char = load i32, ptr %left_char_payload\n";
  out << "  %right_char = load i32, ptr %right_char_payload\n";
  out << "  %char_equal = icmp eq i32 %left_char, %right_char\n";
  out << "  ret i1 %char_equal\n";
  out << "boxed_symbol:\n";
  out << "  %left_symbol_payload = getelementptr i8, ptr %left, i64 "
      << ObjectHeaderSize << "\n";
  out << "  %right_symbol_payload = getelementptr i8, ptr %right, i64 "
      << ObjectHeaderSize << "\n";
  out << "  %left_symbol = load ptr, ptr %left_symbol_payload\n";
  out << "  %right_symbol = load ptr, ptr %right_symbol_payload\n";
  out << "  %symbol_equal = call i1 @__scalanative_string_equals(ptr %left_symbol, "
         "ptr %right_symbol)\n";
  out << "  ret i1 %symbol_equal\n";
  out << "boxed_string:\n";
  out << "  %left_string_payload = getelementptr i8, ptr %left, i64 "
      << ObjectHeaderSize << "\n";
  out << "  %right_string_payload = getelementptr i8, ptr %right, i64 "
      << ObjectHeaderSize << "\n";
  out << "  %left_string = load ptr, ptr %left_string_payload\n";
  out << "  %right_string = load ptr, ptr %right_string_payload\n";
  out << "  %string_equal = call i1 @__scalanative_string_equals(ptr %left_string, "
         "ptr %right_string)\n";
  out << "  ret i1 %string_equal\n";
  out << "equal:\n";
  out << "  ret i1 true\n";
  out << "different:\n";
  out << "  ret i1 false\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_zone_enter() {\n";
  out << "entry:\n";
  out << "  %previous = load ptr, ptr @__scalanative_current_zone\n";
  out << "  %zone = call ptr @__scalanative_arena_create(i64 65536)\n";
  out << "  %previous_field = getelementptr %scalanative.arena, ptr %zone, i32 0, "
         "i32 2\n";
  out << "  store ptr %previous, ptr %previous_field\n";
  out << "  store ptr %zone, ptr @__scalanative_current_zone\n";
  out << "  ret ptr %previous\n";
  out << "}\n\n";

  out << "define internal void @__scalanative_zone_exit(ptr %previous) {\n";
  out << "entry:\n";
  out << "  %zone = load ptr, ptr @__scalanative_current_zone\n";
  out << "  call void @__scalanative_arena_destroy(ptr %zone)\n";
  out << "  store ptr %previous, ptr @__scalanative_current_zone\n";
  out << "  ret void\n";
  out << "}\n\n";

  out << "define internal void @__scalanative_zone_destroy_all() {\n";
  out << "entry:\n";
  out << "  %head = load ptr, ptr @__scalanative_current_zone\n";
  out << "  br label %loop\n";
  out << "loop:\n";
  out << "  %zone = phi ptr [ %head, %entry ], [ %previous, %release ]\n";
  out << "  %exists = icmp ne ptr %zone, null\n";
  out << "  br i1 %exists, label %release, label %done\n";
  out << "release:\n";
  out << "  %previous_field = getelementptr %scalanative.arena, ptr %zone, i32 0, "
         "i32 2\n";
  out << "  %previous = load ptr, ptr %previous_field\n";
  out << "  call void @__scalanative_arena_destroy(ptr %zone)\n";
  out << "  br label %loop\n";
  out << "done:\n";
  out << "  store ptr null, ptr @__scalanative_current_zone\n";
  out << "  ret void\n";
  out << "}\n\n";

  out << "define internal void @__scalanative_zone_unwind_to(ptr %target) {\n";
  out << "entry:\n";
  out << "  %head = load ptr, ptr @__scalanative_current_zone\n";
  out << "  br label %loop\n";
  out << "loop:\n";
  out << "  %zone = phi ptr [ %head, %entry ], [ %previous, %release ]\n";
  out << "  %at_target = icmp eq ptr %zone, %target\n";
  out << "  %has_zone = icmp ne ptr %zone, null\n";
  out << "  %not_target = xor i1 %at_target, true\n";
  out << "  %release_zone = and i1 %has_zone, %not_target\n";
  out << "  br i1 %release_zone, label %release, label %done\n";
  out << "release:\n";
  out << "  %previous_field = getelementptr %scalanative.arena, ptr %zone, i32 0, "
         "i32 2\n";
  out << "  %previous = load ptr, ptr %previous_field\n";
  out << "  call void @__scalanative_arena_destroy(ptr %zone)\n";
  out << "  br label %loop\n";
  out << "done:\n";
  out << "  store ptr %zone, ptr @__scalanative_current_zone\n";
  out << "  ret void\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_gc_object_alloc(i64 %size, ptr "
         "%descriptor) {\n";
  out << "entry:\n";
  out << "  call void @__scalanative_gc_poll()\n";
  out << "  %object = call ptr @__scalanative_alloc(i64 %size, ptr %descriptor, "
         "i32 "
      << runtime::objectOwnershipTag(runtime::ObjectOwnership::Gc) << ")\n";
  out << "  ret ptr %object\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_object_alloc(i64 %size, ptr "
         "%descriptor) {\n";
  out << "entry:\n";
  out << "  %zone = load ptr, ptr @__scalanative_current_zone\n";
  out << "  %scoped = icmp ne ptr %zone, null\n";
  out << "  br i1 %scoped, label %arena, label %gc\n";
  out << "arena:\n";
  out << "  %arena_object = call ptr @__scalanative_arena_alloc(ptr %zone, i64 "
         "%size, ptr %descriptor)\n";
  out << "  ret ptr %arena_object\n";
  out << "gc:\n";
  out << "  %gc_object = call ptr @__scalanative_gc_object_alloc(i64 %size, ptr "
         "%descriptor)\n";
  out << "  ret ptr %gc_object\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_box_alloc(i64 %size, ptr %descriptor) "
         "{\n";
  out << "entry:\n";
  out << "  %zone = load ptr, ptr @__scalanative_current_zone\n";
  out << "  %scoped = icmp ne ptr %zone, null\n";
  out << "  br i1 %scoped, label %arena, label %program\n";
  out << "arena:\n";
  out << "  %arena_object = call ptr @__scalanative_arena_alloc(ptr %zone, i64 "
         "%size, ptr %descriptor)\n";
  out << "  ret ptr %arena_object\n";
  out << "program:\n";
  out << "  %program_object = call ptr @__scalanative_program_arena_alloc(i64 "
         "%size, ptr %descriptor)\n";
  out << "  ret ptr %program_object\n";
  out << "}\n\n";

  out << "define internal i32 @__scalanative_trace_count(ptr %object) {\n";
  out << "entry:\n";
  out << "  %descriptor = call ptr @__scalanative_object_descriptor(ptr %object)\n";
  out << "  %count_field = getelementptr %scalanative.type_descriptor, ptr "
         "%descriptor, i32 0, i32 "
      << TypeDescriptorTraceCountIndex << "\n";
  out << "  %count = load i32, ptr %count_field\n";
  out << "  ret i32 %count\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_trace_reference(ptr %object, i32 "
         "%index) {\n";
  out << "entry:\n";
  out << "  %descriptor = call ptr @__scalanative_object_descriptor(ptr %object)\n";
  out << "  %offsets_field = getelementptr %scalanative.type_descriptor, ptr "
         "%descriptor, i32 0, i32 "
      << TypeDescriptorTraceOffsetsIndex << "\n";
  out << "  %offsets = load ptr, ptr %offsets_field\n";
  out << "  %offset_slot = getelementptr i32, ptr %offsets, i32 %index\n";
  out << "  %offset = load i32, ptr %offset_slot\n";
  out << "  %wide_offset = zext i32 %offset to i64\n";
  out << "  %reference_field = getelementptr i8, ptr %object, i64 %wide_offset\n";
  out << "  %reference = load ptr, ptr %reference_field\n";
  out << "  ret ptr %reference\n";
  out << "}\n\n";

  out << "define internal i32 @__scalanative_default_ownership(ptr %descriptor) "
         "{\n";
  out << "entry:\n";
  out << "  %ownership_field = getelementptr %scalanative.type_descriptor, ptr "
         "%descriptor, i32 0, i32 "
      << TypeDescriptorDefaultOwnershipIndex << "\n";
  out << "  %ownership = load i32, ptr %ownership_field\n";
  out << "  ret i32 %ownership\n";
  out << "}\n\n";

  out << "define internal i1 @__scalanative_is_instance_of(ptr %object, ptr %target) "
         "{\n";
  out << "entry:\n";
  out << "  %has_object = icmp ne ptr %object, null\n";
  out << "  %has_target = icmp ne ptr %target, null\n";
  out << "  %valid = and i1 %has_object, %has_target\n";
  out << "  br i1 %valid, label %load_descriptor, label %fail\n";
  out << "load_descriptor:\n";
  out << "  %descriptor = call ptr @__scalanative_object_descriptor(ptr %object)\n";
  out << "  %exact = icmp eq ptr %descriptor, %target\n";
  out << "  br i1 %exact, label %success, label %prepare\n";
  out << "prepare:\n";
  out << "  %ancestors_field = getelementptr %scalanative.type_descriptor, ptr "
         "%descriptor, i32 0, i32 "
      << TypeDescriptorAncestorsIndex << "\n";
  out << "  %ancestors = load ptr, ptr %ancestors_field\n";
  out << "  %count_field = getelementptr %scalanative.type_descriptor, ptr "
         "%descriptor, i32 0, i32 "
      << TypeDescriptorAncestorCountIndex << "\n";
  out << "  %count = load i32, ptr %count_field\n";
  out << "  br label %loop\n";
  out << "loop:\n";
  out << "  %index = phi i32 [ 0, %prepare ], [ %next, %advance ]\n";
  out << "  %done = icmp uge i32 %index, %count\n";
  out << "  br i1 %done, label %fail, label %check\n";
  out << "check:\n";
  out << "  %slot = getelementptr ptr, ptr %ancestors, i32 %index\n";
  out << "  %ancestor = load ptr, ptr %slot\n";
  out << "  %matches = icmp eq ptr %ancestor, %target\n";
  out << "  br i1 %matches, label %success, label %advance\n";
  out << "advance:\n";
  out << "  %next = add i32 %index, 1\n";
  out << "  br label %loop\n";
  out << "success:\n";
  out << "  ret i1 true\n";
  out << "fail:\n";
  out << "  ret i1 false\n";
  out << "}\n\n";

  out << "define internal ptr @__scalanative_as_instance_of(ptr %object, ptr "
         "%target) {\n";
  out << "entry:\n";
  out << "  %is_null = icmp eq ptr %object, null\n";
  out << "  br i1 %is_null, label %success, label %check\n";
  out << "check:\n";
  out << "  %matches = call i1 @__scalanative_is_instance_of(ptr %object, ptr "
         "%target)\n";
  out << "  br i1 %matches, label %success, label %fail\n";
  out << "fail:\n";
  out << "  call void @__scalanative_throw_class_cast()\n";
  out << "  unreachable\n";
  out << "success:\n";
  out << "  ret ptr %object\n";
  out << "}\n\n";
}

void emitBoxedPrimitiveRuntime(std::ostringstream& out) {
  for (std::string_view type : BoxedPrimitiveTypes) {
    const runtime::BoxedPrimitiveKind kind = runtime::boxedPrimitiveKind(type);
    const runtime::RuntimeTypeLayout layout = runtime::boxedPrimitiveTypeLayout(kind);
    out << '@' << boxedTypeNameConstantName(type)
        << " = private unnamed_addr constant [" << (type.size() + 1) << " x i8] c\""
        << llvmCString(type) << "\\00\"\n";
    out << '@' << boxedPrimitiveDescriptorName(type)
        << " = private constant %scalanative.type_descriptor { i32 "
        << static_cast<std::uint32_t>(layout.kind) << ", i32 " << layout.typeId
        << ", i64 " << layout.instanceSize << ", i32 " << layout.instanceAlignment
        << ", i32 " << layout.payloadOffset << ", i32 " << layout.payloadSize
        << ", i32 " << layout.payloadAlignment << ", ptr @"
        << boxedTypeNameConstantName(type)
        << ", ptr null, i32 0, ptr null, i32 0, ptr null, i32 0, i32 "
        << runtime::objectOwnershipTag(runtime::ObjectOwnership::Arena)
        << " }, align 8\n";
  }
  out << '\n';

  for (std::string_view type : BoxedPrimitiveTypes) {
    const std::string simpleType(type);
    out << "define internal " << llvmType(simpleType) << " @"
        << boxedPrimitiveUnboxName(type) << "(ptr %object) {\n";
    out << "entry:\n";
    out << "  %matches = call i1 @__scalanative_is_instance_of(ptr %object, ptr @"
        << boxedPrimitiveDescriptorName(type) << ")\n";
    out << "  br i1 %matches, label %ok, label %fail\n";
    out << "fail:\n";
    out << "  call void @__scalanative_throw_class_cast()\n";
    out << "  unreachable\n";
    out << "ok:\n";
    if (simpleType == "Unit") {
      out << "  ret void\n";
      out << "}\n\n";
      continue;
    }
    out << "  %payload = getelementptr i8, ptr %object, i64 " << ObjectHeaderSize
        << "\n";
    out << "  %value = load " << llvmType(simpleType) << ", ptr %payload\n";
    out << "  ret " << llvmType(simpleType) << " %value\n";
    out << "}\n\n";
  }
}

std::vector<std::string> runtimeAncestorNames(
    const std::string& name,
    const std::unordered_map<std::string, std::vector<std::string>>& classParents,
    const std::unordered_map<std::string, ClassLayout>& classLayouts) {
  std::vector<std::string> ancestors = nir::linearizedTypeNames(name, classParents);
  if (!ancestors.empty()) {
    ancestors.erase(ancestors.begin());
  }
  ancestors.erase(std::remove_if(ancestors.begin(), ancestors.end(),
                                 [&](const std::string& ancestor) {
                                   return !classLayouts.contains(ancestor);
                                 }),
                  ancestors.end());
  return ancestors;
}

void emitClassTypeDescriptors(
    const std::unordered_map<std::string, ClassLayout>& classLayouts,
    const std::unordered_map<std::string, std::vector<std::string>>& classParents,
    std::ostringstream& out) {
  std::vector<std::string> typeNames;
  for (const auto& [className, layout] : classLayouts) {
    (void)layout;
    typeNames.push_back(className);
  }
  std::sort(typeNames.begin(), typeNames.end());

  for (const std::string& name : typeNames) {
    const std::string typeName = classTypeNameConstantName(name);
    out << '@' << typeName << " = private unnamed_addr constant [" << (name.size() + 1)
        << " x i8] c\"" << llvmCString(name) << "\\00\"\n";
  }
  if (!typeNames.empty()) {
    out << '\n';
  }

  for (const std::string& name : typeNames) {
    const ClassLayout& layout = classLayouts.at(name);
    if (layout.isTrait || layout.traceOffsets.empty()) {
      continue;
    }
    out << '@' << classTraceOffsetsName(name) << " = private constant ["
        << layout.traceOffsets.size() << " x i32] [";
    for (std::size_t i = 0; i < layout.traceOffsets.size(); ++i) {
      if (i != 0) {
        out << ", ";
      }
      out << "i32 " << layout.traceOffsets[i];
    }
    out << "]\n";
  }
  if (!typeNames.empty()) {
    out << '\n';
  }

  for (const std::string& name : typeNames) {
    const std::vector<std::string> ancestors =
        runtimeAncestorNames(name, classParents, classLayouts);
    if (ancestors.empty()) {
      continue;
    }
    out << "@__ancestors_" << sanitizeIdentifier(name) << " = private constant ["
        << ancestors.size() << " x ptr] [";
    for (std::size_t i = 0; i < ancestors.size(); ++i) {
      if (i != 0) {
        out << ", ";
      }
      out << "ptr @" << classLayouts.at(ancestors[i]).descriptorName;
    }
    out << "]\n";
  }
  if (!typeNames.empty()) {
    out << '\n';
  }

  for (const std::string& name : typeNames) {
    const ClassLayout& layout = classLayouts.at(name);
    const std::vector<std::string> ancestors =
        runtimeAncestorNames(name, classParents, classLayouts);
    const std::string typeName = classTypeNameConstantName(name);
    out << '@' << layout.descriptorName
        << " = private constant %scalanative.type_descriptor { i32 "
        << static_cast<std::uint32_t>(layout.isTrait ? runtime::RuntimeTypeKind::Trait
                                      : layout.isModule
                                          ? runtime::RuntimeTypeKind::Module
                                          : runtime::RuntimeTypeKind::Class)
        << ", i32 " << layout.typeId << ", i64 " << (layout.isTrait ? 0 : layout.size)
        << ", i32 " << (layout.isTrait ? 0 : 8) << ", i32 "
        << (layout.isTrait ? 0 : ObjectHeaderSize) << ", i32 "
        << (layout.isTrait ? 0 : layout.size - ObjectHeaderSize) << ", i32 "
        << (layout.isTrait ? 0 : 8) << ", ptr @" << typeName << ", ptr ";
    if (layout.isTrait || layout.vtableName.empty()) {
      out << "null";
    } else {
      out << '@' << layout.vtableName;
    }
    out << ", i32 "
        << (layout.isTrait || layout.vtableName.empty() ? 0
                                                        : layout.virtualSlots.size())
        << ", ptr ";
    if (ancestors.empty()) {
      out << "null";
    } else {
      out << "@__ancestors_" << sanitizeIdentifier(name);
    }
    out << ", i32 " << ancestors.size() << ", ptr ";
    if (layout.isTrait || layout.traceOffsets.empty()) {
      out << "null";
    } else {
      out << '@' << classTraceOffsetsName(name);
    }
    out << ", i32 " << (layout.isTrait ? 0 : layout.traceOffsets.size()) << ", i32 "
        << runtime::objectOwnershipTag(layout.isModule
                                           ? runtime::ObjectOwnership::Immortal
                                           : runtime::ObjectOwnership::Gc)
        << " }, align 8\n";
  }
  if (!typeNames.empty()) {
    out << '\n';
  }
}

void emitModuleSingletons(
    const std::unordered_map<std::string, ClassLayout>& classLayouts,
    const std::unordered_map<std::string, Signature>& functionSignatures,
    std::ostringstream& out) {
  std::vector<std::string> moduleNames;
  for (const auto& [name, layout] : classLayouts) {
    if (layout.isModule) {
      moduleNames.push_back(name);
    }
  }
  std::sort(moduleNames.begin(), moduleNames.end());
  if (moduleNames.empty()) {
    return;
  }

  for (const std::string& name : moduleNames) {
    out << '@' << moduleInstanceName(name) << " = private global ptr null\n";
  }
  out << "@__scalanative_module_roots = private constant [" << moduleNames.size()
      << " x ptr] [";
  for (std::size_t i = 0; i < moduleNames.size(); ++i) {
    if (i != 0) {
      out << ", ";
    }
    out << "ptr @" << moduleInstanceName(moduleNames[i]);
  }
  out << "]\n";
  out << "@__scalanative_module_root_count = private constant i32 "
      << moduleNames.size() << "\n\n";

  out << "define internal ptr @__scalanative_module_root(i32 %index) {\n";
  out << "entry:\n";
  out << "  %slot = getelementptr [" << moduleNames.size()
      << " x ptr], ptr @__scalanative_module_roots, i32 0, i32 %index\n";
  out << "  %root_slot = load ptr, ptr %slot\n";
  out << "  %root = load ptr, ptr %root_slot\n";
  out << "  ret ptr %root\n";
  out << "}\n\n";

  for (const std::string& name : moduleNames) {
    const ClassLayout& layout = classLayouts.at(name);
    out << "define internal ptr @" << moduleAccessorName(name) << "() {\n";
    out << "entry:\n";
    out << "  %current = load ptr, ptr @" << moduleInstanceName(name) << "\n";
    out << "  %initialized = icmp ne ptr %current, null\n";
    out << "  br i1 %initialized, label %ready, label %initialize\n";
    out << "initialize:\n";
    out << "  %created = call ptr @__scalanative_alloc(i64 " << layout.size << ", ptr @"
        << layout.descriptorName << ", i32 "
        << runtime::objectOwnershipTag(runtime::ObjectOwnership::Immortal) << ")\n";
    out << "  store ptr %created, ptr @" << moduleInstanceName(name) << "\n";
    const std::string initializerName =
        name + "." + std::string(support::StdNames::Constructor);
    if (functionSignatures.contains(initializerName)) {
      out << "  call void @" << sanitizeIdentifier(initializerName) << "()\n";
    }
    out << "  br label %ready\n";
    out << "ready:\n";
    out << "  %instance = phi ptr [ %current, %entry ], [ %created, %initialize ]\n";
    out << "  ret ptr %instance\n";
    out << "}\n\n";
  }
}

void emitGcCollector(const std::unordered_map<std::string, ClassLayout>& classLayouts,
                     std::ostringstream& out) {
  const std::size_t moduleCount =
      std::count_if(classLayouts.begin(), classLayouts.end(),
                    [](const auto& entry) { return entry.second.isModule; });

  out << "define internal void @__scalanative_gc_mark_object(ptr %object) {\n";
  out << "entry:\n";
  out << "  %is_null = icmp eq ptr %object, null\n";
  out << "  br i1 %is_null, label %done, label %check_ownership\n";
  out << "check_ownership:\n";
  out << "  %ownership = call i32 @__scalanative_object_ownership(ptr %object)\n";
  out << "  %is_gc = icmp eq i32 %ownership, "
      << runtime::objectOwnershipTag(runtime::ObjectOwnership::Gc) << "\n";
  out << "  br i1 %is_gc, label %prepare, label %done\n";
  out << "prepare:\n";
  out << "  %head = load ptr, ptr @__scalanative_gc_head\n";
  out << "  br label %find\n";
  out << "find:\n";
  out << "  %node = phi ptr [ %head, %prepare ], [ %next, %advance ]\n";
  out << "  %at_end = icmp eq ptr %node, null\n";
  out << "  br i1 %at_end, label %done, label %compare\n";
  out << "compare:\n";
  out << "  %object_field = getelementptr %scalanative.gc_node, ptr %node, i32 0, "
         "i32 0\n";
  out << "  %candidate = load ptr, ptr %object_field\n";
  out << "  %matches = icmp eq ptr %candidate, %object\n";
  out << "  br i1 %matches, label %found, label %advance\n";
  out << "advance:\n";
  out << "  %next_field = getelementptr %scalanative.gc_node, ptr %node, i32 0, i32 "
         "2\n";
  out << "  %next = load ptr, ptr %next_field\n";
  out << "  br label %find\n";
  out << "found:\n";
  out << "  %marked_field = getelementptr %scalanative.gc_node, ptr %node, i32 0, "
         "i32 1\n";
  out << "  %marked = load i1, ptr %marked_field\n";
  out << "  br i1 %marked, label %done, label %mark\n";
  out << "mark:\n";
  out << "  store i1 true, ptr %marked_field\n";
  out << "  call void @__scalanative_gc_mark_children(ptr %object)\n";
  out << "  br label %done\n";
  out << "done:\n";
  out << "  ret void\n";
  out << "}\n\n";

  out << "define internal void @__scalanative_gc_mark_children(ptr %object) {\n";
  out << "entry:\n";
  out << "  %count = call i32 @__scalanative_trace_count(ptr %object)\n";
  out << "  br label %loop\n";
  out << "loop:\n";
  out << "  %index = phi i32 [ 0, %entry ], [ %next, %mark ]\n";
  out << "  %done = icmp uge i32 %index, %count\n";
  out << "  br i1 %done, label %return, label %mark\n";
  out << "mark:\n";
  out << "  %child = call ptr @__scalanative_trace_reference(ptr %object, i32 "
         "%index)\n";
  out << "  call void @__scalanative_gc_mark_object(ptr %child)\n";
  out << "  %next = add i32 %index, 1\n";
  out << "  br label %loop\n";
  out << "return:\n";
  out << "  ret void\n";
  out << "}\n\n";

  out << "define internal void @__scalanative_gc_mark_shadow_roots() {\n";
  out << "entry:\n";
  out << "  %head = load ptr, ptr @__scalanative_shadow_stack\n";
  out << "  br label %frames\n";
  out << "frames:\n";
  out << "  %frame = phi ptr [ %head, %entry ], [ %previous, %next_frame ]\n";
  out << "  %frames_done = icmp eq ptr %frame, null\n";
  out << "  br i1 %frames_done, label %return, label %prepare_roots\n";
  out << "prepare_roots:\n";
  out << "  %count_field = getelementptr %scalanative.shadow_frame, ptr %frame, i32 "
         "0, i32 1\n";
  out << "  %count = load i32, ptr %count_field\n";
  out << "  %roots_field = getelementptr %scalanative.shadow_frame, ptr %frame, i32 "
         "0, i32 2\n";
  out << "  %roots = load ptr, ptr %roots_field\n";
  out << "  br label %roots_loop\n";
  out << "roots_loop:\n";
  out << "  %index = phi i32 [ 0, %prepare_roots ], [ %next_index, %mark_root ]\n";
  out << "  %roots_done = icmp uge i32 %index, %count\n";
  out << "  br i1 %roots_done, label %next_frame, label %mark_root\n";
  out << "mark_root:\n";
  out << "  %slot = getelementptr ptr, ptr %roots, i32 %index\n";
  out << "  %root = load ptr, ptr %slot\n";
  out << "  call void @__scalanative_gc_mark_object(ptr %root)\n";
  out << "  %next_index = add i32 %index, 1\n";
  out << "  br label %roots_loop\n";
  out << "next_frame:\n";
  out << "  %previous_field = getelementptr %scalanative.shadow_frame, ptr %frame, "
         "i32 0, i32 0\n";
  out << "  %previous = load ptr, ptr %previous_field\n";
  out << "  br label %frames\n";
  out << "return:\n";
  out << "  ret void\n";
  out << "}\n\n";

  out << "define internal void @__scalanative_gc_collect() {\n";
  out << "entry:\n";
  out << "  call void @__scalanative_gc_mark_shadow_roots()\n";
  out << "  %current_exception = load ptr, ptr @__scalanative_current_exception\n";
  out << "  call void @__scalanative_gc_mark_object(ptr %current_exception)\n";
  if (moduleCount == 0) {
    out << "  br label %sweep_prepare\n";
  } else {
    out << "  br label %roots\n";
    out << "roots:\n";
    out << "  %root_index = phi i32 [ 0, %entry ], [ %next_root, %root_advance ]\n";
    out << "  %roots_done = icmp uge i32 %root_index, " << moduleCount << "\n";
    out << "  br i1 %roots_done, label %sweep_prepare, label %mark_root\n";
    out << "mark_root:\n";
    out << "  %root = call ptr @__scalanative_module_root(i32 %root_index)\n";
    out << "  %has_root = icmp ne ptr %root, null\n";
    out << "  br i1 %has_root, label %mark_root_children, label %root_advance\n";
    out << "mark_root_children:\n";
    out << "  call void @__scalanative_gc_mark_children(ptr %root)\n";
    out << "  br label %root_advance\n";
    out << "root_advance:\n";
    out << "  %next_root = add i32 %root_index, 1\n";
    out << "  br label %roots\n";
  }
  out << "sweep_prepare:\n";
  out << "  br label %sweep\n";
  out << "sweep:\n";
  out << "  %link = phi ptr [ @__scalanative_gc_head, %sweep_prepare ], [ "
         "%next_field, %keep ], [ %link, %release ]\n";
  out << "  %node = load ptr, ptr %link\n";
  out << "  %sweep_done = icmp eq ptr %node, null\n";
  out << "  br i1 %sweep_done, label %finish, label %inspect\n";
  out << "inspect:\n";
  out << "  %marked_field = getelementptr %scalanative.gc_node, ptr %node, i32 0, "
         "i32 1\n";
  out << "  %marked = load i1, ptr %marked_field\n";
  out << "  %next_field = getelementptr %scalanative.gc_node, ptr %node, i32 0, i32 "
         "2\n";
  out << "  %next = load ptr, ptr %next_field\n";
  out << "  br i1 %marked, label %keep, label %release\n";
  out << "keep:\n";
  out << "  store i1 false, ptr %marked_field\n";
  out << "  br label %sweep\n";
  out << "release:\n";
  out << "  %object_field = getelementptr %scalanative.gc_node, ptr %node, i32 0, "
         "i32 0\n";
  out << "  %object = load ptr, ptr %object_field\n";
  out << "  store ptr %next, ptr %link\n";
  out << "  call void @__scalanative_release_exception_trace(ptr %object)\n";
  out << "  call void @free(ptr %object)\n";
  out << "  call void @free(ptr %node)\n";
  out << "  %count = load i64, ptr @__scalanative_gc_allocation_count\n";
  out << "  %next_count = sub i64 %count, 1\n";
  out << "  store i64 %next_count, ptr @__scalanative_gc_allocation_count\n";
  out << "  br label %sweep\n";
  out << "finish:\n";
  out << "  %collections = load i64, ptr @__scalanative_gc_collection_count\n";
  out << "  %next_collections = add i64 %collections, 1\n";
  out << "  store i64 %next_collections, ptr @__scalanative_gc_collection_count\n";
  out << "  ret void\n";
  out << "}\n\n";

  out << "define internal void @__scalanative_gc_set_threshold(i64 %threshold) {\n";
  out << "entry:\n";
  out << "  %is_positive = icmp ugt i64 %threshold, 0\n";
  out << "  %effective = select i1 %is_positive, i64 %threshold, i64 1\n";
  out << "  store i64 %effective, ptr @__scalanative_gc_collection_threshold\n";
  out << "  ret void\n";
  out << "}\n\n";

  out << "define internal void @__scalanative_gc_poll() {\n";
  out << "entry:\n";
  out << "  %count = load i64, ptr @__scalanative_gc_allocation_count\n";
  out << "  %threshold = load i64, ptr @__scalanative_gc_collection_threshold\n";
  out << "  %due = icmp uge i64 %count, %threshold\n";
  out << "  br i1 %due, label %collect, label %done\n";
  out << "collect:\n";
  out << "  call void @__scalanative_gc_collect()\n";
  out << "  br label %done\n";
  out << "done:\n";
  out << "  ret void\n";
  out << "}\n\n";
}

void emitRuntimeResources(std::ostringstream& out) {
  out << runtime_resources::Lifecycle;
  out << runtime_resources::Exceptions;
}

void emitExceptionRuntimeHelpers(
    const std::unordered_map<std::string, ClassLayout>& classLayouts,
    const std::unordered_map<std::string, FieldInfo>& fields, std::ostringstream& out) {
  const std::string throwableName(support::StdNames::JavaLangThrowable);
  const std::string messageFieldName =
      throwableName + "." + std::string(support::StdNames::ThrowableMessage) + "$field";
  const std::string causeFieldName =
      throwableName + "." + std::string(support::StdNames::ThrowableCause);
  const std::string traceFieldName =
      throwableName + "." + std::string(support::StdNames::ThrowableTrace);
  const std::string suppressedNodeName(
      support::StdNames::RuntimeSuppressedExceptionNode);
  const std::string suppressedHeadFieldName =
      throwableName + "." + std::string(support::StdNames::ThrowableSuppressedHead);
  const std::string suppressedCountFieldName =
      throwableName + "." + std::string(support::StdNames::ThrowableSuppressedCount);
  const std::string suppressedValueFieldName =
      suppressedNodeName + "." +
      std::string(support::StdNames::SuppressedExceptionValue);
  const std::string suppressedNextFieldName =
      suppressedNodeName + "." +
      std::string(support::StdNames::SuppressedExceptionNext);
  const std::string stackTraceElementName(support::StdNames::JavaLangStackTraceElement);
  const std::string stackFunctionFieldName =
      stackTraceElementName + "." +
      std::string(support::StdNames::StackTraceFunctionName) + "$field";
  const std::string stackFileFieldName =
      stackTraceElementName + "." + std::string(support::StdNames::StackTraceFileName) +
      "$field";
  const std::string stackLineFieldName =
      stackTraceElementName + "." +
      std::string(support::StdNames::StackTraceLineNumber) + "$field";
  const std::string stackColumnFieldName =
      stackTraceElementName + "." +
      std::string(support::StdNames::StackTraceColumnNumber) + "$field";
  const auto throwable = classLayouts.find(throwableName);
  const auto assertionError =
      classLayouts.find(std::string(support::StdNames::JavaLangAssertionError));
  const auto illegalArgument = classLayouts.find(
      std::string(support::StdNames::JavaLangIllegalArgumentException));
  const auto nullPointer =
      classLayouts.find(std::string(support::StdNames::JavaLangNullPointerException));
  const auto arithmetic =
      classLayouts.find(std::string(support::StdNames::JavaLangArithmeticException));
  const auto classCast =
      classLayouts.find(std::string(support::StdNames::JavaLangClassCastException));
  const auto arrayStore =
      classLayouts.find(std::string(support::StdNames::JavaLangArrayStoreException));
  const auto arrayIndexOutOfBounds = classLayouts.find(
      std::string(support::StdNames::JavaLangArrayIndexOutOfBoundsException));
  const auto negativeArraySize = classLayouts.find(
      std::string(support::StdNames::JavaLangNegativeArraySizeException));
  const auto bufferUnderflow = classLayouts.find(
      std::string(support::StdNames::JavaNioBufferUnderflowException));
  const auto bufferOverflow =
      classLayouts.find(std::string(support::StdNames::JavaNioBufferOverflowException));
  const auto message = fields.find(messageFieldName);
  const auto cause = fields.find(causeFieldName);
  const auto trace = fields.find(traceFieldName);
  const auto suppressedNode = classLayouts.find(suppressedNodeName);
  const auto suppressedHead = fields.find(suppressedHeadFieldName);
  const auto suppressedCount = fields.find(suppressedCountFieldName);
  const auto suppressedValue = fields.find(suppressedValueFieldName);
  const auto suppressedNext = fields.find(suppressedNextFieldName);
  const auto stackTraceElement = classLayouts.find(stackTraceElementName);
  const auto stackFunction = fields.find(stackFunctionFieldName);
  const auto stackFile = fields.find(stackFileFieldName);
  const auto stackLine = fields.find(stackLineFieldName);
  const auto stackColumn = fields.find(stackColumnFieldName);
  const bool canManageSuppressed =
      throwable != classLayouts.end() && suppressedNode != classLayouts.end() &&
      !throwable->second.descriptorName.empty() && suppressedNode->second.hasHeader &&
      suppressedNode->second.size > 0 &&
      !suppressedNode->second.descriptorName.empty() &&
      suppressedHead != fields.end() &&
      suppressedHead->second.simpleType == suppressedNodeName &&
      suppressedCount != fields.end() && suppressedCount->second.simpleType == "Int" &&
      suppressedValue != fields.end() &&
      suppressedValue->second.simpleType == throwableName &&
      suppressedNext != fields.end() &&
      suppressedNext->second.simpleType == suppressedNodeName;

  const auto emitRuntimeFailure =
      [&](std::string_view helperName, const auto& exceptionLayout,
          std::string_view messageConstant, std::size_t messageSize) {
        out << "define internal void @" << helperName << "() noreturn {\n";
        out << "entry:\n";
        if (exceptionLayout == classLayouts.end() ||
            !exceptionLayout->second.hasHeader || exceptionLayout->second.size == 0 ||
            exceptionLayout->second.descriptorName.empty() || message == fields.end() ||
            message->second.simpleType != "String" || cause == fields.end() ||
            cause->second.simpleType != throwableName) {
          out << "  call void @llvm.trap()\n";
          out << "  unreachable\n";
          out << "}\n\n";
          return;
        }
        out << "  %exception = call ptr @__scalanative_gc_object_alloc(i64 "
            << exceptionLayout->second.size << ", ptr @"
            << exceptionLayout->second.descriptorName << ")\n";
        out << "  %message_field = getelementptr i8, ptr %exception, i64 "
            << message->second.offset << "\n";
        out << "  store ptr getelementptr inbounds ([" << messageSize << " x i8], ptr @"
            << messageConstant << ", i64 0, i64 0), ptr %message_field\n";
        out << "  %cause_field = getelementptr i8, ptr %exception, i64 "
            << cause->second.offset << "\n";
        out << "  store ptr %exception, ptr %cause_field\n";
        out << "  call void @__scalanative_capture_exception_trace(ptr %exception)\n";
        out << "  call void @__scalanative_throw(ptr %exception)\n";
        out << "  unreachable\n";
        out << "}\n\n";
      };
  emitRuntimeFailure("__scalanative_throw_null_array", nullPointer, ".str.array_null",
                     21);
  emitRuntimeFailure("__scalanative_throw_array_index_out_of_bounds",
                     arrayIndexOutOfBounds, ".str.array_index_out_of_bounds", 29);
  emitRuntimeFailure("__scalanative_throw_negative_array_size", negativeArraySize,
                     ".str.negative_array_size", 30);
  emitRuntimeFailure("__scalanative_throw_class_cast", classCast, ".str.class_cast",
                     39);
  emitRuntimeFailure("__scalanative_throw_array_store", arrayStore, ".str.array_store",
                     54);
  emitRuntimeFailure("__scalanative_throw_null_receiver", nullPointer,
                     ".str.null_receiver", 24);
  emitRuntimeFailure("__scalanative_throw_null_exception", nullPointer,
                     ".str.null_throw", 32);
  emitRuntimeFailure("__scalanative_throw_arithmetic", arithmetic,
                     ".str.integer_divisor_zero", 31);
  emitRuntimeFailure("__scalanative_throw_assertion", assertionError,
                     ".str.assertion_failed", 17);
  emitRuntimeFailure("__scalanative_throw_assumption", assertionError,
                     ".str.assumption_failed", 18);
  emitRuntimeFailure("__scalanative_throw_requirement", illegalArgument,
                     ".str.requirement_failed", 19);
  emitRuntimeFailure("__scalanative_throw_array_range_zero_step", illegalArgument,
                     ".str.array_range_zero_step", 10);
  emitRuntimeFailure("__scalanative_throw_array_range_too_large", illegalArgument,
                     ".str.array_range_too_large", 25);
  emitRuntimeFailure("__scalanative_throw_array_concat_too_large", illegalArgument,
                     ".str.array_concat_too_large", 33);
  emitRuntimeFailure("__scalanative_throw_byte_buffer_position", illegalArgument,
                     ".str.byte_buffer_position", 37);
  emitRuntimeFailure("__scalanative_throw_byte_buffer_limit", illegalArgument,
                     ".str.byte_buffer_limit", 34);
  emitRuntimeFailure("__scalanative_throw_byte_buffer_underflow", bufferUnderflow,
                     ".str.byte_buffer_underflow", 21);
  emitRuntimeFailure("__scalanative_throw_byte_buffer_overflow", bufferOverflow,
                     ".str.byte_buffer_overflow", 20);

  const auto emitCheckedCondition = [&](std::string_view helperName,
                                        std::string_view failureHelper) {
    out << "define internal void @" << helperName << "(i1 %condition) {\n";
    out << "entry:\n";
    out << "  br i1 %condition, label %passed, label %failed\n";
    out << "failed:\n";
    out << "  call void @" << failureHelper << "()\n";
    out << "  unreachable\n";
    out << "passed:\n";
    out << "  ret void\n";
    out << "}\n\n";
  };
  emitCheckedCondition("__scalanative_assert", "__scalanative_throw_assertion");
  emitCheckedCondition("__scalanative_assume", "__scalanative_throw_assumption");
  emitCheckedCondition("__scalanative_require", "__scalanative_throw_requirement");

  const auto emitCheckedIntegerArithmetic =
      [&](std::string_view helperName, std::string_view integerType,
          std::string_view minimumValue, std::string_view instruction,
          std::string_view overflowResult) {
        out << "define internal " << integerType << " @" << helperName << "("
            << integerType << " %lhs, " << integerType << " %rhs) {\n";
        out << "entry:\n";
        out << "  %is_zero = icmp eq " << integerType << " %rhs, 0\n";
        out << "  br i1 %is_zero, label %zero, label %checked\n";
        out << "zero:\n";
        out << "  call void @__scalanative_throw_arithmetic()\n";
        out << "  unreachable\n";
        out << "checked:\n";
        out << "  %is_minimum = icmp eq " << integerType << " %lhs, " << minimumValue
            << "\n";
        out << "  %is_negative_one = icmp eq " << integerType << " %rhs, -1\n";
        out << "  %overflows = and i1 %is_minimum, %is_negative_one\n";
        out << "  %safe_rhs = select i1 %overflows, " << integerType << " 1, "
            << integerType << " %rhs\n";
        out << "  %computed = " << instruction << ' ' << integerType
            << " %lhs, %safe_rhs\n";
        out << "  %result = select i1 %overflows, " << integerType << ' '
            << overflowResult << ", " << integerType << " %computed\n";
        out << "  ret " << integerType << " %result\n";
        out << "}\n\n";
      };
  emitCheckedIntegerArithmetic("__scalanative_int_div", "i32", "-2147483648", "sdiv",
                               "-2147483648");
  emitCheckedIntegerArithmetic("__scalanative_int_rem", "i32", "-2147483648", "srem",
                               "0");
  emitCheckedIntegerArithmetic("__scalanative_long_div", "i64", "-9223372036854775808",
                               "sdiv", "-9223372036854775808");
  emitCheckedIntegerArithmetic("__scalanative_long_rem", "i64", "-9223372036854775808",
                               "srem", "0");

  out << "define internal ptr @__scalanative_throwable_to_string(ptr %exception) "
         "{\n";
  out << "entry:\n";
  out << "  %descriptor = call ptr @__scalanative_object_descriptor(ptr "
         "%exception)\n";
  out << "  %type_name = call ptr @__scalanative_descriptor_type_name(ptr "
         "%descriptor)\n";
  out << "  %last_dot = call ptr @strrchr(ptr %type_name, i32 46)\n";
  out << "  %qualified = icmp ne ptr %last_dot, null\n";
  out << "  br i1 %qualified, label %strip_package, label %unqualified\n";
  out << "strip_package:\n";
  out << "  %simple_name = getelementptr i8, ptr %last_dot, i64 1\n";
  out << "  br label %describe\n";
  out << "unqualified:\n";
  out << "  br label %describe\n";
  out << "describe:\n";
  out << "  %name = phi ptr [ %simple_name, %strip_package ], [ %type_name, "
         "%unqualified ]\n";
  if (throwable == classLayouts.end() || message == fields.end() ||
      message->second.simpleType != "String") {
    out << "  ret ptr %name\n";
  } else {
    out << "  %message_field = getelementptr i8, ptr %exception, i64 "
        << message->second.offset << "\n";
    out << "  %message = load ptr, ptr %message_field\n";
    out << "  %message_is_null = icmp eq ptr %message, null\n";
    out << "  br i1 %message_is_null, label %type_only, label %check_message\n";
    out << "check_message:\n";
    out << "  %message_first_byte = load i8, ptr %message\n";
    out << "  %message_is_empty = icmp eq i8 %message_first_byte, 0\n";
    out << "  br i1 %message_is_empty, label %type_only, label %format\n";
    out << "format:\n";
    out << "  %name_length = call i64 @strlen(ptr %name)\n";
    out << "  %message_length = call i64 @strlen(ptr %message)\n";
    out << "  %text_length = add i64 %name_length, %message_length\n";
    out << "  %description_length = add i64 %text_length, 2\n";
    out << "  %payload_size = add i64 %description_length, 1\n";
    out << "  %allocation_size = add i64 %payload_size, " << ObjectHeaderSize << "\n";
    out << "  %storage = call ptr @__scalanative_program_arena_alloc(i64 "
           "%allocation_size, ptr null)\n";
    out << "  %result = getelementptr i8, ptr %storage, i64 " << ObjectHeaderSize
        << "\n";
    out << "  call ptr @strcpy(ptr %result, ptr %name)\n";
    out << "  %separator = getelementptr i8, ptr %result, i64 %name_length\n";
    out << "  store i8 58, ptr %separator\n";
    out << "  %space = getelementptr i8, ptr %separator, i64 1\n";
    out << "  store i8 32, ptr %space\n";
    out << "  %message_offset = add i64 %name_length, 2\n";
    out << "  %message_destination = getelementptr i8, ptr %result, i64 "
           "%message_offset\n";
    out << "  call ptr @strcpy(ptr %message_destination, ptr %message)\n";
    out << "  ret ptr %result\n";
    out << "type_only:\n";
    out << "  ret ptr %name\n";
  }
  out << "}\n\n";

  out << "define internal ptr @__scalanative_get_stack_trace(ptr %exception) {\n";
  out << "entry:\n";
  const bool canMaterializeStackTrace =
      stackTraceElement != classLayouts.end() && stackTraceElement->second.hasHeader &&
      stackTraceElement->second.size > 0 &&
      !stackTraceElement->second.descriptorName.empty() &&
      stackFunction != fields.end() && stackFunction->second.simpleType == "String" &&
      stackFile != fields.end() && stackFile->second.simpleType == "String" &&
      stackLine != fields.end() && stackLine->second.simpleType == "Int" &&
      stackColumn != fields.end() && stackColumn->second.simpleType == "Int";
  if (!canMaterializeStackTrace) {
    out << "  %array = call ptr @__scalanative_program_arena_alloc(i64 16, ptr "
           "null)\n";
    out << "  %length_slot = getelementptr i8, ptr %array, i64 8\n";
    out << "  store i64 0, ptr %length_slot\n";
    out << "  ret ptr %array\n";
  } else {
    out << "  %trace = call ptr @__scalanative_exception_trace(ptr %exception)\n";
    out << "  %has_trace = icmp ne ptr %trace, null\n";
    out << "  br i1 %has_trace, label %load_count, label %empty\n";
    out << "load_count:\n";
    out << "  %count_field = getelementptr %scalanative.exception_trace, ptr "
           "%trace, i32 0, i32 0\n";
    out << "  %stored_count = load i32, ptr %count_field\n";
    out << "  %within_capacity = icmp ult i32 %stored_count, 65\n";
    out << "  %bounded_count = select i1 %within_capacity, i32 %stored_count, i32 "
           "64\n";
    out << "  br label %allocate\n";
    out << "empty:\n";
    out << "  br label %allocate\n";
    out << "allocate:\n";
    out << "  %count = phi i32 [ %bounded_count, %load_count ], [ 0, %empty ]\n";
    out << "  %wide_count = zext i32 %count to i64\n";
    out << "  %elements_size = mul i64 %wide_count, 8\n";
    out << "  %allocation_size = add i64 %elements_size, 16\n";
    out << "  %array = call ptr @__scalanative_program_arena_alloc(i64 "
           "%allocation_size, ptr null)\n";
    out << "  %length_slot = getelementptr i8, ptr %array, i64 8\n";
    out << "  store i64 %wide_count, ptr %length_slot\n";
    out << "  br label %frames\n";
    out << "frames:\n";
    out << "  %index = phi i32 [ 0, %allocate ], [ %next_index, %materialize ]\n";
    out << "  %done = icmp uge i32 %index, %count\n";
    out << "  br i1 %done, label %return, label %materialize\n";
    out << "materialize:\n";
    out << "  %entries = getelementptr %scalanative.exception_trace, ptr %trace, "
           "i32 0, i32 1\n";
    out << "  %trace_entry = getelementptr [64 x "
           "%scalanative.exception_trace_entry], "
           "ptr %entries, i32 0, i32 %index\n";
    out << "  %function_slot = getelementptr %scalanative.exception_trace_entry, "
           "ptr %trace_entry, i32 0, i32 0\n";
    out << "  %function = load ptr, ptr %function_slot\n";
    out << "  %file_slot = getelementptr %scalanative.exception_trace_entry, ptr "
           "%trace_entry, i32 0, i32 1\n";
    out << "  %file = load ptr, ptr %file_slot\n";
    out << "  %line_slot = getelementptr %scalanative.exception_trace_entry, ptr "
           "%trace_entry, i32 0, i32 2\n";
    out << "  %line = load i32, ptr %line_slot\n";
    out << "  %column_slot = getelementptr %scalanative.exception_trace_entry, ptr "
           "%trace_entry, i32 0, i32 3\n";
    out << "  %column = load i32, ptr %column_slot\n";
    out << "  %frame = call ptr @__scalanative_program_arena_alloc(i64 "
        << stackTraceElement->second.size << ", ptr @"
        << stackTraceElement->second.descriptorName << ")\n";
    out << "  %frame_function = getelementptr i8, ptr %frame, i64 "
        << stackFunction->second.offset << "\n";
    out << "  store ptr %function, ptr %frame_function\n";
    out << "  %frame_file = getelementptr i8, ptr %frame, i64 "
        << stackFile->second.offset << "\n";
    out << "  store ptr %file, ptr %frame_file\n";
    out << "  %frame_line = getelementptr i8, ptr %frame, i64 "
        << stackLine->second.offset << "\n";
    out << "  store i32 %line, ptr %frame_line\n";
    out << "  %frame_column = getelementptr i8, ptr %frame, i64 "
        << stackColumn->second.offset << "\n";
    out << "  store i32 %column, ptr %frame_column\n";
    out << "  %wide_index = zext i32 %index to i64\n";
    out << "  %element_bytes = mul i64 %wide_index, 8\n";
    out << "  %element_offset = add i64 %element_bytes, 16\n";
    out << "  %element_slot = getelementptr i8, ptr %array, i64 %element_offset\n";
    out << "  store ptr %frame, ptr %element_slot\n";
    out << "  %next_index = add i32 %index, 1\n";
    out << "  br label %frames\n";
    out << "return:\n";
    out << "  ret ptr %array\n";
  }
  out << "}\n\n";

  out << "define internal i32 @__scalanative_set_stack_trace(ptr %exception, ptr "
         "%array) {\n";
  out << "entry:\n";
  if (!canMaterializeStackTrace) {
    out << "  ret i32 0\n";
  } else {
    out << "  %array_is_null = icmp eq ptr %array, null\n";
    out << "  br i1 %array_is_null, label %invalid_array, label %find_slot\n";
    out << "find_slot:\n";
    out << "  %slot = call ptr @__scalanative_exception_trace_slot(ptr "
           "%exception)\n";
    out << "  %has_slot = icmp ne ptr %slot, null\n";
    out << "  br i1 %has_slot, label %load_length, label %done\n";
    out << "load_length:\n";
    out << "  %length_slot = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize
        << "\n";
    out << "  %stored_length = load i64, ptr %length_slot\n";
    out << "  %within_capacity = icmp ult i64 %stored_length, 65\n";
    out << "  %bounded_length = select i1 %within_capacity, i64 %stored_length, "
           "i64 64\n";
    out << "  %count = trunc i64 %bounded_length to i32\n";
    out << "  %trace_end = getelementptr %scalanative.exception_trace, ptr null, "
           "i32 1\n";
    out << "  %trace_size = ptrtoint ptr %trace_end to i64\n";
    out << "  %replacement = call ptr @malloc(i64 %trace_size)\n";
    out << "  %allocated = icmp ne ptr %replacement, null\n";
    out << "  br i1 %allocated, label %copy_start, label %done\n";
    out << "copy_start:\n";
    out << "  br label %copy\n";
    out << "copy:\n";
    out << "  %index = phi i32 [ 0, %copy_start ], [ %next_index, %copy_frame ]\n";
    out << "  %copy_complete = icmp uge i32 %index, %count\n";
    out << "  br i1 %copy_complete, label %install, label %load_frame\n";
    out << "load_frame:\n";
    out << "  %wide_index = zext i32 %index to i64\n";
    out << "  %elements = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize + 8
        << "\n";
    out << "  %frame_slot = getelementptr ptr, ptr %elements, i64 %wide_index\n";
    out << "  %frame = load ptr, ptr %frame_slot\n";
    out << "  %frame_is_null = icmp eq ptr %frame, null\n";
    out << "  br i1 %frame_is_null, label %invalid_frame, label %copy_frame\n";
    out << "copy_frame:\n";
    out << "  %function_field = getelementptr i8, ptr %frame, i64 "
        << stackFunction->second.offset << "\n";
    out << "  %function = load ptr, ptr %function_field\n";
    out << "  %file_field = getelementptr i8, ptr %frame, i64 "
        << stackFile->second.offset << "\n";
    out << "  %file = load ptr, ptr %file_field\n";
    out << "  %line_field = getelementptr i8, ptr %frame, i64 "
        << stackLine->second.offset << "\n";
    out << "  %line = load i32, ptr %line_field\n";
    out << "  %column_field = getelementptr i8, ptr %frame, i64 "
        << stackColumn->second.offset << "\n";
    out << "  %column = load i32, ptr %column_field\n";
    out << "  %entries = getelementptr %scalanative.exception_trace, ptr "
           "%replacement, i32 0, i32 1\n";
    out << "  %replacement_entry = getelementptr [64 x "
           "%scalanative.exception_trace_entry], ptr %entries, i32 0, i32 "
           "%index\n";
    out << "  %entry_function = getelementptr %scalanative.exception_trace_entry, "
           "ptr %replacement_entry, i32 0, i32 0\n";
    out << "  store ptr %function, ptr %entry_function\n";
    out << "  %entry_file = getelementptr %scalanative.exception_trace_entry, ptr "
           "%replacement_entry, i32 0, i32 1\n";
    out << "  store ptr %file, ptr %entry_file\n";
    out << "  %entry_line = getelementptr %scalanative.exception_trace_entry, ptr "
           "%replacement_entry, i32 0, i32 2\n";
    out << "  store i32 %line, ptr %entry_line\n";
    out << "  %entry_column = getelementptr %scalanative.exception_trace_entry, "
           "ptr %replacement_entry, i32 0, i32 3\n";
    out << "  store i32 %column, ptr %entry_column\n";
    out << "  %next_index = add i32 %index, 1\n";
    out << "  br label %copy\n";
    out << "install:\n";
    out << "  %count_field = getelementptr %scalanative.exception_trace, ptr "
           "%replacement, i32 0, i32 0\n";
    out << "  store i32 %count, ptr %count_field\n";
    out << "  %existing = load ptr, ptr %slot\n";
    out << "  %has_existing = icmp ne ptr %existing, null\n";
    out << "  br i1 %has_existing, label %release_existing, label "
           "%store_replacement\n";
    out << "release_existing:\n";
    out << "  call void @free(ptr %existing)\n";
    out << "  br label %store_replacement\n";
    out << "store_replacement:\n";
    out << "  store ptr %replacement, ptr %slot\n";
    out << "  br label %done\n";
    out << "invalid_frame:\n";
    out << "  call void @free(ptr %replacement)\n";
    out << "  ret i32 2\n";
    out << "invalid_array:\n";
    out << "  ret i32 1\n";
    out << "done:\n";
    out << "  ret i32 0\n";
  }
  out << "}\n\n";

  out << "define internal void @__scalanative_add_suppressed(ptr %exception, ptr "
         "%suppressed) {\n";
  out << "entry:\n";
  if (!canManageSuppressed) {
    out << "  ret void\n";
  } else {
    out << "  %suppressed_is_null = icmp eq ptr %suppressed, null\n";
    out << "  %suppressed_is_self = icmp eq ptr %suppressed, %exception\n";
    out << "  %suppressed_is_present = xor i1 %suppressed_is_null, true\n";
    out << "  %suppressed_is_distinct = xor i1 %suppressed_is_self, true\n";
    out << "  %valid = and i1 %suppressed_is_present, %suppressed_is_distinct\n";
    out << "  br i1 %valid, label %check_capacity, label %invalid\n";
    out << "check_capacity:\n";
    out << "  %count_field = getelementptr i8, ptr %exception, i64 "
        << suppressedCount->second.offset << "\n";
    out << "  %count = load i32, ptr %count_field\n";
    out << "  %has_capacity = icmp ult i32 %count, " << SuppressedExceptionCapacity
        << "\n";
    out << "  br i1 %has_capacity, label %allocate_node, label %done\n";
    out << "allocate_node:\n";
    out << "  %node = call ptr @__scalanative_gc_object_alloc(i64 "
        << suppressedNode->second.size << ", ptr @"
        << suppressedNode->second.descriptorName << ")\n";
    out << "  %value_field = getelementptr i8, ptr %node, i64 "
        << suppressedValue->second.offset << "\n";
    out << "  store ptr %suppressed, ptr %value_field\n";
    out << "  %head_field = getelementptr i8, ptr %exception, i64 "
        << suppressedHead->second.offset << "\n";
    out << "  %head = load ptr, ptr %head_field\n";
    out << "  %next_field = getelementptr i8, ptr %node, i64 "
        << suppressedNext->second.offset << "\n";
    out << "  store ptr %head, ptr %next_field\n";
    out << "  store ptr %node, ptr %head_field\n";
    out << "  %next_count = add i32 %count, 1\n";
    out << "  store i32 %next_count, ptr %count_field\n";
    out << "  br label %done\n";
    out << "invalid:\n";
    out << "  call void @llvm.trap()\n";
    out << "  unreachable\n";
    out << "done:\n";
    out << "  ret void\n";
  }
  out << "}\n\n";

  out << "define internal ptr @__scalanative_get_suppressed(ptr %exception) {\n";
  out << "entry:\n";
  out << "  %array = call ptr @__scalanative_gc_object_alloc(i64 "
      << SuppressedArraySize << ", ptr @__scalanative_suppressed_array_descriptor)\n";
  out << "  %length_field = getelementptr i8, ptr %array, i64 " << ObjectHeaderSize
      << "\n";
  out << "  %owner_field = getelementptr i8, ptr %array, i64 "
      << SuppressedArrayOwnerOffset << "\n";
  out << "  store ptr %exception, ptr %owner_field\n";
  if (!canManageSuppressed) {
    out << "  store i64 0, ptr %length_field\n";
    out << "  ret ptr %array\n";
  } else {
    out << "  %count_field = getelementptr i8, ptr %exception, i64 "
        << suppressedCount->second.offset << "\n";
    out << "  %stored_count = load i32, ptr %count_field\n";
    out << "  %count_is_nonnegative = icmp sge i32 %stored_count, 0\n";
    out << "  %count_is_bounded = icmp ult i32 %stored_count, "
        << SuppressedExceptionCapacity + 1 << "\n";
    out << "  %capacity_count = select i1 %count_is_bounded, i32 %stored_count, "
           "i32 "
        << SuppressedExceptionCapacity << "\n";
    out << "  %count = select i1 %count_is_nonnegative, i32 %capacity_count, i32 "
           "0\n";
    out << "  %wide_count = zext i32 %count to i64\n";
    out << "  store i64 %wide_count, ptr %length_field\n";
    out << "  %head_field = getelementptr i8, ptr %exception, i64 "
        << suppressedHead->second.offset << "\n";
    out << "  %head = load ptr, ptr %head_field\n";
    out << "  br label %copy\n";
    out << "copy:\n";
    out << "  %node = phi ptr [ %head, %entry ], [ %next, %copy_value ]\n";
    out << "  %remaining = phi i32 [ %count, %entry ], [ %next_remaining, "
           "%copy_value ]\n";
    out << "  %has_node = icmp ne ptr %node, null\n";
    out << "  %has_remaining = icmp ugt i32 %remaining, 0\n";
    out << "  %continue_copy = and i1 %has_node, %has_remaining\n";
    out << "  br i1 %continue_copy, label %copy_value, label %done\n";
    out << "copy_value:\n";
    out << "  %value_field = getelementptr i8, ptr %node, i64 "
        << suppressedValue->second.offset << "\n";
    out << "  %value = load ptr, ptr %value_field\n";
    out << "  %target_index = sub i32 %remaining, 1\n";
    out << "  %wide_target_index = zext i32 %target_index to i64\n";
    out << "  %element_bytes = mul i64 %wide_target_index, 8\n";
    out << "  %element_offset = add i64 %element_bytes, " << ObjectHeaderSize + 8
        << "\n";
    out << "  %element_slot = getelementptr i8, ptr %array, i64 %element_offset\n";
    out << "  store ptr %value, ptr %element_slot\n";
    out << "  %next_field = getelementptr i8, ptr %node, i64 "
        << suppressedNext->second.offset << "\n";
    out << "  %next = load ptr, ptr %next_field\n";
    out << "  %next_remaining = sub i32 %remaining, 1\n";
    out << "  br label %copy\n";
    out << "done:\n";
    out << "  ret ptr %array\n";
  }
  out << "}\n\n";

  out << "define internal void @__scalanative_report_suppressed_exceptions(ptr "
         "%exception) {\n";
  out << "entry:\n";
  if (!canManageSuppressed) {
    out << "  ret void\n";
  } else {
    out << "  %is_throwable = call i1 @__scalanative_is_instance_of(ptr "
           "%exception, ptr @"
        << throwable->second.descriptorName << ")\n";
    out << "  br i1 %is_throwable, label %inspect, label %done\n";
    out << "inspect:\n";
    out << "  %values = alloca [" << SuppressedExceptionCapacity
        << " x ptr], align 8\n";
    out << "  %count_field = getelementptr i8, ptr %exception, i64 "
        << suppressedCount->second.offset << "\n";
    out << "  %stored_count = load i32, ptr %count_field\n";
    out << "  %count_is_nonnegative = icmp sge i32 %stored_count, 0\n";
    out << "  %count_is_bounded = icmp ult i32 %stored_count, "
        << SuppressedExceptionCapacity + 1 << "\n";
    out << "  %capacity_count = select i1 %count_is_bounded, i32 %stored_count, "
           "i32 "
        << SuppressedExceptionCapacity << "\n";
    out << "  %count = select i1 %count_is_nonnegative, i32 %capacity_count, i32 "
           "0\n";
    out << "  %head_field = getelementptr i8, ptr %exception, i64 "
        << suppressedHead->second.offset << "\n";
    out << "  %head = load ptr, ptr %head_field\n";
    out << "  br label %gather\n";
    out << "gather:\n";
    out << "  %node = phi ptr [ %head, %inspect ], [ %next, %gather_value ]\n";
    out << "  %remaining = phi i32 [ %count, %inspect ], [ %next_remaining, "
           "%gather_value ]\n";
    out << "  %has_node = icmp ne ptr %node, null\n";
    out << "  %has_remaining = icmp ugt i32 %remaining, 0\n";
    out << "  %continue_gather = and i1 %has_node, %has_remaining\n";
    out << "  br i1 %continue_gather, label %gather_value, label %print_setup\n";
    out << "gather_value:\n";
    out << "  %value_field = getelementptr i8, ptr %node, i64 "
        << suppressedValue->second.offset << "\n";
    out << "  %value = load ptr, ptr %value_field\n";
    out << "  %target_index = sub i32 %remaining, 1\n";
    out << "  %wide_target_index = zext i32 %target_index to i64\n";
    out << "  %value_slot = getelementptr [" << SuppressedExceptionCapacity
        << " x ptr], ptr %values, i64 0, i64 %wide_target_index\n";
    out << "  store ptr %value, ptr %value_slot\n";
    out << "  %next_field = getelementptr i8, ptr %node, i64 "
        << suppressedNext->second.offset << "\n";
    out << "  %next = load ptr, ptr %next_field\n";
    out << "  %next_remaining = sub i32 %remaining, 1\n";
    out << "  br label %gather\n";
    out << "print_setup:\n";
    out << "  br label %print_values\n";
    out << "print_values:\n";
    out << "  %index = phi i32 [ 0, %print_setup ], [ %next_index, %print_value ]\n";
    out << "  %print_complete = icmp uge i32 %index, %count\n";
    out << "  br i1 %print_complete, label %done, label %print_value\n";
    out << "print_value:\n";
    out << "  %wide_index = zext i32 %index to i64\n";
    out << "  %print_slot = getelementptr [" << SuppressedExceptionCapacity
        << " x ptr], ptr %values, i64 0, i64 %wide_index\n";
    out << "  %suppressed = load ptr, ptr %print_slot\n";
    out << "  %description = call ptr @__scalanative_any_to_string(ptr "
           "%suppressed)\n";
    out << "  %error_stream = load ptr, ptr @stderr\n";
    out << "  %reported = call i32 (ptr, ptr, ...) @fprintf(ptr %error_stream, ptr "
           "getelementptr inbounds ([16 x i8], ptr @.fmt.suppressed, i64 0, i64 "
           "0), ptr %description)\n";
    out << "  call void @__scalanative_report_exception_trace(ptr %suppressed, i1 "
           "false)\n";
    out << "  %next_index = add i32 %index, 1\n";
    out << "  br label %print_values\n";
    out << "done:\n";
    out << "  ret void\n";
  }
  out << "}\n\n";

  out << "define internal ptr "
         "@__scalanative_stack_trace_element_to_string(ptr %frame) {\n";
  out << "entry:\n";
  if (!canMaterializeStackTrace) {
    out << "  ret ptr getelementptr inbounds ([25 x i8], ptr "
           "@.str.stack_trace_element_unknown, i64 0, i64 0)\n";
  } else {
    out << "  %function_field = getelementptr i8, ptr %frame, i64 "
        << stackFunction->second.offset << "\n";
    out << "  %stored_function = load ptr, ptr %function_field\n";
    out << "  %function_is_null = icmp eq ptr %stored_function, null\n";
    out << "  %function = select i1 %function_is_null, ptr getelementptr inbounds "
           "([10 x i8], ptr @.str.stack_trace_unknown, i64 0, i64 0), ptr "
           "%stored_function\n";
    out << "  %file_field = getelementptr i8, ptr %frame, i64 "
        << stackFile->second.offset << "\n";
    out << "  %stored_file = load ptr, ptr %file_field\n";
    out << "  %file_is_null = icmp eq ptr %stored_file, null\n";
    out << "  %file = select i1 %file_is_null, ptr getelementptr inbounds ([10 x "
           "i8], ptr @.str.stack_trace_unknown, i64 0, i64 0), ptr %stored_file\n";
    out << "  %line_field = getelementptr i8, ptr %frame, i64 "
        << stackLine->second.offset << "\n";
    out << "  %line = load i32, ptr %line_field\n";
    out << "  %column_field = getelementptr i8, ptr %frame, i64 "
        << stackColumn->second.offset << "\n";
    out << "  %column = load i32, ptr %column_field\n";
    out << "  %required = call i32 (ptr, i64, ptr, ...) @snprintf(ptr null, i64 "
           "0, ptr getelementptr inbounds ([13 x i8], ptr "
           "@.fmt.stack_trace_element, i64 0, i64 0), ptr %function, ptr %file, "
           "i32 %line, i32 %column)\n";
    out << "  %format_succeeded = icmp sge i32 %required, 0\n";
    out << "  br i1 %format_succeeded, label %allocate, label %fallback\n";
    out << "allocate:\n";
    out << "  %wide_required = zext i32 %required to i64\n";
    out << "  %payload_size = add i64 %wide_required, 1\n";
    out << "  %allocation_size = add i64 %payload_size, " << ObjectHeaderSize << "\n";
    out << "  %storage = call ptr @__scalanative_program_arena_alloc(i64 "
           "%allocation_size, ptr null)\n";
    out << "  %result = getelementptr i8, ptr %storage, i64 " << ObjectHeaderSize
        << "\n";
    out << "  %written = call i32 (ptr, i64, ptr, ...) @snprintf(ptr %result, "
           "i64 %payload_size, ptr getelementptr inbounds ([13 x i8], ptr "
           "@.fmt.stack_trace_element, i64 0, i64 0), ptr %function, ptr %file, "
           "i32 %line, i32 %column)\n";
    out << "  ret ptr %result\n";
    out << "fallback:\n";
    out << "  ret ptr getelementptr inbounds ([25 x i8], ptr "
           "@.str.stack_trace_element_unknown, i64 0, i64 0)\n";
  }
  out << "}\n\n";

  out << "define internal ptr @__scalanative_exception_cause(ptr %exception) {\n";
  out << "entry:\n";
  if (throwable == classLayouts.end() || cause == fields.end() ||
      throwable->second.descriptorName.empty()) {
    out << "  ret ptr null\n";
  } else {
    out << "  %is_throwable = call i1 @__scalanative_is_instance_of(ptr "
           "%exception, ptr @"
        << throwable->second.descriptorName << ")\n";
    out << "  br i1 %is_throwable, label %load, label %none\n";
    out << "load:\n";
    out << "  %cause_field = getelementptr i8, ptr %exception, i64 "
        << cause->second.offset << "\n";
    out << "  %cause = load ptr, ptr %cause_field\n";
    out << "  %uninitialized = icmp eq ptr %cause, %exception\n";
    out << "  %visible_cause = select i1 %uninitialized, ptr null, ptr %cause\n";
    out << "  ret ptr %visible_cause\n";
    out << "none:\n";
    out << "  ret ptr null\n";
  }
  out << "}\n\n";

  out << "define internal ptr @__scalanative_exception_trace_slot(ptr %exception) "
         "{\n";
  out << "entry:\n";
  if (throwable == classLayouts.end() || trace == fields.end() ||
      trace->second.simpleType != "Long" || throwable->second.descriptorName.empty()) {
    out << "  ret ptr null\n";
  } else {
    out << "  %is_throwable = call i1 @__scalanative_is_instance_of(ptr "
           "%exception, ptr @"
        << throwable->second.descriptorName << ")\n";
    out << "  br i1 %is_throwable, label %slot, label %none\n";
    out << "slot:\n";
    out << "  %trace_field = getelementptr i8, ptr %exception, i64 "
        << trace->second.offset << "\n";
    out << "  ret ptr %trace_field\n";
    out << "none:\n";
    out << "  ret ptr null\n";
  }
  out << "}\n\n";
}

std::string numericLiteralWithoutSuffix(std::string text) {
  if (!text.empty() &&
      (text.back() == 'l' || text.back() == 'L' || text.back() == 'f' ||
       text.back() == 'F' || text.back() == 'd' || text.back() == 'D')) {
    text.pop_back();
  }
  return text;
}

std::string normalizedFloatingLiteral(const std::string& text,
                                      const std::string& type) {
  try {
    const double parsed = std::stod(numericLiteralWithoutSuffix(text));
    std::ostringstream out;
    out << std::scientific
        << std::setprecision(type == "Float"
                                 ? std::numeric_limits<float>::max_digits10
                                 : std::numeric_limits<double>::max_digits10);
    if (type == "Float") {
      out << static_cast<float>(parsed);
    } else {
      out << parsed;
    }
    return out.str();
  } catch (...) {
    return {};
  }
}

int decodedCharLiteral(std::string_view text) {
  if (text.size() < 3 || text.front() != '\'' || text.back() != '\'') {
    return -1;
  }
  text.remove_prefix(1);
  text.remove_suffix(1);
  if (text.size() == 1) {
    return static_cast<unsigned char>(text.front());
  }
  if (text.size() != 2 || text.front() != '\\') {
    return -1;
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
    return -1;
  }
}

std::string decodeStringLiteral(std::string_view text) {
  if (text.starts_with("\"\"\"") && text.ends_with("\"\"\"") && text.size() >= 6) {
    return std::string(text.substr(3, text.size() - 6));
  }
  if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
    return std::string(text);
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

std::string llvmCString(std::string_view text) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string escaped;
  for (char ch : text) {
    const unsigned char byte = static_cast<unsigned char>(ch);
    if (byte >= 0x20U && byte <= 0x7EU && ch != '"' && ch != '\\') {
      escaped.push_back(ch);
      continue;
    }
    escaped.push_back('\\');
    escaped.push_back(hex[byte >> 4U]);
    escaped.push_back(hex[byte & 0x0FU]);
  }
  return escaped;
}

std::string stringPointerExpression(const StringConstant& constant) {
  return "getelementptr inbounds ([" + std::to_string(constant.contents.size() + 1) +
         " x i8], ptr @" + constant.name + ", i64 0, i64 0)";
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
  if (dot == std::string::npos) {
    return definitionName;
  }
  return definitionName.substr(dot + 1);
}

struct DebugFileInfo {
  std::string filename;
  std::string directory;
  std::size_t fileNode = 0;
  std::size_t compileUnitNode = 0;
};

struct DebugFunctionInfo {
  const nir::Definition* definition = nullptr;
  support::SourceId source;
  Signature signature;
  std::string displayName;
  std::string linkageName;
  std::size_t line = 0;
  std::size_t fileNode = 0;
  std::size_t compileUnitNode = 0;
  std::size_t typeListNode = 0;
  std::size_t subroutineTypeNode = 0;
  std::size_t subprogramNode = 0;
};

struct DebugTypeInfo {
  std::string simpleType;
  std::size_t typeNode = 0;
};

struct DebugCompositeMemberInfo {
  std::string name;
  std::size_t memberNode = 0;
  std::size_t typeNode = 0;
  std::size_t line = 0;
  std::size_t sizeInBits = 0;
  std::size_t offsetInBits = 0;
};

struct DebugCompositeInheritanceInfo {
  std::size_t inheritanceNode = 0;
  std::size_t baseCompositeNode = 0;
  std::size_t offsetInBits = 0;
};

struct DebugCompositeTypeInfo {
  std::string name;
  std::size_t compositeNode = 0;
  std::size_t elementsNode = 0;
  std::size_t fileNode = 0;
  std::size_t line = 0;
  std::size_t sizeInBits = 0;
  bool forwardDeclaration = false;
  std::vector<DebugCompositeInheritanceInfo> inheritances;
  std::vector<DebugCompositeMemberInfo> members;
};

struct DebugLocationInfo {
  std::size_t line = 0;
  std::size_t column = 0;
  std::size_t scopeNode = 0;
  std::size_t locationNode = 0;
};

struct DebugLexicalScopeInfo {
  std::size_t scopeNode = 0;
  std::size_t parentScopeNode = 0;
  std::size_t fileNode = 0;
  std::size_t line = 0;
  std::size_t column = 0;
};

struct DebugVariableInfo {
  const nir::Instruction* instruction = nullptr;
  std::string name;
  std::size_t argumentIndex = 0;
  std::size_t line = 0;
  std::size_t fileNode = 0;
  std::size_t typeNode = 0;
  std::size_t scopeNode = 0;
  std::size_t variableNode = 0;
  std::size_t locationNode = 0;
  bool mutableLocal = false;
};

struct DebugVariableBinding {
  std::size_t variableNode = 0;
  std::size_t locationNode = 0;
  bool mutableLocal = false;
};

class DebugMetadata {
public:
  DebugMetadata(
      const linker::LinkedProgram& program, const CodegenOptions& options,
      const std::unordered_map<std::string, ClassLayout>& classLayouts,
      const std::unordered_map<std::string, FieldInfo>& fields,
      const std::unordered_map<std::string, std::vector<std::string>>& classParents)
      : optimized_(options.optimized) {
    if (!options.emitDebugInfo || options.sources == nullptr) {
      return;
    }

    for (const nir::Module& module : program.modules) {
      for (const nir::Definition& definition : module.definitions) {
        if (definition.kind == nir::DefinitionKind::Class ||
            definition.kind == nir::DefinitionKind::Module ||
            definition.kind == nir::DefinitionKind::Trait) {
          classDefinitions_.emplace(definition.name, &definition);
        }
        if (definition.kind != nir::DefinitionKind::FunctionDef ||
            !definition.span.isValid()) {
          continue;
        }
        const support::SourceFile* source =
            options.sources->get(definition.span.source);
        if (source == nullptr) {
          continue;
        }
        const auto [line, column] = options.sources->lineColumn(definition.span);
        (void)column;
        if (line == 0) {
          continue;
        }

        const std::uint32_t sourceId = definition.span.source.value();
        auto file = sourceFiles_.find(sourceId);
        if (file == sourceFiles_.end()) {
          std::filesystem::path path(source->path);
          std::string filename = path.filename().string();
          if (filename.empty()) {
            filename = source->path;
          }
          std::string directory = path.parent_path().string();
          if (directory.empty()) {
            directory = ".";
          }
          const std::size_t index = files_.size();
          files_.push_back(DebugFileInfo{std::move(filename), std::move(directory),
                                         nextNode_++, nextNode_++});
          file = sourceFiles_.emplace(sourceId, index).first;
        }

        const DebugFileInfo& sourceInfo = files_[file->second];
        functions_.push_back(DebugFunctionInfo{
            &definition, definition.span.source, parseSignature(definition.signature),
            memberNameOf(definition.name), sanitizeIdentifier(definition.name), line,
            sourceInfo.fileNode, sourceInfo.compileUnitNode, 0, 0, 0});
      }
    }

    if (functions_.empty()) {
      return;
    }
    for (const DebugFunctionInfo& function : functions_) {
      registerType(function.signature.returnType);
      for (const std::string& parameterType : function.signature.parameterTypes) {
        registerType(parameterType);
      }
      for (const nir::Instruction& instruction :
           function.definition->body.instructions) {
        if (instruction.kind == nir::InstructionKind::Param ||
            instruction.kind == nir::InstructionKind::Let ||
            instruction.kind == nir::InstructionKind::Var) {
          registerType(instructionType(instruction));
        }
      }
    }
    std::vector<std::string> classNames;
    classNames.reserve(classLayouts.size());
    for (const auto& [className, layout] : classLayouts) {
      (void)layout;
      classNames.push_back(className);
      registerType(className);
    }
    std::sort(classNames.begin(), classNames.end());
    for (const auto& [fieldName, field] : fields) {
      (void)fieldName;
      registerType(field.simpleType);
    }
    buildCompositeTypes(classNames, classLayouts, fields, classParents,
                        *options.sources);
    for (DebugFunctionInfo& function : functions_) {
      function.typeListNode = nextNode_++;
      function.subroutineTypeNode = nextNode_++;
      function.subprogramNode = nextNode_++;
      subprogramNodes_.emplace(function.definition, function.subprogramNode);
    }
    expressionNode_ = nextNode_++;
    for (const DebugFunctionInfo& function : functions_) {
      std::size_t argumentIndex = 0;
      for (const nir::Instruction& instruction :
           function.definition->body.instructions) {
        if (instruction.kind == nir::InstructionKind::Param) {
          ++argumentIndex;
        }
        const support::SourceSpan span = instruction.kind == nir::InstructionKind::Param
                                             ? instruction.span
                                             : diagnosticSpan(instruction);
        if (!span.isValid() || span.source != function.source ||
            options.sources->get(span.source) == nullptr) {
          continue;
        }
        const auto [line, column] = options.sources->lineColumn(span);
        if (line == 0 || column == 0) {
          continue;
        }
        const std::size_t scopeNode =
            lexicalScopeFor(function, instruction, *options.sources);
        const std::size_t locationNode = nextNode_++;
        locations_.push_back(DebugLocationInfo{line, column, scopeNode, locationNode});
        locationNodes_.emplace(&instruction, locationNode);

        if (instruction.kind != nir::InstructionKind::Param &&
            instruction.kind != nir::InstructionKind::Let &&
            instruction.kind != nir::InstructionKind::Var) {
          continue;
        }
        const std::optional<std::size_t> typeNode =
            nodeForType(instructionType(instruction));
        if (!typeNode.has_value()) {
          continue;
        }
        const support::SourceSpan declarationSpan =
            instruction.span.isValid() ? instruction.span : span;
        const auto [declarationLine, declarationColumn] =
            options.sources->lineColumn(declarationSpan);
        (void)declarationColumn;
        const std::size_t variableNode = nextNode_++;
        const bool mutableLocal = instruction.kind == nir::InstructionKind::Var;
        variables_.push_back(DebugVariableInfo{
            &instruction, instruction.name,
            instruction.kind == nir::InstructionKind::Param ? argumentIndex : 0,
            declarationLine == 0 ? line : declarationLine, function.fileNode, *typeNode,
            scopeNode, variableNode, locationNode, mutableLocal});
        variableBindings_.emplace(
            &instruction,
            DebugVariableBinding{variableNode, locationNode, mutableLocal});
      }
    }
    dwarfVersionNode_ = nextNode_++;
    debugInfoVersionNode_ = nextNode_++;
    producerNode_ = nextNode_++;
  }

  [[nodiscard]] std::optional<std::size_t>
  subprogramFor(const nir::Definition& definition) const {
    auto found = subprogramNodes_.find(&definition);
    if (found == subprogramNodes_.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  [[nodiscard]] std::optional<std::size_t>
  locationFor(const nir::Instruction& instruction) const {
    auto found = locationNodes_.find(&instruction);
    if (found == locationNodes_.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  [[nodiscard]] bool hasLocations() const {
    return !locations_.empty();
  }

  [[nodiscard]] std::optional<DebugVariableBinding>
  variableFor(const nir::Instruction& instruction) const {
    auto found = variableBindings_.find(&instruction);
    if (found == variableBindings_.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  [[nodiscard]] bool hasValueVariables() const {
    return std::any_of(
        variables_.begin(), variables_.end(),
        [](const DebugVariableInfo& variable) { return !variable.mutableLocal; });
  }

  [[nodiscard]] bool hasMutableVariables() const {
    return std::any_of(
        variables_.begin(), variables_.end(),
        [](const DebugVariableInfo& variable) { return variable.mutableLocal; });
  }

  [[nodiscard]] std::size_t expressionNode() const {
    return expressionNode_;
  }

  void emit(std::ostringstream& out) const {
    if (functions_.empty()) {
      return;
    }

    out << "\n!llvm.dbg.cu = !{";
    for (std::size_t i = 0; i < files_.size(); ++i) {
      if (i != 0) {
        out << ", ";
      }
      out << '!' << files_[i].compileUnitNode;
    }
    out << "}\n!llvm.module.flags = !{!" << dwarfVersionNode_ << ", !"
        << debugInfoVersionNode_ << "}\n";
    out << "!llvm.ident = !{!" << producerNode_ << "}\n\n";

    for (const DebugFileInfo& file : files_) {
      out << '!' << file.fileNode << " = !DIFile(filename: \""
          << llvmCString(file.filename) << "\", directory: \""
          << llvmCString(file.directory) << "\")\n";
      out << '!' << file.compileUnitNode
          << " = distinct !DICompileUnit(language: DW_LANG_C_plus_plus, file: !"
          << file.fileNode << ", producer: \"cpp-scalanative\", isOptimized: "
          << (optimized_ ? "true" : "false")
          << ", runtimeVersion: 0, emissionKind: FullDebug)\n";
    }
    for (const DebugTypeInfo& type : types_) {
      out << '!' << type.typeNode << " = ";
      if (type.simpleType == "Boolean") {
        out << "!DIBasicType(name: \"Boolean\", size: 1, encoding: "
               "DW_ATE_boolean)\n";
      } else if (type.simpleType == "Byte") {
        out << "!DIBasicType(name: \"Byte\", size: 8, encoding: "
               "DW_ATE_signed)\n";
      } else if (type.simpleType == "Short") {
        out << "!DIBasicType(name: \"Short\", size: 16, encoding: "
               "DW_ATE_signed)\n";
      } else if (type.simpleType == "Int") {
        out << "!DIBasicType(name: \"Int\", size: 32, encoding: "
               "DW_ATE_signed)\n";
      } else if (type.simpleType == "Long") {
        out << "!DIBasicType(name: \"Long\", size: 64, encoding: "
               "DW_ATE_signed)\n";
      } else if (type.simpleType == "Float") {
        out << "!DIBasicType(name: \"Float\", size: 32, encoding: "
               "DW_ATE_float)\n";
      } else if (type.simpleType == "Double") {
        out << "!DIBasicType(name: \"Double\", size: 64, encoding: "
               "DW_ATE_float)\n";
      } else if (type.simpleType == "Char") {
        out << "!DIBasicType(name: \"Char\", size: 32, encoding: "
               "DW_ATE_unsigned)\n";
      } else {
        auto composite = compositeNodes_.find(type.simpleType);
        out << "!DIDerivedType(tag: DW_TAG_pointer_type, name: \""
            << llvmCString(type.simpleType) << "\", baseType: ";
        if (composite == compositeNodes_.end()) {
          out << "null";
        } else {
          out << '!' << composite->second;
        }
        out << ", size: 64)\n";
      }
    }
    for (const DebugCompositeTypeInfo& composite : compositeTypes_) {
      if (composite.forwardDeclaration) {
        out << '!' << composite.compositeNode
            << " = distinct !DICompositeType(tag: DW_TAG_structure_type, name: \""
            << llvmCString(composite.name) << "\", file: !" << composite.fileNode
            << ", line: " << composite.line << ", flags: DIFlagFwdDecl)\n";
        continue;
      }
      out << '!' << composite.elementsNode << " = !{";
      bool hasElement = false;
      for (const DebugCompositeInheritanceInfo& inheritance : composite.inheritances) {
        if (hasElement) {
          out << ", ";
        }
        out << '!' << inheritance.inheritanceNode;
        hasElement = true;
      }
      for (const DebugCompositeMemberInfo& member : composite.members) {
        if (hasElement) {
          out << ", ";
        }
        out << '!' << member.memberNode;
        hasElement = true;
      }
      out << "}\n";
      out << '!' << composite.compositeNode
          << " = distinct !DICompositeType(tag: DW_TAG_structure_type, name: \""
          << llvmCString(composite.name) << "\", file: !" << composite.fileNode
          << ", line: " << composite.line << ", size: " << composite.sizeInBits
          << ", elements: !" << composite.elementsNode << ")\n";
      for (const DebugCompositeInheritanceInfo& inheritance : composite.inheritances) {
        out << '!' << inheritance.inheritanceNode
            << " = !DIDerivedType(tag: DW_TAG_inheritance, scope: !"
            << composite.compositeNode << ", baseType: !"
            << inheritance.baseCompositeNode << ", offset: " << inheritance.offsetInBits
            << ", flags: DIFlagPublic)\n";
      }
      for (const DebugCompositeMemberInfo& member : composite.members) {
        out << '!' << member.memberNode << " = !DIDerivedType(tag: DW_TAG_member, "
            << "name: \"" << llvmCString(member.name) << "\", scope: !"
            << composite.compositeNode << ", file: !" << composite.fileNode
            << ", line: " << member.line << ", baseType: !" << member.typeNode
            << ", size: " << member.sizeInBits << ", offset: " << member.offsetInBits
            << ")\n";
      }
    }
    for (const DebugFunctionInfo& function : functions_) {
      out << '!' << function.typeListNode << " = !{";
      emitTypeReference(out, function.signature.returnType);
      for (const std::string& parameterType : function.signature.parameterTypes) {
        out << ", ";
        emitTypeReference(out, parameterType);
      }
      out << "}\n";
      out << '!' << function.subroutineTypeNode << " = !DISubroutineType(types: !"
          << function.typeListNode << ")\n";
      out << '!' << function.subprogramNode << " = distinct !DISubprogram(name: \""
          << llvmCString(function.displayName) << "\", linkageName: \""
          << llvmCString(function.linkageName) << "\", scope: !" << function.fileNode
          << ", file: !" << function.fileNode << ", line: " << function.line
          << ", type: !" << function.subroutineTypeNode
          << ", scopeLine: " << function.line
          << ", spFlags: DISPFlagDefinition, unit: !" << function.compileUnitNode
          << ")\n";
    }
    for (const DebugLexicalScopeInfo& scope : lexicalScopes_) {
      out << '!' << scope.scopeNode << " = distinct !DILexicalBlock(scope: !"
          << scope.parentScopeNode << ", file: !" << scope.fileNode
          << ", line: " << scope.line << ", column: " << scope.column << ")\n";
    }
    for (const DebugVariableInfo& variable : variables_) {
      out << '!' << variable.variableNode << " = !DILocalVariable(name: \""
          << llvmCString(variable.name) << '\"';
      if (variable.argumentIndex != 0) {
        out << ", arg: " << variable.argumentIndex;
      }
      out << ", scope: !" << variable.scopeNode << ", file: !" << variable.fileNode
          << ", line: " << variable.line << ", type: !" << variable.typeNode << ")\n";
    }
    for (const DebugLocationInfo& location : locations_) {
      out << '!' << location.locationNode << " = !DILocation(line: " << location.line
          << ", column: " << location.column << ", scope: !" << location.scopeNode
          << ")\n";
    }
    out << '!' << expressionNode_ << " = !DIExpression()\n";
    out << '!' << dwarfVersionNode_ << " = !{i32 7, !\"Dwarf Version\", i32 5}\n";
    out << '!' << debugInfoVersionNode_
        << " = !{i32 2, !\"Debug Info Version\", i32 3}\n";
    out << '!' << producerNode_ << " = !{!\"cpp-scalanative\"}\n";
  }

private:
  [[nodiscard]] static std::string
  instructionType(const nir::Instruction& instruction) {
    if (!instruction.type.empty()) {
      return instruction.type;
    }
    return instruction.value.type.empty() ? "Unknown" : instruction.value.type;
  }

  void registerType(const std::string& simpleType) {
    if (simpleType == "Unit" || typeNodes_.contains(simpleType)) {
      return;
    }
    const std::size_t typeNode = nextNode_++;
    typeNodes_.emplace(simpleType, typeNode);
    types_.push_back(DebugTypeInfo{simpleType, typeNode});
  }

  [[nodiscard]] std::optional<std::size_t>
  nodeForType(const std::string& simpleType) const {
    auto found = typeNodes_.find(simpleType);
    if (found == typeNodes_.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  void emitTypeReference(std::ostringstream& out, const std::string& simpleType) const {
    const std::optional<std::size_t> node = nodeForType(simpleType);
    if (node.has_value()) {
      out << '!' << *node;
    } else {
      out << "null";
    }
  }

  [[nodiscard]] std::size_t lexicalScopeFor(const DebugFunctionInfo& function,
                                            const nir::Instruction& instruction,
                                            const support::SourceManager& sources) {
    std::size_t parentScopeNode = function.subprogramNode;
    for (const support::SourceSpan scopeSpan : instruction.lexicalScopes) {
      if (!scopeSpan.isValid() || scopeSpan.source != function.source ||
          sources.get(scopeSpan.source) == nullptr) {
        continue;
      }
      const auto [line, column] = sources.lineColumn(scopeSpan);
      if (line == 0 || column == 0) {
        continue;
      }
      const std::string key = std::to_string(parentScopeNode) + ":" +
                              std::to_string(scopeSpan.source.value()) + ":" +
                              std::to_string(scopeSpan.start) + ":" +
                              std::to_string(scopeSpan.length);
      auto found = lexicalScopeNodes_.find(key);
      if (found == lexicalScopeNodes_.end()) {
        const std::size_t scopeNode = nextNode_++;
        lexicalScopes_.push_back(DebugLexicalScopeInfo{
            scopeNode, parentScopeNode, function.fileNode, line, column});
        found = lexicalScopeNodes_.emplace(key, scopeNode).first;
      }
      parentScopeNode = found->second;
    }
    return parentScopeNode;
  }

  void buildCompositeTypes(
      const std::vector<std::string>& classNames,
      const std::unordered_map<std::string, ClassLayout>& classLayouts,
      const std::unordered_map<std::string, FieldInfo>& fields,
      const std::unordered_map<std::string, std::vector<std::string>>& classParents,
      const support::SourceManager& sources) {
    // Allocate every composite node before resolving base-type references.
    for (const std::string& className : classNames) {
      auto definition = classDefinitions_.find(className);
      auto layout = classLayouts.find(className);
      if (definition == classDefinitions_.end() || definition->second == nullptr ||
          layout == classLayouts.end() || !definition->second->span.isValid()) {
        continue;
      }
      auto file = sourceFiles_.find(definition->second->span.source.value());
      if (file == sourceFiles_.end()) {
        continue;
      }
      const auto [line, column] = sources.lineColumn(definition->second->span);
      (void)column;
      if (line == 0) {
        continue;
      }

      DebugCompositeTypeInfo composite;
      composite.name = className;
      composite.compositeNode = nextNode_++;
      composite.fileNode = files_[file->second].fileNode;
      composite.line = line;
      composite.sizeInBits = layout->second.size * 8;
      composite.forwardDeclaration = layout->second.isTrait;
      if (!composite.forwardDeclaration) {
        composite.elementsNode = nextNode_++;
      }
      compositeNodes_.emplace(className, composite.compositeNode);
      compositeTypes_.push_back(std::move(composite));
    }

    for (DebugCompositeTypeInfo& composite : compositeTypes_) {
      if (composite.forwardDeclaration) {
        continue;
      }
      auto layout = classLayouts.find(composite.name);
      if (layout == classLayouts.end()) {
        continue;
      }
      auto parents = classParents.find(composite.name);
      if (parents != classParents.end()) {
        for (const std::string& parentName : parents->second) {
          auto parentNode = compositeNodes_.find(parentName);
          if (parentNode == compositeNodes_.end()) {
            continue;
          }
          composite.inheritances.push_back(
              DebugCompositeInheritanceInfo{nextNode_++, parentNode->second, 0});
        }
      }
      for (const std::string& fieldName : layout->second.fields) {
        auto field = fields.find(fieldName);
        if (field == fields.end() || field->second.className != composite.name) {
          continue;
        }
        const std::optional<std::size_t> typeNode =
            nodeForType(field->second.simpleType);
        if (!typeNode.has_value()) {
          continue;
        }
        const auto [fieldLine, fieldColumn] = sources.lineColumn(field->second.span);
        (void)fieldColumn;
        std::string displayName = memberNameOf(fieldName);
        const std::size_t suffix = displayName.find('$');
        if (suffix != std::string::npos) {
          displayName.erase(suffix);
        }
        composite.members.push_back(DebugCompositeMemberInfo{
            std::move(displayName), nextNode_++, *typeNode,
            fieldLine == 0 ? composite.line : fieldLine,
            typeSize(field->second.simpleType) * 8, field->second.offset * 8});
      }
    }
  }

  bool optimized_ = false;
  std::size_t nextNode_ = 0;
  std::size_t expressionNode_ = 0;
  std::size_t dwarfVersionNode_ = 0;
  std::size_t debugInfoVersionNode_ = 0;
  std::size_t producerNode_ = 0;
  std::vector<DebugFileInfo> files_;
  std::vector<DebugTypeInfo> types_;
  std::vector<DebugCompositeTypeInfo> compositeTypes_;
  std::vector<DebugFunctionInfo> functions_;
  std::vector<DebugLexicalScopeInfo> lexicalScopes_;
  std::vector<DebugLocationInfo> locations_;
  std::vector<DebugVariableInfo> variables_;
  std::unordered_map<std::string, std::size_t> typeNodes_;
  std::unordered_map<std::string, std::size_t> compositeNodes_;
  std::unordered_map<std::string, std::size_t> lexicalScopeNodes_;
  std::unordered_map<std::uint32_t, std::size_t> sourceFiles_;
  std::unordered_map<const nir::Definition*, std::size_t> subprogramNodes_;
  std::unordered_map<const nir::Instruction*, std::size_t> locationNodes_;
  std::unordered_map<const nir::Instruction*, DebugVariableBinding> variableBindings_;
  std::unordered_map<std::string, const nir::Definition*> classDefinitions_;
};

std::string nextTemporary(LoweringState& state) {
  std::string name = "tmp" + std::to_string(state.nextTemporary);
  ++state.nextTemporary;
  return name;
}

std::string nextLocalValueName(const std::string& sourceName, LoweringState& state) {
  if (state.usedLocalValueNames.insert(sourceName).second) {
    return sourceName;
  }
  std::string candidate;
  do {
    candidate = sourceName + "_" + nextTemporary(state);
  } while (!state.usedLocalValueNames.insert(candidate).second);
  return candidate;
}

std::string resolveGlobalName(const std::string& reference,
                              const LoweringState& state) {
  if (state.globals == nullptr || reference.empty()) {
    return {};
  }

  std::vector<std::string> candidates;
  if (!state.ownerName.empty()) {
    candidates.push_back(state.ownerName + "." + reference);
  }
  if (!state.moduleName.empty()) {
    candidates.push_back(state.moduleName + "." + reference);
  }
  candidates.push_back(reference);

  for (const std::string& candidate : candidates) {
    if (state.globals->contains(candidate)) {
      return candidate;
    }
  }
  return {};
}

const Signature* functionSignature(const std::string& name,
                                   const LoweringState& state) {
  if (state.functionSignatures == nullptr) {
    return nullptr;
  }
  auto found = state.functionSignatures->find(name);
  if (found == state.functionSignatures->end()) {
    return nullptr;
  }
  return &found->second;
}

const FieldInfo* fieldInfo(const std::string& name, const LoweringState& state) {
  if (state.fields == nullptr) {
    return nullptr;
  }
  auto found = state.fields->find(name);
  if (found == state.fields->end()) {
    return nullptr;
  }
  return &found->second;
}

std::string resolveMemberGlobal(const std::string& receiverGlobalName,
                                const std::string& memberName,
                                const LoweringState& state) {
  if (state.globals == nullptr) {
    return {};
  }

  const std::unordered_map<std::string, std::vector<std::string>> emptyParents;
  const auto& parents =
      state.classParents == nullptr ? emptyParents : *state.classParents;
  for (const std::string& current :
       nir::linearizedTypeNames(receiverGlobalName, parents)) {
    if (current.empty() || current == support::StdNames::JavaLangObject) {
      continue;
    }
    const std::string candidate = current + "." + memberName;
    if (state.globals->contains(candidate)) {
      return candidate;
    }
  }
  return {};
}

std::size_t virtualSlotIndex(const ClassLayout& layout, const std::string& memberName) {
  for (std::size_t i = 0; i < layout.virtualSlots.size(); ++i) {
    if (layout.virtualSlots[i].memberName == memberName) {
      return i;
    }
  }
  return layout.virtualSlots.size();
}

std::size_t dynamicToStringVirtualSlotIndex(
    const std::unordered_map<std::string, ClassLayout>& classLayouts,
    const std::unordered_map<std::string, std::vector<std::string>>& classMethods,
    const std::unordered_map<std::string, Signature>& functionSignatures) {
  bool foundToString = false;
  for (const auto& [className, methods] : classMethods) {
    (void)className;
    for (const std::string& methodName : methods) {
      if (memberNameOf(methodName) != support::StdNames::ToString) {
        continue;
      }
      foundToString = true;
      auto signature = functionSignatures.find(methodName);
      if (signature == functionSignatures.end() ||
          signature->second.parameterTypes.size() != 1 ||
          signature->second.returnType != "String") {
        return NoVirtualSlotIndex;
      }
    }
  }
  if (!foundToString) {
    return NoVirtualSlotIndex;
  }

  for (const auto& [className, layout] : classLayouts) {
    (void)className;
    const std::size_t slotIndex =
        virtualSlotIndex(layout, std::string(support::StdNames::ToString));
    if (slotIndex != layout.virtualSlots.size()) {
      return slotIndex;
    }
  }
  return NoVirtualSlotIndex;
}

std::string resolveValueGlobal(const nir::Value& value, const LoweringState& state);

std::string receiverGlobalForSelect(const nir::Value& receiver,
                                    const LoweringState& state) {
  if (receiver.kind == nir::ValueKind::New) {
    return resolveGlobalName(receiver.text, state);
  }
  if (receiver.kind == nir::ValueKind::Super) {
    return resolveGlobalName(receiver.type.empty() ? receiver.text : receiver.type,
                             state);
  }
  if (receiver.kind == nir::ValueKind::AsInstanceOf) {
    return resolveGlobalName(receiver.text.empty() ? receiver.type : receiver.text,
                             state);
  }
  if (receiver.kind == nir::ValueKind::Local) {
    const std::string local = sanitizeIdentifier(receiver.text);
    if (state.values.contains(local) || state.mutableLocalSlots.contains(local)) {
      auto simpleType = state.simpleTypes.find(local);
      if (simpleType == state.simpleTypes.end()) {
        return {};
      }
      return resolveGlobalName(simpleType->second, state);
    }
  }
  if (receiver.kind == nir::ValueKind::Call && !receiver.operands.empty()) {
    const std::string callee = resolveValueGlobal(receiver.operands.front(), state);
    if (const Signature* signature = functionSignature(callee, state)) {
      const std::string receiverType = resolveGlobalName(signature->returnType, state);
      if (!receiverType.empty()) {
        return receiverType;
      }
    }
  }
  const std::string receiverValue = resolveValueGlobal(receiver, state);
  if (const Signature* signature = functionSignature(receiverValue, state)) {
    const std::string receiverType = resolveGlobalName(signature->returnType, state);
    if (!receiverType.empty()) {
      return receiverType;
    }
  }
  if (const FieldInfo* field = fieldInfo(receiverValue, state)) {
    const std::string receiverType = resolveGlobalName(field->simpleType, state);
    if (!receiverType.empty()) {
      return receiverType;
    }
  }
  return receiverValue;
}

std::string resolveValueGlobal(const nir::Value& value, const LoweringState& state) {
  if (value.kind == nir::ValueKind::Local) {
    const std::string local = sanitizeIdentifier(value.text);
    if (state.values.contains(local) || state.mutableLocalSlots.contains(local)) {
      return {};
    }
    return resolveGlobalName(value.text, state);
  }
  if (value.kind == nir::ValueKind::New) {
    return resolveGlobalName(value.text, state);
  }
  if (value.kind == nir::ValueKind::Super) {
    return resolveGlobalName(value.type.empty() ? value.text : value.type, state);
  }
  if (value.kind == nir::ValueKind::Select && value.operands.size() == 1 &&
      !value.text.empty()) {
    if (nir::isStackSuper(value.operands.front()) && state.classParents != nullptr &&
        state.globals != nullptr) {
      const std::string owner = value.operands.front().type;
      const std::vector<std::string> order =
          nir::linearizedTypeNames(owner, *state.classParents);
      for (std::size_t i = 1; i < order.size(); ++i) {
        const std::string candidate = order[i] + "." + value.text;
        if (state.globals->contains(candidate)) {
          return candidate;
        }
      }
      return {};
    }
    const std::string receiver = receiverGlobalForSelect(value.operands.front(), state);
    if (receiver.empty()) {
      return {};
    }
    return resolveMemberGlobal(receiver, value.text, state);
  }
  return {};
}

std::string inferSimpleType(const nir::Value& value, const LoweringState& state) {
  switch (value.kind) {
  case nir::ValueKind::Unit:
    return "Unit";
  case nir::ValueKind::Literal:
    return value.type.empty() ? "Unknown" : value.type;
  case nir::ValueKind::Local: {
    const std::string local = sanitizeIdentifier(value.text);
    auto simpleType = state.simpleTypes.find(local);
    if (simpleType != state.simpleTypes.end()) {
      return simpleType->second;
    }
    auto found = state.values.find(local);
    if (found != state.values.end()) {
      return simpleTypeFromLlvm(found->second);
    }
    const std::string global = resolveValueGlobal(value, state);
    if (state.classLayouts != nullptr) {
      auto layout = state.classLayouts->find(global);
      if (layout != state.classLayouts->end() && layout->second.isModule) {
        return global;
      }
    }
    if (const Signature* signature = functionSignature(global, state)) {
      return signature->returnType;
    }
    return "Unknown";
  }
  case nir::ValueKind::Select: {
    const std::string global = resolveValueGlobal(value, state);
    if (const FieldInfo* field = fieldInfo(global, state)) {
      return field->simpleType;
    }
    if (const Signature* signature = functionSignature(global, state)) {
      return signature->returnType;
    }
    return "Unknown";
  }
  case nir::ValueKind::Call: {
    if (value.operands.empty()) {
      return "Unknown";
    }
    const std::string global = resolveValueGlobal(value.operands.front(), state);
    if (const Signature* signature = functionSignature(global, state)) {
      return signature->returnType;
    }
    return "Unknown";
  }
  case nir::ValueKind::Unary:
    if (value.text == "!") {
      return "Boolean";
    }
    if (!value.operands.empty()) {
      const std::string operandType = inferSimpleType(value.operands.front(), state);
      return operandType == "Byte" || operandType == "Short" ? "Int" : operandType;
    }
    return "Unknown";
  case nir::ValueKind::Binary:
    if (value.text == "==" || value.text == "!=" || value.text == "<" ||
        value.text == ">" || value.text == "<=" || value.text == ">=" ||
        value.text == "&&" || value.text == "||") {
      return "Boolean";
    }
    if (value.text == "+" && value.operands.size() == 2) {
      const std::string lhs = inferSimpleType(value.operands[0], state);
      const std::string rhs = inferSimpleType(value.operands[1], state);
      if (lhs == "String" || rhs == "String") {
        return "String";
      }
    }
    if (value.operands.size() == 2) {
      const std::string lhs = inferSimpleType(value.operands[0], state);
      const std::string rhs = inferSimpleType(value.operands[1], state);
      const auto numericRank = [](std::string_view type) {
        if (type == "Byte") {
          return 1;
        }
        if (type == "Short") {
          return 2;
        }
        if (type == "Int") {
          return 3;
        }
        if (type == "Long") {
          return 4;
        }
        if (type == "Float") {
          return 5;
        }
        if (type == "Double") {
          return 6;
        }
        return 0;
      };
      const int lhsRank = numericRank(lhs);
      const int rhsRank = numericRank(rhs);
      if (lhsRank != 0 && rhsRank != 0) {
        if (std::max(lhsRank, rhsRank) <= 2) {
          return "Int";
        }
        return lhsRank >= rhsRank ? lhs : rhs;
      }
      return lhs;
    }
    if (!value.operands.empty()) {
      return inferSimpleType(value.operands.front(), state);
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
  case nir::ValueKind::If:
    if (value.operands.size() == 3) {
      const std::string thenType = inferSimpleType(value.operands[1], state);
      const std::string elseType = inferSimpleType(value.operands[2], state);
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
  case nir::ValueKind::Block: {
    LoweringState blockState = state;
    std::string resultType = "Unit";
    for (const nir::Value& operand : value.operands) {
      if ((operand.kind == nir::ValueKind::LocalLet ||
           operand.kind == nir::ValueKind::LocalVar) &&
          operand.operands.size() == 1) {
        const std::string initializerType =
            inferSimpleType(operand.operands.front(), blockState);
        const std::string localType =
            operand.type.empty() ? initializerType : operand.type;
        const std::string local = sanitizeIdentifier(operand.text);
        blockState.values[local] = llvmType(localType);
        blockState.simpleTypes[local] = localType;
        if (operand.kind == nir::ValueKind::LocalVar) {
          blockState.mutableLocalSlots[local] = local + "_slot";
        }
        resultType = "Unit";
      } else {
        resultType = inferSimpleType(operand, blockState);
      }
    }
    return resultType;
  }
  case nir::ValueKind::LocalLet:
  case nir::ValueKind::LocalVar:
    return "Unit";
  case nir::ValueKind::New:
    return value.type.empty() ? value.text : value.type;
  case nir::ValueKind::SizeOf:
    return "Int";
  case nir::ValueKind::ZoneScoped:
    if (value.operands.size() == 1) {
      return inferSimpleType(value.operands.front(), state);
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
    return value.type.empty() ? value.text : value.type;
  case nir::ValueKind::Unknown:
    return value.type.empty() ? "Unknown" : value.type;
  }
  return "Unknown";
}

void buildVirtualLayoutForClass(
    const std::string& className,
    const std::unordered_map<std::string, std::vector<std::string>>& classMethods,
    const std::unordered_map<std::string, std::vector<std::string>>& classParents,
    const std::vector<std::string>& virtualMemberNames,
    const std::unordered_set<std::string>& concreteMethods,
    const std::vector<StackSuperSite>& stackSuperSites,
    std::unordered_map<std::string, ClassLayout>& classLayouts,
    std::unordered_set<std::string>& visiting, std::unordered_set<std::string>& built) {
  if (built.contains(className)) {
    return;
  }
  auto layout = classLayouts.find(className);
  if (layout == classLayouts.end()) {
    return;
  }
  (void)visiting;
  if (layout->second.virtualSlots.empty()) {
    for (const std::string& memberName : virtualMemberNames) {
      layout->second.virtualSlots.push_back(VirtualSlot{memberName, {}});
    }
  }

  for (const std::string& owner : nir::linearizedTypeNames(className, classParents)) {
    auto methods = classMethods.find(owner);
    if (methods == classMethods.end()) {
      continue;
    }
    for (const std::string& methodName : methods->second) {
      const std::string memberName = memberNameOf(methodName);
      if (memberName.empty() || memberName == support::StdNames::Constructor ||
          isHiddenTraitValueAccessor(memberName)) {
        continue;
      }
      const std::size_t slot = virtualSlotIndex(layout->second, memberName);
      if (concreteMethods.contains(methodName) &&
          slot != layout->second.virtualSlots.size() &&
          layout->second.virtualSlots[slot].implementationName.empty()) {
        layout->second.virtualSlots[slot].implementationName = methodName;
      }
    }
  }

  const std::vector<std::string> linearization =
      nir::linearizedTypeNames(className, classParents);
  for (const StackSuperSite& site : stackSuperSites) {
    auto owner = std::find(linearization.begin(), linearization.end(), site.ownerName);
    if (owner == linearization.end()) {
      continue;
    }
    const std::size_t slot = virtualSlotIndex(
        layout->second, stackSuperSlotName(site.ownerName, site.memberName));
    if (slot == layout->second.virtualSlots.size()) {
      continue;
    }
    for (auto next = std::next(owner); next != linearization.end(); ++next) {
      const std::string candidate = *next + "." + site.memberName;
      if (concreteMethods.contains(candidate)) {
        layout->second.virtualSlots[slot].implementationName = candidate;
        break;
      }
    }
  }

  auto parents = classParents.find(className);
  const bool hasParent = parents != classParents.end() && !parents->second.empty();
  layout->second.hasHeader =
      !layout->second.isTrait || hasParent || layout->second.hasChildren;
  if (layout->second.hasHeader) {
    layout->second.size = ObjectHeaderSize;
    if (std::any_of(
            layout->second.virtualSlots.begin(), layout->second.virtualSlots.end(),
            [](const VirtualSlot& slot) { return !slot.implementationName.empty(); })) {
      layout->second.vtableName = "__vtable_" + sanitizeIdentifier(className);
    }
  }

  built.insert(className);
}

void buildVirtualLayouts(
    const std::unordered_map<std::string, std::vector<std::string>>& classMethods,
    const std::unordered_map<std::string, std::vector<std::string>>& classParents,
    const std::unordered_set<std::string>& concreteMethods,
    const std::vector<StackSuperSite>& stackSuperSites,
    std::unordered_map<std::string, ClassLayout>& classLayouts) {
  std::vector<std::string> virtualMemberNames;
  std::unordered_set<std::string> seenMembers;
  for (const auto& [className, methods] : classMethods) {
    (void)className;
    for (const std::string& methodName : methods) {
      const std::string memberName = memberNameOf(methodName);
      if (!memberName.empty() && memberName != support::StdNames::Constructor &&
          !isHiddenTraitValueAccessor(memberName) &&
          seenMembers.insert(memberName).second) {
        virtualMemberNames.push_back(memberName);
      }
    }
  }
  for (const StackSuperSite& site : stackSuperSites) {
    const std::string slotName = stackSuperSlotName(site.ownerName, site.memberName);
    if (seenMembers.insert(slotName).second) {
      virtualMemberNames.push_back(slotName);
    }
  }
  std::sort(virtualMemberNames.begin(), virtualMemberNames.end());

  for (const auto& [className, parentNames] : classParents) {
    (void)className;
    for (const std::string& parentName : parentNames) {
      auto parent = classLayouts.find(parentName);
      if (parent != classLayouts.end()) {
        parent->second.hasChildren = true;
      }
    }
  }

  std::unordered_set<std::string> visiting;
  std::unordered_set<std::string> built;
  std::vector<std::string> classNames;
  classNames.reserve(classLayouts.size());
  for (const auto& [className, layout] : classLayouts) {
    (void)layout;
    classNames.push_back(className);
  }
  std::sort(classNames.begin(), classNames.end());
  for (const std::string& className : classNames) {
    buildVirtualLayoutForClass(className, classMethods, classParents,
                               virtualMemberNames, concreteMethods, stackSuperSites,
                               classLayouts, visiting, built);
  }

  std::uint32_t nextTypeId = FirstClassLikeTypeId;
  for (const std::string& className : classNames) {
    ClassLayout& layout = classLayouts.at(className);
    layout.typeId = nextTypeId++;
    layout.descriptorName = classTypeDescriptorName(className);
  }
}

void buildFieldLayoutForClass(
    const std::string& className,
    const std::unordered_map<std::string, std::vector<const nir::Definition*>>&
        classFieldDefinitions,
    const std::unordered_map<std::string, std::vector<std::string>>& classParents,
    std::unordered_map<std::string, ClassLayout>& classLayouts,
    std::unordered_map<std::string, FieldInfo>& fields,
    std::unordered_set<std::string>& visiting, std::unordered_set<std::string>& built) {
  if (built.contains(className)) {
    return;
  }
  auto layout = classLayouts.find(className);
  if (layout == classLayouts.end() || !visiting.insert(className).second) {
    return;
  }

  auto parents = classParents.find(className);
  if (parents != classParents.end()) {
    for (const std::string& parentName : parents->second) {
      auto parentLayout = classLayouts.find(parentName);
      if (parentLayout == classLayouts.end() || parentLayout->second.isTrait) {
        continue;
      }
      buildFieldLayoutForClass(parentName, classFieldDefinitions, classParents,
                               classLayouts, fields, visiting, built);
      layout = classLayouts.find(className);
      parentLayout = classLayouts.find(parentName);
      if (parentLayout != classLayouts.end()) {
        layout->second.fields = parentLayout->second.fields;
        layout->second.traceOffsets = parentLayout->second.traceOffsets;
        layout->second.size = std::max(layout->second.size, parentLayout->second.size);
      }
      break;
    }
  }

  auto ownFields = classFieldDefinitions.find(className);
  if (ownFields != classFieldDefinitions.end()) {
    for (const nir::Definition* definition : ownFields->second) {
      if (definition == nullptr) {
        continue;
      }
      const std::string simpleType =
          definition->signature.empty() ? "Unknown" : definition->signature;
      const std::size_t offset = alignTo(layout->second.size, typeAlign(simpleType));
      layout->second.size = offset + typeSize(simpleType);
      layout->second.fields.push_back(definition->name);
      if (isReferenceType(simpleType)) {
        layout->second.traceOffsets.push_back(offset);
      }
      const bool constructorParameter = definition->body.empty();
      if (constructorParameter) {
        layout->second.constructorFields.push_back(definition->name);
      }
      fields[definition->name] =
          FieldInfo{className,        simpleType,           offset,
                    definition->body, constructorParameter, definition->span};
    }
  }

  visiting.erase(className);
  built.insert(className);
}

void buildFieldLayouts(
    const std::unordered_map<std::string, std::vector<const nir::Definition*>>&
        classFieldDefinitions,
    const std::unordered_map<std::string, std::vector<std::string>>& classParents,
    std::unordered_map<std::string, ClassLayout>& classLayouts,
    std::unordered_map<std::string, FieldInfo>& fields) {
  std::unordered_set<std::string> visiting;
  std::unordered_set<std::string> built;
  std::vector<std::string> classNames;
  classNames.reserve(classLayouts.size());
  for (const auto& [className, layout] : classLayouts) {
    (void)layout;
    classNames.push_back(className);
  }
  std::sort(classNames.begin(), classNames.end());
  for (const std::string& className : classNames) {
    buildFieldLayoutForClass(className, classFieldDefinitions, classParents,
                             classLayouts, fields, visiting, built);
  }
}

void emitVtables(const std::unordered_map<std::string, ClassLayout>& classLayouts,
                 std::ostringstream& out) {
  std::vector<std::string> classNames;
  classNames.reserve(classLayouts.size());
  for (const auto& [className, layout] : classLayouts) {
    (void)layout;
    classNames.push_back(className);
  }
  std::sort(classNames.begin(), classNames.end());

  bool emittedAny = false;
  for (const std::string& className : classNames) {
    const ClassLayout& layout = classLayouts.at(className);
    if (layout.isTrait || !layout.hasHeader || layout.virtualSlots.empty() ||
        layout.vtableName.empty()) {
      continue;
    }
    out << '@' << layout.vtableName << " = private unnamed_addr constant ["
        << layout.virtualSlots.size() << " x ptr] [";
    for (std::size_t i = 0; i < layout.virtualSlots.size(); ++i) {
      if (i != 0) {
        out << ", ";
      }
      if (layout.virtualSlots[i].implementationName.empty()) {
        out << "ptr null";
      } else {
        out << "ptr @" << sanitizeIdentifier(layout.virtualSlots[i].implementationName);
      }
    }
    out << "]\n";
    emittedAny = true;
  }
  if (emittedAny) {
    out << '\n';
  }
}

std::string lowerValue(const nir::Value& value, const std::string& expectedType,
                       LoweringState& state, std::ostringstream& out, bool& supported);
std::string lowerValueUnrooted(const nir::Value& value, const std::string& expectedType,
                               LoweringState& state, std::ostringstream& out,
                               bool& supported);
void emitShadowPop(LoweringState& state, std::ostringstream& out);
std::string lowerStringOperand(const nir::Value& value, LoweringState& state,
                               std::ostringstream& out, bool& supported);
void emitLet(const nir::Instruction& instruction, LoweringState& state,
             std::ostringstream& out);
void emitVar(const nir::Instruction& instruction, LoweringState& state,
             std::ostringstream& out);
void emitEval(const nir::Instruction& instruction, LoweringState& state,
              std::ostringstream& out);
std::string lowerGlobalCall(const std::string& target,
                            const std::vector<std::string>& loweredArguments,
                            const std::string& expectedType, LoweringState& state,
                            std::ostringstream& out, bool& supported);

std::string requireNonNullReceiver(const std::string& receiver, LoweringState& state,
                                   std::ostringstream& out) {
  const std::string checked = nextTemporary(state);
  out << "  %" << checked << " = call ptr @__scalanative_require_non_null_receiver(ptr "
      << receiver << ")\n";
  state.values[checked] = "ptr";
  state.simpleTypes[checked] = "Object";
  return "%" + checked;
}

std::string requireNonNullThrownException(const std::string& exception,
                                          LoweringState& state,
                                          std::ostringstream& out) {
  const std::string checked = nextTemporary(state);
  out << "  %" << checked
      << " = call ptr @__scalanative_require_non_null_thrown_exception(ptr "
      << exception << ")\n";
  state.values[checked] = "ptr";
  state.simpleTypes[checked] = std::string(support::StdNames::JavaLangThrowable);
  return "%" + checked;
}

nir::Value substituteThis(const nir::Value& value, const std::string& thisLocal) {
  nir::Value substituted = value;
  if (substituted.kind == nir::ValueKind::Local && substituted.text == "this") {
    substituted.text = thisLocal;
  }
  for (nir::Value& operand : substituted.operands) {
    operand = substituteThis(operand, thisLocal);
  }
  return substituted;
}

nir::Instruction substituteThis(const nir::Instruction& instruction,
                                const std::string& thisLocal) {
  nir::Instruction substituted = instruction;
  substituted.value = substituteThis(substituted.value, thisLocal);
  return substituted;
}

bool storeFieldValue(const FieldInfo& field, const std::string& objectPointer,
                     const nir::Value& source, LoweringState& state,
                     std::ostringstream& out,
                     const support::SourceSpan* receiverSpan = nullptr) {
  bool valueSupported = false;
  const std::string loweredValue =
      lowerValue(source, field.simpleType, state, out, valueSupported);
  if (!valueSupported) {
    return false;
  }

  std::string checkedObject = objectPointer;
  if (receiverSpan != nullptr) {
    emitSourceLocation(*receiverSpan, state, out);
    checkedObject = requireNonNullReceiver(objectPointer, state, out);
  }

  const std::string pointer = nextTemporary(state);
  out << "  %" << pointer << " = getelementptr i8, ptr " << checkedObject << ", i64 "
      << field.offset << "\n";
  state.values[pointer] = "ptr";
  state.simpleTypes[pointer] = "Object";
  out << "  store " << llvmType(field.simpleType) << ' ' << loweredValue << ", ptr %"
      << pointer << "\n";
  return true;
}

bool initializeFieldFromBody(const FieldInfo& field, const std::string& objectPointer,
                             LoweringState& state, std::ostringstream& out) {
  const nir::Instruction* terminator = field.initializer.terminator();
  if (terminator == nullptr || terminator->kind != nir::InstructionKind::Return) {
    return false;
  }

  std::unordered_map<std::string, std::string> savedValues = state.values;
  std::unordered_map<std::string, std::string> savedSimpleTypes = state.simpleTypes;

  const std::string thisLocal = nextTemporary(state);
  out << "  %" << thisLocal << " = getelementptr i8, ptr " << objectPointer
      << ", i64 0\n";
  state.values[thisLocal] = "ptr";
  state.simpleTypes[thisLocal] = field.className;

  for (const nir::Instruction& instruction : field.initializer.instructions) {
    switch (instruction.kind) {
    case nir::InstructionKind::Param:
      break;
    case nir::InstructionKind::Let:
      emitLet(substituteThis(instruction, thisLocal), state, out);
      break;
    case nir::InstructionKind::Var:
      emitVar(substituteThis(instruction, thisLocal), state, out);
      break;
    case nir::InstructionKind::Eval:
      emitEval(substituteThis(instruction, thisLocal), state, out);
      break;
    case nir::InstructionKind::Return: {
      const bool stored =
          storeFieldValue(field, objectPointer,
                          substituteThis(instruction.value, thisLocal), state, out);
      state.values = std::move(savedValues);
      state.simpleTypes = std::move(savedSimpleTypes);
      return stored;
    }
    case nir::InstructionKind::Throw:
      state.values = std::move(savedValues);
      state.simpleTypes = std::move(savedSimpleTypes);
      return false;
    case nir::InstructionKind::Unreachable:
      state.values = std::move(savedValues);
      state.simpleTypes = std::move(savedSimpleTypes);
      return false;
    }
  }

  state.values = std::move(savedValues);
  state.simpleTypes = std::move(savedSimpleTypes);
  return false;
}

bool initializeFieldBodies(const ClassLayout& layout, const std::string& objectPointer,
                           const std::string& className, bool inheritedOnly,
                           LoweringState& state, std::ostringstream& out) {
  if (state.fields == nullptr) {
    return false;
  }
  for (const std::string& fieldName : layout.fields) {
    auto field = state.fields->find(fieldName);
    if (field == state.fields->end()) {
      return false;
    }
    const bool inherited = field->second.className != className;
    if (inherited != inheritedOnly || field->second.constructorParameter ||
        field->second.initializer.empty()) {
      continue;
    }
    if (!initializeFieldFromBody(field->second, objectPointer, state, out)) {
      return false;
    }
  }
  return true;
}

std::string lowerZeroArgGlobalCall(const std::string& target,
                                   const std::string& expectedType,
                                   LoweringState& state, std::ostringstream& out,
                                   bool& supported) {
  const Signature* signature = functionSignature(target, state);
  if (signature == nullptr || !signature->parameterTypes.empty()) {
    supported = false;
    return defaultValue(expectedType);
  }

  const std::string returnType = llvmType(signature->returnType);
  if (returnType == "void") {
    out << "  call void @" << sanitizeIdentifier(target) << "()\n";
    supported = true;
    return {};
  }

  const std::string temporary = nextTemporary(state);
  out << "  %" << temporary << " = call " << returnType << " @"
      << sanitizeIdentifier(target) << "()\n";
  state.values[temporary] = returnType;
  state.simpleTypes[temporary] = signature->returnType;
  supported = true;
  return "%" + temporary;
}

std::string lowerLiteral(const nir::Value& value, const std::string& expectedType,
                         const LoweringState& state, bool& supported) {
  const std::string lowered = llvmType(expectedType);
  if ((lowered == "i8" || lowered == "i16" || lowered == "i32" || lowered == "i64") &&
      isIntegerLiteral(value.text)) {
    supported = true;
    return value.text;
  }
  if (lowered == "i1" && (value.text == "true" || value.text == "false")) {
    supported = true;
    return value.text == "true" ? "1" : "0";
  }
  if (expectedType == "Long") {
    const std::string literal = numericLiteralWithoutSuffix(value.text);
    if (isIntegerLiteral(literal)) {
      supported = true;
      return literal;
    }
  }
  if (expectedType == "Float" || expectedType == "Double") {
    const std::string literal = normalizedFloatingLiteral(value.text, expectedType);
    if (!literal.empty()) {
      supported = true;
      return literal;
    }
  }
  if (expectedType == "Char") {
    const int decoded = decodedCharLiteral(value.text);
    if (decoded >= 0) {
      supported = true;
      return std::to_string(decoded);
    }
  }
  if (lowered == "ptr" && value.text == "null") {
    supported = true;
    return "null";
  }
  if (expectedType == "Symbol" && value.type == "Symbol" &&
      state.stringConstants != nullptr) {
    auto found = state.stringConstants->find(value.text);
    if (found != state.stringConstants->end()) {
      supported = true;
      return stringPointerExpression(found->second);
    }
  }
  if (lowered == "ptr" && value.type == "String" && state.stringConstants != nullptr) {
    auto found = state.stringConstants->find(value.text);
    if (found != state.stringConstants->end()) {
      supported = true;
      return stringPointerExpression(found->second);
    }
  }
  supported = false;
  return defaultValue(expectedType);
}

std::string lowerNew(const nir::Value& value, LoweringState& state,
                     std::ostringstream& out, bool& supported) {
  if (isStringArrayType(value.text) || isByteArrayType(value.text) ||
      isShortArrayType(value.text) || isIntArrayType(value.text) ||
      isBooleanArrayType(value.text) || isLongArrayType(value.text) ||
      isDoubleArrayType(value.text) || isFloatArrayType(value.text) ||
      isCharArrayType(value.text) || isReferenceArrayType(value.text)) {
    const bool stringElements = isStringArrayType(value.text);
    const bool byteElements = isByteArrayType(value.text);
    const bool shortElements = isShortArrayType(value.text);
    const bool intElements = isIntArrayType(value.text);
    const bool booleanElements = isBooleanArrayType(value.text);
    const bool longElements = isLongArrayType(value.text);
    const bool doubleElements = isDoubleArrayType(value.text);
    const bool floatElements = isFloatArrayType(value.text);
    const bool charElements = isCharArrayType(value.text);
    const std::string elementType = stringElements    ? "String"
                                    : byteElements    ? "Byte"
                                    : shortElements   ? "Short"
                                    : intElements     ? "Int"
                                    : booleanElements ? "Boolean"
                                    : longElements    ? "Long"
                                    : doubleElements  ? "Double"
                                    : floatElements   ? "Float"
                                    : charElements    ? "Char"
                                                   : arrayElementTypeName(value.text);
    const std::size_t elementSize = stringElements    ? 8
                                    : byteElements    ? 1
                                    : shortElements   ? 2
                                    : intElements     ? 4
                                    : booleanElements ? 1
                                    : floatElements   ? 4
                                    : charElements    ? 4
                                                      : 8;
    const char* elementLlvmType = stringElements    ? "ptr"
                                  : byteElements    ? "i8"
                                  : shortElements   ? "i16"
                                  : intElements     ? "i32"
                                  : booleanElements ? "i1"
                                  : longElements    ? "i64"
                                  : doubleElements  ? "double"
                                  : floatElements   ? "float"
                                  : charElements    ? "i32"
                                                    : "ptr";
    const std::size_t allocationSize =
        ObjectHeaderSize + 8 + value.operands.size() * elementSize;
    const std::string array = nextTemporary(state);
    out << "  %" << array << " = call ptr @__scalanative_program_arena_alloc(i64 "
        << allocationSize << ", ptr null)\n";
    state.values[array] = "ptr";
    state.simpleTypes[array] = "Array [ " + elementType + " ]";

    const std::string lengthSlot = nextTemporary(state);
    out << "  %" << lengthSlot << " = getelementptr i8, ptr %" << array << ", i64 "
        << ObjectHeaderSize << "\n";
    state.values[lengthSlot] = "ptr";
    state.simpleTypes[lengthSlot] = "Object";
    out << "  store i64 " << value.operands.size() << ", ptr %" << lengthSlot << "\n";

    for (std::size_t index = 0; index < value.operands.size(); ++index) {
      bool elementSupported = false;
      const std::string element =
          lowerValue(value.operands[index], elementType, state, out, elementSupported);
      if (!elementSupported) {
        supported = false;
        return "null";
      }
      const std::string elementSlot = nextTemporary(state);
      out << "  %" << elementSlot << " = getelementptr i8, ptr %" << array << ", i64 "
          << ObjectHeaderSize + 8 + index * elementSize << "\n";
      state.values[elementSlot] = "ptr";
      state.simpleTypes[elementSlot] = "Object";
      out << "  store " << elementLlvmType << ' ' << element << ", ptr %" << elementSlot
          << "\n";
    }
    supported = true;
    return "%" + array;
  }

  const std::string className = resolveGlobalName(value.text, state);
  if (className.empty()) {
    supported = false;
    return "null";
  }

  const ClassLayout* layout = nullptr;
  if (state.classLayouts != nullptr) {
    auto found = state.classLayouts->find(className);
    if (found != state.classLayouts->end()) {
      layout = &found->second;
    }
  }
  const std::size_t allocationSize =
      layout == nullptr || layout->size == 0 ? 1 : layout->size;
  bool gcOwnedThrowable = className == support::StdNames::JavaLangThrowable;
  if (!gcOwnedThrowable && state.classParents != nullptr) {
    const std::vector<std::string> ancestors =
        nir::linearizedTypeNames(className, *state.classParents);
    gcOwnedThrowable =
        std::find(ancestors.begin(), ancestors.end(),
                  support::StdNames::JavaLangThrowable) != ancestors.end();
  }

  const std::string temporary = nextTemporary(state);
  if (layout == nullptr || !layout->hasHeader) {
    out << "  %" << temporary << " = call ptr @malloc(i64 " << allocationSize << ")\n";
  } else {
    out << "  %" << temporary << " = call ptr @__scalanative_"
        << (gcOwnedThrowable ? "gc_object_alloc" : "object_alloc") << "(i64 "
        << allocationSize << ", ptr @" << layout->descriptorName << ")\n";
  }
  state.values[temporary] = "ptr";
  state.simpleTypes[temporary] = className;

  // Constructor arguments and initializer bodies can allocate, so the object
  // being initialized must already be visible to a collection safepoint.
  if (layout != nullptr && layout->hasHeader && state.hasShadowFrame) {
    if (state.nextTemporaryRootSlot >= state.temporaryRootSlots.size()) {
      supported = false;
      return "null";
    }
    const std::string& rootSlot =
        state.temporaryRootSlots[state.nextTemporaryRootSlot++];
    out << "  store ptr %" << temporary << ", ptr %" << rootSlot << "\n";
  }

  if (layout == nullptr) {
    supported = value.operands.empty();
    return supported ? "%" + temporary : "null";
  }

  if (value.operands.size() != layout->constructorFields.size()) {
    supported = false;
    return "null";
  }

  for (std::size_t i = 0; i < value.operands.size(); ++i) {
    if (state.fields == nullptr) {
      supported = false;
      return "null";
    }
    auto field = state.fields->find(layout->constructorFields[i]);
    if (field == state.fields->end()) {
      supported = false;
      return "null";
    }

    if (!storeFieldValue(field->second, "%" + temporary, value.operands[i], state,
                         out)) {
      supported = false;
      return "null";
    }
  }

  const std::string initializerName = className + ".$init";
  const Signature* initializer = functionSignature(initializerName, state);
  if (initializer == nullptr) {
    if (!initializeFieldBodies(*layout, "%" + temporary, className, false, state,
                               out)) {
      supported = false;
      return "null";
    }
  } else {
    if (initializer->parameterTypes.size() != 1 ||
        initializer->parameterTypes.front() != className ||
        llvmType(initializer->returnType) != "void") {
      supported = false;
      return "null";
    }
    bool initializerSupported = false;
    (void)lowerGlobalCall(initializerName, {"ptr %" + temporary}, "Unit", state, out,
                          initializerSupported);
    if (!initializerSupported) {
      supported = false;
      return "null";
    }
  }

  if (gcOwnedThrowable && layout->hasHeader) {
    emitSourceLocation(value.span, state, out);
    out << "  call void @__scalanative_capture_exception_trace(ptr %" << temporary
        << ")\n";
  }

  supported = true;
  return "%" + temporary;
}

std::string lowerSizeOf(const nir::Value& value, LoweringState& state,
                        bool& supported) {
  if (value.text == "Unit") {
    supported = true;
    return "0";
  }
  if (value.text == "Boolean") {
    supported = true;
    return "1";
  }
  if (value.text == "Byte") {
    supported = true;
    return "1";
  }
  if (value.text == "Short") {
    supported = true;
    return "2";
  }
  if (value.text == "Int" || value.text == "Float" || value.text == "Char") {
    supported = true;
    return "4";
  }
  if (value.text == "Long" || value.text == "Double") {
    supported = true;
    return "8";
  }
  if (state.classLayouts != nullptr) {
    const std::string className = resolveGlobalName(value.text, state);
    auto layout = state.classLayouts->find(className);
    if (layout != state.classLayouts->end() && !layout->second.isTrait &&
        !layout->second.isModule && layout->second.size != 0) {
      supported = true;
      return std::to_string(layout->second.size);
    }
  }
  supported = false;
  return "0";
}

std::string lowerGlobalCall(const std::string& target,
                            const std::vector<std::string>& loweredArguments,
                            const std::string& expectedType, LoweringState& state,
                            std::ostringstream& out, bool& supported) {
  const Signature* signature = functionSignature(target, state);
  if (signature == nullptr) {
    supported = false;
    return defaultValue(expectedType);
  }

  const std::string returnType = llvmType(signature->returnType);
  if (returnType == "void") {
    out << "  call void @" << sanitizeIdentifier(target) << '(';
    for (std::size_t i = 0; i < loweredArguments.size(); ++i) {
      if (i != 0) {
        out << ", ";
      }
      out << loweredArguments[i];
    }
    out << ")\n";
    supported = true;
    return {};
  }

  const std::string temporary = nextTemporary(state);
  out << "  %" << temporary << " = call " << returnType << " @"
      << sanitizeIdentifier(target) << '(';
  for (std::size_t i = 0; i < loweredArguments.size(); ++i) {
    if (i != 0) {
      out << ", ";
    }
    out << loweredArguments[i];
  }
  out << ")\n";
  state.values[temporary] = returnType;
  state.simpleTypes[temporary] = signature->returnType;
  supported = true;
  return "%" + temporary;
}

const ClassLayout* virtualLayoutForTarget(const std::string& target,
                                          const LoweringState& state,
                                          std::size_t& slotIndex) {
  if (state.classLayouts == nullptr) {
    return nullptr;
  }
  const std::string ownerName = ownerNameOf(target);
  const std::string memberName = memberNameOf(target);
  auto layout = state.classLayouts->find(ownerName);
  if (layout == state.classLayouts->end() || !layout->second.hasHeader) {
    return nullptr;
  }
  slotIndex = virtualSlotIndex(layout->second, memberName);
  if (slotIndex == layout->second.virtualSlots.size()) {
    return nullptr;
  }
  return &layout->second;
}

bool receiverIsExactForTarget(const nir::Value& receiver,
                              const std::string& targetOwner,
                              const LoweringState& state) {
  if (receiver.kind == nir::ValueKind::New) {
    return true;
  }
  if (receiver.kind != nir::ValueKind::Local) {
    return false;
  }

  const std::string local = sanitizeIdentifier(receiver.text);
  auto simpleType = state.simpleTypes.find(local);
  if (state.exactLocals.contains(local)) {
    return true;
  }
  if (simpleType == state.simpleTypes.end() || state.classLayouts == nullptr) {
    return false;
  }

  auto staticLayout = state.classLayouts->find(simpleType->second);
  if (staticLayout != state.classLayouts->end() && !staticLayout->second.hasChildren) {
    return true;
  }

  if (simpleType->second != targetOwner) {
    return false;
  }
  auto targetLayout = state.classLayouts->find(targetOwner);
  return targetLayout != state.classLayouts->end() && !targetLayout->second.hasChildren;
}

bool shouldUseVirtualDispatch(const nir::Value& callee, const std::string& target,
                              const LoweringState& state) {
  if (callee.kind != nir::ValueKind::Select || callee.operands.size() != 1) {
    return false;
  }
  if (callee.operands.front().kind == nir::ValueKind::Super) {
    return false;
  }
  std::size_t slotIndex = 0;
  const ClassLayout* layout = virtualLayoutForTarget(target, state, slotIndex);
  if (layout == nullptr) {
    return false;
  }
  (void)slotIndex;
  return !receiverIsExactForTarget(callee.operands.front(), ownerNameOf(target), state);
}

std::string loadObjectVtable(const std::string& receiver, LoweringState& state,
                             std::ostringstream& out) {
  const std::string descriptor = nextTemporary(state);
  out << "  %" << descriptor << " = call ptr @__scalanative_object_descriptor(ptr "
      << receiver << ")\n";
  state.values[descriptor] = "ptr";
  state.simpleTypes[descriptor] = "Object";

  const std::string vtableField = nextTemporary(state);
  out << "  %" << vtableField << " = getelementptr %scalanative.type_descriptor, ptr %"
      << descriptor << ", i32 0, i32 " << TypeDescriptorVtableIndex << "\n";
  state.values[vtableField] = "ptr";
  state.simpleTypes[vtableField] = "Object";

  const std::string vtable = nextTemporary(state);
  out << "  %" << vtable << " = load ptr, ptr %" << vtableField << "\n";
  state.values[vtable] = "ptr";
  state.simpleTypes[vtable] = "Object";
  return vtable;
}

std::string lowerVirtualDispatch(const nir::Value& callee,
                                 const std::vector<nir::Value>& arguments,
                                 const std::string& target, const Signature& signature,
                                 const std::string& expectedType, LoweringState& state,
                                 std::ostringstream& out, bool& supported) {
  if (callee.kind != nir::ValueKind::Select || callee.operands.size() != 1 ||
      signature.parameterTypes.empty()) {
    supported = false;
    return defaultValue(expectedType);
  }

  std::size_t slotIndex = 0;
  const ClassLayout* layout = virtualLayoutForTarget(target, state, slotIndex);
  if (layout == nullptr) {
    supported = false;
    return defaultValue(expectedType);
  }

  std::vector<std::string> loweredArguments;
  bool receiverSupported = false;
  const std::string receiver =
      lowerValue(callee.operands.front(), signature.parameterTypes.front(), state, out,
                 receiverSupported);
  if (!receiverSupported) {
    supported = false;
    return defaultValue(expectedType);
  }
  loweredArguments.push_back(llvmType(signature.parameterTypes.front()) + " " +
                             receiver);

  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const std::size_t parameterIndex = i + 1;
    if (parameterIndex >= signature.parameterTypes.size()) {
      supported = false;
      return defaultValue(expectedType);
    }
    bool argumentSupported = false;
    const std::string loweredArgument =
        lowerValue(arguments[i], signature.parameterTypes[parameterIndex], state, out,
                   argumentSupported);
    if (!argumentSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    loweredArguments.push_back(llvmType(signature.parameterTypes[parameterIndex]) +
                               " " + loweredArgument);
  }

  if (loweredArguments.size() != signature.parameterTypes.size()) {
    supported = false;
    return defaultValue(expectedType);
  }

  emitSourceLocation(callee.span, state, out);
  const std::string checkedReceiver = requireNonNullReceiver(receiver, state, out);
  loweredArguments.front() =
      llvmType(signature.parameterTypes.front()) + " " + checkedReceiver;
  const std::string vtable = loadObjectVtable(checkedReceiver, state, out);

  const std::string slotPointer = nextTemporary(state);
  out << "  %" << slotPointer << " = getelementptr [" << layout->virtualSlots.size()
      << " x ptr], ptr %" << vtable << ", i64 0, i64 " << slotIndex << "\n";
  state.values[slotPointer] = "ptr";
  state.simpleTypes[slotPointer] = "Object";

  const std::string functionPointer = nextTemporary(state);
  out << "  %" << functionPointer << " = load ptr, ptr %" << slotPointer << "\n";
  state.values[functionPointer] = "ptr";
  state.simpleTypes[functionPointer] = "Object";

  const std::string returnType = llvmType(signature.returnType);
  if (returnType == "void") {
    out << "  call void %" << functionPointer << '(';
    for (std::size_t i = 0; i < loweredArguments.size(); ++i) {
      if (i != 0) {
        out << ", ";
      }
      out << loweredArguments[i];
    }
    out << ")\n";
    supported = true;
    return {};
  }

  const std::string temporary = nextTemporary(state);
  out << "  %" << temporary << " = call " << returnType << " %" << functionPointer
      << '(';
  for (std::size_t i = 0; i < loweredArguments.size(); ++i) {
    if (i != 0) {
      out << ", ";
    }
    out << loweredArguments[i];
  }
  out << ")\n";
  state.values[temporary] = returnType;
  state.simpleTypes[temporary] = signature.returnType;
  supported = true;
  return "%" + temporary;
}

std::string lowerStackSuperDispatch(const nir::Value& callee,
                                    const std::vector<nir::Value>& arguments,
                                    const Signature& signature,
                                    const std::string& expectedType,
                                    LoweringState& state, std::ostringstream& out,
                                    bool& supported) {
  if (!isStackSuperSelect(callee) || signature.parameterTypes.empty() ||
      state.classLayouts == nullptr) {
    supported = false;
    return defaultValue(expectedType);
  }

  const nir::Value& stackSuper = callee.operands.front();
  auto layout = state.classLayouts->find(stackSuper.type);
  if (layout == state.classLayouts->end()) {
    supported = false;
    return defaultValue(expectedType);
  }
  const std::size_t slotIndex = virtualSlotIndex(
      layout->second, stackSuperSlotName(stackSuper.type, callee.text));
  if (slotIndex == layout->second.virtualSlots.size()) {
    supported = false;
    return defaultValue(expectedType);
  }

  std::vector<std::string> loweredArguments;
  bool receiverSupported = false;
  const std::string receiver = lowerValue(stackSuper, signature.parameterTypes.front(),
                                          state, out, receiverSupported);
  if (!receiverSupported) {
    supported = false;
    return defaultValue(expectedType);
  }
  loweredArguments.push_back(llvmType(signature.parameterTypes.front()) + " " +
                             receiver);

  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const std::size_t parameterIndex = i + 1;
    if (parameterIndex >= signature.parameterTypes.size()) {
      supported = false;
      return defaultValue(expectedType);
    }
    bool argumentSupported = false;
    const std::string lowered =
        lowerValue(arguments[i], signature.parameterTypes[parameterIndex], state, out,
                   argumentSupported);
    if (!argumentSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    loweredArguments.push_back(llvmType(signature.parameterTypes[parameterIndex]) +
                               " " + lowered);
  }
  if (loweredArguments.size() != signature.parameterTypes.size()) {
    supported = false;
    return defaultValue(expectedType);
  }

  const std::string vtable = loadObjectVtable(receiver, state, out);

  const std::string slotPointer = nextTemporary(state);
  out << "  %" << slotPointer << " = getelementptr ["
      << layout->second.virtualSlots.size() << " x ptr], ptr %" << vtable
      << ", i64 0, i64 " << slotIndex << "\n";
  state.values[slotPointer] = "ptr";
  state.simpleTypes[slotPointer] = "Object";

  const std::string functionPointer = nextTemporary(state);
  out << "  %" << functionPointer << " = load ptr, ptr %" << slotPointer << "\n";
  state.values[functionPointer] = "ptr";
  state.simpleTypes[functionPointer] = "Object";
  emitSourceLocation(callee.span, state, out);

  const std::string returnType = llvmType(signature.returnType);
  if (returnType == "void") {
    out << "  call void %" << functionPointer << '(';
  } else {
    const std::string temporary = nextTemporary(state);
    out << "  %" << temporary << " = call " << returnType << " %" << functionPointer
        << '(';
    for (std::size_t i = 0; i < loweredArguments.size(); ++i) {
      if (i != 0) {
        out << ", ";
      }
      out << loweredArguments[i];
    }
    out << ")\n";
    state.values[temporary] = returnType;
    state.simpleTypes[temporary] = signature.returnType;
    supported = true;
    return "%" + temporary;
  }
  for (std::size_t i = 0; i < loweredArguments.size(); ++i) {
    if (i != 0) {
      out << ", ";
    }
    out << loweredArguments[i];
  }
  out << ")\n";
  supported = true;
  return {};
}

std::string lowerSelect(const nir::Value& value, const std::string& expectedType,
                        LoweringState& state, std::ostringstream& out,
                        bool& supported) {
  const std::string target = resolveValueGlobal(value, state);
  if (const FieldInfo* field = fieldInfo(target, state)) {
    if (value.operands.size() != 1 || llvmType(field->simpleType) == "void") {
      supported = false;
      return defaultValue(expectedType);
    }

    bool receiverSupported = false;
    const std::string receiver = lowerValue(value.operands.front(), field->className,
                                            state, out, receiverSupported);
    if (!receiverSupported) {
      supported = false;
      return defaultValue(expectedType);
    }

    emitSourceLocation(value.span, state, out);
    const std::string checkedReceiver = requireNonNullReceiver(receiver, state, out);
    const std::string pointer = nextTemporary(state);
    out << "  %" << pointer << " = getelementptr i8, ptr " << checkedReceiver
        << ", i64 " << field->offset << "\n";
    state.values[pointer] = "ptr";
    state.simpleTypes[pointer] = "Object";

    const std::string temporary = nextTemporary(state);
    out << "  %" << temporary << " = load " << llvmType(field->simpleType) << ", ptr %"
        << pointer << "\n";
    state.values[temporary] = llvmType(field->simpleType);
    state.simpleTypes[temporary] = field->simpleType;
    supported = true;
    return "%" + temporary;
  }

  const Signature* signature = functionSignature(target, state);
  if (target.empty() || signature == nullptr) {
    supported = false;
    return defaultValue(expectedType);
  }

  if (signature->parameterTypes.empty()) {
    return lowerZeroArgGlobalCall(target, expectedType, state, out, supported);
  }

  if (value.operands.size() != 1 || signature->parameterTypes.size() != 1) {
    supported = false;
    return defaultValue(expectedType);
  }

  if (isStackSuperSelect(value)) {
    return lowerStackSuperDispatch(value, {}, *signature, expectedType, state, out,
                                   supported);
  }

  if (shouldUseVirtualDispatch(value, target, state)) {
    return lowerVirtualDispatch(value, {}, target, *signature, expectedType, state, out,
                                supported);
  }

  bool receiverSupported = false;
  const std::string receiver =
      lowerValue(value.operands.front(), signature->parameterTypes.front(), state, out,
                 receiverSupported);
  if (!receiverSupported) {
    supported = false;
    return defaultValue(expectedType);
  }

  emitSourceLocation(value.span, state, out);
  const std::string checkedReceiver = requireNonNullReceiver(receiver, state, out);

  return lowerGlobalCall(
      target, {llvmType(signature->parameterTypes.front()) + " " + checkedReceiver},
      expectedType, state, out, supported);
}

std::string lowerAssign(const nir::Value& value, const std::string& expectedType,
                        LoweringState& state, std::ostringstream& out,
                        bool& supported) {
  if (value.operands.size() != 2 || expectedType != "Unit") {
    supported = false;
    return defaultValue(expectedType);
  }

  const nir::Value& target = value.operands.front();
  if (target.kind == nir::ValueKind::Local) {
    const std::string local = sanitizeIdentifier(target.text);
    auto simpleType = state.simpleTypes.find(local);
    if (state.mutableUnitLocals.contains(local) &&
        simpleType != state.simpleTypes.end() && simpleType->second == "Unit") {
      bool valueSupported = false;
      (void)lowerValue(value.operands.back(), "Unit", state, out, valueSupported);
      supported = valueSupported;
      return {};
    }

    auto slot = state.mutableLocalSlots.find(local);
    if (slot == state.mutableLocalSlots.end() ||
        simpleType == state.simpleTypes.end()) {
      supported = false;
      return {};
    }

    bool valueSupported = false;
    const std::string lowered = lowerValue(value.operands.back(), simpleType->second,
                                           state, out, valueSupported);
    if (!valueSupported) {
      supported = false;
      return {};
    }
    out << "  store ";
    if (state.preserveMutableLocalsAcrossHandlers) {
      out << "volatile ";
    }
    out << llvmType(simpleType->second) << ' ' << lowered << ", ptr %" << slot->second
        << "\n";
    auto localValueName = state.localValueNames.find(local);
    const std::string& rootKey =
        localValueName == state.localValueNames.end() ? local : localValueName->second;
    auto rootSlot = state.shadowRootSlots.find(rootKey);
    if (rootSlot != state.shadowRootSlots.end()) {
      out << "  store ptr " << lowered << ", ptr %" << rootSlot->second << "\n";
    }
    state.exactLocals.erase(local);
    supported = true;
    return {};
  }

  if (target.kind != nir::ValueKind::Select || target.operands.size() != 1) {
    supported = false;
    return {};
  }

  const std::string targetName = resolveValueGlobal(target, state);
  const FieldInfo* field = fieldInfo(targetName, state);
  if (field != nullptr) {
    bool receiverSupported = false;
    const std::string receiver = lowerValue(target.operands.front(), field->className,
                                            state, out, receiverSupported);
    if (!receiverSupported) {
      supported = false;
      return {};
    }

    supported = storeFieldValue(*field, receiver, value.operands.back(), state, out,
                                &value.span);
    return {};
  }

  const std::string setterTarget = targetName + "_$eq";
  const Signature* setter = functionSignature(setterTarget, state);
  const std::string setterOwner = ownerNameOf(setterTarget);
  bool moduleSetter = false;
  if (state.classLayouts != nullptr) {
    auto ownerLayout = state.classLayouts->find(setterOwner);
    moduleSetter =
        ownerLayout != state.classLayouts->end() && ownerLayout->second.isModule;
  }
  const std::size_t expectedParameterCount = moduleSetter ? 1 : 2;
  if (targetName.empty() || setter == nullptr ||
      setter->parameterTypes.size() != expectedParameterCount ||
      setter->returnType != "Unit") {
    supported = false;
    return {};
  }
  if (moduleSetter) {
    bool valueSupported = false;
    const std::string assigned =
        lowerValue(value.operands.back(), setter->parameterTypes.front(), state, out,
                   valueSupported);
    if (!valueSupported) {
      supported = false;
      return {};
    }
    return lowerGlobalCall(setterTarget,
                           {llvmType(setter->parameterTypes.front()) + " " + assigned},
                           "Unit", state, out, supported);
  }
  if (shouldUseVirtualDispatch(target, setterTarget, state)) {
    return lowerVirtualDispatch(target, {value.operands.back()}, setterTarget, *setter,
                                "Unit", state, out, supported);
  }

  bool receiverSupported = false;
  const std::string receiver =
      lowerValue(target.operands.front(), setter->parameterTypes.front(), state, out,
                 receiverSupported);
  bool valueSupported = false;
  const std::string assigned = lowerValue(
      value.operands.back(), setter->parameterTypes[1], state, out, valueSupported);
  if (!receiverSupported || !valueSupported) {
    supported = false;
    return {};
  }
  emitSourceLocation(value.span, state, out);
  const std::string checkedReceiver = requireNonNullReceiver(receiver, state, out);
  return lowerGlobalCall(
      setterTarget,
      {llvmType(setter->parameterTypes.front()) + " " + checkedReceiver,
       llvmType(setter->parameterTypes[1]) + " " + assigned},
      "Unit", state, out, supported);
}

std::size_t runtimeArrayElementSize(std::string_view elementType) {
  if (elementType == "Boolean") {
    return 1;
  }
  if (elementType == "Byte") {
    return 1;
  }
  if (elementType == "Short") {
    return 2;
  }
  if (elementType == "Int" || elementType == "Float" || elementType == "Char") {
    return 4;
  }
  if (elementType == "Long" || elementType == "Double") {
    return 8;
  }
  if (elementType == "String" || isReferenceType(std::string(elementType))) {
    return 8;
  }
  return 0;
}

void emitArrayDimensionCheck(const std::string& length, LoweringState& state,
                             std::ostringstream& out) {
  const std::string tag = nextTemporary(state);
  const std::string negative = nextTemporary(state);
  const std::string failureLabel = "array_dim_negative_" + tag;
  const std::string validLabel = "array_dim_valid_" + tag;
  out << "  %" << negative << " = icmp slt i32 " << length << ", 0\n";
  out << "  br i1 %" << negative << ", label %" << failureLabel << ", label %"
      << validLabel << "\n";
  out << failureLabel << ":\n";
  out << "  call void @__scalanative_throw_negative_array_size()\n";
  out << "  unreachable\n";
  out << validLabel << ":\n";
  state.currentBlockLabel = validLabel;
}

std::string emitArrayOfDimAllocation(std::size_t dimension,
                                     const std::vector<std::string>& lengths,
                                     const std::string& arrayType, LoweringState& state,
                                     std::ostringstream& out) {
  const std::string elementType = arrayElementTypeName(arrayType);
  const bool innermost = dimension + 1 == lengths.size();
  const std::size_t elementSize = innermost ? runtimeArrayElementSize(elementType) : 8;
  const std::string array = nextTemporary(state);
  out << "  %" << array << " = call ptr @__scalanative_array_alloc(i32 "
      << lengths[dimension] << ", i64 " << elementSize << ")\n";
  state.values[array] = "ptr";
  state.simpleTypes[array] = arrayType;
  if (innermost) {
    return "%" + array;
  }

  const std::string tag = nextTemporary(state);
  const std::string loopLabel = "array_of_dim_loop_" + tag;
  const std::string bodyLabel = "array_of_dim_body_" + tag;
  const std::string latchLabel = "array_of_dim_latch_" + tag;
  const std::string doneLabel = "array_of_dim_done_" + tag;
  const std::string index = nextTemporary(state);
  const std::string inBounds = nextTemporary(state);
  const std::string nextIndex = nextTemporary(state);
  const std::string preheaderLabel = state.currentBlockLabel;
  out << "  br label %" << loopLabel << "\n";
  out << loopLabel << ":\n";
  out << "  %" << index << " = phi i32 [ 0, %" << preheaderLabel << " ], [ %"
      << nextIndex << ", %" << latchLabel << " ]\n";
  out << "  %" << inBounds << " = icmp slt i32 %" << index << ", " << lengths[dimension]
      << "\n";
  out << "  br i1 %" << inBounds << ", label %" << bodyLabel << ", label %" << doneLabel
      << "\n";
  out << bodyLabel << ":\n";
  state.currentBlockLabel = bodyLabel;
  const std::string child =
      emitArrayOfDimAllocation(dimension + 1, lengths, elementType, state, out);
  out << "  call void @__scalanative_array_reference_set(ptr %" << array << ", i32 %"
      << index << ", ptr " << child << ")\n";
  out << "  br label %" << latchLabel << "\n";
  out << latchLabel << ":\n";
  out << "  %" << nextIndex << " = add i32 %" << index << ", 1\n";
  out << "  br label %" << loopLabel << "\n";
  out << doneLabel << ":\n";
  state.currentBlockLabel = doneLabel;
  return "%" + array;
}

std::string emitArrayFillAllocation(std::size_t dimension,
                                    const std::vector<std::string>& lengths,
                                    const std::string& arrayType,
                                    const nir::Value& elementValue,
                                    LoweringState& state, std::ostringstream& out,
                                    bool& supported) {
  const std::string elementType = arrayElementTypeName(arrayType);
  const bool innermost = dimension + 1 == lengths.size();
  const std::size_t elementSize = innermost ? runtimeArrayElementSize(elementType) : 8;
  if (elementType.empty() || elementSize == 0) {
    supported = false;
    return {};
  }

  const std::string array = nextTemporary(state);
  out << "  %" << array << " = call ptr @__scalanative_array_alloc(i32 "
      << lengths[dimension] << ", i64 " << elementSize << ")\n";
  state.values[array] = "ptr";
  state.simpleTypes[array] = arrayType;

  const std::string tag = nextTemporary(state);
  const std::string loopLabel = "array_fill_loop_" + tag;
  const std::string bodyLabel = "array_fill_body_" + tag;
  const std::string latchLabel = "array_fill_latch_" + tag;
  const std::string doneLabel = "array_fill_done_" + tag;
  const std::string index = nextTemporary(state);
  const std::string inBounds = nextTemporary(state);
  const std::string nextIndex = nextTemporary(state);
  const std::string preheaderLabel = state.currentBlockLabel;
  out << "  br label %" << loopLabel << "\n";
  out << loopLabel << ":\n";
  out << "  %" << index << " = phi i32 [ 0, %" << preheaderLabel << " ], [ %"
      << nextIndex << ", %" << latchLabel << " ]\n";
  out << "  %" << inBounds << " = icmp slt i32 %" << index << ", " << lengths[dimension]
      << "\n";
  out << "  br i1 %" << inBounds << ", label %" << bodyLabel << ", label %" << doneLabel
      << "\n";
  out << bodyLabel << ":\n";
  state.currentBlockLabel = bodyLabel;

  std::string element;
  if (innermost) {
    bool elementSupported = false;
    element = lowerValue(elementValue, elementType, state, out, elementSupported);
    if (!elementSupported) {
      supported = false;
      return {};
    }
  } else {
    element = emitArrayFillAllocation(dimension + 1, lengths, elementType, elementValue,
                                      state, out, supported);
    if (!supported) {
      return {};
    }
  }

  const std::string wideIndex = nextTemporary(state);
  const std::string elements = nextTemporary(state);
  const std::string slot = nextTemporary(state);
  const std::string storageType =
      elementType == "Boolean" ? "i8" : llvmType(elementType);
  out << "  %" << wideIndex << " = sext i32 %" << index << " to i64\n";
  out << "  %" << elements << " = getelementptr i8, ptr %" << array << ", i64 "
      << ObjectHeaderSize + 8 << "\n";
  out << "  %" << slot << " = getelementptr " << storageType << ", ptr %" << elements
      << ", i64 %" << wideIndex << "\n";
  out << "  store " << llvmType(elementType) << ' ' << element << ", ptr %" << slot
      << "\n";
  state.values[wideIndex] = "i64";
  state.simpleTypes[wideIndex] = "Long";
  state.values[elements] = "ptr";
  state.simpleTypes[elements] = "Object";
  state.values[slot] = "ptr";
  state.simpleTypes[slot] = "Object";
  out << "  br label %" << latchLabel << "\n";
  out << latchLabel << ":\n";
  out << "  %" << nextIndex << " = add i32 %" << index << ", 1\n";
  out << "  br label %" << loopLabel << "\n";
  out << doneLabel << ":\n";
  state.currentBlockLabel = doneLabel;
  supported = true;
  return "%" + array;
}

std::string lowerCall(const nir::Value& value, const std::string& expectedType,
                      LoweringState& state, std::ostringstream& out, bool& supported) {
  if (value.operands.empty()) {
    supported = false;
    return defaultValue(expectedType);
  }
  emitSourceLocation(value.span, state, out);

  const std::string target = resolveValueGlobal(value.operands.front(), state);
  const Signature* signature = functionSignature(target, state);
  if (target.empty() || signature == nullptr) {
    supported = false;
    return defaultValue(expectedType);
  }

  const bool isRuntimeAssert = target == support::StdNames::RuntimeAssert;
  const bool isRuntimeAssume = target == support::StdNames::RuntimeAssume;
  const bool isRuntimeRequire = target == support::StdNames::RuntimeRequire;
  const bool isRuntimePrintln = target == support::StdNames::RuntimePrintln;
  const bool isRuntimeGcCollect = target == support::StdNames::RuntimeGcCollect;
  const bool isRuntimeGcLiveObjectCount =
      target == support::StdNames::RuntimeGcLiveObjectCount;
  const bool isRuntimeGcCollectionCount =
      target == support::StdNames::RuntimeGcCollectionCount;
  const bool isRuntimeGcSetCollectionThreshold =
      target == support::StdNames::RuntimeGcSetCollectionThreshold;
  const bool isRuntimeStringLength = target == support::StdNames::RuntimeStringLength;
  const bool isRuntimeStringToString =
      target == support::StdNames::RuntimeStringToString;
  const bool isRuntimeStringEquals = target == support::StdNames::RuntimeStringEquals;
  const bool isRuntimeZoneAllocBytes =
      target == support::StdNames::RuntimeZoneAllocBytes;
  const bool isRuntimeNativeBytesGetShort =
      target == support::StdNames::RuntimeNativeBytesGetShortBe ||
      target == support::StdNames::RuntimeNativeBytesGetShortLe;
  const bool isRuntimeNativeBytesPutShort =
      target == support::StdNames::RuntimeNativeBytesPutShortBe ||
      target == support::StdNames::RuntimeNativeBytesPutShortLe;
  const bool isRuntimeNativeBytesLittleEndian =
      target == support::StdNames::RuntimeNativeBytesGetShortLe ||
      target == support::StdNames::RuntimeNativeBytesPutShortLe;
  const bool isRuntimeByteBufferWrap =
      target == support::StdNames::RuntimeByteBufferWrap;
  const bool isRuntimeByteBufferCapacity =
      target == support::StdNames::RuntimeByteBufferCapacity;
  const bool isRuntimeByteBufferPosition =
      target == support::StdNames::RuntimeByteBufferPosition;
  const bool isRuntimeByteBufferSetPosition =
      target == support::StdNames::RuntimeByteBufferSetPosition;
  const bool isRuntimeByteBufferLimit =
      target == support::StdNames::RuntimeByteBufferLimit;
  const bool isRuntimeByteBufferSetLimit =
      target == support::StdNames::RuntimeByteBufferSetLimit;
  const bool isRuntimeByteBufferRemaining =
      target == support::StdNames::RuntimeByteBufferRemaining;
  const bool isRuntimeByteBufferHasRemaining =
      target == support::StdNames::RuntimeByteBufferHasRemaining;
  const bool isRuntimeByteBufferGet = target == support::StdNames::RuntimeByteBufferGet;
  const bool isRuntimeByteBufferPut = target == support::StdNames::RuntimeByteBufferPut;
  const bool isRuntimeByteBufferClear =
      target == support::StdNames::RuntimeByteBufferClear;
  const bool isRuntimeByteBufferFlip =
      target == support::StdNames::RuntimeByteBufferFlip;
  const bool isRuntimeByteBufferRewind =
      target == support::StdNames::RuntimeByteBufferRewind;
  const bool isRuntimeByteBufferOperation =
      isRuntimeByteBufferCapacity || isRuntimeByteBufferPosition ||
      isRuntimeByteBufferSetPosition || isRuntimeByteBufferLimit ||
      isRuntimeByteBufferSetLimit || isRuntimeByteBufferRemaining ||
      isRuntimeByteBufferHasRemaining || isRuntimeByteBufferGet ||
      isRuntimeByteBufferPut || isRuntimeByteBufferClear || isRuntimeByteBufferFlip ||
      isRuntimeByteBufferRewind;
  const bool isRuntimeArrayAlloc = target == support::StdNames::RuntimeArrayAlloc;
  const bool isRuntimeArrayLength = target == support::StdNames::RuntimeArrayLength;
  const bool isRuntimeArrayApply = target == support::StdNames::RuntimeArrayApply;
  const bool isRuntimeArrayUpdate = target == support::StdNames::RuntimeArrayUpdate;
  const bool isRuntimeArrayClone = target == support::StdNames::RuntimeArrayClone;
  const bool isRuntimeArrayRange = target == support::StdNames::RuntimeArrayRange;
  const bool isRuntimeIntArrayLength =
      target == support::StdNames::RuntimeIntArrayLength;
  const bool isRuntimeIntArrayAlloc = target == support::StdNames::RuntimeIntArrayAlloc;
  const bool isRuntimeIntArrayApply = target == support::StdNames::RuntimeIntArrayApply;
  const bool isRuntimeIntArrayUpdate =
      target == support::StdNames::RuntimeIntArrayUpdate;
  const bool isRuntimeIntArrayClone = target == support::StdNames::RuntimeIntArrayClone;
  const bool isRuntimeByteArrayLength =
      target == support::StdNames::RuntimeByteArrayLength;
  const bool isRuntimeByteArrayAlloc =
      target == support::StdNames::RuntimeByteArrayAlloc;
  const bool isRuntimeByteArrayApply =
      target == support::StdNames::RuntimeByteArrayApply;
  const bool isRuntimeByteArrayUpdate =
      target == support::StdNames::RuntimeByteArrayUpdate;
  const bool isRuntimeByteArrayClone =
      target == support::StdNames::RuntimeByteArrayClone;
  const bool isRuntimeShortArrayLength =
      target == support::StdNames::RuntimeShortArrayLength;
  const bool isRuntimeShortArrayAlloc =
      target == support::StdNames::RuntimeShortArrayAlloc;
  const bool isRuntimeShortArrayApply =
      target == support::StdNames::RuntimeShortArrayApply;
  const bool isRuntimeShortArrayUpdate =
      target == support::StdNames::RuntimeShortArrayUpdate;
  const bool isRuntimeShortArrayClone =
      target == support::StdNames::RuntimeShortArrayClone;
  const bool isRuntimeBooleanArrayLength =
      target == support::StdNames::RuntimeBooleanArrayLength;
  const bool isRuntimeBooleanArrayAlloc =
      target == support::StdNames::RuntimeBooleanArrayAlloc;
  const bool isRuntimeBooleanArrayApply =
      target == support::StdNames::RuntimeBooleanArrayApply;
  const bool isRuntimeBooleanArrayUpdate =
      target == support::StdNames::RuntimeBooleanArrayUpdate;
  const bool isRuntimeBooleanArrayClone =
      target == support::StdNames::RuntimeBooleanArrayClone;
  const bool isRuntimeLongArrayLength =
      target == support::StdNames::RuntimeLongArrayLength;
  const bool isRuntimeLongArrayAlloc =
      target == support::StdNames::RuntimeLongArrayAlloc;
  const bool isRuntimeLongArrayApply =
      target == support::StdNames::RuntimeLongArrayApply;
  const bool isRuntimeLongArrayUpdate =
      target == support::StdNames::RuntimeLongArrayUpdate;
  const bool isRuntimeLongArrayClone =
      target == support::StdNames::RuntimeLongArrayClone;
  const bool isRuntimeDoubleArrayLength =
      target == support::StdNames::RuntimeDoubleArrayLength;
  const bool isRuntimeDoubleArrayAlloc =
      target == support::StdNames::RuntimeDoubleArrayAlloc;
  const bool isRuntimeDoubleArrayApply =
      target == support::StdNames::RuntimeDoubleArrayApply;
  const bool isRuntimeDoubleArrayUpdate =
      target == support::StdNames::RuntimeDoubleArrayUpdate;
  const bool isRuntimeDoubleArrayClone =
      target == support::StdNames::RuntimeDoubleArrayClone;
  const bool isRuntimeFloatArrayLength =
      target == support::StdNames::RuntimeFloatArrayLength;
  const bool isRuntimeFloatArrayAlloc =
      target == support::StdNames::RuntimeFloatArrayAlloc;
  const bool isRuntimeFloatArrayApply =
      target == support::StdNames::RuntimeFloatArrayApply;
  const bool isRuntimeFloatArrayUpdate =
      target == support::StdNames::RuntimeFloatArrayUpdate;
  const bool isRuntimeFloatArrayClone =
      target == support::StdNames::RuntimeFloatArrayClone;
  const bool isRuntimeCharArrayLength =
      target == support::StdNames::RuntimeCharArrayLength;
  const bool isRuntimeCharArrayAlloc =
      target == support::StdNames::RuntimeCharArrayAlloc;
  const bool isRuntimeCharArrayApply =
      target == support::StdNames::RuntimeCharArrayApply;
  const bool isRuntimeCharArrayUpdate =
      target == support::StdNames::RuntimeCharArrayUpdate;
  const bool isRuntimeCharArrayClone =
      target == support::StdNames::RuntimeCharArrayClone;
  const std::string referenceArrayLengthPrefix =
      std::string(support::StdNames::RuntimeReferenceArrayLength) + ".";
  const std::string referenceArrayAllocPrefix =
      std::string(support::StdNames::RuntimeReferenceArrayAlloc) + ".";
  const std::string referenceArrayApplyPrefix =
      std::string(support::StdNames::RuntimeReferenceArrayApply) + ".";
  const std::string referenceArrayUpdatePrefix =
      std::string(support::StdNames::RuntimeReferenceArrayUpdate) + ".";
  const std::string referenceArrayClonePrefix =
      std::string(support::StdNames::RuntimeReferenceArrayClone) + ".";
  const std::string arrayOfDimPrefix =
      std::string(support::StdNames::RuntimeArrayOfDim) + ".";
  const std::string arrayFillPrefix =
      std::string(support::StdNames::RuntimeArrayFill) + ".";
  const std::string arrayConcatPrefix =
      std::string(support::StdNames::RuntimeArrayConcat) + ".";
  const std::string arrayCopyPrefix =
      std::string(support::StdNames::RuntimeArrayCopy) + ".";
  const std::string referenceArrayCopyPrefix =
      std::string(support::StdNames::RuntimeReferenceArrayCopy) + ".";
  const bool isRuntimeReferenceArrayLength =
      target.starts_with(referenceArrayLengthPrefix);
  const bool isRuntimeReferenceArrayAlloc =
      target.starts_with(referenceArrayAllocPrefix);
  const bool isRuntimeReferenceArrayApply =
      target.starts_with(referenceArrayApplyPrefix);
  const bool isRuntimeReferenceArrayUpdate =
      target.starts_with(referenceArrayUpdatePrefix);
  const bool isRuntimeReferenceArrayClone =
      target.starts_with(referenceArrayClonePrefix);
  const bool isRuntimeArrayOfDim = target.starts_with(arrayOfDimPrefix);
  const bool isRuntimeArrayFill = target.starts_with(arrayFillPrefix);
  const bool isRuntimeArrayConcat = target.starts_with(arrayConcatPrefix);
  const bool isRuntimeArrayCopy = target.starts_with(arrayCopyPrefix);
  const bool isRuntimeReferenceArrayCopy = target.starts_with(referenceArrayCopyPrefix);
  const bool isRuntimePrimitiveToString =
      target == support::StdNames::RuntimeBooleanToString ||
      target == support::StdNames::RuntimeByteToString ||
      target == support::StdNames::RuntimeShortToString ||
      target == support::StdNames::RuntimeIntToString ||
      target == support::StdNames::RuntimeLongToString ||
      target == support::StdNames::RuntimeFloatToString ||
      target == support::StdNames::RuntimeDoubleToString ||
      target == support::StdNames::RuntimeCharToString;
  const bool isRuntimeAnyToString = target == support::StdNames::RuntimeAnyToString;
  const bool isRuntimeAnyReceiverToString =
      target == support::StdNames::RuntimeAnyReceiverToString;
  const bool isRuntimeThrowableToString =
      target == support::StdNames::RuntimeThrowableToString;
  const bool isRuntimePrintStackTrace =
      target == support::StdNames::RuntimePrintStackTrace;
  const bool isRuntimeFillInStackTrace =
      target == support::StdNames::RuntimeFillInStackTrace;
  const bool isRuntimeGetStackTrace = target == support::StdNames::RuntimeGetStackTrace;
  const bool isRuntimeSetStackTrace = target == support::StdNames::RuntimeSetStackTrace;
  const bool isRuntimeAddSuppressed = target == support::StdNames::RuntimeAddSuppressed;
  const bool isRuntimeGetSuppressed = target == support::StdNames::RuntimeGetSuppressed;
  const bool isRuntimeStackTraceElementToString =
      target == support::StdNames::RuntimeStackTraceElementToString;
  const bool isRuntimeAnyEquals = target == support::StdNames::RuntimeAnyEquals;
  const bool isRuntimeAnyReceiverEquals =
      target == support::StdNames::RuntimeAnyReceiverEquals;
  const bool isRuntimeHashCode =
      target == support::StdNames::RuntimeByteHashCode ||
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
  const bool isRuntimeNumericConversion =
      target == support::StdNames::RuntimeIntToByte ||
      target == support::StdNames::RuntimeIntToShort ||
      target == support::StdNames::RuntimeShortToByte ||
      target == support::StdNames::RuntimeByteToShort ||
      target == support::StdNames::RuntimeByteToInt ||
      target == support::StdNames::RuntimeShortToInt;
  const bool isRuntimeFormat = target == support::StdNames::RuntimeFormat;
  const bool isRuntimeBooleanFormat = target == support::StdNames::RuntimeFormatBoolean;
  if (isRuntimeGcCollect) {
    if (value.operands.size() != 1) {
      supported = false;
      return defaultValue(expectedType);
    }
    out << "  call void @__scalanative_gc_collect()\n";
    supported = true;
    return {};
  }
  if (isRuntimeGcLiveObjectCount || isRuntimeGcCollectionCount) {
    if (value.operands.size() != 1 || expectedType != "Long") {
      supported = false;
      return defaultValue(expectedType);
    }
    const std::string temporary = nextTemporary(state);
    out << "  %" << temporary << " = load i64, ptr @__scalanative_gc_"
        << (isRuntimeGcLiveObjectCount ? "allocation_count" : "collection_count")
        << "\n";
    state.values[temporary] = "i64";
    state.simpleTypes[temporary] = "Long";
    supported = true;
    return "%" + temporary;
  }
  if (isRuntimeGcSetCollectionThreshold) {
    if (value.operands.size() != 2 || expectedType != "Unit") {
      supported = false;
      return defaultValue(expectedType);
    }
    bool thresholdSupported = false;
    const std::string threshold =
        lowerValue(value.operands[1], "Long", state, out, thresholdSupported);
    if (!thresholdSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    out << "  call void @__scalanative_gc_set_threshold(i64 " << threshold << ")\n";
    supported = true;
    return {};
  }
  if (isRuntimeNumericConversion) {
    if (value.operands.size() != 2 || signature->parameterTypes.size() != 1 ||
        expectedType != signature->returnType) {
      supported = false;
      return defaultValue(expectedType);
    }
    const std::string& sourceType = signature->parameterTypes.front();
    const std::string& targetType = signature->returnType;
    bool operandSupported = false;
    const std::string operand =
        lowerValue(value.operands[1], sourceType, state, out, operandSupported);
    if (!operandSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    const std::string result = nextTemporary(state);
    const bool widening = typeSize(sourceType) < typeSize(targetType);
    out << "  %" << result << " = " << (widening ? "sext " : "trunc ")
        << llvmType(sourceType) << ' ' << operand << " to " << llvmType(targetType)
        << "\n";
    state.values[result] = llvmType(targetType);
    state.simpleTypes[result] = targetType;
    supported = true;
    return "%" + result;
  }
  if (isRuntimePrimitiveToString) {
    if (value.operands.size() != 2 || expectedType != "String") {
      supported = false;
      return defaultValue(expectedType);
    }
    return lowerStringOperand(value.operands[1], state, out, supported);
  }
  if (isRuntimeStringToString) {
    if (value.operands.size() != 2 || expectedType != "String") {
      supported = false;
      return defaultValue(expectedType);
    }
    bool stringSupported = false;
    const std::string string =
        lowerValue(value.operands[1], "String", state, out, stringSupported);
    if (!stringSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    emitSourceLocation(value.span, state, out);
    supported = true;
    return requireNonNullReceiver(string, state, out);
  }
  if (isRuntimeAnyToString || isRuntimeAnyReceiverToString) {
    if (value.operands.size() != 2 || expectedType != "String") {
      supported = false;
      return defaultValue(expectedType);
    }
    bool objectSupported = false;
    const std::string object =
        lowerValue(value.operands[1], "Object", state, out, objectSupported);
    if (!objectSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    std::string checkedObject = object;
    if (isRuntimeAnyReceiverToString) {
      emitSourceLocation(value.span, state, out);
      checkedObject = requireNonNullReceiver(object, state, out);
    }
    const std::string result = nextTemporary(state);
    out << "  %" << result << " = call ptr @__scalanative_any_to_string(ptr "
        << checkedObject << ")\n";
    state.values[result] = "ptr";
    state.simpleTypes[result] = "String";
    supported = true;
    return "%" + result;
  }
  if (isRuntimeThrowableToString) {
    if (value.operands.size() != 2 || expectedType != "String") {
      supported = false;
      return defaultValue(expectedType);
    }
    bool throwableSupported = false;
    const std::string throwable =
        lowerValue(value.operands[1], std::string(support::StdNames::JavaLangThrowable),
                   state, out, throwableSupported);
    if (!throwableSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    const std::string result = nextTemporary(state);
    out << "  %" << result << " = call ptr @__scalanative_throwable_to_string(ptr "
        << throwable << ")\n";
    state.values[result] = "ptr";
    state.simpleTypes[result] = "String";
    supported = true;
    return "%" + result;
  }
  if (isRuntimePrintStackTrace) {
    if (value.operands.size() != 2 || expectedType != "Unit") {
      supported = false;
      return defaultValue(expectedType);
    }
    bool throwableSupported = false;
    const std::string throwable =
        lowerValue(value.operands[1], std::string(support::StdNames::JavaLangThrowable),
                   state, out, throwableSupported);
    if (!throwableSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    emitSourceLocation(value.span, state, out);
    out << "  call void @__scalanative_print_stack_trace(ptr " << throwable << ")\n";
    supported = true;
    return {};
  }
  if (isRuntimeFillInStackTrace) {
    if (value.operands.size() != 2 ||
        expectedType != support::StdNames::JavaLangThrowable) {
      supported = false;
      return defaultValue(expectedType);
    }
    bool throwableSupported = false;
    const std::string throwable =
        lowerValue(value.operands[1], std::string(support::StdNames::JavaLangThrowable),
                   state, out, throwableSupported);
    if (!throwableSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    const std::string result = nextTemporary(state);
    out << "  %" << result << " = call ptr @__scalanative_fill_in_stack_trace(ptr "
        << throwable << ")\n";
    state.values[result] = "ptr";
    state.simpleTypes[result] = std::string(support::StdNames::JavaLangThrowable);
    supported = true;
    return "%" + result;
  }
  if (isRuntimeGetStackTrace) {
    const std::string arrayType =
        "Array [ " + std::string(support::StdNames::JavaLangStackTraceElement) + " ]";
    if (value.operands.size() != 2 || expectedType != arrayType) {
      supported = false;
      return defaultValue(expectedType);
    }
    bool throwableSupported = false;
    const std::string throwable =
        lowerValue(value.operands[1], std::string(support::StdNames::JavaLangThrowable),
                   state, out, throwableSupported);
    if (!throwableSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    const std::string result = nextTemporary(state);
    out << "  %" << result << " = call ptr @__scalanative_get_stack_trace(ptr "
        << throwable << ")\n";
    state.values[result] = "ptr";
    state.simpleTypes[result] = arrayType;
    supported = true;
    return "%" + result;
  }
  if (isRuntimeSetStackTrace) {
    const std::string arrayType =
        "Array [ " + std::string(support::StdNames::JavaLangStackTraceElement) + " ]";
    if (value.operands.size() != 3 || expectedType != "Int") {
      supported = false;
      return defaultValue(expectedType);
    }
    bool throwableSupported = false;
    const std::string throwable =
        lowerValue(value.operands[1], std::string(support::StdNames::JavaLangThrowable),
                   state, out, throwableSupported);
    bool arraySupported = false;
    const std::string array =
        lowerValue(value.operands[2], arrayType, state, out, arraySupported);
    if (!throwableSupported || !arraySupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    const std::string result = nextTemporary(state);
    out << "  %" << result << " = call i32 @__scalanative_set_stack_trace(ptr "
        << throwable << ", ptr " << array << ")\n";
    state.values[result] = "i32";
    state.simpleTypes[result] = "Int";
    supported = true;
    return "%" + result;
  }
  if (isRuntimeAddSuppressed) {
    if (value.operands.size() != 3 || expectedType != "Unit") {
      supported = false;
      return defaultValue(expectedType);
    }
    bool throwableSupported = false;
    const std::string throwable =
        lowerValue(value.operands[1], std::string(support::StdNames::JavaLangThrowable),
                   state, out, throwableSupported);
    bool suppressedSupported = false;
    const std::string suppressed =
        lowerValue(value.operands[2], std::string(support::StdNames::JavaLangThrowable),
                   state, out, suppressedSupported);
    if (!throwableSupported || !suppressedSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    out << "  call void @__scalanative_add_suppressed(ptr " << throwable << ", ptr "
        << suppressed << ")\n";
    supported = true;
    return {};
  }
  if (isRuntimeGetSuppressed) {
    const std::string arrayType =
        "Array [ " + std::string(support::StdNames::JavaLangThrowable) + " ]";
    if (value.operands.size() != 2 || expectedType != arrayType) {
      supported = false;
      return defaultValue(expectedType);
    }
    bool throwableSupported = false;
    const std::string throwable =
        lowerValue(value.operands[1], std::string(support::StdNames::JavaLangThrowable),
                   state, out, throwableSupported);
    if (!throwableSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    const std::string result = nextTemporary(state);
    out << "  %" << result << " = call ptr @__scalanative_get_suppressed(ptr "
        << throwable << ")\n";
    state.values[result] = "ptr";
    state.simpleTypes[result] = arrayType;
    supported = true;
    return "%" + result;
  }
  if (isRuntimeStackTraceElementToString) {
    if (value.operands.size() != 2 || expectedType != "String") {
      supported = false;
      return defaultValue(expectedType);
    }
    bool frameSupported = false;
    const std::string frame = lowerValue(
        value.operands[1], std::string(support::StdNames::JavaLangStackTraceElement),
        state, out, frameSupported);
    if (!frameSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    const std::string result = nextTemporary(state);
    out << "  %" << result
        << " = call ptr @__scalanative_stack_trace_element_to_string(ptr " << frame
        << ")\n";
    state.values[result] = "ptr";
    state.simpleTypes[result] = "String";
    supported = true;
    return "%" + result;
  }
  if (isRuntimeAnyEquals || isRuntimeAnyReceiverEquals) {
    if (value.operands.size() != 3 || expectedType != "Boolean") {
      supported = false;
      return defaultValue(expectedType);
    }
    bool leftSupported = false;
    bool rightSupported = false;
    const std::string left =
        lowerValue(value.operands[1], "Object", state, out, leftSupported);
    const std::string right =
        lowerValue(value.operands[2], "Object", state, out, rightSupported);
    if (!leftSupported || !rightSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    std::string checkedLeft = left;
    if (isRuntimeAnyReceiverEquals) {
      emitSourceLocation(value.span, state, out);
      checkedLeft = requireNonNullReceiver(left, state, out);
    }
    const std::string result = nextTemporary(state);
    out << "  %" << result << " = call i1 @__scalanative_any_equals(ptr " << checkedLeft
        << ", ptr " << right << ")\n";
    state.values[result] = "i1";
    state.simpleTypes[result] = "Boolean";
    supported = true;
    return "%" + result;
  }
  if (isRuntimeStringEquals) {
    if (value.operands.size() != 3 || expectedType != "Boolean") {
      supported = false;
      return defaultValue(expectedType);
    }
    bool leftSupported = false;
    const std::string left =
        lowerValue(value.operands[1], "String", state, out, leftSupported);
    bool rightSupported = false;
    const std::string right =
        lowerValue(value.operands[2], "Object", state, out, rightSupported);
    if (!leftSupported || !rightSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    emitSourceLocation(value.span, state, out);
    const std::string checkedLeft = requireNonNullReceiver(left, state, out);
    const std::string result = nextTemporary(state);
    out << "  %" << result << " = call i1 @__scalanative_string_equals_object(ptr "
        << checkedLeft << ", ptr " << right << ")\n";
    state.values[result] = "i1";
    state.simpleTypes[result] = "Boolean";
    supported = true;
    return "%" + result;
  }
  if (isRuntimeHashCode) {
    if (value.operands.size() != 2 || expectedType != "Int") {
      supported = false;
      return defaultValue(expectedType);
    }
    std::string argumentType;
    std::string helper;
    if (target == support::StdNames::RuntimeByteHashCode) {
      argumentType = "Byte";
    } else if (target == support::StdNames::RuntimeShortHashCode) {
      argumentType = "Short";
    } else if (target == support::StdNames::RuntimeBooleanHashCode) {
      argumentType = "Boolean";
      helper = "__scalanative_boolean_hash_code";
    } else if (target == support::StdNames::RuntimeLongHashCode) {
      argumentType = "Long";
      helper = "__scalanative_long_hash_code";
    } else if (target == support::StdNames::RuntimeFloatHashCode) {
      argumentType = "Float";
      helper = "__scalanative_float_hash_code";
    } else if (target == support::StdNames::RuntimeDoubleHashCode) {
      argumentType = "Double";
      helper = "__scalanative_double_hash_code";
    } else if (target == support::StdNames::RuntimeCharHashCode) {
      argumentType = "Char";
      helper = "__scalanative_char_hash_code";
    } else if (target == support::StdNames::RuntimeStringHashCode) {
      argumentType = "String";
      helper = "__scalanative_string_hash_code";
    } else if (target == support::StdNames::RuntimeSymbolHashCode) {
      argumentType = "Symbol";
      helper = "__scalanative_string_hash_code";
    } else {
      argumentType = "Object";
      helper = "__scalanative_any_hash_code";
    }

    bool argumentSupported = false;
    const std::string argument =
        lowerValue(value.operands[1], argumentType, state, out, argumentSupported);
    if (!argumentSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    std::string checkedArgument = argument;
    if (target == support::StdNames::RuntimeStringHashCode ||
        target == support::StdNames::RuntimeAnyReceiverHashCode) {
      emitSourceLocation(value.span, state, out);
      checkedArgument = requireNonNullReceiver(argument, state, out);
    }
    const std::string result = nextTemporary(state);
    if (helper.empty()) {
      out << "  %" << result << " = sext " << llvmType(argumentType) << ' '
          << checkedArgument << " to i32\n";
      state.values[result] = "i32";
      state.simpleTypes[result] = "Int";
      supported = true;
      return "%" + result;
    }
    out << "  %" << result << " = call i32 @" << helper << '(' << llvmType(argumentType)
        << ' ' << checkedArgument << ")\n";
    state.values[result] = "i32";
    state.simpleTypes[result] = "Int";
    supported = true;
    return "%" + result;
  }
  if (isRuntimeFormat || isRuntimeBooleanFormat) {
    if (value.operands.size() != 3 || expectedType != "String") {
      supported = false;
      return defaultValue(expectedType);
    }
    bool formatSupported = false;
    const std::string format =
        lowerValue(value.operands[1], "String", state, out, formatSupported);
    const std::string valueType = inferSimpleType(value.operands[2], state);
    const bool supportedValueType =
        isRuntimeBooleanFormat
            ? valueType == "Boolean"
            : valueType == "Float" || valueType == "Double" || valueType == "Byte" ||
                  valueType == "Short" || valueType == "Int" || valueType == "Long" ||
                  valueType == "Char" || valueType == "String";
    if (!formatSupported || !supportedValueType) {
      supported = false;
      return defaultValue(expectedType);
    }
    bool valueSupported = false;
    const std::string formattedValue =
        lowerValue(value.operands[2], valueType, state, out, valueSupported);
    if (!valueSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    const std::string storage = nextTemporary(state);
    out << "  %" << storage << " = call ptr @__scalanative_program_arena_alloc(i64 "
        << 512 + ObjectHeaderSize << ", ptr null)\n";
    state.values[storage] = "ptr";
    state.simpleTypes[storage] = "Object";
    const std::string result = nextTemporary(state);
    out << "  %" << result << " = getelementptr i8, ptr %" << storage << ", i64 "
        << ObjectHeaderSize << "\n";
    state.values[result] = "ptr";
    state.simpleTypes[result] = "String";
    std::string formatArgument;
    if (valueType == "Float") {
      const std::string promoted = nextTemporary(state);
      out << "  %" << promoted << " = fpext float " << formattedValue << " to double\n";
      state.values[promoted] = "double";
      state.simpleTypes[promoted] = "Double";
      formatArgument = "double %" + promoted;
    } else if (valueType == "Byte" || valueType == "Short" || valueType == "Int") {
      const std::string widened = nextTemporary(state);
      out << "  %" << widened << " = sext " << llvmType(valueType) << ' '
          << formattedValue << " to i64\n";
      state.values[widened] = "i64";
      state.simpleTypes[widened] = "Long";
      formatArgument = "i64 %" + widened;
    } else if (isRuntimeBooleanFormat) {
      const std::string textValue = nextTemporary(state);
      out << "  %" << textValue << " = select i1 " << formattedValue
          << ", ptr getelementptr inbounds ([5 x i8], ptr @.str.boolean.true, i64 0, "
             "i64 "
             "0), ptr getelementptr inbounds ([6 x i8], ptr @.str.boolean.false, i64 "
             "0, "
             "i64 0)\n";
      state.values[textValue] = "ptr";
      state.simpleTypes[textValue] = "String";
      formatArgument = "ptr %" + textValue;
    } else {
      formatArgument = llvmType(valueType) + " " + formattedValue;
    }
    out << "  %" << nextTemporary(state)
        << " = call i32 (ptr, i64, ptr, ...) @snprintf(ptr %" << result
        << ", i64 512, ptr " << format << ", " << formatArgument << ")\n";
    supported = true;
    return "%" + result;
  }
  if (isRuntimeStringLength) {
    if (value.operands.size() != 2 || expectedType != "Int") {
      supported = false;
      return defaultValue(expectedType);
    }
    bool stringSupported = false;
    const std::string string =
        lowerValue(value.operands[1], "String", state, out, stringSupported);
    if (!stringSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    emitSourceLocation(value.span, state, out);
    const std::string checkedString = requireNonNullReceiver(string, state, out);
    const std::string wideLength = nextTemporary(state);
    out << "  %" << wideLength << " = call i64 @strlen(ptr " << checkedString << ")\n";
    state.values[wideLength] = "i64";
    state.simpleTypes[wideLength] = "Long";
    const std::string length = nextTemporary(state);
    out << "  %" << length << " = trunc i64 %" << wideLength << " to i32\n";
    state.values[length] = "i32";
    state.simpleTypes[length] = "Int";
    supported = true;
    return "%" + length;
  }
  if (isRuntimeArrayRange) {
    if (value.operands.size() != 4 || signature->parameterTypes.size() != 3 ||
        expectedType != "Array [ Int ]" || signature->returnType != expectedType ||
        !std::all_of(signature->parameterTypes.begin(), signature->parameterTypes.end(),
                     [](const std::string& type) { return type == "Int"; })) {
      supported = false;
      return defaultValue(expectedType);
    }

    std::vector<std::string> arguments;
    arguments.reserve(3);
    for (std::size_t i = 1; i < value.operands.size(); ++i) {
      bool argumentSupported = false;
      const std::string argument =
          lowerValue(value.operands[i], "Int", state, out, argumentSupported);
      if (!argumentSupported) {
        supported = false;
        return defaultValue(expectedType);
      }
      arguments.push_back(argument);
    }

    const std::string wideStart = nextTemporary(state);
    const std::string wideEnd = nextTemporary(state);
    const std::string wideStep = nextTemporary(state);
    out << "  %" << wideStart << " = sext i32 " << arguments[0] << " to i64\n";
    out << "  %" << wideEnd << " = sext i32 " << arguments[1] << " to i64\n";
    out << "  %" << wideStep << " = sext i32 " << arguments[2] << " to i64\n";

    const std::string tag = nextTemporary(state);
    const std::string zeroStepLabel = "array_range_zero_step_" + tag;
    const std::string directionLabel = "array_range_direction_" + tag;
    const std::string positiveLabel = "array_range_positive_" + tag;
    const std::string negativeLabel = "array_range_negative_" + tag;
    const std::string countLabel = "array_range_count_" + tag;
    const std::string tooLargeLabel = "array_range_too_large_" + tag;
    const std::string allocateLabel = "array_range_allocate_" + tag;
    const std::string loopLabel = "array_range_loop_" + tag;
    const std::string bodyLabel = "array_range_body_" + tag;
    const std::string latchLabel = "array_range_latch_" + tag;
    const std::string doneLabel = "array_range_done_" + tag;

    const std::string zeroStep = nextTemporary(state);
    out << "  %" << zeroStep << " = icmp eq i64 %" << wideStep << ", 0\n";
    out << "  br i1 %" << zeroStep << ", label %" << zeroStepLabel << ", label %"
        << directionLabel << "\n";
    out << zeroStepLabel << ":\n";
    out << "  call void @__scalanative_throw_array_range_zero_step()\n";
    out << "  unreachable\n";
    out << directionLabel << ":\n";

    const std::string positiveStep = nextTemporary(state);
    out << "  %" << positiveStep << " = icmp sgt i64 %" << wideStep << ", 0\n";
    out << "  br i1 %" << positiveStep << ", label %" << positiveLabel << ", label %"
        << negativeLabel << "\n";

    out << positiveLabel << ":\n";
    const std::string positiveDifference = nextTemporary(state);
    const std::string positiveNonEmpty = nextTemporary(state);
    const std::string positiveBias = nextTemporary(state);
    const std::string positiveNumerator = nextTemporary(state);
    const std::string positiveQuotient = nextTemporary(state);
    const std::string positiveCount = nextTemporary(state);
    out << "  %" << positiveDifference << " = sub i64 %" << wideEnd << ", %"
        << wideStart << "\n";
    out << "  %" << positiveNonEmpty << " = icmp sgt i64 %" << positiveDifference
        << ", 0\n";
    out << "  %" << positiveBias << " = sub i64 %" << wideStep << ", 1\n";
    out << "  %" << positiveNumerator << " = add i64 %" << positiveDifference << ", %"
        << positiveBias << "\n";
    out << "  %" << positiveQuotient << " = sdiv i64 %" << positiveNumerator << ", %"
        << wideStep << "\n";
    out << "  %" << positiveCount << " = select i1 %" << positiveNonEmpty << ", i64 %"
        << positiveQuotient << ", i64 0\n";
    out << "  br label %" << countLabel << "\n";

    out << negativeLabel << ":\n";
    const std::string negativeStride = nextTemporary(state);
    const std::string negativeDifference = nextTemporary(state);
    const std::string negativeNonEmpty = nextTemporary(state);
    const std::string negativeBias = nextTemporary(state);
    const std::string negativeNumerator = nextTemporary(state);
    const std::string negativeQuotient = nextTemporary(state);
    const std::string negativeCount = nextTemporary(state);
    out << "  %" << negativeStride << " = sub i64 0, %" << wideStep << "\n";
    out << "  %" << negativeDifference << " = sub i64 %" << wideStart << ", %"
        << wideEnd << "\n";
    out << "  %" << negativeNonEmpty << " = icmp sgt i64 %" << negativeDifference
        << ", 0\n";
    out << "  %" << negativeBias << " = sub i64 %" << negativeStride << ", 1\n";
    out << "  %" << negativeNumerator << " = add i64 %" << negativeDifference << ", %"
        << negativeBias << "\n";
    out << "  %" << negativeQuotient << " = sdiv i64 %" << negativeNumerator << ", %"
        << negativeStride << "\n";
    out << "  %" << negativeCount << " = select i1 %" << negativeNonEmpty << ", i64 %"
        << negativeQuotient << ", i64 0\n";
    out << "  br label %" << countLabel << "\n";

    out << countLabel << ":\n";
    const std::string count = nextTemporary(state);
    const std::string tooLarge = nextTemporary(state);
    out << "  %" << count << " = phi i64 [ %" << positiveCount << ", %" << positiveLabel
        << " ], [ %" << negativeCount << ", %" << negativeLabel << " ]\n";
    out << "  %" << tooLarge << " = icmp sgt i64 %" << count << ", 2147483647\n";
    out << "  br i1 %" << tooLarge << ", label %" << tooLargeLabel << ", label %"
        << allocateLabel << "\n";
    out << tooLargeLabel << ":\n";
    out << "  call void @__scalanative_throw_array_range_too_large()\n";
    out << "  unreachable\n";

    out << allocateLabel << ":\n";
    const std::string length = nextTemporary(state);
    const std::string array = nextTemporary(state);
    out << "  %" << length << " = trunc i64 %" << count << " to i32\n";
    out << "  %" << array << " = call ptr @__scalanative_array_alloc(i32 %" << length
        << ", i64 4)\n";
    state.values[array] = "ptr";
    state.simpleTypes[array] = expectedType;

    const std::string index = nextTemporary(state);
    const std::string inBounds = nextTemporary(state);
    const std::string wideIndex = nextTemporary(state);
    const std::string offset = nextTemporary(state);
    const std::string wideElement = nextTemporary(state);
    const std::string element = nextTemporary(state);
    const std::string elements = nextTemporary(state);
    const std::string slot = nextTemporary(state);
    const std::string nextIndex = nextTemporary(state);
    out << "  br label %" << loopLabel << "\n";
    out << loopLabel << ":\n";
    out << "  %" << index << " = phi i32 [ 0, %" << allocateLabel << " ], [ %"
        << nextIndex << ", %" << latchLabel << " ]\n";
    out << "  %" << inBounds << " = icmp slt i32 %" << index << ", %" << length << "\n";
    out << "  br i1 %" << inBounds << ", label %" << bodyLabel << ", label %"
        << doneLabel << "\n";
    out << bodyLabel << ":\n";
    out << "  %" << wideIndex << " = sext i32 %" << index << " to i64\n";
    out << "  %" << offset << " = mul i64 %" << wideIndex << ", %" << wideStep << "\n";
    out << "  %" << wideElement << " = add i64 %" << wideStart << ", %" << offset
        << "\n";
    out << "  %" << element << " = trunc i64 %" << wideElement << " to i32\n";
    out << "  %" << elements << " = getelementptr i8, ptr %" << array << ", i64 "
        << ObjectHeaderSize + 8 << "\n";
    out << "  %" << slot << " = getelementptr i32, ptr %" << elements << ", i64 %"
        << wideIndex << "\n";
    out << "  store i32 %" << element << ", ptr %" << slot << "\n";
    out << "  br label %" << latchLabel << "\n";
    out << latchLabel << ":\n";
    out << "  %" << nextIndex << " = add i32 %" << index << ", 1\n";
    out << "  br label %" << loopLabel << "\n";
    out << doneLabel << ":\n";
    state.currentBlockLabel = doneLabel;
    supported = true;
    return "%" + array;
  }
  if (isRuntimeArrayConcat) {
    const std::size_t arrayCount = signature->parameterTypes.size();
    if (value.operands.size() != arrayCount + 1 ||
        expectedType != signature->returnType ||
        !std::all_of(signature->parameterTypes.begin(), signature->parameterTypes.end(),
                     [&](const std::string& type) { return type == expectedType; })) {
      supported = false;
      return defaultValue(expectedType);
    }
    const std::string elementType = arrayElementTypeName(expectedType);
    const std::size_t elementSize = runtimeArrayElementSize(elementType);
    if (elementType.empty() || elementSize == 0) {
      supported = false;
      return defaultValue(expectedType);
    }

    std::vector<std::string> arrays;
    arrays.reserve(arrayCount);
    for (std::size_t i = 1; i < value.operands.size(); ++i) {
      bool arraySupported = false;
      const std::string array =
          lowerValue(value.operands[i], expectedType, state, out, arraySupported);
      if (!arraySupported) {
        supported = false;
        return defaultValue(expectedType);
      }
      arrays.push_back(array);
    }

    std::vector<std::string> lengths;
    lengths.reserve(arrayCount);
    for (const std::string& array : arrays) {
      const std::string length = nextTemporary(state);
      out << "  %" << length << " = call i64 @__scalanative_array_length(ptr " << array
          << ")\n";
      state.values[length] = "i64";
      state.simpleTypes[length] = "Long";
      lengths.push_back(length);
    }

    std::string totalLength = "0";
    for (const std::string& length : lengths) {
      const std::string nextLength = nextTemporary(state);
      out << "  %" << nextLength << " = add i64 " << totalLength << ", %" << length
          << "\n";
      totalLength = "%" + nextLength;
    }

    const std::string tag = nextTemporary(state);
    const std::string tooLarge = nextTemporary(state);
    const std::string tooLargeLabel = "array_concat_too_large_" + tag;
    const std::string allocateLabel = "array_concat_allocate_" + tag;
    out << "  %" << tooLarge << " = icmp ugt i64 " << totalLength << ", 2147483647\n";
    out << "  br i1 %" << tooLarge << ", label %" << tooLargeLabel << ", label %"
        << allocateLabel << "\n";
    out << tooLargeLabel << ":\n";
    out << "  call void @__scalanative_throw_array_concat_too_large()\n";
    out << "  unreachable\n";

    out << allocateLabel << ":\n";
    const std::string narrowLength = nextTemporary(state);
    const std::string result = nextTemporary(state);
    out << "  %" << narrowLength << " = trunc i64 " << totalLength << " to i32\n";
    out << "  %" << result << " = call ptr @__scalanative_array_alloc(i32 %"
        << narrowLength << ", i64 " << elementSize << ")\n";
    state.values[result] = "ptr";
    state.simpleTypes[result] = expectedType;

    const std::string destinationElements = nextTemporary(state);
    out << "  %" << destinationElements << " = getelementptr i8, ptr %" << result
        << ", i64 " << ObjectHeaderSize + 8 << "\n";
    std::string destinationElementOffset = "0";
    for (std::size_t i = 0; i < arrays.size(); ++i) {
      const std::string sourceElements = nextTemporary(state);
      const std::string destinationByteOffset = nextTemporary(state);
      const std::string destination = nextTemporary(state);
      const std::string byteCount = nextTemporary(state);
      out << "  %" << sourceElements << " = getelementptr i8, ptr " << arrays[i]
          << ", i64 " << ObjectHeaderSize + 8 << "\n";
      out << "  %" << destinationByteOffset << " = mul i64 " << destinationElementOffset
          << ", " << elementSize << "\n";
      out << "  %" << destination << " = getelementptr i8, ptr %" << destinationElements
          << ", i64 %" << destinationByteOffset << "\n";
      out << "  %" << byteCount << " = mul i64 %" << lengths[i] << ", " << elementSize
          << "\n";
      out << "  call void @llvm.memcpy.p0.p0.i64(ptr %" << destination << ", ptr %"
          << sourceElements << ", i64 %" << byteCount << ", i1 false)\n";
      const std::string nextOffset = nextTemporary(state);
      out << "  %" << nextOffset << " = add i64 " << destinationElementOffset << ", %"
          << lengths[i] << "\n";
      destinationElementOffset = "%" + nextOffset;
    }
    state.currentBlockLabel = allocateLabel;
    supported = true;
    return "%" + result;
  }
  if (isRuntimeArrayFill) {
    const std::size_t dimensions =
        signature->parameterTypes.empty() ? 0 : signature->parameterTypes.size() - 1;
    if (dimensions == 0 || value.operands.size() != dimensions + 2 ||
        expectedType != signature->returnType) {
      supported = false;
      return defaultValue(expectedType);
    }
    if (!std::all_of(signature->parameterTypes.begin(),
                     signature->parameterTypes.begin() +
                         static_cast<std::ptrdiff_t>(dimensions),
                     [](const std::string& type) { return type == "Int"; })) {
      supported = false;
      return defaultValue(expectedType);
    }

    std::string elementType = signature->returnType;
    for (std::size_t i = 0; i < dimensions; ++i) {
      elementType = arrayElementTypeName(elementType);
      if (elementType.empty()) {
        supported = false;
        return defaultValue(expectedType);
      }
    }
    if (runtimeArrayElementSize(elementType) == 0 ||
        signature->parameterTypes.back() != elementType) {
      supported = false;
      return defaultValue(expectedType);
    }

    std::vector<std::string> lengths;
    lengths.reserve(dimensions);
    for (std::size_t i = 0; i < dimensions; ++i) {
      bool lengthSupported = false;
      const std::string length =
          lowerValue(value.operands[i + 1], "Int", state, out, lengthSupported);
      if (!lengthSupported) {
        supported = false;
        return defaultValue(expectedType);
      }
      lengths.push_back(length);
    }

    supported = true;
    const std::string array =
        emitArrayFillAllocation(0, lengths, signature->returnType,
                                value.operands[dimensions + 1], state, out, supported);
    return supported ? array : defaultValue(expectedType);
  }
  if (isRuntimeArrayOfDim) {
    const std::size_t dimensions = signature->parameterTypes.size();
    if (dimensions < 2 || value.operands.size() != dimensions + 1 ||
        expectedType != signature->returnType ||
        !std::all_of(
            signature->parameterTypes.begin(), signature->parameterTypes.end(),
            [](const std::string& type) { return type == "Int"; })) {
      supported = false;
      return defaultValue(expectedType);
    }

    std::string baseElementType = signature->returnType;
    for (std::size_t i = 0; i < dimensions; ++i) {
      baseElementType = arrayElementTypeName(baseElementType);
      if (baseElementType.empty()) {
        supported = false;
        return defaultValue(expectedType);
      }
    }
    if (runtimeArrayElementSize(baseElementType) == 0) {
      supported = false;
      return defaultValue(expectedType);
    }

    std::vector<std::string> lengths;
    lengths.reserve(dimensions);
    for (std::size_t i = 0; i < dimensions; ++i) {
      bool lengthSupported = false;
      const std::string length =
          lowerValue(value.operands[i + 1], "Int", state, out, lengthSupported);
      if (!lengthSupported) {
        supported = false;
        return defaultValue(expectedType);
      }
      lengths.push_back(length);
    }
    for (const std::string& length : lengths) {
      emitArrayDimensionCheck(length, state, out);
    }

    supported = true;
    return emitArrayOfDimAllocation(0, lengths, signature->returnType, state, out);
  }
  if (isRuntimeByteBufferWrap) {
    if (value.operands.size() != 2 ||
        signature->parameterTypes != std::vector<std::string>{"Array [ Byte ]"} ||
        signature->returnType != support::StdNames::JavaNioByteBuffer ||
        expectedType != signature->returnType) {
      supported = false;
      return defaultValue(expectedType);
    }
    bool storageSupported = false;
    const std::string storage =
        lowerValue(value.operands[1], "Array [ Byte ]", state, out, storageSupported);
    if (!storageSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    const std::string result = nextTemporary(state);
    out << "  %" << result << " = call ptr @__scalanative_byte_buffer_wrap(ptr "
        << storage << ")\n";
    state.values[result] = "ptr";
    state.simpleTypes[result] = std::string(support::StdNames::JavaNioByteBuffer);
    supported = true;
    return "%" + result;
  }
  if (isRuntimeByteBufferOperation) {
    const bool takesValue = isRuntimeByteBufferSetPosition ||
                            isRuntimeByteBufferSetLimit || isRuntimeByteBufferPut;
    const bool returnsBuffer = takesValue || isRuntimeByteBufferClear ||
                               isRuntimeByteBufferFlip || isRuntimeByteBufferRewind;
    const bool returnsBoolean = isRuntimeByteBufferHasRemaining;
    const bool returnsByte = isRuntimeByteBufferGet;
    const std::string valueType = isRuntimeByteBufferPut ? "Byte" : "Int";
    const std::vector<std::string> expectedParameters =
        takesValue ? std::vector<std::string>{std::string(
                                                  support::StdNames::JavaNioByteBuffer),
                                              valueType}
                   : std::vector<std::string>{
                         std::string(support::StdNames::JavaNioByteBuffer)};
    const std::string expectedReturn =
        returnsBuffer ? std::string(support::StdNames::JavaNioByteBuffer)
                      : (returnsBoolean ? "Boolean" : (returnsByte ? "Byte" : "Int"));
    if (value.operands.size() != expectedParameters.size() + 1 ||
        signature->parameterTypes != expectedParameters ||
        signature->returnType != expectedReturn || expectedType != expectedReturn) {
      supported = false;
      return defaultValue(expectedType);
    }

    bool bufferSupported = false;
    const std::string buffer =
        lowerValue(value.operands[1], std::string(support::StdNames::JavaNioByteBuffer),
                   state, out, bufferSupported);
    if (!bufferSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    std::string helper;
    if (isRuntimeByteBufferCapacity) {
      helper = "__scalanative_byte_buffer_capacity";
    } else if (isRuntimeByteBufferPosition) {
      helper = "__scalanative_byte_buffer_position";
    } else if (isRuntimeByteBufferSetPosition) {
      helper = "__scalanative_byte_buffer_set_position";
    } else if (isRuntimeByteBufferLimit) {
      helper = "__scalanative_byte_buffer_limit";
    } else if (isRuntimeByteBufferSetLimit) {
      helper = "__scalanative_byte_buffer_set_limit";
    } else if (isRuntimeByteBufferRemaining) {
      helper = "__scalanative_byte_buffer_remaining";
    } else if (isRuntimeByteBufferHasRemaining) {
      helper = "__scalanative_byte_buffer_has_remaining";
    } else if (isRuntimeByteBufferGet) {
      helper = "__scalanative_byte_buffer_get";
    } else if (isRuntimeByteBufferPut) {
      helper = "__scalanative_byte_buffer_put";
    } else if (isRuntimeByteBufferClear) {
      helper = "__scalanative_byte_buffer_clear";
    } else if (isRuntimeByteBufferFlip) {
      helper = "__scalanative_byte_buffer_flip";
    } else {
      helper = "__scalanative_byte_buffer_rewind";
    }

    std::string valueArgument;
    if (takesValue) {
      bool valueSupported = false;
      valueArgument =
          lowerValue(value.operands[2], valueType, state, out, valueSupported);
      if (!valueSupported) {
        supported = false;
        return defaultValue(expectedType);
      }
    }
    const std::string result = nextTemporary(state);
    const std::string llvmReturn =
        returnsBuffer ? "ptr" : (returnsBoolean ? "i1" : (returnsByte ? "i8" : "i32"));
    out << "  %" << result << " = call " << llvmReturn << " @" << helper << "(ptr "
        << buffer;
    if (takesValue) {
      out << ", " << llvmType(valueType) << " " << valueArgument;
    }
    out << ")\n";
    state.values[result] = llvmReturn;
    state.simpleTypes[result] = expectedReturn;
    supported = true;
    return "%" + result;
  }
  if (isRuntimeNativeBytesGetShort || isRuntimeNativeBytesPutShort) {
    const std::vector<std::string> expectedParameters =
        isRuntimeNativeBytesGetShort
            ? std::vector<std::string>{"Array [ Byte ]", "Int"}
            : std::vector<std::string>{"Array [ Byte ]", "Int", "Short"};
    const std::string expectedReturn = isRuntimeNativeBytesGetShort ? "Short" : "Unit";
    if (value.operands.size() != expectedParameters.size() + 1 ||
        signature->parameterTypes != expectedParameters ||
        signature->returnType != expectedReturn || expectedType != expectedReturn) {
      supported = false;
      return defaultValue(expectedType);
    }

    bool bytesSupported = false;
    bool indexSupported = false;
    const std::string bytes =
        lowerValue(value.operands[1], "Array [ Byte ]", state, out, bytesSupported);
    const std::string index =
        lowerValue(value.operands[2], "Int", state, out, indexSupported);
    if (!bytesSupported || !indexSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    const std::string littleEndian =
        isRuntimeNativeBytesLittleEndian ? "true" : "false";
    if (isRuntimeNativeBytesGetShort) {
      const std::string result = nextTemporary(state);
      out << "  %" << result << " = call i16 @__scalanative_native_bytes_get_short(ptr "
          << bytes << ", i32 " << index << ", i1 " << littleEndian << ")\n";
      state.values[result] = "i16";
      state.simpleTypes[result] = "Short";
      supported = true;
      return "%" + result;
    }

    bool valueSupported = false;
    const std::string stored =
        lowerValue(value.operands[3], "Short", state, out, valueSupported);
    if (!valueSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    out << "  call void @__scalanative_native_bytes_put_short(ptr " << bytes << ", i32 "
        << index << ", i16 " << stored << ", i1 " << littleEndian << ")\n";
    supported = true;
    return {};
  }
  if (isRuntimeZoneAllocBytes) {
    if (value.operands.size() != 2 ||
        signature->parameterTypes != std::vector<std::string>{"Int"} ||
        signature->returnType != "Array [ Byte ]" ||
        expectedType != signature->returnType) {
      supported = false;
      return defaultValue(expectedType);
    }
    bool lengthSupported = false;
    const std::string length =
        lowerValue(value.operands[1], "Int", state, out, lengthSupported);
    if (!lengthSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    emitArrayDimensionCheck(length, state, out);

    const std::string wideLength = nextTemporary(state);
    const std::string allocationSize = nextTemporary(state);
    const std::string zone = nextTemporary(state);
    const std::string array = nextTemporary(state);
    const std::string lengthSlot = nextTemporary(state);
    out << "  %" << wideLength << " = zext i32 " << length << " to i64\n";
    out << "  %" << allocationSize << " = add i64 %" << wideLength << ", "
        << ObjectHeaderSize + 8 << "\n";
    out << "  %" << zone << " = load ptr, ptr @__scalanative_current_zone\n";
    out << "  %" << array << " = call ptr @__scalanative_arena_alloc(ptr %" << zone
        << ", i64 %" << allocationSize << ", ptr null)\n";
    out << "  %" << lengthSlot << " = getelementptr i8, ptr %" << array << ", i64 "
        << ObjectHeaderSize << "\n";
    out << "  store i64 %" << wideLength << ", ptr %" << lengthSlot << "\n";
    state.values[wideLength] = "i64";
    state.simpleTypes[wideLength] = "Long";
    state.values[allocationSize] = "i64";
    state.simpleTypes[allocationSize] = "Long";
    state.values[zone] = "ptr";
    state.simpleTypes[zone] = "Object";
    state.values[array] = "ptr";
    state.simpleTypes[array] = "Array [ Byte ]";
    state.values[lengthSlot] = "ptr";
    state.simpleTypes[lengthSlot] = "Object";
    supported = true;
    return "%" + array;
  }
  if (isRuntimeArrayAlloc || isRuntimeIntArrayAlloc || isRuntimeByteArrayAlloc ||
      isRuntimeShortArrayAlloc || isRuntimeBooleanArrayAlloc ||
      isRuntimeLongArrayAlloc || isRuntimeDoubleArrayAlloc ||
      isRuntimeFloatArrayAlloc || isRuntimeCharArrayAlloc ||
      isRuntimeReferenceArrayAlloc) {
    const std::string arrayType = isRuntimeReferenceArrayAlloc ? signature->returnType
                                  : isRuntimeArrayAlloc        ? "Array [ String ]"
                                  : isRuntimeIntArrayAlloc     ? "Array [ Int ]"
                                  : isRuntimeByteArrayAlloc    ? "Array [ Byte ]"
                                  : isRuntimeShortArrayAlloc   ? "Array [ Short ]"
                                  : isRuntimeBooleanArrayAlloc ? "Array [ Boolean ]"
                                  : isRuntimeDoubleArrayAlloc  ? "Array [ Double ]"
                                  : isRuntimeFloatArrayAlloc   ? "Array [ Float ]"
                                  : isRuntimeCharArrayAlloc    ? "Array [ Char ]"
                                                               : "Array [ Long ]";
    const std::size_t elementSize =
        isRuntimeByteArrayAlloc || isRuntimeBooleanArrayAlloc ? 1
        : isRuntimeShortArrayAlloc                            ? 2
        : isRuntimeIntArrayAlloc || isRuntimeFloatArrayAlloc || isRuntimeCharArrayAlloc
            ? 4
            : 8;
    if (value.operands.size() != 2 || expectedType != arrayType) {
      supported = false;
      return defaultValue(expectedType);
    }
    bool lengthSupported = false;
    const std::string length =
        lowerValue(value.operands[1], "Int", state, out, lengthSupported);
    if (!lengthSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    const std::string array = nextTemporary(state);
    out << "  %" << array << " = call ptr @__scalanative_array_alloc(i32 " << length
        << ", i64 " << elementSize << ")\n";
    state.values[array] = "ptr";
    state.simpleTypes[array] = arrayType;
    supported = true;
    return "%" + array;
  }
  if (isRuntimeArrayClone || isRuntimeIntArrayClone || isRuntimeByteArrayClone ||
      isRuntimeShortArrayClone || isRuntimeBooleanArrayClone ||
      isRuntimeLongArrayClone || isRuntimeDoubleArrayClone ||
      isRuntimeFloatArrayClone || isRuntimeCharArrayClone ||
      isRuntimeReferenceArrayClone) {
    const std::string arrayType = isRuntimeReferenceArrayClone ? signature->returnType
                                  : isRuntimeArrayClone        ? "Array [ String ]"
                                  : isRuntimeIntArrayClone     ? "Array [ Int ]"
                                  : isRuntimeByteArrayClone    ? "Array [ Byte ]"
                                  : isRuntimeShortArrayClone   ? "Array [ Short ]"
                                  : isRuntimeBooleanArrayClone ? "Array [ Boolean ]"
                                  : isRuntimeDoubleArrayClone  ? "Array [ Double ]"
                                  : isRuntimeFloatArrayClone   ? "Array [ Float ]"
                                  : isRuntimeCharArrayClone    ? "Array [ Char ]"
                                                               : "Array [ Long ]";
    const std::size_t elementSize =
        isRuntimeByteArrayClone || isRuntimeBooleanArrayClone ? 1
        : isRuntimeShortArrayClone                            ? 2
        : isRuntimeIntArrayClone || isRuntimeFloatArrayClone || isRuntimeCharArrayClone
            ? 4
            : 8;
    if (value.operands.size() != 2 || expectedType != arrayType ||
        signature->parameterTypes.size() != 1 ||
        signature->parameterTypes.front() != arrayType) {
      supported = false;
      return defaultValue(expectedType);
    }
    bool arraySupported = false;
    const std::string source =
        lowerValue(value.operands[1], arrayType, state, out, arraySupported);
    if (!arraySupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    const std::string clone = nextTemporary(state);
    out << "  %" << clone << " = call ptr @__scalanative_array_clone(ptr " << source
        << ", i64 " << elementSize << ")\n";
    state.values[clone] = "ptr";
    state.simpleTypes[clone] = arrayType;
    supported = true;
    return "%" + clone;
  }
  if (isRuntimeArrayCopy || isRuntimeReferenceArrayCopy) {
    if (value.operands.size() != 6 || expectedType != "Unit" ||
        signature->parameterTypes.size() != 5 ||
        signature->parameterTypes[1] != "Int" ||
        signature->parameterTypes[3] != "Int" ||
        signature->parameterTypes[4] != "Int") {
      supported = false;
      return defaultValue(expectedType);
    }
    const std::string& sourceArrayType = signature->parameterTypes[0];
    const std::string& destinationArrayType = signature->parameterTypes[2];
    const std::string sourceElementType = arrayElementTypeName(sourceArrayType);
    const std::string destinationElementType =
        arrayElementTypeName(destinationArrayType);
    const std::size_t sourceElementSize = runtimeArrayElementSize(sourceElementType);
    const std::size_t destinationElementSize =
        runtimeArrayElementSize(destinationElementType);
    const bool crossReferenceCopy = sourceArrayType != destinationArrayType;
    if (sourceElementSize == 0 || destinationElementSize == 0 ||
        sourceElementSize != destinationElementSize ||
        crossReferenceCopy != isRuntimeReferenceArrayCopy) {
      supported = false;
      return defaultValue(expectedType);
    }

    std::string targetDescriptor = "null";
    bool checkElements = false;
    if (crossReferenceCopy) {
      const auto descriptorBackedReference = [&](const std::string& elementType) {
        if (elementType == "Object") {
          return true;
        }
        if (!arrayElementTypeName(elementType).empty() || elementType == "String" ||
            elementType == "Byte" || elementType == "Short" || elementType == "Int" ||
            elementType == "Boolean" || elementType == "Long" ||
            elementType == "Double" || elementType == "Float" ||
            elementType == "Char" || state.classLayouts == nullptr) {
          return false;
        }
        const std::string resolved = resolveGlobalName(elementType, state);
        return !resolved.empty() && state.classLayouts->contains(resolved);
      };
      if (sourceElementSize != 8 || !descriptorBackedReference(sourceElementType) ||
          !descriptorBackedReference(destinationElementType)) {
        supported = false;
        return defaultValue(expectedType);
      }
      if (destinationElementType != "Object") {
        const std::string destinationName =
            resolveGlobalName(destinationElementType, state);
        auto destinationLayout = state.classLayouts->find(destinationName);
        if (destinationLayout == state.classLayouts->end() ||
            destinationLayout->second.descriptorName.empty()) {
          supported = false;
          return defaultValue(expectedType);
        }
        targetDescriptor = "@" + destinationLayout->second.descriptorName;
        checkElements = true;
      }
    }

    bool sourceSupported = false;
    const std::string source =
        lowerValue(value.operands[1], sourceArrayType, state, out, sourceSupported);
    bool sourcePositionSupported = false;
    const std::string sourcePosition =
        lowerValue(value.operands[2], "Int", state, out, sourcePositionSupported);
    bool destinationSupported = false;
    const std::string destination = lowerValue(value.operands[3], destinationArrayType,
                                               state, out, destinationSupported);
    bool destinationPositionSupported = false;
    const std::string destinationPosition =
        lowerValue(value.operands[4], "Int", state, out, destinationPositionSupported);
    bool lengthSupported = false;
    const std::string length =
        lowerValue(value.operands[5], "Int", state, out, lengthSupported);
    if (!sourceSupported || !sourcePositionSupported || !destinationSupported ||
        !destinationPositionSupported || !lengthSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    out << "  call void @__scalanative_array_copy(ptr " << source << ", i32 "
        << sourcePosition << ", ptr " << destination << ", i32 " << destinationPosition
        << ", i32 " << length << ", i64 " << sourceElementSize << ", ptr "
        << targetDescriptor << ", i1 " << (checkElements ? "true" : "false") << ")\n";
    supported = true;
    return {};
  }
  if (isRuntimeArrayLength || isRuntimeIntArrayLength || isRuntimeByteArrayLength ||
      isRuntimeShortArrayLength || isRuntimeBooleanArrayLength ||
      isRuntimeLongArrayLength || isRuntimeDoubleArrayLength ||
      isRuntimeFloatArrayLength || isRuntimeCharArrayLength ||
      isRuntimeReferenceArrayLength) {
    if (value.operands.size() != 2 || expectedType != "Int") {
      supported = false;
      return defaultValue(expectedType);
    }
    bool arraySupported = false;
    const std::string arrayType =
        isRuntimeReferenceArrayLength && !signature->parameterTypes.empty()
            ? signature->parameterTypes.front()
        : isRuntimeArrayLength        ? "Array [ String ]"
        : isRuntimeIntArrayLength     ? "Array [ Int ]"
        : isRuntimeByteArrayLength    ? "Array [ Byte ]"
        : isRuntimeShortArrayLength   ? "Array [ Short ]"
        : isRuntimeBooleanArrayLength ? "Array [ Boolean ]"
        : isRuntimeDoubleArrayLength  ? "Array [ Double ]"
        : isRuntimeFloatArrayLength   ? "Array [ Float ]"
        : isRuntimeCharArrayLength    ? "Array [ Char ]"
                                      : "Array [ Long ]";
    const std::string array =
        lowerValue(value.operands[1], arrayType, state, out, arraySupported);
    if (!arraySupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    const std::string wideLength = nextTemporary(state);
    out << "  %" << wideLength << " = call i64 @__scalanative_array_length(ptr "
        << array << ")\n";
    state.values[wideLength] = "i64";
    state.simpleTypes[wideLength] = "Long";
    const std::string length = nextTemporary(state);
    out << "  %" << length << " = trunc i64 %" << wideLength << " to i32\n";
    state.values[length] = "i32";
    state.simpleTypes[length] = "Int";
    supported = true;
    return "%" + length;
  }
  if (isRuntimeArrayApply || isRuntimeIntArrayApply || isRuntimeByteArrayApply ||
      isRuntimeShortArrayApply || isRuntimeBooleanArrayApply ||
      isRuntimeLongArrayApply || isRuntimeDoubleArrayApply ||
      isRuntimeFloatArrayApply || isRuntimeCharArrayApply ||
      isRuntimeReferenceArrayApply) {
    const bool stringElements = isRuntimeArrayApply;
    const bool intElements = isRuntimeIntArrayApply;
    const bool byteElements = isRuntimeByteArrayApply;
    const bool shortElements = isRuntimeShortArrayApply;
    const bool booleanElements = isRuntimeBooleanArrayApply;
    const bool doubleElements = isRuntimeDoubleArrayApply;
    const bool floatElements = isRuntimeFloatArrayApply;
    const bool charElements = isRuntimeCharArrayApply;
    const bool referenceElements = isRuntimeReferenceArrayApply;
    const std::string elementType =
        referenceElements && !signature->parameterTypes.empty()
            ? arrayElementTypeName(signature->parameterTypes.front())
        : stringElements  ? "String"
        : byteElements    ? "Byte"
        : shortElements   ? "Short"
        : intElements     ? "Int"
        : booleanElements ? "Boolean"
        : doubleElements  ? "Double"
        : floatElements   ? "Float"
        : charElements    ? "Char"
                          : "Long";
    const std::string arrayType = "Array [ " + elementType + " ]";
    const char* elementLlvmType = stringElements || referenceElements ? "ptr"
                                  : byteElements                      ? "i8"
                                  : shortElements                     ? "i16"
                                  : intElements                       ? "i32"
                                  : booleanElements                   ? "i1"
                                  : doubleElements                    ? "double"
                                  : floatElements                     ? "float"
                                  : charElements                      ? "i32"
                                                                      : "i64";
    const char* elementKind = stringElements      ? "string"
                              : byteElements      ? "byte"
                              : shortElements     ? "short"
                              : intElements       ? "int"
                              : booleanElements   ? "boolean"
                              : doubleElements    ? "double"
                              : floatElements     ? "float"
                              : charElements      ? "char"
                              : referenceElements ? "reference"
                                                  : "long";
    if (elementType.empty() || value.operands.size() != 3 ||
        expectedType != elementType) {
      supported = false;
      return defaultValue(expectedType);
    }
    bool arraySupported = false;
    const std::string array =
        lowerValue(value.operands[1], arrayType, state, out, arraySupported);
    bool indexSupported = false;
    const std::string index =
        lowerValue(value.operands[2], "Int", state, out, indexSupported);
    if (!arraySupported || !indexSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    const std::string element = nextTemporary(state);
    out << "  %" << element << " = call " << elementLlvmType << " @__scalanative_array_"
        << elementKind << "_at(ptr " << array << ", i32 " << index << ")\n";
    state.values[element] = elementLlvmType;
    state.simpleTypes[element] = elementType;
    supported = true;
    return "%" + element;
  }
  if (isRuntimeArrayUpdate || isRuntimeIntArrayUpdate || isRuntimeByteArrayUpdate ||
      isRuntimeShortArrayUpdate || isRuntimeBooleanArrayUpdate ||
      isRuntimeLongArrayUpdate || isRuntimeDoubleArrayUpdate ||
      isRuntimeFloatArrayUpdate || isRuntimeCharArrayUpdate ||
      isRuntimeReferenceArrayUpdate) {
    const bool stringElements = isRuntimeArrayUpdate;
    const bool intElements = isRuntimeIntArrayUpdate;
    const bool byteElements = isRuntimeByteArrayUpdate;
    const bool shortElements = isRuntimeShortArrayUpdate;
    const bool booleanElements = isRuntimeBooleanArrayUpdate;
    const bool doubleElements = isRuntimeDoubleArrayUpdate;
    const bool floatElements = isRuntimeFloatArrayUpdate;
    const bool charElements = isRuntimeCharArrayUpdate;
    const bool referenceElements = isRuntimeReferenceArrayUpdate;
    const std::string elementType =
        referenceElements && !signature->parameterTypes.empty()
            ? arrayElementTypeName(signature->parameterTypes.front())
        : stringElements  ? "String"
        : byteElements    ? "Byte"
        : shortElements   ? "Short"
        : intElements     ? "Int"
        : booleanElements ? "Boolean"
        : doubleElements  ? "Double"
        : floatElements   ? "Float"
        : charElements    ? "Char"
                          : "Long";
    const std::string arrayType = "Array [ " + elementType + " ]";
    const char* elementLlvmType = stringElements || referenceElements ? "ptr"
                                  : byteElements                      ? "i8"
                                  : shortElements                     ? "i16"
                                  : intElements                       ? "i32"
                                  : booleanElements                   ? "i1"
                                  : doubleElements                    ? "double"
                                  : floatElements                     ? "float"
                                  : charElements                      ? "i32"
                                                                      : "i64";
    const char* elementKind = stringElements      ? "string"
                              : byteElements      ? "byte"
                              : shortElements     ? "short"
                              : intElements       ? "int"
                              : booleanElements   ? "boolean"
                              : doubleElements    ? "double"
                              : floatElements     ? "float"
                              : charElements      ? "char"
                              : referenceElements ? "reference"
                                                  : "long";
    if (elementType.empty() || value.operands.size() != 4 || expectedType != "Unit") {
      supported = false;
      return defaultValue(expectedType);
    }
    bool arraySupported = false;
    const std::string array =
        lowerValue(value.operands[1], arrayType, state, out, arraySupported);
    bool indexSupported = false;
    const std::string index =
        lowerValue(value.operands[2], "Int", state, out, indexSupported);
    bool valueSupported = false;
    const std::string assigned =
        lowerValue(value.operands[3], elementType, state, out, valueSupported);
    if (!arraySupported || !indexSupported || !valueSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    out << "  call void @__scalanative_array_" << elementKind << "_set(ptr " << array
        << ", i32 " << index << ", " << elementLlvmType << ' ' << assigned << ")\n";
    supported = true;
    return {};
  }
  if (!isRuntimePrintln && isStackSuperSelect(value.operands.front())) {
    std::vector<nir::Value> arguments;
    for (std::size_t i = 1; i < value.operands.size(); ++i) {
      arguments.push_back(value.operands[i]);
    }
    return lowerStackSuperDispatch(value.operands.front(), arguments, *signature,
                                   expectedType, state, out, supported);
  }
  if (!isRuntimePrintln &&
      shouldUseVirtualDispatch(value.operands.front(), target, state)) {
    std::vector<nir::Value> arguments;
    for (std::size_t i = 1; i < value.operands.size(); ++i) {
      arguments.push_back(value.operands[i]);
    }
    return lowerVirtualDispatch(value.operands.front(), arguments, target, *signature,
                                expectedType, state, out, supported);
  }

  std::vector<std::string> loweredArguments;
  std::vector<std::string> argumentSimpleTypes;
  std::size_t implicitReceiverCount = 0;
  std::string implicitReceiver;
  std::string explicitReceiver;
  const std::string targetOwner = ownerNameOf(target);
  bool isModuleMember = false;
  bool isInstanceMember = false;
  if (state.classLayouts != nullptr) {
    auto targetOwnerLayout = state.classLayouts->find(targetOwner);
    isModuleMember = targetOwnerLayout != state.classLayouts->end() &&
                     targetOwnerLayout->second.isModule;
    isInstanceMember = targetOwnerLayout != state.classLayouts->end() &&
                       !targetOwnerLayout->second.isModule;
  }
  if (value.operands.front().kind == nir::ValueKind::Select &&
      !signature->parameterTypes.empty() && !isModuleMember) {
    const nir::Value& callee = value.operands.front();
    if (callee.operands.size() != 1) {
      supported = false;
      return defaultValue(expectedType);
    }
    bool receiverSupported = false;
    const std::string receiver =
        lowerValue(callee.operands.front(), signature->parameterTypes.front(), state,
                   out, receiverSupported);
    if (!receiverSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    loweredArguments.push_back(llvmType(signature->parameterTypes.front()) + " " +
                               receiver);
    argumentSimpleTypes.push_back(signature->parameterTypes.front());
    implicitReceiverCount = 1;
    implicitReceiver = receiver;
  }

  for (std::size_t i = 1; i < value.operands.size(); ++i) {
    const std::size_t parameterIndex = i - 1;
    const std::size_t signatureParameterIndex = parameterIndex + implicitReceiverCount;
    const std::string argumentType =
        isRuntimePrintln ||
                signatureParameterIndex >= signature->parameterTypes.size() ||
                signature->parameterTypes[signatureParameterIndex] == "Unknown"
            ? inferSimpleType(value.operands[i], state)
            : signature->parameterTypes[signatureParameterIndex];
    if (isRuntimePrintln && argumentType == "Unit") {
      loweredArguments.push_back({});
      argumentSimpleTypes.push_back(argumentType);
      continue;
    }
    bool argumentSupported = false;
    const std::string loweredArgument =
        lowerValue(value.operands[i], argumentType, state, out, argumentSupported);
    if (!argumentSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    loweredArguments.push_back(llvmType(argumentType) + " " + loweredArgument);
    argumentSimpleTypes.push_back(argumentType);
    if (implicitReceiverCount == 0 && isInstanceMember && i == 1) {
      explicitReceiver = loweredArgument;
    }
  }

  if (isRuntimeAssert || isRuntimeAssume || isRuntimeRequire) {
    if (loweredArguments.size() != 1 || argumentSimpleTypes.front() != "Boolean") {
      supported = false;
      return defaultValue(expectedType);
    }
    emitSourceLocation(value.span, state, out);
    const std::string_view helper = isRuntimeAssert   ? "__scalanative_assert"
                                    : isRuntimeAssume ? "__scalanative_assume"
                                                      : "__scalanative_require";
    out << "  call void @" << helper << '(' << loweredArguments.front() << ")\n";
    supported = true;
    return {};
  }

  if (isRuntimePrintln) {
    if (loweredArguments.size() != 1) {
      supported = false;
      return defaultValue(expectedType);
    }
    emitSourceLocation(value.span, state, out);
    if (argumentSimpleTypes.front() == "String") {
      out << "  call i32 @puts(" << loweredArguments.front() << ")\n";
    } else if (argumentSimpleTypes.front() == "Unit") {
      out << "  call i32 @puts(ptr getelementptr inbounds ([3 x i8], ptr @.str.unit, "
             "i64 0, i64 0))\n";
    } else if (argumentSimpleTypes.front() == "Symbol") {
      out << "  call i32 @puts(" << loweredArguments.front() << ")\n";
    } else if (argumentSimpleTypes.front() == "Null") {
      out << "  call i32 @puts(ptr getelementptr inbounds ([5 x i8], ptr @.str.null, "
             "i64 0, i64 0))\n";
    } else if (isReferenceType(argumentSimpleTypes.front())) {
      const std::string converted = nextTemporary(state);
      out << "  %" << converted << " = call ptr @__scalanative_any_to_string("
          << loweredArguments.front() << ")\n";
      out << "  call i32 @puts(ptr %" << converted << ")\n";
    } else if (argumentSimpleTypes.front() == "Byte" ||
               argumentSimpleTypes.front() == "Short") {
      const std::string widened = nextTemporary(state);
      out << "  %" << widened << " = sext " << loweredArguments.front() << " to i32\n";
      state.values[widened] = "i32";
      state.simpleTypes[widened] = "Int";
      out << "  call i32 (ptr, ...) @printf(ptr getelementptr inbounds ([4 x i8], "
             "ptr @.fmt.int, i64 0, i64 0), i32 %"
          << widened << ")\n";
    } else if (argumentSimpleTypes.front() == "Int") {
      out << "  call i32 (ptr, ...) @printf(ptr getelementptr inbounds ([4 x i8], "
             "ptr @.fmt.int, i64 0, i64 0), "
          << loweredArguments.front() << ")\n";
    } else if (argumentSimpleTypes.front() == "Boolean") {
      const std::string text = nextTemporary(state);
      out << "  %" << text << " = select i1 " << loweredArguments.front().substr(3)
          << ", ptr getelementptr inbounds ([5 x i8], ptr @.str.boolean.true, i64 "
             "0, i64 0), ptr getelementptr inbounds ([6 x i8], ptr "
             "@.str.boolean.false, i64 0, i64 0)\n";
      state.values[text] = "ptr";
      state.simpleTypes[text] = "String";
      out << "  call i32 @puts(ptr %" << text << ")\n";
    } else if (argumentSimpleTypes.front() == "Long") {
      out << "  call i32 (ptr, ...) @printf(ptr getelementptr inbounds ([6 x i8], "
             "ptr @.fmt.long, i64 0, i64 0), "
          << loweredArguments.front() << ")\n";
    } else if (argumentSimpleTypes.front() == "Float") {
      const std::string promoted = nextTemporary(state);
      out << "  %" << promoted << " = fpext float "
          << loweredArguments.front().substr(6) << " to double\n";
      out << "  call i32 (ptr, ...) @printf(ptr getelementptr inbounds ([4 x i8], "
             "ptr @.fmt.float, i64 0, i64 0), double %"
          << promoted << ")\n";
    } else if (argumentSimpleTypes.front() == "Double") {
      out << "  call i32 (ptr, ...) @printf(ptr getelementptr inbounds ([4 x i8], "
             "ptr @.fmt.float, i64 0, i64 0), "
          << loweredArguments.front() << ")\n";
    } else if (argumentSimpleTypes.front() == "Char") {
      out << "  call i32 (ptr, ...) @printf(ptr getelementptr inbounds ([4 x i8], "
             "ptr @.fmt.char, i64 0, i64 0), "
          << loweredArguments.front() << ")\n";
    } else {
      supported = false;
      return defaultValue(expectedType);
    }
    supported = true;
    return {};
  }

  emitSourceLocation(value.span, state, out);
  const std::string& receiver =
      implicitReceiverCount == 1 ? implicitReceiver : explicitReceiver;
  if (!receiver.empty()) {
    const std::string checkedReceiver = requireNonNullReceiver(receiver, state, out);
    loweredArguments.front() =
        llvmType(signature->parameterTypes.front()) + " " + checkedReceiver;
  }
  const std::string returnType = llvmType(signature->returnType);
  if (returnType == "void") {
    out << "  call void @" << sanitizeIdentifier(target) << '(';
    for (std::size_t i = 0; i < loweredArguments.size(); ++i) {
      if (i != 0) {
        out << ", ";
      }
      out << loweredArguments[i];
    }
    out << ")\n";
    supported = true;
    return {};
  }

  const std::string temporary = nextTemporary(state);
  out << "  %" << temporary << " = call " << returnType << " @"
      << sanitizeIdentifier(target) << '(';
  for (std::size_t i = 0; i < loweredArguments.size(); ++i) {
    if (i != 0) {
      out << ", ";
    }
    out << loweredArguments[i];
  }
  out << ")\n";
  state.values[temporary] = returnType;
  state.simpleTypes[temporary] = signature->returnType;
  supported = true;
  return "%" + temporary;
}

std::string lowerUnary(const nir::Value& value, const std::string& expectedType,
                       LoweringState& state, std::ostringstream& out, bool& supported) {
  if (value.operands.size() != 1 || value.text.empty()) {
    supported = false;
    return defaultValue(expectedType);
  }

  const std::string operandType = inferSimpleType(value.operands.front(), state);
  if (value.text == "!") {
    if (expectedType != "Boolean" ||
        (operandType != "Boolean" && operandType != "Unknown")) {
      supported = false;
      return defaultValue(expectedType);
    }
    bool operandSupported = false;
    const std::string operand =
        lowerValue(value.operands.front(), "Boolean", state, out, operandSupported);
    if (!operandSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    const std::string temporary = nextTemporary(state);
    out << "  %" << temporary << " = xor i1 " << operand << ", true\n";
    state.values[temporary] = "i1";
    state.simpleTypes[temporary] = "Boolean";
    supported = true;
    return "%" + temporary;
  }

  const bool isIntegral = expectedType == "Int" || expectedType == "Long";
  const bool isFloating = expectedType == "Float" || expectedType == "Double";
  const bool narrowIntegralPromotion =
      expectedType == "Int" && (operandType == "Byte" || operandType == "Short");
  if ((value.text != "+" && value.text != "-") || (!isIntegral && !isFloating) ||
      (operandType != expectedType && operandType != "Unknown" &&
       !narrowIntegralPromotion)) {
    supported = false;
    return defaultValue(expectedType);
  }

  bool operandSupported = false;
  const std::string operand =
      lowerValue(value.operands.front(), expectedType, state, out, operandSupported);
  if (!operandSupported) {
    supported = false;
    return defaultValue(expectedType);
  }
  if (value.text == "+") {
    supported = true;
    return operand;
  }

  const std::string temporary = nextTemporary(state);
  if (isFloating) {
    out << "  %" << temporary << " = fneg " << llvmType(expectedType) << ' ' << operand
        << '\n';
  } else {
    out << "  %" << temporary << " = sub " << llvmType(expectedType) << " 0, "
        << operand << '\n';
  }
  state.values[temporary] = llvmType(expectedType);
  state.simpleTypes[temporary] = expectedType;
  supported = true;
  return "%" + temporary;
}

std::string lowerStringOperand(const nir::Value& value, LoweringState& state,
                               std::ostringstream& out, bool& supported) {
  const std::string operandType = inferSimpleType(value, state);
  if (operandType == "String" || operandType == "Unknown") {
    return lowerValue(value, "String", state, out, supported);
  }
  if (operandType == "Unit") {
    supported = true;
    return "getelementptr inbounds ([3 x i8], ptr @.str.unit, i64 0, i64 0)";
  }
  if (operandType == "Null") {
    supported = true;
    return "getelementptr inbounds ([5 x i8], ptr @.str.null, i64 0, i64 0)";
  }
  if (operandType == "Symbol") {
    return lowerValue(value, "Symbol", state, out, supported);
  }
  if (isReferenceType(operandType)) {
    bool operandSupported = false;
    const std::string operand =
        lowerValue(value, operandType, state, out, operandSupported);
    if (!operandSupported) {
      supported = false;
      return defaultValue("String");
    }
    const std::string temporary = nextTemporary(state);
    out << "  %" << temporary << " = call ptr @__scalanative_any_to_string(ptr "
        << operand << ")\n";
    state.values[temporary] = "ptr";
    state.simpleTypes[temporary] = "String";
    supported = true;
    return "%" + temporary;
  }

  std::string helper;
  if (operandType == "Boolean") {
    helper = "__scalanative_string_from_boolean";
  } else if (operandType == "Byte" || operandType == "Short") {
    helper = "__scalanative_string_from_int";
  } else if (operandType == "Int") {
    helper = "__scalanative_string_from_int";
  } else if (operandType == "Long") {
    helper = "__scalanative_string_from_long";
  } else if (operandType == "Float") {
    helper = "__scalanative_string_from_float";
  } else if (operandType == "Double") {
    helper = "__scalanative_string_from_double";
  } else if (operandType == "Char") {
    helper = "__scalanative_string_from_char";
  } else {
    supported = false;
    return defaultValue("String");
  }

  bool operandSupported = false;
  const std::string operand =
      lowerValue(value, operandType, state, out, operandSupported);
  if (!operandSupported) {
    supported = false;
    return defaultValue("String");
  }

  std::string stringOperand = operand;
  std::string stringOperandType = operandType;
  if (operandType == "Byte" || operandType == "Short") {
    const std::string widened = nextTemporary(state);
    out << "  %" << widened << " = sext " << llvmType(operandType) << ' ' << operand
        << " to i32\n";
    state.values[widened] = "i32";
    state.simpleTypes[widened] = "Int";
    stringOperand = "%" + widened;
    stringOperandType = "Int";
  }
  const std::string temporary = nextTemporary(state);
  out << "  %" << temporary << " = call ptr @" << helper << "("
      << llvmType(stringOperandType) << " " << stringOperand << ")\n";
  state.values[temporary] = "ptr";
  state.simpleTypes[temporary] = "String";
  supported = true;
  return "%" + temporary;
}

std::string lowerBinary(const nir::Value& value, const std::string& expectedType,
                        LoweringState& state, std::ostringstream& out,
                        bool& supported) {
  if (value.operands.size() != 2) {
    supported = false;
    return defaultValue(expectedType);
  }

  const bool isStringConcat = value.text == "+" && expectedType == "String";
  if (isStringConcat) {
    bool lhsSupported = false;
    bool rhsSupported = false;
    const std::string lhs =
        lowerStringOperand(value.operands.front(), state, out, lhsSupported);
    const std::string rhs =
        lowerStringOperand(value.operands.back(), state, out, rhsSupported);
    if (!lhsSupported || !rhsSupported) {
      supported = false;
      return defaultValue(expectedType);
    }

    const std::string temporary = nextTemporary(state);
    out << "  %" << temporary << " = call ptr @__scalanative_string_concat(ptr " << lhs
        << ", ptr " << rhs << ")\n";
    state.values[temporary] = "ptr";
    state.simpleTypes[temporary] = "String";
    supported = true;
    return "%" + temporary;
  }

  const bool isLogical = value.text == "&&" || value.text == "||";
  if (isLogical) {
    const std::string lhsType = inferSimpleType(value.operands.front(), state);
    const std::string rhsType = inferSimpleType(value.operands.back(), state);
    if (expectedType != "Boolean" || (lhsType != "Boolean" && lhsType != "Unknown") ||
        (rhsType != "Boolean" && rhsType != "Unknown")) {
      supported = false;
      return defaultValue(expectedType);
    }

    bool lhsSupported = false;
    const std::string lhs =
        lowerValue(value.operands.front(), "Boolean", state, out, lhsSupported);
    if (!lhsSupported) {
      supported = false;
      return defaultValue(expectedType);
    }

    const std::string branchId = nextTemporary(state);
    const std::string rhsLabel = "logical_rhs_" + branchId;
    const std::string shortLabel = "logical_short_" + branchId;
    const std::string mergeLabel = "logical_merge_" + branchId;
    const bool isAnd = value.text == "&&";
    out << "  br i1 " << lhs << ", label %" << (isAnd ? rhsLabel : shortLabel)
        << ", label %" << (isAnd ? shortLabel : rhsLabel) << "\n";

    const LoweringState branchStart = state;
    LoweringState rhsState = branchStart;
    bool rhsSupported = false;
    out << rhsLabel << ":\n";
    rhsState.currentBlockLabel = rhsLabel;
    const std::string rhs =
        lowerValue(value.operands.back(), "Boolean", rhsState, out, rhsSupported);
    const std::string rhsPredecessor = rhsState.currentBlockLabel;
    out << "  br label %" << mergeLabel << "\n";

    out << shortLabel << ":\n";
    out << "  br label %" << mergeLabel << "\n";

    state = branchStart;
    state.nextTemporary = rhsState.nextTemporary;
    state.nextBindingRootSlot = rhsState.nextBindingRootSlot;
    state.nextTemporaryRootSlot = rhsState.nextTemporaryRootSlot;
    state.usedLocalValueNames = rhsState.usedLocalValueNames;
    state.currentBlockLabel = mergeLabel;
    for (auto it = state.exactLocals.begin(); it != state.exactLocals.end();) {
      if (!rhsState.exactLocals.contains(*it)) {
        it = state.exactLocals.erase(it);
      } else {
        ++it;
      }
    }

    out << mergeLabel << ":\n";
    if (!rhsSupported) {
      supported = false;
      return defaultValue(expectedType);
    }

    const std::string temporary = nextTemporary(state);
    out << "  %" << temporary << " = phi i1 [ " << rhs << ", %" << rhsPredecessor
        << " ], [ " << (isAnd ? "0" : "1") << ", %" << shortLabel << " ]\n";
    state.values[temporary] = "i1";
    state.simpleTypes[temporary] = "Boolean";
    supported = true;
    return "%" + temporary;
  }

  const bool isBitwise = value.text == "&" || value.text == "|" || value.text == "^";
  if (isBitwise) {
    const std::string lhsType = inferSimpleType(value.operands.front(), state);
    const std::string rhsType = inferSimpleType(value.operands.back(), state);
    const std::string loweredType = llvmType(expectedType);
    if ((expectedType != "Boolean" && expectedType != "Int" &&
         expectedType != "Long") ||
        (lhsType != expectedType && lhsType != "Unknown") ||
        (rhsType != expectedType && rhsType != "Unknown") ||
        (loweredType != "i1" && loweredType != "i32" && loweredType != "i64")) {
      supported = false;
      return defaultValue(expectedType);
    }

    bool lhsSupported = false;
    bool rhsSupported = false;
    const std::string lhs =
        lowerValue(value.operands.front(), expectedType, state, out, lhsSupported);
    const std::string rhs =
        lowerValue(value.operands.back(), expectedType, state, out, rhsSupported);
    if (!lhsSupported || !rhsSupported) {
      supported = false;
      return defaultValue(expectedType);
    }

    const char* instruction = value.text == "&"   ? "and"
                              : value.text == "|" ? "or"
                                                  : "xor";
    const std::string temporary = nextTemporary(state);
    out << "  %" << temporary << " = " << instruction << ' ' << loweredType << ' '
        << lhs << ", " << rhs << '\n';
    state.values[temporary] = loweredType;
    state.simpleTypes[temporary] = expectedType;
    supported = true;
    return "%" + temporary;
  }

  const bool isShift = value.text == "<<" || value.text == ">>" || value.text == ">>>";
  if (isShift) {
    const std::string lhsType = inferSimpleType(value.operands.front(), state);
    const std::string rhsType = inferSimpleType(value.operands.back(), state);
    if ((expectedType != "Int" && expectedType != "Long") ||
        (lhsType != expectedType && lhsType != "Unknown") ||
        (rhsType != "Int" && rhsType != "Unknown")) {
      supported = false;
      return defaultValue(expectedType);
    }

    bool lhsSupported = false;
    bool rhsSupported = false;
    const std::string lhs =
        lowerValue(value.operands.front(), expectedType, state, out, lhsSupported);
    const std::string rhs =
        lowerValue(value.operands.back(), "Int", state, out, rhsSupported);
    if (!lhsSupported || !rhsSupported) {
      supported = false;
      return defaultValue(expectedType);
    }

    const std::string masked = nextTemporary(state);
    out << "  %" << masked << " = and i32 " << rhs << ", "
        << (expectedType == "Int" ? 31 : 63) << '\n';
    state.values[masked] = "i32";
    state.simpleTypes[masked] = "Int";

    std::string count = "%" + masked;
    if (expectedType == "Long") {
      const std::string widened = nextTemporary(state);
      out << "  %" << widened << " = zext i32 %" << masked << " to i64\n";
      state.values[widened] = "i64";
      state.simpleTypes[widened] = "Long";
      count = "%" + widened;
    }

    const char* instruction = value.text == "<<"   ? "shl"
                              : value.text == ">>" ? "ashr"
                                                   : "lshr";
    const std::string temporary = nextTemporary(state);
    out << "  %" << temporary << " = " << instruction << ' ' << llvmType(expectedType)
        << ' ' << lhs << ", " << count << '\n';
    state.values[temporary] = llvmType(expectedType);
    state.simpleTypes[temporary] = expectedType;
    supported = true;
    return "%" + temporary;
  }

  const bool isComparison = value.text == "==" || value.text == "!=" ||
                            value.text == "<" || value.text == ">" ||
                            value.text == "<=" || value.text == ">=";
  const std::string operandType = inferSimpleType(value.operands.front(), state);
  if (isComparison) {
    const std::string rhsType = inferSimpleType(value.operands.back(), state);
    const std::string operandLlvmType = llvmType(operandType);
    const bool compatiblePointerNullComparison =
        operandLlvmType == "ptr" && (operandType == "Null" || rhsType == "Null");
    const bool compatibleReferencePointerComparison = operandLlvmType == "ptr" &&
                                                      isReferenceType(operandType) &&
                                                      isReferenceType(rhsType);
    if (expectedType != "Boolean" || operandType == "Unknown" ||
        (rhsType != "Unknown" && rhsType != operandType &&
         !compatiblePointerNullComparison && !compatibleReferencePointerComparison) ||
        (operandLlvmType != "i1" && operandLlvmType != "i8" &&
         operandLlvmType != "i16" && operandLlvmType != "i32" &&
         operandLlvmType != "i64" && operandLlvmType != "float" &&
         operandLlvmType != "double" && operandLlvmType != "ptr")) {
      supported = false;
      return defaultValue(expectedType);
    }
    if (operandLlvmType == "ptr" && value.text != "==" && value.text != "!=") {
      supported = false;
      return defaultValue(expectedType);
    }

    bool lhsSupported = false;
    bool rhsSupported = false;
    const std::string lhs =
        lowerValue(value.operands.front(), operandType, state, out, lhsSupported);
    const std::string rhs =
        lowerValue(value.operands.back(), operandType, state, out, rhsSupported);
    if (!lhsSupported || !rhsSupported) {
      supported = false;
      return defaultValue(expectedType);
    }

    if (operandType == "Object" && (value.text == "==" || value.text == "!=")) {
      const std::string matches = nextTemporary(state);
      out << "  %" << matches << " = call i1 @__scalanative_any_equals(ptr " << lhs
          << ", ptr " << rhs << ")\n";
      state.values[matches] = "i1";
      state.simpleTypes[matches] = "Boolean";
      if (value.text == "==") {
        supported = true;
        return "%" + matches;
      }

      const std::string temporary = nextTemporary(state);
      out << "  %" << temporary << " = xor i1 %" << matches << ", true\n";
      state.values[temporary] = "i1";
      state.simpleTypes[temporary] = "Boolean";
      supported = true;
      return "%" + temporary;
    }

    if (operandType == "String") {
      const std::string matches = nextTemporary(state);
      out << "  %" << matches << " = call i1 @__scalanative_string_equals(ptr " << lhs
          << ", ptr " << rhs << ")\n";
      state.values[matches] = "i1";
      state.simpleTypes[matches] = "Boolean";
      if (value.text == "==") {
        supported = true;
        return "%" + matches;
      }

      const std::string temporary = nextTemporary(state);
      out << "  %" << temporary << " = xor i1 %" << matches << ", true\n";
      state.values[temporary] = "i1";
      state.simpleTypes[temporary] = "Boolean";
      supported = true;
      return "%" + temporary;
    }

    std::string predicate;
    if (operandLlvmType == "float" || operandLlvmType == "double") {
      if (value.text == "==") {
        predicate = "oeq";
      } else if (value.text == "!=") {
        predicate = "une";
      } else if (value.text == "<") {
        predicate = "olt";
      } else if (value.text == ">") {
        predicate = "ogt";
      } else if (value.text == "<=") {
        predicate = "ole";
      } else {
        predicate = "oge";
      }
    } else if (value.text == "==") {
      predicate = "eq";
    } else if (value.text == "!=") {
      predicate = "ne";
    } else {
      const bool useUnsigned = operandType == "Char" || operandType == "Boolean";
      if (value.text == "<") {
        predicate = useUnsigned ? "ult" : "slt";
      } else if (value.text == ">") {
        predicate = useUnsigned ? "ugt" : "sgt";
      } else if (value.text == "<=") {
        predicate = useUnsigned ? "ule" : "sle";
      } else {
        predicate = useUnsigned ? "uge" : "sge";
      }
    }

    const std::string temporary = nextTemporary(state);
    out << "  %" << temporary << " = "
        << ((operandLlvmType == "float" || operandLlvmType == "double") ? "fcmp"
                                                                        : "icmp")
        << ' ' << predicate << ' ' << operandLlvmType << ' ' << lhs << ", " << rhs
        << '\n';
    state.values[temporary] = "i1";
    state.simpleTypes[temporary] = "Boolean";
    supported = true;
    return "%" + temporary;
  }

  const std::string loweredType = llvmType(expectedType);
  const bool isInteger = loweredType == "i32" || loweredType == "i64";
  const bool isFloating = loweredType == "float" || loweredType == "double";
  if (!isInteger && !isFloating) {
    supported = false;
    return defaultValue(expectedType);
  }

  bool lhsSupported = false;
  bool rhsSupported = false;
  const std::string lhs =
      lowerValue(value.operands[0], expectedType, state, out, lhsSupported);
  const std::string rhs =
      lowerValue(value.operands[1], expectedType, state, out, rhsSupported);
  if (!lhsSupported || !rhsSupported) {
    supported = false;
    return defaultValue(expectedType);
  }

  if (isInteger && (value.text == "/" || value.text == "%")) {
    const std::string helper = "__scalanative_" +
                               std::string(loweredType == "i64" ? "long_" : "int_") +
                               (value.text == "/" ? "div" : "rem");
    const std::string temporary = nextTemporary(state);
    out << "  %" << temporary << " = call " << loweredType << " @" << helper << '('
        << loweredType << ' ' << lhs << ", " << loweredType << ' ' << rhs << ")\n";
    state.values[temporary] = loweredType;
    state.simpleTypes[temporary] = expectedType;
    supported = true;
    return "%" + temporary;
  }

  std::string instruction;
  if (isFloating) {
    if (value.text == "+") {
      instruction = "fadd";
    } else if (value.text == "-") {
      instruction = "fsub";
    } else if (value.text == "*") {
      instruction = "fmul";
    } else if (value.text == "/") {
      instruction = "fdiv";
    } else if (value.text == "%") {
      instruction = "frem";
    } else {
      supported = false;
      return defaultValue(expectedType);
    }
  } else {
    instruction = "add";
    if (value.text == "-") {
      instruction = "sub";
    } else if (value.text == "*") {
      instruction = "mul";
    } else if (value.text == "/") {
      instruction = "sdiv";
    } else if (value.text == "%") {
      instruction = "srem";
    } else if (value.text != "+") {
      supported = false;
      return defaultValue(expectedType);
    }
  }

  const std::string temporary = nextTemporary(state);
  out << "  %" << temporary << " = " << instruction << ' ' << loweredType << ' ' << lhs
      << ", " << rhs << '\n';
  state.values[temporary] = loweredType;
  state.simpleTypes[temporary] = expectedType;
  supported = true;
  return "%" + temporary;
}

std::string lowerIf(const nir::Value& value, const std::string& expectedType,
                    LoweringState& state, std::ostringstream& out, bool& supported) {
  if (value.operands.size() != 3) {
    supported = false;
    return defaultValue(expectedType);
  }

  bool conditionSupported = false;
  const std::string condition =
      lowerValue(value.operands[0], "Boolean", state, out, conditionSupported);
  if (!conditionSupported) {
    supported = false;
    return defaultValue(expectedType);
  }

  const std::string branchId = nextTemporary(state);
  const std::string thenLabel = "if_then_" + branchId;
  const std::string elseLabel = "if_else_" + branchId;
  const std::string mergeLabel = "if_merge_" + branchId;
  out << "  br i1 " << condition << ", label %" << thenLabel << ", label %" << elseLabel
      << "\n";

  const LoweringState branchStart = state;
  LoweringState thenState = branchStart;
  bool thenSupported = false;
  out << thenLabel << ":\n";
  thenState.currentBlockLabel = thenLabel;
  const std::string thenValue =
      lowerValue(value.operands[1], expectedType, thenState, out, thenSupported);
  const std::string thenPredecessor = thenState.currentBlockLabel;
  out << "  br label %" << mergeLabel << "\n";

  LoweringState elseState = branchStart;
  elseState.nextTemporary = thenState.nextTemporary;
  elseState.nextBindingRootSlot = thenState.nextBindingRootSlot;
  elseState.nextTemporaryRootSlot = thenState.nextTemporaryRootSlot;
  elseState.usedLocalValueNames = thenState.usedLocalValueNames;
  bool elseSupported = false;
  out << elseLabel << ":\n";
  elseState.currentBlockLabel = elseLabel;
  const std::string elseValue =
      lowerValue(value.operands[2], expectedType, elseState, out, elseSupported);
  const std::string elsePredecessor = elseState.currentBlockLabel;
  out << "  br label %" << mergeLabel << "\n";

  state = branchStart;
  state.nextTemporary = elseState.nextTemporary;
  state.nextBindingRootSlot = elseState.nextBindingRootSlot;
  state.nextTemporaryRootSlot = elseState.nextTemporaryRootSlot;
  state.usedLocalValueNames = elseState.usedLocalValueNames;
  state.currentBlockLabel = mergeLabel;
  for (auto it = state.exactLocals.begin(); it != state.exactLocals.end();) {
    if (!thenState.exactLocals.contains(*it) || !elseState.exactLocals.contains(*it)) {
      it = state.exactLocals.erase(it);
    } else {
      ++it;
    }
  }

  out << mergeLabel << ":\n";
  if (!thenSupported || !elseSupported) {
    supported = false;
    return defaultValue(expectedType);
  }

  if (llvmType(expectedType) == "void") {
    supported = true;
    return {};
  }

  const std::string temporary = nextTemporary(state);
  out << "  %" << temporary << " = phi " << llvmType(expectedType) << " [ " << thenValue
      << ", %" << thenPredecessor << " ], [ " << elseValue << ", %" << elsePredecessor
      << " ]\n";
  state.values[temporary] = llvmType(expectedType);
  state.simpleTypes[temporary] = expectedType;
  supported = true;
  return "%" + temporary;
}

std::string lowerThrowValue(const nir::Value& value, const std::string& expectedType,
                            LoweringState& state, std::ostringstream& out,
                            bool& supported) {
  if (value.operands.size() != 1 || value.type != "Nothing") {
    supported = false;
    return defaultValue(expectedType);
  }

  const std::string exceptionType = inferSimpleType(value.operands.front(), state);
  bool exceptionSupported = false;
  const std::string exception =
      lowerValue(value.operands.front(), exceptionType, state, out, exceptionSupported);
  if (!exceptionSupported || llvmType(exceptionType) != "ptr") {
    supported = false;
    return defaultValue(expectedType);
  }

  emitSourceLocation(value.span, state, out);
  emitShadowPop(state, out);
  const std::string checkedException =
      requireNonNullThrownException(exception, state, out);
  out << "  call void @__scalanative_throw(ptr " << checkedException << ")\n";
  out << "  unreachable\n";

  const std::string continuation = "throw_cont_" + nextTemporary(state);
  out << continuation << ":\n";
  state.currentBlockLabel = continuation;
  supported = true;
  return defaultValue(expectedType);
}

std::string lowerTry(const nir::Value& value, const std::string& expectedType,
                     LoweringState& state, std::ostringstream& out, bool& supported) {
  if (value.operands.empty() || value.type != expectedType) {
    supported = false;
    return defaultValue(expectedType);
  }

  std::vector<const nir::Value*> catches;
  const nir::Value* finalizer = nullptr;
  bool wellFormed = true;
  for (std::size_t index = 1; index < value.operands.size(); ++index) {
    const nir::Value& operand = value.operands[index];
    if (operand.kind == nir::ValueKind::Catch && finalizer == nullptr &&
        operand.operands.size() == 2 &&
        operand.operands.front().kind == nir::ValueKind::Local &&
        !operand.operands.front().text.empty() &&
        !operand.operands.front().type.empty()) {
      catches.push_back(&operand);
    } else if (operand.kind == nir::ValueKind::Finally && finalizer == nullptr &&
               index + 1 == value.operands.size() && operand.operands.size() == 1) {
      finalizer = &operand;
    } else {
      wellFormed = false;
    }
  }
  if (!wellFormed || (catches.empty() && finalizer == nullptr) ||
      !state.hasShadowFrame ||
      state.nextTemporaryRootSlot >= state.temporaryRootSlots.size()) {
    supported = false;
    return defaultValue(expectedType);
  }

  const std::string exceptionRoot =
      state.temporaryRootSlots[state.nextTemporaryRootSlot++];
  const std::string tryId = nextTemporary(state);
  const std::string jumpStorage = "try_jump_" + tryId;
  const std::string handlerStorage = "try_handler_" + tryId;
  const std::string previousHandler = "try_previous_" + tryId;
  const std::string savedShadow = "try_shadow_" + tryId;
  const std::string savedSource = "try_source_" + tryId;
  const std::string savedZone = "try_zone_" + tryId;
  const std::string setjmpResult = "try_setjmp_" + tryId;
  const std::string initialResult = "try_initial_" + tryId;
  const std::string bodyLabel = "try_body_" + tryId;
  const std::string dispatchLabel = "try_dispatch_" + tryId;
  const std::string mergeLabel = "try_merge_" + tryId;
  const std::string unmatchedLabel = "try_unmatched_" + tryId;

  out << "  %" << jumpStorage << " = alloca [256 x i8], align 16\n";
  out << "  %" << handlerStorage << " = alloca %scalanative.exception_handler\n";
  out << "  %" << previousHandler
      << " = load ptr, ptr @__scalanative_exception_handler\n";
  out << "  %" << savedShadow << " = load ptr, ptr @__scalanative_shadow_stack\n";
  out << "  %" << savedSource << " = load ptr, ptr @__scalanative_source_stack\n";
  out << "  %" << savedZone << " = load ptr, ptr @__scalanative_current_zone\n";
  out << "  %try_previous_field_" << tryId
      << " = getelementptr %scalanative.exception_handler, ptr %" << handlerStorage
      << ", i32 0, i32 0\n";
  out << "  store ptr %" << previousHandler << ", ptr %try_previous_field_" << tryId
      << "\n";
  out << "  %try_shadow_field_" << tryId
      << " = getelementptr %scalanative.exception_handler, ptr %" << handlerStorage
      << ", i32 0, i32 1\n";
  out << "  store ptr %" << savedShadow << ", ptr %try_shadow_field_" << tryId << "\n";
  out << "  %try_zone_field_" << tryId
      << " = getelementptr %scalanative.exception_handler, ptr %" << handlerStorage
      << ", i32 0, i32 2\n";
  out << "  store ptr %" << savedZone << ", ptr %try_zone_field_" << tryId << "\n";
  out << "  %try_jump_field_" << tryId
      << " = getelementptr %scalanative.exception_handler, ptr %" << handlerStorage
      << ", i32 0, i32 3\n";
  out << "  store ptr %" << jumpStorage << ", ptr %try_jump_field_" << tryId << "\n";
  out << "  %try_source_field_" << tryId
      << " = getelementptr %scalanative.exception_handler, ptr %" << handlerStorage
      << ", i32 0, i32 4\n";
  out << "  store ptr %" << savedSource << ", ptr %try_source_field_" << tryId << "\n";
  out << "  store ptr %" << handlerStorage
      << ", ptr @__scalanative_exception_handler\n";
  out << "  %" << setjmpResult << " = call i32 @_setjmp(ptr %" << jumpStorage << ")\n";
  out << "  %" << initialResult << " = icmp eq i32 %" << setjmpResult << ", 0\n";
  out << "  br i1 %" << initialResult << ", label %" << bodyLabel << ", label %"
      << dispatchLabel << "\n";

  const LoweringState branchStart = state;
  LoweringState bodyState = branchStart;
  bodyState.currentBlockLabel = bodyLabel;
  out << bodyLabel << ":\n";
  bool bodySupported = false;
  const std::string bodyResult =
      lowerValue(value.operands.front(), expectedType, bodyState, out, bodySupported);
  out << "  store ptr %" << previousHandler
      << ", ptr @__scalanative_exception_handler\n";
  const std::string bodyPredecessor = bodyState.currentBlockLabel;
  out << "  br label %" << mergeLabel << "\n";

  LoweringState cursor = branchStart;
  cursor.nextTemporary = bodyState.nextTemporary;
  cursor.nextBindingRootSlot = bodyState.nextBindingRootSlot;
  cursor.nextTemporaryRootSlot = bodyState.nextTemporaryRootSlot;
  cursor.usedLocalValueNames = bodyState.usedLocalValueNames;

  out << dispatchLabel << ":\n";
  cursor.currentBlockLabel = dispatchLabel;
  const std::string exceptionName = "try_exception_" + nextTemporary(cursor);
  out << "  %" << exceptionName
      << " = load ptr, ptr @__scalanative_current_exception\n";
  out << "  store ptr null, ptr @__scalanative_current_exception\n";
  out << "  store ptr %" << exceptionName << ", ptr %" << exceptionRoot << "\n";

  std::vector<std::pair<std::string, std::string>> mergeInputs;
  mergeInputs.emplace_back(bodyResult, bodyPredecessor);
  std::vector<LoweringState> mergeStates;
  mergeStates.push_back(bodyState);
  bool handlersSupported = true;
  bool hasCatchAll = false;
  if (catches.empty()) {
    out << "  br label %" << unmatchedLabel << "\n";
  } else {
    out << "  br label %try_catch_test_" << tryId << "_0\n";
  }

  for (std::size_t index = 0; index < catches.size(); ++index) {
    const nir::Value& handler = *catches[index];
    const nir::Value& binding = handler.operands.front();
    const std::string testLabel =
        "try_catch_test_" + tryId + "_" + std::to_string(index);
    const std::string handlerLabel =
        "try_catch_body_" + tryId + "_" + std::to_string(index);
    const std::string nextLabel =
        index + 1 < catches.size()
            ? "try_catch_test_" + tryId + "_" + std::to_string(index + 1)
            : unmatchedLabel;
    out << testLabel << ":\n";

    LoweringState testState = branchStart;
    testState.nextTemporary = cursor.nextTemporary;
    testState.nextBindingRootSlot = cursor.nextBindingRootSlot;
    testState.nextTemporaryRootSlot = cursor.nextTemporaryRootSlot;
    testState.usedLocalValueNames = cursor.usedLocalValueNames;
    testState.currentBlockLabel = testLabel;
    const bool catchAll =
        binding.type == "Object" || binding.type == support::StdNames::JavaLangObject;
    if (catchAll) {
      hasCatchAll = true;
      if (index + 1 != catches.size()) {
        handlersSupported = false;
      }
      out << "  br label %" << handlerLabel << "\n";
    } else {
      const std::string className = resolveGlobalName(binding.type, testState);
      const ClassLayout* layout = nullptr;
      if (testState.classLayouts != nullptr) {
        auto found = testState.classLayouts->find(className);
        if (found != testState.classLayouts->end()) {
          layout = &found->second;
        }
      }
      if (className.empty() || layout == nullptr || layout->descriptorName.empty()) {
        reportUnsupported(testState, handler.span,
                          "unresolved catch descriptor for " + binding.type);
        handlersSupported = false;
        out << "  br label %" << nextLabel << "\n";
      } else {
        const std::string matches = "try_catch_matches_" + nextTemporary(testState);
        out << "  %" << matches << " = call i1 @__scalanative_is_instance_of(ptr %"
            << exceptionName << ", ptr @" << layout->descriptorName << ")\n";
        out << "  br i1 %" << matches << ", label %" << handlerLabel << ", label %"
            << nextLabel << "\n";
      }
    }

    LoweringState handlerState = branchStart;
    handlerState.nextTemporary = testState.nextTemporary;
    handlerState.nextBindingRootSlot = testState.nextBindingRootSlot;
    handlerState.nextTemporaryRootSlot = testState.nextTemporaryRootSlot;
    handlerState.usedLocalValueNames = testState.usedLocalValueNames;
    handlerState.currentBlockLabel = handlerLabel;
    const std::string sourceName = sanitizeIdentifier(binding.text);
    const std::string localName = nextLocalValueName(sourceName, handlerState);
    handlerState.values[sourceName] = "ptr";
    handlerState.localValueNames[sourceName] = localName;
    handlerState.simpleTypes[sourceName] = binding.type;
    handlerState.exactLocals.erase(sourceName);
    out << handlerLabel << ":\n";
    out << "  %" << localName << " = getelementptr i8, ptr %" << exceptionName
        << ", i64 0\n";
    bool handlerSupported = false;
    const std::string handlerResult = lowerValue(handler.operands.back(), expectedType,
                                                 handlerState, out, handlerSupported);
    handlersSupported = handlersSupported && handlerSupported;
    const std::string handlerPredecessor = handlerState.currentBlockLabel;
    out << "  br label %" << mergeLabel << "\n";
    mergeInputs.emplace_back(handlerResult, handlerPredecessor);
    mergeStates.push_back(handlerState);

    cursor = branchStart;
    cursor.nextTemporary = handlerState.nextTemporary;
    cursor.nextBindingRootSlot = handlerState.nextBindingRootSlot;
    cursor.nextTemporaryRootSlot = handlerState.nextTemporaryRootSlot;
    cursor.usedLocalValueNames = handlerState.usedLocalValueNames;
  }

  bool unmatchedSupported = true;
  if (!hasCatchAll) {
    out << unmatchedLabel << ":\n";
    LoweringState unmatchedState = branchStart;
    unmatchedState.nextTemporary = cursor.nextTemporary;
    unmatchedState.nextBindingRootSlot = cursor.nextBindingRootSlot;
    unmatchedState.nextTemporaryRootSlot = cursor.nextTemporaryRootSlot;
    unmatchedState.usedLocalValueNames = cursor.usedLocalValueNames;
    unmatchedState.currentBlockLabel = unmatchedLabel;
    if (finalizer != nullptr) {
      bool finalizerSupported = false;
      const std::string finalizerType =
          inferSimpleType(finalizer->operands.front(), unmatchedState);
      (void)lowerValue(finalizer->operands.front(), finalizerType, unmatchedState, out,
                       finalizerSupported);
      unmatchedSupported = finalizerSupported;
    }
    emitShadowPop(unmatchedState, out);
    out << "  call void @__scalanative_throw(ptr %" << exceptionName << ")\n";
    out << "  unreachable\n";
    cursor = branchStart;
    cursor.nextTemporary = unmatchedState.nextTemporary;
    cursor.nextBindingRootSlot = unmatchedState.nextBindingRootSlot;
    cursor.nextTemporaryRootSlot = unmatchedState.nextTemporaryRootSlot;
    cursor.usedLocalValueNames = unmatchedState.usedLocalValueNames;
  }

  state = branchStart;
  state.nextTemporary = cursor.nextTemporary;
  state.nextBindingRootSlot = cursor.nextBindingRootSlot;
  state.nextTemporaryRootSlot = cursor.nextTemporaryRootSlot;
  state.usedLocalValueNames = cursor.usedLocalValueNames;
  state.currentBlockLabel = mergeLabel;
  for (auto it = state.exactLocals.begin(); it != state.exactLocals.end();) {
    const bool exactOnEveryPath = std::all_of(
        mergeStates.begin(), mergeStates.end(),
        [&](const LoweringState& path) { return path.exactLocals.contains(*it); });
    if (!exactOnEveryPath) {
      it = state.exactLocals.erase(it);
    } else {
      ++it;
    }
  }

  out << mergeLabel << ":\n";
  std::string result = bodyResult;
  if (llvmType(expectedType) != "void" && mergeInputs.size() > 1) {
    const std::string phi = nextTemporary(state);
    out << "  %" << phi << " = phi " << llvmType(expectedType) << ' ';
    for (std::size_t index = 0; index < mergeInputs.size(); ++index) {
      if (index != 0) {
        out << ", ";
      }
      out << "[ " << mergeInputs[index].first << ", %" << mergeInputs[index].second
          << " ]";
    }
    out << "\n";
    state.values[phi] = llvmType(expectedType);
    state.simpleTypes[phi] = expectedType;
    result = "%" + phi;
  }

  bool sharedFinalizerSupported = true;
  if (finalizer != nullptr) {
    const std::string finalizerType =
        inferSimpleType(finalizer->operands.front(), state);
    (void)lowerValue(finalizer->operands.front(), finalizerType, state, out,
                     sharedFinalizerSupported);
  }
  out << "  store ptr null, ptr %" << exceptionRoot << "\n";
  supported = bodySupported && handlersSupported && unmatchedSupported &&
              sharedFinalizerSupported;
  return supported ? result : defaultValue(expectedType);
}

std::string lowerWhile(const nir::Value& value, const std::string& expectedType,
                       LoweringState& state, std::ostringstream& out, bool& supported) {
  if (value.operands.size() != 2 || expectedType != "Unit") {
    supported = false;
    return defaultValue(expectedType);
  }

  const std::string loopId = nextTemporary(state);
  const std::string conditionLabel = "while_condition_" + loopId;
  const std::string bodyLabel = "while_body_" + loopId;
  const std::string exitLabel = "while_exit_" + loopId;
  out << "  br label %" << conditionLabel << "\n";

  out << conditionLabel << ":\n";
  state.currentBlockLabel = conditionLabel;
  bool conditionSupported = false;
  const std::string condition =
      lowerValue(value.operands.front(), "Boolean", state, out, conditionSupported);
  if (!conditionSupported) {
    out << "  br label %" << exitLabel << "\n";
    out << exitLabel << ":\n";
    state.currentBlockLabel = exitLabel;
    supported = false;
    return {};
  }
  out << "  br i1 " << condition << ", label %" << bodyLabel << ", label %" << exitLabel
      << "\n";

  out << bodyLabel << ":\n";
  state.currentBlockLabel = bodyLabel;
  bool bodySupported = false;
  (void)lowerValue(value.operands.back(), "Unit", state, out, bodySupported);
  out << "  br label %" << conditionLabel << "\n";

  out << exitLabel << ":\n";
  state.currentBlockLabel = exitLabel;
  supported = bodySupported;
  return {};
}

std::string lowerZoneScoped(const nir::Value& value, const std::string& expectedType,
                            LoweringState& state, std::ostringstream& out,
                            bool& supported) {
  if (value.operands.size() != 1) {
    supported = false;
    return defaultValue(expectedType);
  }

  const std::string previousZone = nextTemporary(state);
  out << "  %" << previousZone << " = call ptr @__scalanative_zone_enter()\n";
  state.values[previousZone] = "ptr";
  state.simpleTypes[previousZone] = "Object";

  bool operandSupported = false;
  const std::string result =
      lowerValue(value.operands.front(), expectedType, state, out, operandSupported);
  out << "  call void @__scalanative_zone_exit(ptr %" << previousZone << ")\n";
  supported = operandSupported;
  return operandSupported ? result : defaultValue(expectedType);
}

std::string lowerBlock(const nir::Value& value, const std::string& expectedType,
                       LoweringState& state, std::ostringstream& out, bool& supported) {
  const auto savedValues = state.values;
  const auto savedLocalValueNames = state.localValueNames;
  const auto savedMutableLocalSlots = state.mutableLocalSlots;
  const auto savedMutableUnitLocals = state.mutableUnitLocals;
  const auto savedSimpleTypes = state.simpleTypes;
  const auto savedExactLocals = state.exactLocals;

  if (value.operands.empty()) {
    supported = expectedType == "Unit";
    return {};
  }

  std::string result;
  bool allSupported = true;
  for (std::size_t i = 0; i < value.operands.size(); ++i) {
    const nir::Value& operand = value.operands[i];
    const bool isLast = i + 1 == value.operands.size();
    bool operandSupported = false;
    if (operand.kind == nir::ValueKind::LocalLet ||
        operand.kind == nir::ValueKind::LocalVar) {
      if (operand.text.empty() || operand.operands.size() != 1) {
        allSupported = false;
        continue;
      }
      nir::Instruction binding;
      binding.kind = operand.kind == nir::ValueKind::LocalVar
                         ? nir::InstructionKind::Var
                         : nir::InstructionKind::Let;
      binding.name = operand.text;
      binding.type = operand.type;
      binding.value = operand.operands.front();
      binding.span = operand.span;
      if (binding.kind == nir::InstructionKind::Var) {
        emitVar(binding, state, out);
        const std::string local = sanitizeIdentifier(binding.name);
        operandSupported = state.mutableLocalSlots.contains(local) ||
                           state.mutableUnitLocals.contains(local);
      } else {
        emitLet(binding, state, out);
        operandSupported = state.values.contains(sanitizeIdentifier(binding.name));
      }
      if (isLast) {
        result.clear();
      }
    } else {
      const std::string operandType =
          isLast ? expectedType : inferSimpleType(operand, state);
      result = lowerValue(operand, operandType, state, out, operandSupported);
    }
    allSupported = allSupported && operandSupported;
  }

  state.values = savedValues;
  state.localValueNames = savedLocalValueNames;
  state.mutableLocalSlots = savedMutableLocalSlots;
  state.mutableUnitLocals = savedMutableUnitLocals;
  state.simpleTypes = savedSimpleTypes;
  state.exactLocals = savedExactLocals;
  supported = allSupported;
  return allSupported ? result : defaultValue(expectedType);
}

std::string lowerValueUnrooted(const nir::Value& value, const std::string& expectedType,
                               LoweringState& state, std::ostringstream& out,
                               bool& supported) {
  switch (value.kind) {
  case nir::ValueKind::Unit:
    supported = expectedType == "Unit";
    return {};
  case nir::ValueKind::Literal:
    return lowerLiteral(value, expectedType, state, supported);
  case nir::ValueKind::Local: {
    const std::string local = sanitizeIdentifier(value.text);
    if (state.mutableUnitLocals.contains(local)) {
      supported = expectedType == "Unit";
      return {};
    }
    auto slot = state.mutableLocalSlots.find(local);
    if (slot != state.mutableLocalSlots.end()) {
      auto simpleType = state.simpleTypes.find(local);
      if (simpleType == state.simpleTypes.end()) {
        supported = false;
        return defaultValue(expectedType);
      }
      const std::string temporary = nextTemporary(state);
      out << "  %" << temporary << " = load ";
      if (state.preserveMutableLocalsAcrossHandlers) {
        out << "volatile ";
      }
      out << llvmType(simpleType->second) << ", ptr %" << slot->second << "\n";
      state.values[temporary] = llvmType(simpleType->second);
      state.simpleTypes[temporary] = simpleType->second;
      supported = true;
      return "%" + temporary;
    }
    auto found = state.values.find(local);
    if (found != state.values.end()) {
      supported = true;
      auto loweredName = state.localValueNames.find(local);
      return "%" +
             (loweredName == state.localValueNames.end() ? local : loweredName->second);
    }
    const std::string global = resolveValueGlobal(value, state);
    if (state.classLayouts != nullptr) {
      auto layout = state.classLayouts->find(global);
      if (layout != state.classLayouts->end() && layout->second.isModule) {
        const std::string temporary = nextTemporary(state);
        out << "  %" << temporary << " = call ptr @" << moduleAccessorName(global)
            << "()\n";
        state.values[temporary] = "ptr";
        state.simpleTypes[temporary] = global;
        supported = true;
        return "%" + temporary;
      }
    }
    return lowerZeroArgGlobalCall(global, expectedType, state, out, supported);
  }
  case nir::ValueKind::Super:
    if (!state.values.contains("this")) {
      supported = false;
      return defaultValue(expectedType);
    }
    supported = true;
    return "%this";
  case nir::ValueKind::Select: {
    return lowerSelect(value, expectedType, state, out, supported);
  }
  case nir::ValueKind::Call:
    return lowerCall(value, expectedType, state, out, supported);
  case nir::ValueKind::Unary:
    return lowerUnary(value, expectedType, state, out, supported);
  case nir::ValueKind::Binary:
    return lowerBinary(value, expectedType, state, out, supported);
  case nir::ValueKind::Assign:
    return lowerAssign(value, expectedType, state, out, supported);
  case nir::ValueKind::Throw:
    return lowerThrowValue(value, expectedType, state, out, supported);
  case nir::ValueKind::Try:
    return lowerTry(value, expectedType, state, out, supported);
  case nir::ValueKind::Catch:
  case nir::ValueKind::Finally:
    supported = false;
    return defaultValue(expectedType);
  case nir::ValueKind::If:
    return lowerIf(value, expectedType, state, out, supported);
  case nir::ValueKind::Block:
    return lowerBlock(value, expectedType, state, out, supported);
  case nir::ValueKind::LocalLet:
  case nir::ValueKind::LocalVar:
    supported = false;
    return defaultValue(expectedType);
  case nir::ValueKind::New:
    return lowerNew(value, state, out, supported);
  case nir::ValueKind::SizeOf:
    return lowerSizeOf(value, state, supported);
  case nir::ValueKind::ZoneScoped:
    return lowerZoneScoped(value, expectedType, state, out, supported);
  case nir::ValueKind::Box: {
    if (value.operands.size() != 1 || !isBoxablePrimitiveType(value.text)) {
      supported = false;
      return defaultValue(expectedType);
    }
    bool operandSupported = false;
    const std::string operand =
        lowerValue(value.operands.front(), value.text, state, out, operandSupported);
    if (!operandSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    const runtime::RuntimeTypeLayout layout =
        runtime::boxedPrimitiveTypeLayout(runtime::boxedPrimitiveKind(value.text));
    const std::string object = nextTemporary(state);
    const std::string payload = nextTemporary(state);
    out << "  %" << object << " = call ptr @__scalanative_box_alloc(i64 "
        << layout.instanceSize << ", ptr @" << boxedPrimitiveDescriptorName(value.text)
        << ")\n";
    out << "  %" << payload << " = getelementptr i8, ptr %" << object << ", i64 "
        << layout.payloadOffset << "\n";
    if (value.text != "Unit") {
      out << "  store " << llvmType(value.text) << ' ' << operand << ", ptr %"
          << payload << "\n";
    }
    state.values[object] = "ptr";
    state.simpleTypes[object] = "Object";
    state.values[payload] = "ptr";
    state.simpleTypes[payload] = "Object";
    supported = true;
    return "%" + object;
  }
  case nir::ValueKind::Unbox: {
    if (value.operands.size() != 1 || !isBoxablePrimitiveType(value.text)) {
      supported = false;
      return defaultValue(expectedType);
    }
    bool operandSupported = false;
    const std::string operand =
        lowerValue(value.operands.front(), "Object", state, out, operandSupported);
    if (!operandSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    if (value.text == "Unit") {
      out << "  call void @" << boxedPrimitiveUnboxName(value.text) << "(ptr "
          << operand << ")\n";
      supported = true;
      return {};
    }
    const std::string unboxed = nextTemporary(state);
    out << "  %" << unboxed << " = call " << llvmType(value.text) << " @"
        << boxedPrimitiveUnboxName(value.text) << "(ptr " << operand << ")\n";
    state.values[unboxed] = llvmType(value.text);
    state.simpleTypes[unboxed] = value.text;
    supported = true;
    return "%" + unboxed;
  }
  case nir::ValueKind::IsInstanceOf: {
    if (value.operands.size() != 1 || value.text.empty()) {
      supported = false;
      return defaultValue(expectedType);
    }
    const std::string targetName = isBoxablePrimitiveType(value.text)
                                       ? value.text
                                       : resolveGlobalName(value.text, state);
    std::string descriptorName;
    if (isBoxablePrimitiveType(targetName)) {
      descriptorName = boxedPrimitiveDescriptorName(targetName);
    } else if (state.classLayouts != nullptr) {
      auto target = state.classLayouts->find(targetName);
      if (target != state.classLayouts->end()) {
        descriptorName = target->second.descriptorName;
      }
    }
    if (descriptorName.empty()) {
      supported = false;
      return defaultValue(expectedType);
    }
    bool operandSupported = false;
    const std::string operand =
        lowerValue(value.operands.front(), "Object", state, out, operandSupported);
    if (!operandSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    const std::string result = nextTemporary(state);
    out << "  %" << result << " = call i1 @__scalanative_is_instance_of(ptr " << operand
        << ", ptr @" << descriptorName << ")\n";
    state.values[result] = "i1";
    state.simpleTypes[result] = "Boolean";
    supported = true;
    return "%" + result;
  }
  case nir::ValueKind::AsInstanceOf: {
    if (value.operands.size() != 1 || value.text.empty() ||
        state.classLayouts == nullptr) {
      supported = false;
      return defaultValue(expectedType);
    }
    const std::string targetName = resolveGlobalName(value.text, state);
    auto target = state.classLayouts->find(targetName);
    if (target == state.classLayouts->end() || target->second.descriptorName.empty()) {
      supported = false;
      return defaultValue(expectedType);
    }
    bool operandSupported = false;
    const std::string operand =
        lowerValue(value.operands.front(), "Object", state, out, operandSupported);
    if (!operandSupported) {
      supported = false;
      return defaultValue(expectedType);
    }
    const std::string result = nextTemporary(state);
    out << "  %" << result << " = call ptr @__scalanative_as_instance_of(ptr "
        << operand << ", ptr @" << target->second.descriptorName << ")\n";
    state.values[result] = "ptr";
    state.simpleTypes[result] = targetName;
    supported = true;
    return "%" + result;
  }
  case nir::ValueKind::While:
    return lowerWhile(value, expectedType, state, out, supported);
  case nir::ValueKind::Unknown:
    supported = false;
    return defaultValue(expectedType);
  }

  supported = false;
  return defaultValue(expectedType);
}

std::string lowerValue(const nir::Value& value, const std::string& expectedType,
                       LoweringState& state, std::ostringstream& out, bool& supported) {
  const auto numericRank = [](std::string_view type) {
    if (type == "Byte") {
      return 1;
    }
    if (type == "Short") {
      return 2;
    }
    if (type == "Int") {
      return 3;
    }
    if (type == "Long") {
      return 4;
    }
    if (type == "Float") {
      return 5;
    }
    if (type == "Double") {
      return 6;
    }
    return 0;
  };
  const std::string sourceType = inferSimpleType(value, state);
  const int sourceRank = numericRank(sourceType);
  const int targetRank = numericRank(expectedType);
  const bool widenNumeric =
      sourceType != expectedType && sourceRank != 0 && targetRank > sourceRank;

  std::string lowered = lowerValueUnrooted(
      value, widenNumeric ? sourceType : expectedType, state, out, supported);
  if (supported && widenNumeric) {
    const std::string widened = nextTemporary(state);
    const std::string sourceLlvmType = llvmType(sourceType);
    const std::string targetLlvmType = llvmType(expectedType);
    const bool sourceFloating = sourceType == "Float" || sourceType == "Double";
    const bool targetFloating = expectedType == "Float" || expectedType == "Double";
    const char* instruction = sourceFloating   ? "fpext"
                              : targetFloating ? "sitofp"
                                               : "sext";
    out << "  %" << widened << " = " << instruction << ' ' << sourceLlvmType << ' '
        << lowered << " to " << targetLlvmType << "\n";
    state.values[widened] = targetLlvmType;
    state.simpleTypes[widened] = expectedType;
    lowered = "%" + widened;
  }
  if (!supported || !state.hasShadowFrame || lowered.empty() || lowered == "null") {
    return lowered;
  }

  if (value.kind == nir::ValueKind::New) {
    return lowered;
  }

  const std::string simpleType = inferSimpleType(value, state);
  if (!isReferenceType(simpleType)) {
    return lowered;
  }

  if (state.nextTemporaryRootSlot >= state.temporaryRootSlots.size()) {
    supported = false;
    return defaultValue(expectedType);
  }
  const std::string& rootSlot = state.temporaryRootSlots[state.nextTemporaryRootSlot++];
  out << "  store ptr " << lowered << ", ptr %" << rootSlot << "\n";
  return lowered;
}

std::vector<std::string>
parameterNames(const nir::Definition& definition,
               const std::vector<std::string>& parameterTypes) {
  std::vector<std::string> names;
  for (const nir::Instruction& instruction : definition.body.instructions) {
    if (instruction.kind == nir::InstructionKind::Param) {
      names.push_back(sanitizeIdentifier(instruction.name));
    }
  }
  while (names.size() < parameterTypes.size()) {
    names.push_back("arg" + std::to_string(names.size()));
  }
  return names;
}

bool mayProduceReferenceTemporary(nir::ValueKind kind) {
  switch (kind) {
  case nir::ValueKind::Local:
  case nir::ValueKind::Super:
  case nir::ValueKind::New:
  case nir::ValueKind::Select:
  case nir::ValueKind::Call:
  case nir::ValueKind::Block:
  case nir::ValueKind::If:
  case nir::ValueKind::Try:
  case nir::ValueKind::Catch:
  case nir::ValueKind::ZoneScoped:
  case nir::ValueKind::Box:
  case nir::ValueKind::Unbox:
  case nir::ValueKind::AsInstanceOf:
    return true;
  case nir::ValueKind::Unit:
  case nir::ValueKind::Literal:
  case nir::ValueKind::SizeOf:
  case nir::ValueKind::Unary:
  case nir::ValueKind::Binary:
  case nir::ValueKind::Assign:
  case nir::ValueKind::Throw:
  case nir::ValueKind::Finally:
  case nir::ValueKind::While:
  case nir::ValueKind::LocalLet:
  case nir::ValueKind::LocalVar:
  case nir::ValueKind::IsInstanceOf:
  case nir::ValueKind::Unknown:
    return false;
  }
  return false;
}

std::size_t countTemporaryRootCandidates(const nir::Value& value) {
  std::size_t count = mayProduceReferenceTemporary(value.kind) ? 1U : 0U;
  if (value.kind == nir::ValueKind::Try) {
    ++count;
  }
  for (const nir::Value& operand : value.operands) {
    count += countTemporaryRootCandidates(operand);
  }
  return count;
}

std::size_t countReferenceLocalBindings(const nir::Value& value) {
  std::size_t count = (value.kind == nir::ValueKind::LocalLet ||
                       value.kind == nir::ValueKind::LocalVar) &&
                              isReferenceType(value.type)
                          ? 1U
                          : 0U;
  for (const nir::Value& operand : value.operands) {
    count += countReferenceLocalBindings(operand);
  }
  return count;
}

bool containsExceptionHandler(const nir::Value& value) {
  if (value.kind == nir::ValueKind::Try) {
    return true;
  }
  return std::any_of(value.operands.begin(), value.operands.end(),
                     containsExceptionHandler);
}

std::string takeBindingRootSlot(const std::string& simpleType, LoweringState& state) {
  if (!isReferenceType(simpleType)) {
    return {};
  }
  if (state.nextBindingRootSlot >= state.bindingRootSlots.size()) {
    return {};
  }
  return state.bindingRootSlots[state.nextBindingRootSlot++];
}

void emitLet(const nir::Instruction& instruction, LoweringState& state,
             std::ostringstream& out) {
  const std::string sourceName = sanitizeIdentifier(instruction.name);
  const std::string name = nextLocalValueName(sourceName, state);
  const std::string valueSimpleType = inferSimpleType(instruction.value, state);
  const std::string simpleType =
      instruction.type.empty() ? valueSimpleType : instruction.type;
  const std::string bindingRootSlot = takeBindingRootSlot(simpleType, state);
  bool supported = false;
  const std::string lowered =
      lowerValue(instruction.value, simpleType, state, out, supported);
  if (llvmType(simpleType) == "void") {
    if (!supported) {
      out << "  ; unsupported " << nir::instructionToText(instruction) << '\n';
      reportUnsupported(state, diagnosticSpan(instruction),
                        "unsupported " + nir::instructionToText(instruction));
    }
    state.values[sourceName] = "void";
    state.localValueNames[sourceName] = name;
    state.simpleTypes[sourceName] = simpleType;
    state.exactLocals.erase(sourceName);
    return;
  }
  if (!supported) {
    out << "  ; unsupported " << nir::instructionToText(instruction) << '\n';
    reportUnsupported(state, diagnosticSpan(instruction),
                      "unsupported " + nir::instructionToText(instruction));
    state.values[sourceName] = "i32";
    state.localValueNames[sourceName] = name;
    state.simpleTypes[sourceName] = "Int";
    state.exactLocals.erase(sourceName);
    out << "  %" << name << " = add i32 0, 0\n";
    return;
  }
  if (llvmType(simpleType) == "ptr") {
    state.values[sourceName] = "ptr";
    state.localValueNames[sourceName] = name;
    state.simpleTypes[sourceName] = simpleType;
    if (instruction.value.kind == nir::ValueKind::New &&
        valueSimpleType == simpleType) {
      state.exactLocals.insert(sourceName);
    } else {
      state.exactLocals.erase(sourceName);
    }
    out << "  %" << name << " = getelementptr i8, ptr " << lowered << ", i64 0\n";
    if (!bindingRootSlot.empty()) {
      state.shadowRootSlots[name] = bindingRootSlot;
      out << "  store ptr %" << name << ", ptr %" << bindingRootSlot << "\n";
    }
    return;
  }

  state.values[sourceName] = llvmType(simpleType);
  state.localValueNames[sourceName] = name;
  state.simpleTypes[sourceName] = simpleType;
  state.exactLocals.erase(sourceName);
  const std::string loweredType = llvmType(simpleType);
  if (loweredType == "float" || loweredType == "double") {
    out << "  %" << name << " = fadd " << loweredType << ' ' << lowered
        << ", 0.000000e+00\n";
  } else {
    out << "  %" << name << " = add " << loweredType << ' ' << lowered << ", 0\n";
  }
}

void emitVar(const nir::Instruction& instruction, LoweringState& state,
             std::ostringstream& out) {
  const std::string sourceName = sanitizeIdentifier(instruction.name);
  const std::string name = nextLocalValueName(sourceName, state);
  const std::string valueSimpleType = inferSimpleType(instruction.value, state);
  const std::string simpleType =
      instruction.type.empty() ? valueSimpleType : instruction.type;
  const std::string bindingRootSlot = takeBindingRootSlot(simpleType, state);
  const std::string loweredType = llvmType(simpleType);
  if (loweredType == "void") {
    bool supported = false;
    (void)lowerValue(instruction.value, simpleType, state, out, supported);
    if (!supported) {
      out << "  ; unsupported " << nir::instructionToText(instruction) << '\n';
      reportUnsupported(state, diagnosticSpan(instruction),
                        "unsupported " + nir::instructionToText(instruction));
      return;
    }
    state.values[sourceName] = "void";
    state.localValueNames[sourceName] = name;
    state.simpleTypes[sourceName] = simpleType;
    state.mutableUnitLocals.insert(sourceName);
    state.exactLocals.erase(sourceName);
    return;
  }

  bool supported = false;
  const std::string lowered =
      lowerValue(instruction.value, simpleType, state, out, supported);
  if (!supported) {
    out << "  ; unsupported " << nir::instructionToText(instruction) << '\n';
    reportUnsupported(state, diagnosticSpan(instruction),
                      "unsupported " + nir::instructionToText(instruction));
    return;
  }

  const std::string slot = name + "_slot";
  out << "  %" << slot << " = alloca " << loweredType << "\n";
  out << "  store ";
  if (state.preserveMutableLocalsAcrossHandlers) {
    out << "volatile ";
  }
  out << loweredType << ' ' << lowered << ", ptr %" << slot << "\n";
  if (!bindingRootSlot.empty()) {
    state.shadowRootSlots[name] = bindingRootSlot;
    out << "  store ptr " << lowered << ", ptr %" << bindingRootSlot << "\n";
  }
  state.mutableLocalSlots[sourceName] = slot;
  state.localValueNames[sourceName] = name;
  state.simpleTypes[sourceName] = simpleType;
  state.exactLocals.erase(name);
}

void emitEval(const nir::Instruction& instruction, LoweringState& state,
              std::ostringstream& out) {
  const std::string simpleType = inferSimpleType(instruction.value, state);
  bool supported = false;
  (void)lowerValue(instruction.value, simpleType, state, out, supported);
  if (!supported) {
    out << "  ; eval " << nir::valueToText(instruction.value) << '\n';
    reportUnsupported(state, diagnosticSpan(instruction),
                      "unsupported eval " + nir::valueToText(instruction.value));
  }
}

void emitShadowPop(LoweringState& state, std::ostringstream& out) {
  if (!state.hasShadowFrame) {
    return;
  }
  out << "  store ptr %__shadow_previous_value, ptr @__scalanative_shadow_stack\n";
}

void emitSourcePop(const LoweringState& state, std::ostringstream& out) {
  if (!state.hasSourceFrame) {
    return;
  }
  out << "  store ptr %__source_previous_value, ptr @__scalanative_source_stack\n";
}

void emitReturn(const nir::Instruction& instruction, const std::string& returnType,
                LoweringState& state, std::ostringstream& out) {
  const std::string loweredType = llvmType(returnType);
  if (loweredType == "void") {
    bool supported = false;
    (void)lowerValue(instruction.value, returnType, state, out, supported);
    if (!supported && instruction.value.kind != nir::ValueKind::Unit) {
      out << "  ; unsupported return expression: "
          << nir::valueToText(instruction.value) << '\n';
      reportUnsupported(state, diagnosticSpan(instruction),
                        "unsupported return expression " +
                            nir::valueToText(instruction.value));
    }
    emitShadowPop(state, out);
    emitSourcePop(state, out);
    out << "  ret void\n";
    return;
  }

  bool supported = false;
  const std::string value =
      lowerValue(instruction.value, returnType, state, out, supported);
  if (!supported) {
    out << "  ; unsupported return expression: " << nir::valueToText(instruction.value)
        << '\n';
    reportUnsupported(state, diagnosticSpan(instruction),
                      "unsupported return expression " +
                          nir::valueToText(instruction.value));
  }
  emitShadowPop(state, out);
  emitSourcePop(state, out);
  out << "  ret " << loweredType << ' '
      << (supported ? value : defaultValue(returnType)) << '\n';
}

void emitThrow(const nir::Instruction& instruction, LoweringState& state,
               std::ostringstream& out) {
  const std::string exceptionType = inferSimpleType(instruction.value, state);
  bool supported = false;
  const std::string exception =
      lowerValue(instruction.value, exceptionType, state, out, supported);
  if (!supported || llvmType(exceptionType) != "ptr") {
    reportUnsupported(state, diagnosticSpan(instruction),
                      "unsupported throw operand " +
                          nir::valueToText(instruction.value));
  }
  emitSourceLocation(diagnosticSpan(instruction), state, out);
  emitShadowPop(state, out);
  const std::string checkedException = requireNonNullThrownException(
      supported && llvmType(exceptionType) == "ptr" ? exception : "null", state, out);
  out << "  call void @__scalanative_throw(ptr " << checkedException << ")\n";
  out << "  unreachable\n";
}

void emitFunction(
    const nir::Definition& definition, const std::string& moduleName,
    const std::unordered_set<std::string>& globals,
    const std::unordered_map<std::string, Signature>& functionSignatures,
    const std::unordered_map<std::string, FieldInfo>& fields,
    const std::unordered_map<std::string, ClassLayout>& classLayouts,
    const std::unordered_map<std::string, std::vector<std::string>>& classParents,
    const std::unordered_map<std::string, StringConstant>& stringConstants,
    const SourceFrameInfo* sourceFrame, const support::SourceManager* sources,
    std::vector<CodegenError>& errors, const DebugMetadata& debugMetadata,
    std::ostringstream& out) {
  const Signature signature = parseSignature(definition.signature);
  const std::string returnType = llvmType(signature.returnType);
  const std::vector<std::string> names =
      parameterNames(definition, signature.parameterTypes);

  LoweringState state;
  state.moduleName = moduleName;
  state.ownerName = ownerNameOf(definition.name);
  state.globals = &globals;
  state.functionSignatures = &functionSignatures;
  state.fields = &fields;
  state.classLayouts = &classLayouts;
  state.classParents = &classParents;
  state.stringConstants = &stringConstants;
  state.sources = sources;
  state.errors = &errors;
  state.functionName = definition.name;
  state.preserveMutableLocalsAcrossHandlers = std::any_of(
      definition.body.instructions.begin(), definition.body.instructions.end(),
      [](const nir::Instruction& instruction) {
        return containsExceptionHandler(instruction.value);
      });
  if (sourceFrame != nullptr) {
    state.source = sourceFrame->source;
    state.hasSourceFrame = true;
  }

  std::size_t bindingRootCount = 0;
  for (std::size_t i = 0; i < signature.parameterTypes.size(); ++i) {
    if (isReferenceType(signature.parameterTypes[i])) {
      ++bindingRootCount;
    }
  }
  std::size_t temporaryRootCount = 0;
  for (const nir::Instruction& instruction : definition.body.instructions) {
    if ((instruction.kind == nir::InstructionKind::Let ||
         instruction.kind == nir::InstructionKind::Var) &&
        isReferenceType(instruction.type)) {
      ++bindingRootCount;
    }
    bindingRootCount += countReferenceLocalBindings(instruction.value);
    temporaryRootCount += countTemporaryRootCandidates(instruction.value);
  }

  out << "define " << returnType << " @" << sanitizeIdentifier(definition.name) << '(';
  for (std::size_t i = 0; i < signature.parameterTypes.size(); ++i) {
    if (i != 0) {
      out << ", ";
    }
    const std::string lowered = llvmType(signature.parameterTypes[i]);
    out << lowered << " %" << names[i];
    state.values[names[i]] = lowered;
    state.localValueNames[names[i]] = names[i];
    state.simpleTypes[names[i]] = signature.parameterTypes[i];
    state.usedLocalValueNames.insert(names[i]);
  }
  out << ')';
  const std::optional<std::size_t> debugSubprogram =
      debugMetadata.subprogramFor(definition);
  if (debugSubprogram.has_value()) {
    out << " !dbg !" << *debugSubprogram;
  }
  out << " {\nentry:\n";

  if (sourceFrame != nullptr) {
    out << "  %__source_frame = alloca %scalanative.source_frame\n";
    out << "  %__source_previous_field = getelementptr %scalanative.source_frame, "
           "ptr %__source_frame, i32 0, i32 0\n";
    out << "  %__source_previous_value = load ptr, ptr @__scalanative_source_stack\n";
    out << "  store ptr %__source_previous_value, ptr %__source_previous_field\n";
    out << "  %__source_function_field = getelementptr %scalanative.source_frame, "
           "ptr %__source_frame, i32 0, i32 1\n";
    out << "  store ptr "
        << sourceCStringPointer(sourceFrame->functionConstant,
                                sourceFrame->functionName)
        << ", ptr %__source_function_field\n";
    out << "  %__source_file_field = getelementptr %scalanative.source_frame, ptr "
           "%__source_frame, i32 0, i32 2\n";
    out << "  store ptr "
        << sourceCStringPointer(sourceFrame->fileConstant, sourceFrame->fileName)
        << ", ptr %__source_file_field\n";
    out << "  %__source_line_field = getelementptr %scalanative.source_frame, ptr "
           "%__source_frame, i32 0, i32 3\n";
    out << "  store i32 " << sourceCoordinate(sourceFrame->line)
        << ", ptr %__source_line_field\n";
    out << "  %__source_column_field = getelementptr %scalanative.source_frame, ptr "
           "%__source_frame, i32 0, i32 4\n";
    out << "  store i32 " << sourceCoordinate(sourceFrame->column)
        << ", ptr %__source_column_field\n";
    out << "  store ptr %__source_frame, ptr @__scalanative_source_stack\n";
  }

  std::size_t debugParameterIndex = 0;
  for (const nir::Instruction& instruction : definition.body.instructions) {
    if (instruction.kind != nir::InstructionKind::Param) {
      continue;
    }
    if (debugParameterIndex >= signature.parameterTypes.size() ||
        debugParameterIndex >= names.size()) {
      break;
    }
    const std::optional<DebugVariableBinding> variable =
        debugMetadata.variableFor(instruction);
    if (variable.has_value()) {
      const std::string loweredType =
          llvmType(signature.parameterTypes[debugParameterIndex]);
      if (loweredType != "void") {
        out << "  call void @llvm.dbg.value(metadata " << loweredType << " %"
            << names[debugParameterIndex] << ", metadata !" << variable->variableNode
            << ", metadata !" << debugMetadata.expressionNode() << "), !dbg !"
            << variable->locationNode << '\n';
      }
    }
    ++debugParameterIndex;
  }

  const std::size_t shadowRootCount = bindingRootCount + temporaryRootCount;
  if (shadowRootCount != 0) {
    state.hasShadowFrame = true;
    out << "  %__shadow_roots = alloca [" << shadowRootCount << " x ptr]\n";
    out << "  call void @llvm.memset.p0.i64(ptr %__shadow_roots, i8 0, i64 "
        << shadowRootCount * 8 << ", i1 false)\n";
    out << "  %__shadow_frame = alloca %scalanative.shadow_frame\n";
    out << "  %__shadow_previous_field = getelementptr %scalanative.shadow_frame, ptr "
           "%__shadow_frame, i32 0, i32 0\n";
    out << "  %__shadow_previous_value = load ptr, ptr @__scalanative_shadow_stack\n";
    out << "  store ptr %__shadow_previous_value, ptr %__shadow_previous_field\n";
    out << "  %__shadow_count_field = getelementptr %scalanative.shadow_frame, ptr "
           "%__shadow_frame, i32 0, i32 1\n";
    out << "  store i32 " << shadowRootCount << ", ptr %__shadow_count_field\n";
    out << "  %__shadow_roots_field = getelementptr %scalanative.shadow_frame, ptr "
           "%__shadow_frame, i32 0, i32 2\n";
    out << "  store ptr %__shadow_roots, ptr %__shadow_roots_field\n";
    out << "  store ptr %__shadow_frame, ptr @__scalanative_shadow_stack\n";
    for (std::size_t i = 0; i < shadowRootCount; ++i) {
      const std::string slot = "__shadow_root_" + std::to_string(i);
      out << "  %" << slot << " = getelementptr [" << shadowRootCount
          << " x ptr], ptr %__shadow_roots, i32 0, i32 " << i << "\n";
      if (i < bindingRootCount) {
        state.bindingRootSlots.push_back(slot);
      } else {
        state.temporaryRootSlots.push_back(slot);
      }
    }
    for (std::size_t i = 0; i < signature.parameterTypes.size(); ++i) {
      if (isReferenceType(signature.parameterTypes[i])) {
        const std::string slot =
            takeBindingRootSlot(signature.parameterTypes[i], state);
        state.shadowRootSlots[names[i]] = slot;
        out << "  store ptr %" << names[i] << ", ptr %" << slot << "\n";
      }
    }
  }

  auto ownerLayout = classLayouts.find(state.ownerName);
  if (ownerLayout != classLayouts.end() && ownerLayout->second.isModule &&
      memberNameOf(definition.name) != support::StdNames::Constructor) {
    out << "  call ptr @" << moduleAccessorName(state.ownerName) << "()\n";
  }

  bool emittedReturn = false;
  for (const nir::Instruction& instruction : definition.body.instructions) {
    if (instruction.kind != nir::InstructionKind::Param) {
      emitSourceLocation(diagnosticSpan(instruction), state, out);
    }
    if (instruction.kind != nir::InstructionKind::Param) {
      const std::optional<std::size_t> location =
          debugMetadata.locationFor(instruction);
      if (location.has_value()) {
        out << "  call void @llvm.donothing(), !dbg !" << *location << '\n';
      }
    }
    switch (instruction.kind) {
    case nir::InstructionKind::Param:
      break;
    case nir::InstructionKind::Let: {
      emitLet(instruction, state, out);
      const std::optional<DebugVariableBinding> variable =
          debugMetadata.variableFor(instruction);
      const std::string sourceName = sanitizeIdentifier(instruction.name);
      auto localName = state.localValueNames.find(sourceName);
      auto simpleType = state.simpleTypes.find(sourceName);
      if (variable.has_value() && localName != state.localValueNames.end() &&
          simpleType != state.simpleTypes.end() &&
          llvmType(simpleType->second) != "void") {
        out << "  call void @llvm.dbg.value(metadata " << llvmType(simpleType->second)
            << " %" << localName->second << ", metadata !" << variable->variableNode
            << ", metadata !" << debugMetadata.expressionNode() << "), !dbg !"
            << variable->locationNode << '\n';
      }
      break;
    }
    case nir::InstructionKind::Var: {
      emitVar(instruction, state, out);
      const std::optional<DebugVariableBinding> variable =
          debugMetadata.variableFor(instruction);
      const std::string sourceName = sanitizeIdentifier(instruction.name);
      auto slot = state.mutableLocalSlots.find(sourceName);
      if (variable.has_value() && slot != state.mutableLocalSlots.end()) {
        out << "  call void @llvm.dbg.declare(metadata ptr %" << slot->second
            << ", metadata !" << variable->variableNode << ", metadata !"
            << debugMetadata.expressionNode() << "), !dbg !" << variable->locationNode
            << '\n';
      }
      break;
    }
    case nir::InstructionKind::Eval:
      emitEval(instruction, state, out);
      break;
    case nir::InstructionKind::Return:
      emitReturn(instruction, signature.returnType, state, out);
      emittedReturn = true;
      break;
    case nir::InstructionKind::Throw:
      emitThrow(instruction, state, out);
      emittedReturn = true;
      break;
    case nir::InstructionKind::Unreachable:
      out << "  unreachable\n";
      emittedReturn = true;
      break;
    }
    if (emittedReturn) {
      break;
    }
  }

  if (!emittedReturn) {
    emitShadowPop(state, out);
    emitSourcePop(state, out);
    if (returnType == "void") {
      out << "  ret void\n";
    } else {
      out << "  ret " << returnType << ' ' << defaultValue(signature.returnType)
          << '\n';
    }
  }
  out << "}\n\n";
}

void addStringConstant(const std::string& literalText,
                       std::unordered_map<std::string, StringConstant>& stringConstants,
                       std::vector<std::string>& stringConstantOrder) {
  if (stringConstants.contains(literalText)) {
    return;
  }

  StringConstant constant;
  constant.name = ".str." + std::to_string(stringConstants.size());
  constant.literalText = literalText;
  constant.contents = decodeStringLiteral(literalText);
  stringConstantOrder.push_back(literalText);
  stringConstants.emplace(literalText, std::move(constant));
}

void collectStringConstants(
    const nir::Value& value,
    std::unordered_map<std::string, StringConstant>& stringConstants,
    std::vector<std::string>& stringConstantOrder) {
  if (value.kind == nir::ValueKind::Literal &&
      (value.type == "String" || value.type == "Symbol")) {
    addStringConstant(value.text, stringConstants, stringConstantOrder);
  }
  for (const nir::Value& operand : value.operands) {
    collectStringConstants(operand, stringConstants, stringConstantOrder);
  }
}

void collectStackSuperSites(const nir::Value& value, std::vector<StackSuperSite>& sites,
                            std::unordered_set<std::string>& seenSites) {
  if (isStackSuperSelect(value)) {
    const std::string ownerName = value.operands.front().type;
    const std::string key = stackSuperSlotName(ownerName, value.text);
    if (!ownerName.empty() && !value.text.empty() && seenSites.insert(key).second) {
      sites.push_back(StackSuperSite{ownerName, value.text});
    }
  }
  for (const nir::Value& operand : value.operands) {
    collectStackSuperSites(operand, sites, seenSites);
  }
}

void collectStackSuperSites(const nir::FunctionBody& body,
                            std::vector<StackSuperSite>& sites,
                            std::unordered_set<std::string>& seenSites) {
  for (const nir::Instruction& instruction : body.instructions) {
    collectStackSuperSites(instruction.value, sites, seenSites);
  }
}

void collectStringConstants(
    const nir::FunctionBody& body,
    std::unordered_map<std::string, StringConstant>& stringConstants,
    std::vector<std::string>& stringConstantOrder) {
  for (const nir::Instruction& instruction : body.instructions) {
    collectStringConstants(instruction.value, stringConstants, stringConstantOrder);
  }
}

void emitStringConstants(
    const std::unordered_map<std::string, StringConstant>& stringConstants,
    const std::vector<std::string>& stringConstantOrder, std::ostringstream& out) {
  for (const std::string& literalText : stringConstantOrder) {
    const StringConstant& constant = stringConstants.at(literalText);
    out << '@' << constant.name << " = private unnamed_addr constant ["
        << (constant.contents.size() + 1) << " x i8] c\""
        << llvmCString(constant.contents) << "\\00\"\n";
  }
  if (!stringConstantOrder.empty()) {
    out << '\n';
  }
}

void emitSourceFrameConstants(
    const std::unordered_map<const nir::Definition*, SourceFrameInfo>& sourceFrames,
    const std::vector<const nir::Definition*>& sourceFrameOrder,
    std::ostringstream& out) {
  for (const nir::Definition* definition : sourceFrameOrder) {
    const SourceFrameInfo& frame = sourceFrames.at(definition);
    out << '@' << frame.functionConstant << " = private unnamed_addr constant ["
        << (frame.functionName.size() + 1) << " x i8] c\""
        << llvmCString(frame.functionName) << "\\00\"\n";
    out << '@' << frame.fileConstant << " = private unnamed_addr constant ["
        << (frame.fileName.size() + 1) << " x i8] c\"" << llvmCString(frame.fileName)
        << "\\00\"\n";
  }
  if (!sourceFrameOrder.empty()) {
    out << '\n';
  }
}

} // namespace

CodegenResult LlvmCodegen::emit(const linker::LinkedProgram& program,
                                const CodegenOptions& options) const {
  std::ostringstream out;
  std::vector<CodegenError> errors;
  out << "; ModuleID = 'cpp-scalanative'\n";
  out << "; Runtime ABI = '" << runtime::runtimeAbiName() << "'\n";
  out << "source_filename = \"cpp-scalanative\"\n\n";

  std::unordered_set<std::string> globals;
  std::unordered_map<std::string, Signature> functionSignatures;
  std::unordered_map<std::string, FieldInfo> fields;
  std::unordered_map<std::string, ClassLayout> classLayouts;
  std::unordered_map<std::string, std::vector<std::string>> classParents;
  std::unordered_map<std::string, std::vector<std::string>> classMethods;
  std::unordered_set<std::string> concreteMethods;
  std::vector<StackSuperSite> stackSuperSites;
  std::unordered_set<std::string> seenStackSuperSites;
  std::unordered_map<std::string, std::vector<const nir::Definition*>>
      classFieldDefinitions;
  std::unordered_map<std::string, StringConstant> stringConstants;
  std::vector<std::string> stringConstantOrder;
  std::unordered_map<const nir::Definition*, SourceFrameInfo> sourceFrames;
  std::vector<const nir::Definition*> sourceFrameOrder;
  for (const nir::Module& module : program.modules) {
    for (const nir::Definition& definition : module.definitions) {
      globals.insert(definition.name);
      if (definition.kind == nir::DefinitionKind::Module ||
          definition.kind == nir::DefinitionKind::Class ||
          definition.kind == nir::DefinitionKind::Trait) {
        std::vector<std::string> parents =
            nir::metadataParentNames(definition.signature);
        parents.erase(std::remove(parents.begin(), parents.end(),
                                  support::StdNames::JavaLangObject),
                      parents.end());
        if (!parents.empty()) {
          classParents[definition.name] = std::move(parents);
        }
      }
      if (definition.kind == nir::DefinitionKind::Module ||
          definition.kind == nir::DefinitionKind::Class ||
          definition.kind == nir::DefinitionKind::Trait) {
        ClassLayout& layout = classLayouts[definition.name];
        layout.isTrait = definition.kind == nir::DefinitionKind::Trait;
        layout.isModule = definition.kind == nir::DefinitionKind::Module;
      }
      if (definition.kind == nir::DefinitionKind::Field) {
        classFieldDefinitions[ownerNameOf(definition.name)].push_back(&definition);
      }
      if (definition.kind == nir::DefinitionKind::FunctionDecl ||
          definition.kind == nir::DefinitionKind::FunctionDef) {
        functionSignatures[definition.name] = parseSignature(definition.signature);
      }
      if (definition.kind == nir::DefinitionKind::FunctionDef) {
        concreteMethods.insert(definition.name);
        collectStringConstants(definition.body, stringConstants, stringConstantOrder);
        collectStackSuperSites(definition.body, stackSuperSites, seenStackSuperSites);
        if (options.sources != nullptr && definition.span.isValid() &&
            definition.name != support::StdNames::RuntimeMain) {
          const support::SourceFile* source =
              options.sources->get(definition.span.source);
          const auto [line, column] = options.sources->lineColumn(definition.span);
          if (source != nullptr && line != 0) {
            std::filesystem::path sourcePath(source->path);
            std::string fileName = sourcePath.filename().string();
            if (fileName.empty()) {
              fileName = source->path.empty() ? "<unknown>" : source->path;
            }
            const std::size_t index = sourceFrameOrder.size();
            sourceFrames.emplace(
                &definition,
                SourceFrameInfo{definition.span.source, definition.name,
                                std::move(fileName),
                                ".source.function." + std::to_string(index),
                                ".source.file." + std::to_string(index), line, column});
            sourceFrameOrder.push_back(&definition);
          }
        }
      }
      if (definition.kind == nir::DefinitionKind::Field && !definition.body.empty()) {
        collectStringConstants(definition.body, stringConstants, stringConstantOrder);
      }
    }
  }

  for (const nir::Module& module : program.modules) {
    for (const nir::Definition& definition : module.definitions) {
      if (definition.kind != nir::DefinitionKind::FunctionDecl &&
          definition.kind != nir::DefinitionKind::FunctionDef) {
        continue;
      }
      const std::string ownerName = ownerNameOf(definition.name);
      if (!classLayouts.contains(ownerName)) {
        continue;
      }
      auto signature = functionSignatures.find(definition.name);
      if (signature == functionSignatures.end() ||
          signature->second.parameterTypes.empty() ||
          signature->second.parameterTypes.front() != ownerName) {
        continue;
      }
      classMethods[ownerName].push_back(definition.name);
    }
  }
  buildVirtualLayouts(classMethods, classParents, concreteMethods, stackSuperSites,
                      classLayouts);
  buildFieldLayouts(classFieldDefinitions, classParents, classLayouts, fields);
  const std::size_t dynamicToStringSlotIndex =
      dynamicToStringVirtualSlotIndex(classLayouts, classMethods, functionSignatures);
  const DebugMetadata debugMetadata(program, options, classLayouts, fields,
                                    classParents);

  out << "declare i32 @puts(ptr)\n";
  out << "declare i32 @printf(ptr, ...)\n\n";
  out << "declare i32 @snprintf(ptr, i64, ptr, ...)\n\n";
  out << "declare i64 @strlen(ptr)\n";
  out << "declare ptr @strrchr(ptr, i32)\n";
  out << "declare ptr @strcpy(ptr, ptr)\n";
  out << "declare ptr @strcat(ptr, ptr)\n\n";
  out << "declare i32 @strcmp(ptr, ptr)\n\n";
  out << "declare ptr @malloc(i64)\n\n";
  out << "declare void @free(ptr)\n\n";
  out << "declare i32 @fflush(ptr)\n\n";
  out << "declare i32 @fprintf(ptr, ptr, ...)\n";
  out << "@stderr = external global ptr\n\n";
  out << "declare void @abort() noreturn nounwind\n\n";
  out << "declare i32 @_setjmp(ptr) returns_twice\n";
  out << "declare void @longjmp(ptr, i32) noreturn\n\n";
  out << "declare void @llvm.trap()\n\n";
  if (debugMetadata.hasLocations()) {
    out << "declare void @llvm.donothing()\n\n";
  }
  if (debugMetadata.hasValueVariables()) {
    out << "declare void @llvm.dbg.value(metadata, metadata, metadata)\n\n";
  }
  if (debugMetadata.hasMutableVariables()) {
    out << "declare void @llvm.dbg.declare(metadata, metadata, metadata)\n\n";
  }
  out << "declare void @llvm.memset.p0.i64(ptr nocapture writeonly, i8, i64, i1 "
         "immarg)\n\n";
  out << "declare void @llvm.memcpy.p0.p0.i64(ptr nocapture writeonly, ptr nocapture "
         "readonly, i64, i1 immarg)\n\n";
  out << "declare void @llvm.memmove.p0.p0.i64(ptr nocapture writeonly, ptr nocapture "
         "readonly, i64, i1 immarg)\n\n";
  out << "@.fmt.int = private unnamed_addr constant [4 x i8] c\"%d\\0A\\00\"\n\n";
  out << "@.fmt.long = private unnamed_addr constant [6 x i8] c\"%lld\\0A\\00\"\n";
  out << "@.fmt.float = private unnamed_addr constant [4 x i8] c\"%f\\0A\\00\"\n";
  out << "@.fmt.char = private unnamed_addr constant [4 x i8] c\"%c\\0A\\00\"\n";
  out << "@.fmt.string.int = private unnamed_addr constant [3 x i8] c\"%d\\00\"\n";
  out << "@.fmt.string.long = private unnamed_addr constant [5 x i8] c\"%lld\\00\"\n";
  out << "@.fmt.string.float = private unnamed_addr constant [3 x i8] c\"%f\\00\"\n";
  out << "@.fmt.string.char = private unnamed_addr constant [3 x i8] c\"%c\\00\"\n";
  out << "@.fmt.stack_trace_element = private unnamed_addr constant [13 x i8] "
         "c\"%s(%s:%d:%d)\\00\"\n";
  out << "@.str.boolean.true = private unnamed_addr constant [5 x i8] c\"true\\00\"\n";
  out << "@.str.boolean.false = private unnamed_addr constant [6 x i8] "
         "c\"false\\00\"\n";
  out << "@.str.unit = private unnamed_addr constant [3 x i8] c\"()\\00\"\n";
  out << "@.str.null = private unnamed_addr constant [5 x i8] c\"null\\00\"\n";
  out << "@.str.array_null = private unnamed_addr constant [21 x i8] "
         "c\"Array cannot be null\\00\"\n";
  out << "@.str.array_index_out_of_bounds = private unnamed_addr constant [29 x "
         "i8] c\"Array index is out of bounds\\00\"\n";
  out << "@.str.negative_array_size = private unnamed_addr constant [30 x i8] "
         "c\"Array size cannot be negative\\00\"\n";
  out << "@.str.class_cast = private unnamed_addr constant [39 x i8] "
         "c\"Value cannot be cast to requested type\\00\"\n";
  out << "@.str.array_store = private unnamed_addr constant [54 x i8] "
         "c\"Array element is not compatible with destination type\\00\"\n";
  out << "@.str.null_receiver = private unnamed_addr constant [24 x i8] "
         "c\"Receiver cannot be null\\00\"\n\n";
  out << "@.str.null_throw = private unnamed_addr constant [32 x i8] "
         "c\"Thrown exception cannot be null\\00\"\n\n";
  out << "@.str.integer_divisor_zero = private unnamed_addr constant [31 x i8] "
         "c\"Integer divisor cannot be zero\\00\"\n\n";
  out << "@.str.assertion_failed = private unnamed_addr constant [17 x i8] "
         "c\"Assertion failed\\00\"\n\n";
  out << "@.str.assumption_failed = private unnamed_addr constant [18 x i8] "
         "c\"Assumption failed\\00\"\n\n";
  out << "@.str.requirement_failed = private unnamed_addr constant [19 x i8] "
         "c\"Requirement failed\\00\"\n\n";
  out << "@.str.array_range_zero_step = private unnamed_addr constant [10 x i8] "
         "c\"zero step\\00\"\n";
  out << "@.str.array_range_too_large = private unnamed_addr constant [25 x i8] "
         "c\"Array range is too large\\00\"\n";
  out << "@.str.array_concat_too_large = private unnamed_addr constant [33 x i8] "
         "c\"Array concatenation is too large\\00\"\n\n";
  out << "@.str.byte_buffer_position = private unnamed_addr constant [37 x i8] "
         "c\"ByteBuffer position is out of bounds\\00\"\n";
  out << "@.str.byte_buffer_limit = private unnamed_addr constant [34 x i8] "
         "c\"ByteBuffer limit is out of bounds\\00\"\n";
  out << "@.str.byte_buffer_underflow = private unnamed_addr constant [21 x i8] "
         "c\"ByteBuffer underflow\\00\"\n";
  out << "@.str.byte_buffer_overflow = private unnamed_addr constant [20 x i8] "
         "c\"ByteBuffer overflow\\00\"\n\n";
  out << "@.str.stack_trace_unknown = private unnamed_addr constant [10 x i8] "
         "c\"<unknown>\\00\"\n";
  out << "@.str.stack_trace_element_unknown = private unnamed_addr constant [25 x "
         "i8] c\"<unknown>(<unknown>:0:0)\\00\"\n\n";
  out << "%scalanative.type_descriptor = type { i32, i32, i64, i32, i32, i32, "
         "i32, ptr, ptr, i32, ptr, i32, ptr, i32, i32 }\n\n";
  const std::string suppressedArrayTypeName =
      "scala.scalanative.runtime.SuppressedArray";
  out << "@__typename_suppressed_array = private unnamed_addr constant ["
      << suppressedArrayTypeName.size() + 1 << " x i8] c\""
      << llvmCString(suppressedArrayTypeName) << "\\00\"\n";
  out << "@__trace_offsets_suppressed_array = private constant ["
      << SuppressedArrayTraceCount << " x i32] [";
  for (std::size_t index = 0; index < SuppressedExceptionCapacity; ++index) {
    if (index != 0) {
      out << ", ";
    }
    out << "i32 " << ObjectHeaderSize + 8 + index * 8;
  }
  out << ", i32 " << SuppressedArrayOwnerOffset << "]\n";
  out << "@__scalanative_suppressed_array_descriptor = private constant "
         "%scalanative.type_descriptor { i32 "
      << static_cast<std::uint32_t>(runtime::RuntimeTypeKind::Class) << ", i32 -1, i64 "
      << SuppressedArraySize << ", i32 8, i32 8, i32 "
      << SuppressedArraySize - ObjectHeaderSize
      << ", i32 8, ptr @__typename_suppressed_array, ptr null, i32 0, ptr null, "
         "i32 0, ptr @__trace_offsets_suppressed_array, i32 "
      << SuppressedArrayTraceCount << ", i32 "
      << runtime::objectOwnershipTag(runtime::ObjectOwnership::Gc) << " }, align 8\n\n";
  out << "%scalanative.gc_node = type { ptr, i1, ptr }\n";
  out << "%scalanative.arena_block = type { ptr, i64, i64, ptr }\n";
  out << "%scalanative.arena = type { ptr, i64, ptr }\n\n";
  out << "%scalanative.shadow_frame = type { ptr, i32, ptr }\n";
  out << "%scalanative.source_frame = type { ptr, ptr, ptr, i32, i32 }\n\n";
  out << "%scalanative.exception_trace_entry = type { ptr, ptr, i32, i32 }\n";
  out << "%scalanative.exception_trace = type { i32, [64 x "
         "%scalanative.exception_trace_entry] }\n\n";
  out << "%scalanative.exception_handler = type { ptr, ptr, ptr, ptr, ptr }\n\n";
  out << "@__scalanative_gc_head = private global ptr null\n";
  out << "@__scalanative_gc_allocation_count = private global i64 0\n";
  out << "@__scalanative_gc_collection_count = private global i64 0\n";
  out << "@__scalanative_gc_collection_threshold = private global i64 64\n";
  out << "@__scalanative_shadow_stack = private global ptr null\n";
  out << "@__scalanative_source_stack = private global ptr null\n";
  out << "@__scalanative_program_arena = private global ptr null\n";
  out << "@__scalanative_current_zone = private global ptr null\n";
  out << "@__scalanative_exception_handler = private global ptr null\n";
  out << "@__scalanative_current_exception = private global ptr null\n";
  out << "@__scalanative_reporting_exception = private global i1 false\n";
  out << "@__scalanative_runtime_state = private global i8 0\n\n";
  emitRuntimeTypeHelpers(out, dynamicToStringSlotIndex);
  emitBoxedPrimitiveRuntime(out);
  emitVtables(classLayouts, out);
  emitClassTypeDescriptors(classLayouts, classParents, out);
  emitExceptionRuntimeHelpers(classLayouts, fields, out);
  emitModuleSingletons(classLayouts, functionSignatures, out);
  emitGcCollector(classLayouts, out);
  emitRuntimeResources(out);
  emitStringConstants(stringConstants, stringConstantOrder, out);
  emitSourceFrameConstants(sourceFrames, sourceFrameOrder, out);

  for (const nir::Module& module : program.modules) {
    out << "; NIR module " << module.name << '\n';
    for (const nir::Definition& definition : module.definitions) {
      out << "; " << nir::definitionKindName(definition.kind) << " @" << definition.name
          << '\n';
    }
    out << '\n';
    for (const nir::Definition& definition : module.definitions) {
      if (definition.kind == nir::DefinitionKind::FunctionDef) {
        const auto sourceFrame = sourceFrames.find(&definition);
        emitFunction(definition, module.name, globals, functionSignatures, fields,
                     classLayouts, classParents, stringConstants,
                     sourceFrame == sourceFrames.end() ? nullptr : &sourceFrame->second,
                     options.sources, errors, debugMetadata, out);
      }
    }
  }

  auto runtimeMain =
      functionSignatures.find(std::string(support::StdNames::RuntimeMain));
  if (runtimeMain != functionSignatures.end() &&
      runtimeMain->second.parameterTypes.empty() &&
      llvmType(runtimeMain->second.returnType) == "i32") {
    out << "define i32 @main() {\n";
    out << "entry:\n";
    out << "  call void @__scalanative_runtime_startup()\n";
    out << "  %runtime_main = call i32 @scala_scalanative_runtime_main()\n";
    out << "  call void @__scalanative_runtime_shutdown()\n";
    out << "  ret i32 %runtime_main\n";
  } else if (runtimeMain != functionSignatures.end() &&
             runtimeMain->second.parameterTypes.size() == 1 &&
             isStringArrayType(runtimeMain->second.parameterTypes.front()) &&
             llvmType(runtimeMain->second.returnType) == "i32") {
    out << "define i32 @main(i32 %argc, ptr %argv) {\n";
    out << "entry:\n";
    out << "  call void @__scalanative_runtime_startup()\n";
    out << "  %args = call ptr @__scalanative_args_from_argv(i32 %argc, ptr %argv)\n";
    out << "  %runtime_main = call i32 @scala_scalanative_runtime_main(ptr %args)\n";
    out << "  call void @__scalanative_runtime_shutdown()\n";
    out << "  ret i32 %runtime_main\n";
  } else {
    out << "define i32 @main() {\n";
    out << "entry:\n";
    out << "  call void @__scalanative_runtime_startup()\n";
    out << "  call void @__scalanative_runtime_shutdown()\n";
    out << "  ret i32 0\n";
  }
  out << "}\n";
  debugMetadata.emit(out);
  CodegenResult result;
  result.ok = errors.empty();
  result.llvmIr = out.str();
  result.errors = std::move(errors);
  return result;
}

} // namespace scalanative::tools::codegen
