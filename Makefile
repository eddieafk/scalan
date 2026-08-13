CMAKE ?= cmake
CTEST ?= ctest
JOBS ?= 4

DEBUG_BUILD ?= build/debug
RELEASE_BUILD ?= build/release
TEST_VERSION ?= v0.1.0-alpha0.1.0

INTERFLOW_EXAMPLE ?= tests/v0/examples/InterflowOptimizations.scala
INTERFLOW_NIR ?= /tmp/scalanative-interflow.nir
INTERFLOW_REPORT ?= /tmp/scalanative-interflow-report.json

.PHONY: help
help:
	@printf '%s\n' 'Common targets:'
	@printf '  %-24s %s\n' 'make quick' 'Build and run the current versioned debug tests.'
	@printf '  %-24s %s\n' 'make interflow-example' 'Emit optimized NIR and an interflow JSON report for the example.'
	@printf '  %-24s %s\n' 'make test-debug' 'Build debug and run all debug CTest tests.'
	@printf '  %-24s %s\n' 'make test-release' 'Build release and run all release CTest tests.'
	@printf '  %-24s %s\n' 'make test-version-debug' 'Build and run the current versioned tests only.'
	@printf '  %-24s %s\n' 'make test-version-release' 'Build and run the current versioned tests in release.'
	@printf '  %-24s %s\n' 'make check' 'Run the usual debug smoke, debug CTest, and release CTest validation.'
	@printf '  %-24s %s\n' 'make build-debug' 'Configure and build the debug preset.'
	@printf '  %-24s %s\n' 'make build-release' 'Configure and build the release preset.'
	@printf '%s\n' ''
	@printf '%s\n' 'Variables: JOBS=4 DEBUG_BUILD=build/debug RELEASE_BUILD=build/release'
	@printf '%s\n' '           TEST_VERSION=v0.1.0-alpha0.1.0'
	@printf '%s\n' '           INTERFLOW_EXAMPLE=tests/v0/examples/InterflowOptimizations.scala'
	@printf '%s\n' '           INTERFLOW_NIR=/tmp/scalanative-interflow.nir'
	@printf '%s\n' '           INTERFLOW_REPORT=/tmp/scalanative-interflow-report.json'

.PHONY: configure-debug configure-release
configure-debug:
	$(CMAKE) --preset debug

configure-release:
	$(CMAKE) --preset release

.PHONY: build-debug build-release
build-debug: configure-debug
	$(CMAKE) --build --preset debug --parallel $(JOBS)

build-release: configure-release
	$(CMAKE) --build --preset release --parallel $(JOBS)

.PHONY: test-debug test-release
test-debug: build-debug
	$(CTEST) --test-dir $(DEBUG_BUILD) --output-on-failure

test-release: build-release
	$(CTEST) --test-dir $(RELEASE_BUILD) --output-on-failure

.PHONY: test-version-debug test-version-release
test-version-debug: configure-debug
	$(CMAKE) --build $(DEBUG_BUILD) --target scalanative-tests-$(TEST_VERSION) --parallel $(JOBS)
	$(CTEST) --test-dir $(DEBUG_BUILD) --output-on-failure -L '^$(TEST_VERSION)$$'

test-version-release: configure-release
	$(CMAKE) --build $(RELEASE_BUILD) --target scalanative-tests-$(TEST_VERSION) --parallel $(JOBS)
	$(CTEST) --test-dir $(RELEASE_BUILD) --output-on-failure -L '^$(TEST_VERSION)$$'

.PHONY: quick check-debug check-release check verify
quick: test-version-debug

check-debug: test-debug

check-release: test-release

check: check-debug check-release

verify: check

.PHONY: interflow-example
interflow-example: build-debug
	./$(DEBUG_BUILD)/driver/scalanative --emit-nir --optimize \
		--output $(INTERFLOW_NIR) \
		--optimization-report $(INTERFLOW_REPORT) \
		$(INTERFLOW_EXAMPLE)
	@printf 'optimized NIR: %s\n' '$(INTERFLOW_NIR)'
	@printf 'optimization report: %s\n' '$(INTERFLOW_REPORT)'
