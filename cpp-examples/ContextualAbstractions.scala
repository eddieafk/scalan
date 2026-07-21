package demo.contextual

class Dog(val name: String)
class Cat(val name: String)
class Bird(val name: String)
class Fox(val name: String)
class Box[A](val value: A)
class DerivationSeed(val prefix: String)

object DerivationSeed {
  given seed: DerivationSeed = new DerivationSeed("derived")
}

class Formatter(val label: String) {
  def format(): String = label
}

class DetailedFormatter(label: String) extends Formatter(label)
class AlternateFormatter(label: String) extends Formatter(label)

class OwnerChoice(val label: String)
class SpecificOwnerChoice(label: String) extends OwnerChoice(label)

class LowPriorityOwnerChoices {
  given lowOwnerChoice: OwnerChoice = new OwnerChoice("owner-low")
}

class MiddlePriorityOwnerChoices extends LowPriorityOwnerChoices

object OwnerChoice extends MiddlePriorityOwnerChoices {
  given highOwnerChoice: SpecificOwnerChoice =
    new SpecificOwnerChoice("owner-high")
}

class DirectSeed
class DirectChoice(val label: String)

object DirectChoice {
  given DirectSeed = new DirectSeed
  given directChoice: DirectChoice = new DirectChoice("direct")
  given contextualChoice(using seed: DirectSeed): DirectChoice =
    new DirectChoice("contextual")
}

class GeneralSeed
class SpecificSeed extends GeneralSeed

object SpecificSeed {
  given SpecificSeed = new SpecificSeed
}

class FactoryChoice(val label: String)

object FactoryChoice {
  given fromGeneral(using seed: GeneralSeed): FactoryChoice =
    new FactoryChoice("general-factory")
  given fromSpecific(using seed: SpecificSeed): FactoryChoice =
    new FactoryChoice("specific-factory")
}

class MissingFallbackDependency
class MissingFallbackChoice(val label: String)
class ValidMissingFallbackChoice(label: String) extends MissingFallbackChoice(label)

object MissingFallbackChoice {
  given broken(using dependency: MissingFallbackDependency): MissingFallbackChoice =
    new MissingFallbackChoice("broken-missing")
  given valid: ValidMissingFallbackChoice =
    new ValidMissingFallbackChoice("missing-fallback")
}

class AmbiguousFallbackDependency

object AmbiguousFallbackDependency {
  given first: AmbiguousFallbackDependency = new AmbiguousFallbackDependency
  given second: AmbiguousFallbackDependency = new AmbiguousFallbackDependency
}

class AmbiguousFallbackChoice(val label: String)
class ValidAmbiguousFallbackChoice(label: String)
    extends AmbiguousFallbackChoice(label)

object AmbiguousFallbackChoice {
  given broken(
      using dependency: AmbiguousFallbackDependency): AmbiguousFallbackChoice =
    new AmbiguousFallbackChoice("broken-ambiguous")
  given valid: ValidAmbiguousFallbackChoice =
    new ValidAmbiguousFallbackChoice("ambiguous-fallback")
}

class DivergentFallbackDependency

object DivergentFallbackDependency {
  given loop(
      using next: DivergentFallbackDependency): DivergentFallbackDependency = next
}

class DivergentFallbackChoice(val label: String)
class ValidDivergentFallbackChoice(label: String)
    extends DivergentFallbackChoice(label)

object DivergentFallbackChoice {
  given broken(
      using dependency: DivergentFallbackDependency): DivergentFallbackChoice =
    new DivergentFallbackChoice("broken-divergent")
  given valid: ValidDivergentFallbackChoice =
    new ValidDivergentFallbackChoice("divergent-fallback")
}

trait Show[A] {
  def show(value: A): String
}

class DerivedShow[A](val prefix: String) extends Show[A] {
  override def show(value: A): String = prefix
}

class DogShow(val prefix: String) extends Show[Dog] {
  override def show(value: Dog): String = prefix + value.name
}

class CatShow(val prefix: String) extends Show[Cat] {
  override def show(value: Cat): String = prefix + value.name
}

class BirdShow(val prefix: String) extends Show[Bird] {
  override def show(value: Bird): String = prefix + value.name
}

class FoxShow(val prefix: String) extends Show[Fox] {
  override def show(value: Fox): String = prefix + value.name
}

class BoxShow[A](val elementShow: Show[A]) extends Show[Box[A]] {
  override def show(value: Box[A]): String = "box"
}

object Show {
  given catShow: Show[Cat] = new CatShow("companion:")
  given foxShow: Show[Fox] = new FoxShow("typeclass-companion:")
  given Show[Dog] = new DogShow("shadowed-companion:")

