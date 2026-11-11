# How qtc is tested

Everything here runs on Qt's offscreen platform, so it needs no display. The
numbers are from the runs made for the 1.1.0 release on Linux x86-64 (GCC 13),
against Qt 6.12.0 (6.12 LTS is the supported release), natively and under
AddressSanitizer with UndefinedBehaviorSanitizer, with the GPL-3.0-only add-on
modules built in (so they are tested too); the [results](#results) section has
them.

## The suite (`ctest -LE slow`)

| test | what it checks |
|---|---|
| `test_core` | the C API function by function: init and shutdown, handles and staleness, calls and overload resolution, out-parameters, properties, values and JSON, lists and maps, signals, coalescing, watches, timers, posts from threads, forwarding connections, enums, drawing, QML and the bridge, ownership rules, allocation limits, error policies (about 890 checks) |
| `test_app` | a small application driven by simulated input (`libqtc_test`): a form, tabs, a canvas, keyboard and mouse |
| `test_sweep` | every bound method and constructor called wrongly: a stale, null, garbage and wrong-kind receiver, one argument too many, each argument of a wrong type, a stale object as an argument; every call must fail with the right error |
| `test_stress` | handle churn and generation reuse, event floods, deep object trees, many connections, floods from other threads, big payloads — each with a time budget, so a performance cliff fails like a wrong result |
| `test_concurrent` | futures, promises, tasks, parallel map, worker objects and queued calls from many threads: results, progress, cancel, suspend, failure, watchers, release in every state, shutdown with work in flight (about 1,700 checks) |
| `test_override` | overridable objects: list and table models with Qt's begin/end rules, painting and input events, validators, highlighters, devices, `QThread::run` on its thread, scenes, recursion and the depth limit, results Qt cannot use, handlers that destroy their object, a handler found by symbol |
| `test_class` | runtime classes: signals, slots, invokables and properties seen by `qtc_call`, by Qt's meta-object system and by QML, notify signals, classes based on overridable classes, slots on worker threads |
| `test_rhi` | the graphics layer on every backend the machine has: every refusal (handles, kinds, states, sizes, bindings against shaders, vertex and index ranges, destruction order), rendering read back pixel by pixel (offscreen frames, swapchains on windows, `QRhiWidget`), texture sampling, instancing, compute into buffers and images, native interop, shutdown with frames open |
| `rhi_xcb` | `test_rhi` on an X server (Xvfb), where Mesa's llvmpipe (OpenGL) and lavapipe (Vulkan) render |
| `fuzz_rhi`, `fuzz_rhi_xcb` | random sequences of graphics calls with handles of every kind and edge-value arguments, built around valid scenes so that hundreds of draws and dispatches reach the GPU per run; no handle may be left behind |
| `example_*` | the six examples, each driving itself with `--selftest` and checking the result |

## The fuzzers (`tests/fuzz_methods.cpp`, `tests/fuzz_driver.py`)

The method fuzzer calls every bound method and constructor of every class
(13,617 items with the add-on modules) on a live receiver, with every arity it accepts and edge values
for each parameter kind (321 of the items are on classes it cannot make on its
own, such as network replies, legend markers and abstract classes without a
bound subclass; those are left uncalled): 0, ±1, `INT_MAX`, NaN, empty and 4,096-character
strings, format strings, null, the receiver itself, live instances of the
parameter's class, empty and mixed lists. Receivers and arguments are shared
between items, so state built by one method is used by the next.

- **Fixed order** (no seed): items in declaration order with six argument
  patterns each.
- **Random order** (`--seed N`): the items shuffled, and each method also gets six
  calls with random arities and a random mix of the fixed patterns and wider
  values (`INT_MIN`, 2^20, ±infinity, undeclared enum values, invalid UTF-8,
  control characters, nested JSON). A different seed is a different program;
  most of the crashes fixed for 1.0.0 were found this way.
- **By name** (`--by-name`): every method called by name instead of signature,
  as a program would; it checks that overload resolution picks a method, and
  reports the calls it cannot resolve.
- **Lifetime** (`--lifetime`): objects created, destroyed and used again in
  every order: a method's receiver, its arguments and its results destroyed
  before, during and after calls, parents before children and the reverse.

`fuzz_driver.py` runs the fuzzer in a sandbox (its own home and working
directory, so methods that write files write there), restarts it after each
failure, detects hangs, and writes a JSON report for every failure with the last
calls and the sanitizer's report (without sanitizers, the fuzzer prints the stack
and fault address of a crash itself, and marks when Qt was running queued events
and timers rather than a call). `fuzz_minimize.py` shrinks a failure to
the items that cause it, which is how failures that depend on earlier items were
tracked down.

