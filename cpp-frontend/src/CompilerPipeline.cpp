#include "scalanative/frontend/CompilerPipeline.h"

#include "scalanative/frontend/AstValidator.h"
#include "scalanative/frontend/Lexer.h"
#include "scalanative/frontend/Parser.h"

#include <string>

namespace scalanative::frontend {

CompileResult CompilerPipeline::compile(support::SourceManager& sources,
                                        support::SourceId source,
                                        support::DiagnosticEngine& diagnostics) {
  CompileResult result;

  Lexer lexer(sources, diagnostics);
  std::vector<Token> tokens = lexer.lex(source);
  result.phaseLog.push_back("source");
  result.phaseLog.push_back("lexer: " + std::to_string(tokens.size()) + " tokens");

  Parser parser(std::move(tokens), diagnostics);
  result.ast = parser.parse();
  result.phaseLog.push_back("parser: " +
                            std::to_string(result.ast.declarations.size()) +
                            " declarations");

  AstValidator validator;
  const bool astValid = validator.validate(result.ast, diagnostics);
  result.phaseLog.push_back(astValid ? "ast: valid" : "ast: invalid");
  if (!astValid) {
    result.ok = false;
    return result;
  }

  Typechecker typechecker(diagnostics);
  result.typed = typechecker.typecheck(result.ast);
  result.phaseLog.push_back("typecheck: " +
                            std::to_string(result.typed.declarations.size()) +
                            " typed declarations");

  result.ok = !diagnostics.hasErrors();
  return result;
}

} // namespace scalanative::frontend
