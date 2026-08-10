package examples.compiletimeconstvalueopt

import scala.compiletime.constValueOpt
import scala.compiletime.{constValueOpt => optionalConstant}

object Values {
  transparent inline def optional[T] = constValueOpt[T]

  def integer: Int =
    constValueOpt[42].asInstanceOf[Some[42]].value

  def text: String =
    optionalConstant["present"].asInstanceOf[Some["present"]].value

  def qualified: Boolean =
    scala.compiletime.constValueOpt[true]
      .asInstanceOf[Some[true]]
      .value

  def specialized: String =
    optional["specialized"].asInstanceOf[Some["specialized"]].value

  def missing: Boolean = constValueOpt[String] == None
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Values.integer)
    println(Values.text)
    println(Values.qualified)
    println(Values.specialized)
    println(Values.missing)
  }
}
