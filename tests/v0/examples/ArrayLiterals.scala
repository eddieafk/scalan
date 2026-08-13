package demo.examples

class ArrayBase(val value: Int)
class ArrayChild(value: Int) extends ArrayBase(value)
class ArrayOther(value: Int) extends ArrayBase(value)

object ArrayLiterals {
  def dynamicBehavior(length: Int): String = {
    val values = new Array[Int](length)
    val names = new Array[String](length)
    values(1) = 9
    values.length + "|" + values(0) + "|" + values(1) + "|" +
      (names(0) == null)
  }

  def rejectNegativeSize(): String =
    try {
      println(new Array[Double](0 - 1).length)
      "negative size was accepted"
    } catch {
      case failure: NegativeArraySizeException =>
        "negative size failure: " + failure.getMessage
    }

  def nestedBehavior(rows: Int, columns: Int): String = {
    val matrix = Array.ofDim[Array[Int]](rows)
    val row = Array.ofDim[Int](columns)
    row(1) = 7
    matrix(0) = row
    val selected = matrix(0)
    matrix.length + "|" + (matrix(1) == null) + "|" + selected.length +
      "|" + selected(0) + "|" + selected(1)
  }

  def multiDimensionalBehavior(rows: Int, columns: Int): String = {
    val matrix = Array.ofDim[Int](rows, columns)
    matrix(0)(1) = 11
    matrix.length + "|" + matrix(0).length + "|" + matrix(0)(0) + "|" +
      matrix(0)(1) + "|" + matrix(1)(0)
  }

  def cloneBehavior(): String = {
    val original = Array("first", "second")
    val copied = original.clone()
    copied(1) = "changed"
    val matrix = Array.ofDim[Int](1, 2)
    matrix(0)(1) = 7
    val shallow = matrix.clone()
    shallow(0)(1) = 8
    original(1) + "|" + copied(1) + "|" + matrix(0)(1)
  }

  def copyBehavior(): String = {
    val values = Array(1, 2, 3, 4)
    Array.copy(values, 0, values, 1, 3)
    val names = Array("empty", "empty")
    Array.copy(Array("left", "right"), 0, names, 0, 2)
    values(0) + "|" + values(1) + "|" + values(2) + "|" + values(3) +
      "|" + names(0) + "|" + names(1)
  }

  def checkedReferenceCopy(): String = {
    val child = new ArrayChild(7)
    val widened = new Array[ArrayBase](1)
    Array.copy(Array[ArrayChild](child), 0, widened, 0, 1)
    val narrowed = new Array[ArrayChild](2)
    val failure = try {
      Array.copy(Array[ArrayBase](child, new ArrayOther(8)), 0, narrowed, 0, 2)
      "incompatible element was accepted"
    } catch {
      case error: ArrayStoreException => error.getMessage
    }
    widened(0).value + "|" + (narrowed(0) == child) + "|" +
      (narrowed(1) == null) + "|" + failure
  }

  def fillBehavior(): String = {
    var counter = 0
    val values = Array.fill[Int](3)({
      counter = counter + 1
      counter
    })
    values(0) + "|" + values(1) + "|" + values(2) + "|" + counter
  }

  def multiFillBehavior(): String = {
    var counter = 0
    val matrix = Array.fill[Int](2, 3)({
      counter = counter + 1
      counter
    })
    matrix(0)(0) = 20
    matrix.length + "|" + matrix(0).length + "|" + matrix(0)(0) + "|" +
      matrix(0)(1) + "|" + matrix(1)(0) + "|" + counter + "|" +
      (matrix(0) == matrix(1))
  }

  def rangeBehavior(): String = {
    val ascending = Array.range(1, 8, 2)
    val descending = Array.range(7, 0, 0 - 3)
    val defaults = Array.range(2, 5)
    ascending.length + "|" + ascending(0) + "|" + ascending(3) + "|" +
      descending(0) + "|" + descending(2) + "|" + defaults(2)
  }

  def rejectZeroRangeStep(): String =
    try {
      println(Array.range(1, 5, 0).length)
      "zero range step was accepted"
    } catch {
      case failure: IllegalArgumentException =>
        "zero range step failure: " + failure.getMessage
    }

  def concatBehavior(): String = {
    val values =
      Array.concat[Int](Array(1, 2), Array.empty[Int], Array(3, 4))
    val child = new ArrayChild(7)
    val references = Array.concat[ArrayBase](
      Array[ArrayBase](child),
      Array[ArrayBase](new ArrayOther(8))
    )
    val first = Array(5)
    val nested = Array.concat[Array[Int]](
      Array[Array[Int]](first),
      Array[Array[Int]](Array(6))
    )
    nested(0)(0) = 9
    values.length + "|" + values(0) + "|" + values(3) + "|" +
      references(0).value + "|" + first(0) + "|" + nested(1)(0)
  }

  def rejectNullConcat(): String = {
    val missing: Array[Int] = null
    try {
      println(Array.concat[Int](Array(1), missing, Array(3)).length)
      "null concat input was accepted"
    } catch {
      case failure: NullPointerException =>
        "null concat failure: " + failure.getMessage
    }
  }

  def main = {
    val empty = Array.empty[String]
    val colors = Array("red", "blue")
    colors(1) = "green"
    println(empty.length)
    println(colors.length)
    println(colors(0))
    println(colors(1))
    println(dynamicBehavior(3))
    println(nestedBehavior(2, 3))
    println(multiDimensionalBehavior(2, 3))
    println(cloneBehavior())
    println(copyBehavior())
    println(checkedReferenceCopy())
    println(fillBehavior())
    println(multiFillBehavior())
    println(rangeBehavior())
    println(rejectZeroRangeStep())
    println(concatBehavior())
    println(rejectNullConcat())
    println(rejectNegativeSize())
  }
}
