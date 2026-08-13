// expected-error: getLong does not accept arguments
// expected-error: putLong requires one Long argument
// expected-error: putLong value must have type Long

package tests.v010alpha010.invalidrelativelongbytebuffer

object Invalid {
  def getWithIndex(buffer: ByteBuffer): Long =
    buffer.getLong(0)

  def putWithoutValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putLong()

  def putIntValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putLong(1)
}
