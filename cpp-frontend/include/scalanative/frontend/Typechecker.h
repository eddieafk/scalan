#pragma once

#include "scalanative/frontend/Ast.h"
#include "scalanative/support/Diagnostics.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scalanative::frontend {

enum class SimpleTypeKind {
  Unknown,
  Nothing,
  Unit,
  Byte,
  Short,
  Int,
  Long,
  Float,
  Double,
  Boolean,
  String,
  Char,
  Symbol,
  Null,
  Object
};

struct TypeInfo {
  TypeInfo() = default;
  TypeInfo(SimpleTypeKind typeKind, std::string typeName)
      : kind(typeKind), name(std::move(typeName)) {}

  SimpleTypeKind kind = SimpleTypeKind::Unknown;
  std::string name = "Unknown";
  std::string runtimeName;
  std::string dependentOwnerName;
  std::string dependentMemberName;
  std::string dependentPathName;
  std::string resolvedAliasName;
  std::string typeConstructorName;
  std::vector<TypeInfo> typeArguments;
  std::string typeParameterSymbolName;
  bool abstractTypeMember = false;
  bool pathDependent = false;
  bool typeProjection = false;
  bool typeParameter = false;
};

struct TypeParameterInfo {
  std::string name;
  std::string symbolName;
  TypeInfo lowerBound;
  TypeInfo upperBound;
  TypeVariance variance = TypeVariance::Invariant;
};

struct TypedDeclaration {
  AstDeclarationKind kind = AstDeclarationKind::Def;
  std::string name;
  std::string symbolName;
  support::SourceSpan span;
  std::vector<TypeParameterInfo> typeParameters;
  std::vector<std::string> parameters;
  std::vector<TypeInfo> parameterTypes;
  std::vector<bool> contextualParameters;
  std::vector<std::string> accessorParameters;
  std::string declaredType;
  std::string lowerBound;
  std::string upperBound;
  std::vector<std::string> parentTypes;
  std::vector<TypeInfo> parentTypeInfos;
  std::vector<AstExpression> parentArguments;
  std::string importPath;
  std::vector<AstImportSelector> importSelectors;
  TypeInfo inferredType;
  bool isOverride = false;
  bool isGiven = false;
  bool isAnonymousGiven = false;
  bool hasInitializer = false;
  bool needsAccessor = false;
  AstExpression initializer;
  std::vector<TypedDeclaration> members;
  std::vector<AstExpression> constructorBody;
  std::vector<AstClassBodyItem> classBodyItems;
};

struct TypedExpressionInfo {
  support::SourceSpan span;
  TypeInfo type;
};

struct TypedContextArgument {
  std::string name;
  std::string symbolName;
  TypeInfo type;
  bool requiresAccessor = false;
  bool isCall = false;
  std::vector<TypedContextArgument> arguments;
};

struct TypedContextApplication {
  support::SourceSpan span;
  std::vector<TypedContextArgument> arguments;
};

struct TypedModule {
  std::string packageName;
  std::vector<TypedDeclaration> declarations;
  std::vector<TypedExpressionInfo> expressionTypes;
  std::vector<TypedContextApplication> contextApplications;
};

struct SymbolInfo {
  AstDeclarationKind kind = AstDeclarationKind::Val;
  std::string name;
  std::string symbolName;
  std::string parentSymbolName;
  std::vector<std::string> parentSymbolNames;
  std::vector<TypeInfo> parentTypes;
  TypeInfo type;
  TypeInfo lowerBound;
  TypeInfo upperBound;
  std::vector<TypeParameterInfo> typeParameters;
  std::vector<std::string> parameters;
  std::vector<TypeInfo> parameterTypes;
  std::vector<bool> contextualParameters;
  bool hasImplementation = true;
  bool isGiven = false;
  bool isAnonymousGiven = false;
  bool isContextParameter = false;
  bool isModuleMember = false;
  std::size_t contextualNestingDepth = 0;
};

class Typechecker {
public:
  explicit Typechecker(support::DiagnosticEngine& diagnostics);

  [[nodiscard]] TypedModule typecheck(const AstModule& module);

private:
  using Scope = std::unordered_map<std::string, SymbolInfo>;

