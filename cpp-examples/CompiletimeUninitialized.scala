package examples.compiletimeuninitialized

import scala.compiletime.uninitialized
import scala.compiletime.{uninitialized => zeroed}

class Slot[A] {
  var value: A = uninitialized
  var writes: Int = zeroed

  def set(next: A): Unit = {
    value = next
    writes = writes + 1
  }
}

object State {
  var ready: Boolean = scala.compiletime.uninitialized
  var label: String = uninitialized
}

object Main {
  def main(args: Array[String]): Unit = {
    val slot = new Slot[Any]();
    println(slot.value == null)
    println(slot.writes)
    println(State.ready)
    println(State.label == null)

    slot.set("assigned")
    println(slot.value)
    println(slot.writes)
  }
}
