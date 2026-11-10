# Changelog

## 1.1.0 — unreleased

### Licensing
- A default build now binds only Qt modules available under the LGPL-3.0, which
  closed-source programs may use. The GPL-3.0-only modules (Charts, Network
  Authorization, HTTP Server, MQTT, CoAP) need `-DQTC_GPL_MODULES=ON`; naming one
  in `QTC_MODULES` without it stops the configure step. 1.0.0 bound Qt Charts
  whenever it was installed. `qtc_stats()` lists the GPL-3.0-only modules a build
  links under `"gpl_only_modules"`. See docs/LICENSING.md.

### Fixed
- Crashes in the GPL-3.0-only add-ons that the method fuzzer found: the accessors
  of a default-constructed `QHttpServerRequest` (no data behind it),
  `QCoapMessage::optionAt` past the options, `QAbstractOAuth2::post`/`put` with a
  null multipart and `QAbstractOAuthReplyHandler::networkReplyFinished` with a
  null reply are refused now.
- Calls to meta-object methods with `QVariant` parameters (every untyped QML
  function parameter) passed a value stored outside its variant (types over 24
  bytes, such as `QRectF` or `QTransform`) as the address of the value, which the
  method read as a variant: a crash. They now pass the variant.

### Added
- Network add-ons (opt-in, GPL-3.0-only): Qt Network Authorization, Qt HTTP
  Server, Qt MQTT, Qt CoAP.
- Futures, promises, thread-pool tasks, parallel map, worker objects on QThreads
  and queued invocations (`qtc_promise_*`, `qtc_task_*`, `qtc_future_*`,
  `qtc_move_to_thread`, `qtc_invoke_queued`, `qtc_invoke_future`); QThreadPool is
  bound.
- Subclassing Qt classes from C: overridable objects of 25 classes (widgets,
  windows, QOpenGLWidget, QRhiWidget, item models and proxies, QValidator,
  QSyntaxHighlighter, QIODevice, QThread, QTcpServer, animations, graphics scenes
  and views, delegates, completers) whose virtual methods run a handler
  (`qtc_new_overridable`, `qtc_override`, `qtc_set_override_handler`, the
  `qtc_ov_*` call API, `qtc_draw_begin_painter`), with Qt's model begin/end
  rules checked.
- Runtime-defined QObject classes with signals, slots, invokables and
  properties that Qt, QML and `qtc_call` see like C++ ones (`qtc_class_*`,
  `qtc_emit`). Uses Qt's private QMetaObjectBuilder (Qt6::CorePrivate).
- Graphics (`qtc_rhi_*`): Qt's RHI over Vulkan, OpenGL (ES), Direct3D 11/12 and
  Metal, with offscreen frames, swapchains on windows, QRhiWidget rendering,
  buffers, textures, samplers, bindings, graphics and compute pipelines,
  uploads, readbacks, draws and dispatches, every command checked before the
  GPU sees it; shaders compiled at run time from GLSL with Qt Shader Tools
  (LGPL-3.0), or loaded as `.qsb` packages; native handles and native command
  recording for code that calls Vulkan, OpenGL, Direct3D or Metal itself;
  `qtc_gl_proc_address`; Vulkan instances and surfaces (`qtc_vk_*`); Qt Quick's
  graphics API (`qtc_quick_set_graphics_api`). Uses Qt's semi-public RHI API
  (Qt6::GuiPrivate). Tested on the Null backend, Mesa's llvmpipe (OpenGL) and
  lavapipe (Vulkan); the Direct3D and Metal paths are not compiled or tested
  here.
- goose-qt: overridable objects (through an exported handler), runtime classes,
  futures and promises for Goose worker threads, worker objects, and the
  graphics API.

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