Methods that would act on the machine (delete files, start processes, open
devices and network connections, change application-wide state) are bound and
tested by `test_core`, but not fuzzed: `NOFUZZ_RE` and `NOFUZZ_CLASS_RE` in
`gen/generate.py` list them.

That line was not tight enough until the last rounds. One of the fuzzer's string
values was `"/"`; a `QNetworkDiskCache` given it as its directory, then a maximum
size of 0, expired its cache, which deletes every file ending in `.d` under the
directory. On the build machine, running as root, that deleted three system files
(restored since). Now the fuzzer uses no absolute path, so whatever a method does
with a path stays in the sandbox; the disk cache's directory methods are not
fuzzed; `fuzz_driver.py` and `fuzz_minimize.py` run the fuzzer as `nobody` when
started as root; and qtc refuses the file system root and the home directory as a
cache directory. Run the fuzzers as an unprivileged user all the same.

## Running them

```sh
ctest --test-dir build -LE slow --output-on-failure
python3 tests/fuzz_driver.py --binary build/tests/fuzz_methods --work /tmp/fw --report report.json
python3 tests/fuzz_driver.py --binary build/tests/fuzz_methods --work /tmp/fw --report report.json --seed 7
python3 tests/fuzz_driver.py --binary build/tests/fuzz_methods --work /tmp/fw --report report.json --lifetime
python3 tests/fuzz_driver.py --binary build/tests/fuzz_methods --work /tmp/fw --report report.json --by-name
```

For the sanitizers, configure with `-DQTC_SANITIZE=address,undefined`.

## Results

The last runs, on Qt 6.12.0 (Linux x86-64, GCC 13, two cores). The test suites ran on the release as shipped; the final fuzz rounds ran on it before its last fix (the `QMenu::actionAt` one below).

| suite | release build | ASan + UBSan build |
|---|---|---|
| `test_core` | 861 checks pass | 861 checks pass |
| `ctest -LE slow` (core, app, sweep, stress, five examples) | 9 of 9 pass | 9 of 9 pass |
| goose-qt tests (`test/run.sh`, JIT and native) | 13 of 13 pass (examples: 9 of 9) | not run |

The method fuzzer, release build:

| run | items | calls | failures |
|---|---:|---:|---:|
| fixed order | 13,002 | 85,548 | 0 |
| by name | 13,002 | 14,254 | 0 |
| lifetime | 1,298 | 99,133 | 0 |
| random orders, seeds 1–64 | 13,002 each | 10,355,880 | 1 (63 of 64 orders clean) |

