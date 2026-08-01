#include "scalanative/support/Diagnostics.h"
#include "scalanative/tools/build/BuildDriver.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

int fail(const std::string& message) {
  std::cerr << message << '\n';
  return 1;
}

bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::string readTextFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

int smokeCompositeTypes() {
  constexpr const char* source = R"(package demo.compositetypes

trait CommonName {
  def commonName(): String
}
trait Left extends CommonName {
  def leftName(): String
}
trait Right extends CommonName {
  def rightName(): String
}
trait Tagged {
  def tagName(): String
}
trait LeftLabel {
  def label(): String
}
trait RightLabel {
  def label(): String
}
trait LeftResult {
  def combinedResult(): Left
}
trait RightResult {
  def combinedResult(): Right
}
trait LeftStableResult {
  val stableResult: Left
}
trait RightStableResult {
  val stableResult: Right
}
transparent class HiddenBase {
  def hiddenName(): String = "hidden-base"
}
class HiddenLeft extends HiddenBase
class HiddenRight extends HiddenBase
transparent trait HiddenMarker {
  def hiddenMarker(): String
}
trait VisibleName {
  def visibleName(): String
}
trait Payload[+A] {
  def payload(): A
}
trait LeftPayload[+A] extends Payload[A]
trait RightPayload[+A] extends Payload[A]
trait Consumer[-A] {
  def consumerName(): String
}
trait Cell[A] {
  def cell(): A
}
trait BoundedPayload[+A <: CommonName] {
  def boundedPayload(): A
}

class LeftValue extends Left {
  def commonName(): String = "left-common"
  def leftName(): String = "left-only"
}
class LeftTaggedValue extends Left with Tagged {
  def commonName(): String = "left-tagged-common"
  def leftName(): String = "left-tagged-only"
  def tagName(): String = "tagged-left"
}
class RightValue extends Right {
  def commonName(): String = "right-common"
  def rightName(): String = "right-only"
}
class BothValue extends Right with Tagged {
  def commonName(): String = "both-common"
  def rightName(): String = "both-right"
  def tagName(): String = "tagged-right"
}
class CompleteValue extends Left with Right {
  def commonName(): String = "complete-common"
  def leftName(): String = "complete-left"
  def rightName(): String = "complete-right"
}
class CombinedResult extends LeftResult with RightResult {
  override def combinedResult(): Left & Right = new CompleteValue
}
class CombinedStableResult extends LeftStableResult with RightStableResult {
  override val stableResult: Left & Right = new CompleteValue
}
class BothLabels extends LeftLabel with RightLabel {
  def label(): String = "both-label"
}
class LeftLabelValue extends LeftLabel {
  def label(): String = "left-label"
}
class RightLabelValue extends RightLabel {
  def label(): String = "right-label"
}
class VisibleLeft extends VisibleName with HiddenMarker {
  def visibleName(): String = "visible-left"
  def hiddenMarker(): String = "hidden-left"
}
class VisibleRight extends VisibleName with HiddenMarker {
  def visibleName(): String = "visible-right"
  def hiddenMarker(): String = "hidden-right"
}
class IntPayloadLeft extends Payload[Int] {
  def payload(): Int = 31
}
class IntPayloadRight extends Payload[Int] {
  def payload(): Int = 32
}
class StringPayload extends Payload[String] {
  def payload(): String = "payload-string"
}
class TransitiveLeftPayload extends LeftPayload[Int] {
  def payload(): Int = 51
}
class TransitiveRightPayload extends RightPayload[Int] {
  def payload(): Int = 52
}
class IntConsumer extends Consumer[Int] {
  def consumerName(): String = "int-consumer"
}
class StringConsumer extends Consumer[String] {
  def consumerName(): String = "string-consumer"
}
class IntCell extends Cell[Int] {
  def cell(): Int = 41
}
class StringCell extends Cell[String] {
  def cell(): String = "cell-string"
}
class BoundedLeftPayload extends BoundedPayload[Left] {
  def boundedPayload(): Left = new LeftValue
}
class BoundedRightPayload extends BoundedPayload[Right] {
  def boundedPayload(): Right = new RightValue
}
class Box[A](val value: A)

type Choice = Left | Right
type Required = Right & Tagged
type GenericChoice[A, B] = A | B
type GenericRequired[A, B] = A & B
type GroupedRequired = (Left | Right) & Tagged
type NestedRequired = Choice & Tagged
type GenericDistributed = GenericRequired[Choice, Tagged]
type DistributedSource = Tagged & (Left | Right)
type DistributedTarget = (Tagged & Left) | (Tagged & Right)
type CombinedResultSource = LeftResult & RightResult
type CombinedStableResultSource = LeftStableResult & RightStableResult

object Main {
  val unionField: Left | Right = new LeftValue
  val intersectionField: Right & Tagged = new BothValue
  val narrowedField: Right = intersectionField
  val unionBox: Box[Left | Right] =
    new Box[Left | Right](new LeftValue)

  def unionName(value: Left | Right): String = "union"
  def aliasUnionName(value: Choice): String = "alias-union"
  def genericUnionName(value: GenericChoice[Left, Right]): String =
    "generic-union"
  def projectedCommonName(value: Choice): String = value.commonName()
  def intersectionName(value: Right & Tagged): String = "intersection"
  def projectedRightName(value: Required): String = value.rightName()
  def projectedTagName(value: Required): String = value.tagName()
  def projectedMergedLabel(value: LeftLabel & RightLabel): String =
    value.label()
  def projectedCombinedResult(value: CombinedResultSource): Left & Right =
    value.combinedResult()
  def projectedCombinedLeft(value: CombinedResultSource): String =
    value.combinedResult().leftName()
  def projectedCombinedRight(value: CombinedResultSource): String =
    value.combinedResult().rightName()
  def projectedStableResult(value: CombinedStableResultSource): String =
    value.stableResult.rightName()
  def projectedAppliedResult(
      value: Payload[Left] & Payload[Right]): Left & Right =
    value.payload()
  def genericIntersectionName(
      value: GenericRequired[Right, Tagged]): String =
    "generic-intersection"
  def precedenceName(value: Left | Right & Tagged): String = "precedence"
  def groupedName(value: GroupedRequired): String = "grouped"
  def nestedName(value: NestedRequired): String = "nested"
  def projectedNestedCommonName(value: NestedRequired): String =
    value.commonName()
  def projectedNestedTagName(value: NestedRequired): String = value.tagName()
  def genericDistributedName(value: GenericDistributed): String =
    "generic-distributed"
  def distributedTargetName(value: DistributedTarget): String =
    "distributed-target"
  def distributedSourceName(value: DistributedSource): String =
    "distributed-source"
  def objectName(value: Object): String = "object"
  def rightName(value: Right): String = "right"
  def boxName(value: Box[Left | Right]): String = "box"
  def chooseLeft(): Choice = new LeftValue
  def chooseRight(): GenericChoice[Left, Right] = new RightValue
  def chooseBoth(): Required = new BothValue
  def narrowResult(value: Required): Right = value
  def inferredCommon(flag: Boolean) =
    if (flag) new LeftValue else new RightValue
  def inferredUnrelated(flag: Boolean) =
    if (flag) new LeftLabelValue else new RightLabelValue
  def inferredCommonTagged(flag: Boolean) =
    if (flag) new LeftTaggedValue else new BothValue
  def inferredScalar(flag: Boolean) = if (flag) 7 else "seven"
  def inferredMatch(value: Int) =
    value match {
      case 0 => new LeftValue
      case _ => new RightValue
    }
  def inferredTransparentClass(flag: Boolean) =
    if (flag) new HiddenLeft else new HiddenRight
  def inferredVisibleWithoutMarker(flag: Boolean) =
    if (flag) new VisibleLeft else new VisibleRight
  def firstOf[A](left: A, right: A): A = left
  def localWidened(flag: Boolean): String = {
    val inferred =
      if (flag) new LeftValue else new RightValue
    inferred.commonName()
  }
  def localHardUnion(flag: Boolean): String = {
    val explicit: Choice =
      if (flag) new LeftValue else new RightValue
    explicit.commonName()
  }
  def genericWidened(): String = {
    val inferred = firstOf(new LeftValue, new RightValue)
    inferred.commonName()
  }
  def inferredGenericTransparent() =
    firstOf(new HiddenLeft, new HiddenRight)
  def inferredGenericUnrelated() =
    firstOf(new LeftLabelValue, new RightLabelValue)
  def inferredGenericScalar() = firstOf(11, "eleven")
  def inferredGenericBox(flag: Boolean) =
    new Box(if (flag) new HiddenLeft else new HiddenRight)
  def inferredAppliedSame(flag: Boolean) =
    if (flag) new IntPayloadLeft else new IntPayloadRight
  def inferredAppliedCovariant(flag: Boolean) =
    if (flag) new IntPayloadLeft else new StringPayload
  def inferredAppliedTransitive(flag: Boolean) =
    if (flag) new TransitiveLeftPayload else new TransitiveRightPayload
  def inferredAppliedContravariant(flag: Boolean) =
    if (flag) new IntConsumer else new StringConsumer
  def inferredAppliedInvariant(flag: Boolean) =
    if (flag) new IntCell else new StringCell
  def inferredAppliedBounded(flag: Boolean) =
    if (flag) new BoundedLeftPayload else new BoundedRightPayload
  def appliedSamePayload(value: Payload[Int]): Int = value.payload()
  def appliedCovariantPayload(value: Payload[Int | String]): Int | String =
    value.payload()
  def appliedTransitivePayload(value: Payload[Int]): Int = value.payload()
  def appliedContravariantName(value: Consumer[Int & String]): String =
    value.consumerName()
  def appliedBoundedName(value: BoundedPayload[CommonName]): String =
    value.boundedPayload().commonName()
  def unrelatedName(value: LeftLabelValue | RightLabelValue): String =
    "unrelated"
  def invariantCellName(value: IntCell | StringCell): String = "invariant-cell"

  def main(args: Array[String]): Unit = {
    val localUnion: Choice = new RightValue
    val localIntersection: GenericRequired[Right, Tagged] = new BothValue
    val narrowedLocal: Right = localIntersection
    var narrowedVar: Right = new BothValue
    narrowedVar = localIntersection
    val nestedIntersection: NestedRequired = new LeftTaggedValue
    val genericDistributed: GenericDistributed = new BothValue
    val distributedSource: DistributedSource = new LeftTaggedValue
    val distributedTarget: DistributedTarget = new BothValue
    println(unionName(new LeftValue))
    println(unionName(new RightValue))
    println(aliasUnionName(unionField))
    println(genericUnionName(localUnion))
    println(projectedCommonName(new LeftValue))
    println(projectedCommonName(localUnion))
    println(intersectionName(new BothValue))
    println(projectedRightName(localIntersection))
    println(projectedTagName(localIntersection))
    println(projectedMergedLabel(new BothLabels))
    println(projectedCombinedLeft(new CombinedResult))
    println(projectedCombinedRight(new CombinedResult))
    println(projectedCombinedResult(new CombinedResult).commonName())
    println(projectedStableResult(new CombinedStableResult))
    println(genericIntersectionName(localIntersection))
    println(precedenceName(new LeftValue))
    println(precedenceName(new BothValue))
    println(groupedName(new LeftTaggedValue))
    println(groupedName(new BothValue))
    println(nestedName(nestedIntersection))
    println(projectedNestedCommonName(nestedIntersection))
    println(projectedNestedTagName(nestedIntersection))
    println(genericDistributedName(genericDistributed))
    println(distributedTargetName(distributedSource))
    println(distributedSourceName(distributedTarget))
    println(objectName(localUnion))
    println(rightName(localIntersection))
    println(rightName(narrowedField))
    println(rightName(narrowedLocal))
    println(rightName(narrowedVar))
    println(rightName(narrowResult(localIntersection)))
    println(boxName(unionBox))
    println(unionName(chooseLeft()))
    println(unionName(chooseRight()))
    println(rightName(chooseBoth()))
    println(inferredCommon(true).commonName())
    println(inferredCommon(false).commonName())
    println(inferredCommonTagged(true).commonName())
    println(inferredCommonTagged(false).tagName())
    println(localWidened(false))
    println(localHardUnion(true))
    println(inferredMatch(0).commonName())
    println(inferredMatch(1).commonName())
    println(genericWidened())
    println(inferredTransparentClass(true).hiddenName())
    println(inferredTransparentClass(false).hiddenName())
    println(inferredVisibleWithoutMarker(true).visibleName())
    println(inferredVisibleWithoutMarker(false).visibleName())
    println(inferredGenericTransparent().hiddenName())
    println(unrelatedName(inferredGenericUnrelated()))
    println(inferredGenericScalar().asInstanceOf[Int])
    println(firstOf("twelve", 12).asInstanceOf[String])
    println(inferredGenericBox(false).value.hiddenName())
    println(inferredGenericBox(false).value.isInstanceOf[HiddenRight])
    println(appliedSamePayload(inferredAppliedSame(true)))
    println(appliedSamePayload(inferredAppliedSame(false)))
    println(appliedCovariantPayload(inferredAppliedCovariant(true)).asInstanceOf[Int])
    println(appliedCovariantPayload(inferredAppliedCovariant(false)).asInstanceOf[String])
    println(appliedTransitivePayload(inferredAppliedTransitive(true)))
    println(appliedTransitivePayload(inferredAppliedTransitive(false)))
    println(appliedContravariantName(inferredAppliedContravariant(true)))
    println(appliedContravariantName(inferredAppliedContravariant(false)))
    println(invariantCellName(inferredAppliedInvariant(false)))
    println(appliedBoundedName(inferredAppliedBounded(true)))
    println(appliedBoundedName(inferredAppliedBounded(false)))
    println(inferredScalar(true).asInstanceOf[Int])
    println(inferredScalar(false).asInstanceOf[String])
    println(unrelatedName(inferredUnrelated(false)))
  }
}
)";
  constexpr const char* invalidSource =
      R"(package demo.invalidcompositetypes

