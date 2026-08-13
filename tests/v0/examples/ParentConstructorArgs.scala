package demo.parentargs

class BaseCounter(val start: Int) {
  val doubled: Int = start + start

  def total: Int = start + doubled
}

class ChildCounter(val seed: Int) extends BaseCounter(seed + 1) {
  val child: Int = start + seed

  def all: Int = total + child
}

object Main {
  def main = {
    val child = new ChildCounter(4)
    println(child.start)
    println(child.doubled)
    println(child.child)
    println(child.total)
    println(child.all)
  }
}
