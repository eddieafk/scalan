// expected-error: putShort requires one Short argument or an Int index and Short value
// expected-error: putShort value must have type Short

package tests.v010alpha010.invalidrelativeshortbytebuffer

object Invalid {
  def putWithoutValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putShort()

  def putIntValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putShort(1)
}
