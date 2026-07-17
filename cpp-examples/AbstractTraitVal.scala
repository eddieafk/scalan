package demo.abstractval

trait NamedScore {
  val name: String
  val score: Int

  def label: String = name
  def doubled: Int = score + score
}

class ConcreteScore extends NamedScore {
  override val name: String = "concrete"
  override val score: Int = 6
}

object Main {
  def show(value: NamedScore) = {
    println(value.name)
    println(value.label)
    println(value.score)
    println(value.doubled)
  }

  def main = {
    val value = new ConcreteScore()
    show(value)
    println(value.name)
  }
}
