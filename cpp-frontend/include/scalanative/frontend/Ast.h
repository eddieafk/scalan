#pragma once

#include "scalanative/support/SourceSpan.h"

#include <cstddef>
#include <memory>
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

struct AstContextBound {
  std::string type;
  std::string witnessName;
  support::SourceSpan span;
};

struct AstTypeParameter {
  std::string name;
  std::string lowerBound;
  std::string upperBound;
  std::vector<AstContextBound> contextBounds;
  support::SourceSpan span;
  TypeVariance variance = TypeVariance::Invariant;
};

struct AstLocalMethod {
  std::vector<AstTypeParameter> typeParameters;
  std::vector<std::string> parameters;
  std::vector<bool> contextualParameters;
};

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
  SummonFrom,
  SummonFromCase,
  If,
  While,
  New
};

struct AstExpression {
  AstExpressionKind kind = AstExpressionKind::Empty;
  std::string text;
  std::string declaredType;
  std::vector<std::string> typeArguments;
  std::shared_ptr<AstLocalMethod> localMethod;
  support::SourceSpan span;
  std::vector<AstExpression> children;
  bool mutableLocal = false;
  bool isGiven = false;
  bool isLegacyImplicit = false;
  bool isAnonymousGiven = false;
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
  std::vector<std::string> derivedTypes;
  std::vector<AstExpression> parentArguments;
  std::string importPath;
  std::vector<AstImportSelector> importSelectors;
  std::vector<std::string> importGivenTypes;
  bool importsGivens = false;
  bool importsWildcard = false;
  bool isOverride = false;
  bool isSealed = false;
  bool isTransparent = false;
  bool isGiven = false;
  bool isLegacyImplicit = false;
  bool isAnonymousGiven = false;
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
