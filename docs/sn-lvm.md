# Task: Design a Detailed Scala Native Garbage Collector Compiler Pipeline

Design and explain a complete compiler pipeline for a Scala Native-specific garbage collector and memory model.

The design must operate primarily on Scala Native NIR and should integrate compiler analysis, allocation placement, garbage collector barriers, native interoperability, thread publication, lifetime inference, safepoints, and LLVM lowering.

The result should not merely describe a garbage collection algorithm. It must describe how the compiler analyzes every allocation and memory operation before lowering them into LLVM IR.

The proposed system may be called:

> **Scala Native GC Compiler Pipeline**

Treat this as a new theoretical architecture rather than a description of the current official Scala Native implementation.

---

# Main Objective

Explain how Scala Native can use whole-program analysis and NIR-level information to minimize the amount of work performed by the garbage collector.

The central principle should be:

> The first optimization goal is not to make garbage collection faster, but to eliminate unnecessary allocations, place remaining allocations in the most appropriate memory region, remove unnecessary barriers, and provide the collector with precise metadata.

The pipeline must analyze managed objects, native pointers, stack allocations, Zones, closures, arrays, threads, virtual threads, native callbacks, pinned objects, and long-lived objects.

---

# Required Pipeline

The design must explain the following pipeline in detail:

```text
Scala AST / TASTy
        ↓
Initial NIR
        ↓
Escape Analysis
        ↓
Publication Analysis
        ↓
Lifetime Inference
        ↓
Interop Escape Analysis
        ↓
Allocation Placement
        ↓
Barrier Insertion
        ↓
Barrier Elimination
        ↓
Safepoint Placement
        ↓
Managed Reference Liveness
        ↓
Precise Stack Map Generation
        ↓
LLVM IR Generation
        ↓
LLVM Optimization
        ↓
Native Machine Code + GC Metadata
```

The first four analyses should be allowed to run as a fixed-point computation:

```text
Escape Analysis
      ↓
Publication Analysis
      ↓
Lifetime Inference
      ↓
Interop Escape Analysis
      └───────────────→ repeat until no facts change
```

---

# General Requirements for Every Stage

For every pipeline stage, explain all of the following:

1. Its purpose.
2. The exact question it answers.
3. Its input data.
4. Its output data.
5. The analysis algorithm it may use.
6. The NIR operations it examines.
7. The metadata it produces.
8. Examples using Scala source code.
9. Examples using simplified NIR.
10. How the NIR changes after the pass.
11. How the stage interacts with previous and later passes.
12. Which optimizations it enables.
13. Which safety invariants it enforces.
14. What can go wrong if the analysis is too optimistic.
15. What performance is lost if the analysis is too conservative.
16. How the result should be verified.
17. Which facts are proven and which facts are heuristic.
18. Whether the analysis must be intraprocedural, interprocedural, context-sensitive, or whole-program.

Do not explain stages only at a conceptual level. Show concrete compiler data structures, lattices, graphs, instruction transformations, and validation rules.

---

# 1. Initial NIR

Explain what information should exist in the initial NIR before GC-specific analysis.

The initial NIR should represent program semantics without prematurely deciding the final collector strategy.

Include operations such as:

```text
classalloc
arrayalloc
stackalloc
fieldload
fieldstore
arrayload
arraystore
load
store
call
return
throw
```

Every allocation must receive a stable allocation-site identity:

```scala
final case class AllocationSiteId(
    owner: Global.Member,
    localId: Int,
    originHash: Long
)
```

Explain how allocation identities survive:

* Inlining,
* Method cloning,
* Specialization,
* Devirtualization,
* Dead-code elimination,
* NIR serialization,
* Link-time optimization.

Explain why detailed GC conclusions should not initially be stored directly inside `classalloc`.

---

# 2. GC Analysis Database

Design a side-table-based analysis system rather than placing all mutable analysis facts directly inside NIR instructions.

Use a structure similar to:

