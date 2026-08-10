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

int smokeInlineErasedValue() {
  constexpr const char* source = R"(package demo.inlineerased

import scala.compiletime.erasedValue
import scala.compiletime.{erasedValue => erased}
import scala.compiletime.constValue
import scala.compiletime.{constValue => constant}
import scala.compiletime.{error => compiletimeError}
import scala.compiletime.summonInline
import scala.compiletime.{summonInline => inlineSummon}

trait ErasedResult {
  def text(): String
}

class PreciseResult(val value: String) extends ErasedResult {
  def text(): String = value
  def preciseOnly(): String = value
}

class FallbackResult(val value: String) extends ErasedResult {
  def text(): String = value
  def fallbackOnly(): String = value
}

class Other

trait InlineSignal
object Ready extends InlineSignal
object Waiting extends InlineSignal
object InlineState {
  object Stopped extends InlineSignal
}
class OtherSignal extends InlineSignal

trait Named[A] {
  def label(): String
}

class NamedValue[A](val value: String) extends Named[A] {
  def label(): String = value
}

object TypeKinds {
  inline val stableDouble: Double = 2.5
  inline val stableFloat: Float = 3.5F
  inline val stableString: String = "stable"
  inline val stableChar: Char = 's'
  inline val stableNull: Any = null

  transparent inline def valueOf[T] = constValue[T]

  transparent inline def qualifiedValueOf[T] =
    scala.compiletime.constValue[T]

  transparent inline def aliasedValueOf[T] = constant[T]

  transparent inline def integerChoice[N]: String =
    inline constValue[N] match {
      case 0 => "constant-zero"
      case 7 => "constant-seven"
      case _ => "constant-other"
    }

  transparent inline def booleanChoice[B]: String =
    inline constValue[B] match {
      case true => "constant-true"
      case _ => "constant-false"
    }

  transparent inline def constantResult[B]: ErasedResult =
    inline constValue[B] match {
      case true => new PreciseResult("constant-precise")
      case _ => new FallbackResult("constant-fallback")
    }

  transparent inline def doubleChoice[D]: String =
    inline constValue[D] match {
      case 1.5 => "double-exact"
      case 2.5 | 6.25e1 => "double-alternative"
      case _ => "double-fallback"
    }

  transparent inline def floatChoice[F]: String =
    inline constValue[F] match {
      case 1.5F => "float-exact"
      case 2.5F | 6.25e1F => "float-alternative"
      case _ => "float-fallback"
    }

  inline def doubleParameterChoice(inline value: Double): String =
    inline value match {
      case 4.5 => "double-parameter"
      case _ => "double-parameter-fallback"
    }

  inline def floatParameterChoice(inline value: Float): String =
    inline value match {
      case 4.5F => "float-parameter"
      case _ => "float-parameter-fallback"
    }

  inline def stableDoubleChoice: String =
    inline stableDouble match {
      case 2.5 => "stable-double"
      case _ => "stable-double-fallback"
    }

  inline def stableFloatChoice: String =
    inline stableFloat match {
      case 3.5F => "stable-float"
      case _ => "stable-float-fallback"
    }

  inline def floatingCondition(inline value: Double): String =
    inline if (value >= 2.0) "floating-condition"
    else "floating-condition-fallback"

  transparent inline def floatingResult[D]: ErasedResult =
    inline constValue[D] match {
      case 7.5 => new PreciseResult("floating-precise")
      case _ => new FallbackResult("floating-fallback-result")
    }

  inline def guardedIntegerChoice(
      inline value: Int,
      inline enabled: Boolean): String =
    inline value match {
      case 7 if enabled => "guarded-seven"
      case _ => "guarded-fallback"
    }

  inline def bindingGuardedIntegerChoice(
      inline value: Int,
      inline enabled: Boolean): String =
    inline value match {
      case selected if selected == 7 && enabled => "binding-" + selected.toString
      case 8 => "binding-eight"
      case _ => "binding-fallback"
    }

  inline def bindingGuardedStringChoice(inline value: String): String =
    inline value match {
      case selected if selected == "bound" => selected
      case _ => "binding-string-fallback"
    }

  transparent inline def bindingGuardedResult(
      inline value: Int): ErasedResult =
    inline value match {
      case selected if selected == 7 => new PreciseResult("binding-precise")
      case _ => new FallbackResult("binding-result-fallback")
    }

  inline def typedBindingGuardedChoice(
      value: Any,
      inline enabled: Boolean): String =
    inline value match {
      case selected: String if selected == "typed" && enabled => selected
      case selected: Double if selected >= 2.0 && enabled =>
        "typed-double:" + selected.toString
      case _ => "typed-binding-fallback"
    }

  transparent inline def typedBindingResult(value: Any): ErasedResult =
    inline value match {
      case selected: Int if selected == 7 =>
        new PreciseResult("typed-binding-precise")
      case _ => new FallbackResult("typed-binding-result-fallback")
    }

  inline def boundAlternativeChoice(
      value: Any,
      inline enabled: Boolean): String =
    inline value match {
      case selected: String | selected: Int if enabled =>
        "bound-alternative:" + selected.toString
      case _ => "bound-alternative-fallback"
    }

  transparent inline def boundAlternativeResult(value: Any): ErasedResult =
    inline value match {
      case selected: String | selected: Int =>
        new PreciseResult("bound-alternative-precise:" + selected.toString)
      case _ => new FallbackResult("bound-alternative-result-fallback")
    }

  inline def singletonChoice(
      value: Any,
      inline enabled: Boolean): String =
    inline value match {
      case Ready if enabled => "singleton-ready"
      case Waiting | InlineState.Stopped => "singleton-known"
      case _ => "singleton-fallback"
    }

  transparent inline def singletonResult(value: Any): ErasedResult =
    inline value match {
      case Ready | Waiting => new PreciseResult("singleton-precise")
      case _ => new FallbackResult("singleton-result-fallback")
    }

  inline def nullChoice(
      value: Any,
      inline enabled: Boolean): String =
    inline value match {
      case null if enabled => "null-selected"
      case _ => "null-fallback"
    }

  inline def nullAlternativeChoice(value: Any): String =
    inline value match {
      case "text" | null => "null-known"
      case _ => "null-alternative-fallback"
    }

  inline def nullBindingChoice(value: Any): String =
    inline value match {
      case selected if selected == null => "null-binding"
      case _ => "null-binding-fallback"
    }

  inline def stableNullChoice: String =
    inline stableNull match {
      case null => "stable-null"
      case _ => "stable-null-fallback"
    }

  transparent inline def nullResult(value: Any): ErasedResult =
    inline value match {
      case null => new PreciseResult("null-precise")
      case _ => new FallbackResult("null-result-fallback")
    }

  inline def requireSeven[N](inline message: String): String =
    inline constValue[N] match {
      case 7 => "accepted-seven"
      case _ => compiletimeError("requireSeven: " + message)
    }

  inline def qualifiedRequire[B](inline message: String): String =
    inline constValue[B] match {
      case true => "accepted-true"
      case _ => scala.compiletime.error(message)
    }

  inline def requireCondition(
      inline condition: Boolean,
      inline message: String): String =
    inline if (condition) "accepted-condition"
    else compiletimeError(message)

  transparent inline def nameOf[T]: String =
    inline erasedValue[T] match {
      case _: String => "string"
      case _: Int => "int"
      case _: Long => "long"
      case _ => "other"
    }

  transparent inline def aliasNameOf[T]: String =
    inline erased[T] match {
      case _: String => "alias-string"
      case _ => "alias-other"
    }

  transparent inline def resultFor[T]: ErasedResult =
    inline scala.compiletime.erasedValue[T] match {
      case _: String => new PreciseResult("precise")
      case _ => new FallbackResult("fallback")
    }

  transparent inline def alternativeNameOf[T]: String =
    inline erasedValue[T] match {
      case _: String | _: Int => "alternative-string-int"
      case _: Long | _: Double => "alternative-number"
      case _ => "alternative-other"
    }

  transparent inline def guardedAlternativeNameOf[T](
      inline enabled: Boolean): String =
    inline erasedValue[T] match {
      case _: String | _: Int if enabled => "guarded-alternative"
      case _ => "guarded-alternative-fallback"
    }

  transparent inline def alternativeResult[T]: ErasedResult =
    inline erasedValue[T] match {
      case _: String | _: Int => new PreciseResult("alternative-precise")
      case _ => new FallbackResult("alternative-fallback")
    }

  transparent inline def stringChoice[S]: String =
    inline constValue[S] match {
      case "alpha" => "string-alpha"
      case "beta" | "line\nbreak" => "string-alternative"
      case _ => "string-fallback"
    }

  transparent inline def charChoice[C]: String =
    inline constValue[C] match {
      case 'x' => "char-x"
      case 'y' | '\n' => "char-alternative"
      case _ => "char-fallback"
    }

  inline def stringParameterChoice(inline value: String): String =
    inline value match {
      case "parameter" => "string-parameter"
      case _ => "string-parameter-fallback"
    }

  inline def charParameterChoice(inline value: Char): String =
    inline value match {
      case 'p' => "char-parameter"
      case _ => "char-parameter-fallback"
    }

  inline def stableStringChoice: String =
    inline stableString match {
      case "stable" => "stable-string"
      case _ => "stable-string-fallback"
    }

  inline def stableCharChoice: String =
    inline stableChar match {
      case 's' => "stable-char"
      case _ => "stable-char-fallback"
    }

  transparent inline def stringResult[S]: ErasedResult =
    inline constValue[S] match {
      case "precise" => new PreciseResult("string-precise")
      case _ => new FallbackResult("string-fallback-result")
    }

  inline def summoned[A]: String = summonInline[Named[A]].label()

  inline def qualifiedSummoned[A]: String =
    scala.compiletime.summonInline[Named[A]].label()

  inline def aliasedSummoned[A]: String = inlineSummon[Named[A]].label()

  inline def contextualSummoned[A]()(using named: Named[A]): String =
    summonInline[Named[A]].label()

  inline def nestedSummoned[A]: String = "nested:" + summoned[A]

  inline def selectedSummoned[A](inline required: Boolean): String =
    inline if (required) summonInline[Named[A]].label()
    else "summon-skipped"
}

object Shadowing {
  def summonInline[A](): String = "ordinary-shadow"
  def value: String = summonInline[Int]()
}

object Main {
  given intEvidence: Named[Int] = new NamedValue[Int]("summoned-int")
  given stringEvidence: Named[String] = new NamedValue[String]("summoned-string")
  given longEvidence: Named[Long] = new NamedValue[Long]("summoned-long")

  def runtimeGuard(): Boolean = true

  def directConstant: Int = constValue[11]
  def intConstant: Int = TypeKinds.valueOf[42]
  def longConstant: Long = TypeKinds.qualifiedValueOf[9000000000L]
  def stringConstant: String = TypeKinds.aliasedValueOf["literal"]
  def booleanConstant: Boolean = TypeKinds.valueOf[true]
  def charConstant: Char = TypeKinds.valueOf['x']
  def doubleConstant: Double = TypeKinds.valueOf[1.25]
  def floatConstant: Float = TypeKinds.valueOf[1.25F]
  def constantMatchSeven: String = TypeKinds.integerChoice[7]
  def constantMatchOther: String = TypeKinds.integerChoice[9]
  def constantBooleanFalse: String = TypeKinds.booleanChoice[false]
  def constantPrecise: String = TypeKinds.constantResult[true].preciseOnly()
  def constantFallback: String = TypeKinds.constantResult[false].fallbackOnly()
  def doubleExact: String = TypeKinds.doubleChoice[1.5]
  def doubleAlternative: String = TypeKinds.doubleChoice[62.5]
  def doubleFallback: String = TypeKinds.doubleChoice[9.5]
  def floatExact: String = TypeKinds.floatChoice[1.5F]
  def floatAlternative: String = TypeKinds.floatChoice[62.5F]
  def floatFallback: String = TypeKinds.floatChoice[9.5F]
  def doubleParameter: String = TypeKinds.doubleParameterChoice(4.5)
  def floatParameter: String = TypeKinds.floatParameterChoice(4.5F)
  def stableDouble: String = TypeKinds.stableDoubleChoice
  def stableFloat: String = TypeKinds.stableFloatChoice
  def reducedFloatingCondition: String = TypeKinds.floatingCondition(2.5)
  def floatingPrecise: String = TypeKinds.floatingResult[7.5].preciseOnly()
  def guardedSeven: String = TypeKinds.guardedIntegerChoice(7, true)
  def guardedFallback: String = TypeKinds.guardedIntegerChoice(7, false)
  def guardedSkipped: String = TypeKinds.guardedIntegerChoice(8, runtimeGuard())
  def bindingGuardedSelected: String =
    TypeKinds.bindingGuardedIntegerChoice(7, true)
  def bindingGuardedRejected: String =
    TypeKinds.bindingGuardedIntegerChoice(7, false)
  def bindingGuardedNext: String =
    TypeKinds.bindingGuardedIntegerChoice(8, runtimeGuard())
  def bindingStringSelected: String =
    TypeKinds.bindingGuardedStringChoice("bound")
  def bindingStringFallback: String =
    TypeKinds.bindingGuardedStringChoice("other")
  def bindingPrecise: String =
    TypeKinds.bindingGuardedResult(7).preciseOnly()
  def typedBindingString: String =
    TypeKinds.typedBindingGuardedChoice("typed", true)
  def typedBindingRejected: String =
    TypeKinds.typedBindingGuardedChoice("typed", false)
  def typedBindingSkipped: String =
    TypeKinds.typedBindingGuardedChoice(1, runtimeGuard())
  def typedBindingDouble: String =
    TypeKinds.typedBindingGuardedChoice(2.5, true)
  def typedBindingDoubleRejected: String =
    TypeKinds.typedBindingGuardedChoice(1.5, true)
  def typedBindingPrecise: String =
    TypeKinds.typedBindingResult(7).preciseOnly()
  def boundAlternativeString: String =
    TypeKinds.boundAlternativeChoice("text", true)
  def boundAlternativeInt: String =
    TypeKinds.boundAlternativeChoice(7, true)
  def boundAlternativeRejected: String =
    TypeKinds.boundAlternativeChoice("text", false)
  def boundAlternativeSkipped: String =
    TypeKinds.boundAlternativeChoice(2.5, runtimeGuard())
  def boundAlternativePrecise: String =
    TypeKinds.boundAlternativeResult(7).preciseOnly()
  def singletonReady: String = TypeKinds.singletonChoice(Ready, true)
  def singletonRejected: String = TypeKinds.singletonChoice(Ready, false)
  def singletonWaiting: String = TypeKinds.singletonChoice(Waiting, true)
  def singletonQualified: String =
    TypeKinds.singletonChoice(InlineState.Stopped, true)
  def singletonFallback: String =
    TypeKinds.singletonChoice(new OtherSignal, true)
  def singletonScalarFallback: String = TypeKinds.singletonChoice(7, true)
  def singletonSkipped: String =
    TypeKinds.singletonChoice(new OtherSignal, runtimeGuard())
  def singletonPrecise: String =
    TypeKinds.singletonResult(Ready).preciseOnly()
  def singletonResultFallback: String =
    TypeKinds.singletonResult(new OtherSignal).fallbackOnly()
  def nullSelected: String = TypeKinds.nullChoice(null, true)
  def nullRejected: String = TypeKinds.nullChoice(null, false)
  def nullAlternative: String =
    TypeKinds.nullAlternativeChoice("text")
  def nullAlternativeNull: String =
    TypeKinds.nullAlternativeChoice(null)
  def nullReferenceFallback: String =
    TypeKinds.nullChoice(new OtherSignal, runtimeGuard())
  def nullObjectFallback: String = TypeKinds.nullChoice(Ready, true)
  def nullScalarFallback: String = TypeKinds.nullChoice(7, true)
  def nullBindingSelected: String = TypeKinds.nullBindingChoice(null)
  def nullBindingFallback: String =
    TypeKinds.nullBindingChoice(new OtherSignal)
  def stableNullSelected: String = TypeKinds.stableNullChoice
  def nullPrecise: String = TypeKinds.nullResult(null).preciseOnly()
  def nullResultFallback: String =
    TypeKinds.nullResult(new OtherSignal).fallbackOnly()
  def acceptedSeven: String = TypeKinds.requireSeven[7]("ignored-seven")
  def acceptedTrue: String = TypeKinds.qualifiedRequire[true]("ignored-true")
  def acceptedCondition: String =
    TypeKinds.requireCondition(true, "ignored-condition")
  def stringName: String = TypeKinds.nameOf[String]
  def intName: String = TypeKinds.nameOf[Int]
  def longName: String = TypeKinds.nameOf[Long]
  def otherName: String = TypeKinds.nameOf[Other]
  def aliasStringName: String = TypeKinds.aliasNameOf[String]
  def aliasOtherName: String = TypeKinds.aliasNameOf[Other]
  def preciseResult: String = TypeKinds.resultFor[String].preciseOnly()
  def fallbackResult: String = TypeKinds.resultFor[Other].fallbackOnly()
  def alternativeString: String = TypeKinds.alternativeNameOf[String]
  def alternativeInt: String = TypeKinds.alternativeNameOf[Int]
  def alternativeDouble: String = TypeKinds.alternativeNameOf[Double]
  def alternativeOther: String = TypeKinds.alternativeNameOf[Other]
  def guardedAlternative: String =
    TypeKinds.guardedAlternativeNameOf[String](true)
  def rejectedAlternative: String =
    TypeKinds.guardedAlternativeNameOf[Int](false)
  def skippedAlternative: String =
    TypeKinds.guardedAlternativeNameOf[Other](runtimeGuard())
  def alternativePrecise: String =
    TypeKinds.alternativeResult[String].preciseOnly()
  def stringAlpha: String = TypeKinds.stringChoice["alpha"]
  def stringAlternative: String = TypeKinds.stringChoice["line\nbreak"]
  def stringFallback: String = TypeKinds.stringChoice["other"]
  def charX: String = TypeKinds.charChoice['x']
  def charAlternative: String = TypeKinds.charChoice['\n']
  def charFallback: String = TypeKinds.charChoice['q']
  def stringParameter: String = TypeKinds.stringParameterChoice("parameter")
  def charParameter: String = TypeKinds.charParameterChoice('p')
  def stableString: String = TypeKinds.stableStringChoice
  def stableChar: String = TypeKinds.stableCharChoice
  def stringPrecise: String = TypeKinds.stringResult["precise"].preciseOnly()
  def directSummoned: String = summonInline[Named[Int]].label()
  def inlineSummoned: String = TypeKinds.summoned[Int]
  def qualifiedSummoned: String = TypeKinds.qualifiedSummoned[String]
  def aliasedSummoned: String = TypeKinds.aliasedSummoned[Long]
  def contextualSummoned: String = TypeKinds.contextualSummoned[Int]()
  def nestedSummoned: String = TypeKinds.nestedSummoned[Int]
  def selectedSummoned: String = TypeKinds.selectedSummoned[String](true)
  def skippedSummoned: String = TypeKinds.selectedSummoned[Other](false)

  def main(args: Array[String]): Unit = {
    println(directConstant)
    println(intConstant)
    println(longConstant)
    println(stringConstant)
    println(booleanConstant)
    println(charConstant)
    println(constantMatchSeven)
    println(constantMatchOther)
    println(constantBooleanFalse)
    println(constantPrecise)
    println(constantFallback)
    println(doubleExact)
    println(doubleAlternative)
    println(doubleFallback)
    println(floatExact)
    println(floatAlternative)
    println(floatFallback)
    println(doubleParameter)
    println(floatParameter)
    println(stableDouble)
    println(stableFloat)
    println(reducedFloatingCondition)
    println(floatingPrecise)
    println(guardedSeven)
    println(guardedFallback)
    println(guardedSkipped)
    println(bindingGuardedSelected)
    println(bindingGuardedRejected)
    println(bindingGuardedNext)
    println(bindingStringSelected)
    println(bindingStringFallback)
    println(bindingPrecise)
    println(typedBindingString)
    println(typedBindingRejected)
    println(typedBindingSkipped)
    println(typedBindingDouble)
    println(typedBindingDoubleRejected)
    println(typedBindingPrecise)
    println(boundAlternativeString)
    println(boundAlternativeInt)
    println(boundAlternativeRejected)
    println(boundAlternativeSkipped)
    println(boundAlternativePrecise)
    println(singletonReady)
    println(singletonRejected)
    println(singletonWaiting)
    println(singletonQualified)
    println(singletonFallback)
    println(singletonScalarFallback)
    println(singletonSkipped)
    println(singletonPrecise)
    println(singletonResultFallback)
    println(nullSelected)
    println(nullRejected)
    println(nullAlternative)
    println(nullAlternativeNull)
    println(nullReferenceFallback)
    println(nullObjectFallback)
    println(nullScalarFallback)
    println(nullBindingSelected)
    println(nullBindingFallback)
    println(stableNullSelected)
    println(nullPrecise)
    println(nullResultFallback)
    println(acceptedSeven)
    println(acceptedTrue)
    println(acceptedCondition)
    println(stringName)
    println(intName)
    println(longName)
    println(otherName)
    println(aliasStringName)
    println(aliasOtherName)
    println(preciseResult)
    println(fallbackResult)
    println(alternativeString)
    println(alternativeInt)
    println(alternativeDouble)
    println(alternativeOther)
    println(guardedAlternative)
    println(rejectedAlternative)
    println(skippedAlternative)
    println(alternativePrecise)
    println(stringAlpha)
    println(stringAlternative)
    println(stringFallback)
    println(charX)
    println(charAlternative)
    println(charFallback)
    println(stringParameter)
    println(charParameter)
    println(stableString)
    println(stableChar)
    println(stringPrecise)
    println(directSummoned)
    println(inlineSummoned)
    println(qualifiedSummoned)
    println(aliasedSummoned)
    println(contextualSummoned)
    println(nestedSummoned)
    println(selectedSummoned)
    println(skippedSummoned)
    println(Shadowing.value)
  }
}
)";

  constexpr const char* invalidSource = R"(package demo.invaliderased

