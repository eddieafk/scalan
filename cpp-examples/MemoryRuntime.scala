package demo.examples

class Node(var next: Node, val value: Int)

object Roots {
  val root: Node = new Node(null, 7)
  root.next = root
}

object Collector {
  def keep(node: Node): Int = {
    gcCollect()
    node.value
  }
}

object MemoryRuntime {
  def main = {
    val temporary = new Node(null, 9)
    gcCollect()
    println(Roots.root.value)
    println(Roots.root.next.value)
    println(temporary.value)
    println(Collector.keep(new Node(null, 11)))
  }
}
