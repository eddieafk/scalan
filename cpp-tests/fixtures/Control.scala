package demo.fixtures

object Control {
  def choose(flag: Boolean): Int = if (flag) 1 else 2

  def loopOnce = {
    val start = 0
    while (false) start
    start
  }
}
