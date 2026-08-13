package demo.examples

class AnyPayload(var value: Any) {
  def putUnit(): Int = {
    value = {}
    if (value.isInstanceOf[Unit]) 1 else 0
  }

  def putSymbol(): Int = {
    value = 'ready
    if (value.asInstanceOf[Symbol] == 'ready) 1 else 0
  }
}

object AnyUnitSymbol {
  def accept(value: Any): Int = {
    if (value.isInstanceOf[Symbol]) 1 else 0
  }

  def main = {
    val payload = new AnyPayload({})
    println(if (payload.value.isInstanceOf[Unit]) 1 else 0)
    println(payload.putSymbol())
    println(payload.putUnit())
    println(accept('ok))
  }
}
