package demo.examples


class Entry(val value: Int)

object ClassArrays {
  def replaceAndRead(values: Array[Entry], index: Int, replacement: Entry): Int = {
    values(index) = replacement
    values(index).value
  }

  def main = {
    val empty = Array[Entry]()
    val values = Array[Entry](new Entry(3))
    val replacement = new Entry(9)
    println(empty.length)
    println(replaceAndRead(values, 0, replacement))
    println(values(0).value)
  }
}
