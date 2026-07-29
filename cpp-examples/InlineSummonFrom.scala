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

  inline def inferredSelected[A](value: A): String =
    summonFrom {
      case found: Named[A] => prefix + found.label()
      case _ => "inferred-fallback"
    }

  inline def inferredNull[A](): A = null

  inline def contextual[A]()(using named: Named[A]): String =
    summonFrom {
      case found: Named[A] => prefix + found.label()
      case _ => "contextual-fallback"
    }

  inline def contextualValue[A](value: String)(using named: Named[A]): String =
    prefix + named.label() + ":" + named.label() + ":" + value + ":" + value

  inline def nestedContextual[A](value: String)(using named: Named[A]): String =
    "nested-context:" + contextualValue[A](value)

  inline def inferredContextual[A](value: A)(using named: Named[A]): String =
    prefix + named.label()

  inline def inferredFromContext[A]()(using named: Named[A]): String =
    prefix + named.label()
}

class InstanceSelectors(val instancePrefix: String) {
  inline def selected[A](): String =
    summonFrom {
      case found: Named[A] => instancePrefix + found.label()
      case _ => instancePrefix + "fallback"
    }

  inline def contextualValue[A](value: String)(using named: Named[A]): String =
    instancePrefix + named.label() + ":" + named.label() + ":" + value + ":" + value

  inline def nestedContextual[A](value: String)(using named: Named[A]): String =
    "instance-nested:" + contextualValue[A](value)

  inline def inferred[A](value: A): String = instancePrefix + "inferred"

  inline def thisPrefix[A](): String = this.instancePrefix
}

trait TraitInstanceSelectors {
  def traitPrefix(): String

  inline def traitSelected[A](): String =
    summonFrom {
      case found: Named[A] => traitPrefix() + found.label()
      case _ => traitPrefix() + "fallback"
    }
}

class TraitInstanceSelectorsValue(val value: String) extends TraitInstanceSelectors {
  def traitPrefix(): String = value
}

object Main {
  given intNamed: Named[Int] = new NamedValue[Int]("member-int")
  val instances: InstanceSelectors = new InstanceSelectors("instance:")
  val traitInstances: TraitInstanceSelectors =
    new TraitInstanceSelectorsValue("trait-instance:")

  def nextValue(): String = {
    println("effect")
    "value"
  }

  def nextContextValue(): String = {
    println("context-effect")
    "context-value"
  }

  def nextInstanceValue(): String = {
    println("instance-effect")
    "instance-value"
  }

  def main(args: Array[String]): Unit = {
    println(Selectors.selected[Int])

    {
      given localString: Named[String] =
        new NamedValue[String]("local-string")
      println(Selectors.selected[String]())
    }

    println(Selectors.selected[Boolean])
    println(Selectors.nested[Int])
    println(Selectors.decorated[Int](nextValue()))
    println(Selectors.decorated[Boolean]("bool"))
    println(Selectors.nestedValue[Int]("nested"))
    println(Selectors.passed[Int](42))
    println(Selectors.passed[String]("forty-two"))
    println(Selectors.inferredSelected(7))
    println(Selectors.inferredSelected(true))

    {
      given inferredString: Named[String] =
        new NamedValue[String]("inferred-local")
      println(Selectors.inferredSelected("text"))
    }
    println(Selectors.passed(43))
    println(Selectors.passed("forty-three"))
    val inferredNull: String = Selectors.inferredNull()
    println(inferredNull == null)
    println(Selectors.contextual[Int]())

    {
      given contextualString: Named[String] =
        new NamedValue[String]("contextual-local")
      println(Selectors.contextual[String]())
    }
    println(Selectors.contextualValue[Int](nextContextValue()))
    println(
      Selectors.contextual[String]()(using
        new NamedValue[String]("explicit-context")))
    println(Selectors.nestedContextual[Int]("nested"))
    println(Selectors.inferredContextual(9))
    println(Selectors.inferredFromContext())
    println(instances.selected[Int]())
    println(instances.selected[Boolean]())
    println(instances.contextualValue[Int](nextInstanceValue()))
    println(
      instances.contextualValue[String]("explicit")(using
        new NamedValue[String]("explicit-instance")))
    println(instances.nestedContextual[Int]("nested"))
    println(instances.inferred(10))
    println(instances.thisPrefix[Int]())

    {
      val localInstances: InstanceSelectors =
        new InstanceSelectors("local-instance:")
      println(localInstances.selected[Int]())
    }
    println(traitInstances.traitSelected[Int]())
  }
}
