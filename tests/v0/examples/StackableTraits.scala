package demo.stackable

trait RootValue {
  def value: Int = 1
}

trait AddTwo extends RootValue {
  override def value: Int = super.value + 2
}

trait AddTen extends RootValue {
  override def value: Int = super.value + 10
}

class StackedValue extends RootValue with AddTwo with AddTen


object Main {
  def main = {
    val stacked = new StackedValue()
    println(stacked.value)
  }
}
