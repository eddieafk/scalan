package demo.exceptions

class AppFailure(message: String) extends Exception(message) {
  override def toString: String = "AppFailure: " + getMessage
}

class FatalError extends Error("fatal")

object ExceptionExamples {
  def makeFailure(): AppFailure = {
    val failure = new AppFailure("could not save the document")
    failure.initCause(new Exception("storage is unavailable"))
    failure.addSuppressed(new Exception("temporary-file cleanup failed"))
    failure
  }

  def catchFailure(failure: AppFailure): String =
    try {
      throw failure
    } catch {
      case caught: AppFailure => "caught: " + caught.getMessage
    }

  def printTraceHead(label: String, failure: Throwable): Unit = {
    val frames = failure.getStackTrace
    println(label + frames(0).toString)
  }

  def rejectNullTrace(failure: Throwable): String =
    try {
      failure.setStackTrace(null)
      "null trace was accepted"
    } catch {
      case error: NullPointerException => "typed failure: " + error.getMessage
    }

  def rejectNullArray(): String = {
    val values: Array[Int] = null
    try {
      println(values.length)
      "null array was accepted"
    } catch {
      case error: NullPointerException => "typed array failure: " + error.getMessage
    }
  }

  def rejectBadArrayIndex(): String = {
    val values = Array(1)
    try {
      println(values(1))
      "bad index was accepted"
    } catch {
      case error: IndexOutOfBoundsException =>
        "typed bounds failure: " + error.getMessage
    }
  }

  def rejectBadCast(value: Throwable): String =
    try {
      println(value.asInstanceOf[StackTraceElement])
      "bad cast was accepted"
    } catch {
      case error: ClassCastException => "typed cast failure: " + error.getMessage
    }

  def rejectNullReceiver(): String = {
    val failure: AppFailure = null
    try {
      println(failure.getMessage)
      "null receiver was accepted"
    } catch {
      case error: NullPointerException =>
        "typed receiver failure: " + error.getMessage
    }
  }

  def rejectNullString(): String = {
    val value: String = null
    try {
      println(value.length)
      "null String receiver was accepted"
    } catch {
      case error: NullPointerException =>
        "typed String failure: " + error.getMessage
    }
  }

  def rejectNullAny(): String = {
    val value: Any = null
    try {
      println(value.toString)
      "null Any receiver was accepted"
    } catch {
      case error: NullPointerException =>
        "typed Any failure: " + error.getMessage
    }
  }

  def rejectNullThrow(): String = {
    val failure: Throwable = null
    try {
      throw failure
    } catch {
      case error: NullPointerException =>
        "typed throw failure: " + error.getMessage
    }
  }

  def catchRuntimeFailure(): String = {
    val values: Array[Int] = null
    try {
      println(values.length)
      "RuntimeException was not caught"
    } catch {
      case error: RuntimeException =>
        "runtime parent failure: " + error.getMessage
    }
  }

  def divideInt(value: Int, divisor: Int): Int = value / divisor
  def divideLong(value: Long, divisor: Long): Long = value / divisor
  def remainderLong(value: Long, divisor: Long): Long = value % divisor

  def catchIntArithmetic(): String =
    try {
      divideInt(42, 0)
      "integer division unexpectedly succeeded"
    } catch {
      case error: ArithmeticException =>
        "int arithmetic failure: " + error.getMessage + "|" + error.getCause
    }

  def catchLongArithmeticParent(): String =
    try {
      remainderLong(42L, 0L)
      "long remainder unexpectedly succeeded"
    } catch {
      case error: RuntimeException =>
        "long arithmetic parent failure: " + error.getMessage
    }

  def minimumDivision(): String = {
    val minInt = (0 - 2147483647) - 1
    val minLong = (0L - 9223372036854775807L) - 1L
    divideInt(minInt, 0 - 1) + "|" + divideLong(minLong, 0L - 1L)
  }

  def floatingDivision(): Boolean =
    (1.0 / 0.0) > 0.0

