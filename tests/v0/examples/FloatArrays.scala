package demo.examples

object FloatArrays {
  def replaceAndAverage(values: Array[Float], index: Int, replacement: Float): Float = {
    values(index) = replacement
    val total = values(0) + values(1)
    total / 2.0F
  }

  def main = {
    val empty = Array[Float]()
    val values = Array(1.5F, 2.5F)
    println(empty.length)
    println(replaceAndAverage(values, 1, 4.5F))
    println(values(1))
  }
}
