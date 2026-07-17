package demo.examples

object LocalAny {
  def main = {
    var current: Any = 1
    current = 2L
    val copied: Any = current
    println(current.asInstanceOf[Long])
    println(copied.asInstanceOf[Long])
    current = true
    println(if (current.asInstanceOf[Boolean]) 1 else 0)
  }
}
