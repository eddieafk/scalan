package examples.tuplevalues

class Box(val value: String)

object Values {
  def directInt: Int = (42, "direct")._1
  def directText: String = (42, "direct")._2

  def pair: (Int, String) = (7, "stored")
  def storedInt: Int = pair._1
  def storedText: String = pair._2

  def triple: (Boolean, Long, Box) =
    (true, 9000000000L, new Box("boxed reference"))
  def tripleLong: Long = triple._2
  def tripleBox: Box = triple._3
  def tripleText: String = tripleBox.value

  def nested: ((Int, String), (Boolean, Char)) =
    ((1, "nested"), (true, 'x'))
  def nestedText: String = nested._1._2
  def nestedChar: Char = nested._2._2

  def trailingComma: String = (3, "trailing",)._2
  def groupedArithmetic: Int = (1 + 2) * 3
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Values.directInt)
    println(Values.directText)
    println(Values.storedInt)
    println(Values.storedText)
    println(Values.tripleLong)
    println(Values.tripleText)
    println(Values.nestedText)
    println(Values.nestedChar)
    println(Values.trailingComma)
    println(Values.groupedArithmetic)
  }
}
