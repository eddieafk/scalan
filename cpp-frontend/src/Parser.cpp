#include "scalanative/frontend/Parser.h"

#include "scalanative/support/StdNames.h"

#include <cctype>
#include <utility>

namespace scalanative::frontend {

namespace {

AstExpression makeExpression(AstExpressionKind kind, std::string text,
                             support::SourceSpan span) {
  AstExpression expression;
  expression.kind = kind;
  expression.text = std::move(text);
  expression.span = span;
  return expression;
}

bool isCapitalizedIdentifier(std::string_view text) {
  return !text.empty() && std::isupper(static_cast<unsigned char>(text.front())) != 0;
}

std::size_t formatSpecifierLength(std::string_view text) {
  if (text.empty() || text.front() != '%') {
    return 0;
  }

  std::size_t index = 1;
  while (index < text.size() && (text[index] == '-' || text[index] == '+' ||
                                 text[index] == ' ' || text[index] == '0')) {
    ++index;
  }
  while (index < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[index])) != 0) {
    ++index;
  }
  if (index < text.size() && text[index] == '.') {
    ++index;
    while (index < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[index])) != 0) {
      ++index;
    }
  }
  return index < text.size() &&
                 (text[index] == 'f' || text[index] == 'd' || text[index] == 'c' ||
                  text[index] == 'b' || text[index] == 's')
             ? index + 1
             : 0;
}

std::string runtimeFormatSpecifier(std::string format) {
  if (!format.empty() && format.back() == 'd') {
    format.insert(format.size() - 1, "ll");
  } else if (!format.empty() && format.back() == 'b') {
    // C's snprintf has no Boolean conversion; use the selected text pointer with %s.
    format.back() = 's';
  }
  return format;
}

AstExpression makeFormatExpression(AstExpression value, std::string target,
                                   std::string format, support::SourceSpan span) {
  AstExpression call;
  call.kind = AstExpressionKind::Call;
  call.span = span;
  call.children.push_back(
      makeExpression(AstExpressionKind::Identifier, std::move(target), span));
  call.children.push_back(
      makeExpression(AstExpressionKind::StringLiteral, "\"" + format + "\"", span));
  call.children.push_back(std::move(value));
  return call;
}

} // namespace

Parser::Parser(std::vector<Token> tokens, support::DiagnosticEngine& diagnostics)
    : tokens_(std::move(tokens)), diagnostics_(diagnostics) {}

AstModule Parser::parse() {
  AstModule module;
  consumeSeparators();
  if (match(TokenKind::KeywordPackage)) {
    parsePackage(module);
  }

  while (!isAtEnd()) {
    consumeSeparators();
    if (isAtEnd()) {
      break;
    }
    AstDeclaration declaration = parseDeclaration();
    if (!declaration.name.empty() || declaration.kind == AstDeclarationKind::Import) {
      module.declarations.push_back(std::move(declaration));
    }
  }

  return module;
}

bool Parser::isAtEnd() const {
  return peek().kind == TokenKind::EndOfFile;
}

bool Parser::check(TokenKind kind) const {
  if (isAtEnd()) {
    return kind == TokenKind::EndOfFile;
  }
  return peek().kind == kind;
}

bool Parser::match(TokenKind kind) {
  if (!check(kind)) {
    return false;
  }
  advance();
  return true;
}

bool Parser::isDeclarationStart() const {
  switch (peek().kind) {
  case TokenKind::KeywordObject:
  case TokenKind::KeywordClass:
  case TokenKind::KeywordTrait:
  case TokenKind::KeywordType:
  case TokenKind::KeywordDef:
  case TokenKind::KeywordGiven:
  case TokenKind::KeywordVal:
  case TokenKind::KeywordVar:
  case TokenKind::KeywordImport:
  case TokenKind::KeywordOverride:
    return true;
  default:
    return false;
  }
}

bool Parser::isExpressionBoundary() const {
  switch (peek().kind) {
  case TokenKind::EndOfFile:
  case TokenKind::Semicolon:
  case TokenKind::Comma:
  case TokenKind::RightBrace:
  case TokenKind::RightParen:
  case TokenKind::RightBracket:
  case TokenKind::InterpolationEnd:
    return true;
  default:
    return false;
  }
}

const Token& Parser::peek() const {
  return tokens_[current_];
}

const Token& Parser::previous() const {
  return tokens_[current_ - 1];
}

const Token& Parser::advance() {
  if (!isAtEnd()) {
    ++current_;
  }
  return previous();
}

void Parser::consumeSeparators() {
  while (match(TokenKind::Semicolon)) {
  }
}

void Parser::parsePackage(AstModule& module) {
  const Token& start = previous();
  module.packageName = parseQualifiedName();
  AstDeclaration declaration;
  declaration.kind = AstDeclarationKind::Package;
  declaration.name = module.packageName;
  declaration.span = start.span;
  module.declarations.push_back(std::move(declaration));
  consumeSeparators();
}

AstDeclaration Parser::parseDeclaration() {
  consumeSeparators();
  if (match(TokenKind::KeywordObject)) {
    return parseObjectLike(AstDeclarationKind::Object, previous());
  }
  if (match(TokenKind::KeywordClass)) {
    return parseObjectLike(AstDeclarationKind::Class, previous());
  }
  if (match(TokenKind::KeywordTrait)) {
    return parseObjectLike(AstDeclarationKind::Trait, previous());
  }
  if (match(TokenKind::KeywordType)) {
    return parseType(previous());
  }
  if (match(TokenKind::KeywordDef)) {
    return parseDef(previous());
  }
  if (match(TokenKind::KeywordGiven)) {
    return parseGiven(previous());
  }
  if (match(TokenKind::KeywordVal)) {
    return parseValOrVar(AstDeclarationKind::Val, previous());
  }
  if (match(TokenKind::KeywordVar)) {
    return parseValOrVar(AstDeclarationKind::Var, previous());
  }
  if (match(TokenKind::KeywordImport)) {
    return parseImport(previous());
  }
  if (match(TokenKind::KeywordOverride)) {
    return parseOverride(previous());
  }

  diagnostics_.error(peek().span, "expected declaration, found " +
                                      std::string(tokenKindName(peek().kind)));
  synchronize();
  return {};
}

AstDeclaration Parser::parseObjectLike(AstDeclarationKind kind, const Token& keyword) {
  AstDeclaration declaration;
  declaration.kind = kind;
  declaration.span = keyword.span;

  if (!match(TokenKind::Identifier)) {
    diagnostics_.error(peek().span, "expected declaration name");
    synchronize();
    return declaration;
  }
  declaration.name = previous().text;

  if (check(TokenKind::LeftBracket)) {
    declaration.typeParameters = parseTypeParameterList();
  }

  if (check(TokenKind::LeftParen)) {
    declaration.parameters = parseParameterList(kind == AstDeclarationKind::Class);
  }

  if (match(TokenKind::KeywordExtends)) {
    declaration.declaredType = parseTypeName();
    if (!declaration.declaredType.empty()) {
      declaration.parentTypes.push_back(declaration.declaredType);
    }
    if (check(TokenKind::LeftParen)) {
      declaration.parentArguments = parseArgumentList();
    }
    while (match(TokenKind::KeywordWith)) {
      std::string parent = parseTypeName();
      if (!parent.empty()) {
        declaration.parentTypes.push_back(std::move(parent));
      }
      if (check(TokenKind::LeftParen)) {
        diagnostics_.error(peek().span,
                           "constructor arguments are only supported on the first "
                           "extends parent");
        (void)parseArgumentList();
      }
    }
  }

  if (check(TokenKind::LeftBrace)) {
    const bool capturesInitializationOrder =
        kind == AstDeclarationKind::Class || kind == AstDeclarationKind::Object;
    declaration.members = parseMemberBlock(
        capturesInitializationOrder ? &declaration.constructorBody : nullptr,
        capturesInitializationOrder ? &declaration.classBodyItems : nullptr);
  } else {
    consumeSeparators();
  }

  return declaration;
}

