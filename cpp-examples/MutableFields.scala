package demo.examples


class MutableCounter(var start: Int) {
  var current: Int = start

  def increment: Int = {
    current = current + 1
    current
  }

  def add(amount: Int): Int = {
    this.current = this.current + amount
    current
  }
}

object MutableFields {
  def main = {
    val counter = new MutableCounter(10)
    println(counter.increment)
    println(counter.increment)
    println(counter.add(5))
    counter.current = 100
    println(counter.current)
  }
}
