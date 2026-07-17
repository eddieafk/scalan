package demo.boxeddependent

trait Box {
  type Item
  def value: Item
  def increment(value: Item): Int
}

class IntBox extends Box {
  override type Item = Int
  override def value: Item = 7
  override def increment(value: Item): Int = value + 1
}

object Main {
  def main = {
    val box: Box = new IntBox()
    println(box.increment(box.value))
  }
}