```scala
final case class GCAnalysisDatabase(
    allocations: Map[AllocationSiteId, AllocationFacts],
    values: Map[Local, ValueFacts],
    stores: Map[InstructionId, StoreFacts],
    calls: Map[InstructionId, CallFacts],
    safepoints: Map[InstructionId, SafepointFacts],
    methods: Map[Global.Member, MethodGCSummary]
)
```

Explain why this side table is useful for:

* Analysis invalidation,
* Re-running passes,
* Incremental compilation,
* Whole-program optimization,
* Debugging,
* Optimization reports,
* Profile-guided optimization.

Include an analysis preservation and invalidation model:

```scala
trait NirPass:
  def preserves: Set[AnalysisKind]
  def invalidates: Set[AnalysisKind]
```

---

# 3. Allocation Facts

Define a rich descriptor for every allocation site.

Use or improve a model similar to:

```scala
final case class AllocationFacts(
    site: AllocationSiteId,
    allocatedType: Type,

    escape: EscapeClass,
    visibility: VisibilityClass,
    lifetime: LifetimeClass,
    mobility: MobilityClass,
    interop: InteropClass,

    identityDemand: IdentityDemand,
    synchronizationDemand: SynchronizationDemand,

    size: SizeEstimate,
    referenceShape: ReferenceShape,

    loopDepth: Int,
    hotness: HotnessEstimate,

    placement: Option[PlacementDecision],
    confidence: Confidence,

    reasons: Vector[AnalysisReason]
)
```

Every conclusion must include an explanation trail.

Example:

```text
Allocation User#17

Escape:
    HeapEscape

Reason:
    passed as argument 1 to ConcurrentQueue.offer
    Queue.offer publishes its argument
    queue is reachable from a global root

Visibility:
    Unpublished until instruction #34
    SharedMutable after instruction #34

Placement:
    LocalYoung followed by transitive publication
```

---

# 4. Escape Analysis

Explain how the compiler determines whether an object escapes its allocation scope.

Use an escape lattice such as:

```scala
enum EscapeClass:
  case NoEscape
  case BlockEscape
  case MethodEscape
  case ReturnEscape
  case CallerEscape
  case HeapEscape
  case GlobalEscape
  case NativeEscape
  case UnknownEscape
```

Explain:

* Direct escape,
* Transitive escape,
* Escape through fields,
* Escape through arrays,
* Escape through closures,
* Escape through exceptions,
* Escape through virtual calls,
* Escape through unresolved calls,
* Escape through native calls,
* Escape through returned values,
* Escape through global roots.

Describe points-to graph construction.

Example:

```text
Address#12
    ↑ User.address
User#17
    ↑ Closure.capture0
Closure#21
    ↑ return
```

Show how escape propagates backwards through the object graph.

Explain scalar replacement:

```text
%p = classalloc Point
fieldstore %p.x, %x
fieldstore %p.y, %y
%a = fieldload %p.x
%b = fieldload %p.y
%r = add %a, %b
```

becoming:

```text
%r = add %x, %y
```

Explain partial escape analysis and materialization of virtual objects.

---

# 5. Publication Analysis

Explain the difference between escape and publication.

An object may escape a method while still remaining thread-local.

Use visibility states such as:

```scala
enum VisibilityClass:
  case Unpublished
  case ThreadLocal
  case TaskLocal
  case SharedReadOnly
  case SharedMutable
  case NativeVisible
  case UnknownShared
```

Explain publication through:

* Static fields,
* Volatile stores,
* Atomic stores,
* Concurrent queues,
* Executors,
* Thread startup,
* Actor messages,
* Virtual-thread schedulers,
* Native callbacks,
* Foreign handles,
* Global runtime structures.

Show state transitions:

```text
Unpublished
     ↓ initialize fields
ThreadLocal
     ↓ queue.offer
SharedMutable
```

Explain transitive publication of the reachable object graph.

Example:

