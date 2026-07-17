package demo.shadowedvals

trait LeftValue {
  val value: Int = {
    println("left init")
    3
  }
}

trait RightValue {
  val value: Int = {
    println("right init")
    9
  }
}

class CombinedValue extends LeftValue with RightValue {
  def current: Int = value
  def left: Int = super[LeftValue].value
  def right: Int = super[RightValue].value
}

class ChildValue extends CombinedValue

object Main {
  def show(value: CombinedValue) = {
    println(value.current)
    println(value.left)
    println(value.right)
    println(value.current)
  }

  def main = {
    show(new CombinedValue())
    show(new ChildValue())
  }
}
