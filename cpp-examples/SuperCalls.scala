package demo.supercalls

class BaseScore(val seed: Int) {
  def value: Int = seed + 1
  def add(extra: Int): Int = value + extra
}

class FancyScore(seed: Int) extends BaseScore(seed) {
  override def value: Int = super.value + 10
  override def add(extra: Int): Int = super.add(extra) + value
}

object Main {
  def main = {
    val score = new FancyScore(4)
    println(score.value)
    println(score.add(2))
  }
}
