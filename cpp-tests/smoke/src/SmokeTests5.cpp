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

inline val topLevelBanner: String = "top-inline"
inline val topLevelBannerAlias: String = topLevelBanner

object Selectors {
  val prefix: String = "selected:"
  inline val enabled: Boolean = true
  inline val foldedEnabled: Boolean = !false && true
  inline val banner: String = "inline-val"
  inline val enabledAlias: Boolean = enabled
  inline val foldedEnabledAlias: Boolean = enabledAlias && foldedEnabled
  inline val bannerAlias: String = banner
  inline val numericThreshold: Int = 5
  inline val numericThresholdAlias: Int = numericThreshold + 1

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

  inline def choose(inline condition: Boolean, value: String): String =
    inline if (condition) {
      "inline-true:" + value + ":" + value
    } else {
      "inline-false:" + value + ":" + value
    }

  inline def nestedChoose(inline condition: Boolean, value: String): String =
    "nested-" + choose(condition, value)

  inline def negatedChoose(inline condition: Boolean): String =
    inline if (!condition) "inline-negated" else "inline-original"

  inline def contextualChoose(inline condition: Boolean)(using
      named: Named[Int]): String =
    inline if (condition) {
      "inline-context:" + named.label()
    } else {
      "inline-context-false"
    }

  inline def curriedChoose(inline condition: Boolean)(value: String): String =
    inline if (condition) {
      "inline-curried-true:" + value
    } else {
      "inline-curried-false:" + value
    }

  inline def genericChoose[A](inline condition: Boolean): String =
    inline if (condition) {
      summonFrom {
        case found: Named[A] => "inline-generic:" + found.label()
        case _ => "inline-generic-fallback"
      }
    } else {
      "inline-generic-false"
    }

  transparent inline def refinedChoice(
      inline condition: Boolean): TransparentResult =
    inline if (condition) {
      new PreciseResult("inline-transparent-true")
    } else {
      new FallbackResult("inline-transparent-false")
    }

  inline def recursiveChoose(inline first: Boolean, inline second: Boolean,
      value: String): String =
    inline if (first) {
      "recursive-step:" + recursiveChoose(second, false, value)
    } else {
      "recursive-done:" + value + ":" + value
    }

  inline def recursiveContextual[A](inline continue: Boolean)(using
      named: Named[A]): String =
    inline if (continue) {
      "recursive-context:" + recursiveContextual[A](false)
    } else {
      summonFrom {
        case found: Named[A] => "recursive-context-done:" + found.label()
        case _ => "recursive-context-fallback"
      }
    }

  transparent inline def recursiveRefined(
      inline continue: Boolean): TransparentResult =
    inline if (continue) {
      recursiveRefined(false)
    } else {
      new PreciseResult("recursive-transparent")
    }

  inline def repeatedInline(inline value: String): String =
    value + ":" + value

  inline def discardedInline(inline value: String): String =
    "discarded-inline"

  inline def ordinaryShadow(value: String): String =
    value + ":" + value

  inline def shadowedInline(inline value: String): String =
    ordinaryShadow("shadowed")

  inline def contextualInline[A]()(using inline named: Named[A]): String =
    "contextual-inline:" + named.label() + ":" + named.label()

  inline def nestedContextualInline[A]()(using inline named: Named[A]): String =
    "nested-" + contextualInline[A]()

  inline def numericChoice(inline value: Int): String =
    inline if (value >= numericThresholdAlias) "numeric-high" else "numeric-low"

  inline def computedNumericChoice(inline value: Int): String =
    inline if (value * 2 == 8) "numeric-computed" else "numeric-unexpected"

  inline def arithmeticNumericChoice(inline value: Int): String =
    inline if ((value - 2) / 2 < 4) "numeric-arithmetic" else "numeric-too-large"

  inline def nestedNumericChoice(inline value: Int): String =
    "nested-" + numericChoice(value + 2)

  inline def ordinaryNumericChoice(value: Int): String =
    if (value % 2 == 0) "ordinary-numeric-even" else "ordinary-numeric-odd"

  inline def longNumericChoice(inline value: Long): String =
    inline if (value >= 100L) "long-high" else "long-low"

  inline def booleanInlineMatch(inline value: Boolean): String =
    inline value match {
      case true => "match-true"
      case _ => "match-false"
    }

  inline def numericInlineMatch(inline value: Int): String =
    inline value match {
      case 0 => "match-zero"
      case 1 | 2 => "match-small"
      case other => "match-other"
    }

  inline def computedInlineMatch(inline value: Int): String =
    inline (value + 1) match {
      case 4 => "match-computed"
      case _ => "match-computed-other"
    }

  inline def longInlineMatch(inline value: Long): String =
    inline value match {
      case 100L => "match-long"
      case _ => "match-long-other"
    }

  inline def aliasedInlineMatch(): String =
    inline numericThresholdAlias match {
      case 6 => "match-alias"
      case _ => "match-alias-other"
    }

  transparent inline def refinedTypeMatch(value: Any): TransparentResult =
    inline value match {
      case text: String => new PreciseResult("type-string:" + text)
      case number: Double => new PreciseResult("type-double:" + number.toString)
      case _ => new FallbackResult("type-fallback")
    }

  transparent inline def refinedClassMatch(
      value: TransparentResult): TransparentResult =
    inline value match {
      case selected: PreciseResult =>
        new PreciseResult("class-precise:" + selected.preciseOnly())
      case selected: FallbackResult =>
        new FallbackResult("class-fallback:" + selected.fallbackOnly())
      case _ => new FallbackResult("class-other")
    }

  inline def scalarTypeMatch(value: Any): String =
    inline value match {
      case _: Long => "type-long"
      case _: Int => "type-int"
      case _ => "type-scalar-other"
    }

  inline def ordinaryConditional(condition: Boolean, value: String): String =
    if (condition) {
      "ordinary-true:" + value
    } else {
      "ordinary-false:" + value
    }

  inline def nestedOrdinaryConditional(
      condition: Boolean, value: String): String =
    "nested-" + ordinaryConditional(condition, value)

  transparent inline def ordinaryRefined(
      condition: Boolean): TransparentResult =
    if (condition) {
      new PreciseResult("ordinary-transparent-true")
    } else {
      new FallbackResult("ordinary-transparent-false")
    }

