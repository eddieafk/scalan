#include "scalanative/frontend/Lexer.h"

#include "scalanative/support/StdNames.h"

#include <cctype>
#include <string_view>
#include <utility>

namespace scalanative::frontend {

namespace {

bool containsNewline(std::string_view text) {
  return text.find('\n') != std::string_view::npos;
}

} // namespace

Lexer::Lexer(const support::SourceManager& sources,
             support::DiagnosticEngine& diagnostics)
    : sources_(sources), diagnostics_(diagnostics) {}

std::vector<Token> Lexer::lex(support::SourceId source) {
  currentSource_ = source;
  text_ = sources_.text(source);

  std::vector<Token> tokens;
  std::vector<Trivia> pendingTrivia;
  std::size_t index = 0;
  bool pendingNewline = false;

  auto emitToken = [&](Token token) {
    token.leadingTrivia = std::move(pendingTrivia);
    pendingTrivia.clear();
    pushToken(tokens, std::move(token), pendingNewline);
    pendingNewline = false;
  };

  auto emitInterpolation = [&](std::vector<Token> interpolationTokens) {
    if (interpolationTokens.empty()) {
      return;
    }
    interpolationTokens.front().leadingTrivia = std::move(pendingTrivia);
    pendingTrivia.clear();
    pushToken(tokens, std::move(interpolationTokens.front()), pendingNewline);
    for (std::size_t i = 1; i < interpolationTokens.size(); ++i) {
      tokens.push_back(std::move(interpolationTokens[i]));
    }
    pendingNewline = false;
  };

  while (index < text_.size()) {
    const char ch = text_[index];

    if (std::isspace(static_cast<unsigned char>(ch))) {
      const std::size_t start = index;
      bool sawNewline = false;
      while (index < text_.size() &&
             std::isspace(static_cast<unsigned char>(text_[index]))) {
        sawNewline = sawNewline || text_[index] == '\n';
        ++index;
      }
      pendingTrivia.push_back(
          makeTrivia(sawNewline ? TriviaKind::Newline : TriviaKind::Whitespace, start,
                     index - start));
      pendingNewline = pendingNewline || sawNewline;
      continue;
    }

    if (ch == '/' && index + 1 < text_.size() && text_[index + 1] == '/') {
      const std::size_t start = index;
      index += 2;
      while (index < text_.size() && text_[index] != '\n') {
        ++index;
      }
      pendingTrivia.push_back(
          makeTrivia(TriviaKind::LineComment, start, index - start));
      continue;
    }

    if (ch == '/' && index + 1 < text_.size() && text_[index + 1] == '*') {
      const std::size_t start = index;
      index += 2;
      std::size_t depth = 1;
      while (index + 1 < text_.size() && depth > 0) {
        if (text_[index] == '/' && text_[index + 1] == '*') {
          ++depth;
          index += 2;
        } else if (text_[index] == '*' && text_[index + 1] == '/') {
          --depth;
          index += 2;
        } else {
          ++index;
        }
      }
      pendingNewline =
          pendingNewline || containsNewline(text_.substr(start, index - start));
      pendingTrivia.push_back(
          makeTrivia(TriviaKind::BlockComment, start, index - start));
      if (depth != 0) {
        diagnostics_.error({source, start, index - start},
                           "unterminated block comment");
      }
      continue;
    }

    if (isIdentifierStart(ch)) {
      if (isInterpolationPrefix(index)) {
        emitInterpolation(lexInterpolatedString(index));
      } else {
        emitToken(lexIdentifier(index));
      }
      continue;
    }

    if (ch == '`') {
      emitToken(lexBacktickIdentifier(index));
      continue;
    }

    if (std::isdigit(static_cast<unsigned char>(ch))) {
      emitToken(lexNumber(index));
      continue;
    }

    if (ch == '"') {
      emitToken(lexString(index));
      continue;
    }

    if (ch == '\'') {
      emitToken(lexCharOrSymbol(index));
      continue;
    }

    const std::size_t start = index;
    ++index;
    switch (ch) {
    case '{':
      emitToken(makeToken(TokenKind::LeftBrace, start, 1, "{"));
      break;
    case '}':
      emitToken(makeToken(TokenKind::RightBrace, start, 1, "}"));
      break;
    case '(':
      emitToken(makeToken(TokenKind::LeftParen, start, 1, "("));
      break;
    case ')':
      emitToken(makeToken(TokenKind::RightParen, start, 1, ")"));
      break;
    case '[':
      emitToken(makeToken(TokenKind::LeftBracket, start, 1, "["));
      break;
    case ']':
      emitToken(makeToken(TokenKind::RightBracket, start, 1, "]"));
      break;
    case ';':
      emitToken(makeToken(TokenKind::Semicolon, start, 1, ";"));
      break;
    case ':':
      emitToken(makeToken(TokenKind::Colon, start, 1, ":"));
      break;
    case ',':
      emitToken(makeToken(TokenKind::Comma, start, 1, ","));
      break;
    case '.':
      emitToken(makeToken(TokenKind::Dot, start, 1, "."));
      break;
    case '=':
      if (index < text_.size() && text_[index] == '>') {
        ++index;
        emitToken(makeToken(TokenKind::Arrow, start, 2, "=>"));
      } else if (index < text_.size() && text_[index] == '=') {
        ++index;
        emitToken(makeToken(TokenKind::Operator, start, 2, "=="));
      } else {
        emitToken(makeToken(TokenKind::Equals, start, 1, "="));
      }
      break;
    case '@':
      emitToken(makeToken(TokenKind::At, start, 1, "@"));
      break;
    default:
      --index;
      if (isOperatorChar(ch)) {
        emitToken(lexOperator(index));
        break;
      }
      ++index;
      diagnostics_.error({source, start, 1}, "unexpected character");
      emitToken(makeToken(TokenKind::Unknown, start, 1, std::string(1, ch)));
      break;
    }
  }

  emitToken(makeToken(TokenKind::EndOfFile, text_.size(), 0, ""));
  return tokens;
}

Token Lexer::makeToken(TokenKind kind, std::size_t start, std::size_t length,
                       std::string text) const {
  Token token;
  token.kind = kind;
  token.span = {currentSource_, start, length};
  token.text = std::move(text);
  return token;
}

Trivia Lexer::makeTrivia(TriviaKind kind, std::size_t start, std::size_t length) const {
  std::string text(text_.substr(start, length));
  return Trivia{kind, {currentSource_, start, length}, text, containsNewline(text)};
}

bool Lexer::isInterpolationPrefix(std::size_t index) const {
  if (index + 1 < text_.size() && (text_[index] == 's' || text_[index] == 'f') &&
      text_[index + 1] == '"') {
    return true;
  }
  return index + 3 < text_.size() && text_.substr(index, 3) == "raw" &&
         text_[index + 3] == '"';
}

Token Lexer::lexIdentifier(std::size_t& index) const {
  const std::size_t start = index;
  ++index;
  while (index < text_.size() && isIdentifierPart(text_[index])) {
    ++index;
  }

  std::string word(text_.substr(start, index - start));
  return makeToken(keywordKind(word), start, index - start, word);
}

Token Lexer::lexBacktickIdentifier(std::size_t& index) const {
  const std::size_t start = index;
  ++index;
  const std::size_t contentStart = index;
  while (index < text_.size() && text_[index] != '`') {
    if (text_[index] == '\n') {
      diagnostics_.error({currentSource_, start, index - start},
                         "unterminated backtick identifier");
      return makeToken(TokenKind::Identifier, start, index - start,
                       std::string(text_.substr(contentStart, index - contentStart)));
    }
    ++index;
  }

  if (index >= text_.size()) {
    diagnostics_.error({currentSource_, start, index - start},
                       "unterminated backtick identifier");
    return makeToken(TokenKind::Identifier, start, index - start,
                     std::string(text_.substr(contentStart, index - contentStart)));
  }

  std::string text(text_.substr(contentStart, index - contentStart));
  ++index;
  if (text.empty()) {
    diagnostics_.error({currentSource_, start, index - start},
                       "empty backtick identifier");
  }
  return makeToken(TokenKind::Identifier, start, index - start, std::move(text));
}

Token Lexer::lexNumber(std::size_t& index) const {
  const std::size_t start = index;
  TokenKind kind = TokenKind::IntegerLiteral;

  auto consumeDigits = [&](auto predicate) {
    bool consumed = false;
    while (index < text_.size()) {
      const char ch = text_[index];
      if (ch == '_') {
        ++index;
        continue;
      }
      if (!predicate(ch)) {
        break;
      }
      consumed = true;
      ++index;
    }
    return consumed;
  };

  if (text_[index] == '0' && index + 1 < text_.size() &&
      (text_[index + 1] == 'x' || text_[index + 1] == 'X')) {
    index += 2;
    const bool hasDigits = consumeDigits(
        [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)) != 0; });
    if (!hasDigits) {
      diagnostics_.error({currentSource_, start, index - start},
                         "hex literal requires at least one digit");
    }
  } else {
    consumeDigits(
        [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)) != 0; });

    if (index + 1 < text_.size() && text_[index] == '.' &&
        std::isdigit(static_cast<unsigned char>(text_[index + 1]))) {
      kind = TokenKind::FloatingLiteral;
      ++index;
      consumeDigits(
          [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)) != 0; });
    }

    if (index < text_.size() && (text_[index] == 'e' || text_[index] == 'E')) {
      kind = TokenKind::FloatingLiteral;
      const std::size_t exponentStart = index;
      ++index;
      if (index < text_.size() && (text_[index] == '+' || text_[index] == '-')) {
        ++index;
      }
      const bool hasDigits = consumeDigits(
          [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)) != 0; });
      if (!hasDigits) {
        diagnostics_.error({currentSource_, exponentStart, index - exponentStart},
                           "exponent requires at least one digit");
      }
    }
  }

  if (index < text_.size()) {
    const char suffix = text_[index];
    if (suffix == 'f' || suffix == 'F' || suffix == 'd' || suffix == 'D') {
      kind = TokenKind::FloatingLiteral;
      ++index;
    } else if (kind == TokenKind::IntegerLiteral && (suffix == 'l' || suffix == 'L')) {
      ++index;
    }
  }

  if (index < text_.size() && isIdentifierStart(text_[index])) {
    const std::size_t suffixStart = index;
    while (index < text_.size() && isIdentifierPart(text_[index])) {
      ++index;
    }
    diagnostics_.error({currentSource_, suffixStart, index - suffixStart},
                       "invalid numeric literal suffix");
  }

  std::string literal(text_.substr(start, index - start));
  for (std::size_t i = 0; i < literal.size(); ++i) {
    if (literal[i] != '_') {
      continue;
    }
    const bool badBefore =
        i == 0 || !std::isalnum(static_cast<unsigned char>(literal[i - 1]));
    const bool badAfter = i + 1 == literal.size() ||
                          !std::isalnum(static_cast<unsigned char>(literal[i + 1]));
    if (badBefore || badAfter) {
      diagnostics_.error({currentSource_, start + i, 1},
                         "misplaced numeric literal separator");
      break;
    }
  }

  return makeToken(kind, start, index - start, std::move(literal));
}

