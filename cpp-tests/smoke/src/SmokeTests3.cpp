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

int smokeQualifiedNestedAndContextualInference() {
  constexpr const char* source = R"(package demo.qualifiedcases

trait Ordinal[A] {
  def ordinal(value: A): Int
}

class DerivedOrdinal[A](val mirror: scala.deriving.Mirror.SumOf[A])
    extends Ordinal[A] {
  override def ordinal(value: A): Int = mirror.ordinal(value)
}

class StringOrdinal extends Ordinal[String] {
  override def ordinal(value: String): Int = 99
}

object Ordinal {
  given stringOrdinal: Ordinal[String] = new StringOrdinal

  def derived[A](using mirror: scala.deriving.Mirror.SumOf[A]): Ordinal[A] =
    new DerivedOrdinal[A](mirror)
}

sealed trait Event derives Ordinal
object Event {
  object Started extends Event
  class Stopped(val code: Int) extends Event
}

sealed trait Maybe[+A] derives Ordinal
object Maybe {
  object Empty extends Maybe[Nothing]
  class Present[+A](val value: A) extends Maybe[A]
}

sealed trait Command derives Ordinal
object Command {
  object Primary {
    object Start extends Command
    class Stop(val code: Int) extends Command
  }
}
object MoreCommands {
  object Pause extends Command
}

sealed trait DeepMaybe[+A] derives Ordinal
object DeepMaybe {
  object Cases {
    object Missing extends DeepMaybe[Nothing]
    class Found[+A](val value: A) extends DeepMaybe[A]
  }
}

trait Named[A] {
  val name: String
}
class NamedValue[A](val name: String) extends Named[A]

trait PairNamed[A, B] {
  val name: String
  val value: B
}
class PairNamedValue[A, B](val name: String, val value: B)
    extends PairNamed[A, B]

trait Seed[A] {
  val label: String
}
class SeedValue[A](val label: String) extends Seed[A]

trait Intermediate[A] {
  val label: String
}
class IntermediateValue[A](val label: String) extends Intermediate[A]

trait Generated[A] {
  val label: String
}
class GeneratedValue[A](val label: String) extends Generated[A]

trait AnonymousGenerated[A] {
  val label: String
}
class AnonymousGeneratedValue[A](val label: String)
    extends AnonymousGenerated[A]

trait ContextBoundGenerated[A] {
  val label: String
}
class ContextBoundGeneratedValue[A](val label: String)
    extends ContextBoundGenerated[A]

object Main {
  given intNamed: Named[Int] = new NamedValue[Int]("context-int")
  given intStringPairNamed: PairNamed[Int, String] =
    new PairNamedValue[Int, String]("context-int-string", "expected-context")
  given intSeed: Seed[Int] = new SeedValue[Int]("seed-int")
  given intermediateFromSeed[A](using seed: Seed[A]): Intermediate[A] =
    new IntermediateValue[A]("intermediate:" + seed.label)
  given generatedFromIntermediate[A](
      using intermediate: Intermediate[A]): Generated[A] =
    new GeneratedValue[A]("generated:" + intermediate.label)
  given [A](using seed: Seed[A]): AnonymousGenerated[A] =
    new AnonymousGeneratedValue[A]("anonymous-member:" + seed.label)
  given contextBoundGenerated[A: Seed as seed]: ContextBoundGenerated[A] =
    new ContextBoundGeneratedValue[A]("named-context-bound-factory:" + seed.label)

  def ordinal[A](value: A)(using instance: Ordinal[A]): Int =
    instance.ordinal(value)

  def stopped(value: Event.Stopped): Event.Stopped = value

  def contextName[A]()(using named: Named[A]): String = named.name

  def seedLabel[A]()(using seed: Seed[A]): String = seed.label

  def forwardedContextName[A]()(using named: Named[A]): String =
    contextName()

  def repeatedContextName[A]()(using first: Named[A], second: Named[A]): String =
    first.name + ":" + second.name

  def mixedContextName[A, B](value: A)(using named: PairNamed[A, B]): String =
    named.name

  def forwardedMixedContextName[A, B](
      value: A)(using named: PairNamed[A, B]): String =
    mixedContextName(value)

  def expectedMixedContextValue[A, B]()(using named: PairNamed[A, B]): B =
    named.value

  def compatibleGivenAfterUnrelatedContext(
      value: Int)(using unrelated: PairNamed[String, Long]): String =
    mixedContextName(value)

  def generatedName[A]()(using generated: Generated[A]): String =
    generated.label

  def forwardedGeneratedName[A]()(using generated: Generated[A]): String =
    generatedName()

  def anonymousGeneratedName[A]()(using generated: AnonymousGenerated[A]): String =
    generated.label

  def inferredContextBoundName[A: Named](value: A): String =
    contextName()

  def combinedContextBounds[A: {Named, Seed}](value: A): String =
    contextName() + ":" + seedLabel()

  def contextBoundAndUsing[A: Named](
      value: A)(using seed: Seed[A]): String =
    contextName() + ":" + seed.label

  def contextBoundGeneratedName[A]()(
      using generated: ContextBoundGenerated[A]): String =
    generated.label

  def namedContextBoundName[A: Named as named](value: A): String =
    named.name

  def namedAggregateContextBounds[
      A: {Named as named, Seed as seed}](value: A): String =
    named.name + ":" + seed.label

  def namedContextBoundAndUsing[A: Named as named](
      value: A)(using seed: Seed[A]): String =
    named.name + ":" + seed.label

  def summonedContextName[A: Named as named](value: A): String =
    summon[Named[A]].name

  def summonedMemberName: String =
    summon[Named[Int]].name

  def summonedGeneratedName: String =
    summon[Generated[Int]].label

  def summonedLocalName: String = {
    given localSummoned: Named[Int] =
      new NamedValue[Int]("summoned-local")
    summon[Named[Int]].name
  }

  def localNamedGeneratedName: String = {
    given localIntermediate[A](using seed: Seed[A]): Intermediate[A] =
      new IntermediateValue[A]("local-intermediate:" + seed.label)
    given localGenerated[A](
        using intermediate: Intermediate[A]): Generated[A] =
      new GeneratedValue[A]("local-generated:" + intermediate.label)
    generatedName()
  }

  def localAnonymousGeneratedName: String = {
    given [A](using seed: Seed[A]): Generated[A] =
      new GeneratedValue[A]("local-anonymous:" + seed.label)
    generatedName()
  }

  def nestedLocalGeneratedName: String = {
    given outer[A](using seed: Seed[A]): Generated[A] =
      new GeneratedValue[A]("outer-local:" + seed.label)
    {
      given [A](using seed: Seed[A]): Generated[A] =
        new GeneratedValue[A]("inner-local:" + seed.label)
      generatedName()
    }
  }

  def localContextBoundGeneratedName: String = {
    given localContextBound[A: Seed as seed]: ContextBoundGenerated[A] =
      new ContextBoundGeneratedValue[A](
        "local-named-context-bound:" + seed.label)
    contextBoundGeneratedName[Int]()
  }

  def main = {
    println(ordinal[Event](Event.Started))
    println(ordinal[Event](new Event.Stopped(2)))
    println(ordinal[Maybe[String]](Maybe.Empty))
    println(ordinal[Maybe[String]](new Maybe.Present[String]("value")))
    println(stopped(new Event.Stopped(7)).code)
    println(ordinal[Command](Command.Primary.Start))
    println(ordinal[Command](new Command.Primary.Stop(9)))
    println(ordinal[Command](MoreCommands.Pause))
    println(ordinal[DeepMaybe[String]](DeepMaybe.Cases.Missing))
    println(
      ordinal[DeepMaybe[String]](
        new DeepMaybe.Cases.Found[String]("nested")))
    println(new Command.Primary.Stop(9).code)
    println(contextName())
    println(forwardedContextName[Int]())
    println(repeatedContextName())
    println(mixedContextName(1))
    println(forwardedMixedContextName[Int, String](2))
    val expectedMixed: String = expectedMixedContextValue()
    println(expectedMixed)
    println(
      compatibleGivenAfterUnrelatedContext(3)(
        using new PairNamedValue[String, Long]("unrelated-context", 4L)))
    println(generatedName())
    println(forwardedGeneratedName[Int]())
    println(anonymousGeneratedName())
    println(localNamedGeneratedName)
    println(localAnonymousGeneratedName)
    println(nestedLocalGeneratedName)
    println(inferredContextBoundName(1))
    println(combinedContextBounds(2))
    println(contextBoundAndUsing(3))
    println(contextBoundAndUsing(4)(using intNamed, intSeed))
    println(contextBoundGeneratedName[Int]())
    println(localContextBoundGeneratedName)
    println(namedContextBoundName(5))
    println(namedAggregateContextBounds(6))
    println(namedContextBoundAndUsing(7))
    println(summonedContextName(8))
    println(summonedMemberName)
    println(summonedGeneratedName)
    println(summonedLocalName)
  }
}
)";
  constexpr const char* invalidSource = R"(package demo.invalidqualifiedcase

