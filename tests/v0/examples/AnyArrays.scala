package demo.examples

object AnyArrays {
  def replaceAndRead(values: Array[Any], index: Int, replacement: Int): Int = {
    values(index) = replacement
    values(index).asInstanceOf[Int]
  }

  def main = {
    val values = Array[Any](1, 2L, true, 'a')
    println(values.length)
    println(if (values(0).isInstanceOf[Int]) 1 else 0)
    println(if (values(1).isInstanceOf[Int]) 1 else 0)
    println(replaceAndRead(values, 0, 9))
    println(values(1).asInstanceOf[Long])
    println(if (values(2).asInstanceOf[Boolean]) 1 else 0)
    println(values(3).asInstanceOf[Char])
  }
}
