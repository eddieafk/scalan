package tests.v010alpha010.polymorphicfunctiontypes

class Box[A](val value: A)

object Definitions {
  def identity(): [A] => A => A =
    [A] => (value: A) => value

  def captured(prefix: String): [A] => A => String =
    [A] => (value: A) => prefix + value.toString

  def retain[A](captured: A): [B] => B => A =
    [B] => (ignored: B) => captured

  def map(function: [A] => A => String): (String, String) =
    (1, false).map(function)
}
