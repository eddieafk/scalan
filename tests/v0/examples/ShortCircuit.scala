package demo.examples

object ShortCircuit {
  def right(value: Int): Boolean = {
    println(value)
    true
  }

  def main = {
    println(false && right(10))
    println(true || right(20))
    println(true && right(30))
    println(false || right(40))
  }
}
