package demo.examples

class StressLeaf(val value: Int)

class StressPair(val left: StressLeaf, val right: StressLeaf) {
  def total: Int = left.value + right.value
}

object GcStress {
  def right(): StressLeaf = new StressLeaf(22)

  def main = {
    gcSetCollectionThreshold(1L)
    val pair = new StressPair(new StressLeaf(20), right())
    println(pair.total)
    println(gcCollectionCount())
  }
}
