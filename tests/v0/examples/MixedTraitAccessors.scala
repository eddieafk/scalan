package demo.mixedaccessors

trait Readable {
  val value: Int
}

trait Mutable {
  var value: Int = 7

  def increment: Int = {
    value = value + 1
    value
  }
}

class State extends Readable with Mutable

object Main {
  def read(state: Readable) =
    println(state.value)

  def update(state: Mutable) = {
    state.value = 20
    println(state.increment)
  }

  def main = {
    val state = new State()
    read(state)
    update(state)
    read(state)
  }
}
