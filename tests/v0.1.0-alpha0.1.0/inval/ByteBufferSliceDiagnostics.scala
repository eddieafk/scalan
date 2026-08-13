// expected-error: slice does not accept arguments

package tests.v010alpha010.invalidbytebufferslice

object Invalid {
  def sliceWithArgument(buffer: ByteBuffer): ByteBuffer =
    buffer.slice(1)
}