transparent object Unsupported

trait Left {
  def commonSpelling(): String
}
trait Right {
  def commonSpelling(): String
}
trait Tagged
trait LeftConflict {
  def conflict(value: Int): Left
}
trait RightConflict {
  def conflict(value: String): Right
}
trait LeftResult {
  def combinedResult(): Left
}
trait RightResult {
  def combinedResult(): Right
}
trait Sink[A] {
  def accept(value: A): String
}
trait ConcreteLeftResult {
  def concreteResult(): Left = new LeftValue
}
trait ConcreteRightResult {
  def concreteResult(): Right = new RightValue
}
class LeftValue extends Left {
  def commonSpelling(): String = "left"
}
class RightValue extends Right {
  def commonSpelling(): String = "right"
}
class Other
class InvalidResult extends LeftResult with RightResult {
  override def combinedResult(): Left = new LeftValue
}
class InvalidConcreteResult extends ConcreteLeftResult with ConcreteRightResult

object Main {
  def union(value: Left | Right): Int = 1
  def intersection(value: Left & Tagged): Int = 2
  def needLeft(value: Left): Int = 3
  def grouped(value: (Left | Right) & Tagged): Int = 4
  def boundedFirst[A <: Left](left: A, right: A): A = left
  def invalidUnionProjection(value: Left | Right): String =
    value.commonSpelling()
  def incompatibleIntersectionProjection(
      value: LeftConflict & RightConflict): Left & Right =
    value.conflict(1)
  def incompatibleAppliedIntersection(
      value: Sink[Left] & Sink[Right]): String =
    value.accept(new LeftValue)

