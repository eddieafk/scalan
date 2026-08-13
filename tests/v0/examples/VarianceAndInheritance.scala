package demo.variantgenerics

class Animal(val name: String)
class Dog(name: String) extends Animal(name)

trait Source[+A] {
  def get(): A
}

trait Sink[-A] {
  def put(value: A): Int
}

trait Handler[-A] {
  def handle(value: A): Int
}

trait Registry[+A] {
  def register(handler: Handler[A]): Int
}

trait NamedSource[+A] extends Source[A]

class DogSource(val dog: Dog) extends Source[Dog] {
  override def get(): Dog = dog
}

class NamedDogSource(val dog: Dog) extends NamedSource[Dog] {
  override def get(): Dog = dog
}

class ValueSource[+A](val value: A) extends Source[A] {
  override def get(): A = value
}

class AnimalSink extends Sink[Animal] {
  override def put(value: Animal): Int = 7
}

class Holder[A](val value: A)
class DogHolder(value: Dog) extends Holder[Dog](value)
class DefaultSource[A](val value: A) {
  def get(): A = value
}
class DogDefaultSource(value: Dog) extends DefaultSource[Dog](value) {
  override def get(): Dog = super.get()
}

object VarianceAndInheritance {
  def main = {
    val direct: Source[Animal] = new DogSource(new Dog("direct"))
    val transitive: Source[Animal] =
      new NamedDogSource(new Dog("transitive"))
    val forwarded: Source[Animal] =
      new ValueSource[Dog](new Dog("forwarded"))
    val sink: Sink[Dog] = new AnimalSink
    val holder: Holder[Dog] = new DogHolder(new Dog("inherited field"))
    val defaulted = new DogDefaultSource(new Dog("generic super"))

    println(direct.get().name)
    println(transitive.get().name)
    println(forwarded.get().name)
    println(sink.put(new Dog("ignored")))
    println(holder.value.name)
    println(defaulted.get().name)
  }
}
