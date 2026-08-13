package examples.polymorphicfunctionaliases

class Renderer(val prefix: String) {
  val render: [A] => A => String =
    [A] => (value: A) => prefix + value.toString
  val alias = render

  def map(values: (Int, Boolean)): (String, String) = values.map(alias)
}

object Values {
  val identity: [A] => A => A = [A] => (value: A) => value
  val same = identity

  def heterogeneous: (Int, String, Boolean) =
    (42, "same", true).map(same)

  def localAliases: (Int, String) = {
    val first = same
    val second = first
    val mapped = (7, "local").map(second)
    mapped
  }
}

object Main {
  def main(args: Array[String]): Unit = {
    val heterogeneous = Values.heterogeneous
    println(
      heterogeneous._1.toString + ":" + heterogeneous._2 + ":" +
        heterogeneous._3.toString
    )

    val local = Values.localAliases
    println(local._1.toString + ":" + local._2)

    val rendered = new Renderer("value:").map((1, false))
    println(rendered._1 + ":" + rendered._2)
  }
}
