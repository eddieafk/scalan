#pragma once

#include "scalanative/nir/Nir.h"

#include <string>
#include <vector>

namespace scalanative::nir {

struct VerifyResult {
  bool ok = true;
  std::vector<std::string> errors;
};

class Verifier {
public:
  [[nodiscard]] VerifyResult verify(const Module& module) const;
};

} // namespace scalanative::nir