  inline def configured(value: String): String =
    if (foldedEnabled) banner + ":" + value else "inline-val-disabled"
}

object InlineAliases {
  inline val enabled: Boolean = Selectors.foldedEnabledAlias
  inline val banner: String = Selectors.bannerAlias

  inline def configured(value: String): String =
    if (enabled) banner + ":" + value else "inline-alias-disabled"
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

  inline def choose(inline condition: Boolean, value: String): String =
    inline if (condition) {
      instancePrefix + "inline-true:" + value
    } else {
      instancePrefix + "inline-false:" + value
    }
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

  def nextInlineNamed(): Named[String] = {
    println("inline-context-effect")
    new NamedValue[String]("explicit-inline")
  }

  def nextCondition(): Boolean = {
    println("condition-effect")
    true
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
  def inlineTrue: String = Selectors.choose(true, nextNonGeneric())
  def inlineFalse: String = Selectors.choose(false, "cold")
  def nestedInlineChoice: String =
    Selectors.nestedChoose(!false, "nested")
  def negatedInlineChoice: String = Selectors.negatedChoose(false)
  def contextualInlineChoice: String = Selectors.contextualChoose(true)
  def curriedInlineChoice: String =
    Selectors.curriedChoose(false)(nextValue())
  def genericInlineChoice: String = Selectors.genericChoose[Int](true)
  def instanceInlineChoice: String =
    nextInstances().choose(false, "chosen")
  def transparentInlineChoiceTrue: String =
    Selectors.refinedChoice(true).preciseOnly()
  def transparentInlineChoiceFalse: String =
    Selectors.refinedChoice(false).fallbackOnly()
  def recursiveInlineChoice: String =
    Selectors.recursiveChoose(true, true, nextNonGeneric())
  def recursiveContextualChoice: String =
    Selectors.recursiveContextual[Int](true)
  def recursiveTransparentChoice: String =
    Selectors.recursiveRefined(true).preciseOnly()
  def repeatedInlineParameter: String =
    Selectors.repeatedInline(nextValue())
  def discardedInlineParameter: String =
    Selectors.discardedInline(nextValue())
  def shadowedInlineParameter: String =
    Selectors.shadowedInline(nextValue())
  def inferredContextualInlineParameter: String =
    Selectors.contextualInline[Int]()
  def explicitContextualInlineParameter: String =
    Selectors.contextualInline[String]()(using nextInlineNamed())
  def nestedContextualInlineParameter: String =
    Selectors.nestedContextualInline[Int]()
  def numericHigh: String = Selectors.numericChoice(8)
  def numericLow: String = Selectors.numericChoice(3)
  def computedNumeric: String = Selectors.computedNumericChoice(4)
  def arithmeticNumeric: String = Selectors.arithmeticNumericChoice(8)
  def nestedNumeric: String = Selectors.nestedNumericChoice(5)
  def ordinaryNumeric: String = Selectors.ordinaryNumericChoice(6)
  def longNumeric: String = Selectors.longNumericChoice(120L)
  def directInlineInteger: Int = Selectors.numericThresholdAlias
  def booleanInlineMatchTrue: String = Selectors.booleanInlineMatch(true)
  def booleanInlineMatchFalse: String = Selectors.booleanInlineMatch(false)
  def numericInlineMatchZero: String = Selectors.numericInlineMatch(0)
  def numericInlineMatchAlternative: String = Selectors.numericInlineMatch(2)
  def numericInlineMatchFallback: String = Selectors.numericInlineMatch(9)
  def computedInlineMatch: String = Selectors.computedInlineMatch(3)
  def longInlineMatch: String = Selectors.longInlineMatch(100L)
  def aliasedInlineMatch: String = Selectors.aliasedInlineMatch()
  def stringTypeMatch: String =
    Selectors.refinedTypeMatch("static").preciseOnly()
  def doubleTypeMatch: String =
    Selectors.refinedTypeMatch(1.5).preciseOnly()
  def fallbackTypeMatch: String =
    Selectors.refinedTypeMatch(new FallbackResult("input")).fallbackOnly()
  def preciseClassMatch: String =
    Selectors.refinedClassMatch(new PreciseResult("input")).preciseOnly()
  def fallbackClassMatch: String =
    Selectors.refinedClassMatch(
      new FallbackResult("input")).fallbackOnly()
  def intTypeMatch: String = Selectors.scalarTypeMatch(7)
  def longTypeMatch: String = Selectors.scalarTypeMatch(7L)
  def ordinaryConstantConditional: String =
    Selectors.ordinaryConditional(true, "constant")
  def nestedOrdinaryConstantConditional: String =
    Selectors.nestedOrdinaryConditional(false, "nested")
  def ordinaryRuntimeConditional: String =
    Selectors.ordinaryConditional(nextCondition(), "dynamic")
  def ordinaryTransparentTrue: String =
    Selectors.ordinaryRefined(true).preciseOnly()
  def ordinaryTransparentFalse: String =
    Selectors.ordinaryRefined(false).fallbackOnly()
  def directInlineBoolean: Boolean = Selectors.enabled
  def directInlineString: String = Selectors.banner
  def inlineValConfigured: String = Selectors.configured("configured")
  def directTopLevelInlineString: String = topLevelBanner
  def directTopLevelInlineAlias: String = topLevelBannerAlias
  def directSelectedInlineAlias: String = InlineAliases.banner
  def inlineAliasConfigured: String = InlineAliases.configured("alias")
  def shadowedInlineAlias: String = {
    val banner: String = "caller-shadow"
    Selectors.bannerAlias
  }

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
    println(inlineTrue)
    println(inlineFalse)
    println(nestedInlineChoice)
    println(negatedInlineChoice)
    println(contextualInlineChoice)
    println(curriedInlineChoice)
    println(genericInlineChoice)
    println(instanceInlineChoice)
    println(transparentInlineChoiceTrue)
    println(transparentInlineChoiceFalse)
    println(recursiveInlineChoice)
    println(recursiveContextualChoice)
    println(recursiveTransparentChoice)
    println(repeatedInlineParameter)
    println(discardedInlineParameter)
    println(shadowedInlineParameter)
    println(inferredContextualInlineParameter)
    println(explicitContextualInlineParameter)
    println(nestedContextualInlineParameter)
    println(numericHigh)
    println(numericLow)
    println(computedNumeric)
    println(arithmeticNumeric)
    println(nestedNumeric)
    println(ordinaryNumeric)
    println(longNumeric)
    println(directInlineInteger)
    println(booleanInlineMatchTrue)
    println(booleanInlineMatchFalse)
    println(numericInlineMatchZero)
    println(numericInlineMatchAlternative)
    println(numericInlineMatchFallback)
    println(computedInlineMatch)
    println(longInlineMatch)
    println(aliasedInlineMatch)
    println(stringTypeMatch)
    println(doubleTypeMatch)
    println(fallbackTypeMatch)
    println(preciseClassMatch)
    println(fallbackClassMatch)
    println(intTypeMatch)
    println(longTypeMatch)
    println(ordinaryConstantConditional)
    println(nestedOrdinaryConstantConditional)
    println(ordinaryRuntimeConditional)
    println(ordinaryTransparentTrue)
    println(ordinaryTransparentFalse)
    println(directInlineBoolean)
    println(directInlineString)
    println(inlineValConfigured)
    println(directTopLevelInlineString)
    println(directTopLevelInlineAlias)
    println(directSelectedInlineAlias)
    println(inlineAliasConfigured)
    println(shadowedInlineAlias)
  }
}
)";
  constexpr const char* invalidSource =
      R"(package demo.invalidinlinecalls

