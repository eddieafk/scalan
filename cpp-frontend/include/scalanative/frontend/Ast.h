#pragma once

#include "scalanative/support/SourceSpan.h"

#include <cstddef>
#include <string>
#include <vector>

namespace scalanative::frontend {

enum class AstDeclarationKind {
  Package,
  Import,
  Object,
  Class,
  Trait,
  Type,
  Def,
  Val,
  Var
};

enum class AstClassBodyItemKind { Declaration, Expression };

enum class TypeVariance { Invariant, Covariant, Contravariant };

enum class AstExpressionKind {
  Empty,
  Identifier,
  ModuleReference,
  IntegerLiteral,
  FloatingLiteral,
  StringLiteral,
  CharLiteral,
  SymbolLiteral,
  BooleanLiteral,
  NullLiteral,
  This,
  Super,
  Block,
  LocalDeclaration,
  Call,
  Select,
  TypeApply,
  Unary,
  Binary,
  Assign,
  Return,
  Throw,
  Try,
  Catch,
  Finally,
  If,
  While,
  New
};

struct AstExpression {
  AstExpressionKind kind = AstExpressionKind::Empty;
  std::string text;
  std::string declaredType;
  std::vector<std::string> typeArguments;
  support::SourceSpan span;
  std::vector<AstExpression> children;
  bool mutableLocal = false;
};

struct AstClassBodyItem {
  AstClassBodyItemKind kind = AstClassBodyItemKind::Declaration;
  std::size_t index = 0;
};

struct AstImportSelector {
  std::string name;
  std::string alias;
  support::SourceSpan span;
};

struct AstTypeParameter {
  std::string name;
  std::string lowerBound;
  std::string upperBound;
  support::SourceSpan span;
  TypeVariance variance = TypeVariance::Invariant;
};

struct AstDeclaration {
  AstDeclarationKind kind = AstDeclarationKind::Def;
  std::string name;
  support::SourceSpan span;
  std::vector<AstTypeParameter> typeParameters;
  std::vector<std::string> parameters;
  std::vector<bool> contextualParameters;
  std::string declaredType;
  std::string lowerBound;
  std::string upperBound;
  std::vector<std::string> parentTypes;
  std::vector<AstExpression> parentArguments;
  std::string importPath;
  std::vector<AstImportSelector> importSelectors;
  bool isOverride = false;
  bool isGiven = false;
  bool hasInitializer = false;
  AstExpression initializer;
  std::vector<AstDeclaration> members;
  std::vector<AstExpression> constructorBody;
  std::vector<AstClassBodyItem> classBodyItems;
};

struct AstModule {
  std::string packageName;
  std::vector<AstDeclaration> declarations;
};

[[nodiscard]] const char* declarationKindName(AstDeclarationKind kind);
[[nodiscard]] const char* expressionKindName(AstExpressionKind kind);
[[nodiscard]] std::string debugString(const AstModule& module);

} // namespace scalanative::frontend
