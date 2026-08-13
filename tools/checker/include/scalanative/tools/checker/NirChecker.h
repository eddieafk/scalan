#pragma once

#include "scalanative/nir/Nir.h"
#include "scalanative/support/Diagnostics.h"

namespace scalanative::tools::checker {

class NirChecker {
public:
  [[nodiscard]] bool check(const nir::Module& module,
                           support::DiagnosticEngine& diagnostics) const;
};

} // namespace scalanative::tools::checker

