package demo.examples

class InterflowBox

class InterflowCastBase

class InterflowCastChild extends InterflowCastBase

class InterflowPair(val left: Int, var right: Int)

class InterflowBaseCounter(val start: Int) {
  val doubled: Int = start + start
}

class InterflowChildCounter(val seed: Int)
    extends InterflowBaseCounter(seed + 1) {
  val child: Int = start + seed
}

class InterflowExactScore {
  def value: Int =
    41

  def add(value: Int): Int =
    value + 1
}

object InterflowOptimizations {
  def propagatedLiteral: Int = {
    val base = 1
    val total = base + 2
    total
  }

  def propagatedExpression: Int = {
    val sum = 1 + 2
    sum
  }

  def propagatedIf: Int = {
    val chosen = if (true) 10 else 20
    chosen
  }

  def effectfulFlag(): Boolean = {
    println(false)
    false
  }

  def sameBranchIfAfterEffect: Int =
    if (effectfulFlag()) 12 else 12

  def discardedSameBranchIfAfterEffect: Int = {
    if (effectfulFlag()) 13 else 13
    14
  }

  def inlineConstant: Int =
    41

  def inlineConstantUse: Int =
    inlineConstant + 1

  def inlineAddOne(value: Int): Int =
    value + 1

  def inlineAddOneUse: Int =
    inlineAddOne(41)

  def inlineAddViaLocal(value: Int): Int = {
    val next = value + 1
    next
  }

  def inlineAddViaLocalUse: Int =
    inlineAddViaLocal(41)

  def inlineAddTwo(value: Int): Int =
    inlineAddOne(value) + 1

  def inlineAddTwoUse: Int =
    inlineAddTwo(40)

  def inlineAddTwoViaLocal(value: Int): Int = {
    val next = inlineAddOne(value)
    next + 1
  }

  def inlineAddTwoViaLocalUse: Int =
    inlineAddTwoViaLocal(40)

  def lateBlockInline(value: Int): Int = {
    {
      val next = value + 1
      next
    }
  }

  def lateBlockInlineUse: Int =
    lateBlockInline(41)

  def exactDevirtualizedSelect: Int = {
    val score = new InterflowExactScore()
    score.value + 1
  }

  def exactDevirtualizedCall: Int = {
    val score = new InterflowExactScore()
    score.add(41)
  }

  def leafDevirtualized(score: InterflowExactScore): Int =
    score.value + 1

  def leafCallWithDiscardableArg: Int =
    leafDevirtualized(new InterflowExactScore())

  def constructorFieldRead: Int = {
    val pair = new InterflowPair(20, 22)
    pair.left
  }

  def updatedConstructorFieldRead: Int = {
    val pair = new InterflowPair(20, 22)
    pair.right = 23
    pair.right
  }

  def unrelatedMutation(): Unit = {
    var marker = 0
    marker = 1
  }

  def touchPair(pair: InterflowPair): Unit =
    pair.right = pair.right

  def constructorFieldReadAfterUnrelatedCall: Int = {
    val pair = new InterflowPair(30, 32)
    unrelatedMutation()
    pair.left
  }

  def constructorFieldReadAfterPassedCall: Int = {
    val pair = new InterflowPair(40, 42)
    touchPair(pair)
    pair.left
  }

  def inheritedConstructorFieldRead: Int = {
    val counter = new InterflowChildCounter(4)
    counter.start
  }

  def inheritedInitializerFieldRead: Int = {
    val counter = new InterflowChildCounter(4)
    counter.doubled
  }

  def childInitializerFieldRead: Int = {
    val counter = new InterflowChildCounter(4)
    counter.child
  }

  def touchBaseCounter(counter: InterflowBaseCounter): Unit = {
    var marker = counter
    marker = counter
  }

  def inheritedFieldReadAfterPassedCall: Int = {
    val counter = new InterflowChildCounter(4)
    touchBaseCounter(counter)
    counter.start
  }

  def nestedDeadAllocation: Int = {
    {
      val score = new InterflowExactScore()
      42
    }
  }

  def deadLocalCleanup: Int = {
    val unused = 99
    val alsoUnused = unused + 1
    4
  }

  def integerIdentities(value: Int): Int =
    ((value + 0) * 1) - 0

  def integerNegation(value: Int): Int =
    0 - value

  def longNegation(value: Long): Long =
    value * -1L

  def floatingUnaryLiteral: Float =
    -1.25F

  def doubleUnaryLiteral: Double =
    +2.5

  def absorbingIdentities(value: Int, flag: Boolean): Int =
    if (flag || true) value * 0 else 99

  def booleanIdentities(flag: Boolean): Boolean =
    (flag && true) || false

