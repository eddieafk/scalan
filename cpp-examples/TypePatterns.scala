package demo.examples

trait Tagged {
  def tag: String = "tagged-member"
}

class BaseValue

class FancyValue extends BaseValue with Tagged {
  def label: String = "fancy-member"
}

class TaggedValue extends BaseValue with Tagged

class OtherValue extends BaseValue

object TypePatterns {
  def classify(value: BaseValue, preferFancy: Boolean): String = value match {
    case _: FancyValue if preferFancy => "fancy"
    case _: Tagged => "tagged"
    case _ => "other"
  }

  def describe(value: BaseValue, preferFancy: Boolean): String = value match {
    case fancy: FancyValue if preferFancy => fancy.label
    case tagged: Tagged => tagged.tag
    case _ => "other"
  }

  def grouped(value: BaseValue): String = value match {
    case _: FancyValue | _: TaggedValue => "grouped"
    case _ => "other"
  }

  def main = {
    println(classify(new FancyValue(), true))
    println(classify(new FancyValue(), false))
    println(classify(new OtherValue(), true))
    println(describe(new FancyValue(), true))
    println(describe(new FancyValue(), false))
    println(describe(new TaggedValue(), true))
    println(describe(new OtherValue(), true))
    println(grouped(new FancyValue()))
    println(grouped(new TaggedValue()))
    println(grouped(new OtherValue()))
  }
}
