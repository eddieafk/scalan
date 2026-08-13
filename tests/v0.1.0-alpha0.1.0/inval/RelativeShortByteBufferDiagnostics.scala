// expected-error: getShort does not accept arguments
// expected-error: putShort requires one Short argument
// expected-error: putShort value must have type Short

package tests.v010alpha010.invalidrelativeshortbytebuffer

object Invalid {
  def getWithIndex(buffer: ByteBuffer): Short =
    buffer.getShort(0)

  def putWithoutValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putShort()

  def putIntValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putShort(1)
}
