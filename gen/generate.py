#!/usr/bin/env python3
"""Generate qtc's thunk tables, enum tables and API manifest from scanned Qt models.

Inputs: models written by scan_qt.py, one per Qt version (qt-6.4.2.json ... qt-6.12.0.json).
The newest model defines the API; the others give each class, method and
constructor the first Qt version that has it, which becomes a QT_VERSION guard.

Outputs (under --out):
  src/gen/qtc_gen_<Module>_<n>.cpp   thunks and class tables, per module, in chunks
  src/gen/qtc_gen_enums.cpp          enum value tables used to validate arguments
  src/gen/qtc_gen_register.cpp       qtc_gen_register_all() (the modules in libqtc), the module index
  src/gen/qtc_gen_mod_<M>.cpp        qtc_module_init_<M>(): a module library's registration
  src/gen/modules.cmake              the generated sources of each module
  manifest/qtc_api.json              everything bound, for language bindings
  manifest/coverage.json             what was bound and what was skipped, and why
"""
import argparse
import collections
import glob
import json
import lzma
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import overrides
from preconditions import preconditions  # noqa: E402


def write_if_changed(path, text):
    """Rewrite a generated file only when its content changes, so builds stay incremental."""
    try:
        with open(path) as f:
            if f.read() == text:
                return
    except OSError:
        pass
    with open(path, "w") as f:
        f.write(text)

MODULE_DEPS = {
    "Core": [], "Gui": ["Core"], "Widgets": ["Gui"], "Network": ["Core"], "Sql": ["Core"],
    "PrintSupport": ["Widgets"], "Xml": ["Core"], "DBus": ["Core"], "OpenGL": ["Gui"],
    "OpenGLWidgets": ["OpenGL", "Widgets"], "Svg": ["Gui"], "SvgWidgets": ["Svg", "Widgets"],
    "Qml": ["Network"], "Quick": ["Qml", "Gui"], "QuickWidgets": ["Quick", "Widgets"],
    "QuickControls2": ["Quick"], "Multimedia": ["Gui", "Network"],
    "MultimediaWidgets": ["Multimedia", "Widgets"], "SpatialAudio": ["Multimedia"],
    "Charts": ["Widgets"], "SerialPort": ["Core"], "Positioning": ["Core"],
    "WebSockets": ["Network"], "WebChannel": ["Core"], "UiTools": ["Widgets"],
    "NetworkAuth": ["Network"], "HttpServer": ["Network"], "Mqtt": ["Network"], "Coap": ["Network"],
}
# The modules libqtc binds itself; every other module is a library of its own (libqtc_<M>),
# loaded the first time a program uses one of its classes (src/qtc_module.cpp).
CORE_MODULES = ["Core", "Gui", "Widgets"]
NEED_WIDGETS = {"Widgets", "PrintSupport", "SvgWidgets", "MultimediaWidgets", "QuickWidgets",
                "OpenGLWidgets", "Charts", "UiTools"}
NEED_GUI = {"Gui", "Quick", "QuickControls2", "Svg", "Multimedia", "SpatialAudio", "OpenGL"}
# Gui value classes that work without a QGuiApplication.
CORE_SAFE_GUI = {"QColor", "QImage", "QTransform", "QMatrix4x4", "QVector2D", "QVector3D", "QVector4D",
                 "QQuaternion", "QPolygon", "QPolygonF", "QRegion", "QPainterPath", "QPen", "QBrush",
                 "QLinearGradient", "QRadialGradient", "QConicalGradient", "QGradient", "QPageLayout",
                 "QPageSize", "QPageRanges", "QColorSpace", "QColorTransform", "QRgba64", "QImageReader",
                 "QImageWriter", "QPainterPathStroker", "QTextOption"}

# Classes never bound: GL function tables need a current context, the rest are
# RAII guards, synchronization primitives, iterators or platform internals.
DENY_CLASS_RE = re.compile(
    # QColormap: an 8-bit display helper whose instance() crashes before Qt sets it up (offscreen, Qt 6.4)
    # QObjectCleanupHandler: deletes what it holds when it dies, whoever else owns it, and crashes
    # when what it holds comes to contain it (a handler deleting itself from inside clear())
    # QQmlDebuggingEnabler: starts the QML debug services, which can block the next engine waiting
    # for a debugger; the -qmljsdebugger=... command-line argument (qtc_init_argv) does the same
    r"^(QColormap|QObjectCleanupHandler|QQmlDebuggingEnabler|"
    r"QOpenGL(Extra)?Functions|QAbstractOpenGLFunctions|QOpenGLVersionFunctions|QOpenGLFunctions_|"
    r"QSignalBlocker|QMutexLocker|QReadLocker|QWriteLocker|QScopedValueRollback|QBasicMutex|QMutex|"
    r"QRecursiveMutex|QReadWriteLock|QSemaphore|QSemaphoreReleaser|QWaitCondition|QBasicReadWriteLock|"
    r"QAtomic|QBasicAtomic|QThreadStorage|QStringBuilder|QStringTokenizer|QJni|QNativeInterface|QPlatform|"
    r"QTest|QSignalSpy|QAbstractEventDispatcher|QEventLoopLocker|QDeadlineTimer::|QPropertyBindingSourceLocation|"
    r"QUntypedProperty|QPropertyNotifier|QPropertyChangeHandler|QPropertyObserver|QBindable|QUntypedBindable|"
    r"QPropertyData|QObjectBindableProperty|QStaticByteArrayMatcher|QArrayData|QTypedArrayData|QContainerImplHelper|"
    r"QListSpecialMethods|QMetaTypeInterface|QMetaObject::SuperData|QMetaObject::Data|QtMetaContainerPrivate|"
    r"QIterable|QIterator|QConstIterator|QAssociativeIterable|QSequentialIterable|QMetaSequence|QMetaAssociation|"
    r"QMetaContainer|QTaggedPointer|QBasicUtf8StringView|QByteArray::FromBase64Result|QTimerEvent|QChildEvent|"
    r"QDynamicPropertyChangeEvent|QDeferredDeleteEvent|QEvent$|QInputEvent|QPointerEvent|QSinglePointEvent|"
    r"QMouseEvent|QKeyEvent|QWheelEvent|QTabletEvent|QTouchEvent|QNativeGestureEvent|QHoverEvent|QFocusEvent|"
    r"QPaintEvent|QMoveEvent|QResizeEvent|QCloseEvent|QIconDragEvent|QShowEvent|QHideEvent|QContextMenuEvent|"
    r"QInputMethodEvent|QInputMethodQueryEvent|QDropEvent|QDragMoveEvent|QDragEnterEvent|QDragLeaveEvent|"
    r"QHelpEvent|QStatusTipEvent|QWhatsThisClickedEvent|QActionEvent|QFileOpenEvent|QToolBarChangeEvent|"
    r"QShortcutEvent|QWindowStateChangeEvent|QScrollPrepareEvent|QScrollEvent|QScreenOrientationChangeEvent|"
    r"QApplicationStateChangeEvent|QEnterEvent|QExposeEvent|QPlatformSurfaceEvent|QGraphicsSceneEvent|"
    r"QGraphicsScene\w+Event|QGestureEvent|QChildWindowEvent|QAccessible\w*Event|QAccessibleInterface|"
    r"QAccessible\w*Interface|QStyleOption|QStyleHintReturn|QItemEditorCreatorBase|QQmlInfo|QQmlListReference|"
    r"QQmlListProperty|QJSPrimitiveValue|QDebug|QNoDebug|QDebugStateSaver|QMessageLogger|QLoggingCategory|"
    r"QTextStream|QDataStream|QPainter$|QPainter::|QPaintEngine|QPaintEngineState|QPaintDevice$|QPagedPaintDevice|"
    r"QOpenGLPaintDevice|QBackingStore|QRhi(?!Widget$)|QSG|QQuickRenderTarget|QQuickGraphicsDevice|QQuickGraphicsConfiguration|"
    r"QVulkan|QStringConverter|QStringEncoder|QStringDecoder|QStringDecoder|QAnyStringView|QStringView|QLatin1String|"
    r"QUtf8StringView|QByteArrayView|QStringMatcher|QByteArrayMatcher|QLatin1StringMatcher|QKeyValueIterator|"
    r"QPermission|QFutureInterface|QFuture|QPromise|QFutureWatcher|QFutureSynchronizer|QtConcurrent|QRunnable|"
    r"QSharedPointer|QWeakPointer|QScopedPointer|QPointer|QExplicitlySharedDataPointer|"
    r"QSharedDataPointer|QSharedData$|QAdoptSharedDataTag|QEnableSharedFromThis|QSpan|QTypeInfo|"
    r"QFunctionPointer|QObjectData|QObjectUserData|QAbstractDeclarativeData|QDynamicMetaObjectData|"
    r"QAbstractDynamicMetaObject|QCborStreamReader|QCborStreamWriter|QDirIterator|QDirListing|QStorageInfo_|"
    r"QRawFont|QGlyphRun|QTextInlineObject|QTextEngine|QTextLayout|QTextLine|QAbstractTextDocumentLayout|"
    r"QTextObjectInterface|QWidgetItem|QSpacerItem|QLayoutItem|QWidgetItemV2|QGraphicsItem|QAbstractGraphicsShapeItem|"
    r"QGraphics\w+Item$|QGraphicsLayoutItem|QGraphicsItemGroup|QStandardItem$|QListWidgetItem|QTreeWidgetItem|"
    r"QTableWidgetItem|QTreeWidgetItemIterator|QUndoCommand|QTableWidgetSelectionRange|QItemEditorFactory|"
    r"QStyleFactory|QTextBlockUserData|QTextFrameLayoutData|QGestureRecognizer|QAccessiblePlugin|QStylePlugin|"
    r"QIconEnginePlugin|QImageIOPlugin|QGenericPlugin|QSqlDriverPlugin|QSqlDriverCreatorBase|QFactoryInterface|"
    r"QQmlExtensionPlugin|QQmlEngineExtensionPlugin|QQmlTypesExtensionInterface|QQmlExtensionInterface|"
    r"QQmlParserStatus|QQmlPropertyValueSource|QQmlIncubationController|QQmlImageProviderBase|QQmlAbstractUrlInterceptor|"
    r"QQmlNetworkAccessManagerFactory|QIconEngine|QImageIOHandler|QScopeGuard|QDesignerCustomWidget|"
    r"QAbstractNativeEventFilter|QQuickImageProvider|QQuickAsyncImageProvider|QQuickTextureFactory|"
    r"QQuickImageResponse|QQuickTextDocument|QPdfWriter|QPicture|QPictureIO|QSqlResult|QSqlDriver$|"
    r"QNetworkProxyFactory|QSslPreSharedKeyAuthenticator|QNetworkAccessManager::|QHttpPart|QAbstractVideoBuffer|"
    r"QAbstractVideoBuffer::|QVideoFrame::MapData|QHostInfo$|QTypeRevision::|QMetaObject$|QMetaMethod$|"
    r"QMetaProperty$|QMetaEnum$|QMetaClassInfo$|QMetaType$|QGenericArgument|QGenericReturnArgument|"
    r"QMetaMethodArgument|QMetaMethodReturnArgument|QMetaObject::Connection|QCalendar::SystemId|QOperatingSystemVersionBase|"
    r"QBitRef|QByteRef|QCharRef|QTypedArray|QListData|QHashData|QMapData|QPropertyBindingError|QPropertyBinding|"
    r"QUntypedPropertyBinding|QtPrivate|QWidgetData|QWindowsWindowFunctions|QXcbWindowFunctions|"
    r"QAccessibleBridge|QAccessibleObject|QAccessibleWidget|QAccessibleApplication|QAccessibleTextUpdateEvent|"
    r"QSimpleXmlNodeModel|QAnimationDriver|QUnhandledException|QException|QWinEventNotifier|QDeferredDeleteEvent|"
    r"\w+ImplBase$|QVariant::|QQmlTypeNotAvailable|"
    # iterators reference containers whose lifetime a handle cannot follow; the others only work
    # inside a D-Bus call, an input event or a QFormDataBuilder
    r"\w+::(const_)?iterator$|\w+::(Const)?Iterator$|QDBusContext$|QEventPoint$|QFormDataPartBuilder$|"
    # raw OpenGL resources need the context they were made in to be current, and custom
    # paintGL code (subclassing) to be of any use; QtcCanvas, QPainter and QML draw instead
    r"QOpenGLShaderProgram$|QOpenGLShader$|QOpenGLBuffer$|QOpenGLVertexArrayObject|QOpenGLTexture$|"
    r"QOpenGLFramebufferObject$|QOpenGLTimerQuery$|QOpenGLTimeMonitor$|QOpenGLDebugLogger$|QOpenGLTextureBlitter$|"
    # references into containers, like iterators, and paint-engine internals
    r"QCborValueRef$|QCborValueConstRef$|QJsonValueRef$|QJsonValueConstRef$|QAssociative\w*Iterator$|"
    r"QSequential\w*Iterator$|QTextItem$)")

