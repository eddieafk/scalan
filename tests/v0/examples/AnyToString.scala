package demo.examples

class AnyName(val value: Int)

object AnyToString {
  def main = {
    val intValue: Any = 7
    val booleanValue: Any = true
    val unitValue: Any = {}
    val symbolValue: Any = 'ready
    val nullValue: Any = null
    val objectValue: Any = new AnyName(3)

    println(intValue.toString)
    println(booleanValue.toString)
    println(unitValue.toString)
    println(symbolValue.toString)
    println(nullValue.toString)
    println(objectValue.toString)
    println(intValue)
    println("symbol=" + symbolValue)
  }
}
