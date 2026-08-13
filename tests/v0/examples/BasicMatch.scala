package demo.examples

object BasicMatch {
  def number(value: Int): String = value match {
    case 0 => "zero"
    case 1 => "one"
    case _ => "many"
  }

  def word(value: String): Int = value match {
    case "yes" => 1
    case "no" => 0
    case _ => -1
  }

  def sign(value: Int): Int = value match {
    case 0 => 0
    case positive if positive > 0 => 1
    case _ => -1
  }

  def echo(value: String): String = value match {
    case "yes" => "affirmative"
    case other => other
  }

  def preference(value: Int, prefer: Boolean): String = value match {
    case _ if prefer => "preferred"
    case _ => "fallback"
  }

  def band(value: Int): String = value match {
    case 0 | 1 => "low"
    case 2 | 3 if value == 3 => "guarded"
    case _ => "other"
  }

  def selected: Int = {
    println("selector")
    1
  }

  def main = {
    println(number(0))
    println(number(3))
    println(word("yes"))
    println(word("other"))
    println(sign(8))
    println(sign(-3))
    println(echo("maybe"))
    println(preference(7, true))
    println(preference(7, false))
    println(band(1))
    println(band(2))
    println(band(3))
    println(number(selected))
  }
}
