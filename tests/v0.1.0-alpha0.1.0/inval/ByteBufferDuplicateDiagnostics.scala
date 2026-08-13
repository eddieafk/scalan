// expected-error: duplicate does not accept arguments

package tests.v010alpha010.invalidbytebufferduplicate

object Invalid {
  def duplicateWithArgument(buffer: ByteBuffer): ByteBuffer =
    buffer.duplicate(1)
}
