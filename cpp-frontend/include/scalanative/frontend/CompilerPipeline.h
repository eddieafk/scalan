#pragma once

#include "scalanative/frontend/Ast.h"
#include "scalanative/frontend/Typechecker.h"
#include "scalanative/support/Diagnostics.h"
#include "scalanative/support/SourceManager.h"

#include <string>
#include <vector>

namespace scalanative::frontend {

struct CompileResult {
  bool ok = false;
  AstModule ast;
  TypedModule typed;
  std::vector<std::string> phaseLog;
};

class CompilerPipeline {
public:
  [[nodiscard]] CompileResult compile(support::SourceManager& sources,
                                      support::SourceId source,
                                      support::DiagnosticEngine& diagnostics);
};

} // namespace scalanative::frontend

