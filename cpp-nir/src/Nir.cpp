#include "scalanative/nir/Nir.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace scalanative::nir {

bool FunctionBody::empty() const {
  return instructions.empty();
}

const Instruction* FunctionBody::terminator() const {
  if (instructions.empty()) {
    return nullptr;
  }
  const Instruction& last = instructions.back();
  if (!isTerminator(last.kind)) {
    return nullptr;
  }
  return &last;
}

const char* definitionKindName(DefinitionKind kind) {
  switch (kind) {
  case DefinitionKind::Module:
    return "module";
  case DefinitionKind::Class:
    return "class";
  case DefinitionKind::Trait:
    return "trait";
  case DefinitionKind::TypeMember:
    return "type";
  case DefinitionKind::Field:
    return "field";
  case DefinitionKind::FunctionDecl:
    return "declare";
  case DefinitionKind::FunctionDef:
    return "define";
  }
  return "unknown";
}

const char* valueKindName(ValueKind kind) {
  switch (kind) {
  case ValueKind::Unit:
    return "unit";
  case ValueKind::Local:
    return "local";
  case ValueKind::Literal:
    return "literal";
  case ValueKind::Unary:
    return "unary";
  case ValueKind::Binary:
    return "binary";
  case ValueKind::Assign:
    return "assign";
  case ValueKind::Throw:
    return "throw";
  case ValueKind::Try:
    return "try";
  case ValueKind::Catch:
    return "catch";
  case ValueKind::Finally:
    return "finally";
  case ValueKind::Call:
    return "call";
  case ValueKind::Select:
    return "select";
  case ValueKind::If:
    return "if";
  case ValueKind::While:
    return "while";
  case ValueKind::Block:
    return "block";
  case ValueKind::LocalLet:
    return "local-let";
  case ValueKind::LocalVar:
    return "local-var";
  case ValueKind::New:
    return "new";
  case ValueKind::SizeOf:
    return "sizeof";
  case ValueKind::ZoneScoped:
    return "zone-scoped";
  case ValueKind::Box:
    return "box";
  case ValueKind::Unbox:
    return "unbox";
  case ValueKind::IsInstanceOf:
    return "is-instance-of";
  case ValueKind::AsInstanceOf:
    return "as-instance-of";
  case ValueKind::Super:
    return "super";
  case ValueKind::Unknown:
    return "unknown";
  }
  return "unknown";
}

const char* instructionKindName(InstructionKind kind) {
  switch (kind) {
  case InstructionKind::Param:
    return "param";
  case InstructionKind::Let:
    return "let";
  case InstructionKind::Var:
    return "var";
  case InstructionKind::Eval:
    return "eval";
  case InstructionKind::Return:
    return "ret";
  case InstructionKind::Throw:
    return "throw";
  case InstructionKind::Unreachable:
    return "unreachable";
  }
  return "unknown";
}

bool isTerminator(InstructionKind kind) {
  return kind == InstructionKind::Return || kind == InstructionKind::Throw ||
         kind == InstructionKind::Unreachable;
}

