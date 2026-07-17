package demo.examples

class BaseScore {
  def value: Int = 1
  def label: String = "base"
}

class FancyScore extends BaseScore {
  override def value: Int = 5
  override def label: String = "fancy"
  def doubled: Int = value + value
}

object Overrides {
  def main = {
    val score = new FancyScore()
    println(score.value)
    println(score.label)
    println(score.doubled)
  }
}
