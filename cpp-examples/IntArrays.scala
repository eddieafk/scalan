package demo.examples

object IntArrays {
  def main = {
    val values = Array(3, 7)
    values(0) = 9
    println(values.length)
    println(values(0))
    println(values(1))
  }
}