import scala.compiletime.erasedValue
import scala.compiletime.constValue
import scala.compiletime.error
import scala.compiletime.{error => compiletimeError}
import scala.compiletime.summonInline
import scala.compiletime.{summonInline => inlineSummon}

trait Named[A]
class NamedValue[A] extends Named[A]

object Main {
  given firstLongNamed: Named[Long] = new NamedValue[Long]
  given secondLongNamed: Named[Long] = new NamedValue[Long]

  inline val errorPrefix = "aliased: "

  inline def fail(inline message: String): Nothing =
    compiletimeError("failure: " + message)

  inline def failAliased(inline message: String): Nothing =
    error(errorPrefix + message)

  inline def failOnFalse[B](inline message: String): String =
    inline constValue[B] match {
      case true => "ok"
      case _ => error(message)
    }

  inline def requireCondition(
      inline condition: Boolean,
      inline message: String): String =
    inline if (condition) "ok"
    else error(message)

  val selectedError = fail("boom")
  val selectedAliasedError = failAliased("boom")
  val selectedMatchError = failOnFalse[false]("false branch")
  val selectedConditionError = requireCondition(false, "condition failed")
  val qualifiedError = scala.compiletime.error("direct failure")
  val constValueError = error(constValue["constant value failure"])
  val dynamicMessage: String = "dynamic"
  val nonConstantError = error(dynamicMessage)
  def dynamicInlineError(message: String) = fail(message)
  val wrongErrorType = error(1)
  val malformedError = error()

