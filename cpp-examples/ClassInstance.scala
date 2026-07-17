package demo.examples

class Counter(start: Int) {
  def value: Int = start
  def doubled: Int = value + value
  def shadow(start: Int): Int = start
  def message: String = "class instance method"
}

object ClassInstance {
  def main = {
    val counter = new Counter(42)
    println(counter.value)
    println(counter.doubled)
    println(counter.shadow(5))
    println(new Counter(7).message)
  }
}
