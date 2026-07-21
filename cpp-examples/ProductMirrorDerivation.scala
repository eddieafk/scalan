package demo.mirrorproduct

trait Rebuild[A] {
  def rebuild(product: scala.Product): A
}

class DerivedRebuild[A](val mirror: scala.deriving.Mirror.ProductOf[A])
    extends Rebuild[A] {
  override def rebuild(product: scala.Product): A = mirror.fromProduct(product)
}

object Rebuild {
  def derived[A](using mirror: scala.deriving.Mirror.ProductOf[A]): Rebuild[A] =
    new DerivedRebuild[A](mirror)
}

class Pair(val number: Int, val text: String) derives Rebuild

class Product2(val first: Object, val second: Object) extends scala.Product {
  override def productArity(): Int = 2
  override def productElement(index: Int): Object =
    if (index == 0) first else second
}

object ProductMirrorDerivation {
  def rebuild[A](product: scala.Product)(using instance: Rebuild[A]): A =
    instance.rebuild(product)

  def mirror[A]()(using instance: scala.deriving.Mirror.ProductOf[A]):
      scala.deriving.Mirror.ProductOf[A] = instance

  def main = {
    val pair: Pair = rebuild[Pair](new Product2(42, "answer"))
    println(pair.number)
    println(pair.text)
    println(mirror[Pair]() == mirror[Pair]())
  }
}
