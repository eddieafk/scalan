package demo.abstractdependent

class BaseItem {
  def label: String = "base"
}

class SpecialItem extends BaseItem {
  override def label: String = "special"
}

trait Box {
  type Item <: BaseItem
  def value: Item
}

class SpecialBox extends Box {
  override type Item = SpecialItem
  override def value: Item = new SpecialItem()
}

object Main {
  def show(box: Box, item: box.Item): String = item.label
  def widen(item: Box#Item): BaseItem = item

  def main = {
    val box: Box = new SpecialBox()
    println(show(box, box.value))
  }
}
