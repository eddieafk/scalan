package demo.boundedtypes

class BaseItem {
  def label: String = "base"
}

class SpecialItem extends BaseItem {
  override def label: String = "special"
}

trait ItemCodec {
  type Item <: BaseItem
  def describe(item: Item): String
}

trait SpecialCodec extends ItemCodec {
  override type Item = SpecialItem
  override def describe(item: Item): String = item.label
}

class ConcreteCodec extends SpecialCodec

object Main {
  def show(codec: SpecialCodec, item: SpecialItem) =
    println(codec.describe(item))

  def main =
    show(new ConcreteCodec(), new SpecialItem())
}
