package demo.examples

object LocalVars {
  def total(start: Int): Int = {
    var current = start
    current = current + 1
    current = current + 2
    current
  }

  def main = {
    println(total(10))
  }
}
