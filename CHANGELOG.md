# Changelog

## 1.0.0 — 2026-11-10

The first release.

- The C API in `qtc.h`: handles for objects and values, calls by name or
  signature with overload resolution, properties, values with JSON, lists and
  maps, signals and input events through a queue (`qtc_wait`, `qtc_poll`,
  `qtc_run`), timers, `qtc_post` from any thread, forwarding connections, enums
  by name, drawing (`QtcCanvas`, images), the QML bridge (`QtcBridge`),
  introspection (`qtc_describe`, `manifest/qtc_api.json`).
- For Qt 6.12 LTS, the release it is tested on. Bindings generated from Qt's
  headers: 12,421 methods and 1,009 constructors of 618 classes in 25 modules.
  Qt 6.4 to 6.11 build only with `-DQTC_ALLOW_OLDER_QT=ON` (each method compiled
  only where the release has it) and are not tested.
- About 480 precondition rules (1,761 checks in the generated code; a rule for a
  class also covers the overrides its subclasses declare), ownership rules for 35
  kinds of holders, allocation and work limits, and an application event filter
  that keeps Qt from painting or laying out what it cannot and clears up after
  parent changes Qt mishandles.
- `libqtc_test` for simulated input in tests.
- Tests: the C API suite, an application test, a sweep, a stress test, the
  method fuzzer (fixed order, random orders with random arguments, by name) and
  the lifetime fuzzer; examples that test themselves.
