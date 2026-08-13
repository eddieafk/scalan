package demo.examples

class BasePrintable {
  def toString: String = "base-printable"
}

class FancyPrintable extends BasePrintable {
  override def toString: String = "fancy-printable"
}

object CustomToString {
  def describe(value: BasePrintable): String = value.toString

  def main = {
    val base = new BasePrintable()
    val fancy = new FancyPrintable()
    val asBase: BasePrintable = fancy
    val asAny: Any = fancy

    println(base.toString)
    println(fancy.toString())
    println(describe(fancy))
    println("direct=" + fancy.toString)
    println(asBase)
    println(asAny)
    println("generic=" + asBase)
    println("any=" + asAny)
  }
}
