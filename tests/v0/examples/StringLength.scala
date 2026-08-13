package demo.examples

object StringLength {
  def join(left: String, right: String): String = left + right

  def main = {
    val dynamic = join("Scala ", "Native")
    println(dynamic.length)
    println("abc".length)
    println("".length)
  }
}
