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

trait TransparentResult {
  def text(): String
}

class PreciseResult(val value: String) extends TransparentResult {
  def text(): String = value
  def preciseOnly(): String = value
}

class FallbackResult(val value: String) extends TransparentResult {
  def text(): String = value
  def fallbackOnly(): String = value
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

  inline def curried[A](first: String)(second: String): String =
    summonFrom {
      case found: Named[A] =>
        prefix + found.label() + ":" + first + ":" + first + ":" + second + ":" + second
      case _ => "curried-fallback"
    }

  inline def nestedCurried[A](first: String)(second: String): String =
    "nested-curried:" + curried[A](first)(second)

  inline def curriedContextual[A](first: String)(second: String)(
      using named: Named[A]): String =
    prefix + named.label() + ":" + first + ":" + second

  inline def inferredCurried[A](value: A)(suffix: String)(
      using named: Named[A]): String =
    prefix + named.label() + ":" + suffix

  transparent inline def refined[A](): TransparentResult =
    summonFrom {
      case found: Named[A] =>
        new PreciseResult("transparent:" + found.label())
      case _ => new FallbackResult("transparent-fallback")
    }

  transparent inline def nestedRefined[A](): TransparentResult =
    refined[A]()

  transparent inline def contextualRefined[A]()(using
      named: Named[A]): TransparentResult =
    new PreciseResult("context-transparent:" + named.label())

  transparent inline def inferredRefined[A](value: A)(using
      named: Named[A]): TransparentResult =
    new PreciseResult("inferred-transparent:" + named.label())

  inline def nonGenericConstant: String = "non-generic-constant"

  inline def nonGenericDecorated(value: String): String =
    "non-generic:" + value + ":" + value

  inline def nestedNonGeneric(value: String): String =
    "nested-" + nonGenericDecorated(value)

  inline def nonGenericContextual()(using named: Named[Int]): String =
    "non-generic-context:" + named.label()

  inline def nonGenericCurried(first: String)(second: String): String =
    "non-generic-curried:" + first + ":" + second

  transparent inline def nonGenericRefined(): TransparentResult =
    new PreciseResult("non-generic-transparent")
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

  inline def curried[A](first: String)(second: String)(
      using named: Named[A]): String =
    instancePrefix + named.label() + ":" + first + ":" + second

  inline def nonGeneric(value: String): String =
    instancePrefix + value + ":" + value

  inline def nonGenericConstant: String = instancePrefix + "constant"
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

class GenericInstanceSelectors[Owner](val ownerPrefix: String) {
  inline def selected[A](): String =
    summonFrom {
      case ownerNamed: Named[Owner] =>
        summonFrom {
          case methodNamed: Named[A] =>
            ownerPrefix + ownerNamed.label() + ":" + methodNamed.label()
          case _ => ownerPrefix + "method-fallback"
        }
      case _ => ownerPrefix + "owner-fallback"
    }

  inline def contextual[A](value: A)(
      using ownerNamed: Named[Owner], methodNamed: Named[A]): String =
    ownerPrefix + ownerNamed.label() + ":" + methodNamed.label()

  inline def ownerPassed[A](ownerValue: Owner, methodValue: A): Owner =
    ownerValue
}

trait GenericTraitInstanceSelectors[Owner] {
  def genericTraitPrefix(): String

  inline def selected[A](): String =
    summonFrom {
      case ownerNamed: Named[Owner] =>
        summonFrom {
          case methodNamed: Named[A] =>
            genericTraitPrefix() + ownerNamed.label() + ":" + methodNamed.label()
          case _ => genericTraitPrefix() + "method-fallback"
        }
      case _ => genericTraitPrefix() + "owner-fallback"
    }
}

class GenericTraitInstanceSelectorsValue(val value: String)
    extends GenericTraitInstanceSelectors[String] {
  def genericTraitPrefix(): String = value
}

class InstanceHolder(val value: InstanceSelectors)

object Main {
  given intNamed: Named[Int] = new NamedValue[Int]("member-int")
  val instances: InstanceSelectors = new InstanceSelectors("instance:")
  val traitInstances: TraitInstanceSelectors =
    new TraitInstanceSelectorsValue("trait-instance:")
  val genericInstances: GenericInstanceSelectors[String] =
    new GenericInstanceSelectors[String]("generic:")
  val genericTraitInstances: GenericTraitInstanceSelectors[String] =
    new GenericTraitInstanceSelectorsValue("generic-trait:")
  val inheritedGenericTraitInstances: GenericTraitInstanceSelectorsValue =
    new GenericTraitInstanceSelectorsValue("inherited-trait:")
  val instanceHolder: InstanceHolder =
    new InstanceHolder(new InstanceSelectors("held-instance:"))

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

