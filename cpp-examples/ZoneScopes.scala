package demo.examples

class ZonePair(val left: Int, val right: Int) {
  def total: Int = left + right
}

class ZoneNode(var next: ZoneNode, val value: Int)

object ZoneScopes {
  def main = {
    val answer: Int = Zone.scoped({
      val pair = new ZonePair(20, 22)
      var bonus = 0
      bonus = bonus + 1
      pair.total + bonus
    })
    println(answer)

    Zone.scoped({
      val pair = new ZonePair(7, 8)
      println(pair.total)
    })

    val nested: Int = Zone.scoped({
      val pair = new ZonePair(1, 2)
      val inner = Zone.scoped({
        val pair = new ZonePair(4, 5)
        pair.total
      })
      pair.total + inner
    })
    println(nested)

    val localGraph: Int = Zone.scoped({
      val head = new ZoneNode(null, 7)
      val tail = new ZoneNode(null, 8)
      head.next = tail
      head.next.value
    })
    println(localGraph)
  }
}
