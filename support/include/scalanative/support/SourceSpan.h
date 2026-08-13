#pragma once

#include "scalanative/support/Ids.h"

#include <cstddef>

namespace scalanative::support {

struct SourceSpan {
  SourceId source;
  std::size_t start = 0;
  std::size_t length = 0;

  [[nodiscard]] static SourceSpan none() { return {}; }
  [[nodiscard]] bool isValid() const { return source.isValid(); }
};

} // namespace scalanative::support

