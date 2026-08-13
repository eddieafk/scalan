package demo.examples

class EqualityBox(val value: Int)

object Equals {
  def join(left: String, right: String): String = left + right

  def main = {
    val box = new EqualityBox(1)
    val same = box
    val other = new EqualityBox(1)
    val missing: EqualityBox = null
    val anyBox: Any = box
    val anySame: Any = same

    println(7.equals(7))
    println(7.equals(8))
    println(true.equals(false))
    println("Scala".equals(join("Sca", "la")))
    println(box.equals(same))
    println(box.equals(other))
    try {
      println(missing.equals(null))
    } catch {
      case error: NullPointerException => println(error.getMessage)
    }
    println(anyBox.equals(anySame))
    println(anyBox.equals(other))
  }
}
