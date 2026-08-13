package demo.examples

class TelemetryNode(val value: Int)

object GcTelemetry {
  def allocateDetached(): Long = {
    val temporary = new TelemetryNode(42)
    gcLiveObjectCount()
  }

  def main = {
    val baseline = gcLiveObjectCount()
    val withDetached = allocateDetached()
    gcCollect()
    val afterCollection = gcLiveObjectCount()
    println(withDetached - baseline)
    println(afterCollection - baseline)
    println(gcCollectionCount())
  }
}
