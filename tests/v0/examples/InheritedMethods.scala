package demo.examples

class BaseCounter {
  def zero: Int = 0
  def label: String = "base"
}

class FancyCounter extends BaseCounter {
  def one: Int = zero + 1
  def inheritedLabel: String = label
}

object InheritedMethods {
  def main = {
    val counter = new FancyCounter()
    println(counter.zero)
    println(counter.one)
    println(counter.label)
    println(counter.inheritedLabel)
  }
}
