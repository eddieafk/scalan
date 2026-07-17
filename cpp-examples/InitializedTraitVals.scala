package demo.traitvals

trait RootValues {
  val root: Int = {
    println("root init")
    2
  }

  def rootValue: Int = root
}

trait LeftValues extends RootValues {
  val left: Int = {
    println("left init")
    3
  }

  def leftValue: Int = left
}

trait RightValues {
  val right: Int = {
    println("right init")
    5
  }

  def rightValue: Int = right
}

class CombinedValues extends LeftValues with RightValues {
  def total: Int = root + left + right
}

class ChildValues extends CombinedValues

object Main {
  def show(value: CombinedValues) = {
    println(value.rootValue)
    println(value.leftValue)
    println(value.rightValue)
    println(value.total)
    println(value.total)
  }

  def main = {
    show(new CombinedValues())
    show(new ChildValues())
  }
}
