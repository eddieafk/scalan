// expected-output: true
// expected-output: 1.500000
// expected-output: 8
// expected-output: -2.250000
// expected-output: 16
// expected-output: true
// expected-output: 0
// expected-output: true
// expected-output: 64/10/0/0/0/0/0/0
// expected-output: 8
// expected-output: true
// expected-output: 16
// expected-output: -65/-32/0/0/0/0/0/0
// expected-output: getDouble: ByteBuffer underflow @9
// expected-output: putDouble: ByteBuffer overflow @9/-65/-32/0/0/0/0/0/0

package tests.v010alpha010.relativedoublebytebuffer

object Main {
  def bytesText(bytes: Array[Byte], start: Int): String =
    bytes(start).toInt + "/" + bytes(start + 1).toInt + "/" +
      bytes(start + 2).toInt + "/" + bytes(start + 3).toInt + "/" +
      bytes(start + 4).toInt + "/" + bytes(start + 5).toInt + "/" +
      bytes(start + 6).toInt + "/" + bytes(start + 7).toInt

  def rejectedGet(buffer: ByteBuffer): String = {
    try {
      buffer.getDouble()
      "getDouble was accepted"
    } catch {
      case failure: BufferUnderflowException =>
        "getDouble: " + failure.getMessage + " @" + buffer.position()
    }
  }

  def rejectedPut(buffer: ByteBuffer, bytes: Array[Byte]): String = {
    try {
      buffer.putDouble(7.75)
      "putDouble was accepted"
    } catch {
      case failure: BufferOverflowException =>
        "putDouble: " + failure.getMessage + " @" + buffer.position() + "/" +
          bytesText(bytes, 8)
    }
  }

  def main(args: Array[String]): Unit = {
    val bytes = Array[Byte](63.toByte, 248.toByte, 0.toByte, 0.toByte,
      0.toByte, 0.toByte, 0.toByte, 0.toByte, 192.toByte, 2.toByte,
      0.toByte, 0.toByte, 0.toByte, 0.toByte, 0.toByte, 0.toByte)
    val buffer = ByteBuffer.wrap(bytes)

    println(buffer.mark() == buffer)
    println(buffer.getDouble())
    println(buffer.position())
    println(buffer.getDouble())
    println(buffer.position())
    println(buffer.reset() == buffer)
    println(buffer.position())

    println(buffer.putDouble(3.25) == buffer)
    println(bytesText(bytes, 0))
    println(buffer.position())

    println(buffer.putDouble(-0.5) == buffer)
    println(buffer.position())
    println(bytesText(bytes, 8))

    buffer.position(9)
    println(rejectedGet(buffer))
    println(rejectedPut(buffer, bytes))
  }
}
