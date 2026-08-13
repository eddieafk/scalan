package demo.examples

object UnitEqualsHashCode {
  def main = {
    val unitAny: Any = {}
    val intAny: Any = 7

    println({}.equals({}))
    println({}.equals(7))
    println({}.equals(null))
    println({}.equals(unitAny))
    println({}.equals(intAny))
    println({}.hashCode)
    println(unitAny.hashCode == {}.hashCode)
  }
}
