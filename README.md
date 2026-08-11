# cpp-scalanative

This repository is starting a C++ bootstrap implementation of a Scala Native
compiler.

The intended pipeline is:

```text
Source -> Lexer -> Parser -> AST -> Typecheck -> NIR -> LLVM IR -> native binary
```

The initial project uses `cpp-` prefixed modules:

- `cpp-support`: source management, diagnostics, arenas, IDs, and GC handle stubs.
- `cpp-frontend`: source, lexer, parser, AST, and typecheck scaffolding.
- `cpp-nir`: C++ NIR model, builder, text writer, and verifier.
- `cpp-nscplugin`: typed-AST-to-NIR lowering boundary.
- `cpp-tools/{build,checker,codegen,interflow,linker}`: Scala Native-inspired
  tool phases.
- `cpp-runtime`: runtime ABI and hybrid GC/arena configuration scaffolding.
- `cpp-driver`: `cpp-scalanative` command-line entry point.
- `cpp-tests`: smoke tests.

## Build

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Convenience wrappers are also available:

```sh
make quick
make check
```

The active smoke suite keeps new regressions in numbered translation units and
compiles only the harness at `-O0` without debug symbols; the compiler libraries
still use the selected preset normally. During work on the newest coverage, use:

```sh
cmake --build build/debug --target cpp-smoke-tests -j2
CPP_SCALANATIVE_SMOKE_TESTS7_ONLY=1 \
  build/debug/cpp-tests/smoke/cpp-smoke-tests
```

Try the scaffold compiler:

```sh
build/debug/cpp-driver/cpp-scalanative --emit-llvm path/to/Main.scala

# Emit compact LLVM IR without source debug metadata.
build/debug/cpp-driver/cpp-scalanative --emit-llvm --no-debug-info \
  path/to/Main.scala

# Run Interflow and request aggressive native optimization.
build/debug/cpp-driver/cpp-scalanative --build-binary --opt-level 3 \
  path/to/Main.scala

# Compile the explicit reference-generics example.
build/debug/cpp-driver/cpp-scalanative --build-binary --optimize \
  --output /tmp/reference-generics cpp-examples/ReferenceGenerics.scala

# Compile the boxed primitive-generics example.
build/debug/cpp-driver/cpp-scalanative --build-binary --optimize \
  --output /tmp/primitive-generics cpp-examples/PrimitiveGenerics.scala

# Compile the argument-driven generic-inference example.
build/debug/cpp-driver/cpp-scalanative --build-binary --optimize \
  --output /tmp/generic-inference cpp-examples/GenericInference.scala

# Compile the expected-result generic-inference example.
build/debug/cpp-driver/cpp-scalanative --build-binary --optimize \
  --output /tmp/expected-generic-inference \
  cpp-examples/ExpectedGenericInference.scala

# Compile declaration-site variance and applied generic inheritance.
build/debug/cpp-driver/cpp-scalanative --build-binary --optimize \
  --output /tmp/variance-inheritance \
  cpp-examples/VarianceAndInheritance.scala

# Compile ranked/recursive contextual evidence, bounded, union/intersection,
# and class/trait/type-alias infix Scala 3 `given` imports, interoperable
# `given`/`using` and legacy `implicit` syntax, and generic `derives` clauses.
build/debug/cpp-driver/cpp-scalanative --build-binary --optimize \
  --output /tmp/contextual-abstractions \
  cpp-examples/ContextualAbstractions.scala

# Compile generic and non-generic `inline def` call-site specialization with
# explicit or inferred type arguments, curried ordinary clauses, contextual
# `using` parameters, single-evaluation class/trait receiver expressions,
# per-call `summonFrom` reduction, and `transparent inline def` result
# refinement. General and contextual `inline` parameters use substitution
# semantics; compile-time Boolean, integer, and floating-point arguments reduce
# ordinary or mandatory `inline if` branches, including bounded recursive inline
# expansion. Braced and Scala 3 indentation-based `inline match` expressions
# reduce ordered Boolean, integer, Float, Double, String, Char, and null literal
# cases, literal alternatives, singleton object patterns and alternatives, and
# a final wildcard or binding at the call site, including escaped literals and
# stable inline selectors. Indented sibling cases must align; nested matches
# close when their case indentation returns to the enclosing level.
# Singleton cases compare exact call-site module identity, including qualified
# nested objects. Null cases distinguish proven null values from literals,
# constructed values, module singletons, and scalar values while leaving dynamic
# reference-producing expressions unresolved.
# Compile-time guards use ordered short-circuit reduction, so
# a guard belonging to a nonmatching case need not itself be constant. Guards
# can reference typed or untyped selector bindings when those bindings carry a
# reducible call-site value, including constants passed through a widened `Any`
# parameter and refined by the selected type pattern.
# Type patterns and type-pattern alternatives reduce from call-site static
# types, including boxed scalar tests and transparent-inline result refinement.
# Alternative arms may remain wildcards or consistently bind the same name; a
# shared binding receives the explicit union of the arm types and is available
# in guards and the selected body.
# Compiler-owned `scala.compiletime.erasedValue[T]`, through its canonical
# import (including aliases) or qualified name, drives generic type-only inline
# matches without materializing a selector in expanded caller NIR.
# Compiler-owned `scala.compiletime.constValue[T]` materializes Boolean, Int,
# Long, Float, Double, String, and Char singleton types during compilation.
# Generic inline definitions defer the intrinsic until specialization, so
# constant inline matches and transparent result refinement emit only the
# selected result.
# Compiler-owned `scala.compiletime.constValueOpt[T]` returns a typed
# `Some[T]` containing the materialized singleton value, or `None` when `T`
# is not a constant type. Canonical, renamed, qualified, and specialized-inline
# calls lower without a runtime intrinsic; the current compiler-owned
# `Option`/`Some`/`None` shape supplies `Some.value` and identity with `None`
# while broader collection-library operations remain a later library milestone.
# Compiler-owned `scala.compiletime.constValueTuple[T]` accepts parenthesized
# tuple types whose elements are constant singleton types, plus `EmptyTuple`.
# It materializes demand-driven covariant `TupleN` runtime classes with boxed
# constructor storage and statically typed `_1` through `_N` projections.
# Canonical, renamed, qualified, type-aliased, and specialized-inline calls
# leave no runtime intrinsic; general tuple algorithms and `*:` recursion remain
# later tuple-library milestones.
# Compiler-owned `scala.compiletime.summonAll[T]` accepts a tuple of evidence
# types and performs one contextual search per element. Canonical, renamed,
# qualified, type-aliased, and specialized-inline calls materialize the selected
# givens into `TupleN`, including primitive boxing and companion-scope evidence;
# `EmptyTuple` needs no evidence. Missing, ambiguous, non-tuple, malformed, and
# unresolved non-inline requests receive compile-time diagnostics.
# Compiler-owned `scala.compiletime.requireConst(value)` accepts Boolean, Byte,
# Short, Int, Long, Float, Double, Char, and String expressions and verifies
# them after inline substitution and constant folding. Canonical, renamed, and
# qualified calls return `Unit` during typechecking and are erased with their
# arguments from successful caller NIR; dynamic values receive a compile-time
# diagnostic.
# Compiler-owned `scala.compiletime.codeOf(expression)` captures a canonical
# source representation without evaluating the expression. Direct, renamed,
# qualified, specialized-inline, and nested-inline calls become String literals
# in caller NIR, and those literals can contribute to `compiletime.error`
# messages.
# Compiler-owned `scala.compiletime.uninitialized` is accepted only as the
# complete initializer of an explicitly typed mutable class or object field.
# Canonical, renamed, and qualified spellings emit that field type's zero value
# (`null`, numeric zero, or `false`) directly in initializer NIR and leave no
# runtime intrinsic behind.
# Compiler-owned `scala.compiletime.error(message)` supports literal, aliased,
# and concatenated constant String messages. Calls inside inline definitions
# are deferred to specialization and report only when the selected expansion
# retains the error branch; successful callers contain no residual intrinsic.
# Compiler-owned `scala.compiletime.summonInline[T]` delays contextual search
# until specialization, so only evidence required by a surviving inline branch
# is resolved. Canonical, aliased, and qualified calls materialize the selected
# given directly in caller NIR and report ordinary missing/ambiguous diagnostics.
# Compiler-owned `summonFrom` accepts both braced cases and Scala 3
# `summonFrom:` indentation-based case regions. Indented sibling cases must
# align, and nested regions close when their case indentation returns to the
# enclosing level; selected evidence and fallbacks retain the same per-call
# specialization and dead-branch erasure as the braced form. Canonical and
# renamed `scala.compiletime.summonFrom` imports and the fully qualified
# spelling all resolve to the same compiler-owned construct.
# Stable top-level and object `inline val` constants, including
# dependency-ordered aliases of earlier inline values, are substituted directly
# and can drive branches.
build/debug/cpp-driver/cpp-scalanative --build-binary --optimize \
  --output /tmp/inline-summon-from \
  cpp-examples/InlineSummonFrom.scala

build/debug/cpp-driver/cpp-scalanative --build-binary --optimize \
  --output /tmp/inline-erased-value \
  cpp-examples/InlineErasedValue.scala

build/debug/cpp-driver/cpp-scalanative --build-binary --optimize \
  --output /tmp/compiletime-constants \
  cpp-examples/CompiletimeConstants.scala

build/debug/cpp-driver/cpp-scalanative --build-binary --optimize \
  --output /tmp/compiletime-const-value-opt \
  cpp-examples/CompiletimeConstValueOpt.scala

build/debug/cpp-driver/cpp-scalanative --build-binary --optimize \
  --output /tmp/compiletime-const-value-tuple \
  cpp-examples/CompiletimeConstValueTuple.scala

build/debug/cpp-driver/cpp-scalanative --build-binary --optimize \
  --output /tmp/compiletime-summon-all \
  cpp-examples/CompiletimeSummonAll.scala

build/debug/cpp-driver/cpp-scalanative --build-binary --optimize \
  --output /tmp/compiletime-uninitialized \
  cpp-examples/CompiletimeUninitialized.scala

build/debug/cpp-driver/cpp-scalanative --build-binary --optimize \
  --output /tmp/compiletime-summon-inline \
  cpp-examples/CompiletimeSummonInline.scala

# Compile explicit Scala 3 union/intersection types across parameters, results,
# locals, nested generic arguments, generic type aliases, inferred branch
# unions with parameterized visible-join widening or retained unconstrained
# generic alternatives (including user-declared transparent classes/traits),
# members projected from intersection constituents (including synthesized
# intersection result types), or a union's shared base-type join.
build/debug/cpp-driver/cpp-scalanative --build-binary --optimize \
  --output /tmp/composite-types \
  cpp-examples/CompositeTypes.scala

# Compile top-level and stable-object-nested product `Mirror` derivation.
build/debug/cpp-driver/cpp-scalanative --build-binary --optimize \
  --output /tmp/product-mirror-derivation \
  cpp-examples/ProductMirrorDerivation.scala

# Compile source-ordered sums, including a deriving sum nested in a stable object.
build/debug/cpp-driver/cpp-scalanative --build-binary --optimize \
  --output /tmp/sum-mirror-derivation \
  cpp-examples/SumMirrorDerivation.scala
```

