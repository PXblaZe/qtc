// Internal declarations shared by the qtc runtime and the generated thunk tables.
#pragma once
#ifndef QTC_BUILDING
#define QTC_BUILDING 1
#endif
#include "qtc.h"

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QMetaMethod>
#include <QtCore/QMetaObject>
#include <QtCore/QMetaProperty>
#include <QtCore/QMetaType>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariant>
#include <QtCore/QAbstractItemModel>
#include <QtCore/QPersistentModelIndex>
#include <QtCore/QDateTime>
#include <QtCore/QLine>
#include <QtCore/QPoint>
#include <QtCore/QRect>
#include <QtCore/QSize>
#include <QtCore/QUrl>

QT_BEGIN_NAMESPACE
class QPainterPath;
class QTransform;
class QRegularExpression;
class QWindow;
class QWidget;
class QLayout;
class QTextTable;
QT_END_NAMESPACE

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>

// Shared with the module libraries (qtc_module.h): the runtime's internal API is exported
#pragma GCC visibility push(default)

namespace qtc {

// ------------------------------------------------------------------ errors

qtc_status fail(qtc_status code, const QString &message);
qtc_status fail(qtc_status code, const char *message);
const char *status_name(qtc_status s);

// ---------------------------------------------------------- object handles

// A reference to a live object stored inside values (lists, maps, event args):
// a handle, never a pointer, so a value can outlive the object safely.
struct ObjRef {
    uint64_t handle = 0;
    bool operator==(const ObjRef &o) const { return handle == o.handle; }
};

enum SlotKind : uint8_t { SK_FREE = 0, SK_OBJ = 1, SK_VAL = 2 };
// SF_GUI: the object was last seen living in the GUI thread, by the GUI thread. Only the GUI
// thread can move it away (Qt moves an object from its own thread), and qtc hears of every move
// (QEvent::ThreadChange, see events' AppFilter) and clears it first: while it is set no other
// thread can delete the object or touch its slot, so the GUI thread may read the slot without mu.
// SF_TRACKED: a Tracked<> object (made by qtc): its destructor tells the registry first thing, so
// it needs neither the QPointer nor the destroyed() connection other objects get.
enum SlotFlag : uint8_t { SF_OWNED = 1, SF_DYING = 2, SF_RETIRED = 4, SF_MADE = 8, SF_GUI = 16, SF_TRACKED = 32 };

struct Slot {
    uint32_t gen = 1;
    uint8_t kind = SK_FREE;
    uint8_t flags = 0;
    uint32_t order = 0;            // creation order, for shutdown
    QObject *obj = nullptr;
    QPointer<QObject> qp;          // cleared by ~QObject even when destroyed() is not emitted
    // an object slot whose object still lives (a tracked object's slot is freed as it starts dying)
    bool alive() const { return (flags & SF_TRACKED) || !qp.isNull(); }
    QVariant *val = nullptr;
    QMetaObject::Connection destroyed;
};

inline uint64_t make_handle(uint32_t index, uint32_t gen) { return (uint64_t(gen) << 32) | (uint64_t(index) + 1); }
inline uint32_t handle_index(uint64_t h) { return uint32_t(h & 0xffffffffu) - 1; }
inline uint32_t handle_gen(uint64_t h) { return uint32_t(h >> 32); }

// The registry's lock: recursive, as std::recursive_mutex, but on a plain mutex, whose
// uncontended lock and unlock are one atomic operation each.
class RegMutex {
public:
    void lock() {
        const std::thread::id me = std::this_thread::get_id();
        // only this thread ever stores its own id: equal means this thread holds the lock
        if (owner_.load(std::memory_order_relaxed) == me) {
            depth_++;
            return;
        }
        m_.lock();
        owner_.store(me, std::memory_order_relaxed);
        depth_ = 1;
    }
    void unlock() {
        if (--depth_ == 0) {
            owner_.store(std::thread::id(), std::memory_order_relaxed);
            m_.unlock();
        }
    }

private:
    std::mutex m_;
    std::atomic<std::thread::id> owner_{};
    int depth_ = 0;   // read and written by the holder only
};

// A map from object addresses to V: open addressing with linear probing and backward-shift
// deletion, so that inserting and erasing allocate nothing (QHash allocated on growth and did
// more work per operation, on the path of every object made and destroyed). Keys are never null.
template <class V>
class PtrMap {
public:
    PtrMap() { reset(64); }
    ~PtrMap() { std::free(keys_); std::free(vals_); }
    PtrMap(const PtrMap &) = delete;
    PtrMap &operator=(const PtrMap &) = delete;

    size_t size() const { return n_; }
    bool isEmpty() const { return n_ == 0; }
    const V *lookup(const void *k) const {
        for (size_t i = home(k);; i = (i + 1) & mask_) {
            if (keys_[i] == k) return &vals_[i];
            if (!keys_[i]) return nullptr;
        }
    }
    V *lookup(const void *k) { return const_cast<V *>(static_cast<const PtrMap *>(this)->lookup(k)); }
    bool contains(const void *k) const { return lookup(k) != nullptr; }
    void insert(const void *k, V v) {
        if ((n_ + 1) * 2 > cap_) grow();
        size_t i = home(k);
        for (; keys_[i]; i = (i + 1) & mask_)
            if (keys_[i] == k) { vals_[i] = v; return; }
        keys_[i] = k;
        vals_[i] = v;
        n_++;
    }
    bool remove(const void *k) {
        for (size_t i = home(k);; i = (i + 1) & mask_) {
            if (!keys_[i]) return false;
            if (keys_[i] == k) { erase_at(i); return true; }
        }
    }
    // empties the map; keeps its storage unless that grew large
    void clear() {
        if (cap_ > 4096) {
            std::free(keys_);
            std::free(vals_);
            reset(64);
        } else if (n_) {
            std::memset(static_cast<void *>(keys_), 0, cap_ * sizeof(*keys_));
            n_ = 0;
        }
    }

private:
    const void **keys_ = nullptr;
    V *vals_ = nullptr;
    size_t cap_ = 0, mask_ = 0, n_ = 0;
    int shift_ = 0;

