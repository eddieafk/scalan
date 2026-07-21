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
