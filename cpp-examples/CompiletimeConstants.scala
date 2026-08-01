package demo.inlineconstant

import scala.compiletime.constValue
import scala.compiletime.{constValue => constant}

trait ConstantResult {
  def text(): String
}

class SevenResult(val value: String) extends ConstantResult {
  def text(): String = value
  def sevenOnly(): String = value
}

class OtherResult(val value: String) extends ConstantResult {
  def text(): String = value
  def otherOnly(): String = value
}

object Constants {
  transparent inline def valueOf[T] = constValue[T]

  transparent inline def qualifiedValueOf[T] =
    scala.compiletime.constValue[T]

  transparent inline def labelOf[N]: String =
    inline constant[N] match {
      case 0 => "zero"
      case 7 => "seven"
      case _ => "other"
    }

  transparent inline def resultFor[N]: ConstantResult =
    inline constValue[N] match {
      case 7 => new SevenResult("precise-seven")
      case _ => new OtherResult("precise-other")
    }
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Constants.valueOf[42])
    println(Constants.qualifiedValueOf["literal"])
    println(Constants.valueOf[true])
    println(Constants.valueOf['x'])
    println(Constants.labelOf[7])
    println(Constants.labelOf[9])
    println(Constants.resultFor[7].sevenOnly())
    println(Constants.resultFor[9].otherOnly())
  }
}
