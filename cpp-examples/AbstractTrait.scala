package demo.abstracttrait

trait Metric {
  def label: String
  def score: Int
  def describe: String = label
}

class BasicMetric extends Metric {
  override def label: String = "basic"
  override def score: Int = 2
}

class FancyMetric extends Metric {
  override def label: String = "fancy"
  override def score: Int = 9
}

object Main {
  def show(metric: Metric) = {
    println(metric.describe)
    println(metric.score)
  }

  def main = {
    show(new BasicMetric())
    show(new FancyMetric())
  }
}