  void addRuntimeBuiltins(Scope& scope);
  [[nodiscard]] TypedDeclaration typecheckDeclaration(const AstDeclaration& declaration,
                                                      const std::string& owner,
                                                      Scope& scope);
  void collectDeclaration(const AstDeclaration& declaration, const std::string& owner,
                          Scope& scope);
  [[nodiscard]] std::string declarationSymbolName(const AstDeclaration& declaration,
                                                  const std::string& owner) const;
  [[nodiscard]] std::string importSymbolName(const AstDeclaration& declaration,
                                             const Scope& scope) const;
  void applyImport(const AstDeclaration& declaration, Scope& scope);
  void mergeScope(Scope& destination, const Scope& source) const;
  [[nodiscard]] TypeInfo inferExpressionType(const AstExpression& expression,
                                             Scope& scope,
                                             const TypeInfo* expectedType = nullptr);
  [[nodiscard]] TypeInfo inferExpressionTypeImpl(const AstExpression& expression,
                                                 Scope& scope,
                                                 const TypeInfo* expectedType);
  [[nodiscard]] TypeInfo inferNewType(const AstExpression& expression, Scope& scope);
  [[nodiscard]] TypeInfo inferSelectType(const AstExpression& expression, Scope& scope);
  [[nodiscard]] std::string inferArrayElementTypeName(const AstExpression& expression,
                                                      Scope& scope);
  [[nodiscard]] TypeInfo inferAssignType(const AstExpression& expression, Scope& scope);
  [[nodiscard]] bool
  analyzeZoneExpression(const AstExpression& expression,
                        std::unordered_map<std::string, bool>& arenaReferences,
                        std::unordered_set<std::string>& zoneLocals);
  [[nodiscard]] bool expressionHasReferenceType(const AstExpression& expression) const;
  void recordDirectZoneReceiverEscape(const AstDeclaration& declaration,
                                      const TypedDeclaration& typed);
  void propagateZoneReceiverEffects();
  void collectImplicitReceiverMethodNames(
      const AstExpression& expression, std::unordered_set<std::string>& localNames,
      std::unordered_set<std::string>& methodNames) const;
  [[nodiscard]] bool expressionDirectlyEscapesReceiver(
      const AstExpression& expression,
      std::unordered_map<std::string, bool>& receiverAliases,
      std::unordered_set<std::string>& localNames,
      std::vector<AstExpression>* receiverMethodCallSites = nullptr) const;
  [[nodiscard]] bool selectedMethodMayEscapeZone(const AstExpression& expression) const;
  [[nodiscard]] const SymbolInfo* selectedMember(const AstExpression& expression,
                                                 Scope& scope);
  [[nodiscard]] const SymbolInfo*
  knownMemberForReceiverType(const TypeInfo& receiver,
                             const std::string& memberName) const;
  [[nodiscard]] const SymbolInfo* typeSymbolForDeclaredName(const std::string& name,
                                                            const Scope* scope) const;
  void validateInheritance(const AstDeclaration& declaration, TypedDeclaration& typed,
                           const Scope& scope) const;
  void validateParentConstructorArguments(const AstDeclaration& declaration,
                                          const TypedDeclaration& typed, Scope& scope);
  [[nodiscard]] std::vector<const SymbolInfo*>
  inheritedMembers(const std::vector<std::string>& parentSymbolNames,
                   const std::string& memberName) const;
  [[nodiscard]] std::vector<SymbolInfo>
  specializedInheritedMembers(const std::vector<std::string>& parentSymbolNames,
                              const std::vector<TypeInfo>& parentTypes,
                              const std::string& memberName, const Scope& scope) const;
  void validateInheritedMemberCompatibility(
      const AstDeclaration& declaration,
      const std::vector<std::string>& parentSymbolNames,
      const std::vector<TypeInfo>& parentTypes, const Scope& effectiveScope) const;
  void validateOverride(const TypedDeclaration& overriding,
                        const SymbolInfo& inherited) const;
  void mergeInheritedMembers(Scope& destination,
                             const std::vector<std::string>& parentSymbolNames,
                             const std::vector<TypeInfo>& parentTypes) const;
  [[nodiscard]] TypeInfo substituteTypeMembers(const TypeInfo& type,
                                               const Scope& scope) const;
  [[nodiscard]] SymbolInfo
  specializeInheritedMember(const SymbolInfo& member, const Scope& scope,
                            const TypeInfo* appliedParent = nullptr) const;
  [[nodiscard]] TypeInfo specializeTypeForReceiver(const TypeInfo& type,
                                                   const TypeInfo& receiver) const;
  [[nodiscard]] std::unordered_map<std::string, TypeInfo>
  effectiveParentTypes(const std::vector<TypeInfo>& directParents) const;
  void validateVariance(const AstDeclaration& declaration,
                        const TypedDeclaration& typed) const;
  [[nodiscard]] std::vector<TypeParameterInfo>
  resolvedTypeParameters(const std::vector<AstTypeParameter>& parameters,
                         const std::string& owner, Scope& scope) const;
  [[nodiscard]] TypeInfo substituteTypeParameters(
      const TypeInfo& type,
      const std::unordered_map<std::string, TypeInfo>& substitutions) const;
  [[nodiscard]] SymbolInfo specializeMemberForReceiver(const SymbolInfo& member,
                                                       const TypeInfo& receiver) const;
  [[nodiscard]] SymbolInfo
  specializeTypeApplication(const SymbolInfo& symbol,
                            const std::vector<std::string>& typeArguments,
                            const Scope& scope, const support::SourceSpan& span,
                            bool reportDiagnostics = true) const;
  [[nodiscard]] SymbolInfo specializeResolvedTypeApplication(
      const SymbolInfo& symbol, const std::vector<TypeInfo>& typeArguments,
      const support::SourceSpan& span, bool reportDiagnostics = true) const;
  [[nodiscard]] SymbolInfo inferTypeApplication(
      const SymbolInfo& symbol, const std::vector<TypeInfo>& argumentTypes,
      const support::SourceSpan& span, const TypeInfo* expectedResultType = nullptr,
      bool reportDiagnostics = true) const;
  [[nodiscard]] bool isAbstractTypeMember(const TypeInfo& type) const;
  [[nodiscard]] bool runtimeSignatureUsesAbstractType(const SymbolInfo& member) const;
  [[nodiscard]] TypeInfo
  typeFromDeclaredName(const std::string& name, const Scope* scope = nullptr,
                       const support::SourceSpan* span = nullptr) const;
  [[nodiscard]] bool isSupportedArrayElementType(const TypeInfo& candidate,
                                                 const Scope& scope,
                                                 const support::SourceSpan& span) const;
  [[nodiscard]] bool arrayElementConforms(const TypeInfo& expected,
                                          const TypeInfo& actual) const;
  [[nodiscard]] TypeInfo preliminaryDeclarationType(const AstDeclaration& declaration,
                                                    const Scope* scope = nullptr) const;
  [[nodiscard]] TypeInfo commonType(const TypeInfo& lhs, const TypeInfo& rhs) const;
  [[nodiscard]] bool isAssignable(const TypeInfo& expected,
                                  const TypeInfo& actual) const;
  [[nodiscard]] std::vector<TypedContextArgument>
  resolveContextArguments(const SymbolInfo& callee, std::size_t firstContextParameter,
                          Scope& scope, const support::SourceSpan& span,
                          std::unordered_set<std::string>* resolving = nullptr,
                          bool reportDiagnostics = true) const;
  void recordContextApplication(const support::SourceSpan& span,
                                std::vector<TypedContextArgument> arguments);
  [[nodiscard]] bool isSubtypeOf(const std::string& actual,
                                 const std::string& expected) const;
  void addParametersToScope(const AstDeclaration& declaration, Scope& scope) const;
  [[nodiscard]] std::vector<std::string>
  resolvedParameters(const std::vector<std::string>& parameters, const Scope& scope,
                     std::vector<TypeInfo>* parameterTypes = nullptr,
                     const support::SourceSpan* span = nullptr) const;
  [[nodiscard]] static std::string parameterName(const std::string& parameter);
  [[nodiscard]] static AstDeclarationKind
  parameterDeclarationKind(const std::string& parameter);
  [[nodiscard]] TypeInfo parameterType(const std::string& parameter,
                                       const Scope* scope = nullptr,
                                       const support::SourceSpan* span = nullptr) const;
  [[nodiscard]] static std::string qualify(const std::string& owner,
                                           const std::string& name);

  support::DiagnosticEngine& diagnostics_;
  std::unordered_map<std::string, Scope> declaredMemberScopes_;
  std::unordered_map<std::string, Scope> memberScopes_;
  std::unordered_map<std::string, SymbolInfo> globalSymbols_;
  std::unordered_set<std::string> companionTypeNames_;
  std::vector<TypedExpressionInfo> expressionTypes_;
  std::vector<TypedContextApplication> contextApplications_;
  std::unordered_set<std::string> directZoneReceiverEscapes_;
  std::unordered_map<std::string, std::vector<AstExpression>> receiverMethodCallSites_;
  std::unordered_map<std::string, std::unordered_set<std::string>>
      implicitReceiverMethodNames_;
  std::vector<AstExpression> zoneBodiesToAnalyze_;
  std::size_t zoneInferenceDepth_ = 0;
};

[[nodiscard]] const char* simpleTypeKindName(SimpleTypeKind kind);

} // namespace scalanative::frontend
