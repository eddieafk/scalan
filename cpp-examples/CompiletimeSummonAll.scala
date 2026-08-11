package examples.compiletimesummonall

import scala.compiletime.summonAll
import scala.compiletime.{summonAll => collectAll}

trait Named[A] {
  def label(): String
}

class NamedValue[A](val value: String) extends Named[A] {
  def label(): String = value
}

class Token(val value: String)

object Token {
  given default: Token = new Token("companion-token")
}

object Evidence {
  given intNamed: Named[Int] = new NamedValue[Int]("int")
  given stringNamed: Named[String] = new NamedValue[String]("string")
  given longNamed: Named[Long] = new NamedValue[Long]("long")
  given number: Int = 73
}

import Evidence.given

object Values {
  type PairEvidence = (Named[Int], Named[String])

  inline def collect[T <: Tuple] = summonAll[T]

  def directFirst: String = summonAll[PairEvidence]._1.label()
  def directSecond: String = summonAll[PairEvidence]._2.label()
  def aliased: String = collectAll[Tuple1[Named[String]]]._1.label()
  def qualified: String =
    scala.compiletime.summonAll[Tuple1[Named[Long]]]._1.label()
  def specialized: String = collect[Tuple1[Named[Int]]]._1.label()
  def primitive: Int = summonAll[Tuple1[Int]]._1
  def companionToken: Token = summonAll[Tuple1[Token]]._1
  def companion: String = companionToken.value
  def empty: Boolean = summonAll[EmptyTuple] == summonAll[EmptyTuple]
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Values.directFirst)
    println(Values.directSecond)
    println(Values.aliased)
    println(Values.qualified)
    println(Values.specialized)
    println(Values.primitive)
    println(Values.companion)
    println(Values.empty)
  }
}
