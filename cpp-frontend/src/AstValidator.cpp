#include "scalanative/frontend/AstValidator.h"

#include <unordered_set>

namespace scalanative::frontend {

bool AstValidator::validate(const AstModule& module,
                            support::DiagnosticEngine& diagnostics) const {
  bool ok = true;
  if (!module.packageName.empty()) {
    if (module.declarations.empty() ||
        module.declarations.front().kind != AstDeclarationKind::Package ||
        module.declarations.front().name != module.packageName) {
      diagnostics.error(support::SourceSpan::none(),
                        "module package declaration is inconsistent");
      ok = false;
    }
  }

  ok = validateScope(module.declarations, diagnostics, true) && ok;
  return ok;
}

bool AstValidator::validateScope(const std::vector<AstDeclaration>& declarations,
                                 support::DiagnosticEngine& diagnostics,
                                 bool isTopLevel) const {
  bool ok = true;
  std::unordered_set<std::string> names;

  for (const AstDeclaration& declaration : declarations) {
    if (declaration.kind != AstDeclarationKind::Package &&
        declaration.kind != AstDeclarationKind::Import) {
      if (declaration.name.empty()) {
        diagnostics.error(declaration.span, "declaration has no name");
        ok = false;
      } else if (!names.insert(declaration.name).second) {
        diagnostics.error(declaration.span,
                          "duplicate declaration in the same scope: " +
                              declaration.name);
        ok = false;
      }
    }

    ok = validateDeclaration(declaration, diagnostics, isTopLevel) && ok;
  }

  return ok;
}

bool AstValidator::validateDeclaration(const AstDeclaration& declaration,
                                       support::DiagnosticEngine& diagnostics,
                                       bool isTopLevel) const {
  bool ok = true;
  switch (declaration.kind) {
  case AstDeclarationKind::Package:
    if (!isTopLevel) {
      diagnostics.error(declaration.span, "package declaration cannot be nested");
      ok = false;
    }
    break;
  case AstDeclarationKind::Import:
    if (!isTopLevel) {
      diagnostics.error(declaration.span,
                        "import declaration is only supported at top level in "
                        "this subset");
      ok = false;
    }
    if (declaration.importPath.empty()) {
      diagnostics.error(declaration.span, "import declaration has no target");
      ok = false;
    }
    break;
  case AstDeclarationKind::Object:
    if (declaration.hasInitializer) {
      diagnostics.error(declaration.span,
                        "class-like declaration cannot have an initializer");
      ok = false;
    }
    ok = validateScope(declaration.members, diagnostics, false) && ok;
    break;
  case AstDeclarationKind::Trait:
    if (declaration.hasInitializer) {
      diagnostics.error(declaration.span,
                        "class-like declaration cannot have an initializer");
      ok = false;
    }
    if (!declaration.constructorBody.empty()) {
      diagnostics.error(declaration.span,
                        "constructor body statements are only supported in classes");
      ok = false;
    }
    ok = validateScope(declaration.members, diagnostics, false) && ok;
    break;
  case AstDeclarationKind::Class:
    if (declaration.hasInitializer) {
      diagnostics.error(declaration.span,
                        "class-like declaration cannot have an initializer");
      ok = false;
    }
    for (const AstExpression& expression : declaration.constructorBody) {
      ok = validateExpression(expression, diagnostics) && ok;
    }
    ok = validateScope(declaration.members, diagnostics, false) && ok;
    break;
  case AstDeclarationKind::Type:
    if (isTopLevel) {
      diagnostics.error(declaration.span,
                        "type declarations are only supported as members");
      ok = false;
    }
    if (declaration.hasInitializer && declaration.declaredType.empty()) {
      diagnostics.error(declaration.span, "type alias has no target");
      ok = false;
    }
    if (declaration.hasInitializer &&
        (!declaration.lowerBound.empty() || !declaration.upperBound.empty())) {
      diagnostics.error(declaration.span,
                        "type alias cannot declare bounds in this subset");
      ok = false;
    }
    if (!declaration.initializer.children.empty() ||
        declaration.initializer.kind != AstExpressionKind::Empty) {
      diagnostics.error(declaration.span,
                        "type declaration cannot have a value initializer");
      ok = false;
    }
    break;
  case AstDeclarationKind::Def:
    if (declaration.hasInitializer) {
      ok = validateExpression(declaration.initializer, diagnostics) && ok;
    }
    break;
  case AstDeclarationKind::Val:
  case AstDeclarationKind::Var:
    if (!declaration.hasInitializer && declaration.declaredType.empty()) {
      diagnostics.warning(declaration.span,
                          "value declaration has no initializer in this subset");
    } else if (declaration.hasInitializer) {
      ok = validateExpression(declaration.initializer, diagnostics) && ok;
    }
    break;
  }

  return ok;
}

bool AstValidator::validateExpression(const AstExpression& expression,
                                      support::DiagnosticEngine& diagnostics) const {
  bool ok = true;
  switch (expression.kind) {
  case AstExpressionKind::Empty:
    diagnostics.error(expression.span, "empty expression");
    return false;
  case AstExpressionKind::Unary:
    if (expression.children.size() != 1 || expression.text.empty()) {
      diagnostics.error(expression.span,
                        "unary expression must have one operand and an operator");
      ok = false;
    }
    break;
  case AstExpressionKind::Binary:
    if (expression.children.size() != 2) {
      diagnostics.error(expression.span, "binary expression must have two operands");
      ok = false;
    }
    break;
  case AstExpressionKind::Assign:
    if (expression.children.size() != 2) {
      diagnostics.error(expression.span,
                        "assignment expression must have target and value operands");
      ok = false;
    }
    break;
  case AstExpressionKind::Call:
    if (expression.children.empty()) {
      diagnostics.error(expression.span, "call expression has no callee");
      ok = false;
    }
    break;
  case AstExpressionKind::Select:
    if (expression.children.size() != 1 || expression.text.empty()) {
      diagnostics.error(expression.span,
                        "select expression must have a receiver and member name");
      ok = false;
    }
    break;
  case AstExpressionKind::TypeApply:
    if (expression.children.size() != 1 || expression.declaredType.empty()) {
      diagnostics.error(expression.span,
                        "type application must have a target and type argument");
      ok = false;
    }
    break;
  case AstExpressionKind::If:
    if (expression.children.size() < 2 || expression.children.size() > 3) {
      diagnostics.error(expression.span,
                        "if expression must have condition and branch children");
      ok = false;
    }
    break;
  case AstExpressionKind::While:
    if (expression.children.size() != 2) {
      diagnostics.error(expression.span,
                        "while expression must have condition and body children");
      ok = false;
    }
    break;
  case AstExpressionKind::Throw:
    if (expression.children.size() != 1) {
      diagnostics.error(expression.span,
                        "throw expression must have exactly one exception operand");
      ok = false;
    }
    break;
  case AstExpressionKind::Try: {
    if (expression.children.size() < 2) {
      diagnostics.error(expression.span,
                        "try expression must have a body and catch or finally");
      ok = false;
      break;
    }
    bool sawHandler = false;
    bool sawFinally = false;
    for (std::size_t index = 1; index < expression.children.size(); ++index) {
      const AstExpressionKind childKind = expression.children[index].kind;
      if (childKind == AstExpressionKind::Catch && !sawFinally) {
        sawHandler = true;
      } else if (childKind == AstExpressionKind::Finally && !sawFinally &&
                 index + 1 == expression.children.size()) {
        sawFinally = true;
      } else {
        diagnostics.error(expression.children[index].span,
                          "try children must be catches followed by one finalizer");
        ok = false;
      }
    }
    if (!sawHandler && !sawFinally) {
      diagnostics.error(expression.span,
                        "try expression must have a catch or finally child");
      ok = false;
    }
    break;
  }
  case AstExpressionKind::Catch:
    if (expression.children.size() != 1 || expression.text.empty() ||
        expression.declaredType.empty()) {
      diagnostics.error(expression.span,
                        "catch expression must have a typed binding and body");
      ok = false;
    }
    break;
  case AstExpressionKind::Finally:
    if (expression.children.size() != 1) {
      diagnostics.error(expression.span,
                        "finally expression must have exactly one body");
      ok = false;
    }
    break;
  case AstExpressionKind::LocalDeclaration:
    if (expression.text.empty()) {
      diagnostics.error(expression.span, "local declaration has no name");
      ok = false;
    }
    if (expression.children.size() > 1) {
      diagnostics.error(expression.span,
                        "local declaration must have at most one initializer");
      ok = false;
    }
    break;
  default:
    break;
  }

  for (const AstExpression& child : expression.children) {
    ok = validateExpression(child, diagnostics) && ok;
  }
  return ok;
}

} // namespace scalanative::frontend
