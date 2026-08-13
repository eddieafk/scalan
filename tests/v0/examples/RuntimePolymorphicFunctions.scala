package examples.runtimepolymorphicfunctions

object Functions {
  def identity(): [A] => A => A =
    [A] => (value: A) => value

  def render(): [A] => A => String =
    [A] => (value: A) => value.toString

  def invoke(function: [A] => A => A): String =
    function[Int](42).toString + ":" + function[String]("same")

  def forward(function: [A] => A => A): [A] => A => A =
    function
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Functions.invoke([A] => (value: A) => value))

    val forwarded = Functions.forward(Functions.identity())
    println(forwarded[Boolean](true))

    val rendered = (7, false).map(Functions.render())
    println(rendered._1 + ":" + rendered._2)
  }
}
