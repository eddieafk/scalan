// expected-error: slice accepts no arguments or an Int index and Int length
// expected-error: slice index must have type Int
// expected-error: slice length must have type Int

package tests.v010alpha010.invalidindexedbytebufferslice

object Invalid {
  def wrongArity(buffer: ByteBuffer): ByteBuffer =
    buffer.slice(1)

  def wrongIndex(buffer: ByteBuffer): ByteBuffer =
    buffer.slice(true, 1)

  def wrongLength(buffer: ByteBuffer): ByteBuffer =
    buffer.slice(1, false)
}
