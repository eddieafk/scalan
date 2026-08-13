package demo.examples

object DoubleArrays {
  def replaceAndAverage(values: Array[Double], index: Int, replacement: Double): Double = {
    values(index) = replacement
    val total = values(0) + values(1)
    total / 2.0
  }

  def main = {
    val empty = Array[Double]()
    val values = Array(1.5, 2.5)
    println(empty.length)
    println(replaceAndAverage(values, 1, 4.5))
    println(values(1))
  }
}
