package demo.contextual

class Dog(val name: String)
class Cat(val name: String)
class Bird(val name: String)
class Fox(val name: String)
class Box[A](val value: A)

trait Show[A] {
  def show(value: A): String
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
}

object Bird {
  given Show[Bird] = new BirdShow("argument-companion:")
}

import Show.{catShow => selectedCatShow}

object ContextualAbstractions {
  given dogShow: Show[Dog] = new DogShow("dog:")

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
    println(nestedLocal(new Dog("dog")))
  }
}
