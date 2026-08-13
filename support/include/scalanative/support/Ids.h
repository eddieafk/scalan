#pragma once

#include <cstdint>
#include <limits>

namespace scalanative::support {

template <typename Tag> class StrongId {
public:
  using Value = std::uint32_t;

  constexpr StrongId()
      : value_(std::numeric_limits<Value>::max()) {}

  explicit constexpr StrongId(Value value)
      : value_(value) {}

  [[nodiscard]] constexpr Value value() const { return value_; }
  [[nodiscard]] constexpr bool isValid() const {
    return value_ != std::numeric_limits<Value>::max();
  }

  explicit constexpr operator bool() const { return isValid(); }

  friend constexpr bool operator==(StrongId lhs, StrongId rhs) {
    return lhs.value_ == rhs.value_;
  }

  friend constexpr bool operator!=(StrongId lhs, StrongId rhs) {
    return !(lhs == rhs);
  }

private:
  Value value_;
};

struct SourceIdTag;
struct NameIdTag;
struct NodeIdTag;
struct SymbolIdTag;
struct TypeIdTag;

using SourceId = StrongId<SourceIdTag>;
using NameId = StrongId<NameIdTag>;
using NodeId = StrongId<NodeIdTag>;
using SymbolId = StrongId<SymbolIdTag>;
using TypeId = StrongId<TypeIdTag>;

} // namespace scalanative::support