sealed trait Event
object Event {
  object Started extends Event
}

trait Named[A]
class NamedValue[A] extends Named[A]
trait PairNamed[A, B]
class PairNamedValue[A, B] extends PairNamed[A, B]
trait Seed[A]
class SeedValue[A] extends Seed[A]
trait Generated[A] {
  val label: String
}
class GeneratedValue[A](val label: String) extends Generated[A]
trait LoopEvidence[A]

object Main {
  given intNamed: Named[Int] = new NamedValue[Int]
  given stringNamed: Named[String] = new NamedValue[String]
  given firstLongNamed: Named[Long] = new NamedValue[Long]
  given secondLongNamed: Named[Long] = new NamedValue[Long]
  given intStringPairNamed: PairNamed[Int, String] =
    new PairNamedValue[Int, String]
  given intLongPairNamed: PairNamed[Int, Long] =
    new PairNamedValue[Int, Long]
  given intSeed: Seed[Int] = new SeedValue[Int]
  given stringSeed: Seed[String] = new SeedValue[String]
  given generated[A](using seed: Seed[A]): Generated[A] =
    new GeneratedValue[A]("generated")
  given loop[A](using next: LoopEvidence[A]): LoopEvidence[A] = next

  def choose[A]()(using named: Named[A]): Named[A] = named
  def mixed[A, B](value: A)(using named: PairNamed[A, B]): PairNamed[A, B] =
    named
  def conflicting[A, B](
      left: A, right: A)(using named: PairNamed[A, B]): PairNamed[A, B] =
    named
  def chooseGenerated[A]()(using generated: Generated[A]): Generated[A] =
    generated
  def chooseLoop[A]()(using loop: LoopEvidence[A]): LoopEvidence[A] =
    loop
  def missingContextBound[A: Named](value: A): Named[A] =
    choose[A]()
  def capturedLocalFactory: Generated[Int] = {
    val prefix: String = "captured"
    given captured[A](using seed: Seed[A]): Generated[A] =
      new GeneratedValue[A](prefix)
    chooseGenerated[Int]()
  }