AstDeclaration Parser::parseImport(const Token& keyword) {
  AstDeclaration declaration;
  declaration.kind = AstDeclarationKind::Import;
  declaration.span = keyword.span;

  if (!match(TokenKind::Identifier)) {
    diagnostics_.error(peek().span, "expected import owner");
    consumeSeparators();
    return declaration;
  }
  declaration.importPath = previous().text;
  while (check(TokenKind::Dot) && current_ + 1 < tokens_.size() &&
         tokens_[current_ + 1].kind == TokenKind::Identifier) {
    advance();
    advance();
    declaration.importPath += '.';
    declaration.importPath += previous().text;
  }

  if (match(TokenKind::Dot)) {
    if (!consume(TokenKind::LeftBrace, "expected '{' after import owner")) {
      consumeSeparators();
      return declaration;
    }
    while (!isAtEnd() && !check(TokenKind::RightBrace)) {
      if (!match(TokenKind::Identifier)) {
        diagnostics_.error(peek().span, "expected import selector");
        break;
      }
      AstImportSelector selector;
      selector.name = previous().text;
      selector.alias = selector.name;
      selector.span = previous().span;
      if (match(TokenKind::Arrow)) {
        if (!match(TokenKind::Identifier)) {
          diagnostics_.error(peek().span, "expected alias after '=>'");
          break;
        }
        selector.alias = previous().text;
      }
      declaration.importSelectors.push_back(std::move(selector));
      if (!match(TokenKind::Comma)) {
        break;
      }
    }
    consume(TokenKind::RightBrace, "expected '}' after import selectors");
  }

  if (declaration.importSelectors.empty()) {
    const std::size_t dot = declaration.importPath.rfind('.');
    declaration.name = dot == std::string::npos
                           ? declaration.importPath
                           : declaration.importPath.substr(dot + 1);
  }

  consumeSeparators();
  return declaration;
}

AstDeclaration Parser::parseOverride(const Token& keyword) {
  if (match(TokenKind::KeywordDef)) {
    AstDeclaration declaration = parseDef(previous());
    declaration.isOverride = true;
    declaration.span = keyword.span;
    return declaration;
  }
  if (match(TokenKind::KeywordVal)) {
    AstDeclaration declaration = parseValOrVar(AstDeclarationKind::Val, previous());
    declaration.isOverride = true;
    declaration.span = keyword.span;
    return declaration;
  }
  if (match(TokenKind::KeywordVar)) {
    AstDeclaration declaration = parseValOrVar(AstDeclarationKind::Var, previous());
    declaration.isOverride = true;
    declaration.span = keyword.span;
    return declaration;
  }
  if (match(TokenKind::KeywordType)) {
    AstDeclaration declaration = parseType(previous());
    declaration.isOverride = true;
    declaration.span = keyword.span;
    return declaration;
  }

  diagnostics_.error(keyword.span,
                     "override is only supported for type, def, val, and var "
                     "declarations");
  synchronize();
  return {};
}

AstDeclaration Parser::parseType(const Token& keyword) {
  AstDeclaration declaration;
  declaration.kind = AstDeclarationKind::Type;
  declaration.span = keyword.span;

  if (!match(TokenKind::Identifier)) {
    diagnostics_.error(peek().span, "expected type member name");
    synchronize();
    return declaration;
  }
  declaration.name = previous().text;

  if (check(TokenKind::Operator) && peek().text == ">") {
    advance();
    if (consume(TokenKind::Colon, "expected ':' after '>' in type bound")) {
      declaration.lowerBound = parseTypeName(true);
      if (declaration.lowerBound.empty()) {
        diagnostics_.error(peek().span, "expected lower-bound type");
      }
    }
    if (check(TokenKind::Operator) && peek().text == "<") {
      advance();
      if (consume(TokenKind::Colon, "expected ':' after '<' in type bound")) {
        declaration.upperBound = parseTypeName();
        if (declaration.upperBound.empty()) {
          diagnostics_.error(peek().span, "expected upper-bound type");
        }
      }
    }
  } else if (check(TokenKind::Operator) && peek().text == "<") {
    advance();
    if (consume(TokenKind::Colon, "expected ':' after '<' in type bound")) {
      declaration.upperBound = parseTypeName();
      if (declaration.upperBound.empty()) {
        diagnostics_.error(peek().span, "expected upper-bound type");
      }
    }
  } else if (match(TokenKind::Equals)) {
    declaration.hasInitializer = true;
    declaration.declaredType = parseTypeName();
    if (declaration.declaredType.empty()) {
      diagnostics_.error(peek().span, "expected type alias target");
    }
  }

  consumeSeparators();
  return declaration;
}

AstDeclaration Parser::parseDef(const Token& keyword) {
  AstDeclaration declaration;
  declaration.kind = AstDeclarationKind::Def;
  declaration.span = keyword.span;

  if (!match(TokenKind::Identifier)) {
    diagnostics_.error(peek().span, "expected method name");
    synchronize();
    return declaration;
  }
  declaration.name = previous().text;

  if (check(TokenKind::LeftBracket)) {
    declaration.typeParameters = parseTypeParameterList();
  }

  bool sawParameterClause = false;
  bool sawContextualClause = false;
  while (check(TokenKind::LeftParen)) {
    bool contextualClause = false;
    std::vector<std::string> parameters = parseParameterList(false, &contextualClause);
    if (sawContextualClause && !contextualClause) {
      diagnostics_.error(keyword.span,
                         "ordinary parameter clauses cannot follow a using clause");
    } else if (sawParameterClause && !contextualClause) {
      diagnostics_.error(keyword.span,
                         "multiple ordinary parameter clauses are not supported; "
                         "a trailing using clause is supported");
    }
    declaration.parameters.insert(declaration.parameters.end(), parameters.begin(),
                                  parameters.end());
    declaration.contextualParameters.insert(declaration.contextualParameters.end(),
                                            parameters.size(), contextualClause);
    sawParameterClause = true;
    sawContextualClause = sawContextualClause || contextualClause;
  }

  if (match(TokenKind::Colon)) {
    declaration.declaredType = parseTypeName();
  }

  if (match(TokenKind::Equals)) {
    declaration.hasInitializer = true;
    declaration.initializer = parseExpression();
  } else if (match(TokenKind::LeftBrace)) {
    declaration.hasInitializer = true;
    declaration.initializer = parseBlockExpression();
  }

  consumeSeparators();
  return declaration;
}

