package examples.capturedruntimepolymorphicfunctions

class Renderer(var prefix: String, val suffix: String) {
  def polymorphic(): [A] => A => String =
    [A] => (value: A) => prefix + value.toString + suffix
}

object Functions {
  val objectPrefix = "object:"

  def render(prefix: String, suffix: String): [A] => A => String =
    [A] => (value: A) => prefix + value.toString + suffix

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

    val retained = Functions.retain[String]("kept")
    println(retained[Int](1))
    println(Functions.fromObject()[String]("value"))
  }
}
