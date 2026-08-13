// expected-error: putDouble requires one Double value or an Int index and Double value
// expected-error: putDouble value must have type Double

package tests.v010alpha010.invalidrelativedoublebytebuffer

object Invalid {
  def putWithoutValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putDouble()

  def putFloatValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putDouble(1.0F)
}