Token Lexer::lexString(std::size_t& index) const {
  const std::size_t start = index;
  bool tripleQuoted =
      index + 2 < text_.size() && text_[index + 1] == '"' && text_[index + 2] == '"';

  if (tripleQuoted) {
    index += 3;
    while (index + 2 < text_.size()) {
      if (text_[index] == '"' && text_[index + 1] == '"' && text_[index + 2] == '"') {
        index += 3;
        return makeToken(TokenKind::StringLiteral, start, index - start,
                         std::string(text_.substr(start, index - start)));
      }
      ++index;
    }
    index = text_.size();
    diagnostics_.error({currentSource_, start, index - start},
                       "unterminated triple-quoted string literal");
    return makeToken(TokenKind::StringLiteral, start, index - start,
                     std::string(text_.substr(start, index - start)));
  }

  ++index;
  bool terminated = false;
  while (index < text_.size()) {
    if (text_[index] == '\n') {
      break;
    }
    if (text_[index] == '\\' && index + 1 < text_.size()) {
      const char escaped = text_[index + 1];
      switch (escaped) {
      case 'b':
      case 't':
      case 'n':
      case 'f':
      case 'r':
      case '"':
      case '\'':
      case '\\':
        break;
      default:
        diagnostics_.error({currentSource_, index, 2},
                           "unknown string escape sequence");
        break;
      }
      index += 2;
      continue;
    }
    if (text_[index] == '"') {
      ++index;
      terminated = true;
      break;
    }
    ++index;
  }
  if (!terminated) {
    diagnostics_.error({currentSource_, start, index - start},
                       "unterminated string literal");
  }
  return makeToken(TokenKind::StringLiteral, start, index - start,
                   std::string(text_.substr(start, index - start)));
}