  def booleanComplements(flag: Boolean): Boolean =
    (flag && !flag) || (flag == !flag)

  def deMorgan(flag: Boolean, other: Boolean): Boolean =
    !(flag && other)

  def negatedEquality(left: Int, right: Int): Boolean =
    !(left == right)

  def negatedIntComparison(left: Int, right: Int): Boolean =
    !(left < right)

  def canonicalIf(flag: Boolean): Boolean =
    if (flag) true else false

  def negatedConditionBranch(flag: Boolean): Boolean =
    if (!flag) flag else true

  def conditionalAnd(flag: Boolean, other: Boolean): Boolean =
    if (flag) other else false

  def conditionalOr(flag: Boolean, other: Boolean): Boolean =
    if (flag) true else other

  def negatedConditionalAnd(flag: Boolean, other: Boolean): Boolean =
    if (!flag) false else other

  def literalComparisons: Boolean =
    (3 < 4) && (5L >= 5L) && ('a' < 'z') && (1.5F <= 1.5F) && !(2.0 > 4.0)

  def literalEquality: Boolean =
    ({} == {}) && (null == null) && ("line\n" == """line
""") && ('ready == 'ready)

  def literalStringConcat: String =
    "Inter" + "flow"

  def primitiveStringConcat: String =
    ("n=" + 7) + (", ok=" + false)

  def boxedSymbolStringConcat: String = {
    val symbol: Any = 'ready
    "symbol=" + symbol
  }

  def boxedStringConcat: String = {
    val project: Any = "Scala Native"
    "project=" + project
  }

  def nullStringConcat: String =
    "missing=" + null

  def formattedBooleanLiteral: String = {
    val enabled = true
    f"$enabled%b"
  }

  def formattedCharLiteral: String = {
    val initial = 'N'
    f"$initial%c"
  }

  def formattedStringLiteral: String = {
    val project = "Scala Native"
    f"$project%s"
  }

  def boxedLocalUnbox: Int = {
    val boxed: Any = 42
    boxed.asInstanceOf[Int]
  }

  def boxedAnyEquals: Boolean = {
    val left: Any = 7
    val right: Any = 7
    left == right
  }

  def boxedFloatRoundedAnyEquals: Boolean = {
    val left: Any = 1.0F
    val right: Any = 1.00000001F
    left == right
  }

  def boxedNullAnyEquals: Boolean = {
    val missing: Any = null
    val value: Any = 7
    missing == value
  }

  def effectfulUnit(): Unit =
    println("unit-effect")

  def effectfulUnitAnyEquals: Boolean = {
    val left: Any = effectfulUnit()
    val right: Any = effectfulUnit()
    left == right
  }

  def effectfulUnitHash: Int = {
    val value: Any = effectfulUnit()
    value.hashCode
  }

  def effectfulUnitToString: String = {
    val value: Any = effectfulUnit()
    value.toString
  }

  def effectfulUnitConcat: String = {
    val value: Any = effectfulUnit()
    "value=" + value
  }

  def sameReferenceAnyEquals: Boolean = {
    val box = new InterflowBox()
    val alias: Any = box
    alias == box
  }

  def distinctFreshReferenceEquals: Boolean = {
    val left = new InterflowBox()
    val right = new InterflowBox()
    left == right
  }

  def distinctFreshAnyEquals: Boolean = {
    val left: Any = new InterflowBox()
    val right: Any = new InterflowBox()
    left == right
  }

  def sameReferenceHashEquals: Boolean = {
    val box = new InterflowBox()
    val alias = box
    box.hashCode == alias.hashCode
  }

  def arrayLiteralLength: Int = {
    val values = Array(1, 2, 3)
    values.length
  }

  def arrayLiteralElement: Int = {
    val values = Array(4, 5, 6)
    values(1)
  }

  def arrayUpdatedElement: Int = {
    val values = Array(1, 2)
    values(0) = 9
    values(0)
  }

  def arrayDynamicUpdateLength(index: Int): Int = {
    val values = Array(1, 2)
    values(index) = 9
    values.length
  }

  def arrayAnyUpdatedElement: Int = {
    val values = Array[Any](1, 2)
    values(1) = 11
    values(1).asInstanceOf[Int]
  }

  def arrayPureDiscard: Int = {
    {
      val values = Array(7, 8)
      9
    }
  }

  def stringNullComparison: Boolean =
    "Scala" != null

  def sameLocal(value: Int): Boolean =
    (value == value) && !(value < value)

  def sameSize: Int =
    sizeof[Int] - sizeof[Int]

