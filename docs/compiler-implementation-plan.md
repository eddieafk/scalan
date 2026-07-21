# C++ Scala Native Compiler Implementation Plan

This document is the starting plan for a new Scala Native compiler written in
C++. The intended pipeline is:

```text
Source -> Lexer -> Parser -> AST -> Typecheck -> NIR -> LLVM IR -> native binary
```

The design borrows the major boundaries of upstream Scala Native, especially
`nscplugin`, `nir`, and `tools/{build,checker,codegen,interflow,linker}`, while
keeping this compiler as a new C++ implementation.

## Reference Points

- Upstream repository: https://github.com/scala-native/scala-native
- Scala Native compiler overview: https://scala-native.org/en/v0.4.17/contrib/compiler.html
- Current NIR documentation: https://www.scala-native.org/en/latest/contrib/nir.html
- Current build/toolchain documentation: https://www.scala-native.org/en/stable/contrib/build.html

Useful upstream facts to preserve:

- `nscplugin` is the Scala-to-NIR producer in upstream Scala Native.
- NIR is a high-level, object-oriented, strongly typed SSA representation with
  global definitions, basic blocks, block parameters, and high-level class,
  trait, and module definitions.
- The tools layer owns build orchestration, checking, linking, interprocedural
  optimization, LLVM code generation, and final native linking.

## Corrected Active Queue

The current scaffold has a small Phase 11 codegen prototype because it is useful
for end-to-end smoke testing, but that does not mean Phases 7-10 are complete.
The implementation queue should resume in phase order:

1. Phase 7: strengthen `cpp-nscplugin` typed-AST-to-NIR lowering.
2. Phase 8: strengthen `cpp-tools/checker` and NIR verification.
3. Phase 9: implement `cpp-tools/linker` global indexing and reachability.
4. Phase 10: add `cpp-tools/interflow` pass manager and first checked passes.
5. Phase 11: return to LLVM codegen after the linked NIR surface is reliable.

Phase 11 work should stay limited to smoke-test scaffolding until the Phase 7-10
queue items above are in place.

## Proposed Repository Layout

```text
cpp-nscplugin/
  include/scalanative/nscplugin/
  src/
  tests/

cpp-nir/
  include/scalanative/nir/
  src/
  tests/

cpp-tools/
  build/
    include/scalanative/tools/build/
    src/
    tests/
  checker/
    include/scalanative/tools/checker/
    src/
    tests/
  codegen/
    include/scalanative/tools/codegen/
    src/
    tests/
  interflow/
    include/scalanative/tools/interflow/
    src/
    tests/
  linker/
    include/scalanative/tools/linker/
    src/
    tests/

cpp-frontend/
  source/
  lexer/
  parser/
  ast/
  typecheck/

cpp-runtime/
  memory/
  platform/

cpp-tests/
  golden/
  integration/
  runtime/
```

`frontend/` is the new C++ source compiler. `nscplugin/` keeps the Scala Native
boundary name and should own the typed-AST-to-NIR lowering, Scala Native
annotations/intrinsics, name mangling, and compatibility rules.

## Memory Management Plan

Use a hybrid `gc + arena` strategy.

### Arena Allocation

Use arenas for short-lived compiler phase data:

- `SourceArena`: source buffers, line tables, transient decoding data.
- `TokenArena`: token streams and trivia.
- `AstArena`: untyped and typed AST nodes for one compilation unit.
- `TypecheckArena`: constraint variables, temporary inference state, scope
  snapshots, overload candidates.
- `NirArena`: per-module NIR builders, blocks, instructions, local values.
- `PassArena`: temporary optimizer/checker/linker pass data.

Arena rules:

- No owning `new` or `delete` in compiler code.
- Arena allocations never outlive the arena owner.
- Long-lived tables must not point directly to arena objects unless the arena is
  intentionally kept alive for the full compilation.
- Use stable IDs for cross-phase references.

### GC Allocation

Use GC-managed storage for long-lived graph data:

- Interned names, strings, global symbols, and package/class/module identities.
- Canonical types and signatures.
- Global NIR definitions loaded by the linker.
- Incremental compilation cache entries.
- Shared diagnostics metadata and source file registry.

Initial GC recommendation:

- Start with a non-moving mark-sweep collector for compiler objects.
- Add generational or moving collection only after pointer discipline is proven.
- Treat active arenas, compiler stacks, global tables, and pass roots as GC root
  sets.
- Use `GcPtr<T>` and explicit containers instead of raw owning pointers.

### Scala Native Garbage Collector Theory

Treat a future target-runtime collector as a dedicated compiler/runtime research
track named **Scala Native Garbage Collector Theory**. The objective is not
novelty by itself, but a fully specified collector architecture that can exploit
information a general-purpose collector does not have: exact descriptor tracing,
compiler-generated stack maps, escape and allocation-site summaries, zone
ownership, devirtualized type information, and specialization of allocation or
barrier policy by proven lifetime.

The design must define and test:

- The object, root, safepoint, movement, pinning, finalization, and native-interop
  invariants before selecting a collection algorithm.
- Correctness across cycles, interior/native references, exceptions, threads,
  module roots, scoped zones, and compiler-inserted temporary roots.
- Explicit throughput, pause-time, memory-footprint, fragmentation, generated
  code-size, and compilation-time targets.
- Reproducible comparisons with the current non-moving collector and mature
  Immix/Commix-style baselines on representative Scala Native workloads.
- Stress, differential, fuzz, and model/invariant testing before a new collector
  can become the default.

Candidate mechanisms may be combined or specialized rather than forced into one
universal policy. A from-scratch collector should replace a mature baseline only
after measurements show a meaningful advantage for Scala Native programs.

### Ownership Boundary

The intended data flow is:

```text
arena source/tokens/AST -> GC symbols/types -> arena NIR builder -> GC NIR module cache
```

This keeps hot phase-local allocation fast while preserving stable identities for
semantic and linker-level objects.

## Phase 1: Source

Goal: Build the source ingestion layer that every later phase trusts.

Deliverables:

- `SourceManager` for files, virtual files, in-memory sources, and module roots.
- `SourceFile`, `SourceId`, `SourceSpan`, line/column lookup, and snippet
  extraction.
- UTF-8 validation and newline normalization.
- File hashing for future incremental compilation.
- Diagnostic engine with source spans, severity, notes, and fix-it slots.

Implementation steps:

1. Define source identity and immutable source buffers.
2. Build line tables and byte-offset to line/column mapping.
3. Add diagnostics that can render source excerpts.
4. Add tests for empty files, CRLF, invalid UTF-8, tabs, and multi-line spans.

Exit criteria:

- A source file can be loaded, addressed by stable ID, and used to render precise
  diagnostics.

Current scaffold status:

- `SourceManager` supports file and virtual-file ingestion.
- Source text is stored after UTF-8 BOM stripping and CRLF/CR newline
  normalization.
- Invalid UTF-8 is diagnosed at source-ingestion time.
- Normalized source content has a stable FNV-1a hash for later incremental
  compilation keys.
- Diagnostics render source locations, underlined spans, notes/warnings/errors,
  and basic fix-it replacement text.
- CTest smoke coverage checks source normalization, hashing, invalid UTF-8, and
  diagnostic fix-it rendering.

## Phase 2: Lexer

Goal: Convert source text into a complete Scala token stream.

Deliverables:

- Token model with kind, lexeme span, optional literal payload, and trivia spans.
- Identifier/operator/backtick handling.
- Numeric, character, string, triple-string, symbol, and interpolated-string
  tokenization.
- Nested comments and doc comments.
- Scala semicolon inference.
- Mode flag for Scala 3 indentation-sensitive syntax, implemented after the
  braced syntax subset is stable.

Implementation steps:

1. Implement punctuation, keywords, identifiers, operators, comments, and EOF.
2. Add literal parsing with exact raw text preservation.
3. Implement semicolon inference as a post-lex token filter.
4. Add interpolation token mode.
5. Add indentation token mode if Scala 3 syntax is in scope for the first
   release.

Exit criteria:

- Golden lexer tests can round-trip representative Scala files into stable token
  streams.

Current scaffold status:

- Lexer recognizes a broader Scala-shaped token surface: identifiers, backtick
  identifiers, common declaration/control keywords, integer literals, hex
  integer literals, floating literals, strings, triple-quoted strings, character
  literals, symbol literals, punctuation, annotations, and operator runs.
- Nested block comments and line comments are skipped while preserving newline
  information for separator inference.
- Newline semicolon inference is implemented for the current subset, including
  avoiding separators after trailing operators and before dotted selections.
- Inferred semicolon tokens are marked as virtual.
- Tokens now preserve leading trivia for whitespace, newlines, line comments,
  and block comments.
- Literal validation reports basic malformed numeric suffixes, unknown string
  escapes, and unknown character escapes.
- Interpolation mode emits interpolated string start/part/end tokens,
  `$identifier` tokens, and `${...}` boundary tokens. Expression-hole contents
  use normal Scala tokens with nested-brace tracking, retaining their original
  source spans for parser and diagnostic work.
- Parser accepts explicit and inferred semicolons at package/declaration
  boundaries so the build pipeline still works with normal multiline Scala.
- CTest smoke coverage checks the new token kinds, trivia preservation,
  separator behavior, interpolation tokens, and literal diagnostics.

## Phase 3: Parser

Goal: Parse tokens into an untyped AST with useful error recovery.

Deliverables:

- Recursive-descent parser for top-level packages, imports, classes, traits,
  objects, methods, vals, vars, type aliases, expressions, blocks, matches, try,
  while, for, lambdas, and annotations.
- Pratt or precedence-climbing expression parser for Scala operator precedence.
- Parser recovery at statement, member, and top-level boundaries.
- AST construction through `AstArena`.

Implementation steps:

1. Start with a minimal executable subset: package, object, def, val, literals,
   calls, blocks, if, while, and return.
2. Add classes, traits, constructors, fields, and inheritance.
3. Add pattern matching and destructuring.
4. Add for-comprehension parsing and desugaring hooks.
5. Add type syntax, annotations, implicits/givens only after core syntax is
   stable.

Exit criteria:

- Parser can produce an AST for a small Scala program and recover from common
  malformed input without crashing.

Current scaffold status:

- Parser now builds nested AST structure instead of only recording top-level
  names.
- Current subset includes packages, objects/classes/traits with member blocks,
  simple fully-qualified, direct-member wildcard, and braced selector/rename
  imports, defs, vals, vars, abstract type members,
  lower- and upper-bounded type members, bounded intervals such as
  `type Item >: Lower <: Upper`, concrete member type aliases, parameter lists,
  declared return/value types, simple type names, blocks, literals,
  identifiers, selections, calls, binary operators, local declarations in
  blocks, `return`, `if`, literal-pattern `match`, `while`, `new`, and
  interpolated string tokens as string-like expressions.
- Parser consumes explicit and inferred semicolons at package, declaration,
  member, argument, and block boundaries.
- Parser accepts explicit `override type`, `override def`, `override val`, and
  `override var` modifiers for the current abstract-member implementation
  subset. Type declarations are currently member-only.
- Typecheck preserves nested members, and NIR emission now emits nested members
  such as `demo.Hello.main`.
- CTest smoke coverage validates object members, method parameters, declared
  types, binary expressions, call expressions, selections, block expressions,
  AST debug output, and the full CLI/NIR path.

## Phase 4: AST

Goal: Define the compiler's tree and semantic node model.

Deliverables:

- Untyped AST node hierarchy or tagged union.
- Typed AST overlay, either as separate nodes or side tables keyed by `NodeId`.
- Stable `NodeId`, `SymbolId`, `TypeId`, and `NameId`.
- Tree visitors, mappers, and debug printers.
- AST validation pass for structural invariants.

Implementation steps:

1. Define compact node storage with arena allocation.
2. Store source spans on all user-visible nodes.
3. Add child iteration and debug printing.
4. Add typed metadata side tables.
5. Add AST golden tests from parser output.

Exit criteria:

- AST nodes are stable, inspectable, source-addressable, and usable by typecheck.

Current scaffold status:

- AST declarations now preserve nested members, parameter lists, declared types,
  import targets, initializers, and source spans.
- AST expressions cover the current parser subset: literals, identifiers,
  blocks, local declarations, calls, selections, binary expressions,
  control-flow expressions, and construction expressions.
- `debugString` prints nested declarations and expression trees.
- `AstValidator` checks structural invariants before typecheck, including
  duplicate declarations in a scope, invalid nested package declarations,
  malformed expression arity, class-like initializer misuse, and untyped
  missing value initializers as warnings. Typed bodyless values are preserved
  for abstract-trait checking.
- Stable `NodeId` assignment and arena-backed AST storage are still future work.

## Phase 5: Typecheck

Goal: Resolve names and assign types for the supported Scala subset.

Deliverables:

- Symbol table for packages, objects, classes, traits, methods, fields, locals,
  type params, and synthetic symbols.
- Scope model with imports, local scopes, members, inheritance, and overload
  sets.
- Type model for primitives, singleton objects, classes, traits, method types,
  function types, arrays, tuples, null, nothing, unit, and type parameters.
- Initial type inference for local vals, method applications, overload
  resolution, and numeric literals.
- Namer, typer, post-typer checks, and erasure/lowering-prep phases.
- Diagnostics for unresolved names, type mismatch, ambiguous overloads,
  inheritance conflicts, and illegal overrides.

Implementation steps:

1. Implement namer for packages, top-level objects/classes/traits, members, and
   locals.
2. Implement primitive and standard prelude symbols.
3. Typecheck expressions, blocks, calls, methods, vals, vars, and control flow.
4. Add class and trait conformance.
5. Stage generics through explicit reference-type applications and erasure,
   followed by primitive boxing, inference, bounds/variance expansion, and then
   implicit/`given`/`using` resolution.

Exit criteria:

- A typed AST can be produced for the minimal executable subset, with stable
  symbol and type IDs ready for NIR generation.

Current scaffold status:

- Typecheck now preserves qualified symbol names and nested typed members.
- Typecheck collects a minimal per-scope symbol table for package/object member
  declarations, method parameters, and a global symbol index for simple,
  direct-member wildcard, and braced selector/rename imports.
- A small simple type model exists for `Unit`, numeric primitives, `Boolean`,
  `String`, `Char`, `Symbol`, `Null`, object-like declarations, and `Unknown`.
- The current typer infers simple literal, block, return, if/while, binary, and
  string-concatenation result types.
- Member identifiers and simple member calls can resolve against the current
  declaration scope, so `val y = add(1, 2)` can inherit `add`'s declared return
  type.
- Block-local `val`/`var` declarations can now be referenced by later
  expressions in the same block, and block expression type inference uses the
  final expression.
- Simple member selection can resolve members on known object/class-like
  receivers, so `Config.answer` can inherit `answer`'s type.
- `this` is now typed as the enclosing class/trait receiver for direct member
  bodies, so class methods can call other methods through `this.value`.
- Class constructor parameters now form a first field-layout MVP: `class
  Counter(start: Int)` emits a field, `new Counter(42)` stores the argument,
  and methods can read it through `this.start`.
- Class constructor parameter modifiers are now accepted for the stored-field
  subset: `class Counter(val start: Int, var current: Int)` emits both fields,
  and `var` parameters participate in mutable field assignment. Plain class
  parameters still keep the current captured-field MVP behavior.
- An explicit `val` constructor parameter can now implement an inherited
  abstract trait value. The typechecker validates its type and distinguishes it
  from a plain captured parameter, which does not expose the required accessor.
- Inherited accessor compatibility now follows right-biased member precedence.
  A rightmost `var` can satisfy a same-typed inherited `val` because it provides
  both the required getter and an additional setter. A rightmost `val` cannot
  satisfy an inherited `var`; incompatible accessor types, missing setter
  shapes, and method/accessor collisions receive distinct diagnostics.
- Direct class/trait member bodies now perform an implicit-`this` rewrite for
  unqualified member references while preserving parameter and block-local
  shadowing.
- Initialized `val` and `var` members inside classes now join the raw payload
  field-layout MVP. Their NIR field definitions carry tiny initializer bodies,
  and allocation lowering stores constructor parameters first, then evaluates
  class-body field initializers such as `val base = start + 1`.
- Assignment expressions now typecheck and lower for mutable class fields, so
  `current = current + 1`, `this.current = ...`, and `counter.current = ...`
  produce payload stores while assignments to `val` fields are rejected.
- Block-local `var` declarations now preserve mutability through NIR as
  `var %name = ...`; local assignments such as `current = current + 1` lower to
  stack-slot loads and stores while local `val` assignments are rejected.
- Block-local declared types now survive parser, typecheck, NIR, linker, and
  codegen as typed `let`/`var` instructions, so `val score: Base = new Child()`
  uses `Base` for member resolution while still dispatching to `Child` through
  the vtable when needed.
- Class-body constructor statements now parse and typecheck alongside stored
  class fields. The compiler preserves class-body source order and emits a
  synthetic `Class.$init` NIR function that replays field initializers and
  constructor statements after constructor-parameter storage during
  `new Class(...)`.
- The import MVP resolves top-level `import fully.qualified.Name` declarations
  to the final-segment alias, `import fully.qualified.Owner._` to direct owner
  members, and braced selectors such as `import Owner.{Name, Other => Alias}`.
  Imported nested objects/classes can be referenced by short name in expressions,
  `new` expressions, declared value types, and parameter types in the same
  source module.
- A shared `support::StdNames` catalog now owns the first compiler-known names:
  Scala/Object roots, `this`, `super`, constructors, runtime main/println, and
  type-test/cast spellings. Frontend typing, NIR emission, verification, linking,
  and codegen have begun consuming it. The catalog will expand with import roots,
  standard-library owners, annotations, encoded operators, and native intrinsics
  as those surfaces are implemented.
- A first inheritance metadata MVP validates direct `extends` parents for
  objects, classes, and traits, resolves those parents to qualified symbols, and
  preserves them for NIR. Inherited method/field lookup, inherited initialized
  field layout, static override checks, and a base-typed virtual class-method
  dispatch MVP now build on that metadata. Class parent constructor arguments in
  `extends Parent(args...)` now initialize inherited constructor fields before
  inherited body fields. Explicit `super.member` and `super.method(...)`
  selections now resolve to parent method/field symbols while reusing the
  current receiver pointer. A first `with`-composition dispatch MVP reuses the
  closed-world vtable path for trait-typed receivers. Classes may have one
  superclass followed by multiple traits, while traits may extend multiple
  traits. Traits can now declare abstract methods with explicit return types and
  abstract `val` accessors with explicit value types. Concrete child classes
  must implement every inherited abstract method and value, including members
  inherited through longer trait chains. Intermediate traits may satisfy some
  abstract members with defaults while leaving others for concrete classes.
  Qualified direct-parent calls such as `super[Named].name` are supported and
  lower to the selected parent implementation. Plain `super.member` now follows
  the rightmost direct mixin and continues through that trait's ancestors or
  earlier direct parents until it finds a concrete member. Class constructor
  arguments and inherited field layout continue to follow the first class
  parent. Inherited methods that collapse onto the same dispatch name must have
  compatible parameter and return types. Diamond inheritance now uses a shared
  right-biased C3-style order across typechecking, NIR lookup, linking, and
  vtable construction, with repeated ancestors emitted once. Cyclic inheritance
  and contradictory parent precedence are rejected by both source typechecking
  and NIR verification. Initialized trait `val`s and `var`s now receive concrete
  per-instance storage. Abstract trait variables require explicit types and may
  be implemented by class-body or constructor `var`s. Traits may declare
  abstract type members with `type Element`, `type Element <: Base`, or
  `type Element >: Lower <: Upper`; classes or intermediate traits satisfy them
  with conforming aliases such as `type Element = Derived`. Abstract
  refinements may raise an inherited lower endpoint or narrow an inherited upper
  endpoint, while inconsistent intervals, unrelated aliases, and incompatible
  composed bounds are rejected. Explicit `super(...)` constructor-call syntax
  remains future work.
- Direct inherited `def` lookup now works through the resolved parent chain for
  the supported non-virtual subset, so `counter.zero` and unqualified `zero`
  inside `class FancyCounter extends BaseCounter` can resolve to
  `BaseCounter.zero`. Inherited `val`/`var` lookup now works for parent
  initialized fields, so child methods can read and assign parent fields through
  `this`.
