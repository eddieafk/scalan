// expected-error: getShort index must have type Int
// expected-error: getShort accepts zero or one Int argument
// expected-error: putShort index must have type Int
// expected-error: putShort value must have type Short

package tests.v010alpha010.invalidindexedshortbytebuffer

object Invalid {
  def wrongGetIndex(buffer: ByteBuffer): Short =
    buffer.getShort("zero")

  def tooManyGetArguments(buffer: ByteBuffer): Short =
    buffer.getShort(0, 1)

  def wrongPutIndex(buffer: ByteBuffer): ByteBuffer =
    buffer.putShort("zero", 1.toShort)

  def wrongPutValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putShort(0, 1)
}
