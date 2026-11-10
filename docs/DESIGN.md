# How qtc works

qtc has two halves: a generator that turns Qt's headers into C++ thunks and
tables, and a runtime that resolves C calls against those tables and keeps
everything that crosses the C boundary checked.

## Generation

```
Qt headers ──scan_qt.py──▶ gen/models/qt-6.x.json ──generate.py──▶ src/gen/*.cpp
 (6.4 … 6.12, libclang)     (one per release)          │            manifest/qtc_api.json
                                                        │            manifest/coverage.json
                              gen/preconditions.py ─────┘
```

`scan_qt.py` parses the umbrella headers of 25 Qt modules with libclang and
records every public class: its bases, constructors, methods (with canonical
types), enums, and traits such as being abstract or copyable. It runs once per Qt
release; the models of 6.4.2, 6.5.3, 6.6.3, 6.7.3, 6.8.4, 6.9.3, 6.10.2, 6.11.3
and 6.12.0 ship in `gen/models`.

`generate.py` takes the newest model as the API and the older ones as history:
each class, method, constructor and enum value gets the first release that has it,
and its code is wrapped in a `QT_VERSION` guard. Qt 6.12 is the supported and
tested release; the guards are what lets an explicit opt-in build
(`-DQTC_ALLOW_OLDER_QT=ON`) compile against Qt 6.4 to 6.11, untested. For each method it decides whether the method
can be bound safely; those that cannot are counted with the reason in
`manifest/coverage.json` (see [COVERAGE.md](COVERAGE.md)). The main reasons are
types that cannot cross a C boundary without an owner (raw pointers to non-QObject
types, non-const references, rvalue references, iterators), methods that take a
slot by name as a string (they would let Qt call any method around every check),
and a deny list of methods that are unsafe whatever their arguments.

Each bound method becomes a thunk: a small function that converts the C
arguments to the C++ parameter types, runs the method's preconditions, calls the
method and converts the result. Thunks are templates instantiated per declaring
class and guarded with `if constexpr (requires …)`, so a signature that changed
between releases fails to compile into an `UNSUPPORTED` error instead of a build
break.

### Preconditions

`gen/preconditions.py` is a table of rules: a class pattern, a method pattern,
an optional parameter-type pattern, a C++ expression and the error to report.

```python
("QTextTable", r"removeColumns", None, "{0} >= 0 && {1} >= 0 && int64_t({0}) + {1} <= {s}->columns()",
 OOR, "the columns must exist"),
```

`{s}` is the receiver and `{0}`, `{1}` the converted arguments. A class written with a
trailing `+` (`QLayout+`) also covers its subclasses, so a rule reaches the overrides they
declare (a call on a `QHBoxLayout` runs `QBoxLayout::setGeometry`); the non-null rules
reach overrides the same way. The rules encode
what Qt documents and only asserts in debug builds, and everything the fuzzers
found: about 480 rules, some of them families (every model's
`insertRows`/`removeRows`, every container's indexes, every allocation size,
every method that would put an object inside itself), which expand to 1,761
checks in the generated thunks. A rule can depend on the
Qt version (`QT_VERSION >= QT_VERSION_CHECK(6, 12, 0) || …`) for crashes that only
older releases have.

Enum arguments are checked against tables generated per Qt release: a plain
enum accepts its declared values, a flag enum its declared bits, and a short list
of open enums (key codes, event types, item data roles, font weights …) any value
of the underlying type.

## Runtime

### Handles

The registry (`qtc_core.cpp`) maps 64-bit handles to objects and values. A
handle is a slot index and a generation; destroying an object or freeing a value
bumps the generation, so an old handle is recognised as stale and a made-up one as
bad. Each object slot holds a `QPointer` too, which catches objects deleted while
their signals were blocked (they never emit `destroyed()`), and the registry
sweeps those.

Objects created with `qtc_new` and left without a parent are owned by qtc and
destroyed by `qtc_shutdown`, newest first. Values are `QVariant`s; a value never
holds a raw `QObject *` — object references inside lists, maps and signal
arguments are stored as handles, so a value can outlive the object it mentions.

### Calls

A call takes the pushed arguments into a frame of its own (so a call that runs
inside another — a forwarding connection — cannot disturb the outer one, nor the
string views Qt holds into its arguments), then collects candidates:

1. the bound methods of that name in the receiver's class and its bases (or the
   one with that signature, trailing defaulted parameters may be left out), and