```text
User → Address → String
```

Explain why constructor stores may not require publication or shared-object barriers while the object remains unpublished.

Clearly distinguish:

* GC publication,
* Memory-model synchronization,
* Release/acquire semantics,
* Happens-before relationships.

A GC barrier must not be treated as a replacement for thread synchronization.

---

# 6. Lifetime Inference

Explain how the compiler estimates or proves object lifetime.

Use classes such as:

```scala
enum LifetimeClass:
  case Expression
  case BasicBlock
  case StackFrame
  case LexicalRegion
  case LoopIteration
  case Task
  case Request
  case Young
  case LongLived
  case Program
  case External
  case Unknown
```

Separate:

```text
Proven lifetime
```

from:

```text
Predicted lifetime
```

Only proven information may be used for correctness-sensitive placement such as stack allocation.

Heuristic or profile-based information may be used for:

* Young generation placement,
* Mature pretenuring,
* Large-object placement,
* Nursery sizing.

Explain lifetime constraints between owners and referenced objects:

```text
Mature object → Stack object
```

must be illegal.

Use a constraint such as:

```text
requiredLifetime(referent) >= retainedLifetime(owner)
```

Explain lifetime inference from:

* Loop nesting,
* Call graph,
* Escape information,
* Publication information,
* Startup initialization,
* Cache insertion,
* Request scopes,
* Task scopes,
* Profile-guided survival rates.

---

# 7. Interop Escape Analysis

Design a detailed analysis for Scala Native C interoperability.

The analysis must determine:

* Where a pointer originated,
* Whether native code retains it,
* Whether native code mutates it,
* Whether native code calls back into Scala,
* Whether the callback is asynchronous,
* Whether native code uses another thread,
* Whether the call blocks,
* Whether a returned pointer is owned or borrowed.

Use pointer origins such as:

```scala
enum PointerOrigin:
  case Stack
  case Zone
  case ManagedHeap
  case PinnedHeap
  case NativeMalloc
  case StaticData
  case StableHandle
  case Unknown
```

Use interop states such as:

```scala
enum InteropClass:
  case NoInterop
  case CallBounded
  case RetainedSynchronous
  case RetainedAsynchronous
  case CallbackCaptured
  case NativeOwned
  case UnknownInterop
```

Design native effect summaries:

```scala
final case class ExternGCEffects(
    blocking: TriState,
    retainedArguments: BitSet,
    mutatedArguments: BitSet,
    asynchronouslyUsedArguments: BitSet,
    invokesCallback: TriState,
    invokesCallbackAsync: TriState,
    maySpawnNativeThread: TriState,
    returnedPointerOwnership: PointerOwnership,
    deallocator: Option[Global.Member]
)
```

Explain the decision between:

```text
Copy
Pin lease
Stable handle
Native-owned allocation
```

Explain why raw pointers to movable managed objects cannot be retained by native code.

Explain base and derived pointer tracking:

```text
derivedPointer = baseObject + offset
```

Explain how foreign calls transition threads between managed and unmanaged states.

Unknown native effects must use conservative assumptions.

---

# 8. Mobility Analysis

Determine whether an object may move during collection.

Use:

```scala
enum MobilityClass:
  case Eliminated
  case Movable
  case TemporarilyPinned
  case PermanentlyPinned
  case NonMovingRegion
  case UnknownMobility
```

Explain how mobility is affected by:

* Raw address exposure,
* Interior pointers,
* Native retention,
* Pin operations,
* Large-object placement,
* Identity hash codes,
* Synchronization,
* Monitors,
* Stable handles.

Explain why:

```text
identity required
```

does not necessarily mean:

```text
permanently pinned
```

Show how stable identities and monitor tables can be implemented through side metadata.

---

# 9. Object Shape and Size Analysis

Explain how NIR type information and runtime layout metadata can provide:

```scala
final case class ReferenceShape(
    objectSize: SizeEstimate,
    alignment: Int,
    referenceOffsets: Vector[Int],
    primitiveBytes: Int,
    scanKind: ScanKind
)
```

Use scan kinds such as:

```scala
enum ScanKind:
  case NoScan
  case FixedOffsets
  case ReferenceArray
  case PrimitiveArray
  case Mixed
  case Unknown
```

Explain special handling for:

* Primitive arrays,
* Reference arrays,
* Large arrays,
* Mixed-layout objects,
* Boxed primitives,
* Closures,
* Case classes,
* Tuples,
* Module objects,
* RTTI objects,
* Continuation chunks.

Include estimated GC scan cost.

---

# 10. Allocation Placement

Design a placement solver that selects among:

```scala
enum Placement:
  case ScalarReplacement
  case Stack
  case Zone
  case TaskArena
  case LocalYoung
  case SharedYoung
  case MatureImmix
  case LargeObjectSpace
  case PinnedSpace
  case StaticSpace
  case NativeExternal
```

The solver must use constraints and preferences.

Example:

```text
Candidate placements:
    Scalar
    Stack
    LocalYoung
    SharedYoung
    Mature

Constraints:
    escapes method
        → reject Scalar and Stack

published to another thread
        → reject LocalYoung unless publication promotion is inserted

native retained pointer
        → require PinnedSpace, StableHandle, or NativeExternal

high survival probability
        → prefer Mature
```

For every rejected placement, report the reason.

Example:

```text
Stack rejected:
    escapes through Queue.offer

LocalYoung conditionally accepted:
    requires transitive publication before queue insertion

Mature rejected:
    low predicted survival probability
```

---

# 11. GC-Aware NIR Dialect

After placement decisions become stable, lower ordinary NIR into a temporary GC-aware dialect.

Include operations such as:

```text
gc.alloc.local
gc.alloc.shared
gc.alloc.mature
gc.alloc.large
gc.alloc.pinned

gc.stackobject
gc.publish
gc.promote

gc.pin
gc.unpin

gc.handle.retain
gc.handle.release

gc.safepoint
gc.keepalive

gc.native.enter
gc.native.leave

gc.derivedptr
```

Explain that this dialect may be internal to the optimizer and does not necessarily need to be serialized as public NIR.

Show transformation:

```text
%user = classalloc @User
```

into:

```text
%user = gc.alloc.local @User, site #17
```

and later:

```text
gc.publish.transitive %user, release
```

---

# 12. Barrier Insertion

Explain why barriers are needed and design multiple barrier kinds:

```scala
enum BarrierKind:
  case Generational
  case Publication
  case ConcurrentMarking
  case ReferenceAccounting
  case Handle
  case Pin
  case Allocation
  case Read
```

Explain:

## Generational barriers

```text
Old object → Young object
```

Remembered sets and card marking.

## Publication barriers

Moving an object graph from local or unpublished state to shared state.

## Concurrent-marking barriers

Explain both:

```text
SATB pre-barrier
```

and:

```text
Incremental-update post-barrier
```

## Reference-accounting barriers

Coalescing multiple slot updates within one epoch.

## Handle barriers

Maintaining stable handle root tables.

## Pin barriers

Synchronizing pin state with evacuation.

## Allocation barriers

Correctly coloring objects allocated during concurrent marking.

## Array and bulk barriers

Optimize:

```text
System.arraycopy
memcpy-like reference movement
```

using bulk barriers rather than one barrier per element.

## Atomic barriers

Handle CAS and atomic stores correctly.

Show simplified NIR before and after barrier insertion.

---

# 13. Barrier Elimination

Explain why the compiler should first insert conservative barriers and then eliminate unnecessary ones.

Remove barriers when proven safe due to:

* Unpublished owner,
* Primitive field,
* Young-to-young edge,
* Old-to-old edge for generational purposes,
* Scalar replacement,
* Stack placement,
* Dead stores,
* Dominating card marks,
* Bulk card coverage,
* Constructor initialization,
* Immutable graph construction,
* Same-region stores,
* Null values where applicable.

