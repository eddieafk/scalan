package demo.examples

class ConstructedCounter(start: Int) {
  var current: Int = start

  current = current + 5
  val snapshot: Int = current
  current = current + 1
  val finalValue: Int = current + snapshot

  def value: Int = finalValue
  def snapshotValue: Int = snapshot
  def currentValue: Int = current
}

object ConstructorBody {
  def main = {
    val counter = new ConstructedCounter(10)
    println(counter.snapshotValue)
    println(counter.currentValue)
    println(counter.value)
  }
}
