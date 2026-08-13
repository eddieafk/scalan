// expected-error: runtime polymorphic function closures cannot capture mutable local values yet: prefix
// expected-error: runtime polymorphic function closures cannot capture erased compiler-known values yet: identity
// expected-error: runtime polymorphic function closures cannot capture super yet

package tests.v010alpha010.invalidruntimepolymorphiccaptures

object Invalid {
  def mutableLocal(): [A] => A => String = {
    var prefix = "mutable:";
    [A] => (value: A) => prefix + value.toString
  }

  def erasedLocal(): [A] => A => String = {
    val identity = [A] => (value: A) => value;
    [A] => (value: A) => identity[String]("erased")
  }
}

class Parent {
  def name: String = "parent"
}

class Child extends Parent {
  def fromSuper(): [A] => A => String =
    [A] => (value: A) => super.name + value.toString
}
