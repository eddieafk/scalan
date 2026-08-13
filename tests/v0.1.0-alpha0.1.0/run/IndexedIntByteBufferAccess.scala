// expected-output: 305419896
// expected-output: 1
// expected-output: -19088744
// expected-output: 1
// expected-output: true
// expected-output: 69
// expected-output: 103
// expected-output: -119
// expected-output: -85
// expected-output: 1
// expected-output: true
// expected-output: 1
// expected-output: getInt: ByteBuffer index is out of bounds @1
// expected-output: putInt: ByteBuffer index is out of bounds @1/-36/-70/-104

package tests.v010alpha010.indexedintbytebuffer

object Main {
  def rejectedGet(buffer: ByteBuffer): String = {
    try {
      buffer.getInt(-1)
      "getInt was accepted"
    } catch {
      case failure: IndexOutOfBoundsException =>
        "getInt: " + failure.getMessage + " @" + buffer.position()
    }
  }

  def rejectedPut(buffer: ByteBuffer, bytes: Array[Byte]): String = {
    try {
      buffer.putInt(5, 16909060)
      "putInt was accepted"
    } catch {
      case failure: IndexOutOfBoundsException =>
        "putInt: " + failure.getMessage + " @" + buffer.position() + "/" +
          bytes(5).toInt + "/" + bytes(6).toInt + "/" + bytes(7).toInt
    }
  }

  def main(args: Array[String]): Unit = {
    val bytes = Array[Byte](18.toByte, 52.toByte, 86.toByte, 120.toByte,
      254.toByte, 220.toByte, 186.toByte, 152.toByte)
    val buffer = ByteBuffer.wrap(bytes)
    buffer.position(1).mark()

    println(buffer.getInt(0))
    println(buffer.position())
    println(buffer.getInt(4))
    println(buffer.position())

    println(buffer.putInt(1, 1164413355) == buffer)
    println(bytes(1).toInt)
    println(bytes(2).toInt)
    println(bytes(3).toInt)
    println(bytes(4).toInt)
    println(buffer.position())
    println(buffer.reset() == buffer)
    println(buffer.position())

    println(rejectedGet(buffer))
    println(rejectedPut(buffer, bytes))
  }
}
