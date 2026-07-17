package demo.examples

class Box {
  var value: Any = 1

  def replaceWithLong(): Long = {
    value = 2L
    value.asInstanceOf[Long]
  }

  def replaceWithBoolean(): Int = {
    value = true
    if (value.asInstanceOf[Boolean]) 1 else 0
  }
}

object AnyFields {
  var moduleValue: Any = 3

  def main = {
    val box = new Box()
    println(box.value.asInstanceOf[Int])
    println(box.replaceWithLong())
    println(box.replaceWithBoolean())
    moduleValue = 4L
    println(moduleValue.asInstanceOf[Long])
  }
}
