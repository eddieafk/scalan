package demo.traitvars

trait MutableCounter {
  var current: Int

  def bump: Int = {
    current = current + 1
    current
  }
}

trait InitializedCounter extends MutableCounter {
  override var current: Int = {
    println("trait var init")
    2
  }
}

class TraitCounter extends InitializedCounter

class ParameterCounter(var current: Int) extends MutableCounter

class BodyCounter extends MutableCounter {
  override var current: Int = 30
}

object Main {
  def change(counter: MutableCounter, value: Int) = {
    counter.current = value
    println(counter.bump)
    println(counter.current)
  }

  def main = {
    change(new TraitCounter(), 5)
    change(new ParameterCounter(10), 20)
    change(new BodyCounter(), 40)
  }
}
