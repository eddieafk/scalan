package demo.inlineconstant

import scala.compiletime.constValue
import scala.compiletime.{constValue => constant}
import scala.compiletime.{error => compiletimeError}

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

  transparent inline def stringLabelOf[S]: String =
    inline constValue[S] match {
      case "alpha" => "string-alpha"
      case "beta" | "line\nbreak" => "string-alternative"
      case _ => "string-fallback"
    }

  transparent inline def charLabelOf[C]: String =
    inline constValue[C] match {
      case 'x' => "char-x"
      case 'y' | '\n' => "char-alternative"
      case _ => "char-fallback"
    }

  inline def stringParameterLabel(inline value: String): String =
    inline value match {
      case "parameter" => "string-parameter"
      case _ => "string-parameter-fallback"
    }

  inline def charParameterLabel(inline value: Char): String =
    inline value match {
      case 'p' => "char-parameter"
      case _ => "char-parameter-fallback"
    }

  inline def requireSeven[N](inline message: String): String =
    inline constValue[N] match {
      case 7 => "accepted-seven"
      case _ => compiletimeError("requireSeven: " + message)
    }

  inline def requireTrue[B](inline message: String): String =
    inline constValue[B] match {
      case true => "accepted-true"
      case _ => scala.compiletime.error(message)
    }

  inline def requireCondition(
      inline condition: Boolean,
      inline message: String): String =
    inline if (condition) "accepted-condition"
    else compiletimeError(message)
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
    println(Constants.stringLabelOf["alpha"])
    println(Constants.stringLabelOf["line\nbreak"])
    println(Constants.stringLabelOf["other"])
    println(Constants.charLabelOf['x'])
    println(Constants.charLabelOf['\n'])
    println(Constants.charLabelOf['q'])
    println(Constants.stringParameterLabel("parameter"))
    println(Constants.charParameterLabel('p'))
    println(Constants.requireSeven[7]("not seven"))
    println(Constants.requireTrue[true]("not true"))
    println(Constants.requireCondition(true, "condition failed"))
  }
}