# Methods never bound (anywhere): they would break qtc's own guarantees.
DENY_METHOD = {"moveToThread", "deleteLater", "connect", "disconnect", "qt_metacall", "qt_metacast", "metaObject",
               "tr", "trUtf8", "d_func", "qt_check_for_QOBJECT_macro", "staticMetaObject", "setEventDispatcher",
               "installNativeEventFilter", "removeNativeEventFilter", "notify", "event", "eventFilter",
               "nativeEvent", "registerUserData", "setUserData", "userData", "qt_static_metacall",
               "devType", "metric", "initPainter", "redirected", "sharedPainter", "paintEngine", "data_ptr",
               "bindingStorage", "isSignalConnected", "receivers", "senderSignalIndex",
               # the result would point into qtc's temporary argument buffer
               "fromRawData", "setRawData"}
DENY_PAIRS = {("QCoreApplication", "exec"), ("QGuiApplication", "exec"), ("QApplication", "exec"),
              ("QCoreApplication", "exit"), ("QCoreApplication", "quit"), ("QObject", "destroyed"),
              ("QWidget", "destroy"), ("QWidget", "create"), ("QWindow", "destroy"), ("QWindow", "create"),
              ("QThread", "terminate"), ("QCoreApplication", "setEventDispatcher"),
              # a released thread nobody reserved breaks the pool's count (qtc_task_start_in runs tasks)
              ("QThreadPool", "reserveThread"), ("QThreadPool", "releaseThread"),
              ("QThreadPool", "startOnReservedThread"), ("QThreadPool", "tryTake"),
              ("QCoreApplication", "eventDispatcher"), ("QThread", "setEventDispatcher"),
              ("QThread", "eventDispatcher"), ("QObject", "setParent"), ("QWidget", "setParent"),
              # only valid inside a child process modifier; elsewhere it ends the calling process
              ("QProcess", "failChildProcessModifier"),
              # raw output buffers of a fixed size or a pointer range
              ("QMatrix4x4", "copyDataTo"), ("QRandomGenerator", "generate"), ("QRandomGenerator64", "generate"),
              # moves rendering to another thread, outside qtc's GUI-thread model
              ("QQuickRenderControl", "prepareThread"),
              # deletes its argument behind everyone's back (views call it for their own editors)
              ("QAbstractItemDelegate", "destroyEditor"), ("QItemDelegate", "destroyEditor"),
              ("QStyledItemDelegate", "destroyEditor"),
              # only valid for a context that a component created (crashes otherwise)
              ("QQmlContext", "importedScript"),
              # internal API for QML tooling: keeps plain pointers to the component and the item
              ("QQuickView", "setContent"), ("QQuickWidget", "setContent"),
              # the media backend reads or writes the device from its own threads, concurrently with
              # anything else that uses it: play and record through URLs (files, qrc:) instead
              ("QMediaPlayer", "setSourceDevice"), ("QAudioDecoder", "setSourceDevice"),
              ("QMediaRecorder", "setOutputDevice"),
              # call a method named by a string (see MEMBER_PARAMS)
              ("QDesktopServices", "setUrlHandler"), ("QGeoAreaMonitorSource", "requestUpdate"),
              # declared but not exported before Qt 6.5 (and the constructor takes the device)
              ("QWaveDecoder", "setIODevice")}
# Constructors the fuzzer leaves alone: they listen on or connect to system services.
NOFUZZ_CTOR_CLASSES = {"QDBusServer", "QDBusInterface", "QDBusAbstractInterface", "QDBusConnectionInterface",
                       "QDBusPendingCallWatcher", "QDBusPendingCall"}
# Constructors never bound: (class, canonical parameter types joined by ",").
MEMBER_PARAMS = {"member", "slot", "ambiguousMember", "errorSlot", "returnMethod", "errorMethod"}
DENY_CTORS = {("QQmlComponent", "QObject *")}   # a component without an engine can do nothing but crash
# Text objects belong to a document's structure; made on their own they crash on first use.
# QTextCursor::insertTable / insertList / insertFrame and QTextDocument::rootFrame make real ones.
DENY_CTOR_CLASSES = {"QTextFrame", "QTextList", "QTextTable", "QTextBlockGroup", "QTextObject",
                     # legend markers are made and owned by a chart's legend
                     "QLegendMarker", "QAreaLegendMarker", "QBarLegendMarker", "QBoxPlotLegendMarker",
                     "QCandlestickLegendMarker", "QPieLegendMarker", "QXYLegendMarker",
                     # a splitter makes its handles (QSplitter::handle); one made apart is taken in by
                     # the splitter as a content widget, which replaceWidget then takes out of it
                     # (its size hint reads the missing parent)
                     "QSplitterHandle"}
# Pointer parameters of primitive type that are buffers or arrays, not single out-values: they stay
# null-only, and a method that requires one is not bound.
RAW_POINTER_PARAMS = {"tabarray", "tabArray", "tabStops", "data", "buffer", "begin", "end", "values", "array"}
# setParent is bound for QWidget/QObject through dedicated entries in the runtime (with a cycle check);
# the generated overloads are kept because do_call guards them. Remove the two pairs from the deny set:
DENY_PAIRS -= {("QObject", "setParent"), ("QWidget", "setParent")}
# "without telling the window system" (Qt's own warning): the widget's flags or state then disagree
# with what it is, and Qt crashes on that (QGraphicsScene::addWidget after overrideWindowFlags)
DENY_PAIRS |= {("QWidget", "overrideWindowFlags"), ("QWidget", "overrideWindowState")}

# Methods that block (nested event loops, sleeps, waits) or touch the file system,
# processes or the desktop: bound, but the fuzzer leaves them alone.
BLOCKING_RE = re.compile(r"^(exec|wait|waitFor\w*|sleep|msleep|usleep|getOpenFileNames?|getOpenFileUrls?|"
                         r"getSaveFileName|getSaveFileUrl|getExistingDirectory\w*|getText|getMultiLineText|getInt|"
                         r"getDouble|getItem|getColor|getFont|information|question|warning|critical|about|aboutQt|"
                         r"processEvents|sendPostedEvents|execute|exec_|connectToBus|connectToPeer|sessionBus|systemBus|"
                         # QToolButton and QPushButton run their menu's event loop until it closes
                         r"showMenu|"
                         # QSharedMemory and QSystemSemaphore wait for a lock another process may hold
                         r"attach|lock|acquire)$")
# Names the fuzzer skips on any class: they end the process or start external programs.
NOFUZZ_ANY_RE = re.compile(r"^(start|startDetached|openUrl|kill|terminate|exit|quit|beep|alert|setUnixSignalMask|"
                           r"installMessageHandler|setMessagePattern|setFilterRules|setUncaughtExceptionHandler|"
                           r"reportError|setDefaultFactory|setApplicationProxy|registerSqlDriver|setStandardLocation|"
                           r"setTestModeEnabled|setLibraryPaths|addLibraryPath|removeLibraryPath|showHelp|showVersion|"
                           r"showMessageAndExit|setDesktopSettingsAware|setOverrideCursor|restoreOverrideCursor|"
                           r"changeOverrideCursor|setActiveWindow|installTranslator|removeTranslator|"
                           # these adopt a file descriptor and close it later: only for descriptors the caller owns
                           r"setSocketDescriptor|setNativeDescriptor|giveFileDescriptor|startCommand)$")
