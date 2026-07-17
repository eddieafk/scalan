package demo.examples

object MainArgs {
  def main(args: Array[String]): Unit = {
    if (args == null) {
      println(-1)
    } else {
      println(args.length)
      if (args.length > 0) {
        println(args(0))
      } else {
        println("no args")
      }
    }
  }
}
