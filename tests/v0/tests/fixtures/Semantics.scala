package demo.fixtures

object Config {
  val answer: Int = 42
}

object MathBox {
  def add(a: Int, b: Int): Int = a + b
  val selected = Config.answer
  val called = add(1, 2)
  val text = "hello" + " native"
}
