#pragma once

#include "scalanative/support/SourceSpan.h"

#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace scalanative::support {

class SourceManager;

enum class Severity { Note, Warning, Error };

struct FixIt {
  SourceSpan span;
  std::string replacement;
};

struct Diagnostic {
  Severity severity = Severity::Error;
  SourceSpan span;
  std::string message;
  std::vector<FixIt> fixIts;
};

class DiagnosticEngine {
public:
  void report(Severity severity, SourceSpan span, std::string message);
  void report(Severity severity, SourceSpan span, std::string message,
              std::vector<FixIt> fixIts);
  void note(SourceSpan span, std::string message);
  void warning(SourceSpan span, std::string message);
  void error(SourceSpan span, std::string message);
  void error(SourceSpan span, std::string message, std::vector<FixIt> fixIts);

  [[nodiscard]] bool hasErrors() const;
  [[nodiscard]] std::size_t errorCount() const;
  [[nodiscard]] std::size_t warningCount() const;
  [[nodiscard]] bool empty() const { return diagnostics_.empty(); }
  [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const {
    return diagnostics_;
  }

  void clear();
  void render(const SourceManager& sources, std::ostream& out) const;

private:
  std::vector<Diagnostic> diagnostics_;
};

[[nodiscard]] const char* severityName(Severity severity);

} // namespace scalanative::support
