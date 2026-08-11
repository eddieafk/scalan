package examples.tupleoperations

class Box(val value: String)

class Counter {
  var calls: Int = 0

  def nextLabel: String = {
    calls = calls + 1
    "once"
  }

  def make: (String, Int, Boolean) = (nextLabel, calls, true)
}

object Values {
  def triple: (Int, String, Box) = (7, "middle", new Box("last"))
  def first: Int = triple.head

  def rest: String *: Box *: EmptyTuple = triple.tail
  def restHead: String = rest.head
  def restLast: Box = rest.tail.head
  def restLastText: String = restLast.value
  def restEnd: EmptyTuple = rest.tail.tail

  def tripleSize: Int = triple.size
  def exactSize: 3 = triple.size
  def restSize: Int = rest.size
  def emptySize: Int = EmptyTuple.size

  def singleton: Int *: EmptyTuple = 99 *: EmptyTuple
  def singletonHead: Int = singleton.head
  def singletonTail: EmptyTuple = singleton.tail

  def directHead: String = ("direct", 1).head
  def nestedTailHead: Boolean = (1, "drop", true).tail.tail.head

  def headOnce: String = {
    val counter = new Counter
    counter.make.head + ":" + counter.calls
  }

  def tailOnce: String = {
    val counter = new Counter
    val tail = counter.make.tail
    tail.head + ":" + counter.calls
  }

  def sizeOnce: String = {
    val counter = new Counter
    counter.make.size + ":" + counter.calls
  }
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Values.first)
    println(Values.restHead)
    println(Values.restLastText)
    println(Values.tripleSize)
    println(Values.restSize)
    println(Values.emptySize)
    println(Values.singletonHead)
    println(Values.directHead)
    println(Values.nestedTailHead)
    println(Values.headOnce)
    println(Values.tailOnce)
    println(Values.sizeOnce)
  }
}
