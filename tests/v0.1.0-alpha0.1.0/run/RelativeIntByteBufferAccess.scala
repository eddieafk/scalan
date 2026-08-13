// expected-output: true
// expected-output: 305419896
// expected-output: 4
// expected-output: -19088744
// expected-output: 8
// expected-output: true
// expected-output: 0
// expected-output: true
// expected-output: 69
// expected-output: 103
// expected-output: -119
// expected-output: -85
// expected-output: 4
// expected-output: true
// expected-output: 8
// expected-output: 1
// expected-output: 2
// expected-output: 3
// expected-output: 4
// expected-output: getInt: ByteBuffer underflow @5
// expected-output: putInt: ByteBuffer overflow @5/2/3/4

package tests.v010alpha010.relativeintbytebuffer

object Main {
  def rejectedGet(buffer: ByteBuffer): String = {
    try {
      buffer.getInt()
      "getInt was accepted"
    } catch {
      case failure: BufferUnderflowException =>
        "getInt: " + failure.getMessage + " @" + buffer.position()
    }
  }

  def rejectedPut(buffer: ByteBuffer, bytes: Array[Byte]): String = {
    try {
      buffer.putInt(16909060)
      "putInt was accepted"
    } catch {
      case failure: BufferOverflowException =>
        "putInt: " + failure.getMessage + " @" + buffer.position() + "/" +
          bytes(5).toInt + "/" + bytes(6).toInt + "/" + bytes(7).toInt
    }
  }

  def main(args: Array[String]): Unit = {
    val bytes = Array[Byte](18.toByte, 52.toByte, 86.toByte, 120.toByte,
      254.toByte, 220.toByte, 186.toByte, 152.toByte)
    val buffer = ByteBuffer.wrap(bytes)

    println(buffer.mark() == buffer)
    println(buffer.getInt())
    println(buffer.position())
    println(buffer.getInt())
    println(buffer.position())
    println(buffer.reset() == buffer)
    println(buffer.position())

    println(buffer.putInt(1164413355) == buffer)
    println(bytes(0).toInt)
    println(bytes(1).toInt)
    println(bytes(2).toInt)
    println(bytes(3).toInt)
    println(buffer.position())

    buffer.position(4)
    println(buffer.putInt(16909060) == buffer)
    println(buffer.position())
    println(bytes(4).toInt)
    println(bytes(5).toInt)
    println(bytes(6).toInt)
    println(bytes(7).toInt)

    buffer.position(5)
    println(rejectedGet(buffer))
    println(rejectedPut(buffer, bytes))
  }
}
