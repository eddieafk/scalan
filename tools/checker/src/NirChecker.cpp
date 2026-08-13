#include "scalanative/tools/checker/NirChecker.h"

#include "scalanative/nir/Verifier.h"

namespace scalanative::tools::checker {

bool NirChecker::check(const nir::Module& module,
                       support::DiagnosticEngine& diagnostics) const {
  nir::Verifier verifier;
  nir::VerifyResult result = verifier.verify(module);
  for (const std::string& error : result.errors) {
    diagnostics.error(support::SourceSpan::none(), "NIR checker: " + error);
  }
  return result.ok;
}

} // namespace scalanative::tools::checker

