# Changelog

## 1.0.0 — 2026-11-12

The first release: Qt 6.12 LTS for C, and for any language that can call C.

### What it binds
- About 12,400 methods and 1,000 constructors of 618 classes across 25 Qt modules,
  generated from Qt's headers with libclang: widgets, Qt Quick and QML, painting, item
  models, networking, SQL, XML, D-Bus, multimedia, charts, serial ports, positioning,
  WebSockets, MQTT, CoAP and more. Qt Core, Gui and Widgets are in `libqtc`; every other
  module is a library of its own (`libqtc_<Module>`), loaded the first time a program
  uses one of its classes. The GPL-3.0-only modules are left out unless asked for
  ([LICENSING.md](docs/LICENSING.md)).
- Calls by name (`qtc_call`, with overloads resolved at run time) and a C function per
  bound signature (`qtc_direct.h`: `qtcm_setText__QString`, constructors as `qtcn_*`).
- Small value types as C structs with Qt's own layout (`qtc_inline.h`: points, sizes,
  rectangles, lines, margins, colors); other values as handles.
- Signals and watched events queued and polled (`qtc_wait`, `qtc_poll`), or handed to a
  handler inside the emission (`QTC_CONN_DIRECT`, `qtc_set_signal_handler`).
- Subclassing (`qtc_new_overridable`, `qtc_override`), classes defined at run time with
  signals, slots and properties Qt and QML see (`qtc_class_*`), futures, promises, tasks
  and worker threads, and Qt's RHI for GPU rendering and compute (`qtc_rhi_*`).

### How it keeps programs from crashing
- Objects and values are 64-bit handles with a generation count: a handle to something
  destroyed is `QTC_ERR_STALE_HANDLE`, never a dangling pointer.
- Every call checks the receiver (alive, its thread, its class), every argument's type and
  range, and a table of nearly 400 preconditions for inputs that crash or hang Qt, each
  found by the fuzzers. Objects Qt owns or still holds cannot be destroyed out from under
  it: their holders let go first.
- Handlers that run inside Qt (overrides, direct signals) cannot run the event loop, and
  what they destroy is deleted once Qt's call returns.

### How fast it is
- A call through a direct entry point costs 15–60 ns on a 2.1 GHz Xeon (C++: 1–26 ns),
  with every check made; a small value made and read as a struct, 15 ns. Each entry point
  remembers the class it was last called on.
- Making and destroying a `QObject` costs about 135–155 ns: objects are made as a subclass
  whose destructor releases the handle, deleted at once, and skip the event and ownership
  cleanups they have nothing for.
- A widgets program starts in about 24 ms and peaks at 39 MiB (C++: 17 ms, 29 MiB).
- goose-qt's comparison (`goose-qt/docs/comparison.md`) runs the same benchmark in C++,
  qtc, goose-qt, PyQt6, PySide6, Go's miqt and Rust's cxx-qt.

### How it is tested
- 19 test suites, among them `test_core` (1,202 checks) and `test_concurrent` (1,696), in
  release and AddressSanitizer + UndefinedBehaviorSanitizer builds; sanitizer builds also
  check the registry's shortcuts at run time (`QTC_CROSSCHECK`).
- A method fuzzer that calls every bound method with generated arguments in fixed, named
  and random orders and through object lifetimes, also under the sanitizers. What it and
  the tests found, and what they do not cover, is in [TESTING.md](docs/TESTING.md) and
  [LIMITS.md](docs/LIMITS.md).
- Tested on Linux x86-64 with Qt 6.12.0. macOS and Windows builds are prepared for but
  not tested.