std::vector<Token> Lexer::lexInterpolatedString(std::size_t& index) const {
  std::vector<Token> tokens;
  const std::size_t start = index;
  const std::size_t prefixLength =
      text_.substr(index, 3) == "raw" ? std::size_t{3} : std::size_t{1};
  const std::size_t quoteStart = index + prefixLength;
  const bool tripleQuoted = quoteStart + 2 < text_.size() &&
                            text_[quoteStart + 1] == '"' &&
                            text_[quoteStart + 2] == '"';
  const std::size_t quoteLength = tripleQuoted ? 3 : 1;

  index = quoteStart + quoteLength;
  tokens.push_back(makeToken(TokenKind::InterpolatedStringStart, start, index - start,
                             std::string(text_.substr(start, index - start))));

  auto emitPart = [&](std::size_t partStart, std::size_t partEnd) {
    if (partEnd <= partStart) {
      return;
    }
    tokens.push_back(
        makeToken(TokenKind::InterpolatedStringPart, partStart, partEnd - partStart,
                  std::string(text_.substr(partStart, partEnd - partStart))));
  };

  std::size_t partStart = index;
  while (index < text_.size()) {
    if (tripleQuoted) {
      if (index + 2 < text_.size() && text_[index] == '"' && text_[index + 1] == '"' &&
          text_[index + 2] == '"') {
        emitPart(partStart, index);
        const std::size_t endStart = index;
        index += 3;
        tokens.push_back(
            makeToken(TokenKind::InterpolatedStringEnd, endStart, 3, "\"\"\""));
        return tokens;
      }
    } else {
      if (text_[index] == '\n') {
        break;
      }
      if (text_[index] == '"') {
        emitPart(partStart, index);
        tokens.push_back(makeToken(TokenKind::InterpolatedStringEnd, index, 1, "\""));
        ++index;
        return tokens;
      }
      if (text_[index] == '\\' && index + 1 < text_.size()) {
        if (text_[start] != 'r') {
          const char escaped = text_[index + 1];
          switch (escaped) {
          case 'b':
          case 't':
          case 'n':
          case 'f':
          case 'r':
          case '"':
          case '\'':
          case '\\':
          case '$':
            break;
          default:
            diagnostics_.error({currentSource_, index, 2},
                               "unknown interpolated string escape sequence");
            break;
          }
        }
        index += 2;
        continue;
      }
    }

    if (text_[index] == '$' && index + 1 < text_.size()) {
      const std::size_t interpolationStart = index;
      if (isIdentifierStart(text_[index + 1])) {
        emitPart(partStart, interpolationStart);
        index += 2;
        const std::size_t identifierStart = interpolationStart + 1;
        while (index < text_.size() && isIdentifierPart(text_[index])) {
          ++index;
        }
        tokens.push_back(makeToken(
            TokenKind::InterpolationIdentifier, interpolationStart,
            index - interpolationStart,
            std::string(text_.substr(identifierStart, index - identifierStart))));
        partStart = index;
        continue;
      }

      if (text_[index + 1] == '{') {
        emitPart(partStart, interpolationStart);
        tokens.push_back(
            makeToken(TokenKind::InterpolationStart, interpolationStart, 2, "${"));
        index += 2;
        std::vector<Token> expressionTokens = lexInterpolationExpression(index);
        for (Token& token : expressionTokens) {
          tokens.push_back(std::move(token));
        }
        if (index < text_.size() && text_[index] == '}') {
          tokens.push_back(makeToken(TokenKind::InterpolationEnd, index, 1, "}"));
          ++index;
          partStart = index;
          continue;
        }
        diagnostics_.error(
            {currentSource_, interpolationStart, index - interpolationStart},
            "unterminated interpolation block");
        partStart = index;
        continue;
      }
    }

    ++index;
  }

  emitPart(partStart, index);
  diagnostics_.error({currentSource_, start, index - start},
                     "unterminated interpolated string literal");
  return tokens;
}

