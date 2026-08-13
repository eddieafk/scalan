package examples.tupleinitlast

class Box(val value: String)

class Counter {
  var calls: Int = 0

  def nextLabel: String = {
    calls = calls + 1
    "once"
  }

  def make: (String, Int, Box) = (nextLabel, calls, new Box("tail"))
}

object Values {
  def tuple: (Int, String, Boolean, Box) =
    (7, "middle", true, new Box("last"))

  def initial: (Int, String, Boolean) = tuple.init
  def initialFirst: Int = initial.head
  def initialLast: Boolean = initial.last
  def last: Box = tuple.last
  def lastText: String = last.value

  def twice: (Int, String) = tuple.init.init
  def twiceLast: String = twice.last
  def singleton: Int *: EmptyTuple = tuple.init.init.init
  def singletonLast: Int = singleton.last
  def empty: EmptyTuple = tuple.init.init.init.init
  def emptySize: Int = empty.size

  def consLast: String = (99 *: "end" *: EmptyTuple).last
  def directLast: String = (1, "direct").last

  def initOnce: String = {
    val counter = new Counter
    val value = counter.make.init.last
    value + ":" + counter.calls
  }

  def lastOnce: String = {
    val counter = new Counter
    val box = counter.make.last
    val value = box.value
    value + ":" + counter.calls
  }
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Values.initialFirst)
    println(Values.initialLast)
    println(Values.lastText)
    println(Values.twiceLast)
    println(Values.singletonLast)
    println(Values.emptySize)
    println(Values.consLast)
    println(Values.directLast)
    println(Values.initOnce)
    println(Values.lastOnce)
  }
}
