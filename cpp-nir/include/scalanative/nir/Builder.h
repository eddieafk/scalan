#pragma once

#include "scalanative/nir/Nir.h"

#include <string>
#include <vector>

namespace scalanative::nir {

[[nodiscard]] Value unitValue(support::SourceSpan span = {});
[[nodiscard]] Value localValue(std::string name, support::SourceSpan span = {});
[[nodiscard]] Value literalValue(std::string text, std::string type,
                                 support::SourceSpan span = {});
[[nodiscard]] Value newValue(std::string typeName, support::SourceSpan span = {});
[[nodiscard]] Value newValue(std::string typeName, std::vector<Value> arguments,
                             support::SourceSpan span = {});
[[nodiscard]] Value sizeOfValue(std::string typeName, support::SourceSpan span = {});
[[nodiscard]] Value zoneScopedValue(Value value, support::SourceSpan span = {});
[[nodiscard]] Value boxValue(std::string primitiveType, Value value,
                             support::SourceSpan span = {});
[[nodiscard]] Value unboxValue(std::string primitiveType, Value value,
                               support::SourceSpan span = {});
[[nodiscard]] Value isInstanceOfValue(std::string targetType, Value value,
                                      support::SourceSpan span = {});
[[nodiscard]] Value asInstanceOfValue(std::string targetType, Value value,
                                      support::SourceSpan span = {});
[[nodiscard]] Value superValue(std::string parentType, support::SourceSpan span = {});
[[nodiscard]] Value qualifiedSuperValue(std::string parentType,
                                        support::SourceSpan span = {});
[[nodiscard]] Value stackSuperValue(std::string ownerType,
                                    support::SourceSpan span = {});
[[nodiscard]] Value selectValue(Value receiver, std::string member,
                                support::SourceSpan span = {});
[[nodiscard]] Value callValue(Value callee, std::vector<Value> arguments,
                              support::SourceSpan span = {});
[[nodiscard]] Value unaryValue(std::string op, Value value,
                               support::SourceSpan span = {});
[[nodiscard]] Value binaryValue(std::string op, Value lhs, Value rhs,
                                support::SourceSpan span = {});
[[nodiscard]] Value assignValue(Value target, Value assignedValue,
                                support::SourceSpan span = {});
[[nodiscard]] Value throwValue(Value exception, support::SourceSpan span = {});
[[nodiscard]] Value catchValue(std::string bindingName, std::string exceptionType,
                               Value body, std::string resultType,
                               support::SourceSpan span = {});
[[nodiscard]] Value finallyValue(Value body, support::SourceSpan span = {});
[[nodiscard]] Value tryValue(Value body, std::vector<Value> catches,
                             std::string resultType, support::SourceSpan span = {});
[[nodiscard]] Value tryValue(Value body, std::vector<Value> catches, Value finalizer,
                             std::string resultType, support::SourceSpan span = {});
[[nodiscard]] Value ifValue(Value condition, Value thenValue, Value elseValue,
                            support::SourceSpan span = {});
[[nodiscard]] Value whileValue(Value condition, Value body,
                               support::SourceSpan span = {});
[[nodiscard]] Value blockValue(std::vector<Value> values,
                               support::SourceSpan span = {});
[[nodiscard]] Value localLetValue(std::string name, std::string type, Value value,
                                  support::SourceSpan span = {});
[[nodiscard]] Value localVarValue(std::string name, std::string type, Value value,
                                  support::SourceSpan span = {});
[[nodiscard]] Value unknownValue(std::string text, support::SourceSpan span = {});

class FunctionBodyBuilder {
public:
  explicit FunctionBodyBuilder(std::string entry = "entry");

  [[nodiscard]] bool addParameter(std::string name, std::string type,
                                  support::SourceSpan span = {},
                                  std::vector<support::SourceSpan> lexicalScopes = {});
  [[nodiscard]] bool addLet(std::string name, Value value,
                            support::SourceSpan span = {},
                            std::vector<support::SourceSpan> lexicalScopes = {});
  [[nodiscard]] bool addLet(std::string name, std::string type, Value value,
                            support::SourceSpan span = {},
                            std::vector<support::SourceSpan> lexicalScopes = {});
  [[nodiscard]] bool addVar(std::string name, Value value,
                            support::SourceSpan span = {},
                            std::vector<support::SourceSpan> lexicalScopes = {});
  [[nodiscard]] bool addVar(std::string name, std::string type, Value value,
                            support::SourceSpan span = {},
                            std::vector<support::SourceSpan> lexicalScopes = {});
  [[nodiscard]] bool addEval(Value value, support::SourceSpan span = {},
                             std::vector<support::SourceSpan> lexicalScopes = {});
  [[nodiscard]] bool addReturn(std::string type, Value value,
                               support::SourceSpan span = {},
                               std::vector<support::SourceSpan> lexicalScopes = {});
  [[nodiscard]] bool addThrow(Value exception, support::SourceSpan span = {},
                              std::vector<support::SourceSpan> lexicalScopes = {});
  [[nodiscard]] bool
  addUnreachable(support::SourceSpan span = {},
                 std::vector<support::SourceSpan> lexicalScopes = {});

  [[nodiscard]] bool terminated() const;
  [[nodiscard]] FunctionBody build() &&;

private:
  [[nodiscard]] bool append(Instruction instruction);

  FunctionBody body_;
  bool terminated_ = false;
};

class ModuleBuilder {
public:
  explicit ModuleBuilder(std::string name);

  void addDefinition(Definition definition);
  void addModule(std::string name, support::SourceSpan span);
  void addFunctionDecl(std::string name, std::string signature,
                       support::SourceSpan span);
  void addFunctionDef(std::string name, std::string signature, FunctionBody body,
                      support::SourceSpan span);

  [[nodiscard]] Module build() &&;

private:
  Module module_;
};

} // namespace scalanative::nir
