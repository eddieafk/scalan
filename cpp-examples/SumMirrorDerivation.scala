package demo.mirrorsum

trait Ordinal[A] {
  def ordinal(value: A): Int
}

class DerivedOrdinal[A](val mirror: scala.deriving.Mirror.SumOf[A])
    extends Ordinal[A] {
  override def ordinal(value: A): Int = mirror.ordinal(value)
}

class StringOrdinal extends Ordinal[String] {
  override def ordinal(value: String): Int = 99
}

object Ordinal {
  given stringOrdinal: Ordinal[String] = new StringOrdinal

  def derived[A](using mirror: scala.deriving.Mirror.SumOf[A]): Ordinal[A] =
    new DerivedOrdinal[A](mirror)
}

sealed trait Event derives Ordinal
class Started(val id: Int) extends Event
class Stopped(val reason: String) extends Event
class Failed(val code: Int) extends Event

sealed trait GenericEvent[A] derives Ordinal
class GenericStarted[A](val value: A) extends GenericEvent[A]
class GenericStopped[A](val value: A) extends GenericEvent[A]

sealed trait Maybe[+A] derives Ordinal
object Empty extends Maybe[Nothing]
class Present[+A](val value: A) extends Maybe[A]

sealed trait Choice[+A, +B] derives Ordinal
class First[+A](val value: A) extends Choice[A, Nothing]
class Second[+B](val value: B) extends Choice[Nothing, B]

sealed trait Routed[A, B] derives Ordinal
class Straight[A, B] extends Routed[A, B]
class Reversed[X, Y] extends Routed[Y, X]

sealed trait Handler[-A] derives Ordinal
object Ignore extends Handler[Object]
class Use[-A](value: A) extends Handler[A]

sealed trait NestedEvent derives Ordinal
object NestedEvent {
  object Cases {
    object Pending extends NestedEvent
    class Recorded(val code: Int) extends NestedEvent
  }
}

sealed trait NestedMaybe[+A] derives Ordinal
object NestedMaybe {
  object Cases {
    object Missing extends NestedMaybe[Nothing]
    class Found[+A](val value: A) extends NestedMaybe[A]
  }
}

sealed trait Placed derives Ordinal
class Before extends Placed
object PlacedCases {
  object Middle extends Placed
}
class After extends Placed

sealed trait Signal derives Ordinal
object Idle extends Signal
class Active(val value: Int) extends Signal
object Done extends Signal

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
    println(
      ordinal[GenericEvent[String]](new GenericStarted[String]("begin")))
    println(
      ordinal[GenericEvent[String]](new GenericStopped[String]("done")))
    println(
      sumMirror[GenericEvent[String]]() !=
        sumMirror[GenericEvent[String]]())
    println(
      mirror[GenericEvent[String]]() !=
        sumMirror[GenericEvent[String]]())
    println(ordinal[Signal](Idle))
    println(ordinal[Signal](new Active(1)))
    println(ordinal[Signal](Done))
    println(ordinal[Maybe[String]](Empty))
    println(ordinal[Maybe[String]](new Present[String]("value")))
    println(ordinal[Choice[String, String]](new First[String]("left")))
    println(ordinal[Choice[String, String]](new Second[String]("right")))
    println(
      sumMirror[Routed[Object, String]]().ordinal(
        new Reversed[String, Object]))
    println(ordinal[Handler[String]](Ignore))
    println(ordinal[Handler[String]](new Use[String]("value")))
    println(ordinal[NestedEvent](NestedEvent.Cases.Pending))
    println(ordinal[NestedEvent](new NestedEvent.Cases.Recorded(3)))
    println(ordinal[NestedMaybe[String]](NestedMaybe.Cases.Missing))
    println(
      ordinal[NestedMaybe[String]](
        new NestedMaybe.Cases.Found[String]("value")))
    println(ordinal[Placed](new Before))
    println(ordinal[Placed](PlacedCases.Middle))
    println(ordinal[Placed](new After))
  }
}
