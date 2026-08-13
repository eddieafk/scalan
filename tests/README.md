# Versioned compiler tests

Tests are grouped by the first compiler version whose behavior they protect.
The initial layout is:

```text
tests/
  include/                       shared C++ test-resource helpers
  issues/                        focused regressions for reported issues
  runner/                        data-driven Scala test runner
  smoke/                         directly implemented, named C++ smoke tests
  v0/
    examples/                    archived pre-versioned public examples
    tests/                       archived pre-versioned test sources
  v0.1.0-alpha0.1.0/
    val/                         Scala sources that must compile
    inval/                       Scala sources that must not compile
    run/                         Scala sources that must compile and run
```

Every resource is registered with an explicit CTest name in its version's
`CMakeLists.txt`. Invalid resources declare required diagnostic substrings with
`// expected-error:` comments. Run resources declare their exact standard output
one line at a time with `// expected-output:` comments.

Direct C++ smoke tests use
`scalanative/testing/TestResources.h` to locate versioned Scala resources and
are registered through `scalanative_add_smoke_test`, which gives each executable
and CTest case a stable name.

The material under `v0/` is preserved as an inactive historical baseline and is
not registered by `tests/CMakeLists.txt`. All active coverage uses the named,
versioned model beginning with `v0.1.0-alpha0.1.0`.