# Classes whose methods named in NOFUZZ_RE touch files, the network, devices, processes or
# application-wide state; the same names on other classes (QString::remove, QLabel::setText) are fuzzed.
NOFUZZ_CLASS_RE = re.compile(
    r"^(QFile|QFileDevice|QSaveFile|QTemporaryFile|QTemporaryDir|QDir|QLockFile|QFileInfo|QStorageInfo|QStandardPaths|"
    r"QSettings|QFileSystemModel|QFileSystemWatcher|QResource|QLibrary|QPluginLoader|QProcess|QDesktopServices|"
    r"QCoreApplication|QGuiApplication|QApplication|QClipboard|QCursor|QTranslator|QFontDatabase|QSqlDatabase|"
    r"QSqlQuery|QSqlQueryModel|QSqlTableModel|QSqlRelationalTableModel|QNetworkAccessManager|QAbstractSocket|"
    r"QTcpSocket|QUdpSocket|QSslSocket|QTcpServer|QSslServer|QLocalServer|QLocalSocket|QHostInfo|QDnsLookup|"
    r"QNetworkInformation|QHttp2Configuration|QWebSocket|QWebSocketServer|QSerialPort|QSerialPortInfo|QMediaPlayer|"
    r"QMediaRecorder|QMediaCaptureSession|QSoundEffect|QAudioSink|QAudioSource|QCamera|QImageCapture|QScreenCapture|"
    r"QWindowCapture|QAudioInput|QAudioOutput|QAudioDecoder|QAudioBufferInput|QAudioBufferOutput|QVideoFrameInput|"
    r"QSystemTrayIcon|QSessionManager|QSharedMemory|QSystemSemaphore|QPrinter|QPrinterInfo|QPrintDialog|"
    r"QPrintPreviewDialog|QPrintPreviewWidget|QPageSetupDialog|QFileDialog|QFileIconProvider|QDBusConnection|"
    r"QDBusInterface|QDBusAbstractInterface|QDBusConnectionInterface|QDBusServer|QDBusServiceWatcher|QDBusMessage|"
    r"QGeoPositionInfoSource|QGeoSatelliteInfoSource|QGeoAreaMonitorSource|QNmeaPositionInfoSource|"
    r"QNmeaSatelliteInfoSource|QQmlApplicationEngine|QQmlEngine|QJSEngine|QQmlComponent|QQuickView|QQuickWidget|"
    r"QUiLoader|QImageReader|QImageWriter|QMovie|QPixmap|QImage|QIcon|QPicture|QTextDocumentWriter|QSvgGenerator|"
    r"QSvgRenderer|QSvgWidget|QOpenGLContext|QOffscreenSurface|QWindow|QScreen|QStyleHints|QInputMethod|"
    r"QThread|QThreadPool|QTimer|QSocketNotifier|QWinEventNotifier|QMimeDatabase|QSslConfiguration|QSslCertificate|"
    r"QSslKey|QNetworkCookieJar|QNetworkDiskCache|QAbstractNetworkCache|QNetworkProxy|QNetworkInterface|"
    r"QAuthenticator|QHttpMultiPart|QRestAccessManager|QNetworkRequestFactory)$")
NOFUZZ_RE = re.compile(r"^(remove\w*|rename|rmdir|rmpath|mkdir|mkpath|removeRecursively|setCurrent|start|"
                       r"startDetached|openUrl|kill|terminate|close|link|copy|moveToTrash|setPermissions|"
                       r"resize|setFileName|open|load|save|setPath|cd|cdUp|setLibraryPaths|addLibraryPath|"
                       r"removeLibraryPath|installTranslator|setStyle|setPalette|setFont|setWindowIcon|"
                       r"setDesktopSettingsAware|setOverrideCursor|restoreOverrideCursor|changeOverrideCursor|"
                       r"setApplicationName|setOrganizationName|setOrganizationDomain|setApplicationVersion|"
                       r"setQuitOnLastWindowClosed|setAttribute|setSetuidAllowed|setLayoutDirection|"
                       r"setHighDpiScaleFactorRoundingPolicy|beep|alert|setActiveWindow|setStartDragTime|"
                       r"setStartDragDistance|setDoubleClickInterval|setKeyboardInputInterval|setWheelScrollLines|"
                       r"setCursorFlashTime|setEffectEnabled|setNavigationMode|setClipboard|setText|setImage|"
                       r"setPixmap|setMimeData|clear|showMessage|play|record|sync|flush|setValue|beginGroup|"
                       r"connectToHost\w*|bind|listen|abort|ignoreSslErrors|setProxy|lookupHost|setDefaultFactory|"
                       r"setApplicationProxy|setUserAgent|post|put|get|head|deleteResource|sendCustomRequest|"
                       r"connectToBackend|enableTracking|addDatabase|removeDatabase|cloneDatabase|registerSqlDriver|"
                       r"database|setDatabaseName|exec|prepare|setQuery|select|submitAll|setStandardLocation|"
                       r"setTestModeEnabled|setMessagePattern|setFilterRules|installMessageHandler|setPath|"
                       r"setUncaughtExceptionHandler|reportError|exit|quit|setUnixSignalMask|attach|lock|unlock|acquire|create|detach|"
                       # starts a camera or a screen or window capture (Qt grabs windows from a thread of its own)
                       r"setActive|"
                       # a disk cache owns its directory: expire() and clear() delete every *.d file under it, and
                       # setMaximumCacheSize expires (a cache directory of "/" deleted system files)
                       r"setCacheDirectory|setMaximumCacheSize|insert|updateMetaData|expire|"
                       r"setOfflineStoragePath|setWorkingDirectory)$")

PRIMITIVES = {
    "bool": ("bool", "PK_BOOL"),
    "char": ("char", "PK_INT"), "signed char": ("signed char", "PK_INT"), "unsigned char": ("unsigned char", "PK_UINT"),
    "short": ("short", "PK_INT"), "unsigned short": ("unsigned short", "PK_UINT"),
    "int": ("int", "PK_INT"), "unsigned int": ("unsigned int", "PK_UINT"),
    "long": ("long", "PK_INT"), "unsigned long": ("unsigned long", "PK_UINT"),
    "long long": ("long long", "PK_INT"), "unsigned long long": ("unsigned long long", "PK_UINT"),
    "char16_t": ("char16_t", "PK_UINT"), "char32_t": ("char32_t", "PK_UINT"),
    "float": ("float", "PK_DOUBLE"), "double": ("double", "PK_DOUBLE"),
}
STRINGS = {"QString": "PK_STR", "QByteArray": "PK_BYTES", "QChar": "PK_CHAR", "QStringView": "PK_STR",
           "QAnyStringView": "PK_STR", "QLatin1String": "PK_STR", "QLatin1StringView": "PK_STR",
           "QBasicUtf8StringView<false>": "PK_BYTES", "QByteArrayView": "PK_BYTES"}
BAD_TYPE_PARTS = ("std::", "QSharedPointer", "QWeakPointer", "QScopedPointer", "QFuture", "QPromise", "QPointer<",
                  "QSpan", "QFunctionPointer", "QtPrivate", "Private", "QAtomic", "QMutex", "QReadWriteLock",
                  "QSemaphore", "QThreadStorage", "Iterator", "iterator", "QKeyValue", "QStringBuilder", "QArrayData",
                  "QStringTokenizer", "QDataStream", "QTextStream", "QDebug", "(anonymous", "__", "QPropertyBinding",
                  "QBindable", "QProperty", "QUntypedBindable", "QMetaObject::Connection", "QJSManagedValue",
                  "QQmlListProperty", "QPair<", "QStringDecoder", "QStringEncoder", "QStringConverter",
                  "QNativeInterface", "QPlatform", "QRhi", "QSG", "QVulkan", "QFlags<QFlags", "QTypedArrayData",
                  "QtMetaContainer", "QMetaSequence", "QMetaAssociation", "QPermission", "QJniObject",
                  "QVariantConstPointer", "QVariantPointer", "QVariantRef", "QAccessibleInterface", "QStyleOption",
                  "QOpenGLFunctions", "QAbstractOpenGLFunctions", "QChronoTimer::", "QMetaType::", "QRunnable",
                  "QDeadlineTimer::ForeverConstant", "QTimeZone::Initialization", "QDateTime::TransitionResolution",
                  "QPaintEngine", "QPaintDevice", "QPainter", "QGraphicsItem", "QLayoutItem", "QStandardItem",
                  "QListWidgetItem", "QTreeWidgetItem", "QTableWidgetItem", "QUndoCommand", "QTextBlockUserData",
                  "QTextLayout", "QTextLine", "QTextEngine", "QGlyphRun", "QRawFont", "QIconEngine", "QImageIOHandler",
                  "QAbstractVideoBuffer", "QHttpPart", "QMetaMethod", "QMetaProperty", "QMetaEnum", "QMetaClassInfo",
                  "QGenericArgument", "QGenericReturnArgument")


def vt(s):
    return tuple(int(x) for x in s.split(".")[:3])


def split_targs(s):
    """'QMap<QString, QList<int>>' -> ('QMap', ['QString', 'QList<int>'])"""
    if "<" not in s or not s.endswith(">"):
        return s, []
    i = s.index("<")
    name, inner = s[:i], s[i + 1:-1]
    out, depth, cur = [], 0, ""
    for ch in inner:
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return name, out


def strip_cv_ref(s):
    s = s.strip()
    if s.endswith("&&"):
        return None
    if s.endswith("&"):
        s = s[:-1].strip()
    if s.startswith("const "):
        s = s[6:].strip()
    return s


