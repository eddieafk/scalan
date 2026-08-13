// expected-error: putFloat requires one Float value or an Int index and Float value
// expected-error: putFloat value must have type Float

package tests.v010alpha010.invalidrelativefloatbytebuffer

object Invalid {
  def putWithoutValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putFloat()

  def putDoubleValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putFloat(1.0)
}
