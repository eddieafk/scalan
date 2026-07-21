#include "scalanative/frontend/Ast.h"

#include <sstream>
#include <string>

namespace scalanative::frontend {

namespace {

void writeExpression(std::ostringstream& out, const AstExpression& expression,
                     std::size_t depth) {
  std::string indent(depth * 2, ' ');
  out << indent << expressionKindName(expression.kind);
  if (!expression.text.empty()) {
    out << ' ' << expression.text;
  }
  if ((expression.kind == AstExpressionKind::LocalDeclaration ||
       expression.kind == AstExpressionKind::TypeApply) &&
      !expression.declaredType.empty()) {
    out << ": " << expression.declaredType;
  }
  if (expression.kind == AstExpressionKind::TypeApply &&
      expression.typeArguments.size() > 1) {
    for (std::size_t i = 1; i < expression.typeArguments.size(); ++i) {
      out << ", " << expression.typeArguments[i];
    }
  }
  if (expression.kind == AstExpressionKind::LocalDeclaration &&
      expression.mutableLocal) {
    out << " mutable";
  }
  out << '\n';
  for (const AstExpression& child : expression.children) {
    writeExpression(out, child, depth + 1);
  }
}

void writeDeclaration(std::ostringstream& out, const AstDeclaration& declaration,
                      std::size_t depth) {
  std::string indent(depth * 2, ' ');
  out << indent << declarationKindName(declaration.kind) << ' ' << declaration.name;
  if (declaration.isOverride) {
    out << " override";
  }
  if (!declaration.typeParameters.empty()) {
    out << '[';
    for (std::size_t i = 0; i < declaration.typeParameters.size(); ++i) {
      if (i != 0) {
        out << ", ";
      }
      const AstTypeParameter& parameter = declaration.typeParameters[i];
      out << parameter.name;
      if (!parameter.lowerBound.empty()) {
        out << " >: " << parameter.lowerBound;
      }
      if (!parameter.upperBound.empty()) {
        out << " <: " << parameter.upperBound;
      }
    }
    out << ']';
  }
  if (!declaration.parameters.empty()) {
    out << '(';
    for (std::size_t i = 0; i < declaration.parameters.size(); ++i) {
      if (i != 0) {
        out << ", ";
      }
      out << declaration.parameters[i];
    }
    out << ')';
  }
  if (declaration.kind == AstDeclarationKind::Type && declaration.hasInitializer) {
    out << " = " << declaration.declaredType;
  } else if (declaration.kind == AstDeclarationKind::Type) {
    if (!declaration.lowerBound.empty()) {
      out << " >: " << declaration.lowerBound;
    }
    if (!declaration.upperBound.empty()) {
      out << " <: " << declaration.upperBound;
    }
  } else if (!declaration.declaredType.empty()) {
    out << ": " << declaration.declaredType;
  }
  for (std::size_t i = 1; i < declaration.parentTypes.size(); ++i) {
    out << " with " << declaration.parentTypes[i];
  }
  if (!declaration.importPath.empty()) {
    out << " <- " << declaration.importPath;
  }
  out << '\n';
  if (declaration.hasInitializer) {
    writeExpression(out, declaration.initializer, depth + 1);
  }
  if (!declaration.classBodyItems.empty()) {
    for (const AstClassBodyItem& item : declaration.classBodyItems) {
      if (item.kind == AstClassBodyItemKind::Declaration &&
          item.index < declaration.members.size()) {
        writeDeclaration(out, declaration.members[item.index], depth + 1);
      } else if (item.kind == AstClassBodyItemKind::Expression &&
                 item.index < declaration.constructorBody.size()) {
        writeExpression(out, declaration.constructorBody[item.index], depth + 1);
      }
    }
  } else {
    for (const AstExpression& expression : declaration.constructorBody) {
      writeExpression(out, expression, depth + 1);
    }
    for (const AstDeclaration& member : declaration.members) {
      writeDeclaration(out, member, depth + 1);
    }
  }
}

} // namespace

const char* declarationKindName(AstDeclarationKind kind) {
  switch (kind) {
  case AstDeclarationKind::Package:
    return "package";
  case AstDeclarationKind::Import:
    return "import";
  case AstDeclarationKind::Object:
    return "object";
  case AstDeclarationKind::Class:
    return "class";
  case AstDeclarationKind::Trait:
    return "trait";
  case AstDeclarationKind::Type:
    return "type";
  case AstDeclarationKind::Def:
    return "def";
  case AstDeclarationKind::Val:
    return "val";
  case AstDeclarationKind::Var:
    return "var";
  }
  return "unknown";
}

const char* expressionKindName(AstExpressionKind kind) {
  switch (kind) {
  case AstExpressionKind::Empty:
    return "empty";
  case AstExpressionKind::Identifier:
    return "identifier";
  case AstExpressionKind::ModuleReference:
    return "module-reference";
  case AstExpressionKind::IntegerLiteral:
    return "integer";
  case AstExpressionKind::FloatingLiteral:
    return "floating";
  case AstExpressionKind::StringLiteral:
    return "string";
  case AstExpressionKind::CharLiteral:
    return "char";
  case AstExpressionKind::SymbolLiteral:
    return "symbol";
  case AstExpressionKind::BooleanLiteral:
    return "boolean";
  case AstExpressionKind::NullLiteral:
    return "null";
  case AstExpressionKind::This:
    return "this";
  case AstExpressionKind::Super:
    return "super";
  case AstExpressionKind::Block:
    return "block";
  case AstExpressionKind::LocalDeclaration:
    return "local-declaration";
  case AstExpressionKind::Call:
    return "call";
  case AstExpressionKind::Select:
    return "select";
  case AstExpressionKind::TypeApply:
    return "type-apply";
  case AstExpressionKind::Unary:
    return "unary";
  case AstExpressionKind::Binary:
    return "binary";
  case AstExpressionKind::Assign:
    return "assign";
  case AstExpressionKind::Return:
    return "return";
  case AstExpressionKind::Throw:
    return "throw";
  case AstExpressionKind::Try:
    return "try";
  case AstExpressionKind::Catch:
    return "catch";
  case AstExpressionKind::Finally:
    return "finally";
  case AstExpressionKind::If:
    return "if";
  case AstExpressionKind::While:
    return "while";
  case AstExpressionKind::New:
    return "new";
  }
  return "unknown";
}

std::string debugString(const AstModule& module) {
  std::ostringstream out;
  out << "package " << (module.packageName.empty() ? "<root>" : module.packageName)
      << '\n';
  for (const AstDeclaration& declaration : module.declarations) {
    writeDeclaration(out, declaration, 0);
  }
  return out.str();
}

} // namespace scalanative::frontend
