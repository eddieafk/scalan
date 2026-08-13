package demo.examples

object StringEquality {
  def join(left: String, right: String): String = left + right

  def main = {
    val dynamic = join("Scala ", "Native")
    println(dynamic == "Scala Native")
    println(dynamic != "Scala")
    println("literal" == "literal")
    println("literal" != "other")
    println("literal" == null)
  }
}
