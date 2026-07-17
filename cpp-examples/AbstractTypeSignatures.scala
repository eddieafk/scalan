package demo.typesignatures

trait Codec {
  type Value
  val seed: Value
  def default: Value
  def echo(value: Value): Value
}

trait IntCodec extends Codec {
  override type Value = Int
  override val seed: Value = 5
  override def default: Value = 7
  override def echo(value: Value): Value = value
}

class ConcreteCodec extends IntCodec

object Main {
  def show(codec: IntCodec) = {
    println(codec.seed)
    println(codec.default)
    println(codec.echo(9))
  }

  def main =
    show(new ConcreteCodec())
}
