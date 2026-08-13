package demo.localtyped

class BaseScore {
  def value: Int = 1
  def label: String = "base"
}

class FancyScore extends BaseScore {
  override def value: Int = 9
  override def label: String = "fancy"
}

object Main {
  def main = {
    val score: BaseScore = new FancyScore()
    println(score.value)
    println(score.label)
  }
}