AstDeclaration Parser::parseGiven(const Token& keyword) {
  AstDeclaration declaration;
  declaration.kind = AstDeclarationKind::Val;
  declaration.span = keyword.span;
  declaration.isGiven = true;

  bool namedParameterized = false;
  if (check(TokenKind::Identifier) && current_ + 1 < tokens_.size()) {
    if (tokens_[current_ + 1].kind == TokenKind::LeftParen) {
      namedParameterized = true;
    } else if (tokens_[current_ + 1].kind == TokenKind::LeftBracket) {
      std::size_t cursor = current_ + 1;
      std::size_t depth = 0;
      while (cursor < tokens_.size()) {
        if (tokens_[cursor].kind == TokenKind::LeftBracket) {
          ++depth;
        } else if (tokens_[cursor].kind == TokenKind::RightBracket) {
          if (--depth == 0) {
            ++cursor;
            break;
          }
        }
        ++cursor;
      }
      namedParameterized =
          cursor < tokens_.size() && (tokens_[cursor].kind == TokenKind::LeftParen ||
                                      tokens_[cursor].kind == TokenKind::Colon);
    }
  }
  const bool named = check(TokenKind::Identifier) && current_ + 1 < tokens_.size() &&
                     tokens_[current_ + 1].kind == TokenKind::Colon;
  if (namedParameterized) {
    declaration.kind = AstDeclarationKind::Def;
    advance();
    declaration.name = previous().text;
    if (check(TokenKind::LeftBracket)) {
      declaration.typeParameters = parseTypeParameterList();
    }
    while (check(TokenKind::LeftParen)) {
      bool contextualClause = false;
      std::vector<std::string> parameters =
          parseParameterList(false, &contextualClause);
      declaration.parameters.insert(declaration.parameters.end(), parameters.begin(),
                                    parameters.end());
      declaration.contextualParameters.insert(declaration.contextualParameters.end(),
                                              parameters.size(), contextualClause);
      if (!contextualClause && !parameters.empty()) {
        diagnostics_.error(keyword.span,
                           "parameterized given parameters must use a using clause");
      }
    }
    if (!consume(TokenKind::Colon,
                 "expected ':' before parameterized given result type")) {
      synchronize();
      return declaration;
    }
    declaration.declaredType = parseTypeName();
  } else if (named) {
    advance();
    declaration.name = previous().text;
    advance();
    declaration.declaredType = parseTypeName();
  } else {
    declaration.isAnonymousGiven = true;
    declaration.name = "given$" + std::to_string(keyword.span.start);
    declaration.declaredType = parseTypeName();
  }
  if (declaration.declaredType.empty()) {
    diagnostics_.error(peek().span, "expected given result type");
  }
  if (match(TokenKind::Equals)) {
    declaration.hasInitializer = true;
    declaration.initializer = parseExpression();
  } else {
    diagnostics_.error(peek().span, "given declaration requires an initializer");
  }

  consumeSeparators();
  return declaration;
}

AstDeclaration Parser::parseValOrVar(AstDeclarationKind kind, const Token& keyword) {
  AstDeclaration declaration;
  declaration.kind = kind;
  declaration.span = keyword.span;

  if (!match(TokenKind::Identifier)) {
    diagnostics_.error(peek().span, "expected value name");
    synchronize();
    return declaration;
  }
  declaration.name = previous().text;

  if (match(TokenKind::Colon)) {
    declaration.declaredType = parseTypeName();
  }

  if (match(TokenKind::Equals)) {
    declaration.hasInitializer = true;
    declaration.initializer = parseExpression();
  }

  consumeSeparators();
  return declaration;
}

std::vector<AstTypeParameter> Parser::parseTypeParameterList() {
  std::vector<AstTypeParameter> parameters;
  if (!consume(TokenKind::LeftBracket, "expected '['")) {
    return parameters;
  }

  consumeSeparators();
  while (!isAtEnd() && !check(TokenKind::RightBracket)) {
    TypeVariance variance = TypeVariance::Invariant;
    if (check(TokenKind::Operator) && (peek().text == "+" || peek().text == "-")) {
      variance =
          peek().text == "+" ? TypeVariance::Covariant : TypeVariance::Contravariant;
      advance();
    }
    if (!match(TokenKind::Identifier)) {
      diagnostics_.error(peek().span, "expected type parameter name");
      synchronize();
      break;
    }

    AstTypeParameter parameter;
    parameter.name = previous().text;
    parameter.span = previous().span;
    parameter.variance = variance;
    if (check(TokenKind::Operator) && peek().text == ">") {
      advance();
      if (consume(TokenKind::Colon, "expected ':' after '>' in type bound")) {
        parameter.lowerBound = parseTypeName(true, true);
        if (parameter.lowerBound.empty()) {
          diagnostics_.error(peek().span, "expected lower-bound type");
        }
      }
    }
    if (check(TokenKind::Operator) && peek().text == "<") {
      advance();
      if (consume(TokenKind::Colon, "expected ':' after '<' in type bound")) {
        parameter.upperBound = parseTypeName(false, true);
        if (parameter.upperBound.empty()) {
          diagnostics_.error(peek().span, "expected upper-bound type");
        }
      }
    }
    parameters.push_back(std::move(parameter));

    consumeSeparators();
    if (!match(TokenKind::Comma)) {
      break;
    }
    consumeSeparators();
  }

  consume(TokenKind::RightBracket, "expected ']' after type parameter list");
  return parameters;
}

std::vector<std::string> Parser::parseParameterList(bool allowModifiers,
                                                    bool* contextualClause) {
  std::vector<std::string> parameters;
  if (!consume(TokenKind::LeftParen, "expected '('")) {
    return parameters;
  }

  consumeSeparators();
  const bool contextual = match(TokenKind::KeywordUsing);
  if (contextualClause != nullptr) {
    *contextualClause = contextual;
  }
  consumeSeparators();
  while (!isAtEnd() && !check(TokenKind::RightParen)) {
    std::string modifier;
    if (allowModifiers &&
        (check(TokenKind::KeywordVal) || check(TokenKind::KeywordVar))) {
      modifier = peek().text;
      advance();
    }
    if (!match(TokenKind::Identifier)) {
      diagnostics_.error(peek().span, "expected parameter name");
      synchronize();
      break;
    }
    std::string parameter =
        modifier.empty() ? previous().text : modifier + " " + previous().text;
    if (match(TokenKind::Colon)) {
      std::string type = parseTypeName();
      if (!type.empty()) {
        parameter += ": " + type;
      }
    }
    parameters.push_back(std::move(parameter));

    consumeSeparators();
    if (!match(TokenKind::Comma)) {
      break;
    }
    consumeSeparators();
  }

  consume(TokenKind::RightParen, "expected ')' after parameter list");
  return parameters;
}

std::vector<AstExpression> Parser::parseArgumentList() {
  std::vector<AstExpression> arguments;
  if (!consume(TokenKind::LeftParen, "expected '('")) {
    return arguments;
  }

  consumeSeparators();
  while (!isAtEnd() && !check(TokenKind::RightParen)) {
    arguments.push_back(parseExpression());
    consumeSeparators();
    if (!match(TokenKind::Comma)) {
      break;
    }
    consumeSeparators();
  }

  consume(TokenKind::RightParen, "expected ')' after argument list");
  return arguments;
}

