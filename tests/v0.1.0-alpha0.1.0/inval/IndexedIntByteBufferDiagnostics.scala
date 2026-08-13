// expected-error: getInt index must have type Int
// expected-error: getInt accepts zero or one Int argument
// expected-error: putInt index must have type Int
// expected-error: putInt value must have type Int

package tests.v010alpha010.invalidindexedintbytebuffer

object Invalid {
  def wrongGetIndex(buffer: ByteBuffer): Int =
    buffer.getInt("zero")

  def tooManyGetArguments(buffer: ByteBuffer): Int =
    buffer.getInt(0, 1)

  def wrongPutIndex(buffer: ByteBuffer): ByteBuffer =
    buffer.putInt("zero", 1)

  def wrongPutValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putInt(0, 1.toShort)
}
