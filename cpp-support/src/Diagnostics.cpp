#include "scalanative/support/Diagnostics.h"

#include "scalanative/support/SourceManager.h"

#include <algorithm>

namespace scalanative::support {

void DiagnosticEngine::report(Severity severity, SourceSpan span,
                              std::string message) {
  report(severity, span, std::move(message), {});
}

void DiagnosticEngine::report(Severity severity, SourceSpan span,
                              std::string message, std::vector<FixIt> fixIts) {
  diagnostics_.push_back(
      Diagnostic{severity, span, std::move(message), std::move(fixIts)});
}

void DiagnosticEngine::note(SourceSpan span, std::string message) {
  report(Severity::Note, span, std::move(message));
}

void DiagnosticEngine::warning(SourceSpan span, std::string message) {
  report(Severity::Warning, span, std::move(message));
}

void DiagnosticEngine::error(SourceSpan span, std::string message) {
  report(Severity::Error, span, std::move(message));
}

void DiagnosticEngine::error(SourceSpan span, std::string message,
                             std::vector<FixIt> fixIts) {
  report(Severity::Error, span, std::move(message), std::move(fixIts));
}

bool DiagnosticEngine::hasErrors() const {
  return errorCount() != 0;
}

std::size_t DiagnosticEngine::errorCount() const {
  return static_cast<std::size_t>(
      std::count_if(diagnostics_.begin(), diagnostics_.end(),
                    [](const Diagnostic& diagnostic) {
                      return diagnostic.severity == Severity::Error;
                    }));
}

std::size_t DiagnosticEngine::warningCount() const {
  return static_cast<std::size_t>(
      std::count_if(diagnostics_.begin(), diagnostics_.end(),
                    [](const Diagnostic& diagnostic) {
                      return diagnostic.severity == Severity::Warning;
                    }));
}

void DiagnosticEngine::clear() { diagnostics_.clear(); }

void DiagnosticEngine::render(const SourceManager& sources,
                              std::ostream& out) const {
  for (const Diagnostic& diagnostic : diagnostics_) {
    if (!diagnostic.span.isValid()) {
      out << severityName(diagnostic.severity) << ": " << diagnostic.message
          << '\n';
      for (const FixIt& fixIt : diagnostic.fixIts) {
        out << "fix-it: replace with \"" << fixIt.replacement << "\"\n";
      }
      continue;
    }

    const SourceFile* file = sources.get(diagnostic.span.source);
    auto [line, column] = sources.lineColumn(diagnostic.span);
    out << (file != nullptr ? file->path : "<unknown>") << ':' << line << ':'
        << column << ": " << severityName(diagnostic.severity) << ": "
        << diagnostic.message << '\n';

    std::string text = sources.lineText(diagnostic.span.source, line);
    if (!text.empty()) {
      out << text << '\n';
      for (std::size_t i = 1; i < column; ++i) {
        out << ' ';
      }
      out << '^';
      const std::size_t underline =
          std::max<std::size_t>(diagnostic.span.length, 1);
      for (std::size_t i = 1; i < underline; ++i) {
        out << '~';
      }
      out << '\n';
    }

    for (const FixIt& fixIt : diagnostic.fixIts) {
      auto [fixLine, fixColumn] = sources.lineColumn(fixIt.span);
      out << "fix-it: " << fixLine << ':' << fixColumn << ": replace with \""
          << fixIt.replacement << "\"\n";
    }
  }
}

const char* severityName(Severity severity) {
  switch (severity) {
  case Severity::Note:
    return "note";
  case Severity::Warning:
    return "warning";
  case Severity::Error:
    return "error";
  }
  return "error";
}

} // namespace scalanative::support
