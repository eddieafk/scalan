package examples.inlinesummonfrom

trait Named[A] {
  def label(): String
}

class NamedValue[A](val value: String) extends Named[A] {
  def label(): String = value
}

trait TransparentResult {
  def text(): String
}

class PreciseResult(val value: String) extends TransparentResult {
  def text(): String = value
  def preciseOnly(): String = value
}

class FallbackResult(val value: String) extends TransparentResult {
  def text(): String = value
  def fallbackOnly(): String = value
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

  inline def curried[A](first: String)(second: String): String =
    summonFrom {
      case found: Named[A] =>
        prefix + found.label() + ":" + first + ":" + first + ":" + second + ":" + second
      case _ => "curried-fallback"
    }

  inline def nestedCurried[A](first: String)(second: String): String =
    "nested-curried:" + curried[A](first)(second)

  inline def curriedContextual[A](first: String)(second: String)(
      using named: Named[A]): String =
    prefix + named.label() + ":" + first + ":" + second

  inline def inferredCurried[A](value: A)(suffix: String)(
      using named: Named[A]): String =
    prefix + named.label() + ":" + suffix

  transparent inline def refined[A](): TransparentResult =
    summonFrom {
      case found: Named[A] =>
        new PreciseResult("transparent:" + found.label())
      case _ => new FallbackResult("transparent-fallback")
    }

  transparent inline def nestedRefined[A](): TransparentResult =
    refined[A]()

  transparent inline def contextualRefined[A]()(using
      named: Named[A]): TransparentResult =
    new PreciseResult("context-transparent:" + named.label())

  transparent inline def inferredRefined[A](value: A)(using
      named: Named[A]): TransparentResult =
    new PreciseResult("inferred-transparent:" + named.label())

  inline def nonGenericConstant: String = "non-generic-constant"

  inline def nonGenericDecorated(value: String): String =
    "non-generic:" + value + ":" + value

  inline def nestedNonGeneric(value: String): String =
    "nested-" + nonGenericDecorated(value)

  inline def nonGenericContextual()(using named: Named[Int]): String =
    "non-generic-context:" + named.label()

  inline def nonGenericCurried(first: String)(second: String): String =
    "non-generic-curried:" + first + ":" + second

  transparent inline def nonGenericRefined(): TransparentResult =
    new PreciseResult("non-generic-transparent")

  inline def choose(inline condition: Boolean, value: String): String =
    inline if (condition) {
      "inline-true:" + value + ":" + value
    } else {
      "inline-false:" + value + ":" + value
    }

  inline def nestedChoose(inline condition: Boolean, value: String): String =
    "nested-" + choose(condition, value)

  inline def negatedChoose(inline condition: Boolean): String =
    inline if (!condition) "inline-negated" else "inline-original"

  inline def contextualChoose(inline condition: Boolean)(using
      named: Named[Int]): String =
    inline if (condition) {
      "inline-context:" + named.label()
    } else {
      "inline-context-false"
    }

  inline def curriedChoose(inline condition: Boolean)(value: String): String =
    inline if (condition) {
      "inline-curried-true:" + value
    } else {
      "inline-curried-false:" + value
    }

  inline def genericChoose[A](inline condition: Boolean): String =
    inline if (condition) {
      summonFrom {
        case found: Named[A] => "inline-generic:" + found.label()
        case _ => "inline-generic-fallback"
      }
    } else {
      "inline-generic-false"
    }

  transparent inline def refinedChoice(
      inline condition: Boolean): TransparentResult =
    inline if (condition) {
      new PreciseResult("inline-transparent-true")
    } else {
      new FallbackResult("inline-transparent-false")
    }

  inline def recursiveChoose(inline first: Boolean, inline second: Boolean,
      value: String): String =
    inline if (first) {
      "recursive-step:" + recursiveChoose(second, false, value)
    } else {
      "recursive-done:" + value + ":" + value
    }

  inline def recursiveContextual[A](inline continue: Boolean)(using
      named: Named[A]): String =
    inline if (continue) {
      "recursive-context:" + recursiveContextual[A](false)
    } else {
      summonFrom {
        case found: Named[A] => "recursive-context-done:" + found.label()
        case _ => "recursive-context-fallback"
      }
    }

  transparent inline def recursiveRefined(
      inline continue: Boolean): TransparentResult =
    inline if (continue) {
      recursiveRefined(false)
    } else {
      new PreciseResult("recursive-transparent")
    }
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

  inline def curried[A](first: String)(second: String)(
      using named: Named[A]): String =
    instancePrefix + named.label() + ":" + first + ":" + second

  inline def nonGeneric(value: String): String =
    instancePrefix + value + ":" + value

  inline def nonGenericConstant: String = instancePrefix + "constant"

  inline def choose(inline condition: Boolean, value: String): String =
    inline if (condition) {
      instancePrefix + "inline-true:" + value
    } else {
      instancePrefix + "inline-false:" + value
    }
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

class GenericInstanceSelectors[Owner](val ownerPrefix: String) {
  inline def selected[A](): String =
    summonFrom {
      case ownerNamed: Named[Owner] =>
        summonFrom {
          case methodNamed: Named[A] =>
            ownerPrefix + ownerNamed.label() + ":" + methodNamed.label()
          case _ => ownerPrefix + "method-fallback"
        }
      case _ => ownerPrefix + "owner-fallback"
    }

