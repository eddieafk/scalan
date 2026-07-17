package demo.intervaltypes

class WiderItem {
  def label: String = "wider"
}

class UpperItem extends WiderItem {
  override def label: String = "upper"
}

class MiddleItem extends UpperItem {
  override def label: String = "middle"
}

class LowerItem extends MiddleItem {
  override def label: String = "lower"
}

trait ItemCodec {
  type Item >: LowerItem <: UpperItem
  def describe(item: Item): String
}

trait MiddleCodec extends ItemCodec {
  override type Item = MiddleItem
  override def describe(item: Item): String = item.label
}

class ConcreteCodec extends MiddleCodec

object Main {
  def show(codec: MiddleCodec, item: MiddleItem) =
    println(codec.describe(item))

  def main =
    show(new ConcreteCodec(), new MiddleItem())
}
