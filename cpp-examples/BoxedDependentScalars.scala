package demo.boxedscalars

trait ScalarBox {
  type Item
  def value: Item
  def echo(value: Item): Item
}

class BooleanBox extends ScalarBox {
  override type Item = Boolean
  override def value: Item = true
  override def echo(value: Item): Item = value
}

class LongBox extends ScalarBox {
  override type Item = Long
  override def value: Item = 42L
  override def echo(value: Item): Item = value
}

class FloatBox extends ScalarBox {
  override type Item = Float
  override def value: Item = 1.5F
  override def echo(value: Item): Item = value
}

class DoubleBox extends ScalarBox {
  override type Item = Double
  override def value: Item = 2.25
  override def echo(value: Item): Item = value
}

class CharBox extends ScalarBox {
  override type Item = Char
  override def value: Item = 'Z'
  override def echo(value: Item): Item = value
}

object Main {
  def main = {
    val booleanBox: BooleanBox = new BooleanBox()
    val longBox: LongBox = new LongBox()
    val floatBox: FloatBox = new FloatBox()
    val doubleBox: DoubleBox = new DoubleBox()
    val charBox: CharBox = new CharBox()
    val forwarded = longBox

    println(booleanBox.echo(booleanBox.value))
    println(longBox.echo(longBox.value))
    println(forwarded.echo(forwarded.value))
    println(floatBox.echo(floatBox.value))
    println(doubleBox.echo(doubleBox.value))
    println(charBox.echo(charBox.value))
  }
}
