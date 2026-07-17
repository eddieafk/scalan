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

  def main = {
    val empty = Array[String]()
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
    println(rejectNegativeSize())
  }
}
