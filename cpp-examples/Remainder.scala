package demo.examples

object Remainder {
  def intRemainder(value: Int, divisor: Int): Int = value % divisor
  def longRemainder(value: Long, divisor: Long): Long = value % divisor
  def doubleRemainder(value: Double, divisor: Double): Double = value % divisor

  def main = {
    println(intRemainder(17, 5))
    println(intRemainder(0 - 17, 5))
    println(longRemainder(20L, 6L))
    println(doubleRemainder(5.75, 2.0))
  }
}
