"""Preconditions checked by generated thunks before they call into Qt.

Qt documents many requirements that it only asserts in debug builds: an index
inside [0, size()), a non-null pointer, a source model that is not the proxy
itself. Release builds of Qt run straight into undefined behaviour instead. The
rules here turn each such requirement into a check, so a bad argument becomes a
qtc error and never a crash, a hang or an out-of-memory kill.

A rule's expression is C++ with placeholders:
  {s}           the receiver (a pointer to the declaring class)
  {0}, {1}, ... converted parameters; the check runs only when that argument was given
  {2|QModelIndex()}  parameter 2, or the default when it was not given
and may use the helpers in qtc_internal.h (qtc::idx, qtc::pos, qtc::span, ...).

The tables come from reading Qt's documentation and sources and from the method
fuzzer (tests/fuzz_methods.cpp), which calls every bound method with boundary
arguments and records every crash, hang and abort.
"""
import re

OOR = "QTC_ERR_OUT_OF_RANGE"
BAD = "QTC_ERR_BAD_ARGS"
STATE = "QTC_ERR_STATE"
LIMIT = "QTC_ERR_LIMIT"
PART_HELD = ("the part this replaces (and deletes) holds an object another owner keeps a plain pointer to "
             "and cannot let go of")
GRID = 65535          # rows and columns of grid and form layouts
CELLS = 1 << 16       # cells of a text table (Qt inserts cells one by one, each moving the ones after it)
STRETCH = 65535       # stretch factors of box and grid layouts (summed in int by the layout engine)

STR = r"QString|QByteArray"
LISTLIKE = r"QString|QByteArray|QBitArray|QJsonArray|QCborArray|QHttpHeaders|QPolygon|QPolygonF"

