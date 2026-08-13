# scalanative

`scalanative` is a C++ bootstrap compiler for Scala 3 programs. Its goal is to
compile Scala source through a typed frontend and Native Intermediate
Representation (NIR), then use LLVM to produce native executables.

The project is under active development and currently implements a growing
subset of Scala 3 and the Scala Native runtime model.

## Build and use

The project requires CMake, Ninja, a C++20 compiler, LLVM, Clang, and LLD.

```bash
cmake --preset debug
cmake --build --preset debug
```

Make the compiler available on your `PATH`, then inspect its options or compile
a Scala program:

```bash
scalanative --help
scalanative --build-binary --output hello Hello.scala
```

Use `--emit-nir` or `--emit-llvm` when you only want an intermediate artifact.

## Tests

Run the current versioned test suite during development:

```bash
make quick
```

Run all Debug and Release checks before submitting a change:

```bash
make check
```

Active compiler tests are grouped by version under `tests/` as valid, invalid,
runtime, and named C++ smoke tests. See `tests/README.md` for the test layout and
resource conventions.
