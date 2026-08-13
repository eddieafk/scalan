#pragma once

#include "scalanative/support/SourceSpan.h"
#include "scalanative/tools/linker/Linker.h"

#include <string>
#include <vector>

namespace scalanative::support {
class SourceManager;
}

namespace scalanative::tools::codegen {

struct CodegenOptions {
  const support::SourceManager* sources = nullptr;
  bool optimized = false;
  bool emitDebugInfo = true;
};

struct CodegenError {
  support::SourceSpan span;
  std::string message;
};

struct CodegenResult {
  bool ok = true;
  std::string llvmIr;
  std::vector<CodegenError> errors;
};

class LlvmCodegen {
public:
  [[nodiscard]] CodegenResult emit(const linker::LinkedProgram& program,
                                   const CodegenOptions& options = {}) const;
};

} // namespace scalanative::tools::codegen