  val badUnion: Left | Right = new Other
  val badIntersection: Left & Tagged = new LeftValue

  def main(args: Array[String]): Unit = {
    val choice: Left | Right = new RightValue
    union(new Other)
    intersection(new LeftValue)
    grouped(new LeftValue)
    boundedFirst(new LeftValue, new RightValue)
    println(needLeft(choice))
  }
}
)";
  constexpr const char* malformedSource =
      R"(package demo.malformedcompositetypes

trait Left

object Main {
  def broken(value: Left |): Int = 1
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "cpp-scalanative-smoke-composite-types";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-composite-types.out";
  std::error_code ignored;
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildOptions options;
  options.action = scalanative::tools::build::BuildAction::BuildBinary;
  options.optimize = true;
  options.outputPath = binary;
  scalanative::support::DiagnosticEngine diagnostics;
  const scalanative::tools::build::BuildResult result =
      driver.buildSource("CompositeTypes.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidCompositeTypes.scala", invalidSource, {}, invalidDiagnostics);
  scalanative::support::DiagnosticEngine malformedDiagnostics;
  const scalanative::tools::build::BuildResult malformed = driver.buildSource(
      "MalformedCompositeTypes.scala", malformedSource, {}, malformedDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("composite-type native build failed: " + result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string text = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  const bool valid =
      status == 0 &&
      text == "union\nunion\nalias-union\ngeneric-union\n"
              "left-common\nright-common\n"
              "intersection\nboth-right\ntagged-right\nboth-label\n"
              "complete-left\ncomplete-right\ncomplete-common\ncomplete-right\n"
              "generic-intersection\n"
              "precedence\nprecedence\ngrouped\ngrouped\nnested\n"
              "left-tagged-common\ntagged-left\n"
              "generic-distributed\n"
              "distributed-target\ndistributed-source\n"
              "object\nright\nright\nright\nright\nright\nbox\n"
              "union\nunion\nright\n"
              "left-common\nright-common\nleft-tagged-common\ntagged-right\n"
              "right-common\nleft-common\n"
              "left-common\nright-common\nleft-common\n"
              "hidden-base\nhidden-base\nvisible-left\nvisible-right\n"
              "hidden-base\nunrelated\n11\ntwelve\nhidden-base\ntrue\n"
              "31\n32\n31\npayload-string\n"
              "51\n52\nint-consumer\nstring-consumer\n"
              "invariant-cell\nleft-common\nright-common\n"
              "7\nseven\nunrelated\n" &&
      !invalid.ok &&
      contains(invalid.diagnosticsText,
               "'transparent' must modify a class, trait, or inline def") &&
      contains(invalid.diagnosticsText, "does not conform to declared type "
                                        "demo.invalidcompositetypes.Left | "
                                        "demo.invalidcompositetypes.Right") &&
      contains(invalid.diagnosticsText, "does not conform to declared type "
                                        "demo.invalidcompositetypes.Left & "
                                        "demo.invalidcompositetypes.Tagged") &&
      contains(invalid.diagnosticsText, "does not conform to parameter type "
                                        "demo.invalidcompositetypes.Left | "
                                        "demo.invalidcompositetypes.Right") &&
      contains(invalid.diagnosticsText, "does not conform to parameter type "
                                        "demo.invalidcompositetypes.Left & "
                                        "demo.invalidcompositetypes.Tagged") &&
      contains(invalid.diagnosticsText, "does not conform to parameter type "
                                        "demo.invalidcompositetypes.Left & "
                                        "demo.invalidcompositetypes.Tagged | "
                                        "demo.invalidcompositetypes.Right & "
                                        "demo.invalidcompositetypes.Tagged") &&
      contains(invalid.diagnosticsText,
               "type argument demo.invalidcompositetypes.LeftValue | "
               "demo.invalidcompositetypes.RightValue for A does not conform to "
               "upper bound demo.invalidcompositetypes.Left") &&
      contains(invalid.diagnosticsText,
               "type demo.invalidcompositetypes.Left | "
               "demo.invalidcompositetypes.Right does not conform to parameter "
               "type demo.invalidcompositetypes.Left") &&
      contains(invalid.diagnosticsText,
               "unresolved member: commonSpelling on union type "
               "demo.invalidcompositetypes.Left | "
               "demo.invalidcompositetypes.Right; union members must come from "
               "a common base type") &&
      contains(invalid.diagnosticsText,
               "unresolved or incompatible member: conflict on intersection type "
               "demo.invalidcompositetypes.LeftConflict & "
               "demo.invalidcompositetypes.RightConflict") &&
      contains(invalid.diagnosticsText,
               "unresolved or incompatible member: accept on intersection type "
               "demo.invalidcompositetypes.Sink [ "
               "demo.invalidcompositetypes.Left ] & "
               "demo.invalidcompositetypes.Sink [ "
               "demo.invalidcompositetypes.Right ]") &&
      contains(invalid.diagnosticsText,
               "override combinedResult return type "
               "demo.invalidcompositetypes.Left does not match inherited "
               "return type demo.invalidcompositetypes.Right") &&
      contains(invalid.diagnosticsText,
               "class InvalidConcreteResult inherits incompatible method "
               "concreteResult") &&
      !malformed.ok &&
      contains(malformed.diagnosticsText,
               "malformed intersection or union type: Left |") &&
      contains(result.nirText,
               "field @demo.compositetypes.Main.unionField$field : Object") &&
      contains(result.nirText,
               "field @demo.compositetypes.Main.intersectionField$field : "
               "Object") &&
      contains(result.nirText,
               "define @demo.compositetypes.Main.unionName : (Object)String") &&
      contains(result.nirText, "define @demo.compositetypes.Main.intersectionName : "
                               "(Object)String") &&
      contains(result.nirText, "let %localUnion : Object = new "
                               "demo.compositetypes.RightValue") &&
      contains(result.nirText, "let %localIntersection : Object = new "
                               "demo.compositetypes.BothValue") &&
      contains(result.nirText,
               "call %rightName(as-instance-of[demo.compositetypes.Right]"
               "(%localIntersection))") &&
      contains(result.nirText, "as-instance-of[demo.compositetypes.CommonName](%value)"
                               ".commonName") &&
      contains(result.nirText,
               "as-instance-of[demo.compositetypes.Right](%value).rightName") &&
      contains(result.nirText,
               "as-instance-of[demo.compositetypes.Tagged](%value).tagName") &&
      contains(result.nirText,
               "as-instance-of[demo.compositetypes.LeftLabel](%value).label") &&
      contains(result.nirText,
               "define @demo.compositetypes.Main.projectedCombinedResult : "
               "(Object)Object") &&
      contains(result.nirText,
               "as-instance-of[demo.compositetypes.LeftResult](%value)"
               ".combinedResult") &&
      contains(result.nirText,
               "define @demo.compositetypes.CombinedResult.combinedResult : "
               "(demo.compositetypes.CombinedResult)Object") &&
      contains(result.nirText,
               "define @demo.compositetypes.Main.projectedStableResult : "
               "(Object)String") &&
      contains(result.nirText,
               "as-instance-of[demo.compositetypes.LeftStableResult](%value)"
               ".stableResult") &&
      contains(result.nirText,
               "define @demo.compositetypes.Main.projectedAppliedResult : "
               "(Object)Object") &&
      contains(result.nirText,
               "define @demo.compositetypes.Main.chooseLeft : ()Object") &&
      contains(result.nirText,
               "define @demo.compositetypes.Main.chooseBoth : ()Object") &&
      contains(result.nirText, "define @demo.compositetypes.Main.inferredCommon : "
                               "(Boolean)demo.compositetypes.CommonName") &&
      contains(result.nirText, "define @demo.compositetypes.Main.inferredUnrelated : "
                               "(Boolean)Object") &&
      contains(result.nirText,
               "define @demo.compositetypes.Main.inferredCommonTagged : "
               "(Boolean)Object") &&
      contains(result.nirText, "define @demo.compositetypes.Main.inferredScalar : "
                               "(Boolean)Object") &&
      contains(result.nirText,
               "define @demo.compositetypes.Main.inferredTransparentClass : "
               "(Boolean)Object") &&
      contains(result.nirText,
               "as-instance-of[demo.compositetypes.HiddenBase]") &&
      contains(result.nirText,
               "define @demo.compositetypes.Main.inferredVisibleWithoutMarker : "
               "(Boolean)demo.compositetypes.VisibleName") &&
      contains(result.nirText,
               "define @demo.compositetypes.Main.inferredGenericTransparent : "
               "()Object") &&
      contains(result.nirText,
               "define @demo.compositetypes.Main.inferredGenericUnrelated : "
               "()Object") &&
      contains(result.nirText,
               "define @demo.compositetypes.Main.inferredGenericScalar : "
               "()Object") &&
      contains(result.nirText,
               "define @demo.compositetypes.Main.inferredGenericBox : "
               "(Boolean)demo.compositetypes.Box") &&
      contains(result.nirText,
               "define @demo.compositetypes.Main.inferredAppliedSame : "
               "(Boolean)demo.compositetypes.Payload") &&
      contains(result.nirText,
               "define @demo.compositetypes.Main.inferredAppliedCovariant : "
               "(Boolean)demo.compositetypes.Payload") &&
      contains(result.nirText,
               "define @demo.compositetypes.Main.inferredAppliedTransitive : "
               "(Boolean)demo.compositetypes.Payload") &&
      contains(result.nirText,
               "define @demo.compositetypes.Main.inferredAppliedContravariant : "
               "(Boolean)demo.compositetypes.Consumer") &&
      contains(result.nirText,
               "define @demo.compositetypes.Main.inferredAppliedInvariant : "
               "(Boolean)Object") &&
      contains(result.nirText,
               "define @demo.compositetypes.Main.inferredAppliedBounded : "
               "(Boolean)demo.compositetypes.BoundedPayload") &&
      contains(result.nirText, "let %inferred : demo.compositetypes.CommonName = "
                               "as-instance-of[demo.compositetypes.CommonName](if(") &&
      contains(result.nirText, "let %explicit : Object = if(") &&
      contains(result.nirText, "as-instance-of[demo.compositetypes.CommonName]"
                               "(call %firstOf(") &&
      contains(result.nirText, "box[Int](7)") && !contains(result.nirText, " | ") &&
      !contains(result.nirText, " & ");
  return valid ? 0
               : fail("composite-type smoke test failed (output='" + text +
                      "', diagnostics='" + result.diagnosticsText +
                      "', invalid-diagnostics='" + invalid.diagnosticsText +
                      "', malformed-diagnostics='" + malformed.diagnosticsText + "')");
}

} // namespace

int runSmokeTests4() {
  return smokeCompositeTypes();
}