  def catchAssertion(): String = {
    assert(true)
    try {
      assert(false)
      "failed assertion unexpectedly passed"
    } catch {
      case exception: Exception => "AssertionError incorrectly matched Exception"
      case error: AssertionError =>
        "assertion failure: " + error.getMessage + "|" + error.getCause
    }
  }

  def catchAssumption(): String = {
    assume(true)
    try {
      assume(false)
      "failed assumption unexpectedly passed"
    } catch {
      case exception: Exception => "AssertionError incorrectly matched Exception"
      case error: AssertionError =>
        "assumption failure: " + error.getMessage + "|" + error.getCause
    }
  }

  def pendingOperation(): String = ???

  def catchNotImplemented(): String =
    try {
      pendingOperation()
      "unimplemented expression unexpectedly returned"
    } catch {
      case exception: Exception =>
        "NotImplementedError incorrectly matched Exception"
      case error: NotImplementedError =>
        "not implemented failure: " + error.getMessage + "|" + error.getCause
    }

  def catchRequirement(): String = {
    require(true)
    try {
      require(false)
      "failed requirement unexpectedly passed"
    } catch {
      case error: Error => "IllegalArgumentException incorrectly matched Error"
      case failure: IllegalArgumentException =>
        "requirement failure: " + failure.getMessage + "|" + failure.getCause
    }
  }

  def catchErrorBranch(): String =
    try {
      throw new FatalError()
    } catch {
      case exception: Exception => "Error incorrectly matched Exception"
      case error: Error =>
        "error branch: " + error.getMessage + "|" + error.getCause
    }

  def printHandledDiagnostic(): String = {
    val failure = new AppFailure("handled diagnostic")
    failure.initCause(new Exception("diagnostic cause"))
    failure.addSuppressed(new Exception("diagnostic cleanup"))
    failure.fillInStackTrace
    failure.printStackTrace()
    "continued after printStackTrace"
  }

  def throwUncaught(failure: AppFailure): Int =
    throw failure
}

object Main {
  def main = {
    val failure = ExceptionExamples.makeFailure()

    println(failure.toString)
    println("cause: " + failure.getCause.toString)

    val suppressed = failure.getSuppressed
    println("suppressed: " + suppressed(0).toString)

    ExceptionExamples.printTraceHead("constructed at: ", failure)
    println(ExceptionExamples.catchFailure(failure))
    ExceptionExamples.printTraceHead("after catch: ", failure)

    failure.fillInStackTrace
    ExceptionExamples.printTraceHead("refilled at: ", failure)

    failure.setStackTrace(Array(
      new StackTraceElement("demo.Debugger.entry", "Exceptions.scala", 100, 1)
    ))
    ExceptionExamples.printTraceHead("custom trace: ", failure)
    println(ExceptionExamples.rejectNullTrace(failure))
    ExceptionExamples.printTraceHead("preserved trace: ", failure)
    println(ExceptionExamples.rejectNullArray())
    println(ExceptionExamples.rejectBadArrayIndex())
    println(ExceptionExamples.rejectBadCast(failure))
    println(ExceptionExamples.rejectNullReceiver())
    println(ExceptionExamples.rejectNullString())
    println(ExceptionExamples.rejectNullAny())
    println(ExceptionExamples.rejectNullThrow())
    println(ExceptionExamples.catchRuntimeFailure())
    println(ExceptionExamples.catchIntArithmetic())
    println(ExceptionExamples.catchLongArithmeticParent())
    println(ExceptionExamples.minimumDivision())
    println(ExceptionExamples.floatingDivision())
    println(ExceptionExamples.catchAssertion())
    println(ExceptionExamples.catchAssumption())
    println(ExceptionExamples.catchNotImplemented())
    println(ExceptionExamples.catchRequirement())
    println(ExceptionExamples.catchErrorBranch())
    println(ExceptionExamples.printHandledDiagnostic())

    println("throwing uncaught failure")
    ExceptionExamples.throwUncaught(failure)
    println("unreachable")
  }
}