    size_t home(const void *k) const { return size_t((uint64_t(uintptr_t(k)) >> 4) * 0x9E3779B97F4A7C15ull >> shift_); }
    void reset(size_t cap) {
        cap_ = cap;
        mask_ = cap - 1;
        n_ = 0;
        shift_ = 64 - __builtin_ctzll(cap);
        keys_ = static_cast<const void **>(std::calloc(cap, sizeof(*keys_)));
        vals_ = static_cast<V *>(std::calloc(cap, sizeof(V)));
        if (!keys_ || !vals_) throw std::bad_alloc();
    }
    void grow() {
        const void **ok = keys_;
        V *ov = vals_;
        const size_t oc = cap_;
        reset(oc * 2);
        for (size_t i = 0; i < oc; i++)
            if (ok[i]) insert(ok[i], ov[i]);
        std::free(ok);
        std::free(ov);
    }
    void erase_at(size_t i) {
        size_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            if (!keys_[j]) break;
            const size_t k = home(keys_[j]);
            // keys_[j] may move to i unless its home lies cyclically in (i, j]
            const bool stays = i <= j ? (i < k && k <= j) : (i < k || k <= j);
            if (stays) continue;
            keys_[i] = keys_[j];
            vals_[i] = vals_[j];
            i = j;
        }
        keys_[i] = nullptr;
        n_--;
    }
};

// Thread-safe for lookups and destruction notices; creation and calls happen on the GUI thread.
class Registry {
public:
    RegMutex mu;
    std::vector<Slot> table;
    std::vector<uint32_t> free_list;
    PtrMap<uint32_t> by_ptr;   // object -> slot index
    int64_t live_objects = 0;
    int64_t live_values = 0;
    uint32_t order_counter = 0;

    uint64_t handle_for(QObject *o);                // existing handle or a new one; 0 for nullptr
    uint64_t adopt_new(QObject *o, bool owned);     // for objects qtc just created
    // Living in this thread. An object whose QThread was destroyed (no thread affinity) is moved
    // back to the calling GUI thread on the way.
    QObject *object(uint64_t h, qtc_status *err, bool allow_dying = false);
    // In any thread. For one in another thread only what is safe across threads, with mu held
    // for as long as the pointer is used (it cannot finish dying meanwhile: see destroy_far).
    QObject *object_anywhere(uint64_t h, qtc_status *err, bool allow_dying = false);
    // qtc_destroy of an object living in another thread: deleted by its thread, or here when
    // nothing runs in that thread (not started, or finished).
    qtc_status destroy_far(uint64_t h);
    // The handle of an object qtc already knows and that still lives (0 otherwise); never makes one.
    uint64_t known_handle(const QObject *o);
    QVariant *value(uint64_t h, qtc_status *err);
    uint64_t new_value(QVariant v);
    qtc_status free_value(uint64_t h);
    qtc_status mark_dying(uint64_t h, QObject **out);
    void set_owned(uint64_t h, bool owned);
    void set_made(uint64_t h);                       // qtc_new made it
    bool made(const QObject *o);
    bool made_handle(uint64_t h);                    // the object of h was made by qtc_new
    bool alive(uint64_t h);
    int kind_of(uint64_t h);
    // A call's receiver in one lookup: the object (as object() checks it) or the value.
    bool receiver(uint64_t h, QObject **o, QVariant **v, qtc_status *err);
    // receiver() for the GUI thread (the caller checked): objects of that thread and values are
    // read without mu; anything else takes receiver()
    bool receiver_gui(uint64_t h, QObject **o, QVariant **v, qtc_status *err) {
        const uint32_t idx = handle_index(h);
        if (h && idx < table.size()) {
            const Slot &s = table[idx];
            if (s.gen == handle_gen(h)) {
                if (s.kind == SK_VAL) { *o = nullptr; *v = s.val; *err = 0; return true; }
                if (s.kind == SK_OBJ && (s.flags & (SF_GUI | SF_DYING)) == SF_GUI && s.alive()) {
                    *o = s.obj;
                    *v = nullptr;
                    *err = 0;
                    return true;
                }
            }
        }
        return receiver(h, o, v, err);
    }
    void note_thread_change(QObject *o);   // o is about to move to another thread
    void on_destroyed(QObject *o);
    // An object deleted while its signals were blocked never emits destroyed(): its QPointer
    // is the only trace. These find such objects and forget them like destroyed ones.
    void sweep_dead();                               // every object slot (shutdown)
    void note_blocked(QObject *o);                   // blockSignals(true) went through qtc
    void sweep_blocked();                            // the noted ones (cheap, every wait)
    void clear_all();                                // at shutdown
    std::vector<QPointer<QObject>> owned_roots();    // newest first
private:
    QObject *lookup_locked(uint64_t h, qtc_status *err, bool allow_dying);
    bool pull_orphan(QObject *o);
    uint32_t alloc_slot(qtc_status *err);
    void release_slot(uint32_t idx);
    void forget_dead(uint32_t idx);                  // the slot's object is gone without notice
    std::vector<QPointer<QObject>> blocked_;
    std::vector<QObject *> blocked_raw_;
};
Registry &reg();

// Hooks the event layer installs (watches, connections tied to an object).
extern void (*on_object_destroyed_hook)(QObject *o, uint64_t handle);

// --------------------------------------------------------------- arguments

enum ArgKind : uint8_t { AK_NONE = 0, AK_BOOL, AK_INT, AK_UINT, AK_DOUBLE, AK_STR, AK_BYTES, AK_OBJ, AK_VAL, AK_NULL };

struct Arg {
    ArgKind kind = AK_NONE;
    bool b = false;
    int64_t i = 0;
    uint64_t u = 0;
    double d = 0;
    uint64_t h = 0;
    QString s;
    QByteArray bytes;
    QVariant v;
    mutable QByteArray cache;   // UTF-8 / Latin-1 bytes for const char* and view parameters
    mutable QString scache;
    void reset() {   // most arguments use one of these: the others are left alone when empty
        kind = AK_NONE;
        if (!s.isNull()) s.clear();
        if (!bytes.isNull()) bytes.clear();
        if (v.isValid()) v.clear();
        if (!cache.isNull()) cache.clear();
        if (!scache.isNull()) scache.clear();
    }
};

constexpr int kMaxArgs = 16;
struct ArgStack {
    Arg a[kMaxArgs];
    int n = 0;
    bool overflow = false;
    void clear() { for (int i = 0; i < n; i++) a[i].reset(); n = 0; overflow = false; }
};
ArgStack &args();