val ordinaryConstantSource: Boolean = true
inline val invalidOrdinaryReference: Boolean = ordinaryConstantSource
inline val invalidForwardReference: Boolean = followingInlineValue
inline val followingInlineValue: Boolean = true
inline val invalidSelfReference: Boolean = invalidSelfReference

object Main {
  trait RuntimeResult
  class RuntimePrecise extends RuntimeResult
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
  def runtimeCondition(): Boolean = true
  def runtimeNumber(): Int = 1
  def runtimeAny(): Any = runtimeNumber()
  def runtimeResult(): RuntimeResult = new RuntimePrecise
  inline def requiresConstant(inline condition: Boolean): String =
    inline if (condition) "constant" else "not-constant"
  def invalidInlineArgument: String = requiresConstant(runtimeCondition())
  inline def requiresInlineCondition(condition: Boolean): String =
    inline if (condition) "constant" else "not-constant"
  def invalidInlineCondition: String = requiresInlineCondition(runtimeCondition())
  inline def requiresNumeric(inline value: Int): String =
    inline if (value >= 0) "non-negative" else "negative"
  def invalidNumericCondition: String = requiresNumeric(runtimeNumber())
  inline def requiresInlineMatch(inline value: Int): String =
    inline value match {
      case 0 => "zero"
      case _ => "other"
    }
  def invalidInlineMatch: String = requiresInlineMatch(runtimeNumber())
  transparent inline def requiresStaticType(value: Any): Any =
    inline value match {
      case text: String => text
      case _ => value
    }
  def invalidStaticTypeMatch: Any = requiresStaticType(runtimeAny())
  inline def requiresStaticClass(value: RuntimeResult): String =
    inline value match {
      case _: RuntimePrecise => "precise"
      case _ => "fallback"
    }
  def invalidStaticClassMatch: String = requiresStaticClass(runtimeResult())
  inline val dynamicInlineValue: Boolean = runtimeCondition()
  inline val missingInlineValue: Boolean
}

)";
  constexpr const char* structurallyInvalidSource =
      R"(package demo.invalidinlineplacement

object Main {
  def misplacedInlineParameter(inline condition: Boolean): String = "invalid"
  def misplacedInlineIf(condition: Boolean): String =
    inline if (condition) "invalid" else "invalid"
  def misplacedInlineMatch(value: Int): String =
    inline value match {
      case 0 => "zero"
      case _ => "other"
    }
}

class UnsupportedInlineValueOwner {
  inline val value: Boolean = true
}

)";
  constexpr const char* inlineMatchParserInvalidSource =
      R"(package demo.invalidinlinematchparser

object Main {
  inline def guarded(inline value: Int): String =
    inline value match {
      case 0 if true => "zero"
      case _ => "other"
    }

  inline def floatingPattern(inline value: Double): String =
    inline value match {
      case 1.0 => "selected"
      case _ => "other"
    }
}

)";
  constexpr const char* parserInvalidSource =
      R"(package demo.invalidinlinevalparser

