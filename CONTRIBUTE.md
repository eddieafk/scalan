# Contributing

Thank you for helping improve `scalanative`.

## What to contribute

Useful contributions include:

- focused Scala 3 language support;
- clearer compiler diagnostics;
- NIR validation and optimization improvements;
- LLVM code generation and runtime support;
- small build, test, and documentation improvements;
- versioned regression tests for supported or rejected behavior.

For a large feature or architectural change, open an issue first so its scope
and direction can be agreed before implementation begins.

## How to contribute

Keep each change focused. Follow the existing C++20 style and preserve the
compiler pipeline boundaries. Register tests under the first version whose
behavior they protect:

- `val/` for source that must compile;
- `inval/` for source that must fail with a specific diagnostic;
- `run/` for source that must compile and produce exact output;
- `tests/smoke/` for named direct C++ coverage when source tests are
  insufficient.

Run the focused suite while developing and the complete checks before opening a
pull request:

```bash
make quick
make check
```

Use a clear commit message. In the pull request, explain the behavior being
changed, its limits, and the tests that demonstrate it.

## What not to do

- Do not add new active tests to `tests/v0/`; it is an inactive archive.
- Do not restore the retired legacy smoke suite to the active CMake build.
- Do not reintroduce the retired `cpp-` prefix in modules, targets, or packages.
- Do not mix unrelated refactoring or broad formatting with a compiler change.
- Do not bypass typechecking, NIR verification, or diagnostics to make a test
  pass.
- Do not commit build products, generated artifacts, temporary files, or local
  editor settings.
- Do not silently broaden a feature beyond its documented and tested scope.
