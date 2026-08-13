// expected-output: 1.500000
// expected-output: 1
// expected-output: -2.250000
// expected-output: 1
// expected-output: true
// expected-output: 64/80/0/0
// expected-output: 1
// expected-output: true
// expected-output: 1
// expected-output: getFloat: ByteBuffer index is out of bounds @1
// expected-output: putFloat: ByteBuffer index is out of bounds @1/0/16/0/0

package tests.v010alpha010.indexedfloatbytebuffer

object Main {
  def bytesText(bytes: Array[Byte], start: Int): String =
    bytes(start).toInt + "/" + bytes(start + 1).toInt + "/" +
      bytes(start + 2).toInt + "/" + bytes(start + 3).toInt

  def rejectedGet(buffer: ByteBuffer): String = {
    try {
      buffer.getFloat(-1)
      "getFloat was accepted"
    } catch {
      case failure: IndexOutOfBoundsException =>
        "getFloat: " + failure.getMessage + " @" + buffer.position()
    }
  }

  def rejectedPut(buffer: ByteBuffer, bytes: Array[Byte]): String = {
    try {
      buffer.putFloat(5, 7.75F)
      "putFloat was accepted"
    } catch {
      case failure: IndexOutOfBoundsException =>
        "putFloat: " + failure.getMessage + " @" + buffer.position() + "/" +
          bytesText(bytes, 4)
    }
  }

  def main(args: Array[String]): Unit = {
    val bytes = Array[Byte](63.toByte, 192.toByte, 0.toByte, 0.toByte,
      192.toByte, 16.toByte, 0.toByte, 0.toByte)
    val buffer = ByteBuffer.wrap(bytes)
    buffer.position(1).mark()

    println(buffer.getFloat(0))
    println(buffer.position())
    println(buffer.getFloat(4))
    println(buffer.position())

    println(buffer.putFloat(1, 3.25F) == buffer)
    println(bytesText(bytes, 1))
    println(buffer.position())
    println(buffer.reset() == buffer)
    println(buffer.position())

    println(rejectedGet(buffer))
    println(rejectedPut(buffer, bytes))
  }
}
