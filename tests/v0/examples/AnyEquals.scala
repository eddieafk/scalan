package demo.examples

class AnyEqualityBox(val value: Int)

object AnyEquals {
  def main = {
    val seven: Any = 7
    val anotherSeven: Any = 7
    val eight: Any = 8
    val truth: Any = true
    val anotherTruth: Any = true
    val longValue: Any = 4294967296L
    val anotherLongValue: Any = 4294967296L
    val unitValue: Any = {}
    val anotherUnitValue: Any = {}
    val symbolValue: Any = 'ready
    val anotherSymbolValue: Any = 'ready
    val box = new AnyEqualityBox(1)
    val sameBox: Any = box
    val otherBox: Any = new AnyEqualityBox(1)

    println(seven == anotherSeven)
    println(seven == eight)
    println(seven.equals(7))
    println(7 == seven)
    println(truth == anotherTruth)
    println(longValue == anotherLongValue)
    println(unitValue == anotherUnitValue)
    println(symbolValue == anotherSymbolValue)
    println(sameBox == box)
    println(sameBox == otherBox)
    println(sameBox != otherBox)
  }
}
