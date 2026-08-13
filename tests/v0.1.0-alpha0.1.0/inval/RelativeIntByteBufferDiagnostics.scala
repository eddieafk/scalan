// expected-error: getInt does not accept arguments
// expected-error: putInt requires one Int argument
// expected-error: putInt value must have type Int

package tests.v010alpha010.invalidrelativeintbytebuffer

object Invalid {
  def getWithIndex(buffer: ByteBuffer): Int =
    buffer.getInt(0)

  def putWithoutValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putInt()

  def putShortValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putInt(1.toShort)
}
