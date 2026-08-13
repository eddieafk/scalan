package demo.examples

trait Named {
  def name: String = "Named trait"
}

class Counter {
  def zero: Int = 0
  def label: String = "Counter class"
}

object ClassTraitDeclarations {
  def main = println("class and trait declarations are in NIR")
}
