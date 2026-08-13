package examples.storedpolymorphicfunction

class Box[A](val value: A)

class Renderer(val prefix: String) {
  val render = [A] => (value: A) => prefix + value.toString

  def own: String = render[Int](7)
}

class ReceiverSource {
  var calls: Int = 0

  def next: Renderer = {
    calls = calls + 1
    new Renderer("selected:")
  }
}

object Values {
  val identity = [A] => (value: A) => value
  val boxed = [A] => (value: A) => new Box(value)

  def member: String =
    identity[Int](42).toString + ":" + identity[String]("same")

  def boxedValue: Box[String] = boxed[String]("boxed")

  def local: String = {
    var calls = 0
    val observed = [A] => (value: A) => {
      calls = calls + 1
      calls + ":" + value.toString
    }
    observed[Int](7) + "|" + observed[String]("x") + ":" + calls
  }

  def selected: String = {
    val source = new ReceiverSource
    val result = source.next.render[Boolean](true)
    result + ":" + source.calls
  }

  def classOwned: String = new Renderer("class:").own
}

object Main {
  def main(args: Array[String]): Unit = {
    println(Values.member)
    println(Values.boxedValue.value)
    println(Values.local)
    println(Values.selected)
    println(Values.classOwned)
  }
}
