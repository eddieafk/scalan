// expected-error: getDouble index must have type Int
// expected-error: getDouble accepts zero or one Int argument
// expected-error: putDouble index must have type Int
// expected-error: putDouble value must have type Double

package tests.v010alpha010.invalidindexeddoublebytebuffer

object Invalid {
  def wrongGetIndex(buffer: ByteBuffer): Double =
    buffer.getDouble("zero")

  def tooManyGetArguments(buffer: ByteBuffer): Double =
    buffer.getDouble(0, 1)

  def wrongPutIndex(buffer: ByteBuffer): ByteBuffer =
    buffer.putDouble("zero", 1.0)

  def wrongPutValue(buffer: ByteBuffer): ByteBuffer =
    buffer.putDouble(0, 1.0F)
}
