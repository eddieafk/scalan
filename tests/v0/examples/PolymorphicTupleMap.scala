package examples.polymorphictuplemap

class Box[A](val value: A)

class TupleSource {
  var calls: Int = 0

  def next: Int = {
    calls = calls + 1
    7
  }

  def tuple: (Int, String, Boolean) = (next, "x", true)
}

object Values {
  def identity: (Int, String, Boolean) =
    (1, "same", true).map([A] => (value: A) => value)

  def wrapped: (Box[Int], Box[String], Box[Boolean]) =
    (2, "boxed", false).map([A] => (value: A) => new Box(value))

  def text: (String, String, String) =
    (3, "text", true).map([A] => (value: A) => value.toString)

  def constant: (Int, Int, Int) =
    (4, "constant", false).map([A] => (value: A) => 9)

  def observed: String = {
    val source = new TupleSource
    var calls = 0
    val mapped = source.tuple.map([A] => (value: A) => {
      calls = calls + 1
      calls + ":" + value.toString
    })
    mapped._1 + "|" + mapped._2 + "|" + mapped._3 + ":" +
      source.calls + ":" + calls
  }

  def emptyObserved: String = {
    var calls = 0
    val mapped = EmptyTuple.map([A] => (value: A) => {
      calls = calls + 1
      value
    })
    mapped.size + ":" + calls
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
    println(Values.constant._1)
    println(Values.constant._3)
    println(Values.observed)
    println(Values.emptyObserved)
  }
}