Explain that barrier kinds must be optimized independently.

For example, storing `null` may remove a generational post-barrier but may still require a SATB pre-barrier for the previous value.

Include barrier-verification strategies for debug builds.

---

# 14. Safepoint Placement

Explain where safepoints should appear:

```scala
enum SafepointKind:
  case AllocationSlowPath
  case ManagedCall
  case LoopBackedge
  case ExplicitPoll
  case NativeTransition
  case BlockingCall
  case CallbackEntry
  case ThreadPark
  case ContinuationSuspend
```

Discuss tradeoffs:

* Too many safepoints increase execution overhead.
* Too few safepoints increase pause latency.
* Tight loops require bounded time to reach a safepoint.
* Native calls require thread-state transitions.
* Virtual-thread suspension requires continuation metadata.

Show safepoint poll lowering.

---

# 15. Managed Reference Liveness

Explain how the compiler determines which managed references are live at each safepoint.

Include:

* SSA liveness,
* Phi nodes,
* Exceptional control flow,
* Temporary expression values,
* Register candidates,
* Stack spills,
* Stack objects containing references,
* Base and derived pointers,
* Continuation slots,
* Foreign-call root snapshots.

Explain why named source variables are insufficient.

Example:

```text
%fresult = call @f()
%gresult = call @g()  // may safepoint
%result = call @h(%fresult, %gresult)
```

`%fresult` must remain a root during `@g()`.

---

# 16. Precise Stack Map Generation

Design logical stack-map facts at NIR level and physical stack maps at LLVM code-generation level.

Use:

```scala
final case class SafepointFacts(
    instruction: InstructionId,
    kind: SafepointKind,
    liveManagedValues: Vector[LiveReference],
    derivedPointers: Vector[DerivedPointerFact],
    stackObjects: Vector[StackObjectFact],
    estimatedRootCount: Int,
    rootScanCost: CostEstimate
)
```

Explain root kinds:

```scala
enum RootKind:
  case Register
  case StackSlot
  case StackObjectField
  case DerivedPointer
  case Handle
  case ContinuationSlot
  case StaticRoot
  case PinnedRoot
```

Explain:

* Base/derived pointer relationships,
* Relocation,
* Exceptional unwind paths,
* Parked continuations,
* Native-state thread snapshots,
* Root minimization,
* Root-map verification.

Compare:

```text
LLVM stack maps
```

with:

```text
LLVM GC statepoints
```

Explain when relocating collectors require `gc.relocate`.

---

# 17. LLVM Lowering

Explain lowering of:

* Local allocation fast paths,
* Slow allocation paths,
* Stack allocations,
* Zone allocations,
* Shared allocations,
* Mature allocations,
* Large-object allocations,
* Pinned allocations,
* Card marking,
* SATB barriers,
* Publication operations,
* Pin/unpin operations,
* Handle operations,
* Safepoint polls,
* Statepoints,
* Native state transitions,
* RTTI and type maps.

Show simplified LLVM IR examples.

Allocation fast path should resemble:

```llvm
%cursor = load ptr, ptr %thread.allocCursor
%next   = getelementptr i8, ptr %cursor, i64 %size
%limit  = load ptr, ptr %thread.allocLimit
%fits   = icmp ule ptr %next, %limit
br i1 %fits, label %fast, label %slow
```

Explain why allocation and barrier fast paths must be inline and must not require an expensive foreign function call for every operation.

---

# 18. GC Verification

Design a separate verifier from the ordinary structural NIR checker.

Suggested architecture:

```text
nir.Check
    → type and structural correctness

gc.GCAnalyze
    → produces GC facts

gc.GCOptimize
    → chooses placement and barriers

gc.GCVerify
    → verifies memory-model invariants

gc.GCAudit
    → generates human-readable reports
```

