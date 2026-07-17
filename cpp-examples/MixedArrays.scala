package demo.examples

object MixedArrays {
  def firstValue(values: Array[Any]): Int = values(0).asInstanceOf[Int]

  def main = {
    val values = Array(1, 2L, true, 'a')
    println(values.length)
    println(if (values(0).isInstanceOf[Int]) 1 else 0)
    println(firstValue(values))
    println(values(1).asInstanceOf[Long])
    println(if (values(2).asInstanceOf[Boolean]) 1 else 0)
    println(values(3).asInstanceOf[Char])
  }
}
