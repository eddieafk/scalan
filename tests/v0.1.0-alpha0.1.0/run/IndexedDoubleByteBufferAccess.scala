// expected-output: 1.500000
// expected-output: 1
// expected-output: -2.250000
// expected-output: 1
// expected-output: true
// expected-output: 64/10/0/0/0/0/0/0
// expected-output: 1
// expected-output: true
// expected-output: 1
// expected-output: getDouble: ByteBuffer index is out of bounds @1
// expected-output: putDouble: ByteBuffer index is out of bounds @1/0/2/0/0/0/0/0/0

package tests.v010alpha010.indexeddoublebytebuffer

object Main {
  def bytesText(bytes: Array[Byte], start: Int): String =
    bytes(start).toInt + "/" + bytes(start + 1).toInt + "/" +
      bytes(start + 2).toInt + "/" + bytes(start + 3).toInt + "/" +
      bytes(start + 4).toInt + "/" + bytes(start + 5).toInt + "/" +
      bytes(start + 6).toInt + "/" + bytes(start + 7).toInt

  def rejectedGet(buffer: ByteBuffer): String = {
    try {
      buffer.getDouble(-1)
      "getDouble was accepted"
    } catch {
      case failure: IndexOutOfBoundsException =>
        "getDouble: " + failure.getMessage + " @" + buffer.position()
    }
  }

  def rejectedPut(buffer: ByteBuffer, bytes: Array[Byte]): String = {
    try {
      buffer.putDouble(9, 7.75)
      "putDouble was accepted"
    } catch {
      case failure: IndexOutOfBoundsException =>
        "putDouble: " + failure.getMessage + " @" + buffer.position() + "/" +
          bytesText(bytes, 8)
    }
  }

  def main(args: Array[String]): Unit = {
    val bytes = Array[Byte](63.toByte, 248.toByte, 0.toByte, 0.toByte,
      0.toByte, 0.toByte, 0.toByte, 0.toByte, 192.toByte, 2.toByte,
      0.toByte, 0.toByte, 0.toByte, 0.toByte, 0.toByte, 0.toByte)
    val buffer = ByteBuffer.wrap(bytes)
    buffer.position(1).mark()

    println(buffer.getDouble(0))
    println(buffer.position())
    println(buffer.getDouble(8))
    println(buffer.position())

    println(buffer.putDouble(1, 3.25) == buffer)
    println(bytesText(bytes, 1))
    println(buffer.position())
    println(buffer.reset() == buffer)
    println(buffer.position())

    println(rejectedGet(buffer))
    println(rejectedPut(buffer, bytes))
  }
}