std::string Parser::parseTypeName(bool stopAtUpperBound, bool stopAtRightBracket,
                                  bool stopAtMatchAlternative) {
  std::string type;
  bool previousWasTypeJoiner = false;
  std::size_t bracketDepth = 0;
  while (!isAtEnd()) {
    if (stopAtUpperBound && check(TokenKind::Operator) && peek().text == "<" &&
        current_ + 1 < tokens_.size() &&
        tokens_[current_ + 1].kind == TokenKind::Colon) {
      return type;
    }
    if (stopAtRightBracket && check(TokenKind::RightBracket) && bracketDepth == 0) {
      return type;
    }
    if (stopAtMatchAlternative && check(TokenKind::Operator) && peek().text == "|") {
      return type;
    }
    if (check(TokenKind::Comma) && bracketDepth > 0) {
      if (!type.empty() && !previousWasTypeJoiner) {
        type += ' ';
      }
      type += ',';
      previousWasTypeJoiner = false;
      advance();
      continue;
    }
    switch (peek().kind) {
    case TokenKind::Equals:
    case TokenKind::Comma:
    case TokenKind::Semicolon:
    case TokenKind::LeftParen:
    case TokenKind::LeftBrace:
    case TokenKind::RightBrace:
    case TokenKind::RightParen:
    case TokenKind::Arrow:
    case TokenKind::KeywordIf:
    case TokenKind::KeywordWith:
      return type;
    default:
      if (peek().kind == TokenKind::LeftBracket) {
        ++bracketDepth;
      } else if (peek().kind == TokenKind::RightBracket && bracketDepth > 0) {
        --bracketDepth;
      }
      const bool isTypeJoiner =
          peek().kind == TokenKind::Dot ||
          (peek().kind == TokenKind::Operator && peek().text == "#");
      if (!type.empty() && !isTypeJoiner && !previousWasTypeJoiner) {
        type += ' ';
      }
      type += peek().text.empty() ? tokenKindName(peek().kind) : peek().text;
      previousWasTypeJoiner = isTypeJoiner;
      advance();
      break;
    }
  }
  return type;
}

std::vector<AstDeclaration>
Parser::parseMemberBlock(std::vector<AstExpression>* constructorBody,
                         std::vector<AstClassBodyItem>* classBodyItems) {
  std::vector<AstDeclaration> members;
  if (!consume(TokenKind::LeftBrace, "expected '{' to start member block")) {
    return members;
  }

  consumeSeparators();
  while (!isAtEnd() && !check(TokenKind::RightBrace)) {
    if (isDeclarationStart()) {
      AstDeclaration member = parseDeclaration();
      if (!member.name.empty() || member.kind == AstDeclarationKind::Import) {
        if (classBodyItems != nullptr) {
          classBodyItems->push_back(
              AstClassBodyItem{AstClassBodyItemKind::Declaration, members.size()});
        }
        members.push_back(std::move(member));
      }
      consumeSeparators();
      continue;
    }

    if (constructorBody != nullptr) {
      const std::size_t expressionIndex = constructorBody->size();
      constructorBody->push_back(parseExpression());
      if (classBodyItems != nullptr) {
        classBodyItems->push_back(
            AstClassBodyItem{AstClassBodyItemKind::Expression, expressionIndex});
      }
      consumeSeparators();
      continue;
    }

    diagnostics_.error(peek().span, "expected member declaration, found " +
                                        std::string(tokenKindName(peek().kind)));
    synchronize();
    consumeSeparators();
  }

  consume(TokenKind::RightBrace, "expected '}' after member block");
  consumeSeparators();
  return members;
}

AstExpression Parser::parseExpression() {
  return parseAssignmentExpression();
}

AstExpression Parser::parseAssignmentExpression() {
  AstExpression left = parseBinaryExpression();
  if (!match(TokenKind::Equals)) {
    return left;
  }

  const Token& equals = previous();
  if (check(TokenKind::Equals)) {
    diagnostics_.error(peek().span, "expected expression after assignment operator");
    advance();
    return left;
  }
  AstExpression assign;
  assign.kind = AstExpressionKind::Assign;
  assign.text = equals.text;
  assign.span = equals.span;
  assign.children.push_back(std::move(left));
  assign.children.push_back(parseAssignmentExpression());
  return assign;
}

AstExpression Parser::parseBinaryExpression(int minPrecedence) {
  AstExpression left = parseUnaryExpression();

  while (!isAtEnd()) {
    const int currentPrecedence = precedence(peek());
    if (currentPrecedence < minPrecedence) {
      break;
    }

    Token op = advance();
    AstExpression right = parseBinaryExpression(currentPrecedence + 1);
    AstExpression binary;
    binary.kind = AstExpressionKind::Binary;
    binary.text = op.text;
    binary.span = op.span;
    binary.children.push_back(std::move(left));
    binary.children.push_back(std::move(right));
    left = std::move(binary);
  }

  return left;
}

AstExpression Parser::parseUnaryExpression() {
  if (check(TokenKind::Operator) &&
      (peek().text == "!" || peek().text == "+" || peek().text == "-")) {
    Token op = advance();
    AstExpression unary;
    unary.kind = AstExpressionKind::Unary;
    unary.text = std::move(op.text);
    unary.span = op.span;
    unary.children.push_back(parseUnaryExpression());
    return unary;
  }
  return parsePostfixExpression();
}

AstExpression Parser::parsePostfixExpression() {
  AstExpression expression = parsePrimaryExpression();

  while (!isAtEnd()) {
    if (expression.kind == AstExpressionKind::Super && match(TokenKind::LeftBracket)) {
      if (expression.text != "super") {
        diagnostics_.error(previous().span, "super may only have one parent qualifier");
      }

      std::string qualifier;
      if (!match(TokenKind::Identifier)) {
        diagnostics_.error(peek().span, "expected parent type inside super qualifier");
      } else {
        qualifier = previous().text;
        while (match(TokenKind::Dot)) {
          if (!match(TokenKind::Identifier)) {
            diagnostics_.error(peek().span,
                               "expected type name after '.' in super qualifier");
            break;
          }
          qualifier += "." + previous().text;
        }
      }
      consume(TokenKind::RightBracket, "expected ']' after super parent qualifier");
      if (!qualifier.empty()) {
        expression.text = std::move(qualifier);
      }
      continue;
    }

    if (match(TokenKind::LeftBracket)) {
      const Token& bracket = previous();
      AstExpression typeApply;
      typeApply.kind = AstExpressionKind::TypeApply;
      typeApply.span = bracket.span;
      consumeSeparators();
      while (!isAtEnd() && !check(TokenKind::RightBracket)) {
        std::string typeArgument = parseTypeName(false, true);
        if (typeArgument.empty()) {
          diagnostics_.error(peek().span, "expected type argument inside brackets");
          break;
        }
        typeApply.typeArguments.push_back(std::move(typeArgument));
        consumeSeparators();
        if (!match(TokenKind::Comma)) {
          break;
        }
        consumeSeparators();
      }
      if (!typeApply.typeArguments.empty()) {
        typeApply.declaredType = typeApply.typeArguments.front();
      }
      consume(TokenKind::RightBracket, "expected ']' after type arguments");
      typeApply.children.push_back(std::move(expression));
      expression = std::move(typeApply);
      continue;
    }

    if (match(TokenKind::Dot)) {
      const Token& dot = previous();
      if (!match(TokenKind::Identifier)) {
        diagnostics_.error(peek().span, "expected member name after '.'");
        break;
      }
      AstExpression select;
      select.kind = AstExpressionKind::Select;
      select.text = previous().text;
      select.span = dot.span;
      select.children.push_back(std::move(expression));
      expression = std::move(select);
      continue;
    }

    if (check(TokenKind::LeftParen)) {
      const Token& callStart = advance();
      const bool usingClause = match(TokenKind::KeywordUsing);
      consumeSeparators();
      AstExpression call;
      call.kind = AstExpressionKind::Call;
      call.span = callStart.span;
      if (usingClause && expression.kind == AstExpressionKind::Call) {
        call = std::move(expression);
      } else {
        call.children.push_back(std::move(expression));
      }
      const std::size_t firstArgument = call.children.size();
      while (!isAtEnd() && !check(TokenKind::RightParen)) {
        call.children.push_back(parseExpression());
        consumeSeparators();
        if (!match(TokenKind::Comma)) {
          break;
        }
        consumeSeparators();
      }
      consume(TokenKind::RightParen, "expected ')' after argument list");
      if (usingClause && call.children.size() == firstArgument) {
        diagnostics_.error(callStart.span,
                           "using argument clause requires at least one argument");
      }
      expression = std::move(call);
      continue;
    }

    if (match(TokenKind::KeywordMatch)) {
      expression = parseMatchExpression(std::move(expression), previous());
      continue;
    }

    break;
  }

  return expression;
}

