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
}

object Main {
  def main(args: Array[String]): Unit = {
    println(TypeDefaults.nameOf[String])
    println(TypeDefaults.nameOf[Int])
    println(TypeDefaults.nameOf[Long])
    println(TypeDefaults.nameOf[Other])
    println(TypeDefaults.resultFor[String].preciseOnly())
    println(TypeDefaults.resultFor[Other].fallbackOnly())
  }
}
