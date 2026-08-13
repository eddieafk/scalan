package demo.primitivegenerics

class Cell[A](var value: A) {
  def get(): A = value

  def set(next: A): Unit = {
    value = next
  }

  def choose[B](next: B): B = next
}

object PrimitiveGenerics {
  def identity[A](value: A): A = value

  def main = {
    val ints = new Cell[Int](40)
    ints.value = identity[Int](41)
    ints.set(ints.choose[Int](42))

    println(ints.get())
    println(new Cell[Long](7L).get())
    println(if (new Cell[Boolean](true).get()) 1 else 0)
    println(new Cell[Char]('Z').get())
    println(new Cell[Byte](1.toByte).get().toInt)
    println(new Cell[Short](2.toShort).get().toInt)
    println(if (new Cell[Float](1.5f).get() > 1.0f) 1 else 0)
    println(if (new Cell[Double](2.5).get() > 2.0) 1 else 0)
    println(if (new Cell[Symbol]('ready).get() == 'ready) 1 else 0)
    println(new Cell[Unit](println("unit")).get().toString)
  }
}
