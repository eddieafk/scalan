package demo.examples

class Counter(start: Int) {
  val base: Int = start + 1
  val doubled: Int = base + base
  val label: String = "class field"

  def total: Int = doubled + start
}

object ClassFields {
  def main = {
    val counter = new Counter(41)
    println(counter.base)
    println(counter.doubled)
    println(counter.total)
    println(counter.label)
  }
}
