// expected-error: putLong requires one Long value or an Int index and Long value
// expected-error: putLong value must have type Long

package tests.v010alpha010.invalidrelativelongbytebuffer

object Invalid {
  def putWithoutValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putLong()

  def putIntValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putLong(1)
}
