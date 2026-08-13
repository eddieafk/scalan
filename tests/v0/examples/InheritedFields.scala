package demo.inheritedfields

class BaseFields {
  val base: Int = 10
  var current: Int = base + 1

  def baseValue: Int = base
  def currentValue: Int = current
}

class ChildFields extends BaseFields {
  val child: Int = base + current

  def total: Int = base + current + child
  def bump: Int = {
    current = current + 1
    current
  }
}

object Main {
  def main = {
    val child = new ChildFields()
    println(child.base)
    println(child.currentValue)
    println(child.child)
    println(child.total)
    println(child.bump)
  }
}
