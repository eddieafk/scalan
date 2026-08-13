// expected-output: true
// expected-output: 4660
// expected-output: 2
// expected-output: -292
// expected-output: 4
// expected-output: true
// expected-output: 0
// expected-output: true
// expected-output: 69
// expected-output: 103
// expected-output: 2
// expected-output: true
// expected-output: 0
// expected-output: true
// expected-output: 5
// expected-output: 1
// expected-output: 2
// expected-output: getShort: ByteBuffer underflow @4
// expected-output: putShort: ByteBuffer overflow @4/2

package tests.v010alpha010.relativeshortbytebuffer

object Main {
  def rejectedGet(buffer: ByteBuffer): String = {
    try {
      buffer.getShort()
      "getShort was accepted"
    } catch {
      case failure: BufferUnderflowException =>
        "getShort: " + failure.getMessage + " @" + buffer.position()
    }
  }

  def rejectedPut(buffer: ByteBuffer, bytes: Array[Byte]): String = {
    try {
      buffer.putShort(772.toShort)
      "putShort was accepted"
    } catch {
      case failure: BufferOverflowException =>
        "putShort: " + failure.getMessage + " @" +
          buffer.position() + "/" + bytes(4).toInt
    }
  }

  def main(args: Array[String]): Unit = {
    val bytes = Array[Byte](18.toByte, 52.toByte, 254.toByte, 220.toByte, 85.toByte)
    val buffer = ByteBuffer.wrap(bytes)

    println(buffer.mark() == buffer)
    println(buffer.getShort().toInt)
    println(buffer.position())
    println(buffer.getShort().toInt)
    println(buffer.position())
    println(buffer.reset() == buffer)
    println(buffer.position())

    println(buffer.putShort(17767.toShort) == buffer)
    println(bytes(0).toInt)
    println(bytes(1).toInt)
    println(buffer.position())
    println(buffer.reset() == buffer)
    println(buffer.position())

    buffer.clear().position(3).limit(5)
    println(buffer.putShort(258.toShort) == buffer)
    println(buffer.position())
    println(bytes(3).toInt)
    println(bytes(4).toInt)

    buffer.position(4)
    println(rejectedGet(buffer))
    println(rejectedPut(buffer, bytes))
  }
}
