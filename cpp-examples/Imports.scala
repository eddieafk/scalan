package demo.examples

object Library {
  object Config {
    val answer: Int = 42
  }

  class Counter {
    def value: Int = 7
  }
}

import demo.examples.Library.{Config, Counter => Tally}

object Imports {
  val typed: Tally = new Tally()

  def read(counter: Tally): Int = counter.value

  def main = {
    println(Config.answer)
    println(read(typed))
  }
}