def norm_type(s):
    """Spelling as Qt's normalizedType would write it."""
    s = s.strip()
    if s.endswith("&"):
        s = s[:-1].strip()
    if s.startswith("const ") and not s.endswith("*"):
        s = s[6:].strip()
    s = re.sub(r"\s*\*", "*", s)
    s = re.sub(r"\s*,\s*", ",", s)
    s = re.sub(r"<\s*", "<", s)
    s = re.sub(r"\s*>", ">", s)
    s = s.replace("unsigned int", "uint").replace("unsigned short", "ushort").replace("unsigned long long", "qulonglong")
    s = s.replace("long long", "qlonglong").replace("unsigned long", "ulong").replace("unsigned char", "uchar")
    return s


class Ctx:
    def __init__(self, model):
        self.model = model
        self.classes = model["classes"]
        self.enums = model["enums"]
        self.qobject_memo = {}
        self.enum_ids = {}
        self.flag_enums = set()
        self.skip = collections.Counter()
        self.skip_examples = collections.defaultdict(list)

    def is_qobject(self, name):
        if name in self.qobject_memo:
            return self.qobject_memo[name]
        self.qobject_memo[name] = False
        if name == "QObject":
            self.qobject_memo[name] = True
            return True
        c = self.classes.get(name)
        r = False
        if c:
            for b in c["bases"]:
                b = b.replace("class ", "").replace("struct ", "").strip()
                if self.is_qobject(b):
                    r = True
                    break
        self.qobject_memo[name] = r
        return r

    def copyable(self, name):
        c = self.classes.get(name)
        if c is None:
            return True
        if self.is_qobject(name) or c.get("abstract") or c.get("copy_nonpublic") or c.get("dtor") == "nonpublic":
            return False
        for k in c["ctors"]:
            if k.get("copy") and k.get("deleted"):
                return False
        return True

    def enum_id(self, name):
        return self.enum_ids.get(name, -1)


class PT:
    """A mapped parameter type."""
    __slots__ = ("cpp", "pk", "conv", "eid", "meta", "tname", "sig")

    def __init__(self, cpp, pk, conv="cv", eid=-1, meta=None, tname=None, sig=None):
        self.cpp, self.pk, self.conv, self.eid, self.meta, self.tname, self.sig = cpp, pk, conv, eid, meta, tname, sig


# parts match anywhere in a name (QStyleOption covers QStyleOptionViewItem), except these, which
# name one class: QPainter is not QPainterPath
WHOLE_NAME_PARTS = {"QPainter"}
_BAD_TYPE_RE = re.compile("|".join(re.escape(p) + (r"(?![A-Za-z0-9_])" if p in WHOLE_NAME_PARTS else "")
                                   for p in BAD_TYPE_PARTS))


def bad_type(name):
    return _BAD_TYPE_RE.search(name) is not None


def map_value(ctx, name, depth=0):
    """Map a by-value / const-reference type name (canonical, no cv/ref)."""
    if depth > 3:
        return None, "nested too deeply"
    if name in PRIMITIVES:
        cpp, pk = PRIMITIVES[name]
        return PT(cpp, pk, tname=name), None
    if name.endswith("*"):
        base = name[:-1].strip()
        const = base.startswith("const ")
        b = base[6:].strip() if const else base
        if b in ("char",) and const:
            return PT("const char *", "PK_BYTES", tname="const char*"), None
        if b in PRIMITIVES and not const:
            return PT(PRIMITIVES[b][0] + " *", "PK_NULLPTR", tname=b + "*"), None
        if b in ctx.classes and ctx.is_qobject(b):
            if not ctx.classes[b].get("has_meta"):
                return None, "QObject subclass without Q_OBJECT"
            return PT(("const " if const else "") + b + " *", "PK_OBJ", meta=b, tname=b + "*"), None
        return None, "raw pointer to a non-QObject type"
    if name in ctx.enums:
        eid = ctx.enum_id(name)
        if eid < 0:
            return None, "unknown enum"
        return PT(name, "PK_ENUM", conv="cve", eid=eid, tname=name), None
    if bad_type(name):
        return None, "type not bindable safely"
    base, targs = split_targs(name)
    if base == "QFlags" and len(targs) == 1:
        e = targs[0]
        eid = ctx.enum_id(e)
        if eid < 0:
            return None, "flags of an unknown enum"
        return PT(name, "PK_FLAGS", conv="cvf", eid=eid, tname=name), None
    if name in STRINGS:
        return PT(name, STRINGS[name], tname=name), None
    if name == "QVariant":
        return PT(name, "PK_VARIANT", tname=name), None
    if name in ("QModelIndex", "QPersistentModelIndex"):
        return PT(name, "PK_INDEX", tname=name), None
    if base == "QList" and len(targs) == 1:
        el, why = map_value(ctx, targs[0], depth + 1)
        if el is None or el.pk == "PK_NULLPTR":
            return None, "list of %s" % (why or "pointers")
        if el.pk in ("PK_LIST", "PK_MAP") and depth > 0:
            return None, "nested containers"
        return PT(name, "PK_LIST", tname=name), None
    if name in ("QMap<QString, QVariant>", "QHash<QString, QVariant>"):
        return PT(name, "PK_MAP", tname=name), None
    if base in ("QMap", "QHash", "QMultiMap", "QMultiHash", "QSet", "QVarLengthArray", "QQueue", "QStack"):
        for t in targs:
            el, why = map_value(ctx, t, depth + 1)
            if el is None or el.pk in ("PK_NULLPTR", "PK_OBJ"):
                return None, "container of %s" % (why or "objects")
        if base == "QVarLengthArray":
            return None, "QVarLengthArray"
        return PT(name, "PK_VALUE", tname=name), None
    if targs:
        return None, "template %s" % base
    if not name.startswith("Q"):
        return None, "non-Qt type"
    c = ctx.classes.get(name)
    if c is None:
        return None, "unknown class %s" % name
    if ctx.is_qobject(name):
        return None, "QObject by value"
    if not ctx.copyable(name):
        return None, "non-copyable value type"
    return PT(name, "PK_VALUE", tname=name), None


# iterators that are plain pointers underneath (QByteArray::const_iterator is const char *) look
# like buffers once canonical: the spelling tells them apart
ITERATOR_SPELLING = re.compile(r"\b(const_|reverse_|const_reverse_)?iterator\b|Iterator\b")


def map_param(ctx, ti):
    k = ti["kind"]
    if ITERATOR_SPELLING.search(ti.get("spelling", "")):
        return None, "type not bindable safely"
    if k == "RVALUEREFERENCE":
        return None, "rvalue reference"
    if k == "LVALUEREFERENCE":
        pt = ti["pointee"]
        if not pt.get("const"):
            return None, "non-const reference (out parameter)"
        name = strip_cv_ref(ti["canon"])
        if name is None:
            return None, "rvalue reference"
        return map_value(ctx, name)
    if k in ("FUNCTIONPROTO", "MEMBERPOINTER", "CONSTANTARRAY", "INCOMPLETEARRAY", "BLOCKPOINTER", "VARIABLEARRAY"):
        return None, k.lower()
    name = ti["canon"]
    if name.startswith("const ") and not name.endswith("*"):
        name = name[6:]
    return map_value(ctx, name)


RET_KIND = {"PK_BOOL": "QTC_T_BOOL", "PK_INT": "QTC_T_INT", "PK_UINT": "QTC_T_UINT", "PK_DOUBLE": "QTC_T_DOUBLE",
            "PK_ENUM": "QTC_T_INT", "PK_FLAGS": "QTC_T_INT", "PK_STR": "QTC_T_STR", "PK_BYTES": "QTC_T_BYTES",
            "PK_CHAR": "QTC_T_STR", "PK_OBJ": "QTC_T_OBJ", "PK_VARIANT": "QTC_T_VALUE", "PK_VALUE": "QTC_T_VALUE",
            "PK_LIST": "QTC_T_LIST", "PK_MAP": "QTC_T_MAP", "PK_INDEX": "QTC_T_VALUE"}


def map_ret(ctx, ti):
    k = ti["kind"]
    if ITERATOR_SPELLING.search(ti.get("spelling", "")):
        return None, "returns type not bindable safely"
    if k == "VOID":
        return ("QTC_T_NONE", "void", "none"), None
    if k == "RVALUEREFERENCE":
        return None, "returns an rvalue reference"
    if k == "LVALUEREFERENCE":
        name = ti["canon"].rstrip("&").strip()
        if name.startswith("const "):
            name = name[6:].strip()
        if name in ctx.classes and ctx.is_qobject(name):
            return None, "returns a QObject by reference"
    else:
        name = ti["canon"]
        if name.startswith("const ") and not name.endswith("*"):
            name = name[6:]
    if k == "POINTER" or name.endswith("*"):
        base = name[:-1].strip()
        if base.startswith("const "):
            base = base[6:].strip()
        if base == "char":
            return ("QTC_T_STR", "const char*", "str"), None
        if base in ctx.classes and ctx.is_qobject(base):
            return ("QTC_T_OBJ", base + "*", "obj"), None
        return None, "returns a raw pointer"
    if k == "ENUM" or name in ctx.enums:
        return ("QTC_T_INT", name, "int"), None
    if name in PRIMITIVES:
        pk = PRIMITIVES[name][1]
        return (RET_KIND[pk], name, pk_short(pk)), None
    if name.startswith("QFlags<"):
        return ("QTC_T_INT", name, "int"), None
    pt, why = map_value(ctx, name)
    if pt is None:
        return None, "returns " + why
    if pt.pk == "PK_NULLPTR":
        return None, "returns a pointer"
    return (RET_KIND[pt.pk], name, pk_short(pt.pk)), None


def pk_short(pk):
    return {"PK_BOOL": "bool", "PK_INT": "int", "PK_UINT": "uint", "PK_DOUBLE": "double", "PK_ENUM": "int",
            "PK_FLAGS": "int", "PK_STR": "str", "PK_BYTES": "bytes", "PK_CHAR": "str", "PK_OBJ": "obj",
            "PK_VARIANT": "variant", "PK_VALUE": "value", "PK_LIST": "list", "PK_MAP": "map", "PK_INDEX": "value",
            "PK_NULLPTR": "null", "PK_OUT": "out"}[pk]