  val nonConstant = constValue[String]
  val malformedConstant = constValue[1, 2]

  def unresolvedConstant[T] = constValue[T]

  inline def deferredConstant[T] = constValue[T]

  def unresolvedConstantCall[A] = deferredConstant[A]

  inline def deferredSummon[A] = summonInline[Named[A]]

  val missingDeferredSummon = deferredSummon[Boolean]
  val missingDirectSummon = summonInline[Named[String]]
  val missingAliasedSummon = inlineSummon[Named[Char]]
  val ambiguousQualifiedSummon =
    scala.compiletime.summonInline[Named[Long]]
  val malformedSummon = summonInline[Named[Int], Named[String]]

  def runtimeString(): String = "runtime"
  def runtimeChar(): Char = 'r'
  def runtimeDouble(): Double = 1.0
  def runtimeFloat(): Float = 1.0F
  def runtimeBoolean(): Boolean = true
  def runtimeSignal(): InlineSignal = Ready
  def runtimeAny(): Any = null

  inline def requiresStringMatch(inline value: String): String =
    inline value match {
      case "known" => "known"
      case _ => "other"
    }

  inline def requiresCharMatch(inline value: Char): String =
    inline value match {
      case 'k' => "known"
      case _ => "other"
    }

  inline def requiresDoubleMatch(inline value: Double): String =
    inline value match {
      case 1.0 => "known"
      case _ => "other"
    }