- Static method overrides now shadow inherited `def` members for receivers whose
  static type is the subclass. The parser accepts explicit `override def` and
  `override val`; the typechecker rejects obvious override shape mismatches,
  parameter type/count mismatches, incompatible value types, declarations that
  override the wrong member kind, and declarations that override nothing.
  A concrete class-body `override val` can satisfy an inherited abstract trait
  value and is marked for accessor generation. Explicit `val` constructor
  parameters use the same validation and accessor path, including inherited
  construction through a child class. Base-typed method parameters can now
  dispatch overridden class methods dynamically through codegen vtables, and trait-typed method
  parameters can dispatch concrete trait defaults or class overrides in the
  trait-composition subset. Concrete trait default methods may call abstract
  methods through the receiver vtable. Later traits take precedence over
  earlier traits for same-named defaults. Mutable trait accessors dispatch
  getters and setters through the same closed-world vtable path. Type-member
  bounds and stable path-dependent selections of concrete aliases are now
  supported. Owner-qualified projections of concrete aliases are also
  supported. Abstract dependent signatures use their explicit reference upper
  bound as the runtime erasure. Unbounded abstract members and members resolved
  to `Boolean`, `Int`, `Long`, `Float`, `Double`, or `Char` now use an
  object-erased ABI with explicit NIR boxing and unboxing at implementations,
  calls, selections, inherited defaults, and initialized trait-value storage.
  Covariance declarations, self types, and `super(...)` constructor calls remain
  future work.
- Unresolved identifiers now produce diagnostics.
- Unresolved selected members now produce diagnostics.
- Declared type checks catch obvious initializer mismatches such as
  `val x: Int = "text"`.
- NIR declaration signatures now use declared or inferred simple types, so
  `def main = 0` emits `()Int` in the scaffold NIR.
- Relative imports, cross-file/classpath imports, overload resolution, richer
  nested owner/shadowing rules, inherited
  field lookup/layout, full override semantics, inheritance layout/dispatch
  checks, and `TypeId` canonicalization remain future work.

## Phase 6: NIR

Goal: Build the C++ representation of Scala Native NIR.

Deliverables:

- `nir/` library with definitions, types, values, instructions, control-flow
  nodes, attributes, globals, locals, signatures, and source positions.
- NIR builder for SSA blocks with block parameters.
- Textual NIR parser/writer for golden tests and debugging.
- Binary serialization format, versioned from day one.
- NIR verifier for type and control-flow invariants.

Implementation steps:

1. Model NIR names, types, values, attributes, and definitions.
2. Add block/instruction/control-flow representation.
3. Add builder APIs that prevent malformed blocks where possible.
4. Add textual writer, then parser.
5. Add binary reader/writer after the text format is useful.

Exit criteria:

- The compiler can construct, print, parse, and verify NIR for simple functions,
  classes, traits, and modules.

Current scaffold status:

- `cpp-nir` contains an initial module/definition model plus a textual writer.
- The typed-AST-to-NIR bridge emits package/object/member-shaped definitions
  with simple Scala-like signatures such as `()Int`.
- Declarations with initializers now emit structured function bodies that render
  as textual `define` bodies with `entry:`, pseudo `let`/`eval` lines for
  block-local scaffolding, and `ret Type value` terminators.
- The tools checker runs the NIR verifier before later build phases.
- The verifier rejects empty module names, empty definition names, missing
  function signatures, malformed function signatures, duplicate definition
  names, declaration bodies, and definitions without a body terminator.
- CTest now exercises the verifier directly and runs basic source fixtures
  through the CLI/NIR path.

### Future Scala 3 artifact interoperability: TASTy

TASTy is a frontend input and library-interoperability concern, not a replacement
for NIR. Once the internal symbol/type model and Scala 3 language surface are
stable enough, add a version-gated TASTy reader that unpickles names, symbols,
types, ownership, annotations, and supported typed trees into the same internal
typed AST consumed by Phase 7. Source-compiled and TASTy-loaded definitions must
therefore share erasure, intrinsic recognition, linking, optimization, codegen,
and diagnostics after the import boundary.

The first milestone should consume a pinned Scala 3 TASTy version, reject newer
or malformed files with located compatibility diagnostics, resolve external
classpaths deterministically, and compile fixtures that mix source units with
precompiled Scala 3 libraries. Generic signatures, traits, inline-expanded
artifacts, and Scala Native annotations need explicit round-trip tests. TASTy
emission, full minor-version compatibility, quoted code, staging, macros, and
tooling APIs remain separate later milestones rather than implicit reader
requirements.

## Phase 7: nscplugin Equivalent - Typed AST to NIR

Goal: Lower typed Scala AST into NIR.

Deliverables:

- Name mangling compatible with Scala Native expectations.
- NIR emission for packages, modules, classes, traits, fields, methods,
  constructors, closures, arrays, primitives, boxes, exceptions, and module
  initialization.
- Intrinsic and annotation handling for Scala Native APIs.
- Runtime call surface for allocation, type tests, casts, monitor operations,
  exceptions, strings, arrays, and reflection-like metadata.
- Debug source position mapping.

Implementation steps:

1. Emit NIR for literals, locals, blocks, calls, primitive operations, and return.
2. Add object/module initialization and `main` discovery.
3. Add class layout, fields, methods, virtual dispatch, and constructors.
4. Add trait dispatch metadata.
5. Add closures/lambdas, pattern matching lowerings, arrays, exceptions, and
   native interop.

Exit criteria:

- A typed "hello world" style program can be emitted as valid NIR.

Current scaffold status:

- `cpp-nscplugin` emits package/object/member-shaped NIR definitions from the
  typed AST.
- Declarations with initializers emit structured NIR function bodies for
  literals, parameters, simple binary expressions, block-local scaffolding,
  calls, selections, `if`, and `while`.
- Method signatures now carry simple parameter types, such as `(Int,Int)Int`.
- `nir::FunctionBodyBuilder` now provides the first builder API for parameters,
  `let`, `eval`, return, and unreachable instructions, and prevents appending
  after a terminator.
- The emitter discovers supported object-owned `main` methods and emits a
  `scala.scalanative.runtime.main` bridge that calls them, falling back to a
  declaration when no supported main is found. The bridge supports zero-argument
  mains plus `main(args: Array[String])`: native `main(argc, argv)` creates an
  arena-owned argument array containing `argv[1..]` and passes it through the
  typed bridge. `Array[String].length` and checked `args(index)` access are
  available for this runtime argument array; null, negative, and out-of-range
  indexed access traps. The first source-level constructor slice also supports
  non-empty `Array("first", "second")` literals and explicit
  `Array[String]()` empty literals, sharing that layout and API. Explicit
  `Array[Int](1, 2)` and inferred homogeneous `Array(1, 2)` literals use
  unboxed scalar storage. Boolean, Long, Double, Float, and Char literals
  support the same explicit and inferred forms, including `Array[Boolean]()`,
  `Array(true, false)`, `Array[Long]()`, `Array(1L, 2L)`, `Array[Double]()`,
  `Array(1.5, 2.5)`, `Array[Float]()`, `Array(1.5F, 2.5F)`, `Array[Char]()`,
  and `Array('a', 'b')`, with unboxed scalar slots. Arrays retain reference identity
  across typed method parameters.
  Recognized class and trait reference types also support precise pointer-slot arrays
  such as `Array[Entry]()` and `Array[Named](new Entry(...))`; reads preserve the
  static element type for member selection and dynamic dispatch. Local element
  updates use the same checked bounds path. Explicit `Array[Any]` literals use
  pointer slots and box supported scalar elements (`Boolean`, `Int`, `Long`,
  `Float`, `Double`, and `Char`); scalar reads use checked `asInstanceOf[T]`
  unboxing, and `isInstanceOf[T]` checks the matching boxed scalar descriptor.
  Inferred literals such as `Array(1, 2L, true, 'a')` select those same
  `Array[Any]` object slots when no more precise element type covers every
  supported value. Direct function parameters typed `Any` accept those scalar
  values through the same boxed `Object` ABI, and pass-through `Any` returns
  preserve that reference for later type tests or casts. Typed local `val` and
  `var` initializers plus mutable local assignments use the same boxed storage.
  Typed `Any` class and module `val`/`var` fields initialize and mutate through
  that same boxed storage; `cpp-examples/AnyFields.scala` exercises `Int`,
  `Long`, and `Boolean` field values through the optimized native pipeline.
  Constructor parameters typed `Any` now use the same boxed `Object` storage for
  direct class allocation and parent-constructor forwarding;
  `cpp-examples/AnyConstructorParameters.scala` covers constructor `Int`,
  `Long`, and `Boolean` values through the optimized native pipeline.
  `Unit` and `Symbol` now join that boxed `Any` path for explicit and inferred
  `Array[Any]` object slots, locals, fields, constructor parameters, and direct
  `Any` parameters; `cpp-examples/AnyUnitSymbol.scala` exercises `isInstanceOf`
  checks and `Symbol` round-trips through the optimized native pipeline.
  `String` now joins the same Object ABI whenever it is stored as `Any`;
  `cpp-examples/AnyString.scala` covers boxed String locals, `Array[Any]`
  entries, `isInstanceOf[String]`, `asInstanceOf[String]`, `toString`, content
  equality, and content hash values through the optimized native pipeline.
  Boxed and reference-backed `Any.toString`, `println(any)`, and String
  concatenation now lower through `scala.scalanative.runtime.anyToString`;
  `cpp-examples/AnyToString.scala` covers boxed primitive, `Unit`, `Symbol`,
  `Null`, and class-reference formatting through the optimized native pipeline.
  Resolvable user-defined zero-argument `toString` members now win over that
  compiler fallback for direct calls, and the generic `Any` helper now probes
  the runtime vtable for compatible custom `toString` slots before falling back
  to descriptor names; `cpp-examples/CustomToString.scala` demonstrates direct,
  base-typed, `Any`-typed, `println`, and concatenation calls.
  Compiler-known `equals(value)` now lowers through the same equality NIR used
  by `==`; `cpp-examples/Equals.scala` covers primitive, String, reference,
  typed-null, and `Any` identity comparisons through the optimized pipeline.
  Boxed `Any` equality now lowers through `scala.scalanative.runtime.anyEquals`;
  `cpp-examples/AnyEquals.scala` covers boxed `Int`, `Boolean`, `Long`, `Unit`,
  `Symbol`, primitive-vs-`Any` comparison, and object identity through the
  optimized pipeline.
  Direct Unit equality through `.equals`, `==`, and `!=`, plus Unit `hashCode`,
  now lower without trying to compare or hash a raw Unit payload: Unit-to-Unit
  equality is `true`, direct Unit-to-non-Unit equality is `false`,
  Unit-to-`Any` equality delegates through boxed `Any`, and direct Unit hashCode
  is `0`; `cpp-examples/UnitEqualsHashCode.scala` and
  `cpp-examples/UnitOperators.scala` cover those direct and boxed paths.
  Compiler-known `hashCode`/`hashCode()` now lowers through typed hash helpers;
  `cpp-examples/HashCode.scala` covers primitive, String/Symbol content,
  typed-null, object identity, and boxed `Any` hash values through the optimized
  pipeline.
  Non-`Int` mains normalize to process exit code `0`.
- The emitter rewrites explicit, direct-member wildcard, and braced selector
  imported identifier and parameter-type aliases to their fully-qualified NIR
  globals.
- Object/class/trait `extends` metadata now emits into NIR definition signatures,
  such as `class @demo.Child : @demo.Parent` and
  `trait @demo.Named : @java.lang.Object`.
- Inherited direct method selections are preserved as ordinary NIR selections,
  such as `%this.zero`; the checker/linker/codegen resolve the selected global
  through parent metadata.
- Static override selections resolve to the nearest subclass method in the
  parent chain, while inherited selections still resolve to the parent method
  symbol when the subclass does not define one.
- Base-typed class-method selections keep the same NIR shape and lower in
  codegen through a closed-world vtable when the receiver may be a subtype.
- Trait-typed method selections keep the same NIR shape across composed parent
  lists; codegen gives trait definitions virtual slots and lets concrete classes
  copy or override those slots.
- Ordered parent metadata now renders as
  `@Base with @TraitA with @TraitB`, preserving one superclass plus multiple
  trait mixins through checker, linker, and codegen.
- Bodyless trait methods emit as typed NIR function declarations, while concrete
  trait default methods remain ordinary function definitions.
- Bodyless trait `val` members emit as zero-argument receiver function
  declarations. A concrete class-body implementation emits separate
  `name$field` storage plus a `name` getter definition, preserving one virtual
  accessor ABI for trait-typed and concrete receivers.
- Trait chains and composed trait lists preserve abstract and concrete slots;
  concrete descendant classes assemble one complete vtable from all ancestors.
- Block-local declared value types are emitted on NIR `let`/`var` instructions,
  preserving widened static receiver types for linker and codegen resolution.
- Class parameter fields are emitted as NIR `field @Class.name : Type`
  definitions, and constructor arguments are preserved on `new Class(...)`
  values.
- Constructor `val` implementations of abstract trait values emit
  `field @Class.name$field` storage plus a receiver getter. Parent-constructor
  forwarding targets the renamed backing field, so subclasses inherit the
  completed accessor without reimplementing the root declaration.
- Initialized trait `val`s remain accessor declarations on the trait and are
  materialized as class-owned owner-qualified `$trait$field` storage plus
  getters in the first concrete class that needs them. Synthetic `$init` bodies
  evaluate root traits before later mixins and subclasses reuse inherited
  storage. Same-name trait values retain separate fields and hidden owner
  getters; only the right-biased winner receives the public accessor.
- Trait `var`s use paired `name` getter and `name_$eq` setter declarations.
  Abstract variables can be implemented by constructor or class-body `var`s;
  initialized variables use the same ordered, owner-qualified per-instance
  storage as initialized trait values. Assignments through trait-typed receivers
  retain and dispatch the setter symbol through checker, linker, optimizer, and
  codegen.
- A same-typed mutable trait accessor can also satisfy a lower-priority abstract
  `val` contract. The concrete class emits one public getter shared by both trait
  views and the additional setter required by the mutable view.
- Abstract type members emit as `type @Owner.Name : abstract`; concrete aliases
  use the same NIR definition kind with their resolved target type. Concrete
  classes may implement the member directly or inherit a concrete alias through
  an intermediate trait. This metadata is compile-time-only and does not add a
  runtime field or virtual slot.
- Upper bounds emit as metadata such as
  `type @Owner.Name : abstract <: qualified.Base`. Concrete aliases and abstract
  refinements are checked against every inherited bound after right-biased
  linearization, both in the frontend and by the NIR verifier.
- Lower bounds and intervals emit as metadata such as
  `type @Owner.Name : abstract >: qualified.Lower <: qualified.Upper`. The
  frontend checks `Lower <: alias <: Upper`, carries omitted endpoints through
  abstract refinements, and rejects internally inconsistent intervals before
  NIR emission.
- Effective concrete aliases are substituted into inherited method parameters,
  method returns, value accessors, and stored-field types during override and
  member-scope construction. The lexical root declaration keeps its symbolic
  semantic type. Concrete aliases that do not override an abstract ABI may emit
  a direct primitive signature such as `(Int)Int`; overrides preserve the
  inherited runtime erasure so virtual slots remain compatible.
- Stable path-dependent selections now resolve from left-to-right method
  parameter scope. A signature such as
  `def echo(box: IntBox, value: box.Item): box.Item`, where `IntBox.Item = Int`,
  emits the concrete ABI `(IntBox,Int)Int`. Mutable values and methods are
  rejected as unstable path prefixes, and missing or non-type selected members
  receive focused diagnostics. Abstract selected types retain their path
  identity during typechecking. When the selected member has a concrete
  reference upper bound, NIR and LLVM erase the runtime parameter or return to
  that bound while the frontend continues checking same-path identity.
- Owner-qualified type projections such as `IntCarrier#Element` resolve through
  effective inherited member scopes. Concrete projected aliases specialize
  directly into method parameters and returns, so an inherited
  `type Element = Int` emits an `(Int)Int` ABI. Unknown owners, absent members,
  and projections of value members receive dedicated diagnostics. Abstract
  projections retain canonical `qualified.Owner#Member` identity during
  typechecking. Bounded abstract projections use the same reference-upper-bound
  erasure rule as stable path selections.
- Bounded abstract dependent members now support native dispatch. For
  `type Item <: Base`, a method using `box.Item` or `Box#Item` emits `Base` in
  its NIR ABI, while expressions selected from the dependent value resolve
  members through `Base`. Distinct stable paths remain distinct semantic types.
- Unbounded and scalar-resolved dependent members now erase to `Object`. NIR has
  typed `box[T]` and `unbox[T]` values for `Boolean`, `Int`, `Long`, `Float`,
  `Double`, and `Char`. The verifier checks their operand and result types, LLVM
  lowers them to the current header-plus-payload object layout, and the emitter
  inserts conversions at override bodies, inherited defaults, call sites,
  selections, and trait-value initialization. Optimized native fixtures cover
  abstract values plus parameter/return round trips for every supported scalar.
- Typechecking now publishes span-keyed semantic `TypeInfo` records for every
  visited expression. NIR emission consumes these annotations for inferred local
  types and receiver resolution instead of rebuilding a local-name/type table.
  An optimized regression passes a dependent scalar value through an inferred
  local alias before selecting and calling its members.
- Abstract dependent types now retain canonical owner/member identity in
  addition to their source spelling. This permits the sound directional
  widening `box.Item` to `Box#Item` when the path owner conforms to `Box`, while
  preserving the rejection of `Box#Item` to one particular `box.Item` and of
  values crossing between two unrelated stable paths. The shared reference
  upper-bound ABI means this widening requires no runtime cast.
- Concrete dependent aliases retain provenance after normalization. A type can
  therefore lower as its concrete target, such as `Int`, while separately
  recording the stable occurrence (`box.Item`), canonical owner/member
  (`IntBox.Item`), and exact alias definition that resolved it. Member selections
  propagate the same data into typed-expression records, and inherited member
  specialization preserves the original dependent occurrence while recording
  the effective concrete alias.
- The first source-generics slice now preserves structured type parameters on
  classes, traits, and methods plus multiple explicit type arguments on postfix
  applications. Explicit invariant reference applications such as `Box[Label]`,
  bounded declarations such as `Box[A <: Base]`, generic constructor calls, and
  generic member/object method calls are typechecked with arity, bound, and
  substitution diagnostics. Runtime signatures erase a type parameter to
  `Object` or its concrete reference upper bound; NIR inserts checked casts when
  an erased field read or method result returns to a concrete static type.
  `cpp-examples/ReferenceGenerics.scala` and optimized native smoke coverage
  exercise one- and two-parameter classes, a generic trait declaration, nested
  generic construction, generic member methods, invariant conformance, and
  erased NIR.
- Explicit generic applications now also accept every scalar supported by the
  boxed runtime (`Unit`, `Boolean`, `Byte`, `Short`, `Int`, `Long`, `Float`,
  `Double`, `Char`, and `Symbol`). Generic fields and parameters retain their
  erased `Object` ABI: constructors and calls box concrete scalar arguments,
  while specialized field reads and method results unbox before arithmetic,
  comparisons, mutation, or other typed use. Mutable generic constructor fields
  preserve raw storage targets so assignments re-box instead of attempting to
  write through an unboxed selection. `cpp-examples/PrimitiveGenerics.scala`
  and optimized native smoke coverage exercise every supported scalar, generic
  class and method round trips, direct mutation, and erased NIR signatures.
  Primitive upper/lower bounds remain deferred.
- Omitted type arguments are now inferred from value arguments for generic
  method and constructor calls. The current Scala 3-oriented subset collects
  direct and nested applied-type constraints, specializes generic members after
  receiver substitution, merges repeated compatible candidates (including
  numeric widening), and validates inferred arguments against existing bounds.
  Missing evidence, incompatible candidates, and bound failures receive focused
  diagnostics that recommend an explicit application where inference cannot
  proceed. Inferred calls reuse the same erased `Object` ABI, boxing, unboxing,
  and checked casts as explicit applications.
  `cpp-examples/GenericInference.scala` and optimized native smoke coverage
  exercise primitive, reference, bounded, nested, multiple-parameter,
  receiver-member, and generic-inside-generic inference.
- Expected result types on fields, locals, and methods now complete omitted
  method and constructor type arguments that value arguments cannot determine.
  Expected types flow through final block expressions, explicit returns,
  conditional branches, and try/catch results. Value-argument constraints are
  collected first and remain authoritative; the expected result fills only
  missing parameters, after which ordinary conformance and bound checks report
  any conflict. This supports no-argument generic methods, phantom constructors,
  partially inferred multi-parameter applications, and contextual inference
  inside generic method bodies. Interflow also preserves the target reference
  type when folding a cast of `null`, keeping contextually typed conditional
  branches valid after optimization.
  `cpp-examples/ExpectedGenericInference.scala` and optimized native smoke
  coverage exercise these paths and their focused failure diagnostics.