  def nextInstances(): InstanceSelectors = {
    println("receiver-effect")
    new InstanceSelectors("effectful-instance:")
  }

  def nextTraitInstances(): TraitInstanceSelectors = {
    println("trait-receiver-effect")
    new TraitInstanceSelectorsValue("effectful-trait:")
  }

  def nextFirst(): String = {
    println("first-effect")
    "first"
  }

  def nextSecond(): String = {
    println("second-effect")
    "second"
  }

  def nextNonGeneric(): String = {
    println("non-generic-effect")
    "plain"
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
  def genericInstanceSelected: String = {
    given ownerString: Named[String] =
      new NamedValue[String]("owner-string")
    genericInstances.selected[Int]()
  }
  def genericInstanceContextual: String = {
    given ownerString: Named[String] =
      new NamedValue[String]("owner-string")
    genericInstances.contextual(11)
  }
  def genericOwnerPassed: String =
    genericInstances.ownerPassed("owner-value", 12)
  def genericTraitInstanceSelected: String = {
    given traitOwnerString: Named[String] =
      new NamedValue[String]("trait-owner")
    genericTraitInstances.selected[Int]()
  }
  def inheritedGenericTraitSelected: String = {
    given inheritedOwnerString: Named[String] =
      new NamedValue[String]("inherited-owner")
    inheritedGenericTraitInstances.selected[Int]()
  }
  def effectfulReceiverSelected: String =
    nextInstances().selected[Int]()
  def effectfulReceiverContextual: String =
    nextInstances().contextualValue[Int](nextInstanceValue())
  def constructedReceiverSelected: String =
    new InstanceSelectors("constructed-instance:").selected[Int]()
  def selectedReceiverSelected: String =
    instanceHolder.value.selected[Int]()
  def effectfulTraitReceiverSelected: String =
    nextTraitInstances().traitSelected[Int]()
  def curriedSelected: String =
    Selectors.curried[Int](nextFirst())(nextSecond())
  def nestedCurriedSelected: String =
    Selectors.nestedCurried[Int]("left")("right")
  def curriedContextualSelected: String =
    Selectors.curriedContextual[Int]("context-first")("context-second")
  def explicitCurriedContextual: String =
    Selectors.curriedContextual[String]("explicit-first")("explicit-second")(using
      new NamedValue[String]("explicit-curried"))
  def inferredCurriedSelected: String =
    Selectors.inferredCurried(21)("inferred-second")
  def effectfulReceiverCurried: String =
    nextInstances().curried[Int](nextFirst())(nextSecond())
  def transparentSelected: String =
    Selectors.refined[Int]().preciseOnly()
  def transparentFallback: String =
    Selectors.refined[Boolean]().fallbackOnly()
  def nestedTransparentSelected: String =
    Selectors.nestedRefined[Int]().preciseOnly()
  def contextualTransparentSelected: String =
    Selectors.contextualRefined[Int]().preciseOnly()
  def explicitTransparentSelected: String =
    Selectors.contextualRefined[String]()(using
      new NamedValue[String]("explicit-transparent")).preciseOnly()
  def inferredTransparentSelected: String =
    Selectors.inferredRefined(22).preciseOnly()
  def nonGenericConstant: String = Selectors.nonGenericConstant
  def nonGenericDecorated: String =
    Selectors.nonGenericDecorated(nextNonGeneric())
  def nestedNonGeneric: String = Selectors.nestedNonGeneric("nested")
  def nonGenericContextual: String = Selectors.nonGenericContextual()
  def nonGenericCurried: String =
    Selectors.nonGenericCurried(nextFirst())(nextSecond())
  def effectfulReceiverNonGeneric: String =
    nextInstances().nonGeneric("receiver")
  def effectfulReceiverNonGenericConstant: String =
    nextInstances().nonGenericConstant
  def nonGenericTransparent: String =
    Selectors.nonGenericRefined().preciseOnly()

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
    println(genericInstanceSelected)
    println(genericInstanceContextual)
    println(genericOwnerPassed)
    println(genericTraitInstanceSelected)
    println(inheritedGenericTraitSelected)
    println(effectfulReceiverSelected)
    println(effectfulReceiverContextual)
    println(constructedReceiverSelected)
    println(selectedReceiverSelected)
    println(effectfulTraitReceiverSelected)
    println(curriedSelected)
    println(nestedCurriedSelected)
    println(curriedContextualSelected)
    println(explicitCurriedContextual)
    println(inferredCurriedSelected)
    println(effectfulReceiverCurried)
    println(transparentSelected)
    println(transparentFallback)
    println(nestedTransparentSelected)
    println(contextualTransparentSelected)
    println(explicitTransparentSelected)
    println(inferredTransparentSelected)
    println(nonGenericConstant)
    println(nonGenericDecorated)
    println(nestedNonGeneric)
    println(nonGenericContextual)
    println(nonGenericCurried)
    println(effectfulReceiverNonGeneric)
    println(effectfulReceiverNonGenericConstant)
    println(nonGenericTransparent)
  }
}
)";
  constexpr const char* invalidSource =
      R"(package demo.invalidinlinecalls