# (class regex, method regex, parameter-type regex or None, expression, code, message)
TABLE = [
    # ---------------------------------------------------------------- threads
    # a stack the system refuses makes start() fail with only a warning, and a pool would then
    # never run its tasks (their futures would never finish)
    ("QThread|QThreadPool", r"setStackSize", None, "{0} == 0 || ({0} >= 65536 && {0} <= (1u << 30))", OOR,
     "stack size must be 0 (the default) or between 64 KiB and 1 GiB"),
    ("QThreadPool", r"setMaxThreadCount", None, "{0} <= 4096", LIMIT, "at most 4096 threads"),
    # ---------------------------------------------------------------- model subclasses
    # (protected methods of overridable models, see src/qtc_override.h). Qt keeps begin*/end* on a
    # stack it pops in end* without looking, and checks the ranges only in debug builds: one pair
    # at a time here, each end* the begin* waiting, and the ranges Qt's documentation states.
    ("QAbstractItemModel", r"begin\w+", None, "qtc::model_idle({s})", STATE,
     "another begin* is still waiting for its end*"),
    ("QAbstractItemModel", r"beginInsertRows", None, "{1} >= 0 && {1} <= qtc::rows_of({s}, {0}) && {2} >= {1}", OOR,
     "first must lie in [0, rowCount(parent)] and last must not be less than first"),
    ("QAbstractItemModel", r"beginInsertColumns", None, "{1} >= 0 && {1} <= qtc::cols_of({s}, {0}) && {2} >= {1}", OOR,
     "first must lie in [0, columnCount(parent)] and last must not be less than first"),
    ("QAbstractItemModel", r"beginRemoveRows", None, "{1} >= 0 && {1} <= {2} && {2} < qtc::rows_of({s}, {0})", OOR,
     "first and last must lie in [0, rowCount(parent)) with first <= last"),
    ("QAbstractItemModel", r"beginRemoveColumns", None, "{1} >= 0 && {1} <= {2} && {2} < qtc::cols_of({s}, {0})", OOR,
     "first and last must lie in [0, columnCount(parent)) with first <= last"),
    ("QAbstractItemModel", r"beginMoveRows", None,
     "{1} >= 0 && {1} <= {2} && {2} < qtc::rows_of({s}, {0}) && {4} >= 0 && {4} <= qtc::rows_of({s}, {3})", OOR,
     "the rows must lie in [0, rowCount(sourceParent)) and the destination in [0, rowCount(destinationParent)]"),
    ("QAbstractItemModel", r"beginMoveColumns", None,
     "{1} >= 0 && {1} <= {2} && {2} < qtc::cols_of({s}, {0}) && {4} >= 0 && {4} <= qtc::cols_of({s}, {3})", OOR,
     "the columns must lie in [0, columnCount(sourceParent)) and the destination in [0, columnCount(destinationParent)]"),
    ("QAbstractItemModel", r"endInsertRows", None, "qtc::model_pending({s}, qtc::MOP_INSERT_ROWS)", STATE,
     "no beginInsertRows is waiting for it"),
    ("QAbstractItemModel", r"endRemoveRows", None, "qtc::model_pending({s}, qtc::MOP_REMOVE_ROWS)", STATE,
     "no beginRemoveRows is waiting for it"),
    ("QAbstractItemModel", r"endMoveRows", None, "qtc::model_pending({s}, qtc::MOP_MOVE_ROWS)", STATE,
     "no beginMoveRows is waiting for it"),
    ("QAbstractItemModel", r"endInsertColumns", None, "qtc::model_pending({s}, qtc::MOP_INSERT_COLUMNS)", STATE,
     "no beginInsertColumns is waiting for it"),
    ("QAbstractItemModel", r"endRemoveColumns", None, "qtc::model_pending({s}, qtc::MOP_REMOVE_COLUMNS)", STATE,
     "no beginRemoveColumns is waiting for it"),
    ("QAbstractItemModel", r"endMoveColumns", None, "qtc::model_pending({s}, qtc::MOP_MOVE_COLUMNS)", STATE,
     "no beginMoveColumns is waiting for it"),
    ("QAbstractItemModel", r"endResetModel", None, "qtc::model_pending({s}, qtc::MOP_RESET)", STATE,
     "no beginResetModel is waiting for it"),
    ("QAbstractItemModel", r"changePersistentIndexList", None,
     "qtc::indexes_of({s}, {0}) && qtc::indexes_of({s}, {1}) && {0}.size() == {1}.size()", BAD,
     "both lists must hold indexes of this model (or invalid ones), as many in each"),
    # ---------------------------------------------------------------- strings and byte arrays
    (STR, r"at", None, "qtc::idx({s}, {0})", OOR, "index out of range [0, size())"),
    (STR, r"first|last|chopped", r"qsizetype", "qtc::pos({s}, {0})", OOR, "n out of range [0, size()]"),
    (STR, r"sliced|slice", r"qsizetype", "qtc::pos({s}, {0})", OOR, "pos out of range [0, size()]"),
    (STR, r"sliced|slice", r"qsizetype,qsizetype", "qtc::span({s}, {0}, {1})", OOR, "pos and n must lie inside the string"),
    (STR, r"insert", r"qsizetype,.*", "qtc::pad({s}, {0})", LIMIT, "position too far past the end"),
    ("QByteArray", r"insert", r"qsizetype,qsizetype,char", "qtc::alloc_elems({s}, {1})", LIMIT, "count too large"),
    ("QByteArray", r"replace", r"qsizetype,qsizetype,.*", "qtc::pad({s}, {0})", LIMIT, "position too far past the end"),
    (STR, r"append|prepend|assign", r"qsizetype,.*", "qtc::alloc_elems({s}, {0})", LIMIT, "count too large"),
    (STR, r"fill", r".*,qsizetype", "qtc::alloc_elems({s}, {1})", LIMIT, "size too large"),
    (STR, r"reserve|resize|resizeForOverwrite|leftJustified|rightJustified", r"qsizetype.*",
     "qtc::alloc_elems({s}, {0})", LIMIT, "size too large"),
    (STR, r"repeated", None, "qtc::alloc_times({s}, {0})", LIMIT, "result too large"),
    ("QString", r"<ctor>", r"qsizetype,.*", "qtc::alloc_ok({0}, 2)", LIMIT, "size too large"),
    ("QByteArray", r"<ctor>", r"qsizetype,.*", "qtc::alloc_ok({0}, 1)", LIMIT, "size too large"),
    (STR, r"number|setNum", r"\w+,int", "qtc::in({1}, 2, 36)", OOR, "base must be in [2, 36]"),
    (STR, r"number|setNum", r"(double|float),char.*", "qtc::fmt_ok({1})", OOR, "format must be one of e E f g G"),
    (STR, r"number|setNum", r"(double|float),char,int", "qtc::prec_ok({2})", OOR, "precision must be in [-128, 1000]"),
    # ---------------------------------------------------------------- bit arrays
    ("QBitArray", r"at|testBit|setBit|clearBit|toggleBit", None, "qtc::idx({s}, {0})", OOR, "index out of range [0, size())"),
    ("QBitArray", r"fill", r"bool,qsizetype,qsizetype",
     "{1} >= 0 && {1} <= {2} && {2} <= qtc::size_of({s})", OOR, "need 0 <= first <= last <= size()"),
    ("QBitArray", r"fill", r"bool,qsizetype", "qtc::alloc_ok({1} / 8, 1)", LIMIT, "size too large"),
    ("QBitArray", r"resize", None, "qtc::alloc_ok({0} / 8, 1)", LIMIT, "size too large"),
    ("QBitArray", r"<ctor>", r"qsizetype.*", "qtc::alloc_ok({0} / 8, 1)", LIMIT, "size too large"),
    ("QBitArray", r"fromBits", None, "{1} >= 0 && {1} <= 8 * qtc::cstr_len(a[0])", OOR, "len (in bits) exceeds the data"),
    # ---------------------------------------------------------------- JSON, CBOR, HTTP headers, polygons, paths
    ("QJsonArray", r"insert", r"qsizetype,.*", "qtc::pos({s}, {0})", OOR, "index out of range [0, size()]"),
    ("QJsonArray", r"replace", r"qsizetype,.*", "qtc::idx({s}, {0})", OOR, "index out of range [0, size())"),
    ("QCborArray", r"insert", r"qsizetype,.*", "{0} >= -1 && qtc::pad({s}, {0})", LIMIT, "index must be -1 or not too far past the end"),
    ("QCborArray", r"takeAt|removeAt", None, "qtc::idx({s}, {0})", OOR, "index out of range [0, size())"),
    ("QCborArray", r"takeFirst|takeLast|removeFirst|removeLast|pop_front|pop_back", None, "qtc::nonempty({s})", OOR,
     "the array is empty"),
    ("QHttpHeaders", r"insert", r"qsizetype,.*", "qtc::pos({s}, {0})", OOR, "index out of range [0, size()]"),
    ("QHttpHeaders", r"replace|nameAt|valueAt|removeAt", r"qsizetype.*", "qtc::idx({s}, {0})", OOR, "index out of range [0, size())"),
    ("QHttpHeaders", r"reserve", None, "qtc::alloc_ok({0}, 64)", LIMIT, "size too large"),
    ("QPolygonF?", r"point|setPoint", r"int.*", "qtc::idx({s}, {0})", OOR, "index out of range [0, size())"),
    ("QPolygonF?", r"putPoints", r"int,int,\w+(,int)?",
     "{0} >= 0 && {1} >= 0 && {3|0} >= 0 && int64_t({3|0}) + {1} <= qtc::size_of(&{2}) && qtc::pad({s}, int64_t({0}) + {1})",
     OOR, "the points must lie inside both polygons"),
    ("QPolygon", r"<ctor>", r"qsizetype", "qtc::alloc_ok({0}, 8)", LIMIT, "size too large"),
    ("QPolygonF", r"<ctor>", r"qsizetype", "qtc::alloc_ok({0}, 16)", LIMIT, "size too large"),
    ("QPainterPath", r"setElementPositionAt|elementAt", None, "qtc::idx({s}, {0})", OOR, "index out of range [0, elementCount())"),
    # Qt keeps non-finite points out of a path everywhere else (moveTo, lineTo, ...): its clipper
    # crashes on one (simplified, united)
    ("QPainterPath", r"setElementPositionAt", None, "std::isfinite({1}) && std::isfinite({2})", OOR,
     "x and y must be finite"),
    ("QPainterPath", r"reserve", None, "qtc::alloc_ok({0}, 24)", LIMIT, "size too large"),
    ("QMatrix4x4", r"row|column|setRow|setColumn", r"int.*", "qtc::in({0}, 0, 3)", OOR, "index must be in [0, 3]"),
    ("QGeoPolygon", r"hole|holePath|removeHole", None, "{0} >= 0 && {0} < {s}->holesCount()", OOR, "index out of range [0, holesCount())"),
    # length() walks the coordinates from indexFrom without a check
    ("QGeoPath|QGeoPolygon", r"length", None, "{0|0} >= 0 && ({0|0} == 0 || {0|0} < {s}->size())", OOR,
     "indexFrom out of range [0, size())"),
    ("QSqlRecord", r"insert", r"int,.*", "qtc::pos({s}, {0})", OOR, "pos out of range [0, count()]"),
    ("QBarCategoryAxis", r"at", None, "qtc::idx({s}, {0})", OOR, "index out of range [0, count())"),
    ("QLocale", r"toString", r"(double|float),char.*", "qtc::fmt_ok({1})", OOR, "format must be one of e E f g G"),
    ("QLocale", r"toString", r"(double|float),char,int", "qtc::prec_ok({2})", OOR, "precision must be in [-128, 1000]"),
    ("QLocale", r"toString", r"\w+,int,char32_t", "qtc::in({1}, -(1 << 20), 1 << 20)", LIMIT, "fieldWidth too large"),
    ("QLocale", r"toCurrencyString", r"(double|float),QString,int", "qtc::prec_ok({2})", OOR, "precision must be in [-128, 1000]"),
    ("QLocale", r"formattedDataSize", r"qint64,int.*", "qtc::prec_ok({1})", OOR, "precision must be in [-128, 1000]"),
    # ---------------------------------------------------------------- text
    ("QTextCursor", r"insertTable", r"int,int.*", "{0} >= 1 && {1} >= 1 && int64_t({0}) * {1} <= %d" % CELLS, LIMIT,
     "rows and columns must be positive and the table at most %d cells" % CELLS),
    ("QTextCursor", r"movePosition", r"QTextCursor::MoveOperation,.*",
     "{0} != QTextCursor::NoMove || {2|1} <= 1", OOR, "NoMove cannot repeat"),
    ("QTextCursor", r"<ctor>", r"QTextBlock", "{0}.isValid()", STATE, "the block is not valid"),
    ("QTextTableCell", r"(?!isValid$|operator).*", None, "{s}->isValid()", STATE, "the cell is not valid"),
    ("QTextBlock", r"textDirection", None, "{s}->isValid()", STATE, "the block is not valid"),
    ("QDomAttr", r"ownerElement", None, "!{s}->isNull()", STATE, "the attribute is null"),
    ("QItemSelectionRange", r"intersected", None, "{s}->isValid() && {0}.isValid()", STATE, "both ranges must be valid"),
    ("QSvgRenderer", r"currentFrame|setCurrentFrame|animationDuration", None, "{s}->isValid()", STATE,
     "no document is loaded"),
    # ---------------------------------------------------------------- layouts, item models and views
    ("QFormLayout", r"isRowVisible|setRowVisible", r"int.*", "{0} >= 0 && {0} < {s}->rowCount()", OOR,
     "row out of range [0, rowCount())"),
    ("QFormLayout", r"setWidget|setLayout|setItem", r"int,.*", "qtc::in({0}, 0, int64_t({s}->rowCount()) + %d)" % GRID, OOR,
     "row out of range"),
    ("QBoxLayout", r"insert\w*", r"int,.*", "{0} < 0 || {0} <= {s}->count()", OOR, "index out of range [-1, count()]"),
    ("QGridLayout", r"setDefaultPositioning", None, "qtc::in({0}, 1, %d)" % GRID, OOR, "n must be in [1, %d]" % GRID),
    # a dimension counts even when the other one is 0: the model keeps a slot per row or column
    ("QStandardItemModel|QTableWidget", r"<ctor>", r"int,int.*",
     "{0} >= 0 && {1} >= 0 && qtc::alloc_ok(int64_t(qMax(1, {0})) * qMax(1, {1}), 8)", LIMIT, "rows x columns too large"),
    ("QStandardItemModel|QTableWidget", r"setRowCount", None,
     "{0} >= 0 && qtc::alloc_ok(int64_t({0}) * qMax(1, {s}->columnCount()), 8)", LIMIT, "rows x columns too large"),
    ("QStandardItemModel|QTableWidget", r"setColumnCount", None,
     "{0} >= 0 && qtc::alloc_ok(int64_t({0}) * qMax(1, {s}->rowCount()), 8)", LIMIT, "rows x columns too large"),
    ("QTreeWidget", r"setColumnCount", None, "qtc::in({0}, 0, %d)" % GRID, OOR, "columns must be in [0, %d]" % GRID),
    ("QSqlRelationalTableModel", r"setRelation", r"int,.*", "qtc::in({0}, 0, %d)" % GRID, OOR, "column out of range"),
    # ---------------------------------------------------------------- objects with state requirements
    ("QLCDNumber", r"<ctor>", r"uint.*", "{0} <= 99", OOR, "at most 99 digits"),
    # the engine crashes on rates such as 44101 or 1002 (its resampler); the usual rates all work
    ("QAudioEngine", r"<ctor>", r"int.*", "qtc::standard_sample_rate({0})", OOR,
     "sampleRate must be a standard rate (8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 64000, "
     "88200, 96000, 128000, 176400, 192000, 352800, 384000, 705600 or 768000)"),
    ("QProcess", r"setStandardOutputProcess", None, "{0} != nullptr && static_cast<QObject *>({0}) != {s}", BAD,
     "destination must be another process"),
    ("QOpenGLShaderProgram|QOpenGLTimerQuery|QOpenGLTimeMonitor", r"(?!<ctor>$).*", None, "qtc::gl_current()", STATE,
     "needs a current OpenGL context"),
    ("QOpenGLShader|QOpenGLFramebufferObject", r".*", None, "qtc::gl_current()", STATE, "needs a current OpenGL context"),
    ("QQmlComponent", r"loadUrl|loadFromModule|setData|create|beginCreate|createWithInitialProperties|createObject",
     None, "qtc::qml_has_engine({s})", STATE, "the component has no engine"),
    # ---------------------------------------------------------------- found by the method fuzzer
    (STR, r"to(U?(Short|Int|Long|LongLong))", r"bool\*,int", "{1} == 0 || qtc::in({1}, 2, 36)", OOR,
     "base must be 0 or in [2, 36]"),
    ("QRandomGenerator", r"bounded", r"q(u)?int64", "{0} > 0", OOR, "highest must be positive"),
    ("QRandomGenerator", r"bounded", r"(\w+),\1", "{0} < {1}", OOR, "lowest must be below highest"),
    # Qt's inline code subtracts in the parameter type and shifts by 64 past 2^63: both undefined
    ("QRandomGenerator", r"bounded", r"quint64", "quint64({0}) <= (quint64(1) << 63)", OOR, "highest must be at most 2^63"),
    ("QRandomGenerator", r"bounded", r"int,int", "int64_t({1}) - int64_t({0}) <= INT32_MAX", OOR,
     "lowest and highest may be at most 2^31 - 1 apart"),
    ("QRandomGenerator", r"bounded", r"(int|qint64),(int|qint64)",
     "qint64({0}) < qint64({1}) && (qint64({0}) >= 0 || qint64({1}) <= std::numeric_limits<qint64>::max() + qint64({0}))", OOR,
     "need lowest < highest, at most 2^63 - 1 apart"),
    ("QRandomGenerator", r"bounded", r"(uint|quint64),(uint|quint64)",
     "quint64({0}) < quint64({1}) && quint64({1}) - quint64({0}) <= (quint64(1) << 63)", OOR,
     "need lowest < highest, at most 2^63 apart"),
    ("QCborValue", r"fromCbor", r"QByteArray", "qtc::cbor_ok({0})", "QTC_ERR_INVALID_ARG", "the data is not valid CBOR"),
    ("QCborValue", r"fromCbor", r"const char\*,qsizetype", "qtc::cbor_ok(QByteArray({0}, {1}))", "QTC_ERR_INVALID_ARG",
     "the data is not valid CBOR"),
    ("QUrlQuery", r"setQueryDelimiters", None, "qtc::query_delims_ok({0}, {1})", BAD,
     "delimiters must be two different RFC 3986 sub-delimiters (! $ & ' ( ) * + , ; =)"),
    ("QProcessEnvironment", r"insert", r"QProcessEnvironment", "qtc::unshare({0})", BAD, "cannot copy the environment"),
    ("QConcatenateTablesProxyModel", r"setData|itemData|setItemData", r"QModelIndex.*", "{0}.isValid()", BAD,
     "the index must be valid"),
    ("QTextCursor", r"document", None, "!{s}->isNull()", STATE, "the cursor is null"),
    # resize changes the columns first, then the rows: the table in between counts too
    ("QTextTable", r"resize", None,
     "{0} >= 1 && {1} >= 1 && int64_t(qMax({0}, {s}->rows())) * qMax({1}, {s}->columns()) <= %d" % CELLS, LIMIT,
     "rows and columns must be positive and the table at most %d cells, also on the way" % CELLS),
    ("QTextTable", r"insertRows", None, "{1} >= 0 && (int64_t({s}->rows()) + {1}) * qMax(1, {s}->columns()) <= %d" % CELLS,
     LIMIT, "the table would exceed %d cells" % CELLS),
    ("QTextTable", r"appendRows", None, "{0} >= 0 && (int64_t({s}->rows()) + {0}) * qMax(1, {s}->columns()) <= %d" % CELLS,
     LIMIT, "the table would exceed %d cells" % CELLS),
    ("QTextTable", r"insertColumns", None, "{1} >= 0 && (int64_t({s}->columns()) + {1}) * qMax(1, {s}->rows()) <= %d" % CELLS,
     LIMIT, "the table would exceed %d cells" % CELLS),
    ("QTextTable", r"appendColumns", None, "{0} >= 0 && (int64_t({s}->columns()) + {0}) * qMax(1, {s}->rows()) <= %d" % CELLS,
     LIMIT, "the table would exceed %d cells" % CELLS),
    ("QAbstractScrollArea", r"setVerticalScrollBar", None, "{0} != {s}->verticalScrollBar()", BAD,
     "this already is the vertical scroll bar"),
    ("QAbstractScrollArea", r"setHorizontalScrollBar", None, "{0} != {s}->horizontalScrollBar()", BAD,
     "this already is the horizontal scroll bar"),
    ("QColumnView", r"setPreviewColumnVisible", None, "{s}->model() != nullptr", STATE, "set a model first"),
    ("QLCDNumber", r"<ctor>", r"uint.*", "qtc::in({0}, 1, 99)", OOR, "numDigits must be in [1, 99]"),
    # with no digits, a small decimal point makes display() write past an empty bit array
    ("QLCDNumber", r"setDigitCount|setNumDigits", None, "{0} >= 1", OOR, "at least one digit"),
    ("QProxyStyle", r"setBaseStyle", None, "qtc::style_no_cycle({s}, {0})", BAD, "the style would become its own base"),
    ("QTableWidget", r"removeRow", None, "{0} >= 0 && {0} < {s}->rowCount()", OOR, "row out of range [0, rowCount())"),
    ("QTableWidget", r"removeColumn", None, "{0} >= 0 && {0} < {s}->columnCount()", OOR,
     "column out of range [0, columnCount())"),
    ("QSqlQuery", r"boundValueName", None, "{0} >= 0", OOR, "pos must not be negative"),
    ("QQuickItem", r"grabTouchPoints", None, "{s}->window() != nullptr", STATE, "the item is not in a window"),
    ("QBarCategoryAxis", r"insert", r"int,QString", "qtc::pos({s}, {0})", OOR, "index out of range [0, count()]"),
    ("QPainterPath", r"toFillPolygons", None, "qtc::fill_polygons_ok({s})", LIMIT,
     "more than 8192 subpaths (the computation is quadratic); use toSubpathPolygons"),
    ("QPainterPath", r"toSubpathPolygons|toFillPolygons|toFillPolygon", r"(QTransform)?",
     "qtc::flatten_ok(*{s}, {0|QTransform()})", LIMIT, "flattening the path would exceed the allocation limit"),
    # Qt refuses non-finite points when they are added but not when a path is moved: a path of NaN
    # points defeats the bounding-box tests, and every hit test then walks all of it (selecting
    # with one in a scene of a few dozen items took minutes)
    ("QPainterPath", r"translate|translated", r"qreal,qreal", "std::isfinite({0}) && std::isfinite({1})", BAD,
     "the offset must be finite"),
    ("QPainterPath", r"translate|translated", r"QPointF", "std::isfinite({0}.x()) && std::isfinite({0}.y())", BAD,
     "the offset must be finite"),
    ("QPainterPath", r"toSubpathPolygons|toFillPolygons|toFillPolygon", None,
     "qtc::path_finite(*{s}) && qtc::transform_finite({0|QTransform()})", BAD,
     "the path or the transform has values that are not finite"),
    ("QTransform", r"map", r"QPainterPath", "qtc::transform_finite(*{s})", BAD,
     "the transform has values that are not finite (the path would get NaN points)"),
    ("QPainterPath", r"united|intersected|subtracted|contains|intersects|connectPath|addPath", r"QPainterPath",
     "qtc::path_finite({0})", BAD, "the path has points that are not finite"),
    ("QGraphicsScene", r"setSelectionArea", r"QPainterPath(,.*)?",
     "qtc::path_finite({0}) && [&] { const double items = std::max<qsizetype>(1, {s}->items().size()), "
     "budget = double(int64_t(1) << 24); return std::max(double({0}.elementCount()), "
     "qtc::path_points({0}, QTransform(), budget / items)) * items <= budget; }()",
     LIMIT, "the path has points that are not finite, or its elements (curves counted as flattened) times the "
     "scene's items exceed 2^24 (each item is tested against the whole path: about a second)"),
    ("QPainterPath", r"united|intersected|subtracted|contains|intersects", r"QPainterPath",
     "qtc::flatten_ok(*{s}, QTransform()) && qtc::flatten_ok({0}, QTransform()) && qtc::path_room(*{s}, {0}.elementCount())",
     LIMIT, "the paths are too large for a boolean operation"),
    # Qt's clipper slows down about as the square of the crossings: minutes for paths of 512 random points
    ("QPainterPath", r"united|intersected|subtracted", r"QPainterPath", "qtc::clip_ok(*{s}, {0}, true)", LIMIT,
     "the paths cross too often for a boolean operation (more than 4,096 crossings or 32,768 segments), or have "
     "a point beyond +-2^30 or not finite"),
    ("QPainterPath", r"contains|intersects", r"QPainterPath", "qtc::clip_ok(*{s}, {0}, false)", LIMIT,
     "the paths have a point beyond +-2^30 (where Qt's clipper overflows) or not finite"),
    ("QPainterPath", r"simplified", None, "qtc::flatten_ok(*{s}, QTransform())", LIMIT, "the path is too large"),
    ("QPainterPath", r"simplified", None, "qtc::clip_ok(*{s}, QPainterPath(), true)", LIMIT,
     "the path crosses itself too often to simplify (more than 4,096 crossings or 32,768 segments), or has a "
     "point beyond +-2^30 or not finite"),
    # a chart another view shows would be moved into this view's scene, leaving the other view a
    # plain pointer to a chart this view deletes
    ("QChartView", r"<ctor>", r"QChart\*(,.*)?", "qtc::not_held(nullptr, {0})", STATE,
     "the chart is shown by another view; take it from there first"),
    # a QChartView keeps a plain pointer to its chart, which lives in the view's own scene
    ("QGraphicsScene", r"clear", r"", "!({s}->parent() && {s}->parent()->inherits(\"QChartView\"))", STATE,
     "the scene belongs to a QChartView, which keeps its chart in it"),
    # a popup view must be a list, table or tree: a QColumnView or QHeaderView there crashes when the
    # current item changes (QColumnView::currentChanged)
    ("QComboBox|QCompleter", r"setView|setPopup", r"QAbstractItemView\*",
     "!{0} || qobject_cast<QListView *>({0}) || qobject_cast<QTableView *>({0}) || qobject_cast<QTreeView *>({0})", BAD,
     "the popup view must be a QListView, QTableView or QTreeView"),
    # a QFileDialog's completer takes its proxy model's source for a QFileSystemModel
    ("QAbstractProxyModel|QSortFilterProxyModel|QIdentityProxyModel|QTransposeProxyModel", r"setSourceModel", None,
     "qtc::proxy_source_free({s})", STATE, "the proxy is a QFileDialog's proxy model: its source stays the dialog's model"),
    # the popup deletes its current calendar before taking the new one: the same one again is used
    # after it was deleted
    ("QDateTimeEdit", r"setCalendarWidget", None, "{0} == nullptr || {s}->calendarWidget() != {0}", STATE,
     "the calendar widget is already this edit's calendar (Qt would delete it, then use it)"),
    # indexes of another model reach this model's data() and index() as its own
    ("QComboBox", r"setRootModelIndex", None, "qtc::index_of({s}->model(), {0})", BAD, "the index belongs to another model"),
    ("QCompleter", r"pathFromIndex", None, "qtc::index_of({s}->model(), {0})", BAD, "the index belongs to another model"),
    ("QItemSelection|QItemSelectionRange", r"<ctor>|select", r"QModelIndex,QModelIndex", "{0}.model() == {1}.model()", BAD,
     "the two indexes belong to different models"),
    # an id for a button outside the group stays in the group's table after the button is gone
    ("QButtonGroup", r"setId", None, "{0} == nullptr || {s}->buttons().contains({0})", STATE,
     "the button is not in this group"),
    # a perspective transform maps each curve as up to 512 line segments
    ("QTransform", r"map", r"QPainterPath", "qtc::project_ok(*{s}, {0})", LIMIT,
     "a perspective transform maps each curve as up to 512 lines: at most 2^20 points"),
    ("QGraphicsView", r"mapToScene|mapFromScene", r"QPainterPath", "qtc::project_ok({s}->viewportTransform(), {0})", LIMIT,
     "a perspective transform maps each curve as up to 512 lines: at most 2^20 points"),
    # measuring a path splits each curve into about sqrt(length) pieces
    ("QPainterPath", r"length|percentAtLength|pointAtPercent|angleAtPercent|slopeAtPercent|trimmed", None,
     "qtc::path_length_ok(*{s})", LIMIT, "the path's curves are too long to measure (more than about 2^20 pieces)"),
    # a length or fraction that is not a number never ends the search along a curve
    ("QPainterPath", r"percentAtLength|pointAtPercent|angleAtPercent|slopeAtPercent", r"qreal", "std::isfinite({0})", OOR,
     "must be a finite number"),
    ("QPainterPath", r"trimmed", None, "std::isfinite({0}) && std::isfinite({1}) && std::isfinite({2|0.0})", OOR,
     "the fractions and the offset must be finite numbers"),
    ("QPainterPath", r"addPath|connectPath", r"QPainterPath", "qtc::path_room(*{s}, {0}.elementCount())", LIMIT,
     "the path would exceed 4,194,304 elements"),
    ("QPainterPath", r"addText", r"QPointF,QFont,QString", "qtc::path_room(*{s}, 64 * int64_t({2}.size()))", LIMIT,
     "the path would exceed 4,194,304 elements"),
    ("QPainterPath", r"addText", r"qreal,qreal,QFont,QString", "qtc::path_room(*{s}, 64 * int64_t({3}.size()))", LIMIT,
     "the path would exceed 4,194,304 elements"),
    ("QPainterPath", r"addPolygon", r"QPolygonF", "qtc::path_room(*{s}, {0}.size())", LIMIT,
     "the path would exceed 4,194,304 elements"),
    ("QPainterPath", r"addRegion", r"QRegion", "qtc::path_room(*{s}, 5 * int64_t({0}.rectCount()))", LIMIT,
     "the path would exceed 4,194,304 elements"),
    ("QPainterPathStroker", r"createStroke", None, "qtc::flatten_ok({0}, QTransform()) && qtc::path_room({0}, {0}.elementCount())",
     LIMIT, "the path is too large to stroke"),
    (STR, r"indexOf|lastIndexOf|count|contains|remove|split|section|replace|startsWith|endsWith|removeAll",
     r"(QString|QLatin1StringView|QStringView|QByteArrayView|QByteArray|const char\*)(,.*)?",
     "qtc::search_ok(qtc::size_of({s}), qtc::text_len({0}))", LIMIT,
     "pattern too long for the text (searching could take text x pattern steps)"),
    ("QString", r"replace", r"(QString|QChar|QLatin1StringView),(QString|QLatin1StringView)(,Qt::CaseSensitivity)?",
     "qtc::replace_ok(*{s}, QString({0}), QString({1}).size(), {2|Qt::CaseSensitive})", LIMIT,
     "the result would exceed the allocation limit"),
    ("QString", r"replace", r"QRegularExpression,QString", "qtc::replace_ok(*{s}, {0}, {1}.size())", LIMIT,
     "the result would exceed the allocation limit"),
    ("QByteArray", r"replace", r"QByteArrayView,QByteArrayView", "qtc::replace_ok(*{s}, QByteArray({0}.data(), {0}.size()), {1}.size())", LIMIT,
     "the result would exceed the allocation limit"),
    ("QByteArray", r"replace", r"char,QByteArrayView", "qtc::replace_ok(*{s}, QByteArray(1, {0}), {1}.size())", LIMIT,
     "the result would exceed the allocation limit"),
    ("QByteArray", r"replace", r"const char\*,qsizetype,const char\*,qsizetype",
     "qtc::replace_ok(*{s}, QByteArray({0}, {1}), {3} < 0 ? int64_t(qstrlen({2})) : int64_t({3}))", LIMIT,
     "the result would exceed the allocation limit"),
    ("QConcatenateTablesProxyModel", r"removeSourceModel", None, "{s}->sourceModels().contains({0})", BAD,
     "the model is not one of its sources"),
    ("QDtls", r"handleTimeout", None, "{s}->handshakeState() == QDtls::HandshakeInProgress", STATE,
     "no handshake is in progress"),
    ("QDBusConnection", r"(?!isConnected$|name$|baseService$|lastError$|connectionCapabilities$|<ctor>$).*", None,
     "{s}->isConnected()", STATE, "the connection is not connected"),
    ("QBarSet", r"insert", r"int,.*", "qtc::pos({s}, {0})", OOR, "index out of range [0, count()]"),
    # the series and the category axis insert into their lists at the index unchecked
    ("QAbstractBarSeries|QPieSeries|QBoxPlotSeries|QCandlestickSeries|QBarCategoryAxis", r"insert", r"int,.*",
     "qtc::pos({s}, {0})", OOR, "index out of range [0, count()]"),
    ("QBarCategoryAxis", r"at", None, "qtc::idx({s}, {0})", OOR, "index out of range [0, count())"),
    ("QBarSet", r"replace", r"int,.*", "qtc::idx({s}, {0})", OOR, "index out of range [0, count()))"),
    ("QXYSeries", r"at|remove|setPointSelected|isPointSelected|pointConfiguration|setPointConfiguration|clearPointConfiguration|"
     r"setPointLabelsFormat|sizeBy", r"int(,.*)?", "qtc::idx({s}, {0})", OOR, "index out of range [0, count())"),
    ("QXYSeries", r"replace", r"int,.*", "qtc::idx({s}, {0})", OOR, "index out of range [0, count())"),
    ("QXYSeries", r"insert", r"int,.*", "qtc::pos({s}, {0})", OOR, "index out of range [0, count()]"),
    ("QXYSeries", r"removePoints", r"int,int", "qtc::span({s}, {0}, {1})", OOR, "the points must exist"),
    ("QIODevice", r"read|peek|readLine", r"qint64", "qtc::alloc_ok({0}, 1)", LIMIT, "maxSize too large"),
    # QIODevice points at the buffer of the current channel: a channel outside [0, count) leaves
    # that pointer outside the buffer list (negative) or null (too large), and the next read or
    # write corrupts the heap or crashes. Channel 0 is the one of a device that is not open.
    ("QIODevice", r"setCurrentReadChannel", None, "{0} == 0 || ({0} > 0 && {0} < {s}->readChannelCount())", OOR,
     "channel out of range [0, readChannelCount())"),
    ("QIODevice", r"setCurrentWriteChannel", None, "{0} == 0 || ({0} > 0 && {0} < {s}->writeChannelCount())", OOR,
     "channel out of range [0, writeChannelCount())"),
    ("QProcess", r"setReadChannel", None, "{0} == QProcess::StandardOutput || {0} == QProcess::StandardError", OOR,
     "channel must be StandardOutput or StandardError"),
    # unlike startClientEncryption, Qt does not check this itself and crashes on a socket without a connection
    ("QSslSocket", r"startServerEncryption", None, "{s}->state() == QAbstractSocket::ConnectedState", STATE,
     "the socket is not connected"),
    # a style that belongs to something (a proxy style's base, a scene's, the application's) is
    # deleted when that owner gets another one; a widget would keep a dangling pointer
    ("QWidget", r"setStyle", None, "{0} == nullptr || {0}->parent() == nullptr", STATE,
     "the style belongs to something else (a proxy style, a scene or the application) that may delete it; "
     "give the widget a style of its own"),
    # a widget does not own the style it is given, and a style sheet keeps a plain pointer to it:
    # qtc_destroy on the style first gives its widgets back the application style
    ("QWidget", r"setStyle", None, "qtc::note_style_user({s}, {0})", BAD, "-"),
    # a context watches the objects it names until they die: the context itself or one of its
    # children dies with it, after the context is gone, and calls into the destroyed context
    ("QQmlContext", r"setContextProperty", None, "qtc::not_within({1}, {s})", BAD,
     "the context itself or an object inside it cannot be one of its properties"),
    # the parent must be one of the widget's ancestors (Qt walks up to it and crashes at the top)
    ("QWidget", r"mapTo|mapFrom", r"const QWidget\*,.*", "{0} != nullptr && ({0} == {s} || {0}->isAncestorOf({s}))", BAD,
     "the widget must be this widget or one of its ancestors"),
    # ---- crashes of older Qt 6 releases (found fuzzing against Qt 6.4.2); harmless on newer ones
    ("QTextDocument", r"object", r"int",
     "{0} >= 0 && {0} < {s}->allFormats().size() && ({s}->allFormats().at({0}).isFrameFormat() || "
     "{s}->allFormats().at({0}).isListFormat())", OOR, "no text object has that index"),
    ("QHeaderView", r"setSectionResizeMode|resizeSection|setSectionHidden|hideSection|showSection", r"int(,.*)?",
     "{0} >= 0 && {0} < {s}->count()", OOR, "no section has that logical index"),
    ("QQmlComponent", r"isBound|createWithInitialProperties|setInitialProperties|completeCreate", None,
     "QT_VERSION >= QT_VERSION_CHECK(6, 12, 0) || {s}->status() == QQmlComponent::Ready", STATE,
     "the component is not ready (this Qt version crashes on it)"),
    ("QWebSocketServer", r"closed", None, "QT_VERSION >= QT_VERSION_CHECK(6, 12, 0) || {s}->isListening()", STATE,
     "the server is not listening (this Qt version aborts on it)"),
    # Qt 6.4's engine crashes on sounds made after stop() (spatial audio was a preview there)
    ("QAudioEngine", r"stop", None, "QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)", STATE,
     "this Qt version crashes on sounds made after stop(): pause() the engine or destroy it"),
    # the index makes 2^(depth + 1) nodes the next time it updates (Qt picks at most about 12)
    ("QGraphicsScene", r"setBspTreeDepth", None, "qtc::in({0}, 0, 20)", OOR, "depth must be in [0, 20] (0 picks one)"),
    # Qt keeps both in a qint8 and reserves room for maxPoints touch points up front
    ("QPointingDevice", r"<ctor>", r"QString,qint64,QInputDevice::DeviceType,QPointingDevice::PointerType,.*",
     "qtc::in({5}, 0, 127) && qtc::in({6}, 0, 127)", OOR, "maxPoints and buttonCount must be in [0, 127]"),
    # setView deletes the old view first, even when it is the new one
    ("QComboBox", r"setView", None, "{0} != {s}->view()", BAD, "it is the combo box's view already"),
    # setters that delete the part they replace at once: owners that hold something in it let go
    # first, as before qtc_destroy (QColumnView's preview widget lives in its preview column's viewport)
    ("QAbstractScrollArea+", r"setViewport", r"QWidget\*", "{0} == {s}->viewport() || qtc::part_free({s}, {s}->viewport())",
     STATE, PART_HELD),
    ("QAbstractScrollArea+", r"setVerticalScrollBar", r"QScrollBar\*",
     "{0} == {s}->verticalScrollBar() || qtc::part_free({s}, {s}->verticalScrollBar())", STATE, PART_HELD),
    ("QAbstractScrollArea+", r"setHorizontalScrollBar", r"QScrollBar\*",
     "{0} == {s}->horizontalScrollBar() || qtc::part_free({s}, {s}->horizontalScrollBar())", STATE, PART_HELD),
    ("QScrollArea+", r"setWidget", r"QWidget\*", "{0} == {s}->widget() || qtc::part_free({s}, {s}->widget())", STATE,
     PART_HELD),
    # a scroll area whose widget is also its viewport resizes one to fit the other: each resize
    # event resizes again, and when the scroll bars come and go it recurses until the stack runs out
    ("QScrollArea+", r"setWidget", r"QWidget\*", "!{0} || ({0} != {s}->viewport() && {0} != {s} && !{0}->isAncestorOf({s}))",
     BAD, "the widget is the scroll area's viewport, the scroll area, or one of its ancestors"),
    # a scroll area keeps watching its widget after another one takes it (and after it is moved
    # elsewhere): each then resizes it to its own viewport on the other's resize, until the stack
    # runs out
    ("QScrollArea+", r"setWidget", r"QWidget\*",
     "qtc::in_no_scroll_area({0}, {s})",
     STATE, "the widget is another scroll area's widget (a tool box's page is one too): take it from that one first "
     "(takeWidget, removeItem)"),
    # (a tool box puts each page in a scroll area of its own)
    ("QToolBox", r"addItem", None, "qtc::in_no_scroll_area({0})", STATE,
     "the widget is a scroll area's widget (or another tool box's page): take it from there first"),
    ("QToolBox", r"insertItem", None, "qtc::in_no_scroll_area({1})", STATE,
     "the widget is a scroll area's widget (or another tool box's page): take it from there first"),
    ("QAbstractScrollArea+", r"setViewport", r"QWidget\*",
     "!{0} || ({0} != {s} && !{0}->isAncestorOf({s}) && !(qobject_cast<QScrollArea *>({s}) && "
     "static_cast<QScrollArea *>({s})->widget() == {0}))",
     BAD, "the viewport is the scroll area's widget, the scroll area, or one of its ancestors"),
    ("QTabWidget+", r"setTabBar", r"QTabBar\*", "{0} == {s}->tabBar() || qtc::part_free({s}, {s}->tabBar())", STATE,
     PART_HELD),
    ("QTableView+", r"setHorizontalHeader", r"QHeaderView\*",
     "{0} == {s}->horizontalHeader() || qtc::part_free({s}, {s}->horizontalHeader())", STATE, PART_HELD),
    ("QTableView+", r"setVerticalHeader", r"QHeaderView\*",
     "{0} == {s}->verticalHeader() || qtc::part_free({s}, {s}->verticalHeader())", STATE, PART_HELD),
    ("QTreeView+", r"setHeader", r"QHeaderView\*", "{0} == {s}->header() || qtc::part_free({s}, {s}->header())", STATE,
     PART_HELD),
    ("QComboBox+", r"setLineEdit", r"QLineEdit\*", "{0} == {s}->lineEdit() || qtc::part_free({s}, {s}->lineEdit())",
     STATE, PART_HELD),
    ("QComboBox+", r"setView", r"QAbstractItemView\*", "qtc::part_free({s}, {s}->view())", STATE, PART_HELD),
    ("QTextEdit+|QPlainTextEdit+", r"setDocument", r"QTextDocument\*",
     "{0} == {s}->document() || qtc::part_free({s}, {s}->document())", STATE, PART_HELD),
    # QAudioBufferOutput remembers a player that replaced it by a plain pointer and calls it when it
    # is destroyed, even after the player is gone: a player gets one output for its life
    ("QMediaPlayer", r"setAudioBufferOutput", None, "{s}->audioBufferOutput() == nullptr || {s}->audioBufferOutput() == {0}",
     STATE, "the player already has an audio buffer output (Qt cannot replace it safely): use a new QMediaPlayer"),
    # these keep a plain pointer to the argument; only members are tracked when they die
    ("QUndoGroup", r"setActiveStack", None, "{0} == nullptr || {s}->stacks().contains({0})", BAD,
     "the stack is not in this group (addStack it first)"),
    ("QMenu|QMenuBar", r"setActiveAction", None, "{0} == nullptr || {s}->actions().contains({0})", BAD,
     "the action is not in this menu (addAction it first)"),
    ("QTextDocument", r"setDocumentLayout", None, "{0} == nullptr || {0}->document() == {s}", BAD,
     "the layout was made for another document"),
    ("QTextDocument", r"setDocumentLayout", None, "qtc::doc_layout_ok({s}, {0})", BAD,
     "a document shown by a QPlainTextEdit takes only a QPlainTextDocumentLayout"),
    # setters that take ownership without making the object their child: one owner only, or the
    # object is deleted twice
    ("QDrag|QClipboard", r"setMimeData", None, "qtc::free_for({s}, {0})", STATE,
     "the mime data belongs to something else (another drag or the clipboard)"),
    ("QNetworkAccessManager", r"setCache|setCookieJar", None, "qtc::free_for({s}, {0})", STATE,
     "it belongs to something else (another network access manager)"),
    # these take over a style (it becomes their child): one that has a parent is owned already
    # (and the new owner deletes it when it gets another one, so no widget may be using it; a proxy
    # style deletes its current base when given it again)
    ("QGraphicsScene", r"setStyle", None,
     "qtc::free_for({s}, {0}) && ({0} == nullptr || ({0}->parent() == nullptr && qtc::style_unshared({0})))", STATE,
     "the style belongs to something else, or widgets use it"),
    # Qt deletes the scene's own style first and then tells its proxy widgets, whose widgets still
    # use it (QGraphicsScene::setStyle): a use after free
    ("QGraphicsScene", r"setStyle", None, "qtc::scene_style_swappable({s})", STATE,
     "the scene's own style is in use by the widgets in its proxy widgets, and Qt deletes it before they "
     "let go of it: set the scene's style before adding proxy widgets, or remove them first"),
    ("QApplication", r"setStyle", r"QStyle\s*\*",
     "qtc::free_for(nullptr, {0}) && ({0} == nullptr || ({0}->parent() == nullptr && qtc::style_unshared({0})))",
     STATE, "the style belongs to something else, or widgets use it"),
    ("QProxyStyle", r"<ctor>", r"QStyle\s*\*",
     "qtc::free_for(nullptr, {0}) && ({0} == nullptr || ({0}->parent() == nullptr && qtc::style_unshared({0})))",
     STATE, "the style belongs to something else, or widgets use it"),
    ("QProxyStyle", r"setBaseStyle", None,
     "qtc::free_for({s}, {0}) && ({0} == nullptr || ({0}->parent() == nullptr && qtc::style_unshared({0})))",
     STATE, "the style belongs to something else (this proxy's base already, another proxy), or widgets use it"),
    # writing past the end fills the gap with zeros: the buffer grows to the position, so the
    # position is what is limited (seeking a little further each time grew it without bound)
    # (QIODevice::seek called on a buffer, and the classes that seek a device they are given)
    ("QIODevice+", r"seek", None, "qtc::buffer_seek_ok({s}, {0})", LIMIT,
     "seeking that far past the end would make the buffer larger than the allocation limit (qtc_set_alloc_limit)"),
    ("QWaveDecoder", r"seek", None, "qtc::buffer_seek_ok({s}->getDevice(), {0})", LIMIT,
     "seeking that far past the end would make the buffer larger than the allocation limit (qtc_set_alloc_limit)"),
    ("QTextStream", r"seek", None, "qtc::buffer_seek_ok({s}->device(), {0})", LIMIT,
     "seeking that far past the end would make the buffer larger than the allocation limit (qtc_set_alloc_limit)"),
    ("QString", r"split", r"(QString|QChar)(,.*)?", "qtc::split_ok(*{s}, QString({0}), {2|Qt::CaseSensitive})", LIMIT,
     "the result would have too many parts"),
    ("QString", r"normalized", None, "qtc::normalize_ok(*{s})", LIMIT,
     "a run of more than 16,384 combining marks (Qt orders each run in time growing as its square), or length x "
     "characters past ASCII over 2^36 (it rewrites the string in place: about a second)"),
    ("QString", r"split", r"QRegularExpression(,.*)?", "qtc::split_ok(*{s}, {0})", LIMIT, "the result would have too many parts"),
    ("QByteArray", r"split", r"char", "qtc::split_ok(*{s}, {0})", LIMIT, "the result would have too many parts"),
    ("QSplashScreen", r"finish", None, "{0} != {s} && ({0} == nullptr || {0}->isWindow())", BAD,
     "mainWin must be another top-level window"),
    ("QTextList", r"add|remove", r"QTextBlock", "{0}.isValid() && {0}.document() == {s}->document()", BAD,
     "the block must be a valid block of the list's document"),
    ("QJSEngine", r"importModule", None, "qtc::file_exists({0})", "QTC_ERR_IO", "no module file at that path"),
    ("QChart", r"setAxisX|setAxisY", r"QAbstractAxis\*.*", "!{s}->axes().contains({0})", BAD,
     "the axis already is one of the chart's axes"),
    ("QBoxSet", r"setValue|at", r"int.*", "qtc::in({0}, 0, 4)", OOR, "index must be in [0, 4] (QBoxSet::ValuePositions)"),
    ("QRandomGenerator+", r"discard", None, "{0} <= (1ull << 28)", LIMIT, "at most 2^28 values can be discarded in one call"),
    ("QLineEdit", r"cursorForward|cursorBackward", r"bool,int", "qtc::in({1}, -(int64_t({s}->text().size()) + 1), int64_t({s}->text().size()) + 1)",
     OOR, "steps must not exceed the text length"),
    ("QTcpServer|QWebSocketServer", r"pauseAccepting|resumeAccepting", None, "{s}->isListening()", STATE,
     "the server is not listening"),
    (r"Q\w+ModelMapper", r"setModel", None, "qobject_cast<QConcatenateTablesProxyModel *>({0}) == nullptr", STATE,
     "a chart model mapper writes into its model; QConcatenateTablesProxyModel cannot take new rows"),
    # (index + count wraps for a count near INT_MAX, and Qt then removes past the end)
    ("QBarSet", r"remove", r"int(,int)?", "qtc::idx({s}, {0}) && {1|1} >= 0 && int64_t({0}) + {1|1} <= INT_MAX", OOR,
     "index out of range [0, count()), or count negative or past INT_MAX - index"),
    (r"Q\w+ModelMapper", r"setSeries", None, "{0} == nullptr || {s}->model() != nullptr", STATE,
     "set the model first"),
    (r"Q\w+ModelMapper", r"setModel", None, "{0} != nullptr || {s}->series() == nullptr", STATE,
     "a mapper with a series needs a model"),
    # ---- found by the method fuzzer's random rounds (--seed): wider values, shuffled order
    ("QVersionNumber", r"segmentAt", None, "{0} >= 0", OOR, "index must not be negative"),
    # -i overflows for INT_MIN, and the stacks are then read far out of bounds
    ("QTextBrowser", r"historyUrl|historyTitle", None, "{0} > std::numeric_limits<int>::min()", OOR,
     "i out of range"),
    # a default-constructed proxy has no data yet, and these read it unchecked
    ("QNetworkProxy", r"header|headers|rawHeader|hasRawHeader|rawHeaderList", None, "!(*{s} == QNetworkProxy())", STATE,
     "the proxy has no settings yet (set its type first)"),
    ("QSortFilterProxyModel", r"sort", None, "{0} >= -1", OOR, "column must be -1 (the source order) or a column"),
    # Qt's inline (deprecated) fontFamily() takes first() of the families list, empty or not
    ("QTextCharFormat", r"fontFamily", None, "!{s}->fontFamilies().toStringList().isEmpty()", STATE,
     "no font family is set (fontFamilies() is empty)"),
    # pos + num past the end makes Qt move pos back by num, below 0 when num exceeds the count
    ("QTextTable", r"removeRows", None, "{0} >= 0 && {1} >= 0 && int64_t({0}) + {1} <= {s}->rows()", OOR,
     "the rows must exist"),
    ("QTextTable", r"removeColumns", None, "{0} >= 0 && {1} >= 0 && int64_t({0}) + {1} <= {s}->columns()", OOR,
     "the columns must exist"),
    # the cell is split into numRows x numCols: fewer than one makes Qt insert span - n rows
    ("QTextTable", r"splitCell", None, "{2} >= 1 && {3} >= 1", OOR, "numRows and numCols must be at least 1"),
    # Qt's layout engine (qGeomCalc) adds stretch factors, spacing and sizes up in int: a negative
    # stretch, or sums past int (a stretch or a spacer of INT_MAX), keep its space distribution
    # from ever finishing. Each value stays within what a screen can use; layout_sums_ok checks
    # the totals when a layout is laid out.
    ("QBoxLayout", r"addStretch", None, "qtc::in({0|0}, 0, %d)" % STRETCH, OOR, "the stretch must be in [0, %d]" % STRETCH),
    ("QBoxLayout", r"insertStretch|setStretch|setStretchFactor", None, "qtc::in({1|0}, 0, %d)" % STRETCH, OOR,
     "the stretch must be in [0, %d]" % STRETCH),
    ("QBoxLayout", r"addWidget|addLayout", r"(QWidget|QLayout)\*,int(,.*)?", "qtc::in({1|0}, 0, %d)" % STRETCH, OOR,
     "the stretch must be in [0, %d]" % STRETCH),
    ("QBoxLayout", r"insertWidget|insertLayout", r"int,(QWidget|QLayout)\*,int(,.*)?", "qtc::in({2|0}, 0, %d)" % STRETCH, OOR,
     "the stretch must be in [0, %d]" % STRETCH),
    ("QGridLayout", r"setRowStretch|setColumnStretch", None, "qtc::in({1}, 0, %d)" % STRETCH, OOR,
     "the stretch must be in [0, %d]" % STRETCH),
    ("QGridLayout", r"setRowMinimumHeight|setColumnMinimumWidth", None, "qtc::in({1}, 0, 16777215)", OOR,
     "the minimum size must be in [0, 16777215]"),
    ("QBoxLayout", r"addSpacing|addStrut", None, "qtc::in({0}, 0, 16777215)", OOR, "the size must be in [0, 16777215]"),
    ("QBoxLayout", r"insertSpacing", None, "qtc::in({1}, 0, 16777215)", OOR, "the size must be in [0, 16777215]"),
    ("QLayout+", r"setSpacing|setHorizontalSpacing|setVerticalSpacing", r"int",
     "{0} <= 16777215", OOR, "the spacing must be at most 16777215 (a negative one means the style's)"),
    ("QLayout+|QWidget+", r"setContentsMargins", r"int,int,int,int",
     "qtc::in({0}, -16777215, 16777215) && qtc::in({1}, -16777215, 16777215) && "
     "qtc::in({2}, -16777215, 16777215) && qtc::in({3}, -16777215, 16777215)",
     OOR, "each margin must be in [-16777215, 16777215]"),
    ("QLayout+|QWidget+", r"setContentsMargins", r"QMargins",
     "qtc::in({0}.left(), -16777215, 16777215) && qtc::in({0}.top(), -16777215, 16777215) && "
     "qtc::in({0}.right(), -16777215, 16777215) && qtc::in({0}.bottom(), -16777215, 16777215)",
     OOR, "each margin must be in [-16777215, 16777215]"),
    # Qt Charts' model mappers write a series' changes into their model without checking that
    # there is one (a crash in QCandlestickModelMapperPrivate::candlestickSetsAdded and the like):
    # a mapper with a series has a model, and when the model is destroyed the series goes first
    ("QXYModelMapper+|QBarModelMapper+|QPieModelMapper+|QBoxPlotModelMapper+|QCandlestickModelMapper+", r"setSeries",
     None, "{0} == nullptr || {s}->model() != nullptr", STATE, "set the model first: the mapper writes the series' changes into it"),
    # these only stop following the series and keep pointing at it: once it is destroyed, the
    # next model they get is filled from the dead series
    ("QXYModelMapper+|QBarModelMapper+|QPieModelMapper+|QBoxPlotModelMapper+", r"setSeries", None, "{0} != nullptr", BAD,
     "this mapper cannot let go of its series (Qt keeps pointing at it): destroy the mapper instead"),
    ("QXYModelMapper+|QBarModelMapper+|QPieModelMapper+|QBoxPlotModelMapper+|QCandlestickModelMapper+", r"setModel",
     None, "{0} != nullptr || {s}->series() == nullptr", STATE,
     "a mapper with a series keeps a model: it writes the series' changes into it"),
    # Widget and window positions and geometries: Qt keeps rectangles as corners in int, so a
    # corner near INT_MAX wraps the far edge and painting then gets an invalid backing store (a
    # crash in QWidgetPrivate::setSystemClip). Coordinates stay within +-16777215, like sizes.
    ("QWidget+|QWindow+", r"move|setPosition|setFramePosition", r"int,int",
     "qtc::in({0}, -16777215, 16777215) && qtc::in({1}, -16777215, 16777215)", OOR,
     "coordinates must be in [-16777215, 16777215]"),
    ("QWidget+|QWindow+", r"move|setPosition|setFramePosition", r"QPoint",
     "qtc::in({0}.x(), -16777215, 16777215) && qtc::in({0}.y(), -16777215, 16777215)", OOR,
     "coordinates must be in [-16777215, 16777215]"),
    ("QWindow+", r"setX|setY", r"int", "qtc::in({0}, -16777215, 16777215)", OOR,
     "coordinates must be in [-16777215, 16777215]"),
    ("QWidget+|QWindow+", r"setGeometry", r"int,int,int,int",
     "qtc::in({0}, -16777215, 16777215) && qtc::in({1}, -16777215, 16777215)", OOR,
     "coordinates must be in [-16777215, 16777215]"),
    ("QWidget+|QWindow+|QLayout+", r"setGeometry", r"QRect",
     "qtc::in({0}.left(), -16777215, 16777215) && qtc::in({0}.top(), -16777215, 16777215) && "
     "qtc::in({0}.right(), -2 * 16777215, 2 * 16777215) && qtc::in({0}.bottom(), -2 * 16777215, 2 * 16777215)",
     OOR, "the rectangle must lie within +-16777215 (its corner) and +-33554430 (its far edge)"),
    ("QLayout+", r"setGeometry|activate", None, "qtc::layout_sums_ok({s})", LIMIT,
     "the layout's items add up to more than Qt's layout engine can count (its distribution would not finish)"),
    ("QWidget+", r"show|showNormal|showMaximized|showFullScreen|showMinimized|adjustSize", r"", "qtc::layouts_ok_below({s})", LIMIT,
     "a layout in it adds up to more than Qt's layout engine can count (laying it out would not finish)"),
    ("QWidget+", r"setVisible", r"bool", "!{0} || qtc::layouts_ok_below({s})", LIMIT,
     "a layout in it adds up to more than Qt's layout engine can count (laying it out would not finish)"),
    ("QGraphicsGridLayout", r"setRowStretchFactor|setColumnStretchFactor", None, "{1} >= 0", OOR,
     "the stretch must not be negative"),
    # cutting rows or columns through merged cells leaves spans Qt later trips over (a crash in
    # the document's fragment map once the table is resized again)
    ("QTextTable", r"removeRows|removeColumns", None, "qtc::table_unmerged({s})", STATE,
     "the table has merged cells: split them first (Qt corrupts the document removing rows or columns across them)"),
    ("QTextTable", r"resize", None, "({0} >= {s}->rows() && {1} >= {s}->columns()) || qtc::table_unmerged({s})", STATE,
     "the table has merged cells: split them first (Qt corrupts the document shrinking a table across them)"),
    # adding columns next to a cell merged across several rows and columns builds a grid Qt then
    # reads past the end of (resize adds columns the same way)
    ("QTextTable", r"insertColumns|appendColumns", None, "qtc::table_columns_addable({s})", STATE,
     "a cell spans several rows and columns: split it first (Qt corrupts the table adding columns next to it)"),
    ("QTextTable", r"resize", None, "{1} <= {s}->columns() || qtc::table_columns_addable({s})", STATE,
     "a cell spans several rows and columns: split it first (Qt corrupts the table adding columns next to it)"),
    # replace(pos, n, ...) moves memory by n: a negative n is not clamped
    ("QString|QByteArray", r"replace", r"qsizetype,qsizetype,.*", "{1} >= 0", OOR, "n must not be negative"),
    # discard(z) draws z numbers one by one
    ("QRandomGenerator|QRandomGenerator64", r"discard", None, "quint64({0}) <= (quint64(1) << 26)", LIMIT,
     "at most 2^26 numbers at a time"),
    # the tool box keeps a page per call, and drops one when the widget dies: a widget added twice
    # leaves a page behind with a dangling pointer
    ("QToolBox", r"addItem", None, "{0} == nullptr || {s}->indexOf({0}) < 0", STATE, "the widget is in the tool box already"),
    ("QToolBox", r"insertItem", None, "{1} == nullptr || {s}->indexOf({1}) < 0", STATE, "the widget is in the tool box already"),
    # with the delegate drawing the label, Qt's default combo delegate crashes on size hints and
    # paints whenever there is no current item (an empty box, after clear())
    ("QComboBox", r"setLabelDrawingMode", None, "{0} == QComboBox::LabelDrawingMode::UseStyle", "QTC_ERR_UNSUPPORTED",
     "UseDelegate is not available: Qt crashes painting a combo box without a current item in that mode"),
    # a widget in two subwindows has both filtering its events: they set each other's state forever
    ("QMdiArea", r"addSubWindow", None,
     "{0} == nullptr || qobject_cast<QMdiSubWindow *>({0}) != nullptr || qobject_cast<QMdiSubWindow *>({0}->parentWidget()) == nullptr",
     STATE, "the widget is in an MDI subwindow already: take it out first (QMdiSubWindow::setWidget(nullptr))"),
    ("QMdiSubWindow", r"setWidget", None,
     "{0} == nullptr || {0}->parentWidget() == {s} || qobject_cast<QMdiSubWindow *>({0}->parentWidget()) == nullptr",
     STATE, "the widget is in another MDI subwindow: take it out first"),
    # a QStandardItem keeps its children in one row-major array and inserts a column row by row,
    # moving everything after it each time: rows x rows x columns
    ("QStandardItemModel", r"insertColumns", r"int,int(,QModelIndex)?",
     "qtc::std_columns_ok(qtc::rows_of({s}, {2|QModelIndex()}), int64_t(qtc::cols_of({s}, {2|QModelIndex()})) + {1})",
     LIMIT, "inserting columns into this many rows takes too long in Qt (rows x rows x columns over 2^30): set the columns before adding rows"),
    ("QStandardItemModel", r"insertColumn|appendColumn", None,
     "qtc::std_columns_ok(int64_t({s}->rowCount()), int64_t({s}->columnCount()) + 1)", LIMIT,
     "inserting columns into this many rows takes too long in Qt (rows x rows x columns over 2^30): set the columns before adding rows"),
    ("QStandardItemModel", r"setColumnCount", None,
     "{0} == {s}->columnCount() || qtc::std_columns_ok(int64_t({s}->rowCount()), std::max<int64_t>({0}, {s}->columnCount()))", LIMIT,
     "adding or removing columns in this many rows takes too long in Qt (rows x rows x columns over 2^30): set the columns before adding rows"),
    # removing a column goes row by row the same way
    ("QStandardItemModel", r"removeColumns", r"int,int(,QModelIndex)?",
     "{1} <= 0 || qtc::std_columns_ok(qtc::rows_of({s}, {2|QModelIndex()}), qtc::cols_of({s}, {2|QModelIndex()}))",
     LIMIT, "removing columns from this many rows takes too long in Qt (rows x rows x columns over 2^30): remove rows first"),
    ("QStandardItem", r"removeColumns", None, "{1} <= 0 || qtc::std_columns_ok(int64_t({s}->rowCount()), int64_t({s}->columnCount()))",
     LIMIT, "removing columns from this many rows takes too long in Qt (rows x rows x columns over 2^30): remove rows first"),
    ("QStandardItem", r"removeColumn|takeColumn", None, "qtc::std_columns_ok(int64_t({s}->rowCount()), int64_t({s}->columnCount()))",
     LIMIT, "removing columns from this many rows takes too long in Qt (rows x rows x columns over 2^30): remove rows first"),
    ("QStandardItemModel", r"takeColumn", None, "qtc::std_columns_ok(int64_t({s}->rowCount()), int64_t({s}->columnCount()))",
     LIMIT, "removing columns from this many rows takes too long in Qt (rows x rows x columns over 2^30): remove rows first"),
    ("QStandardItem", r"insertColumns", None, "qtc::std_columns_ok(int64_t({s}->rowCount()), int64_t({s}->columnCount()) + {1})",
     LIMIT, "inserting columns into this many rows takes too long in Qt (rows x rows x columns over 2^30): set the columns before adding rows"),
    ("QStandardItem", r"insertColumn|appendColumn", None,
     "qtc::std_columns_ok(int64_t({s}->rowCount()), int64_t({s}->columnCount()) + 1)", LIMIT,
     "inserting columns into this many rows takes too long in Qt (rows x rows x columns over 2^30): set the columns before adding rows"),
    ("QStandardItem", r"setColumnCount", None,
     "{0} == {s}->columnCount() || qtc::std_columns_ok(int64_t({s}->rowCount()), std::max<int64_t>({0}, {s}->columnCount()))",
     LIMIT, "adding or removing columns in this many rows takes too long in Qt (rows x rows x columns over 2^30): set the columns before adding rows"),
    # when the copy fails (a descriptor that is not open), Qt closes the descriptor it holds but keeps
    # its number, and closes that number again later: by then it is someone else's (Qt's own event
    # loop wakeups, found as a hang at exit)
    ("QDBusUnixFileDescriptor", r"setFileDescriptor|giveFileDescriptor", None, "qtc::fd_open({0})", BAD,
     "the descriptor is not open"),
    # these take the descriptor over and close it later: it must at least be open (whether it is the
    # program's to give, qtc cannot tell)
    ("QAbstractSocket+|QLocalSocket+|QTcpServer+", r"setSocketDescriptor", None, "qtc::fd_open({0})", BAD,
     "the descriptor is not open"),
    ("QLocalServer", r"listen", r"(qintptr|long long)", "qtc::fd_open({0})", BAD, "the descriptor is not open"),
    ("QFile+", r"open", r"int,.*", "qtc::fd_open({0})", BAD, "the descriptor is not open"),
    # Qt looks the main window's dock layout up unchecked
    ("QDockWidget", r"setDockLocation", None, "qobject_cast<QMainWindow *>({s}->parentWidget()) != nullptr", STATE,
     "the dock widget is not in a main window"),
    # it inserts column by column, moving every later column each time: quadratic in the count
    ("QSqlQueryModel", r"insertColumns", None, "{1} <= 4096", LIMIT, "at most 4096 columns at a time"),
    # Images: Qt checks their size only for overflow; a large one is allocated, then touched, and
    # the system kills the process. Each image a call makes stays within the allocation limit.
    ("QImage", r"<ctor>", r"int,int,QImage::Format", "qtc::pixels_ok({0}, {1}, qtc::image_bpp({2}))", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QImage", r"<ctor>", r"QSize,QImage::Format", "qtc::pixels_ok({0}.width(), {0}.height(), qtc::image_bpp({1}))", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QImage", r"scaled", r"int,int(,.*)?",
     "qtc::scaled_ok({s}->size(), {0}, {1}, {2|Qt::IgnoreAspectRatio}, qMax(4.0, qtc::image_bpp({s}->format())))", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QImage", r"scaled", r"QSize(,.*)?",
     "qtc::scaled_ok({s}->size(), {0}.width(), {0}.height(), {1|Qt::IgnoreAspectRatio}, qMax(4.0, qtc::image_bpp({s}->format())))",
     LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QImage", r"scaledToWidth", None,
     "{s}->width() <= 0 || qtc::pixels_ok({0}, double({0}) * {s}->height() / {s}->width(), qMax(4.0, qtc::image_bpp({s}->format())))",
     LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QImage", r"scaledToHeight", None,
     "{s}->height() <= 0 || qtc::pixels_ok(double({0}) * {s}->width() / {s}->height(), {0}, qMax(4.0, qtc::image_bpp({s}->format())))",
     LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QImage", r"copy", r"QRect", "qtc::pixels_ok({0|QRect()}.width(), {0|QRect()}.height(), qtc::image_bpp({s}->format()))", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QImage", r"copy", r"int,int,int,int", "qtc::pixels_ok({2}, {3}, qtc::image_bpp({s}->format()))", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QImage", r"transformed", None, "qtc::transform_ok({s}->size(), {0}, qMax(4.0, qtc::image_bpp({s}->format())))", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QImage", r"convertToFormat|convertedTo|convertTo", r"QImage::Format(,.*)?",
     "qtc::pixels_ok({s}->width(), {s}->height(), qtc::image_bpp({0}))", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QPixmap", r"<ctor>", r"int,int", "qtc::pixels_ok({0}, {1}, 4)", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QPixmap", r"<ctor>", r"QSize", "qtc::pixels_ok({0}.width(), {0}.height(), 4)", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QPixmap", r"scaled", r"int,int(,.*)?", "qtc::scaled_ok({s}->size(), {0}, {1}, {2|Qt::IgnoreAspectRatio}, 4)", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QPixmap", r"scaled", r"QSize(,.*)?",
     "qtc::scaled_ok({s}->size(), {0}.width(), {0}.height(), {1|Qt::IgnoreAspectRatio}, 4)", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QPixmap", r"scaledToWidth", None, "{s}->width() <= 0 || qtc::pixels_ok({0}, double({0}) * {s}->height() / {s}->width(), 4)",
     LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QPixmap", r"scaledToHeight", None, "{s}->height() <= 0 || qtc::pixels_ok(double({0}) * {s}->width() / {s}->height(), {0}, 4)",
     LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QPixmap", r"copy", r"QRect", "qtc::pixels_ok({0|QRect()}.width(), {0|QRect()}.height(), 4)", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QPixmap", r"copy", r"int,int,int,int", "qtc::pixels_ok({2}, {3}, 4)", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QPixmap+", r"transformed", None, "qtc::transform_ok({s}->size(), {0}, 4)", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QPixmap+", r"fromImage", None, "qtc::pixels_ok({0}.width(), {0}.height(), 4)", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QBitmap", r"<ctor>", r"int,int", "qtc::pixels_ok({0}, {1}, 1)", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QBitmap", r"<ctor>", r"QSize", "qtc::pixels_ok({0}.width(), {0}.height(), 1)", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QBitmap", r"transformed", None, "qtc::transform_ok({s}->size(), {0}, 1)", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QIcon", r"pixmap", r"QSize(,QIcon::Mode(,QIcon::State)?)?",
     "qtc::pixels_ok({0}.width() * qtc::app_dpr(), {0}.height() * qtc::app_dpr(), 4)", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QIcon", r"pixmap", r"int,int(,.*)?", "qtc::pixels_ok({0} * qtc::app_dpr(), {1} * qtc::app_dpr(), 4)", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QIcon", r"pixmap", r"int(,QIcon::Mode(,QIcon::State)?)?", "qtc::pixels_ok({0} * qtc::app_dpr(), {0} * qtc::app_dpr(), 4)",
     LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QIcon", r"pixmap", r"QSize,qreal(,.*)?", "qtc::pixels_ok({0}.width() * {1}, {0}.height() * {1}, 4)", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QIcon", r"pixmap", r"QWindow\*,QSize(,.*)?",
     "qtc::pixels_ok({1}.width() * qtc::app_dpr(), {1}.height() * qtc::app_dpr(), 4)", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QWidget", r"grab", None, "qtc::grab_ok({s}, {0|QRect(QPoint(0, 0), QSize(-1, -1))})", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    # an empty rectangle that is not a null one (QRect(0, 1, 2, 0)) still "intersects" the widget:
    # Qt paints into the null pixmap, which crashes where a graphics effect draws its source
    ("QWidget", r"grab", None, "{0}.isNull() || {0}.width() < 0 || {0}.height() < 0 || !{0}.isEmpty()", BAD,
     "the rectangle is empty (zero width or height): pass a null rectangle or one with an area"),
    ("QQuickWindow", r"grabWindow", None,
     "qtc::pixels_ok(double({s}->width()) * {s}->devicePixelRatio(), double({s}->height()) * {s}->devicePixelRatio(), 4)",
     LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QQuickWidget|QOpenGLWidget", r"grabFramebuffer", None,
     "qtc::pixels_ok(double({s}->width()) * {s}->devicePixelRatioF(), double({s}->height()) * {s}->devicePixelRatioF(), 4)",
     LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QQuickItem", r"grabToImage", None,
     "qtc::pixels_ok({1|QSize()}.isValid() ? double({1|QSize()}.width()) : {s}->width(), "
     "{1|QSize()}.isValid() ? double({1|QSize()}.height()) : {s}->height(), 4)", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    ("QScreen", r"grabWindow", None, "qtc::pixels_ok({3|-1}, {4|-1}, 4)", LIMIT, "the image would exceed the allocation limit (qtc_set_alloc_limit)"),
    # windows get a backing store (or render target) of their size when they show
    ("QWidget|QRubberBand", r"resize|setFixedSize|setMinimumSize", r"int,int", "qtc::window_size_ok({s}, {0}, {1})", LIMIT, "the window would exceed the allocation limit (qtc_set_alloc_limit): it gets a backing store of its size"),
    ("QWidget|QRubberBand", r"resize|setFixedSize|setMinimumSize", r"QSize", "qtc::window_size_ok({s}, {0}.width(), {0}.height())",
     LIMIT, "the window would exceed the allocation limit (qtc_set_alloc_limit): it gets a backing store of its size"),
    ("QWidget|QRubberBand", r"setGeometry", r"int,int,int,int", "qtc::window_size_ok({s}, {2}, {3})", LIMIT, "the window would exceed the allocation limit (qtc_set_alloc_limit): it gets a backing store of its size"),
    ("QWidget|QRubberBand", r"setGeometry", r"QRect", "qtc::window_size_ok({s}, {0}.width(), {0}.height())", LIMIT, "the window would exceed the allocation limit (qtc_set_alloc_limit): it gets a backing store of its size"),
    ("QWidget", r"setFixedWidth|setMinimumWidth", r"int",
     "qtc::window_size_ok({s}, {0}, qMax({s}->height(), {s}->minimumHeight()))", LIMIT, "the window would exceed the allocation limit (qtc_set_alloc_limit): it gets a backing store of its size"),
    ("QWidget", r"setFixedHeight|setMinimumHeight", r"int",
     "qtc::window_size_ok({s}, qMax({s}->width(), {s}->minimumWidth()), {0})", LIMIT, "the window would exceed the allocation limit (qtc_set_alloc_limit): it gets a backing store of its size"),
    ("QWidget", r"show|showNormal|showMaximized|showFullScreen|showMinimized", r"", "qtc::window_show_ok({s})", LIMIT, "the window would exceed the allocation limit (qtc_set_alloc_limit): it gets a backing store of its size"),
    ("QWidget|QDialog|QColorDialog|QFileDialog|QFontDialog|QInputDialog|QMenuBar|QSizeGrip|QWizard|QPrintDialog|"
     "QPrintPreviewDialog|QPrintPreviewWidget|QHeaderView", r"setVisible", r"bool", "!{0} || qtc::window_show_ok({s})", LIMIT, "the window would exceed the allocation limit (qtc_set_alloc_limit): it gets a backing store of its size"),
    ("QWindow", r"resize|setMinimumSize", r"QSize", "qtc::qwindow_size_ok({s}, {0}.width(), {0}.height())", LIMIT, "the window would exceed the allocation limit (qtc_set_alloc_limit): it gets a backing store of its size"),
    ("QWindow", r"resize", r"int,int", "qtc::qwindow_size_ok({s}, {0}, {1})", LIMIT, "the window would exceed the allocation limit (qtc_set_alloc_limit): it gets a backing store of its size"),
    ("QWindow", r"setGeometry", r"int,int,int,int", "qtc::qwindow_size_ok({s}, {2}, {3})", LIMIT, "the window would exceed the allocation limit (qtc_set_alloc_limit): it gets a backing store of its size"),
    ("QWindow", r"setGeometry", r"QRect", "qtc::qwindow_size_ok({s}, {0}.width(), {0}.height())", LIMIT, "the window would exceed the allocation limit (qtc_set_alloc_limit): it gets a backing store of its size"),
    ("QWindow", r"setWidth|setMinimumWidth", r"int",
     "qtc::qwindow_size_ok({s}, {0}, qMax({s}->height(), {s}->minimumHeight()))", LIMIT, "the window would exceed the allocation limit (qtc_set_alloc_limit): it gets a backing store of its size"),
    ("QWindow", r"setHeight|setMinimumHeight", r"int",
     "qtc::qwindow_size_ok({s}, qMax({s}->width(), {s}->minimumWidth()), {0})", LIMIT, "the window would exceed the allocation limit (qtc_set_alloc_limit): it gets a backing store of its size"),
    ("QWindow", r"show|showNormal|showMaximized|showFullScreen|showMinimized", r"", "qtc::qwindow_show_ok({s})", LIMIT, "the window would exceed the allocation limit (qtc_set_alloc_limit): it gets a backing store of its size"),
    ("QWindow", r"setVisible", r"bool", "!{0} || qtc::qwindow_show_ok({s})", LIMIT, "the window would exceed the allocation limit (qtc_set_alloc_limit): it gets a backing store of its size"),
    ("QDialog+", r"open|exec", r"", "qtc::window_show_ok({s})", LIMIT,
     "the window would exceed the allocation limit (qtc_set_alloc_limit): it gets a backing store of its size"),
    ("QDialog+", r"open|exec", r"", "qtc::layouts_ok_below({s})", LIMIT,
     "a layout in it adds up to more than Qt's layout engine can count (laying it out would not finish)"),
    # the getters read their lists without a bounds check
    ("QGridLayout", r"rowStretch|rowMinimumHeight", r"int", "{0} >= 0 && {0} < {s}->rowCount()", OOR,
     "row out of range [0, rowCount())"),
    ("QGridLayout", r"columnStretch|columnMinimumWidth", r"int", "{0} >= 0 && {0} < {s}->columnCount()", OOR,
     "column out of range [0, columnCount())"),
    # QWizardPage indexes the wizard's buttons with it unchecked (QWizard's own methods check it)
    ("QWizardPage", r"setButtonText", None, "{0} >= 0 && {0} < QWizard::NButtons", OOR,
     "not a button: Stretch and NoButton name none"),
    # the current group indexes the palette's brushes: Current, All and NColorGroups are no groups
    ("QPalette", r"setCurrentColorGroup", None, "{0} >= 0 && {0} < QPalette::NColorGroups", OOR,
     "the current group must be Active, Disabled or Inactive"),
    # QWizard takes its side widget into its layout only when it next restyles (with a watermark),
    # in the middle of QApplication::setStyle going over every widget: a splitter the widget
    # leaves then deletes a handle Qt is about to restyle (crash)
    ("QWizard+", r"setSideWidget", None, "!{0} || !{0}->parentWidget() || {s}->isAncestorOf({0})", STATE,
     "the side widget must have no parent (or be in the wizard already); take it out of its parent first"),
    # Qt's NMEA sources read their device while it says bytes are available: open without reading
    # it says so while every read fails, and the simulation reader spins forever
    ("QNmeaPositionInfoSource|QNmeaSatelliteInfoSource", r"setDevice", None,
     "!{0} || !{0}->isOpen() || {0}->isReadable()", STATE, "the device is open but not for reading"),
    ("QNmeaPositionInfoSource|QNmeaSatelliteInfoSource", r"startUpdates|requestUpdate", None,
     "!{s}->device() || !{s}->device()->isOpen() || {s}->device()->isReadable()", STATE,
     "the device is open but not for reading"),
    ("QIODevice+", r"open", r"(QIODevice::)?OpenMode|QIODeviceBase::OpenMode|QFlags<QIODeviceBase::OpenModeFlag>",
     "qtc::nmea_open_ok({s}, int({0}))", STATE, "an NMEA source reads this device: open it for reading"),
    # QMenu::actionAt reads its cached action rectangles, which a hidden menu refreshes only on
    # layout: after clear() it hands out the actions just deleted. sizeHint() refreshes them.
    ("QMenu+", r"actionAt", None, "({s}->sizeHint(), true)", STATE, "unreachable"),
    # a dock widget tabified with (or split from) itself: Qt makes a floating tab group around it
    # that no close reaches, and QApplication::closeAllWindows retries it forever
    ("QMainWindow+", r"tabifyDockWidget|splitDockWidget", None, "{0} != {1}", BAD,
     "the two dock widgets must differ"),
    # Qt keeps its widget state in attributes (a window marked visible without a native window
    # crashes QApplication::closeAllWindows; one marked created, setWindowTitle)
    ("QWidget+", r"setAttribute", r"Qt::WidgetAttribute(,bool)?", "qtc::widget_attribute_settable({0})", OOR,
     "Qt sets and clears this attribute itself (the widget's state, or bookkeeping Qt documents as its own)"),
    # operations that visit every match take about a microsecond per match
    ("QString", r"lastIndexOf|section|replace|remove|split", r"QRegularExpression(,.*)?", "{s}->size() <= (1 << 20)", LIMIT,
     "operations with a regular expression that visit every match take about a microsecond per match: at most "
     "1,048,576 characters (iterate QRegularExpression::globalMatch over longer text)"),
    # QMdiSubWindow makes a menu that is not its child a child widget (setParent), and showing the
    # system menu then crashes in QMenu::popup
    ("QMdiSubWindow", r"setSystemMenu", None, "!{0} || ({0}->parent() == {s} && {0}->isWindow())", STATE,
     "the system menu must be a menu made with this subwindow as its parent"),
    ("QMdiSubWindow", r"showSystemMenu", None, "!{s}->systemMenu() || {s}->systemMenu()->isWindow()", STATE,
     "the system menu is no longer a popup window"),
    # QMessageBox keeps plain pointers to these buttons, also after they are deleted
    # emitted by hand, these change the chart item's own copy of the points or values without the
    # series changing: it then reads past the end (an added or removed point, also right after the
    # series left its chart, whose item lives until the next deferred delete), or
    # takes the index as given (a replaced point, a changed value)
    ("QXYSeries", r"pointAdded|pointRemoved|pointsRemoved", None, "false", STATE,
     "the series emits this itself when its points change; emitted by hand, the copy a chart keeps of the points "
     "(also one the series has just left) no longer matches"),
    ("QXYSeries", r"pointReplaced", None, "qtc::in({0}, 0, int64_t({s}->count()) - 1)", OOR,
     "index out of range [0, count())"),
    ("QBarSet", r"valuesAdded|valuesRemoved", None, "false", STATE,
     "the set emits this itself when its values change; emitted by hand, the bar charts' copy no longer matches"),
    ("QBarSet", r"valueChanged", r"int", "qtc::in({0}, 0, int64_t({s}->count()) - 1)", OOR,
     "index out of range [0, count())"),
    # emitted by hand, contentsChange has the document's layouts lay out the range it names
    ("QTextDocument", r"contentsChange", None,
     "qtc::in({0}, 0, {s}->characterCount()) && qtc::in({1}, 0, {s}->characterCount()) && "
     "qtc::in({2}, 0, {s}->characterCount() - {0})", OOR, "the range must lie within the document"),
    # a disk cache owns its directory: expire() and clear() delete every file ending in .d under
    # it, so neither the root of the file system nor the home directory can be one
    ("QNetworkDiskCache", r"setCacheDirectory", None,
     "[&] { const QString d = QDir({0}).absolutePath(); return {0}.isEmpty() || (QDir(d) != QDir::root() && QDir(d) != QDir::home()); }()",
     BAD, "the cache deletes every file ending in .d under its directory: give it a directory of its own"),
    # a message box keeps its custom buttons in a list of its own: clearing its button box from
    # outside would delete them under it
    ("QDialogButtonBox", r"clear", r"", "!qobject_cast<QMessageBox *>({s}->parentWidget())", STATE,
     "this is a message box's own button box: remove buttons through QMessageBox::removeButton"),
    # QWizard keeps its side widget by a plain pointer, and that widget is not always its child
    ("QWizard", r"sideWidget", r"", "qtc::known_live({s}->sideWidget(), {s})", STATE,
     "the side widget was destroyed (Qt keeps pointing at it)"),
    # toFillPolygons compares every subpath with every other (16,384 take about a second)
    ("QPainterPath", r"toFillPolygons", None,
     "[&] { int64_t n = 0; for (int i = 0, c = {s}->elementCount(); i < c; i++) n += {s}->elementAt(i).isMoveTo(); "
     "return n <= 16384; }()", LIMIT, "at most 16,384 subpaths (it compares every subpath with every other)"),
    # Regions: a rectangle's far edge must not wrap (an ellipse region then takes a polygon from
    # an empty path), and a polygon region costs about its height times the edges crossing each
    # line, which for a self-crossing polygon over a large area takes minutes
    ("QRegion", r"<ctor>", r"int,int,int,int(,.*)?",
     "qtc::in({0}, -16777215, 16777215) && qtc::in({1}, -16777215, 16777215) && "
     "qtc::in({2}, -16777215, 16777215) && qtc::in({3}, -16777215, 16777215)", OOR,
     "coordinates and sizes must be in [-16777215, 16777215]"),
    ("QRegion", r"<ctor>", r"QRect(,.*)?",
     "qtc::in({0}.left(), -16777215, 16777215) && qtc::in({0}.top(), -16777215, 16777215) && "
     "qtc::in({0}.right(), -2 * 16777215, 2 * 16777215) && qtc::in({0}.bottom(), -2 * 16777215, 2 * 16777215)", OOR,
     "the rectangle must lie within +-16777215 (its corner) and +-33554430 (its far edge)"),
    ("QRegion", r"<ctor>", r"QPolygon(,.*)?",
     "[&] { const QPolygon &p = {0}; const qsizetype n = p.size(); int64_t dy = 0, x0 = INT64_MAX, x1 = INT64_MIN, "
     "y0 = INT64_MAX, y1 = INT64_MIN; for (qsizetype i = 0; i < n; i++) { const QPoint a = p[i], b = p[(i + 1) % n]; "
     "dy += std::abs(int64_t(b.y()) - a.y()); x0 = std::min<int64_t>(x0, a.x()); x1 = std::max<int64_t>(x1, a.x()); "
     "y0 = std::min<int64_t>(y0, a.y()); y1 = std::max<int64_t>(y1, a.y()); } "
     "return n < 3 || (dy <= (int64_t(1) << 19) && x1 - x0 <= 16384 && y1 - y0 <= 16384); }()", LIMIT,
     "the polygon must fit in 16384 x 16384 and its edges cross at most 2^19 lines in all (a complex region takes minutes)"),
    ("QMessageBox", r"clickedButton", r"", "qtc::known_live({s}->clickedButton(), {s})", STATE,
     "the clicked button was destroyed"),
    ("QMessageBox", r"defaultButton", r"", "qtc::known_live({s}->defaultButton(), {s})", STATE,
     "the default button was destroyed"),
    ("QMessageBox", r"escapeButton", r"", "qtc::known_live({s}->escapeButton(), {s})", STATE,
     "the escape button was destroyed"),
    # a backward search for whole words with a regular expression tries every earlier position, and
    # each try matches from the start of the block: block length x block length steps
    ("QTextEdit|QPlainTextEdit", r"find", r"QRegularExpression(,.*)?", "!(({1|QTextDocument::FindFlags()}) & QTextDocument::FindBackward) || !(({1|QTextDocument::FindFlags()}) & QTextDocument::FindWholeWords) || [&] { int64_t t = 0; for (QTextBlock b = {s}->document()->begin(); b.isValid(); b = b.next()) { t += int64_t(b.length()) * b.length(); if (t > (int64_t(1) << 24)) return false; } return true; }()", LIMIT,
     "a backward whole-word search with a regular expression takes block length x block length steps: the squares of the blocks' lengths must add up to at most 2^24 (one block of 4096 characters)"),
    ("QTextDocument", r"find", r"QRegularExpression,.*", "!(({2|QTextDocument::FindFlags()}) & QTextDocument::FindBackward) || !(({2|QTextDocument::FindFlags()}) & QTextDocument::FindWholeWords) || [&] { int64_t t = 0; for (QTextBlock b = {s}->begin(); b.isValid(); b = b.next()) { t += int64_t(b.length()) * b.length(); if (t > (int64_t(1) << 24)) return false; } return true; }()", LIMIT,
     "a backward whole-word search with a regular expression takes block length x block length steps: the squares of the blocks' lengths must add up to at most 2^24 (one block of 4096 characters)"),
    # counting overlapping matches takes up to size x size steps
    ("QString", r"count", r"QRegularExpression", "{s}->size() <= 8192", LIMIT,
     "count() with a regular expression counts overlapping matches, up to size x size steps: at most 8192 "
     "characters (QRegularExpression::globalMatch counts the matches in longer text)"),
    # a grid layout lays out from a rows x columns array it allocates on every pass
    ("QLayout+", r"addWidget", r"QWidget\*", "qtc::grid_grow_ok({s})",
     LIMIT, "a grid layout must stay under 2^20 cells (rows x columns)"),
    # Qt Charts crashes on an area series in a chart whose boundary line has point label clipping
    # off (6.12 at least): when the area series goes into the chart, when the clipping changes, and
    # until the area series' chart item is deleted after it left the chart. Boundary lines keep it on.
    ("QXYSeries", r"setPointLabelsClipping", None, "{0|true} == {s}->pointLabelsClipping() || qtc::bound_clipping_ok({s})",
     STATE, "the boundary lines of an area series keep point label clipping on (Qt Charts crashes on them otherwise)"),
    ("QChart|QPolarChart", r"addSeries", None, "qtc::area_clipping_ok({0})", STATE,
     "an area series goes into a chart with its upper series, and its boundary lines keep point label clipping on "
     "(Qt Charts crashes otherwise)"),
    # removeAllSeries deletes the series at once, but an area series' chart item lives until the next
    # deferred delete and still follows the boundary lines: a change to one then reads the deleted
    # area series. removeSeries and qtc_destroy (deferred, after the item) are safe.
    ("QChart|QPolarChart", r"removeAllSeries", None,
     "[&] { for (QAbstractSeries *x : {s}->series()) if (x->type() == QAbstractSeries::SeriesTypeArea) return false; return true; }()",
     STATE, "the chart has an area series, whose chart item outlives it here (Qt Charts then crashes on a change to "
     "its boundary lines): remove the area series with removeSeries and destroy them, then call removeAllSeries"),
    ("QAreaSeries", r"setUpperSeries|setLowerSeries", None, "!{0} || {0}->pointLabelsClipping()", STATE,
     "the boundary lines of an area series keep point label clipping on (Qt Charts crashes on them otherwise)"),
    # header labels add the columns or rows they need: columns cost what insertColumns costs
    ("QStandardItemModel", r"setHorizontalHeaderLabels", None,
     "{0}.size() <= {s}->columnCount() || (qtc::std_columns_ok(int64_t({s}->rowCount()), {0}.size()) && "
     "qtc::alloc_ok(int64_t({0}.size()) * qMax(1, {s}->rowCount()), 8))", LIMIT,
     "adding columns to this many rows takes too long in Qt (rows x rows x columns over 2^30): set the columns before adding rows"),
    ("QStandardItemModel", r"setVerticalHeaderLabels", None,
     "{0}.size() <= {s}->rowCount() || qtc::alloc_ok(int64_t({0}.size()) * qMax(1, {s}->columnCount()), 8)", LIMIT,
     "rows x columns too large"),
    # QTableWidget's model inserts a column row by row too
    ("QTableWidget", r"insertColumn", None, "qtc::std_columns_ok(int64_t({s}->rowCount()), int64_t({s}->columnCount()) + 1)",
     LIMIT, "inserting columns into this many rows takes too long in Qt (rows x rows x columns over 2^30): set the columns before adding rows"),
    ("QTableWidget", r"setColumnCount", None, "{0} <= {s}->columnCount() || qtc::std_columns_ok(int64_t({s}->rowCount()), {0})",
     LIMIT, "adding columns to this many rows takes too long in Qt (rows x rows x columns over 2^30): set the columns before adding rows"),
    # QListWidget inserts items one at a time, moving the items after them each time
    ("QListWidget", r"insertItems", None,
     "double({1}.size()) * double(qMax(0, {s}->count() - qMax(0, {0}))) <= double(int64_t(1) << 27)", LIMIT,
     "inserting this many items before this many takes too long in Qt (items x items after them over 2^27)"),
    # the status bar keeps a list of its widgets and drops one entry per widget it loses: a widget
    # added twice stays behind once it is gone
    ("QStatusBar", r"addWidget|addPermanentWidget", None, "{0} == nullptr || {0}->parent() != {s}", STATE,
     "the widget is in the status bar already"),
    ("QStatusBar", r"insertWidget|insertPermanentWidget", None, "{1} == nullptr || {1}->parent() != {s}", STATE,
     "the widget is in the status bar already"),
    # pause() reads playback state that only play() creates (and stop() clears): it aborts otherwise
    ("QAmbientSound|QSpatialSound", r"play", None, "qtc::note_sound({s}, true)", STATE, "-"),
    ("QAmbientSound|QSpatialSound", r"stop", None, "qtc::note_sound({s}, false)", STATE, "-"),
    ("QAmbientSound|QSpatialSound", r"pause", None, "qtc::sound_playing({s})", STATE,
     "the sound is not playing: call play() first (Qt aborts on pause() before play() or after stop())"),
    # the channel count is kept in a short: past 32767 it turns negative, and framesForBytes then
    # divides by a negative frame size (INT_MIN / -1 traps)
    ("QAudioFormat", r"setChannelCount", None, "{0} >= 0 && {0} <= 1024", OOR,
     "channel count must lie in [0, 1024]"),
    # the backing store moves its pixels by the offset without checking it: offsets past any
    # widget size overflow the rectangle arithmetic and copy out of the image
    ("QWidget", r"scroll", None,
     "{0} >= -QWIDGETSIZE_MAX && {0} <= QWIDGETSIZE_MAX && {1} >= -QWIDGETSIZE_MAX && {1} <= QWIDGETSIZE_MAX", OOR,
     "dx and dy must lie in [-16777215, 16777215] (QWIDGETSIZE_MAX)"),
    # ---------------------------------------------------------------- network add-ons (GPL-3.0-only)
    ("QAbstractOAuth2", r"post|put", r"QUrl,QHttpMultiPart\*", "{1} != nullptr", BAD, "the multipart is null"),
    ("QAbstractOAuthReplyHandler", r"networkReplyFinished", None, "{0} != nullptr", BAD, "the reply is null"),
    # a default-constructed request (Qt 6.8+) has no data, and every accessor reads it: requests
    # worth reading come from QHttpServer. The request is one shared-data pointer (when Qt
    # changes that, the check fails safe and refuses every call).
    ("QHttpServerRequest",
     r"value|url|query|method|headers|body|remoteAddress|remotePort|localAddress|localPort|sslConfiguration", None,
     "sizeof(*{s}) == sizeof(void *) && *reinterpret_cast<void *const *>({s}) != nullptr", STATE,
     "the request has no data (a default-constructed request): requests come from QHttpServer"),
    ("QCoapMessage", r"optionAt", None, "{0} >= 0 && {0} < {s}->optionCount()", OOR,
     "index must lie in [0, optionCount())"),
    # RSA-SHA1 and values outside the enum abort (qFatal) on the first signed request
    ("QOAuth1", r"setSignatureMethod", None,
     "{0} == QOAuth1::SignatureMethod::Hmac_Sha1 || {0} == QOAuth1::SignatureMethod::PlainText", BAD,
     "only HMAC-SHA1 and PLAINTEXT are supported (Qt aborts on RSA-SHA1)"),
    # the client casts its transport to the type named without checking: a device of another
    # type, or the websocket types (whose device class is private), crash on connectToHost
    ("QMqttClient", r"setTransport", None,
     "{0} != nullptr && ({1} == QMqttClient::IODevice || ({1} == QMqttClient::AbstractSocket && "
     "{0}->inherits(\"QTcpSocket\")) || ({1} == QMqttClient::SecureSocket && {0}->inherits(\"QSslSocket\")))", BAD,
     "the device must not be null and must suit the transport type: any QIODevice for IODevice, a QTcpSocket "
     "for AbstractSocket, a QSslSocket for SecureSocket (websockets: connectToHostWebSocket)"),
    # the state follows the connection; a client set to Connected writes to a transport it has not
    # made yet
    ("QMqttClient", r"setState", None, "{0} == {s}->state()", STATE,
     "the state follows the connection: use connectToHost and disconnectFromHost"),
    # the client keeps a websocket handed to it by a plain pointer qtc cannot see: it makes its own
    ("QMqttClient", r"connectToHostWebSocket(Encrypted)?", None, "{0} == nullptr", BAD,
     "pass no websocket: the client then makes its own (one passed in could be destroyed under it)"),
]

# Pointer parameters that must not be null: "Class::method" -> parameter indexes (None: every object parameter
# except a trailing parent). Methods named here crash on null in Qt.
NONNULL = {
    "QConcatenateTablesProxyModel::addSourceModel": [0], "QConcatenateTablesProxyModel::removeSourceModel": [0],
    "QAnimationGroup::addAnimation": [0], "QAnimationGroup::insertAnimation": [1], "QAnimationGroup::removeAnimation": [0],
    "QLayout::closestAcceptableSize": [0], "QQuickView::setContent": [1, 2], "QQuickWidget::setContent": [1, 2],
    "QQmlComponent::QQmlComponent": [0],   # an engine: a component without one crashes on load
    # older Qt 6 releases crash on null here
    "QColumnView::setPreviewWidget": [0], "QProxyStyle::polish": [0], "QProxyStyle::unpolish": [0],
    "QCommonStyle::polish": [0], "QCommonStyle::unpolish": [0], "QStyle::polish": [0], "QStyle::unpolish": [0],
    "QAudioRoom::QAudioRoom": [0], "QSpatialSound::QSpatialSound": [0], "QAmbientSound::QAmbientSound": [0],
    "QAudioListener::QAudioListener": [0],
    "QWebChannel::deregisterObject": [0],
    # a null transport goes into the channel's list; publishing to it later reads through it
    "QWebChannel::connectTo": [0], "QWebChannel::disconnectFrom": [0],
    # a handle reads its splitter for every size hint and paint
    # signals a delegate emits: the views listening take the editor as a live widget
    "QAbstractItemDelegate::closeEditor": [0], "QAbstractItemDelegate::commitData": [0],
    # null matches the entries of mapped widgets that were destroyed, and Qt then reads them
    "QDataWidgetMapper::mappedPropertyName": [0], "QDataWidgetMapper::removeMapping": [0],
    "QDataWidgetMapper::mappedSection": [0],
    "QGeoAreaMonitorSource::setPositionInfoSource": [0], "QWidgetAction::releaseWidget": [0],
    "QPieSeries::append": None, "QPieSeries::insert": None, "QPieSeries::remove": None, "QPieSeries::take": None,
    "QAbstractBarSeries::append": None, "QAbstractBarSeries::insert": None, "QAbstractBarSeries::remove": None,
    "QAbstractBarSeries::take": None, "QBoxPlotSeries::append": None, "QBoxPlotSeries::insert": None,
    "QBoxPlotSeries::remove": None, "QBoxPlotSeries::take": None, "QCandlestickSeries::append": None,
    "QCandlestickSeries::insert": None, "QCandlestickSeries::remove": None, "QCandlestickSeries::take": None,
    "QCompleter::setPopup": [0], "QDataWidgetMapper::setModel": [0], "QDataWidgetMapper::setItemDelegate": [0], "QQmlFileSelector::get": [0],
    "QLayout::addWidget": [0], "QGridLayout::addWidget": [0], "QFormLayout::addWidget": [0],
    "QAbstractItemView::setSelectionModel": [0], "QColumnView::setSelectionModel": [0],
    "QTreeView::setSelectionModel": [0], "QTableView::setSelectionModel": [0], "QListView::setSelectionModel": [0],
    "QTreeWidget::setSelectionModel": [0], "QListWidget::setSelectionModel": [0], "QTableWidget::setSelectionModel": [0],
    "QHeaderView::setSelectionModel": [0], "QUndoView::setSelectionModel": [0],
    "QAbstractItemDelegate::setEditorData": [0], "QItemDelegate::setEditorData": [0],
    "QStyledItemDelegate::setEditorData": [0],
    "QAbstractItemDelegate::setModelData": [0, 1], "QItemDelegate::setModelData": [0, 1],
    "QStyledItemDelegate::setModelData": [0, 1],
    "QChart::addSeries": [0], "QChart::setAxisX": [0], "QChart::setAxisY": [0], "QChart::addAxis": [0],
    "QChart::removeSeries": [0], "QChart::removeAxis": [0], "QChartView::setChart": [0],
    # Qt asserts the axis only in debug builds, then reads through it
    "QAbstractSeries::attachAxis": [0], "QAbstractSeries::detachAxis": [0],
    "QDataWidgetMapper::addMapping": [0],
    "QMenu::addMenu": None, "QMenu::insertMenu": None, "QMenu::menuInAction": [0], "QMenuBar::addMenu": None,
    "QMenuBar::insertMenu": None,
    "QSplitter::addWidget": [0], "QSplitter::insertWidget": [1], "QSplitter::replaceWidget": [1],
    "QStackedLayout::addWidget": [0], "QStackedLayout::insertWidget": [1],
    "QStackedWidget::addWidget": [0], "QStackedWidget::insertWidget": [1],
    "QMainWindow::addToolBar": None, "QMainWindow::insertToolBar": None, "QMainWindow::restoreDockWidget": [0],
    "QMainWindow::addDockWidget": None, "QMainWindow::tabifyDockWidget": None, "QMainWindow::splitDockWidget": None,
    "QMainWindow::removeDockWidget": [0], "QMainWindow::removeToolBar": [0],
    "QActionGroup::addAction": None, "QActionGroup::removeAction": [0],
    "QUndoGroup::addStack": [0], "QUndoGroup::removeStack": [0],
    "QButtonGroup::addButton": [0], "QButtonGroup::removeButton": [0], "QButtonGroup::setId": [0],
    "QDialogButtonBox::addButton": None, "QDialogButtonBox::removeButton": [0],
    "QGraphicsProxyWidget::createProxyForChildWidget": [0],
    "QDomDocument::setContent": None, "QMovie::QMovie": None, "QImageReader::imageFormat": None,
    "QOpenGLContext::areSharing": [0, 1], "QWindow::isAncestorOf": [0],
    "QQuickRenderControl::prepareThread": [0],
    "QQmlComponent::QQmlComponent": [0], "QQmlComponent::beginCreate": [0],
    "QQmlExpression::QQmlExpression": [0], "QQmlFileSelector::QQmlFileSelector": [0],
    "QTextFrame::QTextFrame": [0], "QTextList::QTextList": [0], "QTextTable::QTextTable": [0],
    "QTextCursor::QTextCursor": None,
    "QAreaLegendMarker::QAreaLegendMarker": None, "QBarLegendMarker::QBarLegendMarker": None,
    "QBoxPlotLegendMarker::QBoxPlotLegendMarker": None, "QCandlestickLegendMarker::QCandlestickLegendMarker": None,
    "QPieLegendMarker::QPieLegendMarker": None, "QXYLegendMarker::QXYLegendMarker": None,
}

# Methods that move the object argument into the receiver's tree (or take ownership of it):
# passing the receiver or one of its ancestors would create a cycle and hang or crash.
REPARENT_RE = re.compile(r"^(set|add|insert|replace|tabify|split)\w*$")
REPARENT_EXEMPT = {"setBuddy", "setFocusProxy", "setTabOrder", "setParent", "setModel", "setSourceModel",
                   "setSelectionModel", "setTarget", "setTargetObject", "setDefaultAction", "setActiveAction",
                   "setActiveWindow", "setActiveSubWindow", "setCurrentWidget", "setDefaultButton", "setEscapeButton",
                   "setTransientParent", "setScreen", "setFocusWidget", "setCompleter", "setValidator", "setDocument",
                   "setStyle", "setMenuRole", "setButtonGroup", "setItemDelegate", "setItemDelegateForColumn",
                   "setItemDelegateForRow", "setTabKeyNavigation", "setFocus", "setBuddyWidget"}

MODEL_INDEX_ROW_RULES = [
    # (method regex, parameter-type regex, expression, message)
    (r"insertRows", r"int,int(,QModelIndex)?",
     "{0} >= 0 && {1} >= 0 && {1} <= (1 << 20) && {0} <= qtc::rows_of({s}, {2|QModelIndex()})",
     "need 0 <= row <= rowCount() and at most 2^20 rows at a time"),
    (r"insertColumns", r"int,int(,QModelIndex)?",
     "{0} >= 0 && {1} >= 0 && {1} <= (1 << 16) && {0} <= qtc::cols_of({s}, {2|QModelIndex()})",
     "need 0 <= column <= columnCount() and at most 65536 columns at a time"),
    (r"removeRows", r"int,int(,QModelIndex)?",
     "{0} >= 0 && {1} >= 0 && int64_t({0}) + {1} <= qtc::rows_of({s}, {2|QModelIndex()})",
     "the rows must exist"),
    (r"removeColumns", r"int,int(,QModelIndex)?",
     "{0} >= 0 && {1} >= 0 && int64_t({0}) + {1} <= qtc::cols_of({s}, {2|QModelIndex()})",
     "the columns must exist"),
    (r"moveRows", r"QModelIndex,int,int,QModelIndex,int",
     "{1} >= 0 && {2} >= 0 && int64_t({1}) + {2} <= qtc::rows_of({s}, {0}) && {4} >= 0 && {4} <= qtc::rows_of({s}, {3})",
     "the rows and the destination must exist"),
    (r"moveColumns", r"QModelIndex,int,int,QModelIndex,int",
     "{1} >= 0 && {2} >= 0 && int64_t({1}) + {2} <= qtc::cols_of({s}, {0}) && {4} >= 0 && {4} <= qtc::cols_of({s}, {3})",
     "the columns and the destination must exist"),
    # slow insertions of QStringListModel, QStandardItemModel and QSqlQueryModel, also through proxies
    (r"insertRows", r"int,int(,QModelIndex)?", "qtc::insert_cheap({s}, Qt::Vertical, {0}, {1}, {2|QModelIndex()})",
     "inserting this many rows here takes too long or too much memory in Qt (see LIMITS.md)", "LIMIT"),
    (r"insertColumns", r"int,int(,QModelIndex)?", "qtc::insert_cheap({s}, Qt::Horizontal, {0}, {1}, {2|QModelIndex()})",
     "inserting this many columns here takes too long or too much memory in Qt (see LIMITS.md)", "LIMIT"),
    (r"insertRow", r"int(,QModelIndex)?", "qtc::insert_cheap({s}, Qt::Vertical, {0}, 1, {1|QModelIndex()})",
     "inserting a row here takes too long or too much memory in Qt (see LIMITS.md)", "LIMIT"),
    (r"insertColumn", r"int(,QModelIndex)?", "qtc::insert_cheap({s}, Qt::Horizontal, {0}, 1, {1|QModelIndex()})",
     "inserting a column here takes too long or too much memory in Qt (see LIMITS.md)", "LIMIT"),
    (r"insertRow", r"int(,QModelIndex)?", "{0} >= 0 && {0} <= qtc::rows_of({s}, {1|QModelIndex()})", "row out of range"),
    (r"insertColumn", r"int(,QModelIndex)?", "{0} >= 0 && {0} <= qtc::cols_of({s}, {1|QModelIndex()})", "column out of range"),
    (r"removeRow", r"int(,QModelIndex)?", "{0} >= 0 && {0} < qtc::rows_of({s}, {1|QModelIndex()})", "row out of range"),
    (r"removeColumn", r"int(,QModelIndex)?", "{0} >= 0 && {0} < qtc::cols_of({s}, {1|QModelIndex()})", "column out of range"),
    (r"moveRow", r"QModelIndex,int,QModelIndex,int",
     "{1} >= 0 && {1} < qtc::rows_of({s}, {0}) && {3} >= 0 && {3} <= qtc::rows_of({s}, {2})", "row out of range"),
    (r"moveColumn", r"QModelIndex,int,QModelIndex,int",
     "{1} >= 0 && {1} < qtc::cols_of({s}, {0}) && {3} >= 0 && {3} <= qtc::cols_of({s}, {2})", "column out of range"),
]


def inherits(ctx, cn, base, depth=0):
    if cn == base:
        return True
    c = ctx.classes.get(cn)
    if not c or depth > 30:
        return False
    return any(inherits(ctx, b.replace("class ", "").strip(), base, depth + 1) for b in c.get("bases", []))


def expand(expr, nparams):
    """Placeholders -> C++. Returns (code, highest required parameter index) or (None, None) when a
    placeholder names a parameter this overload does not have."""
    used = []
    bad = []

    def rep(mm):
        key = mm.group(1)
        if key == "s":
            return "s"
        i, _, dflt = key.partition("|")
        i = int(i)
        if i >= nparams:
            bad.append(i)
            return "0"
        if dflt:
            return "(n > %d ? static_cast<decltype(%s)>(p%d.get()) : (%s))" % (i, dflt, i, dflt)
        used.append(i)
        return "(p%d.get())" % i

    code = re.sub(r"\{([0-9s][^{}]*)\}", rep, expr)
    if bad:
        return None, None
    return code, (max(used) if used else -1)


def full(rx):
    return re.compile("^(?:%s)$" % rx)


# A class written "QLayout+" also matches every class that inherits it: the rule then reaches the
# overrides a subclass declares (a call on a QHBoxLayout runs QBoxLayout::setGeometry).
def _class_matcher(c):
    if "+" not in c:
        rx = full(c)
        return lambda cn, ctx: bool(rx.match(cn))
    parts = c.split("|")
    exact = {p for p in parts if not p.endswith("+")}
    bases = [p[:-1] for p in parts if p.endswith("+")]
    return lambda cn, ctx: cn in exact or any(inherits(ctx, cn, b) for b in bases)


_TABLE = [(_class_matcher(c), full(m), full(sg) if sg is not None else None, e, code, msg) for c, m, sg, e, code, msg in TABLE]
_ROWS = [(full(r[0]), full(r[1]), r[2], r[3], r[4] if len(r) > 4 else None) for r in MODEL_INDEX_ROW_RULES]


def preconditions(ctx, cn, m, plan, kind):
    """The checks for one bound method or constructor: a list of (C++ condition, max param index, code, message)."""
    is_ctor = kind in ("ctor_obj", "ctor_val", "ctor_ov")
    name = "<ctor>" if is_ctor else m["name"]
    params = plan["params"]
    sig = ",".join(pt.sig for _, pt in params)
    pretty = "%s::%s(%s)" % (cn, cn.split("::")[-1] if is_ctor else name, sig)
    has_self = kind in ("obj", "val")
    out = []

    def add(expr, code, msg):
        if "{s}" in expr and not has_self:
            return
        c, mx = expand(expr, len(params))
        if c is None:
            return
        out.append((c, mx, code, "%s: %s" % (pretty, msg)))

    # ownership of model indexes comes first: the row rules below ask the model about the index's parent
    if has_self and kind == "obj":
        is_model = inherits(ctx, cn, "QAbstractItemModel")
        is_view = inherits(ctx, cn, "QAbstractItemView")
        is_selmodel = inherits(ctx, cn, "QItemSelectionModel")
        is_mapper = inherits(ctx, cn, "QDataWidgetMapper")
        for i, (pn, pt) in enumerate(params):
            if pt.pk != "PK_INDEX":
                continue
            if is_model:
                if name in ("mapFromSource", "mapSelectionFromSource"):
                    if inherits(ctx, cn, "QAbstractProxyModel"):
                        add("qtc::index_of({s}->sourceModel(), {%d})" % i, BAD, "the index belongs to another model than the source model")
                    continue
                add("qtc::index_of({s}, {%d})" % i, BAD, "the index belongs to another model")
            elif is_view or is_selmodel or is_mapper:
                add("qtc::index_of({s}->model(), {%d})" % i, BAD, "the index belongs to another model")
        if name in ("setModelData",) and inherits(ctx, cn, "QAbstractItemDelegate") and len(params) >= 3:
            # the delegate writes into the model at the index: an invalid one reaches the model's
            # setData, which proxies (QConcatenateTablesProxyModel) cannot take
            add("{2}.isValid() && qtc::index_of({1}, {2})", BAD, "the index must be a valid index of the model")
        if is_model and (inherits(ctx, cn, "QAbstractProxyModel") or cn == "QConcatenateTablesProxyModel") and \
                name in ("itemData", "setData", "setItemData") and params and params[0][1].pk == "PK_INDEX":
            # forwarded to the source as an invalid index, which QConcatenateTablesProxyModel cannot take
            add("{0}.isValid()", BAD, "the index must be valid")
        if name in ("setModel", "setSourceModel") and not (is_view or is_mapper or is_selmodel or cn == "QCompleter"):
            for i, (pn, pt) in enumerate(params):
                if pt.pk == "PK_OBJ" and inherits(ctx, pt.meta, "QAbstractItemModel"):
                    add("qobject_cast<const QConcatenateTablesProxyModel *>({%d}) == nullptr" % i, STATE,
                        "QConcatenateTablesProxyModel can only be the model of an item view or a QDataWidgetMapper "
                        "(it crashes on the invalid indexes other users pass it)")
        if is_model:
            for mrx, srx, e, msg, code in _ROWS:
                if mrx.match(name) and srx.match(sig):
                    add(e, LIMIT if code == "LIMIT" else OOR, msg)
            if name in ("setSourceModel", "addSourceModel"):
                for i, (pn, pt) in enumerate(params):
                    if pt.pk == "PK_OBJ":
                        add("qtc::model_no_cycle({s}, {%d})" % i, BAD, "the model would become its own source")
        # reparenting cycles: widgets, layouts and animation groups that take the argument in
        owner = inherits(ctx, cn, "QWidget") or inherits(ctx, cn, "QLayout") or inherits(ctx, cn, "QAnimationGroup")
        if owner and REPARENT_RE.match(name) and name not in REPARENT_EXEMPT and \
                not (cn == "QFocusFrame" and name == "setWidget"):
            for i, (pn, pt) in enumerate(params):
                if pt.pk != "PK_OBJ" or pt.cpp.startswith("const ") or pn == "parent":
                    continue
                if inherits(ctx, pt.meta, "QWidget") or inherits(ctx, pt.meta, "QLayout") or \
                        inherits(ctx, pt.meta, "QAbstractAnimation") or inherits(ctx, pt.meta, "QGraphicsObject"):
                    add("qtc::not_self_or_ancestor({s}, {%d})" % i, BAD, "the argument is the receiver or one of its ancestors")
        # an object another owner holds by a plain pointer (see qtc_roles.cpp) cannot be moved:
        # the old owner would be left pointing at it
        if REPARENT_RE.match(name) and name not in REPARENT_EXEMPT:
            for i, (pn, pt) in enumerate(params):
                if pt.pk == "PK_OBJ" and not pt.cpp.startswith("const ") and pn != "parent":
                    add("qtc::not_held({s}, {%d})" % i, "QTC_ERR_STATE",
                        "the argument is held by another object (a check box, header, viewport, ...); take it from there first")

    # layouts without a parent widget keep a widget added twice twice, and nothing removes a
    # deleted widget from them: a widget may be in one such layout once
    if has_self and kind == "obj" and inherits(ctx, cn, "QLayout") and re.match(r"^(add|insert|set|replace)", name):
        wparams = [i for i, (pn, pt) in enumerate(params) if pt.pk == "PK_OBJ" and inherits(ctx, pt.meta, "QWidget")]
        for i in wparams:
            add("qtc::layout_accepts({s}, {%d})" % i, BAD, "the widget already is in a layout without a parent widget")
        for x in range(len(wparams)):
            for y in range(x + 1, len(wparams)):
                add("{%d} == nullptr || {%d} != {%d}" % (wparams[x], wparams[x], wparams[y]), BAD,
                    "the same widget cannot fill two places")

    # QGridLayout: rows and columns by parameter name
    if cn == "QGridLayout" and has_self:
        pos = {}
        for i, (pn, pt) in enumerate(params):
            if pt.pk != "PK_INT":
                continue
            if pn in ("row", "column", "fromRow", "fromColumn"):
                add("qtc::in({%d}, 0, %d)" % (i, GRID), OOR, "%s must be in [0, %d]" % (pn, GRID))
                pos[pn] = i
            elif pn in ("rowSpan", "columnSpan"):
                add("qtc::in({%d}, -1, %d)" % (i, GRID), OOR, "%s must be in [-1, %d]" % (pn, GRID))
                pos[pn] = i
        # it lays out from a rows x columns array it allocates and clears on every pass: what a
        # call adds to the grid must keep it under 2^20 cells (a span of -1 adds none)
        if not m.get("const"):
            r = next((pos[n] for n in ("row", "fromRow") if n in pos), None)
            c = next((pos[n] for n in ("column", "fromColumn") if n in pos), None)
            span = lambda n: "qMax(1, {%d})" % pos[n] if n in pos else "1"
            if r is not None or c is not None:
                rows = "int64_t({%d}) + %s" % (r, span("rowSpan")) if r is not None else "0"
                cols = "int64_t({%d}) + %s" % (c, span("columnSpan")) if c is not None else "0"
                add("qtc::grid_cells_ok({s}, %s, %s)" % (rows, cols), LIMIT,
                    "a grid layout must stay under 2^20 cells (rows x columns)")
            if name == "setDefaultPositioning":
                add("qtc::grid_cells_ok({s}, {1} == Qt::Horizontal ? 1 : {0}, {1} == Qt::Horizontal ? {0} : 1)", LIMIT,
                    "a grid layout must stay under 2^20 cells (rows x columns)")

    # (const char *data, integer length): the length must not run past the data
    for i in range(len(params) - 1):
        pt, nt = params[i][1], params[i + 1][1]
        if pt.pk == "PK_BYTES" and pt.cpp == "const char *" and nt.pk in ("PK_INT", "PK_UINT") and \
                not (cn == "QBitArray" and name == "fromBits"):
            # -1 means "up to the NUL" only where Qt says so with a default of -1 (QByteArray(data, size = -1));
            # elsewhere (QBuffer::setData) a negative length is undefined behaviour
            raw = m.get("params", [])
            allow_neg = nt.pk == "PK_INT" and i + 1 < len(raw) and bool(raw[i + 1].get("default"))
            if allow_neg:
                add("{%d} == -1 || ({%d} >= 0 && {%d} <= qtc::cstr_len(a[%d]))" % (i + 1, i + 1, i + 1, i), OOR,
                    "the length runs past the data")
            else:
                add("int64_t({%d}) >= 0 && int64_t({%d}) <= qtc::cstr_len(a[%d])" % (i + 1, i + 1, i), OOR,
                    "the length must be in [0, the data's length]")

    # table rules
    for crx, mrx, srx, e, code, msg in _TABLE:
        if mrx.match(name) and (srx is None or srx.match(sig)) and crx(cn, ctx):
            add(e, code, msg)

    # non-null object parameters
    if has_self and name in ("canDropMimeData", "dropMimeData") and params and params[0][1].pk == "PK_OBJ":
        add("{0} != nullptr", BAD, "data must not be null")
    key = "%s::%s" % (cn, cn.split("::")[-1] if is_ctor else name)
    if key not in NONNULL and not is_ctor:
        # an override takes the rule of the method it overrides (a call on a QTableView runs
        # QTableView::setSelectionModel)
        key = next((k for k in NONNULL if k.rsplit("::", 1)[1] == name and k.rsplit("::", 1)[0] != cn
                    and inherits(ctx, cn, k.rsplit("::", 1)[0])), key)
    if key in NONNULL:
        idxs = NONNULL[key]
        for i, (pn, pt) in enumerate(params):
            if pt.pk != "PK_OBJ":
                continue
            if idxs is None:
                if pn == "parent" or (i == len(params) - 1 and pt.meta == "QObject"):
                    continue
            elif i not in idxs:
                continue
            add("{%d} != nullptr" % i, BAD, "argument %d must not be null" % (i + 1))
    return out