std::string valueToText(const Value& value) {
  switch (value.kind) {
  case ValueKind::Unit:
    return "unit";
  case ValueKind::Local:
    return "%" + value.text;
  case ValueKind::Super:
    return "%" + (value.text.empty() ? std::string{"super"} : value.text);
  case ValueKind::Literal:
    return value.text;
  case ValueKind::New: {
    std::ostringstream out;
    out << "new " << value.text;
    if (!value.operands.empty()) {
      out << '(';
      for (std::size_t i = 0; i < value.operands.size(); ++i) {
        if (i != 0) {
          out << ", ";
        }
        out << valueToText(value.operands[i]);
      }
      out << ')';
    }
    return out.str();
  }
  case ValueKind::SizeOf:
    return value.text.empty() ? "<malformed-sizeof>" : "sizeof[" + value.text + "]";
  case ValueKind::ZoneScoped:
    if (value.operands.size() != 1) {
      return "<malformed-zone-scoped>";
    }
    return "zone-scoped(" + valueToText(value.operands.front()) + ")";
  case ValueKind::Box:
  case ValueKind::Unbox:
    if (value.operands.size() != 1 || value.text.empty()) {
      return value.kind == ValueKind::Box ? "<malformed-box>" : "<malformed-unbox>";
    }
    return std::string(value.kind == ValueKind::Box ? "box[" : "unbox[") + value.text +
           "](" + valueToText(value.operands.front()) + ")";
  case ValueKind::IsInstanceOf:
    if (value.operands.size() != 1 || value.text.empty()) {
      return "<malformed-is-instance-of>";
    }
    return "is-instance-of[" + value.text + "](" + valueToText(value.operands.front()) +
           ")";
  case ValueKind::AsInstanceOf:
    if (value.operands.size() != 1 || value.text.empty()) {
      return "<malformed-as-instance-of>";
    }
    return "as-instance-of[" + value.text + "](" + valueToText(value.operands.front()) +
           ")";
  case ValueKind::Select:
    if (value.operands.empty()) {
      return "." + value.text;
    }
    return valueToText(value.operands.front()) + "." + value.text;
  case ValueKind::Call: {
    std::ostringstream out;
    out << "call ";
    if (value.operands.empty()) {
      out << "<empty>";
    } else {
      out << valueToText(value.operands.front());
    }
    out << '(';
    for (std::size_t i = 1; i < value.operands.size(); ++i) {
      if (i != 1) {
        out << ", ";
      }
      out << valueToText(value.operands[i]);
    }
    out << ')';
    return out.str();
  }
  case ValueKind::Unary:
    if (value.operands.size() != 1 || value.text.empty()) {
      return "<malformed-unary>";
    }
    return "(" + value.text + valueToText(value.operands.front()) + ")";
  case ValueKind::Binary:
    if (value.operands.size() != 2) {
      return "<malformed-binary>";
    }
    return "(" + valueToText(value.operands[0]) + " " + value.text + " " +
           valueToText(value.operands[1]) + ")";
  case ValueKind::Assign:
    if (value.operands.size() != 2) {
      return "<malformed-assign>";
    }
    return "assign " + valueToText(value.operands[0]) + " = " +
           valueToText(value.operands[1]);
  case ValueKind::Throw:
    if (value.operands.size() != 1) {
      return "<malformed-throw>";
    }
    return "throw(" + valueToText(value.operands.front()) + ")";
  case ValueKind::Try: {
    if (value.operands.size() < 2) {
      return "<malformed-try>";
    }
    std::ostringstream out;
    out << "try(";
    for (std::size_t index = 0; index < value.operands.size(); ++index) {
      if (index != 0) {
        out << "; ";
      }
      out << valueToText(value.operands[index]);
    }
    out << ')';
    return out.str();
  }
  case ValueKind::Catch:
    if (value.operands.size() != 2 || value.operands.front().kind != ValueKind::Local ||
        value.operands.front().text.empty() || value.operands.front().type.empty()) {
      return "<malformed-catch>";
    }
    return "catch %" + value.operands.front().text + " : " +
           value.operands.front().type + " => " + valueToText(value.operands.back());
  case ValueKind::Finally:
    if (value.operands.size() != 1) {
      return "<malformed-finally>";
    }
    return "finally(" + valueToText(value.operands.front()) + ")";
  case ValueKind::If:
    if (value.operands.size() < 2) {
      return "<malformed-if>";
    }
    if (value.operands.size() == 2) {
      return "if(" + valueToText(value.operands[0]) + ", " +
             valueToText(value.operands[1]) + ", unit)";
    }
    return "if(" + valueToText(value.operands[0]) + ", " +
           valueToText(value.operands[1]) + ", " + valueToText(value.operands[2]) + ")";
  case ValueKind::While:
    if (value.operands.size() != 2) {
      return "<malformed-while>";
    }
    return "while(" + valueToText(value.operands[0]) + ", " +
           valueToText(value.operands[1]) + ")";
  case ValueKind::Block: {
    std::ostringstream out;
    out << "block(";
    for (std::size_t i = 0; i < value.operands.size(); ++i) {
      if (i != 0) {
        out << "; ";
      }
      out << valueToText(value.operands[i]);
    }
    out << ')';
    return out.str();
  }
  case ValueKind::LocalLet:
  case ValueKind::LocalVar:
    if (value.operands.size() != 1 || value.text.empty()) {
      return value.kind == ValueKind::LocalLet ? "<malformed-local-let>"
                                               : "<malformed-local-var>";
    }
    return std::string(value.kind == ValueKind::LocalLet ? "let %" : "var %") +
           value.text + (value.type.empty() ? "" : " : " + value.type) + " = " +
           valueToText(value.operands.front());
  case ValueKind::Unknown:
    if (!value.text.empty()) {
      return value.text;
    }
    return "<unknown-value>";
  }
  return "<unknown-value>";
}

std::string instructionToText(const Instruction& instruction) {
  switch (instruction.kind) {
  case InstructionKind::Param:
    return "param %" + instruction.name + " : " + instruction.type;
  case InstructionKind::Let:
    if (!instruction.type.empty()) {
      return "let %" + instruction.name + " : " + instruction.type + " = " +
             valueToText(instruction.value);
    }
    return "let %" + instruction.name + " = " + valueToText(instruction.value);
  case InstructionKind::Var:
    if (!instruction.type.empty()) {
      return "var %" + instruction.name + " : " + instruction.type + " = " +
             valueToText(instruction.value);
    }
    return "var %" + instruction.name + " = " + valueToText(instruction.value);
  case InstructionKind::Eval:
    return "eval " + valueToText(instruction.value);
  case InstructionKind::Return:
    return "ret " + instruction.type + " " + valueToText(instruction.value);
  case InstructionKind::Throw:
    return "throw " + valueToText(instruction.value);
  case InstructionKind::Unreachable:
    return "unreachable";
  }
  return "unknown";
}

std::vector<std::string> bodyToText(const FunctionBody& body) {
  std::vector<std::string> lines;
  if (body.entry.empty()) {
    lines.push_back("entry:");
  } else {
    lines.push_back(body.entry + ":");
  }
  for (const Instruction& instruction : body.instructions) {
    lines.push_back(instructionToText(instruction));
  }
  return lines;
}

