package demo.transitivesuper

trait RootValue {
  def value: Int = 1
}

trait MiddleValue extends RootValue {
  def middle: Int = 5
}

trait Marker {
  def marker: Int = 2
}

class CombinedValue extends MiddleValue with Marker {
  override def value: Int = super.value + 10
}

object Main {
  def main = {
    val combined = new CombinedValue()
    println(combined.value)
  }
}
