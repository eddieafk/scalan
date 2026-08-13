package demo.examples

class ParamCounter(val start: Int, var current: Int) {
  def bump: Int = {
    current = current + start
    current
  }
}

object ParamFields {
  def main = {
    val counter = new ParamCounter(3, 4)
    println(counter.start)
    println(counter.current)
    println(counter.bump)
    counter.current = 10
    println(counter.current)
  }
}
