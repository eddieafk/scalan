package demo

class Failure(val code: Int) extends Exception("failure")

object Thrower {
    def fail(): Int = {
        val failure = new Failure(7)
        throw failure
    }
}

object Main {
  def main = {
    println("before")
    Zone.scoped({
      Zone.scoped({
        Thrower.fail()
      })
    })
    println("after")
  }
}
