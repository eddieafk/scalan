// expected-output: 81985529216486895
// expected-output: 1
// expected-output: -81985529216486896
// expected-output: 1
// expected-output: true
// expected-output: 69/103/-119/-85/-51/-17/1/35
// expected-output: 1
// expected-output: true
// expected-output: 1
// expected-output: getLong: ByteBuffer index is out of bounds @1
// expected-output: putLong: ByteBuffer index is out of bounds @1/35/-36/-70/-104/118/84/50/16

package tests.v010alpha010.indexedlongbytebuffer

object Main {
  def bytesText(bytes: Array[Byte], start: Int): String =
    bytes(start).toInt + "/" + bytes(start + 1).toInt + "/" +
      bytes(start + 2).toInt + "/" + bytes(start + 3).toInt + "/" +
      bytes(start + 4).toInt + "/" + bytes(start + 5).toInt + "/" +
      bytes(start + 6).toInt + "/" + bytes(start + 7).toInt

  def rejectedGet(buffer: ByteBuffer): String = {
    try {
      buffer.getLong(-1)
      "getLong was accepted"
    } catch {
      case failure: IndexOutOfBoundsException =>
        "getLong: " + failure.getMessage + " @" + buffer.position()
    }
  }

  def rejectedPut(buffer: ByteBuffer, bytes: Array[Byte]): String = {
    try {
      buffer.putLong(9, 1234605616436508552L)
      "putLong was accepted"
    } catch {
      case failure: IndexOutOfBoundsException =>
        "putLong: " + failure.getMessage + " @" + buffer.position() + "/" +
          bytesText(bytes, 8)
    }
  }

  def main(args: Array[String]): Unit = {
    val bytes = Array[Byte](1.toByte, 35.toByte, 69.toByte, 103.toByte,
      137.toByte, 171.toByte, 205.toByte, 239.toByte, 254.toByte, 220.toByte,
      186.toByte, 152.toByte, 118.toByte, 84.toByte, 50.toByte, 16.toByte)
    val buffer = ByteBuffer.wrap(bytes)
    buffer.position(1).mark()

    println(buffer.getLong(0))
    println(buffer.position())
    println(buffer.getLong(8))
    println(buffer.position())

    println(buffer.putLong(1, 5001117282205630755L) == buffer)
    println(bytesText(bytes, 1))
    println(buffer.position())
    println(buffer.reset() == buffer)
    println(buffer.position())

    println(rejectedGet(buffer))
    println(rejectedPut(buffer, bytes))
  }
}