Verify invariants such as:

```text
A stack object cannot escape its frame.

A Zone pointer cannot survive after the Zone closes.

A continuation cannot retain a pointer into a dead native stack.

A movable managed object cannot be passed as a retained raw native pointer.

A mature object cannot point to an unregistered local-only object.

An old-to-young edge must be represented in the remembered set.

A live managed reference must appear in the safepoint root map.

A relocated object pointer cannot be replaced by its stale pre-GC address.

A static object cannot retain a pointer into a shorter-lived region.
```

Produce detailed diagnostics including escape paths and suggested valid placements.

---

# 19. Method Summaries

Design interprocedural GC summaries:

```scala
final case class MethodGCSummary(
    parameters: Vector[ParameterEffect],
    result: ResultEffect,

    mayAllocate: Boolean,
    maySafepoint: Boolean,
    mayPublish: Boolean,
    mayBlock: Boolean,

    mayEnterNative: Boolean,
    mayRetainNativePointer: Boolean,
    mayInvokeCallback: Boolean,

    allocations: AllocationSummary,
    writes: WriteSummary
)
```

Parameter effects may include:

```scala
enum ParameterEffect:
  case Unused
  case ReadOnly
  case Mutated
  case Returned
  case StoredInReceiver
  case StoredInHeap
  case StoredInGlobal
  case Published
  case NativeCallBounded
  case NativeRetained
  case Unknown
```

Explain summary fixed-point computation for recursive call graphs.

---

# 20. Metrics and Optimization Reports

The GC checker must provide detailed static metrics rather than only reporting correctness errors.

Include metrics for:

## Allocations

```text
Total allocation sites
Estimated allocated bytes
Loop-weighted allocated bytes
Scalar-replaced allocations
Stack allocations
Zone allocations
Local-young allocations
Shared-young allocations
Mature allocations
Pinned allocations
Large-object allocations
```

## Escape

```text
No-escape rate
Method-escape rate
Heap-escape rate
Global-escape rate
Native-escape rate
Unknown-escape rate
```

## Publication

```text
Unpublished construction count
Thread-local object count
Shared-read-only object count
Shared-mutable object count
Transitive publication count
```

## Barriers

```text
Potential barriers
Inserted barriers
Eliminated barriers
Barrier elimination percentage
Card marks
SATB logs
Publication barriers
Handle barriers
```

## Safepoints

```text
Safepoint count
Average live root count
Maximum live root count
Derived pointer count
Continuation root count
Estimated root scanning cost
```

## Interop

```text
Extern calls
Unknown extern-effect calls
Blocking transitions
Pin count
Estimated pinned bytes
Stable handle count
Native copy count
Retained-pointer hazards
```

## Object shape

```text
Average object size
Average reference-field count
Primitive-to-reference payload ratio
Large-object candidates
Types contributing most to heap scanning
```

---

# 21. Loop-Weighted Metrics

Explain why static allocation-site counts are insufficient.

Calculate an allocation pressure score such as:

```text
object size
× estimated execution frequency
× loop weight
× survival cost
```

Distinguish a one-time 8 KB startup allocation from a 24-byte allocation executed millions of times inside a loop.

Include optimization rankings:

```text
Top allocation sites by estimated pressure
Top sites preventing scalar replacement
Top sources of native pinning
Top sources of barrier traffic
Top contributors to root scanning
```

---

# 22. Profile-Guided Optimization

Design runtime profiling keyed by `AllocationSiteId`.

Runtime metrics may include:

```text
Allocation count
Allocated bytes
Minor-GC survival rate
Promotion rate
Average object age
Publication frequency
Pin duration
Pin frequency
Remembered-set contribution
Dirty-card count
Evacuation success
Fragmentation contribution
```

Explain sampled profiling to avoid incrementing counters on every allocation.

Use profile feedback to improve:

* Pretenuring,
* Nursery sizing,
* Large-object thresholds,
* Publication placement,
* Copy-versus-pin decisions,
* Safepoint density.

