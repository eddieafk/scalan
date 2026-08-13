object ZoneBytes {
  def checksum(): Int =
    Zone.scoped({
      val bytes = Zone.allocBytes(6)
      var index = 0
      var total = 0
      while (index < bytes.length) {
        bytes(index) = (index * 17 - 20).toByte
        total = total + bytes(index).toInt
        index = index + 1
      }
      total
    })

  def zeroedFacts(): Int =
    Zone.scoped({
      val bytes = Zone.allocBytes(4)
      bytes.length * 100 + bytes(0).toInt + bytes(3).toInt
    })

  def rejectNegativeLength(): String =
    try {
      Zone.scoped({
        Zone.allocBytes(0 - 1).length
      })
      "negative length was accepted"
    } catch {
      case failure: NegativeArraySizeException =>
        "negative length: " + failure.getMessage
    }

  def bigEndianRoundTrip(): Int =
    Zone.scoped({
      val bytes = Zone.allocBytes(2)
      NativeBytes.putShortBE(bytes, 0, 4660.toShort)
      NativeBytes.getShortBE(bytes, 0).toInt
    })

  def littleEndianRoundTrip(): Int =
    Zone.scoped({
      val bytes = Zone.allocBytes(2)
      NativeBytes.putShortLE(bytes, 0, (0 - 292).toShort)
      NativeBytes.getShortLE(bytes, 0).toInt
    })

  def crossEndianRead(): Int =
    Zone.scoped({
      val bytes = Zone.allocBytes(2)
      NativeBytes.putShortBE(bytes, 0, 4660.toShort)
      NativeBytes.getShortLE(bytes, 0).toInt
    })

  def rejectPartialWrite(): Int =
    Zone.scoped({
      val bytes = Zone.allocBytes(2)
      bytes(0) = 7.toByte
      bytes(1) = 9.toByte
      try {
        NativeBytes.putShortBE(bytes, 1, 4660.toShort)
        0 - 1
      } catch {
        case failure: ArrayIndexOutOfBoundsException =>
          bytes(0).toInt * 100 + bytes(1).toInt
      }
    })

  def ordinaryArrayRoundTrip(): Int = {
    val bytes = Array[Byte](0.toByte, 0.toByte)
    NativeBytes.putShortLE(bytes, 0, 4660.toShort)
    NativeBytes.getShortLE(bytes, 0).toInt
  }

  def main = {
    println(checksum())
    println(zeroedFacts())
    println(rejectNegativeLength())
    println(bigEndianRoundTrip())
    println(littleEndianRoundTrip())
    println(crossEndianRead())
    println(rejectPartialWrite())
    println(ordinaryArrayRoundTrip())
  }
}
