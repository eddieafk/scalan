#include "scalanative/support/Diagnostics.h"
#include "scalanative/tools/build/BuildDriver.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

int fail(const std::string& message) {
  std::cerr << message << '\n';
  return 1;
}

bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::size_t countOccurrences(std::string_view haystack, std::string_view needle) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while ((offset = haystack.find(needle, offset)) != std::string_view::npos) {
    ++count;
    offset += needle.size();
  }
  return count;
}

std::string readTextFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

std::string_view functionText(std::string_view nir, std::string_view name) {
  const std::string marker = "define @" + std::string(name);
  const std::size_t start = nir.find(marker);
  if (start == std::string_view::npos) {
    return {};
  }
  const std::size_t end = nir.find("\ndefine @", start + marker.size());
  return nir.substr(start,
                    end == std::string_view::npos ? nir.size() - start : end - start);
}

int smokeInlineSummonFrom() {
  constexpr const char* source = R"(package demo.inlinecalls

trait Named[A] {
  def label(): String
}

class NamedValue[A](val value: String) extends Named[A] {
  def label(): String = value
}

object Selectors {
  val prefix: String = "selected:"

  inline def selected[A]: String =
    summonFrom {
      case found: Named[A] => prefix + found.label()
      case _ => "fallback"
    }

  inline def nested[A]: String = "nested:" + selected[A]

  inline def decorated[A](value: String): String =
    summonFrom {
      case found: Named[A] =>
        prefix + found.label() + ":" + value + ":" + value
      case _ => "fallback:" + value + ":" + value
    }

  inline def nestedValue[A](value: String): String =
    "nested-value:" + decorated[A](value)

  inline def passed[A](value: A): A = value

  inline def inferredSelected[A](value: A): String =
    summonFrom {
      case found: Named[A] => prefix + found.label()
      case _ => "inferred-fallback"
    }

  inline def inferredNull[A](): A = null

  inline def contextual[A]()(using named: Named[A]): String =
    summonFrom {
      case found: Named[A] => prefix + found.label()
      case _ => "contextual-fallback"
    }

  inline def contextualValue[A](value: String)(using named: Named[A]): String =
    prefix + named.label() + ":" + named.label() + ":" + value + ":" + value

  inline def nestedContextual[A](value: String)(using named: Named[A]): String =
    "nested-context:" + contextualValue[A](value)

  inline def inferredContextual[A](value: A)(using named: Named[A]): String =
    prefix + named.label()

  inline def inferredFromContext[A]()(using named: Named[A]): String =
    prefix + named.label()
}

class InstanceSelectors(val instancePrefix: String) {
  inline def selected[A](): String =
    summonFrom {
      case found: Named[A] => instancePrefix + found.label()
      case _ => instancePrefix + "fallback"
    }

  inline def contextualValue[A](value: String)(using named: Named[A]): String =
    instancePrefix + named.label() + ":" + named.label() + ":" + value + ":" + value

  inline def nestedContextual[A](value: String)(using named: Named[A]): String =
    "instance-nested:" + contextualValue[A](value)

  inline def inferred[A](value: A): String = instancePrefix + "inferred"

  inline def thisPrefix[A](): String = this.instancePrefix
}

trait TraitInstanceSelectors {
  def traitPrefix(): String

  inline def traitSelected[A](): String =
    summonFrom {
      case found: Named[A] => traitPrefix() + found.label()
      case _ => traitPrefix() + "fallback"
    }
}

class TraitInstanceSelectorsValue(val value: String) extends TraitInstanceSelectors {
  def traitPrefix(): String = value
}

object Main {
  given intNamed: Named[Int] = new NamedValue[Int]("member-int")
  val instances: InstanceSelectors = new InstanceSelectors("instance:")
  val traitInstances: TraitInstanceSelectors =
    new TraitInstanceSelectorsValue("trait-instance:")

  def nextValue(): String = {
    println("effect")
    "value"
  }

  def nextContextValue(): String = {
    println("context-effect")
    "context-value"
  }

  def nextInstanceValue(): String = {
    println("instance-effect")
    "instance-value"
  }

  def intSelected: String = Selectors.selected[Int]

  def localSelected: String = {
    given localString: Named[String] =
      new NamedValue[String]("local-string")
    Selectors.selected[String]()
  }

  def fallbackSelected: String = Selectors.selected[Boolean]
  def nestedSelected: String = Selectors.nested[Int]
  def valueSelected: String = Selectors.decorated[Int](nextValue())
  def valueFallback: String = Selectors.decorated[Boolean]("bool")
  def nestedValueSelected: String = Selectors.nestedValue[Int]("nested")
  def passedInt: Int = Selectors.passed[Int](42)
  def passedString: String = Selectors.passed[String]("forty-two")
  def inferredInt: String = Selectors.inferredSelected(7)
  def inferredFallback: String = Selectors.inferredSelected(true)
  def inferredLocal: String = {
    given inferredString: Named[String] =
      new NamedValue[String]("inferred-local")
    Selectors.inferredSelected("text")
  }
  def inferredPassedInt: Int = Selectors.passed(43)
  def inferredPassedString: String = Selectors.passed("forty-three")
  def inferredNullString: String = Selectors.inferredNull()
  def contextualSelected: String = Selectors.contextual[Int]()
  def contextualLocal: String = {
    given contextualString: Named[String] =
      new NamedValue[String]("contextual-local")
    Selectors.contextual[String]()
  }
  def contextualValueSelected: String =
    Selectors.contextualValue[Int](nextContextValue())
  def explicitContextual: String =
    Selectors.contextual[String]()(using
      new NamedValue[String]("explicit-context"))
  def nestedContextualSelected: String =
    Selectors.nestedContextual[Int]("nested")
  def inferredContextualSelected: String = Selectors.inferredContextual(9)
  def inferredContextOnly: String = Selectors.inferredFromContext()
  def instanceSelected: String = instances.selected[Int]()
  def instanceFallback: String = instances.selected[Boolean]()
  def instanceContextualValue: String =
    instances.contextualValue[Int](nextInstanceValue())
  def instanceExplicitContext: String =
    instances.contextualValue[String]("explicit")(using
      new NamedValue[String]("explicit-instance"))
  def instanceNestedContextual: String =
    instances.nestedContextual[Int]("nested")
  def instanceInferred: String = instances.inferred(10)
  def instanceThisPrefix: String = instances.thisPrefix[Int]()
  def localInstanceSelected: String = {
    val localInstances: InstanceSelectors =
      new InstanceSelectors("local-instance:")
    localInstances.selected[Int]()
  }
  def traitInstanceSelected: String = traitInstances.traitSelected[Int]()

  def main(args: Array[String]): Unit = {
    println(intSelected)
    println(localSelected)
    println(fallbackSelected)
    println(nestedSelected)
    println(valueSelected)
    println(valueFallback)
    println(nestedValueSelected)
    println(passedInt)
    println(passedString)
    println(inferredInt)
    println(inferredFallback)
    println(inferredLocal)
    println(inferredPassedInt)
    println(inferredPassedString)
    println(inferredNullString == null)
    println(contextualSelected)
    println(contextualLocal)
    println(contextualValueSelected)
    println(explicitContextual)
    println(nestedContextualSelected)
    println(inferredContextualSelected)
    println(inferredContextOnly)
    println(instanceSelected)
    println(instanceFallback)
    println(instanceContextualValue)
    println(instanceExplicitContext)
    println(instanceNestedContextual)
    println(instanceInferred)
    println(instanceThisPrefix)
    println(localInstanceSelected)
    println(traitInstanceSelected)
  }
}
)";
  constexpr const char* invalidSource =
      R"(package demo.invalidinlinecalls

