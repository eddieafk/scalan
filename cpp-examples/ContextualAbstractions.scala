package demo.contextual

class Dog(val name: String)

trait Show[A] {
  def show(value: A): String
}

class DogShow(val prefix: String) extends Show[Dog] {
  override def show(value: Dog): String = prefix + value.name
}

object ContextualAbstractions {
  given dogShow: Show[Dog] = new DogShow("dog:")

  def render[A](value: A)(using show: Show[A]): String =
    show.show(value)

  def forwarded[A](value: A)(using show: Show[A]): String =
    render(value)

  def explicit(value: Dog): String =
    render(value)(using dogShow)

  def locally(value: Dog)(using show: Show[Dog]): String =
    render(value)

  def main = {
    println(render(new Dog("inferred")))
    println(forwarded(new Dog("forwarded")))
    println(explicit(new Dog("explicit")))
    println(locally(new Dog("local"))(using new DogShow("local:")))
  }
}
