// expected-output: 6
// expected-output: 9
// expected-output: true
// expected-output: true
// expected-output: 6
// expected-output: 4
// expected-output: 4
// expected-output: 0
// expected-output: 4
// expected-output: true
// expected-output: indexed: ByteBuffer mark is not set @0
// expected-output: 2
// expected-output: 5
// expected-output: 0
// expected-output: true
// expected-output: 99
// expected-output: 99
// expected-output: true
// expected-output: 88
// expected-output: 6
// expected-output: true
// expected-output: 18
// expected-output: 52
// expected-output: 86
// expected-output: 120
// expected-output: 305419896
// expected-output: 2018915346
// expected-output: 0
// expected-output: 2
// expected-output: 2
// expected-output: true
// expected-output: 52
// expected-output: true
// expected-output: 77
// expected-output: 77
// expected-output: 0
// expected-output: 0
// expected-output: 0
// expected-output: 0
// expected-output: 0
// expected-output: negative-index: ByteBuffer index is out of bounds @6/9
// expected-output: negative-length: ByteBuffer index is out of bounds @6/9
// expected-output: past-limit: ByteBuffer index is out of bounds @6/9
// expected-output: too-long: ByteBuffer index is out of bounds @6/9
// expected-output: overflow: ByteBuffer index is out of bounds @6/9
// expected-output: 6
// expected-output: 9
// expected-output: true

package tests.v010alpha010.indexedbytebufferslice

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

  def rejectedSlice(label: String, buffer: ByteBuffer, index: Int, length: Int): Unit = {
    try {
      buffer.slice(index, length)
      println(label + ": slice was accepted")
    } catch {
      case failure: IndexOutOfBoundsException =>
        println(label + ": " + failure.getMessage + " @" +
          buffer.position() + "/" + buffer.limit())
    }
  }

  def main(args: Array[String]): Unit = {
    val bytes = Array[Byte](0.toByte, 1.toByte, 2.toByte, 3.toByte, 4.toByte,
      5.toByte, 6.toByte, 7.toByte, 8.toByte, 9.toByte)
    val source = ByteBuffer.wrap(bytes)
    source.position(6).mark().limit(9).order(ByteOrder.LITTLE_ENDIAN)

    val indexed = source.slice(2, 4)
    println(source.position())
    println(source.limit())
    println(source.order() == ByteOrder.LITTLE_ENDIAN)
    source.position(7)
    println(source.reset() == source)
    println(source.position())

    println(indexed.capacity())
    println(indexed.limit())
    println(indexed.position())
    println(indexed.remaining())
    println(indexed.order() == ByteOrder.BIG_ENDIAN)
    rejectedReset("indexed", indexed)

    println(indexed.get(0).toInt)
    println(indexed.get(3).toInt)
    println(indexed.position())
    println(indexed.put(1, 99.toByte) == indexed)
    println(bytes(3).toInt)
    println(source.get(3).toInt)
    println(source.put(4, 88.toByte) == source)
    println(indexed.get(2).toInt)
    println(source.position())

    println(indexed.putInt(0, 305419896) == indexed)
    println(bytes(2).toInt)
    println(bytes(3).toInt)
    println(bytes(4).toInt)
    println(bytes(5).toInt)
    println(indexed.getInt(0))
    println(source.getInt(2))
    println(indexed.position())

    val nested = indexed.slice(1, 2)
    println(nested.capacity())
    println(nested.limit())
    println(nested.order() == ByteOrder.BIG_ENDIAN)
    println(nested.get(0).toInt)
    println(nested.put(1, 77.toByte) == nested)
    println(bytes(4).toInt)
    println(indexed.get(2).toInt)
    println(nested.position())

    val emptyStart = indexed.slice(0, 0)
    val emptyEnd = source.slice(source.limit(), 0)
    println(emptyStart.capacity())
    println(emptyStart.limit())
    println(emptyEnd.capacity())
    println(emptyEnd.limit())

    rejectedSlice("negative-index", source, -1, 1)
    rejectedSlice("negative-length", source, 0, -1)
    rejectedSlice("past-limit", source, 10, 0)
    rejectedSlice("too-long", source, 8, 2)
    rejectedSlice("overflow", source, 2147483647, 1)
    println(source.position())
    println(source.limit())
    println(source.order() == ByteOrder.LITTLE_ENDIAN)
  }
}