// ------------------------------------------------------------- tracked objects
//
// The QObjects qtc constructs are made as Tracked<Class>: the class itself (no meta-object of its
// own, so className(), qobject_cast and properties are the class's), whose destructor tells the
// registry before the class's own destructor runs. That is sip's way: no destroyed() connection
// and no QPointer per object, and the handle goes stale while the object is still whole.
void tracked_dying(QObject *o);
extern std::atomic<QObject *> g_tracked_new;   // the object make_obj just made, until it is registered

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED   // a deprecated constructor is inherited (and checked for) as it is
template <class S>
struct __attribute__((visibility("hidden"))) Tracked : S {   // each library its own
    using S::S;
    ~Tracked() override { tracked_dying(this); }
};

// whether S can be made as a Tracked<S> with these arguments (final classes cannot be derived from)
template <class S, class... A>
constexpr bool trackable() {
    if constexpr (std::is_final_v<S> || !std::has_virtual_destructor_v<S>) return false;
    else return requires(A &&...b) { new Tracked<S>(std::forward<A>(b)...); };
}

template <class S, class... A>
    requires requires(A &&...a) { new S(std::forward<A>(a)...); }
S *make_obj(A &&...a) {
    if constexpr (trackable<S, A...>()) {
        S *o = new Tracked<S>(std::forward<A>(a)...);
        g_tracked_new.store(o, std::memory_order_relaxed);
        return o;
    } else {
        return new S(std::forward<A>(a)...);
    }
}
QT_WARNING_POP

// ------------------------------------------- direct entry points (qtc_direct.h, gen/entries.py)

struct EntryDesc { const char *sig; uint32_t len; uint8_t nargs; uint8_t flags; };   // flags: 1 qtc_call's path, 2 may be protected
struct CtorEntryDesc { const char *cls; uint32_t cls_len; const char *sig; uint8_t nargs; };
extern const EntryDesc *const g_entries;
extern const uint32_t g_entry_count;
extern const CtorEntryDesc *const g_ctor_entries;
extern const uint32_t g_ctor_entry_count;
bool entry_ready();   // false, with the error set, unless qtc is initialized and this is the GUI thread
qtc_status last_error_code();
inline void put_bool(Arg &x, bool v) { x.kind = AK_BOOL; x.b = v; }
inline void put_int(Arg &x, int64_t v) { x.kind = AK_INT; x.i = v; }
inline void put_double(Arg &x, double v) { x.kind = AK_DOUBLE; x.d = v; }
inline void put_obj(Arg &x, uint64_t v) { x.kind = AK_OBJ; x.h = v; }
inline void put_null(Arg &x) { x.kind = AK_NULL; }
bool put_str(Arg &x, qtc_str v);     // as qtc_arg_str; false (and an error) if malformed
bool put_bytes(Arg &x, qtc_str v);
bool put_val(Arg &x, uint64_t v);    // as qtc_arg_val
qtc_status entry_call(uint64_t self, uint32_t id, Arg *a, int n);
uint64_t entry_new(uint32_t id, Arg *a, int n);

// --------------------------------------------------------------- the return

struct Ret {
    int kind = QTC_T_NONE;
    bool b = false;
    int64_t i = 0;
    uint64_t u = 0;
    double d = 0;
    uint64_t h = 0;
    QByteArray s;          // UTF-8 (strings) or raw bytes
    QVariant v;            // for QTC_T_LIST / MAP / VALUE
    // values written through out-parameters by the last call
    struct OutVal { int64_t i; double d; bool b; };
    static constexpr int kMaxOut = 8;
    OutVal outs[kMaxOut];
    int nout = 0;
    void clear() {
        kind = QTC_T_NONE; b = false; i = 0; u = 0; d = 0; h = 0;
        if (!s.isNull()) s.clear();
        if (v.isValid()) v.clear();
    }
    void clear_all() { clear(); nout = 0; }
    void out(bool x) { if (nout < kMaxOut) outs[nout++] = {x ? 1 : 0, x ? 1.0 : 0.0, x}; }
    template <class I>
        requires(std::is_integral_v<I> && !std::is_same_v<I, bool>)
    void out(I x) { if (nout < kMaxOut) outs[nout++] = {int64_t(x), double(x), x != 0}; }
    template <class F>
        requires std::is_floating_point_v<F>
    void out(F x) { if (nout < kMaxOut) outs[nout++] = {std::isfinite(x) ? int64_t(x) : 0, double(x), x != 0}; }
    void set_none() { clear(); }
    void set_bool(bool x) { clear(); kind = QTC_T_BOOL; b = x; i = x; d = x; }
    void set_int(int64_t x) { clear(); kind = QTC_T_INT; i = x; u = uint64_t(x); d = double(x); b = x != 0; }
    void set_uint(uint64_t x) { clear(); kind = QTC_T_UINT; u = x; i = int64_t(x); d = double(x); b = x != 0; }
    void set_double(double x) { clear(); kind = QTC_T_DOUBLE; d = x; i = std::isfinite(x) ? int64_t(x) : 0; b = x != 0; }
    void set_str(const QString &x) { clear(); kind = QTC_T_STR; s = x.toUtf8(); }
    void set_bytes(const QByteArray &x) { clear(); kind = QTC_T_BYTES; s = x; }
    void set_obj(QObject *o);
    void set_variant(const QVariant &x);  // sanitizes and picks the kind
};
Ret &ret();

// A call's own arguments and result. A call moves the pushed arguments (args()) into its frame
// when it starts and publishes its result into ret() when it ends, so a call that runs while
// another one is in Qt (a qtc_connect_to forward fires inside a Qt call) cannot touch the
// interrupted call's arguments, or the string views Qt holds into them, and the result left in
// ret() is the interrupted call's own.
struct CallFrame {
    ArgStack *st;
    Ret *r;
    CallFrame();
    ~CallFrame();
    CallFrame(const CallFrame &) = delete;
    CallFrame &operator=(const CallFrame &) = delete;
};
// Keeps the program's pending arguments and last result aside while qtc makes a call of its own
// (a forward), and puts them back afterwards.
struct Stash {
    ArgStack *st;
    Ret *r;
    Stash();
    ~Stash();
    Stash(const Stash &) = delete;
    Stash &operator=(const Stash &) = delete;
};
// Calls a method by name or signature with the pushed arguments, as qtc_call does.
qtc_status do_call(uint64_t receiver, qtc_str method);
// Whether a call of this method could reach anything: false when the class's bound methods leave
// it out (see collect_object), with the reason.
bool method_callable(QObject *o, const QByteArray &signature, QString *why);
// The bound method `name` of o taking the most of a signal's leading parameter types (exactly
// those types): its signature, with *nargs set to how many it takes; empty if there is none.
QByteArray bound_method_for_signal(QObject *o, const QByteArray &name, const QList<QByteArray> &types, int *nargs);