Clearly distinguish metric origins:

```scala
enum MetricOrigin:
  case Proven
  case StaticEstimate
  case Heuristic
  case Profiled
  case Sampled
  case Unknown
```

Correctness-sensitive decisions must use proven facts only.

---

# 23. Serialization and Build Pipeline

Explain which data may be stored in library NIR and which must be computed after whole-program linking.

Safe to serialize:

```text
Allocation-site identities
Type and reference shapes
Local method effects
Extern effect annotations
May-allocate information
May-safepoint information
Source positions
Provenance
```

Compute after linking:

```text
Final escape state
Final publication graph
Final lifetime
Allocation placement
Barrier plan
Safepoint root maps
Pretenuring
Copy/pin/handle decision
```

Design optional sidecar outputs:

```text
application
application.gcmap
application.gcreport.json
application.gcir
application.gcprofile
```

---

# 24. Compiler Modes

Design compiler options such as:

```text
--gc-check=off
--gc-check=basic
--gc-check=full
--gc-check=audit
--gc-check=profile
--gc-check=verify-barriers
```

Explain the compilation-time and runtime cost of each mode.

---

# 25. Complete End-to-End Example

Provide at least one full example beginning with Scala source:

```scala
def submitJob(name: String): Unit =
  val address = new Address("Istanbul")
  val user = new User(name, address)

  Zone {
    val nativeBuffer = makeNativeBuffer(user.name)
    nativeLog(nativeBuffer)
  }

  workerQueue.offer(user)
```

Show:

1. Initial NIR.
2. Allocation-site indexing.
3. Points-to graph.
4. Escape results.
5. Publication transitions.
6. Lifetime results.
7. Interop effects.
8. Mobility results.
9. Placement decisions.
10. Inserted barriers.
11. Eliminated barriers.
12. Safepoint placement.
13. Live-root analysis.
14. GC-aware NIR.
15. Simplified LLVM IR.
16. Final stack-map metadata.
17. Generated optimization report.

The example should demonstrate that:

* `User` and `Address` are unpublished during construction.
* Constructor barriers can be removed.
* Publication occurs at `workerQueue.offer`.
* Publication is transitive through the object graph.
* The native buffer is Zone-scoped.
* The native call does not retain the pointer.
* The queue call may be a safepoint.
* Only necessary roots are included in the stack map.

---

# 26. Final Architectural Recommendation

End with a recommended implementation order:

```text
1. AllocationSiteId
2. Method effect summaries
3. Points-to graph
4. Escape analysis
5. Publication analysis
6. Lifetime constraints
7. Interop effect analysis
8. Mobility analysis
9. Allocation placement
10. Store and barrier facts
11. Barrier insertion
12. Barrier elimination
13. Safepoint placement
14. Managed-reference liveness
15. GC verifier
16. LLVM lowering
17. Runtime profile feedback
```

Also provide a smaller minimum viable implementation:

```text
AllocationSiteId
    ↓
Escape Analysis
    ↓
Thread-local versus Shared classification
    ↓
Scalar / Stack / Young placement
    ↓
Generational barrier planning
    ↓
Safepoint root liveness
```

Later extensions may include:

```text
Zone lifetime verification
Native retention analysis
Pin and stable-handle support
Task arenas
Profile-guided pretenuring
Concurrent-marking barriers
Continuation-aware stack maps
```

---

# Writing Style

The response must be:

* Highly technical,
* Explicit,
* Implementation-oriented,
* Structured with Markdown headings,
* Rich in Scala, NIR, and LLVM examples,
* Clear about proven facts versus heuristics,
* Clear about correctness versus performance decisions,
* Honest when discussing theoretical or proposed behavior.

Do not merely summarize the pipeline.

Treat the output as a preliminary compiler and runtime design document that could guide the actual implementation of a Scala Native GC analysis and optimization subsystem.
