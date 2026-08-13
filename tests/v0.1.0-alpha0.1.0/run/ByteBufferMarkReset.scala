// expected-output: initial: ByteBuffer mark is not set @0
// expected-output: true
// expected-output: 20
// expected-output: 2
// expected-output: true
// expected-output: 1
// expected-output: true
// expected-output: 1
// expected-output: true
// expected-output: 30
// expected-output: 1
// expected-output: position: ByteBuffer mark is not set @1
// expected-output: limit: ByteBuffer mark is not set @2
// expected-output: clear: ByteBuffer mark is not set @0
// expected-output: flip: ByteBuffer mark is not set @0
// expected-output: rewind: ByteBuffer mark is not set @0

package tests.v010alpha010.bytebuffermark

object Main {
  def rejectedReset(label: String, buffer: ByteBuffer): Unit = {
    try {
      buffer.reset()
      println(label + ": reset was accepted")
    } catch {
      case failure: InvalidMarkException =>
        println(label + ": " + failure.getMessage + " @" + buffer.position())
    }
  }

  def main(args: Array[String]): Unit = {
    val bytes = Array[Byte](10.toByte, 20.toByte, 30.toByte, 40.toByte)
    val buffer = ByteBuffer.wrap(bytes)

    rejectedReset("initial", buffer)

    buffer.position(1)
    println(buffer.mark() == buffer)
    println(buffer.get().toInt)
    println(buffer.position())
    println(buffer.reset() == buffer)
    println(buffer.position())
    println(buffer.reset() == buffer)
    println(buffer.position())

    println(buffer.put(0, 99.toByte) == buffer)
    println(buffer.get(2).toInt)
    println(buffer.position())

    buffer.position(3).mark().position(1)
    rejectedReset("position", buffer)

    buffer.clear().position(3).mark().limit(2)
    rejectedReset("limit", buffer)

    buffer.clear().position(2).mark().clear()
    rejectedReset("clear", buffer)

    buffer.position(2).mark().flip()
    rejectedReset("flip", buffer)

    buffer.clear().position(2).mark().rewind()
    rejectedReset("rewind", buffer)
  }
}
