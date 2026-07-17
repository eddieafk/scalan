package demo.dynamic

class BaseScore {
  def value: Int = 1
  def label: String = "base"
}

class FancyScore extends BaseScore {
  override def value: Int = 7
  override def label: String = "fancy"
}

class Random extends BaseScore{
  def example(): Int = 0
}

object Main {
  def read(score: BaseScore): Int = score.value
  def describe(score: BaseScore): String = score.label

  def main = {
    println(read(new BaseScore()))
    println(read(new FancyScore()))
    println(describe(new FancyScore()))
  }
}