std::vector<Token> Lexer::lexInterpolationExpression(std::size_t& index) const {
  std::vector<Token> tokens;
  std::vector<Trivia> pendingTrivia;
  std::size_t braceDepth = 1;
  bool pendingNewline = false;

  auto emitToken = [&](Token token) {
    token.leadingTrivia = std::move(pendingTrivia);
    pendingTrivia.clear();
    pushToken(tokens, std::move(token), pendingNewline);
    pendingNewline = false;
  };

  auto emitInterpolation = [&](std::vector<Token> interpolationTokens) {
    if (interpolationTokens.empty()) {
      return;
    }
    interpolationTokens.front().leadingTrivia = std::move(pendingTrivia);
    pendingTrivia.clear();
    pushToken(tokens, std::move(interpolationTokens.front()), pendingNewline);
    for (std::size_t tokenIndex = 1; tokenIndex < interpolationTokens.size();
         ++tokenIndex) {
      tokens.push_back(std::move(interpolationTokens[tokenIndex]));
    }
    pendingNewline = false;
  };

  while (index < text_.size()) {
    const char ch = text_[index];

    if (std::isspace(static_cast<unsigned char>(ch))) {
      const std::size_t start = index;
      bool sawNewline = false;
      while (index < text_.size() &&
             std::isspace(static_cast<unsigned char>(text_[index]))) {
        sawNewline = sawNewline || text_[index] == '\n';
        ++index;
      }
      pendingTrivia.push_back(
          makeTrivia(sawNewline ? TriviaKind::Newline : TriviaKind::Whitespace, start,
                     index - start));
      pendingNewline = pendingNewline || sawNewline;
      continue;
    }

    if (ch == '/' && index + 1 < text_.size() && text_[index + 1] == '/') {
      const std::size_t start = index;
      index += 2;
      while (index < text_.size() && text_[index] != '\n') {
        ++index;
      }
      pendingTrivia.push_back(
          makeTrivia(TriviaKind::LineComment, start, index - start));
      continue;
    }

    if (ch == '/' && index + 1 < text_.size() && text_[index + 1] == '*') {
      const std::size_t start = index;
      index += 2;
      std::size_t depth = 1;
      while (index + 1 < text_.size() && depth > 0) {
        if (text_[index] == '/' && text_[index + 1] == '*') {
          ++depth;
          index += 2;
        } else if (text_[index] == '*' && text_[index + 1] == '/') {
          --depth;
          index += 2;
        } else {
          ++index;
        }
      }
      pendingNewline =
          pendingNewline || containsNewline(text_.substr(start, index - start));
      pendingTrivia.push_back(
          makeTrivia(TriviaKind::BlockComment, start, index - start));
      if (depth != 0) {
        diagnostics_.error({currentSource_, start, index - start},
                           "unterminated block comment");
      }
      continue;
    }

    if (ch == '}' && braceDepth == 1) {
      return tokens;
    }

    if (isIdentifierStart(ch)) {
      if (isInterpolationPrefix(index)) {
        emitInterpolation(lexInterpolatedString(index));
      } else {
        emitToken(lexIdentifier(index));
      }
      continue;
    }

    if (ch == '`') {
      emitToken(lexBacktickIdentifier(index));
      continue;
    }

    if (std::isdigit(static_cast<unsigned char>(ch))) {
      emitToken(lexNumber(index));
      continue;
    }

    if (ch == '"') {
      emitToken(lexString(index));
      continue;
    }

    if (ch == '\'') {
      emitToken(lexCharOrSymbol(index));
      continue;
    }

    const std::size_t start = index;
    ++index;
    switch (ch) {
    case '{':
      ++braceDepth;
      emitToken(makeToken(TokenKind::LeftBrace, start, 1, "{"));
      break;
    case '}':
      --braceDepth;
      emitToken(makeToken(TokenKind::RightBrace, start, 1, "}"));
      break;
    case '(':
      emitToken(makeToken(TokenKind::LeftParen, start, 1, "("));
      break;
    case ')':
      emitToken(makeToken(TokenKind::RightParen, start, 1, ")"));
      break;
    case '[':
      emitToken(makeToken(TokenKind::LeftBracket, start, 1, "["));
      break;
    case ']':
      emitToken(makeToken(TokenKind::RightBracket, start, 1, "]"));
      break;
    case ';':
      emitToken(makeToken(TokenKind::Semicolon, start, 1, ";"));
      break;
    case ':':
      emitToken(makeToken(TokenKind::Colon, start, 1, ":"));
      break;
    case ',':
      emitToken(makeToken(TokenKind::Comma, start, 1, ","));
      break;
    case '.':
      emitToken(makeToken(TokenKind::Dot, start, 1, "."));
      break;
    case '=':
      if (index < text_.size() && text_[index] == '>') {
        ++index;
        emitToken(makeToken(TokenKind::Arrow, start, 2, "=>"));
      } else if (index < text_.size() && text_[index] == '=') {
        ++index;
        emitToken(makeToken(TokenKind::Operator, start, 2, "=="));
      } else {
        emitToken(makeToken(TokenKind::Equals, start, 1, "="));
      }
      break;
    case '@':
      emitToken(makeToken(TokenKind::At, start, 1, "@"));
      break;
    default:
      --index;
      if (isOperatorChar(ch)) {
        emitToken(lexOperator(index));
        break;
      }
      ++index;
      diagnostics_.error({currentSource_, start, 1}, "unexpected character");
      emitToken(makeToken(TokenKind::Unknown, start, 1, std::string(1, ch)));
      break;
    }
  }

  return tokens;
}

