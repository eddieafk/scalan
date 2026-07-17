#include "scalanative/nir/Builder.h"

#include <utility>

namespace scalanative::nir {

Value unitValue(support::SourceSpan span) {
  return Value{ValueKind::Unit, "Unit", "unit", {}, span};
}

Value localValue(std::string name, support::SourceSpan span) {
  return Value{ValueKind::Local, {}, std::move(name), {}, span};
}

Value literalValue(std::string text, std::string type, support::SourceSpan span) {
  return Value{ValueKind::Literal, std::move(type), std::move(text), {}, span};
}

Value newValue(std::string typeName, support::SourceSpan span) {
  return newValue(std::move(typeName), {}, span);
}

Value newValue(std::string typeName, std::vector<Value> arguments,
               support::SourceSpan span) {
  return Value{ValueKind::New, typeName, std::move(typeName), std::move(arguments),
               span};
}

Value sizeOfValue(std::string typeName, support::SourceSpan span) {
  return Value{ValueKind::SizeOf, "Int", std::move(typeName), {}, span};
}

Value zoneScopedValue(Value value, support::SourceSpan span) {
  const std::string resultType = value.type;
  std::vector<Value> operands;
  operands.push_back(std::move(value));
  return Value{ValueKind::ZoneScoped, resultType, "Zone.scoped", std::move(operands),
               span};
}

Value boxValue(std::string primitiveType, Value value, support::SourceSpan span) {
  std::vector<Value> operands;
  operands.push_back(std::move(value));
  return Value{ValueKind::Box, "Object", std::move(primitiveType), std::move(operands),
               span};
}

Value unboxValue(std::string primitiveType, Value value, support::SourceSpan span) {
  std::vector<Value> operands;
  operands.push_back(std::move(value));
  return Value{ValueKind::Unbox, primitiveType, std::move(primitiveType),
               std::move(operands), span};
}

Value isInstanceOfValue(std::string targetType, Value value, support::SourceSpan span) {
  std::vector<Value> operands;
  operands.push_back(std::move(value));
  return Value{ValueKind::IsInstanceOf, "Boolean", std::move(targetType),
               std::move(operands), span};
}

Value asInstanceOfValue(std::string targetType, Value value, support::SourceSpan span) {
  std::vector<Value> operands;
  operands.push_back(std::move(value));
  return Value{ValueKind::AsInstanceOf, targetType, std::move(targetType),
               std::move(operands), span};
}

Value superValue(std::string parentType, support::SourceSpan span) {
  return Value{ValueKind::Super, std::move(parentType), "super", {}, span};
}

Value qualifiedSuperValue(std::string parentType, support::SourceSpan span) {
  std::string displayName = "super[" + parentType + "]";
  return Value{
      ValueKind::Super, std::move(parentType), std::move(displayName), {}, span};
}

Value stackSuperValue(std::string ownerType, support::SourceSpan span) {
  std::string displayName = "stack-super[" + ownerType + "]";
  return Value{
      ValueKind::Super, std::move(ownerType), std::move(displayName), {}, span};
}

Value selectValue(Value receiver, std::string member, support::SourceSpan span) {
  std::vector<Value> operands;
  operands.push_back(std::move(receiver));
  return Value{ValueKind::Select, {}, std::move(member), std::move(operands), span};
}

Value callValue(Value callee, std::vector<Value> arguments, support::SourceSpan span) {
  std::vector<Value> operands;
  operands.push_back(std::move(callee));
  for (Value& argument : arguments) {
    operands.push_back(std::move(argument));
  }
  return Value{ValueKind::Call, {}, {}, std::move(operands), span};
}

Value unaryValue(std::string op, Value value, support::SourceSpan span) {
  std::vector<Value> operands;
  operands.push_back(std::move(value));
  return Value{ValueKind::Unary, {}, std::move(op), std::move(operands), span};
}

Value binaryValue(std::string op, Value lhs, Value rhs, support::SourceSpan span) {
  std::vector<Value> operands;
  operands.push_back(std::move(lhs));
  operands.push_back(std::move(rhs));
  return Value{ValueKind::Binary, {}, std::move(op), std::move(operands), span};
}

Value assignValue(Value target, Value assignedValue, support::SourceSpan span) {
  std::vector<Value> operands;
  operands.push_back(std::move(target));
  operands.push_back(std::move(assignedValue));
  return Value{ValueKind::Assign, "Unit", "=", std::move(operands), span};
}

Value throwValue(Value exception, support::SourceSpan span) {
  std::vector<Value> operands;
  operands.push_back(std::move(exception));
  return Value{ValueKind::Throw, "Nothing", "throw", std::move(operands), span};
}

Value catchValue(std::string bindingName, std::string exceptionType, Value body,
                 std::string resultType, support::SourceSpan span) {
  Value binding = localValue(std::move(bindingName), span);
  binding.type = std::move(exceptionType);
  std::vector<Value> operands;
  operands.push_back(std::move(binding));
  operands.push_back(std::move(body));
  return Value{ValueKind::Catch, std::move(resultType), "catch", std::move(operands),
               span};
}

Value finallyValue(Value body, support::SourceSpan span) {
  std::vector<Value> operands;
  operands.push_back(std::move(body));
  return Value{ValueKind::Finally, "Unit", "finally", std::move(operands), span};
}

Value tryValue(Value body, std::vector<Value> catches, std::string resultType,
               support::SourceSpan span) {
  std::vector<Value> operands;
  operands.reserve(catches.size() + 1);
  operands.push_back(std::move(body));
  for (Value& handler : catches) {
    operands.push_back(std::move(handler));
  }
  return Value{ValueKind::Try, std::move(resultType), "try", std::move(operands), span};
}

Value tryValue(Value body, std::vector<Value> catches, Value finalizer,
               std::string resultType, support::SourceSpan span) {
  Value value =
      tryValue(std::move(body), std::move(catches), std::move(resultType), span);
  value.operands.push_back(std::move(finalizer));
  return value;
}

Value ifValue(Value condition, Value thenValue, Value elseValue,
              support::SourceSpan span) {
  std::vector<Value> operands;
  operands.push_back(std::move(condition));
  operands.push_back(std::move(thenValue));
  operands.push_back(std::move(elseValue));
  return Value{ValueKind::If, {}, {}, std::move(operands), span};
}

Value whileValue(Value condition, Value body, support::SourceSpan span) {
  std::vector<Value> operands;
  operands.push_back(std::move(condition));
  operands.push_back(std::move(body));
  return Value{ValueKind::While, {}, {}, std::move(operands), span};
}

Value blockValue(std::vector<Value> values, support::SourceSpan span) {
  const std::string resultType = values.empty() ? "Unit" : values.back().type;
  return Value{ValueKind::Block, resultType, {}, std::move(values), span};
}

Value localLetValue(std::string name, std::string type, Value value,
                    support::SourceSpan span) {
  std::vector<Value> operands;
  operands.push_back(std::move(value));
  return Value{ValueKind::LocalLet, std::move(type), std::move(name),
               std::move(operands), span};
}

Value localVarValue(std::string name, std::string type, Value value,
                    support::SourceSpan span) {
  std::vector<Value> operands;
  operands.push_back(std::move(value));
  return Value{ValueKind::LocalVar, std::move(type), std::move(name),
               std::move(operands), span};
}

Value unknownValue(std::string text, support::SourceSpan span) {
  return Value{ValueKind::Unknown, "Unknown", std::move(text), {}, span};
}

FunctionBodyBuilder::FunctionBodyBuilder(std::string entry) {
  body_.entry = std::move(entry);
}

bool FunctionBodyBuilder::addParameter(std::string name, std::string type,
                                       support::SourceSpan span,
                                       std::vector<support::SourceSpan> lexicalScopes) {
  return append(Instruction{InstructionKind::Param, std::move(name), std::move(type),
                            unitValue(span), span, std::move(lexicalScopes)});
}

bool FunctionBodyBuilder::addLet(std::string name, Value value,
                                 support::SourceSpan span,
                                 std::vector<support::SourceSpan> lexicalScopes) {
  return append(Instruction{InstructionKind::Let,
                            std::move(name),
                            {},
                            std::move(value),
                            span,
                            std::move(lexicalScopes)});
}

bool FunctionBodyBuilder::addLet(std::string name, std::string type, Value value,
                                 support::SourceSpan span,
                                 std::vector<support::SourceSpan> lexicalScopes) {
  return append(Instruction{InstructionKind::Let, std::move(name), std::move(type),
                            std::move(value), span, std::move(lexicalScopes)});
}

bool FunctionBodyBuilder::addVar(std::string name, Value value,
                                 support::SourceSpan span,
                                 std::vector<support::SourceSpan> lexicalScopes) {
  return append(Instruction{InstructionKind::Var,
                            std::move(name),
                            {},
                            std::move(value),
                            span,
                            std::move(lexicalScopes)});
}

bool FunctionBodyBuilder::addVar(std::string name, std::string type, Value value,
                                 support::SourceSpan span,
                                 std::vector<support::SourceSpan> lexicalScopes) {
  return append(Instruction{InstructionKind::Var, std::move(name), std::move(type),
                            std::move(value), span, std::move(lexicalScopes)});
}

bool FunctionBodyBuilder::addEval(Value value, support::SourceSpan span,
                                  std::vector<support::SourceSpan> lexicalScopes) {
  return append(Instruction{
      InstructionKind::Eval, {}, {}, std::move(value), span, std::move(lexicalScopes)});
}

bool FunctionBodyBuilder::addReturn(std::string type, Value value,
                                    support::SourceSpan span,
                                    std::vector<support::SourceSpan> lexicalScopes) {
  return append(Instruction{InstructionKind::Return,
                            {},
                            std::move(type),
                            std::move(value),
                            span,
                            std::move(lexicalScopes)});
}

bool FunctionBodyBuilder::addThrow(Value exception, support::SourceSpan span,
                                   std::vector<support::SourceSpan> lexicalScopes) {
  return append(Instruction{InstructionKind::Throw,
                            {},
                            "Nothing",
                            std::move(exception),
                            span,
                            std::move(lexicalScopes)});
}

bool FunctionBodyBuilder::addUnreachable(
    support::SourceSpan span, std::vector<support::SourceSpan> lexicalScopes) {
  return append(Instruction{InstructionKind::Unreachable,
                            {},
                            {},
                            unitValue(span),
                            span,
                            std::move(lexicalScopes)});
}

bool FunctionBodyBuilder::terminated() const {
  return terminated_;
}

FunctionBody FunctionBodyBuilder::build() && {
  return std::move(body_);
}

bool FunctionBodyBuilder::append(Instruction instruction) {
  if (terminated_) {
    return false;
  }
  terminated_ = isTerminator(instruction.kind);
  body_.instructions.push_back(std::move(instruction));
  return true;
}

ModuleBuilder::ModuleBuilder(std::string name) {
  module_.name = std::move(name);
}

void ModuleBuilder::addDefinition(Definition definition) {
  module_.definitions.push_back(std::move(definition));
}

void ModuleBuilder::addModule(std::string name, support::SourceSpan span) {
  addDefinition(Definition{
      DefinitionKind::Module, std::move(name), "@java.lang.Object", {}, span});
}

void ModuleBuilder::addFunctionDecl(std::string name, std::string signature,
                                    support::SourceSpan span) {
  addDefinition(Definition{
      DefinitionKind::FunctionDecl, std::move(name), std::move(signature), {}, span});
}

void ModuleBuilder::addFunctionDef(std::string name, std::string signature,
                                   FunctionBody body, support::SourceSpan span) {
  addDefinition(Definition{DefinitionKind::FunctionDef, std::move(name),
                           std::move(signature), std::move(body), span});
}

Module ModuleBuilder::build() && {
  return std::move(module_);
}

} // namespace scalanative::nir