2. methods only the meta-object has (slots, signals and invokables without a
   thunk) — but only for classes qtc does not bind: objects QML created, QML
   declarations, the program's own C++ classes, qtc's built-in classes. For a
   class qtc binds, its bound methods are the API, so private signals
   (`rowsInserted`) and denied slots (`QThread::terminate`, `deleteLater`) stay
   out of reach.

Candidates are scored against the argument kinds; an exact winner is called, a
tie is reported as `QTC_ERR_AMBIGUOUS` with both signatures. Candidate lists are
cached per class, name and signature. Pointer out-parameters (`bool *ok`) are
left out of the call and read afterwards with `qtc_out_*`.

Property writes go through the property's bound setter when there is one, so a
property gets the same checks as the method.

### Ownership rules

The hardest crashes in Qt come from objects that hold another one by a plain
pointer: a scroll area's viewport, a combo box's view, a model mapper's delegate,
a drag's mime data, a QML engine a component was made for. Delete the held object
first and the holder crashes later. `qtc_roles.cpp` knows about 35 such owners.
Before `qtc_destroy` deletes something, every owner that holds it, or anything
inside it, lets go (`setViewport(nullptr)`, a fresh delegate …); what cannot be let
go of is refused with the owner's name. Objects Qt owns outright are refused too:
the application object, screens, the clipboard, a window's internal parts, a
document's layout, the parts a constructor made (a combo box's line edit, a tab
widget's tab bar) unless the program put them there itself.

Finding the owners of an object is an index, not a search: each owner's held
objects are recorded, and an owner is looked at again only after a call that may
have changed what it holds. Destroying an object costs about a microsecond with
thousands of owners alive.

Methods that place an object somewhere (`addWidget`, `setViewport`, `setModel` …)
check that it is not held somewhere else already, and that it is not the receiver
or one of its ancestors.

Qt deletes objects itself too, mostly deferred: a widget closed with
`Qt::WA_DeleteOnClose`, a closed MDI subwindow, a message box's old check box. An
application event filter sees each `DeferredDelete` before Qt acts on it and makes
the owners let go first, as `qtc_destroy` does. The same filter keeps a window
larger than the allocation limit from being painted (Qt would allocate its backing
store at that size, and paint on without a paint engine when that fails), and a
layout whose sizes add up past what Qt's layout engine counts from being laid out
on a resize or layout request. It also sees each `ParentChange` before the widget
does: a `QFocusFrame` lets go of a widget Qt took out of its parent (the frame
would map from the missing parent), and a widget that went into a parent with the
native window it had as a window left on it (when it had new window flags and was
not shown in between) loses that window, as Qt does with one it shows.

Layouts without a parent widget are a case of their own: Qt takes a widget out of
a layout only through the layout's parent widget, so such a layout would keep a
widget that moved elsewhere, and delete it there (`QFormLayout::removeRow`). qtc
watches the widgets it puts into such layouts (an event filter for
`ParentChange`) and takes them out when they move, and the owners that keep a
widget by a plain pointer refuse one still in such a layout.

Some limits are about cost rather than validity. A call that allocates more than
the allocation limit at once is refused, images and windows included (Qt
allocates a large image and the system kills the process when it is touched);
insertions that Qt makes in quadratic time (`QStringListModel` rows,
`QStandardItemModel` columns) are bounded, also when they arrive through proxy
models, which `insert_cheap` follows to the model that does the work.

### Events

Signals never call C code from inside Qt. A connection is a slot index on a
private `Spy` object; when the signal fires, its arguments are converted and
queued with the connection's tag. `qtc_wait` runs Qt's event loop until something
is queued (or a timeout, or a quit), and `qtc_poll` hands queued events over one
at a time. The arguments of the last 64 events stay readable with
`qtc_event_arg_*`; older ones report `STALE_HANDLE`.

- `QTC_CONN_COALESCE` updates a queued event instead of queueing another (its
  `count` says how many were merged), for chatty signals.
- `qtc_watch` installs an event filter and queues mouse, key, wheel, focus,
  resize, close, drag-and-drop and paint events; those in the consume mask are
  kept from the widget.
- `qtc_post` may be called from any thread; it is the only function that may.
  Everything else checks the calling thread and reports `WRONG_THREAD`.
- Events from an object destroyed after they were queued carry
  `QTC_EVF_STALE_SOURCE`.
- The queue holds at most a million events by default (`qtc_set_queue_limit`);
  what is dropped is counted (`qtc_dropped_events`).

`qtc_connect_to` forwards a signal to another object's method without going
through C: the forward is a checked call made inside the emission, so the
receiver's preconditions apply; forwards that set each other off stop 32 levels
deep.

