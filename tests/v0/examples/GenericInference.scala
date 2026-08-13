package demo.genericinference

class Label(val code: Int)

class Box[A](val value: A) {
  def get(): A = value
  def choose[B](next: B): B = next
}

class Pair[A, B](val first: A, val second: B)
class Restricted[A <: Label](val value: A)

object GenericInference {
  def identity[A](value: A): A = value
  def unwrap[A](box: Box[A]): A = box.get()
  def wider[A](left: A, right: A): A = right
  def duplicated[A](value: A): Pair[A, A] = new Pair(value, value)

  def main = {
    val numbers = new Box(40)
    val pair = new Pair("inferred", 7L)
    val nested = unwrap(new Box(new Label(2)))
    val bounded = new Restricted(new Label(3))
    val duplicate = duplicated(new Label(4))

    println(identity(numbers.choose(41)))
    println(pair.first)
    println(pair.second)
    println(nested.code + bounded.value.code)
    println(wider(1, 9L))
    println(duplicate.first.code + duplicate.second.code)
  }
}