  inline def requiresFloatMatch(inline value: Float): String =
    inline value match {
      case 1.0F => "known"
      case _ => "other"
    }

  inline def requiresGuardedMatch(
      inline value: Int,
      inline enabled: Boolean): String =
    inline value match {
      case 1 if enabled => "known"
      case _ => "other"
    }

  inline def requiresGuardedType[T](inline enabled: Boolean): String =
    inline erasedValue[T] match {
      case _: String | _: Int if enabled => "known"
      case _ => "other"
    }

  inline def requiresBindingGuard(
      inline value: Int,
      inline enabled: Boolean): String =
    inline value match {
      case selected if selected == 1 && enabled => "known"
      case _ => "other"
    }

  inline def requiresTypedBindingGuard(
      value: Any,
      inline enabled: Boolean): String =
    inline value match {
      case selected: String if selected == "known" && enabled => selected
      case _ => "other"
    }

  inline def requiresBoundAlternative(
      value: Any,
      inline enabled: Boolean): String =
    inline value match {
      case selected: String | selected: Int if enabled => selected.toString
      case _ => "other"
    }

  inline def requiresSingleton(value: InlineSignal): String =
    inline value match {
      case Ready => "ready"
      case _ => "other"
    }

  inline def requiresNull(value: Any): String =
    inline value match {
      case null => "null"
      case _ => "other"
    }

  val unresolvedStringMatch = requiresStringMatch(runtimeString())
  val unresolvedCharMatch = requiresCharMatch(runtimeChar())
  val unresolvedDoubleMatch = requiresDoubleMatch(runtimeDouble())
  val unresolvedFloatMatch = requiresFloatMatch(runtimeFloat())
  val unresolvedGuardedMatch = requiresGuardedMatch(1, runtimeBoolean())
  val unresolvedGuardedType = requiresGuardedType[String](runtimeBoolean())
  val unresolvedBindingGuard = requiresBindingGuard(1, runtimeBoolean())
  val unresolvedTypedBindingGuard =
    requiresTypedBindingGuard("known", runtimeBoolean())
  val unresolvedBoundAlternative =
    requiresBoundAlternative("known", runtimeBoolean())
  val unresolvedSingleton = requiresSingleton(runtimeSignal())
  val unresolvedNull = requiresNull(runtimeAny())

  val escaped = erasedValue[String]
  val qualifiedEscaped = scala.compiletime.erasedValue[Int]
  val malformed = erasedValue[String, Int]

  inline def bound[T]: String =
    inline erasedValue[T] match {
      case value: String => value
      case _ => "fallback"
    }

  inline def catchBound[T]: String =
    inline erasedValue[T] match {
      case _: String => "string"
      case other => other.toString
    }

  inline def classify[T]: String =
    inline erasedValue[T] match {
      case _: String => "string"
      case _ => "other"
    }

  def unresolved[A]: String = classify[A]

