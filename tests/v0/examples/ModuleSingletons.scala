package demo.examples

object Settings {
  println("settings init")
  val base: Int = 40
  var answer: Int = base + 2
}

object Reader {
  def read(settings: Settings): Int = settings.answer
}

object ModuleSingletons {
  def main = {
    val first: Settings = Settings
    val second: Settings = Settings
    println(Reader.read(first))
    second.answer = second.answer + 1
    println(first.answer)
  }
}
