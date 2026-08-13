// expected-error: get index must have type Int
// expected-error: get accepts zero or one Int argument
// expected-error: put index must have type Int
// expected-error: put value must have type Byte

package tests.v010alpha010.invalidindexedbytebuffer

object Invalid {
  def wrongGetIndex(buffer: ByteBuffer): Byte =
    buffer.get("zero")

  def tooManyGetArguments(buffer: ByteBuffer): Byte =
    buffer.get(0, 1)

  def wrongPutIndex(buffer: ByteBuffer): ByteBuffer =
    buffer.put("zero", 1.toByte)

  def wrongPutValue(buffer: ByteBuffer): ByteBuffer =
    buffer.put(0, 1)
}
