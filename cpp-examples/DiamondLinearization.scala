package demo.diamond

trait RootValue {
  def value: Int = 1
  def root: Int = 7
}

trait LeftValue extends RootValue {
  override def value: Int = 10
}

trait RightValue extends RootValue {
  def right: Int = 20
}

class DiamondValue extends LeftValue with RightValue {
  def inheritedValue: Int = value
  def superValue: Int = super.value
}

object Main {
  def main = {
    val diamond = new DiamondValue()
    println(diamond.value)
    println(diamond.inheritedValue)
    println(diamond.superValue)
    println(diamond.right)
    println(diamond.root)
  }
}
