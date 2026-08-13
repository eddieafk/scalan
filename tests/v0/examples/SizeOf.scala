package demo.examples

class SizePair(val left: Int, val right: Int)

object SizeOf {
  def main = {
    println(sizeof[Unit])
    println(sizeof[Boolean])
    println(sizeof[Int])
    println(sizeof[Long])
    println(sizeof[SizePair])
  }
}