- Class and trait type parameters now preserve Scala 3 declaration-site `+` and
  `-` variance. Applied-type conformance follows each constructor parameter's
  variance without admitting numeric widening inside type arguments, and
  declaration checking composes positive, negative, and invariant positions
  through nested generic types. Illegal method parameters, returns, mutable
  fields, constructor fields, and parent applications receive focused
  diagnostics.
- Class and trait parents now retain their applied static types alongside erased
  runtime parent names. Direct and transitive generic ancestors specialize
  inherited fields, methods, constructor parameters, override contracts, and
  compatibility checks while keeping `Object` bridge signatures in NIR. The
  subtype relation follows those applied parent patterns, enabling covariant and
  contravariant widening across concrete and still-generic children. Exact
  devirtualization also adapts a statically widened trait receiver before calling
  a concrete override. `cpp-examples/VarianceAndInheritance.scala` and optimized
  native smoke coverage exercise direct, transitive, and forwarded generic
  inheritance, inherited storage, both variance directions, illegal positions,
  invariant rejection, and erased dispatch.
- The first Scala 3 contextual-abstraction slice supports named `given` values,
  trailing method `using` clauses, explicit `(using ...)` applications, and
  omission of contextual arguments at call sites. Generic method inference runs
  from ordinary arguments before contextual search, so an inferred `A` can
  specialize evidence such as `Show[A]`. Search considers visible contextual
  parameters and named givens, prefers a matching contextual parameter, and
  diagnoses missing or ambiguous evidence. Selected arguments are recorded on
  the typed module and lowered as ordinary erased call arguments; concrete
  reference overrides reconstruct their semantic parameter type inside the
  erased body. `cpp-examples/ContextualAbstractions.scala` and optimized native
  smoke coverage exercise inferred, forwarded, explicit, and locally overridden
  evidence.
- Anonymous givens and block-local named or anonymous givens now use the same
  typed evidence model. Lexical depth is retained during search, so the
  innermost matching local given wins while equally nested matches remain
  ambiguous. Top-level class/trait companions and their objects receive distinct
  internal type and module symbols (`Type` and `Type$`), allowing contextual
  search to inspect both the expected type constructor's companion and the
  companions of its applied type arguments. Lexically visible evidence still
  takes precedence over companion evidence, and selected companion values lower
  through their module accessors so initialization remains lazy and reachable.
  Native coverage exercises named and anonymous locals, nested shadowing,
  typeclass and type-argument companions, and companion ambiguity. Parameterized
  and derived givens, direct companion-object term selection/imports,
  contextual-only type inference, nested companion declarations, and Scala 2
  `implicit` syntax remain later milestones.
- Postfix type application now supports typed `value.isInstanceOf[Target]` and
  checked `value.asInstanceOf[Target]` for known classes and traits. NIR uses
  dedicated `is-instance-of[T]` and `as-instance-of[T]` values; verification
  rejects malformed targets and non-reference receivers, linking retains
  metadata-only target types, and LLVM lowers both through ABI 4 descriptor
  compatibility helpers. Casts preserve null, return the original pointer when
  compatible, and use Runtime ABI 26's typed `ClassCastException` when
  incompatible. Optimized native tests cover exact, parent, direct/transitive
  trait, unrelated, null, successful-cast member selection, and failing-cast
  cases.

Next queue items:

1. Expand structured NIR from single-entry function bodies toward explicit
   blocks, locals, typed values, and terminators.
2. Lower literals, locals, blocks, calls, primitive operations, and returns
   through builder APIs that prevent malformed NIR.
3. Expose the argv array through the eventual source-level `Array` API, then
   expand module initialization with cycle diagnostics and thread-safe
   publication.
4. Establish name mangling rules before class, trait, and dispatch work expands.
5. Move from span-keyed expression annotations to stable typed node IDs as the
   typed AST matures.
6. Connect the structured `Zone.scoped` body intrinsic to the eventual stdlib
   callback API, extend closed-world receiver-effect summaries to library and
   cross-module contracts, then replace conservative function shadow frames
   with liveness-aware stack maps where profitable.

## Phase 8: tools/checker

Goal: Validate NIR and linked programs before optimization and codegen.

Deliverables:

- Per-definition NIR verifier.
- Whole-program checks for missing symbols, illegal inheritance, invalid
  dispatch tables, unresolved externals, invalid attributes, and bad GC roots.
- Diagnostics with NIR source positions and original source positions where
  available.

Implementation steps:

1. Verify local SSA properties: defined-before-use, block terminators, type
   consistency, and block argument arity.
2. Verify global definition consistency.
3. Verify class/trait/module hierarchies.
4. Verify runtime and GC metadata expectations.

Exit criteria:

- Every later tool can require checker-clean NIR as input.

Current scaffold status:

- `cpp-tools/checker` wraps `nir::Verifier` and reports verifier failures as
  diagnostics.
- The verifier catches empty module/definition names, missing or malformed
  function signatures, duplicate definitions, declaration bodies, and function
  definitions without terminators.
- The verifier now checks structured single-entry function bodies for parameter
  ordering/count/type consistency, duplicate locals, instructions after
  terminators, undefined local/global references, selected member resolution,
  call arity and argument types, simple expression type consistency, and return
  type consistency.
- Verifier type consistency now treats `Null` as conforming to reference-like
  NIR types, including the typed `main(args: Array[String])` bridge and the
  current `Array[String]` runtime representation.
- Class-like NIR definitions now require valid parent metadata. The verifier
  accepts `@java.lang.Object`, checks resolved class/trait parents, rejects
  self-extension, and rejects malformed trait parents.
- Selected member verification and receiver conformance now follow parent
  metadata for inherited direct-method calls.
- Assignment verification accepts mutable accessor targets by resolving the
  paired `name_$eq` setter and checking its receiver, value parameter, and
  `Unit` result signature.
- Type-member definitions require class-like owners and explicit `abstract` or
  alias-target metadata. The verifier follows parent linearization and rejects a
  concrete class or module whose effective type member remains abstract.
- Bounded metadata uses `abstract <: Type`, `abstract >: Type`, or
  `abstract >: Lower <: Upper`. The verifier checks interval consistency plus
  concrete aliases and abstract refinements through NIR parent metadata,
  rejecting targets outside either inherited endpoint and incompatible composed
  traits.
- The verifier also rejects concrete classes that inherit a default function
  whose runtime signature still references an abstract type member without a
  concrete override, matching the frontend ABI-specialization rule for cached
  or hand-built NIR.
- Explicit `box[T]` and `unbox[T]` values are checked structurally for the
  supported scalar set. Boxing requires an operand of exactly `T` and produces
  `Object`; unboxing requires a reference operand and produces `T`.
- Override validation currently happens in the frontend for the supported method
  subset. Checker/linker/codegen still resolve selected method symbols from the
  static receiver type, and codegen performs the current vtable dispatch MVP
  when that static receiver may have subclasses.

Next queue items:

1. Extend checks from single-entry bodies to explicit NIR blocks once Phase 7
   introduces them.
2. Broaden type consistency for richer primitive operations, object values,
   casts, allocation, and control-flow joins.
3. Move unresolved-reference reporting toward whole-program linker diagnostics
   once Phase 9 has a global index across modules.

## Phase 9: tools/linker

Goal: Load NIR, resolve reachability, and produce a closed linked program.

Deliverables:

- NIR module loader from source output, libraries, runtime, and cache.
- Reachability analysis from entry points, `pin` attributes, and runtime roots.
- Dependency closure over methods, fields, classes, modules, traits, constants,
  and extern declarations.
- Class hierarchy analysis and dispatch metadata generation.
- Link report and unresolved-symbol diagnostics.

Implementation steps:

1. Implement module loading and global index construction.
2. Add root discovery and reachability worklist.
3. Add class hierarchy and virtual method resolution.
4. Add dead definition elimination.
5. Add linked-program serialization for codegen.

Exit criteria:

- A set of `.nir` modules can be reduced to a closed linked program or a clear
  linker error.

Current scaffold status:

- `cpp-tools/linker` builds a global definition index across input modules.
- Duplicate globals are diagnosed before reachability runs.
- Root discovery prefers `scala.scalanative.runtime.main`, falling back to
  explicit `main`-named function definitions when the runtime bridge is absent.
- The linker runs a reachability worklist from roots and records reachable
  globals in `LinkedProgram::reachableGlobals`.
- Reachable virtual method references now conservatively retain matching
  overrides on known class/trait subtypes, so interflow pruning cannot remove
  implementations that remain callable through a vtable slot.
- Virtual reachability accepts covariant reference returns, preserving concrete
  overrides whose return type narrows a bounded abstract member's erased upper
  bound.
- Reachable mutable accessor assignments retain both the selected getter target
  and its paired setter; virtual setter references retain concrete subtype
  implementations before optimized dead-definition pruning.
- Linked programs retain the full module set; the Phase 10 interflow pruning
  pass consumes `reachableGlobals` when optimization is enabled.

Next queue items:

1. Move unresolved-reference diagnostics fully into linker once multi-module
   verifier behavior is relaxed.
2. Add explicit root configuration and pinned globals.
3. Add link reports that distinguish roots, reachable definitions, and retained
   but unreachable definitions.
4. Broaden dynamic-target reachability as arrays, closures, multiple traits, and
   richer dispatch metadata are introduced.

## Phase 10: tools/interflow

Goal: Add whole-program optimization after correctness is reliable.

Deliverables:

- Pass manager with validation before and after every pass.
- Constant propagation, dead-code elimination, local simplification, inlining,
  devirtualization, escape analysis, allocation simplification, and closure
  cleanup.
- Optimization remarks and pass timing.

Implementation steps:

1. Start with local canonicalization and DCE.
2. Add call graph and conservative inlining.
3. Add class-hierarchy-based devirtualization.
4. Add escape analysis for arena/stack allocation opportunities.
5. Add profile-guided and flow-sensitive improvements later.

Exit criteria:

- Optimized NIR remains checker-clean and produces identical results on
  integration tests.

Current scaffold status:

- `cpp-tools/interflow` now returns an `InterflowResult` with pass reports,
  validation errors, and removed-definition counts.
- The first pass manager step validates NIR before and after each pass using the
  current NIR verifier.
- Pass reports now include duration, removed-definition counts, folded-value
  counts, and pre/post validation error counts.
- The build driver can now emit those pass reports as machine-readable JSON with
  `--optimization-report <path>` whenever `--optimize` runs interflow.
- `--emit-nir --optimize` now emits linked, post-interflow NIR and can also
  write an optimization report, making local optimizer effects inspectable
  before LLVM codegen.
- The first propagation pass substitutes literal/Unit/`sizeof` local `let`
  values into later structured expressions within a single-entry function body,
  including nested block-local `let` scopes with shadowing-aware replacement.
- The propagation pass also substitutes aliases to immutable params and `let`
  locals while refusing aliases to mutable `var` locals, preserving captured
  value semantics around later assignments.
- Literal-only unary, binary, and conditional expression aliases are propagated
  as well, letting the later fold and DCE passes collapse shapes such as
  `val sum = 1 + 2; sum` and `val chosen = if (true) 1 else 2; chosen` without
  propagating parameter-dependent arithmetic like `x + 1`.
- The fold pass also reuses immutable local values that have just folded into
  literal/Unit/`sizeof`-style shapes, so a later `ret %local` can collapse in the
  same pass and expose the now-dead binding to DCE.
- The first local simplification pass folds literal-only `Int`/`Long`
  arithmetic, literal `Int`/`Long`/`Float`/`Double`/`Char` comparisons, exact
  `Unit` equality/inequality, exact `Null` equality/inequality, literal
  `String` equality/inequality, literal `String` concatenation, literal
  `Symbol` equality/inequality, literal Boolean equality/logical expressions,
  and constant Boolean `if` expressions in structured NIR values.
- Same-branch `if` expressions now collapse even when the condition is
  effectful: pure conditions drop away as before, while effectful conditions are
  preserved in a block before the shared branch value. When that block result is
  itself discarded by an `eval` or an enclosing block operand used only for
  effects, block cleanup trims the pure shared value and keeps only the condition
  effect. Discarded `if` results with two pure branch values now similarly trim
  down to the condition effect, while branches that may carry effects keep the
  conditional structure but drop their unused result values to `Unit`.
- String concatenation folding now also handles exact ABI-known values, so
  literal concatenations with locally known `Int`, `Long`, `Boolean`, `Char`,
  `Symbol`, `String`, `Null`, `Unit`, and boxed `Any` versions of those values
  collapse to string literals. Ordinary object `toString` and floating
  formatting remain runtime operations. Direct and immutable-local effectful
  Unit boxes also fold into `"()"` during concatenation while retaining payload
  evaluation; dynamic effectful boxed values remain runtime concatenations.
- Stable same-local scalar comparisons are folded for non-floating scalar
  locals, such as `x == x`, `x != x`, and `x < x`.
- Stable same-local equality and inequality are also folded for known
  non-floating reference-like locals, including `String`, `Symbol`, `Object`,
  and concrete class-like values, while preserving Float/Double NaN behavior.
- Exact same-type `sizeof[T]` comparisons and subtraction fold to constants
  without assuming target-specific sizes for different types.
- Same-local integer subtraction is folded for typed `Int`/`Long` locals, such
  as `x - x` to typed zero; division and modulo are intentionally left intact
  because zero operands must preserve runtime failure behavior.
- Same-local Boolean idempotent operations are folded for typed locals, so
  `x && x` and `x || x` collapse to `x`; same-local complements such as
  `x && !x`, `x || !x`, `x == !x`, and `x != !x` collapse to constants.
- Pure Boolean absorption identities are folded too, such as `x && (x || y)`
  and `x || (x && y)`, but only when the repeated value and the dropped operand
  are both proven pure typed Booleans. Complemented absorption such as
  `x && (!x || y)` and `x || (!x && y)` removes only the pure complement and
  keeps the pure remainder.
- The same conservative identities now apply to identical pure structured
  operands when the result is non-floating and trap-free, such as
  `(x + 1) - (x + 1)`, `(x + 1) == (x + 1)`, repeated pure Boolean tests, and
  pure Boolean complements like `test && !test` / `test || !test`. Division and
  modulo by the same structured operand remain runtime operations because zero
  still has to trap.
- Redundant `if` values are canonicalized when safe: Boolean result branches
  such as `if (x) true else false` become `x`, and `if (x) false else true`
  becomes `!x`, even for effectful conditions because the condition value is
  still evaluated exactly once. Identical branches collapse only when the
  condition is pure enough to drop. Pure Boolean conditions also fold branch
  identities such as `if (x) x else false`, `if (x) false else x`, and their
  negated counterparts, including branch identities exposed through a negated
  condition such as `if (!x) x else true`. Boolean branches with one literal side
  are also lowered to short-circuit operators, such as
  `if (x) y else false` to `x && y` and `if (x) true else y` to `x || y`, while
  preserving negated-condition forms.
- Constant-false structured loops such as `while(false, body)` collapse to
  `unit`, since the loop body is unreachable and the NIR result type is Unit.
- The local simplification pass also folds conservative `Int`, `Long`, `Float`,
  `Double`, and `Boolean` identity operations such as `x + 0`, `x * 1`,
  `x * 1.0`, `x / 1.0`, `x && true`, and `x || false` when local type
  information proves the retained operand type; floating zero identities are
  intentionally left intact to preserve signed-zero behavior.
- Integer negation identities are canonicalized for typed `Int`/`Long` values
  as well, including `0 - x`, `x * -1`, and `-1 * x`, while keeping
  division/modulo identities conservative.
- Absorbing identities such as `x * 0`, `0 * x`, `x % 1`, `x && false`, and
  `x || true` are folded when the dropped operand is proven pure and the
  retained result type is known.
- Boolean comparisons against literals are canonicalized for typed Boolean
  values, so forms such as `x == true`, `x == false`, `x != true`, and
  `x != false` collapse to `x` or `!x`; already-negated operands cancel cleanly
  instead of leaving transient `!(!x)` shapes. Negated equality and inequality
  wrappers such as `!(a == b)` and `!(a != b)` are also inverted directly, as
  are negated `Int`/`Long`/`Char` ordering comparisons where floating NaN
  behavior is not involved. Pure same-type `Int`/`Long`/`Char` `>` and `>=`
  comparisons are direction-normalized to `<` and `<=` by swapping operands.
- Pure Boolean De Morgan forms are canonicalized as well, so typed expressions
  such as `!(a && b)` and `!(a || b)` become `!a || !b` and `!a && !b` only
  when both operands are proven side-effect-free.
- Unary identities are also canonicalized for typed values, including `+x` on
  `Int`/`Long`/`Float`/`Double` values, numeric double negation `-(-x)` for
  `Int`/`Long`, and double negation `!!x` on `Boolean` values.
- Unary literal folding now includes `Float`/`Double` literals as well as
  integer literals, preserving textual signed zero forms such as `-0.0` in NIR.
- The local simplification pass also removes immediate redundant primitive
  boxing pairs such as `unbox[Int](box[Int](value))`, and exact immutable locals
  bound to pure boxed values can be unboxed directly as well, preserving the
  conservative boxing ABI while eliminating boxes that provably cannot escape.
- A first conservative direct-call inlining hook now folds exact calls to tiny
  straight-line functions shaped as `param*`, pure `let*`, then `return`. It
  substitutes only pure arguments, and only drops an unused impure argument when
  it is proven to be a discardable empty-class allocation. Identity-style
  summaries such as `ret %param` may forward one non-pure argument without
  duplicating it, which exposes later local folds while preserving the argument
  evaluation. Effectful wrappers and effectful ignored arguments remain visible
  for later summary-based inlining. The fold pass now rebuilds summaries to a
  fixed point, so newly exposed pure wrappers and local bindings whose values
  become pure can inline during the same optimization run.
- Interflow now runs a second checked fold/DCE/block-simplify cycle after the
  first cleanup pass. Helpers that only become straight-line after block
  simplification can still inline and fold before post-inline pruning removes
  the now-unused helper definition.
- A first exact/monomorphic receiver devirtualization hook rewrites method
  selections and selected calls on immutable locals bound to fresh `new` class
  instances, plus concrete class/module receiver types with no known concrete
  subtype in the linked program, into direct function calls. This exposes simple
  receiver methods to the existing direct-call inliner while leaving base-typed
  receivers with known children and other non-exact dispatch paths dynamic.
- Exact constructor-field reads now fold for immutable locals bound to fresh
  class allocations when the selected own constructor field still has a pure
  tracked value. Exact field assignments refresh that local field snapshot for
  later reads. Calls now invalidate only the tracked fresh-object snapshots that
  flow into the call as receivers, arguments, or reachable constructor-field
  contents, while uncertain assignments and mutable-local escapes invalidate the
  affected exact objects until escape/effect analysis becomes stronger. Direct
  reads such as `new X(args).field` also fold to the matching constructor
  argument when the allocation has only own constructor-field storage and
  trivial initialization. The direct fold captures the selected argument at its
  original position and retains all other argument effects in order. Local
  fresh-object snapshots
  now also summarize simple `$init` assignments to `this`/`super` fields, so
  inherited constructor fields and pure class-body initializer fields can fold
  after allocation without removing initialization. Source-level example and
  smoke coverage now exercise inherited `super` field assignment, initializer
  fields, the unrelated-call case, which may keep folding, and the object-passing
  call case, which must not fold stale field contents.
