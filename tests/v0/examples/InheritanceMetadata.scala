package demo.examples

trait Named {
  def name: String = "named"
}

trait Labeled extends Named {
  def label: String = "labeled"
}

class BaseCounter {
  def zero: Int = 0
}

class FancyCounter extends BaseCounter {
  def one() = {
    println(zero)
    println(zero)
  }
}

object InheritanceMetadata extends Labeled {
  def main = {
    val counter = new FancyCounter
    println(counter.one())
  }
}
