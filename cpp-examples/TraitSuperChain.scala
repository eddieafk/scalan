package demo.traitsuper

trait BaseValue {
  def value: Int = 1
}

trait PlusTen extends BaseValue {
  override def value: Int = super.value + 10
}

class CombinedValue extends BaseValue with PlusTen {
  override def value: Int = super.value + 100
}

object Main {
  def main = {
    val combined = new CombinedValue()
    println(combined.value)
  }
}
