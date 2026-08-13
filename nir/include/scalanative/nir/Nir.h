#pragma once

#include "scalanative/support/SourceSpan.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace scalanative::nir {

enum class DefinitionKind {
  Module,
  Class,
  Trait,
  TypeMember,
  Field,
  FunctionDecl,
  FunctionDef
};

enum class ValueKind {
  Unit,
  Local,
  Literal,
  Unary,
  Binary,
  Assign,
  Throw,
  Try,
  Catch,
  Finally,
  Call,
  Select,
  If,
  While,
  Block,
  LocalLet,
  LocalVar,
  New,
  SizeOf,
  ZoneScoped,
  Box,
  Unbox,
  IsInstanceOf,
  AsInstanceOf,
  Super,
  Unknown
};

enum class InstructionKind { Param, Let, Var, Eval, Return, Throw, Unreachable };

struct Value {
  ValueKind kind = ValueKind::Unknown;
  std::string type;
  std::string text;
  std::vector<Value> operands;
  support::SourceSpan span;
};

struct Instruction {
  InstructionKind kind = InstructionKind::Eval;
  std::string name;
  std::string type;
  Value value;
  support::SourceSpan span;
  std::vector<support::SourceSpan> lexicalScopes;
};

struct FunctionBody {
  std::string entry = "entry";
  std::vector<Instruction> instructions;

  [[nodiscard]] bool empty() const;
  [[nodiscard]] const Instruction* terminator() const;
};

struct Definition {
  DefinitionKind kind = DefinitionKind::FunctionDecl;
  std::string name;
  std::string signature;
  FunctionBody body;
  support::SourceSpan span;
};

struct Module {
  std::string name;
  std::vector<Definition> definitions;
};

struct LinearizationResult {
  std::vector<std::string> names;
  bool cyclic = false;
  bool consistent = true;
};

[[nodiscard]] const char* definitionKindName(DefinitionKind kind);
[[nodiscard]] const char* valueKindName(ValueKind kind);
[[nodiscard]] const char* instructionKindName(InstructionKind kind);
[[nodiscard]] bool isTerminator(InstructionKind kind);
[[nodiscard]] std::string valueToText(const Value& value);
[[nodiscard]] std::string instructionToText(const Instruction& instruction);
[[nodiscard]] std::vector<std::string> bodyToText(const FunctionBody& body);
[[nodiscard]] std::vector<std::string>
metadataParentNames(const std::string& signature);
[[nodiscard]] LinearizationResult checkedLinearizedParentNames(
    const std::vector<std::string>& directParents,
    const std::unordered_map<std::string, std::vector<std::string>>& parentMap);
[[nodiscard]] std::vector<std::string> linearizedParentNames(
    const std::vector<std::string>& directParents,
    const std::unordered_map<std::string, std::vector<std::string>>& parentMap);
[[nodiscard]] std::vector<std::string> linearizedTypeNames(
    const std::string& typeName,
    const std::unordered_map<std::string, std::vector<std::string>>& parentMap);
[[nodiscard]] bool isStackSuper(const Value& value);
[[nodiscard]] std::string toText(const Module& module);

} // namespace scalanative::nir
