package demo.projections

trait Carrier {
  type Element
}

trait IntCarrier extends Carrier {
  override type Element = Int
}

object Main {
  def echo(value: IntCarrier#Element): IntCarrier#Element = value

  def main =
    println(echo(13))
}
