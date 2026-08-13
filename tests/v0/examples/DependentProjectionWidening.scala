package demo.projectionwidening

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
  def widen(box: Box, item: box.Item): Box#Item = item

  def label(item: Box#Item): String = item.label

  def main = {
    val box: Box = new SpecialBox()
    println(label(widen(box, box.value)))
  }
}
