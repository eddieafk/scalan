package demo.examples

object LongArrays {
  def replaceAndTotal(values: Array[Long], index: Int, replacement: Long): Long = {
    values(index) = replacement
    values(0) + values(1)
  }

  def main = {
    val values = Array(4L, 6L)
    println(replaceAndTotal(values, 1, 9L))
    println(values(1))
  }
}
