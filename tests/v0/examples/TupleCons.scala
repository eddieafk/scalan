package examples.tuplecons

class Box(val value: String)

class Counter {
  var count: Int = 0

  def next(value: String): String = {
    count = count + 1
    value
  }
}

object Values {
  def singleton: Int *: EmptyTuple = 42 *: EmptyTuple
  def singletonValue: Int = singleton._1

  def chain: String *: Boolean *: Long *: EmptyTuple =
    "head" *: true *: 9000000000L *: EmptyTuple
  def chainHead: String = chain._1
  def chainMiddle: Boolean = chain._2
  def chainLast: Long = chain._3

  def groupedType: Int *: (String *: EmptyTuple) = 5 *: "grouped" *: EmptyTuple
  def groupedText: String = groupedType._2

  def tail: String *: Boolean *: EmptyTuple = "tail" *: true *: EmptyTuple
  def prepended: Int *: String *: Boolean *: EmptyTuple = 7 *: tail
  def prependedText: String = prepended._2

  def literalTail: Box *: (Int, String) =
    new Box("reference") *: (1, "literal tail")
  def literalTailBox: Box = literalTail._1
  def literalTailText: String = literalTail._3

  def evaluationOrder: String = {
    val counter = new Counter
    val values =
      counter.next("first") *: (counter.next("second"), counter.count)
    values._1 + ":" + values._2 + ":" + values._3
  }
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Values.singletonValue)
    println(Values.chainHead)
    println(Values.chainMiddle)
    println(Values.chainLast)
    println(Values.groupedText)
    println(Values.prependedText)
    println(Values.literalTailBox.value)
    println(Values.literalTailText)
    println(Values.evaluationOrder)
  }
}
