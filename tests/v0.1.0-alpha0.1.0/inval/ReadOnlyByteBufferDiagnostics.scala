// expected-error: asReadOnlyBuffer does not accept arguments
// expected-error: isReadOnly does not accept arguments

package tests.v010alpha010.invalidreadonlybytebuffer

object Invalid {
  def readOnlyWithArgument(buffer: ByteBuffer): ByteBuffer =
    buffer.asReadOnlyBuffer(1)

  def queryWithArgument(buffer: ByteBuffer): Boolean =
    buffer.isReadOnly(1)
}
