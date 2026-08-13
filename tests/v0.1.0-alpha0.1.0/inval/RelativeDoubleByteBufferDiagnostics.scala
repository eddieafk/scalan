// expected-error: getDouble does not accept arguments
// expected-error: putDouble requires one Double argument
// expected-error: putDouble value must have type Double

package tests.v010alpha010.invalidrelativedoublebytebuffer

object Invalid {
  def getWithIndex(buffer: ByteBuffer): Double =
    buffer.getDouble(0)

  def putWithoutValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putDouble()

  def putFloatValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putDouble(1.0F)
}
