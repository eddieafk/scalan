#pragma once

#include "scalanative/nir/Nir.h"
#include "scalanative/support/Diagnostics.h"

#include <string>
#include <vector>

namespace scalanative::tools::linker {

struct LinkedProgram {
  std::vector<nir::Module> modules;
  std::vector<std::string> roots;
  std::vector<std::string> reachableGlobals;
};

struct LinkResult {
  bool ok = false;
  LinkedProgram program;
};

class Linker {
public:
  [[nodiscard]] LinkResult link(std::vector<nir::Module> modules,
                                support::DiagnosticEngine& diagnostics) const;
};

} // namespace scalanative::tools::linker
