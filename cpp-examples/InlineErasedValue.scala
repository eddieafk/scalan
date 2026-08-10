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
    println(TypeDefaults.alternativeResultFor[Int].preciseOnly())
  }
}