- Dead-local cleanup now also removes unused top-level and nested block-local
  fresh class allocations when constructor arguments are pure and any `$init`
  body is a simple sequence of pure assignments into `this`/`super` fields,
  including empty classes and classes whose folded field reads left only a dead
  allocation shell. Private nested object graphs are removed as well when each
  constructor argument allocation is independently proven discardable. For
  classes with only direct constructor-field storage and trivial or simple
  pure field-assignment initialization, an unused outer shell is also removed
  while effectful constructor arguments remain in evaluation order. Object
  escapes, non-simple initializers, and remaining field assignments keep their
  allocations visible. Unused structured `if` values with pure conditions are
  also removed when both branches independently satisfy the same pure/allocation
  proof, while an effectful allocation branch keeps the conditional evaluation.
  When both branches are discardable but the condition is effectful, dead
  binding and `eval` cleanup retains only the condition effect. Effect-only
  extraction applies recursively through nested allocation operands and retained
  conditional branches, independent of the number of cleanup cycles.
  When just one branch is discardable, cleanup replaces that branch with `Unit`
  while retaining the conditional and the other branch's evaluation effects.
  Private exact field assignments into such fresh objects are also removed when
  the assigned value is pure and the object has no aliases or remaining uses, so
  folded field-update reads can shed their final mutation shell; later shadowed
  locals with the same name do not count as remaining uses of the fresh object.
  Effectful field assignments remain visible. It also removes unused literal
  array allocation shells: pure and independently discardable fresh-allocation
  elements disappear, while calls and effectful element constructors remain in
  their original evaluation order. This trims the allocation shells left behind
  after exact receiver devirtualization, field-read folding, and array folds
  without dropping constructor, field-initializer, field assignment, or
  element-evaluation effects.
- Post-inline pruning now refreshes direct reachability after interflow, allowing
  now-unused pure helper functions and unused plain function declarations to
  disappear while preserving directly referenced declarations, non-inlineable
  function definitions, and class/trait-owned dispatch methods from the linker's
  original closed-world result.
- Exact boxed `scala.scalanative.runtime.anyEquals` calls now fold for pure boxed
  ABI values, including immutable locals bound to those boxes; matching boxed
  payload literals compare locally, Unit boxes compare true, disjoint boxed ABI
  types compare false, Float payloads compare after runtime-width rounding,
  exact same-literal string constants compare by their interned runtime
  identity, and exact null-vs-null or null-vs-known pure non-null reference
  cases, such as boxes and string literals, fold without entering the runtime
  equality helper. Alias-aware equality now also folds exact immutable
  non-null references that resolve to the same local root, including
  `anyEquals(alias, original)` shapes after propagation, while distinct
  fresh `new` object roots fold to false for equality and true for inequality.
  Direct binary comparisons and direct `anyEquals` calls between two fresh
  allocations fold similarly while retaining both allocation evaluations in
  left-to-right order before the Boolean result, allowing effect-aware DCE to
  trim only safe shells. Direct `anyEquals` between exact null and a fresh
  allocation likewise folds to false while preserving argument evaluation.
  Exact immutable locals already bound to fresh objects or arrays also fold
  against null after their allocation and initialization have been evaluated.
  Direct and immutable-local effectful boxes use the same non-null proof while
  retaining payload evaluation. `anyEquals` between exact disjoint effectful box
  types also folds to false after evaluating both payloads in argument order;
  two exact effectful Unit boxes similarly fold to true because their payload
  value is uniquely known, while both payload evaluations remain ordered.
  `cpp-examples/InterflowOptimizations.scala` now exercises effectful boxed Unit
  equality, hash, `toString`, and string concatenation through source lowering;
  optimized BuildDriver smoke coverage asserts the resulting effect-only calls
  and constants directly in emitted NIR, and verifies that every reported core
  optimization pass remains validation-clean before and after it runs.
  Boxes and strings are kept on their content/value-specific paths rather than
  treated as merely distinct references.
- Exact runtime hash helpers now fold when the ABI result is fully known:
  Boolean, Long, Float, Double, Char, String, and Symbol helper calls fold for
  literals and immutable locals, with Float/Double hashes preserving the runtime
  bitcast behavior for forms such as signed zero; `anyHashCode` folds exact null
  and pure boxed Unit/Boolean/Int/Long/Float/Double/Char/String/Symbol values
  through cycle-safe immutable local alias chains. Direct and immutable-local
  effectful Unit boxes also fold to hash `0` while retaining payload evaluation;
  dynamic effectful boxes remain runtime hashes. Same-reference comparisons of
  `anyHashCode` calls now fold to equality/inequality constants for exact
  immutable non-null reference aliases, while ordinary object identity hash
  values and distinct-reference comparisons remain runtime operations.
- Exact `scala.scalanative.runtime.stringLength` calls fold for decoded string
  literals and immutable locals bound to those literals, matching the current
  `strlen`-based runtime ABI while leaving null and dynamic strings untouched.
- Exact runtime array length helpers now fold for immutable locals bound to
  literal `new Array [...]` values, including reference array helper names with
  their element-type suffix. Direct fresh arrays fold as well by replacing the
  allocation with ordered element evaluation followed by the constant length,
  allowing later DCE to remove pure elements while retaining effects. Parameter
  arrays stay as runtime calls.
- Exact runtime array apply helpers now fold in-bounds reads from immutable
  locals bound to literal arrays when the selected element is pure enough to
  reuse. Direct fresh arrays with an exact in-bounds index also fold by evaluating
  every element in order, capturing the selected value at its original position,
  and returning it after later element effects. The local fold is mutation-aware:
  array updates and possible mutating calls invalidate exact array contents,
  while parameter arrays, out-of-bounds indices, and uncertain array contents
  remain runtime reads.
- Exact in-bounds runtime array updates now refine that local array-content
  snapshot when the updated value is pure, including reference arrays such as
  `Array[Any]`, so later exact reads can fold to the updated element while the
  effectful update call itself remains in optimized NIR. Uncertain updates now
  invalidate exact element contents while preserving known array shape, so later
  length reads can still fold safely.
- Dead-update cleanup removes an exact in-bounds runtime array update when the
  target is still a private literal array, the assigned value is pure, and the
  array has no remaining uses after the update. Later shadowed locals with the
  same name do not keep the private update alive. Dynamic indices, effectful
  assigned values, and arrays whose contents have escaped or become unknown keep
  the runtime update. Exact in-bounds updates on a direct fresh array also remove
  the array and store while retaining initial element, index, and assigned-value
  evaluation in call order. Out-of-bounds and dynamic direct updates stay at
  runtime so their checks remain observable.
- Exact runtime `toString` helpers now fold when their string result is
  ABI-known: Int, Long, Boolean, and Char primitive helpers, plus `anyToString`
  for null, exact string literals, and pure boxed
  Unit/Boolean/Int/Long/Char/String/Symbol values. Floating and ordinary object
  `toString` paths stay at runtime for formatting and dynamic dispatch fidelity.
  Direct and immutable-local effectful Unit boxes also fold to `"()"` while
  retaining payload evaluation; dynamic effectful boxes remain runtime calls.
- Exact runtime format helpers now fold only for ABI-stable pass-through
  formats: `format("%s", literalString)`, `format("%c", literalChar)`, and
  `formatBoolean("%s", literalBoolean)`, including immutable local alias chains.
  Source-level f-interpolation now has optimized NIR coverage for those paths,
  including `%b` holes after parser normalization to the Boolean `%s` helper.
  Width, padding, numeric, floating, dynamic format strings, and other
  libc-sensitive formatting paths remain runtime calls.
- Exact binary null comparisons now fold for null-vs-null and pure known
  non-null reference values such as boxed ABI values and string literals,
  including immutable locals where the comparison can be proven locally. A
  direct fresh allocation compared with exact null also folds while retaining
  both operand evaluations before the known Boolean result. Immutable locals
  bound to fresh allocations fold too, without removing effectful initialization.
  Exact effectful boxes likewise fold against null while their payload effects
  remain visible.
- The local simplification pass canonicalizes redundant reference casts by
  erasing exact same-type `asInstanceOf[T]` wrappers, including operands whose
  type is known through local type tracking, collapsing duplicate same-target
  casts, erasing hierarchy-safe child-to-parent casts using linked class/trait
  metadata, and erasing exact pure non-null reference upcasts to top `Object`.
  Fresh arrays and immutable locals with exact array types now use the same
  redundant-upcast path for `asInstanceOf[Object]`, retaining the array value and
  all element evaluation while removing only the cast wrapper.
  Direct `null.asInstanceOf[T]` reference casts also collapse back to `null`,
  and aliases of null-derived casts now propagate before folding and DCE remove
  the temporary binding.
- Null type tests are folded locally as well: direct or propagated
  `null.isInstanceOf[T]` checks, including aliases of such tests, become `false`
  without touching non-null runtime hierarchy tests.
- Exact non-null type tests are folded for current ABI-proven values, such as
  `box[T](value).isInstanceOf[T]`, immutable locals bound to those exact boxes,
  and bare `String` literals tested against `String`. Disjoint exact ABI tests
  such as a known `box[String](value).isInstanceOf[Symbol]` or bare
  `"Scala".isInstanceOf[Symbol]` fold to `false`; exact pure non-null reference
  values also fold positively when tested against top `Object`. Fresh class
  allocations fold linked-hierarchy positive and negative tests while first
  retaining allocation evaluation as a preceding block operand; effect-aware
  DCE can then remove a safe shell, preserve only constructor-argument effects,
  or retain an allocation with an effectful initializer. Direct fresh arrays
  and immutable locals bound to them now fold positively against top `Object`
  as well, preserving element evaluation before removing a dead array shell.
  Effectful boxed ABI values retain payload evaluation while exact positive,
  top-`Object`, and disjoint boxed type tests fold around the evaluated box.
  Immutable locals already bound to fresh class allocations now also fold
  linked-hierarchy positive and negative tests while preserving the evaluated
  allocation for later DCE/effect checks.
- A conservative local DCE pass removes unused top-level pure `let`/`var`
  bindings, pure `eval` instructions whose results are discarded, non-final
  nested block-local pure bindings, and non-final pure block operands after
  propagation and folding. Its purity model is intentionally narrow:
  literal/local arithmetic, comparisons, pure-receiver type tests, pure boxed
  values whose payloads are pure, `if`, `sizeof`, and Unit are removable; calls,
  allocations, boxes around effectful values, unboxes, casts, field reads,
  assignments, final block results, loops, and zone scopes are retained until
  escape/effect analysis is stronger. Unused block-local bindings with effectful
  initializers are unwrapped to their initializer in effect-only positions, so
  the binding shell disappears without dropping the initializer effect. Unused
  top-level `let`/`var` bindings with retained initializer effects are similarly
  demoted to `eval` instructions.
- A conservative block simplification pass now collapses empty blocks, single
  expression blocks, and pure local binding shells such as
  `block(let %x : Int = 3; %x)` or `block(var %x : Int = 3; %x)` after
  propagation, folding, and DCE have made the structure obviously effect-free,
  and final block-local binding statements normalize to initializer effects plus
  `Unit` when the enclosing block result is returned directly. Block result
  metadata now consistently treats a final local binding as `Unit` rather than
  reusing the bound local's declared type. The pass also flattens final nested
  block values into the enclosing block where no
  later operands can observe inner binding scope. Non-final nested block operands
  are also flattened when their own top-level operands contain no local bindings,
  preserving effect order without widening local scope.
  It also removes discardable pure operands, such as literals, locals, `unit`,
  `sizeof`, pure unary/binary expressions, pure `if` expressions, pure boxes, and
  pure type tests, when nested block simplification exposes them before a final
  result value. It now also removes `eval` instructions and unused top-level
  `let`/`var` bindings whose nested block values collapse to discardable pure
  values during the same pass. Discarded effect-transparent unary, box, and type
  test wrappers unwrap to their operand so effectful operand evaluation is kept
  without retaining a now-unused wrapper result. Discarded non-trapping binary
  comparisons similarly unwrap to ordered operand evaluation. Discarded
  short-circuit logical binaries now lower to effect-only `if` expressions, so
  the right-hand side keeps its original conditional execution. Discarded
  effectful `if` branches also trim each unused branch result to `Unit` while
  keeping branch-local effects. Every structured `while` body is normalized to
  an effect-only `Unit` result because its value is inherently discarded on each
  iteration, including loops returned directly from `Unit` functions. Discarded
  `Zone.scoped` bodies similarly keep scope entry/exit while
  dropping unused scoped result values. Arithmetic and other potentially
  semantically significant binary results stay intact.
  Single-use pure block-local `let` bindings can now be inlined into a pure final
  block expression without duplicating effects or touching mutable locals,
  including adjacent pure `let` chains where each binding is consumed exactly
  once by the next pure binding. Literal-like block-local `let` bindings can
  also inline into their sole later use across intervening effect-only operands,
  including effectful non-final uses exposed after an unassigned local `var` is
  promoted. Bindings that read locals stay in place so later mutable assignments
  cannot change the captured value. Adjacent single-use top-level
  pure `let` bindings are also inlined into the next pure value when the
  replacement locals are known immutable and cannot be captured by nested local
  bindings. Single-use literal-like top-level `let` bindings can additionally
  inline across intervening effectful instructions; this also removes shells
  exposed when an unassigned top-level `var` is promoted after constant
  propagation. The local-use replacement helper follows the same nested block
  shadowing rules as visible-use counting.
- Top-level local `var` instructions and block-local `var` bindings whose
  remaining optimized scope has no visible assignment target are promoted to
  `let` during block simplification. The later checked fold/DCE cycle can then
  propagate and remove shells left behind by erased control flow, while vars with
  real assignments remain mutable. Shadowed nested assignments do not block an
  unrelated outer local from being promoted, and shadowed nested uses do not keep
  that promoted outer binding alive.
- The reachability pass prunes unreachable function declarations and definitions
  using `LinkedProgram::reachableGlobals`, while retaining module/class/trait
  metadata definitions.
- The build driver reports removed definition counts when `--optimize` is used.
- The active native smoke suite builds the same representative arithmetic and
  control-flow source with Interflow disabled and enabled, runs both binaries,
  and requires identical known output while also confirming that Interflow
  reported changes and produced different lowered LLVM IR. This directly guards
  the Phase 10 behavioral equivalence exit criterion in debug and release
  configurations.

Phase boundary status:

- Phase 10 meets its exit criterion for the currently supported frontend and NIR
  surface: all optimizer passes are verifier-checked, optimized and unoptimized
  native execution agree in the active suite, and the representative full-source
  workload remains reportable and checker-clean.
- The queue below contains incremental optimizer improvements rather than
  blockers for beginning focused Phase 11 work. Interflow coverage should keep
  expanding alongside any new NIR forms introduced there.

Next queue items:

1. Broaden local canonicalization for additional structured expression values.
2. Extend DCE through effect/escape-aware value tracking.
3. Keep running verifier validation before and after every new pass.

## Phase 11: tools/codegen

Goal: Lower linked NIR into LLVM IR.

Scope boundary: source generics, variance, Scala 2 implicit resolution, and
Scala 3 `given`/`using` resolution belong to the parser/typechecker/NIR-lowering
phases. Codegen handles only their already-resolved runtime consequences, such
as erased signatures, bridge methods, boxed generic values, trait dispatch, and
explicitly selected specializations represented in linked NIR.

Deliverables:

- LLVM context/module management through LLVM C++ APIs.
- NIR-to-LLVM type lowering.
- Object layout, method tables, trait tables, array layout, string constants,
  globals, and runtime metadata.
- Function emission for blocks, values, instructions, and control flow.
- Exception lowering and landing pads or platform-specific unwinding strategy.
- GC root metadata, safepoints/yieldpoints, and stack map integration.
- Debug information where source positions are available.

Implementation steps:

1. Emit LLVM IR for primitives, functions, calls, branches, loads, stores, and
   returns.
2. Add object/class/module layouts and allocation calls.
3. Add dispatch tables and virtual calls.
4. Add exception handling.
5. Add GC metadata and safepoints.
6. Add debug info and optimization-level configuration.

Exit criteria:

- Codegen emits valid LLVM IR that `opt`, `llc`, or `clang` accepts for the
  supported subset.

Current scaffold status:

- Codegen emits one LLVM function per NIR `define`, using sanitized global names.
- Simple NIR signatures lower to LLVM parameter and return types for `Unit`,
  `Boolean`, `Int`, `Long`, `Float`, `Double`, and pointer-like fallback types.
- The current lowering consumes structured NIR values/instructions directly
  rather than parsing textual NIR body lines.
- The current lowering handles integer literals, known parameters, simple
  integer arithmetic, boolean `if` expressions as LLVM `select`, integer
  block-local `let` values, same-owner method calls, and same-module object
  member selections.
- Mutable `Unit` locals lower as effect-only bindings rather than impossible
  LLVM `void` storage: initializers and later assignments retain their ordered
  effects, while reads produce the unique Unit value without emitting a load.
- Codegen resolves inherited non-virtual method selections through parent
  metadata, so a NIR selection such as `%counter.zero` can lower to a call of
  `BaseCounter.zero` when `counter` has subclass type `FancyCounter`.
- Codegen resolves static overrides to the nearest subclass method, so
  `%score.value` on static type `FancyScore` lowers to `FancyScore.value` even
  when `BaseScore.value` also exists.
- Codegen emits closed-world vtables for method-bearing class hierarchies and
  lowers base-typed receiver selections through a loaded vtable slot when the
  receiver may be a subtype.
- Codegen includes traits in the closed-world slot layout graph, so
  `def show(x: Named) = x.name` can dispatch to a concrete class override when
  `Named` is inherited directly or through a composed trait list.
- Virtual method names receive stable closed-world slot indices across all
  class/trait views, allowing unrelated trait interfaces to share one concrete
  class vtable. A shared right-biased C3-style order selects implementations in
  diamond hierarchies without allowing a repeated root implementation to
  overwrite a nearer branch override. Plain `super` inside a trait is now
  stackable: NIR preserves the lexical trait owner and concrete class vtables
  provide hidden `(trait, member)` slots that point to the next implementation
  in that class's linearization.
- Abstract trait methods reserve slots without emitting an allocatable trait
  vtable. Concrete child-class vtables must replace those slots with method
  definitions; inherited default methods can themselves dispatch abstract calls.
- Abstract trait values use the same virtual slot machinery as zero-argument
  methods. Concrete `override val` getters load their class backing fields, and
  optimized linking retains both getter targets and storage definitions.
- Constructor `val` getters participate in the same slots. Their allocation
  operands initialize `$field` storage directly, and child initializers can
  forward parent constructor arguments into inherited accessor storage.
- Initialized trait `val` getters also use ordinary virtual slots. Their backing
  fields are initialized once per object in root-to-mixin order, while child
  classes inherit both the storage layout and getter implementation.
- Shadowed initialized values keep owner-qualified fields and hidden direct
  getters. Public dispatch remains right-biased, while qualified
  `super[T].value` resolves the requested trait's stored value without adding
  hidden getters to the public virtual slot table.
- Trait variables add a setter slot beside the getter slot. Trait-typed
  assignments dispatch `name_$eq` virtually, while owner-qualified
  `super[T].variable = value` calls the matching hidden setter directly so
  shadowed mutable fields remain independent.
- Intermediate trait defaults and concrete implementations can fill different
  slots from the same root trait, including optimized builds where all dynamic
  implementations must remain linker-reachable.
- Codegen now respects explicitly widened block-local types and avoids marking
  `val score: Base = new Child()` as an exact `Child` local, keeping virtual
  dispatch active for parent-typed selections.
- Codegen builds inherited class field layouts by copying parent field offsets
  before child fields. Parent constructor argument assignments run before
  inherited parent body-field initializers, which run before child initializers.
- Codegen lowers explicit `super` selections to direct parent field/method
  symbols while keeping the object pointer as `%this`, so child overrides can
  call parent implementations without going through the vtable. For composed
  parents, plain `super` searches right-to-left and recursively through parent
  traits for the requested concrete member; qualified `super[T]` remains
  available when a specific direct parent is required.
- Plain trait `super` calls lower through hidden stable vtable slots rather than
  fixed parent symbols. This allows `Root with AddTwo with AddTen` to execute
  `AddTen -> AddTwo -> Root` using one shared trait method body per trait, and
  remains correct when interflow optimization is enabled.
- Runtime ABI 6 gives every concrete class, module singleton, and boxed primitive
  the same
  descriptor-shaped object header. Descriptors record kind, type ID, instance
  and payload layout, readable type name, optional vtable, slot count, and a
  C3-ordered transitive ancestry table. They now also carry inherited
  reference-field offset maps and a default ownership policy. Traits receive
  non-allocatable descriptors so class ancestry can refer to both class and
  interface identity. ABI 6 adds boxed String as a pointer-payload boxed
  primitive for the `Any`/`Object` ABI.
  Concrete class IDs are deterministic within the linked closed world, while
  `Boolean`, `Int`, `Long`, `Float`, `Double`, and `Char` retain fixed scalar
  tags. Virtual and stackable-trait dispatch load their method table through the
  decoded descriptor. The one-word object header tags descriptor pointers with
  per-object `gc`, `arena`, or `immortal` ownership. Class allocations outside
  a scoped zone are zero-initialized and registered in a non-moving mark/sweep
  heap, module instances are immortal roots, and boxed primitive temporaries
  use the active zone or a program-lifetime bump arena. LLVM tracing helpers
  expose each object's
  reference count and child references. A shared `is_instance_of` helper handles
  exact identity, transitive ancestry, and null; scalar-specific unbox helpers
  use it and trap before loading a mismatched payload.
