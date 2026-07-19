package demo.examples

object ByteShort {
  def arithmetic(byte: Byte, short: Short): String =
    (byte + short) + "|" + (-byte) + "|" + (short * 2)

  def arrayBehavior(): String = {
    val bytes = Array[Byte](1.toByte, 255.toByte, 3.toByte)
    val shorts = Array.fill[Short](3)(300.toShort)
    bytes(0) = 127.toByte
    shorts(1) = 40000.toShort

    val cloned = bytes.clone()
    cloned(2) = 9.toByte
    val joined = Array.concat[Byte](bytes, Array[Byte](4.toByte))
    val copied = Array.fill[Short](3)(0.toShort)
    Array.copy(shorts, 0, copied, 0, shorts.length)

    bytes(0) + "|" + bytes(1) + "|" + bytes(2) + "|" +
      cloned(2) + "|" + joined.length + "|" + joined(3) + "|" +
      copied(0) + "|" + copied(1)
  }

  def boxedBehavior(byte: Byte, short: Short): String = {
    val values = Array[Any](byte, short)
    val restoredByte = values(0).asInstanceOf[Byte]
    val restoredShort = values(1).asInstanceOf[Short]
    values(0).isInstanceOf[Byte] + "|" + values(1).isInstanceOf[Short] +
      "|" + restoredByte + "|" + restoredShort + "|" +
      restoredByte.equals(byte) + "|" + restoredShort.equals(short)
  }

  def main = {
    val byte: Byte = 130.toByte
    val short: Short = 40000.toShort
    val wideByte: Int = byte
    val wideShort: Int = short

    println(byte)
    println(short)
    println(wideByte)
    println(wideShort)
    println(byte.toInt)
    println(short.toInt)
    println(byte.toString)
    println(short.toString())
    println(byte.hashCode)
    println(short.hashCode())
    println(arithmetic(byte, short))
    println(arrayBehavior())
    println(boxedBehavior(byte, short))
    println(sizeof[Byte])
    println(sizeof[Short])
  }
}