Token Lexer::lexCharOrSymbol(std::size_t& index) const {
  const std::size_t start = index;
  ++index;

  if (index >= text_.size() || text_[index] == '\n') {
    diagnostics_.error({currentSource_, start, index - start},
                       "unterminated character literal");
    return makeToken(TokenKind::CharLiteral, start, index - start,
                     std::string(text_.substr(start, index - start)));
  }

  if (text_[index] == '\\') {
    if (index + 1 < text_.size()) {
      const char escaped = text_[index + 1];
      switch (escaped) {
      case 'b':
      case 't':
      case 'n':
      case 'f':
      case 'r':
      case '"':
      case '\'':
      case '\\':
        break;
      default:
        diagnostics_.error({currentSource_, index, 2},
                           "unknown character escape sequence");
        break;
      }
    }
    index += index + 1 < text_.size() ? 2 : 1;
    if (index < text_.size() && text_[index] == '\'') {
      ++index;
      return makeToken(TokenKind::CharLiteral, start, index - start,
                       std::string(text_.substr(start, index - start)));
    }
    diagnostics_.error({currentSource_, start, index - start},
                       "unterminated character literal");
    return makeToken(TokenKind::CharLiteral, start, index - start,
                     std::string(text_.substr(start, index - start)));
  }

  if (index + 1 < text_.size() && text_[index + 1] == '\'') {
    index += 2;
    return makeToken(TokenKind::CharLiteral, start, index - start,
                     std::string(text_.substr(start, index - start)));
  }

  if (isIdentifierStart(text_[index])) {
    while (index < text_.size() && isIdentifierPart(text_[index])) {
      ++index;
    }
    return makeToken(TokenKind::SymbolLiteral, start, index - start,
                     std::string(text_.substr(start, index - start)));
  }

  while (index < text_.size() && text_[index] != '\'' && text_[index] != '\n') {
    ++index;
  }
  if (index < text_.size() && text_[index] == '\'') {
    ++index;
  }
  diagnostics_.error({currentSource_, start, index - start},
                     "invalid character literal");
  return makeToken(TokenKind::CharLiteral, start, index - start,
                   std::string(text_.substr(start, index - start)));
}

