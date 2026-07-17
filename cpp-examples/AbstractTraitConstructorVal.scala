package demo.constructorval

trait NamedScore {
  val name: String
  val score: Int

  def label: String = name
  def doubled: Int = score + score
}

class ConstructorScore(val name: String, val score: Int) extends NamedScore

class ChildScore(name: String, score: Int)
    extends ConstructorScore(name, score)

object Main {
  def show(value: NamedScore) = {
    println(value.name)
    println(value.label)
    println(value.score)
    println(value.doubled)
  }

  def main = {
    val direct = new ConstructorScore("direct", 7)
    val child = new ChildScore("child", 5)
    show(direct)
    show(child)
    println(direct.name)
  }
}
