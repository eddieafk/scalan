package demo.examples

object Conditionals {
  def choose(flag: Boolean): Int =
    if (flag) {
      println(10)
      1
    } else {
      println(20)
      2
    }

  def main = {
    println(choose(true))
    println(choose(false))
    if (false) println(99)
    if (true) println(30) else println(40)
  }
}
