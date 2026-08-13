object ByteBufferState {
  def initialState(): Int = {
    val buffer = ByteBuffer.wrap(Array[Byte](0.toByte, 0.toByte, 0.toByte, 0.toByte,
      0.toByte, 0.toByte, 0.toByte, 0.toByte))
    buffer.capacity() * 10000 +
      buffer.position() * 1000 +
      buffer.limit() * 100 +
      buffer.remaining() * 10 +
      (if (buffer.hasRemaining()) 1 else 0)
  }

  def zoneTransitions(): Int =
    Zone.scoped({
      val buffer = ByteBuffer.wrap(Zone.allocBytes(8))
      buffer.position(6)
      buffer.flip()
      buffer.position(2).limit(4)
      buffer.capacity() * 10000 +
        buffer.position() * 1000 +
        buffer.limit() * 100 +
        buffer.remaining() * 10 +
        (if (buffer.hasRemaining()) 1 else 0)
    })

  def clearAndRewind(): Int = {
    val buffer = ByteBuffer.wrap(Array[Byte](0.toByte, 0.toByte, 0.toByte, 0.toByte,
      0.toByte, 0.toByte, 0.toByte, 0.toByte))
    buffer.limit(6).position(5).rewind()
    val rewindState = buffer.position() * 10 + buffer.limit()
    buffer.clear()
    rewindState * 10000 + buffer.position() * 100 + buffer.limit()
  }

  def clampedPosition(): Int = {
    val buffer = ByteBuffer.wrap(Array[Byte](0.toByte, 0.toByte, 0.toByte, 0.toByte,
      0.toByte, 0.toByte, 0.toByte, 0.toByte))
    buffer.position(7)
    buffer.limit(3)
    buffer.position() * 100 +
      buffer.limit() * 10 +
      buffer.remaining() +
      (if (buffer.hasRemaining()) 1 else 0)
  }

  def emptyState(): Int = {
    val buffer = ByteBuffer.wrap(Array[Byte]())
    buffer.capacity() * 100 +
      buffer.remaining() * 10 +
      (if (buffer.hasRemaining()) 1 else 0)
  }

  def relativeRoundTrip(): Int = {
    val buffer = ByteBuffer.wrap(Array[Byte](0.toByte, 0.toByte, 0.toByte))
    buffer.put(7.toByte).put((0 - 2).toByte)
    val writePosition = buffer.position()
    buffer.flip()
    val first = buffer.get().toInt
    val second = buffer.get().toInt
    first * 10000 + (second + 128) * 100 +
      writePosition * 10 + buffer.position()
  }

  def zoneRelativeAccess(): Int =
    Zone.scoped({
      val buffer = ByteBuffer.wrap(Zone.allocBytes(2))
      buffer.put(12.toByte).put(34.toByte).flip()
      buffer.get().toInt * 1000 +
        buffer.get().toInt * 10 +
        buffer.position()
    })

  def rejectUnderflow(): String = {
    val buffer = ByteBuffer.wrap(Array[Byte](1.toByte))
    buffer.position(1)
    try {
      buffer.get()
      "underflow was accepted"
    } catch {
      case failure: BufferUnderflowException =>
        "underflow: " + failure.getMessage + " @" + buffer.position()
    }
  }

  def rejectOverflow(): String = {
    val bytes = Array[Byte](9.toByte)
    val buffer = ByteBuffer.wrap(bytes)
    buffer.limit(0)
    try {
      buffer.put(3.toByte)
      "overflow was accepted"
    } catch {
      case failure: BufferOverflowException =>
        "overflow: " + failure.getMessage + " @" +
          buffer.position() + "/" + bytes(0).toInt
    }
  }

  def rejectPosition(): String =
    try {
      ByteBuffer.wrap(Array[Byte](0.toByte, 0.toByte)).position(3)
      "position was accepted"
    } catch {
      case failure: IllegalArgumentException =>
        "position: " + failure.getMessage
    }

  def rejectLimit(): String =
    try {
      ByteBuffer.wrap(Array[Byte](0.toByte, 0.toByte)).limit(3)
      "limit was accepted"
    } catch {
      case failure: IllegalArgumentException =>
        "limit: " + failure.getMessage
    }

  def rejectNullStorage(): String =
    try {
      val bytes: Array[Byte] = null
      ByteBuffer.wrap(bytes)
      "null storage was accepted"
    } catch {
      case failure: NullPointerException =>
        "null storage: " + failure.getMessage
    }

  def rejectNullReceiver(): String =
    try {
      val buffer: ByteBuffer = null
      buffer.remaining()
      "null receiver was accepted"
    } catch {
      case failure: NullPointerException =>
        "null receiver: " + failure.getMessage
    }

  def main = {
    println(initialState())
    println(zoneTransitions())
    println(clearAndRewind())
    println(clampedPosition())
    println(emptyState())
    println(relativeRoundTrip())
    println(zoneRelativeAccess())
    println(rejectUnderflow())
    println(rejectOverflow())
    println(rejectPosition())
    println(rejectLimit())
    println(rejectNullStorage())
    println(rejectNullReceiver())
  }
}
