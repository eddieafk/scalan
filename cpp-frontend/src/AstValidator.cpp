#include "scalanative/frontend/AstValidator.h"

#include <algorithm>
#include <unordered_map>
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
  std::unordered_map<std::string, unsigned> names;

  for (const AstDeclaration& declaration : declarations) {
    if (declaration.kind != AstDeclarationKind::Package &&
        declaration.kind != AstDeclarationKind::Import) {
      if (declaration.name.empty()) {
        diagnostics.error(declaration.span, "declaration has no name");
        ok = false;
      } else {
        const unsigned kind = declaration.kind == AstDeclarationKind::Object ? 1U
                              : declaration.kind == AstDeclarationKind::Class ||
                                      declaration.kind == AstDeclarationKind::Trait
                                  ? 2U
                                  : 4U;
        unsigned& seen = names[declaration.name];
        const bool companionPair =
            isTopLevel && (seen | kind) == 3U && (seen & kind) == 0U;
        if (seen != 0U && !companionPair) {
          diagnostics.error(declaration.span,
                            "duplicate declaration in the same scope: " +
                                declaration.name);
          ok = false;
        }
        seen |= kind;
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
  if (declaration.isGiven && declaration.kind != AstDeclarationKind::Val &&
      declaration.kind != AstDeclarationKind::Def) {
    diagnostics.error(declaration.span, "given declaration must be a value or method");
    ok = false;
  }
  if (declaration.isGiven && declaration.kind == AstDeclarationKind::Def &&
      (declaration.isAnonymousGiven ||
       std::any_of(declaration.contextualParameters.begin(),
                   declaration.contextualParameters.end(),
                   [](bool contextual) { return !contextual; }))) {
    diagnostics.error(
        declaration.span,
        "parameterized given must be named and accept only using parameters");
    ok = false;
  }
  if (declaration.isAnonymousGiven && !declaration.isGiven) {
    diagnostics.error(declaration.span,
                      "anonymous-given metadata requires a given declaration");
    ok = false;
  }
  if (declaration.isGiven &&
      (declaration.declaredType.empty() || !declaration.hasInitializer)) {
    diagnostics.error(declaration.span,
                      "given declaration requires an explicit type and initializer");
    ok = false;
  }
  if (!declaration.typeParameters.empty() &&
      declaration.kind != AstDeclarationKind::Class &&
      declaration.kind != AstDeclarationKind::Trait &&
      declaration.kind != AstDeclarationKind::Def) {
    diagnostics.error(
        declaration.span,
        "type parameters are only supported on classes, traits, and methods");
    ok = false;
  }
  std::unordered_set<std::string> typeParameterNames;
  for (const AstTypeParameter& parameter : declaration.typeParameters) {
    if (parameter.name.empty()) {
      diagnostics.error(parameter.span, "type parameter has no name");
      ok = false;
    } else if (!typeParameterNames.insert(parameter.name).second) {
      diagnostics.error(parameter.span, "duplicate type parameter: " + parameter.name);
      ok = false;
    }
    if (declaration.kind == AstDeclarationKind::Def &&
        parameter.variance != TypeVariance::Invariant) {
      diagnostics.error(parameter.span,
                        "method type parameters cannot declare variance");
      ok = false;
    }
  }
  if (!declaration.contextualParameters.empty() &&
      declaration.contextualParameters.size() != declaration.parameters.size()) {
    diagnostics.error(declaration.span,
                      "contextual parameter metadata is inconsistent");
    ok = false;
  }
  bool sawContextualParameter = false;
  for (std::size_t i = 0; i < declaration.contextualParameters.size(); ++i) {
    if (!declaration.contextualParameters[i]) {
      if (sawContextualParameter) {
        diagnostics.error(declaration.span,
                          "ordinary parameters cannot follow using parameters");
        ok = false;
      }
      continue;
    }
    sawContextualParameter = true;
    if (declaration.kind != AstDeclarationKind::Def) {
      diagnostics.error(declaration.span,
                        "using parameters are currently supported only on methods");
      ok = false;
    }
    if (declaration.parameters[i].find(':') == std::string::npos) {
      diagnostics.error(declaration.span, "using parameter requires an explicit type");
      ok = false;
    }
  }
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
    if (expression.children.size() != 1 || expression.typeArguments.empty() ||
        expression.declaredType.empty()) {
      diagnostics.error(expression.span,
                        "type application must have a target and type arguments");
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
    if (expression.isGiven &&
        (expression.mutableLocal || expression.declaredType.empty() ||
         expression.children.size() != 1)) {
      diagnostics.error(expression.span,
                        "local given requires an explicit type and initializer");
      ok = false;
    }
    if (expression.isAnonymousGiven && !expression.isGiven) {
      diagnostics.error(expression.span,
                        "anonymous-given metadata requires a local given");
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
