#pragma once

#include "scalanative/frontend/Ast.h"
#include "scalanative/frontend/Lexer.h"
#include "scalanative/support/Diagnostics.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace scalanative::frontend {

class Parser {
public:
  Parser(std::vector<Token> tokens, support::DiagnosticEngine& diagnostics);

  [[nodiscard]] AstModule parse();

private:
  [[nodiscard]] bool isAtEnd() const;
  [[nodiscard]] bool check(TokenKind kind) const;
  [[nodiscard]] bool match(TokenKind kind);
  [[nodiscard]] bool isDeclarationStart() const;
  [[nodiscard]] bool isExpressionBoundary() const;
  [[nodiscard]] const Token& peek() const;
  [[nodiscard]] const Token& previous() const;
  const Token& advance();

  void consumeSeparators();
  void parsePackage(AstModule& module);
  [[nodiscard]] AstDeclaration parseDeclaration();
  [[nodiscard]] AstDeclaration parseObjectLike(AstDeclarationKind kind,
                                               const Token& keyword);
  [[nodiscard]] AstDeclaration parseImport(const Token& keyword);
  [[nodiscard]] AstDeclaration parseOverride(const Token& keyword);
  [[nodiscard]] AstDeclaration parseType(const Token& keyword);
  [[nodiscard]] AstDeclaration
  parseDef(const Token& keyword, bool allowMultipleOrdinaryClauses = false);
  [[nodiscard]] AstDeclaration parseGiven(const Token& keyword);
  [[nodiscard]] AstDeclaration parseImplicit(const Token& keyword);
  [[nodiscard]] AstDeclaration parseValOrVar(AstDeclarationKind kind,
                                             const Token& keyword);
  [[nodiscard]] std::vector<AstTypeParameter> parseTypeParameterList();
  [[nodiscard]] std::vector<std::string>
  parseParameterList(bool allowModifiers = false,
                     bool* contextualClause = nullptr,
                     std::vector<bool>* inlineParameters = nullptr);
  [[nodiscard]] std::vector<AstExpression> parseArgumentList();
  [[nodiscard]] std::string parseTypeName(bool stopAtUpperBound = false,
                                          bool stopAtRightBracket = false,
                                          bool stopAtMatchAlternative = false,
                                          bool stopAtContextBound = false,
                                          bool stopAtNamedContextBound = false,
                                          bool allowParenthesized = false);
  [[nodiscard]] std::vector<AstDeclaration>
  parseMemberBlock(std::vector<AstExpression>* constructorBody = nullptr,
                   std::vector<AstClassBodyItem>* classBodyItems = nullptr);
  [[nodiscard]] AstExpression parseExpression();
  [[nodiscard]] AstExpression parseAssignmentExpression();
  [[nodiscard]] AstExpression parseBinaryExpression(int minPrecedence = 0);
  [[nodiscard]] AstExpression parseUnaryExpression();
  [[nodiscard]] AstExpression parsePostfixExpression();
  [[nodiscard]] AstExpression parsePrimaryExpression();
  [[nodiscard]] AstExpression parsePolymorphicFunctionExpression();
  [[nodiscard]] AstExpression parseBlockExpression();
  [[nodiscard]] AstExpression parseInterpolatedStringExpression();
  [[nodiscard]] AstExpression parseReturnExpression(const Token& keyword);
  [[nodiscard]] AstExpression parseThrowExpression(const Token& keyword);
  [[nodiscard]] AstExpression parseTryExpression(const Token& keyword);
  [[nodiscard]] AstExpression parseIfExpression(const Token& keyword);
  [[nodiscard]] AstExpression parseWhileExpression(const Token& keyword);
  [[nodiscard]] AstExpression parseStableModuleReference(const Token& first);
  [[nodiscard]] AstExpression parseSummonFromExpression(const Token& identifier);
  [[nodiscard]] AstExpression parseMatchExpression(AstExpression selector,
                                                   const Token& keyword,
                                                   bool isInline = false,
                                                   support::SourceSpan inlineSpan = {});
  [[nodiscard]] int precedence(const Token& token) const;
  bool consume(TokenKind kind, const std::string& message);
  [[nodiscard]] std::string parseQualifiedName();
  void synchronize();

  std::vector<Token> tokens_;
  support::DiagnosticEngine& diagnostics_;
  std::size_t current_ = 0;
  std::size_t nextSyntheticLocal_ = 0;
  std::unordered_set<std::string> summonFromNames_{"summonFrom"};
};

} // namespace scalanative::frontend
