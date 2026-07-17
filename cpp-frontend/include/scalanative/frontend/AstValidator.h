#pragma once

#include "scalanative/frontend/Ast.h"
#include "scalanative/support/Diagnostics.h"

namespace scalanative::frontend {

class AstValidator {
public:
  [[nodiscard]] bool validate(const AstModule& module,
                              support::DiagnosticEngine& diagnostics) const;

private:
  [[nodiscard]] bool validateScope(const std::vector<AstDeclaration>& declarations,
                                   support::DiagnosticEngine& diagnostics,
                                   bool isTopLevel) const;
  [[nodiscard]] bool validateDeclaration(const AstDeclaration& declaration,
                                         support::DiagnosticEngine& diagnostics,
                                         bool isTopLevel) const;
  [[nodiscard]] bool validateExpression(const AstExpression& expression,
                                        support::DiagnosticEngine& diagnostics) const;
};

} // namespace scalanative::frontend