AstExpression Parser::parsePrimaryExpression() {
  if (match(TokenKind::Identifier)) {
    if (previous().text == "super") {
      return makeExpression(AstExpressionKind::Super, previous().text, previous().span);
    }
    return makeExpression(AstExpressionKind::Identifier, previous().text,
                          previous().span);
  }
  if (match(TokenKind::IntegerLiteral)) {
    return makeExpression(AstExpressionKind::IntegerLiteral, previous().text,
                          previous().span);
  }
  if (match(TokenKind::FloatingLiteral)) {
    return makeExpression(AstExpressionKind::FloatingLiteral, previous().text,
                          previous().span);
  }
  if (match(TokenKind::StringLiteral)) {
    return makeExpression(AstExpressionKind::StringLiteral, previous().text,
                          previous().span);
  }
  if (match(TokenKind::CharLiteral)) {
    return makeExpression(AstExpressionKind::CharLiteral, previous().text,
                          previous().span);
  }
  if (match(TokenKind::SymbolLiteral)) {
    return makeExpression(AstExpressionKind::SymbolLiteral, previous().text,
                          previous().span);
  }
  if (match(TokenKind::KeywordTrue) || match(TokenKind::KeywordFalse)) {
    return makeExpression(AstExpressionKind::BooleanLiteral, previous().text,
                          previous().span);
  }
  if (match(TokenKind::KeywordNull)) {
    return makeExpression(AstExpressionKind::NullLiteral, previous().text,
                          previous().span);
  }
  if (match(TokenKind::KeywordThis)) {
    return makeExpression(AstExpressionKind::This, previous().text, previous().span);
  }
  if (match(TokenKind::InterpolatedStringStart)) {
    return parseInterpolatedStringExpression();
  }
  if (match(TokenKind::LeftBrace)) {
    return parseBlockExpression();
  }
  if (match(TokenKind::LeftParen)) {
    AstExpression expression = parseExpression();
    consume(TokenKind::RightParen, "expected ')' after expression");
    return expression;
  }
  if (match(TokenKind::KeywordReturn)) {
    return parseReturnExpression(previous());
  }
  if (match(TokenKind::KeywordThrow)) {
    return parseThrowExpression(previous());
  }
  if (match(TokenKind::KeywordTry)) {
    return parseTryExpression(previous());
  }
  if (match(TokenKind::KeywordIf)) {
    return parseIfExpression(previous());
  }
  if (match(TokenKind::KeywordWhile)) {
    return parseWhileExpression(previous());
  }
  if (match(TokenKind::KeywordNew)) {
    const Token& keyword = previous();
    AstExpression expression;
    expression.kind = AstExpressionKind::New;
    expression.span = keyword.span;
    expression.text = parseQualifiedName();
    if (check(TokenKind::LeftParen)) {
      AstExpression callee = expression;
      expression.kind = AstExpressionKind::Call;
      expression.children.push_back(std::move(callee));
      advance();
      while (!isAtEnd() && !check(TokenKind::RightParen)) {
        expression.children.push_back(parseExpression());
        if (!match(TokenKind::Comma)) {
          break;
        }
      }
      consume(TokenKind::RightParen, "expected ')' after constructor arguments");
    }
    return expression;
  }

  diagnostics_.error(peek().span, "expected expression, found " +
                                      std::string(tokenKindName(peek().kind)));
  if (!isAtEnd() && !isExpressionBoundary()) {
    advance();
  }
  return {};
}

AstExpression Parser::parseBlockExpression() {
  const Token& start = previous();
  AstExpression block;
  block.kind = AstExpressionKind::Block;
  block.span = start.span;

  consumeSeparators();
  while (!isAtEnd() && !check(TokenKind::RightBrace)) {
    if (isDeclarationStart()) {
      AstDeclaration local = parseDeclaration();
      if (local.kind == AstDeclarationKind::Type) {
        diagnostics_.error(local.span, "local type declarations are not supported yet");
        consumeSeparators();
        continue;
      }
      AstExpression localExpression;
      localExpression.kind = AstExpressionKind::LocalDeclaration;
      localExpression.text = local.name;
      localExpression.declaredType = local.declaredType;
      localExpression.span = local.span;
      localExpression.mutableLocal = local.kind == AstDeclarationKind::Var;
      localExpression.isGiven = local.isGiven;
      localExpression.isAnonymousGiven = local.isAnonymousGiven;
      if (local.isGiven && local.kind == AstDeclarationKind::Def) {
        diagnostics_.error(local.span,
                           "local parameterized givens are not supported yet");
      }
      if (local.hasInitializer) {
        localExpression.children.push_back(std::move(local.initializer));
      }
      block.children.push_back(std::move(localExpression));
    } else {
      block.children.push_back(parseExpression());
    }
    consumeSeparators();
  }

  consume(TokenKind::RightBrace, "expected '}' after block");
  return block;
}

