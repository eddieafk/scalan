package demo.examples

object BooleanArrays {
  def flipAt(values: Array[Boolean], index: Int): Boolean = {
    values(index) = !values(index)
    values(index)
  }

  def asInt(value: Boolean): Int = if (value) 1 else 0

  def main = {
    val flags = Array(true, false)
    println(flags.length)
    println(asInt(flipAt(flags, 1)))
    println(asInt(flags(0)))
    println(asInt(flags(1)))
  }
}