object Main {
  trait Missing[A]

  inline def missingContext[A]()(using value: Missing[A]): String =
    "missing-context"
  def unsupportedMissingContext: String = missingContext[Int]()
  inline def noInference[A](value: String): String = value
  def unsupportedInference: String = noInference("value")
  inline def curried[A](first: String)(second: String): String = first + second
  def flattenedCurried: String = curried[Int]("first", "second")
  transparent def unsupportedTransparent[A](): String = "transparent"
  inline def missingBody[A]: String
  inline def recursive[A]: String = recursive[A]
  inline val unsupported: String = "value"
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
  const std::string_view genericInstanceSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.genericInstanceSelected");
  const std::string_view genericInstanceContextual =
      functionText(result.nirText, "demo.inlinecalls.Main.genericInstanceContextual");
  const std::string_view genericOwnerPassed =
      functionText(result.nirText, "demo.inlinecalls.Main.genericOwnerPassed");
  const std::string_view genericTraitInstanceSelected = functionText(
      result.nirText, "demo.inlinecalls.Main.genericTraitInstanceSelected");
  const std::string_view inheritedGenericTraitSelected = functionText(
      result.nirText, "demo.inlinecalls.Main.inheritedGenericTraitSelected");
  const std::string_view effectfulReceiverSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.effectfulReceiverSelected");
  const std::string_view effectfulReceiverContextual =
      functionText(result.nirText, "demo.inlinecalls.Main.effectfulReceiverContextual");
  const std::string_view constructedReceiverSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.constructedReceiverSelected");
  const std::string_view selectedReceiverSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.selectedReceiverSelected");
  const std::string_view effectfulTraitReceiverSelected = functionText(
      result.nirText, "demo.inlinecalls.Main.effectfulTraitReceiverSelected");
  const std::string_view curriedSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.curriedSelected");
  const std::string_view nestedCurriedSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.nestedCurriedSelected");
  const std::string_view curriedContextualSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.curriedContextualSelected");
  const std::string_view explicitCurriedContextual =
      functionText(result.nirText, "demo.inlinecalls.Main.explicitCurriedContextual");
  const std::string_view inferredCurriedSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.inferredCurriedSelected");
  const std::string_view effectfulReceiverCurried =
      functionText(result.nirText, "demo.inlinecalls.Main.effectfulReceiverCurried");
  const std::string_view transparentSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.transparentSelected");
  const std::string_view transparentFallback =
      functionText(result.nirText, "demo.inlinecalls.Main.transparentFallback");
  const std::string_view nestedTransparentSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.nestedTransparentSelected");
  const std::string_view contextualTransparentSelected = functionText(
      result.nirText, "demo.inlinecalls.Main.contextualTransparentSelected");
  const std::string_view explicitTransparentSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.explicitTransparentSelected");
  const std::string_view inferredTransparentSelected =
      functionText(result.nirText, "demo.inlinecalls.Main.inferredTransparentSelected");
  const std::string_view nonGenericConstant =
      functionText(result.nirText, "demo.inlinecalls.Main.nonGenericConstant");
  const std::string_view nonGenericDecorated =
      functionText(result.nirText, "demo.inlinecalls.Main.nonGenericDecorated");
  const std::string_view nestedNonGeneric =
      functionText(result.nirText, "demo.inlinecalls.Main.nestedNonGeneric");
  const std::string_view nonGenericContextual =
      functionText(result.nirText, "demo.inlinecalls.Main.nonGenericContextual");
  const std::string_view nonGenericCurried =
      functionText(result.nirText, "demo.inlinecalls.Main.nonGenericCurried");
  const std::string_view effectfulReceiverNonGeneric =
      functionText(result.nirText, "demo.inlinecalls.Main.effectfulReceiverNonGeneric");
  const std::string_view effectfulReceiverNonGenericConstant = functionText(
      result.nirText, "demo.inlinecalls.Main.effectfulReceiverNonGenericConstant");
  const std::string_view nonGenericTransparent =
      functionText(result.nirText, "demo.inlinecalls.Main.nonGenericTransparent");

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
              "trait-instance:member-int\n"
              "generic:owner-string:member-int\n"
              "generic:owner-string:member-int\nowner-value\n"
              "generic-trait:trait-owner:member-int\n"
              "inherited-trait:inherited-owner:member-int\n"
              "receiver-effect\neffectful-instance:member-int\n"
              "receiver-effect\ninstance-effect\n"
              "effectful-instance:member-int:member-int:"
              "instance-value:instance-value\n"
              "constructed-instance:member-int\nheld-instance:member-int\n"
              "trait-receiver-effect\neffectful-trait:member-int\n"
              "first-effect\nsecond-effect\n"
              "selected:member-int:first:first:second:second\n"
              "nested-curried:selected:member-int:left:left:right:right\n"
              "selected:member-int:context-first:context-second\n"
              "selected:explicit-curried:explicit-first:explicit-second\n"
              "selected:member-int:inferred-second\n"
              "receiver-effect\nfirst-effect\nsecond-effect\n"
              "effectful-instance:member-int:first:second\n"
              "transparent:member-int\ntransparent-fallback\n"
              "transparent:member-int\n"
              "context-transparent:member-int\n"
              "context-transparent:explicit-transparent\n"
              "inferred-transparent:member-int\n"
              "non-generic-constant\n"
              "non-generic-effect\nnon-generic:plain:plain\n"
              "nested-non-generic:nested:nested\n"
              "non-generic-context:member-int\n"
              "first-effect\nsecond-effect\n"
              "non-generic-curried:first:second\n"
              "receiver-effect\neffectful-instance:receiver:receiver\n"
              "receiver-effect\neffectful-instance:constant\n"
              "non-generic-transparent\n" &&
      !invalid.ok &&
      contains(invalid.diagnosticsText,
               "no given value found for context parameter value of type") &&
      contains(invalid.diagnosticsText, "required by missingContext") &&
      contains(invalid.diagnosticsText,
               "cannot infer type argument A for noInference from value arguments") &&
      contains(invalid.diagnosticsText,
               "inline call to curried must preserve its declared ordinary "
               "argument clauses") &&
      contains(invalid.diagnosticsText,
               "'transparent' must modify a class, trait, or inline def") &&
      contains(invalid.diagnosticsText, "inline method requires an implementation") &&
      contains(invalid.diagnosticsText,
               "recursive inline call-site specialization is not supported yet") &&
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
                "%demo.inlinecalls.TraitInstanceSelectors.traitSelected") &&
      contains(genericInstanceSelected,
               "let %this : demo.inlinecalls.GenericInstanceSelectors") &&
      contains(genericInstanceSelected, "owner-string") &&
      countOccurrences(genericInstanceSelected, "Main.genericInstances") == 1 &&
      countOccurrences(genericInstanceSelected, "Main.intNamed") == 1 &&
      !contains(genericInstanceSelected,
                "%demo.inlinecalls.GenericInstanceSelectors.selected") &&
      contains(genericInstanceContextual, "let %value : Int = 11") &&
      contains(genericInstanceContextual, "owner-string") &&
      countOccurrences(genericInstanceContextual, "Main.genericInstances") == 1 &&
      countOccurrences(genericInstanceContextual, "Main.intNamed") == 1 &&
      !contains(genericInstanceContextual,
                "%demo.inlinecalls.GenericInstanceSelectors.contextual") &&
      contains(genericOwnerPassed, "let %ownerValue : String = \"owner-value\"") &&
      contains(genericOwnerPassed, "let %methodValue : Int = 12") &&
      countOccurrences(genericOwnerPassed, "Main.genericInstances") == 1 &&
      !contains(genericOwnerPassed,
                "%demo.inlinecalls.GenericInstanceSelectors.ownerPassed") &&
      contains(genericTraitInstanceSelected, "trait-owner") &&
      countOccurrences(genericTraitInstanceSelected, "Main.genericTraitInstances") ==
          1 &&
      countOccurrences(genericTraitInstanceSelected, "Main.intNamed") == 1 &&
      !contains(genericTraitInstanceSelected,
                "%demo.inlinecalls.GenericTraitInstanceSelectors.selected") &&
      contains(inheritedGenericTraitSelected, "inherited-owner") &&
      countOccurrences(inheritedGenericTraitSelected,
                       "Main.inheritedGenericTraitInstances") == 1 &&
      countOccurrences(inheritedGenericTraitSelected, "Main.intNamed") == 1 &&
      !contains(inheritedGenericTraitSelected,
                "%demo.inlinecalls.GenericTraitInstanceSelectors.selected") &&
      contains(effectfulReceiverSelected,
               "let %this : demo.inlinecalls.InstanceSelectors") &&
      countOccurrences(effectfulReceiverSelected, "nextInstances") == 1 &&
      countOccurrences(effectfulReceiverSelected, "Main.intNamed") == 1 &&
      !contains(effectfulReceiverSelected,
                "%demo.inlinecalls.InstanceSelectors.selected") &&
      countOccurrences(effectfulReceiverContextual, "nextInstances") == 1 &&
      countOccurrences(effectfulReceiverContextual, "nextInstanceValue") == 1 &&
      effectfulReceiverContextual.find("nextInstances") <
          effectfulReceiverContextual.find("nextInstanceValue") &&
      countOccurrences(effectfulReceiverContextual, "Main.intNamed") == 1 &&
      !contains(effectfulReceiverContextual,
                "%demo.inlinecalls.InstanceSelectors.contextualValue") &&
      contains(constructedReceiverSelected, "constructed-instance:") &&
      countOccurrences(constructedReceiverSelected, "Main.intNamed") == 1 &&
      !contains(constructedReceiverSelected,
                "%demo.inlinecalls.InstanceSelectors.selected") &&
      countOccurrences(selectedReceiverSelected, "Main.instanceHolder") == 1 &&
      countOccurrences(selectedReceiverSelected, "Main.intNamed") == 1 &&
      !contains(selectedReceiverSelected,
                "%demo.inlinecalls.InstanceSelectors.selected") &&
      countOccurrences(effectfulTraitReceiverSelected, "nextTraitInstances") == 1 &&
      countOccurrences(effectfulTraitReceiverSelected, "Main.intNamed") == 1 &&
      !contains(effectfulTraitReceiverSelected,
                "%demo.inlinecalls.TraitInstanceSelectors.traitSelected") &&
      contains(curriedSelected, "let %first : String") &&
      contains(curriedSelected, "let %second : String") &&
      countOccurrences(curriedSelected, "nextFirst") == 1 &&
      countOccurrences(curriedSelected, "nextSecond") == 1 &&
      curriedSelected.find("nextFirst") < curriedSelected.find("nextSecond") &&
      countOccurrences(curriedSelected, "Main.intNamed") == 1 &&
      !contains(curriedSelected, "%demo.inlinecalls.Selectors.curried") &&
      contains(nestedCurriedSelected, "\"nested-curried:\"") &&
      countOccurrences(nestedCurriedSelected, "Main.intNamed") == 1 &&
      !contains(nestedCurriedSelected, "%demo.inlinecalls.Selectors.nestedCurried") &&
      !contains(nestedCurriedSelected, "%demo.inlinecalls.Selectors.curried") &&
      countOccurrences(curriedContextualSelected, "Main.intNamed") == 1 &&
      !contains(curriedContextualSelected,
                "%demo.inlinecalls.Selectors.curriedContextual") &&
      contains(explicitCurriedContextual, "explicit-curried") &&
      !contains(explicitCurriedContextual, "Main.intNamed") &&
      !contains(explicitCurriedContextual,
                "%demo.inlinecalls.Selectors.curriedContextual") &&
      countOccurrences(inferredCurriedSelected, "Main.intNamed") == 1 &&
      !contains(inferredCurriedSelected,
                "%demo.inlinecalls.Selectors.inferredCurried") &&
      countOccurrences(effectfulReceiverCurried, "nextInstances") == 1 &&
      countOccurrences(effectfulReceiverCurried, "nextFirst") == 1 &&
      countOccurrences(effectfulReceiverCurried, "nextSecond") == 1 &&
      effectfulReceiverCurried.find("nextInstances") <
          effectfulReceiverCurried.find("nextFirst") &&
      effectfulReceiverCurried.find("nextFirst") <
          effectfulReceiverCurried.find("nextSecond") &&
      countOccurrences(effectfulReceiverCurried, "Main.intNamed") == 1 &&
      !contains(effectfulReceiverCurried,
                "%demo.inlinecalls.InstanceSelectors.curried") &&
      contains(transparentSelected, "demo.inlinecalls.PreciseResult") &&
      contains(transparentSelected, ".preciseOnly") &&
      countOccurrences(transparentSelected, "Main.intNamed") == 1 &&
      !contains(transparentSelected, "%demo.inlinecalls.Selectors.refined") &&
      contains(transparentFallback, "demo.inlinecalls.FallbackResult") &&
      contains(transparentFallback, ".fallbackOnly") &&
      !contains(transparentFallback, "Main.intNamed") &&
      !contains(transparentFallback, "%demo.inlinecalls.Selectors.refined") &&
      contains(nestedTransparentSelected, "demo.inlinecalls.PreciseResult") &&
      contains(nestedTransparentSelected, ".preciseOnly") &&
      countOccurrences(nestedTransparentSelected, "Main.intNamed") == 1 &&
      !contains(nestedTransparentSelected,
                "%demo.inlinecalls.Selectors.nestedRefined") &&
      !contains(nestedTransparentSelected, "%demo.inlinecalls.Selectors.refined") &&
      contains(contextualTransparentSelected, "demo.inlinecalls.PreciseResult") &&
      contains(contextualTransparentSelected, ".preciseOnly") &&
      countOccurrences(contextualTransparentSelected, "Main.intNamed") == 1 &&
      !contains(contextualTransparentSelected,
                "%demo.inlinecalls.Selectors.contextualRefined") &&
      contains(explicitTransparentSelected, "explicit-transparent") &&
      contains(explicitTransparentSelected, ".preciseOnly") &&
      !contains(explicitTransparentSelected, "Main.intNamed") &&
      !contains(explicitTransparentSelected,
                "%demo.inlinecalls.Selectors.contextualRefined") &&
      contains(inferredTransparentSelected, "demo.inlinecalls.PreciseResult") &&
      contains(inferredTransparentSelected, ".preciseOnly") &&
      countOccurrences(inferredTransparentSelected, "Main.intNamed") == 1 &&
      !contains(inferredTransparentSelected,
                "%demo.inlinecalls.Selectors.inferredRefined") &&
      contains(nonGenericConstant, "\"non-generic-constant\"") &&
      !contains(nonGenericConstant, "%demo.inlinecalls.Selectors.nonGenericConstant") &&
      contains(nonGenericDecorated, "let %value : String") &&
      countOccurrences(nonGenericDecorated, "nextNonGeneric") == 1 &&
      !contains(nonGenericDecorated,
                "%demo.inlinecalls.Selectors.nonGenericDecorated") &&
      contains(nestedNonGeneric, "\"nested-\"") &&
      !contains(nestedNonGeneric, "%demo.inlinecalls.Selectors.nestedNonGeneric") &&
      !contains(nestedNonGeneric, "%demo.inlinecalls.Selectors.nonGenericDecorated") &&
      countOccurrences(nonGenericContextual, "Main.intNamed") == 1 &&
      !contains(nonGenericContextual,
                "%demo.inlinecalls.Selectors.nonGenericContextual") &&
      countOccurrences(nonGenericCurried, "nextFirst") == 1 &&
      countOccurrences(nonGenericCurried, "nextSecond") == 1 &&
      nonGenericCurried.find("nextFirst") < nonGenericCurried.find("nextSecond") &&
      !contains(nonGenericCurried, "%demo.inlinecalls.Selectors.nonGenericCurried") &&
      countOccurrences(effectfulReceiverNonGeneric, "nextInstances") == 1 &&
      !contains(effectfulReceiverNonGeneric,
                "%demo.inlinecalls.InstanceSelectors.nonGeneric") &&
      countOccurrences(effectfulReceiverNonGenericConstant, "nextInstances") == 1 &&
      !contains(effectfulReceiverNonGenericConstant,
                "%demo.inlinecalls.InstanceSelectors.nonGenericConstant") &&
      contains(nonGenericTransparent, "demo.inlinecalls.PreciseResult") &&
      contains(nonGenericTransparent, ".preciseOnly") &&
      !contains(nonGenericTransparent, "%demo.inlinecalls.Selectors.nonGenericRefined");
  return valid
             ? 0
             : fail(
                   "inline summonFrom smoke test failed (output='" + text +
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
                   std::string(localInstanceSelected) + "', trait-instance-selected='" +
                   std::string(traitInstanceSelected) +
                   "', generic-instance-selected='" +
                   std::string(genericInstanceSelected) +
                   "', generic-instance-contextual='" +
                   std::string(genericInstanceContextual) +
                   "', generic-owner-passed='" + std::string(genericOwnerPassed) +
                   "', generic-trait-instance-selected='" +
                   std::string(genericTraitInstanceSelected) +
                   "', inherited-generic-trait-selected='" +
                   std::string(inheritedGenericTraitSelected) +
                   "', effectful-receiver-selected='" +
                   std::string(effectfulReceiverSelected) +
                   "', effectful-receiver-contextual='" +
                   std::string(effectfulReceiverContextual) +
                   "', constructed-receiver-selected='" +
                   std::string(constructedReceiverSelected) +
                   "', selected-receiver-selected='" +
                   std::string(selectedReceiverSelected) +
                   "', effectful-trait-receiver-selected='" +
                   std::string(effectfulTraitReceiverSelected) +
                   "', curried-selected='" + std::string(curriedSelected) +
                   "', nested-curried-selected='" + std::string(nestedCurriedSelected) +
                   "', curried-contextual-selected='" +
                   std::string(curriedContextualSelected) +
                   "', explicit-curried-contextual='" +
                   std::string(explicitCurriedContextual) +
                   "', inferred-curried-selected='" +
                   std::string(inferredCurriedSelected) +
                   "', effectful-receiver-curried='" +
                   std::string(effectfulReceiverCurried) + "', transparent-selected='" +
                   std::string(transparentSelected) + "', transparent-fallback='" +
                   std::string(transparentFallback) +
                   "', nested-transparent-selected='" +
                   std::string(nestedTransparentSelected) +
                   "', contextual-transparent-selected='" +
                   std::string(contextualTransparentSelected) +
                   "', explicit-transparent-selected='" +
                   std::string(explicitTransparentSelected) +
                   "', inferred-transparent-selected='" +
                   std::string(inferredTransparentSelected) +
                   "', non-generic-constant='" + std::string(nonGenericConstant) +
                   "', non-generic-decorated='" + std::string(nonGenericDecorated) +
                   "', nested-non-generic='" + std::string(nestedNonGeneric) +
                   "', non-generic-contextual='" + std::string(nonGenericContextual) +
                   "', non-generic-curried='" + std::string(nonGenericCurried) +
                   "', effectful-receiver-non-generic='" +
                   std::string(effectfulReceiverNonGeneric) +
                   "', effectful-receiver-non-generic-constant='" +
                   std::string(effectfulReceiverNonGenericConstant) +
                   "', non-generic-transparent='" + std::string(nonGenericTransparent) +
                   "')");
}

} // namespace

int runSmokeTests5() {
  return smokeInlineSummonFrom();
}