  inline def ordinary[T]: String =
    erasedValue[T] match {
      case _: String => "string"
      case _ => "other"
    }
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "cpp-scalanative-smoke-inline-erased-value";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-inline-erased-value.out";
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
      driver.buildSource("InlineErasedValue.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidInlineErasedValue.scala", invalidSource, {}, invalidDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("inline erasedValue native build failed: " + result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string outputText = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  const std::string_view stringName =
      functionText(result.nirText, "demo.inlineerased.Main.stringName");
  const std::string_view directConstant =
      functionText(result.nirText, "demo.inlineerased.Main.directConstant");
  const std::string_view intConstant =
      functionText(result.nirText, "demo.inlineerased.Main.intConstant");
  const std::string_view longConstant =
      functionText(result.nirText, "demo.inlineerased.Main.longConstant");
  const std::string_view stringConstant =
      functionText(result.nirText, "demo.inlineerased.Main.stringConstant");
  const std::string_view booleanConstant =
      functionText(result.nirText, "demo.inlineerased.Main.booleanConstant");
  const std::string_view charConstant =
      functionText(result.nirText, "demo.inlineerased.Main.charConstant");
  const std::string_view doubleConstant =
      functionText(result.nirText, "demo.inlineerased.Main.doubleConstant");
  const std::string_view floatConstant =
      functionText(result.nirText, "demo.inlineerased.Main.floatConstant");
  const std::string_view constantMatchSeven =
      functionText(result.nirText, "demo.inlineerased.Main.constantMatchSeven");
  const std::string_view constantMatchOther =
      functionText(result.nirText, "demo.inlineerased.Main.constantMatchOther");
  const std::string_view constantBooleanFalse = functionText(
      result.nirText, "demo.inlineerased.Main.constantBooleanFalse");
  const std::string_view constantPrecise =
      functionText(result.nirText, "demo.inlineerased.Main.constantPrecise");
  const std::string_view constantFallback =
      functionText(result.nirText, "demo.inlineerased.Main.constantFallback");
  const std::string_view doubleExact =
      functionText(result.nirText, "demo.inlineerased.Main.doubleExact");
  const std::string_view doubleAlternative =
      functionText(result.nirText, "demo.inlineerased.Main.doubleAlternative");
  const std::string_view doubleFallback =
      functionText(result.nirText, "demo.inlineerased.Main.doubleFallback");
  const std::string_view floatExact =
      functionText(result.nirText, "demo.inlineerased.Main.floatExact");
  const std::string_view floatAlternative =
      functionText(result.nirText, "demo.inlineerased.Main.floatAlternative");
  const std::string_view floatFallback =
      functionText(result.nirText, "demo.inlineerased.Main.floatFallback");
  const std::string_view doubleParameter =
      functionText(result.nirText, "demo.inlineerased.Main.doubleParameter");
  const std::string_view floatParameter =
      functionText(result.nirText, "demo.inlineerased.Main.floatParameter");
  const std::string_view stableDouble =
      functionText(result.nirText, "demo.inlineerased.Main.stableDouble");
  const std::string_view stableFloat =
      functionText(result.nirText, "demo.inlineerased.Main.stableFloat");
  const std::string_view reducedFloatingCondition = functionText(
      result.nirText, "demo.inlineerased.Main.reducedFloatingCondition");
  const std::string_view floatingPrecise =
      functionText(result.nirText, "demo.inlineerased.Main.floatingPrecise");
  const std::string_view guardedSeven =
      functionText(result.nirText, "demo.inlineerased.Main.guardedSeven");
  const std::string_view guardedFallback =
      functionText(result.nirText, "demo.inlineerased.Main.guardedFallback");
  const std::string_view guardedSkipped =
      functionText(result.nirText, "demo.inlineerased.Main.guardedSkipped");
  const std::string_view bindingGuardedSelected = functionText(
      result.nirText, "demo.inlineerased.Main.bindingGuardedSelected");
  const std::string_view bindingGuardedRejected = functionText(
      result.nirText, "demo.inlineerased.Main.bindingGuardedRejected");
  const std::string_view bindingGuardedNext =
      functionText(result.nirText, "demo.inlineerased.Main.bindingGuardedNext");
  const std::string_view bindingStringSelected = functionText(
      result.nirText, "demo.inlineerased.Main.bindingStringSelected");
  const std::string_view bindingStringFallback = functionText(
      result.nirText, "demo.inlineerased.Main.bindingStringFallback");
  const std::string_view bindingPrecise =
      functionText(result.nirText, "demo.inlineerased.Main.bindingPrecise");
  const std::string_view typedBindingString =
      functionText(result.nirText, "demo.inlineerased.Main.typedBindingString");
  const std::string_view typedBindingRejected = functionText(
      result.nirText, "demo.inlineerased.Main.typedBindingRejected");
  const std::string_view typedBindingSkipped = functionText(
      result.nirText, "demo.inlineerased.Main.typedBindingSkipped");
  const std::string_view typedBindingDouble =
      functionText(result.nirText, "demo.inlineerased.Main.typedBindingDouble");
  const std::string_view typedBindingDoubleRejected = functionText(
      result.nirText, "demo.inlineerased.Main.typedBindingDoubleRejected");
  const std::string_view typedBindingPrecise =
      functionText(result.nirText, "demo.inlineerased.Main.typedBindingPrecise");
  const std::string_view boundAlternativeString = functionText(
      result.nirText, "demo.inlineerased.Main.boundAlternativeString");
  const std::string_view boundAlternativeInt =
      functionText(result.nirText, "demo.inlineerased.Main.boundAlternativeInt");
  const std::string_view boundAlternativeRejected = functionText(
      result.nirText, "demo.inlineerased.Main.boundAlternativeRejected");
  const std::string_view boundAlternativeSkipped = functionText(
      result.nirText, "demo.inlineerased.Main.boundAlternativeSkipped");
  const std::string_view boundAlternativePrecise = functionText(
      result.nirText, "demo.inlineerased.Main.boundAlternativePrecise");
  const std::string_view singletonReady =
      functionText(result.nirText, "demo.inlineerased.Main.singletonReady");
  const std::string_view singletonRejected =
      functionText(result.nirText, "demo.inlineerased.Main.singletonRejected");
  const std::string_view singletonWaiting =
      functionText(result.nirText, "demo.inlineerased.Main.singletonWaiting");
  const std::string_view singletonQualified =
      functionText(result.nirText, "demo.inlineerased.Main.singletonQualified");
  const std::string_view singletonFallback =
      functionText(result.nirText, "demo.inlineerased.Main.singletonFallback");
  const std::string_view singletonScalarFallback = functionText(
      result.nirText, "demo.inlineerased.Main.singletonScalarFallback");
  const std::string_view singletonSkipped =
      functionText(result.nirText, "demo.inlineerased.Main.singletonSkipped");
  const std::string_view singletonPrecise =
      functionText(result.nirText, "demo.inlineerased.Main.singletonPrecise");
  const std::string_view singletonResultFallback = functionText(
      result.nirText, "demo.inlineerased.Main.singletonResultFallback");
  const std::string_view nullSelected =
      functionText(result.nirText, "demo.inlineerased.Main.nullSelected");
  const std::string_view nullRejected =
      functionText(result.nirText, "demo.inlineerased.Main.nullRejected");
  const std::string_view nullAlternative =
      functionText(result.nirText, "demo.inlineerased.Main.nullAlternative");
  const std::string_view nullAlternativeNull = functionText(
      result.nirText, "demo.inlineerased.Main.nullAlternativeNull");
  const std::string_view nullReferenceFallback = functionText(
      result.nirText, "demo.inlineerased.Main.nullReferenceFallback");
  const std::string_view nullObjectFallback =
      functionText(result.nirText, "demo.inlineerased.Main.nullObjectFallback");
  const std::string_view nullScalarFallback =
      functionText(result.nirText, "demo.inlineerased.Main.nullScalarFallback");
  const std::string_view nullBindingSelected = functionText(
      result.nirText, "demo.inlineerased.Main.nullBindingSelected");
  const std::string_view nullBindingFallback = functionText(
      result.nirText, "demo.inlineerased.Main.nullBindingFallback");
  const std::string_view stableNullSelected =
      functionText(result.nirText, "demo.inlineerased.Main.stableNullSelected");
  const std::string_view nullPrecise =
      functionText(result.nirText, "demo.inlineerased.Main.nullPrecise");
  const std::string_view nullResultFallback =
      functionText(result.nirText, "demo.inlineerased.Main.nullResultFallback");
  const std::string_view acceptedSeven =
      functionText(result.nirText, "demo.inlineerased.Main.acceptedSeven");
  const std::string_view acceptedTrue =
      functionText(result.nirText, "demo.inlineerased.Main.acceptedTrue");
  const std::string_view acceptedCondition =
      functionText(result.nirText, "demo.inlineerased.Main.acceptedCondition");
  const std::string_view intName =
      functionText(result.nirText, "demo.inlineerased.Main.intName");
  const std::string_view longName =
      functionText(result.nirText, "demo.inlineerased.Main.longName");
  const std::string_view otherName =
      functionText(result.nirText, "demo.inlineerased.Main.otherName");
  const std::string_view aliasStringName =
      functionText(result.nirText, "demo.inlineerased.Main.aliasStringName");
  const std::string_view aliasOtherName =
      functionText(result.nirText, "demo.inlineerased.Main.aliasOtherName");
  const std::string_view preciseResult =
      functionText(result.nirText, "demo.inlineerased.Main.preciseResult");
  const std::string_view fallbackResult =
      functionText(result.nirText, "demo.inlineerased.Main.fallbackResult");
  const std::string_view alternativeString =
      functionText(result.nirText, "demo.inlineerased.Main.alternativeString");
  const std::string_view alternativeInt =
      functionText(result.nirText, "demo.inlineerased.Main.alternativeInt");
  const std::string_view alternativeDouble =
      functionText(result.nirText, "demo.inlineerased.Main.alternativeDouble");
  const std::string_view alternativeOther =
      functionText(result.nirText, "demo.inlineerased.Main.alternativeOther");
  const std::string_view guardedAlternative =
      functionText(result.nirText, "demo.inlineerased.Main.guardedAlternative");
  const std::string_view rejectedAlternative =
      functionText(result.nirText, "demo.inlineerased.Main.rejectedAlternative");
  const std::string_view skippedAlternative =
      functionText(result.nirText, "demo.inlineerased.Main.skippedAlternative");
  const std::string_view alternativePrecise =
      functionText(result.nirText, "demo.inlineerased.Main.alternativePrecise");
  const std::string_view stringAlpha =
      functionText(result.nirText, "demo.inlineerased.Main.stringAlpha");
  const std::string_view stringAlternative =
      functionText(result.nirText, "demo.inlineerased.Main.stringAlternative");
  const std::string_view stringFallback =
      functionText(result.nirText, "demo.inlineerased.Main.stringFallback");
  const std::string_view charX =
      functionText(result.nirText, "demo.inlineerased.Main.charX");
  const std::string_view charAlternative =
      functionText(result.nirText, "demo.inlineerased.Main.charAlternative");
  const std::string_view charFallback =
      functionText(result.nirText, "demo.inlineerased.Main.charFallback");
  const std::string_view stringParameter =
      functionText(result.nirText, "demo.inlineerased.Main.stringParameter");
  const std::string_view charParameter =
      functionText(result.nirText, "demo.inlineerased.Main.charParameter");
  const std::string_view stableString =
      functionText(result.nirText, "demo.inlineerased.Main.stableString");
  const std::string_view stableChar =
      functionText(result.nirText, "demo.inlineerased.Main.stableChar");
  const std::string_view stringPrecise =
      functionText(result.nirText, "demo.inlineerased.Main.stringPrecise");
  const std::string_view directSummoned =
      functionText(result.nirText, "demo.inlineerased.Main.directSummoned");
  const std::string_view inlineSummoned =
      functionText(result.nirText, "demo.inlineerased.Main.inlineSummoned");
  const std::string_view qualifiedSummoned =
      functionText(result.nirText, "demo.inlineerased.Main.qualifiedSummoned");
  const std::string_view aliasedSummoned =
      functionText(result.nirText, "demo.inlineerased.Main.aliasedSummoned");
  const std::string_view contextualSummoned =
      functionText(result.nirText, "demo.inlineerased.Main.contextualSummoned");
  const std::string_view nestedSummoned =
      functionText(result.nirText, "demo.inlineerased.Main.nestedSummoned");
  const std::string_view selectedSummoned =
      functionText(result.nirText, "demo.inlineerased.Main.selectedSummoned");
  const std::string_view skippedSummoned =
      functionText(result.nirText, "demo.inlineerased.Main.skippedSummoned");

  const auto fullyReduced = [](std::string_view function) {
    return !function.empty() && !contains(function, "$match") &&
           !contains(function, "is-instance-of") &&
           !contains(function, "compiletime.error") &&
           !contains(function, "constValue") &&
           !contains(function, "erasedValue") &&
           !contains(function, "summonInline") && !contains(function, "TypeKinds.");
  };
  const bool constantsReduced =
      fullyReduced(directConstant) && contains(directConstant, "11") &&
      fullyReduced(intConstant) && contains(intConstant, "42") &&
      fullyReduced(longConstant) && contains(longConstant, "9000000000L") &&
      fullyReduced(stringConstant) && contains(stringConstant, "\"literal\"") &&
      fullyReduced(booleanConstant) && contains(booleanConstant, "true") &&
      fullyReduced(charConstant) && contains(charConstant, "'x'") &&
      fullyReduced(doubleConstant) && contains(doubleConstant, "1.25") &&
      fullyReduced(floatConstant) && contains(floatConstant, "1.25F") &&
      fullyReduced(constantMatchSeven) &&
      contains(constantMatchSeven, "\"constant-seven\"") &&
      !contains(constantMatchSeven, "constant-other") &&
      fullyReduced(constantMatchOther) &&
      contains(constantMatchOther, "\"constant-other\"") &&
      fullyReduced(constantBooleanFalse) &&
      contains(constantBooleanFalse, "\"constant-false\"") &&
      fullyReduced(constantPrecise) &&
      contains(constantPrecise, "demo.inlineerased.PreciseResult") &&
      contains(constantPrecise, ".preciseOnly") &&
      !contains(constantPrecise, "FallbackResult") &&
      fullyReduced(constantFallback) &&
      contains(constantFallback, "demo.inlineerased.FallbackResult") &&
      contains(constantFallback, ".fallbackOnly") &&
      !contains(constantFallback, "PreciseResult");
  const bool compiletimeErrorsErased =
      fullyReduced(acceptedSeven) &&
      contains(acceptedSeven, "\"accepted-seven\"") &&
      !contains(acceptedSeven, "ignored-seven") &&
      !contains(acceptedSeven, "NotImplementedError") &&
      fullyReduced(acceptedTrue) &&
      contains(acceptedTrue, "\"accepted-true\"") &&
      !contains(acceptedTrue, "ignored-true") &&
      !contains(acceptedTrue, "NotImplementedError") &&
      fullyReduced(acceptedCondition) &&
      contains(acceptedCondition, "\"accepted-condition\"") &&
      !contains(acceptedCondition, "ignored-condition") &&
      !contains(acceptedCondition, "NotImplementedError");
  const bool summonInlineReduced =
      fullyReduced(directSummoned) && contains(directSummoned, ".label") &&
      fullyReduced(inlineSummoned) && contains(inlineSummoned, ".label") &&
      fullyReduced(qualifiedSummoned) && contains(qualifiedSummoned, ".label") &&
      fullyReduced(aliasedSummoned) && contains(aliasedSummoned, ".label") &&
      fullyReduced(contextualSummoned) &&
      contains(contextualSummoned, ".label") && fullyReduced(nestedSummoned) &&
      contains(nestedSummoned, "\"nested:\"") &&
      contains(nestedSummoned, ".label") && fullyReduced(selectedSummoned) &&
      contains(selectedSummoned, ".label") &&
      !contains(selectedSummoned, "summon-skipped") &&
      fullyReduced(skippedSummoned) &&
      contains(skippedSummoned, "\"summon-skipped\"") &&
      !contains(skippedSummoned, ".label");
  const auto selectedLiteral = [&](std::string_view function,
                                   std::string_view selected,
                                   std::string_view unselected) {
    return !function.empty() && contains(function, selected) &&
           !contains(function, unselected) && !contains(function, " == ") &&
           !contains(function, "TypeKinds.");
  };
  const bool literalMatchesReduced =
      selectedLiteral(doubleExact, "\"double-exact\"", "double-fallback") &&
      selectedLiteral(doubleAlternative, "\"double-alternative\"",
                      "double-fallback") &&
      selectedLiteral(doubleFallback, "\"double-fallback\"", "double-exact") &&
      selectedLiteral(floatExact, "\"float-exact\"", "float-fallback") &&
      selectedLiteral(floatAlternative, "\"float-alternative\"",
                      "float-fallback") &&
      selectedLiteral(floatFallback, "\"float-fallback\"", "float-exact") &&
      selectedLiteral(doubleParameter, "\"double-parameter\"",
                      "double-parameter-fallback") &&
      selectedLiteral(floatParameter, "\"float-parameter\"",
                      "float-parameter-fallback") &&
      selectedLiteral(stableDouble, "\"stable-double\"",
                      "stable-double-fallback") &&
      selectedLiteral(stableFloat, "\"stable-float\"",
                      "stable-float-fallback") &&
      selectedLiteral(reducedFloatingCondition, "\"floating-condition\"",
                      "floating-condition-fallback") &&
      fullyReduced(floatingPrecise) &&
      contains(floatingPrecise, "demo.inlineerased.PreciseResult") &&
      contains(floatingPrecise, ".preciseOnly") &&
      !contains(floatingPrecise, "FallbackResult") &&
      selectedLiteral(stringAlpha, "\"string-alpha\"", "string-fallback") &&
      selectedLiteral(stringAlternative, "\"string-alternative\"",
                      "string-fallback") &&
      selectedLiteral(stringFallback, "\"string-fallback\"", "string-alpha") &&
      selectedLiteral(charX, "\"char-x\"", "char-fallback") &&
      selectedLiteral(charAlternative, "\"char-alternative\"", "char-fallback") &&
      selectedLiteral(charFallback, "\"char-fallback\"", "char-x") &&
      selectedLiteral(stringParameter, "\"string-parameter\"",
                      "string-parameter-fallback") &&
      selectedLiteral(charParameter, "\"char-parameter\"",
                      "char-parameter-fallback") &&
      selectedLiteral(stableString, "\"stable-string\"",
                      "stable-string-fallback") &&
      selectedLiteral(stableChar, "\"stable-char\"", "stable-char-fallback") &&
      fullyReduced(stringPrecise) &&
      contains(stringPrecise, "demo.inlineerased.PreciseResult") &&
      contains(stringPrecise, ".preciseOnly") &&
      !contains(stringPrecise, "FallbackResult");
  const bool guardedMatchesReduced =
      selectedLiteral(guardedSeven, "\"guarded-seven\"", "guarded-fallback") &&
      selectedLiteral(guardedFallback, "\"guarded-fallback\"",
                      "guarded-seven") &&
      selectedLiteral(guardedSkipped, "\"guarded-fallback\"",
                      "guarded-seven") &&
      !contains(guardedSkipped, "runtimeGuard") &&
      selectedLiteral(bindingGuardedSelected, "\"binding-\"",
                      "binding-fallback") &&
      contains(bindingGuardedSelected, "%selected") &&
      contains(bindingGuardedSelected, "intToString") &&
      selectedLiteral(bindingGuardedRejected, "\"binding-fallback\"",
                      "binding-eight") &&
      selectedLiteral(bindingGuardedNext, "\"binding-eight\"",
                      "binding-fallback") &&
      !contains(bindingGuardedNext, "runtimeGuard") &&
      selectedLiteral(bindingStringSelected, "\"bound\"",
                      "binding-string-fallback") &&
      selectedLiteral(bindingStringFallback, "\"binding-string-fallback\"",
                      "\"bound\"") &&
      selectedLiteral(bindingPrecise, "demo.inlineerased.PreciseResult",
                      "FallbackResult") &&
      contains(bindingPrecise, ".preciseOnly") &&
      selectedLiteral(typedBindingString, "\"typed\"",
                      "typed-binding-fallback") &&
      !contains(typedBindingString, "is-instance-of") &&
      selectedLiteral(typedBindingRejected, "\"typed-binding-fallback\"",
                      "%selected") &&
      !contains(typedBindingRejected, "is-instance-of") &&
      selectedLiteral(typedBindingSkipped, "\"typed-binding-fallback\"",
                      "typed-double:") &&
      !contains(typedBindingSkipped, "is-instance-of") &&
      !contains(typedBindingSkipped, "runtimeGuard") &&
      selectedLiteral(typedBindingDouble, "\"typed-double:\"",
                      "typed-binding-fallback") &&
      !contains(typedBindingDouble, "is-instance-of") &&
      !contains(typedBindingDouble, " >= ") &&
      contains(typedBindingDouble, "%selected") &&
      contains(typedBindingDouble, "doubleToString") &&
      selectedLiteral(typedBindingDoubleRejected,
                      "\"typed-binding-fallback\"", "typed-double:") &&
      !contains(typedBindingDoubleRejected, "is-instance-of") &&
      selectedLiteral(typedBindingPrecise,
                      "demo.inlineerased.PreciseResult", "FallbackResult") &&
      !contains(typedBindingPrecise, "is-instance-of") &&
      contains(typedBindingPrecise, ".preciseOnly") &&
      selectedLiteral(boundAlternativeString, "\"bound-alternative:\"",
                      "bound-alternative-fallback") &&
      contains(boundAlternativeString, "%selected") &&
      !contains(boundAlternativeString, "is-instance-of") &&
      !contains(boundAlternativeString, "String | Int") &&
      selectedLiteral(boundAlternativeInt, "\"bound-alternative:\"",
                      "bound-alternative-fallback") &&
      contains(boundAlternativeInt, "%selected") &&
      !contains(boundAlternativeInt, "is-instance-of") &&
      !contains(boundAlternativeInt, "String | Int") &&
      selectedLiteral(boundAlternativeRejected,
                      "\"bound-alternative-fallback\"", "%selected") &&
      selectedLiteral(boundAlternativeSkipped,
                      "\"bound-alternative-fallback\"", "%selected") &&
      !contains(boundAlternativeSkipped, "runtimeGuard") &&
      selectedLiteral(boundAlternativePrecise,
                      "demo.inlineerased.PreciseResult", "FallbackResult") &&
      contains(boundAlternativePrecise, "%selected") &&
      contains(boundAlternativePrecise, ".preciseOnly") &&
      !contains(boundAlternativePrecise, "is-instance-of") &&
      !contains(boundAlternativePrecise, "String | Int") &&
      selectedLiteral(singletonReady, "\"singleton-ready\"",
                      "singleton-fallback") &&
      selectedLiteral(singletonRejected, "\"singleton-fallback\"",
                      "singleton-ready") &&
      selectedLiteral(singletonWaiting, "\"singleton-known\"",
                      "singleton-fallback") &&
      selectedLiteral(singletonQualified, "\"singleton-known\"",
                      "singleton-fallback") &&
      selectedLiteral(singletonFallback, "\"singleton-fallback\"",
                      "singleton-known") &&
      selectedLiteral(singletonScalarFallback, "\"singleton-fallback\"",
                      "singleton-known") &&
      selectedLiteral(singletonSkipped, "\"singleton-fallback\"",
                      "singleton-ready") &&
      !contains(singletonSkipped, "runtimeGuard") &&
      selectedLiteral(singletonPrecise, "demo.inlineerased.PreciseResult",
                      "FallbackResult") &&
      contains(singletonPrecise, ".preciseOnly") &&
      selectedLiteral(singletonResultFallback,
                      "demo.inlineerased.FallbackResult", "PreciseResult") &&
      contains(singletonResultFallback, ".fallbackOnly") &&
      selectedLiteral(nullSelected, "\"null-selected\"", "null-fallback") &&
      selectedLiteral(nullRejected, "\"null-fallback\"", "null-selected") &&
      selectedLiteral(nullAlternative, "\"null-known\"",
                      "null-alternative-fallback") &&
      selectedLiteral(nullAlternativeNull, "\"null-known\"",
                      "null-alternative-fallback") &&
      selectedLiteral(nullReferenceFallback, "\"null-fallback\"",
                      "null-selected") &&
      !contains(nullReferenceFallback, "runtimeGuard") &&
      selectedLiteral(nullObjectFallback, "\"null-fallback\"",
                      "null-selected") &&
      selectedLiteral(nullScalarFallback, "\"null-fallback\"",
                      "null-selected") &&
      selectedLiteral(nullBindingSelected, "\"null-binding\"",
                      "null-binding-fallback") &&
      selectedLiteral(nullBindingFallback, "\"null-binding-fallback\"",
                      "\"null-binding\"") &&
      selectedLiteral(stableNullSelected, "\"stable-null\"",
                      "stable-null-fallback") &&
      selectedLiteral(nullPrecise, "demo.inlineerased.PreciseResult",
                      "FallbackResult") &&
      contains(nullPrecise, ".preciseOnly") &&
      selectedLiteral(nullResultFallback,
                      "demo.inlineerased.FallbackResult", "PreciseResult") &&
      contains(nullResultFallback, ".fallbackOnly") &&
      fullyReduced(alternativeString) &&
      contains(alternativeString, "\"alternative-string-int\"") &&
      !contains(alternativeString, "alternative-other") &&
      fullyReduced(alternativeInt) &&
      contains(alternativeInt, "\"alternative-string-int\"") &&
      !contains(alternativeInt, "alternative-other") &&
      fullyReduced(alternativeDouble) &&
      contains(alternativeDouble, "\"alternative-number\"") &&
      !contains(alternativeDouble, "alternative-other") &&
      fullyReduced(alternativeOther) &&
      contains(alternativeOther, "\"alternative-other\"") &&
      !contains(alternativeOther, "alternative-string-int") &&
      fullyReduced(guardedAlternative) &&
      contains(guardedAlternative, "\"guarded-alternative\"") &&
      !contains(guardedAlternative, "guarded-alternative-fallback") &&
      fullyReduced(rejectedAlternative) &&
      contains(rejectedAlternative, "\"guarded-alternative-fallback\"") &&
      !contains(rejectedAlternative, "\"guarded-alternative\"") &&
      fullyReduced(skippedAlternative) &&
      contains(skippedAlternative, "\"guarded-alternative-fallback\"") &&
      !contains(skippedAlternative, "runtimeGuard") &&
      fullyReduced(alternativePrecise) &&
      contains(alternativePrecise, "demo.inlineerased.PreciseResult") &&
      contains(alternativePrecise, ".preciseOnly") &&
      !contains(alternativePrecise, "FallbackResult");
  const bool valid =
      status == 0 &&
      outputText == "11\n42\n9000000000\nliteral\ntrue\nx\nconstant-seven\n"
                    "constant-other\nconstant-false\nconstant-precise\n"
                    "constant-fallback\ndouble-exact\ndouble-alternative\n"
                    "double-fallback\nfloat-exact\nfloat-alternative\n"
                    "float-fallback\ndouble-parameter\nfloat-parameter\n"
                    "stable-double\nstable-float\nfloating-condition\n"
                    "floating-precise\nguarded-seven\nguarded-fallback\n"
                    "guarded-fallback\nbinding-7\nbinding-fallback\n"
                    "binding-eight\nbound\nbinding-string-fallback\n"
                    "binding-precise\ntyped\ntyped-binding-fallback\n"
                    "typed-binding-fallback\ntyped-double:2.500000\n"
                    "typed-binding-fallback\ntyped-binding-precise\n"
                    "bound-alternative:text\nbound-alternative:7\n"
                    "bound-alternative-fallback\n"
                    "bound-alternative-fallback\n"
                    "bound-alternative-precise:7\n"
                    "singleton-ready\nsingleton-fallback\n"
                    "singleton-known\nsingleton-known\n"
                    "singleton-fallback\nsingleton-fallback\n"
                    "singleton-fallback\n"
                    "singleton-precise\nsingleton-result-fallback\n"
                    "null-selected\nnull-fallback\nnull-known\nnull-known\n"
                    "null-fallback\nnull-fallback\nnull-fallback\n"
                    "null-binding\nnull-binding-fallback\nstable-null\n"
                    "null-precise\nnull-result-fallback\n"
                    "accepted-seven\naccepted-true\n"
                    "accepted-condition\n"
                    "string\nint\nlong\nother\nalias-string\n"
                    "alias-other\n"
                    "precise\nfallback\nalternative-string-int\n"
                    "alternative-string-int\nalternative-number\n"
                    "alternative-other\nguarded-alternative\n"
                    "guarded-alternative-fallback\n"
                    "guarded-alternative-fallback\nalternative-precise\n"
                    "string-alpha\nstring-alternative\n"
                    "string-fallback\nchar-x\nchar-alternative\nchar-fallback\n"
                    "string-parameter\nchar-parameter\nstable-string\n"
                    "stable-char\nstring-precise\n"
                    "summoned-int\nsummoned-int\nsummoned-string\n"
                    "summoned-long\nsummoned-int\nnested:summoned-int\n"
                    "summoned-string\nsummon-skipped\nordinary-shadow\n" &&
      !invalid.ok &&
      contains(invalid.diagnosticsText, "failure: boom") &&
      contains(invalid.diagnosticsText, "aliased: boom") &&
      contains(invalid.diagnosticsText, "false branch") &&
      contains(invalid.diagnosticsText, "condition failed") &&
      contains(invalid.diagnosticsText, "direct failure") &&
      contains(invalid.diagnosticsText, "constant value failure") &&
      countOccurrences(
          invalid.diagnosticsText,
          "compiletime.error requires a compile-time constant String message") ==
          2 &&
      contains(invalid.diagnosticsText,
               "compiletime.error message must have type String") &&
      contains(invalid.diagnosticsText,
               "compiletime.error requires exactly one String argument") &&
      compiletimeErrorsErased &&
      countOccurrences(
          invalid.diagnosticsText,
          "no given value found for context parameter evidence of type") >= 3 &&
      contains(invalid.diagnosticsText,
               "Named [ Boolean ] required by summonInline") &&
      contains(invalid.diagnosticsText,
               "Named [ String ] required by summonInline") &&
      contains(invalid.diagnosticsText,
               "Named [ Char ] required by summonInline") &&
      contains(invalid.diagnosticsText,
               "ambiguous given values for context parameter evidence of type ") &&
      contains(invalid.diagnosticsText,
               "Named [ Long ] required by summonInline: firstLongNamed, "
               "secondLongNamed") &&
      contains(invalid.diagnosticsText,
               "summonInline requires exactly one type argument") &&
      summonInlineReduced &&
      literalMatchesReduced &&
      guardedMatchesReduced &&
      countOccurrences(invalid.diagnosticsText,
                       "constValue requires a constant singleton type") == 3 &&
      contains(invalid.diagnosticsText,
               "constValue requires exactly one type argument") &&
      constantsReduced &&
      countOccurrences(
          invalid.diagnosticsText,
          "erasedValue may only be used as the selector of an inline match") == 3 &&
      countOccurrences(
          invalid.diagnosticsText,
          "erasedValue inline match patterns cannot bind a runtime value") == 2 &&
      contains(invalid.diagnosticsText,
               "erasedValue requires exactly one type argument") &&
      countOccurrences(
          invalid.diagnosticsText,
          "inline match selector must be reducible from a compile-time value or "
          "static type") == 12 &&
      fullyReduced(stringName) && contains(stringName, "\"string\"") &&
      !contains(stringName, "\"other\"") && fullyReduced(intName) &&
      contains(intName, "\"int\"") && fullyReduced(longName) &&
      contains(longName, "\"long\"") && fullyReduced(otherName) &&
      contains(otherName, "\"other\"") && fullyReduced(aliasStringName) &&
      contains(aliasStringName, "\"alias-string\"") && fullyReduced(aliasOtherName) &&
      contains(aliasOtherName, "\"alias-other\"") && fullyReduced(preciseResult) &&
      contains(preciseResult, "demo.inlineerased.PreciseResult") &&
      contains(preciseResult, ".preciseOnly") &&
      !contains(preciseResult, "FallbackResult") && fullyReduced(fallbackResult) &&
      contains(fallbackResult, "demo.inlineerased.FallbackResult") &&
      contains(fallbackResult, ".fallbackOnly") &&
      !contains(fallbackResult, "PreciseResult");

  return valid ? 0
               : fail("inline erasedValue smoke test failed (output='" + outputText +
                      "', diagnostics='" + result.diagnosticsText +
                      "', invalid-diagnostics='" + invalid.diagnosticsText +
                      "', direct-constant='" + std::string(directConstant) +
                      "', int-constant='" + std::string(intConstant) +
                      "', long-constant='" + std::string(longConstant) +
                      "', string-constant='" + std::string(stringConstant) +
                      "', boolean-constant='" + std::string(booleanConstant) +
                      "', char-constant='" + std::string(charConstant) +
                      "', double-constant='" + std::string(doubleConstant) +
                      "', float-constant='" + std::string(floatConstant) +
                      "', constant-match-seven='" +
                      std::string(constantMatchSeven) +
                      "', constant-match-other='" +
                      std::string(constantMatchOther) +
                      "', constant-boolean-false='" +
                      std::string(constantBooleanFalse) +
                      "', constant-precise='" + std::string(constantPrecise) +
                      "', constant-fallback='" + std::string(constantFallback) +
                      "', double-exact='" + std::string(doubleExact) +
                      "', double-alternative='" +
                      std::string(doubleAlternative) +
                      "', double-fallback='" + std::string(doubleFallback) +
                      "', float-exact='" + std::string(floatExact) +
                      "', float-alternative='" +
                      std::string(floatAlternative) +
                      "', float-fallback='" + std::string(floatFallback) +
                      "', double-parameter='" + std::string(doubleParameter) +
                      "', float-parameter='" + std::string(floatParameter) +
                      "', stable-double='" + std::string(stableDouble) +
                      "', stable-float='" + std::string(stableFloat) +
                      "', floating-condition='" +
                      std::string(reducedFloatingCondition) +
                      "', floating-precise='" + std::string(floatingPrecise) +
                      "', guarded-seven='" + std::string(guardedSeven) +
                      "', guarded-fallback='" + std::string(guardedFallback) +
                      "', guarded-skipped='" + std::string(guardedSkipped) +
                      "', typed-binding-string='" +
                      std::string(typedBindingString) +
                      "', typed-binding-rejected='" +
                      std::string(typedBindingRejected) +
                      "', typed-binding-skipped='" +
                      std::string(typedBindingSkipped) +
                      "', typed-binding-double='" +
                      std::string(typedBindingDouble) +
                      "', typed-binding-double-rejected='" +
                      std::string(typedBindingDoubleRejected) +
                      "', typed-binding-precise='" +
                      std::string(typedBindingPrecise) +
                      "', bound-alternative-string='" +
                      std::string(boundAlternativeString) +
                      "', bound-alternative-int='" +
                      std::string(boundAlternativeInt) +
                      "', bound-alternative-rejected='" +
                      std::string(boundAlternativeRejected) +
                      "', bound-alternative-skipped='" +
                      std::string(boundAlternativeSkipped) +
                      "', bound-alternative-precise='" +
                      std::string(boundAlternativePrecise) +
                      "', singleton-ready='" + std::string(singletonReady) +
                      "', singleton-rejected='" +
                      std::string(singletonRejected) +
                      "', singleton-waiting='" +
                      std::string(singletonWaiting) +
                      "', singleton-qualified='" +
                      std::string(singletonQualified) +
                      "', singleton-fallback='" +
                      std::string(singletonFallback) +
                      "', singleton-scalar-fallback='" +
                      std::string(singletonScalarFallback) +
                      "', singleton-skipped='" +
                      std::string(singletonSkipped) +
                      "', singleton-precise='" +
                      std::string(singletonPrecise) +
                      "', singleton-result-fallback='" +
                      std::string(singletonResultFallback) +
                      "', accepted-seven='" + std::string(acceptedSeven) +
                      "', accepted-true='" + std::string(acceptedTrue) +
                      "', accepted-condition='" +
                      std::string(acceptedCondition) +
                      "', string-name='" + std::string(stringName) + "', int-name='" +
                      std::string(intName) + "', long-name='" + std::string(longName) +
                      "', other-name='" + std::string(otherName) +
                      "', alias-string-name='" + std::string(aliasStringName) +
                      "', alias-other-name='" + std::string(aliasOtherName) +
                      "', precise-result='" + std::string(preciseResult) +
                      "', fallback-result='" + std::string(fallbackResult) +
                      "', alternative-string='" +
                      std::string(alternativeString) +
                      "', alternative-int='" + std::string(alternativeInt) +
                      "', alternative-double='" +
                      std::string(alternativeDouble) +
                      "', alternative-other='" + std::string(alternativeOther) +
                      "', guarded-alternative='" +
                      std::string(guardedAlternative) +
                      "', rejected-alternative='" +
                      std::string(rejectedAlternative) +
                      "', skipped-alternative='" +
                      std::string(skippedAlternative) +
                      "', alternative-precise='" +
                      std::string(alternativePrecise) +
                      "', string-alpha='" + std::string(stringAlpha) +
                      "', string-alternative='" + std::string(stringAlternative) +
                      "', string-fallback='" + std::string(stringFallback) +
                      "', char-x='" + std::string(charX) +
                      "', char-alternative='" + std::string(charAlternative) +
                      "', char-fallback='" + std::string(charFallback) +
                      "', string-parameter='" + std::string(stringParameter) +
                      "', char-parameter='" + std::string(charParameter) +
                      "', stable-string='" + std::string(stableString) +
                      "', stable-char='" + std::string(stableChar) +
                      "', string-precise='" + std::string(stringPrecise) + "')");
}

} // namespace

int runSmokeTests6() {
  return smokeInlineErasedValue();
}