// Variant helpers: values crossing into C hold ObjRef instead of QObject pointers.
QVariant sanitize(const QVariant &v);
// The same for values that come from another thread: an object pointer there may already be
// dangling, so only objects qtc knows and that still live get their handles (others become 0).
QVariant sanitize_known(const QVariant &v);
QVariant desanitize(const QVariant &v);
// Whether a value holds an object (a pointer, an ObjRef, or one inside a list or map).
bool holds_object(const QVariant &v);
// Meta-object methods (slots, signals, invokables) of o that qtc_call reaches through the
// meta-object, by name or signature, as qtc_call would list them (see collect_object).
std::vector<QMetaMethod> meta_candidates(QObject *o, const QByteArray &spec);
// The most derived class of o that qtc binds (QObject at least).
const struct ClassDef *bound_class_of(const QObject *o);
int variant_kind(const QVariant &v);
QVariant arg_to_variant(const Arg &a);                        // natural variant of an argument
int arg_to_metatype(const Arg &a, QMetaType target, QVariant &out, int index);   // strict conversion
QString arg_describe(const Arg &a);
int arg_error(int index, int code, const char *expected, const Arg &a);

// ------------------------------------------------------------ thunk tables

enum ParamKind : uint8_t {
    PK_BOOL = 1, PK_INT, PK_UINT, PK_DOUBLE, PK_ENUM, PK_FLAGS, PK_STR, PK_BYTES, PK_CHAR,
    PK_OBJ, PK_VARIANT, PK_VALUE, PK_LIST, PK_MAP, PK_INDEX, PK_NULLPTR, PK_OUT
};

struct EnumInfo {
    const char *name;        // "Qt::AlignmentFlag"
    const int64_t *values;   // sorted
    const char *const *keys; // parallel to values, sorted by value
    int32_t count;
    uint8_t flags;           // 1 = used as QFlags (validate as a bit mask), 2 = open (any value of the type)
    int64_t mask;
    int64_t min, max;        // range of the underlying type
};
extern const EnumInfo *g_enums;
extern int32_t g_enum_count;

struct ParamDesc {
    uint8_t kind;
    int16_t enum_id;                 // PK_ENUM / PK_FLAGS
    const char *type;                // C++ type name, for messages and scoring
    const QMetaObject *meta;         // PK_OBJ: the required class
};

typedef int (*ThunkFn)(void *self, Arg *a, int n, Ret *r);

enum MethodFlag : uint16_t { MF_STATIC = 1, MF_CONST = 2, MF_DEPRECATED = 4, MF_BLOCKING = 8, MF_NOFUZZ = 16, MF_SIGNAL = 32 };

struct MethodDef {
    const char *name;
    const char *sig;          // normalized, e.g. "addItem(QString,QVariant)"
    ThunkFn fn;
    const ParamDesc *params;
    uint8_t nparams;
    uint8_t min_args;
    uint16_t flags;
    uint8_t ret_kind;         // QTC_T_*
    const char *ret_type;
};

enum ClassKind : uint8_t { CK_OBJECT = 1, CK_VALUE = 2 };
enum AppNeed : uint8_t { NEED_CORE = 0, NEED_GUI = 1, NEED_WIDGETS = 2 };

struct ClassDef {
    const char *name;
    const char *module;
    uint8_t kind;
    uint8_t need;             // AppNeed
    uint8_t abstract;
    const QMetaObject *meta;            // CK_OBJECT
    QMetaType (*metatype)();            // CK_VALUE
    const char *const *bases;           // value classes: public bases (names), null-terminated
    void *(*upcast)(void *self, int base);
    const MethodDef *methods;
    uint32_t nmethods;
    const MethodDef *ctors;
    uint32_t nctors;
};

void register_classes(const ClassDef *defs, int count);
// qtc_new's work with a given set of constructors (c, if any, for messages and needs)
uint64_t new_with_ctors(const char *cls, const MethodDef *ctors, uint32_t nctors, const ClassDef *c = nullptr);
void unregister_class(const QByteArray &name);
// runtime classes (qtc_class.cpp)
bool dyn_class_named(const QByteArray &name);
uint64_t dyn_new(const QByteArray &name, bool *handled);   // qtc_new of a runtime class
void classes_shutdown();
void rhi_shutdown();   // qtc_rhi.cpp: RHIs, their resources, shaders and Vulkan instances
const ClassDef *find_class(const QByteArray &name);
int app_kind();   // QTC_APP_*, or -1 before init

// --------------------------------------------------------------- converting

template <class T> struct H {
    std::optional<T> o;
    T &get() { return *o; }
};

int cv(const Arg &a, H<bool> &h, int idx);
int cv(const Arg &a, H<double> &h, int idx);
int cv(const Arg &a, H<float> &h, int idx);
int cv(const Arg &a, H<QString> &h, int idx);
int cv(const Arg &a, H<QByteArray> &h, int idx);
int cv(const Arg &a, H<QChar> &h, int idx);
int cv(const Arg &a, H<QVariant> &h, int idx);
int cv(const Arg &a, H<const char *> &h, int idx);
int cv(const Arg &a, H<QStringView> &h, int idx);
int cv(const Arg &a, H<QAnyStringView> &h, int idx);
int cv(const Arg &a, H<QLatin1StringView> &h, int idx);
int cv(const Arg &a, H<QUtf8StringView> &h, int idx);
int cv(const Arg &a, H<QByteArrayView> &h, int idx);
int cv(const Arg &a, H<QModelIndex> &h, int idx);
int cv(const Arg &a, H<QPersistentModelIndex> &h, int idx);
int cv(const Arg &a, H<QStringList> &h, int idx);
int cv(const Arg &a, H<QVariantList> &h, int idx);
int cv(const Arg &a, H<QVariantMap> &h, int idx);
int cv(const Arg &a, H<QVariantHash> &h, int idx);

