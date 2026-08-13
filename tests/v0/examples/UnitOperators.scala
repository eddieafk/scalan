package demo.examples

object UnitOperators {
  def main = {
    val unitAny: Any = {}
    val intAny: Any = 7

    println({} == {})
    println({} != {})
    println({} == 7)
    println(7 != {})
    println({} == null)
    println(null != {})
    println(unitAny == {})
    println({} != unitAny)
    println(intAny == {})
    println({} != intAny)
  }
}
