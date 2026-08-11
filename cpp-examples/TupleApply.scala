package examples.tupleapply

class Box(val value: String)

class Counter {
  var calls: Int = 0

  def nextLabel: String = {
    calls = calls + 1
    "once"
  }

  def make: (String, Int, Boolean) = (nextLabel, calls, true)
}

class Lookup {
  def apply(index: Int): String = "custom:" + index
}

object Values {
  def triple: (Int, String, Box) = (7, "middle", new Box("last"))

  def first: Int = triple(0)
  def second: String = triple.apply(1)
  def third: Box = triple(1 + 1)
  def thirdText: String = third.value

  def stableIndex: String = {
    val index: 1 = 1
    triple(index)
  }

  def direct: Boolean = (9, true)(1)
  def cons: Int = (99 *: EmptyTuple).apply(0)

  def receiverOnce: String = {
    val counter = new Counter
    counter.make.apply(1) + ":" + counter.calls
  }

  def customApply: String = new Lookup().apply(4)
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Values.first)
    println(Values.second)
    println(Values.thirdText)
    println(Values.stableIndex)
    println(Values.direct)
    println(Values.cons)
    println(Values.receiverOnce)
    println(Values.customApply)
  }
}