- The compiler-known `sizeof[T]` intrinsic is now a compile-time `Int` value
  that passes through source typing, NIR, verification, linking, and LLVM
  lowering. It supports `Unit` (`0`), scalar primitive payloads, and concrete
  classes. Class results use the emitted ABI instance size, including the
  descriptor header and inherited fields. Traits, module singletons, strings,
  and unresolved types are rejected until their runtime layouts have a defined
  allocation contract. A `typeof` surface remains deliberately deferred in
  favor of a runtime-metadata design such as `classOf`.
- The target-runtime collector marks recursively from initialized module slots
  and active per-function shadow frames, terminates on cyclic class graphs, and
  sweeps unmarked GC allocations both during execution and after Scala `main`
  returns. Reference parameters and local `val`/`var` bindings receive stable
  shadow slots, mutable assignments refresh their slot, and normal returns pop
  the frame. The compiler-known `gcCollect()` intrinsic provides an exact
  collection point for runtime testing until the standard runtime API is wired.
  Arenas now grow through linked, zeroing, 8-byte-aligned bump blocks. The
  program arena begins at 1 MiB and doubles as needed; destruction walks and
  releases every block. The compiler-known `Zone.scoped({ body })`
  intrinsic creates a 64 KiB scoped arena, supports nesting by saving the
  previous active zone, routes class and boxed allocations through that zone,
  and destroys it on normal scope exit. NIR carries an explicit `zone-scoped`
  value so linking and optimization preserve the lifetime boundary. Scoped
  bodies now retain ordered side effects plus lexical `val` and `var` bindings,
  including mutable assignment, nested zones, and shadowing. NIR represents
  these bodies with structured `block`, `local-let`, and `local-var` values;
  verification and linking enter and leave matching local scopes, while LLVM
  gives repeated bindings unique SSA names and restores outer bindings after a
  nested block. Typecheck and NIR verification reject direct reference-valued
  results, while Unit and primitive results may leave the scope. The frontend
  also tracks arena-reference provenance through scoped aliases and
  reference-valued selections. It permits links between two zone-owned objects,
  but rejects storing a zone reference into an outer local or non-zone field and
  rejects passing one as an ordinary call argument. The NIR verifier repeats
  these structural checks and derives the same closed-world receiver summaries,
  so transformed or hand-built NIR cannot bypass the source-level analysis.
  A closed-world fixed point extends direct method effects through calls on
  `this` or its local aliases, including implicit receiver calls, and marks a
  base method unsafe when a reachable class or trait override can leak its
  receiver. A zone-owned reference therefore cannot invoke a directly,
  transitively, or virtually leaking method. The stdlib callback shape, library
  and cross-module effect contracts, handled-exception unwinding, and
  liveness-aware stack maps remain future work. Generated shadow frames now
  include conservative slots for reference-valued temporaries, and a newly
  allocated object is rooted before its constructor arguments or initializer
  body can allocate. This makes the existing non-moving collector safe at
  generated allocation safepoints even for nested constructor expressions.
  GC-owned class allocation polls before allocating; the default threshold is
  64 tracked objects, while Zone and program-arena allocations do not poll.
  `gcCollect()` remains an exact explicit collection point, and
  `gcSetCollectionThreshold(n: Long)` changes the runtime threshold (with zero
  clamped to one) for diagnostics and stress testing. Temporary roots currently
  remain live until function return, so precise liveness still remains future
  optimization work.
  The read-only diagnostic intrinsics `gcLiveObjectCount(): Long` and
  `gcCollectionCount(): Long` expose the current GC-owned object-list size and
  the number of completed collections, respectively. They allow native tests
  and future runtime diagnostics to observe reclamation without making a
  collection policy decision part of user program semantics.
  Raw C-string placeholders are not traced until strings become header-bearing
  objects.
- Every NIR module definition now receives a non-null immortal singleton slot,
  a module descriptor, and a lazy accessor. Object-owned functions initialize
  their owner on entry, and module values lower to the same accessor pointer even
  when passed through typed locals and parameters. A deterministic global table
  contains the addresses of all singleton slots so collector startup can treat
  initialized modules as roots. Object `val` and `var` members now occupy the
  singleton payload, expose zero-receiver getter/setter functions, and initialize
  once in source order through a generated `$init`. Object-body expressions run
  in the same initializer, and later fields can read earlier stored fields.
  Publishing the singleton slot before `$init` gives recursive references stable
  identity. Object members still use the existing static function ABI; cycle
  diagnostics and thread-safe publication remain future work.
- Unsupported expression bodies remain visible as LLVM comments for inspection,
  but `LlvmCodegen` now returns failed structured errors carrying the original
  NIR source span and naming each owning function and unsupported instruction.
  BuildDriver turns those entries into located diagnostics and stops before
  writing an artifact or invoking the native toolchain, so reachable unsupported
  NIR can no longer silently ship with a conservative default value.
- Source-backed codegen now emits the first LLVM debug-information layer:
  per-source compile units and Scala files plus source-line `DISubprogram`
  scopes attached to every reachable NIR function definition. Compile units use
  LLVM's portable C++ language tag because LLVM has no Scala DWARF language
  token; filenames, source lines, Scala display names, and sanitized LLVM
  linkage names remain native to the input program. Metadata also records
  whether Interflow optimization ran, while direct codegen without a
  `SourceManager` remains valid and metadata-free.
- Source-backed non-parameter NIR instructions now receive line-and-column
  `DILocation` nodes scoped to their owning `DISubprogram`. Codegen emits a
  location-bearing `llvm.donothing` marker before each instruction; LLVM removes
  the intrinsic from machine code while preserving a stable debugger stepping
  anchor without coupling debug metadata to individual lowering helpers.
- Function debug scopes now carry typed return/parameter tuples instead of an
  empty placeholder signature. Scala numeric and Boolean primitives use DWARF
  basic types, while reference-like runtime values use named pointer types.
  Source parameters and immutable locals are tracked with `llvm.dbg.value`;
  mutable non-Unit locals use `llvm.dbg.declare` against their stable alloca
  slots, allowing debuggers to observe later stores. Unit locals remain
  intentionally storage-free.
- NIR instructions retain their outer-to-inner source block chain. Codegen
  deduplicates those chains into nested `DILexicalBlock` scopes and uses the
  innermost scope for instruction locations and local-variable declarations,
  preserving outer-local visibility while separating nested local lifetimes.
- `BuildOptions.debugInfo` controls debug emission and defaults to enabled. The
  CLI exposes matching `--debug-info` and `--no-debug-info` switches; disabling
  it removes compile units, scopes, locations, variable records, and all debug
  marker/value/declare intrinsics while leaving executable lowering unchanged.
- Reachable source classes and modules now emit concrete DWARF structure types.
  Their total sizes and declared member offsets come from the same finalized
  `ClassLayout`/`FieldInfo` data used by loads, stores, tracing, and allocation.
  Derived composites carry zero-offset `DW_TAG_inheritance` entries and list
  only their own physical members, so debuggers recover the source hierarchy
  without duplicating inherited fields. Traits remain named forward
  declarations. Reference signatures point through named DWARF pointer types
  to those composites.
- Build configuration now exposes explicit `O0` through `O3` optimization
  levels. `O1` runs one Interflow cleanup cycle, `O2` retains the standard
  two-cycle pipeline, and `O3` adds an aggressive propagation/folding/dead-code/
  block-simplification convergence cycle before reachability pruning. Each level
  passes the matching `-O` flag to Clang; `--optimize` and the compatibility
  `BuildOptions.optimize` boolean select `O2`. The selected effective level
  appears in build logs and optimization reports and drives the debug compile
  unit's optimized flag.
- CTest covers the full source-to-LLVM path for parameterized methods, simple
  arithmetic, calls, selections, `if`, and block-local values.
- The Phase 11 backend work is complete for the currently supported language
  surface, and Phase 12 now meets its source-to-native exit criterion.

## Phase 12: tools/build

Goal: Orchestrate the complete compiler toolchain.

Deliverables:

- CLI driver for compile, check, link, emit-nir, emit-llvm, build-object, and
  build-binary.
- Build configuration model for target triple, sysroot, optimization mode, GC
  mode, runtime libraries, link libraries, output paths, and cache directories.
- Incremental build cache based on source hashes, compiler flags, NIR versions,
  and runtime versions.
- Explicit dynamic/default and static native linkage modes, with clear
  diagnostics when a target toolchain does not provide the required static
  system archives.
- External tool discovery for LLVM/Clang/Lld.
- Build logs and machine-readable diagnostics.

Implementation steps:

1. Add command-line parsing and config loading.
2. Wire source -> NIR compilation.
3. Wire checker, linker, interflow, codegen, and native link.
4. Add cache keys and invalidation.
5. Add cross-platform toolchain discovery and linkage modes.

Exit criteria:

- One command can compile a supported Scala source file into a runnable native
  binary.

Current scaffold status:

- `cpp-tools/build` owns a `BuildAction` model for `compile`, `check`,
  `emit-nir`, `emit-llvm`, `build-object`, and `build-binary`.
- The build config records optimization mode, target triple, native sysroot,
  memory runtime mode, default/static linkage mode, platform/LLD linker
  selection, cache directory, runtime libraries, native link libraries, and
  output path.
- `--config <path>` loads a strict `schemaVersion: 1` JSON project
  configuration. It can provide the source, action, output/report paths,
  optimization/debug settings, target/sysroot/GC settings, cache directory,
  linkage modes, and runtime/link libraries. A recursive JSON parser validates
  syntax, escapes, duplicate and unknown keys, value types, enum values, and
  schema compatibility. Relative paths resolve against the configuration file,
  scalar CLI options override configured values independent of argument order,
  and CLI library inputs append to configured lists.
- The CLI supports `--compile`, `--check`, `--emit-nir`, `--emit-llvm`,
  `--build-object`, `--build-binary`, `--config`, `--output`, `--target`,
  `--sysroot`, `--gc`, `--cache-dir`, `--runtime-lib`, `--link-lib`, `--optimize`,
  `--opt-level`, `--link-mode`, `--linker`, `--static`, `--debug-info`,
  `--no-debug-info`, and `--optimization-report`, plus `--build-report` for a
  versioned JSON result.
- `BuildResult` retains source-resolved structured diagnostics in addition to
  the human-readable rendering. `--build-report <path>` serializes the effective
  build configuration, cache applicability/keys/hits, diagnostic counts and
  source ranges, fix-its, phase log, and produced artifacts. The report is
  written for successful and failed compiler runs, and the CLI prevents it from
  overwriting either the source file or selected build output.
- `--cache-dir` now enables a versioned incremental artifact cache. Stable keys
  cover normalized source content and source identity, artifact kind,
  optimization/debug/target/GC flags, compiler and NIR format versions, runtime
  ABI, and backend identity. Validated post-Interflow NIR, generated LLVM IR,
  and native object bytes are stored with manifest-last publication. Object
  keys hash the LLVM contents together with the Clang identity, optimization
  level, target, and normalized sysroot; warm native builds therefore skip
  backend compilation but can also skip linking. Ordered runtime/link inputs
  receive a separate stable
  link fingerprint, using canonical path, size, modification time, and content
  hash for direct file inputs. Bare library names are resolved through the
  selected Clang and target so fingerprints name the actual shared object or
  archive. For final executables, Clang's normalized `-###` link plan contributes
  the linker executable, startup/shutdown objects, dynamic loader, search paths,
  implicit compiler runtimes, system libraries, and every effective linker flag.
  Temporary object/output paths are replaced with stable placeholders. Link mode
  and linker selection are part of the link fingerprint but not the reusable
  object key. Optimization reports are restored with optimized cache entries,
  malformed or incomplete entries degrade to ordinary cache misses, and an
  unresolved implicit plan disables only final-binary caching rather than the
  build.
- `emit-nir`, `emit-llvm`, and the default compile scaffold can write produced
  artifacts to disk and record them in the build result.
- Native toolchain discovery finds `clang` and LLD from `PATH`, with
  `CPP_SCALANATIVE_CLANG` and `CPP_SCALANATIVE_LLD` providing explicit paths.
- `--target` and `--sysroot` are forwarded consistently to native compilation,
  target-library probes, Clang link-plan inspection, and the final link. Missing
  or non-directory sysroots are diagnosed before frontend or native work starts.
  Sysroot selection is excluded from reusable frontend LLVM keys but included
  in native object and executable cache identities.
- Native builds with explicit target, sysroot, static, or LLD settings run a
  capability preflight before frontend compilation. Clang must normalize the
  requested triple and produce a valid linker plan; the plan inspector resolves
  the selected linker, startup/shutdown objects, dynamic loader, direct files,
  and libraries relative to the configured sysroot and toolchain search paths.
  GNU-style linker emulation is probed directly, catching cases such as a host
  `ld` that cannot link the requested architecture. Missing target runtime inputs
  are reported together in one diagnostic.
- Native binary builds use Clang's platform linker by default. `--linker lld`
  passes the discovered LLD executable through Clang's `-fuse-ld` interface and
  diagnoses a missing LLD installation before compilation or linking.
- Native binary builds default to ordinary platform linkage. `--static` and
  `--link-mode static` select fully static linkage and add Clang's `-static`
  option. A preflight query resolves `libc`, `libm`, and user-provided bare
  libraries to target-specific archives; missing archives produce a focused
  diagnostic before linker invocation. Explicit shared-library paths are
  rejected as static inputs, while archives and relocatable objects are valid.
- `build-object` writes an intermediate LLVM IR file and invokes `clang -c` for
  the supported LLVM subset. With caching enabled, subsequent builds restore the
  binary object directly when LLVM and backend fingerprints match.
- `build-binary` writes an intermediate LLVM IR file and invokes `clang` as the
  platform linker driver. Cached builds compile LLVM to a reusable intermediate
  object. Once the complete implicit link plan has a valid fingerprint, the
  resulting executable is cached as binary data and restored with executable
  permissions on a warm build. Runtime library and link library options are
  passed through and included in the link fingerprint.
- Generated C `main` now calls `scala.scalanative.runtime.main` when the NIR
  bridge exists, so smoke binaries execute the discovered zero-argument Scala
  main path.
- Phase 12 meets its exit criterion: one CLI command can compile a supported
  Scala source file into a runnable native binary, with incremental artifacts,
  native linkage controls, reports, and persistent configuration participating
  in that path. Multiple source compilation is intentionally deferred until the
  frontend has cross-unit symbol ownership and typechecking rather than exposing
  a build-only loop with incomplete Scala semantics. The active implementation
  queue has moved to Phase 13.

## Phase 13: Native Binary and Runtime Interface

Goal: Produce runnable native programs with the expected Scala Native runtime
semantics.

Deliverables:

- Runtime startup and shutdown.
- Main method discovery and invocation.
- Module initialization protocol.
- Hybrid GC/arena memory runtime surface.
- Object allocation, arrays, strings, type metadata, casts, exceptions, and
  stack traces.
- C ABI interop layer for external/native calls.
- Staged unsafe/native intrinsics and a precisely specified type-query facility
  (`classOf`/runtime metadata first; any non-standard `typeof` syntax only
  after its compile-time semantics are defined).

Implementation steps:

1. Define runtime ABI headers used by generated LLVM IR.
2. Implement startup, allocation, strings, arrays, and main invocation.
3. Add GC root registration and safepoints.
4. Add exception support.
5. Add platform libraries and native linking rules.

Exit criteria:

- The produced executable runs integration tests without depending on the old
  Scala Native compiler implementation.

Current scaffold status:

- Runtime ABI 7 introduces an explicit idempotent lifecycle around every
  generated entrypoint. Startup establishes the collector counters, default
  threshold, root stack, program arena, and zone state before argument
  conversion or Scala code executes. Shutdown transitions the runtime once,
  performs the final collection, destroys program-lifetime arena storage, and
  clears transient root/zone state. The ABI bump invalidates cached LLVM and
  native artifacts produced under the earlier implicit-global lifecycle.
- Runtime ABI 8 and NIR text format 3 add the first uncaught-exception path.
  `throw` is tokenized and typed as the bottom type `Nothing`; primitive
  operands are rejected. A final method-body throw becomes a verified NIR
  terminator, while nested throws are represented as verified bottom-typed NIR
  values inside branches, blocks, call arguments, loops, and scoped zones.
  Verifier, linker, Interflow, and codegen type inference all preserve the
  surviving branch type when the other branch is `Nothing`; Interflow treats
  throws as effectful and non-discardable. Codegen evaluates and roots the
  exception object, restores the throwing function's shadow frame, and calls a
  non-returning runtime helper. Nested sites end in `unreachable` and continue
  in an uninhabited compiler-only block so surrounding LLVM phi nodes and
  expression shapes remain valid without making execution continue.
  Active zone arenas now carry a link to the previous zone, allowing abnormal
  shutdown to destroy every nested scoped arena in order without an additional
  allocation per scope, before final GC/program-arena cleanup, a C stream flush,
  and native `abort`. Optimized smoke coverage executes both a direct throw
  through two nested zones and nested throws embedded in structured expressions;
  normal branches retain their values, output produced before the selected throw
  is flushed, active zones are reclaimed, and code after the throw is not
  reached. Standard exception objects, uncaught diagnostics, and stack traces
  remain later exception stages; the first two are supplied by Runtime ABI 10
  below.
- NIR text format 4 establishes the checked handler contract before runtime
  unwinding is introduced. The lexer and parser accept Scala-style `try`,
  ordered typed `catch` cases, catch-all `_`, and optional `finally` bodies;
  `try` with only `finally` is also valid. Typechecking requires catch targets
  to resolve to a class, trait, or `Object`, gives each catch parameter an
  immutable lexical scope, joins protected/handler result types with `Nothing`
  as the bottom type, and discards the finalizer result. NIR records protected
  bodies, typed catch bindings, ordered handler bodies, and an optional finalizer
  as dedicated values. Verification rejects primitive or unresolved catch
  types, escaped/malformed bindings, invalid result types, misplaced handlers,
  and finalizers that are duplicated or not last. Linker reachability retains
  catch types, while Interflow optimizes inside protected and handler bodies
  without treating the handler boundary as pure or discardable. Plain and
  optimized `--emit-nir` preserve this contract for executable lowering.
- Runtime ABI 9 makes NIR text format 4 handlers executable. Each generated
  `try` installs a stack-local handler frame containing the previous handler,
  shadow-root head, active zone, and an aligned native jump buffer. Runtime
  `throw` pops that frame, restores the saved root and zone state, destroys
  zones opened below the handler, and transfers back through the target libc
  `_setjmp`/`longjmp` ABI. The resumed path keeps the exception in a shadow root,
  checks ordered class and trait descriptors with the existing runtime type
  relation, binds the first matching handler lexically, and rethrows when no
  handler matches. Catch-all handlers bypass descriptor lookup. Normal and
  handled results join before one shared finalizer; unmatched exceptions run a
  separate finalizer before rethrow, and an exception raised by a handler or
  finalizer correctly targets the next outer frame. The collector also marks
  the in-flight runtime exception, and startup/shutdown clear all handler state.
  Optimized native smoke coverage exercises cross-call catches, ordered and
  catch-all matching, normal and exceptional finalization, unmatched and
  handler rethrows, finalizer exception replacement, rooted reference results,
  trait handlers, and active-zone recovery.
  The current jump-buffer storage is sized for the supported 64-bit glibc
  target; target-specific sizing or a platform-independent unwinder is required
  before this mechanism is portable to other native ABIs.
  Functions containing a structured exception handler now give mutable local
  slots an explicit live-across-handler policy. Their LLVM loads and stores are
  volatile, so Boolean, numeric, Char, and reference assignments performed after
  `_setjmp` remain observable after a caught `longjmp`; reference locals continue
  to update their GC shadow roots as well. The policy is conservatively scoped
  to the whole handler-bearing function, while mutable locals in functions
  without handlers retain ordinary optimizable accesses. This is a codegen
  correctness rule and does not revise the runtime ABI.
