// Overridable objects: instances of generated C++ subclasses of Qt classes (QtcOv_<Class>, see
// gen/overrides.py) whose virtual methods a C (or Goose) program overrides one object at a time.
//
// Each overridden virtual goes through a trampoline: when the object has an override for it, the
// call becomes a frame (pointers to the C++ arguments, a typed return slot) handed to the
// program's handler, which reads the arguments, sets the result or asks for the class's own
// implementation (qtc_ov_*). Objects without overrides, and methods not overridden, cost one
// inline check per call.
#pragma once
#include "qtc_internal.h"

#include <QtCore/QAbstractItemModel>

#include <type_traits>
#include <vector>

QT_BEGIN_NAMESPACE
class QPainter;
class QRhiCommandBuffer;
class QEvent;
QT_END_NAMESPACE

namespace qtc {

enum OvKind : uint8_t {
    OT_VOID = 0,
    OT_BOOL,      // bool
    OT_SINT,      // a signed integer of `size` bytes
    OT_UINT,      // an unsigned integer of `size` bytes
    OT_FLOAT,     // float
    OT_DOUBLE,    // double (qreal)
    OT_ENUM,      // an enum or QFlags: an integer of `size` bytes
    OT_META,      // a copyable value with a metatype (QString, QVariant, QModelIndex, QRect, ...)
    OT_OBJ,       // a pointer to a QObject subclass
    OT_EVENT,     // a pointer to a QEvent subclass
    OT_PAINTER,   // QPainter *
    OT_REF,       // a non-const reference to a value with a metatype: an out-parameter (QString &, int &)
    OT_BUF,       // char *: a buffer to fill; its length is the next parameter
    OT_CBUF,      // const char *: a buffer to read; its length is the next parameter
    OT_OPAQUE,    // anything else: not readable, passed on to the class's own implementation as it is
};

struct OvType {
    uint8_t kind;
    uint8_t size;              // OT_SINT, OT_UINT, OT_ENUM: bytes
    QMetaType (*meta)();       // OT_META, OT_REF (the referenced type), OT_OBJ (the pointer type)
    const char *name;          // the C++ type, for descriptions
    const void *rt = nullptr;  // runtime classes' types: the QMetaType's interface, when meta is null
};
inline QMetaType ov_metatype(const OvType &t) {
    if (t.meta) return t.meta();
    return QMetaType(static_cast<const QtPrivate::QMetaTypeInterface *>(t.rt));
}

template <class T> QMetaType ov_meta() { return QMetaType::fromType<T>(); }

// The descriptor of a parameter or return type T as the generator classified it.
template <class T>
constexpr OvType ov_t(uint8_t kind, const char *name) {
    using V = std::remove_cvref_t<T>;
    if constexpr (std::is_void_v<V>) {
        return OvType{kind, 0, nullptr, name};
    } else {
        if (kind == OT_SINT || kind == OT_UINT || kind == OT_ENUM) return OvType{kind, uint8_t(sizeof(V)), nullptr, name};
        if constexpr (std::is_copy_constructible_v<V> && requires { QMetaType::fromType<V>(); }) {
            if (kind == OT_META || kind == OT_REF || kind == OT_OBJ) return OvType{kind, 0, &ov_meta<V>, name};
        }
        return OvType{kind, 0, nullptr, name};
    }
}

enum OvFlag : uint8_t {
    OVF_PURE = 1,          // no implementation of its own: without a result the call returns a default value
    OVF_ANY_THREAD = 2,    // Qt calls it from other threads too (QThread::run): the handler runs there
    OVF_PAINT = 4,         // qtc_draw_begin(self) paints on the object during the call (paintEvent, paintGL)
    OVF_NONNEG = 8,        // a count: a negative result becomes 0 (rowCount, columnCount)
    OVF_OWN_INDEX = 16,    // returns an index of this model (or an invalid one): others become invalid
};

typedef void (*OvBaseFn)(QObject *self, void **argv, void *ret);

struct OvMethod {
    const char *name;
    const char *sig;           // normalized, e.g. "rowCount(QModelIndex)"
    uint8_t nparams;
    const OvType *params;
    OvType ret;
    OvBaseFn base;             // the class's own implementation; null for pure virtuals
    uint8_t flags;             // OvFlag
};

struct OvClassDef {
    const char *name;          // the Qt class
    const char *module;
    uint8_t need;              // AppNeed
    const MethodDef *ctors;
    uint32_t nctors;
    const OvMethod *methods;   // what its objects can override (inherited ones included)
    uint32_t nmethods;
    const MethodDef *extra;    // protected methods callable (qtc_call) on its objects
    uint32_t nextra;
    // models: createIndex on one of its objects (qtc_ov_ret_index); null for other classes
    QModelIndex (*make_index)(const QObject *self, int row, int column, quintptr id);
};

void register_overridables(const OvClassDef *defs, int count);
const OvClassDef *find_overridable(const QByteArray &name);

// Per object state: a base of every QtcOv_ class.
class OvHost {
public:
    OvHost();
    virtual ~OvHost();
    OvHost(const OvHost &) = delete;
    OvHost &operator=(const OvHost &) = delete;
    bool ov_on(int id) const { return id < int(ov_tags.size()) && ov_tags[size_t(id)] != 0 && !ov_dying; }
    void ov_end() { ov_dying = true; }        // the subclass's destructor began: no more dispatch
    const OvClassDef *ov_class = nullptr;
    std::vector<uint64_t> ov_tags;            // per method of ov_class: its tag, 0 when not overridden
    std::vector<uint8_t> model_ops;           // QAbstractItemModel begin*/end* pairs in progress
    bool ov_dying = false;
    // a runtime class's object (qtc_class_*): its class and its properties' values
    const struct DynClass *ov_dyn = nullptr;
    std::vector<QVariant> dyn_props;
};
// Runtime classes (qtc_class.cpp), for the QtcOv_ classes' meta-object functions.
const QMetaObject *dyn_meta(const DynClass *d);
bool dyn_is(const DynClass *d, const char *class_name);
int dyn_metacall(OvHost *h, QObject *self, QMetaObject::Call c, int id, void **a);

// The object's host when it is an overridable object (cheap when a program has none).
OvHost *ov_host(const QObject *o);
void ov_link(const QObject *o, OvHost *h);
void ov_unlink(const QObject *o);
// A member of every QtcOv_ class: links the object to its host for as long as it lives.
struct OvLink {
    const QObject *o;
    OvLink(QObject *obj, OvHost *h) : o(obj) { ov_link(obj, h); }
    ~OvLink() { ov_unlink(o); }
    OvLink(const OvLink &) = delete;
    OvLink &operator=(const OvLink &) = delete;
};
int ov_depth();
// Called by a trampoline when the method is overridden: true when the handler gave the result
// (in ret), false to run the class's own implementation.
bool ov_dispatch(OvHost *h, QObject *self, int id, void **argv, void *ret);
// The same for a method described elsewhere (a runtime class's slot, see qtc_class.cpp).
bool ov_dispatch_method(OvHost *h, QObject *self, const OvMethod *m, uint64_t tag, void **argv, void *ret);
// The innermost override call on this thread paints `o` (OVF_PAINT), or has a QPainter argument.
bool ov_painting(const QObject *o);
QPainter *ov_painter_arg(int index);
// the candidates of qtc_call among the protected methods of an overridable object
const MethodDef *ov_extras(const QObject *o, uint32_t *count);

// ------------------------------------------------- QAbstractItemModel begin*/end* pairs

enum ModelOp : uint8_t {
    MOP_INSERT_ROWS = 1, MOP_REMOVE_ROWS, MOP_MOVE_ROWS, MOP_INSERT_COLUMNS, MOP_REMOVE_COLUMNS,
    MOP_MOVE_COLUMNS, MOP_RESET
};
bool model_idle(const QObject *m);               // no begin* waits for its end*
bool model_pending(const QObject *m, int op);    // the begin* waiting is op's
void model_begun(QObject *m, int op);
void model_ended(QObject *m, int op);
// every index in the list is invalid or one of m's (changePersistentIndexList)
bool indexes_of(const QAbstractItemModel *m, const QModelIndexList &l);

// Wraps a model class's protected begin*/end* so that every one is checked against the pairs
// in progress (Qt pops its stack in end* without looking: one too many is undefined behaviour).
template <class B>
class QtcModelOps : public B {
public:
    using B::B;
    void beginInsertRows(const QModelIndex &p, int f, int l) { B::beginInsertRows(p, f, l); model_begun(this, MOP_INSERT_ROWS); }
    void endInsertRows() { model_ended(this, MOP_INSERT_ROWS); B::endInsertRows(); }
    void beginRemoveRows(const QModelIndex &p, int f, int l) { B::beginRemoveRows(p, f, l); model_begun(this, MOP_REMOVE_ROWS); }
    void endRemoveRows() { model_ended(this, MOP_REMOVE_ROWS); B::endRemoveRows(); }
    bool beginMoveRows(const QModelIndex &sp, int sf, int sl, const QModelIndex &dp, int dc) {
        const bool ok = B::beginMoveRows(sp, sf, sl, dp, dc);   // false (and nothing begun) for a move into itself
        if (ok) model_begun(this, MOP_MOVE_ROWS);
        return ok;
    }
    void endMoveRows() { model_ended(this, MOP_MOVE_ROWS); B::endMoveRows(); }
    void beginInsertColumns(const QModelIndex &p, int f, int l) { B::beginInsertColumns(p, f, l); model_begun(this, MOP_INSERT_COLUMNS); }
    void endInsertColumns() { model_ended(this, MOP_INSERT_COLUMNS); B::endInsertColumns(); }
    void beginRemoveColumns(const QModelIndex &p, int f, int l) { B::beginRemoveColumns(p, f, l); model_begun(this, MOP_REMOVE_COLUMNS); }
    void endRemoveColumns() { model_ended(this, MOP_REMOVE_COLUMNS); B::endRemoveColumns(); }
    bool beginMoveColumns(const QModelIndex &sp, int sf, int sl, const QModelIndex &dp, int dc) {
        const bool ok = B::beginMoveColumns(sp, sf, sl, dp, dc);
        if (ok) model_begun(this, MOP_MOVE_COLUMNS);
        return ok;
    }
    void endMoveColumns() { model_ended(this, MOP_MOVE_COLUMNS); B::endMoveColumns(); }
    void beginResetModel() { B::beginResetModel(); model_begun(this, MOP_RESET); }
    void endResetModel() { model_ended(this, MOP_RESET); B::endResetModel(); }
    QModelIndex createIndex(int row, int column, quintptr id = 0) const { return B::createIndex(row, column, id); }
    using B::changePersistentIndex;
    using B::changePersistentIndexList;
    using B::persistentIndexList;
};

}  // namespace qtc
