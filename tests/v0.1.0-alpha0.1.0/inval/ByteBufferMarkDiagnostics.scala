// expected-error: mark does not accept arguments
// expected-error: reset does not accept arguments

package tests.v010alpha010.invalidbytebuffermark

object Invalid {
  def markWithArgument(buffer: ByteBuffer): ByteBuffer =
    buffer.mark(1)

  def resetWithArgument(buffer: ByteBuffer): ByteBuffer =
    buffer.reset(1)
}
