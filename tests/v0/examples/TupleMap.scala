package examples.tuplemap

class Box[A](val value: A)

object Identity extends PolyFunction {
  def apply[A](value: A): A = value
}

object Wrap extends PolyFunction {
  def apply[A](value: A): Box[A] = new Box(value)
}

object Text extends PolyFunction {
  def apply[A](value: A): String = value.toString
}

class CountingText extends PolyFunction {
  var calls: Int = 0

  def apply[A](value: A): String = {
    calls = calls + 1
    calls + ":" + value.toString
  }
}

class MapperFactory {
  var calls: Int = 0
  val mapper: CountingText = new CountingText

  def nextMapper: CountingText = {
    calls = calls + 1
    mapper
  }
}

class TupleSource {
  var calls: Int = 0

  def next: Int = {
    calls = calls + 1
    7
  }

  def tuple: (Int, String, Boolean) = (next, "x", true)
}

object Values {
  def identity: (Int, String, Boolean) = (1, "same", true).map(Identity)

  def wrapped: (Box[Int], Box[String], Box[Boolean]) =
    (2, "boxed", false).map(Wrap)

  def text: (String, String, String) = (3, "text", true).map(Text)
  def empty: EmptyTuple = EmptyTuple.map(Text)

  def observed: String = {
    val source = new TupleSource
    val factory = new MapperFactory
    val mapped = source.tuple.map(factory.nextMapper)
    val first = mapped._1
    val second = mapped._2
    val third = mapped._3
    first + "|" + second + "|" + third + ":" + source.calls + ":" +
      factory.calls + ":" + factory.mapper.calls
  }

  def emptyObserved: String = {
    val factory = new MapperFactory
    val mapped = EmptyTuple.map(factory.nextMapper)
    mapped.size + ":" + factory.calls + ":" + factory.mapper.calls
  }
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Values.identity._1)
    println(Values.identity._2)
    println(Values.identity._3)
    println(Values.wrapped._1.value)
    println(Values.wrapped._2.value)
    println(Values.wrapped._3.value)
    println(Values.text._1)
    println(Values.text._2)
    println(Values.text._3)
    println(Values.empty.size)
    println(Values.observed)
    println(Values.emptyObserved)
  }
}
