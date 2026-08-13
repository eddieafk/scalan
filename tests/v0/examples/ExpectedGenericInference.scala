package demo.expectedinference

class Label(val code: Int)
class Token[A]
class Duo[A, B](val value: A)

object ExpectedGenericInference {
  def empty[A](): A = null
  def convert[A, B](value: A): B = null
  def token[A](): Token[A] = new Token()
  def delegated(): Label = {
    empty()
  }
  def conditional(flag: Boolean): Label =
    if (flag) new Label(1) else empty()
  def returned(): Label = return empty()
  def bounded[A <: Label](): A = null

  val field: Label = empty()

  def main = {
    val label: Label = empty()
    val partial: Label = convert(42)
    val marker: Token[Label] = new Token()
    val nested: Token[Label] = token()
    val duo: Duo[Int, Label] = new Duo(7)
    val boundedLabel: Label = bounded()

    println(if (field == null) 1 else 0)
    println(if (label == null) 1 else 0)
    println(if (partial == null) 1 else 0)
    println(if (delegated() == null) 1 else 0)
    println(if (conditional(false) == null) 1 else 0)
    println(if (returned() == null) 1 else 0)
    println(if (marker == null) 0 else 1)
    println(if (nested == null) 0 else 1)
    println(if (boundedLabel == null) 1 else 0)
    println(duo.value)
  }
}
