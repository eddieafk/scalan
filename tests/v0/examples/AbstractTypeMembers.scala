package demo.abstracttypes

trait Carrier {
  type Element
  def label: String = "carrier"
}

trait IntCarrier extends Carrier {
  override type Element = Int
  override def label: String = "int"
}

class DirectCarrier extends Carrier {
  type Element = String
  override def label: String = "string"
}

class InheritedCarrier extends IntCarrier

object Main {
  def show(carrier: Carrier) =
    println(carrier.label)

  def main = {
    show(new DirectCarrier())
    show(new InheritedCarrier())
  }
}
