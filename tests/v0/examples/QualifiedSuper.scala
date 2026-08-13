package demo.qualifiedsuper

trait LeftValue {
  def label: String = "left"
  def value: Int = 1
}

trait RightValue {
  def label: String = "right"
  def value: Int = 10
}

class CombinedValue extends LeftValue with RightValue {
  def leftLabel: String = super[LeftValue].label
  def rightLabel: String = super[RightValue].label

  override def value: Int =
    super[LeftValue].value + super[RightValue].value
}

object Main {
  def main = {
    val combined = new CombinedValue()
    println(combined.leftLabel)
    println(combined.rightLabel)
    println(combined.value)
  }
}
