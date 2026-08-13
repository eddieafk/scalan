package examples.compiletimesummoninline

import scala.compiletime.summonInline
import scala.compiletime.{summonInline => inlineSummon}

trait Named[A] {
  def label(): String
}

class NamedValue[A](val value: String) extends Named[A] {
  def label(): String = value
}

class Other

object Requirements {
  inline def labelOf[A]: String = summonInline[Named[A]].label()

  inline def aliasedLabelOf[A]: String = inlineSummon[Named[A]].label()

  inline def qualifiedLabelOf[A]: String =
    scala.compiletime.summonInline[Named[A]].label()

  inline def selected[A](inline required: Boolean): String =
    inline if (required) summonInline[Named[A]].label()
    else "not-required"
}

object Main {
  given intNamed: Named[Int] = new NamedValue[Int]("int")
  given stringNamed: Named[String] = new NamedValue[String]("string")

  def main(args: Array[String]): Unit = {
    println(Requirements.labelOf[Int])
    println(Requirements.aliasedLabelOf[String])
    println(Requirements.qualifiedLabelOf[Int])
    println(Requirements.selected[Other](false))
  }
}