def sig_type(ctx, ti, owner):
    """The parameter's type as a signature would spell it, qualified like moc does."""
    s = norm_type(ti["spelling"])
    core = s.rstrip("*")
    if "::" not in core and "<" not in core:
        q = owner + "::" + core
        if q in ctx.enums or q in ctx.classes:
            return owner + "::" + s
        canon = norm_type(ti["canon"])
        if canon.startswith("QFlags<" + owner + "::"):
            return owner + "::" + s
    return s


# ------------------------------------------------------------------ versions

class Versions:
    def __init__(self, models):
        self.order = sorted(models, key=lambda m: vt(m["qt_version"]))
        self.sets = []
        for m in self.order:
            cls, meth, ctor = set(), set(), set()
            for cn, c in m["classes"].items():
                cls.add(cn)
                for x in c["methods"]:
                    meth.add(self.mkey(cn, x))
                for k in c["ctors"]:
                    ctor.add(self.ckey(cn, k))
            self.sets.append((vt(m["qt_version"]), set(m["modules"]), cls, meth, ctor))

    @staticmethod
    def mkey(cn, x):
        return "%s|%s|%s|%d|%d" % (cn, x["name"], ",".join(p["type"]["canon"] for p in x["params"]), x["const"], x["static"])

    @staticmethod
    def ckey(cn, k):
        return "%s|%s" % (cn, ",".join(p["type"]["canon"] for p in k["params"]))

    def since(self, module, what, key):
        idx = {"class": 2, "method": 3, "ctor": 4}[what]
        for s in self.sets:
            if module not in s[1]:
                continue
            if key in s[idx]:
                return s[0]
        return self.sets[-1][0]


def guard(since, floor):
    if since[:2] <= floor[:2]:
        return None
    return "QT_VERSION >= QT_VERSION_CHECK(%d, %d, 0)" % (since[0], since[1])


# Qt API that its headers declare on some systems only: the models are read from Linux's
# headers, so a method naming one of these types is built only where the type exists.
PLATFORM_ONLY = [("QProcess::UnixProcess", "defined(Q_OS_UNIX)")]


def platform_guard(m):
    names = [p["type"]["canon"] for p in m["params"]] + [(m.get("ret") or {}).get("canon", "")]
    conds = sorted({cond for prefix, cond in PLATFORM_ONLY for n in names if prefix in n})
    return " && ".join(conds) or None


# ------------------------------------------------------------------ emission

def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


