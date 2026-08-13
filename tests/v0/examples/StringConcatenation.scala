package demo.examples

object StringConcatenation {
  def join(left: String, right: String): String = left + right

  def main = {
    println(join("Scala ", "Native"))
    println("a" + "b" + "c")
  }
}