int cv_int64(const Arg &a, int64_t &out, int idx, int64_t lo, int64_t hi);
int cv_uint64(const Arg &a, uint64_t &out, int idx, uint64_t hi);
QObject *cv_object(const Arg &a, const QMetaObject *mo, int idx, int *err);
int cv_enum_value(const Arg &a, int eid, int idx, int64_t &out);
int cv_value_variant(const Arg &a, QMetaType target, QVariant &out, int idx);

template <class I>
    requires(std::is_integral_v<I> && !std::is_same_v<I, bool>)
int cv(const Arg &a, H<I> &h, int idx) {
    if constexpr (std::is_signed_v<I>) {
        int64_t v;
        if (int e = cv_int64(a, v, idx, int64_t(std::numeric_limits<I>::min()), int64_t(std::numeric_limits<I>::max())))
            return e;
        h.o = I(v);
    } else {
        uint64_t v;
        if (int e = cv_uint64(a, v, idx, uint64_t(std::numeric_limits<I>::max())))
            return e;
        h.o = I(v);
    }
    return 0;
}

template <class T>
    requires std::is_base_of_v<QObject, T>
int cv(const Arg &a, H<T *> &h, int idx) {
    int err = 0;
    QObject *o = cv_object(a, &T::staticMetaObject, idx, &err);
    if (err) return err;
    h.o = static_cast<T *>(o);
    return 0;
}
template <class T>
    requires std::is_base_of_v<QObject, T>
int cv(const Arg &a, H<const T *> &h, int idx) {
    int err = 0;
    QObject *o = cv_object(a, &T::staticMetaObject, idx, &err);
    if (err) return err;
    h.o = static_cast<const T *>(o);
    return 0;
}

// Optional out-parameters such as `bool *ok`: only null (or 0) may be passed.
template <class T>
    requires std::is_arithmetic_v<T>
int cv(const Arg &a, H<T *> &h, int idx) {
    if (a.kind == AK_NULL || a.kind == AK_NONE || (a.kind == AK_INT && a.i == 0) || (a.kind == AK_UINT && a.u == 0)) {
        h.o = nullptr;
        return 0;
    }
    return arg_error(idx, QTC_ERR_BAD_ARGS, "null (an optional out-parameter)", a);
}

// Out-parameters such as `int *x`: the thunk passes storage and records what was written.
// The argument in that position, if any, must be null.
template <class T> struct Out {};
template <class T> struct H<Out<T>> {
    T v{};
    T *get() { return &v; }
};
template <class T>
int cv(const Arg &a, H<Out<T>> &, int idx) {
    if (a.kind == AK_NULL || a.kind == AK_NONE || (a.kind == AK_INT && a.i == 0) || (a.kind == AK_UINT && a.u == 0)) return 0;
    return arg_error(idx, QTC_ERR_BAD_ARGS, "null (an out-parameter: leave it out and read it with qtc_out_*)", a);
}

template <class E>
    requires std::is_enum_v<E>
int cve(const Arg &a, H<E> &h, int idx, int eid) {
    int64_t v;
    if (int e = cv_enum_value(a, eid, idx, v)) return e;
    h.o = static_cast<E>(v);
    return 0;
}
template <class E>
int cvf(const Arg &a, H<QFlags<E>> &h, int idx, int eid) {
    int64_t v;
    if (int e = cv_enum_value(a, eid, idx, v)) return e;
    h.o = QFlags<E>::fromInt(typename QFlags<E>::Int(v));
    return 0;
}

template <class T>
concept ValueLike = std::is_class_v<T> && !std::is_abstract_v<T> && std::is_copy_constructible_v<T> &&
                    std::is_destructible_v<T> && requires { sizeof(T); } && requires { QMetaType::fromType<T>(); };

template <class T>
    requires(ValueLike<T> && !std::is_base_of_v<QObject, T>)
int cv(const Arg &a, H<T> &h, int idx) {
    QVariant out;
    if (int e = cv_value_variant(a, QMetaType::fromType<T>(), out, idx)) return e;
    h.o.emplace(*static_cast<const T *>(out.constData()));
    return 0;
}

template <class T> QMetaType metatype_of() {
    if constexpr (ValueLike<T>) return QMetaType::fromType<T>();
    else return QMetaType();
}

// Element conversion for lists: wraps one QVariant as an argument.
Arg element_arg(const QVariant &v);
template <class T>
int cv_elem(const QVariant &v, std::optional<T> &out, int idx) {
    Arg a = element_arg(v);
    H<T> h;
    if constexpr (std::is_enum_v<T>) {
        int64_t x;
        if (int e = cv_int64(a, x, idx, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max())) return e;
        out = static_cast<T>(x);
        return 0;
    } else {
        if (int e = cv(a, h, idx)) return e;
        out = std::move(h.o);
        return 0;
    }
}
template <class T>
    requires requires(const Arg &a, H<T> &h) { cv(a, h, 0); }
int cv(const Arg &a, H<QList<T>> &h, int idx) {
    if (a.kind == AK_VAL && a.v.metaType() == QMetaType::fromType<QList<T>>()) {
        h.o = a.v.value<QList<T>>();
        return 0;
    }
    if (a.kind != AK_VAL || !a.v.canConvert<QVariantList>())
        return arg_error(idx, QTC_ERR_BAD_ARGS, "a list", a);
    const QVariantList in = a.v.toList();
    QList<T> out;
    out.reserve(in.size());
    for (const QVariant &e : in) {
        std::optional<T> x;
        if (int err = cv_elem<T>(e, x, idx)) return err;
        out.append(std::move(*x));
    }
    h.o = std::move(out);
    return 0;
}

// --------------------------------------------------------------- returning

void rt(Ret &r, bool v);
void rt(Ret &r, double v);
void rt(Ret &r, float v);
void rt(Ret &r, const QString &v);
void rt(Ret &r, const QByteArray &v);
void rt(Ret &r, QChar v);
void rt(Ret &r, const QVariant &v);
void rt(Ret &r, const char *v);
void rt(Ret &r, QStringView v);
void rt(Ret &r, QLatin1StringView v);
void rt(Ret &r, QByteArrayView v);
void rt(Ret &r, const QModelIndex &v);
void rt(Ret &r, const QStringList &v);
void rt(Ret &r, const QVariantList &v);
void rt(Ret &r, const QVariantMap &v);
void rt(Ret &r, const QVariantHash &v);