  def value = new Event.Started()
  val ambiguous = choose()
  val mixedAmbiguous = mixed(1)
  val valueConflict = conflicting(1, "no")
  val generatedAmbiguous = chooseGenerated()
  val unresolvedLoop = chooseLoop()
  val missingBound = missingContextBound(true)
  val missingSummon = summon[Named[Boolean]]
  val ambiguousSummon = summon[Named[Long]]
  val divergingSummon = summon[LoopEvidence[Int]]
  val malformedSummon = summon[Named[Int], Seed[Int]]
  val captured = capturedLocalFactory
}
)";
  constexpr const char* invalidContextBoundSource =
      R"(package demo.invalidcontextbounds

trait Named[A]
trait Seed[A]
class UnsupportedContextBound[A: Named]

object InvalidContextBounds {
  def duplicate[A: {Named as evidence, Seed as evidence}](value: A): A = value
  def sourceCollision[A: Named as evidence](evidence: A): A = evidence
  def dependent[A: Named as named](value: named.Member): A
  def localDuplicate: Int = {
    given local[
        A: {Named as localEvidence, Seed as localEvidence}]: Named[A] =
      localEvidence
    1
  }
}
)";
  constexpr const char* invalidContextBoundSyntaxSource =
      R"(package demo.invalidcontextboundsyntax

trait Named[A]

