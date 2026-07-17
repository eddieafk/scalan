package demo.zonebench

class ZonePair(val left: Int, val right: Int) {
  def total: Int = left + right
}

class ZoneNode(var next: ZoneNode, val value: Int)

object ZoneBench {
  def main = {
    var sum = 0

    var i = 0
    while (i < 10000000) {
      sum += Zone.scoped({
        val p = new ZonePair(i, i + 1)
        p.total
      })
      i += 1
    }

    println(sum)
  }
}