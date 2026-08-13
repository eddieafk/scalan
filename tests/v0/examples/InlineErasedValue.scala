package demo.inlineerased

import scala.compiletime.erasedValue

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

object TypeDefaults {
  transparent inline def nameOf[T]: String =
    inline erasedValue[T] match {
      case _: String => "string"
      case _: Int => "int"
      case _: Long => "long"
      case _ => "other"
    }

  transparent inline def resultFor[T]: ErasedResult =
    inline scala.compiletime.erasedValue[T] match {
      case _: String => new PreciseResult("precise")
      case _ => new FallbackResult("fallback")
    }

  transparent inline def alternativeNameOf[T]: String =
    inline erasedValue[T] match {
      case _: String | _: Int => "string-or-int"
      case _: Long | _: Double => "numeric"
      case _ => "other"
    }

  transparent inline def guardedNameOf[T](inline enabled: Boolean): String =
    inline erasedValue[T] match {
      case _: String | _: Int if enabled => "guarded"
      case _ => "guarded-fallback"
    }

  inline def bindingGuardedChoice(
      inline value: Int,
      inline enabled: Boolean): String =
    inline value match {
      case selected if selected == 7 && enabled => "binding-" + selected.toString
      case 8 => "binding-eight"
      case _ => "binding-fallback"
    }

  inline def bindingGuardedString(inline value: String): String =
    inline value match {
      case selected if selected == "bound" => selected
      case _ => "binding-string-fallback"
    }

  inline def typedBindingGuardedChoice(value: Any): String =
    inline value match {
      case selected: String if selected == "typed" => selected
      case selected: Double if selected >= 2.0 =>
        "typed-double:" + selected.toString
      case _ => "typed-binding-fallback"
    }

  inline def boundAlternativeChoice(value: Any): String =
    inline value match {
      case selected: String | selected: Int =>
        "bound-alternative:" + selected.toString
      case _ => "bound-alternative-fallback"
    }

  inline def singletonChoice(value: Any): String =
    inline value match {
      case Ready => "singleton-ready"
      case Waiting | InlineState.Stopped => "singleton-known"
      case _ => "singleton-fallback"
    }

  inline def nullChoice(value: Any): String =
    inline value match
      case null => "null-selected"
      case _ => "null-fallback"

  inline def nullAlternativeChoice(value: Any): String =
    inline value match
      case "text" | null => "null-or-text"
      case _ => "null-alternative-fallback"

  transparent inline def nullResult(value: Any): ErasedResult =
    inline value match
      case null => new PreciseResult("null-precise")
      case _ => new FallbackResult("null-fallback")

  transparent inline def alternativeResultFor[T]: ErasedResult =
    inline erasedValue[T] match {
      case _: String | _: Int => new PreciseResult("alternative-precise")
      case _ => new FallbackResult("alternative-fallback")
    }
}

object Main {
  def main(args: Array[String]): Unit = {
    println(TypeDefaults.nameOf[String])
    println(TypeDefaults.nameOf[Int])
    println(TypeDefaults.nameOf[Long])
    println(TypeDefaults.nameOf[Other])
    println(TypeDefaults.resultFor[String].preciseOnly())
    println(TypeDefaults.resultFor[Other].fallbackOnly())
    println(TypeDefaults.alternativeNameOf[String])
    println(TypeDefaults.alternativeNameOf[Int])
    println(TypeDefaults.alternativeNameOf[Double])
    println(TypeDefaults.alternativeNameOf[Other])
    println(TypeDefaults.guardedNameOf[String](true))
    println(TypeDefaults.guardedNameOf[Int](false))
    println(TypeDefaults.bindingGuardedChoice(7, true))
    println(TypeDefaults.bindingGuardedChoice(7, false))
    println(TypeDefaults.bindingGuardedChoice(8, true))
    println(TypeDefaults.bindingGuardedString("bound"))
    println(TypeDefaults.bindingGuardedString("other"))
    println(TypeDefaults.typedBindingGuardedChoice("typed"))
    println(TypeDefaults.typedBindingGuardedChoice(2.5))
    println(TypeDefaults.typedBindingGuardedChoice(1))
    println(TypeDefaults.boundAlternativeChoice("text"))
    println(TypeDefaults.boundAlternativeChoice(7))
    println(TypeDefaults.boundAlternativeChoice(2.5))
    println(TypeDefaults.singletonChoice(Ready))
    println(TypeDefaults.singletonChoice(Waiting))
    println(TypeDefaults.singletonChoice(InlineState.Stopped))
    println(TypeDefaults.singletonChoice(new OtherSignal))
    println(TypeDefaults.singletonChoice(7))
    println(TypeDefaults.nullChoice(null))
    println(TypeDefaults.nullChoice(new OtherSignal))
    println(TypeDefaults.nullAlternativeChoice(null))
    println(TypeDefaults.nullAlternativeChoice("text"))
    println(TypeDefaults.nullResult(null).preciseOnly())
    println(TypeDefaults.nullResult(new OtherSignal).fallbackOnly())
    println(TypeDefaults.alternativeResultFor[Int].preciseOnly())
  }
}
