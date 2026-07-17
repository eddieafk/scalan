#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace scalanative::runtime {

enum class MemoryMode { HybridGcArena };

enum class ObjectOwnership : std::uint32_t {
  Gc = 0,
  Arena = 1,
  Immortal = 2,
};

enum class RuntimeTypeKind : std::uint32_t {
  Class = 1,
  BoxedPrimitive = 2,
  Trait = 3,
  Module = 4,
};

enum class BoxedPrimitiveKind : std::uint32_t {
  Unsupported = 0,
  Boolean = 1,
  Int = 2,
  Long = 3,
  Float = 4,
  Double = 5,
  Char = 6,
  Unit = 7,
  Symbol = 8,
  String = 9,
};

struct BoxedPrimitiveDescriptor {
  std::uint32_t kind = 0;
  std::uint32_t payloadSize = 0;
  std::uint32_t payloadAlignment = 0;
};

struct RuntimeTypeLayout {
  RuntimeTypeKind kind = RuntimeTypeKind::Class;
  std::uint32_t typeId = 0;
  std::uint64_t instanceSize = 0;
  std::uint32_t instanceAlignment = 0;
  std::uint32_t payloadOffset = 0;
  std::uint32_t payloadSize = 0;
  std::uint32_t payloadAlignment = 0;
};

struct RuntimeConfig {
  MemoryMode memoryMode = MemoryMode::HybridGcArena;
  bool gcStress = false;
  bool arenaLifetimeChecks = true;
};

[[nodiscard]] const char* memoryModeName(MemoryMode mode);
[[nodiscard]] const char* objectOwnershipName(ObjectOwnership ownership);
[[nodiscard]] std::uint32_t objectOwnershipTag(ObjectOwnership ownership);
[[nodiscard]] const char* runtimeTypeKindName(RuntimeTypeKind kind);
[[nodiscard]] BoxedPrimitiveKind boxedPrimitiveKind(std::string_view typeName);
[[nodiscard]] const char* boxedPrimitiveKindName(BoxedPrimitiveKind kind);
[[nodiscard]] BoxedPrimitiveDescriptor
boxedPrimitiveDescriptor(BoxedPrimitiveKind kind);
[[nodiscard]] RuntimeTypeLayout boxedPrimitiveTypeLayout(BoxedPrimitiveKind kind);
[[nodiscard]] std::string runtimeAbiName();

} // namespace scalanative::runtime