Optimization levels select distinct Interflow pipelines: `O1` performs one
cleanup cycle, `O2` uses the standard two-cycle pipeline, and `O3` adds an
aggressive convergence cycle. Native compilation receives the matching Clang
`-O` level, and `--optimize` remains an alias for `--opt-level 2`.

Reuse validated NIR or generated LLVM IR across builds with `--cache-dir`:

```sh
build/debug/cpp-driver/cpp-scalanative --emit-llvm --opt-level 3 \
  --cache-dir /tmp/cpp-scalanative-cache --output /tmp/Main.ll \
  path/to/Main.scala
```

Native builds also cache Clang object output and final executables. Before a
binary is reused, Clang's complete implicit link plan is normalized and its
linker, startup objects, runtime libraries, loader, flags, and explicit inputs
are fingerprinted. Cache and link decisions are shown in the phase log.

Write a machine-readable summary alongside the normal CLI output with
`--build-report`:

```sh
build/debug/cpp-driver/cpp-scalanative --build-binary \
  --cache-dir /tmp/cpp-scalanative-cache --output /tmp/Main \
  --build-report /tmp/Main.build.json path/to/Main.scala
```

The versioned JSON report records the action and effective configuration,
frontend/object/binary cache keys and hits, structured diagnostics with source
ranges and fix-its, phase logs, and produced artifacts. Reports are also written
for failed builds, making the option suitable for editors and build systems.

