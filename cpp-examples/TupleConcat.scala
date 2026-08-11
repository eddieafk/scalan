package examples.tupleconcat

class Box(val value: String)

class Counter {
  var calls: Int = 0

  def nextLeft: String = {
    calls = calls + 1
    "left"
  }

  def nextRight: Boolean = {
    calls = calls + 1
    true
  }

  def left: (String, Int) = (nextLeft, calls)
  def right: (Boolean, Box) = (nextRight, new Box("right"))
  def combined: (String, Int, Boolean, Box) = left ++ right
}

object Values {
  def left: (Int, String) = (7, "middle")
  def right: (Boolean, Box) = (true, new Box("last"))
  def merged: (Int, String, Boolean, Box) = left ++ right

  def first: Int = merged._1
  def second: String = merged._2
  def third: Boolean = merged._3
  def fourth: Box = merged._4

  def leftIdentity: (Int, String) = EmptyTuple ++ left
  def rightIdentity: (Int, String) = left ++ EmptyTuple
  def bothEmpty: EmptyTuple = EmptyTuple ++ EmptyTuple

  def consAndLiteral: (Int, String, Boolean) =
    (99 *: EmptyTuple) ++ ("end", true)

  def chained: (Int, String, Boolean, Box, Long) =
    (1, "two") ++ (true *: EmptyTuple) ++ (new Box("four"), 5L)

  def orderOnce: String = {
    val counter = new Counter
    val result = counter.combined
    val first = result._1
    val second = result._2
    val third = result._3
    val box = result._4
    val fourth = box.value
    first + ":" + second + ":" + third + ":" + fourth + ":" + counter.calls
  }
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Values.first)
    println(Values.second)
    println(Values.third)
    println(Values.fourth.value)
    println(Values.leftIdentity.last)
    println(Values.rightIdentity.head)
    println(Values.bothEmpty.size)
    println(Values.consAndLiteral.last)
    println(Values.chained.last)
    println(Values.orderOnce)
  }
}
