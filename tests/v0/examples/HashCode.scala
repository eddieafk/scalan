package demo.examples

class HashBox(val value: Int)

object HashCode {
  def join(left: String, right: String): String = left + right

  def main = {
    val box = new HashBox(1)
    val same = box
    val missing: HashBox = null
    val anyInt: Any = 7
    val anySymbol: Any = 'ready
    val anyBox: Any = box

    println(7.hashCode)
    println(true.hashCode())
    println(4294967296L.hashCode)
    println('A'.hashCode)
    println("Scala".hashCode == join("Sca", "la").hashCode)
    println('ready.hashCode == "'ready".hashCode)
    println(missing.hashCode)
    println(box.hashCode == same.hashCode)
    println(anyInt.hashCode)
    println(anySymbol.hashCode == 'ready.hashCode)
    println(anyBox.hashCode == box.hashCode)
  }
}
