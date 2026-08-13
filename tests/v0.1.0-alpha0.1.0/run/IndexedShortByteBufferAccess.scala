// expected-output: 4660
// expected-output: 1
// expected-output: -292
// expected-output: 1
// expected-output: true
// expected-output: 69
// expected-output: 103
// expected-output: 1
// expected-output: true
// expected-output: 1
// expected-output: 26588
// expected-output: 1
// expected-output: getShort: ByteBuffer index is out of bounds @1
// expected-output: putShort: ByteBuffer index is out of bounds @1/-36/85

package tests.v010alpha010.indexedshortbytebuffer

object Main {
  def rejectedGet(buffer: ByteBuffer): String = {
    try {
      buffer.getShort(-1)
      "getShort was accepted"
    } catch {
      case failure: IndexOutOfBoundsException =>
        "getShort: " + failure.getMessage + " @" + buffer.position()
    }
  }

  def rejectedPut(buffer: ByteBuffer, bytes: Array[Byte]): String = {
    try {
      buffer.putShort(3, 258.toShort)
      "putShort was accepted"
    } catch {
      case failure: IndexOutOfBoundsException =>
        "putShort: " + failure.getMessage + " @" + buffer.position() + "/" +
          bytes(3).toInt + "/" + bytes(4).toInt
    }
  }

  def main(args: Array[String]): Unit = {
    val bytes = Array[Byte](18.toByte, 52.toByte, 254.toByte, 220.toByte, 85.toByte)
    val buffer = ByteBuffer.wrap(bytes)
    buffer.position(1).mark()

    println(buffer.getShort(0).toInt)
    println(buffer.position())
    println(buffer.getShort(2).toInt)
    println(buffer.position())

    println(buffer.putShort(1, 17767.toShort) == buffer)
    println(bytes(1).toInt)
    println(bytes(2).toInt)
    println(buffer.position())
    println(buffer.reset() == buffer)
    println(buffer.position())

    buffer.limit(4)
    println(buffer.getShort(2).toInt)
    println(buffer.position())
    println(rejectedGet(buffer))
    println(rejectedPut(buffer, bytes))
  }
}
