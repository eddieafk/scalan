package demo.examples

object FormattedInterpolation {
  def main = {
    val pi = 3.14159
    val count = 7
    val total = 9876543210L
    val initial = 'N'
    val enabled = true
    val disabled = false
    val project = "Scala Native"
    println(f"pi=$pi%.2f!")
    println(f"next=${pi + 1.0}%.1f")
    println(f"count=$count%04d")
    println(f"total=$total%012d")
    println(f"initial=$initial%c")
    println(f"enabled=$enabled%b")
    println(f"disabled=$disabled%b")
    println(f"project=$project%s")
  }
}
