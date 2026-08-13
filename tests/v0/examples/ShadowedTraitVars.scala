package demo.shadowedvars

trait LeftState {
  var value: Int = {
    println("left var init")
    3
  }
}

trait RightState {
  var value: Int = {
    println("right var init")
    9
  }
}

class CombinedState extends LeftState with RightState {
  def current: Int = value
  def left: Int = super[LeftState].value
  def right: Int = super[RightState].value

  def updateCurrent(next: Int): Int = {
    value = next
    value
  }

  def updateLeft(next: Int): Int = {
    super[LeftState].value = next
    super[LeftState].value
  }

  def updateRight(next: Int): Int = {
    super[RightState].value = next
    super[RightState].value
  }
}

object Main {
  def main = {
    val state = new CombinedState()
    println(state.current)
    println(state.left)
    println(state.right)
    println(state.updateLeft(4))
    println(state.current)
    println(state.updateRight(10))
    println(state.current)
    println(state.updateCurrent(12))
    println(state.left)
    println(state.right)
  }
}
