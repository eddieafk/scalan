// expected-error: getLong index must have type Int
// expected-error: getLong accepts zero or one Int argument
// expected-error: putLong index must have type Int
// expected-error: putLong value must have type Long

package tests.v010alpha010.invalidindexedlongbytebuffer

object Invalid {
  def wrongGetIndex(buffer: ByteBuffer): Long =
    buffer.getLong("zero")

  def tooManyGetArguments(buffer: ByteBuffer): Long =
    buffer.getLong(0, 1)

  def wrongPutIndex(buffer: ByteBuffer): ByteBuffer =
    buffer.putLong("zero", 1L)

  def wrongPutValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putLong(0, 1)
}