- Runtime ABI 10 begins the standard exception surface and uncaught diagnostic
  contract. The frontend now seeds `java.lang.Throwable` and
  `java.lang.Exception`; source can construct `Exception(message)`, catch either
  type, subclass `Exception`, call `message`/`getMessage`, and override
  `toString`. Their class, field, accessor, parent-constructor, and virtual method
  definitions are emitted as ordinary checked NIR, so descriptor matching and
  optimized linking use the same machinery as source-defined classes. Throwing
  arbitrary objects remains temporarily accepted for compatibility with the
  earlier exception stages; restricting operands to `Throwable` waits until the
  standard hierarchy covers the required runtime failures.

  Stable generated-runtime builtins now live as LLVM source resources under
  `cpp-tools/codegen/resources/runtime`. CMake embeds those `.ll` files into the
  codegen library, so compiler binaries remain self-contained while lifecycle
  and exception IR can be maintained independently from the C++ lowering
  implementation. Layout- and program-dependent metadata stays generated in
  C++ behind narrow runtime helpers.

  An uncaught throw now writes `Uncaught exception: <description>` to `stderr`
  before zone or collector teardown. The description uses the exception's
  retained virtual `toString`, including when its static type was widened to
  `Object` or a trait, and defaults to the runtime type name. A reporting guard
  makes an exception thrown by `toString` fall back once to its type name instead
  of recursively reporting forever. Native smoke coverage checks standard and
  subclass messages, `Throwable` catches, inherited parent-message
  initialization, widened throws, and recursive-report fallback. Causes,
  suppressed exceptions, source stack capture, and printed stack traces remain
  later exception-runtime work.
- Runtime ABI 11 moves standard exception state to `java.lang.Throwable`.
  `Throwable` now owns GC-traced `message` and `cause` fields and supplies
  concrete `getMessage`, `getCause`, `initCause`, and `toString` methods.
  `Exception(message)` remains source-compatible and delegates its message plus
  an initially null cause through the existing inherited-constructor lowering,
  so subclasses initialize the same state transitively. Optimized native smoke
  coverage initializes a cause, drops the constructor temporary, runs a
  collection, and reads the cause through a `Throwable` API before exercising
  handled and uncaught throws. Java-compatible one-time `initCause` checks,
  self-causation rejection, cause-taking constructor overloads, suppressed
  exceptions, and cause-chain diagnostics remain later work; ABI 12 and ABI 13
  supply cause-chain diagnostics and strict initialization below.
- The canonical exception symbols intentionally remain `java.lang.Throwable`
  and `java.lang.Exception`. Scala source receives the unqualified `Throwable`
  and `Exception` names through the frontend's standard scope, while preserving
  the class identities expected by Scala, JVM libraries, and upstream Scala
  Native. A separate `scala.lang` hierarchy would fragment type identity and is
  therefore not introduced.
- Runtime ABI 12 adds fatal cause-chain diagnostics. The C++ layout emitter
  exposes a narrow helper that verifies an object is a `Throwable` and reads the
  cause using the computed inherited field offset; the embedded
  `resources/runtime/exceptions.ll` implementation prints each retained dynamic
  description as `Caused by: ...`. A fixed 64-entry seen-object table bounds
  reporting and detects self or multi-object cycles, producing one circular
  reference marker. Linker reachability now retains dynamic `toString`
  implementations for values passed to `initCause`, including statically
  widened `Throwable` values. Native smoke coverage exercises a normal cause
  and a two-object cycle under optimization.
- Runtime ABI 13 gives `initCause` strict initialization semantics. An
  `Exception` constructor seeds its inherited cause field with the object itself
  as a private uninitialized sentinel. `getCause` and the fatal-reporting layout
  helper normalize that sentinel to visible `null`; the stored constructor field
  has no source-level raw accessor. An explicit
  `initCause(null)` replaces it and therefore counts as initialization. A later
  call throws the newly seeded `java.lang.IllegalStateException`; direct
  self-causation throws `java.lang.IllegalArgumentException`. Both failures are
  ordinary `Exception` subclasses with inherited messages and participate in
  typed catches, NIR verification, linker reachability, and native unwinding.
  Indirect cause cycles remain representable and are bounded by ABI 12's
  reporter. Native coverage checks fresh and explicit-null causes, both typed
  failures, and the existing valid and circular cause paths.
- Runtime ABI 14 adds portable source stack frames without depending on a
  platform backtrace API. Every source-backed generated function pushes a
  compact frame containing static function/file names and the current source
  line/column, refreshes that location at instructions, calls, and throw sites,
  and pops the frame on normal return. This stack is deliberately separate from
  GC shadow frames, so functions without reference roots remain visible and GC
  layout stays unchanged. Exception handlers save and restore the source head
  alongside their shadow-root and zone snapshots before `longjmp`, preventing a
  handled cross-call throw from leaving dangling callee frames. Runtime startup
  and shutdown clear the head, and uncaught reporting walks at most 64 active
  frames as `at owner.member(File.scala:line)` before printing cause diagnostics.
  Source traces are emitted even with LLVM debug information disabled; the
  synthesized runtime-main bridge is omitted. ABI 15 below retains immutable
  traces across handled rethrows and for previously thrown causes; public
  stack-trace APIs remain later exception-runtime work.
- Runtime ABI 15 retains the first thrown source trace on each `Throwable`.
  `Throwable` owns a private, source-inaccessible `Long` storage word emitted as
  an initialized NIR field rather than a constructor parameter. On first throw,
  the runtime copies at most 64 active source frames into a fixed native
  snapshot; subsequent throws preserve that pointer, so a handled exception
  rethrown elsewhere still reports its original throw site. Uncaught roots use
  their retained snapshot, and causes print an independent snapshot only when
  they were previously thrown, avoiding a duplicated or invented cause stack.
  Arbitrary non-`Throwable` throws keep ABI 14's active-stack fallback.

  The collector releases retained native snapshots before reclaiming GC-owned
  exception objects. Arena-owned exception finalization remains tied to the
  future arena finalizer contract. Native coverage captures a root and cause,
  handles both, performs a collection, and rethrows the root through a different
  function; diagnostics retain the two original sites and omit the relay site.
  Runtime-generated cause errors now use concise native wording (`Cause already
  initialized` and `An exception cannot cause itself`) rather than copying JVM
  message text. Canonical `java.lang` type identity remains unchanged, but fatal
  presentation is intentionally allowed to be clearer and less verbose than a
  JVM stack dump. Public stack-trace arrays/elements, explicit refill APIs,
  suppressed exceptions, and arena trace finalization remain later work.
- Runtime ABI 16 gives the standard `Throwable.toString` and
  `Exception.toString` implementations a concise dynamic description. Both
  delegate to a compiler-known, non-virtual runtime helper that reads the
  exception's real type descriptor, removes only its package prefix, and appends
  `: message` only when the inherited message is non-null and non-empty. A
  `demo.errors.Failure("root")` therefore defaults to `Failure: root`, while a
  missing message defaults to `Failure`. The helper formats into one
  program-arena allocation and cannot recurse through virtual dispatch.

  User `toString` overrides still dispatch normally and completely control their
  own text. If such an override throws while the uncaught reporter is already
  active, the existing recursion guard retains its canonical descriptor-name
  fallback. Runtime descriptors, subtype checks, catches, and linker identity
  continue to use canonical names such as `java.lang.Exception`; concise naming
  is presentation only. Frontend null conformance now matches the NIR reference
  rule, so `null` is assignable to `String` as well as class references and the
  no-message path is representable directly.
- Runtime ABI 17 makes `Throwable` ownership safe across `Zone` unwinding.
  Codegen recognizes `java.lang.Throwable` and every linked subclass through
  the existing class linearization and always allocates those objects through a
  dedicated GC path, even while a zone is active. This prevents an exception
  created in an inner zone from being reclaimed before an outer handler reads
  it. Ordinary objects continue to use the scoped arena path, so the change does
  not silently turn `Zone` into general heap allocation.

  Retained source snapshots on handled zone-crossing exceptions now follow the
  existing GC lifetime contract: once the handler and runtime exception root
  release the object, collection calls `__scalanative_release_exception_trace`
  before reclaiming it. Native coverage throws a default exception through two
  nested zones, formats it in an outer catch after both zones unwind, then
  collects it and observes a zero live-object count; a neighboring ordinary
  class allocation confirms that non-`Throwable` zone allocation is unchanged.
  Arbitrary custom exception fields that point at other zone-owned objects still
  require future ownership validation or promotion if they are allowed to cross
  the same boundary.
- Runtime ABI 18 adds the first public retained-stack operation,
  `Throwable.fillInStackTrace`. In the current parameterless-method frontend it
  is invoked as `failure.fillInStackTrace` and returns the same `Throwable` for
  chaining. The standard overridable NIR method delegates to a non-virtual
  runtime intrinsic that releases any previous native snapshot and immediately
  captures the active source-frame stack. Generated standard-library methods
  carry no source frame, so the first visible frame is the user's refill call
  site rather than a runtime implementation detail.

  A later throw sees the replacement snapshot and preserves it under ABI 15's
  first-existing-trace rule. Optimized native coverage first throws and handles
  a `Failure`, explicitly refills it in a different method, and rethrows it;
  fatal diagnostics contain the refill method, omit the original capture method
  and relay throw site, and retain the cause's independent original trace. The
  returned object remains usable through `getMessage`. Public stack-frame
  arrays/elements, stack mutation from arrays, suppressed exceptions, and
  constructor-time capture remain later exception-runtime work.
- Runtime ABI 19 exposes retained snapshots through
  `Throwable.getStackTrace` and the seeded `java.lang.StackTraceElement` class.
  A frame contains `functionName: String`, `fileName: String`,
  `lineNumber: Int`, and `columnNumber: Int`. The parameterless method returns
  `Array[StackTraceElement]`; an exception that has neither been thrown nor
  explicitly filled returns an empty array under the current first-throw
  capture contract.

  The layout-dependent LLVM helper clamps the internal count to 64, allocates a
  fresh program-lifetime reference array, and materializes fresh typed frame
  objects using computed descriptors and field offsets. The native snapshot is
  never exposed directly: mutating one returned array cannot alter later reads
  or fatal diagnostics. Optimized source coverage reads every frame field,
  replaces the first element in one returned array, obtains an unaffected
  second copy, and verifies the original capture function/file and positive
  line/column values. Suppressed exceptions and constructor-time capture remain
  later work.
- Runtime ABI 20 gives `StackTraceElement` a standard overridable `toString`
  implementation. It uses the compiler-native compact form
  `function(file:line:column)` rather than copying the JVM's more elaborate
  formatting. Null function and file fields independently render as
  `<unknown>`, so manually constructed frames remain safe and useful.

  The generated NIR method delegates to a non-virtual runtime intrinsic. Its
  layout-aware LLVM helper reads the four typed fields by computed offsets,
  asks `snprintf` for the exact required size, and creates the result with one
  program-arena allocation. Formatting failure returns the stable
  `<unknown>(<unknown>:0:0)` text. Optimized native coverage formats both a
  retained source frame and a manually constructed all-null frame.
- Runtime ABI 21 adds the standard overridable
  `Throwable.setStackTrace(Array[StackTraceElement]): Unit` operation. Valid
  input is copied field by field into a replacement native snapshot and capped
  at the runtime's existing 64-frame diagnostic limit. The caller's array and
  the fresh arrays later returned by `getStackTrace` therefore remain detached
  from retained fatal-report state. Null arrays or null frame entries currently
  follow checked native array behavior and trap; Runtime ABI 24 below replaces
  those traps with typed `NullPointerException` failures.

  An empty input installs a real zero-frame snapshot rather than clearing the
  trace pointer. This preserves the caller's explicit empty trace across later
  throws under the first-retained-trace rule. Replacement allocates and fills
  the new snapshot before releasing the old one, and an allocation failure
  leaves the old trace intact. Retained fatal reporting now substitutes
  `<unknown>` independently for null function or file names. Optimized native
  coverage mutates both the input and a returned copy, checks an explicit empty
  trace after throw/catch, and verifies that a two-frame synthetic trace is the
  one printed by the final uncaught throw.
- Runtime ABI 22 adds bounded suppressed-exception support through the standard
  overridable `Throwable.addSuppressed` and `Throwable.getSuppressed` methods.
  Null and self-suppression are rejected with concise
  `IllegalArgumentException` messages, duplicate values are retained, and each
  throwable accepts up to 64 suppressed values in insertion order. The bound
  matches the diagnostic runtime's existing cause and frame limits.

  Storage uses a hidden GC-traced head field and compact GC-owned linked nodes;
  both the suppressed throwable and the next node are descriptor-traced strong
  references. `getSuppressed` returns a fresh fixed-capacity GC object with the
  normal reference-array layout plus a hidden traced owner reference. As a
  result, mutating one returned array cannot alter the retained list, and a
  detached result keeps its source throwable and suppressed values alive across
  collection. All 64 element slots are traced as well, so caller-written
  replacements retain normal reference-array GC semantics. Fatal reporting
  prints root and cause-local suppressed values in insertion order before
  continuing through the cause chain. Optimized native coverage exercises
  validation, empty results, detached-array and replaced-element GC retention,
  defensive copies, ordering, and final fatal output. Constructor-time trace
  capture remains later exception-runtime work.
- Runtime ABI 23 captures a `Throwable` source trace when construction completes,
  before the new expression returns. Codegen reuses the existing linked-hierarchy
  check that makes every `Throwable` subtype GC-owned, refreshes the caller's
  source location to the construction expression, and invokes the bounded native
  snapshot helper only after constructor arguments, inherited initialization,
  field initializers, and constructor statements have completed. Generated
  standard-library constructor machinery therefore does not appear as a frame.

  A newly constructed exception now exposes a non-empty `getStackTrace` before it
  is ever thrown. Throw-time capture remains an idempotent fallback, so relaying
  that object through another throwing method preserves the construction site.
  `fillInStackTrace` still replaces the snapshot, `setStackTrace` still installs
  the caller's detached copy (including an explicit empty trace), and native
  snapshot reclamation continues to follow the GC-owned Throwable lifetime.
  Optimized native coverage checks pre-throw visibility and verifies that a
  later relay throw cannot replace the constructor-site frame. Suppressed
  exceptions created before fatal reporting now expose their own construction
  traces as well.
- Runtime ABI 24 seeds `java.lang.NullPointerException` as an ordinary standard
  `Exception` subclass and replaces `Throwable.setStackTrace`'s null-input traps
  with typed failures. The layout-dependent native copier now returns a compact
  status: success, null array, or null frame entry. The generated standard NIR
  method maps the latter statuses to `NullPointerException` with the concise
  messages `Stack trace cannot be null` and
  `Stack trace cannot contain null elements`, respectively.

  Validation remains transactional. A null frame is discovered while filling a
  newly allocated snapshot, which is released before unwinding; the previously
  retained constructor, refill, or custom trace is not changed. Native coverage
  catches both failures by their concrete standard type and verifies that a
  rejected element array preserves the original constructor-site frame. Runtime
  ABI 25 below extends typed failures to general array operations.
- Runtime ABI 25 seeds the standard `java.lang.IndexOutOfBoundsException`
  hierarchy, including `java.lang.ArrayIndexOutOfBoundsException`, and replaces
  checked array traps with ordinary catchable exceptions. Reading `.length`
  from a null array now throws `NullPointerException` with the concise message
  `Array cannot be null`. Negative and upper-bound reads or writes throw
  `ArrayIndexOutOfBoundsException` with
  `Array index is out of bounds`. The concrete bounds failure remains catchable
  through its `IndexOutOfBoundsException` parent.

  String, reference, `Int`, `Boolean`, `Long`, `Double`, `Float`, and `Char`
  arrays all share this contract. Layout-aware native constructors allocate the
  standard exception as a GC-owned object, initialize inherited message and
  cause state, capture the current source trace, and enter the existing
  unwinder. The linker treats every array length, apply, and update intrinsic as
  an implicit dependency on both runtime failure classes, so optimized programs
  retain typed behavior without explicit source catches. Native coverage checks
  null length, negative reads, upper-bound writes, reference-array access,
  superclass catching, preserved in-range data, and intrinsic-only retention.
- Runtime ABI 26 seeds `java.lang.ClassCastException` as an ordinary standard
  `Exception` subclass and replaces incompatible cast traps with catchable
  failures. Reference `asInstanceOf` compatibility checks and checked boxed
  unboxing now share the concise message
  `Value cannot be cast to requested type`. Successful reference casts still
  return the original object, while a null reference cast still returns null.

  The layout-aware native constructor allocates a GC-owned standard exception,
  initializes inherited message and cause state, captures the cast source
  location, and enters the existing unwinder. Linker traversal treats both NIR
  `as-instance-of` and `unbox` values as implicit dependencies on
  `ClassCastException`, retaining typed behavior in optimized programs that do
  not name or catch the class. Native coverage exercises caught reference and
  boxed failures, null preservation, unbox-only optimized retention, and an
  uncaught failure with its source frame.
- Runtime ABI 27 makes null class and trait receivers ordinary catchable
  `NullPointerException` failures for instance field reads and writes, direct
  methods and accessors, setters, and virtual dispatch. These paths share the
  concise message `Receiver cannot be null`; module access and stack-super
  dispatch remain outside the check because neither represents a nullable
  source receiver.

  Receiver expressions are evaluated first, followed by assignment values or
  explicit call arguments, and the null check runs immediately before the
  field access or invocation. Interflow may still devirtualize closed receiver
  types, but it only removes an instance invocation by inlining when the
  receiver is proven non-null. LLVM codegen also recognizes devirtualized class
  and trait globals so their explicit receiver argument keeps the same check.
  Linker traversal treats instance-member `select`, `call`, and `assign` values
  as implicit dependencies on `NullPointerException`. Optimized native coverage
  exercises caught field, direct, accessor, setter, and virtual failures,
  side-effect ordering, valid receivers, and an uncaught source-framed failure.
- Runtime ABI 28 extends the typed receiver contract to compiler-special String
  members. Explicit `String.length`, `hashCode`, `toString`, and `equals`
  invocations now throw `NullPointerException` with
  `Receiver cannot be null` when their receiver is null. `String.equals`
  evaluates and boxes its argument before checking the receiver, preserving
  invocation order, while language-level `==` and `!=` retain their null-safe
  value-equality semantics.

  The frontend now emits distinct `stringToString` and `stringEquals` NIR
  intrinsics instead of erasing `toString` to identity or conflating `equals`
  with binary equality. LLVM checks each String receiver through the shared ABI
  27 guard; String/Object equality recognizes boxed String arguments without
  changing null or non-String results. Interflow folds exact non-null literals
  but no longer rewrites a null String hash to zero, and linker traversal treats
  every String member intrinsic as an implicit `NullPointerException`
  dependency. Optimized native coverage exercises all four caught failures,
  argument order, valid String/null/non-String equality, null-safe `==`, and an
  uncaught source-framed failure.
- Runtime ABI 29 separates explicit universal-member invocation from null-safe
  Any operations. Compiler-special `equals`, `hashCode`, and `toString` calls on
  `Any`, null, and ordinary class references now throw `NullPointerException`
  with `Receiver cannot be null` when the receiver is null. Explicit `equals`
  still evaluates and boxes its argument before checking the receiver.

  The frontend emits `anyReceiverEquals`, `anyReceiverHashCode`, and
  `anyReceiverToString` NIR intrinsics for invocation syntax. LLVM checks the
  receiver through the shared ABI 27 guard and then delegates to the existing
  boxed Any helpers. Language `==`/`!=`, String concatenation, and generic
  printing continue to use null-safe `anyEquals` and `anyToString`, so null
  remains a valid value in those contexts. Primitive receivers remain
  non-nullable value operations even when their argument is boxed as `Any`.

  Interflow folds checked calls only when the receiver is proven non-null and
  preserves effectful argument order; linker traversal retains
  `NullPointerException` for all three new intrinsics. Optimized native coverage
  exercises caught Any and class failures, valid boxed and identity behavior,
  null-safe equality and concatenation, and an uncaught source-framed failure.
- Runtime ABI 30 normalizes a null `throw` operand into an ordinary catchable
  `NullPointerException` with the concise message
  `Thrown exception cannot be null`. Both direct `throw null` and a nullable
  object-reference operand are accepted by source and NIR validation; primitive
  operands remain rejected.

  LLVM evaluates the operand once, publishes the throw expression's source
  location, restores the current shadow-root frame, and passes the pointer
  through a dedicated non-null guard before entering the existing unwinder. A
  null pointer uses the same layout-aware typed-failure construction as other
  runtime faults. Ordinary throws and unmatched-handler rethrows preserve their
  existing object identity and retained trace.

  Linker traversal treats both nested throw values and throw terminators as
  implicit `NullPointerException` dependencies, including optimized programs
  that never name or catch that class. Native coverage exercises direct and
  indirect null throws, effectful operand order, `finally` unwinding, an
  unaffected ordinary exception, and an uncaught source-framed null throw.