  inline def contextual[A](value: A)(
      using ownerNamed: Named[Owner], methodNamed: Named[A]): String =
    ownerPrefix + ownerNamed.label() + ":" + methodNamed.label()

  inline def ownerPassed[A](ownerValue: Owner, methodValue: A): Owner =
    ownerValue
}

trait GenericTraitInstanceSelectors[Owner] {
  def genericTraitPrefix(): String

  inline def selected[A](): String =
    summonFrom {
      case ownerNamed: Named[Owner] =>
        summonFrom {
          case methodNamed: Named[A] =>
            genericTraitPrefix() + ownerNamed.label() + ":" + methodNamed.label()
          case _ => genericTraitPrefix() + "method-fallback"
        }
      case _ => genericTraitPrefix() + "owner-fallback"
    }
}

class GenericTraitInstanceSelectorsValue(val value: String)
    extends GenericTraitInstanceSelectors[String] {
  def genericTraitPrefix(): String = value
}

class InstanceHolder(val value: InstanceSelectors)

object Main {
  given intNamed: Named[Int] = new NamedValue[Int]("member-int")
  val instances: InstanceSelectors = new InstanceSelectors("instance:")
  val traitInstances: TraitInstanceSelectors =
    new TraitInstanceSelectorsValue("trait-instance:")
  val genericInstances: GenericInstanceSelectors[String] =
    new GenericInstanceSelectors[String]("generic:")
  val genericTraitInstances: GenericTraitInstanceSelectors[String] =
    new GenericTraitInstanceSelectorsValue("generic-trait:")
  val inheritedGenericTraitInstances: GenericTraitInstanceSelectorsValue =
    new GenericTraitInstanceSelectorsValue("inherited-trait:")
  val instanceHolder: InstanceHolder =
    new InstanceHolder(new InstanceSelectors("held-instance:"))

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

  def nextInstances(): InstanceSelectors = {
    println("receiver-effect")
    new InstanceSelectors("effectful-instance:")
  }

  def nextTraitInstances(): TraitInstanceSelectors = {
    println("trait-receiver-effect")
    new TraitInstanceSelectorsValue("effectful-trait:")
  }

  def nextFirst(): String = {
    println("first-effect")
    "first"
  }

  def nextSecond(): String = {
    println("second-effect")
    "second"
  }

  def nextNonGeneric(): String = {
    println("non-generic-effect")
    "plain"
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

    {
      given ownerString: Named[String] =
        new NamedValue[String]("owner-string")
      println(genericInstances.selected[Int]())
      println(genericInstances.contextual(11))
    }
    println(genericInstances.ownerPassed("owner-value", 12))

    {
      given traitOwnerString: Named[String] =
        new NamedValue[String]("trait-owner")
      println(genericTraitInstances.selected[Int]())
    }

    {
      given inheritedOwnerString: Named[String] =
        new NamedValue[String]("inherited-owner")
      println(inheritedGenericTraitInstances.selected[Int]())
    }

    println(nextInstances().selected[Int]())
    println(nextInstances().contextualValue[Int](nextInstanceValue()))
    println(new InstanceSelectors("constructed-instance:").selected[Int]())
    println(instanceHolder.value.selected[Int]())
    println(nextTraitInstances().traitSelected[Int]())
    println(Selectors.curried[Int](nextFirst())(nextSecond()))
    println(Selectors.nestedCurried[Int]("left")("right"))
    println(
      Selectors.curriedContextual[Int]("context-first")("context-second"))
    println(
      Selectors.curriedContextual[String]("explicit-first")("explicit-second")(using
        new NamedValue[String]("explicit-curried")))
    println(Selectors.inferredCurried(21)("inferred-second"))
    println(nextInstances().curried[Int](nextFirst())(nextSecond()))
    println(Selectors.refined[Int]().preciseOnly())
    println(Selectors.refined[Boolean]().fallbackOnly())
    println(Selectors.nestedRefined[Int]().preciseOnly())
    println(Selectors.contextualRefined[Int]().preciseOnly())
    println(
      Selectors.contextualRefined[String]()(using
        new NamedValue[String]("explicit-transparent")).preciseOnly())
    println(Selectors.inferredRefined(22).preciseOnly())
    println(Selectors.nonGenericConstant)
    println(Selectors.nonGenericDecorated(nextNonGeneric()))
    println(Selectors.nestedNonGeneric("nested"))
    println(Selectors.nonGenericContextual())
    println(Selectors.nonGenericCurried(nextFirst())(nextSecond()))
    println(nextInstances().nonGeneric("receiver"))
    println(nextInstances().nonGenericConstant)
    println(Selectors.nonGenericRefined().preciseOnly())
    println(Selectors.choose(true, nextNonGeneric()))
    println(Selectors.choose(false, "cold"))
    println(Selectors.nestedChoose(!false, "nested"))
    println(Selectors.negatedChoose(false))
    println(Selectors.contextualChoose(true))
    println(Selectors.curriedChoose(false)(nextValue()))
    println(Selectors.genericChoose[Int](true))
    println(nextInstances().choose(false, "chosen"))
    println(Selectors.refinedChoice(true).preciseOnly())
    println(Selectors.refinedChoice(false).fallbackOnly())
    println(Selectors.recursiveChoose(true, true, nextNonGeneric()))
    println(Selectors.recursiveContextual[Int](true))
    println(Selectors.recursiveRefined(true).preciseOnly())
  }
}
