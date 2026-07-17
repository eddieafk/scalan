#pragma once

#include "scalanative/frontend/Typechecker.h"
#include "scalanative/nir/Builder.h"
#include "scalanative/nir/Nir.h"

#include <map>
#include <unordered_set>

namespace scalanative::nscplugin {

class NirEmitter {
public:
  [[nodiscard]] nir::Module emit(const frontend::TypedModule& module) const;

private:
  static void
  emitDeclaration(nir::ModuleBuilder& builder, const frontend::TypedModule& module,
                  const frontend::TypedDeclaration& declaration,
                  const std::string& owner, frontend::AstDeclarationKind ownerKind,
                  std::unordered_set<std::string>* referenceArrayElementTypes,
                  std::map<std::string, std::string>* runtimeArrayDeclarations);
  [[nodiscard]] static std::string qualify(const frontend::TypedModule& module,
                                           const std::string& name);
  [[nodiscard]] static std::string qualifyMember(const std::string& owner,
                                                 const std::string& name);
};

} // namespace scalanative::nscplugin