### Drawing and QML

`qtc_draw_*` records painter commands. Recording onto a `QtcCanvas` (a widget qtc
adds) replaces what it paints; recording onto a `QImage` or `QPixmap` value paints
into it at `qtc_draw_end`. No paint callback runs in C.

QML objects are ordinary objects: properties, functions and signals declared in
QML are reached with the same calls. `QtcBridge` sends structured values both
ways: QML calls `bridge.send(name, payload)` and C receives a `sent` signal event;
C calls `post(name, payload)` and QML receives `message`.

### Overridable objects

`gen/overrides.py` generates a subclass `QtcOv_<Class>` for each overridable
class: every virtual method qtc exposes gets a trampoline that, when the object
has an override for it (a per-object table of tags, checked with one load),
packs its arguments into a frame and calls the program's handler; otherwise, or
when the handler answers `QTC_OV_BASE`, it calls the class's own implementation.
Protected methods a handler needs (`beginInsertRows`, `createIndex`) are reached
through static members of the subclass. The frame keeps the program's pending
arguments, return register and open drawing aside while the handler runs, so a
handler may call qtc freely; what it may not do is run the event loop or shut
qtc down, and calls nest at most 64 deep. Handlers run on the GUI thread, except
for methods Qt calls elsewhere (`QThread::run`), which are marked as such.
Results are checked before Qt sees them: negative counts, model indexes another
model made, begin/end calls of models out of order.

### Runtime classes

`qtc_class_*` builds a meta-object with Qt's `QMetaObjectBuilder` and makes its
objects as `QtcOv_` objects of the base class whose `metaObject`, `qt_metacast`
and `qt_metacall` answer for the runtime class: signals are activated through
the meta-object, slots and invokables call the override handler with their tag
(on the object's thread), and properties keep their values in the object and
emit their notify signals on change.

### Concurrency

A future (`qtc_concurrent.cpp`) is a job with a result list and a mirrored
`QPromise<QVariant>`, so Qt's watchers and `QtFuture::whenAll/whenAny` work on it.
Every path that runs work (a task, a map chunk, a queued call) finishes its
future when it ends, also when it throws, is dropped by the pool or never runs,
so no future can hang. Worker objects move to a `QThread` only when nothing on
the GUI thread can reach them; calls to them are queued, and their signals
reach the program as events.

### Graphics

`qtc_rhi.cpp` keeps its own handle table (handles tagged so that they never
match an object handle). Each resource records what it uses and is used by:
bindings hold their buffers, textures and samplers, a render target its
texture, a pipeline the bindings it was made with, so nothing a QRhi object
points at is deleted first; destroying an RHI destroys its resources in
dependency order. Pipelines keep their own render-pass descriptor, so they
outlive the target they were made for and draw into any compatible one.
Validation happens before every command reaches Qt (which checks little in
release builds and nothing about memory): pipeline creation compares vertex
attributes, the vertex-to-fragment interface and bindings with the shaders'
reflection data; draws compute the bytes every vertex binding will read from
the counts and offsets; indexed draws find the largest index in a CPU copy of
the index buffer as the GPU will see it at that point of the frame (uploads are
queued, and reach the GPU when the next pass begins). Readback results live in
the RHI's context until the GPU has written them, so a readback handle can be
destroyed at any time; likewise a buffer or texture destroyed while uploads or
readbacks queued for it wait for a frame stays alive until they are recorded,
and anything destroyed during a frame goes once the frame is submitted. A swapchain listens for its window's surface going away
and lets go of it first. A QRhiWidget's RHI is wrapped once per QRhi, and the
frame Qt is recording is lent to the program for the length of the override call
(`qtc_rhi_widget`); the wrapper and what was made with it go when Qt destroys
that QRhi.

### Errors

Every function records its outcome in a thread-local status and message
(`qtc_last_error`, `qtc_last_error_message`); functions that return a status
return it directly. With `QTC_ERRORS_LOG` each failure is printed, with
`QTC_ERRORS_ABORT` the program stops at the first one (for development). C++
exceptions never cross into C: they are caught and reported as
`QTC_ERR_EXCEPTION`.

### Shutdown

`qtc_shutdown` first ends tasks and futures and stops worker threads, then
destroys RHIs and their resources (while the windows and widgets they draw into
exist), then destroys what qtc owns newest first, in up to four rounds: an
object something still uses (an engine a context was made for) waits for a later
round, and a thread that still runs is asked to quit first. Then the application
object goes, and `qtc_init` may be called again.
