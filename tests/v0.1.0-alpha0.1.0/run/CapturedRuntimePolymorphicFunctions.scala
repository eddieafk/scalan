// expected-output: captured:7!
// expected-output: after:true?
// expected-output: 1!:false!
// expected-output: 11
// expected-output: kept
// expected-output: object:value

package tests.v010alpha010.capturedruntimepolymorphicfunctions

class Box[A](val value: A)

class Renderer(var prefix: String, val suffix: String) {
  def polymorphic(): [A] => A => String =
    [A] => (value: A) => prefix + value.toString + suffix
}

object Functions {
  var objectPrefix = "before-object:"

  def render(prefix: String, suffix: String): [A] => A => String =
    [A] => (value: A) => prefix + value.toString + suffix

  def constant(number: Int): [A] => A => Int =
    [A] => (ignored: A) => number

  def retain[A](captured: A): [B] => B => A =
    [B] => (ignored: B) => captured

  def fromObject(): [A] => A => String =
    [A] => (value: A) => objectPrefix + value.toString
}

object Main {
  def main(args: Array[String]): Unit = {
    val rendered = Functions.render("captured:", "!")
    println(rendered[Int](7))

    val renderer = new Renderer("before:", "?")
    val fromReceiver = renderer.polymorphic()
    renderer.prefix = "after:"
    println(fromReceiver[Boolean](true))

    val mapped = (1, false).map(Functions.render("", "!"))
    println(mapped._1 + ":" + mapped._2)

    println(Functions.constant(11)[String]("ignored"))

    val retained = Functions.retain[Box[String]](new Box[String]("kept"))
    println(retained[Int](1).value)

    val fromObject = Functions.fromObject()
    Functions.objectPrefix = "object:"
    println(fromObject[String]("value"))
  }
}
