// expected-error: getFloat does not accept arguments
// expected-error: putFloat requires one Float argument
// expected-error: putFloat value must have type Float

package tests.v010alpha010.invalidrelativefloatbytebuffer

object Invalid {
  def getWithIndex(buffer: ByteBuffer): Float =
    buffer.getFloat(0)

  def putWithoutValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putFloat()

  def putDoubleValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putFloat(1.0)
}