- Runtime ABI 31 gives `throw` its complete static operand contract. Source
  operands must conform to `Throwable` or have the `Null` type, and NIR throw
  values must likewise be a transitive subtype of `java.lang.Throwable` or
  `Null`. A value statically widened to `Object` is rejected even when its
  runtime value happens to be an exception. ABI 30's null case remains legal
  and is still normalized into a catchable `NullPointerException` at runtime.

  The frontend checks the transitive source hierarchy, while the NIR verifier
  checks serialized class-parent metadata for both throw terminators and nested
  throw values. Invalid source reports
  `throw operand must conform to Throwable or be null`; invalid NIR identifies
  the non-Throwable operand type. Legacy failure fixtures now extend
  `Exception`. Coverage rejects primitive, plain-class, and Object-widened
  throws while retaining direct and indirect exception subclasses, exception
  classes with trait mixins, and direct or nullable null throws.
- Runtime ABI 32 aligns catch-handler validation with ABI 31's Throwable-only
  throw contract. A class-typed handler must now be a transitive
  `Throwable` subtype. Trait handlers remain legal because a Throwable subclass
  can mix in an otherwise unrelated trait, and `Object` or `case _` remains the
  universal handler. Catch-all handlers must be last so no serialized NIR can
  rely on a backend-only ordering failure.

  The frontend reports a known plain-class handler separately from an
  unresolved or primitive catch type. The NIR verifier independently checks
  class ancestry from parent metadata and rejects any handler after a catch-all.
  Coverage exercises both source and hand-built NIR rejection while retaining
  direct and inherited exception classes, a mixed-in trait handler, ordered
  typed handlers, and terminal catch-all execution in optimized native code.
- Runtime ABI 33 rejects provably unreachable typed handlers. A later handler is
  shadowed when its class or trait is a subtype of an earlier handler type, or
  when the earlier type already covers the `Throwable` root. The latter rule is
  important for a later unrelated trait: although that trait does not itself
  extend `Throwable`, every value entering a catch sequence does.

  Validation remains conservative for overlapping mixins. An earlier concrete
  exception class does not suppress a later trait merely because one known
  subclass mixes in both; another Throwable subtype may still implement that
  trait. Source diagnostics name the later and shadowing declared types, and
  the NIR verifier repeats the proof from serialized class/trait ancestry.
  Coverage exercises transitive exception classes, trait-before-class
  shadowing, Throwable-before-trait shadowing, and a valid partially overlapping
  class-before-trait sequence that still dispatches natively.
- Runtime ABI 34 introduces the canonical `java.lang.RuntimeException` layer.
  `IllegalArgumentException`, `IllegalStateException`,
  `NullPointerException`, `ClassCastException`, and
  `IndexOutOfBoundsException` now inherit from `RuntimeException`; the existing
  `ArrayIndexOutOfBoundsException` remains below `IndexOutOfBoundsException`.
  Every class retains its concise message constructor and inherited Throwable
  state, formatting, trace, cause, and suppression behavior.

  The frontend standard scope and typed declarations, shared standard names,
  NIR type qualification, serialized parent metadata, linker ancestry, and LLVM
  descriptors all observe the same hierarchy. Optimized native coverage catches
  a compiler-generated null-array failure through `RuntimeException`, while NIR
  assertions cover every reparented runtime failure and the transitive array
  subtype. `Error` remains a separate branch and is added independently by ABI
  35 below.
- Runtime ABI 35 adds that separate `java.lang.Error` branch directly beneath
  `Throwable`, as a sibling of `Exception`. `Error(message)` initializes the
  inherited Throwable message and the same private uninitialized-cause sentinel
  used by `Exception`, inherits standard formatting and trace/suppression
  behavior, and can be subclassed, thrown, and caught through ordinary source
  and NIR machinery.

  The shared standard name, frontend scope and synthetic declaration, NIR type
  qualification, linker metadata, and LLVM descriptor ancestry all preserve the
  sibling relationship. Optimized native coverage throws a user-defined Error
  subclass through an `Exception`-then-`Error` handler sequence, proves the
  Exception branch does not match, reads the retained message, and observes a
  visible null cause. The user-facing exception example carries the same
  diagnostic path.
- Runtime ABI 36 introduces `java.lang.ArithmeticException` beneath
  `RuntimeException` and gives integer division and remainder defined failure
  behavior. `Int` and `Long` divisors equal to zero now throw a catchable typed
  exception with the concise `Integer divisor cannot be zero` message; floating
  division and remainder retain their IEEE behavior.

  LLVM lowering routes integer `/` and `%` through checked helpers instead of
  emitting potentially undefined raw `sdiv` or `srem` operations. The helpers
  also preserve Scala's signed-overflow result for minimum-value division by
  negative one, and return zero for the corresponding remainder. Interflow's
  exact integral folder mirrors those rules without performing undefined host
  arithmetic. Linker value scanning retains `ArithmeticException` whenever a
  reachable integer division or remainder may need it, even when source code
  never explicitly names the class. Interflow conservatively treats division
  and remainder as potentially throwing so discarded results and calls retain
  their failure behavior. Its post-inlining reachability scan also follows
  selected module members, preventing pure retained calls from losing their
  definitions during pruning. Optimized native coverage exercises typed and
  `RuntimeException` parent catches, message/cause state, `Int` and `Long`
  minimum values, and non-throwing floating division; the public exception
  example exposes the same paths.
- Runtime ABI 37 adds the first operational `Error` subtype,
  `java.lang.AssertionError`, directly beneath `java.lang.Error`. The frontend
  seeds a Boolean-only `assert(condition)` builtin and NIR maps it to the typed
  `scala.scalanative.runtime.assert : (Boolean)Unit` declaration. A true
  condition returns normally; a false condition throws an `AssertionError` with
  the concise `Assertion failed` message and the standard visible null cause.

  The linker makes the runtime assertion declaration retain its failure class
  even when source never names or catches `AssertionError`. LLVM lowers the
  builtin through a checked branch and the shared typed allocation, trace
  capture, and unwinding path. Optimized native coverage runs a passing
  assertion before a failing one, proves that the failure bypasses an
  `Exception` handler and reaches `AssertionError`, and reads its message and
  cause. A standalone passing-assertion program verifies linker ancestry, and
  the public exception example carries the same sibling-dispatch path.
- Runtime ABI 38 adds the complementary Boolean-only `require(condition)`
  precondition builtin. NIR maps it to
  `scala.scalanative.runtime.require : (Boolean)Unit`; a true condition returns
  normally, while false throws the existing `java.lang.IllegalArgumentException`
  with the concise `Requirement failed` message and a visible null cause.

  The runtime declaration retains `IllegalArgumentException` through the linker
  even when source does not mention the class. LLVM shares the checked-condition
  emission shape introduced for assertions but uses an independent typed
  allocation and throw helper. Optimized native coverage proves a failed
  requirement bypasses `Error`, reaches `IllegalArgumentException`, and retains
  its message/cause state. A standalone passing-requirement program verifies the
  complete `RuntimeException -> Exception -> Throwable` ancestry, and the public
  exception example exposes the same path.
- Runtime ABI 39 completes the one-argument Scala precondition trio with
  `assume(condition)`. The frontend requires a Boolean operand and NIR maps the
  call to `scala.scalanative.runtime.assume : (Boolean)Unit`. A true assumption
  returns normally; false throws the existing `java.lang.AssertionError` with
  the concise `Assumption failed` message and a visible null cause.

  The linker retains `AssertionError` from the runtime declaration even when
  source code never names the class. LLVM reuses the checked-condition emission
  shape while keeping an independent assumption message and throw helper.
  Optimized native coverage proves that a failed assumption bypasses
  `Exception`, reaches `AssertionError`, and preserves its message/cause state.
  A standalone passing-assumption program verifies the complete
  `Error -> Throwable` ancestry, and the public exception example includes the
  same dispatch path. Message-bearing overloads remain deferred until overload
  resolution and by-name arguments are part of the supported language surface.
- Runtime ABI 40 adds the canonical `scala.NotImplementedError` beneath
  `java.lang.Error` and recognizes Scala's symbolic `???` placeholder as a
  bottom-typed expression. The lexer classifies the complete `???` token as an
  identifier so statement boundaries remain correct, while the frontend gives
  it the existing `Nothing` type and therefore permits it in every result
  position.

  NIR lowers `???` directly to construction of `NotImplementedError` with the
  concise `Implementation is missing` message followed by an ordinary typed
  throw. This deliberately reuses established allocation, trace capture, GC
  rooting, handler transfer, and bottom-branch behavior instead of introducing
  a redundant backend intrinsic. Optimized native coverage catches the failure
  across a function boundary, proves it bypasses `Exception`, and reads its
  message and visible null cause. A standalone optimized program verifies that
  `???` alone retains the complete `Error -> Throwable` ancestry, and the public
  exception example includes the same placeholder path.
- Runtime ABI 41 exposes `Throwable.printStackTrace(): Unit` as the public,
  non-terminating entry point to the runtime's concise exception-tree reporter.
  The root description uses dynamic `toString`, followed by retained source
  frames, suppressed failures, and the bounded cause chain. Unlike a fatal
  throw, a handled diagnostic omits the misleading `Uncaught exception:` prefix
  and returns to its caller after writing to standard error.

  NIR delegates the standard method to
  `scala.scalanative.runtime.printStackTrace : (java.lang.Throwable)Unit`, and
  LLVM lowers it to a wrapper around the same reporter used by uncaught throws.
  The fatal-only recursion guard remains isolated from public reporting so a
  throwing user `toString` follows ordinary exception transfer. The linker
  retains the argument's dynamic formatting path through optimization. Native
  coverage proves root, frame, suppressed, and cause output, continued execution,
  and ordinary propagation from a throwing user formatter. The public exception
  example demonstrates the handled diagnostic path before its intentionally
  uncaught final failure.
- Runtime ABI 42 adds dynamic-length construction with Scala's conventional
  `new Array[T](length)` syntax for every currently supported scalar, `String`,
  `Any`, class, trait, and object element type. The frontend requires exactly
  one `Int` length, while NIR uses typed allocation intrinsics rather than
  overloading literal-array `new` operands. This preserves the established
  verifier and Interflow meaning of `Array(...)` literals.

  LLVM lowers all typed intrinsics through one element-width-aware allocator.
  It checks the signed length before widening, computes the payload size, and
  uses the existing zero-initializing program arena, yielding Scala defaults of
  zero, false, or null. A negative length throws the canonical
  `java.lang.NegativeArraySizeException` beneath `RuntimeException` with the
  concise `Array size cannot be negative` message. Linker retention follows the
  allocation intrinsic, so optimized code keeps the typed failure even when
  source never names it. Native coverage exercises every slot width, mutation,
  reference defaults, malformed constructor diagnostics, and exact typed catch
  behavior; the public array example demonstrates both success and failure.
- Runtime ABI 43 extends array element types recursively and introduces the first
  `Array.ofDim[T](length)` surface. One-dimensional `ofDim` shares the typed
  dynamic allocation intrinsics from ABI 42, while an array element type uses an
  escaped, deterministic reference-intrinsic suffix so nested types remain valid
  NIR symbols. Multi-argument recursive construction such as
  `Array.ofDim[Int](rows, columns)` is intentionally not part of this increment.

  The frontend and NIR verifier independently reject unsupported nested element
  types. Interflow recognizes the escaped length, apply, and update helpers, and
  LLVM continues to represent every nested-array slot as an eight-byte reference.
  Native coverage exercises null defaults, outer and inner allocation, mutation,
  readback, nested literals, optimized function boundaries, and malformed
  `Array.ofDim` diagnostics; the public array example includes the same workflow.
- Runtime ABI 44 generalizes `Array.ofDim[T]` to two or more dimensions. The
  frontend derives one nested `Array` result layer per `Int` dimension, and NIR
  records a typed, dimension-counted intrinsic while preserving left-to-right,
  single evaluation of every dimension expression.

  LLVM validates every dimension before allocating any array, including inner
  dimensions when an outer dimension is zero. It then emits compact nested loops:
  outer and intermediate arrays use reference slots, while each innermost array
  uses the width of `T`. Coverage includes two-dimensional primitive arrays,
  three-dimensional reference arrays, independent row mutation, zero defaults,
  optimized function-boundary allocation, and negative inner dimensions. These
  arrays retain the existing program-arena ownership policy; ABI 44 does not
  change GC placement or collection behavior.
- The ABI 44 array surface also supports direct indexing and assignment through
  completed calls that are statically known to return nested arrays, including
  `matrix(0)(1)`, `matrix(0)(1) = value`, and
  `allocate(rows, columns)(0)(0)`. The frontend distinguishes completed
  array-returning calls from the method symbols that begin ordinary calls, and
  NIR recursively lowers each proven array layer through the existing typed
  apply/update intrinsics. No runtime helper or ABI revision is required.
- Runtime ABI 45 adds Scala-style `array.clone()` for every supported primitive,
  String, class, trait, object, `Any`, and nested-array element type. The frontend
  preserves the receiver's exact array type and requires an empty argument list;
  NIR selects the corresponding typed clone intrinsic.

  LLVM checks the receiver through the existing catchable null-array path,
  allocates a same-length array in the program arena, and copies the payload using
  the statically known one-, four-, or eight-byte element width. Primitive storage
  is therefore independent, while reference and nested arrays deliberately retain
  Scala's shallow-copy semantics. Optimized linkage retains `NullPointerException`
  even when cloning is the only checked array operation. ABI 45 does not change
  array ownership or GC behavior.
- Runtime ABI 46 adds the first exact-typed
  `Array.copy(source, sourcePos, destination, destinationPos, length)` surface.
  Source and destination must currently have the same statically known array type;
  this keeps primitive/reference storage selection explicit while deferring
  cross-reference-array copies and their `ArrayStoreException` contract.

  NIR uses a deterministic element-typed copy intrinsic. LLVM evaluates each
  operand once in source order, checks both arrays for null, validates nonnegative
  positions and length plus both complete ranges, and performs the payload move
  with overlap-safe `llvm.memmove`. Boolean, four-byte scalar, eight-byte scalar,
  String, class, `Any`, and nested-array storage all share this path. Reference
  elements are copied shallowly. Null and range failures reuse the existing
  catchable `NullPointerException` and `ArrayIndexOutOfBoundsException` hierarchy;
  optimized copy-only programs retain the required runtime classes. ABI 46 does
  not change array placement or GC behavior.
- Runtime ABI 47 extends `Array.copy` across arrays whose elements use ordinary
  runtime object descriptors: classes, traits, objects, and `Any`. NIR records
  both source and destination array types in a dedicated cross-reference copy
  intrinsic. Copies into `Any` use the overlap-safe bulk path; other destination
  types check each non-null element with the existing class/trait ancestor
  metadata before storing it.

  A failed compatibility check throws the new catchable
  `java.lang.ArrayStoreException` after preserving elements already copied. The
  linker adds an explicit retention edge from cross-reference copies to that
  exception, while exact primitive copies continue to select ABI 46's unchecked
  bulk path. Null elements remain valid, and range validation still completes
  before any element is written. `String` arrays and nested arrays retain
  exact-type copying because their current specialized representations do not
  carry ordinary object descriptors. ABI 47 does not change array ownership or
  GC placement.
- The ABI 47 frontend surface also recognizes `Array.empty[T]` for every
  supported primitive, `String`, class, trait, object, `Any`, and nested-array
  element type. It shares constructor/`ofDim` element validation and lowers
  directly to the existing element-typed allocation intrinsic with a constant
  zero length. No new runtime helper, allocation policy, or ABI revision is
  required.
- Runtime ABI 48 adds the explicit-type
  `Array.fill[T](length)(element)` companion operation for the same supported
  element universe. The frontend requires one `Int` length and one conforming
  element expression; NIR records an element-typed fill intrinsic while keeping
  the element structurally unevaluated.

  LLVM evaluates the length once, allocates through the existing checked typed
  array path, and lowers the element expression inside a compact fill loop.
  This preserves Scala's by-name behavior: the expression runs once per slot,
  does not run for zero length, and is not evaluated when a negative length
  throws `NegativeArraySizeException`. Primitive widths use direct one-, four-,
  or eight-byte stores; `String`, class, `Any`, and nested-array elements use
  reference slots, with `Any` boxing also occurring per iteration. Filled arrays
  retain the program-arena ownership policy; ABI 48 does not change GC placement.
- Runtime ABI 49 generalizes explicit-type fill to
  `Array.fill[T](first, remaining...)(element)`. The frontend accepts one or
  more `Int` dimensions and constructs a result with one nested `Array` layer
  per dimension. NIR uses dimension-qualified typed fill intrinsics for the
  multidimensional forms while preserving the element expression structurally.

  LLVM evaluates every dimension expression exactly once from left to right,
  then recursively allocates independent nested arrays. The element remains
  by-name and is lowered only in the innermost loop, once per final slot.
  Consequently an empty outer dimension neither allocates unreachable inner
  arrays nor evaluates the element; a negative inner dimension throws only when
  an outer slot reaches it. Primitive and reference slots retain ABI 48's direct
  width-aware stores and program-arena ownership. ABI 49 does not change GC
  placement.
- Runtime ABI 50 adds `Array.range(start, end)` and
  `Array.range(start, end, step)` for `Int`. The frontend requires two or three
  `Int` arguments, and NIR normalizes the two-argument form to the typed
  `(Int,Int,Int)Array[Int]` intrinsic with a unit step.

  LLVM evaluates source arguments exactly once from left to right and computes
  the exclusive-end element count with signed 64-bit intermediates. Positive and
  negative steps share one direct four-byte fill loop; a direction mismatch
  produces an empty array. Step zero throws a catchable
  `IllegalArgumentException` with Scala's concise `zero step` message before
  allocation. A range whose count exceeds the runtime's signed 32-bit array
  length limit throws the same exception class with
  `Array range is too large`, avoiding arithmetic wraparound and accidental
  undersized allocation. Range arrays retain program-arena ownership; ABI 50
  does not change GC placement.
- Runtime ABI 51 adds explicit-type `Array.concat[T](arrays...)`, including the
  zero-input empty-array form. The current frontend accepts supported scalar,
  reference, `Any`, and nested-array element types, and requires each operand to
  have the exact declared `Array[T]` type. NIR records an element- and
  arity-qualified intrinsic so variadic signatures remain unambiguous.

  LLVM evaluates every operand exactly once from left to right before inspecting
  any input. It then performs catchable null checks, accumulates lengths in 64
  bits, rejects totals above the signed 32-bit array-length limit with
  `IllegalArgumentException("Array concatenation is too large")`, and allocates
  the destination once. Payloads are copied in order with width-aware
  `llvm.memcpy`; reference and nested-array elements therefore preserve Scala's
  shallow concatenation semantics. Concat arrays retain program-arena ownership;
  ABI 51 does not change GC placement.
- Runtime ABI 52 adds first-class signed `Byte` and `Short` values. LLVM stores
  them as `i8` and `i16`; explicit `toByte`/`toShort` conversions truncate, while
  lossless widening sign-extends through `Short` and `Int`. Unary and binary
  arithmetic promotes narrow integral operands to `Int`, matching Scala's
  value-level result types rather than silently retaining the storage width.

  The new types participate in typed arrays, literals built through explicit
  narrowing, fill/copy/clone/concat operations, `Any` boxing and checked
  unboxing, equality, hashing, string conversion, formatted interpolation,
  `println`, and `sizeof`. Their boxed primitive descriptor IDs extend the
  existing stable sequence without renumbering older kinds. Narrow arrays use
  direct one- and two-byte slots and retain the current program-arena ownership
  policy, so ABI 52 does not change GC placement.

  This milestone supplies the scalar and array-width foundation needed by a
  future native-memory and `ByteBuffer` surface. It does not yet add buffer
  ownership, byte order, indexed bounds contracts, bulk buffer operations, or
  off-heap allocation.
