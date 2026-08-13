package examples.compiletimeconstvaluetuple

import scala.compiletime.constValueTuple
import scala.compiletime.{constValueTuple => materializeConstants}

object Values {
  type NamedConstants = (99, "aliased type")

  transparent inline def materialize[T <: Tuple] = constValueTuple[T]

  def integer: Int = constValueTuple[(42, "present")]._1

  def text: String = constValueTuple[(42, "present")]._2

  def aliased: Boolean = materializeConstants[(true, 'x')]._1

  def aliasedType: String = constValueTuple[NamedConstants]._2

  def qualified: Char =
    scala.compiletime.constValueTuple[(true, 'x')]._2

  def specialized: String = materialize[(7, "specialized")]._2

  def triple: Long = constValueTuple[(1, 9000000000L, 2.5)]._2

  def empty: Boolean =
    constValueTuple[EmptyTuple] == constValueTuple[EmptyTuple]
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Values.integer)
    println(Values.text)
    println(Values.aliased)
    println(Values.aliasedType)
    println(Values.qualified)
    println(Values.specialized)
    println(Values.triple)
    println(Values.empty)
  }
}
