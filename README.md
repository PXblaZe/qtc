# qtc — Qt 6 for C

qtc is a C library that gives C programs (and any language that can call C) the
Qt 6 API: widgets, Qt Quick and QML, painting, item models, networking, SQL,
multimedia, charts and more. It is generated from Qt's own headers, so it covers
what Qt covers — about 12,400 methods and 1,000 constructors of 618 classes
across 25 modules. It is built and tested on Qt 6.12 LTS.

```c
#include "qtc.h"

int main(int argc, char **argv) {
    qtc_init_argv(QTC_APP_WIDGETS, argc, (const char *const *)argv);
    qtc_arg_str(QTC_S("Click me"));
    qtc_obj button = qtc_new(QTC_S("QPushButton"));
    qtc_connect(button, QTC_S("clicked"), 1, 0);
    qtc_call(button, QTC_S("show"));

    qtc_event ev;
    while (qtc_wait(-1) == QTC_WAIT_EVENTS)          /* until the window closes */
        while (qtc_poll(&ev))
            if (ev.tag == 1) qtc_set_str(button, QTC_S("text"), QTC_S("Clicked"));
    qtc_shutdown();
    return 0;
}
```

## Why another binding

There is no official C API for Qt. The existing ones each fall short in a way that
matters for production use: some crash on ordinary misuse (a stale pointer, an
index out of range), and others trail Qt releases by years. qtc takes a different
approach:

- **Handles, not pointers.** Objects and values are 64-bit handles with a
  generation count. A handle to something that was destroyed is reported as
  `QTC_ERR_STALE_HANDLE`; it never reaches Qt.
- **Every call is checked.** Arguments are converted with range checks, enum
  values are validated against the Qt version in use, and a table of
  preconditions turns what Qt only asserts in debug builds (indexes, sizes,
  ownership cycles, objects that must not be null, states a method needs) into
  errors. Qt's internal parts and objects it owns cannot be destroyed from C.
- **No callbacks inside Qt unless you ask.** Signals and watched events are
  queued and read with `qtc_wait` / `qtc_poll` at the top of the program's own
  loop, so C code never runs in the middle of a Qt call. Subclassing and direct
  signals (`QTC_CONN_DIRECT`) are the exceptions a program opts into: a handler
  runs inside Qt's call, with the arguments and results checked, deletes
  deferred and the event loop off limits inside it.
