package demo.examples

trait Named {
  def name: String
  def score: Int
}

class Plain extends Named {
  def name: String = "plain"
  def score: Int = 1
}

class Fancy extends Named {
  def name: String = "fancy"
  def score: Int = 9
}

object TraitArrays {
  def replaceAndShow(values: Array[Named], index: Int, replacement: Named): String = {
    values(index) = replacement
    values(index).name + " " + values(index).score
  }

  def main = {
    val values = Array[Named](new Plain())
    println(replaceAndShow(values, 0, new Fancy()))
    println(values(0).name)
  }
}
