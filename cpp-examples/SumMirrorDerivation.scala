package demo.mirrorsum

trait Ordinal[A] {
  def ordinal(value: A): Int
}

class DerivedOrdinal[A](val mirror: scala.deriving.Mirror.SumOf[A])
    extends Ordinal[A] {
  override def ordinal(value: A): Int = mirror.ordinal(value)
}

object Ordinal {
  def derived[A](using mirror: scala.deriving.Mirror.SumOf[A]): Ordinal[A] =
    new DerivedOrdinal[A](mirror)
}

sealed trait Event derives Ordinal
class Started(val id: Int) extends Event
class Stopped(val reason: String) extends Event
class Failed(val code: Int) extends Event

object SumMirrorDerivation {
  def ordinal[A](value: A)(using instance: Ordinal[A]): Int =
    instance.ordinal(value)

  def sumMirror[A]()(using instance: scala.deriving.Mirror.SumOf[A]):
      scala.deriving.Mirror.SumOf[A] = instance

  def mirror[A]()(using instance: scala.deriving.Mirror.Of[A]):
      scala.deriving.Mirror.Of[A] = instance

  def mono[A](instance: scala.deriving.Mirror.SumOf[A], value: A):
      instance.MirroredMonoType = value

  def label[A](
      instance: scala.deriving.Mirror.SumOf[A],
      value: instance.MirroredLabel): instance.MirroredLabel = value

  def main = {
    println(ordinal[Event](new Started(1)))
    println(ordinal[Event](new Stopped("done")))
    println(ordinal[Event](new Failed(2)))
    println(sumMirror[Event]().ordinal(new Failed(3)))
    println(sumMirror[Event]() == sumMirror[Event]())
    println(mirror[Event]() == sumMirror[Event]())
    println(mono[Event](sumMirror[Event](), new Started(4)).isInstanceOf[Started])
  }
}