Token Lexer::lexOperator(std::size_t& index) const {
  const std::size_t start = index;
  while (index < text_.size() && isOperatorChar(text_[index])) {
    if (text_[index] == '=' && index + 1 < text_.size() && text_[index + 1] == '>') {
      break;
    }
    ++index;
  }
  std::string text(text_.substr(start, index - start));
  const TokenKind kind = text == support::StdNames::NotImplemented
                             ? TokenKind::Identifier
                             : TokenKind::Operator;
  return makeToken(kind, start, index - start, std::move(text));
}

void Lexer::pushToken(std::vector<Token>& tokens, Token token,
                      bool hadLeadingNewline) const {
  if (hadLeadingNewline && !tokens.empty() && canEndStatement(tokens.back().kind) &&
      canStartStatement(token.kind)) {
    Token separator = makeToken(TokenKind::Semicolon, token.span.start, 0, "\n");
    separator.isVirtual = true;
    tokens.push_back(std::move(separator));
  }
  tokens.push_back(std::move(token));
}

TokenKind Lexer::keywordKind(std::string_view text) const {
  if (text == "case") {
    return TokenKind::KeywordCase;
  }
  if (text == "catch") {
    return TokenKind::KeywordCatch;
  }
  if (text == "class") {
    return TokenKind::KeywordClass;
  }
  if (text == "def") {
    return TokenKind::KeywordDef;
  }
  if (text == "else") {
    return TokenKind::KeywordElse;
  }
  if (text == "extends") {
    return TokenKind::KeywordExtends;
  }
  if (text == "false") {
    return TokenKind::KeywordFalse;
  }
  if (text == "finally") {
    return TokenKind::KeywordFinally;
  }
  if (text == "for") {
    return TokenKind::KeywordFor;
  }
  if (text == "given") {
    return TokenKind::KeywordGiven;
  }
  if (text == "if") {
    return TokenKind::KeywordIf;
  }
  if (text == "import") {
    return TokenKind::KeywordImport;
  }
  if (text == "match") {
    return TokenKind::KeywordMatch;
  }
  if (text == "new") {
    return TokenKind::KeywordNew;
  }
  if (text == "null") {
    return TokenKind::KeywordNull;
  }
  if (text == "object") {
    return TokenKind::KeywordObject;
  }
  if (text == "override") {
    return TokenKind::KeywordOverride;
  }
  if (text == "package") {
    return TokenKind::KeywordPackage;
  }
  if (text == "return") {
    return TokenKind::KeywordReturn;
  }
  if (text == "throw") {
    return TokenKind::KeywordThrow;
  }
  if (text == "try") {
    return TokenKind::KeywordTry;
  }
  if (text == "this") {
    return TokenKind::KeywordThis;
  }
  if (text == "trait") {
    return TokenKind::KeywordTrait;
  }
  if (text == "true") {
    return TokenKind::KeywordTrue;
  }
  if (text == "type") {
    return TokenKind::KeywordType;
  }
  if (text == "using") {
    return TokenKind::KeywordUsing;
  }
  if (text == "val") {
    return TokenKind::KeywordVal;
  }
  if (text == "var") {
    return TokenKind::KeywordVar;
  }
  if (text == "with") {
    return TokenKind::KeywordWith;
  }
  if (text == "while") {
    return TokenKind::KeywordWhile;
  }
  return TokenKind::Identifier;
}

