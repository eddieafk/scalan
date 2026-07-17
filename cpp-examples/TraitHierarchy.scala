package demo.traitchain

trait RootMetric {
  def label: String
  def score: Int
}

trait LabeledMetric extends RootMetric {
  override def label: String = "labeled"
  def doubled: Int = score + score
}

class ConcreteMetric extends LabeledMetric {
  override def score: Int = 6
}

object Main {
  def showRoot(metric: RootMetric) = {
    println(metric.label)
    println(metric.score)
  }

  def showLabeled(metric: LabeledMetric) =
    println(metric.doubled)

  def main = {
    val metric = new ConcreteMetric()
    showRoot(metric)
    showLabeled(metric)
  }
}
