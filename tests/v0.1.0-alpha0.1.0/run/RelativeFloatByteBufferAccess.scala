// expected-output: true
// expected-output: 1.500000
// expected-output: 4
// expected-output: -2.250000
// expected-output: 8
// expected-output: true
// expected-output: 0
// expected-output: true
// expected-output: 64/80/0/0
// expected-output: 4
// expected-output: true
// expected-output: 8
// expected-output: -65/0/0/0
// expected-output: getFloat: ByteBuffer underflow @5
// expected-output: putFloat: ByteBuffer overflow @5/-65/0/0/0

package tests.v010alpha010.relativefloatbytebuffer

object Main {
  def bytesText(bytes: Array[Byte], start: Int): String =
    bytes(start).toInt + "/" + bytes(start + 1).toInt + "/" +
      bytes(start + 2).toInt + "/" + bytes(start + 3).toInt

  def rejectedGet(buffer: ByteBuffer): String = {
    try {
      buffer.getFloat()
      "getFloat was accepted"
    } catch {
      case failure: BufferUnderflowException =>
        "getFloat: " + failure.getMessage + " @" + buffer.position()
    }
  }

  def rejectedPut(buffer: ByteBuffer, bytes: Array[Byte]): String = {
    try {
      buffer.putFloat(7.75F)
      "putFloat was accepted"
    } catch {
      case failure: BufferOverflowException =>
        "putFloat: " + failure.getMessage + " @" + buffer.position() + "/" +
          bytesText(bytes, 4)
    }
  }

  def main(args: Array[String]): Unit = {
    val bytes = Array[Byte](63.toByte, 192.toByte, 0.toByte, 0.toByte,
      192.toByte, 16.toByte, 0.toByte, 0.toByte)
    val buffer = ByteBuffer.wrap(bytes)

    println(buffer.mark() == buffer)
    println(buffer.getFloat())
    println(buffer.position())
    println(buffer.getFloat())
    println(buffer.position())
    println(buffer.reset() == buffer)
    println(buffer.position())

    println(buffer.putFloat(3.25F) == buffer)
    println(bytesText(bytes, 0))
    println(buffer.position())

    println(buffer.putFloat(-0.5F) == buffer)
    println(buffer.position())
    println(bytesText(bytes, 4))

    buffer.position(5)
    println(rejectedGet(buffer))
    println(rejectedPut(buffer, bytes))
  }
}
