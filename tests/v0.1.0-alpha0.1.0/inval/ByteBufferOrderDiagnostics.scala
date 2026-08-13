// expected-error: order accepts zero or one ByteOrder argument
// expected-error: order value must have type ByteOrder

package tests.v010alpha010.invalidbytebufferorder

object Invalid {
  def tooManyOrders(buffer: ByteBuffer): ByteBuffer =
    buffer.order(ByteOrder.BIG_ENDIAN, ByteOrder.LITTLE_ENDIAN)

  def wrongOrder(buffer: ByteBuffer): ByteBuffer =
    buffer.order(true)
}