AstExpression Parser::parseInterpolatedStringExpression() {
  const Token& start = previous();
  const bool isSInterpolation =
      start.text.size() >= 2 && start.text[0] == 's' && start.text[1] == '"';
  const bool isFInterpolation =
      start.text.size() >= 2 && start.text[0] == 'f' && start.text[1] == '"';
  const bool isRawInterpolation =
      start.text.size() >= 4 && start.text.rfind("raw\"", 0) == 0;
  const bool isTripleQuoted =
      start.text.size() >= 3 && start.text.substr(start.text.size() - 3) == "\"\"\"";
  if (!isSInterpolation && !isFInterpolation && !isRawInterpolation) {
    diagnostics_.error(
        start.span, "only s-, f-, and raw-interpolation are supported in this subset");
  }

  std::vector<AstExpression> parts;
  auto appendStringPart = [&](std::string text, support::SourceSpan span) {
    const std::string delimiter =
        isTripleQuoted || isRawInterpolation ? "\"\"\"" : "\"";
    parts.push_back(makeExpression(AstExpressionKind::StringLiteral,
                                   delimiter + text + delimiter, span));
  };
  bool awaitingFloatingFormat = false;
  std::size_t floatingValueIndex = 0;
  auto appendHole = [&](AstExpression value) {
    if (isFInterpolation && awaitingFloatingFormat) {
      diagnostics_.error(
          value.span, "expected a %...f, %...d, %...c, %...b, or %...s specifier after "
                      "f-interpolation hole");
      awaitingFloatingFormat = false;
    }
    parts.push_back(std::move(value));
    if (isFInterpolation) {
      floatingValueIndex = parts.size() - 1;
      awaitingFloatingFormat = true;
    }
  };

  while (!isAtEnd() && !check(TokenKind::InterpolatedStringEnd)) {
    if (match(TokenKind::InterpolatedStringPart)) {
      const Token& part = previous();
      if (isFInterpolation && awaitingFloatingFormat) {
        const std::size_t formatLength = formatSpecifierLength(part.text);
        if (formatLength == 0) {
          diagnostics_.error(
              part.span,
              "f-interpolation holes require a %...f, %...d, %...c, %...b, or %...s "
              "format specifier");
          awaitingFloatingFormat = false;
          appendStringPart(part.text, part.span);
          continue;
        }
        const std::string sourceFormat = part.text.substr(0, formatLength);
        const std::string formatTarget =
            sourceFormat.back() == 'b'
                ? std::string(support::StdNames::RuntimeFormatBoolean)
                : std::string(support::StdNames::RuntimeFormat);
        parts[floatingValueIndex] =
            makeFormatExpression(std::move(parts[floatingValueIndex]), formatTarget,
                                 runtimeFormatSpecifier(sourceFormat), part.span);
        awaitingFloatingFormat = false;
        if (formatLength < part.text.size()) {
          appendStringPart(part.text.substr(formatLength), part.span);
        }
        continue;
      }
      appendStringPart(part.text, part.span);
      continue;
    }
    if (match(TokenKind::InterpolationIdentifier)) {
      appendHole(makeExpression(AstExpressionKind::Identifier, previous().text,
                                previous().span));
      continue;
    }
    if (match(TokenKind::InterpolationStart)) {
      const Token& interpolationStart = previous();
      if (check(TokenKind::InterpolationEnd)) {
        diagnostics_.error(interpolationStart.span,
                           "expected expression inside interpolation hole");
      } else {
        appendHole(parseExpression());
      }
      if (!match(TokenKind::InterpolationEnd)) {
        diagnostics_.error(interpolationStart.span,
                           "expected '}' after interpolation expression");
      }
      continue;
    }

    diagnostics_.error(peek().span, "unexpected token in interpolated string");
    advance();
  }

  if (!match(TokenKind::InterpolatedStringEnd)) {
    diagnostics_.error(start.span, "expected end of interpolated string");
  }
  if (isFInterpolation && awaitingFloatingFormat) {
    diagnostics_.error(start.span,
                       "expected a %...f, %...d, %...c, %...b, or %...s specifier "
                       "after f-interpolation hole");
  }

  if (parts.empty()) {
    return makeExpression(
        AstExpressionKind::StringLiteral,
        isTripleQuoted || isRawInterpolation ? "\"\"\"\"\"\"" : "\"\"", start.span);
  }

  AstExpression expression = std::move(parts.front());
  for (std::size_t index = 1; index < parts.size(); ++index) {
    AstExpression concatenation;
    concatenation.kind = AstExpressionKind::Binary;
    concatenation.text = "+";
    concatenation.span = start.span;
    concatenation.children.push_back(std::move(expression));
    concatenation.children.push_back(std::move(parts[index]));
    expression = std::move(concatenation);
  }
  return expression;
}

AstExpression Parser::parseReturnExpression(const Token& keyword) {
  AstExpression expression;
  expression.kind = AstExpressionKind::Return;
  expression.span = keyword.span;
  if (!isExpressionBoundary()) {
    expression.children.push_back(parseExpression());
  }
  return expression;
}

AstExpression Parser::parseThrowExpression(const Token& keyword) {
  AstExpression expression;
  expression.kind = AstExpressionKind::Throw;
  expression.span = keyword.span;
  if (isExpressionBoundary()) {
    diagnostics_.error(keyword.span, "expected exception expression after throw");
  } else {
    expression.children.push_back(parseExpression());
  }
  return expression;
}

AstExpression Parser::parseTryExpression(const Token& keyword) {
  AstExpression expression;
  expression.kind = AstExpressionKind::Try;
  expression.span = keyword.span;
  expression.children.push_back(parseExpression());

  bool hasCatch = false;
  if (match(TokenKind::KeywordCatch)) {
    hasCatch = true;
    if (consume(TokenKind::LeftBrace, "expected '{' after catch")) {
      bool sawCatchAll = false;
      consumeSeparators();
      while (!isAtEnd() && !check(TokenKind::RightBrace)) {
        if (!match(TokenKind::KeywordCase)) {
          diagnostics_.error(peek().span, "expected case in catch expression");
          break;
        }
        const Token& caseKeyword = previous();
        if (!match(TokenKind::Identifier)) {
          diagnostics_.error(peek().span, "catch cases require a binding name or '_'");
          break;
        }

        const Token& binding = previous();
        const bool catchAll = binding.text == "_";
        std::string catchType = "Object";
        if (match(TokenKind::Colon)) {
          catchType = parseTypeName(false, false, true);
          if (catchType.empty()) {
            diagnostics_.error(previous().span,
                               "expected exception type after ':' in catch case");
          }
        } else if (!catchAll) {
          diagnostics_.error(binding.span,
                             "named catch bindings require an exception type");
        }
        if (sawCatchAll) {
          diagnostics_.error(caseKeyword.span,
                             "catch-all case must be the final catch case");
        }
        sawCatchAll = sawCatchAll || catchAll;

        consume(TokenKind::Arrow, "expected '=>' after catch pattern");
        AstExpression handler;
        handler.kind = AstExpressionKind::Catch;
        handler.text =
            catchAll ? "$catch" + std::to_string(nextSyntheticLocal_++) : binding.text;
        handler.declaredType = std::move(catchType);
        handler.span = caseKeyword.span;
        handler.children.push_back(match(TokenKind::LeftBrace) ? parseBlockExpression()
                                                               : parseExpression());
        expression.children.push_back(std::move(handler));
        consumeSeparators();
      }
      consume(TokenKind::RightBrace, "expected '}' after catch cases");
    }
  }

  bool hasFinally = false;
  if (match(TokenKind::KeywordFinally)) {
    hasFinally = true;
    AstExpression finalizer;
    finalizer.kind = AstExpressionKind::Finally;
    finalizer.span = previous().span;
    finalizer.children.push_back(parseExpression());
    expression.children.push_back(std::move(finalizer));
  }
  if (!hasCatch && !hasFinally) {
    diagnostics_.error(keyword.span, "try expression requires catch or finally");
  }
  if (hasCatch && expression.children.size() == 1 + (hasFinally ? 1U : 0U)) {
    diagnostics_.error(keyword.span, "catch expression requires at least one case");
  }
  return expression;
}

AstExpression Parser::parseIfExpression(const Token& keyword) {
  AstExpression expression;
  expression.kind = AstExpressionKind::If;
  expression.span = keyword.span;

  consume(TokenKind::LeftParen, "expected '(' after if");
  expression.children.push_back(parseExpression());
  consume(TokenKind::RightParen, "expected ')' after if condition");
  expression.children.push_back(parseExpression());
  if (match(TokenKind::KeywordElse)) {
    expression.children.push_back(parseExpression());
  }
  return expression;
}

