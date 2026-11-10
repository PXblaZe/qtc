# What qtc does not do

qtc's aim is that no sequence of calls through its API crashes the process, hangs
it, or corrupts memory, and that every refusal says why. This page says where
that aim stops, and which parts of Qt are not reachable.

## What "cannot crash" covers

It covers calls made through `qtc.h` with any arguments: stale or made-up
handles, wrong types and counts, out-of-range numbers and enum values, null
objects, objects in the wrong state, objects that would end up inside
themselves, destroying things in any order, and huge sizes. It is tested by
fuzzing every bound method with edge values, in a fixed order and in random
orders with random argument combinations, under AddressSanitizer and
UndefinedBehaviorSanitizer (see [TESTING.md](TESTING.md)).

It does not cover:

- **Qt bugs no check can see yet.** qtc refuses what is known to crash Qt, and
  the fuzzers keep finding such cases; the precondition table holds what they
  found (each rule says which crash it prevents). The fuzzers have not run out:
  the last round found one failure in 64 random orders (fixed since), earlier
  rounds more, and what was seen but is not fixed is listed under
  [Known open failures](#known-open-failures). A crash in Qt that no fuzzer has
  hit can still happen; rare combinations are the likeliest (Qt Charts states,
  rich-text tables, item views with unusual models), and so are plugins, platform
  integration and hardware access.
- **Memory exhaustion by many small steps.** One call that would allocate more
  than the limit (256 MiB by default, `qtc_set_alloc_limit`) is refused: a string,
  a list, an item model, an image or pixmap (Qt checks those only for overflow,
  allocates them and the system kills the process when they are touched), a grab,
  a window (it gets a backing store of its size; a window that grows past the
  limit anyway, through its layout, is not painted), a `QBuffer` grown by seeking
  past its end. A program can still allocate without bound in a loop.
- **Slow operations Qt is slow at.** Calls known to take seconds to minutes are
  refused with `QTC_ERR_LIMIT`; other quadratic paths may remain. The known ones:
  - inserting rows into a `QStringListModel` anywhere but the front, or into the
    models of `QListWidget` and `QTreeWidget` before many rows (Qt inserts them one
    at a time), also through `QIdentityProxyModel`, `QSortFilterProxyModel` and
    `QTransposeProxyModel`;
  - adding columns to a `QStandardItemModel` or a `QTableWidget` with many rows
    (rows × rows × columns; set the columns before adding rows), also through
    header labels and proxies;
  - a `QGridLayout` of more than 2^20 cells (rows × columns: Qt clears that many
    on every layout pass);
  - `QString::count(QRegularExpression)` on more than 8,192 characters (it counts
    overlapping matches, size × size steps), and the other operations with a
    regular expression that visit every match (`split`, `section`, `replace`,
    `remove`, `lastIndexOf`) on more than 1,048,576 characters (about a microsecond
    per match);
  - a text table of a million cells.
- **Objects Qt deletes inside its own calls.** Before qtc_destroy, and before the
  deletions Qt defers (`Qt::WA_DeleteOnClose`, a closed MDI subwindow, a replaced
  check box), the owners that hold an object by a plain pointer let go of it. Qt
  also deletes some objects on the spot (the widgets of a row
  `QFormLayout::removeRow` removes, the buttons `setStandardButtons` replaces):
  qtc refuses to put such an object into a second owner, but cannot release one
  that got there another way.
- **Code that is not qtc's.** `qtc_unsafe_pointer` and `qtc_wrap_pointer` hand
  out and take raw `QObject *` for programs that mix C++ in; what C++ does with
  them is outside qtc. The same goes for QML/JavaScript code a program loads, and
  for Qt plugins.
- **File descriptors handed to Qt.** Methods that take a descriptor over and
  close it later (`QAbstractSocket::setSocketDescriptor`, `QLocalServer::listen`
  with a descriptor, `QFile::open` with `AutoCloseHandle`,
  `QDBusUnixFileDescriptor::giveFileDescriptor`) are refused for a descriptor that
  is not open, but qtc cannot tell whether an open one is the program's to give:
  handing over one Qt itself uses breaks Qt.
- **Threads.** Only the thread that called `qtc_init` may use most of qtc; calls
  from others fail with `QTC_ERR_WRONG_THREAD`. The exceptions are marked "any
  thread" in `qtc.h` (posting, promises, futures, tasks) and the calls an
  override handler or a worker object's slot makes about its own call. Nothing
  stops a program from sharing qtc's views or handles with another thread's own
  code.
- **What override handlers do with the call.** A handler that sets a result Qt
  cannot use (a row count that disagrees with the rows a model reported through
  its begin/end calls, a size hint of a billion pixels) is refused where qtc can
  see it (negative counts, model indexes of other models, begin/end pairs out of
  order) but not where it cannot: a model whose `data()` contradicts its
  `rowCount()` gives views wrong answers, as in C++.
- **What shaders compute.** Every RHI command is checked against what qtc can
  see: the frame and pass, the pipeline's inputs and bindings against the
  shaders' reflection data, every vertex and index a draw fetches against the
  buffers' sizes (and the indices the GPU will see), dispatch sizes against the
  device's limits. A shader that indexes a storage buffer or an image out of
  bounds is undefined behaviour on the GPU, as in C++. On real GPUs that hangs or
  loses the device (reported as `QTC_ERR_REJECTED`, after which the RHI is made
  again); with a software rasterizer (llvmpipe, lavapipe) it is undefined
  behaviour in the process. The same holds for native commands a program
  records itself (`qtc_rhi_begin_external`) and for what it does with native
  handles.
- **The operating system.** Files, sockets, processes and devices behave as the
  OS makes them behave; a blocking call (`QDialog::exec`, `waitForFinished`) blocks.

## What is not bound

Of the 16,298 public methods the Qt 6.12 headers declare in the bound modules,
12,421 are callable, plus 1,009 constructors. [COVERAGE.md](COVERAGE.md) has the
numbers per module and every reason. The parts that are missing, and what to use
instead:

| not bound | why | instead |
|---|---|---|
| operators (`==`, `+`, `<<`, `[]`, …) | not methods C can name | `qtc_val_equal`; the named methods (`QRect::intersected`, `QString::append`, `QTransform::map` …) |
| raw pointers to non-QObject types (`QPainter *`, `QTextBlock *`, `const char *` buffers) | no owner, no lifetime | drawing through `qtc_draw_*`; the value types |
| non-const references (`int &x`) | out-parameters C cannot hold | most have a returning overload; pointer out-parameters (`bool *ok`) are supported |
| iterators, `begin()`/`end()` | pointers into containers | indexes and `qtc_list_*` |
| templates, callables (`std::function`, lambdas, functors) | no C equivalent | signals through `qtc_connect`; `qtc_timer`; `qtc_post` |
| subclassing and virtual overrides (`paintEvent`, `data()` of a model) | C cannot define C++ classes | `QtcCanvas` for painting, event watches, `QStandardItemModel` / `QStringListModel` for models, QML for custom items |
| slots named by strings (`QTimer::singleShot(ms, obj, SLOT(...))`, `QMetaObject::invokeMethod`) | would call any method around every check | `qtc_connect`, `qtc_connect_to`, `qtc_timer` |
| private signals (`rowsInserted`, `QTimer::timeout` as a method) | emitting them by hand breaks the views and proxies listening | they can still be connected to |
| methods of non-copyable value classes (`QBasicTimer`, `QMutex`, `QFile` is an object and fine) | a handle would need to own it | the QObject equivalents |
| OpenGL function tables (`QOpenGLFunctions` and the versioned ones), Vulkan, RHI, the scene graph's C++ API (`QSG*`) | raw GL calls and native handles | `QOpenGLWidget` and `QOpenGLContext` are bound; draw with Qt Quick or `qtc_draw_*` |
| locks and atomics (`QMutex`, `QReadWriteLock`, `QAtomic*`) | qtc runs on one thread | `qtc_post` from other threads |
| a handful of methods that are unsafe with any arguments | listed in `gen/generate.py` (DENY_PAIRS) | |

The bound modules are Core, Gui, Widgets, Network, Sql, Xml, DBus, PrintSupport,
OpenGL, OpenGLWidgets, Svg, SvgWidgets, Qml, Quick, QuickControls2, QuickWidgets,
Multimedia, MultimediaWidgets, SpatialAudio, Charts, Positioning, SerialPort,
WebSockets, WebChannel and UiTools. Not bound: Qt 3D, Qt WebEngine, Qt Bluetooth,
Qt NFC, Qt Sensors, Qt Location, Qt Remote Objects, Qt Virtual Keyboard, Qt Data
Visualization / Graphs, Qt HTTP Server and the other add-ons. QML types of any
installed module still work from QML.

## Refusals that are qtc's, not Qt's

Some things Qt allows, qtc refuses, because Qt would crash on them in some state
and qtc cannot tell the states apart. Each is reported with the reason. The main
ones:

- Destroying an object something else holds by a plain pointer it cannot let go
  of, or one Qt owns (the application object, screens, the clipboard, a window's
  internal parts, the parts a constructor made, and widgets of classes qtc does not
  bind inside other widgets: parts Qt makes for itself later, such as a combo box's
  popup container). Destroy the owner instead.
- Moving an object that is held somewhere (a widget in a status bar, a style a
  proxy style owns, a widget in an MDI subwindow, a widget embedded in a graphics
  scene, whose proxy deletes it wherever it went, an item view's index or cell
  widget) without taking it out first. Destroying an index widget makes its view
  let go of it first (`setIndexWidget(index, nullptr)`); Qt itself would show it
  through a null pointer on the next layout.
- A device an NMEA source (`QNmeaPositionInfoSource`, `QNmeaSatelliteInfoSource`)
  reads is open for reading or not open at all: Qt's reader loops forever on a
  device that reports bytes available while every read fails.
- `QWizard::setSideWidget` takes a widget with no parent (or one in the wizard):
  Qt takes it in only when the wizard is next restyled, in the middle of
  `QApplication::setStyle`.
- Setters that delete the part they replace (`QAbstractScrollArea::setViewport`
  and the scroll bars, `QScrollArea::setWidget`, `QTableView` and `QTreeView`
  headers, `QComboBox::setView` and `setLineEdit`, `setDocument` of the text
  edits) first make other owners let go of what they hold in that part, as
  `qtc_destroy` does, and are refused when one cannot.
- Layouts without a parent widget: Qt takes a widget out of a layout only through
  the layout's parent widget, so such a layout would keep, and could delete, a
  widget that moved elsewhere. qtc takes a widget out of them when it gets another
  parent, and an owner that keeps a widget by a plain pointer (a tab widget's
  corner, a scroll area's viewport, a message box's check box …) refuses a widget
  still in one.
- `QMdiSubWindow::setSystemMenu` takes only a menu made with the subwindow as its
  parent: Qt makes any other menu a child widget, and showing it then crashes.
- `QMainWindow::tabifyDockWidget` and `splitDockWidget` take two different dock
  widgets: one tabified with itself can end up in a floating tab group no close
  reaches, and `QApplication::closeAllWindows` then never returns.
- `QWidget::setAttribute` refuses the attributes Qt keeps itself: the widget state
  (`Qt::WA_WState_*`), the others Qt's documentation leaves out as internal
  (`WA_LaidOut`, `WA_PendingUpdate` …) and those it documents as set and cleared
  by Qt alone (`WA_Mapped`, `WA_UnderMouse`, `WA_UpdatesDisabled`,
  `WA_PendingMoveEvent` …). A window marked visible from outside has no native
  window, and `QApplication::closeAllWindows` crashes on it.
- `QMessageBox::clickedButton()`, `defaultButton()` and `escapeButton()` fail with
  `QTC_ERR_STATE` once that button is destroyed: Qt keeps pointing at it. A button
  destroyed through qtc leaves its message box first (`removeButton`), because the
  box also keeps its custom buttons in a list `buttonText()` and `exec()` read.
- Qt Charts: the boundary lines of an area series keep their point label clipping
  on (Qt crashes building or updating the area series' chart item when it is off,
  also for a while after the area series left the chart), and
  `attachAxis`/`detachAxis` need an axis (Qt checks for null only in debug builds).
- Qt Charts model mappers write a series' changes into their model without checking
  that there is one: a mapper takes a series only once it has a model and keeps a
  model while it has a series; when its model is destroyed it gets an empty one of
  its own (the series follows it and empties). The XY, bar, pie and box plot
  mappers cannot let go of a series (Qt only stops following it and keeps pointing
  at it): destroy the mapper instead.
- Qt Charts views: a `QChartView` keeps a plain pointer to its chart, which lives
  in the view's own scene, so that scene cannot be cleared and a second view cannot
  be made with the first one's chart. An area series goes into a chart only with its
  upper series (Qt animates the missing one).
- Rich text tables: rows and columns are not removed across merged cells, and
  columns are not added to a table with a cell merged across several rows and
  columns (split the cells first); Qt corrupts the document otherwise.
- Layouts: Qt's layout engine adds sizes, spacing and stretch factors up in `int`,
  and once a sum wraps its space distribution can loop forever. So stretch
  factors are 0 to 65,535, spacing, spacer sizes and margins at most 16,777,215
  (`QWIDGETSIZE_MAX`), and a layout whose items add up past 2^30 is not laid out:
  `setGeometry`, `activate`, `show()` and `exec()` fail with `QTC_ERR_LIMIT`, and
  resizes and layout requests leave it as it was. `QSplitterHandle` has no
  constructor (a splitter makes its own, `QSplitter::handle`), and a handle stays
  in its splitter: it is never placed anywhere as a widget.
- Widget and window coordinates (`move`, `setGeometry`, `QWindow::setPosition` …)
  are within ±16,777,215: a corner near `INT_MAX` wraps the far edge, and painting
  the window then crashes. The same bound holds for `QRegion` rectangles and
  ellipses; a polygon region must fit in 16,384 × 16,384 with its edges crossing at
  most 2^19 lines in all (a self-crossing polygon over a large area takes Qt
  minutes), and `QPainterPath::toFillPolygons` takes at most 16,384 subpaths (it
  compares each with every other).
- A button in a `QDialogButtonBox` stays there until `removeButton`: the box deletes
  the buttons in its lists on `clear()` and `setStandardButtons()`, wherever they
  have moved since (and a message box's own button box is not cleared from
  outside). A window embedded with `QWidget::createWindowContainer` stays
  with its container and goes with it (the container reads it without checking
  whenever one of its parents changes).
- A widget that `setParent` moves into a window of its own (window flags) loses its
  focus first: Qt would leave its old parents pointing at it as their focus child.
- A widget a `QFocusFrame` watches stays where it is until the frame lets go of it
  (the frame maps the widget's position from the widget's parent, which must stay
  one of its ancestors). When Qt itself takes the widget out of its parent (a tool
  bar or menu letting go of a `QWidgetAction`'s widget), the frame lets go of it
  first. `QWizard::sideWidget()` fails with `QTC_ERR_STATE` when the side widget
  was destroyed another way than through qtc.
- A window that was given new window flags (or no parent) after it was shown, and
  then went into a parent before being shown again, loses the native window Qt
  leaves it: Qt would take it for the native parent of what goes into it, and a
  `QOpenGLWidget` placed there crashes Qt.
- A `QComboBox` or `QCompleter` popup is a `QListView`, `QTableView` or `QTreeView`
  (a `QColumnView` there crashes); a `QFileDialog`'s proxy model keeps the dialog's
  model as its source (the dialog's completer assumes it).
- `QButtonGroup::setId` only for buttons in the group (the group would keep the id
  of a deleted button).
- Signals emitted by hand are checked like methods where listeners act on their
  arguments: `QTextDocument::contentsChange` only for a range inside the document
  (its layouts lay out the range named).
- `QPainterPath`: boolean operations on paths crossing more than 4,096 times or with
  more than 32,768 segments, measuring paths of more than about 2^20 curve pieces,
  and mapping more than 2^20 points through a perspective transform are refused
  (Qt takes minutes on each), as are coordinates beyond ±2^30 or not finite for the
  clipper, and more than 1,024 copies of one point (the clipper's k-d tree goes
  one level deeper per copy). `setElementPositionAt` takes finite coordinates only, as Qt's own
  `moveTo`, `lineTo` and the rest do.
- Values Qt reads past the end of a list with: `QLCDNumber::setDigitCount(0)` (then
  `display()` with a small decimal point), `QPalette::setCurrentColorGroup` with
  `Current`, `All` or `NColorGroups`, `QWizardPage::setButtonText` with `Stretch`,
  the `QGridLayout` getters (`rowStretch`, `columnMinimumWidth` …) past the last
  row or column, `QGeoPath::length` from a negative index.
- `QComboBox::setLabelDrawingMode(UseDelegate)`: Qt crashes painting such a box
  without a current item.
- `QAmbientSound::pause()` and `QSpatialSound::pause()` before `play()`: Qt
  aborts.
- `QAudioEngine` with a sample rate that is not a standard one (44,100, 48,000 …):
  Qt's resampler crashes on others.
- `QNetworkDiskCache::setCacheDirectory` refuses the root of the file system and
  the home directory: the cache deletes every file ending in `.d` under its
  directory when it expires. Any other directory is the program's choice; give
  the cache one of its own.
- Screen and window capture (`QScreenCapture`, `QWindowCapture`) is bound but not
  fuzzed: Qt grabs from a thread of its own, and on the offscreen platform it reads
  the window list from that thread while the program changes it (a crash inside
  Qt that qtc cannot prevent). Avoid creating or destroying windows while a capture
  runs on that platform.
- `QQmlDebuggingEnabler` is not bound: its services can make the next QML engine
  wait for a debugger forever. Pass `-qmljsdebugger=port:3768` (or `file:...`) in
  the arguments to `qtc_init_argv` instead.
- `QObjectCleanupHandler` is not bound: it deletes what it holds whoever else owns
  it, and crashes when what it holds comes to contain it.
- Some refusals depend on the Qt version: crashes fixed in later releases are
  refused only on the releases that have them.

## Known open failures

Every failure the last fuzz rounds found is fixed in this release (see
[TESTING.md](TESTING.md#results)). Two failures from earlier rounds are not
explained:

- **A crash while painting, seen once.** In one random order of an earlier round,
  painting a window while queued events were delivered crashed in Qt's
  graphics-effect path, with no paint engine. Replaying the same calls did not
  crash again, and no round since has met it. It may depend on timing, or on
  memory corruption elsewhere that a later fix removed; it is not known which.
- **A hang after `QIODevice::readChannelFinished` was emitted by hand on a
  `QBuffer`, seen once.** That buffer was the device of a `QNmeaSatelliteInfoSource`,
  so it was most likely the NMEA reader looping on a buffer opened without
  reading, which this release refuses (see above); it was not reduced to its
  calls to make sure, and no round since has met it.

## Graphics

- **Backends.** The Null backend, OpenGL (Mesa llvmpipe, under Xvfb) and Vulkan
  (Mesa lavapipe) are tested, rendering included. The Direct3D 11/12 and Metal
  code paths (backend creation, native handles) are written against Qt's headers
  but cannot be compiled or run on Linux: they are untested.
- **What the RHI layer leaves out**: textures other than 2D with one mip level
  (no cube maps, arrays, 3D textures, mipmaps or multisampling), depth textures,
  formats beyond RGBA8, BGRA8, R8, RGBA16F, RGBA32F and R32F, multiple render
  targets, scissors, stencil operations, blend modes other than premultiplied
  alpha, dynamic uniform-buffer offsets, push constants, separate samplers,
  tessellation and geometry shaders, buffer-to-buffer copies, timestamps, and
  Qt Quick's own rendering hooks (`QQuickRhiItem` and the scene graph's render
  passes run on Qt Quick's render thread). Shaders with matrix, double or array
  vertex inputs are refused.
- **Index buffers** are `QTC_RHI_IMMUTABLE` or `QTC_RHI_STATIC` and keep a CPU
  copy of their contents (twice their size in memory) so indexed draws can be
  checked; each check reads the indices it draws (cached while they do not
  change). Draws of very large index ranges cost CPU time for that.
- **Widgets.** A `QRhiWidget` renders through its window's RHI. A second
  `grabFramebuffer()` on a hidden widget with OpenGL was seen to render into a
  texture the grab does not read (Qt's behaviour: the grab makes a new RHI
  without calling `initialize` again); shown widgets and Vulkan behave.
- **Thread.** RHI calls are GUI-thread calls; Qt's RHI objects are not
  thread-safe.

## Private Qt APIs

Runtime classes use Qt's private `QMetaObjectBuilder` (Qt6::CorePrivate) and the
graphics layer Qt's semi-public RHI and shader baker (Qt6::GuiPrivate,
Qt6::ShaderToolsPrivate). Qt does not keep those compatible across minor
releases: a libqtc built against Qt 6.12 must run on Qt 6.12.x, and needs a
rebuild (and possibly changes) for another minor release. Without the private
modules the configure step leaves those parts out (`QTC_ERR_UNSUPPORTED` at run
time) instead of failing.

## Qt versions

Qt 6.12 LTS is the supported release, and the only one this release of qtc was
tested on (6.12.0). CMake refuses an older Qt unless `-DQTC_ALLOW_OLDER_QT=ON` is
passed; with it, Qt 6.4 to 6.11 are accepted, and the methods, enum values and
classes a release does not have are compiled out. Those builds are not tested:
crashes that only older releases have may not be guarded. (Earlier development
builds were also fuzzed on Qt 6.4.2, and the crashes found there are refused; the
current code was not.) Newer Qt 6 releases are accepted but not tested. Enum
values are checked against the release qtc was built with.

## Platforms

Developed and tested on Linux (x86-64, GCC 13), with Qt's offscreen platform.
Running on a display (xcb, Wayland) uses the same code, but the automated tests do
not. The code has no Linux-only parts, but macOS and Windows builds are
untested.

## goose-qt

The Goose binding is generated from qtc's manifest and inherits all of the above.
Goose has no function pointers and its exported functions run on the program's
main thread only, so: override handlers are one exported function
(`goose_qt_override`) that runs for calls on the GUI thread (methods Qt calls on
other threads, such as `QThread::run`, run Qt's own implementation); qtc's
thread-pool tasks (`qtc_task_*`, which take C functions) are not bound, and Goose
worker threads (`thread_fn`) fill promises instead.
Its enum constants are those of Qt 6.12; on an older Qt (an untested opt-in build),
values the release does not have are refused at the call.