bool Lexer::isIdentifierStart(char ch) {
  return std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '$';
}

bool Lexer::isIdentifierPart(char ch) {
  return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '$';
}

bool Lexer::isOperatorChar(char ch) {
  switch (ch) {
  case '!':
  case '#':
  case '%':
  case '&':
  case '*':
  case '+':
  case '-':
  case '/':
  case '<':
  case '=':
  case '>':
  case '?':
  case '\\':
  case '^':
  case '|':
  case '~':
    return true;
  default:
    return false;
  }
}

bool Lexer::canEndStatement(TokenKind kind) {
  switch (kind) {
  case TokenKind::Identifier:
  case TokenKind::IntegerLiteral:
  case TokenKind::FloatingLiteral:
  case TokenKind::StringLiteral:
  case TokenKind::CharLiteral:
  case TokenKind::SymbolLiteral:
  case TokenKind::InterpolatedStringEnd:
  case TokenKind::KeywordFalse:
  case TokenKind::KeywordNull:
  case TokenKind::KeywordThis:
  case TokenKind::KeywordTrue:
  case TokenKind::RightBrace:
  case TokenKind::RightParen:
  case TokenKind::RightBracket:
    return true;
  default:
    return false;
  }
}

bool Lexer::canStartStatement(TokenKind kind) {
  switch (kind) {
  case TokenKind::Identifier:
  case TokenKind::IntegerLiteral:
  case TokenKind::FloatingLiteral:
  case TokenKind::StringLiteral:
  case TokenKind::CharLiteral:
  case TokenKind::SymbolLiteral:
  case TokenKind::InterpolatedStringStart:
  case TokenKind::KeywordCase:
  case TokenKind::KeywordTry:
  case TokenKind::KeywordClass:
  case TokenKind::KeywordDef:
  case TokenKind::KeywordFalse:
  case TokenKind::KeywordFor:
  case TokenKind::KeywordGiven:
  case TokenKind::KeywordIf:
  case TokenKind::KeywordImport:
  case TokenKind::KeywordNew:
  case TokenKind::KeywordNull:
  case TokenKind::KeywordObject:
  case TokenKind::KeywordOverride:
  case TokenKind::KeywordReturn:
  case TokenKind::KeywordThrow:
  case TokenKind::KeywordThis:
  case TokenKind::KeywordTrait:
  case TokenKind::KeywordTrue:
  case TokenKind::KeywordType:
  case TokenKind::KeywordVal:
  case TokenKind::KeywordVar:
  case TokenKind::KeywordWhile:
  case TokenKind::LeftBrace:
  case TokenKind::RightBrace:
    return true;
  default:
    return false;
  }
}

