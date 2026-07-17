package demo.examples

object ArrayParameters {
  def replaceAndTotal(values: Array[Int], index: Int, replacement: Int): Int = {
    values(index) = replacement
    values(0) + values(1)
  }

  def main = {
    val values = Array(4, 6)
    println(replaceAndTotal(values, 1, 9))
    println(values(1))
  }
}
