// expected-error: getFloat index must have type Int
// expected-error: getFloat accepts zero or one Int argument
// expected-error: putFloat index must have type Int
// expected-error: putFloat value must have type Float

package tests.v010alpha010.invalidindexedfloatbytebuffer

object Invalid {
  def wrongGetIndex(buffer: ByteBuffer): Float =
    buffer.getFloat("zero")

  def tooManyGetArguments(buffer: ByteBuffer): Float =
    buffer.getFloat(0, 1)

  def wrongPutIndex(buffer: ByteBuffer): ByteBuffer =
    buffer.putFloat("zero", 1.0F)

  def wrongPutValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putFloat(0, 1.0)
}