template <class I>
    requires(std::is_integral_v<I> && !std::is_same_v<I, bool>)
void rt(Ret &r, I v) {
    if constexpr (std::is_signed_v<I>) r.set_int(int64_t(v));
    else r.set_uint(uint64_t(v));
}
template <class E>
    requires std::is_enum_v<E>
void rt(Ret &r, E v) { r.set_int(int64_t(v)); }
template <class E>
void rt(Ret &r, QFlags<E> v) { r.set_int(int64_t(v.toInt())); }
template <class T>
    requires std::is_base_of_v<QObject, T>
void rt(Ret &r, T *o) { r.set_obj(const_cast<QObject *>(static_cast<const QObject *>(o))); }

QVariant out_variant_obj(QObject *o);
template <class T>
QVariant out_elem(const T &v) {
    if constexpr (std::is_pointer_v<T> && std::is_base_of_v<QObject, std::remove_cv_t<std::remove_pointer_t<T>>>)
        return out_variant_obj(const_cast<QObject *>(static_cast<const QObject *>(v)));
    else if constexpr (std::is_enum_v<T>)
        return QVariant(qint64(v));
    else if constexpr (std::is_same_v<T, QModelIndex>)
        return QVariant::fromValue(QPersistentModelIndex(v));
    else
        return sanitize(QVariant::fromValue(v));
}
// Element types a list can keep as it is: the QVariant shares the QList (no copy, no QVariant
// per element: a million chart points stay one allocation) and the list functions read it in
// place. Others become a QVariantList of converted elements (objects, enums, indexes).
template <class T>
constexpr bool plain_list_elem = (std::is_arithmetic_v<T> && !std::is_same_v<T, char> && !std::is_same_v<T, signed char> &&
                                  !std::is_same_v<T, unsigned char>) ||
                                 std::is_same_v<T, QPoint> || std::is_same_v<T, QPointF> || std::is_same_v<T, QSize> ||
                                 std::is_same_v<T, QSizeF> || std::is_same_v<T, QRect> || std::is_same_v<T, QRectF> ||
                                 std::is_same_v<T, QLine> || std::is_same_v<T, QLineF> || std::is_same_v<T, QUrl> ||
                                 std::is_same_v<T, QDate> || std::is_same_v<T, QTime> || std::is_same_v<T, QDateTime> ||
                                 std::is_same_v<T, QByteArray>;
bool native_list(const QVariant &v);   // a QList<T> kept as it is (see plain_list_elem)

template <class T>
    requires(requires(const T &x) { out_elem<T>(x); } && (ValueLike<T> || std::is_pointer_v<T> || std::is_enum_v<T> || std::is_arithmetic_v<T>))
void rt(Ret &r, const QList<T> &v) {
    if constexpr (plain_list_elem<T>) {
        r.set_variant(QVariant::fromValue(v));
        return;
    }
    QVariantList out;
    out.reserve(v.size());
    for (const T &e : v) out.append(out_elem<T>(e));
    r.set_variant(QVariant(out));
}
template <class T>
    requires(ValueLike<T> && !std::is_base_of_v<QObject, T>)
void rt(Ret &r, const T &v) { r.set_variant(QVariant::fromValue(v)); }

// ------------------------------------------------------------- preconditions
// Generated thunks check what Qt only asserts in debug builds (indexes, sizes,
// non-null pointers, ownership cycles) before calling, and fail with these.

int pre_fail(int code, const char *what);
int64_t alloc_limit();   // bytes, see qtc_set_alloc_limit

inline bool in(int64_t v, int64_t lo, int64_t hi) { return v >= lo && v <= hi; }
// count elements of elem bytes fit in the allocation limit
inline bool alloc_ok(int64_t count, int64_t elem) { return count <= 0 || (elem > 0 && count <= alloc_limit() / elem); }
// a QBuffer seeked past its end grows to the position (Qt fills the gap with zeros): such a seek
// stays within the allocation limit; other devices are left to Qt
bool buffer_seek_ok(const QObject *device, int64_t pos);
// Qt puts each run of combining marks in canonical order in time that grows as the run's square, and
// rewrites the string in place: length x the characters past ASCII
bool normalize_ok(const QString &s);

// The number of elements: the first of size(), count(), elementCount() and length() that counts
// (QPainterPath::length() is the path's length in pixels, QRect::size() is a QSize).
template <class T> constexpr bool counts = std::is_integral_v<std::remove_cvref_t<T>>;
template <class S> int64_t size_of(const S *s) {
    if constexpr (requires { requires counts<decltype(s->size())>; }) return int64_t(s->size());
    else if constexpr (requires { requires counts<decltype(s->count())>; }) return int64_t(s->count());
    else if constexpr (requires { requires counts<decltype(s->elementCount())>; }) return int64_t(s->elementCount());
    else if constexpr (requires { requires counts<decltype(s->length())>; }) return int64_t(s->length());
    else static_assert(sizeof(S) == 0, "size_of: no size(), count(), elementCount() or length() that counts");
}
template <class S> constexpr int64_t elem_size() {
    if constexpr (requires { typename S::value_type; }) return int64_t(sizeof(typename S::value_type));
    else return 16;
}
template <class S> bool idx(const S *s, int64_t i) { return i >= 0 && i < size_of(s); }            // [0, size)
template <class S> bool pos(const S *s, int64_t i) { return i >= 0 && i <= size_of(s); }           // [0, size]
template <class S> bool span(const S *s, int64_t p, int64_t n) {                                    // [p, p + n) inside
    int64_t z = size_of(s);
    return p >= 0 && n >= 0 && p <= z && n <= z - p;
}
template <class S> bool nonempty(const S *s) { return size_of(s) > 0; }
template <class S> bool alloc_elems(const S *, int64_t n) { return alloc_ok(n, elem_size<S>()); }
// a position that Qt pads up to with fill characters when it is past the end
template <class S> bool pad(const S *s, int64_t i) { return i <= size_of(s) || alloc_ok(i - size_of(s), elem_size<S>()); }
// times copies of the receiver's contents
template <class S> bool alloc_times(const S *s, int64_t times) {
    int64_t z = size_of(s);
    return times <= 1 || z <= 0 || (times <= alloc_limit() / elem_size<S>() / z);
}
inline bool fmt_ok(int64_t c) { return c == 'e' || c == 'E' || c == 'f' || c == 'g' || c == 'G'; }
inline bool prec_ok(int64_t p) { return p >= -128 && p <= 1000; }   // -128: QLocale::FloatingPointShortest
// the sample rates audio hardware and files use (QAudioEngine crashes on some others, 44101 say)
inline bool standard_sample_rate(int64_t r) {
    static constexpr int64_t rates[] = {8000,  11025, 12000,  16000,  22050,  24000,  32000,  44100,  48000, 64000,
                                        88200, 96000, 128000, 176400, 192000, 352800, 384000, 705600, 768000};
    for (int64_t x : rates)
        if (r == x) return true;
    return false;
}
// QStandardItem inserts a column row by row into one array: about rows x rows x columns moves
inline bool std_columns_ok(int64_t rows, int64_t cols_after) {
    return double(rows) * double(rows) * double(cols_after < 1 ? 1 : cols_after) <= double(int64_t(1) << 30);
}
// a file descriptor that is open in this process (always true where there are none)
bool fd_open(int64_t fd);
// no widget uses the style through QWidget::setStyle
bool style_unshared(const QObject *style);
// QGraphicsScene::setStyle deletes the scene's own style before the widgets of its proxy widgets
// let go of it: false while it has a style of its own and proxy widgets
bool scene_style_swappable(const QObject *scene);
// no scroll area but `except` has the widget as its widget (a scroll area keeps watching its widget
// after something else takes it, and two of them resize it in turn without end)
bool in_no_scroll_area(const QObject *widget, const QObject *except = nullptr);
// play() and stop() of a spatial-audio sound note whether it plays; pause() needs it to
bool note_sound(QObject *sound, bool playing);   // always true
bool sound_playing(const QObject *sound);

