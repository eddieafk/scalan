package demo.examples

trait Named {
  def name: String = "named"
}

trait Labeled extends Named {
  def label: String = "labeled"
}

class BaseValue

class FancyValue extends BaseValue with Labeled

class OtherValue

object RuntimeTypeTests {
  def report(value: BaseValue) = {
    println(value.isInstanceOf[BaseValue])
    println(value.isInstanceOf[FancyValue])
    println(value.isInstanceOf[Named])
    println(value.isInstanceOf[Labeled])
    println(value.isInstanceOf[OtherValue])
    println(value.asInstanceOf[BaseValue].isInstanceOf[FancyValue])
    println(value.asInstanceOf[Labeled].label)
    println(value.asInstanceOf[FancyValue].name)
  }

  def main = {
    val value: BaseValue = new FancyValue()
    report(value)
    val missing: BaseValue = null.asInstanceOf[BaseValue]
    println(missing.isInstanceOf[BaseValue])
  }
}