- **Generated from Qt's headers.** The bindings come from scanning Qt's headers
  with libclang. The generator also knows the Qt 6 releases before 6.12 and
  compiles each method in only where the Qt in use has it, but only Qt 6.12 is
  tested (see [Qt versions](#qt-versions)).

Beyond calling Qt's classes, qtc has:

- **Subclassing** (`qtc_new_overridable`, `qtc_override`): item models, widgets
  that paint and handle events, validators, devices, threads and more, written
  in C.
- **Classes defined at run time** (`qtc_class_*`): signals, slots, invokables and
  properties that Qt and QML see like a C++ class's.
- **Futures and worker threads** (`qtc_future_*`, `qtc_promise_*`, `qtc_task_*`,
  `qtc_move_to_thread`): work on Qt's thread pool or a QThread, with progress,
  cancellation and results.
- **Graphics** (`qtc_rhi_*`): Qt's rendering hardware interface over Vulkan,
  OpenGL, Direct3D and Metal, with shaders compiled at run time, offscreen
  rendering, windows, `QRhiWidget` and compute; every command checked before the
  GPU sees it. Native handles for code that calls those APIs itself.

What this does and does not promise is spelled out in [docs/LIMITS.md](docs/LIMITS.md),
and how it is tested in [docs/TESTING.md](docs/TESTING.md).

## Building

You need CMake 3.21+, a C++20 compiler and Qt 6.12 (LTS). Python is only
needed to run the fuzzers or to regenerate the bindings; the generated sources
are part of the tree.

qtc is developed and tested on Linux (x86-64, GCC 13). The library uses no
Linux-specific APIs and has the export macros and build settings for macOS and
Windows (MSVC), but builds on those systems have not been tested yet.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/qt-6.12
cmake --build build
ctest --test-dir build -LE slow          # the test suite (the method fuzzer is labelled slow)
sudo cmake --install build               # libqtc, its module libraries, qtc.h, a CMake package and qtc.pc
```

qtc binds every Qt module it finds and leaves out the ones that are not
installed. To bind fewer, pass `-DQTC_MODULES="Core;Gui;Widgets"`. `libqtc` holds
Qt Core, Gui and Widgets; every other module is bound by its own `libqtc_<Module>`
library next to it, loaded the first time a program uses one of its classes, so a
program pays (in startup time and memory) only for the Qt it uses.

### Getting Qt 6.12

The Qt that qtc was tested with came from conda-forge, which needs no root and
no Qt account:

```sh
micromamba create -y -p ~/qt612 -c conda-forge qt6-main=6.12.0 qt6-charts=6.12.0 \
    qt6-multimedia=6.12.0 qt6-serialport=6.12.0 qt6-positioning=6.12.0
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=$HOME/qt612
```

The Qt Online Installer and `aqtinstall` give the same release; point
`CMAKE_PREFIX_PATH` at the directory that holds `lib/cmake/Qt6`.

Distribution packages are often older than 6.12. **Arch Linux** has Qt 6.12
(6.12.0 in November 2026):

```sh
sudo pacman -S --needed cmake ninja gcc qt6-base qt6-declarative qt6-svg \
    qt6-multimedia qt6-serialport qt6-positioning qt6-websockets \
    qt6-webchannel qt6-tools qt6-shadertools
cmake -S . -B build -G Ninja && cmake --build build && sudo cmake --install build
```

`qt6-base` alone is enough for Core, Gui, Widgets, Network, Sql, PrintSupport,
Xml, DBus and OpenGL; the other packages add the modules of the same names
(`qt6-tools` provides UiTools). Add `qt6-shadertools` to compile shaders at run
time (`qtc_rhi_shader_bake`; conda-forge's `qt6-main` includes it). Charts and
the network add-ons are GPL-3.0-only and left out of a default build; see
[docs/LICENSING.md](docs/LICENSING.md). **Debian and Ubuntu** releases ship older Qt 6
packages (Ubuntu 24.04 has 6.4.2), so use one of the routes above there.

### Qt versions

Qt 6.12 LTS is the supported release: everything in [docs/TESTING.md](docs/TESTING.md)
ran against Qt 6.12.0. CMake refuses an older Qt unless you pass
`-DQTC_ALLOW_OLDER_QT=ON`, which accepts Qt 6.4 to 6.11. Such a build leaves out
the methods its Qt lacks, but it is not tested, and crashes particular to those
releases may not be guarded. Newer Qt 6 releases are accepted but have not been
tested either.

### Options

| option | default | |
|---|---|---|
| `QTC_MODULES` | every installed one | Qt modules to bind |
| `QTC_BUILD_TESTS` | `ON` | the test suite and fuzzers |
| `QTC_BUILD_TESTLIB` | `ON` | `libqtc_test`, simulated input for tests (needs Qt Test) |
| `QTC_BUILD_EXAMPLES` | `ON` | the programs in `examples/` |
| `QTC_SANITIZE` | | e.g. `address,undefined` for the library and tests |
| `QTC_CROSSCHECK` | `ON` with `QTC_SANITIZE` | check at run time that the registry's fast paths skip only work there is none of (slower) |
| `QTC_ALLOW_OLDER_QT` | `OFF` | accept Qt 6.4 to 6.11 (untested) |
| `QTC_SPLIT_MODULES` | `ON` (`OFF` on Windows) | a library per Qt module beyond Core, Gui and Widgets, loaded on first use |

### Using it

```sh
cc app.c $(pkg-config --cflags --libs qtc) -o app
```

or, from CMake, `find_package(qtc)` and `target_link_libraries(app PRIVATE qtc::qtc)`.

## A short tour

Everything is in [include/qtc.h](include/qtc.h), which documents every function;
[docs/API.md](docs/API.md) is a guide to it. The examples in `examples/` each
run interactively, or test themselves with `--selftest`:

| example | shows |
|---|---|
| `hello.c` | a window, a layout, a signal |
| `form.c` | properties, enums by name, several signals, coalesced events, reading a model |
| `canvas.c` | custom drawing on a widget and into an image, mouse events |
| `qml.c` | a Qt Quick window and a two-way bridge between QML and C |
| `threads.c` | worker threads reporting to the GUI with `qtc_post` |
| `rhi.c` | a spinning triangle drawn with Qt's RHI on Vulkan, Metal, Direct3D, OpenGL or the Null backend, in a window or offscreen |

The pieces:

- **Objects** — `qtc_new("QLabel")` with constructor arguments pushed first;
  `qtc_destroy`, `qtc_alive`, `qtc_find_child`. Objects without a parent belong to
  qtc and are destroyed by `qtc_shutdown`.
- **Calls** — push arguments with `qtc_arg_*`, then `qtc_call(obj, "name")` or
  `qtc_call(obj, "name(int,QString)")`; read the result with `qtc_ret_*`.
  Overloads are resolved from the argument kinds. `qtc_call_static` for static
  methods.
- **Direct calls** — `qtc_direct.h` has one C function per bound signature:
  `qtcm_setText__QString(label, QTC_S("hi"))`, `qtcn_QColor__int_int_int(255, 0, 0)`.
  Same checks as `qtc_call`, about half its cost: 20–60 ns for a plain call.
- **Properties** — `qtc_set_int(obj, "value", 5)`, `qtc_get_str(obj, "text")`.
  A property with a bound setter is written through it, with its checks.
- **Values** — `qtc_val` handles hold any Qt value (colours, fonts, images, dates,
  lists, maps); `qtc_val_new("QColor")`, `qtc_val_from_json`, `qtc_val_to_json`,
  and their methods through `qtc_call` too.
- **Signals and events** — `qtc_connect` queues a signal's arguments as an event;
  `qtc_watch` delivers mouse, key, resize, close and other events of a widget;
  `qtc_timer`; `qtc_post` from any thread. Read them with `qtc_wait` and `qtc_poll`,
  or hand a callback to `qtc_run`. `qtc_connect_to` links a signal to another
  object's method without a round trip through C.
- **Enums** — `qtc_enum_value("Qt::AlignCenter", &v)`, `qtc_enum_key`.
- **Drawing** — `qtc_draw_*` records onto a `QtcCanvas` widget or paints straight
  into a `QImage` or `QPixmap`.
- **QML** — QML objects are objects like any other; a `QtcBridge` carries
  structured messages both ways.
- **Introspection** — `qtc_describe` returns JSON for any class or object;
  `manifest/qtc_api.json` lists everything bound, for generating bindings in other
  languages (that is how [goose-qt](../goose-qt) is made).
- **Errors** — every call returns or records a status (`qtc_last_error`,
  `qtc_last_error_message`). `qtc_set_error_policy(QTC_ERRORS_LOG)` prints each one.

## Layout of the tree

```
include/qtc.h          the API
include/qtc_direct.h   generated: a C function per bound signature
include/qtc_test.h     simulated input for tests (libqtc_test)
src/                   the runtime: handles, calls, events, ownership rules, drawing
src/gen/               generated: thunks, class and enum tables (do not edit)
src/mod/               what the runtime knows of each module library's classes
gen/                   the scanner, generator, precondition table and Qt models
manifest/              qtc_api.json (the bound API) and coverage.json
tests/                 test suite, fuzzers, benchmark
examples/              the examples above
docs/                  API guide, design, testing, limits, coverage
```

## Regenerating the bindings

The models of the Qt headers the bindings come from (6.12.0, and the earlier 6.x
releases back to 6.4.2) are in `gen/models`. After changing the generator or the
precondition table:

```sh
python3 gen/generate.py --models gen/models --out .
```

To add a Qt release, scan its headers first (needs the Python libclang package):

```sh
python3 gen/scan_qt.py --qt-include /path/to/qt/include --out gen/models/qt-6.13.0.json
xz -9 gen/models/qt-6.13.0.json
```

[docs/DESIGN.md](docs/DESIGN.md) explains how the pieces fit together.

## License

MIT, see [LICENSE](LICENSE). Qt itself is available under the LGPL 3, the GPL
or a commercial license; programs that use qtc use Qt, and its terms apply to
them. A default build binds only Qt modules available under the LGPL-3.0, which
closed-source programs may use; the GPL-3.0-only modules (Charts, Network
Authorization, HTTP Server, MQTT, CoAP) need `-DQTC_GPL_MODULES=ON`.
[docs/LICENSING.md](docs/LICENSING.md) has the details, the obligations in
outline and non-GPL alternatives.
