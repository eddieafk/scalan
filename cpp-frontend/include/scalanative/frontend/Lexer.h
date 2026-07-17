#pragma once

#include "scalanative/support/Diagnostics.h"
#include "scalanative/support/SourceManager.h"
#include "scalanative/support/SourceSpan.h"

#include <string>
#include <string_view>
#include <vector>

namespace scalanative::frontend {

enum class TokenKind {
  EndOfFile,
  Identifier,
  IntegerLiteral,
  FloatingLiteral,
  StringLiteral,
  CharLiteral,
  SymbolLiteral,
  InterpolatedStringStart,
  InterpolatedStringPart,
  InterpolatedStringEnd,
  InterpolationIdentifier,
  InterpolationStart,
  InterpolationEnd,
  KeywordClass,
  KeywordCase,
  KeywordCatch,
  KeywordDef,
  KeywordElse,
  KeywordExtends,
  KeywordFalse,
  KeywordFinally,
  KeywordFor,
  KeywordIf,
  KeywordImport,
  KeywordMatch,
  KeywordNew,
  KeywordNull,
  KeywordObject,
  KeywordOverride,
  KeywordPackage,
  KeywordReturn,
  KeywordThrow,
  KeywordTry,
  KeywordThis,
  KeywordTrait,
  KeywordTrue,
  KeywordType,
  KeywordVal,
  KeywordVar,
  KeywordWith,
  KeywordWhile,
  LeftBrace,
  RightBrace,
  LeftParen,
  RightParen,
  LeftBracket,
  RightBracket,
  Semicolon,
  Colon,
  Comma,
  Dot,
  Equals,
  Arrow,
  At,
  Operator,
  Unknown
};

enum class TriviaKind { Whitespace, Newline, LineComment, BlockComment };

struct Trivia {
  TriviaKind kind = TriviaKind::Whitespace;
  support::SourceSpan span;
  std::string text;
  bool containsNewline = false;
};

struct Token {
  TokenKind kind = TokenKind::Unknown;
  support::SourceSpan span;
  std::string text;
  std::vector<Trivia> leadingTrivia;
  bool isVirtual = false;
};

class Lexer {
public:
  Lexer(const support::SourceManager& sources, support::DiagnosticEngine& diagnostics);

  [[nodiscard]] std::vector<Token> lex(support::SourceId source);

private:
  [[nodiscard]] Token makeToken(TokenKind kind, std::size_t start, std::size_t length,
                                std::string text) const;
  [[nodiscard]] Trivia makeTrivia(TriviaKind kind, std::size_t start,
                                  std::size_t length) const;
  [[nodiscard]] TokenKind keywordKind(std::string_view text) const;
  [[nodiscard]] bool isInterpolationPrefix(std::size_t index) const;
  [[nodiscard]] Token lexIdentifier(std::size_t& index) const;
  [[nodiscard]] Token lexBacktickIdentifier(std::size_t& index) const;
  [[nodiscard]] Token lexNumber(std::size_t& index) const;
  [[nodiscard]] Token lexString(std::size_t& index) const;
  [[nodiscard]] std::vector<Token> lexInterpolatedString(std::size_t& index) const;
  [[nodiscard]] std::vector<Token> lexInterpolationExpression(std::size_t& index) const;
  [[nodiscard]] Token lexCharOrSymbol(std::size_t& index) const;
  [[nodiscard]] Token lexOperator(std::size_t& index) const;
  void pushToken(std::vector<Token>& tokens, Token token, bool hadLeadingNewline) const;

  [[nodiscard]] static bool isIdentifierStart(char ch);
  [[nodiscard]] static bool isIdentifierPart(char ch);
  [[nodiscard]] static bool isOperatorChar(char ch);
  [[nodiscard]] static bool canEndStatement(TokenKind kind);
  [[nodiscard]] static bool canStartStatement(TokenKind kind);

  const support::SourceManager& sources_;
  support::DiagnosticEngine& diagnostics_;
  support::SourceId currentSource_;
  std::string_view text_;
};

[[nodiscard]] const char* tokenKindName(TokenKind kind);

} // namespace scalanative::frontend