  given boxShow[A](using elementShow: Show[A]): Show[Box[A]] =
    new BoxShow[A](elementShow)

  def derived[A](using seed: DerivationSeed): Show[A] =
    new DerivedShow[A](seed.prefix)
}

class DerivationBase
class AutomaticallyShown extends DerivationBase derives Show
trait AutomaticallyShownTrait derives Show
class AutomaticallyShownTraitValue extends AutomaticallyShownTrait
object AutomaticallyShownObject derives Show

object Bird {
  given Show[Bird] = new BirdShow("argument-companion:")
}

import Show.{catShow => selectedCatShow}

object ContextualAbstractions {
  given dogShow: Show[Dog] = new DogShow("dog:")
  given generalFormatter: Formatter = new Formatter("general")
  given detailedFormatter: DetailedFormatter = new DetailedFormatter("detailed")
  given alternateFormatter: AlternateFormatter =
    new AlternateFormatter("alternate")

  def render[A](value: A)(using show: Show[A]): String =
    show.show(value)

  def forwarded[A](value: A)(using show: Show[A]): String =
    render(value)

  def explicit(value: Dog): String =
    render(value)(using dogShow)

  def locally(value: Dog)(using show: Show[Dog]): String =
    render(value)

  def localNamed(value: Dog): String = {
    given localShow: Show[Dog] = new DogShow("named-local:")
    render(value)
  }

  def localAnonymous(value: Dog): String = {
    given Show[Dog] = new DogShow("anonymous-local:")
    render(value)
  }

  def companion(value: Fox): String =
    render(value)

  def argumentCompanion(value: Bird): String =
    render(value)

  def directCompanion(value: Cat): String =
    Show.catShow.show(value)

  def importedCompanion(value: Cat): String =
    selectedCatShow.show(value)

  def parameterized(value: Box[Cat]): String =
    render(value)

  def recursivelyParameterized(value: Box[Box[Cat]]): String =
    render(value)

  def automaticallyDerivedClass: String =
    render(new AutomaticallyShown)

  def automaticallyDerivedTrait(value: AutomaticallyShownTrait): String =
    render(value)

  def automaticallyDerivedObject: String =
    render(AutomaticallyShownObject)

  def format()(using formatter: Formatter): String =
    formatter.format()

  def generallyPreferred: String =
    format()

  def nestedPreference: String = {
    given nestedFormatter: DetailedFormatter = new DetailedFormatter("nested")
    format()
  }

  def chooseOwner()(using choice: OwnerChoice): String =
    choice.label

  def ownerPreferred: String =
    chooseOwner()

  def chooseDirect()(using choice: DirectChoice): String =
    choice.label

  def nonContextualPreferred: String =
    chooseDirect()

  def chooseFactory()(using choice: FactoryChoice): String =
    choice.label

  def specificFactoryPreferred: String =
    chooseFactory()

  def chooseMissingFallback()(using choice: MissingFallbackChoice): String =
    choice.label

  def missingFallback: String =
    chooseMissingFallback()

  def chooseAmbiguousFallback()(using choice: AmbiguousFallbackChoice): String =
    choice.label

  def ambiguousFallback: String =
    chooseAmbiguousFallback()

  def chooseDivergentFallback()(using choice: DivergentFallbackChoice): String =
    choice.label

  def divergentFallback: String =
    chooseDivergentFallback()

  def nestedLocal(value: Dog): String = {
    given outerShow: Show[Dog] = new DogShow("outer-local:")
    {
      given Show[Dog] = new DogShow("inner-local:")
      render(value)
    }
  }

  def main = {
    println(render(new Dog("inferred")))
    println(forwarded(new Dog("forwarded")))
    println(explicit(new Dog("explicit")))
    println(locally(new Dog("local"))(using new DogShow("local:")))
    println(localNamed(new Dog("dog")))
    println(localAnonymous(new Dog("dog")))
    println(companion(new Fox("fox")))
    println(argumentCompanion(new Bird("bird")))
    println(directCompanion(new Cat("direct")))
    println(importedCompanion(new Cat("imported")))
    println(parameterized(new Box[Cat](new Cat("boxed"))))
    println(recursivelyParameterized(
      new Box[Box[Cat]](new Box[Cat](new Cat("recursive")))))
    println(automaticallyDerivedClass)
    println(automaticallyDerivedTrait(new AutomaticallyShownTraitValue))
    println(automaticallyDerivedObject)
    println(generallyPreferred)
    println(nestedPreference)
    println(ownerPreferred)
    println(nonContextualPreferred)
    println(specificFactoryPreferred)
    println(missingFallback)
    println(ambiguousFallback)
    println(divergentFallback)
    println(nestedLocal(new Dog("dog")))
  }
}