AstExpression Parser::parseWhileExpression(const Token& keyword) {
  AstExpression expression;
  expression.kind = AstExpressionKind::While;
  expression.span = keyword.span;

  consume(TokenKind::LeftParen, "expected '(' after while");
  expression.children.push_back(parseExpression());
  consume(TokenKind::RightParen, "expected ')' after while condition");
  expression.children.push_back(parseExpression());
  return expression;
}

AstExpression Parser::parseStableModuleReference(const Token& first) {
  AstExpression expression =
      makeExpression(AstExpressionKind::ModuleReference, first.text, first.span);
  while (match(TokenKind::Dot)) {
    if (!match(TokenKind::Identifier)) {
      diagnostics_.error(peek().span,
                         "expected object name after '.' in singleton match pattern");
      break;
    }
    expression.text += "." + previous().text;
  }
  return expression;
}

AstExpression Parser::parseMatchExpression(AstExpression selector,
                                           const Token& keyword) {
  struct MatchCase {
    AstExpression pattern;
    AstExpression body;
    AstExpression guard;
    std::vector<AstExpression> alternativePatterns;
    std::vector<std::string> alternativeTypePatterns;
    std::string bindingName;
    std::string typePattern;
    bool isWildcard = false;
    bool hasGuard = false;
  };

  if (!consume(TokenKind::LeftBrace, "expected '{' after match")) {
    return selector;
  }

  std::vector<MatchCase> cases;
  consumeSeparators();
  while (!isAtEnd() && !check(TokenKind::RightBrace)) {
    if (!match(TokenKind::KeywordCase)) {
      diagnostics_.error(peek().span, "expected case in match expression");
      break;
    }

    MatchCase current;
    if (match(TokenKind::Identifier)) {
      if (previous().text == "_") {
        current.isWildcard = true;
        current.pattern = makeExpression(AstExpressionKind::Identifier, previous().text,
                                         previous().span);
      } else if (isCapitalizedIdentifier(previous().text)) {
        current.pattern = parseStableModuleReference(previous());
      } else {
        current.bindingName = previous().text;
        current.pattern = makeExpression(AstExpressionKind::Identifier, previous().text,
                                         previous().span);
      }
    } else if (match(TokenKind::IntegerLiteral)) {
      current.pattern = makeExpression(AstExpressionKind::IntegerLiteral,
                                       previous().text, previous().span);
    } else if (match(TokenKind::FloatingLiteral)) {
      current.pattern = makeExpression(AstExpressionKind::FloatingLiteral,
                                       previous().text, previous().span);
    } else if (match(TokenKind::StringLiteral)) {
      current.pattern = makeExpression(AstExpressionKind::StringLiteral,
                                       previous().text, previous().span);
    } else if (match(TokenKind::CharLiteral)) {
      current.pattern = makeExpression(AstExpressionKind::CharLiteral, previous().text,
                                       previous().span);
    } else if (match(TokenKind::KeywordTrue) || match(TokenKind::KeywordFalse)) {
      current.pattern = makeExpression(AstExpressionKind::BooleanLiteral,
                                       previous().text, previous().span);
    } else if (match(TokenKind::KeywordNull)) {
      current.pattern = makeExpression(AstExpressionKind::NullLiteral, previous().text,
                                       previous().span);
    } else {
      diagnostics_.error(
          peek().span,
          "match patterns currently support literals, bindings, and '_' only");
    }

    if (match(TokenKind::Colon)) {
      const support::SourceSpan colonSpan = previous().span;
      const std::string parsedType = parseTypeName(false, false, true);
      if (!current.isWildcard && current.bindingName.empty()) {
        diagnostics_.error(colonSpan,
                           "type patterns require a wildcard or binding name");
      } else if (parsedType.empty()) {
        diagnostics_.error(colonSpan, "expected type name after ':' in match pattern");
      } else {
        current.typePattern = parsedType;
      }
    }

    while (check(TokenKind::Operator) && peek().text == "|") {
      const Token& separator = advance();
      if (!current.typePattern.empty()) {
        if (!current.isWildcard) {
          diagnostics_.error(
              separator.span,
              "match type pattern alternatives require wildcard patterns");
        }
        if (!match(TokenKind::Identifier) || previous().text != "_") {
          diagnostics_.error(peek().span,
                             "expected wildcard type pattern after '|' in match case");
        }
        if (!consume(TokenKind::Colon,
                     "expected ':' after wildcard in type pattern alternative")) {
          continue;
        }
        const std::string alternativeType = parseTypeName(false, false, true);
        if (alternativeType.empty()) {
          diagnostics_.error(peek().span,
                             "expected type name in match pattern alternative");
        } else {
          current.alternativeTypePatterns.push_back(alternativeType);
        }
        continue;
      }
      if (current.pattern.kind == AstExpressionKind::ModuleReference) {
        if (!match(TokenKind::Identifier)) {
          diagnostics_.error(
              peek().span, "expected singleton object pattern after '|' in match case");
        } else if (!isCapitalizedIdentifier(previous().text)) {
          diagnostics_.error(
              previous().span,
              "singleton pattern alternatives require capitalized object names");
        } else {
          current.alternativePatterns.push_back(parseStableModuleReference(previous()));
        }
        continue;
      }
      if (current.isWildcard || !current.bindingName.empty()) {
        diagnostics_.error(separator.span,
                           "match pattern alternatives require literal patterns");
      }

      AstExpression alternative;
      if (match(TokenKind::IntegerLiteral)) {
        alternative = makeExpression(AstExpressionKind::IntegerLiteral, previous().text,
                                     previous().span);
      } else if (match(TokenKind::FloatingLiteral)) {
        alternative = makeExpression(AstExpressionKind::FloatingLiteral,
                                     previous().text, previous().span);
      } else if (match(TokenKind::StringLiteral)) {
        alternative = makeExpression(AstExpressionKind::StringLiteral, previous().text,
                                     previous().span);
      } else if (match(TokenKind::CharLiteral)) {
        alternative = makeExpression(AstExpressionKind::CharLiteral, previous().text,
                                     previous().span);
      } else if (match(TokenKind::KeywordTrue) || match(TokenKind::KeywordFalse)) {
        alternative = makeExpression(AstExpressionKind::BooleanLiteral, previous().text,
                                     previous().span);
      } else if (match(TokenKind::KeywordNull)) {
        alternative = makeExpression(AstExpressionKind::NullLiteral, previous().text,
                                     previous().span);
      } else {
        diagnostics_.error(peek().span,
                           "match pattern alternatives require literal patterns");
      }
      current.alternativePatterns.push_back(std::move(alternative));
    }

    if (match(TokenKind::KeywordIf)) {
      current.hasGuard = true;
      current.guard = parseExpression();
    }

    consume(TokenKind::Arrow, "expected '=>' after match pattern");
    current.body =
        match(TokenKind::LeftBrace) ? parseBlockExpression() : parseExpression();
    cases.push_back(std::move(current));
    consumeSeparators();
  }
  consume(TokenKind::RightBrace, "expected '}' after match cases");

  const auto isCatchAll = [](const MatchCase& current) {
    return ((current.isWildcard || !current.bindingName.empty()) &&
            current.typePattern.empty() && !current.hasGuard);
  };
  bool hasCatchAll = false;
  for (std::size_t index = 0; index < cases.size(); ++index) {
    if (!isCatchAll(cases[index])) {
      continue;
    }
    hasCatchAll = true;
    if (index + 1 != cases.size()) {
      diagnostics_.error(cases[index].pattern.span,
                         "catch-all match case must be the final case");
    }
  }
  if (!hasCatchAll) {
    diagnostics_.error(keyword.span,
                       "match expression requires a final wildcard or binding case");
  }

  const std::string selectorName = "$match" + std::to_string(nextSyntheticLocal_++);
  const auto bindSelector = [&](std::string name, AstExpression expression,
                                support::SourceSpan span,
                                const std::string& declaredType) {
    AstExpression binding;
    binding.kind = AstExpressionKind::LocalDeclaration;
    binding.text = std::move(name);
    binding.span = span;
    binding.declaredType = declaredType;

    AstExpression selectorReference =
        makeExpression(AstExpressionKind::Identifier, selectorName, span);
    if (declaredType.empty()) {
      binding.children.push_back(std::move(selectorReference));
    } else {
      AstExpression castMember;
      castMember.kind = AstExpressionKind::Select;
      castMember.text = support::StdNames::AsInstanceOf;
      castMember.span = span;
      castMember.children.push_back(std::move(selectorReference));

      AstExpression cast;
      cast.kind = AstExpressionKind::TypeApply;
      cast.declaredType = declaredType;
      cast.span = span;
      cast.children.push_back(std::move(castMember));
      binding.children.push_back(std::move(cast));
    }

    AstExpression block;
    block.kind = AstExpressionKind::Block;
    block.span = span;
    block.children.push_back(std::move(binding));
    block.children.push_back(std::move(expression));
    return block;
  };

  AstExpression fallback;
  for (auto current = cases.rbegin(); current != cases.rend(); ++current) {
    if (isCatchAll(*current)) {
      AstExpression body = std::move(current->body);
      if (!current->bindingName.empty()) {
        body = bindSelector(current->bindingName, std::move(body),
                            current->pattern.span, current->typePattern);
      }
      fallback = std::move(body);
      continue;
    }

    AstExpression condition;
    if (!current->typePattern.empty()) {
      const auto makeTypeTest = [&](const std::string& typeName) {
        AstExpression typeTestMember;
        typeTestMember.kind = AstExpressionKind::Select;
        typeTestMember.text = support::StdNames::IsInstanceOf;
        typeTestMember.span = current->pattern.span;
        typeTestMember.children.push_back(
            makeExpression(AstExpressionKind::Identifier, selectorName, selector.span));

        AstExpression typeTest;
        typeTest.kind = AstExpressionKind::TypeApply;
        typeTest.declaredType = typeName;
        typeTest.span = current->pattern.span;
        typeTest.children.push_back(std::move(typeTestMember));
        return typeTest;
      };

      condition = makeTypeTest(current->typePattern);
      for (const std::string& alternativeType : current->alternativeTypePatterns) {
        AstExpression combined;
        combined.kind = AstExpressionKind::Binary;
        combined.text = "||";
        combined.span = condition.span;
        combined.children.push_back(std::move(condition));
        combined.children.push_back(makeTypeTest(alternativeType));
        condition = std::move(combined);
      }
      if (current->hasGuard) {
        AstExpression guard = std::move(current->guard);
        if (!current->bindingName.empty()) {
          guard = bindSelector(current->bindingName, std::move(guard),
                               current->pattern.span, current->typePattern);
        }
        AstExpression guarded;
        guarded.kind = AstExpressionKind::Binary;
        guarded.text = "&&";
        guarded.span = condition.span;
        guarded.children.push_back(std::move(condition));
        guarded.children.push_back(std::move(guard));
        condition = std::move(guarded);
      }
    } else if (current->isWildcard) {
      condition = std::move(current->guard);
    } else if (!current->bindingName.empty()) {
      condition = bindSelector(current->bindingName, std::move(current->guard),
                               current->pattern.span, "");
    } else {
      const auto makeLiteralComparison = [&](AstExpression pattern) {
        AstExpression comparison;
        comparison.kind = AstExpressionKind::Binary;
        comparison.text = "==";
        comparison.span = pattern.span;
        comparison.children.push_back(
            makeExpression(AstExpressionKind::Identifier, selectorName, selector.span));
        comparison.children.push_back(std::move(pattern));
        return comparison;
      };

      condition = makeLiteralComparison(std::move(current->pattern));
      for (AstExpression& alternative : current->alternativePatterns) {
        AstExpression combined;
        combined.kind = AstExpressionKind::Binary;
        combined.text = "||";
        combined.span = condition.span;
        combined.children.push_back(std::move(condition));
        combined.children.push_back(makeLiteralComparison(std::move(alternative)));
        condition = std::move(combined);
      }
      if (current->hasGuard) {
        AstExpression guarded;
        guarded.kind = AstExpressionKind::Binary;
        guarded.text = "&&";
        guarded.span = condition.span;
        guarded.children.push_back(std::move(condition));
        guarded.children.push_back(std::move(current->guard));
        condition = std::move(guarded);
      }
    }

    AstExpression body = std::move(current->body);
    if (!current->bindingName.empty()) {
      body = bindSelector(current->bindingName, std::move(body), current->pattern.span,
                          current->typePattern);
    }

    AstExpression branch;
    branch.kind = AstExpressionKind::If;
    branch.span = keyword.span;
    branch.children.push_back(std::move(condition));
    branch.children.push_back(std::move(body));
    branch.children.push_back(std::move(fallback));
    fallback = std::move(branch);
  }

  AstExpression binding;
  binding.kind = AstExpressionKind::LocalDeclaration;
  binding.text = selectorName;
  binding.span = selector.span;
  binding.children.push_back(std::move(selector));

  AstExpression block;
  block.kind = AstExpressionKind::Block;
  block.span = keyword.span;
  block.children.push_back(std::move(binding));
  block.children.push_back(std::move(fallback));
  return block;
}

