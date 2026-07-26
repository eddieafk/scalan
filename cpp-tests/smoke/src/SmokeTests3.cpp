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

  def ordinal[A](value: A)(using instance: Ordinal[A]): Int =
    instance.ordinal(value)

  def stopped(value: Event.Stopped): Event.Stopped = value

  def contextName[A]()(using named: Named[A]): String = named.name

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
  val captured = capturedLocalFactory
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
              "local-anonymous:seed-int\ninner-local:seed-int\n" &&
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
      contains(result.nirText, "call %generatedName(call %demo.qualifiedcases.$local$");
  return valid ? 0
               : fail("Scala 3 incremental smoke test failed (output='" + text +
                      "', invalid-diagnostics='" + invalid.diagnosticsText + "')");
}

} // namespace

int runSmokeTests3() {
  return smokeQualifiedNestedAndContextualInference();
}
