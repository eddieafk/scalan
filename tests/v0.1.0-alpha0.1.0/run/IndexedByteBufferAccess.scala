// expected-output: 30
// expected-output: 1
// expected-output: true
// expected-output: 99
// expected-output: 1
// expected-output: get: ByteBuffer index is out of bounds @1
// expected-output: put: ByteBuffer index is out of bounds @1/20

package tests.v010alpha010.indexedbytebuffer

object Main {
  def rejectedGet(buffer: ByteBuffer): String = {
    try {
      buffer.get(2)
      "get was accepted"
    } catch {
      case failure: IndexOutOfBoundsException =>
        "get: " + failure.getMessage + " @" + buffer.position()
    }
  }

  def rejectedPut(buffer: ByteBuffer, bytes: Array[Byte]): String = {
    try {
      buffer.put(-1, 77.toByte)
      "put was accepted"
    } catch {
      case failure: IndexOutOfBoundsException =>
        "put: " + failure.getMessage + " @" +
          buffer.position() + "/" + bytes(1).toInt
    }
  }

  def main(args: Array[String]): Unit = {
    val bytes = Array[Byte](10.toByte, 20.toByte, 30.toByte)
    val buffer = ByteBuffer.wrap(bytes)
    buffer.position(1)

    println(buffer.get(2).toInt)
    println(buffer.position())

    val same = buffer.put(0, 99.toByte)
    println(same == buffer)
    println(bytes(0).toInt)
    println(buffer.position())

    buffer.limit(2)
    println(rejectedGet(buffer))
    println(rejectedPut(buffer, bytes))
  }
}