object Main {
  inline var unsupportedInlineVariable: Boolean = true
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
  scalanative::support::DiagnosticEngine structuralInvalidDiagnostics;
  const scalanative::tools::build::BuildResult structuralInvalid =
      driver.buildSource("StructurallyInvalidInline.scala", structurallyInvalidSource,
                         {}, structuralInvalidDiagnostics);
  scalanative::support::DiagnosticEngine parserInvalidDiagnostics;
  const scalanative::tools::build::BuildResult parserInvalid =
      driver.buildSource("ParserInvalidInlineVal.scala", parserInvalidSource, {},
                         parserInvalidDiagnostics);
  scalanative::support::DiagnosticEngine inlineMatchParserInvalidDiagnostics;
  const scalanative::tools::build::BuildResult inlineMatchParserInvalid =
      driver.buildSource("ParserInvalidInlineMatch.scala",
                         inlineMatchParserInvalidSource, {},
                         inlineMatchParserInvalidDiagnostics);

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
  const std::string_view inlineTrue =
      functionText(result.nirText, "demo.inlinecalls.Main.inlineTrue");
  const std::string_view inlineFalse =
      functionText(result.nirText, "demo.inlinecalls.Main.inlineFalse");
  const std::string_view nestedInlineChoice =
      functionText(result.nirText, "demo.inlinecalls.Main.nestedInlineChoice");
  const std::string_view negatedInlineChoice =
      functionText(result.nirText, "demo.inlinecalls.Main.negatedInlineChoice");
  const std::string_view contextualInlineChoice =
      functionText(result.nirText, "demo.inlinecalls.Main.contextualInlineChoice");
  const std::string_view curriedInlineChoice =
      functionText(result.nirText, "demo.inlinecalls.Main.curriedInlineChoice");
  const std::string_view genericInlineChoice =
      functionText(result.nirText, "demo.inlinecalls.Main.genericInlineChoice");
  const std::string_view instanceInlineChoice =
      functionText(result.nirText, "demo.inlinecalls.Main.instanceInlineChoice");
  const std::string_view transparentInlineChoiceTrue =
      functionText(result.nirText, "demo.inlinecalls.Main.transparentInlineChoiceTrue");
  const std::string_view transparentInlineChoiceFalse = functionText(
      result.nirText, "demo.inlinecalls.Main.transparentInlineChoiceFalse");
  const std::string_view recursiveInlineChoice =
      functionText(result.nirText, "demo.inlinecalls.Main.recursiveInlineChoice");
  const std::string_view recursiveContextualChoice =
      functionText(result.nirText, "demo.inlinecalls.Main.recursiveContextualChoice");
  const std::string_view recursiveTransparentChoice =
      functionText(result.nirText, "demo.inlinecalls.Main.recursiveTransparentChoice");
  const std::string_view repeatedInlineParameter =
      functionText(result.nirText, "demo.inlinecalls.Main.repeatedInlineParameter");
  const std::string_view discardedInlineParameter =
      functionText(result.nirText, "demo.inlinecalls.Main.discardedInlineParameter");
  const std::string_view shadowedInlineParameter =
      functionText(result.nirText, "demo.inlinecalls.Main.shadowedInlineParameter");
  const std::string_view inferredContextualInlineParameter = functionText(
      result.nirText, "demo.inlinecalls.Main.inferredContextualInlineParameter");
  const std::string_view explicitContextualInlineParameter = functionText(
      result.nirText, "demo.inlinecalls.Main.explicitContextualInlineParameter");
  const std::string_view nestedContextualInlineParameter = functionText(
      result.nirText, "demo.inlinecalls.Main.nestedContextualInlineParameter");
  const std::string_view numericHigh =
      functionText(result.nirText, "demo.inlinecalls.Main.numericHigh");
  const std::string_view numericLow =
      functionText(result.nirText, "demo.inlinecalls.Main.numericLow");
  const std::string_view computedNumeric =
      functionText(result.nirText, "demo.inlinecalls.Main.computedNumeric");
  const std::string_view arithmeticNumeric =
      functionText(result.nirText, "demo.inlinecalls.Main.arithmeticNumeric");
  const std::string_view nestedNumeric =
      functionText(result.nirText, "demo.inlinecalls.Main.nestedNumeric");
  const std::string_view ordinaryNumeric =
      functionText(result.nirText, "demo.inlinecalls.Main.ordinaryNumeric");
  const std::string_view longNumeric =
      functionText(result.nirText, "demo.inlinecalls.Main.longNumeric");
  const std::string_view directInlineInteger =
      functionText(result.nirText, "demo.inlinecalls.Main.directInlineInteger");
  const std::string_view booleanInlineMatchTrue =
      functionText(result.nirText, "demo.inlinecalls.Main.booleanInlineMatchTrue");
  const std::string_view booleanInlineMatchFalse =
      functionText(result.nirText, "demo.inlinecalls.Main.booleanInlineMatchFalse");
  const std::string_view numericInlineMatchZero =
      functionText(result.nirText, "demo.inlinecalls.Main.numericInlineMatchZero");
  const std::string_view numericInlineMatchAlternative = functionText(
      result.nirText, "demo.inlinecalls.Main.numericInlineMatchAlternative");
  const std::string_view numericInlineMatchFallback = functionText(
      result.nirText, "demo.inlinecalls.Main.numericInlineMatchFallback");
  const std::string_view computedInlineMatch =
      functionText(result.nirText, "demo.inlinecalls.Main.computedInlineMatch");
  const std::string_view longInlineMatch =
      functionText(result.nirText, "demo.inlinecalls.Main.longInlineMatch");
  const std::string_view aliasedInlineMatch =
      functionText(result.nirText, "demo.inlinecalls.Main.aliasedInlineMatch");
  const std::string_view stringTypeMatch =
      functionText(result.nirText, "demo.inlinecalls.Main.stringTypeMatch");
  const std::string_view doubleTypeMatch =
      functionText(result.nirText, "demo.inlinecalls.Main.doubleTypeMatch");
  const std::string_view fallbackTypeMatch =
      functionText(result.nirText, "demo.inlinecalls.Main.fallbackTypeMatch");
  const std::string_view preciseClassMatch =
      functionText(result.nirText, "demo.inlinecalls.Main.preciseClassMatch");
  const std::string_view fallbackClassMatch =
      functionText(result.nirText, "demo.inlinecalls.Main.fallbackClassMatch");
  const std::string_view intTypeMatch =
      functionText(result.nirText, "demo.inlinecalls.Main.intTypeMatch");
  const std::string_view longTypeMatch =
      functionText(result.nirText, "demo.inlinecalls.Main.longTypeMatch");
  const std::string_view ordinaryConstantConditional =
      functionText(result.nirText, "demo.inlinecalls.Main.ordinaryConstantConditional");
  const std::string_view nestedOrdinaryConstantConditional = functionText(
      result.nirText, "demo.inlinecalls.Main.nestedOrdinaryConstantConditional");
  const std::string_view ordinaryRuntimeConditional =
      functionText(result.nirText, "demo.inlinecalls.Main.ordinaryRuntimeConditional");
  const std::string_view ordinaryTransparentTrue =
      functionText(result.nirText, "demo.inlinecalls.Main.ordinaryTransparentTrue");
  const std::string_view ordinaryTransparentFalse =
      functionText(result.nirText, "demo.inlinecalls.Main.ordinaryTransparentFalse");
  const std::string_view directInlineBoolean =
      functionText(result.nirText, "demo.inlinecalls.Main.directInlineBoolean");
  const std::string_view directInlineString =
      functionText(result.nirText, "demo.inlinecalls.Main.directInlineString");
  const std::string_view inlineValConfigured =
      functionText(result.nirText, "demo.inlinecalls.Main.inlineValConfigured");
  const std::string_view directTopLevelInlineString =
      functionText(result.nirText, "demo.inlinecalls.Main.directTopLevelInlineString");
  const std::string_view directTopLevelInlineAlias =
      functionText(result.nirText, "demo.inlinecalls.Main.directTopLevelInlineAlias");
  const std::string_view directSelectedInlineAlias =
      functionText(result.nirText, "demo.inlinecalls.Main.directSelectedInlineAlias");
  const std::string_view inlineAliasConfigured =
      functionText(result.nirText, "demo.inlinecalls.Main.inlineAliasConfigured");
  const std::string_view shadowedInlineAlias =
      functionText(result.nirText, "demo.inlinecalls.Main.shadowedInlineAlias");

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
              "non-generic-transparent\n"
              "non-generic-effect\ninline-true:plain:plain\n"
              "inline-false:cold:cold\n"
              "nested-inline-true:nested:nested\n"
              "inline-negated\n"
              "inline-context:member-int\n"
              "effect\ninline-curried-false:value\n"
              "inline-generic:member-int\n"
              "receiver-effect\neffectful-instance:inline-false:chosen\n"
              "inline-transparent-true\n"
              "inline-transparent-false\n"
              "non-generic-effect\n"
              "recursive-step:recursive-step:recursive-done:plain:plain\n"
              "recursive-context:recursive-context-done:member-int\n"
              "recursive-transparent\n"
              "effect\neffect\nvalue:value\n"
              "discarded-inline\n"
              "shadowed:shadowed\n"
              "contextual-inline:member-int:member-int\n"
              "inline-context-effect\ninline-context-effect\n"
              "contextual-inline:explicit-inline:explicit-inline\n"
              "nested-contextual-inline:member-int:member-int\n"
              "numeric-high\nnumeric-low\nnumeric-computed\nnumeric-arithmetic\n"
              "nested-numeric-high\nordinary-numeric-even\nlong-high\n6\n"
              "match-true\nmatch-false\nmatch-zero\nmatch-small\nmatch-other\n"
              "match-computed\nmatch-long\nmatch-alias\n"
              "type-string:static\ntype-double:1.500000\ntype-fallback\n"
              "class-precise:input\nclass-fallback:input\n"
              "type-int\ntype-long\n"
              "ordinary-true:constant\n"
              "nested-ordinary-false:nested\n"
              "condition-effect\nordinary-true:dynamic\n"
              "ordinary-transparent-true\n"
              "ordinary-transparent-false\n"
              "true\ninline-val\ninline-val:configured\ntop-inline\n"
              "top-inline\ninline-val\ninline-val:alias\ninline-val\n" &&
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
               "maximum inline expansion depth of 32 exceeded while expanding "
               "recursive") &&
      !structuralInvalid.ok &&
      contains(structuralInvalid.diagnosticsText,
               "inline parameters are only supported on inline methods") &&
      countOccurrences(
          invalid.diagnosticsText,
          "inline if condition must be a compile-time Boolean constant") == 3 &&
      countOccurrences(
          invalid.diagnosticsText,
          "inline match selector must be reducible from a compile-time value or "
          "static type") == 3 &&
      contains(invalid.diagnosticsText,
               "inline value initializer must use literals, operators, and "
               "previously defined inline values") &&
      contains(invalid.diagnosticsText, "inline value requires an initializer") &&
      contains(structuralInvalid.diagnosticsText,
               "inline if is only supported inside an inline method") &&
      contains(structuralInvalid.diagnosticsText,
               "inline match is only supported inside an inline method") &&
      contains(structuralInvalid.diagnosticsText,
               "inline values are currently supported only at top level or in "
               "objects") &&
      !parserInvalid.ok &&
      contains(parserInvalid.diagnosticsText,
               "'inline' must modify a def or val in this milestone") &&
      !inlineMatchParserInvalid.ok &&
      contains(inlineMatchParserInvalid.diagnosticsText,
               "inline match guards are not supported yet") &&
      contains(inlineMatchParserInvalid.diagnosticsText,
               "inline match currently supports Boolean, integer, String, and Char "
               "literals, one unguarded type pattern per case, and a final wildcard "
               "or binding") &&
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
      !contains(nonGenericTransparent,
                "%demo.inlinecalls.Selectors.nonGenericRefined") &&
      contains(inlineTrue, "\"inline-true:\"") &&
      !contains(inlineTrue, "inline-false:") &&
      countOccurrences(inlineTrue, "nextNonGeneric") == 1 &&
      !contains(inlineTrue, "%demo.inlinecalls.Selectors.choose") &&
      contains(inlineFalse, "\"inline-false:\"") &&
      !contains(inlineFalse, "inline-true:") &&
      !contains(inlineFalse, "%demo.inlinecalls.Selectors.choose") &&
      contains(nestedInlineChoice, "\"inline-true:\"") &&
      !contains(nestedInlineChoice, "inline-false:") &&
      !contains(nestedInlineChoice, "%demo.inlinecalls.Selectors.nestedChoose") &&
      !contains(nestedInlineChoice, "%demo.inlinecalls.Selectors.choose") &&
      contains(negatedInlineChoice, "\"inline-negated\"") &&
      !contains(negatedInlineChoice, "inline-original") &&
      countOccurrences(contextualInlineChoice, "Main.intNamed") == 1 &&
      !contains(contextualInlineChoice, "inline-context-false") &&
      !contains(contextualInlineChoice,
                "%demo.inlinecalls.Selectors.contextualChoose") &&
      contains(curriedInlineChoice, "\"inline-curried-false:\"") &&
      !contains(curriedInlineChoice, "inline-curried-true:") &&
      countOccurrences(curriedInlineChoice, "nextValue") == 1 &&
      !contains(curriedInlineChoice, "%demo.inlinecalls.Selectors.curriedChoose") &&
      contains(genericInlineChoice, "\"inline-generic:\"") &&
      !contains(genericInlineChoice, "inline-generic-false") &&
      countOccurrences(genericInlineChoice, "Main.intNamed") == 1 &&
      !contains(genericInlineChoice, "%demo.inlinecalls.Selectors.genericChoose") &&
      contains(instanceInlineChoice, "\"inline-false:\"") &&
      !contains(instanceInlineChoice, "inline-true:") &&
      countOccurrences(instanceInlineChoice, "nextInstances") == 1 &&
      !contains(instanceInlineChoice, "%demo.inlinecalls.InstanceSelectors.choose") &&
      contains(transparentInlineChoiceTrue, "demo.inlinecalls.PreciseResult") &&
      contains(transparentInlineChoiceTrue, ".preciseOnly") &&
      !contains(transparentInlineChoiceTrue, "demo.inlinecalls.FallbackResult") &&
      !contains(transparentInlineChoiceTrue,
                "%demo.inlinecalls.Selectors.refinedChoice") &&
      contains(transparentInlineChoiceFalse, "demo.inlinecalls.FallbackResult") &&
      contains(transparentInlineChoiceFalse, ".fallbackOnly") &&
      !contains(transparentInlineChoiceFalse, "demo.inlinecalls.PreciseResult") &&
      !contains(transparentInlineChoiceFalse,
                "%demo.inlinecalls.Selectors.refinedChoice") &&
      contains(recursiveInlineChoice, "\"recursive-step:\"") &&
      contains(recursiveInlineChoice, "\"recursive-done:\"") &&
      countOccurrences(recursiveInlineChoice, "nextNonGeneric") == 1 &&
      !contains(recursiveInlineChoice, "%demo.inlinecalls.Selectors.recursiveChoose") &&
      contains(recursiveContextualChoice, "\"recursive-context:\"") &&
      contains(recursiveContextualChoice, "\"recursive-context-done:\"") &&
      countOccurrences(recursiveContextualChoice, "Main.intNamed") == 1 &&
      !contains(recursiveContextualChoice,
                "%demo.inlinecalls.Selectors.recursiveContextual") &&
      contains(recursiveTransparentChoice, "demo.inlinecalls.PreciseResult") &&
      contains(recursiveTransparentChoice, ".preciseOnly") &&
      !contains(recursiveTransparentChoice,
                "%demo.inlinecalls.Selectors.recursiveRefined") &&
      countOccurrences(repeatedInlineParameter, "nextValue") == 2 &&
      !contains(repeatedInlineParameter, "let %value") &&
      !contains(repeatedInlineParameter,
                "%demo.inlinecalls.Selectors.repeatedInline") &&
      countOccurrences(discardedInlineParameter, "nextValue") == 0 &&
      !contains(discardedInlineParameter,
                "%demo.inlinecalls.Selectors.discardedInline") &&
      contains(shadowedInlineParameter, "\"shadowed\"") &&
      countOccurrences(shadowedInlineParameter, "nextValue") == 0 &&
      !contains(shadowedInlineParameter,
                "%demo.inlinecalls.Selectors.shadowedInline") &&
      !contains(shadowedInlineParameter,
                "%demo.inlinecalls.Selectors.ordinaryShadow") &&
      countOccurrences(inferredContextualInlineParameter, "Main.intNamed") == 2 &&
      !contains(inferredContextualInlineParameter,
                "%demo.inlinecalls.Selectors.contextualInline") &&
      countOccurrences(explicitContextualInlineParameter, "nextInlineNamed") == 2 &&
      !contains(explicitContextualInlineParameter,
                "%demo.inlinecalls.Selectors.contextualInline") &&
      countOccurrences(nestedContextualInlineParameter, "Main.intNamed") == 2 &&
      !contains(nestedContextualInlineParameter,
                "%demo.inlinecalls.Selectors.nestedContextualInline") &&
      !contains(nestedContextualInlineParameter,
                "%demo.inlinecalls.Selectors.contextualInline") &&
      contains(numericHigh, "\"numeric-high\"") &&
      !contains(numericHigh, "numeric-low") &&
      !contains(numericHigh, "%demo.inlinecalls.Selectors.numericChoice") &&
      contains(numericLow, "\"numeric-low\"") &&
      !contains(numericLow, "numeric-high") &&
      !contains(numericLow, "%demo.inlinecalls.Selectors.numericChoice") &&
      contains(computedNumeric, "\"numeric-computed\"") &&
      !contains(computedNumeric, "numeric-unexpected") &&
      !contains(computedNumeric,
                "%demo.inlinecalls.Selectors.computedNumericChoice") &&
      contains(arithmeticNumeric, "\"numeric-arithmetic\"") &&
      !contains(arithmeticNumeric, "numeric-too-large") &&
      !contains(arithmeticNumeric,
                "%demo.inlinecalls.Selectors.arithmeticNumericChoice") &&
      contains(nestedNumeric, "\"nested-\"") &&
      contains(nestedNumeric, "\"numeric-high\"") &&
      !contains(nestedNumeric, "numeric-low") &&
      !contains(nestedNumeric,
                "%demo.inlinecalls.Selectors.nestedNumericChoice") &&
      !contains(nestedNumeric, "%demo.inlinecalls.Selectors.numericChoice") &&
      contains(ordinaryNumeric, "\"ordinary-numeric-even\"") &&
      !contains(ordinaryNumeric, "ordinary-numeric-odd") &&
      !contains(ordinaryNumeric,
                "%demo.inlinecalls.Selectors.ordinaryNumericChoice") &&
      contains(longNumeric, "\"long-high\"") &&
      !contains(longNumeric, "long-low") &&
      !contains(longNumeric, "%demo.inlinecalls.Selectors.longNumericChoice") &&
      contains(directInlineInteger, "5") &&
      contains(directInlineInteger, "1") &&
      !contains(directInlineInteger,
                "%demo.inlinecalls.Selectors.numericThresholdAlias") &&
      !contains(directInlineInteger,
                "%demo.inlinecalls.Selectors.numericThreshold") &&
      contains(booleanInlineMatchTrue, "\"match-true\"") &&
      !contains(booleanInlineMatchTrue, "match-false") &&
      !contains(booleanInlineMatchTrue,
                "%demo.inlinecalls.Selectors.booleanInlineMatch") &&
      contains(booleanInlineMatchFalse, "\"match-false\"") &&
      !contains(booleanInlineMatchFalse, "match-true") &&
      !contains(booleanInlineMatchFalse,
                "%demo.inlinecalls.Selectors.booleanInlineMatch") &&
      contains(numericInlineMatchZero, "\"match-zero\"") &&
      !contains(numericInlineMatchZero, "match-small") &&
      !contains(numericInlineMatchZero, "match-other") &&
      !contains(numericInlineMatchZero,
                "%demo.inlinecalls.Selectors.numericInlineMatch") &&
      contains(numericInlineMatchAlternative, "\"match-small\"") &&
      !contains(numericInlineMatchAlternative, "match-zero") &&
      !contains(numericInlineMatchAlternative, "match-other") &&
      !contains(numericInlineMatchAlternative,
                "%demo.inlinecalls.Selectors.numericInlineMatch") &&
      contains(numericInlineMatchFallback, "\"match-other\"") &&
      !contains(numericInlineMatchFallback, "match-zero") &&
      !contains(numericInlineMatchFallback, "match-small") &&
      !contains(numericInlineMatchFallback,
                "%demo.inlinecalls.Selectors.numericInlineMatch") &&
      contains(computedInlineMatch, "\"match-computed\"") &&
      !contains(computedInlineMatch, "match-computed-other") &&
      !contains(computedInlineMatch,
                "%demo.inlinecalls.Selectors.computedInlineMatch") &&
      contains(longInlineMatch, "\"match-long\"") &&
      !contains(longInlineMatch, "match-long-other") &&
      !contains(longInlineMatch,
                "%demo.inlinecalls.Selectors.longInlineMatch") &&
      contains(aliasedInlineMatch, "\"match-alias\"") &&
      !contains(aliasedInlineMatch, "match-alias-other") &&
      !contains(aliasedInlineMatch,
                "%demo.inlinecalls.Selectors.aliasedInlineMatch") &&
      contains(stringTypeMatch, "\"type-string:\"") &&
      contains(stringTypeMatch, "demo.inlinecalls.PreciseResult") &&
      contains(stringTypeMatch, ".preciseOnly") &&
      !contains(stringTypeMatch, "type-double:") &&
      !contains(stringTypeMatch, "type-fallback") &&
      !contains(stringTypeMatch,
                "%demo.inlinecalls.Selectors.refinedTypeMatch") &&
      contains(doubleTypeMatch, "\"type-double:\"") &&
      contains(doubleTypeMatch, "demo.inlinecalls.PreciseResult") &&
      contains(doubleTypeMatch, ".preciseOnly") &&
      !contains(doubleTypeMatch, "type-string:") &&
      !contains(doubleTypeMatch, "type-fallback") &&
      !contains(doubleTypeMatch,
                "%demo.inlinecalls.Selectors.refinedTypeMatch") &&
      contains(fallbackTypeMatch, "\"type-fallback\"") &&
      contains(fallbackTypeMatch, "demo.inlinecalls.FallbackResult") &&
      contains(fallbackTypeMatch, ".fallbackOnly") &&
      !contains(fallbackTypeMatch, "type-string:") &&
      !contains(fallbackTypeMatch, "type-double:") &&
      !contains(fallbackTypeMatch,
                "%demo.inlinecalls.Selectors.refinedTypeMatch") &&
      contains(preciseClassMatch, "\"class-precise:\"") &&
      contains(preciseClassMatch, "demo.inlinecalls.PreciseResult") &&
      contains(preciseClassMatch, ".preciseOnly") &&
      !contains(preciseClassMatch, "class-fallback:") &&
      !contains(preciseClassMatch, "class-other") &&
      !contains(preciseClassMatch,
                "%demo.inlinecalls.Selectors.refinedClassMatch") &&
      contains(fallbackClassMatch, "\"class-fallback:\"") &&
      contains(fallbackClassMatch, "demo.inlinecalls.FallbackResult") &&
      contains(fallbackClassMatch, ".fallbackOnly") &&
      !contains(fallbackClassMatch, "class-precise:") &&
      !contains(fallbackClassMatch, "class-other") &&
      !contains(fallbackClassMatch,
                "%demo.inlinecalls.Selectors.refinedClassMatch") &&
      contains(intTypeMatch, "\"type-int\"") &&
      !contains(intTypeMatch, "type-long") &&
      !contains(intTypeMatch, "type-scalar-other") &&
      !contains(intTypeMatch,
                "%demo.inlinecalls.Selectors.scalarTypeMatch") &&
      contains(longTypeMatch, "\"type-long\"") &&
      !contains(longTypeMatch, "type-int") &&
      !contains(longTypeMatch, "type-scalar-other") &&
      !contains(longTypeMatch,
                "%demo.inlinecalls.Selectors.scalarTypeMatch") &&
      contains(ordinaryConstantConditional, "\"ordinary-true:\"") &&
      !contains(ordinaryConstantConditional, "ordinary-false:") &&
      !contains(ordinaryConstantConditional,
                "%demo.inlinecalls.Selectors.ordinaryConditional") &&
      contains(nestedOrdinaryConstantConditional, "\"ordinary-false:\"") &&
      !contains(nestedOrdinaryConstantConditional, "ordinary-true:") &&
      !contains(nestedOrdinaryConstantConditional,
                "%demo.inlinecalls.Selectors.nestedOrdinaryConditional") &&
      !contains(nestedOrdinaryConstantConditional,
                "%demo.inlinecalls.Selectors.ordinaryConditional") &&
      countOccurrences(ordinaryRuntimeConditional, "nextCondition") == 1 &&
      contains(ordinaryRuntimeConditional, "ordinary-true:") &&
      contains(ordinaryRuntimeConditional, "ordinary-false:") &&
      !contains(ordinaryRuntimeConditional,
                "%demo.inlinecalls.Selectors.ordinaryConditional") &&
      contains(ordinaryTransparentTrue, "demo.inlinecalls.PreciseResult") &&
      contains(ordinaryTransparentTrue, ".preciseOnly") &&
      !contains(ordinaryTransparentTrue, "demo.inlinecalls.FallbackResult") &&
      !contains(ordinaryTransparentTrue,
                "%demo.inlinecalls.Selectors.ordinaryRefined") &&
      contains(ordinaryTransparentFalse, "demo.inlinecalls.FallbackResult") &&
      contains(ordinaryTransparentFalse, ".fallbackOnly") &&
      !contains(ordinaryTransparentFalse, "demo.inlinecalls.PreciseResult") &&
      !contains(ordinaryTransparentFalse,
                "%demo.inlinecalls.Selectors.ordinaryRefined") &&
      contains(directInlineBoolean, "true") &&
      !contains(directInlineBoolean, "%demo.inlinecalls.Selectors.enabled") &&
      contains(directInlineString, "\"inline-val\"") &&
      !contains(directInlineString, "%demo.inlinecalls.Selectors.banner") &&
      contains(inlineValConfigured, "\"inline-val\"") &&
      contains(inlineValConfigured, "\"configured\"") &&
      !contains(inlineValConfigured, "inline-val-disabled") &&
      !contains(inlineValConfigured, "%demo.inlinecalls.Selectors.foldedEnabled") &&
      !contains(inlineValConfigured, "%demo.inlinecalls.Selectors.banner") &&
      !contains(inlineValConfigured, "%demo.inlinecalls.Selectors.configured") &&
      contains(directTopLevelInlineString, "\"top-inline\"") &&
      !contains(directTopLevelInlineString, "%demo.inlinecalls.topLevelBanner") &&
      contains(directTopLevelInlineAlias, "\"top-inline\"") &&
      !contains(directTopLevelInlineAlias,
                "%demo.inlinecalls.topLevelBannerAlias") &&
      !contains(directTopLevelInlineAlias, "%demo.inlinecalls.topLevelBanner") &&
      contains(directSelectedInlineAlias, "\"inline-val\"") &&
      !contains(directSelectedInlineAlias,
                "%demo.inlinecalls.InlineAliases.banner") &&
      !contains(directSelectedInlineAlias,
                "%demo.inlinecalls.Selectors.bannerAlias") &&
      !contains(directSelectedInlineAlias,
                "%demo.inlinecalls.Selectors.banner") &&
      contains(inlineAliasConfigured, "\"inline-val\"") &&
      contains(inlineAliasConfigured, "\"alias\"") &&
      !contains(inlineAliasConfigured, "inline-alias-disabled") &&
      !contains(inlineAliasConfigured,
                "%demo.inlinecalls.InlineAliases.enabled") &&
      !contains(inlineAliasConfigured,
                "%demo.inlinecalls.InlineAliases.banner") &&
      !contains(inlineAliasConfigured,
                "%demo.inlinecalls.InlineAliases.configured") &&
      contains(shadowedInlineAlias, "\"caller-shadow\"") &&
      contains(shadowedInlineAlias, "\"inline-val\"") &&
      !contains(shadowedInlineAlias,
                "%demo.inlinecalls.Selectors.bannerAlias") &&
      !contains(shadowedInlineAlias, "%demo.inlinecalls.Selectors.banner");
  return valid
             ? 0
             : fail(
                   "inline summonFrom smoke test failed (output='" + text +
                   "', diagnostics='" + result.diagnosticsText +
                   "', invalid-diagnostics='" + invalid.diagnosticsText +
                   "', structural-invalid-diagnostics='" +
                   structuralInvalid.diagnosticsText +
                   "', parser-invalid-diagnostics='" + parserInvalid.diagnosticsText +
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
                   "', inline-true='" + std::string(inlineTrue) + "', inline-false='" +
                   std::string(inlineFalse) + "', nested-inline-choice='" +
                   std::string(nestedInlineChoice) + "', negated-inline-choice='" +
                   std::string(negatedInlineChoice) + "', contextual-inline-choice='" +
                   std::string(contextualInlineChoice) + "', curried-inline-choice='" +
                   std::string(curriedInlineChoice) + "', generic-inline-choice='" +
                   std::string(genericInlineChoice) + "', instance-inline-choice='" +
                   std::string(instanceInlineChoice) +
                   "', transparent-inline-choice-true='" +
                   std::string(transparentInlineChoiceTrue) +
                   "', transparent-inline-choice-false='" +
                   std::string(transparentInlineChoiceFalse) +
                   "', recursive-inline-choice='" + std::string(recursiveInlineChoice) +
                   "', recursive-contextual-choice='" +
                   std::string(recursiveContextualChoice) +
                   "', recursive-transparent-choice='" +
                   std::string(recursiveTransparentChoice) +
                   "', repeated-inline-parameter='" +
                   std::string(repeatedInlineParameter) +
                   "', discarded-inline-parameter='" +
                   std::string(discardedInlineParameter) +
                   "', shadowed-inline-parameter='" +
                   std::string(shadowedInlineParameter) +
                   "', inferred-contextual-inline-parameter='" +
                   std::string(inferredContextualInlineParameter) +
                   "', explicit-contextual-inline-parameter='" +
                   std::string(explicitContextualInlineParameter) +
                   "', nested-contextual-inline-parameter='" +
                   std::string(nestedContextualInlineParameter) +
                   "', numeric-high='" + std::string(numericHigh) +
                   "', numeric-low='" + std::string(numericLow) +
                   "', computed-numeric='" + std::string(computedNumeric) +
                   "', arithmetic-numeric='" + std::string(arithmeticNumeric) +
                   "', nested-numeric='" + std::string(nestedNumeric) +
                   "', ordinary-numeric='" + std::string(ordinaryNumeric) +
                   "', long-numeric='" + std::string(longNumeric) +
                   "', direct-inline-integer='" +
                   std::string(directInlineInteger) +
                   "', ordinary-constant-conditional='" +
                   std::string(ordinaryConstantConditional) +
                   "', nested-ordinary-constant-conditional='" +
                   std::string(nestedOrdinaryConstantConditional) +
                   "', ordinary-runtime-conditional='" +
                   std::string(ordinaryRuntimeConditional) +
                   "', ordinary-transparent-true='" +
                   std::string(ordinaryTransparentTrue) +
                   "', ordinary-transparent-false='" +
                   std::string(ordinaryTransparentFalse) +
                   "', direct-inline-boolean='" + std::string(directInlineBoolean) +
                   "', direct-inline-string='" + std::string(directInlineString) +
                   "', inline-val-configured='" + std::string(inlineValConfigured) +
                   "', direct-top-level-inline-string='" +
                   std::string(directTopLevelInlineString) +
                   "', direct-top-level-inline-alias='" +
                   std::string(directTopLevelInlineAlias) +
                   "', direct-selected-inline-alias='" +
                   std::string(directSelectedInlineAlias) +
                   "', inline-alias-configured='" +
                   std::string(inlineAliasConfigured) +
                   "', shadowed-inline-alias='" +
                   std::string(shadowedInlineAlias) + "')");
}

} // namespace

int runSmokeTests5() {
  return smokeInlineSummonFrom();
}