const char* tokenKindName(TokenKind kind) {
  switch (kind) {
  case TokenKind::EndOfFile:
    return "eof";
  case TokenKind::Identifier:
    return "identifier";
  case TokenKind::IntegerLiteral:
    return "integer";
  case TokenKind::FloatingLiteral:
    return "floating";
  case TokenKind::StringLiteral:
    return "string";
  case TokenKind::CharLiteral:
    return "char";
  case TokenKind::SymbolLiteral:
    return "symbol";
  case TokenKind::InterpolatedStringStart:
    return "interpolated-string-start";
  case TokenKind::InterpolatedStringPart:
    return "interpolated-string-part";
  case TokenKind::InterpolatedStringEnd:
    return "interpolated-string-end";
  case TokenKind::InterpolationIdentifier:
    return "interpolation-identifier";
  case TokenKind::InterpolationStart:
    return "interpolation-start";
  case TokenKind::InterpolationEnd:
    return "interpolation-end";
  case TokenKind::KeywordCase:
    return "case";
  case TokenKind::KeywordCatch:
    return "catch";
  case TokenKind::KeywordClass:
    return "class";
  case TokenKind::KeywordDef:
    return "def";
  case TokenKind::KeywordElse:
    return "else";
  case TokenKind::KeywordExtends:
    return "extends";
  case TokenKind::KeywordFalse:
    return "false";
  case TokenKind::KeywordFinally:
    return "finally";
  case TokenKind::KeywordFor:
    return "for";
  case TokenKind::KeywordGiven:
    return "given";
  case TokenKind::KeywordIf:
    return "if";
  case TokenKind::KeywordImport:
    return "import";
  case TokenKind::KeywordMatch:
    return "match";
  case TokenKind::KeywordNew:
    return "new";
  case TokenKind::KeywordNull:
    return "null";
  case TokenKind::KeywordObject:
    return "object";
  case TokenKind::KeywordOverride:
    return "override";
  case TokenKind::KeywordPackage:
    return "package";
  case TokenKind::KeywordReturn:
    return "return";
  case TokenKind::KeywordThrow:
    return "throw";
  case TokenKind::KeywordTry:
    return "try";
  case TokenKind::KeywordThis:
    return "this";
  case TokenKind::KeywordTrait:
    return "trait";
  case TokenKind::KeywordTrue:
    return "true";
  case TokenKind::KeywordType:
    return "type";
  case TokenKind::KeywordUsing:
    return "using";
  case TokenKind::KeywordVal:
    return "val";
  case TokenKind::KeywordVar:
    return "var";
  case TokenKind::KeywordWith:
    return "with";
  case TokenKind::KeywordWhile:
    return "while";
  case TokenKind::LeftBrace:
    return "{";
  case TokenKind::RightBrace:
    return "}";
  case TokenKind::LeftParen:
    return "(";
  case TokenKind::RightParen:
    return ")";
  case TokenKind::LeftBracket:
    return "[";
  case TokenKind::RightBracket:
    return "]";
  case TokenKind::Semicolon:
    return ";";
  case TokenKind::Colon:
    return ":";
  case TokenKind::Comma:
    return ",";
  case TokenKind::Dot:
    return ".";
  case TokenKind::Equals:
    return "=";
  case TokenKind::Arrow:
    return "=>";
  case TokenKind::At:
    return "@";
  case TokenKind::Operator:
    return "operator";
  case TokenKind::Unknown:
    return "unknown";
  }
  return "unknown";
}

} // namespace scalanative::frontend
