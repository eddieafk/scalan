package demo.examples

object FloatingArithmetic {
  def addFloat(left: Float, right: Float): Float = left + right
  def divideDouble(left: Double, right: Double): Double = left / right

  def main = {
    println(addFloat(1.5F, 2.25F))
    println(divideDouble(5.0, 2.0))
  }
}