  def constantFalseLoop: Int = {
    var marker = 7
    while (false) {
      marker = 99
    }
    marker
  }

  def nestedPureDiscard(value: Any): Int = {
    { value.isInstanceOf[String] }
    4
  }

  def nullTypeTest: Boolean =
    null.isInstanceOf[InterflowBox]

  def nullCast: InterflowBox =
    null.asInstanceOf[InterflowBox]

  def nullAlias: InterflowBox = {
    val missing = null.asInstanceOf[InterflowBox]
    missing
  }

  def exactStringTypeTest: Boolean = {
    val direct: Any = "Scala"
    direct.isInstanceOf[String]
  }

  def disjointStringTypeTest: Boolean = {
    val direct: Any = "Scala"
    direct.isInstanceOf[Symbol]
  }

  def sameTypeCast(value: InterflowBox): InterflowBox =
    value.asInstanceOf[InterflowBox]

  def hierarchyCast(value: InterflowCastChild): InterflowCastBase =
    value.asInstanceOf[InterflowCastBase]

  def hierarchyTypeTest: Boolean = {
    val child = new InterflowCastChild()
    child.isInstanceOf[InterflowCastBase]
  }

  def hierarchyTypeMismatch: Boolean = {
    val child = new InterflowCastChild()
    child.isInstanceOf[InterflowBox]
  }

  def main = {
    println(propagatedLiteral)
    println(propagatedExpression)
    println(propagatedIf)
    println(sameBranchIfAfterEffect)
    println(discardedSameBranchIfAfterEffect)
    println(inlineConstantUse)
    println(inlineAddOneUse)
    println(inlineAddViaLocalUse)
    println(inlineAddTwoUse)
    println(inlineAddTwoViaLocalUse)
    println(lateBlockInlineUse)
    println(exactDevirtualizedSelect)
    println(exactDevirtualizedCall)
    println(leafDevirtualized(new InterflowExactScore()))
    println(leafCallWithDiscardableArg)
    println(constructorFieldRead)
    println(updatedConstructorFieldRead)
    println(constructorFieldReadAfterUnrelatedCall)
    println(constructorFieldReadAfterPassedCall)
    println(inheritedConstructorFieldRead)
    println(inheritedInitializerFieldRead)
    println(childInitializerFieldRead)
    println(inheritedFieldReadAfterPassedCall)
    println(nestedDeadAllocation)
    println(deadLocalCleanup)
    println(integerIdentities(41))
    println(integerNegation(41))
    println(longNegation(41L))
    println(floatingUnaryLiteral)
    println(doubleUnaryLiteral)
    println(absorbingIdentities(123, false))
    println(booleanIdentities(true))
    println(booleanComplements(true))
    println(deMorgan(true, false))
    println(negatedEquality(1, 2))
    println(negatedIntComparison(1, 2))
    println(canonicalIf(false))
    println(negatedConditionBranch(false))
    println(conditionalAnd(true, false))
    println(conditionalOr(false, true))
    println(negatedConditionalAnd(false, true))
    println(literalComparisons)
    println(literalEquality)
    println(literalStringConcat)
    println(primitiveStringConcat)
    println(boxedSymbolStringConcat)
    println(boxedStringConcat)
    println(nullStringConcat)
    println(formattedBooleanLiteral)
    println(formattedCharLiteral)
    println(formattedStringLiteral)
    println(boxedLocalUnbox)
    println(boxedAnyEquals)
    println(boxedFloatRoundedAnyEquals)
    println(boxedNullAnyEquals)
    println(effectfulUnitAnyEquals)
    println(effectfulUnitHash)
    println(effectfulUnitToString)
    println(effectfulUnitConcat)
    println(sameReferenceAnyEquals)
    println(distinctFreshReferenceEquals)
    println(distinctFreshAnyEquals)
    println(sameReferenceHashEquals)
    println(arrayLiteralLength)
    println(arrayLiteralElement)
    println(arrayUpdatedElement)
    println(arrayDynamicUpdateLength(1))
    println(arrayAnyUpdatedElement)
    println(arrayPureDiscard)
    println(stringNullComparison)
    println(sameLocal(5))
    println(sameSize)
    println(constantFalseLoop)
    println(nestedPureDiscard("Scala"))
    println(nullTypeTest)
    println(nullCast == null)
    println(nullAlias == null)
    println(exactStringTypeTest)
    println(disjointStringTypeTest)
    println(sameTypeCast(new InterflowBox).isInstanceOf[InterflowBox])
    println(hierarchyCast(new InterflowCastChild).isInstanceOf[InterflowCastBase])
    println(hierarchyTypeTest)
    println(hierarchyTypeMismatch)
  }
}