Store repeatable project settings in a versioned JSON configuration:

```json
{
  "schemaVersion": 1,
  "source": "src/Main.scala",
  "action": "build-binary",
  "output": "build/Main",
  "optimizationLevel": 2,
  "debugInfo": true,
  "cacheDirectory": ".cpp-scalanative-cache",
  "linkMode": "default",
  "linker": "lld",
  "runtimeLibraries": [],
  "linkLibraries": ["m"],
  "buildReport": "build/Main.build.json"
}
```

```sh
build/debug/cpp-driver/cpp-scalanative --config cpp-scalanative.json
```

The optional `target`, `sysroot`, `gc`, and `optimizationReport` keys map to the
matching CLI options. Relative source, output, cache, sysroot, report, and direct
library paths are resolved from the configuration file's directory. Explicit
CLI scalar options override configured values regardless of argument order;
CLI `--runtime-lib` and `--link-lib` entries append to configured lists. Unknown
keys, duplicate keys, invalid types, and unsupported schema versions are errors.

Select LLVM's LLD explicitly with `--linker lld`:

```sh
build/debug/cpp-driver/cpp-scalanative --build-binary --linker lld \
  --output /tmp/Main path/to/Main.scala
```

The driver discovers LLD on `PATH`; `CPP_SCALANATIVE_LLD` can name an explicit
executable. Linker selection changes only the final executable cache identity,
so compatible LLVM and native object entries remain reusable.

Cross-target native builds can select both a target triple and an installed
target filesystem root:

```sh
build/debug/cpp-driver/cpp-scalanative --build-binary \
  --target aarch64-unknown-linux-gnu --sysroot /opt/aarch64-sysroot \
  --linker lld --output /tmp/Main-aarch64 path/to/Main.scala
```

The sysroot must already contain the target's headers, libraries, startup
objects, and loader. It is passed to native compilation, library discovery, and
link planning. Frontend LLVM cache entries remain reusable across sysroots;
native object and executable cache identities include the normalized sysroot.

Configured target, sysroot, static, and LLD builds run a native capability
preflight before compiling source. The driver asks Clang to normalize the target,
builds a dry-run linker plan, resolves its linker, startup objects, target loader,
and libraries, and checks GNU-style linker target emulation. Unsupported triples,
incompatible linkers, and incomplete target filesystems therefore fail with a
focused diagnostic instead of surfacing after code generation.

Request fully static native linkage with `--static` or
`--link-mode static`:

```sh
build/debug/cpp-driver/cpp-scalanative --build-binary --static \
  --output /tmp/Main path/to/Main.scala
```

Default linkage remains the portable default. Static mode resolves the concrete
archives through Clang before linking and reports unavailable archives directly;
fully static libc and platform archives are not installed by every target
toolchain.

On Fedora, install the glibc static archives with:

```sh
sudo dnf install glibc-static
```

Try the current interflow optimizer coverage example:

```sh
make interflow-example

build/debug/cpp-driver/cpp-scalanative --emit-nir --optimize \
  --optimization-report /tmp/interflow-report.json \
  cpp-examples/InterflowOptimizations.scala

build/debug/cpp-driver/cpp-scalanative --emit-llvm --optimize \
  --optimization-report /tmp/interflow-report.json \
  cpp-examples/InterflowOptimizations.scala
```

This first version validates the module shape and phase wiring. It emits minimal
LLVM IR while the real Scala parser, typechecker, NIR lowering, optimizer,
runtime, and native linker are built out.
