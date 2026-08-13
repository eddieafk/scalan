package examples.polymorphicfunctioninvocation

class Box[A](val value: A)
class Token(val label: String)

class Source {
  var calls: Int = 0

  def next: Int = {
    calls = calls + 1
    7
  }
}

object Values {
  def identity: Int = ([A] => (value: A) => value)[Int](42)

  def text: String =
    ([A] => (value: A) => value.toString)[Boolean](true)

  def boxed: Box[String] =
    ([A] => (value: A) => new Box(value))[String]("boxed")

  def constant: Int =
    ([A] => (value: A) => 9)[String]("ignored")

  def reference: Token =
    ([A] => (value: A) => value)[Token](new Token("reference"))

  def observed: String = {
    val source = new Source
    var calls = 0
    val result = ([A] => (value: A) => {
      calls = calls + 1
      calls + ":" + value.toString
    })[Int](source.next)
    result + ":" + calls + ":" + source.calls
  }
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Values.identity)
    println(Values.text)
    println(Values.boxed.value)
    println(Values.constant)
    println(Values.reference.label)
    println(Values.observed)
  }
}
