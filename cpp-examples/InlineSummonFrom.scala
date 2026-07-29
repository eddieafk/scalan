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

  inline def decorated[A](value: String): String =
    summonFrom {
      case found: Named[A] =>
        prefix + found.label() + ":" + value + ":" + value
      case _ => "fallback:" + value + ":" + value
    }

  inline def nestedValue[A](value: String): String =
    "nested-value:" + decorated[A](value)

  inline def passed[A](value: A): A = value
}

object Main {
  given intNamed: Named[Int] = new NamedValue[Int]("member-int")

  def nextValue(): String = {
    println("effect")
    "value"
  }

  def main(args: Array[String]): Unit = {
    println(Selectors.selected[Int])

    given localString: Named[String] =
      new NamedValue[String]("local-string")
    println(Selectors.selected[String]())

    println(Selectors.selected[Boolean])
    println(Selectors.nested[Int])
    println(Selectors.decorated[Int](nextValue()))
    println(Selectors.decorated[Boolean]("bool"))
    println(Selectors.nestedValue[Int]("nested"))
    println(Selectors.passed[Int](42))
    println(Selectors.passed[String]("forty-two"))
  }
}
