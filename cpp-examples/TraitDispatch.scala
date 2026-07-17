package demo.traitdispatch

trait Named {
  def name: String = "trait"
  def score: Int = 1
}

class Plain extends Named

class Fancy extends Named {
  override def name: String = "fancy"
  override def score: Int = 9
}

object Main {
  def show(named: Named) = {
    println(named.name)
    println(named.score)
  }

  def main = {
    show(new Plain())
    show(new Fancy())
  }
}
