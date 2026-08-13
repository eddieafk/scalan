// expected-output: false
// expected-output: true
// expected-output: 16
// expected-output: 4
// expected-output: 12
// expected-output: 8
// expected-output: true
// expected-output: true
// expected-output: 2
// expected-output: 4
// expected-output: true
// expected-output: 99
// expected-output: true
// expected-output: 5
// expected-output: 10
// expected-output: true
// expected-output: true
// expected-output: 3
// expected-output: 4
// expected-output: put-0: ByteBuffer is read-only @8/true
// expected-output: put-1: ByteBuffer is read-only @8/true
// expected-output: put-2: ByteBuffer is read-only @8/true
// expected-output: put-3: ByteBuffer is read-only @8/true
// expected-output: put-4: ByteBuffer is read-only @8/true
// expected-output: put-5: ByteBuffer is read-only @8/true
// expected-output: put-6: ByteBuffer is read-only @8/true
// expected-output: put-7: ByteBuffer is read-only @8/true
// expected-output: put-8: ByteBuffer is read-only @8/true
// expected-output: put-9: ByteBuffer is read-only @8/true
// expected-output: put-10: ByteBuffer is read-only @8/true
// expected-output: put-11: ByteBuffer is read-only @8/true
// expected-output: parent: ByteBuffer is read-only
// expected-output: true
// expected-output: 5
// expected-output: 0
// expected-output: 5
// expected-output: true
// expected-output: 4
// expected-output: true
// expected-output: 16
// expected-output: 4
// expected-output: 9
// expected-output: true
// expected-output: true
// expected-output: 3
// expected-output: 4
// expected-output: true
// expected-output: 4
// expected-output: 9
// expected-output: true
// expected-output: true
// expected-output: 77
// expected-output: 77
// expected-output: 77
// expected-output: true
// expected-output: 42
// expected-output: 42
// expected-output: 4

package tests.v010alpha010.readonlybytebuffer

object Main {
  def rejectedPut(buffer: ByteBuffer, operation: Int): String = {
    val position = buffer.position()
    try {
      if (operation == 0) buffer.put(1.toByte)
      else if (operation == 1) buffer.putShort(2.toShort)
      else if (operation == 2) buffer.putInt(3)
      else if (operation == 3) buffer.putLong(4L)
      else if (operation == 4) buffer.putFloat(5.0F)
      else if (operation == 5) buffer.putDouble(6.0)
      else if (operation == 6) buffer.put(-1, 7.toByte)
      else if (operation == 7) buffer.putShort(-1, 8.toShort)
      else if (operation == 8) buffer.putInt(-1, 9)
      else if (operation == 9) buffer.putLong(-1, 10L)
      else if (operation == 10) buffer.putFloat(-1, 11.0F)
      else buffer.putDouble(-1, 12.0)
      "put-" + operation + " was accepted"
    } catch {
      case failure: ReadOnlyBufferException =>
        "put-" + operation + ": " + failure.getMessage + " @" +
          buffer.position() + "/" + (buffer.position() == position)
    }
  }

  def rejectedAsParent(buffer: ByteBuffer): String = {
    try {
      buffer.put(1.toByte)
      "parent put was accepted"
    } catch {
      case failure: UnsupportedOperationException =>
        "parent: " + failure.getMessage
    }
  }

  def main(args: Array[String]): Unit = {
    val bytes = Array[Byte](0.toByte, 1.toByte, 2.toByte, 3.toByte,
      4.toByte, 5.toByte, 6.toByte, 7.toByte, 8.toByte, 9.toByte,
      10.toByte, 11.toByte, 12.toByte, 13.toByte, 14.toByte, 15.toByte)
    val root = ByteBuffer.wrap(bytes)
    root.position(2).mark().position(4).limit(12).order(ByteOrder.LITTLE_ENDIAN)

    val readOnly = root.asReadOnlyBuffer()
    println(root.isReadOnly())
    println(readOnly.isReadOnly())
    println(readOnly.capacity())
    println(readOnly.position())
    println(readOnly.limit())
    println(readOnly.remaining())
    println(readOnly.order() == ByteOrder.BIG_ENDIAN)

    println(readOnly.reset() == readOnly)
    println(readOnly.position())
    println(root.position())
    println(root.put(2, 99.toByte) == root)
    println(readOnly.get(2).toInt)

    println(readOnly.clear() == readOnly)
    readOnly.position(3).mark().position(5).limit(10)
      .order(ByteOrder.LITTLE_ENDIAN)
    println(readOnly.position())
    println(readOnly.limit())
    println(readOnly.order() == ByteOrder.LITTLE_ENDIAN)
    println(readOnly.reset() == readOnly)
    println(readOnly.position())
    println(root.position())

    readOnly.position(8).limit(8)
    var operation = 0
    while (operation < 12) {
      println(rejectedPut(readOnly, operation))
      operation = operation + 1
    }
    println(rejectedAsParent(readOnly))

    readOnly.limit(9).position(4)
    val slice = readOnly.slice()
    println(slice.isReadOnly())
    println(slice.capacity())
    println(slice.position())
    println(slice.limit())
    println(slice.order() == ByteOrder.BIG_ENDIAN)
    println(slice.get(0).toInt)

    val duplicate = readOnly.duplicate()
    println(duplicate.isReadOnly())
    println(duplicate.capacity())
    println(duplicate.position())
    println(duplicate.limit())
    println(duplicate.order() == ByteOrder.BIG_ENDIAN)
    println(duplicate.reset() == duplicate)
    println(duplicate.position())
    println(readOnly.position())

    val secondReadOnly = readOnly.asReadOnlyBuffer()
    println(secondReadOnly.isReadOnly())
    println(secondReadOnly.position())
    println(secondReadOnly.limit())
    println(secondReadOnly.order() == ByteOrder.BIG_ENDIAN)

    println(root.put(4, 77.toByte) == root)
    println(slice.get(0).toInt)
    println(duplicate.get(4).toInt)
    println(secondReadOnly.get(4).toInt)
    println(root.put(0, 42.toByte) == root)
    println(bytes(0).toInt)
    println(readOnly.get(0).toInt)
    println(root.position())
  }
}