Seeds 1–48 are orders earlier rounds ran (the failures they found are fixed since); 49–64 had
never run. The one failure, in order 50, was `QMenu::actionAt` handing out an
action `clear()` had just deleted (a hidden menu's cached rectangles outlive its actions). It is
fixed in this release, with a test, and order 50 runs clean on it.

Under AddressSanitizer and UndefinedBehaviorSanitizer:

| run | items | calls | failures |
|---|---:|---:|---:|
| fixed order | 13,002 | 85,548 | 0 |
| by name | 13,002 | 14,254 | 0 |
| lifetime | 1,298 | 99,133 | 0 |
| random orders, seeds 1–12 | 13,002 each | 1,941,894 | 1 (11 of 12 orders clean) |

The one failure is the time limit, not a fault: order 12 splits a large string with a regular
expression eight times, half a second each in the release build and ten times that under the
sanitizers, past the 60-second limit. With a longer limit the order runs clean.

`test_stress`, release build (the budget is what the test allows):

| case | time | budget |
|---|---:|---:|
| 1,000,000 value handles created and freed | 58 ms | 4,000 ms |
| 200,000 live values at once, then freed | 54 ms | 3,000 ms |
| 100,000 QObjects created and destroyed | 200 ms | 8,000 ms |
| widget tree 1,000 deep, destroyed from the root | 91 ms | 5,000 ms |
| 1,000,000 emissions through a coalesced connection | 1,023 ms | 6,000 ms |
| 200,000 queued signal events captured and polled | 139 ms | 6,000 ms |
| 20,000 connections on one object, then disconnected | 130 ms | 4,000 ms |
| 8 threads x 100,000 posts, consumed concurrently | 199 ms | 15,000 ms |
| queue flood with a 1,000 event limit: drops counted, memory bounded | 117 ms | 3,000 ms |
| 64 MB string through a property and back | 230 ms | 4,000 ms |
| list value with 1,000,000 elements | 81 ms | 5,000 ms |
| 100,000 items into a QListWidget through qtc_call | 119 ms | 8,000 ms |
| drawing 200,000 commands onto a 1024x1024 image | 3,758 ms | 6,000 ms |
| handles stay valid across 50 shutdown/init cycles | 343 ms | 30,000 ms |

What a call costs (`tests/bench.cpp`, release build; "direct" is the same call in C++):

| operation                                      |       qtc |    direct |   ratio |
|------------------------------------------------|-----------|-----------|---------|
| QLabel::setText through a method call          |       337 |         7 |   48.6x |
| QLabel::setText with a full signature          |       547 |         - |       - |
| QLabel text through the property system        |       766 |        56 |   13.7x |
| QSlider::setValue (int)                        |       343 |        30 |   11.4x |
| QSlider::value() and read the result           |       215 |         2 |   89.3x |
| QSlider value property get                     |       181 |        80 |    2.3x |
| QLabel::text() returning a string              |       263 |        25 |   10.6x |
| QComboBox::addItem, resolved among 3 overloads |       944 |       544 |    1.7x |
| signal to C: emit, capture, poll (with args)   |       274 |       282 |    1.0x |
| signal to C without argument capture           |       201 |         - |       - |
| value handle create + free                     |        57 |         - |       - |
| qtc_alive on a live handle                     |        12 |         - |       - |
| call on a stale handle (rejected)              |       276 |         - |       - |

## What the fuzzers found

Most of the precondition table is the fuzzers' work: each rule names the crash it
prevents. Some of what the random-order runs found for 1.0.0, all crashes or
hangs inside Qt reached through ordinary calls:

| call | what Qt does |
|---|---|
| `QTextCharFormat::fontFamily()` with no family set | takes `first()` of an empty list (inline, in Qt's header) |
| `QTextTable::removeColumns(1, 3)` on a two-column table | moves the position below zero, then reads before the table |
| `QTextTable::splitCell` into fewer than one row | inserts span − n rows: billions for `INT_MIN` |
| `QNetworkProxy::headers()` on a default proxy | reads through a null pointer |
| `QSortFilterProxyModel::sort(-2)` without a source model | crashes |
| `QAmbientSound::pause()` before `play()` | dereferences an empty `std::optional` (abort) |
| `QAudioEngine(44101)` | the resampler crashes on rates that are not standard ones |
| `QComboBox` with `LabelDrawingMode::UseDelegate` and no current item | crashes in every size hint and paint |
| `QDataWidgetMapper::mappedPropertyName(nullptr)` after a mapped widget died | matches the dead entry, then reads it |
| `QDockWidget::setDockLocation` outside a main window | dereferences the missing dock layout |
| a widget added twice to a `QStatusBar` or a `QToolBox` | one entry outlives the widget |
| a widget added to two MDI subwindows | the two set each other's state until the stack overflows |
| `QMdiArea`, `QProxyStyle`, `QGraphicsScene` taking a style or widget another owner holds | dangling pointers when that owner lets go |
| `QDBusUnixFileDescriptor::setFileDescriptor` with a closed descriptor | closes the descriptor it holds but keeps its number, and later closes whatever reuses it (here Qt's own D-Bus thread wakeup: a hang at exit) |
| `QString::replace(pos, -n, …)`, `QCborArray::pop_back()` on an empty array, `QVersionNumber::segmentAt(-1)` | out-of-bounds memory access |
| `QStandardItemModel(INT_MAX, 0)` | crashes; with many rows, inserting columns takes minutes (rows × rows × columns) |
| `QObjectCleanupHandler` holding an object that comes to contain it | deletes itself from inside `clear()` |
| `QMdiSubWindow::setSystemMenu` with a menu that is not its child, then `showSystemMenu()` | makes the menu a child widget, then pops it up as a window that does not exist |
| an area series in a chart whose boundary line has point label clipping off | `LineChartItem` updates a graphics item that is not there (Qt Charts) |
| `QAbstractSeries::attachAxis(nullptr)` on a series in a chart | asserts the axis only in debug builds, then reads through it |
| `QMessageBox::clickedButton()` (and the default and escape buttons) after the button was deleted | returns the deleted button |
| a widget put into a layout without a parent widget, then made a tab widget's corner | the layout deletes it (`QFormLayout::removeRow`) under the tab widget |
| a `QGridLayout` stretched to 4,096 × 65,536 cells | allocates and clears rows × columns pointers on every pass: a crash when that fails |
| `QStringListModel::insertRows(100, 1 << 20)` | inserts one row at a time in the middle: hours |
| `QStandardItemModel::setHorizontalHeaderLabels` on a million rows | adds the columns row by row: rows × rows × columns |
| `QString::count(QRegularExpression)` on a long string | counts overlapping matches: size × size steps |
| `QString::section(QRegularExpression, …)` on a long string | about a microsecond per match: minutes |
| `QLCDNumber::setDigitCount(0)`, a small decimal point, then `display("")` | clears a bit of an empty bit array |
| `QPalette::setCurrentColorGroup(QPalette::All)`, then `color()` | reads brushes past the end of the palette |
| `QWizardPage::setButtonText(QWizard::Stretch, …)` on the current page | indexes the wizard's buttons past the end |
| `QGridLayout::columnMinimumWidth(65535)` | reads past the end of a list |
| `QStandardItemModel::insertRows(0, 1 << 20)` with 4,096 columns | allocates 32 GiB: the system kills the process |
| `QWidget::grab()` of a 16,777,215 × 1,000 window | the same, for a 64 GiB pixmap |
| an area series added to a chart without an upper series, then `setAnimationOptions(AllAnimations)` | animates the missing upper line |
| `QGraphicsScene::clear()` on a `QChartView`'s scene, or a second `QChartView` made with the first one's chart | deletes the chart the first view keeps a plain pointer to |
| `QButtonGroup::setId()` for a button not in the group, then deleting the button | `button(id)` returns the deleted button |
| `QWebChannel::connectTo(nullptr)` | keeps the null transport and publishes to it later |
| a `QSplitterHandle` made by hand for a splitter | the splitter takes it in as a content widget; `replaceWidget` then takes it out, and its size hint reads the parent it no longer has |
| `QPainterPath::intersected` of two paths of 512 random points | the clipper needs minutes (about the square of the crossings) |
| `QPainterPath::subtracted` of paths reaching 1e100 | the clipper's arithmetic overflows, then it crashes |
| `QPainterPath::percentAtLength` on long curves | measures each curve in about √length pieces for every step of its search |
| `QTransform::map(QPainterPath)` through a perspective transform | up to 512 lines for each curve of a four-million-element path |
| `QGeoPath::length(-1)` | reads the coordinate before the first |
| `QTextTable::appendColumns` next to a cell merged across rows and columns | builds a cell grid it then reads past the end of |
| a custom `QMessageBox` button destroyed, then `buttonText(0)` | the box keeps its custom buttons in a list of plain pointers apart from the button box |
| a box layout with stretch factors and spacers of `INT_MAX` | the layout engine's sums wrap and its space distribution never finishes: `show()`, `grab()`, `QTabWidget::addTab` hang |
| a Qt Charts model mapper with a series and no model, then a change to the series | every mapper writes the change into the missing model |
| a window at `setGeometry(INT_MAX - 10, …)`, with a graphics effect inside | the far edge wraps, the backing store gets no image, and the effect path paints into it without checking |
| a widget a `QFocusFrame` watches, moved out of its parent (`setParent(nullptr)`, `QWidgetAction::setDefaultWidget`, or by Qt when a tool bar lets go of a widget action, also while the tool bar is destroyed) | the frame maps from the widget's new parent to its old one and walks off the top, or reads through the missing parent |
| `QRegion` from a self-crossing polygon of 16 points over 65,536 × 65,536 | about ten seconds; with more points, minutes |
| `QRegion(x, y, 1, INT_MIN, Ellipse)` | the far edge wraps and Qt takes the first polygon of an empty path |
| `QPainterPath::toFillPolygons` of a path with many subpaths | compares each subpath with every other |
| a button moved from a `QDialogButtonBox` into a wizard or message box, then `clear()` on the box | the box deletes it there; the wizard (`setWizardStyle`) or message box (`setButtonText`) writes to it |
| a focused widget moved by `setParent(nullptr, Qt::ToolTip)`, later destroyed, then its old parent reparented | the old parent still names it as its focus child |
| a window container whose window was destroyed, when one of its parents changes | `QWindowContainer::parentWasChanged` reads the window without checking (`QToolBox::removeItem` reached it) |
| a `QNmeaPositionInfoSource` or `QNmeaSatelliteInfoSource` reading a buffer open without reading (write-only or append) | the device reports bytes available while every read fails, and the reader loops forever |
| a `QTableWidget` cell widget deleted (or moved into a form layout whose `removeRow` deletes it), then the application restyled | the view keeps the widget in its index-widget table and shows it through a null pointer |
| `QWizard::setSideWidget(w)` with `w` still in a splitter, then `QApplication::setStyleSheet` | the wizard takes the widget in while Qt restyles every widget, and the splitter deletes a handle Qt is about to restyle |
| a path of thousands of copies of the same points, then `simplified()` | the clipper's k-d tree goes one level deeper per copy: seconds of work, then a stack overflow |
| `QMainWindow::tabifyDockWidget(d, d)` on a dock widget taken out and floated with no area (`setDockLocation(NoDockWidgetArea)`), then `QApplication::closeAllWindows()` | Qt wraps the dock in a floating tab group that no close reaches, and closeAllWindows retries it forever |
| a widget embedded with `QGraphicsScene::addWidget`, then made a `QColumnView`'s preview widget, then `QGraphicsScene::clear()` | the proxy deletes the widget wherever it went, and the column view keeps pointing at it |
| `QPainterPath::setElementPositionAt` with NaN or infinity, then `simplified()` or `united()` | the clipper crashes on the point (Qt keeps non-finite points out of `moveTo`, `lineTo` and the rest) |
| a window given a window state, then new flags with no parent, then put in a parent before being shown again, then a `QOpenGLWidget` put in it | Qt leaves the old native window on what is now a plain child, takes it for the native parent, remakes it for OpenGL and gets no window back |

Found for 1.1.0 (the first four in the GPL-3.0-only add-ons):

| call | what Qt does |
|---|---|
| `QOAuth1::setSignatureMethod(Rsa_Sha1)`, then a request | aborts (`qFatal`): RSA-SHA1 is not supported |
| `QMqttClient::setTransport(buffer, AbstractSocket)`, then `connectToHost()` | casts the buffer to a socket without checking and reads through null |
| `QMqttClient::setState(Connected)`, then `publish()` | writes to a transport it never made |
| the default reply handler of an OAuth object, or an MQTT client's transport, destroyed | the owner keeps a plain pointer to it (a scoped pointer, or one it never lets go of) |
| `QWidget::grab(QRect(0, 1, 2, 0))` on a widget with a graphics effect | the empty rectangle still "intersects" the widget; the effect paints into the null pixmap (the crash 1.0.0 listed as unexplained) |
| `QWidget::grab` with one negative size and a far-off top left | grabs from that corner to the widget's bottom right: a pixmap too large to allocate, painted into anyway |
| `QWidget::scroll(INT_MIN, INT_MIN)` | the backing store's rectangle arithmetic overflows and copies out of the image |
| `QGraphicsScene::setStyle` while proxy widgets use the scene's own style | deletes the style, then tells the embedded widgets, which unpolish with it |
| `QAudioFormat::setChannelCount(40000)`, then `framesForBytes(INT_MIN)` | the count is kept in a short and turns negative: `INT_MIN / -1` traps |
| `QWaveDecoder::seek` far past the end of the buffer it reads | the buffer grows to the position: gigabytes, until the system kills the process |
| an area series in a chart, then `QChart::removeAllSeries()`, then `setPointsVisible` on its upper line | the area series is deleted at once, its chart item later, and the item reads the deleted series when the line changes |
| `QBarSet::remove(1, INT_MAX)` | index + count wraps, and Qt removes past the end |
| `QGridLayout::setRowMinimumHeight(100, INT_MIN)` and others, then `setGeometry` | the layout engine's sums wrap and its space distribution never finishes (the 1.0.0 box-layout hang, through the grid's own row and column sizes) |
| `QPlainTextEdit::find` with an empty regular expression, backward, whole words, over a 4,096-character line | tries every earlier position, matching each from the start of the line: seconds to minutes |
| a text path moved by NaN, joined to itself, then `QGraphicsScene::setSelectionArea` with it | the NaN points defeat the bounding-box early outs, and each item is tested against every element: minutes |
| a scroll area's widget added to a tool box (whose page is a scroll area of its own), both resizable | the first keeps watching it: each resizes it to its own viewport on the other's resize, until the stack runs out |

They found bugs in qtc too: `QByteArray::erase` was bound with its iterators taken
for byte buffers (it then freed memory it did not own); deletions Qt makes itself
(`Qt::WA_DeleteOnClose`) went around the ownership rules; a widget moved out of a
layout without a parent widget stayed in it; the forwarding connections
of `qtc_connect_to` went around every check; methods only the meta-object has could reach what the
generator left out; one index check measured a `QPainterPath` by its length in
pixels instead of its element count; the limit on `QBuffer::seek` measured the step past
the end instead of the position, so a buffer grew by 256 MiB a step; rules written for a base
class missed the overrides subclasses declare (a call on a `QHBoxLayout` runs
`QBoxLayout::setGeometry`, which had none of `QLayout`'s checks); the record of which
widgets use which style kept a style Qt had deleted itself, and destroying an object then
read it; the ownership rules did not know that a graphics proxy keeps the widget it embeds
(a scene's `clear()` deleted it inside a column view), that an item view keeps its index
widgets, or that graphics widgets using a style lose it when the style's parent is destroyed
(qtc_shutdown read the style afterwards); and 144 common calls by name were reported as
ambiguous between equivalent overloads (`QColor(QString)` and `QColor(QStringView)`). For 1.1.0:
the `QBuffer::seek` limit covered only calls that resolved to `QBuffer::seek`, not
`QIODevice::seek` on a buffer or classes that seek a buffer they read; and the size check
before `QWidget::grab` did not follow Qt's rule for a rectangle with one negative size.

## What the tests do not show

- Fuzzing finds what its values and orders reach, and it has not run out. The
  rate falls as rounds go (for 1.0.0: 4 failures in 16 orders, then 2 in 16
  new ones, then 5 in 48, then 1 in 64; for 1.1.0, with new modules and
  classes: 3 in 16, then 1 in 16, then 2 in 16, then 1 in 16), but each new
  order can still reach a rarely combined corner; the open ones are in
  [LIMITS.md](LIMITS.md#known-open-failures). A change to the bound methods
  reshuffles every seed, and so does a new refusal, so an order that ran clean
  before may not stay so.
- Some failures depend on timing (they do not happen again when the same calls
  are replayed), which points at memory corruption elsewhere or at Qt's own
  threads; those are the hardest to pin down.
- Only Qt 6.12 is tested. Earlier development rounds also ran on Qt 6.4.2 (the
  crashes particular to it are refused), but this release was not tested there,
  and Qt 6.5 to 6.11 never were; CMake accepts them only with
  `-DQTC_ALLOW_OLDER_QT=ON`.
- Real displays (xcb, Wayland), macOS and Windows are not tested; the graphics
  tests use an X server without a display (Xvfb) and Mesa's software renderers
  (llvmpipe, lavapipe). Direct3D and Metal are neither compiled nor run, and no
  GPU vendor's driver was used.
- `fuzz_rhi` covers the graphics API's argument checking and resource lifetimes;
  shader programs are compiled from a few fixed sources, so what a shader
  computes is not fuzzed (see [LIMITS.md](LIMITS.md#graphics)).
- [LIMITS.md](LIMITS.md) lists what the API does not cover.
