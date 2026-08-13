// expected-output: true
// expected-output: true
// expected-output: true
// expected-output: true
// expected-output: true
// expected-output: 26
// expected-output: 52/18
// expected-output: 120/86/52/18
// expected-output: 8/7/6/5/4/3/2/1
// expected-output: 0/0/-64/63
// expected-output: 0/0/0/0/0/0/-8/63
// expected-output: true
// expected-output: 0
// expected-output: 4660
// expected-output: 305419896
// expected-output: 72623859790382856
// expected-output: 1.500000
// expected-output: 1.500000
// expected-output: 26
// expected-output: true
// expected-output: 26
// expected-output: 34/17
// expected-output: 64/48/32/16
// expected-output: 24/23/22/21/20/19/18/17
// expected-output: 0/0/0/-65
// expected-output: 0/0/0/0/0/0/2/-64
// expected-output: 4386
// expected-output: 270544960
// expected-output: 1230066625199609624
// expected-output: -0.500000
// expected-output: -2.250000
// expected-output: 26
// expected-output: true
// expected-output: 0
// expected-output: true
// expected-output: true
// expected-output: 8721
// expected-output: 0

package tests.v010alpha010.bytebufferorder

object Main {
  def pair(bytes: Array[Byte], start: Int): String =
    bytes(start).toInt + "/" + bytes(start + 1).toInt

  def quad(bytes: Array[Byte], start: Int): String =
    pair(bytes, start) + "/" + pair(bytes, start + 2)

  def octet(bytes: Array[Byte], start: Int): String =
    quad(bytes, start) + "/" + quad(bytes, start + 4)

  def main(args: Array[String]): Unit = {
    val bytes = Array.fill[Byte](26)(0.toByte)
    val buffer = ByteBuffer.wrap(bytes)

    println(buffer.order() == ByteOrder.BIG_ENDIAN)
    println(buffer.order(ByteOrder.LITTLE_ENDIAN) == buffer)
    println(buffer.order() == ByteOrder.LITTLE_ENDIAN)
    println(buffer.mark() == buffer)
    val relativeInts = buffer.putShort(4660.toShort).putInt(305419896).putLong(72623859790382856L)
    println(relativeInts.putFloat(1.5F).putDouble(1.5) == buffer)
    println(buffer.position())
    println(pair(bytes, 0))
    println(quad(bytes, 2))
    println(octet(bytes, 6))
    println(quad(bytes, 14))
    println(octet(bytes, 18))

    println(buffer.reset() == buffer)
    println(buffer.position())
    println(buffer.getShort())
    println(buffer.getInt())
    println(buffer.getLong())
    println(buffer.getFloat())
    println(buffer.getDouble())
    println(buffer.position())

    val indexedInts = buffer.putShort(0, 4386.toShort).putInt(2, 270544960).putLong(6, 1230066625199609624L)
    println(indexedInts.putFloat(14, -0.5F).putDouble(18, -2.25) == buffer)
    println(buffer.position())
    println(pair(bytes, 0))
    println(quad(bytes, 2))
    println(octet(bytes, 6))
    println(quad(bytes, 14))
    println(octet(bytes, 18))
    println(buffer.getShort(0))
    println(buffer.getInt(2))
    println(buffer.getLong(6))
    println(buffer.getFloat(14))
    println(buffer.getDouble(18))
    println(buffer.position())
    println(buffer.reset() == buffer)
    println(buffer.position())

    println(buffer.order(ByteOrder.BIG_ENDIAN) == buffer)
    println(buffer.order() == ByteOrder.BIG_ENDIAN)
    println(buffer.getShort(0))
    println(buffer.position())
  }
}
