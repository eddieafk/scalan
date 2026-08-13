package demo.pathdependent

class IntBox {
  type Item = Int
  def seed: Item = 7
}

object Main {
  def echo(box: IntBox, value: box.Item): box.Item = value

  def main = {
    val box = new IntBox()
    println(echo(box, box.seed))
  }
}
