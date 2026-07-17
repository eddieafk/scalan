package demo.examples

object AnyParameters {
  def inspect(value: Any): Int = {
    if (value.isInstanceOf[Int]) value.asInstanceOf[Int] else 0
  }

  def echo(value: Any): Any = value

  def main = {
    println(inspect(7))
    println(inspect(8L))
    println(echo(9).asInstanceOf[Int])
  }
}