- Runtime ABI 53 adds the first explicit scoped native-byte allocation surface:
  `Zone.allocBytes(length): Array[Byte]`. The operation is valid only within a
  lexical `Zone.scoped` body. LLVM computes the exact array header plus payload
  size, obtains zeroed storage from the active arena, records arena ownership in
  the existing tagged header, and stores the requested length. Negative lengths
  throw `NegativeArraySizeException`; reads and writes reuse the ordinary
  `Array[Byte]` null and index checks.

  Frontend provenance analysis treats the result as a zone-owned reference and
  rejects assignment to outer locals or fields, ordinary escaping arguments,
  and reference-valued zone results. The NIR verifier independently requires
  the allocation call to remain nested beneath a `zone-scoped` value and allows
  the owned reference through only the non-escaping Byte-array length, apply,
  and update operations. Normal scope exit and exception unwinding therefore
  release the payload with its arena.

  This is a low-level ownership and storage bootstrap, not yet the stateful
  `ByteBuffer` API. Position/limit/mark state, slicing, byte-order selection,
  multibyte typed access, bulk transfers, and independently releasable native
  allocations remain later layers.
- Runtime ABI 54 adds portable two-byte access over `Array[Byte]` through
  `NativeBytes.getShortBE`, `getShortLE`, `putShortBE`, and `putShortLE`.
  Each operation validates nullability, a nonnegative index, and the complete
  two-byte range before reading or writing. An out-of-range store therefore
  throws `ArrayIndexOutOfBoundsException` without modifying either byte.

  LLVM composes and separates the two bytes explicitly rather than relying on
  an unaligned native `i16` load or the host byte order. Reads preserve all
  sixteen bits in the signed `Short` result, and writes preserve the two's
  complement representation of negative values. The operations accept ordinary
  and zone-owned Byte arrays; frontend and NIR lifetime analyses recognize them
  as non-escaping accesses when the storage belongs to `Zone.scoped`.

  These primitives are the endian and typed-access substrate for a future
  stateful `ByteBuffer`. Buffer position, limit, mark, relative operations, and
  views remain separate higher-level contracts.
- Runtime ABI 55 begins the stateful `ByteBuffer` surface with
  `ByteBuffer.wrap(Array[Byte])`. A compact runtime object records the backing
  array, immutable capacity, mutable position and limit, and an internal
  invalidated mark. The initial state is position zero and limit equal to
  capacity. `capacity()`, both `position` forms, both `limit` forms,
  `remaining()`, `hasRemaining()`, `clear()`, `flip()`, and `rewind()` now lower
  to typed NIR intrinsics and dedicated LLVM helpers.

  Position changes require `0 <= position <= limit`; limit changes require
  `0 <= limit <= capacity`, clamp the current position when necessary, and
  invalidate an out-of-range mark. Violations throw catchable
  `IllegalArgumentException` instances with concise buffer-specific messages.
  Null storage and null receivers retain the existing catchable null contracts.
  Mutating operations return the same buffer so state transitions can be
  chained.

  Buffer state follows the lightweight ownership policy: wrapping inside
  `Zone.scoped` allocates the state in the active arena and provenance analysis
  prevents it from escaping, while wrapping outside a zone uses the
  program-lifetime arena. Zone-backed Byte storage can therefore be wrapped
  without weakening ABI 53's lifetime checks. This first slice does not yet add
  relative or indexed get/put, underflow/overflow exceptions, byte-order state,
  mark/reset, slicing, duplication, read-only views, bulk transfer, or a complete
  `java.nio` compatibility layer.
- Runtime ABI 56 adds relative byte access with `ByteBuffer.get(): Byte` and
  `put(Byte): ByteBuffer`. Both operations require `position < limit` before
  touching the backing storage. A successful access reads or writes exactly one
  byte and advances position exactly once; a failed access leaves position and
  storage unchanged. `put` remains fluent and returns the same buffer.

  Exhausted reads throw a catchable `java.nio.BufferUnderflowException` with the
  concise message `ByteBuffer underflow`; exhausted writes analogously throw
  `java.nio.BufferOverflowException` with `ByteBuffer overflow`. The linker
  retains the distinct exception layouts for the corresponding intrinsics, and
  LLVM performs the state check before loading the backing-array reference.
  Relative reads discard scoped-zone provenance because they return a primitive,
  while relative writes preserve receiver provenance.

  Indexed byte access is intentionally separate because it does not advance
  position. Byte-order state, relative multibyte access, mark/reset, slicing,
  duplication, read-only views, and bulk transfer remain later slices.
- The frontend seeds a tiny runtime builtin, `println(value): Unit`, for the
  currently supported literal/value subset.
- `cpp-nscplugin` emits `scala.scalanative.runtime.println : (Unknown)Unit` as a
  NIR runtime declaration and maps `println(...)` calls to that intrinsic.
- LLVM codegen pre-scans structured NIR for string literals, emits private
  global byte constants, and lowers string values to `ptr`.
- Runtime `println` currently lowers strings to the C ABI `puts(ptr)` and
  `Boolean`, `Byte`, `Short`, `Int`, `Long`, `Float`, `Double`, and `Char`
  values to matching native output calls, including the required C vararg
  promotions. Boolean values select the immortal `true`/`false` text constants
  and use `puts`, matching Scala's textual output rather than printing numeric
  bits.
- The runtime ABI exposes a common type kind and layout model for boxed
  primitives, concrete classes, and non-allocatable traits. All allocated class objects now reserve a
  one-word descriptor header, including standalone classes that previously used
  a headerless compact payload. Class descriptors own the optional vtable pointer
  used by virtual dispatch; scalar descriptors use the same shape and checked
  unbox helpers. Class descriptors list transitive class and trait ancestors in
  the compiler's existing C3 order. Source-level `isInstanceOf[T]` and checked
  `asInstanceOf[T]` lower through generated compatibility helpers, including
  null and trait handling; incompatible casts throw Runtime ABI 26's typed
  `ClassCastException`.
  Allocation flows through ABI 4 tagged-header helpers. Class objects are
  zero-initialized, tracked, and reclaimed by a module/shadow-rooted non-moving
  mark/sweep collector during execution or after `main`; boxed primitive
  temporaries use a program-lifetime bump arena. Class descriptors expose
  inherited reference-field tracing maps and objects carry GC/arena/immortal
  ownership tags. Arenas use linked growth blocks, and nested
  `Zone.scoped({ body })` regions retain ordered statements and lexical mutable
  or immutable locals, route class and boxed allocations to a normally
  destroyed active arena, support nested shadowing, and prevent direct
  reference results from escaping. Scoped reference provenance additionally
  blocks outer-local stores, non-zone field stores, and ordinary call arguments,
  while allowing object graphs contained entirely within one active zone.
  Closed-world receiver-effect summaries reject zone calls to methods that leak
  `this` directly, through helper calls, or through a potentially leaking
  virtual override. Modules receive immortal
  singleton descriptors, lazy instance slots, and a global root-slot table.
  Stored module fields and source-ordered initializer bodies execute once and
  retain mutable state across aliases. The stdlib Zone callback API,
  library/cross-module arena-effect contracts, precise stack liveness,
  handled-exception cleanup across frames, concurrent module publication, and
  initialization-cycle diagnostics remain future work.
- `sizeof[T]` lowers primitive payload sizes and concrete class ABI instance
  sizes without allocating; smoke coverage builds and runs `Unit`, scalar, and
  two-field class cases and rejects traits at both source and NIR-verifier
  boundaries.
- `gcLiveObjectCount()` and `gcCollectionCount()` are compiler-known diagnostic
  calls that return `Long` values from the collector's live-object and completed
  collection counters. Optimized native smoke coverage proves that an
  unreachable allocation disappears from the live count after an explicit
  collection.
- Automatic non-moving collection now polls before GC-owned class allocation.
  Conservative shadow-frame temporary roots protect nested constructor values
  and objects under initialization across those safepoints. The default is 64
  tracked objects; `gcSetCollectionThreshold(n: Long)` is available for
  deterministic stress runs, and a threshold-one native fixture proves that
  `new Pair(new Leaf(...), helper())` remains valid across automatic collections.
- Primitive comparison operators now lower as native LLVM booleans: signed
  `Int`/`Long`, unsigned `Char`/`Boolean`, ordered floating-point comparisons,
  and pointer identity equality/inequality. The lexer treats `==` as one
  operator token rather than two assignment tokens, and parser recovery rejects
  a repeated assignment token without recursing indefinitely.
  Compiler-known `.equals(value)` now typechecks as a Boolean method surface
  when no user-defined member named `equals` exists, then emits the same equality
  NIR as `==` for supported primitive, String, Symbol, reference, and
  null/reference pairs. Comparisons involving the `Object` ABI now call
  `scala.scalanative.runtime.anyEquals`, which preserves identity semantics for
  ordinary objects while comparing same-kind boxed primitive payloads by value,
  including `Unit`, `Symbol`, and boxed `String`. Direct `Unit.equals` now
  lowers to constants for statically-known Unit/non-Unit cases and delegates
  through boxed `Any` when the argument uses the Object ABI. Direct Unit `==`
  and `!=` follow the same constant/Object-ABI split.
  Compiler-known `.hashCode` and `.hashCode()` now type as `Int` for supported
  Unit, primitive, String, Symbol, Null, object, and boxed/object-backed `Any`
  receivers. Unit and Null return `0`, Boolean/Long/Float/Double use typed
  runtime helpers, String and Symbol use a Java-style content hash, and objects
  use an address-derived identity hash. String values stored through `Any` now
  use the boxed String descriptor and retain content hash/equality behavior.
- `if` expressions now lower to LLVM branch and merge blocks rather than an
  eager `select`. Only the selected branch executes its calls, assignments, or
  block-local setup; value-producing conditionals merge through a typed `phi`,
  while Unit-valued conditionals retain only control-flow effects. Source and
  NIR validation both require Boolean conditions.
- A first `match` slice accepts literal `Int`, floating, String, Char, Boolean,
  and `Null` cases, plus literal alternatives such as `case 0 | 1`, guarded
  bindings such as `case value if value > 0`, and guarded wildcards such as
  `case _ if ready`. A final unguarded `_` or binding case is required. The parser
  evaluates the selector once into a synthetic local and desugars cases into
  nested equality tests and branch-based `if` expressions. Wildcard and named type
  patterns such as `case _: Type` and `case value: Type`, plus wildcard type
  alternatives such as `case _: First | _: Second`, reuse runtime metadata tests
  for known classes and traits; named bindings receive a checked, refined local
  type in their guard and body. Unqualified capitalized object patterns such as
  `case Ready`, `case Ready | Waiting`, and qualified paths such as
  `case State.Ready` lower to singleton identity comparisons. Extractors and
  exhaustiveness analysis beyond the final catch-all remain future work.
- `while` expressions now lower to LLVM condition, body, and exit blocks.
  Conditions are re-evaluated at the top of each iteration, while mutable
  locals retain their slot-backed updates through the loop body; an initially
  false condition executes no body effects. Source and NIR validation both
  require Boolean conditions.
- Boolean `&&` and `||` now have Scala-like precedence below comparisons and
  lower through exclusive right-hand-side blocks. Their operands are checked as
  Boolean, and the compiler emits a Boolean `phi` result so a skipped operand
  cannot call, allocate, mutate, or print.
- Primitive bitwise `&`, `|`, and `^` lower to LLVM `and`, `or`, and `xor` for
  `Boolean`, `Int`, and `Long`. Boolean bitwise operations remain eager, unlike
  short-circuit `&&`/`||`, so both operand effects execute in source order.
- Primitive shifts `<<`, `>>`, and `>>>` accept an `Int` count for `Int` or
  `Long` left operands and lower to LLVM `shl`, `ashr`, and `lshr`. Generated
  code masks counts by `31` or `63` before shifting, preserving Scala/JVM count
  wrapping and avoiding LLVM poison for oversized counts.
- Interflow folds literal `Boolean`/`Int`/`Long` bitwise expressions and
  `Int`/`Long` shifts using explicit-width two's-complement operations. Shift
  folding shares codegen's masked-count, arithmetic-right, and logical-right
  semantics without relying on host-language signed-shift behavior.
- Numeric remainder (`%`) now lowers for `Int` and `Long` through LLVM signed
  remainder and for `Float` and `Double` through LLVM floating remainder.
  Interflow folds integral constants when the divisor is non-zero; unsupported
  scalar operands receive a source-level type diagnostic.
- Float and Double `+`, `-`, `*`, and `/` now lower to LLVM floating-point
  arithmetic. Existing scalar literal typing, comparison lowering, boxing, and
  `println` promotion share the same primitive ABI.
- Unix-like native binary links now include the system math library by default,
  which supplies the platform `fmod` implementation used by floating remainder.
- Unary `!`, `+`, and `-` are represented explicitly in the AST and NIR. Boolean
  inversion lowers to LLVM `xor`; unary plus preserves any supported numeric
  value; unary minus lowers through integer subtraction or LLVM `fneg` for
  Float/Double. Interflow folds integral literal unary values, while unsupported
  operands receive source-level diagnostics.
- `String + String` now lowers through a generated C-string runtime helper. The
  helper allocates tagged backing storage in the program arena and returns its
  payload pointer, so dynamic concatenation survives scope exit and is released
  during runtime shutdown. When either operand is String, the supported
  primitive values (`Boolean`, `Int`, `Long`, `Float`, `Double`, and `Char`),
  `Unit`, `Symbol`, `Null`, and object-like references lower through
  program-arena conversion helpers before concatenation; boxed/object values use
  the runtime `anyToString` helper.
- Compiler-known zero-argument `.toString` now exposes those same conversion
  helpers for primitive receivers, while `String.toString` is a no-allocation
  pass-through. Both `value.toString` and `value.toString()` work for boxed
  `Any`, `Unit`, `Symbol`, `Null`, and object-like references through
  `scala.scalanative.runtime.anyToString`. When the receiver has a resolvable
  user-defined zero-argument `toString`, the typechecker and emitter route the
  direct call through the selected member instead, so class overrides participate
  in the existing virtual dispatch path. The generic `anyToString` helper also
  probes the object's runtime vtable for a compatible zero-argument `toString`
  slot, letting `println(obj)`, `println(any)`, and `"x" + obj` use overrides
  when available; descriptor names remain the fallback for objects without that
  slot.
- String `==` and `!=` now compare C-string contents rather than pointer
  identity. The generated helper treats identical pointers as equal, handles
  null safely, and delegates non-null content comparison to the platform C
  library. Compiler-known `String.equals(value)` reuses that same helper for
  statically String-compatible values.
- Compiler-known `String.length` selections now type as `Int`, emit a typed
  `(String)Int` runtime call, and lower through `strlen` with a checked ABI
  narrowing step. It works for both string literals and program-arena-backed
  concatenation results.
- `s"..."` interpolation now desugars `$identifier` and `${expression}` holes
  into ordinary String concatenation AST nodes, reusing String typing and the
  arena-backed runtime helper. Hole values can be String, the supported
  primitives, or `Null`. `raw"..."` preserves raw literal bytes while sharing
  the same hole semantics. The initial `f"..."` slice supports Float/Double
  holes with an immediately following validated `%...f` specifier, Int/Long
  holes with `%...d`, Char holes with `%...c`, String holes with `%...s`, and
  Boolean holes with `%...b`. These lower through an arena-backed `snprintf`
  buffer, selecting static `true`/`false` text for Boolean values; object
  formatting, other directives, and primitive combinations receive focused
  diagnostics.
  Triple-quoted `s"""..."""` interpolation preserves embedded quotes and
  newlines through NIR and LLVM lowering.
- NIR lowering now preserves local bindings when a block appears as a
  subexpression, so scoped blocks work in interpolation holes as well as call
  arguments, operators, conditions, assignments, and selections.
- `ret Unit call ...` and `eval call ...` now lower side-effecting calls before
  returning or discarding the result.
- Smoke tests compile and run a printable native binary and assert the emitted
  output.
- Class methods now lower with an explicit implicit receiver parameter in NIR
  when their owner is a class or trait.
- `new ClassName(...)` is represented as a NIR `new` value with constructor
  operands for the supported direct class-constructor subset.
- The native subset supports direct instance method calls for simple classes,
  including `val c = new Counter(42)`, `c.value`, and `this.value` inside class
  methods. Direct inherited `def` calls now resolve statically through parent
  metadata. A first closed-world vtable path handles base-typed class-method
  receivers such as `def read(x: Base) = x.value` when `new Child()` is passed,
  plus a `with`-composition trait dispatch MVP for concrete trait defaults and
  class overrides.
- Class allocation now uses a descriptor header followed by the payload layout for constructor
  parameter fields and initialized class-body `val`/`var` fields. Allocation
  lowering stores constructor arguments first, then calls a synthetic
  `Class.$init` that executes class-body field initializers and constructor
  statements in source order. Explicit `val`/`var` constructor parameters are
  accepted for this layout, mutable class-field assignments lower to direct
  payload stores, and mutable local variables lower to stack slots. Classes that
  participate in an inheritance hierarchy expose vtables through that descriptor;
  standalone classes carry the same header with an optional vtable. Inherited initialized
  parent fields are laid out before child fields, and `extends Parent(args...)`
  lowers parent constructor arguments into the synthetic child initializer.
  Explicit `super.member`/`super.method(...)` selections lower to parent
  symbols. Qualified direct-parent trait calls such as `super[Named].name` also
  lower directly to the selected trait symbol and remain stable under
  optimization. Abstract trait `val` accessors can now be implemented by either
  concrete class-body `override val` fields or explicit constructor `val`
  parameters and dispatch correctly in optimized binaries. Initialized trait
  `val`s and `var`s now use one-time per-instance storage and ordered trait
  initialization. Trait-variable getters and setters occupy virtual slots, so
  mutation through a trait-typed receiver remains correct under optimization.
  There is still no GC metadata, explicit `super(...)` constructor-call syntax,
  self types, or classpath-wide linearization metadata.

## Milestones

### M0: Project Skeleton

- Choose C++ standard, build system, LLVM version, formatter, test framework,
  and CI shape.
- Create core support libraries: diagnostics, source manager, arenas, GC handles,
  IDs, and interners.

Started choices:

- C++ standard: C++20.
- Build system: CMake with `debug` and `release` presets.
- Formatter baseline: `.clang-format` using LLVM style.
- Test framework: first pass uses plain executable smoke tests through CTest;
  a richer framework can be introduced when the test surface grows.
- Module prefix: top-level modules and CMake targets use `cpp-`.
- CI shape: GitHub Actions configure, build, and CTest workflow.

### M1: Parseable Scala Subset

- Source, lexer, parser, and AST for packages, objects, defs, vals, literals,
  blocks, calls, `if`, `while`, and primitive operators.

### M2: Typed Minimal Subset

- Symbols, scopes, primitive types, method calls, local inference, and basic
  class/object typing.

### M3: NIR Producer

- NIR library, verifier, textual golden tests, and typed-AST-to-NIR lowering for
  a minimal program.

### M4: First Native Binary

- Linker, codegen, build driver, runtime startup, and a runnable executable for
  a small program.

### M5: Object-Oriented Scala

- Classes, traits, modules, constructors, fields, virtual dispatch, trait
  dispatch, arrays, boxing, exceptions, and module initialization.

### M6: Scala Native Compatibility

- Scala Native annotations, intrinsics, NIR format compatibility decisions,
  native interop, runtime APIs, and standard-library integration strategy.

### M7: Optimizing Toolchain

- Interflow passes, incremental compilation, debug info, platform hardening,
  performance budgets, and binary-size work.

## Testing Strategy

- Unit tests per phase: source, lexer, parser, AST, typecheck, NIR, checker,
  linker, interflow, codegen, build.
- Golden tests for tokens, AST debug output, NIR text, linker reports, and LLVM
  IR shape.
- Integration tests that compile and run small Scala programs.
- Differential tests against upstream Scala/Scala Native behavior where legally
  and practically useful.
- Fuzz tests for lexer/parser and NIR parser.
- Sanitizer runs for C++ memory errors.
- GC stress mode and arena lifetime assertions.

## Open Decisions

- Primary Scala dialect: Scala 2.13-like syntax first, Scala 3 syntax first, or
  a shared subset.
- Build system: CMake, Bazel, Meson, or another system.
- LLVM version floor and supported target triples.
- NIR compatibility target: exact Scala Native NIR compatibility or a C++
  implementation with compatible concepts but independent serialization.
- Standard library strategy: consume existing Scala Native libraries, recompile
  them, or implement a staged subset.
- Runtime GC strategy details: continue the non-moving mark-sweep foundation,
  evaluate Immix/Commix-style baselines, and develop the compiler-co-designed
  Scala Native Garbage Collector Theory track before selecting a mature default.
- Legal policy for using upstream implementation details beyond public
  documentation and high-level structure.
