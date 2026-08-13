package demo.generics

trait Producer[A] {
  def produce(): A
}

class Label(val code: Int)

class Box[A](val value: A) {
  def get(): A = value
  def replaced(next: A): Box[A] = new Box[A](next)
  def choose[B](next: B): B = next
}

class Pair[A, B](val first: A, val second: B)

object ReferenceGenerics {
  def identity[A](value: A): A = value

  def main = {
    val box: Box[Label] = new Box[Label](new Label(40))
    val pair: Pair[Label, Label] =
      new Pair[Label, Label](box.get(), box.choose[Label](new Label(2)))
    val text = new Box[String]("reference")
    val producer: Producer[Label] = null
    val replaced = box.replaced(pair.second)

    println(pair.first.code + identity[Label](pair.second).code)
    println(replaced.get().code)
    println(text.get())
  }
}
