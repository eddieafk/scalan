package demo.examples

object Loops {
  def main = {
    var current = 0
    var total = 0
    while (current < 5) {
      total = total + current
      current = current + 1
    }
    while (false) println(99)
    println(total)
    println(current)
  }
}
