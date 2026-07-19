#include "scalanative/runtime/RuntimeConfig.h"

namespace scalanative::runtime {

const char* memoryModeName(MemoryMode mode) {
  switch (mode) {
  case MemoryMode::HybridGcArena:
    return "hybrid-gc-arena";
  }
  return "hybrid-gc-arena";
}

const char* objectOwnershipName(ObjectOwnership ownership) {
  switch (ownership) {
  case ObjectOwnership::Gc:
    return "gc";
  case ObjectOwnership::Arena:
    return "arena";
  case ObjectOwnership::Immortal:
    return "immortal";
  }
  return "gc";
}

std::uint32_t objectOwnershipTag(ObjectOwnership ownership) {
  return static_cast<std::uint32_t>(ownership);
}

const char* runtimeTypeKindName(RuntimeTypeKind kind) {
  switch (kind) {
  case RuntimeTypeKind::Class:
    return "class";
  case RuntimeTypeKind::BoxedPrimitive:
    return "boxed-primitive";
  case RuntimeTypeKind::Trait:
    return "trait";
  case RuntimeTypeKind::Module:
    return "module";
  }
  return "class";
}

BoxedPrimitiveKind boxedPrimitiveKind(std::string_view typeName) {
  if (typeName == "Unit") {
    return BoxedPrimitiveKind::Unit;
  }
  if (typeName == "Boolean") {
    return BoxedPrimitiveKind::Boolean;
  }
  if (typeName == "Byte") {
    return BoxedPrimitiveKind::Byte;
  }
  if (typeName == "Short") {
    return BoxedPrimitiveKind::Short;
  }
  if (typeName == "Int") {
    return BoxedPrimitiveKind::Int;
  }
  if (typeName == "Long") {
    return BoxedPrimitiveKind::Long;
  }
  if (typeName == "Float") {
    return BoxedPrimitiveKind::Float;
  }
  if (typeName == "Double") {
    return BoxedPrimitiveKind::Double;
  }
  if (typeName == "Char") {
    return BoxedPrimitiveKind::Char;
  }
  if (typeName == "Symbol") {
    return BoxedPrimitiveKind::Symbol;
  }
  if (typeName == "String") {
    return BoxedPrimitiveKind::String;
  }
  return BoxedPrimitiveKind::Unsupported;
}

const char* boxedPrimitiveKindName(BoxedPrimitiveKind kind) {
  switch (kind) {
  case BoxedPrimitiveKind::Unit:
    return "Unit";
  case BoxedPrimitiveKind::Boolean:
    return "Boolean";
  case BoxedPrimitiveKind::Byte:
    return "Byte";
  case BoxedPrimitiveKind::Short:
    return "Short";
  case BoxedPrimitiveKind::Int:
    return "Int";
  case BoxedPrimitiveKind::Long:
    return "Long";
  case BoxedPrimitiveKind::Float:
    return "Float";
  case BoxedPrimitiveKind::Double:
    return "Double";
  case BoxedPrimitiveKind::Char:
    return "Char";
  case BoxedPrimitiveKind::Symbol:
    return "Symbol";
  case BoxedPrimitiveKind::String:
    return "String";
  case BoxedPrimitiveKind::Unsupported:
    return "Unsupported";
  }
  return "Unsupported";
}

BoxedPrimitiveDescriptor boxedPrimitiveDescriptor(BoxedPrimitiveKind kind) {
  switch (kind) {
  case BoxedPrimitiveKind::Unit:
    return {static_cast<std::uint32_t>(kind), 0, 1};
  case BoxedPrimitiveKind::Boolean:
    return {static_cast<std::uint32_t>(kind), 1, 1};
  case BoxedPrimitiveKind::Byte:
    return {static_cast<std::uint32_t>(kind), 1, 1};
  case BoxedPrimitiveKind::Short:
    return {static_cast<std::uint32_t>(kind), 2, 2};
  case BoxedPrimitiveKind::Int:
  case BoxedPrimitiveKind::Float:
  case BoxedPrimitiveKind::Char:
    return {static_cast<std::uint32_t>(kind), 4, 4};
  case BoxedPrimitiveKind::Long:
  case BoxedPrimitiveKind::Double:
  case BoxedPrimitiveKind::Symbol:
  case BoxedPrimitiveKind::String:
    return {static_cast<std::uint32_t>(kind), 8, 8};
  case BoxedPrimitiveKind::Unsupported:
    return {};
  }
  return {};
}

RuntimeTypeLayout boxedPrimitiveTypeLayout(BoxedPrimitiveKind kind) {
  constexpr std::uint32_t objectHeaderSize = 8;
  const BoxedPrimitiveDescriptor descriptor = boxedPrimitiveDescriptor(kind);
  if (descriptor.kind == 0) {
    return {};
  }
  return {RuntimeTypeKind::BoxedPrimitive,
          descriptor.kind,
          objectHeaderSize + descriptor.payloadSize,
          8,
          objectHeaderSize,
          descriptor.payloadSize,
          descriptor.payloadAlignment};
}

std::string runtimeAbiName() {
  return "cpp-scalanative-runtime-55";
}

} // namespace scalanative::runtime
