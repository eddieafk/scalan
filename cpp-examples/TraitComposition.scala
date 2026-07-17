package demo.composition

trait Named {
  def name: String
}

trait Scored {
  def score: Int
}

trait BaseLabel {
  def label: String = "base-label"
}

trait FancyLabel {
  def label: String = "fancy-label"
}

class Base(val seed: Int) {
  def base: Int = seed
}

class Combined(seed: Int)
    extends Base(seed)
    with Named
    with Scored
    with BaseLabel
    with FancyLabel {
  override def name: String = "combined"
  override def score: Int = base + 2
}

object Main {
  def showName(value: Named) = println(value.name)
  def showScore(value: Scored) = println(value.score)
  def showLabel(value: BaseLabel) = println(value.label)

  def main = {
    val value = new Combined(5)
    showName(value)
    showScore(value)
    showLabel(value)
  }
}
