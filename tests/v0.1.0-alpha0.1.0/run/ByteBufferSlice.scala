// expected-output: 2
// expected-output: 8
// expected-output: true
// expected-output: true
// expected-output: 2
// expected-output: 6
// expected-output: 6
// expected-output: 0
// expected-output: 6
// expected-output: true
// expected-output: slice: ByteBuffer mark is not set @0
// expected-output: 2
// expected-output: true
// expected-output: 99
// expected-output: true
// expected-output: 88
// expected-output: 2
// expected-output: 4
// expected-output: 4
// expected-output: 0
// expected-output: true
// expected-output: 88
// expected-output: true
// expected-output: 77
// expected-output: 77
// expected-output: 0
// expected-output: 2
// expected-output: 88
// expected-output: 1
// expected-output: 2
// expected-output: true
// expected-output: 18
// expected-output: 52
// expected-output: 4660
// expected-output: 13330
// expected-output: 2
// expected-output: 0
// expected-output: 0
// expected-output: 0
// expected-output: true
// expected-output: true
// expected-output: 26
// expected-output: 3
// expected-output: true
// expected-output: 4660
// expected-output: 305419896
// expected-output: 72623859790382856
// expected-output: 1.500000
// expected-output: 1.500000
// expected-output: 4660
// expected-output: 305419896
// expected-output: 72623859790382856
// expected-output: 1.500000
// expected-output: 1.500000
// expected-output: true
// expected-output: true
// expected-output: 4386
// expected-output: 270544960
// expected-output: 1230066625199609624
// expected-output: -0.500000
// expected-output: -2.250000
// expected-output: 0
// expected-output: 4386

package tests.v010alpha010.bytebufferslice

object Main {
  def rejectedReset(label: String, buffer: ByteBuffer): Unit = {
    try {
      buffer.reset()
      println(label + ": reset was accepted")
    } catch {
      case failure: InvalidMarkException =>
        println(label + ": " + failure.getMessage + " @" + buffer.position())
    }
  }

  def main(args: Array[String]): Unit = {
    val bytes = Array[Byte](0.toByte, 1.toByte, 2.toByte, 3.toByte, 4.toByte,
      5.toByte, 6.toByte, 7.toByte, 8.toByte, 9.toByte)
    val parent = ByteBuffer.wrap(bytes)
    parent.position(2).mark().limit(8).order(ByteOrder.LITTLE_ENDIAN)

    val slice = parent.slice()
    println(parent.position())
    println(parent.limit())
    println(parent.order() == ByteOrder.LITTLE_ENDIAN)
    parent.position(3)
    println(parent.reset() == parent)
    println(parent.position())

    println(slice.capacity())
    println(slice.limit())
    println(slice.position())
    println(slice.remaining())
    println(slice.order() == ByteOrder.BIG_ENDIAN)
    rejectedReset("slice", slice)

    println(slice.get(0).toInt)
    println(slice.put(1, 99.toByte) == slice)
    println(parent.get(3).toInt)
    println(parent.put(4, 88.toByte) == parent)
    println(slice.get(2).toInt)
    println(parent.position())

    slice.position(2)
    val nested = slice.slice()
    println(nested.capacity())
    println(nested.limit())
    println(nested.position())
    println(nested.order() == ByteOrder.BIG_ENDIAN)
    println(nested.get(0).toInt)
    println(nested.put(1, 77.toByte) == nested)
    println(bytes(5).toInt)
    println(slice.get(3).toInt)
    println(nested.position())
    println(slice.position())
    println(nested.get().toInt)
    println(nested.position())
    println(parent.position())

    println(slice.putShort(0, 4660.toShort) == slice)
    println(bytes(2).toInt)
    println(bytes(3).toInt)
    println(slice.getShort(0))
    println(parent.getShort(2))
    println(parent.position())

    slice.position(slice.limit())
    val empty = slice.slice()
    println(empty.capacity())
    println(empty.limit())
    println(empty.remaining())

    val primitiveBytes = Array.fill[Byte](32)(0.toByte)
    val primitiveParent = ByteBuffer.wrap(primitiveBytes)
    primitiveParent.position(3).limit(29).order(ByteOrder.LITTLE_ENDIAN)
    val primitives = primitiveParent.slice()
    println(primitives.order(ByteOrder.LITTLE_ENDIAN) == primitives)

    val relativeInts = primitives.putShort(4660.toShort).putInt(305419896).putLong(72623859790382856L)
    println(relativeInts.putFloat(1.5F).putDouble(1.5) == primitives)
    println(primitives.position())
    println(primitiveParent.position())
    println(primitives.rewind() == primitives)
    println(primitives.getShort())
    println(primitives.getInt())
    println(primitives.getLong())
    println(primitives.getFloat())
    println(primitives.getDouble())
    println(primitiveParent.getShort(3))
    println(primitiveParent.getInt(5))
    println(primitiveParent.getLong(9))
    println(primitiveParent.getFloat(17))
    println(primitiveParent.getDouble(21))

    println(primitives.rewind() == primitives)
    val indexedInts = primitives.putShort(0, 4386.toShort).putInt(2, 270544960).putLong(6, 1230066625199609624L)
    println(indexedInts.putFloat(14, -0.5F).putDouble(18, -2.25) == primitives)
    println(primitives.getShort(0))
    println(primitives.getInt(2))
    println(primitives.getLong(6))
    println(primitives.getFloat(14))
    println(primitives.getDouble(18))
    println(primitives.position())
    println(primitiveParent.getShort(3))
  }
}