object Main {
  trait Missing[A]

  inline def nonGeneric: String = "non-generic"
  inline def missingContext[A]()(using value: Missing[A]): String =
    "missing-context"
  def unsupportedMissingContext: String = missingContext[Int]()
  inline def noInference[A](value: String): String = value
  def unsupportedInference: String = noInference("value")
  def unsupportedReceiver: String =
    new EffectfulInstance().value[Int]()
  inline def missingBody[A]: String
  inline def recursive[A]: String = recursive[A]
  inline val unsupported: String = "value"
}

class GenericInstance[T] {
  inline def unsupportedInstance[A]: String = "instance"
}

class EffectfulInstance {
  inline def value[A](): String = "instance"
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "cpp-scalanative-smoke-inline-summon-from";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-inline-summon-from.out";
  std::error_code ignored;
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildOptions options;
  options.action = scalanative::tools::build::BuildAction::BuildBinary;
  options.optimize = true;
  options.outputPath = binary;
  scalanative::support::DiagnosticEngine diagnostics;
  const scalanative::tools::build::BuildResult result =
      driver.buildSource("InlineSummonFrom.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidInlineSummonFrom.scala", invalidSource, {}, invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("inline summonFrom native build failed: " + result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string text = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  const std::string_view intSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.intSelected");
  const std::string_view localSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.localSelected");
  const std::string_view fallbackSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.fallbackSelected");
  const std::string_view nestedSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.nestedSelected");
  const std::string_view valueSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.valueSelected");
  const std::string_view valueFallback =
      functionText(result.nirText, "demo.inlinecalls.Main.valueFallback");
  const std::string_view nestedValueSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.nestedValueSelected");
  const std::string_view passedInt =
      functionText(result.nirText, "demo.inlinecalls.Main.passedInt");
  const std::string_view passedString =
      functionText(result.nirText, "demo.inlinecalls.Main.passedString");
  const std::string_view inferredInt =
      functionText(result.nirText, "demo.inlinecalls.Main.inferredInt");
  const std::string_view inferredFallback =
      functionText(result.nirText, "demo.inlinecalls.Main.inferredFallback");
  const std::string_view inferredLocal =
      functionText(result.nirText, "demo.inlinecalls.Main.inferredLocal");
  const std::string_view inferredPassedInt =
      functionText(result.nirText, "demo.inlinecalls.Main.inferredPassedInt");
  const std::string_view inferredPassedString =
      functionText(result.nirText, "demo.inlinecalls.Main.inferredPassedString");
  const std::string_view inferredNullString =
      functionText(result.nirText, "demo.inlinecalls.Main.inferredNullString");
  const std::string_view contextualSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.contextualSelected");
  const std::string_view contextualLocal =
      functionText(result.nirText, "demo.inlinecalls.Main.contextualLocal");
  const std::string_view contextualValueSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.contextualValueSelected");
  const std::string_view explicitContextual =
      functionText(result.nirText, "demo.inlinecalls.Main.explicitContextual");
  const std::string_view nestedContextualSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.nestedContextualSelected");
  const std::string_view inferredContextualSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.inferredContextualSelected");
  const std::string_view inferredContextOnly =
      functionText(result.nirText, "demo.inlinecalls.Main.inferredContextOnly");
  const std::string_view instanceSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.instanceSelected");
  const std::string_view instanceFallback =
      functionText(result.nirText, "demo.inlinecalls.Main.instanceFallback");
  const std::string_view instanceContextualValue =
      functionText(result.nirText, "demo.inlinecalls.Main.instanceContextualValue");
  const std::string_view instanceExplicitContext =
      functionText(result.nirText, "demo.inlinecalls.Main.instanceExplicitContext");
  const std::string_view instanceNestedContextual =
      functionText(result.nirText, "demo.inlinecalls.Main.instanceNestedContextual");
  const std::string_view instanceInferred =
      functionText(result.nirText, "demo.inlinecalls.Main.instanceInferred");
  const std::string_view instanceThisPrefix =
      functionText(result.nirText, "demo.inlinecalls.Main.instanceThisPrefix");
  const std::string_view localInstanceSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.localInstanceSelected");
  const std::string_view traitInstanceSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.traitInstanceSelected");

  const bool valid =
      status == 0 &&
      text == "selected:member-int\nselected:local-string\nfallback\n"
              "nested:selected:member-int\n"
              "effect\nselected:member-int:value:value\n"
              "fallback:bool:bool\n"
              "nested-value:selected:member-int:nested:nested\n"
              "42\nforty-two\n"
              "selected:member-int\ninferred-fallback\n"
              "selected:inferred-local\n43\nforty-three\ntrue\n"
              "selected:member-int\nselected:contextual-local\n"
              "context-effect\n"
              "selected:member-int:member-int:context-value:context-value\n"
              "selected:explicit-context\n"
              "nested-context:selected:member-int:member-int:nested:nested\n"
              "selected:member-int\nselected:member-int\n"
              "instance:member-int\ninstance:fallback\n"
              "instance-effect\n"
              "instance:member-int:member-int:instance-value:instance-value\n"
              "instance:explicit-instance:explicit-instance:explicit:explicit\n"
              "instance-nested:instance:member-int:member-int:nested:nested\n"
              "instance:inferred\ninstance:\nlocal-instance:member-int\n"
              "trait-instance:member-int\n" &&
      !invalid.ok &&
      contains(invalid.diagnosticsText,
               "inline call-site specialization currently requires a generic "
               "method") &&
      contains(invalid.diagnosticsText,
               "no given value found for context parameter value of type") &&
      contains(invalid.diagnosticsText, "required by missingContext") &&
      contains(invalid.diagnosticsText,
               "cannot infer type argument A for noInference from value arguments") &&
      contains(invalid.diagnosticsText, "inline method requires an implementation") &&
      contains(invalid.diagnosticsText,
               "recursive inline call-site specialization is not supported yet") &&
      contains(invalid.diagnosticsText,
               "inline instance method specialization does not support generic "
               "owner classes or traits yet") &&
      contains(invalid.diagnosticsText,
               "inline instance method specialization requires a stable identifier "
               "or this receiver") &&
      contains(invalid.diagnosticsText,
               "'inline' must modify a def in this milestone") &&
      contains(intSelected, "call %demo.inlinecalls.Main.intNamed()") &&
      contains(intSelected, "call %demo.inlinecalls.Selectors.prefix()") &&
      !contains(intSelected, "%demo.inlinecalls.Selectors.selected") &&
      contains(localSelected, "local-string") &&
      contains(localSelected, "call %demo.inlinecalls.Selectors.prefix()") &&
      !contains(localSelected, "%demo.inlinecalls.Selectors.selected") &&
      contains(fallbackSelected, "\"fallback\"") &&
      !contains(fallbackSelected, "%demo.inlinecalls.Selectors.prefix") &&
      !contains(fallbackSelected, "%demo.inlinecalls.Selectors.selected") &&
      contains(nestedSelected, "\"nested:\"") &&
      contains(nestedSelected, "call %demo.inlinecalls.Selectors.prefix()") &&
      !contains(nestedSelected, "%demo.inlinecalls.Selectors.nested") &&
      !contains(nestedSelected, "%demo.inlinecalls.Selectors.selected") &&
      contains(valueSelected, "let %value : String") &&
      countOccurrences(valueSelected, "nextValue") == 1 &&
      contains(valueSelected, "call %demo.inlinecalls.Selectors.prefix()") &&
      !contains(valueSelected, "%demo.inlinecalls.Selectors.decorated") &&
      contains(valueFallback, "\"fallback:\"") &&
      !contains(valueFallback, "%demo.inlinecalls.Selectors.prefix") &&
      !contains(valueFallback, "%demo.inlinecalls.Selectors.decorated") &&
      contains(nestedValueSelected, "\"nested-value:\"") &&
      !contains(nestedValueSelected, "%demo.inlinecalls.Selectors.nestedValue") &&
      !contains(nestedValueSelected, "%demo.inlinecalls.Selectors.decorated") &&
      contains(passedInt, "let %value : Int = 42") &&
      !contains(passedInt, "%demo.inlinecalls.Selectors.passed") &&
      contains(passedString, "let %value : String = \"forty-two\"") &&
      !contains(passedString, "%demo.inlinecalls.Selectors.passed") &&
      contains(inferredInt, "call %demo.inlinecalls.Main.intNamed()") &&
      !contains(inferredInt, "%demo.inlinecalls.Selectors.inferredSelected") &&
      contains(inferredFallback, "\"inferred-fallback\"") &&
      !contains(inferredFallback, "%demo.inlinecalls.Selectors.prefix") &&
      !contains(inferredFallback, "%demo.inlinecalls.Selectors.inferredSelected") &&
      contains(inferredLocal, "inferred-local") &&
      !contains(inferredLocal, "%demo.inlinecalls.Selectors.inferredSelected") &&
      contains(inferredPassedInt, "let %value : Int = 43") &&
      !contains(inferredPassedInt, "%demo.inlinecalls.Selectors.passed") &&
      contains(inferredPassedString, "let %value : String = \"forty-three\"") &&
      !contains(inferredPassedString, "%demo.inlinecalls.Selectors.passed") &&
      contains(inferredNullString, "ret String null") &&
      !contains(inferredNullString, "%demo.inlinecalls.Selectors.inferredNull") &&
      contains(contextualSelected, "let %named : demo.inlinecalls.Named") &&
      countOccurrences(contextualSelected, "Main.intNamed") == 1 &&
      !contains(contextualSelected, "%demo.inlinecalls.Selectors.contextual") &&
      contains(contextualLocal, "contextual-local") &&
      !contains(contextualLocal, "%demo.inlinecalls.Selectors.contextual") &&
      contains(contextualValueSelected, "let %value : String") &&
      contains(contextualValueSelected, "let %named : demo.inlinecalls.Named") &&
      countOccurrences(contextualValueSelected, "nextContextValue") == 1 &&
      countOccurrences(contextualValueSelected, "Main.intNamed") == 1 &&
      !contains(contextualValueSelected,
                "%demo.inlinecalls.Selectors.contextualValue") &&
      contains(explicitContextual, "explicit-context") &&
      !contains(explicitContextual, "Main.intNamed") &&
      !contains(explicitContextual, "%demo.inlinecalls.Selectors.contextual") &&
      contains(nestedContextualSelected, "\"nested-context:\"") &&
      countOccurrences(nestedContextualSelected, "Main.intNamed") == 1 &&
      !contains(nestedContextualSelected,
                "%demo.inlinecalls.Selectors.nestedContextual") &&
      !contains(nestedContextualSelected,
                "%demo.inlinecalls.Selectors.contextualValue") &&
      countOccurrences(inferredContextualSelected, "Main.intNamed") == 1 &&
      !contains(inferredContextualSelected,
                "%demo.inlinecalls.Selectors.inferredContextual") &&
      countOccurrences(inferredContextOnly, "Main.intNamed") == 1 &&
      !contains(inferredContextOnly,
                "%demo.inlinecalls.Selectors.inferredFromContext") &&
      contains(instanceSelected, "let %this : demo.inlinecalls.InstanceSelectors") &&
      contains(instanceSelected, ".instancePrefix") &&
      countOccurrences(instanceSelected, "Main.instances") == 1 &&
      countOccurrences(instanceSelected, "Main.intNamed") == 1 &&
      !contains(instanceSelected, "%demo.inlinecalls.InstanceSelectors.selected") &&
      contains(instanceFallback, "\"fallback\"") &&
      countOccurrences(instanceFallback, "Main.instances") == 1 &&
      !contains(instanceFallback, "Main.intNamed") &&
      !contains(instanceFallback, "%demo.inlinecalls.InstanceSelectors.selected") &&
      contains(instanceContextualValue, "let %value : String") &&
      contains(instanceContextualValue, "let %named : demo.inlinecalls.Named") &&
      countOccurrences(instanceContextualValue, "Main.instances") == 1 &&
      countOccurrences(instanceContextualValue, "nextInstanceValue") == 1 &&
      countOccurrences(instanceContextualValue, "Main.intNamed") == 1 &&
      !contains(instanceContextualValue,
                "%demo.inlinecalls.InstanceSelectors.contextualValue") &&
      contains(instanceExplicitContext, "explicit-instance") &&
      countOccurrences(instanceExplicitContext, "Main.instances") == 1 &&
      !contains(instanceExplicitContext, "Main.intNamed") &&
      !contains(instanceExplicitContext,
                "%demo.inlinecalls.InstanceSelectors.contextualValue") &&
      contains(instanceNestedContextual, "\"instance-nested:\"") &&
      countOccurrences(instanceNestedContextual, "Main.instances") == 1 &&
      countOccurrences(instanceNestedContextual, "Main.intNamed") == 1 &&
      !contains(instanceNestedContextual,
                "%demo.inlinecalls.InstanceSelectors.nestedContextual") &&
      !contains(instanceNestedContextual,
                "%demo.inlinecalls.InstanceSelectors.contextualValue") &&
      contains(instanceInferred, "\"inferred\"") &&
      countOccurrences(instanceInferred, "Main.instances") == 1 &&
      !contains(instanceInferred, "%demo.inlinecalls.InstanceSelectors.inferred") &&
      contains(instanceThisPrefix, ".instancePrefix") &&
      countOccurrences(instanceThisPrefix, "Main.instances") == 1 &&
      !contains(instanceThisPrefix, "%demo.inlinecalls.InstanceSelectors.thisPrefix") &&
      contains(localInstanceSelected, "local-instance:") &&
      countOccurrences(localInstanceSelected, "Main.intNamed") == 1 &&
      !contains(localInstanceSelected,
                "%demo.inlinecalls.InstanceSelectors.selected") &&
      contains(traitInstanceSelected,
               "let %this : demo.inlinecalls.TraitInstanceSelectors") &&
      countOccurrences(traitInstanceSelected, "Main.traitInstances") == 1 &&
      countOccurrences(traitInstanceSelected, "Main.intNamed") == 1 &&
      !contains(traitInstanceSelected,
                "%demo.inlinecalls.TraitInstanceSelectors.traitSelected");
  return valid
             ? 0
             : fail("inline summonFrom smoke test failed (output='" + text +
                    "', diagnostics='" + result.diagnosticsText +
                    "', invalid-diagnostics='" + invalid.diagnosticsText +
                    "', int-selected='" + std::string(intSelected) +
                    "', local-selected='" + std::string(localSelected) +
                    "', fallback-selected='" + std::string(fallbackSelected) +
                    "', nested-selected='" + std::string(nestedSelected) +
                    "', value-selected='" + std::string(valueSelected) +
                    "', value-fallback='" + std::string(valueFallback) +
                    "', nested-value-selected='" + std::string(nestedValueSelected) +
                    "', passed-int='" + std::string(passedInt) + "', passed-string='" +
                    std::string(passedString) + "', inferred-int='" +
                    std::string(inferredInt) + "', inferred-fallback='" +
                    std::string(inferredFallback) + "', inferred-local='" +
                    std::string(inferredLocal) + "', inferred-passed-int='" +
                    std::string(inferredPassedInt) + "', inferred-passed-string='" +
                    std::string(inferredPassedString) + "', inferred-null-string='" +
                    std::string(inferredNullString) + "', contextual-selected='" +
                    std::string(contextualSelected) + "', contextual-local='" +
                    std::string(contextualLocal) + "', contextual-value-selected='" +
                    std::string(contextualValueSelected) + "', explicit-contextual='" +
                    std::string(explicitContextual) + "', nested-contextual='" +
                    std::string(nestedContextualSelected) + "', inferred-contextual='" +
                    std::string(inferredContextualSelected) +
                    "', inferred-context-only='" + std::string(inferredContextOnly) +
                    "', instance-selected='" + std::string(instanceSelected) +
                    "', instance-fallback='" + std::string(instanceFallback) +
                    "', instance-contextual-value='" +
                    std::string(instanceContextualValue) +
                    "', instance-explicit-context='" +
                    std::string(instanceExplicitContext) +
                    "', instance-nested-contextual='" +
                    std::string(instanceNestedContextual) + "', instance-inferred='" +
                    std::string(instanceInferred) + "', instance-this-prefix='" +
                    std::string(instanceThisPrefix) + "', local-instance-selected='" +
                    std::string(localInstanceSelected) +
                    "', trait-instance-selected='" +
                    std::string(traitInstanceSelected) + "')");
}

} // namespace

int runSmokeTests5() {
  return smokeInlineSummonFrom();
}