int Parser::precedence(const Token& token) const {
  if (token.kind == TokenKind::Equals) {
    return -1;
  }
  if (token.kind != TokenKind::Operator) {
    return -1;
  }
  if (token.text == "*" || token.text == "/" || token.text == "%") {
    return 7;
  }
  if (token.text == "+" || token.text == "-") {
    return 6;
  }
  if (token.text == "==" || token.text == "!=" || token.text == "<" ||
      token.text == ">" || token.text == "<=" || token.text == ">=") {
    return 4;
  }
  if (token.text == "&&") {
    return 3;
  }
  if (token.text == "||") {
    return 2;
  }
  return 5;
}

bool Parser::consume(TokenKind kind, const std::string& message) {
  if (match(kind)) {
    return true;
  }
  diagnostics_.error(peek().span, message);
  return false;
}

std::string Parser::parseQualifiedName() {
  if (!match(TokenKind::Identifier)) {
    diagnostics_.error(peek().span, "expected qualified name");
    return {};
  }

  std::string name = previous().text;
  while (match(TokenKind::Dot)) {
    if (!match(TokenKind::Identifier)) {
      diagnostics_.error(peek().span, "expected identifier after '.'");
      break;
    }
    name += '.';
    name += previous().text;
  }
  return name;
}

void Parser::synchronize() {
  if (!isAtEnd()) {
    advance();
  }
  while (!isAtEnd()) {
    if (check(TokenKind::Semicolon) || check(TokenKind::RightBrace) ||
        isDeclarationStart()) {
      return;
    }
    advance();
  }
}

} // namespace scalanative::frontend
