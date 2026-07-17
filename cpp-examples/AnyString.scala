package demo.examples

object AnyString {
  def join(left: String, right: String): String = left + right

  def main = {
    val direct: Any = "Scala"
    val joined: Any = join("Sca", "la")
    val different: Any = "Native"
    val values = Array[Any]("left", join("ri", "ght"))
    val missing: Any = null

    println(direct.toString)
    println(direct == joined)
    println(direct.equals("Scala"))
    println("Scala".equals(direct))
    println(direct == different)
    println(direct.isInstanceOf[String])
    println(direct.asInstanceOf[String])
    println(direct.hashCode == "Scala".hashCode)
    println(values(0).asInstanceOf[String])
    println(values(1) == "right")
    println(missing == direct)
  }
}