class Emitter:
    def __init__(self, ctx, versions, floor, include_root, header_configs=None):
        self.ctx = ctx
        self.v = versions
        self.floor = floor
        self.include_root = include_root
        self.header_configs = header_configs   # recorded by scan_qt.py; else read from the headers
        self.tid = 0
        self.npre = 0
        self.manifest_classes = []
        self.cov = collections.defaultdict(lambda: collections.Counter())
        self.cfg_cache = {}

    def header_config(self, header):
        if header in self.cfg_cache:
            return self.cfg_cache[header]
        if self.header_configs is not None:
            cfg = list(self.header_configs.get(header, []))
            self.cfg_cache[header] = cfg
            return cfg
        cfg = []
        try:
            with open(os.path.join(self.include_root, header), errors="replace") as f:
                for line in f:
                    m = re.match(r"\s*QT_REQUIRE_CONFIG\((\w+)\)", line)
                    if m:
                        cfg.append(m.group(1))
        except OSError:
            pass
        self.cfg_cache[header] = cfg
        return cfg

    def method_plan(self, cn, c, m, is_obj, is_ctor=False):
        """Returns (plan dict, None) or (None, reason)."""
        ctx = self.ctx
        if not is_ctor:
            name = m["name"]
            if name.startswith("operator") or name.startswith("~") or name.startswith("qt_") or name.startswith("_q_"):
                return None, "operator or internal"
            if name in DENY_METHOD or (cn, name) in DENY_PAIRS:
                return None, "denied"
            if m.get("deleted"):
                return None, "deleted"
            if m.get("variadic"):
                return None, "variadic"
            if not m["static"] and not is_obj and not ctx.copyable(cn):
                return None, "instance method of a non-copyable value class"
            r, why = map_ret(ctx, m["ret"])
            if r is None:
                return None, why
        else:
            if m.get("deleted") or m.get("copy") or m.get("move"):
                return None, "copy/move constructor"
            if (cn, ",".join(p["type"]["canon"] for p in m["params"])) in DENY_CTORS or cn in DENY_CTOR_CLASSES:
                return None, "denied"
            r = ("QTC_T_OBJ", cn + "*", "obj") if is_obj else ("QTC_T_VALUE", cn, "value")
        # A string naming a slot makes Qt call any method of the receiver by name, with whatever
        # arguments a signal carries: around every check qtc makes. Signals reach the program through
        # qtc_connect instead (and QTimer, QShortcut, dialogs and menus all have signals for it).
        for p in m["params"]:
            if p["type"]["canon"] == "const char *" and p["name"] in MEMBER_PARAMS:
                return None, "slot named by a string"
        params = []
        maxa = len(m["params"])
        mina = sum(1 for p in m["params"] if not p["default"])
        for i, p in enumerate(m["params"]):
            pt, why = map_param(ctx, p["type"])
            if pt is None:
                if i < mina:
                    return None, why
                maxa = i
                break
            if pt.pk == "PK_NULLPTR":
                # a primitive pointer: an out-value (bool *ok, int *x) gets storage; a buffer stays null-only
                base = pt.tname[:-1]
                if base in ("char", "signed char", "unsigned char") or p["name"] in RAW_POINTER_PARAMS:
                    if i < mina:
                        return None, "required raw buffer pointer"
                else:
                    pt = PT("qtc::Out<%s>" % PRIMITIVES[base][0], "PK_OUT", tname=pt.tname)
            pt.sig = sig_type(ctx, p["type"], cn)
            params.append((p["name"], pt))
        params = params[:maxa]
        if maxa < mina:
            return None, "required parameter not bindable"
        return {"params": params, "min": mina, "max": maxa, "ret": r}, None

    def thunk(self, cn, c, m, plan, kind, out):
        """Emit one thunk template. kind: 'obj', 'val', 'static', 'ctor_obj', 'ctor_val'."""
        self.tid += 1
        tid = self.tid
        params = plan["params"]
        name = m.get("name", cn)
        rk = plan["ret"][0]
        lines = ["template <class S> static int t%d(void *q, qtc::Arg *a, int n, qtc::Ret *r) {" % tid]
        decl_holders = []
        for i, (pn, pt) in enumerate(params):
            decl_holders.append("qtc::H<%s> p%d;" % (pt.cpp, i))
        cases = []
        any_case = False
        for k in range(plan["min"], plan["max"] + 1):
            argl = ", ".join("p%d.get()" % i for i in range(k))
            if kind == "obj":
                call = "s->%s(%s)" % (name, argl)
            elif kind == "val":
                call = "s->%s(%s)" % (name, argl)
            elif kind == "static":
                call = "S::%s(%s)" % (name, argl)
            elif kind == "ctor_obj":
                call = "qtc::make_obj<S>(%s)" % argl   # a Tracked<S> (qtc_internal.h)
            elif kind == "ctor_ov":
                call = "new S(%s)" % argl              # an overridable subclass: its own bookkeeping
            else:
                call = "S(%s)" % argl
            if kind in ("obj", "val"):
                rp = "S *s"
            else:
                rp = None
            req_params = ([rp] if rp else []) + ["qtc::Ret *rr", "qtc::Arg *aa"] + ["qtc::H<%s> &p%d" % (params[i][1].cpp, i) for i in range(k)]
            conv_req = []
            for i in range(k):
                pt = params[i][1]
                if pt.conv == "cv":
                    conv_req.append("qtc::cv(aa[%d], p%d, %d)" % (i, i, i))
                elif pt.conv == "cve":
                    conv_req.append("qtc::cve(aa[%d], p%d, %d, %d)" % (i, i, i, pt.eid))
                else:
                    conv_req.append("qtc::cvf(aa[%d], p%d, %d, %d)" % (i, i, i, pt.eid))
            outs = "".join(" r->out(p%d.v);" % i for i in range(k) if params[i][1].pk == "PK_OUT")
            if kind in ("ctor_obj", "ctor_ov"):
                expr = "rr->set_obj(%s)" % call
                body_call = "r->set_obj(%s);%s return 0;" % (call, outs)
            elif kind == "ctor_val":
                expr = "qtc::rt(*rr, %s)" % call
                body_call = "qtc::rt(*r, %s);%s return 0;" % (call, outs)
            elif rk == "QTC_T_NONE":
                expr = call
                body_call = "%s; r->set_none();%s return 0;" % (call, outs)
            else:
                expr = "qtc::rt(*rr, %s)" % call
                body_call = "qtc::rt(*r, %s);%s return 0;" % (call, outs)
            req = "requires(%s) { %s; }" % (", ".join(req_params), "; ".join(conv_req + [expr]))
            cases.append("        case %d:\n            if constexpr (%s) { %s }\n            else return QTC_ERR_UNSUPPORTED;" % (k, req, body_call))
            any_case = True
        if not any_case:
            return None
        if kind in ("obj",):
            lines.append("    S *s = qtc::self_obj<S>(q);")
        elif kind == "val":
            lines.append("    S *s = qtc::self_val<S>(q);")
        else:
            lines.append("    (void)q;")
        for d in decl_holders:
            lines.append("    " + d)
        for i, (pn, pt) in enumerate(params):
            cond = "" if i < plan["min"] else "if (n > %d) " % i
            if pt.conv == "cv":
                lines.append("    %sQTC_CV(%d, p%d);" % (cond, i, i))
            elif pt.conv == "cve":
                lines.append("    %sQTC_CVE(%d, p%d, %d);" % (cond, i, i, pt.eid))
            else:
                lines.append("    %sQTC_CVF(%d, p%d, %d);" % (cond, i, i, pt.eid))
        for cond, mx, code, msg in preconditions(self.ctx, cn, m, plan, kind):
            guard_n = "n > %d && " % mx if mx >= plan["min"] else ""
            lines.append("    if (%s!(%s)) return qtc::pre_fail(%s, %s);" % (guard_n, cond, code, cstr(msg)))
            self.npre += 1
        lines.append("    switch (n) {")
        lines.extend(cases)
        lines.append("        default: break;")
        lines.append("    }")
        if kind in ("obj", "val"):
            lines.append("    (void)s;")
        lines.append("    (void)a; (void)r;")
        lines.append("    return QTC_ERR_BAD_ARGS;")
        lines.append("}")
        out.append("\n".join(l for l in lines if l is not None))
        return tid

    def param_desc(self, plan):
        items = []
        for pn, pt in plan["params"]:
            meta = "&%s::staticMetaObject" % pt.meta if pt.meta else "nullptr"
            items.append("{qtc::%s, %d, %s, %s}" % (pt.pk, pt.eid, cstr(pt.tname or pt.cpp), meta))
        return items

    def flags_of(self, cn, m, is_ctor):
        f = []
        if not is_ctor:
            if m["static"]:
                f.append("qtc::MF_STATIC")
            if m["const"]:
                f.append("qtc::MF_CONST")
            if m.get("deprecated"):
                f.append("qtc::MF_DEPRECATED")
            if BLOCKING_RE.match(m["name"]):
                f.append("qtc::MF_BLOCKING")
            if NOFUZZ_ANY_RE.match(m["name"]) or (NOFUZZ_CLASS_RE.match(cn) and NOFUZZ_RE.match(m["name"])):
                f.append("qtc::MF_NOFUZZ")
        return "|".join(f) if f else "0"

    def emit_class(self, cn, c, out_thunks, out_tables, out_classdefs):
        ctx = self.ctx
        module = c["module"]
        is_obj = ctx.is_qobject(cn)
        if is_obj and not c.get("has_meta"):
            self.cov[module]["classes_skipped"] += 1
            return
        has_instance_receiver = is_obj or ctx.copyable(cn)
        csince = self.v.since(module, "class", cn)
        cguard = guard(csince, self.floor)
        meth_entries, ctor_entries = [], []
        man_methods, man_ctors = [], []
        thunk_code = []
        for m in c["methods"]:
            if m.get("access", "public") != "public":
                continue   # protected: callable only on overridable objects (see emit_overridable)
            self.cov[module]["methods_seen"] += 1
            plan, why = self.method_plan(cn, c, m, is_obj)
            if plan is None:
                self.cov[module]["skip:" + why] += 1
                ctx.skip[why] += 1
                # examples worth showing: not the metaObject()/tr() every QObject class declares
                if len(ctx.skip_examples[why]) < 8 and m["name"] not in ("metaObject", "tr", "qt_metacast", "qt_metacall",
                                                                          "staticMetaObject", "trUtf8"):
                    ctx.skip_examples[why].append("%s::%s" % (cn, m["name"]))
                continue
            if not m["static"] and not has_instance_receiver:
                self.cov[module]["skip:no receiver"] += 1
                continue
            kind = "static" if m["static"] else ("obj" if is_obj else "val")
            msince = self.v.since(module, "method", Versions.mkey(cn, m))
            mg = guard(msince, csince if cguard else self.floor)
            mg = " && ".join(g for g in (mg, platform_guard(m)) if g) or None
            # the thunk names the method's types: an older Qt may not have them, nor another system
            start = len(thunk_code)
            tid = self.thunk(cn, c, m, plan, kind, thunk_code)
            if tid is None:
                del thunk_code[start:]
                continue
            if mg:
                thunk_code.insert(start, "#if " + mg)
                thunk_code.append("#endif")
            pdesc = self.param_desc(plan)
            parr = "P%d" % tid
            sig = "%s(%s)" % (m["name"], ",".join(pt.sig for _, pt in plan["params"]))
            entry = []
            if mg:
                entry.append("#if " + mg)
            if pdesc:
                out_tables.append(("#if %s\n" % mg if mg else "") + "static const qtc::ParamDesc %s[] = {%s};" % (parr, ", ".join(pdesc)) + ("\n#endif" if mg else ""))
            entry.append('    {%s, %s, t%d<%s>, %s, %d, %d, %s, %s, %s},' % (
                cstr(m["name"]), cstr(sig), tid, cn, parr if pdesc else "nullptr", plan["max"], plan["min"],
                self.flags_of(cn, m, False), plan["ret"][0], cstr(plan["ret"][1])))
            if mg:
                entry.append("#endif")
            meth_entries.append("\n".join(entry))
            self.cov[module]["methods_bound"] += 1
            man_methods.append({"name": m["name"], "sig": sig, "static": m["static"], "const": m["const"],
                                "min": plan["min"], "params": [{"name": pn, "pk": pk_short(pt.pk), "type": pt.sig}
                                                               for pn, pt in plan["params"]],
                                "ret": plan["ret"][2], "ret_type": plan["ret"][1],
                                "since": "%d.%d" % msince[:2], "deprecated": bool(m.get("deprecated")),
                                "blocking": bool(BLOCKING_RE.match(m["name"]))})
        can_create = not c.get("abstract") and (c.get("dtor") != "nonpublic")
        if can_create and (is_obj or ctx.copyable(cn)):
            for k in c["ctors"]:
                plan, why = self.method_plan(cn, c, k, is_obj, is_ctor=True)
                if plan is None:
                    continue
                kind = "ctor_obj" if is_obj else "ctor_val"
                ksince = self.v.since(module, "ctor", Versions.ckey(cn, k))
                kg = guard(ksince, csince if cguard else self.floor)
                start = len(thunk_code)
                tid = self.thunk(cn, c, k, plan, kind, thunk_code)
                if tid is None:
                    del thunk_code[start:]
                    continue
                if kg:
                    thunk_code.insert(start, "#if " + kg)
                    thunk_code.append("#endif")
                pdesc = self.param_desc(plan)
                parr = "P%d" % tid
                sig = "%s(%s)" % (cn.split("::")[-1], ",".join(pt.sig for _, pt in plan["params"]))
                if pdesc:
                    out_tables.append(("#if %s\n" % kg if kg else "") + "static const qtc::ParamDesc %s[] = {%s};" % (parr, ", ".join(pdesc)) + ("\n#endif" if kg else ""))
                entry = []
                if kg:
                    entry.append("#if " + kg)
                entry.append('    {%s, %s, t%d<%s>, %s, %d, %d, %s, %s, %s},' % (
                    cstr(cn), cstr(sig), tid, cn, parr if pdesc else "nullptr", plan["max"], plan["min"],
                    "qtc::MF_NOFUZZ" if cn in NOFUZZ_CTOR_CLASSES else "0", plan["ret"][0], cstr(cn)))
                if kg:
                    entry.append("#endif")
                ctor_entries.append("\n".join(entry))
                self.cov[module]["ctors_bound"] += 1
                man_ctors.append({"sig": sig, "min": plan["min"], "since": "%d.%d" % ksince[:2],
                                  "params": [{"name": pn, "pk": pk_short(pt.pk), "type": pt.sig} for pn, pt in plan["params"]]})
        if not meth_entries and not ctor_entries and not is_obj:
            self.cov[module]["classes_empty"] += 1
            return
        safe = re.sub(r"\W", "_", cn)
        cfg = self.header_config(c["header"])
        conds = []
        if cguard:
            conds.append(cguard)
        for x in cfg:
            conds.append("QT_CONFIG(%s)" % x)
        conds.append("__has_include(<%s>)" % c["header"])
        cond = " && ".join(conds)
        block = ["#if " + cond]
        block.extend(thunk_code)
        block.append("#endif")
        out_thunks.append("\n".join(block))
        tab = ["#if " + cond]
        tab.append("static const qtc::MethodDef M_%s[] = {" % safe)
        tab.extend(meth_entries)
        tab.append("    {nullptr, nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr}};")
        tab.append("static const qtc::MethodDef C_%s[] = {" % safe)
        tab.extend(ctor_entries)
        tab.append("    {nullptr, nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr}};")
        bases = []
        if not is_obj:
            for b in c["bases"]:
                b = b.replace("class ", "").strip()
                bases.append(b)
            if bases:
                # a base an older Qt does not have (QOperatingSystemVersionUnexported) is left out there
                bguards = [guard(self.v.since(module, "class", b), csince if cguard else self.floor) for b in bases]

                def base_tables(keep):
                    bs = [b for b, k in zip(bases, keep) if k]
                    t = ["static const char *const B_%s[] = {%s};" % (safe, ", ".join([cstr(b) for b in bs] + ["nullptr"]))]
                    cases = " ".join("case %d: return static_cast<%s *>(static_cast<%s *>(p));" % (i, b, cn) for i, b in enumerate(bs))
                    t.append("static void *U_%s(void *p, int b) { switch (b) { %s default: return p; } }" % (safe, cases))
                    return t
                conds = sorted({g for g in bguards if g})
                if conds:
                    tab.append("#if " + " && ".join(conds))
                    tab.extend(base_tables([True] * len(bases)))
                    tab.append("#else")
                    tab.extend(base_tables([g is None for g in bguards]))
                    tab.append("#endif")
                else:
                    tab.extend(base_tables([True] * len(bases)))
        tab.append("#endif")
        out_tables.append("\n".join(tab))
        need = "qtc::NEED_CORE"
        if module in NEED_WIDGETS:
            need = "qtc::NEED_WIDGETS"
        elif module in NEED_GUI and cn not in CORE_SAFE_GUI:
            need = "qtc::NEED_GUI"
        if is_obj:
            meta = "&%s::staticMetaObject" % cn
            mt = "nullptr"
        else:
            meta = "nullptr"
            mt = "&qtc::metatype_of<%s>" % cn if ctx.copyable(cn) else "nullptr"
        cd = ["#if " + cond,
              '    {%s, %s, %s, %s, %d, %s, %s, %s, %s, M_%s, sizeof(M_%s) / sizeof(M_%s[0]) - 1, C_%s, sizeof(C_%s) / sizeof(C_%s[0]) - 1},' % (
                  cstr(cn), cstr(module), "qtc::CK_OBJECT" if is_obj else "qtc::CK_VALUE", need,
                  1 if c.get("abstract") else 0, meta, mt, ("B_%s" % safe) if bases else "nullptr",
                  ("U_%s" % safe) if bases else "nullptr", safe, safe, safe, safe, safe, safe),
              "#endif"]
        out_classdefs.append("\n".join(cd))
        self.cov[module]["classes_bound"] += 1
        self.manifest_classes.append({"name": cn, "module": module, "kind": "object" if is_obj else "value",
                                      "bases": [b.replace("class ", "") for b in c["bases"]],
                                      "abstract": bool(c.get("abstract")), "since": "%d.%d" % csince[:2],
                                      "methods": man_methods, "ctors": man_ctors})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", required=True, help="directory with qt-*.json models")
    ap.add_argument("--out", required=True, help="the qtc source tree")
    ap.add_argument("--chunk", type=int, default=900, help="thunks per generated file")
    args = ap.parse_args()

    paths = sorted(glob.glob(os.path.join(args.models, "qt-*.json")) + glob.glob(os.path.join(args.models, "qt-*.json.xz")))
    models = [json.load(lzma.open(p, "rt") if p.endswith(".xz") else open(p)) for p in paths]
    if not models:
        sys.exit("no qt-*.json or qt-*.json.xz models in " + args.models)
    models.sort(key=lambda m: vt(m["qt_version"]))
    primary = models[-1]
    floor = vt(models[0]["qt_version"])
    versions = Versions(models)
    ctx = Ctx(primary)

    # enums: ids, flag usage, validation data
    names = sorted(primary["enums"])
    for i, n in enumerate(names):
        ctx.enum_ids[n] = i
    for c in primary["classes"].values():
        for m in c["methods"] + [dict(k, ret={"canon": ""}) for k in c["ctors"]]:
            for t in [p["type"]["canon"] for p in m["params"]] + [m["ret"]["canon"]]:
                for mm in re.finditer(r"QFlags<([\w:]+)>", t):
                    ctx.flag_enums.add(mm.group(1))

    gen_dir = os.path.join(args.out, "src", "gen")
    man_dir = os.path.join(args.out, "manifest")
    os.makedirs(gen_dir, exist_ok=True)
    os.makedirs(man_dir, exist_ok=True)
    stale_files = set(glob.glob(os.path.join(gen_dir, "qtc_gen_*.cpp")) + glob.glob(os.path.join(gen_dir, "qtc_ov_*.cpp")))
    stale_files -= set(glob.glob(os.path.join(gen_dir, "qtc_gen_entr*.cpp")))   # gen/entries.py's

    em = Emitter(ctx, versions, floor, primary["include_root"], primary.get("header_config"))
    ove = overrides.OvEmitter(em, ctx, map_value, cstr, guard, Versions, NEED_WIDGETS, NEED_GUI)
    by_module = collections.defaultdict(list)
    for cn in sorted(primary["classes"]):
        c = primary["classes"][cn]
        if c["module"] not in MODULE_DEPS or DENY_CLASS_RE.match(cn):
            continue
        by_module[c["module"]].append(cn)

    cmake = ["# Generated by gen/generate.py. Do not edit."]
    reg_decls, reg_calls = [], []
    header = ["// Generated by gen/generate.py from the Qt %s headers. Do not edit." % primary["qt_version"],
              '#include "../qtc_internal.h"']
    pragmas = ["#if defined(__GNUC__)", "#pragma GCC diagnostic ignored \"-Wdeprecated-declarations\"",
               "#pragma GCC diagnostic ignored \"-Wunused-parameter\"", "#pragma GCC diagnostic ignored \"-Wunused-variable\"",
               "#pragma GCC diagnostic ignored \"-Wunused-but-set-variable\"", "#endif",
               "#if defined(_MSC_VER)", "#pragma warning(disable: 4996 4100 4189)", "#endif",
               "QT_WARNING_DISABLE_DEPRECATED"]
    for module in [m for m in MODULE_DEPS if m in by_module]:
        incs = []
        seen = set()

        def add_inc(mod):
            if mod in seen:
                return
            seen.add(mod)
            for d in MODULE_DEPS[mod]:
                add_inc(d)
            incs.append("#include <Qt%s/Qt%s>" % (mod, mod))
        add_inc(module)
        chunk, files, count = [], [], 0
        groups = [[]]
        for cn in by_module[module]:
            nm = len(primary["classes"][cn]["methods"]) + len(primary["classes"][cn]["ctors"])
            if count + nm > args.chunk and groups[-1]:
                groups.append([])
                count = 0
            groups[-1].append(cn)
            count += nm
        for gi, group in enumerate(groups):
            thunks, tables, classdefs = [], [], []
            for cn in group:
                em.emit_class(cn, primary["classes"][cn], thunks, tables, classdefs)
            fn = "qtc_gen_%s_%d.cpp" % (module, gi)
            fname = "qtc_gen_register_%s_%d" % (module, gi)
            body = header + pragmas[:6] + incs + pragmas[6:] + ["", "namespace {"] + thunks + tables + [
                "static const qtc::ClassDef CLASSES[] = {"] + classdefs + [
                "    {nullptr, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr, 0}};",
                "}  // namespace", "",
                "void %s() { qtc::register_classes(CLASSES, int(sizeof(CLASSES) / sizeof(CLASSES[0])) - 1); }" % fname, ""]
            write_if_changed(os.path.join(gen_dir, fn), "\n".join(body))
            stale_files.discard(os.path.join(gen_dir, fn))
            files.append(fn)
            reg_decls.append((module, "void %s();" % fname))
            reg_calls.append((module, "    %s();" % fname))
        # overridable subclasses of this module's classes (gen/overrides.py)
        ov_classes = [cn for cn in overrides.OVERRIDABLE
                      if cn in primary["classes"] and primary["classes"][cn]["module"] == module]
        if ov_classes:
            code, tables, defs = [], [], []
            for cn in ov_classes:
                ove.emit(cn, code, tables, defs)
            if defs:
                fn = "qtc_ov_%s.cpp" % module
                fname = "qtc_ov_register_%s" % module
                ov_pragmas = ["#if defined(__GNUC__)", "#pragma GCC diagnostic ignored \"-Wdeprecated-declarations\"",
                              "#pragma GCC diagnostic ignored \"-Wunused-parameter\"",
                              "#pragma GCC diagnostic ignored \"-Wunused-variable\"",
                              "#pragma GCC diagnostic ignored \"-Wunused-but-set-variable\"",
                              "#pragma GCC diagnostic ignored \"-Woverloaded-virtual\"", "#endif"]
                body = header + ['#include "../qtc_override.h"'] + ov_pragmas + incs + pragmas[6:] + ["", "namespace {"] + \
                    code + tables + ["static const qtc::OvClassDef OVCLASSES[] = {"] + defs + [
                    "    {nullptr, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr}};", "}  // namespace", "",
                    "void %s() { qtc::register_overridables(OVCLASSES, int(sizeof(OVCLASSES) / sizeof(OVCLASSES[0])) - 1); }" % fname,
                    ""]
                write_if_changed(os.path.join(gen_dir, fn), "\n".join(body))
                stale_files.discard(os.path.join(gen_dir, fn))
                files.append(fn)
                reg_decls.append((module, "void %s();" % fname))
                reg_calls.append((module, "    %s();" % fname))
        cmake.append("set(QTC_GEN_%s %s)" % (module, " ".join("${CMAKE_CURRENT_LIST_DIR}/" + f for f in files)))

    # enums: an older Qt lacks values added since, so each enum has one table per Qt version that
    # added values, and the build uses the one for the Qt it compiles against
    ev = ["// Generated by gen/generate.py. Do not edit.", '#include "../qtc_internal.h"', "", "namespace {"]
    entries = []
    value_since = collections.defaultdict(dict)   # enum -> key -> first Qt version that has it
    for m in models:
        for en, e in m["enums"].items():
            d = value_since[en]
            for k, v in e["values"]:
                d.setdefault(k, vt(m["qt_version"]))

    def lit(v):
        v = v if v < 2**63 else v - 2**64
        return "INT64_MIN" if v == -2**63 else "%dLL" % v

    def variant(n, e, vals):
        under = e["underlying"]
        lo, hi = {"int": (-2**31, 2**31 - 1), "unsigned int": (0, 2**32 - 1), "char": (-128, 127),
                  "signed char": (-128, 127), "unsigned char": (0, 255), "short": (-2**15, 2**15 - 1),
                  "unsigned short": (0, 2**16 - 1), "long": (-2**63, 2**63 - 1), "long long": (-2**63, 2**63 - 1),
                  "unsigned long": (0, 2**63 - 1), "unsigned long long": (0, 2**63 - 1), "bool": (0, 1),
                  "char16_t": (0, 2**16 - 1)}.get(under, (-2**31, 2**31 - 1))
        if not e.get("scoped") and vals:
            # without a fixed underlying type only the values of the smallest bit-field that holds
            # every enumerator exist ([dcl.enum]); anything else in the enum type is undefined
            emin, emax = min(v for k, v in vals), max(v for k, v in vals)
            if emax >= 2**63:
                emax -= 2**64
            if emin >= 2**63:
                emin -= 2**64
            need = max(abs(emin) - 1 if emin < 0 else 0, abs(emax))
            bmax = 0
            while bmax < need:
                bmax = bmax * 2 + 1
            bmin = 0 if emin >= 0 else -(bmax + 1)
            lo, hi = max(lo, bmin), min(hi, bmax)
        mask = 0
        if n in ctx.flag_enums:
            for k, v in vals:
                mask |= (v & 0xffffffffffffffff)
        # sentinels such as QPalette::NColorRoles are not valid arguments
        sent = {k for k, v in vals if re.match(r"^N[A-Z]\w*s$|^Num[A-Z]|Count$|^Max\w*Value$", k) and v == max(x[1] for x in vals) and v > 0}
        vals = [(k, v) for k, v in vals if k not in sent]
        if not vals:
            vals = [("", 0)]
        return vals, mask, lo, hi

    for i, n in enumerate(names):
        e = primary["enums"][n]
        vals = sorted(e["values"], key=lambda kv: kv[1])
        flags = (1 if n in ctx.flag_enums else 0) | (2 if n in OPEN_ENUMS else 0)
        since = value_since.get(n, {})
        first = min(since.values()) if since else floor
        # Qt versions that added values to an enum that existed before them
        steps = sorted({since.get(k, vt(primary["qt_version"]))[:2] for k, v in vals} - {first[:2]})
        steps = [st for st in steps if st > floor[:2]]
        groups = []   # (condition or None, values)
        for j in range(len(steps), -1, -1):
            upto = steps[j - 1] if j > 0 else None
            if upto is None:
                sub = [(k, v) for k, v in vals if since.get(k, (99, 99))[:2] <= max(first[:2], floor[:2])]
            else:
                sub = [(k, v) for k, v in vals if since.get(k, vt(primary["qt_version"]))[:2] <= upto]
            groups.append(("QT_VERSION >= QT_VERSION_CHECK(%d, %d, 0)" % upto if upto else None, sub or vals))
        for gi, (cond, sub) in enumerate(groups):
            if len(groups) > 1:
                ev.append(("#if " if gi == 0 else ("#elif " if cond else "#else")) + (cond or ""))
            vv, mask, lo, hi = variant(n, e, sub)
            ev.append("static const int64_t V%d[] = {%s};" % (i, ", ".join(lit(v) for k, v in vv)))
            ev.append("static const char *const K%d[] = {%s};" % (i, ", ".join(cstr(k) for k, v in vv)))
            ev.append("constexpr int64_t M%d = %s, L%d = %s, H%d = %s;" % (i, lit(mask), i, lit(lo), i, lit(hi)))
        if len(groups) > 1:
            ev.append("#endif")
        entries.append("    {%s, V%d, K%d, int32_t(sizeof(V%d) / sizeof(V%d[0])), %d, M%d, L%d, H%d}," % (cstr(n), i, i, i, i, flags, i, i, i))
    ev.append("static const qtc::EnumInfo ENUMS[] = {")
    ev.extend(entries)
    ev.append("};")
    ev.append("}  // namespace")
    ev.append("const qtc::EnumInfo *qtc::g_enums = ENUMS;")
    ev.append("int32_t qtc::g_enum_count = %d;" % len(names))
    write_if_changed(os.path.join(gen_dir, "qtc_gen_enums.cpp"), "\n".join(ev) + "\n")
    stale_files.discard(os.path.join(gen_dir, "qtc_gen_enums.cpp"))

    rg = ["// Generated by gen/generate.py. Do not edit.", '#include "../qtc_module.h"', ""]
    for module in CORE_MODULES:
        ds = [d for m, d in reg_decls if m == module]
        if ds:
            rg.append("#ifdef QTC_HAS_MODULE_%s" % module)
            rg.extend(ds)
            rg.append("#endif")
    rg.append("")
    rg.append("void qtc_gen_register_all() {")
    for module in CORE_MODULES:
        cs = [c for m, c in reg_calls if m == module]
        if cs:
            rg.append("#ifdef QTC_HAS_MODULE_%s" % module)
            rg.extend(cs)
            rg.append("#endif")
    rg.append("}")
    # the module libraries: what each needs, and which binds which class
    lib_modules = [m for m in MODULE_DEPS if m in by_module and m not in CORE_MODULES]
    rg += ["", "#ifdef QTC_STATIC_MODULES"]
    for m in lib_modules:
        rg += ["#ifdef QTC_HAS_MODULE_%s" % m, "QTC_MODULE_INIT(%s);" % m, "#define QTC_INIT_%s qtc_module_init_%s" % (m, m),
               "#else", "#define QTC_INIT_%s nullptr" % m, "#endif"]
    rg += ["#else"] + ["#define QTC_INIT_%s nullptr" % m for m in lib_modules] + ["#endif", ""]
    rg.append("const qtc::ModuleDef qtc::g_modules[] = {")
    for m in lib_modules:
        rg.append('    {"%s", "%s", QTC_INIT_%s},' % (m, " ".join(MODULE_DEPS[m]), m))
    rg.append("    {nullptr, nullptr, nullptr}};")
    rg.append("const int qtc::g_module_count = %d;" % len(lib_modules))
    mod_classes = []
    for i, m in enumerate(lib_modules):
        names_m = set(by_module[m])
        names_m.update(cn for cn in overrides.OVERRIDABLE if cn in primary["classes"] and primary["classes"][cn]["module"] == m)
        mod_classes += [(cn, i) for cn in names_m]
    mod_classes.sort(key=lambda x: x[0].encode())
    rg.append("const qtc::ModuleClass qtc::g_module_classes[] = {")
    for cn, i in mod_classes:
        rg.append('    {%s, %d},' % (cstr(cn), i))
    rg.append("    {nullptr, 0}};")
    rg.append("const int qtc::g_module_class_count = %d;" % len(mod_classes))
    write_if_changed(os.path.join(gen_dir, "qtc_gen_register.cpp"), "\n".join(rg) + "\n")
    stale_files.discard(os.path.join(gen_dir, "qtc_gen_register.cpp"))
    # each module library's init: its classes, its overridable classes, its part of the runtime (src/mod)
    for m in lib_modules:
        decls = [d for mm, d in reg_decls if mm == m]
        calls = [c for mm, c in reg_calls if mm == m]
        has_ext = os.path.exists(os.path.join(args.out, "src", "mod", "qtc_mod_%s.cpp" % m))
        body = ["// Generated by gen/generate.py. Do not edit.", '#include "../qtc_module.h"', ""] + decls
        if has_ext:
            body.append("QTC_MODULE_EXT(%s);" % m)
        body += ["", "QTC_MODULE_INIT(%s) {" % m] + calls
        if has_ext:
            body.append("    qtc_mod_ext_%s();" % m)
        body += ["}", ""]
        fn = "qtc_gen_mod_%s.cpp" % m
        write_if_changed(os.path.join(gen_dir, fn), "\n".join(body))
        stale_files.discard(os.path.join(gen_dir, fn))
        cmake.append("set(QTC_GEN_INIT_%s ${CMAKE_CURRENT_LIST_DIR}/%s)" % (m, fn))
    for f in stale_files:
        os.remove(f)
    cmake.append("set(QTC_GEN_COMMON ${CMAKE_CURRENT_LIST_DIR}/qtc_gen_enums.cpp ${CMAKE_CURRENT_LIST_DIR}/qtc_gen_register.cpp)")
    cmake.append("set(QTC_GEN_MODULES %s)" % " ".join(m for m in MODULE_DEPS if m in by_module))
    cmake.append("set(QTC_CORE_MODULES %s)" % " ".join(CORE_MODULES))
    for m in MODULE_DEPS:
        if m in by_module:
            cmake.append("set(QTC_GEN_DEPS_%s %s)" % (m, " ".join(MODULE_DEPS[m]) or '""'))

    enum_man = []
    for n in names:
        e = primary["enums"][n]
        enum_man.append({"name": n, "flags": n in ctx.flag_enums, "scoped": bool(e.get("scoped")), "values": e["values"]})
    manifest = {"qtc_version": "1.0.0", "qt_version": primary["qt_version"],
                "qt_versions_scanned": [m["qt_version"] for m in models],
                "classes": em.manifest_classes, "enums": enum_man, "overridables": ove.manifest}
    with open(os.path.join(man_dir, "qtc_api.json"), "w") as f:
        json.dump(manifest, f, separators=(",", ":"))
    # direct entry points (gen/entries.py), from the manifest just written
    import entries
    import inline
    inline.generate(json.loads(json.dumps(manifest)), args.out)   # gen/inline.py: before the entries, which use it
    efiles, nme, nce, nie = entries.generate(json.loads(json.dumps(manifest)), args.out)
    cmake.append("set(QTC_GEN_ENTRIES %s)" % " ".join("${CMAKE_CURRENT_LIST_DIR}/" + f for f in efiles))
    write_if_changed(os.path.join(gen_dir, "modules.cmake"), "\n".join(cmake) + "\n")
    totals = collections.Counter()
    for mod, c in em.cov.items():
        totals.update(c)
    cov = {"qt_version": primary["qt_version"], "modules": {m: dict(c) for m, c in em.cov.items()},
           "totals": dict(totals), "skip_reasons": dict(ctx.skip.most_common()),
           "skip_examples": {k: v for k, v in ctx.skip_examples.items()}}
    with open(os.path.join(man_dir, "coverage.json"), "w") as f:
        json.dump(cov, f, indent=1)
    print("generated: thunks=%d classes=%d methods_bound=%d/%d ctors=%d enums=%d" % (
        em.tid, totals["classes_bound"], totals["methods_bound"], totals["methods_seen"], totals["ctors_bound"], len(names)))
    for k, v in ctx.skip.most_common(14):
        print("  skip %-44s %d" % (k, v))


OPEN_ENUMS = {"Qt::Key", "QEvent::Type", "QMetaType::Type", "Qt::ItemDataRole", "QFont::Weight",
              "QTextFormat::Property", "QTextFormat::ObjectTypes", "QTextFormat::FormatType",
              "QStandardItem::ItemType", "QListWidgetItem::ItemType", "QTreeWidgetItem::ItemType",
              "QTableWidgetItem::ItemType", "QAccessible::Role", "QAccessible::Event",
              "QNetworkRequest::Attribute", "QNetworkRequest::KnownHeaders", "QGraphicsItem::Extension",
              "QSqlError::ErrorType", "QDialog::DialogCode", "QMessageBox::ButtonRole", "QKeySequence::StandardKey",
              "QFont::Stretch", "QFont::HintingPreference", "QLocale::Language", "QLocale::Country",
              "QLocale::Territory", "QLocale::Script", "QChar::SpecialCharacter", "QChar::UnicodeVersion",
              "QSizePolicy::Policy", "QPlatformSurfaceEvent::SurfaceEventType", "Qt::MouseButton",
              "QProcess::ProcessError", "QTextCharFormat::FontPropertiesInheritanceBehavior"}

if __name__ == "__main__":
    main()
