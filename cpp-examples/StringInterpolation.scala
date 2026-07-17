package demo.examples

object StringInterpolation {
  def greet(name: String): String =
    s"Hello, ${{ val punctuation = "!"; name + punctuation }}"
  def banner(name: String): String =
    s"""Banner: "$name"
ready"""
  def rawLine(name: String): String = raw"raw\n$name"

  def main = {
    val project = "Scala Native"
    val count = 7
    println(greet(project))
    println(s"${project + " works"}")
    println(s"count: $count")
    println(rawLine(project))
    println(banner(project))
  }
}
