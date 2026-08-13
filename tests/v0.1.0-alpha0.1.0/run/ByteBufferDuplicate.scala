// expected-output: 6
// expected-output: 3
// expected-output: 5
// expected-output: 2
// expected-output: true
// expected-output: 6
// expected-output: 3
// expected-output: 5
// expected-output: 2
// expected-output: true
// expected-output: true
// expected-output: 1
// expected-output: 3
// expected-output: true
// expected-output: 1
// expected-output: 1
// expected-output: 4
// expected-output: 4
// expected-output: 1
// expected-output: 5
// expected-output: true
// expected-output: 0
// expected-output: 6
// expected-output: duplicate: ByteBuffer mark is not set @0
// expected-output: true
// expected-output: 1
// expected-output: true
// expected-output: 99
// expected-output: 99
// expected-output: true
// expected-output: 88
// expected-output: 88
// expected-output: 0
// expected-output: 1
// expected-output: true
// expected-output: 18
// expected-output: 52
// expected-output: 86
// expected-output: 120
// expected-output: 305419896
// expected-output: 2018915346
// expected-output: 0
// expected-output: 1
// expected-output: 6
// expected-output: 4
// expected-output: 5
// expected-output: 1
// expected-output: true
// expected-output: true
// expected-output: 2
// expected-output: 4
// expected-output: true
// expected-output: 2
// expected-output: 2
// expected-output: true
// expected-output: true
// expected-output: 0
// expected-output: 10
// expected-output: true

package tests.v010alpha010.bytebufferduplicate

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
    val root = ByteBuffer.wrap(bytes)
    val view = root.slice(2, 6)
    view.position(1).mark().position(3).limit(5).order(ByteOrder.LITTLE_ENDIAN)

    val duplicate = view.duplicate()
    println(view.capacity())
    println(view.position())
    println(view.limit())
    println(view.remaining())
    println(view.order() == ByteOrder.LITTLE_ENDIAN)
    println(duplicate.capacity())
    println(duplicate.position())
    println(duplicate.limit())
    println(duplicate.remaining())
    println(duplicate.order() == ByteOrder.BIG_ENDIAN)

    println(duplicate.reset() == duplicate)
    println(duplicate.position())
    println(view.position())
    println(view.reset() == view)
    println(view.position())
    println(duplicate.position())

    duplicate.position(4).limit(4)
    println(duplicate.position())
    println(duplicate.limit())
    println(view.position())
    println(view.limit())
    println(duplicate.clear() == duplicate)
    println(duplicate.position())
    println(duplicate.limit())
    rejectedReset("duplicate", duplicate)
    view.position(3)
    println(view.reset() == view)
    println(view.position())

    println(duplicate.put(0, 99.toByte) == duplicate)
    println(bytes(2).toInt)
    println(view.get(0).toInt)
    println(view.put(1, 88.toByte) == view)
    println(bytes(3).toInt)
    println(duplicate.get(1).toInt)
    println(duplicate.position())
    println(view.position())

    println(duplicate.putInt(0, 305419896) == duplicate)
    println(bytes(2).toInt)
    println(bytes(3).toInt)
    println(bytes(4).toInt)
    println(bytes(5).toInt)
    println(duplicate.getInt(0))
    println(view.getInt(0))
    println(duplicate.position())
    println(view.position())

    duplicate.position(2).mark().position(4).limit(5).order(ByteOrder.LITTLE_ENDIAN)
    val nested = duplicate.duplicate()
    println(nested.capacity())
    println(nested.position())
    println(nested.limit())
    println(nested.remaining())
    println(nested.order() == ByteOrder.BIG_ENDIAN)
    println(nested.reset() == nested)
    println(nested.position())
    println(duplicate.position())
    println(duplicate.reset() == duplicate)
    println(duplicate.position())
    println(nested.position())
    println(duplicate.order() == ByteOrder.LITTLE_ENDIAN)
    println(nested.order() == ByteOrder.BIG_ENDIAN)

    println(root.position())
    println(root.limit())
    println(root.order() == ByteOrder.BIG_ENDIAN)
  }
}