object Main {
  def missingName[A: Named as](value: A): A = value
  def empty[A: {}](value: A): A = value
  def trailing[A: {Named,}](value: A): A = value
}
)";

  const std::filesystem::path temporary = std::filesystem::temp_directory_path();
  const std::filesystem::path binary =
      temporary / "cpp-scalanative-smoke-scala3-incremental";
  const std::filesystem::path output =
      temporary / "cpp-scalanative-smoke-scala3-incremental.out";
  std::error_code ignored;
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  scalanative::tools::build::BuildDriver driver;
  scalanative::tools::build::BuildOptions options;
  options.action = scalanative::tools::build::BuildAction::BuildBinary;
  options.outputPath = binary;
  scalanative::support::DiagnosticEngine diagnostics;
  const scalanative::tools::build::BuildResult result =
      driver.buildSource("Scala3Incremental.scala", source, options, diagnostics);

  scalanative::support::DiagnosticEngine invalidDiagnostics;
  const scalanative::tools::build::BuildResult invalid = driver.buildSource(
      "InvalidScala3Incremental.scala", invalidSource, {}, invalidDiagnostics);
  scalanative::support::DiagnosticEngine invalidContextBoundDiagnostics;
  const scalanative::tools::build::BuildResult invalidContextBound =
      driver.buildSource("InvalidContextBounds.scala", invalidContextBoundSource, {},
                         invalidContextBoundDiagnostics);
  scalanative::support::DiagnosticEngine invalidContextBoundSyntaxDiagnostics;
  const scalanative::tools::build::BuildResult invalidContextBoundSyntax =
      driver.buildSource("InvalidContextBoundSyntax.scala",
                         invalidContextBoundSyntaxSource, {},
                         invalidContextBoundSyntaxDiagnostics);

  if (!result.ok) {
    if (contains(result.diagnosticsText, "clang toolchain not found")) {
      return 0;
    }
    return fail("Scala 3 incremental native build failed: " + result.diagnosticsText);
  }

  const std::string command = binary.string() + " > " + output.string();
  const int status = std::system(command.c_str());
  const std::string text = readTextFile(output);
  std::filesystem::remove(binary, ignored);
  std::filesystem::remove(output, ignored);

  const bool valid =
      status == 0 &&
      text == "0\n1\n0\n1\n7\n0\n1\n2\n0\n1\n9\ncontext-int\n"
              "context-int\ncontext-int:context-int\ncontext-int-string\n"
              "context-int-string\nexpected-context\ncontext-int-string\n"
              "generated:intermediate:seed-int\n"
              "generated:intermediate:seed-int\nanonymous-member:seed-int\n"
              "local-generated:local-intermediate:seed-int\n"
              "local-anonymous:seed-int\ninner-local:seed-int\n"
              "context-int\ncontext-int:seed-int\ncontext-int:seed-int\n"
              "context-int:seed-int\n"
              "named-context-bound-factory:seed-int\n"
              "local-named-context-bound:seed-int\n"
              "context-int\ncontext-int:seed-int\ncontext-int:seed-int\n"
              "context-int\ncontext-int\n"
              "generated:intermediate:seed-int\nsummoned-local\n" &&
      !invalid.ok &&
      contains(invalid.diagnosticsText,
               "constructor target is not a class: Event.Started") &&
      contains(invalid.diagnosticsText,
               "ambiguous contextual type inference for choose; "
               "use explicit type arguments") &&
      contains(invalid.diagnosticsText,
               "ambiguous contextual type inference for mixed; "
               "use explicit type arguments") &&
      contains(invalid.diagnosticsText,
               "conflicting inferred types Int and String for type parameter A "
               "of conflicting") &&
      !contains(invalid.diagnosticsText,
                "ambiguous contextual type inference for conflicting") &&
      contains(invalid.diagnosticsText,
               "ambiguous contextual type inference for chooseGenerated; "
               "use explicit type arguments") &&
      contains(invalid.diagnosticsText,
               "cannot infer type argument A for chooseLoop from value arguments; "
               "use explicit type arguments") &&
      contains(invalid.diagnosticsText,
               "capturing local parameterized given references local value "
               "prefix") &&
      !invalidContextBound.ok &&
      contains(invalidContextBound.diagnosticsText,
               "context bounds are currently supported only on methods") &&
      contains(invalidContextBound.diagnosticsText, "duplicate parameter: evidence") &&
      contains(invalidContextBound.diagnosticsText,
               "duplicate parameter: localEvidence") &&
      contains(invalidContextBound.diagnosticsText,
               "named context bounds referenced by preceding parameter types are "
               "not supported yet") &&
      !invalidContextBoundSyntax.ok &&
      contains(invalidContextBoundSyntax.diagnosticsText,
               "expected context-bound witness name after 'as'") &&
      contains(invalidContextBoundSyntax.diagnosticsText,
               "expected context-bound type") &&
      contains(invalidContextBoundSyntax.diagnosticsText,
               "expected context-bound type after ','") &&
      contains(invalid.diagnosticsText,
               "demo.invalidqualifiedcase.Named [ Boolean ] required by "
               "missingContextBound") &&
      contains(invalid.diagnosticsText,
               "demo.invalidqualifiedcase.Named [ Boolean ] required by summon") &&
      contains(invalid.diagnosticsText,
               "ambiguous given values for context parameter evidence of type "
               "demo.invalidqualifiedcase.Named [ Long ] required by summon: "
               "firstLongNamed, secondLongNamed") &&
      contains(invalid.diagnosticsText,
               "diverging given expansion for type "
               "demo.invalidqualifiedcase.LoopEvidence [ Int ] via loop") &&
      contains(invalid.diagnosticsText, "summon requires exactly one type argument") &&
      contains(result.nirText, "module @demo.qualifiedcases.Event$.Started : "
                               "@demo.qualifiedcases.Event") &&
      contains(result.nirText, "class @demo.qualifiedcases.Event$.Stopped : "
                               "@demo.qualifiedcases.Event") &&
      contains(result.nirText, "module @demo.qualifiedcases.Maybe$.Empty : "
                               "@demo.qualifiedcases.Maybe") &&
      contains(result.nirText, "class @demo.qualifiedcases.Maybe$.Present : "
                               "@demo.qualifiedcases.Maybe") &&
      contains(result.nirText, "module @demo.qualifiedcases.Command$.Primary.Start : "
                               "@demo.qualifiedcases.Command") &&
      contains(result.nirText, "class @demo.qualifiedcases.Command$.Primary.Stop : "
                               "@demo.qualifiedcases.Command") &&
      contains(result.nirText, "module @demo.qualifiedcases.MoreCommands.Pause : "
                               "@demo.qualifiedcases.Command") &&
      contains(result.nirText,
               "$mirror$Sum$demo$qualifiedcases$Command.MirroredElemTypes : "
               "scala.Tuple3 [ demo.qualifiedcases.Command$.Primary.Start, "
               "demo.qualifiedcases.Command$.Primary.Stop, "
               "demo.qualifiedcases.MoreCommands.Pause ]") &&
      contains(result.nirText,
               "$mirror$Sum$demo$qualifiedcases$DeepMaybe.MirroredElemTypes : "
               "scala.Tuple2 [ demo.qualifiedcases.DeepMaybe$.Cases.Missing, "
               "demo.qualifiedcases.DeepMaybe$.Cases.Found [ A ] ]") &&
      contains(result.nirText, "new demo.qualifiedcases.Event$.Stopped") &&
      contains(result.nirText, "new demo.qualifiedcases.Maybe$.Present") &&
      contains(result.nirText, "new demo.qualifiedcases.Command$.Primary.Stop") &&
      contains(result.nirText, "new demo.qualifiedcases.DeepMaybe$.Cases.Found") &&
      contains(result.nirText, "define @demo.qualifiedcases.Main.contextName : "
                               "(demo.qualifiedcases.Named)String") &&
      contains(result.nirText, "ret String call %contextName(%named)") &&
      contains(result.nirText, "call %contextName(call "
                               "%demo.qualifiedcases.Main.intNamed())") &&
      contains(result.nirText, "define @demo.qualifiedcases.Main.mixedContextName : "
                               "(Object,demo.qualifiedcases.PairNamed)String") &&
      contains(result.nirText, "ret String call %mixedContextName(%value, %named)") &&
      contains(result.nirText, "call %mixedContextName(box[Int](1), call "
                               "%demo.qualifiedcases.Main.intStringPairNamed())") &&
      contains(result.nirText,
               "define @demo.qualifiedcases.Main.generatedFromIntermediate : "
               "(demo.qualifiedcases.Intermediate)"
               "demo.qualifiedcases.Generated") &&
      contains(result.nirText,
               "call %generatedName(call "
               "%demo.qualifiedcases.Main.generatedFromIntermediate(call "
               "%demo.qualifiedcases.Main.intermediateFromSeed(call "
               "%demo.qualifiedcases.Main.intSeed())))") &&
      contains(result.nirText, "define @demo.qualifiedcases.Main.given$") &&
      contains(result.nirText, "call %anonymousGeneratedName(call "
                               "%demo.qualifiedcases.Main.given$") &&
      contains(result.nirText, "define @demo.qualifiedcases.$local$") &&
      contains(result.nirText, ".localIntermediate : (demo.qualifiedcases.Seed)"
                               "demo.qualifiedcases.Intermediate") &&
      contains(result.nirText, ".localGenerated : (demo.qualifiedcases.Intermediate)"
                               "demo.qualifiedcases.Generated") &&
      contains(result.nirText,
               "call %generatedName(call %demo.qualifiedcases.$local$") &&
      contains(result.nirText,
               "define @demo.qualifiedcases.Main.inferredContextBoundName : "
               "(Object,demo.qualifiedcases.Named)String") &&
      contains(result.nirText,
               "define @demo.qualifiedcases.Main.combinedContextBounds : "
               "(Object,demo.qualifiedcases.Named,demo.qualifiedcases.Seed)String") &&
      contains(result.nirText,
               "define @demo.qualifiedcases.Main.contextBoundAndUsing : "
               "(Object,demo.qualifiedcases.Named,demo.qualifiedcases.Seed)String") &&
      contains(result.nirText,
               "define @demo.qualifiedcases.Main.contextBoundGenerated : "
               "(demo.qualifiedcases.Seed)"
               "demo.qualifiedcases.ContextBoundGenerated") &&
      contains(result.nirText, "call %contextBoundGeneratedName(call "
                               "%demo.qualifiedcases.Main.contextBoundGenerated(call "
                               "%demo.qualifiedcases.Main.intSeed()))") &&
      contains(result.nirText, ".localContextBound : (demo.qualifiedcases.Seed)"
                               "demo.qualifiedcases.ContextBoundGenerated") &&
      contains(result.nirText,
               "define @demo.qualifiedcases.Main.namedContextBoundName : "
               "(Object,demo.qualifiedcases.Named)String") &&
      contains(result.nirText, "ret String %named.name") &&
      contains(result.nirText,
               "define @demo.qualifiedcases.Main.namedAggregateContextBounds : "
               "(Object,demo.qualifiedcases.Named,demo.qualifiedcases.Seed)String") &&
      contains(result.nirText,
               "define @demo.qualifiedcases.Main.namedContextBoundAndUsing : "
               "(Object,demo.qualifiedcases.Named,demo.qualifiedcases.Seed)String") &&
      contains(result.nirText, "define @demo.qualifiedcases.Main.summonedContextName : "
                               "(Object,demo.qualifiedcases.Named)String") &&
      contains(result.nirText, "define @demo.qualifiedcases.Main.summonedMemberName : "
                               "()String") &&
      contains(result.nirText,
               "define @demo.qualifiedcases.Main.summonedGeneratedName : "
               "()String") &&
      contains(result.nirText, "call "
                               "%demo.qualifiedcases.Main.generatedFromIntermediate("
                               "call %demo.qualifiedcases.Main.intermediateFromSeed("
                               "call %demo.qualifiedcases.Main.intSeed()))") &&
      contains(result.nirText,
               "define @demo.qualifiedcases.Main.summonedLocalName : ()String") &&
      contains(result.nirText, "%localSummoned.name");
  return valid ? 0
               : fail("Scala 3 incremental smoke test failed (output='" + text +
                      "', invalid-diagnostics='" + invalid.diagnosticsText +
                      "', invalid-context-bound-diagnostics='" +
                      invalidContextBound.diagnosticsText +
                      "', invalid-context-bound-syntax-diagnostics='" +
                      invalidContextBoundSyntax.diagnosticsText + "')");
}

} // namespace

int runSmokeTests3() {
  return smokeQualifiedNestedAndContextualInference();
}
