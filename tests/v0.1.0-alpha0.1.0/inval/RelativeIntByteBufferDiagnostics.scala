// expected-error: putInt requires one Int value or an Int index and Int value
// expected-error: putInt value must have type Int

package tests.v010alpha010.invalidrelativeintbytebuffer

object Invalid {
  def putWithoutValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putInt()

  def putShortValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putInt(1.toShort)
}
