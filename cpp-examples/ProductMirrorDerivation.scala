package demo.mirrorproduct

trait Rebuild[A] {
  def rebuild(product: scala.Product): A
}

class DerivedRebuild[A](val mirror: scala.deriving.Mirror.ProductOf[A])
    extends Rebuild[A] {
  override def rebuild(product: scala.Product): A = mirror.fromProduct(product)
}

class StringRebuild extends Rebuild[String] {
  override def rebuild(product: scala.Product): String =
    product.productElement(0).asInstanceOf[String]
}

object Rebuild {
  given stringRebuild: Rebuild[String] = new StringRebuild

  def derived[A](using mirror: scala.deriving.Mirror.ProductOf[A]): Rebuild[A] =
    new DerivedRebuild[A](mirror)
}

class Pair(val number: Int, val text: String) derives Rebuild
class GenericPair[A, B](val value: A, val label: B, val count: Int) derives Rebuild
class Empty() derives Rebuild {
  def marker(): Int = 0
}

class Product0() extends scala.Product {
  override def productArity(): Int = 0
  override def productElement(index: Int): Object = null
}

class Product2(val first: Object, val second: Object) extends scala.Product {
  override def productArity(): Int = 2
  override def productElement(index: Int): Object =
    if (index == 0) first else second
}

class Product3(val first: Object, val second: Object, val third: Object)
    extends scala.Product {
  override def productArity(): Int = 3
  override def productElement(index: Int): Object =
    if (index == 0) first else if (index == 1) second else third
}

object ProductMirrorDerivation {
  def rebuild[A](product: scala.Product)(using instance: Rebuild[A]): A =
    instance.rebuild(product)

  def mirror[A]()(using instance: scala.deriving.Mirror.ProductOf[A]):
      scala.deriving.Mirror.ProductOf[A] = instance

  def mirrorOf[A]()(using instance: scala.deriving.Mirror.Of[A]):
      scala.deriving.Mirror.Of[A] = instance

  def mono[A](instance: scala.deriving.Mirror.ProductOf[A], value: A):
      instance.MirroredMonoType = value

  def elementTypes[A](
      instance: scala.deriving.Mirror.ProductOf[A],
      value: instance.MirroredElemTypes): instance.MirroredElemTypes = value

  def elementLabels[A](
      instance: scala.deriving.Mirror.ProductOf[A],
      value: instance.MirroredElemLabels): instance.MirroredElemLabels = value

  def label[A](
      instance: scala.deriving.Mirror.ProductOf[A],
      value: instance.MirroredLabel): instance.MirroredLabel = value

  def main = {
    println(rebuild[Empty](new Product0()).marker())
    val pair: Pair = rebuild[Pair](new Product2(42, "answer"))
    println(pair.number)
    println(pair.text)
    println(mono[Pair](mirror[Pair](), pair).number)
    println(mirror[Pair]() == mirror[Pair]())
    println(mirrorOf[Pair]() == mirror[Pair]())
    val generic: GenericPair[String, String] =
      rebuild[GenericPair[String, String]](new Product3("generic", "label", 7))
    println(generic.value)
    println(generic.label)
    println(generic.count)
    println(
      mirror[GenericPair[String, String]]() !=
        mirror[GenericPair[String, String]]())
  }
}
