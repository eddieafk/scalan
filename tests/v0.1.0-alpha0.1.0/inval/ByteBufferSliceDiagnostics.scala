// expected-error: slice accepts no arguments or an Int index and Int length

package tests.v010alpha010.invalidbytebufferslice

object Invalid {
  def sliceWithArgument(buffer: ByteBuffer): ByteBuffer =
    buffer.slice(1)
}
