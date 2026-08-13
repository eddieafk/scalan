package demo.examples

class AnyBox(var value: Any) {
  def replaceWithLong(): Long = {
    value = 2L
    value.asInstanceOf[Long]
  }

  def replaceWithBoolean(): Int = {
    value = true
    if (value.asInstanceOf[Boolean]) 1 else 0
  }
}

class AnyParent(val value: Any)

class AnyChild(seed: Int) extends AnyParent(seed) {
  def parentInt: Int = value.asInstanceOf[Int]
}

object AnyConstructorParameters {
  def main = {
    val box = new AnyBox(1)
    println(box.value.asInstanceOf[Int])
    println(box.replaceWithLong())
    println(box.replaceWithBoolean())
    val child = new AnyChild(3)
    println(child.parentInt)
  }
}