// arg is not the receiver or one of its ancestors (adding it would create an ownership cycle)
bool not_self_or_ancestor(const QObject *self, const QObject *arg);
// arg is not root or inside it (a QObject, or a QVariant holding one); null passes
bool not_within(const QObject *arg, const QObject *root);
bool not_within(const QVariant &arg, const QObject *root);
// arg (a source model) does not lead back to self through proxies
bool model_no_cycle(const QAbstractItemModel *self, const QAbstractItemModel *arg);
// an invalid index, or one of model m
bool index_of(const QAbstractItemModel *m, const QModelIndex &i);
int64_t rows_of(const QAbstractItemModel *m, const QModelIndex &parent);
int64_t cols_of(const QAbstractItemModel *m, const QModelIndex &parent);
// inserting count rows (Qt::Vertical) or columns at pos is not one of the slow insertions of
// QStringListModel, QStandardItemModel and QSqlQueryModel, directly or through proxies
bool insert_cheap(const QAbstractItemModel *m, Qt::Orientation o, int64_t pos, int64_t count, QModelIndex parent);
// p (which may dangle) has a live handle, or is root or one of its descendants
bool known_live(const QObject *p, const QObject *root);
// a grid layout of at least rows x columns stays under 2^20 cells
bool grid_cells_ok(const QObject *layout, int64_t rows, int64_t cols);
bool grid_grow_ok(const QObject *layout);
// images: w x h pixels at bytes_per_pixel fit the allocation limit (Qt checks only for overflow)
bool pixels_ok(double w, double h, double bytes_per_pixel);
#ifdef QTC_HAS_GUI
double image_bpp(int format);   // bytes per pixel of a QImage::Format, at least 1
double app_dpr();               // the application's device pixel ratio, at least 1
bool scaled_ok(const QSize &from, double w, double h, int aspect_mode, double bpp);
bool transform_ok(const QSize &from, const QTransform &t, double bpp);
bool qwindow_size_ok(const QWindow *w, double width, double height);
bool qwindow_show_ok(const QWindow *w);
#endif
#ifdef QTC_HAS_WIDGETS
bool grab_ok(const QWidget *w, const QRect &r);
bool window_size_ok(const QWidget *w, double width, double height);   // true for a child widget
bool window_show_ok(const QWidget *w);
bool layout_sums_ok(const QLayout *l);       // Qt's layout engine can add up its items (see qtc_core.cpp)
bool layouts_ok_below(const QWidget *w);     // ... for the widget's layout and those of all widgets in it
bool widget_attribute_settable(Qt::WidgetAttribute a);   // not one of the attributes Qt keeps itself
#endif
bool gl_current();                       // an OpenGL context is current on this thread
bool cbor_ok(const QByteArray &data);    // decodes without error
bool file_exists(const QString &path);   // a file (or resource) is there
bool doomed(const QObject *o);           // deleteLater was called on it, or its destruction began
bool widget_flag(const QObject *o);      // a QWidget (readable while it is being destroyed)
// A substring search does at most text x pattern comparisons (Qt's Boyer-Moore matcher on
// repetitive text): patterns up to 64 units always pass, longer ones need text x pattern <= 2^30.
inline bool search_ok(int64_t text, int64_t pattern) {
    return pattern <= 64 || (long double)text * (long double)pattern <= (long double)(int64_t(1) << 30);
}
inline int64_t text_len(const QString &x) { return x.size(); }
inline int64_t text_len(QStringView x) { return x.size(); }
inline int64_t text_len(QLatin1StringView x) { return x.size(); }
inline int64_t text_len(QByteArrayView x) { return x.size(); }
inline int64_t text_len(const QByteArray &x) { return x.size(); }
inline int64_t text_len(const char *x) { return x ? int64_t(std::strlen(x)) : 0; }
// split() would not make more parts than the allocation limit holds
bool split_ok(const QString &s, const QString &sep, Qt::CaseSensitivity cs);
bool split_ok(const QString &s, const QRegularExpression &re);
bool split_ok(const QByteArray &s, char sep);
// replace() would not grow the receiver past the allocation limit
bool replace_ok(const QString &s, const QString &before, int64_t after_len, Qt::CaseSensitivity cs);
bool replace_ok(const QString &s, const QRegularExpression &re, int64_t after_len);
bool replace_ok(const QByteArray &s, const QByteArray &before, int64_t after_len);
// QPainterPath (qtc_builtin.cpp): flattening it with t stays within the allocation limit,
// and adding extra elements keeps it under kMaxPathElements
bool flatten_ok(const QPainterPath &p, const QTransform &t);
bool path_room(const QPainterPath &p, int64_t extra);
// every point of the path is finite (a NaN point defeats the bounding-box tests that keep hit
// testing quick: each test then walks the whole path)
bool path_finite(const QPainterPath &p);
bool transform_finite(const QTransform &t);
// about how many points flattening the path makes (each curve split until flat to half a pixel);
// counting stops once past stop
double path_points(const QPainterPath &p, const QTransform &t, double stop);
// Qt's path clipper is safe on the paths (coordinates within +-2^30) and, with count_crossings, quick:
// at most 32,768 segments in all, crossing each other at most 4,096 times (it slows down about as
// the square of the crossings)
bool clip_ok(const QPainterPath &a, const QPainterPath &b, bool count_crossings);
// mapping the path through a perspective transform stays under 2^20 points (512 per curve)
bool project_ok(const QTransform &t, const QPainterPath &p);
// measuring the path's length (length, percentAtLength, pointAtPercent ...) takes at most about 2^20 steps
bool path_length_ok(const QPainterPath &p);
// no cell of the table is merged (rows and columns are removed safely only then)
bool table_unmerged(const QTextTable *t);
bool table_columns_addable(const QTextTable *t);
// the object is not held by another object's role (qtc_roles.cpp), so it may be moved
bool not_held(const QObject *receiver, const QObject *arg);
// an object that keeps a plain pointer to the one it was made in (a QSplitterHandle its splitter)
QObject *bound_parent(const QObject *o);
bool roles_defer_delete(QObject *o);     // qtc_destroy: Qt needs the object alive a while longer
// qtc_new: objects that must not outlive another get it as parent (made_with: its object arguments)
void roles_adopt_new(QObject *o, const QList<QObject *> &made_with);
void roles_constructed(QObject *o);      // qtc_new: the parts its constructor made are internal
// a document shown by a QPlainTextEdit takes only a QPlainTextDocumentLayout
bool doc_layout_ok(const QObject *doc, const QObject *layout);
// Qt Charts crashes on an area series in a chart whose boundary line has point label clipping off:
// the line is no area series' boundary / the area series has no such boundary
bool bound_clipping_ok(const QObject *series);
// the proxy is not a QFileDialog's proxy model (whose source must stay the dialog's model)
bool proxy_source_free(const QObject *proxy);
bool area_clipping_ok(const QObject *series);
// an object a setter takes ownership of: not Qt's own, and held by no other owner (one owner only)
bool free_for(const QObject *receiver, const QObject *arg);
// the part a setter replaces and deletes: other owners let go of what they hold in it
bool part_free(const QObject *receiver, const QObject *old);
// a device reopened without reading is not one an NMEA source reads
bool nmea_open_ok(const QObject *device, int mode);
// QWidget::setStyle: remembers which widgets use a style they do not own (always true)
bool note_style_user(QObject *widget, QObject *style);
bool query_delims_ok(QChar value, QChar pair);
// a widget may be added to a layout that has no parent widget only if no such layout holds it yet
bool layout_accepts(const QObject *layout, const QObject *widget);
// qtc_roles.cpp: owners that hold plain pointers to objects, see there
void roles_register(QObject *o);              // every object that gets a handle
void roles_note_container(QObject *container, QObject *window);   // QWidget::createWindowContainer made it
void roles_forget(QObject *o, bool readable); // it is being destroyed (readable) or is gone
void roles_clear();                           // qtc_shutdown
void roles_init();                      // after the application object exists: watch Qt's own deletions
QString roles_prepare_destroy(QObject *victim, bool made = false);   // releases it from its owners; non-empty: must not be deleted
QObject *roles_holder(QObject *x);            // an owner holding x in a role, if any
// qtc_move_to_thread: why root (and what is inside it) cannot live in another thread, or empty
QString roles_movable(QObject *root);
void roles_forget_tree(QObject *root);        // it moved to another thread: the GUI-thread index lets go
void roles_register_tree(QObject *root);      // it came back: objects with handles are registered again
// Bumped by whatever may give an owner something new to hold; the held-object index follows it.
extern uint64_t roles_epoch;
void roles_note_call(const QObject *o);       // a call on o is about to run
bool roles_is_owner(const QObject *o);        // whether o holds anything in a role (roles_note_call matters)
bool roles_may_own(QObject *o);   // whether an object of its class can hold others in a role
// a copy that shares no data with anything (QProcessEnvironment::insert(self) deadlocks otherwise)
template <class E> bool unshare(E &e) {
    E fresh;
    fresh.insert(e);
    e = fresh;
    return true;
}
template <class P, class St> bool style_no_cycle(P *self, St *arg) {
    int depth = 0;
    for (St *x = arg; x; depth++) {
        if (depth > 64 || static_cast<const QObject *>(x) == static_cast<const QObject *>(self)) return false;
        P *px = qobject_cast<P *>(x);
        if (!px) return true;
        x = px->baseStyle();
    }
    return true;
}
template <class P> bool fill_polygons_ok(const P *p) {
    int64_t subpaths = 0;
    for (int i = 0, e = p->elementCount(); i < e; i++)
        if (p->elementAt(i).isMoveTo()) subpaths++;
    return subpaths <= 8192;
}
template <class C> bool qml_has_engine(const C *c) {
    if constexpr (requires { c->engine(); }) return c->engine() != nullptr;
    else return true;
}
int64_t cstr_len(const Arg &a);          // bytes behind a converted const char* argument

// ------------------------------------------------------------- thunk helpers

template <class S> inline S *self_obj(void *q) { return static_cast<S *>(static_cast<QObject *>(q)); }
template <class S> inline S *self_val(void *q) { return static_cast<S *>(q); }

}  // namespace qtc
#pragma GCC visibility pop

Q_DECLARE_METATYPE(qtc::ObjRef)

#define QTC_CV(i, h) do { if (int e_ = qtc::cv(a[i], h, i)) return e_; } while (0)
#define QTC_CVE(i, h, eid) do { if (int e_ = qtc::cve(a[i], h, i, eid)) return e_; } while (0)
#define QTC_CVF(i, h, eid) do { if (int e_ = qtc::cvf(a[i], h, i, eid)) return e_; } while (0)