std::vector<std::string> metadataParentNames(const std::string& signature) {
  std::vector<std::string> parents;
  std::string_view remaining(signature);
  constexpr std::string_view Separator = " with ";
  while (!remaining.empty()) {
    const std::size_t separator = remaining.find(Separator);
    std::string_view parent = separator == std::string_view::npos
                                  ? remaining
                                  : remaining.substr(0, separator);
    while (!parent.empty() && parent.front() == ' ') {
      parent.remove_prefix(1);
    }
    while (!parent.empty() && parent.back() == ' ') {
      parent.remove_suffix(1);
    }
    if (!parent.empty() && parent.front() == '@') {
      parent.remove_prefix(1);
    }
    if (!parent.empty()) {
      parents.emplace_back(parent);
    }
    if (separator == std::string_view::npos) {
      break;
    }
    remaining.remove_prefix(separator + Separator.size());
  }
  return parents;
}

LinearizationResult checkedLinearizedParentNames(
    const std::vector<std::string>& directParents,
    const std::unordered_map<std::string, std::vector<std::string>>& parentMap) {
  using Sequence = std::vector<std::string>;
  LinearizationResult checked;
  std::unordered_map<std::string, Sequence> cache;
  std::unordered_set<std::string> visiting;

  auto merge = [&](std::vector<Sequence> sequences) {
    Sequence result;
    std::unordered_set<std::string> emitted;
    while (true) {
      sequences.erase(
          std::remove_if(sequences.begin(), sequences.end(),
                         [](const Sequence& sequence) { return sequence.empty(); }),
          sequences.end());
      if (sequences.empty()) {
        break;
      }
      std::string candidate;
      for (const Sequence& sequence : sequences) {
        const std::string& head = sequence.front();
        bool appearsInTail = false;
        for (const Sequence& other : sequences) {
          if (std::find(std::next(other.begin()), other.end(), head) != other.end()) {
            appearsInTail = true;
            break;
          }
        }
        if (!appearsInTail) {
          candidate = head;
          break;
        }
      }
      if (candidate.empty()) {
        checked.consistent = false;
        return Sequence{};
      }
      if (emitted.insert(candidate).second) {
        result.push_back(candidate);
      }
      for (Sequence& sequence : sequences) {
        if (!sequence.empty() && sequence.front() == candidate) {
          sequence.erase(sequence.begin());
        }
      }
    }
    return result;
  };

  std::function<Sequence(const std::string&)> linearize =
      [&](const std::string& typeName) -> Sequence {
    if (auto cached = cache.find(typeName); cached != cache.end()) {
      return cached->second;
    }
    if (!visiting.insert(typeName).second) {
      checked.cyclic = true;
      return {};
    }
    std::vector<std::string> parents;
    if (auto found = parentMap.find(typeName); found != parentMap.end()) {
      parents = found->second;
    }
    Sequence priority(parents.rbegin(), parents.rend());
    std::vector<Sequence> sequences;
    for (const std::string& parent : priority) {
      sequences.push_back(linearize(parent));
    }
    if (!priority.empty()) {
      sequences.push_back(priority);
    }
    Sequence result{typeName};
    Sequence ancestors = merge(std::move(sequences));
    result.insert(result.end(), ancestors.begin(), ancestors.end());
    visiting.erase(typeName);
    cache[typeName] = result;
    return result;
  };

  Sequence priority(directParents.rbegin(), directParents.rend());
  std::vector<Sequence> sequences;
  for (const std::string& parent : priority) {
    sequences.push_back(linearize(parent));
  }
  if (!priority.empty()) {
    sequences.push_back(priority);
  }
  checked.names = merge(std::move(sequences));
  return checked;
}

std::vector<std::string> linearizedParentNames(
    const std::vector<std::string>& directParents,
    const std::unordered_map<std::string, std::vector<std::string>>& parentMap) {
  return checkedLinearizedParentNames(directParents, parentMap).names;
}

std::vector<std::string> linearizedTypeNames(
    const std::string& typeName,
    const std::unordered_map<std::string, std::vector<std::string>>& parentMap) {
  std::vector<std::string> result{typeName};
  auto parents = parentMap.find(typeName);
  if (parents == parentMap.end()) {
    return result;
  }
  std::vector<std::string> ancestors =
      linearizedParentNames(parents->second, parentMap);
  result.insert(result.end(), ancestors.begin(), ancestors.end());
  return result;
}

bool isStackSuper(const Value& value) {
  return value.kind == ValueKind::Super && value.text.rfind("stack-super[", 0) == 0;
}

std::string toText(const Module& module) {
  std::ostringstream out;
  out << "# nir module " << module.name << '\n';
  for (const Definition& definition : module.definitions) {
    out << definitionKindName(definition.kind) << " @" << definition.name;
    if (!definition.signature.empty()) {
      out << " : " << definition.signature;
    }
    out << '\n';
    if (!definition.body.empty()) {
      for (const std::string& instruction : bodyToText(definition.body)) {
        out << "  " << instruction << '\n';
      }
    }
  }
  return out.str();
}

} // namespace scalanative::nir
