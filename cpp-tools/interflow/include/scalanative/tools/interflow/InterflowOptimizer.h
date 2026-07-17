#pragma once

#include "scalanative/tools/linker/Linker.h"

#include <cstddef>
#include <string>
#include <vector>

namespace scalanative::tools::interflow {

enum class OptimizationTier {
  Basic,
  Standard,
  Aggressive,
};

struct InterflowOptions {
  OptimizationTier tier = OptimizationTier::Standard;
};

struct PassReport {
  std::string name;
  std::size_t definitionsBefore = 0;
  std::size_t definitionsAfter = 0;
  std::size_t removedDefinitions = 0;
  std::size_t changedValues = 0;
  std::size_t validationErrorsBefore = 0;
  std::size_t validationErrorsAfter = 0;
  std::size_t durationMicros = 0;
};

struct InterflowResult {
  bool ok = true;
  linker::LinkedProgram program;
  std::vector<PassReport> reports;
  std::vector<std::string> errors;
  std::size_t removedDefinitions = 0;
  std::size_t changedValues = 0;
};

class InterflowOptimizer {
public:
  [[nodiscard]] InterflowResult optimize(linker::LinkedProgram program,
                                         const InterflowOptions& options = {}) const;
};

} // namespace scalanative::tools::interflow
