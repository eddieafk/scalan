package demo.examples

object CharArrays {
  def replaceAndRead(values: Array[Char], index: Int, replacement: Char): Char = {
    values(index) = replacement
    values(index)
  }

  def main = {
    val empty = Array[Char]()
    val letters = Array('a', 'b')
    println(empty.length)
    println(replaceAndRead(letters, 1, 'Z'))
    println(letters(1))
  }
}
