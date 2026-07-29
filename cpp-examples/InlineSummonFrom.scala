package examples.inlinesummonfrom

trait Named[A] {
  def label(): String
}

class NamedValue[A](val value: String) extends Named[A] {
  def label(): String = value
}

object Selectors {
  val prefix: String = "selected:"

  inline def selected[A]: String =
    summonFrom {
      case found: Named[A] => prefix + found.label()
      case _ => "fallback"
    }

  inline def nested[A]: String = "nested:" + selected[A]
}

object Main {
  given intNamed: Named[Int] = new NamedValue[Int]("member-int")

  def main(args: Array[String]): Unit = {
    println(Selectors.selected[Int])

    given localString: Named[String] =
      new NamedValue[String]("local-string")
    println(Selectors.selected[String]())

    println(Selectors.selected[Boolean])
    println(Selectors.nested[Int])
  }
}
