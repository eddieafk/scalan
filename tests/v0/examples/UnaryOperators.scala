package demo.examples

object UnaryOperators {
  def negate(value: Int): Int = -value
  def retain(value: Long): Long = +value
  def negateFloat(value: Float): Float = -value
  def retainDouble(value: Double): Double = +value
  def invert(value: Boolean): Boolean = !value

  def main = {
    println(negate(7))
    println(retain(8L))
    println(negateFloat(1.25F))
    println(retainDouble(2.5))
    println(invert(false))
    println(!true)
    println(-(-5))
  }
}
