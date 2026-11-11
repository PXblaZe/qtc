// qtc runtime: lifecycle, errors, handles, values, calls, properties, introspection.
#include "qtc_internal.h"
#include "qtc_events.h"
#include "qtc_override.h"

#include <QtCore/QBuffer>
#include <QtCore/QAbstractProxyModel>
#include <cmath>
#include <QtCore/QConcatenateTablesProxyModel>
#include <QtCore/QIdentityProxyModel>
#include <QtCore/QSortFilterProxyModel>
#include <QtCore/QStringListModel>
#include <QtCore/QTransposeProxyModel>
#include <QtCore/QCborValue>
#include <QtCore/QCoreApplication>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QLibraryInfo>
#include <QtCore/QSequentialIterable>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QDateTime>
#ifdef QTC_HAS_MODULE_Qml
#include <QtQml/QJSValue>
#include <QtQml/qqml.h>   // qmlContext
#endif
#ifdef QTC_HAS_GUI
#include <QtGui/QGuiApplication>
#if QT_CONFIG(opengl)
#include <QtGui/QOpenGLContext>
#endif
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtGui/QPixmap>
#include <QtGui/QStandardItemModel>
#include <QtGui/QTransform>
#include <QtGui/QWindow>
#endif
#ifdef QTC_HAS_MODULE_Sql
#include <QtSql/QSqlQueryModel>
#endif
#ifdef QTC_HAS_WIDGETS
#include <QtWidgets/QApplication>
#include <QtWidgets/QBoxLayout>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QWidget>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#endif
#include <string>

void qtc_gen_register_all();   // generated: registers every bound module's tables
namespace qtc {
void register_builtin_classes();   // QtcCanvas, QtcBridge
void events_init();
void events_shutdown();
void events_top_level();
}

namespace qtc {

// ================================================================== state

struct State {
    std::atomic<bool> inited{false};
    std::atomic<int> policy{QTC_ERRORS_RETURN};
    int kind = -1;
    QCoreApplication *app = nullptr;
    Qt::HANDLE gui_thread = nullptr;
    std::vector<std::string> argv_store;
    std::vector<char *> argv_ptrs;
    int argc = 0;
    bool classes_registered = false;
};
static State &st() { static State *s = new State; return *s; }
int app_kind() { return st().inited ? st().kind : -1; }

static thread_local qtc_status t_err = 0;
static thread_local QByteArray t_msg;
static thread_local QByteArray t_view;

bool is_inited() { return st().inited.load(std::memory_order_acquire); }
qtc_status last_error_code() { return t_err; }
qtc_view view_of(const QByteArray &b) {
    t_view = b;
    return qtc_view{t_view.constData(), int64_t(t_view.size())};
}
static qtc_view view_static(const char *s) { return qtc_view{s, int64_t(std::strlen(s))}; }

const char *status_name(qtc_status s) {
    switch (s) {
    case QTC_OK: return "QTC_OK";
    case QTC_ERR_NOT_INITIALIZED: return "QTC_ERR_NOT_INITIALIZED";
    case QTC_ERR_WRONG_THREAD: return "QTC_ERR_WRONG_THREAD";
    case QTC_ERR_NULL_HANDLE: return "QTC_ERR_NULL_HANDLE";
    case QTC_ERR_STALE_HANDLE: return "QTC_ERR_STALE_HANDLE";
    case QTC_ERR_BAD_HANDLE: return "QTC_ERR_BAD_HANDLE";
    case QTC_ERR_WRONG_TYPE: return "QTC_ERR_WRONG_TYPE";
    case QTC_ERR_NOT_FOUND: return "QTC_ERR_NOT_FOUND";
    case QTC_ERR_AMBIGUOUS: return "QTC_ERR_AMBIGUOUS";
    case QTC_ERR_BAD_ARGS: return "QTC_ERR_BAD_ARGS";
    case QTC_ERR_OUT_OF_RANGE: return "QTC_ERR_OUT_OF_RANGE";
    case QTC_ERR_READ_ONLY: return "QTC_ERR_READ_ONLY";
    case QTC_ERR_CONVERSION: return "QTC_ERR_CONVERSION";
    case QTC_ERR_UNSUPPORTED: return "QTC_ERR_UNSUPPORTED";
    case QTC_ERR_EXCEPTION: return "QTC_ERR_EXCEPTION";
    case QTC_ERR_STATE: return "QTC_ERR_STATE";
    case QTC_ERR_LIMIT: return "QTC_ERR_LIMIT";
    case QTC_ERR_IO: return "QTC_ERR_IO";
    case QTC_ERR_INVALID_ARG: return "QTC_ERR_INVALID_ARG";
    case QTC_ERR_REJECTED: return "QTC_ERR_REJECTED";
    default: return "QTC_ERR_UNKNOWN";
    }
}

qtc_status fail(qtc_status code, const QString &message) {
    t_err = code;
    t_msg = (QString::fromLatin1(status_name(code)) + QStringLiteral(": ") + message).toUtf8();
    int p = st().policy.load(std::memory_order_relaxed);
    if (p != QTC_ERRORS_RETURN) {
        std::fprintf(stderr, "qtc: %s\n", t_msg.constData());
        std::fflush(stderr);
        if (p == QTC_ERRORS_ABORT) std::abort();
    }
    return code;
}
qtc_status fail(qtc_status code, const char *message) { return fail(code, QString::fromUtf8(message)); }

bool gui_thread_ok() { return QThread::currentThreadId() == st().gui_thread; }

// Reads a qtc_str. False (and an error) if it is malformed.
bool read_str(qtc_str s, QString &out, const char *what) {
    if (s.len < 0) {
        out = s.data ? QString::fromUtf8(s.data) : QString();
        return true;
    }
    if (s.len > 0 && !s.data) {
        fail(QTC_ERR_INVALID_ARG, QStringLiteral("%1: NULL data with length %2").arg(QLatin1StringView(what)).arg(s.len));
        return false;
    }
    out = QString::fromUtf8(s.data, qsizetype(s.len));
    return true;
}
bool read_bytes(qtc_str s, QByteArray &out, const char *what) {
    if (s.len < 0) {
        out = s.data ? QByteArray(s.data) : QByteArray();
        return true;
    }
    if (s.len > 0 && !s.data) {
        fail(QTC_ERR_INVALID_ARG, QStringLiteral("%1: NULL data with length %2").arg(QLatin1StringView(what)).arg(s.len));
        return false;
    }
    out = QByteArray(s.data, qsizetype(s.len));
    return true;
}

// ================================================================ registry

void (*on_object_destroyed_hook)(QObject *o, uint64_t handle) = nullptr;
static QSet<QObject *> *g_recently_destroyed = new QSet<QObject *>;

Registry &reg() { static Registry *r = new Registry; return *r; }

uint32_t Registry::alloc_slot(qtc_status *err) {
    if (!free_list.empty()) {
        uint32_t idx = free_list.back();
        free_list.pop_back();
        return idx;
    }
    if (table.size() >= 0x7fffffffu) {
        if (err) *err = QTC_ERR_LIMIT;
        return UINT32_MAX;
    }
    table.emplace_back();
    return uint32_t(table.size() - 1);
}

void Registry::release_slot(uint32_t idx) {
    Slot &s = table[idx];
    s.kind = SK_FREE;
    s.flags = 0;
    s.obj = nullptr;
    s.qp.clear();
    s.val = nullptr;
    s.destroyed = QMetaObject::Connection();
    if (s.gen >= 0xfffffffeu) {
        s.flags = SF_RETIRED;   // never reused: a handle stays unique forever
        return;
    }
    s.gen++;
    free_list.push_back(idx);
}

uint64_t Registry::handle_for(QObject *o) {
    if (!o) return 0;
    std::lock_guard<std::recursive_mutex> lk(mu);
    auto it = by_ptr.constFind(o);
    if (it != by_ptr.constEnd()) {
        if (table[*it].qp) return make_handle(*it, table[*it].gen);
        forget_dead(*it);   // an earlier object at this address died without notice
    }
    qtc_status err = 0;
    uint32_t idx = alloc_slot(&err);
    if (idx == UINT32_MAX) return 0;
    Slot &s = table[idx];
    s.kind = SK_OBJ;
    s.obj = o;
    s.qp = o;
    s.flags = 0;
    s.order = ++order_counter;
    by_ptr.insert(o, idx);
    live_objects++;
    s.destroyed = QObject::connect(o, &QObject::destroyed, [](QObject *ob) { reg().on_destroyed(ob); });
    uint64_t h = make_handle(idx, s.gen);
    roles_register(o);
    return h;
}

uint64_t Registry::adopt_new(QObject *o, bool owned) {
    if (!o) return 0;
    {
        std::lock_guard<std::recursive_mutex> lk(mu);
        g_recently_destroyed->remove(o);
    }
    uint64_t h = handle_for(o);
    if (h && owned) set_owned(h, true);
    return h;
}

void Registry::set_made(uint64_t h) {
    std::lock_guard<std::recursive_mutex> lk(mu);
    uint32_t idx = handle_index(h);
    if (idx >= table.size()) return;
    Slot &s = table[idx];
    if (s.gen == handle_gen(h) && s.kind == SK_OBJ) s.flags |= SF_MADE;
}

bool Registry::made(const QObject *o) {
    std::lock_guard<std::recursive_mutex> lk(mu);
    auto it = by_ptr.constFind(const_cast<QObject *>(o));
    if (it == by_ptr.constEnd()) return false;
    const Slot &s = table[*it];
    return s.kind == SK_OBJ && s.qp == o && (s.flags & SF_MADE);
}

void Registry::set_owned(uint64_t h, bool owned) {
    std::lock_guard<std::recursive_mutex> lk(mu);
    uint32_t idx = handle_index(h);
    if (idx >= table.size()) return;
    Slot &s = table[idx];
    if (s.gen != handle_gen(h) || s.kind != SK_OBJ) return;
    if (owned) s.flags |= SF_OWNED;
    else s.flags &= ~SF_OWNED;
}

QObject *Registry::object(uint64_t h, qtc_status *err, bool allow_dying) {
    std::lock_guard<std::recursive_mutex> lk(mu);
    QObject *o = lookup_locked(h, err, allow_dying);
    if (!o) return nullptr;
    // read under mu: an object of another thread cannot finish dying while mu is held
    QThread *t = o->thread();
    if (t == QThread::currentThread()) return o;
    if (!t && pull_orphan(o)) return o;
    *err = QTC_ERR_WRONG_THREAD;
    return nullptr;
}

QObject *Registry::object_anywhere(uint64_t h, qtc_status *err, bool allow_dying) {
    std::lock_guard<std::recursive_mutex> lk(mu);
    return lookup_locked(h, err, allow_dying);
}

// An object whose QThread was destroyed has no thread affinity, and Qt lets the current thread
// pull it (and the tree it is in) over. GUI thread only: it becomes an ordinary object again.
bool Registry::pull_orphan(QObject *o) {
    if (!gui_thread_ok()) return false;
    QObject *root = o;
    for (int d = 0; root->parent() && d < 10000; d++) root = root->parent();
    if (root->thread()) return false;
    root->moveToThread(QThread::currentThread());
    if (o->thread() != QThread::currentThread()) return false;
    roles_register_tree(root);
    return true;
}

uint64_t Registry::known_handle(const QObject *o) {
    if (!o) return 0;
    std::lock_guard<std::recursive_mutex> lk(mu);
    if (g_recently_destroyed->contains(const_cast<QObject *>(o))) return 0;
    auto it = by_ptr.constFind(const_cast<QObject *>(o));
    if (it == by_ptr.constEnd()) return 0;
    const Slot &s = table[*it];
    if (s.kind != SK_OBJ || !s.qp || (s.flags & SF_DYING)) return 0;
    return make_handle(*it, s.gen);
}

qtc_status Registry::destroy_far(uint64_t h) {
    // mu stays held throughout: the object's own thread, if it is destroying it right now, waits in
    // on_destroyed (emitted before ~QObject frees anything or drops posted events) until we are done
    std::lock_guard<std::recursive_mutex> lk(mu);
    qtc_status err = 0;
    QObject *o = lookup_locked(h, &err, false);
    if (!o) return err;
    QThread *t = o->thread();
    if (t == QThread::currentThread()) return QTC_ERR_STATE;   // not far: the caller's usual path
    table[handle_index(h)].flags |= SF_DYING;
    // wait(0) is true only when the thread never started or has completely finished: then nothing
    // can be running in it, and it would never process a deleteLater
    if (t && !t->wait(QDeadlineTimer(0))) {
        o->deleteLater();
        return QTC_OK;
    }
    delete o;
    return QTC_OK;
}

QObject *Registry::lookup_locked(uint64_t h, qtc_status *err, bool allow_dying) {
    if (h == 0) { *err = QTC_ERR_NULL_HANDLE; return nullptr; }
    uint32_t idx = handle_index(h);
    if (idx >= table.size() || handle_gen(h) == 0) { *err = QTC_ERR_BAD_HANDLE; return nullptr; }
    Slot &s = table[idx];
    if (s.gen != handle_gen(h)) {
        *err = handle_gen(h) < s.gen ? QTC_ERR_STALE_HANDLE : QTC_ERR_BAD_HANDLE;
        return nullptr;
    }
    if (s.kind == SK_FREE) { *err = QTC_ERR_STALE_HANDLE; return nullptr; }
    if (s.kind != SK_OBJ) { *err = QTC_ERR_WRONG_TYPE; return nullptr; }
    if (!s.qp) {   // deleted while its signals were blocked
        if (gui_thread_ok()) forget_dead(idx);
        *err = QTC_ERR_STALE_HANDLE;
        return nullptr;
    }
    if ((s.flags & SF_DYING) && !allow_dying) { *err = QTC_ERR_STALE_HANDLE; return nullptr; }
    *err = 0;
    return s.obj;
}

QVariant *Registry::value(uint64_t h, qtc_status *err) {
    if (h == 0) { *err = QTC_ERR_NULL_HANDLE; return nullptr; }
    std::lock_guard<std::recursive_mutex> lk(mu);
    uint32_t idx = handle_index(h);
    if (idx >= table.size() || handle_gen(h) == 0) { *err = QTC_ERR_BAD_HANDLE; return nullptr; }
    Slot &s = table[idx];
    if (s.gen != handle_gen(h)) {
        *err = handle_gen(h) < s.gen ? QTC_ERR_STALE_HANDLE : QTC_ERR_BAD_HANDLE;
        return nullptr;
    }
    if (s.kind == SK_FREE) { *err = QTC_ERR_STALE_HANDLE; return nullptr; }
    if (s.kind != SK_VAL) { *err = QTC_ERR_WRONG_TYPE; return nullptr; }
    *err = 0;
    return s.val;
}

uint64_t Registry::new_value(QVariant v) {
    std::lock_guard<std::recursive_mutex> lk(mu);
    qtc_status err = 0;
    uint32_t idx = alloc_slot(&err);
    if (idx == UINT32_MAX) { fail(QTC_ERR_LIMIT, "too many live handles"); return 0; }
    Slot &s = table[idx];
    s.kind = SK_VAL;
    s.val = new QVariant(std::move(v));
    s.order = ++order_counter;
    live_values++;
    return make_handle(idx, s.gen);
}

qtc_status Registry::free_value(uint64_t h) {
    qtc_status err = 0;
    QVariant *v = value(h, &err);
    if (!v) return err;
    std::lock_guard<std::recursive_mutex> lk(mu);
    uint32_t idx = handle_index(h);
    delete table[idx].val;
    release_slot(idx);
    live_values--;
    return QTC_OK;
}

qtc_status Registry::mark_dying(uint64_t h, QObject **out) {
    qtc_status err = 0;
    QObject *o = object_anywhere(h, &err);
    if (!o) return err;
    std::lock_guard<std::recursive_mutex> lk(mu);
    table[handle_index(h)].flags |= SF_DYING;
    *out = o;
    return QTC_OK;
}

bool Registry::alive(uint64_t h) {
    if (h == 0) return false;
    std::lock_guard<std::recursive_mutex> lk(mu);
    uint32_t idx = handle_index(h);
    if (idx >= table.size()) return false;
    const Slot &s = table[idx];
    return s.gen == handle_gen(h) && s.kind != SK_FREE && !(s.flags & SF_DYING) && (s.kind != SK_OBJ || !s.qp.isNull());
}

int Registry::kind_of(uint64_t h) {
    if (h == 0) return SK_FREE;
    std::lock_guard<std::recursive_mutex> lk(mu);
    uint32_t idx = handle_index(h);
    if (idx >= table.size()) return SK_FREE;
    const Slot &s = table[idx];
    return s.gen == handle_gen(h) ? int(s.kind) : int(SK_FREE);
}

void Registry::on_destroyed(QObject *o) {
    uint64_t handle = 0;
    {
        std::lock_guard<std::recursive_mutex> lk(mu);
        auto it = by_ptr.find(o);
        if (it == by_ptr.end()) return;
        uint32_t idx = *it;
        by_ptr.erase(it);
        handle = make_handle(idx, table[idx].gen);
        release_slot(idx);
        live_objects--;
        g_recently_destroyed->insert(o);
    }
    // an object moved to a worker thread dies there: the roles index (GUI thread) never had it
    if (gui_thread_ok()) roles_forget(o, true);
    if (on_object_destroyed_hook) on_object_destroyed_hook(o, handle);
}

void Registry::forget_dead(uint32_t idx) {
    QObject *o = nullptr;
    uint64_t handle = 0;
    {
        std::lock_guard<std::recursive_mutex> lk(mu);
        Slot &s = table[idx];
        if (s.kind != SK_OBJ || s.qp) return;
        o = s.obj;   // only its address: the object itself is gone
        auto it = by_ptr.find(o);
        if (it != by_ptr.end() && *it == idx) by_ptr.erase(it);
        QObject::disconnect(s.destroyed);
        handle = make_handle(idx, s.gen);
        release_slot(idx);
        live_objects--;
        g_recently_destroyed->insert(o);
    }
    roles_forget(o, false);
    if (on_object_destroyed_hook) on_object_destroyed_hook(o, handle);
}

void Registry::sweep_dead() {
    std::vector<uint32_t> dead;
    {
        std::lock_guard<std::recursive_mutex> lk(mu);
        for (uint32_t i = 0; i < table.size(); i++)
            if (table[i].kind == SK_OBJ && !table[i].qp) dead.push_back(i);
    }
    for (uint32_t i : dead) forget_dead(i);
}

void Registry::note_blocked(QObject *o) {
    std::lock_guard<std::recursive_mutex> lk(mu);
    for (const QObject *x : blocked_raw_)
        if (x == o) return;
    blocked_.push_back(o);
    blocked_raw_.push_back(o);
}

void Registry::sweep_blocked() {
    std::vector<uint32_t> dead;
    {
        std::lock_guard<std::recursive_mutex> lk(mu);
        if (blocked_.empty()) return;
        for (size_t i = 0; i < blocked_.size();) {
            if (blocked_[i]) { i++; continue; }
            auto it = by_ptr.constFind(blocked_raw_[i]);
            if (it != by_ptr.constEnd() && !table[*it].qp) dead.push_back(*it);
            blocked_[i] = blocked_.back();
            blocked_.pop_back();
            blocked_raw_[i] = blocked_raw_.back();
            blocked_raw_.pop_back();
        }
    }
    for (uint32_t i : dead) forget_dead(i);
}

std::vector<QPointer<QObject>> Registry::owned_roots() {
    std::lock_guard<std::recursive_mutex> lk(mu);
    std::vector<std::pair<uint32_t, QPointer<QObject>>> v;
    for (const Slot &s : table)
        if (s.kind == SK_OBJ && (s.flags & SF_OWNED) && s.qp) v.push_back({s.order, s.qp});
    std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.first > b.first; });
    std::vector<QPointer<QObject>> out;
    for (auto &p : v) out.push_back(p.second);
    return out;
}

void Registry::clear_all() {
    std::lock_guard<std::recursive_mutex> lk(mu);
    blocked_.clear();
    blocked_raw_.clear();
    for (uint32_t i = 0; i < table.size(); i++) {
        Slot &s = table[i];
        if (s.kind == SK_OBJ) {
            QObject::disconnect(s.destroyed);
            release_slot(i);
        } else if (s.kind == SK_VAL) {
            delete s.val;
            release_slot(i);
        }
    }
    by_ptr.clear();
    live_objects = 0;
    live_values = 0;
    g_recently_destroyed->clear();
}

// Signal arguments may name an object in the middle of its destruction.
uint64_t handle_for_signal_arg(QObject *o) {
    if (!o) return 0;
    {
        std::lock_guard<std::recursive_mutex> lk(reg().mu);
        if (g_recently_destroyed->contains(o)) return 0;
    }
    return reg().handle_for(o);
}

void top_level_point() {
    std::lock_guard<std::recursive_mutex> lk(reg().mu);
    g_recently_destroyed->clear();
}

// ================================================================== values

QVariant out_variant_obj(QObject *o) { return QVariant::fromValue(ObjRef{reg().handle_for(o)}); }

static QVariant sanitize_impl(const QVariant &v, bool known) {
    const QMetaType mt = v.metaType();
    if (!mt.isValid()) return v;
    if (mt.flags() & QMetaType::PointerToQObject) {
        QObject *o = *static_cast<QObject *const *>(v.constData());
        return known ? QVariant::fromValue(ObjRef{reg().known_handle(o)}) : out_variant_obj(o);
    }
    const int id = mt.id();
    if (id == QMetaType::QVariantList) {
        QVariantList l = v.toList();
        for (QVariant &e : l) e = sanitize_impl(e, known);
        return QVariant(l);
    }
    if (id == QMetaType::QVariantMap) {
        QVariantMap m = v.toMap();
        for (auto it = m.begin(); it != m.end(); ++it) it.value() = sanitize_impl(it.value(), known);
        return QVariant(m);
    }
    if (id == QMetaType::QVariantHash) {
        QVariantHash m = v.toHash();
        for (auto it = m.begin(); it != m.end(); ++it) it.value() = sanitize_impl(it.value(), known);
        return QVariant(m);
    }
    // an index of a model in another thread cannot be made persistent from here
    if (id == QMetaType::QModelIndex) return known ? QVariant() : QVariant::fromValue(QPersistentModelIndex(v.value<QModelIndex>()));
    if (known && id == QMetaType::QPersistentModelIndex) return QVariant();
    if (id == QMetaType::QObjectStar) {
        QObject *o = v.value<QObject *>();
        return known ? QVariant::fromValue(ObjRef{reg().known_handle(o)}) : out_variant_obj(o);
    }
#ifdef QTC_HAS_MODULE_Qml
    // a JavaScript value (a QML var, an object QML passes as a QVariant: older Qt keeps these as
    // QJSValue) becomes the maps, lists and scalars it holds
    if (mt == QMetaType::fromType<QJSValue>()) {
        const QVariant plain = v.value<QJSValue>().toVariant();
        return plain.metaType() == mt ? QVariant() : sanitize_impl(plain, known);
    }
#endif
    return v;
}

QVariant sanitize(const QVariant &v) { return sanitize_impl(v, false); }
QVariant sanitize_known(const QVariant &v) { return sanitize_impl(v, true); }

bool holds_object(const QVariant &v) {
    const QMetaType mt = v.metaType();
    if (!mt.isValid()) return false;
    if ((mt.flags() & QMetaType::PointerToQObject) || mt.id() == QMetaType::QObjectStar || mt == QMetaType::fromType<ObjRef>())
        return true;
    switch (mt.id()) {
    case QMetaType::QVariantList:
        for (const QVariant &e : v.toList())
            if (holds_object(e)) return true;
        return false;
    case QMetaType::QVariantMap: {
        const QVariantMap m = v.toMap();
        for (auto it = m.cbegin(); it != m.cend(); ++it)
            if (holds_object(it.value())) return true;
        return false;
    }
    case QMetaType::QVariantHash: {
        const QVariantHash m = v.toHash();
        for (auto it = m.cbegin(); it != m.cend(); ++it)
            if (holds_object(it.value())) return true;
        return false;
    }
    default:
        return false;
    }
}

QVariant desanitize(const QVariant &v) {
    const QMetaType mt = v.metaType();
    if (mt == QMetaType::fromType<ObjRef>()) {
        qtc_status err = 0;
        QObject *o = reg().object(v.value<ObjRef>().handle, &err);
        return QVariant::fromValue(o);
    }
    const int id = mt.id();
    if (id == QMetaType::QVariantList) {
        QVariantList l = v.toList();
        for (QVariant &e : l) e = desanitize(e);
        return QVariant(l);
    }
    if (id == QMetaType::QVariantMap) {
        QVariantMap m = v.toMap();
        for (auto it = m.begin(); it != m.end(); ++it) it.value() = desanitize(it.value());
        return QVariant(m);
    }
    if (id == QMetaType::QVariantHash) {
        QVariantHash m = v.toHash();
        for (auto it = m.begin(); it != m.end(); ++it) it.value() = desanitize(it.value());
        return QVariant(m);
    }
    return v;
}
// Typed lists (QList<int>, QList<QPointF> ...) are read in place through QSequentialIterable. Qt
// deprecates it from 6.11 for QMetaSequence::Iterable, which Qt 6.4 does not have: it stays, with
// the warning off here.
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
static bool seq_viewable(QMetaType mt) { return QMetaType::canView(mt, QMetaType::fromType<QSequentialIterable>()); }
static qsizetype seq_size(const QVariant &v) { return v.value<QSequentialIterable>().size(); }
static QVariant seq_at(const QVariant &v, qsizetype i) { return v.value<QSequentialIterable>().at(i); }
QT_WARNING_POP


bool native_list(const QVariant &v) {
    const QMetaType mt = v.metaType();
    if (!mt.isValid()) return false;
    const char *n = mt.name();
    return n && std::strncmp(n, "QList<", 6) == 0 && seq_viewable(mt);
}

int variant_kind(const QVariant &v) {
    const QMetaType mt = v.metaType();
    if (!mt.isValid()) return QTC_T_NONE;
    if (mt == QMetaType::fromType<ObjRef>()) return QTC_T_OBJ;
    switch (mt.id()) {
    case QMetaType::Nullptr: return QTC_T_NULL;
    case QMetaType::Bool: return QTC_T_BOOL;
    case QMetaType::Char: case QMetaType::SChar: case QMetaType::Short: case QMetaType::Int:
    case QMetaType::Long: case QMetaType::LongLong: return QTC_T_INT;
    case QMetaType::UChar: case QMetaType::UShort: case QMetaType::UInt: case QMetaType::ULong:
    case QMetaType::ULongLong: return QTC_T_UINT;
    case QMetaType::Float: case QMetaType::Double: return QTC_T_DOUBLE;
    case QMetaType::QString: return QTC_T_STR;
    case QMetaType::QByteArray: return QTC_T_BYTES;
    case QMetaType::QVariantList: case QMetaType::QStringList: return QTC_T_LIST;
    case QMetaType::QVariantMap: case QMetaType::QVariantHash: return QTC_T_MAP;
    default: break;
    }
    if (mt.flags() & QMetaType::IsEnumeration) return QTC_T_INT;
    if (mt.flags() & QMetaType::PointerToQObject) return QTC_T_OBJ;
    if (native_list(v)) return QTC_T_LIST;
    return QTC_T_VALUE;
}

void Ret::set_obj(QObject *o) {
    clear();
    kind = QTC_T_OBJ;
    h = reg().handle_for(o);
}

void Ret::set_variant(const QVariant &x) {
    QVariant sv = sanitize(x);
    int k = variant_kind(sv);
    switch (k) {
    case QTC_T_NONE: set_none(); return;
    case QTC_T_NULL: clear(); kind = QTC_T_NULL; return;
    case QTC_T_BOOL: set_bool(sv.toBool()); return;
    case QTC_T_INT: set_int(sv.toLongLong()); return;
    case QTC_T_UINT: set_uint(sv.toULongLong()); return;
    case QTC_T_DOUBLE: set_double(sv.toDouble()); return;
    case QTC_T_STR: set_str(sv.toString()); return;
    case QTC_T_BYTES: set_bytes(sv.toByteArray()); return;
    case QTC_T_OBJ: clear(); kind = QTC_T_OBJ; h = sv.value<ObjRef>().handle; v = sv; return;
    default:
        clear();
        kind = k;
        v = sv;
        if (sv.canConvert<QString>() && k == QTC_T_VALUE) s = sv.toString().toUtf8();
        return;
    }
}

ArgStack &args() { static ArgStack *a = new ArgStack; return *a; }
Ret &ret() { static Ret *r = new Ret; return *r; }

// Frames come from a pool, one level per call in progress: taking one costs a few moves.
namespace {
struct FramePool {
    std::vector<std::unique_ptr<ArgStack>> st;
    std::vector<std::unique_ptr<Ret>> r;
    int depth = 0;
    void take(ArgStack *&s, Ret *&rr) {
        if (depth == int(st.size())) {
            st.push_back(std::make_unique<ArgStack>());
            r.push_back(std::make_unique<Ret>());
        }
        s = st[depth].get();
        rr = r[depth].get();
        depth++;
    }
};
FramePool &frames() {
    static auto *p = new FramePool;
    return *p;
}
// Moves the first n arguments of `from` into `to` (whose arguments are empty) and empties `from`.
void move_args(ArgStack &from, ArgStack &to) {
    for (int i = 0; i < from.n; i++) std::swap(to.a[i], from.a[i]);
    to.n = from.n;
    to.overflow = from.overflow;
    from.n = 0;
    from.overflow = false;
}
}  // namespace

CallFrame::CallFrame() {
    frames().take(st, r);
    move_args(args(), *st);
    r->clear_all();
}
CallFrame::~CallFrame() {
    st->clear();
    ret() = std::move(*r);
    r->clear_all();
    frames().depth--;
}
Stash::Stash() {
    frames().take(st, r);
    move_args(args(), *st);
    *r = std::move(ret());
    ret().clear_all();
}
Stash::~Stash() {
    ArgStack &g = args();
    g.clear();
    move_args(*st, g);
    ret() = std::move(*r);
    r->clear_all();
    frames().depth--;
}

QString arg_describe(const Arg &a) {
    switch (a.kind) {
    case AK_NONE: return QStringLiteral("nothing");
    case AK_BOOL: return QStringLiteral("bool");
    case AK_INT: return QStringLiteral("int %1").arg(a.i);
    case AK_UINT: return QStringLiteral("uint %1").arg(a.u);
    case AK_DOUBLE: return QStringLiteral("double %1").arg(a.d);
    case AK_STR: return QStringLiteral("string");
    case AK_BYTES: return QStringLiteral("bytes");
    case AK_OBJ: return QStringLiteral("object");
    case AK_VAL: return QStringLiteral("value of type %1").arg(QLatin1StringView(a.v.typeName() ? a.v.typeName() : "invalid"));
    case AK_NULL: return QStringLiteral("null");
    }
    return QStringLiteral("?");
}

int arg_error(int index, int code, const char *expected, const Arg &a) {
    fail(code, QStringLiteral("argument %1: expected %2, got %3").arg(index + 1).arg(QLatin1StringView(expected)).arg(arg_describe(a)));
    return code;
}

QVariant arg_to_variant(const Arg &a) {
    switch (a.kind) {
    case AK_BOOL: return QVariant(a.b);
    case AK_INT: return QVariant(qlonglong(a.i));
    case AK_UINT: return QVariant(qulonglong(a.u));
    case AK_DOUBLE: return QVariant(a.d);
    case AK_STR: return QVariant(a.s);
    case AK_BYTES: return QVariant(a.bytes);
    case AK_OBJ: {
        qtc_status err = 0;
        QObject *o = reg().object(a.h, &err);
        return QVariant::fromValue(o);
    }
    case AK_VAL: return desanitize(a.v);
    case AK_NULL: return QVariant::fromValue(nullptr);
    default: return QVariant();
    }
}

Arg element_arg(const QVariant &v) {
    Arg a;
    if (v.metaType() == QMetaType::fromType<ObjRef>()) {
        a.kind = AK_OBJ;
        a.h = v.value<ObjRef>().handle;
        return a;
    }
    switch (variant_kind(v)) {
    case QTC_T_BOOL: a.kind = AK_BOOL; a.b = v.toBool(); break;
    case QTC_T_INT: a.kind = AK_INT; a.i = v.toLongLong(); break;
    case QTC_T_UINT: a.kind = AK_UINT; a.u = v.toULongLong(); break;
    case QTC_T_DOUBLE: a.kind = AK_DOUBLE; a.d = v.toDouble(); break;
    case QTC_T_STR: a.kind = AK_STR; a.s = v.toString(); break;
    case QTC_T_BYTES: a.kind = AK_BYTES; a.bytes = v.toByteArray(); break;
    case QTC_T_NULL: a.kind = AK_NULL; break;
    case QTC_T_NONE: a.kind = AK_NULL; break;
    default: a.kind = AK_VAL; a.v = v; break;
    }
    return a;
}

// --------------------------------------------------------- scalar converts

int cv_int64(const Arg &a, int64_t &out, int idx, int64_t lo, int64_t hi) {
    switch (a.kind) {
    case AK_INT:
        if (a.i < lo || a.i > hi) return arg_error(idx, QTC_ERR_OUT_OF_RANGE, "an integer in range", a);
        out = a.i;
        return 0;
    case AK_UINT:
        if (a.u > uint64_t(hi)) return arg_error(idx, QTC_ERR_OUT_OF_RANGE, "an integer in range", a);
        out = int64_t(a.u);
        return 0;
    case AK_BOOL: out = a.b ? 1 : 0; return 0;
    case AK_DOUBLE:
        if (!std::isfinite(a.d) || a.d != std::trunc(a.d) || a.d < double(lo) || a.d > double(hi))
            return arg_error(idx, QTC_ERR_OUT_OF_RANGE, "an integral number in range", a);
        out = int64_t(a.d);
        return 0;
    case AK_VAL: {
        bool ok = false;
        qlonglong x = a.v.toLongLong(&ok);
        if (!ok) return arg_error(idx, QTC_ERR_BAD_ARGS, "an integer", a);
        if (x < lo || x > hi) return arg_error(idx, QTC_ERR_OUT_OF_RANGE, "an integer in range", a);
        out = x;
        return 0;
    }
    default: return arg_error(idx, QTC_ERR_BAD_ARGS, "an integer", a);
    }
}

int cv_uint64(const Arg &a, uint64_t &out, int idx, uint64_t hi) {
    switch (a.kind) {
    case AK_INT:
        if (a.i < 0 || uint64_t(a.i) > hi) return arg_error(idx, QTC_ERR_OUT_OF_RANGE, "an unsigned integer in range", a);
        out = uint64_t(a.i);
        return 0;
    case AK_UINT:
        if (a.u > hi) return arg_error(idx, QTC_ERR_OUT_OF_RANGE, "an unsigned integer in range", a);
        out = a.u;
        return 0;
    case AK_BOOL: out = a.b ? 1 : 0; return 0;
    case AK_DOUBLE:
        if (!std::isfinite(a.d) || a.d != std::trunc(a.d) || a.d < 0 || a.d > double(hi))
            return arg_error(idx, QTC_ERR_OUT_OF_RANGE, "an integral number in range", a);
        out = uint64_t(a.d);
        return 0;
    case AK_VAL: {
        bool ok = false;
        qulonglong x = a.v.toULongLong(&ok);
        if (!ok || x > hi) return arg_error(idx, QTC_ERR_BAD_ARGS, "an unsigned integer", a);
        out = x;
        return 0;
    }
    default: return arg_error(idx, QTC_ERR_BAD_ARGS, "an unsigned integer", a);
    }
}

int cv(const Arg &a, H<bool> &h, int idx) {
    switch (a.kind) {
    case AK_BOOL: h.o = a.b; return 0;
    case AK_INT: h.o = a.i != 0; return 0;
    case AK_UINT: h.o = a.u != 0; return 0;
    case AK_VAL:
        if (a.v.canConvert<bool>()) { h.o = a.v.toBool(); return 0; }
        [[fallthrough]];
    default: return arg_error(idx, QTC_ERR_BAD_ARGS, "a bool", a);
    }
}

int cv(const Arg &a, H<double> &h, int idx) {
    switch (a.kind) {
    case AK_DOUBLE: h.o = a.d; return 0;
    case AK_INT: h.o = double(a.i); return 0;
    case AK_UINT: h.o = double(a.u); return 0;
    case AK_BOOL: h.o = a.b ? 1.0 : 0.0; return 0;
    case AK_VAL: {
        bool ok = false;
        double x = a.v.toDouble(&ok);
        if (ok) { h.o = x; return 0; }
        [[fallthrough]];
    }
    default: return arg_error(idx, QTC_ERR_BAD_ARGS, "a number", a);
    }
}
int cv(const Arg &a, H<float> &h, int idx) {
    H<double> d;
    if (int e = cv(a, d, idx)) return e;
    h.o = float(d.get());
    return 0;
}

int cv(const Arg &a, H<QString> &h, int idx) {
    switch (a.kind) {
    case AK_STR: h.o = a.s; return 0;
    case AK_BYTES: h.o = QString::fromUtf8(a.bytes); return 0;
    case AK_NULL: h.o = QString(); return 0;
    case AK_VAL:
        if (a.v.canConvert<QString>()) { h.o = a.v.toString(); return 0; }
        [[fallthrough]];
    default: return arg_error(idx, QTC_ERR_BAD_ARGS, "a string", a);
    }
}
int cv(const Arg &a, H<QByteArray> &h, int idx) {
    switch (a.kind) {
    case AK_BYTES: h.o = a.bytes; return 0;
    case AK_STR: h.o = a.s.toUtf8(); return 0;
    case AK_NULL: h.o = QByteArray(); return 0;
    case AK_VAL:
        if (a.v.canConvert<QByteArray>()) { h.o = a.v.toByteArray(); return 0; }
        [[fallthrough]];
    default: return arg_error(idx, QTC_ERR_BAD_ARGS, "bytes or a string", a);
    }
}
int cv(const Arg &a, H<QChar> &h, int idx) {
    if (a.kind == AK_STR && a.s.size() == 1) { h.o = a.s.at(0); return 0; }
    if ((a.kind == AK_INT && a.i >= 0 && a.i <= 0xffff) || (a.kind == AK_UINT && a.u <= 0xffff)) {
        h.o = QChar(char16_t(a.kind == AK_INT ? a.i : int64_t(a.u)));
        return 0;
    }
    return arg_error(idx, QTC_ERR_BAD_ARGS, "a one-character string", a);
}
int cv(const Arg &a, H<QVariant> &h, int idx) {
    Q_UNUSED(idx);
    h.o = arg_to_variant(a);
    return 0;
}
int cv(const Arg &a, H<const char *> &h, int idx) {
    switch (a.kind) {
    case AK_STR: a.cache = a.s.toUtf8(); h.o = a.cache.constData(); return 0;
    case AK_BYTES: a.cache = a.bytes; h.o = a.cache.constData(); return 0;
    case AK_NULL: h.o = nullptr; return 0;
    default: return arg_error(idx, QTC_ERR_BAD_ARGS, "a string", a);
    }
}
int cv(const Arg &a, H<QStringView> &h, int idx) {
    H<QString> s;
    if (int e = cv(a, s, idx)) return e;
    a.scache = s.get();
    h.o = QStringView(a.scache);
    return 0;
}
int cv(const Arg &a, H<QAnyStringView> &h, int idx) {
    H<QString> s;
    if (int e = cv(a, s, idx)) return e;
    a.scache = s.get();
    h.o = QAnyStringView(a.scache);
    return 0;
}
int cv(const Arg &a, H<QLatin1StringView> &h, int idx) {
    H<QString> s;
    if (int e = cv(a, s, idx)) return e;
    a.cache = s.get().toLatin1();
    h.o = QLatin1StringView(a.cache);
    return 0;
}
int cv(const Arg &a, H<QUtf8StringView> &h, int idx) {
    H<QByteArray> s;
    if (int e = cv(a, s, idx)) return e;
    a.cache = s.get();
    h.o = QUtf8StringView(a.cache);
    return 0;
}
int cv(const Arg &a, H<QByteArrayView> &h, int idx) {
    H<QByteArray> s;
    if (int e = cv(a, s, idx)) return e;
    a.cache = s.get();
    h.o = QByteArrayView(a.cache);
    return 0;
}
int cv(const Arg &a, H<QPersistentModelIndex> &h, int idx) {
    if (a.kind == AK_VAL) {
        if (a.v.metaType() == QMetaType::fromType<QPersistentModelIndex>()) { h.o = a.v.value<QPersistentModelIndex>(); return 0; }
        if (a.v.metaType() == QMetaType::fromType<QModelIndex>()) { h.o = QPersistentModelIndex(a.v.value<QModelIndex>()); return 0; }
    }
    if (a.kind == AK_NULL) { h.o = QPersistentModelIndex(); return 0; }
    return arg_error(idx, QTC_ERR_BAD_ARGS, "a model index", a);
}
int cv(const Arg &a, H<QModelIndex> &h, int idx) {
    H<QPersistentModelIndex> p;
    if (int e = cv(a, p, idx)) return e;
    h.o = QModelIndex(p.get());   // invalid if the model or the row is gone
    return 0;
}
int cv(const Arg &a, H<QStringList> &h, int idx) {
    if (a.kind == AK_VAL && (a.v.canConvert<QStringList>())) {
        if (a.v.metaType().id() == QMetaType::QVariantList) {
            QStringList out;
            for (const QVariant &e : a.v.toList()) {
                if (!e.canConvert<QString>()) return arg_error(idx, QTC_ERR_BAD_ARGS, "a list of strings", a);
                out.append(e.toString());
            }
            h.o = out;
            return 0;
        }
        h.o = a.v.toStringList();
        return 0;
    }
    if (a.kind == AK_STR) { h.o = QStringList{a.s}; return 0; }
    if (a.kind == AK_NULL) { h.o = QStringList(); return 0; }
    return arg_error(idx, QTC_ERR_BAD_ARGS, "a list of strings", a);
}
int cv(const Arg &a, H<QVariantList> &h, int idx) {
    if (a.kind == AK_VAL && a.v.canConvert<QVariantList>()) { h.o = desanitize(a.v).toList(); return 0; }
    if (a.kind == AK_NULL) { h.o = QVariantList(); return 0; }
    return arg_error(idx, QTC_ERR_BAD_ARGS, "a list", a);
}
int cv(const Arg &a, H<QVariantMap> &h, int idx) {
    if (a.kind == AK_VAL && a.v.canConvert<QVariantMap>()) { h.o = desanitize(a.v).toMap(); return 0; }
    if (a.kind == AK_NULL) { h.o = QVariantMap(); return 0; }
    return arg_error(idx, QTC_ERR_BAD_ARGS, "a map", a);
}
int cv(const Arg &a, H<QVariantHash> &h, int idx) {
    if (a.kind == AK_VAL && a.v.canConvert<QVariantHash>()) { h.o = desanitize(a.v).toHash(); return 0; }
    if (a.kind == AK_NULL) { h.o = QVariantHash(); return 0; }
    return arg_error(idx, QTC_ERR_BAD_ARGS, "a map", a);
}

// Qt's own flags for an object on its way out: deleteLater() was called on it (by Qt
// itself too, e.g. QMessageBox::setCheckBox(nullptr) deletes the old box), or its
// destruction has begun. Handing such an object to a setter would leave the setter's
// owner with a dangling pointer once the deferred delete runs.
namespace {
struct DPtrPeek : QObject {
    static const QObjectData *of(const QObject *o) {
        auto member = &DPtrPeek::d_ptr;   // a pointer to QObject's protected d_ptr, usable on any QObject
        return (o->*member).data();
    }
};
}  // namespace
bool doomed(const QObject *o) {
    const QObjectData *d = DPtrPeek::of(o);
    return d && (d->deleteLaterCalled || d->wasDeleted || d->isDeletingChildren);
}

bool widget_flag(const QObject *o) {
    const QObjectData *d = DPtrPeek::of(o);
    return d && d->isWidget;
}

bool fd_open(int64_t fd) {
#ifdef Q_OS_UNIX
    return fd >= 0 && fd <= std::numeric_limits<int>::max() && ::fcntl(int(fd), F_GETFD) != -1;
#else
    return fd >= 0;
#endif
}

// A dynamic property travels with the object and dies with it (no address reuse to worry about).
static const char kSoundPlaying[] = "_qtc_sound_playing";
bool note_sound(QObject *sound, bool playing) {
    if (sound) sound->setProperty(kSoundPlaying, playing ? QVariant(true) : QVariant());
    return true;
}
bool sound_playing(const QObject *sound) { return sound && sound->property(kSoundPlaying).toBool(); }

QObject *cv_object(const Arg &a, const QMetaObject *mo, int idx, int *err) {
    *err = 0;
    uint64_t h = 0;
    if (a.kind == AK_NULL || (a.kind == AK_INT && a.i == 0) || (a.kind == AK_UINT && a.u == 0)) return nullptr;
    if (a.kind == AK_OBJ) h = a.h;
    else if (a.kind == AK_VAL && a.v.metaType() == QMetaType::fromType<ObjRef>()) h = a.v.value<ObjRef>().handle;
    else { *err = arg_error(idx, QTC_ERR_BAD_ARGS, mo ? mo->className() : "an object", a); return nullptr; }
    if (h == 0) return nullptr;
    qtc_status e = 0;
    QObject *o = reg().object(h, &e);
    if (!o) {
        *err = fail(e, QStringLiteral("argument %1: the object handle is %2").arg(idx + 1)
                           .arg(e == QTC_ERR_STALE_HANDLE ? QStringLiteral("stale (the object was destroyed)") : QLatin1StringView(status_name(e))));
        return nullptr;
    }
    if (doomed(o)) {
        *err = fail(QTC_ERR_STALE_HANDLE, QStringLiteral("argument %1: the object is being deleted (deleteLater was called on it)").arg(idx + 1));
        return nullptr;
    }
    if (mo && !mo->cast(o)) {
        *err = fail(QTC_ERR_WRONG_TYPE, QStringLiteral("argument %1: expected %2, got %3")
                                            .arg(idx + 1).arg(QLatin1StringView(mo->className())).arg(QLatin1StringView(o->metaObject()->className())));
        return nullptr;
    }
    return o;
}

int cv_enum_value(const Arg &a, int eid, int idx, int64_t &out) {
    int64_t v;
    if (int e = cv_int64(a, v, idx, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max())) return e;
    if (eid < 0 || eid >= g_enum_count) { out = v; return 0; }
    const EnumInfo &ei = g_enums[eid];
    if (v < ei.min || v > ei.max)
        return arg_error(idx, QTC_ERR_OUT_OF_RANGE, ei.name, a);
    if (ei.flags & 2) { out = v; return 0; }          // open enum
    if (ei.flags & 1) {                               // flags
        if (uint64_t(v) & ~uint64_t(ei.mask))
            return fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("argument %1: 0x%2 has bits that are not %3 flags")
                                                  .arg(idx + 1).arg(qulonglong(v), 0, 16).arg(QLatin1StringView(ei.name)));
        out = v;
        return 0;
    }
    if (!std::binary_search(ei.values, ei.values + ei.count, v))
        return fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("argument %1: %2 is not a %3 value").arg(idx + 1).arg(v).arg(QLatin1StringView(ei.name)));
    out = v;
    return 0;
}

int arg_to_metatype(const Arg &a, QMetaType target, QVariant &out, int index) {
    if (!target.isValid()) return arg_error(index, QTC_ERR_UNSUPPORTED, "a registered type", a);
    if (target == QMetaType::fromType<QVariant>()) { out = arg_to_variant(a); return 0; }
    if (target.flags() & QMetaType::PointerToQObject) {
        int err = 0;
        QObject *o = cv_object(a, target.metaObject(), index, &err);
        if (err) return err;
        out = QVariant(target, &o);
        return 0;
    }
    if (target.flags() & QMetaType::IsEnumeration) {
        int64_t v;
        if (int e = cv_int64(a, v, index, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max())) return e;
        out = QVariant(qlonglong(v));
        if (!out.convert(target)) return arg_error(index, QTC_ERR_CONVERSION, target.name(), a);
        return 0;
    }
    switch (target.id()) {
    case QMetaType::Bool: { H<bool> h; if (int e = cv(a, h, index)) return e; out = QVariant(h.get()); return 0; }
    case QMetaType::Int: { H<int> h; if (int e = cv(a, h, index)) return e; out = QVariant(h.get()); return 0; }
    case QMetaType::UInt: { H<uint> h; if (int e = cv(a, h, index)) return e; out = QVariant(h.get()); return 0; }
    case QMetaType::LongLong: { H<qlonglong> h; if (int e = cv(a, h, index)) return e; out = QVariant(h.get()); return 0; }
    case QMetaType::ULongLong: { H<qulonglong> h; if (int e = cv(a, h, index)) return e; out = QVariant(h.get()); return 0; }
    case QMetaType::Short: { H<short> h; if (int e = cv(a, h, index)) return e; out = QVariant::fromValue(h.get()); return 0; }
    case QMetaType::UShort: { H<ushort> h; if (int e = cv(a, h, index)) return e; out = QVariant::fromValue(h.get()); return 0; }
    case QMetaType::Long: { H<long> h; if (int e = cv(a, h, index)) return e; out = QVariant::fromValue(h.get()); return 0; }
    case QMetaType::ULong: { H<ulong> h; if (int e = cv(a, h, index)) return e; out = QVariant::fromValue(h.get()); return 0; }
    case QMetaType::Double: { H<double> h; if (int e = cv(a, h, index)) return e; out = QVariant(h.get()); return 0; }
    case QMetaType::Float: { H<float> h; if (int e = cv(a, h, index)) return e; out = QVariant(h.get()); return 0; }
    case QMetaType::QString: { H<QString> h; if (int e = cv(a, h, index)) return e; out = QVariant(h.get()); return 0; }
    case QMetaType::QByteArray: { H<QByteArray> h; if (int e = cv(a, h, index)) return e; out = QVariant(h.get()); return 0; }
    case QMetaType::QStringList: { H<QStringList> h; if (int e = cv(a, h, index)) return e; out = QVariant(h.get()); return 0; }
    case QMetaType::QVariantList: { H<QVariantList> h; if (int e = cv(a, h, index)) return e; out = QVariant(h.get()); return 0; }
    case QMetaType::QVariantMap: { H<QVariantMap> h; if (int e = cv(a, h, index)) return e; out = QVariant(h.get()); return 0; }
    case QMetaType::QModelIndex: { H<QModelIndex> h; if (int e = cv(a, h, index)) return e; out = QVariant::fromValue(h.get()); return 0; }
    case QMetaType::QPersistentModelIndex: { H<QPersistentModelIndex> h; if (int e = cv(a, h, index)) return e; out = QVariant::fromValue(h.get()); return 0; }
    default: break;
    }
    return cv_value_variant(a, target, out, index);
}

int cv_value_variant(const Arg &a, QMetaType target, QVariant &out, int idx) {
    QVariant v;
    switch (a.kind) {
    case AK_VAL: v = desanitize(a.v); break;
    case AK_STR: v = QVariant(a.s); break;
    case AK_BYTES: v = QVariant(a.bytes); break;
    case AK_INT: v = QVariant(qlonglong(a.i)); break;
    case AK_UINT: v = QVariant(qulonglong(a.u)); break;
    case AK_DOUBLE: v = QVariant(a.d); break;
    case AK_BOOL: v = QVariant(a.b); break;
    default: return arg_error(idx, QTC_ERR_BAD_ARGS, target.name(), a);
    }
    if (v.metaType() == target) { out = v; return 0; }
    if (!v.canConvert(target)) return arg_error(idx, QTC_ERR_BAD_ARGS, target.name(), a);
    if (!v.convert(target)) return arg_error(idx, QTC_ERR_CONVERSION, target.name(), a);
    out = v;
    return 0;
}

// ---------------------------------------------------------------- returns

void rt(Ret &r, bool v) { r.set_bool(v); }
void rt(Ret &r, double v) { r.set_double(v); }
void rt(Ret &r, float v) { r.set_double(double(v)); }
void rt(Ret &r, const QString &v) { r.set_str(v); }
void rt(Ret &r, const QByteArray &v) { r.set_bytes(v); }
void rt(Ret &r, QChar v) { r.set_str(QString(v)); }
void rt(Ret &r, const QVariant &v) { r.set_variant(v); }
void rt(Ret &r, const char *v) { r.set_str(v ? QString::fromUtf8(v) : QString()); }
void rt(Ret &r, QStringView v) { r.set_str(v.toString()); }
void rt(Ret &r, QLatin1StringView v) { r.set_str(QString(v)); }
void rt(Ret &r, QByteArrayView v) { r.set_bytes(v.toByteArray()); }
void rt(Ret &r, const QModelIndex &v) { r.set_variant(QVariant::fromValue(QPersistentModelIndex(v))); }
void rt(Ret &r, const QStringList &v) {
    QVariantList l;
    l.reserve(v.size());
    for (const QString &s : v) l.append(s);
    r.set_variant(QVariant(l));
}
void rt(Ret &r, const QVariantList &v) { r.set_variant(QVariant(v)); }
void rt(Ret &r, const QVariantMap &v) { r.set_variant(QVariant(v)); }
void rt(Ret &r, const QVariantHash &v) { r.set_variant(QVariant(v)); }

// ================================================================ classes

static QHash<QByteArray, const ClassDef *> &classes() { static auto *c = new QHash<QByteArray, const ClassDef *>; return *c; }
static QList<QByteArray> &class_names() { static auto *c = new QList<QByteArray>; return *c; }
struct NameIndex { QHash<QByteArray, QList<const MethodDef *>> by_name; };
static QHash<const ClassDef *, NameIndex *> &name_indexes() { static auto *c = new QHash<const ClassDef *, NameIndex *>; return *c; }

void register_classes(const ClassDef *defs, int count) {
    for (int i = 0; i < count; i++) {
        QByteArray n(defs[i].name);
        if (!classes().contains(n)) class_names().append(n);
        classes().insert(n, &defs[i]);
        // QMetaType::fromName only knows a type once fromType<T>() ran for it
        if (defs[i].kind == CK_VALUE && defs[i].metatype) (void)defs[i].metatype().id();
    }
}
const ClassDef *find_class(const QByteArray &name) { return classes().value(name, nullptr); }
void unregister_class(const QByteArray &name) {
    const ClassDef *c = classes().value(name, nullptr);
    if (!c) return;
    classes().remove(name);
    class_names().removeAll(name);
    delete name_indexes().take(c);
}

static const QList<const MethodDef *> &methods_named(const ClassDef *c, const QByteArray &name) {
    NameIndex *ni = name_indexes().value(c, nullptr);
    if (!ni) {
        ni = new NameIndex;
        for (uint32_t i = 0; i < c->nmethods; i++) ni->by_name[QByteArray(c->methods[i].name)].append(&c->methods[i]);
        name_indexes().insert(c, ni);
    }
    static const QList<const MethodDef *> none;
    auto it = ni->by_name.constFind(name);
    return it == ni->by_name.constEnd() ? none : *it;
}

static bool need_ok(const ClassDef *c, QString *why) {
    int k = st().kind;
    if (c->need == NEED_WIDGETS && k != QTC_APP_WIDGETS) {
        *why = QStringLiteral("%1 needs qtc_init(QTC_APP_WIDGETS)").arg(QLatin1StringView(c->name));
        return false;
    }
    if (c->need == NEED_GUI && k == QTC_APP_CORE) {
        *why = QStringLiteral("%1 needs qtc_init(QTC_APP_GUI) or QTC_APP_WIDGETS").arg(QLatin1StringView(c->name));
        return false;
    }
    return true;
}

// ============================================================ resolution

struct Cand {
    mutable int call_n = 0;          // > 0: call at this arity, filling pointer parameters with null (see score)
    mutable bool skip = false;       // the arguments leave out the pointer parameters (else they are trailing)
    const MethodDef *md = nullptr;   // generated thunk
    const ClassDef *cls = nullptr;
    int base = -1;                   // value receivers: which base the method came from
    QMetaMethod mm;                  // meta-object method
    int depth = 0;
};

// Overloads that differ only in the width or signedness of an integer, float against double, or
// QString against a string view do the same thing: the order below picks one instead of
// reporting the call as ambiguous. Signed for a signed argument, unsigned for an unsigned one,
// then the widest; double over float; the owning QString and QByteArray over views.
static int int_type_penalty(const char *type, bool arg_unsigned) {
    if (!type) return 0;
    struct T { const char *name; int bits; bool is_unsigned; };
    static constexpr T types[] = {
        {"qlonglong", 64, false}, {"qint64", 64, false}, {"long long", 64, false}, {"long", 64, false},
        {"qsizetype", 64, false}, {"qptrdiff", 64, false}, {"qintptr", 64, false}, {"int64_t", 64, false},
        {"int", 32, false}, {"qint32", 32, false}, {"int32_t", 32, false},
        {"short", 16, false}, {"qint16", 16, false}, {"int16_t", 16, false},
        {"signed char", 8, false}, {"qint8", 8, false}, {"char", 8, false}, {"int8_t", 8, false},
        {"qulonglong", 64, true}, {"quint64", 64, true}, {"unsigned long long", 64, true}, {"ulong", 64, true},
        {"unsigned long", 64, true}, {"size_t", 64, true}, {"quintptr", 64, true}, {"uint64_t", 64, true},
        {"uint", 32, true}, {"quint32", 32, true}, {"unsigned int", 32, true}, {"unsigned", 32, true},
        {"uint32_t", 32, true}, {"char32_t", 32, true}, {"QRgb", 32, true},
        {"ushort", 16, true}, {"quint16", 16, true}, {"unsigned short", 16, true}, {"uint16_t", 16, true},
        {"char16_t", 16, true}, {"uchar", 8, true}, {"quint8", 8, true}, {"unsigned char", 8, true}, {"uint8_t", 8, true},
    };
    for (const T &t : types) {
        if (std::strcmp(type, t.name) != 0) continue;
        // long is 32 bits on Windows: long long first
        const int rank = t.bits == 64 ? (std::strstr(t.name, "long") && !std::strstr(t.name, "long long") ? 1 : 0)
                                      : (t.bits == 32 ? 2 : (t.bits == 16 ? 3 : 4));
        return (t.is_unsigned != arg_unsigned ? 5 : 0) + rank;
    }
    return 0;
}
// Whether an enum parameter takes the value (as cv_enum_value checks it): an overload whose enum
// does not have it is not a match (QDialogButtonBox(Qt::Orientation) for StandardButtons).
static bool enum_takes(int eid, int64_t v) {
    if (eid < 0 || eid >= g_enum_count) return true;
    const EnumInfo &ei = g_enums[eid];
    if (v < ei.min || v > ei.max) return false;
    if (ei.flags & 2) return true;
    if (ei.flags & 1) return (uint64_t(v) & ~uint64_t(ei.mask)) == 0;
    return std::binary_search(ei.values, ei.values + ei.count, v);
}
// "QList<QTextLength>" -> "QTextLength"; empty for lists of anything (QVariantList, QStringList, ...)
static QByteArray typed_list_element(const char *type) {
    if (!type || std::strncmp(type, "QList<", 6) != 0) return {};
    QByteArray t(type + 6);
    t.chop(1);
    if (t == "QVariant" || t == "QString") return {};
    return t;
}
static int string_type_penalty(const char *type) {
    if (!type || std::strcmp(type, "QString") == 0) return 0;
    if (std::strstr(type, "QLatin1String")) return 5;   // UTF-8 text would lose what Latin-1 cannot hold
    if (std::strstr(type, "Utf8StringView")) return 3;   // QUtf8StringView is QBasicUtf8StringView<false>
    if (std::strstr(type, "QStringView")) return 2;
    if (std::strstr(type, "QAnyStringView")) return 1;
    return 0;
}
static int bytes_type_penalty(const char *type) {
    if (!type || std::strcmp(type, "QByteArray") == 0) return 0;
    if (std::strcmp(type, "const char*") == 0) return 3;   // keeps neither the length nor embedded NULs
    if (std::strstr(type, "Utf8StringView")) return 2;
    if (std::strstr(type, "QByteArrayView")) return 1;
    return 0;
}

static int score_param_kind(const Arg &a, uint8_t kind, const QMetaObject *mo, const char *type, int *hard, int eid = -1) {
    switch (kind) {
    case PK_BOOL:
        if (a.kind == AK_BOOL) return 100;
        if (a.kind == AK_INT || a.kind == AK_UINT) return 40;
        if (a.kind == AK_VAL && a.v.canConvert<bool>()) return 20;
        return 0;
    case PK_INT: case PK_UINT:
        if (a.kind == AK_INT || a.kind == AK_UINT) return 100 - int_type_penalty(type, a.kind == AK_UINT);
        if (a.kind == AK_BOOL) return 50;
        if (a.kind == AK_DOUBLE) return (std::isfinite(a.d) && a.d == std::trunc(a.d)) ? 30 : 0;
        if (a.kind == AK_VAL && a.v.canConvert<qlonglong>()) return 20;
        return 0;
    case PK_ENUM: case PK_FLAGS:
        if (a.kind == AK_INT || a.kind == AK_UINT) {
            if (!enum_takes(eid, a.kind == AK_INT ? a.i : int64_t(a.u))) return 10;
            // Qt::Modifiers is the older spelling of Qt::KeyboardModifiers (QKeyCombination takes both)
            return (type && std::strstr(type, "Qt::Modifier")) ? 89 : 90;
        }
        if (a.kind == AK_VAL && a.v.canConvert<qlonglong>()) return 20;
        return 0;
    case PK_DOUBLE: {
        const int narrow = (type && std::strcmp(type, "float") == 0) ? 1 : 0;
        if (a.kind == AK_DOUBLE) return 100 - narrow;
        if (a.kind == AK_INT || a.kind == AK_UINT) return 70 - narrow;
        if (a.kind == AK_VAL && a.v.canConvert<double>()) return 20;
        return 0;
    }
    case PK_STR: {
        const int view = string_type_penalty(type);
        if (a.kind == AK_STR) return 100 - view;
        if (a.kind == AK_BYTES) return 70 - view;
        if (a.kind == AK_NULL) return 40;
        if (a.kind == AK_VAL && a.v.canConvert<QString>()) return 25;
        return 0;
    }
    case PK_BYTES: {
        const int view = bytes_type_penalty(type);
        if (a.kind == AK_BYTES) return 100 - view;
        if (a.kind == AK_STR) return 85 - view;
        if (a.kind == AK_NULL) return 40;
        if (a.kind == AK_VAL && a.v.canConvert<QByteArray>()) return 25;
        return 0;
    }
    case PK_CHAR:
        if (a.kind == AK_STR && a.s.size() == 1) return 80;
        if (a.kind == AK_INT || a.kind == AK_UINT) return 40;
        return 0;
    case PK_OBJ: {
        uint64_t h = 0;
        if (a.kind == AK_NULL || (a.kind == AK_INT && a.i == 0)) return 50;
        if (a.kind == AK_OBJ) h = a.h;
        else if (a.kind == AK_VAL && a.v.metaType() == QMetaType::fromType<ObjRef>()) h = a.v.value<ObjRef>().handle;
        else return 0;
        if (h == 0) return 50;
        qtc_status e = 0;
        QObject *o = reg().object(h, &e);
        if (!o) { *hard = e; return 0; }
        if (!mo) return 60;
        if (!mo->cast(o)) return 0;
        int d = 0;
        for (const QMetaObject *m = o->metaObject(); m && m != mo; m = m->superClass()) d++;
        // f(QObject *) and f(const QObject *) are the same call
        return 100 - std::min(d, 30) - (type && std::strncmp(type, "const ", 6) == 0 ? 1 : 0);
    }
    case PK_VARIANT:
        // anything fits a QVariant: a parameter of the value's own kind wins
        return a.kind == AK_VAL ? 79 : (a.kind == AK_NONE ? 0 : 60);
    case PK_INDEX: {
        const QMetaType t = a.kind == AK_VAL ? a.v.metaType() : QMetaType();
        const bool persistent = type && std::strstr(type, "QPersistentModelIndex");
        if (t == QMetaType::fromType<QModelIndex>()) return persistent ? 99 : 100;
        if (t == QMetaType::fromType<QPersistentModelIndex>()) return persistent ? 100 : 99;
        if (a.kind == AK_NULL) return 30;
        return 0;
    }
    case PK_LIST: {
        if (a.kind == AK_NULL) return 30;
        if (a.kind != AK_VAL || !a.v.canConvert<QVariantList>()) return 0;
        // a list of the element type, as far as its first element tells
        if (const QByteArray elem = typed_list_element(type); !elem.isEmpty()) {
            const QVariantList l = a.v.toList();
            const QMetaType et = QMetaType::fromName(elem);
            if (!l.isEmpty() && et.isValid() && l.first().metaType() != et && !l.first().canConvert(et)) return 10;
        }
        // a QVariantList keeps what the list holds, a QStringList only its text
        return (type && std::strcmp(type, "QStringList") == 0) || (type && std::strcmp(type, "QList<QString>") == 0) ? 79 : 80;
    }
    case PK_MAP:
        // qtc's maps are QVariantMaps: a QVariantHash overload is the second choice
        if (a.kind == AK_VAL && a.v.canConvert<QVariantMap>()) return (type && std::strstr(type, "QHash")) ? 79 : 80;
        if (a.kind == AK_NULL) return 30;
        return 0;
    case PK_NULLPTR:
    case PK_OUT:
        if (a.kind == AK_NULL) return 100;
        if ((a.kind == AK_INT && a.i == 0) || (a.kind == AK_UINT && a.u == 0)) return 90;
        return 0;
    case PK_VALUE: {
        QMetaType target = type ? QMetaType::fromName(type) : QMetaType();
        if (a.kind == AK_VAL) {
            if (target.isValid() && a.v.metaType() == target) return 100;
            if (target.isValid() && a.v.canConvert(target)) return 30;
            if (!target.isValid()) return 15;
            return 0;
        }
        if (!target.isValid()) return 0;
        QVariant probe;
        switch (a.kind) {
        case AK_STR: probe = QVariant(a.s); break;
        case AK_BYTES: probe = QVariant(a.bytes); break;
        case AK_INT: probe = QVariant(qlonglong(a.i)); break;
        case AK_UINT: probe = QVariant(qulonglong(a.u)); break;
        case AK_DOUBLE: probe = QVariant(a.d); break;
        case AK_BOOL: probe = QVariant(a.b); break;
        default: return 0;
        }
        return probe.canConvert(target) ? 20 : 0;
    }
    }
    return 0;
}

static uint8_t kind_of_metatype(QMetaType t, const QMetaObject **mo) {
    *mo = nullptr;
    if (!t.isValid()) return 0;
    if (t == QMetaType::fromType<QVariant>()) return PK_VARIANT;
    if (t.flags() & QMetaType::PointerToQObject) { *mo = t.metaObject(); return PK_OBJ; }
    if (t.flags() & QMetaType::IsEnumeration) return PK_ENUM;
    switch (t.id()) {
    case QMetaType::Bool: return PK_BOOL;
    case QMetaType::Int: case QMetaType::Long: case QMetaType::LongLong: case QMetaType::Short: case QMetaType::Char:
    case QMetaType::SChar: return PK_INT;
    case QMetaType::UInt: case QMetaType::ULong: case QMetaType::ULongLong: case QMetaType::UShort: case QMetaType::UChar: return PK_UINT;
    case QMetaType::Double: case QMetaType::Float: return PK_DOUBLE;
    case QMetaType::QString: return PK_STR;
    case QMetaType::QByteArray: return PK_BYTES;
    case QMetaType::QChar: return PK_CHAR;
    case QMetaType::QVariantList: case QMetaType::QStringList: return PK_LIST;
    case QMetaType::QVariantMap: case QMetaType::QVariantHash: return PK_MAP;
    case QMetaType::QModelIndex: case QMetaType::QPersistentModelIndex: return PK_INDEX;
    default: return PK_VALUE;
    }
}

static inline bool pointer_param(uint8_t k) { return k == PK_NULLPTR || k == PK_OUT; }

// The smallest arity at which n arguments fill exactly the parameters that are not
// pointer parameters, with at least one pointer parameter among them (0: none).
static int compact_arity(const MethodDef *md, int n) {
    int seen = 0;
    bool any = false;
    for (int k = 0; k <= md->nparams; k++) {
        if (k >= md->min_args && seen == n && any) return k;
        if (k == md->nparams) break;
        if (pointer_param(md->params[k].kind)) any = true;
        else seen++;
    }
    return 0;
}

// Out-parameters right after the last argument are passed too, so their values can be read.
static int with_trailing_outs(const MethodDef *md, int k) {
    while (k < md->nparams && md->params[k].kind == PK_OUT) k++;
    return k;
}

// Returns the candidate's score for these arguments (0 = not callable). Thunk
// candidates accept the arguments either positionally, or with the pointer
// parameters (bool *ok, int *x, ...) left out; those get null, and out-parameters
// get storage that qtc_out_* reads.
static int64_t score(const Cand &c, const ArgStack &st, int *hard) {
    int n = st.n;
    int64_t worst = 1000, sum = 0;
    c.call_n = 0;
    c.skip = false;
    if (c.md) {
        bool direct = n >= c.md->min_args && n <= c.md->nparams;
        if (direct) {
            for (int i = 0; i < n; i++) {
                const ParamDesc &p = c.md->params[i];
                int s = score_param_kind(st.a[i], p.kind, p.meta, p.type, hard, p.enum_id);
                if (*hard) return 0;
                if (s == 0) { direct = false; break; }
                worst = std::min<int64_t>(worst, s);
                sum += s;
            }
            if (direct) {
                int k = with_trailing_outs(c.md, n);
                if (k > n) c.call_n = k;
            }
        }
        if (!direct) {
            int k = compact_arity(c.md, n);
            if (!k) return 0;
            worst = 1000;
            sum = 0;
            int j = 0;
            for (int i = 0; i < k; i++) {
                const ParamDesc &p = c.md->params[i];
                if (pointer_param(p.kind)) continue;
                int s = score_param_kind(st.a[j++], p.kind, p.meta, p.type, hard, p.enum_id);
                if (*hard || s == 0) return 0;
                worst = std::min<int64_t>(worst, s);
                sum += s;
            }
            c.call_n = with_trailing_outs(c.md, k);
            c.skip = true;
            sum -= 3;
        }
    } else {
        if (n != c.mm.parameterCount()) return 0;
        for (int i = 0; i < n; i++) {
            const QMetaObject *mo = nullptr;
            QMetaType t = c.mm.parameterMetaType(i);
            uint8_t k = kind_of_metatype(t, &mo);
            if (!k) return 0;
            int s = score_param_kind(st.a[i], k, mo, t.name(), hard);
            if (*hard) return 0;
            if (s == 0) return 0;
            worst = std::min<int64_t>(worst, s);
            sum += s;
        }
    }
    int64_t sc = worst * 1000000 + sum * 100;
    if (c.md) sc += 10;                                  // prefer statically typed thunks
    if (c.md && n == c.md->nparams) sc += 5;             // prefer an exact arity
    if (c.md && std::strstr(c.md->sig, "const ")) sc -= 1; // f(T *) over the same f(const T *)
    sc -= c.depth;                                       // prefer the most derived class
    return sc + 1;
}

static QString cand_sig(const Cand &c) {
    if (c.md) return QString::fromLatin1(c.md->sig);
    return QString::fromLatin1(c.mm.methodSignature());
}

// The parameter types of a normalized signature: "f(int,QMap<int,QString>)" -> int, QMap<int,QString>.
static QList<QByteArray> param_types_of(const char *sig) {
    QList<QByteArray> out;
    const char *p = std::strchr(sig, '(');
    if (!p) return out;
    int depth = 0;
    QByteArray cur;
    for (const char *c = p + 1; *c; c++) {
        if (depth == 0 && (*c == ',' || *c == ')')) {
            if (!cur.isEmpty()) out.append(cur);
            cur.clear();
            if (*c == ')') break;
            continue;
        }
        if (*c == '<' || *c == '(') depth++;
        else if (*c == '>' || *c == ')') depth--;
        cur.append(*c);
    }
    return out;
}

// A signature names a bound method when it is the method's, or the method's with trailing
// defaulted parameters left out ("start()" for start(QAbstractAnimation::DeletionPolicy)).
static bool sig_matches(const MethodDef *md, const QByteArray &sig) {
    if (sig.isEmpty()) return true;
    if (sig == md->sig) return true;
    const char *open = std::strchr(md->sig, '(');
    const int b = sig.indexOf('(');
    if (!open || b < 0 || QByteArrayView(md->sig, open - md->sig) != QByteArrayView(sig.constData(), b)) return false;
    const QList<QByteArray> fl = param_types_of(md->sig);
    const QList<QByteArray> sl = param_types_of(sig.constData());
    if (sl.size() < md->min_args || sl.size() > fl.size()) return false;
    for (int i = 0; i < sl.size(); i++)
        if (sl[i] != fl[i]) return false;
    return true;
}

// Methods only a meta-object has (slots, signals and invokables with no bound thunk). For a class
// qtc binds, its bound methods are its API, and what the generator left out stays out: private
// signals (emitting rowsInserted() or destroyed() by hand breaks the views and proxies that
// listen) and denied methods (QThread::terminate, QObject::deleteLater) would otherwise be one
// meta-call away, around every check qtc makes. Other classes are reached through their
// meta-objects: qtc's own, QML types (an object QML made, and what QML declares), and the
// program's own C++ classes; Qt's private classes (QComboBoxPrivateContainer, ...) are not.
static const QMetaObject *declaring_class(const QMetaObject *mo, int index) {
    const QMetaObject *x = mo;
    while (x->superClass() && index < x->methodOffset()) x = x->superClass();
    return x;
}
// Qt's own class names: QWidget, QQuickRectangle, QFFmpeg::..., QV4::..., QtPrivate::..., Qt3DCore::...
static bool qt_style_name(const char *n) {
    if (!n || n[0] != 'Q') return false;
    if (n[1] >= 'A' && n[1] <= 'Z') return true;
    return n[1] == 't' && ((n[2] >= 'A' && n[2] <= 'Z') || (n[2] >= '0' && n[2] <= '9'));
}
static bool meta_callable(const QMetaObject *decl, bool qml_made) {
    if (const ClassDef *c = find_class(QByteArray(decl->className()))) return std::strcmp(c->module, "qtc") == 0;
    return qml_made || !qt_style_name(decl->className());
}
static bool qml_object(const QObject *o) {
#ifdef QTC_HAS_MODULE_Qml
    return qmlContext(o) != nullptr;
#else
    Q_UNUSED(o);
    return false;
#endif
}

static void collect_object(QObject *o, const QByteArray &name, const QByteArray &sig, bool want_static, std::vector<Cand> &out) {
    int depth = 0;
    for (const QMetaObject *mo = o->metaObject(); mo; mo = mo->superClass(), depth++) {
        if (const ClassDef *c = find_class(QByteArray(mo->className()))) {
            for (const MethodDef *md : methods_named(c, name)) {
                if (!sig_matches(md, sig)) continue;
                if (want_static && !(md->flags & MF_STATIC)) continue;
                Cand k;
                k.md = md;
                k.cls = c;
                k.depth = depth;
                out.push_back(k);
            }
        }
    }
    if (want_static) return;
    const QMetaObject *mo = o->metaObject();
    const bool qml = qml_object(o);
    for (int i = 0; i < mo->methodCount(); i++) {
        QMetaMethod m = mo->method(i);
        if (m.access() != QMetaMethod::Public || m.methodType() == QMetaMethod::Constructor) continue;
        if (m.name() != name) continue;
        if (!sig.isEmpty() && m.methodSignature() != sig) continue;
        if (!meta_callable(declaring_class(mo, i), qml)) continue;
        Cand k;
        k.mm = m;
        int d = 0;
        for (const QMetaObject *x = mo; x && i < x->methodOffset(); x = x->superClass()) d++;
        k.depth = d;
        out.push_back(k);
    }
}

static void collect_value(const ClassDef *c, const QByteArray &name, const QByteArray &sig, std::vector<Cand> &out, int base, int depth) {
    for (const MethodDef *md : methods_named(c, name)) {
        if (!sig_matches(md, sig)) continue;
        Cand k;
        k.md = md;
        k.cls = c;
        k.base = base;
        k.depth = depth;
        out.push_back(k);
    }
    if (c->bases)
        for (int i = 0; c->bases[i]; i++)
            if (const ClassDef *b = find_class(QByteArray(c->bases[i])))
                collect_value(b, name, sig, out, base < 0 ? i : base, depth + 1);
}

// Candidates depend only on the class, the name and the signature: they are cached per
// static meta-object (objects with a dynamic one, such as QML objects, are collected each time)
// and per value class. Scoring against the arguments still happens on every call.
struct CandKey {
    const void *cls;
    QByteArray name, sig;
    bool want_static;
    bool qml;   // the object came from QML: its class's meta methods count (see meta_callable)
    bool operator==(const CandKey &o) const {
        return cls == o.cls && want_static == o.want_static && qml == o.qml && name == o.name && sig == o.sig;
    }
};
static size_t qHash(const CandKey &k, size_t seed = 0) { return qHashMulti(seed, k.cls, k.name, k.sig, k.want_static, k.qml); }
static QHash<CandKey, std::vector<Cand>> &cand_cache() {
    static auto *h = new QHash<CandKey, std::vector<Cand>>;
    return *h;
}
void clear_cand_cache() { cand_cache().clear(); }

// The protected methods an overridable object makes callable (qtc_override.h): its own, never
// cached by class (the same meta-object serves plain objects of the class).
static void collect_extras(QObject *o, const QByteArray &name, const QByteArray &sig, std::vector<Cand> &out) {
    uint32_t n = 0;
    const MethodDef *ex = ov_extras(o, &n);
    for (uint32_t i = 0; i < n; i++) {
        if (name != ex[i].name || !sig_matches(&ex[i], sig)) continue;
        Cand k;
        k.md = &ex[i];
        k.cls = nullptr;
        k.depth = 0;
        out.push_back(k);
    }
}

static void collect_object_cached(QObject *o, const QByteArray &name, const QByteArray &sig, bool want_static, std::vector<Cand> &out) {
    if (!want_static) collect_extras(o, name, sig, out);
    const QObjectData *d = DPtrPeek::of(o);
    if (d && d->metaObject) {   // a dynamic meta-object: it can change, and its address can be reused
        collect_object(o, name, sig, want_static, out);
        return;
    }
    CandKey key{o->metaObject(), name, sig, want_static, !want_static && qml_object(o)};
    auto it = cand_cache().constFind(key);
    if (it == cand_cache().constEnd()) {
        std::vector<Cand> fresh;
        collect_object(o, name, sig, want_static, fresh);
        it = cand_cache().insert(key, std::move(fresh));
    }
    out.insert(out.end(), it->begin(), it->end());
}

bool method_callable(QObject *o, const QByteArray &signature, QString *why) {
    const int p = signature.indexOf('(');
    const QByteArray name = p < 0 ? signature : signature.left(p);
    const QByteArray sig = p < 0 ? QByteArray() : QMetaObject::normalizedSignature(signature.constData());
    std::vector<Cand> cands;
    collect_object_cached(o, name, sig, false, cands);
    if (!cands.empty()) return true;
    *why = QStringLiteral("%1::%2 is not available through qtc").arg(QLatin1StringView(o->metaObject()->className()),
                                                                     QString::fromLatin1(p < 0 ? name : sig));
    return false;
}

std::vector<QMetaMethod> meta_candidates(QObject *o, const QByteArray &spec) {
    const int p = spec.indexOf('(');
    const QByteArray name = p < 0 ? spec : spec.left(p);
    const QByteArray sig = p < 0 ? QByteArray() : QMetaObject::normalizedSignature(spec.constData());
    std::vector<Cand> cands;
    collect_object_cached(o, name, sig, false, cands);
    std::vector<QMetaMethod> out;
    for (const Cand &k : cands)
        if (!k.md && k.mm.isValid()) out.push_back(k.mm);
    return out;
}

const ClassDef *bound_class_of(const QObject *o) {
    for (const QMetaObject *mo = o->metaObject(); mo; mo = mo->superClass()) {
        const QByteArray n(mo->className());
        if (dyn_class_named(n)) continue;   // a runtime class: its Qt base counts
        if (const ClassDef *c = find_class(n)) return c;
    }
    return nullptr;
}

QByteArray bound_method_for_signal(QObject *o, const QByteArray &name, const QList<QByteArray> &types, int *nargs) {
    QByteArray best;
    int bestn = -1;
    for (const QMetaObject *mo = o->metaObject(); mo; mo = mo->superClass()) {
        const ClassDef *c = find_class(QByteArray(mo->className()));
        if (!c) continue;
        for (const MethodDef *md : methods_named(c, name)) {
            if (md->flags & MF_STATIC) continue;
            const QList<QByteArray> ps = param_types_of(md->sig);
            for (int k = int(std::min(ps.size(), types.size())); k >= int(md->min_args) && k > bestn; k--) {
                bool same = true;
                for (int i = 0; i < k && same; i++)
                    same = QMetaObject::normalizedType(ps[i].constData()) == QMetaObject::normalizedType(types[i].constData());
                if (same) {
                    bestn = k;
                    best = md->sig;
                    break;
                }
            }
        }
    }
    *nargs = bestn;
    return best;
}

static void collect_value_cached(const ClassDef *c, const QByteArray &name, const QByteArray &sig, std::vector<Cand> &out) {
    CandKey key{c, name, sig, false, false};
    auto it = cand_cache().constFind(key);
    if (it == cand_cache().constEnd()) {
        std::vector<Cand> fresh;
        collect_value(c, name, sig, fresh, -1, 0);
        it = cand_cache().insert(key, std::move(fresh));
    }
    out.insert(out.end(), it->begin(), it->end());
}

static bool split_method(qtc_str method, QByteArray &name, QByteArray &sig) {
    QByteArray m;
    if (!read_bytes(method, m, "method")) return false;
    m = m.trimmed();
    if (m.isEmpty()) {
        fail(QTC_ERR_INVALID_ARG, "empty method name");
        return false;
    }
    int p = m.indexOf('(');
    if (p < 0) {
        name = m;
        sig.clear();
    } else {
        name = m.left(p).trimmed();
        sig = QMetaObject::normalizedSignature(m.constData());
    }
    return true;
}

static int invoke_meta(QObject *o, const QMetaMethod &m, ArgStack &st, Ret &r) {
    const int n = st.n;
    QVariant vals[kMaxArgs];
    void *argv[kMaxArgs + 1];
    for (int i = 0; i < n; i++) {
        const QMetaType pt = m.parameterMetaType(i);
        if (int e = arg_to_metatype(st.a[i], pt, vals[i], i)) return e;
        // A QVariant parameter (every untyped QML function parameter) takes the variant itself.
        // data() would point at the value inside it, which is the variant's own address only while
        // the value is stored inline: a large value (a QTransform) would be read as a variant.
        argv[i + 1] = pt == QMetaType::fromType<QVariant>() ? static_cast<void *>(&vals[i]) : vals[i].data();
    }
    QMetaType rtype = m.returnMetaType();
    QVariant rv;
    if (rtype.isValid() && rtype.id() != QMetaType::Void) {
        rv = QVariant(rtype);
        argv[0] = rv.data();
    } else {
        argv[0] = nullptr;
    }
    QMetaObject::metacall(o, QMetaObject::InvokeMetaMethod, m.methodIndex(), argv);
    if (argv[0]) {
        if (rtype == QMetaType::fromType<QVariant>()) r.set_variant(*static_cast<QVariant *>(rv.data()));
        else r.set_variant(rv);
    } else {
        r.set_none();
    }
    return 0;
}

// What a call is, for error messages, built only when one is needed: "QLabel::setText".
struct Label {
    QLatin1StringView a;   // class
    QLatin1StringView b;   // method (empty for constructors)
    const char *fmt;       // "%1::%2", "new %1", ...
    QString str() const { return b.isEmpty() ? QString::fromLatin1(fmt).arg(a) : QString::fromLatin1(fmt).arg(a, b); }
};

static qtc_status run_candidates(std::vector<Cand> &cands, void *self_obj, QVariant *self_val, const Label &label, ArgStack &st, Ret &r) {
    const auto what = [&]() { return label.str(); };
    if (st.overflow) return fail(QTC_ERR_BAD_ARGS, QStringLiteral("%1: more than %2 arguments").arg(what()).arg(kMaxArgs));
    if (cands.empty()) return fail(QTC_ERR_NOT_FOUND, QStringLiteral("%1: no such method").arg(what()));
    int64_t best = 0, second = 0;
    const Cand *bc = nullptr, *sc = nullptr;
    int hard = 0;
    for (const Cand &c : cands) {
        int h = 0;
        int64_t s = score(c, st, &h);
        if (h) hard = h;
        if (s > best) { second = best; sc = bc; best = s; bc = &c; }
        else if (s > second) { second = s; sc = &c; }
    }
    if (!bc) {
        if (hard) return fail(hard, QStringLiteral("%1: an argument handle is invalid (%2)").arg(what()).arg(QLatin1StringView(status_name(hard))));
        QStringList sigs;
        for (const Cand &c : cands) sigs << cand_sig(c);
        sigs.removeDuplicates();
        QStringList got;
        for (int i = 0; i < st.n; i++) got << arg_describe(st.a[i]);
        return fail(QTC_ERR_BAD_ARGS, QStringLiteral("%1: no overload accepts (%2); candidates: %3")
                                          .arg(what(), got.join(QStringLiteral(", ")), sigs.join(QStringLiteral(", "))));
    }
    if (sc && second == best && cand_sig(*sc) != cand_sig(*bc))
        return fail(QTC_ERR_AMBIGUOUS, QStringLiteral("%1: ambiguous between %2 and %3; pass a full signature")
                                           .arg(what(), cand_sig(*bc), cand_sig(*sc)));
    r.clear();
    if (bc->md) {
        QString why;
        if (bc->cls && !need_ok(bc->cls, &why)) return fail(QTC_ERR_STATE, why);
        // a nested event loop (exec, waitFor...) inside a virtual call Qt is making would run
        // anything (another paint, a deferred delete of what Qt is using) in the middle of it
        if ((bc->md->flags & MF_BLOCKING) && ov_depth() > 0)
            return fail(QTC_ERR_STATE, QStringLiteral("%1: %2 waits in an event loop, which an override handler may not start")
                                           .arg(what(), QLatin1StringView(bc->md->sig)));
        void *self = nullptr;
        if (!(bc->md->flags & MF_STATIC)) {
            if (self_obj) self = self_obj;
            else if (self_val) {
                self = self_val->data();
                if (bc->base >= 0) {
                    const ClassDef *vc = find_class(QByteArray(self_val->metaType().name()));
                    if (!vc || !vc->upcast) return fail(QTC_ERR_UNSUPPORTED, QStringLiteral("%1: cannot reach the base class").arg(what()));
                    self = vc->upcast(self, bc->base);
                }
            }
        }
        int rc;
        if (bc->call_n > 0) {
            // the arity to call differs from the arguments given: fill in the pointer parameters
            Arg full[kMaxArgs];   // copies, so the frame's own arguments stay as they were
            int j = 0;
            for (int i = 0; i < bc->call_n; i++) {
                bool ptr = pointer_param(bc->md->params[i].kind);
                if (bc->skip ? ptr : i >= st.n) full[i].kind = AK_NULL;
                else full[i] = st.a[bc->skip ? j++ : i];
            }
            rc = bc->md->fn(self, full, bc->call_n, &r);
        } else {
            rc = bc->md->fn(self, st.a, st.n, &r);
        }
        if (rc) {
            if (rc == QTC_ERR_UNSUPPORTED)
                return fail(rc, QStringLiteral("%1: %2 is not available in this Qt build").arg(what(), QLatin1StringView(bc->md->sig)));
            if (t_err != rc) fail(rc, QStringLiteral("%1 failed").arg(what()));
            return rc;
        }
        return QTC_OK;
    }
    if (!self_obj) return fail(QTC_ERR_UNSUPPORTED, what());
    return invoke_meta(static_cast<QObject *>(self_obj), bc->mm, st, r) ? t_err : QTC_OK;
}

// Creates an object with one of these constructors, chosen by the pushed arguments as qtc_new
// chooses (the arguments are consumed). GUI thread, inside guarded().
uint64_t new_with_ctors(const char *cls, const MethodDef *ctors, uint32_t nctors, const ClassDef *c) {
    CallFrame f;
    Ret &r = *f.r;
    std::vector<Cand> cands;
    for (uint32_t i = 0; i < nctors; i++) {
        Cand k;
        k.md = &ctors[i];
        k.cls = c;
        cands.push_back(k);
    }
    const Label what{QLatin1StringView(cls), QLatin1StringView(), "new %1"};
    if (run_candidates(cands, nullptr, nullptr, what, *f.st, r) != QTC_OK) return 0;
    if (r.kind != QTC_T_OBJ || !r.h) { fail(QTC_ERR_REJECTED, what.str()); return 0; }
    qtc_status e = 0;
    QObject *o = reg().object(r.h, &e);
    if (o) {
        reg().set_made(r.h);
        QList<QObject *> made_with;
        for (int i = 0; i < f.st->n; i++)
            if (f.st->a[i].kind == AK_OBJ)
                if (QObject *x = reg().object(f.st->a[i].h, &e)) made_with.append(x);
        roles_adopt_new(o, made_with);
        roles_constructed(o);
    }
    if (o && !o->parent()) reg().set_owned(r.h, true);
    return r.h;
}


struct ArgsConsumer {
    ~ArgsConsumer() { args().clear(); }
};

static QString obj_label(QObject *o) {
    return QStringLiteral("%1").arg(QLatin1StringView(o->metaObject()->className()));
}

// Calls that would break qtc's own loop are redirected or refused.
// A window container uses its window without checking that it is still there (when one of
// its parents changes): the roles keep the window with it, so they learn the pair here.
static void note_window_container(const QByteArray &name, const ArgStack &st, const Ret &r, qtc_status rc) {
    if (rc != QTC_OK || name != "createWindowContainer" || r.kind != QTC_T_OBJ || st.n < 1) return;
    int e = 0;
    QObject *window = cv_object(st.a[0], nullptr, 0, &e);
    qtc_status he = 0;
    QObject *container = reg().object(r.h, &he);
    if (!e && window && container) roles_note_container(container, window);
}

static bool intercept_call(QObject *o, const QByteArray &name, const ArgStack &st, qtc_status *rc) {
    if (o != QCoreApplication::instance()) return false;
    if (name == "quit") { *rc = qtc_quit(0); return true; }
    if (name == "exit") {
        int64_t code = 0;
        if (st.n >= 1) {
            if (int e = cv_int64(st.a[0], code, 0, INT32_MIN, INT32_MAX)) { *rc = e; return true; }
        }
        *rc = qtc_quit(int32_t(code));
        return true;
    }
    if (name == "exec") { *rc = fail(QTC_ERR_STATE, "use qtc_run or qtc_wait instead of QCoreApplication::exec"); return true; }
    return false;
}

static const QSet<QByteArray> &denied_methods() {
    static const QSet<QByteArray> s = {"moveToThread", "deleteLater", "setEventDispatcher", "installNativeEventFilter",
                                       "removeNativeEventFilter", "qt_metacall", "qt_metacast", "destroy"};
    return s;
}

static bool would_cycle(QObject *self, QObject *newparent) {
    for (QObject *p = newparent; p; p = p->parent())
        if (p == self) return true;
    return false;
}

static qtc_status do_call_in(uint64_t receiver, qtc_str method, ArgStack &st, Ret &r) {
    QByteArray name, sig;
    if (!split_method(method, name, sig)) return t_err;
    int kind = reg().kind_of(receiver);
    std::vector<Cand> cands;
    if (kind == SK_VAL) {
        qtc_status err = 0;
        QVariant *v = reg().value(receiver, &err);
        if (!v) return fail(err, "receiver");
        const QByteArray tn(v->metaType().name() ? v->metaType().name() : "");
        const ClassDef *c = find_class(tn);
        const Label what{QLatin1StringView(tn), QLatin1StringView(name), "%1::%2"};
        if (!c) return fail(QTC_ERR_NOT_FOUND, QStringLiteral("%1: values of type %2 have no methods").arg(what.str(), QLatin1StringView(tn)));
        collect_value_cached(c, name, sig, cands);
        return run_candidates(cands, nullptr, v, what, st, r);
    }
    qtc_status err = 0;
    QObject *o = reg().object(receiver, &err);
    if (!o) {
        if (err == QTC_ERR_WRONG_TYPE) return fail(err, "receiver is not an object or value");
        return fail(err, err == QTC_ERR_STALE_HANDLE ? "the receiver was destroyed" : "receiver");
    }
    const Label what{QLatin1StringView(o->metaObject()->className()), QLatin1StringView(name), "%1::%2"};
    // the call may give an owner something new to hold: it is looked at again afterwards
    struct NoteAfter {
        QPointer<QObject> o;
        ~NoteAfter() { if (o) roles_note_call(o); }
    } note_after{o};
    if (denied_methods().contains(name))
        return fail(QTC_ERR_UNSUPPORTED, QStringLiteral("%1 is not available through qtc (use qtc_destroy for deleteLater)").arg(what.str()));
    qtc_status irc = 0;
    if (intercept_call(o, name, st, &irc)) return irc;
    if (name == "setParent" && st.n >= 1) {
        int e = 0;
        QObject *np = cv_object(st.a[0], nullptr, 0, &e);
        if (e) return e;
        if (np && would_cycle(o, np)) return fail(QTC_ERR_REJECTED, QStringLiteral("%1: the new parent is the object or one of its children").arg(what.str()));
        if (QObject *holder = roles_holder(o); holder && holder != np)
            return fail(QTC_ERR_STATE, QStringLiteral("%1: the object is held by %2; take it from there first").arg(what.str(), obj_label(holder)));
        if (QObject *b = bound_parent(o); b && b != np)
            return fail(QTC_ERR_STATE, QStringLiteral("%1: the object stays inside the %2 it was made for").arg(what.str(), obj_label(b)));
#ifdef QTC_HAS_WIDGETS
        // Qt clears the focus of a widget it moves only when the widget stays a child: moved
        // into a window of its own (window flags), the old parents keep pointing at it as their
        // focus child, and read it once it is gone. The focus is cleared first here.
        if (o->isWidgetType()) {
            auto *w = static_cast<QWidget *>(o);
            if (QWidget *fw = w->focusWidget(); fw && np != w->parentWidget() && (fw == w || w->isAncestorOf(fw)))
                fw->clearFocus();
        }
#endif
    }
    collect_object_cached(o, name, sig, false, cands);
    qtc_status rc = run_candidates(cands, o, nullptr, what, st, r);
    if (rc == QTC_OK && name == "blockSignals" && o->signalsBlocked()) reg().note_blocked(o);
    note_window_container(name, st, r, rc);
    return rc;
}

qtc_status do_call(uint64_t receiver, qtc_str method) {
    CallFrame f;
    return do_call_in(receiver, method, *f.st, *f.r);
}

}  // namespace qtc

using namespace qtc;

// ================================================================== C API

extern "C" {

QTC_API void qtc_set_error_policy(int32_t policy) {
    if (policy < QTC_ERRORS_RETURN || policy > QTC_ERRORS_ABORT) policy = QTC_ERRORS_RETURN;
    st().policy.store(policy);
}
QTC_API int32_t qtc_error_policy(void) { return st().policy.load(); }
QTC_API qtc_status qtc_last_error(void) { return t_err; }
QTC_API qtc_view qtc_last_error_message(void) { return qtc_view{t_msg.constData(), int64_t(t_msg.size())}; }
QTC_API void qtc_clear_error(void) { t_err = 0; t_msg.clear(); }
QTC_API qtc_view qtc_status_name(qtc_status s) { return view_static(status_name(s)); }
QTC_API qtc_view qtc_version(void) { return view_static(QTC_VERSION_STRING); }
QTC_API qtc_view qtc_qt_version(void) { return view_static(qVersion()); }
QTC_API qtc_view qtc_qt_build_version(void) { return view_static(QT_VERSION_STR); }
QTC_API bool qtc_is_initialized(void) { return st().inited.load(); }

static void message_handler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg);
static QtMessageHandler g_prev_handler = nullptr;

static qtc_status init_common(int32_t kind) {
    State &s = st();
    if (s.inited) return fail(QTC_ERR_STATE, "qtc_init was already called");
    if (QCoreApplication::instance()) return fail(QTC_ERR_STATE, "a Q*Application already exists in this process");
    if (kind < QTC_APP_WIDGETS || kind > QTC_APP_CORE) return fail(QTC_ERR_INVALID_ARG, "unknown application kind");
#ifndef QTC_HAS_WIDGETS
    if (kind == QTC_APP_WIDGETS) kind = QTC_APP_GUI;
#endif
#ifndef QTC_HAS_GUI
    kind = QTC_APP_CORE;
#endif
#if defined(Q_OS_LINUX)
    if (kind != QTC_APP_CORE) {
        bool has_platform_arg = false;
        for (auto &a : s.argv_store) if (a == "-platform" || a.rfind("-platform=", 0) == 0) has_platform_arg = true;
        if (!has_platform_arg && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && qEnvironmentVariableIsEmpty("DISPLAY") &&
            qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
            return fail(QTC_ERR_STATE, "no display: set DISPLAY or WAYLAND_DISPLAY, or QT_QPA_PLATFORM=offscreen for headless use");
    }
#endif
    s.argv_ptrs.clear();
    for (auto &a : s.argv_store) s.argv_ptrs.push_back(a.data());
    s.argv_ptrs.push_back(nullptr);
    s.argc = int(s.argv_store.size());
    g_prev_handler = qInstallMessageHandler(message_handler);
    try {
        switch (kind) {
#ifdef QTC_HAS_WIDGETS
        case QTC_APP_WIDGETS: s.app = new QApplication(s.argc, s.argv_ptrs.data()); break;
#endif
#ifdef QTC_HAS_GUI
        case QTC_APP_GUI: s.app = new QGuiApplication(s.argc, s.argv_ptrs.data()); break;
#endif
        default: s.app = new QCoreApplication(s.argc, s.argv_ptrs.data()); break;
        }
    } catch (...) {
        qInstallMessageHandler(g_prev_handler);
        return fail(QTC_ERR_EXCEPTION, "creating the application failed");
    }
    s.kind = kind;
    s.gui_thread = QThread::currentThreadId();
    qRegisterMetaType<ObjRef>("qtc::ObjRef");
    if (!s.classes_registered) {
        register_builtin_classes();
        qtc_gen_register_all();
        s.classes_registered = true;
    }
    s.inited.store(true, std::memory_order_release);
    events_init();
    roles_init();
    concurrent_init();
    return QTC_OK;
}

QTC_API qtc_status qtc_init(int32_t app_kind, qtc_str argstr) {
    QByteArray all;
    if (argstr.len > 0 && !argstr.data) return fail(QTC_ERR_INVALID_ARG, "args: NULL data with a length");
    if (argstr.data) all = argstr.len < 0 ? QByteArray(argstr.data) : QByteArray(argstr.data, argstr.len);
    State &s = st();
    if (s.inited) return fail(QTC_ERR_STATE, "qtc_init was already called");
    s.argv_store.clear();
    for (const QByteArray &p : all.split('\0'))
        if (!p.isEmpty()) s.argv_store.push_back(p.toStdString());
    if (s.argv_store.empty()) s.argv_store.push_back("qtc");
    return init_common(app_kind);
}

QTC_API qtc_status qtc_init_argv(int32_t app_kind, int32_t argc, const char *const *argv) {
    State &s = st();
    if (s.inited) return fail(QTC_ERR_STATE, "qtc_init was already called");
    if (argc < 0 || (argc > 0 && !argv)) return fail(QTC_ERR_INVALID_ARG, "argc/argv");
    s.argv_store.clear();
    for (int i = 0; i < argc; i++)
        if (argv[i]) s.argv_store.push_back(argv[i]);
    if (s.argv_store.empty()) s.argv_store.push_back("qtc");
    return init_common(app_kind);
}

QTC_API qtc_status qtc_shutdown(void) {
    State &s = st();
    if (!s.inited) return fail(QTC_ERR_NOT_INITIALIZED, "qtc_init has not been called");
    if (!gui_thread_ok()) return fail(QTC_ERR_WRONG_THREAD, "qtc_shutdown must be called from the GUI thread");
    if (ov_depth() > 0) return fail(QTC_ERR_STATE, "qtc_shutdown cannot run inside an override handler (Qt is in the middle of a call)");
    try {
        reg().sweep_dead();   // objects deleted with their signals blocked, before anything walks them
        // futures end and worker threads stop first: what runs there may still emit and post
        concurrent_shutdown();
        events_shutdown();
        // GPU resources go while their windows and widgets (and the QRhi of a widget's window) exist
        rhi_shutdown();
        // Newest first, with the checks qtc_destroy makes: owners that hold an object let go
        // (one that owns it, like a QDrag its mime data, deletes it), and an object something
        // still uses (an engine a context was made for) waits for a later round.
        std::vector<QPointer<QObject>> roots = reg().owned_roots();
        for (int round = 0; round < 4 && !roots.empty(); round++) {
            std::vector<QPointer<QObject>> later;
            for (QPointer<QObject> &p : roots) {
                if (!p || p->parent()) continue;
                // an object of a worker thread that did not stop (concurrent_shutdown asked them
                // all to) cannot be deleted from here safely: the process ends it
                if (QThread *t = p->thread(); t && t != QThread::currentThread() && !t->wait(QDeadlineTimer(0))) continue;
                // a thread that still runs would abort the program when deleted
                if (auto *t = qobject_cast<QThread *>(p.data()); t && t->isRunning() && t != QThread::currentThread()) {
                    t->quit();
                    if (!t->wait(QDeadlineTimer(2000))) continue;   // left running: the process ends it
                }
                if (!roles_prepare_destroy(p.data()).isEmpty()) {
                    later.push_back(p);
                    continue;
                }
                if (p) delete p.data();
            }
            if (later.size() == roots.size()) break;   // nothing moved: what is left stays
            roots.swap(later);
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        args().clear();
        ret().clear_all();
        roles_clear();
        clear_cand_cache();
        delete s.app;
        s.app = nullptr;
        classes_shutdown();
        reg().clear_all();
    } catch (...) {
    }
    qInstallMessageHandler(g_prev_handler);
    s.inited.store(false);
    s.kind = -1;
    return QTC_OK;
}

QTC_API qtc_obj qtc_app(void) {
    return guarded<qtc_obj>(0, [] { return reg().handle_for(QCoreApplication::instance()); });
}

// ---------------------------------------------------------------- objects

QTC_API qtc_obj qtc_new(qtc_str class_name) {
    return guarded<qtc_obj>(0, [&]() -> qtc_obj {
        CallFrame f;
        Ret &r = *f.r;
        QByteArray cn;
        if (!read_bytes(class_name, cn, "class_name")) return 0;
        bool runtime = false;
        // a runtime class (qtc_class_*): made like qtc_class_new makes it (the frame gives the
        // arguments back first)
        if (dyn_class_named(cn.trimmed())) {
            ArgStack &g = args();
            g.clear();
            for (int i = 0; i < f.st->n; i++) g.a[i] = f.st->a[i];
            g.n = f.st->n;
            return dyn_new(cn.trimmed(), &runtime);
        }
        const ClassDef *c = find_class(cn.trimmed());
        if (!c) { fail(QTC_ERR_NOT_FOUND, QStringLiteral("no class named %1").arg(QString::fromUtf8(cn))); return 0; }
        if (c->kind != CK_OBJECT) { fail(QTC_ERR_WRONG_TYPE, QStringLiteral("%1 is a value type: use qtc_val_new").arg(QLatin1StringView(c->name))); return 0; }
        if (c->abstract || c->nctors == 0) { fail(QTC_ERR_UNSUPPORTED, QStringLiteral("%1 cannot be created").arg(QLatin1StringView(c->name))); return 0; }
        std::vector<Cand> cands;
        for (uint32_t i = 0; i < c->nctors; i++) {
            Cand k;
            k.md = &c->ctors[i];
            k.cls = c;
            cands.push_back(k);
        }
        const Label what{QLatin1StringView(c->name), QLatin1StringView(), "new %1"};
        if (run_candidates(cands, nullptr, nullptr, what, *f.st, r) != QTC_OK) return 0;
        if (r.kind != QTC_T_OBJ || !r.h) { fail(QTC_ERR_REJECTED, what.str()); return 0; }
        qtc_status e = 0;
        QObject *o = reg().object(r.h, &e);
        if (o) {
            reg().set_made(r.h);
            QList<QObject *> made_with;
            for (int i = 0; i < f.st->n; i++)
                if (f.st->a[i].kind == AK_OBJ)
                    if (QObject *x = reg().object(f.st->a[i].h, &e)) made_with.append(x);
            roles_adopt_new(o, made_with);   // e.g. a parentless QQmlComponent becomes its engine's child
            roles_constructed(o);
        }
        if (o && !o->parent()) reg().set_owned(r.h, true);
        return r.h;
    });
}

QTC_API qtc_status qtc_destroy(qtc_obj obj) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        QObject *o = reg().object(obj, &err);
        if (!o && err == QTC_ERR_WRONG_THREAD) {
            // moved to a worker thread (qtc_move_to_thread): nothing in the GUI thread holds it
            // (moving refused objects held by others), and its own thread deletes it
            if (qtc_status e = reg().destroy_far(obj)) return fail(e, "qtc_destroy");
            return QTC_OK;
        }
        if (!o) return fail(err, "qtc_destroy");
        if (o == QCoreApplication::instance()) return fail(QTC_ERR_STATE, "the application object is destroyed by qtc_shutdown");
        // owners holding it by a plain pointer let go of it first (see qtc_roles.cpp)
        QString why = roles_prepare_destroy(o);
        if (!why.isEmpty())
            return fail(QTC_ERR_STATE, QStringLiteral("qtc_destroy: %1 is %2; destroy its owner instead").arg(obj_label(o), why));
        if (!reg().alive(obj)) return QTC_OK;   // releasing it deleted it
#ifdef QTC_HAS_WIDGETS
        if (o->isWidgetType()) static_cast<QWidget *>(o)->hide();
#endif
        QObject *x = nullptr;
        if (qtc_status e = reg().mark_dying(obj, &x)) return fail(e, "qtc_destroy");
        if (!roles_defer_delete(x)) x->deleteLater();
        return QTC_OK;
    });
}

QTC_API qtc_status qtc_adopt(qtc_obj obj) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        QObject *o = reg().object(obj, &err);
        if (!o) return fail(err, "qtc_adopt");
        if (o == QCoreApplication::instance()) return fail(QTC_ERR_STATE, "the application object cannot be adopted");
        reg().set_owned(obj, true);
        return QTC_OK;
    });
}

QTC_API bool qtc_alive(uint64_t h) { return st().inited && reg().alive(h); }
QTC_API bool qtc_is_object(uint64_t h) { return st().inited && reg().alive(h) && reg().kind_of(h) == SK_OBJ; }
QTC_API bool qtc_is_value(uint64_t h) { return st().inited && reg().alive(h) && reg().kind_of(h) == SK_VAL; }

QTC_API qtc_view qtc_class_name(qtc_obj obj) {
    return guarded<qtc_view>(qtc_view{"", 0}, [&]() -> qtc_view {
        qtc_status err = 0;
        QObject *o = reg().object(obj, &err);
        if (!o) { fail(err, "qtc_class_name"); return qtc_view{"", 0}; }
        return view_of(QByteArray(o->metaObject()->className()));
    });
}

QTC_API bool qtc_inherits(qtc_obj obj, qtc_str class_name) {
    return guarded<bool>(false, [&]() -> bool {
        qtc_status err = 0;
        QObject *o = reg().object(obj, &err);
        if (!o) { fail(err, "qtc_inherits"); return false; }
        QByteArray cn;
        if (!read_bytes(class_name, cn, "class_name")) return false;
        return o->inherits(cn.constData());
    });
}

QTC_API qtc_obj qtc_find_child(qtc_obj parent, qtc_str object_name, qtc_str class_name, bool recursive) {
    return guarded<qtc_obj>(0, [&]() -> qtc_obj {
        qtc_status err = 0;
        QObject *o = reg().object(parent, &err);
        if (!o) { fail(err, "qtc_find_child"); return 0; }
        QString on;
        QByteArray cn;
        if (!read_str(object_name, on, "object_name") || !read_bytes(class_name, cn, "class_name")) return 0;
        const QList<QObject *> kids = o->findChildren<QObject *>(on, recursive ? Qt::FindChildrenRecursively : Qt::FindDirectChildrenOnly);
        for (QObject *k : kids)
            if (cn.isEmpty() || k->inherits(cn.constData())) return reg().handle_for(k);
        fail(QTC_ERR_NOT_FOUND, QStringLiteral("no child named '%1'").arg(on));
        return 0;
    });
}

QTC_API int64_t qtc_live_objects(void) { std::lock_guard<std::recursive_mutex> lk(reg().mu); return reg().live_objects; }
QTC_API int64_t qtc_live_values(void) { std::lock_guard<std::recursive_mutex> lk(reg().mu); return reg().live_values; }

QTC_API void *qtc_unsafe_pointer(qtc_obj obj) {
    return guarded<void *>(nullptr, [&]() -> void * {
        qtc_status err = 0;
        QObject *o = reg().object(obj, &err);
        if (!o) fail(err, "qtc_unsafe_pointer");
        return o;
    });
}
QTC_API qtc_obj qtc_wrap_pointer(void *p) {
    return guarded<qtc_obj>(0, [&]() -> qtc_obj {
        if (!p) { fail(QTC_ERR_NULL_HANDLE, "qtc_wrap_pointer(NULL)"); return 0; }
        return reg().handle_for(static_cast<QObject *>(p));
    });
}

// ---------------------------------------------------------------- arguments

static Arg *push_arg() {
    ArgStack &a = args();
    if (a.n >= kMaxArgs) { a.overflow = true; return nullptr; }
    Arg *x = &a.a[a.n++];
    x->reset();
    return x;
}

QTC_API void qtc_args_clear(void) { if (st().inited && gui_thread_ok()) args().clear(); }
QTC_API int32_t qtc_args_count(void) { return (st().inited && gui_thread_ok()) ? args().n : 0; }

#define QTC_PUSH(body) \
    if (!st().inited || !gui_thread_ok()) return; \
    Arg *x = push_arg(); \
    if (!x) return; \
    body

QTC_API void qtc_arg_bool(bool v) { QTC_PUSH(x->kind = AK_BOOL; x->b = v;) }
QTC_API void qtc_arg_int(int64_t v) { QTC_PUSH(x->kind = AK_INT; x->i = v;) }
QTC_API void qtc_arg_uint(uint64_t v) { QTC_PUSH(x->kind = AK_UINT; x->u = v;) }
QTC_API void qtc_arg_double(double v) { QTC_PUSH(x->kind = AK_DOUBLE; x->d = v;) }
QTC_API void qtc_arg_null(void) { QTC_PUSH(x->kind = AK_NULL;) }
QTC_API void qtc_arg_obj(qtc_obj v) { QTC_PUSH(x->kind = AK_OBJ; x->h = v;) }
QTC_API void qtc_arg_str(qtc_str v) {
    QTC_PUSH(if (!read_str(v, x->s, "argument")) { x->kind = AK_NONE; args().overflow = true; return; } x->kind = AK_STR;)
}
QTC_API void qtc_arg_bytes(qtc_str v) {
    QTC_PUSH(if (!read_bytes(v, x->bytes, "argument")) { x->kind = AK_NONE; args().overflow = true; return; } x->kind = AK_BYTES;)
}
QTC_API void qtc_arg_val(qtc_val v) {
    QTC_PUSH(
        qtc_status err = 0;
        QVariant *p = reg().value(v, &err);
        if (!p) {
            // an object handle passed as a value is accepted as that object
            if (err == QTC_ERR_WRONG_TYPE) { x->kind = AK_OBJ; x->h = v; return; }
            x->kind = AK_NONE;
            fail(err, "qtc_arg_val");
            args().overflow = true;
            return;
        }
        x->kind = AK_VAL;
        x->v = *p;)
}

// ------------------------------------------------------------------ calls

QTC_API qtc_status qtc_call(uint64_t receiver, qtc_str method) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&] { return do_call(receiver, method); });
}

QTC_API qtc_status qtc_call_static(qtc_str class_name, qtc_str method) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        CallFrame f;
        roles_epoch++;   // a static call may change anything (the application style, ...)
        QByteArray cn, name, sig;
        if (!read_bytes(class_name, cn, "class_name")) return t_err;
        if (!split_method(method, name, sig)) return t_err;
        const ClassDef *c = find_class(cn.trimmed());
        if (!c) return fail(QTC_ERR_NOT_FOUND, QStringLiteral("no class named %1").arg(QString::fromUtf8(cn)));
        std::vector<Cand> cands;
        if (c->kind == CK_OBJECT) {
            int depth = 0;
            for (const QMetaObject *mo = c->meta; mo; mo = mo->superClass(), depth++)
                if (const ClassDef *cc = find_class(QByteArray(mo->className())))
                    for (const MethodDef *md : methods_named(cc, name))
                        if ((md->flags & MF_STATIC) && sig_matches(md, sig)) {
                            Cand k;
                            k.md = md;
                            k.cls = cc;
                            k.depth = depth;
                            cands.push_back(k);
                        }
        } else {
            for (const MethodDef *md : methods_named(c, name))
                if ((md->flags & MF_STATIC) && sig_matches(md, sig)) {
                    Cand k;
                    k.md = md;
                    k.cls = c;
                    cands.push_back(k);
                }
        }
        const Label what{QLatin1StringView(c->name), QLatin1StringView(name), "%1::%2"};
        if (c->name == QByteArrayView("QCoreApplication") || c->name == QByteArrayView("QGuiApplication") ||
            c->name == QByteArrayView("QApplication")) {
            if (name == "exec") return fail(QTC_ERR_STATE, "use qtc_run or qtc_wait instead of exec");
            if (name == "quit") return qtc_quit(0);
            if (name == "exit") {
                int64_t code = 0;
                if (f.st->n >= 1)
                    if (int e = cv_int64(f.st->a[0], code, 0, INT32_MIN, INT32_MAX)) return e;
                return qtc_quit(int32_t(code));
            }
        }
        const qtc_status rc = run_candidates(cands, nullptr, nullptr, what, *f.st, *f.r);
        note_window_container(name, *f.st, *f.r, rc);
        return rc;
    });
}

QTC_API int32_t qtc_ret_kind(void) { return (st().inited && gui_thread_ok()) ? ret().kind : QTC_T_NONE; }
QTC_API bool qtc_ret_bool(void) {
    if (!st().inited || !gui_thread_ok()) return false;
    Ret &r = ret();
    if (r.kind == QTC_T_VALUE || r.kind == QTC_T_LIST || r.kind == QTC_T_MAP) return r.v.toBool();
    if (r.kind == QTC_T_STR) return !r.s.isEmpty();
    return r.b;
}
QTC_API int64_t qtc_ret_int(void) {
    if (!st().inited || !gui_thread_ok()) return 0;
    Ret &r = ret();
    if (r.kind == QTC_T_VALUE) return r.v.toLongLong();
    if (r.kind == QTC_T_STR || r.kind == QTC_T_BYTES) return r.s.toLongLong();
    if (r.kind == QTC_T_OBJ) return int64_t(r.h);
    return r.i;
}
QTC_API uint64_t qtc_ret_uint(void) {
    if (!st().inited || !gui_thread_ok()) return 0;
    Ret &r = ret();
    if (r.kind == QTC_T_VALUE) return r.v.toULongLong();
    if (r.kind == QTC_T_OBJ) return r.h;
    return r.u;
}
QTC_API double qtc_ret_double(void) {
    if (!st().inited || !gui_thread_ok()) return 0;
    Ret &r = ret();
    if (r.kind == QTC_T_VALUE) return r.v.toDouble();
    if (r.kind == QTC_T_STR || r.kind == QTC_T_BYTES) return r.s.toDouble();
    return r.d;
}
QTC_API qtc_view qtc_ret_str(void) {
    if (!st().inited || !gui_thread_ok()) return qtc_view{"", 0};
    Ret &r = ret();
    switch (r.kind) {
    case QTC_T_STR: case QTC_T_BYTES: return view_of(r.s);
    case QTC_T_BOOL: return view_of(r.b ? QByteArray("true") : QByteArray("false"));
    case QTC_T_INT: return view_of(QByteArray::number(qlonglong(r.i)));
    case QTC_T_UINT: return view_of(QByteArray::number(qulonglong(r.u)));
    case QTC_T_DOUBLE: return view_of(QByteArray::number(r.d, 'g', 17));
    case QTC_T_VALUE: case QTC_T_LIST: case QTC_T_MAP:
        if (r.v.canConvert<QString>()) return view_of(r.v.toString().toUtf8());
        return view_of(QByteArray());
    default: return view_of(QByteArray());
    }
}
QTC_API qtc_obj qtc_ret_obj(void) {
    if (!st().inited || !gui_thread_ok()) return 0;
    Ret &r = ret();
    return r.kind == QTC_T_OBJ ? r.h : 0;
}
// ------------------------------------------------------------------ out-parameters

static const Ret::OutVal *out_at(int32_t index) {
    if (!st().inited || !gui_thread_ok()) return nullptr;
    Ret &r = ret();
    if (index < 0 || index >= r.nout) {
        fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("qtc_out: index %1 out of range (the last call filled %2)").arg(index).arg(r.nout));
        return nullptr;
    }
    return &r.outs[index];
}
QTC_API int32_t qtc_out_count(void) { return (st().inited && gui_thread_ok()) ? ret().nout : 0; }
QTC_API bool qtc_out_bool(int32_t index) { const Ret::OutVal *o = out_at(index); return o ? o->b : false; }
QTC_API int64_t qtc_out_int(int32_t index) { const Ret::OutVal *o = out_at(index); return o ? o->i : 0; }
QTC_API double qtc_out_double(int32_t index) { const Ret::OutVal *o = out_at(index); return o ? o->d : 0.0; }

// ------------------------------------------------------------------ preconditions

static constexpr int64_t kDefaultAllocLimit = int64_t(256) << 20;
static std::atomic<int64_t> g_alloc_limit{kDefaultAllocLimit};

QTC_API qtc_status qtc_set_alloc_limit(int64_t bytes) {
    if (bytes < 0) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_set_alloc_limit: the limit cannot be negative");
    g_alloc_limit.store(bytes == 0 ? kDefaultAllocLimit : bytes);
    return QTC_OK;
}
QTC_API int64_t qtc_alloc_limit(void) { return g_alloc_limit.load(); }

}  // extern "C"

namespace qtc {
int64_t alloc_limit() { return g_alloc_limit.load(std::memory_order_relaxed); }

int pre_fail(int code, const char *what) {
    fail(code, QString::fromUtf8(what));
    return code;
}

bool not_within(const QObject *arg, const QObject *root) {
    if (!arg || !root) return true;
    for (int d = 0; arg && d < 10000; arg = arg->parent(), d++)
        if (arg == root) return false;
    return true;
}
bool not_within(const QVariant &arg, const QObject *root) {
    const QVariant v = desanitize(arg);
    if (!(v.metaType().flags() & QMetaType::PointerToQObject)) return true;
    return not_within(*static_cast<QObject *const *>(v.constData()), root);
}

bool not_self_or_ancestor(const QObject *self, const QObject *arg) {
    if (!arg || !self) return true;
    int depth = 0;
    for (const QObject *p = self; p && depth < 10000; p = p->parent(), depth++)
        if (p == arg) return false;
    return true;
}

static bool model_reaches(const QAbstractItemModel *from, const QAbstractItemModel *target, int depth) {
    if (!from) return false;
    if (from == target) return true;
    if (depth > 64) return true;   // absurdly deep: treat as a cycle
    if (auto *px = qobject_cast<const QAbstractProxyModel *>(from)) return model_reaches(px->sourceModel(), target, depth + 1);
    if (auto *cc = qobject_cast<const QConcatenateTablesProxyModel *>(from)) {
        for (const QAbstractItemModel *m : cc->sourceModels())
            if (model_reaches(m, target, depth + 1)) return true;
    }
    return false;
}
bool model_no_cycle(const QAbstractItemModel *self, const QAbstractItemModel *arg) {
    return !arg || !model_reaches(arg, self, 0);
}

bool index_of(const QAbstractItemModel *m, const QModelIndex &i) { return !i.isValid() || i.model() == m; }
int64_t rows_of(const QAbstractItemModel *m, const QModelIndex &parent) { return m ? int64_t(m->rowCount(parent)) : 0; }
int64_t cols_of(const QAbstractItemModel *m, const QModelIndex &parent) { return m ? int64_t(m->columnCount(parent)) : 0; }

// p, which may dangle, is an object qtc knows to be alive: one with a live handle, or root or
// one of its descendants. Only addresses are compared.
bool known_live(const QObject *p, const QObject *root) {
    if (!p) return true;
    {
        Registry &r = reg();
        std::lock_guard<std::recursive_mutex> lk(r.mu);
        auto it = r.by_ptr.constFind(const_cast<QObject *>(p));
        if (it != r.by_ptr.constEnd() && r.table[*it].qp) return true;
    }
    return root && (p == root || root->findChildren<QObject *>().contains(const_cast<QObject *>(p)));
}

// QGridLayout lays out from a rows x columns array it allocates and clears on every pass
bool grid_cells_ok(const QObject *layout, int64_t rows, int64_t cols) {
#ifdef QTC_HAS_WIDGETS
    auto *g = qobject_cast<const QGridLayout *>(layout);
    if (!g) return true;
    rows = std::max<int64_t>(rows, g->rowCount());
    cols = std::max<int64_t>(cols, g->columnCount());
    return rows <= (int64_t(1) << 20) && cols <= (int64_t(1) << 20) && rows * cols <= (int64_t(1) << 20);
#else
    (void)layout;
    (void)rows;
    (void)cols;
    return true;
#endif
}

// Images. Qt checks an image's size only for overflow: a large one is allocated, then touched
// (filled, painted, converted) and the system kills the process. These keep each image a call
// makes within the allocation limit.
bool buffer_seek_ok(const QObject *device, int64_t pos) {
    auto *b = qobject_cast<const QBuffer *>(device);
    return !b || pos <= b->size() || alloc_ok(pos, 1);
}

bool pixels_ok(double w, double h, double bytes_per_pixel) {
    if (std::isnan(w) || std::isnan(h) || std::isinf(w) || std::isinf(h)) return false;
    if (!(w > 0 && h > 0)) return true;   // Qt makes a null image
    return w * h * std::max(bytes_per_pixel, 1.0) <= double(alloc_limit());
}

#ifdef QTC_HAS_GUI
double image_bpp(int format) {
    if (format <= int(QImage::Format_Invalid) || format >= int(QImage::NImageFormats)) return 4;
    return std::max(1.0, QImage::toPixelFormat(QImage::Format(format)).bitsPerPixel() / 8.0);
}

double app_dpr() { return qGuiApp ? std::max(1.0, qGuiApp->devicePixelRatio()) : 1.0; }

// what scaled() makes of an image of size from (QSize::scaled, in floating point)
bool scaled_ok(const QSize &from, double w, double h, int mode, double bpp) {
    if (mode != Qt::IgnoreAspectRatio && from.width() > 0 && from.height() > 0 && w > 0 && h > 0) {
        const double rw = h * from.width() / from.height();
        const bool use_height = mode == Qt::KeepAspectRatio ? rw <= w : rw >= w;
        if (use_height) w = rw;
        else h = w * from.height() / from.width();
    }
    return pixels_ok(std::max(w, 1.0), std::max(h, 1.0), bpp);
}

// what transformed() makes: the bounding rectangle of the transformed image
bool transform_ok(const QSize &from, const QTransform &t, double bpp) {
    const QRectF r = t.mapRect(QRectF(0, 0, from.width(), from.height()));
    return pixels_ok(r.width(), r.height(), bpp);
}

// a window gets a backing store (or render target) of its size in device pixels when it shows
bool qwindow_size_ok(const QWindow *w, double width, double height) {
    const double dpr = std::max(1.0, w->devicePixelRatio());
    return pixels_ok(width * dpr, height * dpr, 4);
}

bool qwindow_show_ok(const QWindow *w) {
    return qwindow_size_ok(w, std::max(w->width(), w->minimumWidth()), std::max(w->height(), w->minimumHeight()));
}
#endif

#ifdef QTC_HAS_WIDGETS
bool grab_ok(const QWidget *w, const QRect &r) {
    double gw = r.width(), gh = r.height();
    if (r.width() < 0 || r.height() < 0) {
        // Qt lays the widget out first (an unshown window takes its size hint) and then grabs
        // from the given top left to the widget's bottom right, in both directions: estimate
        // the widget's size from above
        const QWidget *top = w->window();
        const double ew = std::max({w->width(), w->minimumWidth(), w->sizeHint().width(), top->width(),
                                    top->minimumWidth(), top->sizeHint().width()});
        const double eh = std::max({w->height(), w->minimumHeight(), w->sizeHint().height(), top->height(),
                                    top->minimumHeight(), top->sizeHint().height()});
        gw = ew - r.x();
        gh = eh - r.y();
        if (gw <= 0 || gh <= 0) return true;   // nothing to grab
    }
    const double dpr = std::max(1.0, w->devicePixelRatioF());
    return pixels_ok(gw * dpr, gh * dpr, 4);
}

bool window_size_ok(const QWidget *w, double width, double height) {
    if (!w->isWindow()) return true;   // a child paints into its window's backing store
    const double dpr = std::max(1.0, w->devicePixelRatioF());
    return pixels_ok(width * dpr, height * dpr, 4);
}

// a window shows at least at its minimum size, and its layout's
bool window_show_ok(const QWidget *w) {
    if (!w->isWindow()) return true;
    const QSize hint = w->minimumSizeHint();
    return window_size_ok(w, std::max({w->width(), w->minimumWidth(), hint.width()}),
                          std::max({w->height(), w->minimumHeight(), hint.height()}));
}

// Qt's layout engine (qGeomCalc) adds a layout's sizes, spacing and stretch factors up in int;
// past that the sums wrap, and its space distribution can then loop forever. Here they are added
// up in 64 bits and kept under 2^30: every item's size in both directions (a nested layout by
// what it holds), the spacing after each item, the margins and the stretch factors.
namespace {
constexpr int64_t kLayoutSumMax = int64_t(1) << 30;
struct LayoutSums { int64_t w = 0, h = 0, stretch = 0; };

int64_t count_items(const QLayout *l, int depth) {
    if (depth > 64) return INT64_MAX;
    int64_t n = 0;
    if (auto *g = qobject_cast<const QGridLayout *>(l)) n += int64_t(g->rowCount()) + g->columnCount();
    for (int i = 0, c = l->count(); i < c; i++) {
        QLayoutItem *it = l->itemAt(i);
        if (!it) continue;
        n++;
        if (QLayout *sub = it->layout()) n += count_items(sub, depth + 1);
        if (n > 64) break;
    }
    return n;
}

bool add_layout(const QLayout *l, LayoutSums &s, int depth) {
    if (depth > 64) return false;
    const QMargins m = l->contentsMargins();
    s.w += std::abs(int64_t(m.left())) + std::abs(int64_t(m.right()));
    s.h += std::abs(int64_t(m.top())) + std::abs(int64_t(m.bottom()));
    int64_t spacing = std::max(0, l->spacing());
    if (auto *g = qobject_cast<const QGridLayout *>(l)) {
        spacing = std::max({int64_t(0), int64_t(g->horizontalSpacing()), int64_t(g->verticalSpacing())});
        // rows and columns count with their minimum sizes even when no item is in them
        for (int r = 0, rows = g->rowCount(); r < rows; r++) {
            s.stretch += std::max(0, g->rowStretch(r));
            s.h += std::abs(int64_t(g->rowMinimumHeight(r))) + spacing;
        }
        for (int c = 0, cols = g->columnCount(); c < cols; c++) {
            s.stretch += std::max(0, g->columnStretch(c));
            s.w += std::abs(int64_t(g->columnMinimumWidth(c))) + spacing;
        }
    } else if (auto *f = qobject_cast<const QFormLayout *>(l)) {
        spacing = std::max({int64_t(0), int64_t(f->horizontalSpacing()), int64_t(f->verticalSpacing())});
    }
    auto *box = qobject_cast<const QBoxLayout *>(l);
    for (int i = 0, c = l->count(); i < c; i++) {
        QLayoutItem *it = l->itemAt(i);
        if (!it) continue;
        if (box) s.stretch += std::max(0, box->stretch(i));
        s.w += spacing;
        s.h += spacing;
        if (QLayout *sub = it->layout()) {
            if (!add_layout(sub, s, depth + 1)) return false;
        } else {
            const QSize hint = it->sizeHint(), min = it->minimumSize();
            s.w += std::max(std::abs(int64_t(hint.width())), std::abs(int64_t(min.width())));
            s.h += std::max(std::abs(int64_t(hint.height())), std::abs(int64_t(min.height())));
        }
        if (s.w > kLayoutSumMax || s.h > kLayoutSumMax || s.stretch > kLayoutSumMax) return false;
    }
    return s.w <= kLayoutSumMax && s.h <= kLayoutSumMax && s.stretch <= kLayoutSumMax;
}
}   // namespace

bool layout_sums_ok(const QLayout *l) {
    if (!l) return true;
    // a few items cannot add up that far: sizes, spacing and margins are each at most
    // QWIDGETSIZE_MAX (Qt bounds widgets, the rules the rest) and stretch factors 65535
    if (count_items(l, 0) <= 8) return true;   // (a grid's rows and columns count as items)
    LayoutSums s;
    return add_layout(l, s, 0);
}

// show() lays out the widget and every widget inside it
bool layouts_ok_below(const QWidget *w) {
    if (!layout_sums_ok(w->layout())) return false;
    const QList<QWidget *> kids = w->findChildren<QWidget *>();
    for (const QWidget *k : kids)
        if (!layout_sums_ok(k->layout())) return false;
    return true;
}

// The attributes Qt keeps itself: the widget's state (Qt::WA_WState_*, and the others Qt's
// documentation leaves out as internal) and those it documents as set or cleared by Qt alone.
// Set from outside they contradict what Qt relies on: a window marked visible without a native
// window crashes QApplication::closeAllWindows, one marked created crashes setWindowTitle.
bool widget_attribute_settable(Qt::WidgetAttribute a) {
    switch (a) {
    case Qt::WA_WState_Visible: case Qt::WA_WState_Hidden: case Qt::WA_WState_Created:
    case Qt::WA_WState_CompressKeys: case Qt::WA_WState_InPaintEvent: case Qt::WA_WState_Reparented:
    case Qt::WA_WState_ConfigPending: case Qt::WA_WState_Polished: case Qt::WA_WState_OwnSizePolicy:
    case Qt::WA_WState_ExplicitShowHide: case Qt::WA_WState_WindowOpacitySet:
    case Qt::WA_WState_AcceptedTouchBeginEvent:
    case Qt::WA_LaidOut: case Qt::WA_PendingUpdate: case Qt::WA_InvalidSize: case Qt::WA_GrabbedShortcut:
    case Qt::WA_ForceUpdatesDisabled: case Qt::WA_UpdatesDisabled: case Qt::WA_Mapped: case Qt::WA_UnderMouse:
    case Qt::WA_OutsideWSRange: case Qt::WA_PendingMoveEvent: case Qt::WA_PendingResizeEvent:
    case Qt::WA_DropSiteRegistered: case Qt::WA_StyleSheet: case Qt::WA_SetWindowModality:
    case Qt::WA_AttributeCount:
        return false;
    default:
        return true;
    }
}
#endif

// adding an item at the next free place adds a row or a column
bool grid_grow_ok(const QObject *layout) {
#ifdef QTC_HAS_WIDGETS
    auto *g = qobject_cast<const QGridLayout *>(layout);
    return !g || (grid_cells_ok(g, int64_t(g->rowCount()) + 1, 0) && grid_cells_ok(g, 0, int64_t(g->columnCount()) + 1));
#else
    (void)layout;
    return true;
#endif
}

// The insertions some of Qt's models make slowly. QStringListModel inserts rows one at a time
// at the same place, moving every row after it each time: count x (rows after it + count / 2)
// moves anywhere but the front. The models of QListWidget and QTreeWidget insert one row at a
// time too, moving the rows after them each time (count x rows after it). QStandardItemModel
// and QTableWidget's model insert a column row by row into one array (rows x rows x columns),
// QSqlQueryModel columns one at a time (quadratic in the count). The proxies that pass an
// insertion on to their source are followed to the model that makes it.
bool insert_cheap(const QAbstractItemModel *m, Qt::Orientation o, int64_t pos, int64_t count, QModelIndex parent) {
    constexpr double kMoves = double(int64_t(1) << 27);
    bool exact = true;   // pos is where the model inserts (a sorting proxy maps it elsewhere)
    for (int depth = 0; m && count > 0 && depth < 64; depth++) {
#ifdef QTC_HAS_WIDGETS
        // the item widgets' own models are private classes: known by the widget that made them
        const QObject *up = m->parent();
        auto *lw = qobject_cast<const QListWidget *>(up);
        auto *trw = qobject_cast<const QTreeWidget *>(up);
        auto *tw = qobject_cast<const QTableWidget *>(up);
#endif
        if (o == Qt::Vertical) {
            if (auto *sl = qobject_cast<const QStringListModel *>(m)) {
                if (exact && pos == 0) return true;   // Qt's lists grow at the front cheaply
                int64_t after = std::max<int64_t>(0, int64_t(sl->rowCount()) - (exact ? pos : 0));
                return double(count) * double(after + count / 2) <= kMoves;
            }
#ifdef QTC_HAS_WIDGETS
            if ((lw && lw->model() == m) || (trw && trw->model() == m)) {
                int64_t after = std::max<int64_t>(0, rows_of(m, parent) - (exact ? pos : 0));
                return double(count) * double(after) <= kMoves;
            }
            if (tw && tw->model() == m)   // one array of rows x columns item pointers
                return alloc_ok((rows_of(m, parent) + count) * std::max<int64_t>(1, cols_of(m, parent)), 8);
#endif
#ifdef QTC_HAS_GUI
            if (auto *st = qobject_cast<const QStandardItemModel *>(m))
                return alloc_ok((rows_of(st, parent) + count) * std::max<int64_t>(1, cols_of(st, parent)), 8);
#endif
        } else {
#ifdef QTC_HAS_GUI
            if (auto *st = qobject_cast<const QStandardItemModel *>(m))
                return std_columns_ok(rows_of(st, parent), cols_of(st, parent) + count) &&
                       alloc_ok(std::max<int64_t>(1, rows_of(st, parent)) * (cols_of(st, parent) + count), 8);
#endif
#ifdef QTC_HAS_WIDGETS
            if (tw && tw->model() == m)
                return std_columns_ok(rows_of(m, parent), cols_of(m, parent) + count) &&
                       alloc_ok(std::max<int64_t>(1, rows_of(m, parent)) * (cols_of(m, parent) + count), 8);
#endif
#ifdef QTC_HAS_MODULE_Sql
            if (qobject_cast<const QSqlQueryModel *>(m)) return count <= 4096;
#endif
        }
        if (qobject_cast<const QTransposeProxyModel *>(m)) o = o == Qt::Vertical ? Qt::Horizontal : Qt::Vertical;
        else if (qobject_cast<const QSortFilterProxyModel *>(m)) exact = false;
        else if (!qobject_cast<const QIdentityProxyModel *>(m)) return true;
        auto *p = static_cast<const QAbstractProxyModel *>(m);
        parent = p->mapToSource(parent);
        m = p->sourceModel();
    }
    return true;
}
int64_t cstr_len(const Arg &a) { return a.kind == AK_NULL ? 0 : int64_t(a.cache.size()); }
bool file_exists(const QString &path) { return !path.isEmpty() && QFileInfo(path).isFile(); }
bool cbor_ok(const QByteArray &data) {
    QCborParserError err;
    (void)QCborValue::fromCbor(data, &err);
    return err.error == QCborError::NoError;
}
bool query_delims_ok(QChar value, QChar pair) {
    static const char16_t sub_delims[] = u"!$&'()*+,;=";
    auto ok = [](QChar c) {
        for (const char16_t *p = sub_delims; *p; p++)
            if (c.unicode() == *p) return true;
        return false;
    };
    return value != pair && ok(value) && ok(pair);
}
// The size of s after replacing every occurrence (count) of a before_len pattern with
// after_len units must fit in the allocation limit.
static bool replaced_size_ok(int64_t size, int64_t count, int64_t before_len, int64_t after_len, int64_t unit) {
    if (after_len <= before_len || count <= 0) return true;
    long double result = (long double)size + (long double)count * (long double)(after_len - before_len);
    return result * unit <= (long double)alloc_limit();
}
// Counts the occurrences replace() would replace (non-overlapping, like replace), stopping
// once the result is known to be too large.
template <class S, class P, class... Cs>
static int64_t count_for_replace(const S &s, const P &before, int64_t step, int64_t stop_at, Cs... cs) {
    int64_t count = 0;
    for (qsizetype i = s.indexOf(before, 0, cs...); i >= 0; i = s.indexOf(before, i + step, cs...))
        if (++count > stop_at) break;
    return count;
}
bool replace_ok(const QString &s, const QString &before, int64_t after_len, Qt::CaseSensitivity cs) {
    if (after_len <= before.size()) return true;
    if (!search_ok(s.size(), before.size())) return false;
    int64_t room = (alloc_limit() / 2 - s.size()) / (after_len - before.size()) + 1;
    int64_t count = before.isEmpty() ? s.size() + 1 : count_for_replace(s, before, before.size(), room, cs);
    return replaced_size_ok(s.size(), count, before.size(), after_len, 2);
}
bool replace_ok(const QString &s, const QRegularExpression &re, int64_t after_len) {
    if (!re.isValid() || after_len == 0) return true;
    // count matches, stopping as soon as the result would be too large
    int64_t budget = alloc_limit() / 2 - s.size();
    int64_t count = 0;
    QRegularExpressionMatchIterator it = re.globalMatch(s);
    while (it.hasNext()) {
        (void)it.next();
        if (++count * after_len > budget) return false;
    }
    return true;
}
// Each part of a split is a QString or QByteArray (24 bytes) plus its data.
static constexpr int64_t kPartBytes = 32;
bool split_ok(const QString &s, const QString &sep, Qt::CaseSensitivity cs) {
    int64_t max_parts = alloc_limit() / kPartBytes;
    if (sep.isEmpty()) return s.size() + 2 <= max_parts;
    if (!search_ok(s.size(), sep.size())) return false;
    return count_for_replace(s, sep, sep.size(), max_parts, cs) + 1 <= max_parts;
}
bool split_ok(const QString &s, const QRegularExpression &re) {
    if (!re.isValid()) return true;
    int64_t max_parts = alloc_limit() / kPartBytes, count = 0;
    QRegularExpressionMatchIterator it = re.globalMatch(s);
    while (it.hasNext()) {
        (void)it.next();
        if (++count + 1 > max_parts) return false;
    }
    return true;
}
bool split_ok(const QByteArray &s, char sep) { return s.count(sep) + 1 <= alloc_limit() / kPartBytes; }

bool replace_ok(const QByteArray &s, const QByteArray &before, int64_t after_len) {
    if (after_len <= before.size()) return true;
    if (!search_ok(s.size(), before.size())) return false;
    int64_t room = (alloc_limit() - s.size()) / (after_len - before.size()) + 1;
    int64_t count = before.isEmpty() ? s.size() + 1 : count_for_replace(s, before, before.size(), room);
    return replaced_size_ok(s.size(), count, before.size(), after_len, 1);
}
bool gl_current() {
#if QT_CONFIG(opengl)
    return QOpenGLContext::currentContext() != nullptr;
#else
    return false;
#endif
}

// A new value holding a call's result.
static qtc_val value_of_ret(const Ret &r) {
    switch (r.kind) {
    case QTC_T_NONE: return 0;
    case QTC_T_NULL: return reg().new_value(QVariant::fromValue(nullptr));
    case QTC_T_BOOL: return reg().new_value(QVariant(r.b));
    case QTC_T_INT: return reg().new_value(QVariant(qlonglong(r.i)));
    case QTC_T_UINT: return reg().new_value(QVariant(qulonglong(r.u)));
    case QTC_T_DOUBLE: return reg().new_value(QVariant(r.d));
    case QTC_T_STR: return reg().new_value(QVariant(QString::fromUtf8(r.s)));
    case QTC_T_BYTES: return reg().new_value(QVariant(r.s));
    case QTC_T_OBJ: return reg().new_value(QVariant::fromValue(ObjRef{r.h}));
    default: return reg().new_value(r.v);
    }
}
}  // namespace qtc

extern "C" {

QTC_API qtc_val qtc_ret_val(void) {
    return guarded<qtc_val>(0, []() -> qtc_val { return value_of_ret(ret()); });
}

// -------------------------------------------------------------- properties

// The bound C++ setter of a property (setFoo for foo), if there is one: writing through it applies
// the checks qtc makes for that method (preconditions, ownership, cycles), which a plain
// QMetaProperty::write would go around.
static QByteArray bound_setter(const QObject *o, const QByteArray &prop) {
    if (prop.isEmpty()) return QByteArray();
    QByteArray name;
    if (prop == "parent" && o->inherits("QQuickItem")) name = "setParentItem";
    else name = "set" + prop.left(1).toUpper() + prop.mid(1);
    for (const QMetaObject *mo = o->metaObject(); mo; mo = mo->superClass()) {
        const ClassDef *c = find_class(QByteArray(mo->className()));
        if (!c) continue;
        for (const MethodDef *md : methods_named(c, name))
            if (!(md->flags & MF_STATIC) && md->min_args <= 1 && md->nparams >= 1) return name;
    }
    return QByteArray();
}

static qtc_status prop_set_impl(uint64_t obj, qtc_str name, bool dynamic) {
    CallFrame f;
    qtc_status err = 0;
    QObject *o = reg().object(obj, &err);
    if (!o) return fail(err, "qtc_prop_set");
    QByteArray pn;
    if (!read_bytes(name, pn, "name")) return t_err;
    ArgStack &st = *f.st;
    if (st.overflow) return fail(QTC_ERR_BAD_ARGS, "qtc_prop_set: bad argument");
    if (st.n != 1) return fail(QTC_ERR_BAD_ARGS, QStringLiteral("qtc_prop_set: expected 1 argument, got %1").arg(st.n));
    const QMetaObject *mo = o->metaObject();
    int idx = mo->indexOfProperty(pn.constData());
    QString what = QStringLiteral("%1.%2").arg(QLatin1StringView(mo->className()), QString::fromUtf8(pn));
    if (dynamic) {
        if (idx >= 0) return fail(QTC_ERR_STATE, QStringLiteral("%1 is a declared property: use qtc_prop_set").arg(what));
        o->setProperty(pn.constData(), arg_to_variant(st.a[0]));
        return QTC_OK;
    }
    if (idx < 0) {
        if (o->dynamicPropertyNames().contains(pn)) {
            o->setProperty(pn.constData(), arg_to_variant(st.a[0]));
            return QTC_OK;
        }
        return fail(QTC_ERR_NOT_FOUND, QStringLiteral("%1: no such property").arg(what));
    }
    QMetaProperty p = mo->property(idx);
    if (!p.isWritable()) return fail(QTC_ERR_READ_ONLY, what);
    QVariant v;
    if (p.isEnumType()) {
        int64_t x;
        if (int e = cv_int64(st.a[0], x, 0, INT32_MIN, UINT32_MAX)) return e;
        QMetaEnum me = p.enumerator();
        if (me.isFlag()) {
            int all = 0;
            for (int i = 0; i < me.keyCount(); i++) all |= me.value(i);
            if (uint32_t(x) & ~uint32_t(all)) return fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("%1: invalid flags 0x%2").arg(what).arg(qulonglong(x), 0, 16));
        } else if (!me.valueToKey(int(x))) {
            return fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("%1: %2 is not a %3 value").arg(what).arg(x).arg(QLatin1StringView(me.name())));
        }
        v = QVariant(int(x));
        if (!v.convert(p.metaType())) v = QVariant(int(x));
    } else if (int e = arg_to_metatype(st.a[0], p.metaType(), v, 0)) {
        return e;
    }
    // the value suits the property: it goes in through the setter when qtc binds one
    if (const QByteArray setter = bound_setter(o, pn); !setter.isEmpty())
        return do_call_in(obj, qtc_sn(setter.constData(), setter.size()), st, *f.r);
    if (!p.write(o, v)) return fail(QTC_ERR_REJECTED, QStringLiteral("%1: the value was not accepted").arg(what));
    return QTC_OK;
}

QTC_API qtc_status qtc_prop_set(qtc_obj obj, qtc_str name) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&] { return prop_set_impl(obj, name, false); });
}
QTC_API qtc_status qtc_dynprop_set(qtc_obj obj, qtc_str name) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&] { return prop_set_impl(obj, name, true); });
}

QTC_API qtc_status qtc_prop_get(qtc_obj obj, qtc_str name) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        ArgsConsumer consume;
        ret().clear_all();
        qtc_status err = 0;
        QObject *o = reg().object(obj, &err);
        if (!o) return fail(err, "qtc_prop_get");
        QByteArray pn;
        if (!read_bytes(name, pn, "name")) return t_err;
        const QMetaObject *mo = o->metaObject();
        int idx = mo->indexOfProperty(pn.constData());
        if (idx < 0) {
            if (o->dynamicPropertyNames().contains(pn)) {
                ret().set_variant(o->property(pn.constData()));
                return QTC_OK;
            }
            return fail(QTC_ERR_NOT_FOUND, QStringLiteral("%1.%2: no such property").arg(QLatin1StringView(mo->className()), QString::fromUtf8(pn)));
        }
        QMetaProperty p = mo->property(idx);
        if (!p.isReadable()) return fail(QTC_ERR_UNSUPPORTED, "property is not readable");
        QVariant v = p.read(o);
        if (p.isEnumType()) ret().set_int(v.toLongLong());
        else ret().set_variant(v);
        return QTC_OK;
    });
}

QTC_API qtc_status qtc_set_bool(qtc_obj o, qtc_str p, bool v) { qtc_args_clear(); qtc_arg_bool(v); return qtc_prop_set(o, p); }
QTC_API qtc_status qtc_set_int(qtc_obj o, qtc_str p, int64_t v) { qtc_args_clear(); qtc_arg_int(v); return qtc_prop_set(o, p); }
QTC_API qtc_status qtc_set_double(qtc_obj o, qtc_str p, double v) { qtc_args_clear(); qtc_arg_double(v); return qtc_prop_set(o, p); }
QTC_API qtc_status qtc_set_str(qtc_obj o, qtc_str p, qtc_str v) { qtc_args_clear(); qtc_arg_str(v); return qtc_prop_set(o, p); }
QTC_API bool qtc_get_bool(qtc_obj o, qtc_str p) { return qtc_prop_get(o, p) == QTC_OK ? qtc_ret_bool() : false; }
QTC_API int64_t qtc_get_int(qtc_obj o, qtc_str p) { return qtc_prop_get(o, p) == QTC_OK ? qtc_ret_int() : 0; }
QTC_API double qtc_get_double(qtc_obj o, qtc_str p) { return qtc_prop_get(o, p) == QTC_OK ? qtc_ret_double() : 0.0; }
QTC_API qtc_view qtc_get_str(qtc_obj o, qtc_str p) { return qtc_prop_get(o, p) == QTC_OK ? qtc_ret_str() : view_of(QByteArray()); }

// ------------------------------------------------------------------ values

QTC_API qtc_val qtc_val_bool(bool v) { return guarded<qtc_val>(0, [&] { return reg().new_value(QVariant(v)); }); }
QTC_API qtc_val qtc_val_int(int64_t v) { return guarded<qtc_val>(0, [&] { return reg().new_value(QVariant(qlonglong(v))); }); }
QTC_API qtc_val qtc_val_uint(uint64_t v) { return guarded<qtc_val>(0, [&] { return reg().new_value(QVariant(qulonglong(v))); }); }
QTC_API qtc_val qtc_val_double(double v) { return guarded<qtc_val>(0, [&] { return reg().new_value(QVariant(v)); }); }
QTC_API qtc_val qtc_val_str(qtc_str v) {
    return guarded<qtc_val>(0, [&]() -> qtc_val {
        QString s;
        if (!read_str(v, s, "value")) return 0;
        return reg().new_value(QVariant(s));
    });
}
QTC_API qtc_val qtc_val_bytes(qtc_str v) {
    return guarded<qtc_val>(0, [&]() -> qtc_val {
        QByteArray s;
        if (!read_bytes(v, s, "value")) return 0;
        return reg().new_value(QVariant(s));
    });
}
QTC_API qtc_val qtc_val_obj(qtc_obj v) {
    return guarded<qtc_val>(0, [&]() -> qtc_val {
        qtc_status err = 0;
        if (!reg().object(v, &err)) { fail(err, "qtc_val_obj"); return 0; }
        return reg().new_value(QVariant::fromValue(ObjRef{v}));
    });
}
QTC_API qtc_val qtc_val_list(void) { return guarded<qtc_val>(0, [] { return reg().new_value(QVariant(QVariantList())); }); }
QTC_API qtc_val qtc_val_map(void) { return guarded<qtc_val>(0, [] { return reg().new_value(QVariant(QVariantMap())); }); }

QTC_API qtc_val qtc_val_new(qtc_str type_name) {
    return guarded<qtc_val>(0, [&]() -> qtc_val {
        CallFrame f;
        QByteArray tn;
        if (!read_bytes(type_name, tn, "type_name")) return 0;
        tn = tn.trimmed();
        const ClassDef *c = find_class(tn);
        if (c && c->kind == CK_OBJECT) { fail(QTC_ERR_WRONG_TYPE, QStringLiteral("%1 is an object class: use qtc_new").arg(QString::fromUtf8(tn))); return 0; }
        if (c && c->nctors > 0) {
            QString why;
            if (!need_ok(c, &why)) { fail(QTC_ERR_STATE, why); return 0; }
            std::vector<Cand> cands;
            for (uint32_t i = 0; i < c->nctors; i++) {
                Cand k;
                k.md = &c->ctors[i];
                k.cls = c;
                cands.push_back(k);
            }
            if (run_candidates(cands, nullptr, nullptr, Label{QLatin1StringView(c->name), QLatin1StringView(), "%1 value"}, *f.st, *f.r) != QTC_OK) return 0;
            return value_of_ret(*f.r);
        }
        if (f.st->n != 0) { fail(QTC_ERR_BAD_ARGS, QStringLiteral("%1 takes no constructor arguments").arg(QString::fromUtf8(tn))); return 0; }
        QMetaType mt = (c && c->metatype) ? c->metatype() : QMetaType::fromName(tn);
        if (!mt.isValid() || (mt.flags() & QMetaType::PointerToQObject)) { fail(QTC_ERR_NOT_FOUND, QStringLiteral("no value type named %1").arg(QString::fromUtf8(tn))); return 0; }
        if (c) {
            QString why;
            if (!need_ok(c, &why)) { fail(QTC_ERR_STATE, why); return 0; }
        }
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        const bool default_ok = mt.isDefaultConstructible();
#else
        const bool default_ok = mt.iface() && mt.iface()->defaultCtr;
#endif
        if (!(mt.flags() & QMetaType::NeedsConstruction) || default_ok)
            return reg().new_value(QVariant(mt));
        fail(QTC_ERR_UNSUPPORTED, QStringLiteral("%1 cannot be default-constructed").arg(QString::fromUtf8(tn)));
        return 0;
    });
}

static QVariant *val_or_fail(qtc_val v, const char *what) {
    qtc_status err = 0;
    QVariant *p = reg().value(v, &err);
    if (!p) fail(err, what);
    return p;
}

QTC_API qtc_val qtc_val_from_json(qtc_str json) {
    return guarded<qtc_val>(0, [&]() -> qtc_val {
        QByteArray b;
        if (!read_bytes(json, b, "json")) return 0;
        QJsonParseError pe;
        QJsonDocument d = QJsonDocument::fromJson("[" + b + "]", &pe);
        if (pe.error != QJsonParseError::NoError || !d.isArray()) {
            fail(QTC_ERR_CONVERSION, QStringLiteral("invalid JSON: %1").arg(pe.errorString()));
            return 0;
        }
        return reg().new_value(d.array().at(0).toVariant());
    });
}
QTC_API qtc_status qtc_val_free(qtc_val v) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status e = reg().free_value(v);
        return e ? fail(e, "qtc_val_free") : QTC_OK;
    });
}
QTC_API qtc_val qtc_val_copy(qtc_val v) {
    return guarded<qtc_val>(0, [&]() -> qtc_val {
        QVariant *p = val_or_fail(v, "qtc_val_copy");
        return p ? reg().new_value(*p) : 0;
    });
}
QTC_API int32_t qtc_val_kind(qtc_val v) {
    return guarded<int32_t>(QTC_ERR_STATE, [&]() -> int32_t {
        QVariant *p = val_or_fail(v, "qtc_val_kind");
        return p ? variant_kind(*p) : t_err;
    });
}
QTC_API qtc_view qtc_val_type_name(qtc_val v) {
    return guarded<qtc_view>(qtc_view{"", 0}, [&]() -> qtc_view {
        QVariant *p = val_or_fail(v, "qtc_val_type_name");
        if (!p) return view_of(QByteArray());
        if (p->metaType() == QMetaType::fromType<ObjRef>()) return view_of(QByteArray("QObject*"));
        return view_of(QByteArray(p->typeName() ? p->typeName() : ""));
    });
}
QTC_API qtc_status qtc_val_get_bool(qtc_val v, bool *out) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        if (!out) return fail(QTC_ERR_INVALID_ARG, "out is NULL");
        QVariant *p = val_or_fail(v, "qtc_val_get_bool");
        if (!p) return t_err;
        if (!p->canConvert<bool>()) return fail(QTC_ERR_CONVERSION, "not convertible to bool");
        *out = p->toBool();
        return QTC_OK;
    });
}
QTC_API qtc_status qtc_val_get_int(qtc_val v, int64_t *out) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        if (!out) return fail(QTC_ERR_INVALID_ARG, "out is NULL");
        QVariant *p = val_or_fail(v, "qtc_val_get_int");
        if (!p) return t_err;
        bool ok = false;
        qlonglong x = p->toLongLong(&ok);
        if (!ok) return fail(QTC_ERR_CONVERSION, QStringLiteral("%1 is not convertible to an integer").arg(QLatin1StringView(p->typeName())));
        *out = x;
        return QTC_OK;
    });
}
QTC_API qtc_status qtc_val_get_uint(qtc_val v, uint64_t *out) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        if (!out) return fail(QTC_ERR_INVALID_ARG, "out is NULL");
        QVariant *p = val_or_fail(v, "qtc_val_get_uint");
        if (!p) return t_err;
        bool ok = false;
        qulonglong x = p->toULongLong(&ok);
        if (!ok) return fail(QTC_ERR_CONVERSION, "not convertible to an unsigned integer");
        *out = x;
        return QTC_OK;
    });
}
QTC_API qtc_status qtc_val_get_double(qtc_val v, double *out) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        if (!out) return fail(QTC_ERR_INVALID_ARG, "out is NULL");
        QVariant *p = val_or_fail(v, "qtc_val_get_double");
        if (!p) return t_err;
        bool ok = false;
        double x = p->toDouble(&ok);
        if (!ok) return fail(QTC_ERR_CONVERSION, "not convertible to a number");
        *out = x;
        return QTC_OK;
    });
}
QTC_API qtc_view qtc_val_get_str(qtc_val v) {
    return guarded<qtc_view>(qtc_view{"", 0}, [&]() -> qtc_view {
        QVariant *p = val_or_fail(v, "qtc_val_get_str");
        if (!p) return view_of(QByteArray());
        if (p->metaType().id() == QMetaType::QByteArray) return view_of(p->toByteArray());
        if (!p->canConvert<QString>()) { fail(QTC_ERR_CONVERSION, "not convertible to a string"); return view_of(QByteArray()); }
        return view_of(p->toString().toUtf8());
    });
}
QTC_API qtc_obj qtc_val_get_obj(qtc_val v) {
    return guarded<qtc_obj>(0, [&]() -> qtc_obj {
        QVariant *p = val_or_fail(v, "qtc_val_get_obj");
        if (!p) return 0;
        if (p->metaType() != QMetaType::fromType<ObjRef>()) { fail(QTC_ERR_CONVERSION, "the value does not hold an object"); return 0; }
        uint64_t h = p->value<ObjRef>().handle;
        if (!reg().alive(h)) { fail(QTC_ERR_STALE_HANDLE, "the object was destroyed"); return 0; }
        return h;
    });
}
QTC_API qtc_val qtc_val_convert(qtc_val v, qtc_str type_name) {
    return guarded<qtc_val>(0, [&]() -> qtc_val {
        QVariant *p = val_or_fail(v, "qtc_val_convert");
        if (!p) return 0;
        QByteArray tn;
        if (!read_bytes(type_name, tn, "type_name")) return 0;
        QMetaType mt = QMetaType::fromName(tn.trimmed());
        if (!mt.isValid()) { fail(QTC_ERR_NOT_FOUND, QStringLiteral("no type named %1").arg(QString::fromUtf8(tn))); return 0; }
        QVariant c = desanitize(*p);
        if (!c.canConvert(mt) || !c.convert(mt)) { fail(QTC_ERR_CONVERSION, QStringLiteral("cannot convert %1 to %2").arg(QLatin1StringView(p->typeName()), QString::fromUtf8(tn))); return 0; }
        return reg().new_value(sanitize(c));
    });
}
QTC_API bool qtc_val_equal(qtc_val a, qtc_val b) {
    return guarded<bool>(false, [&]() -> bool {
        QVariant *pa = val_or_fail(a, "qtc_val_equal");
        QVariant *pb = val_or_fail(b, "qtc_val_equal");
        if (!pa || !pb) return false;
        return *pa == *pb;
    });
}
static QJsonValue to_json(const QVariant &v) {
    if (v.metaType() == QMetaType::fromType<ObjRef>()) {
        QJsonObject o;
        o.insert(QStringLiteral("$object"), QString::number(v.value<ObjRef>().handle));
        return o;
    }
    if (native_list(v)) {
        QJsonArray a;
        for (qsizetype i = 0, n = seq_size(v); i < n; i++) a.append(to_json(seq_at(v, i)));
        return a;
    }
    switch (v.metaType().id()) {
    case QMetaType::QVariantList: {
        QJsonArray a;
        for (const QVariant &e : v.toList()) a.append(to_json(e));
        return a;
    }
    case QMetaType::QVariantMap: case QMetaType::QVariantHash: {
        QJsonObject o;
        const QVariantMap m = v.toMap();
        for (auto it = m.begin(); it != m.end(); ++it) o.insert(it.key(), to_json(it.value()));
        return o;
    }
    default: break;
    }
    QJsonValue j = QJsonValue::fromVariant(v);
    if (j.isNull() && v.isValid() && v.canConvert<QString>()) return QJsonValue(v.toString());
    return j;
}
QTC_API qtc_view qtc_val_to_json(qtc_val v) {
    return guarded<qtc_view>(qtc_view{"", 0}, [&]() -> qtc_view {
        QVariant *p = val_or_fail(v, "qtc_val_to_json");
        if (!p) return view_of(QByteArray());
        QJsonArray wrap;
        wrap.append(to_json(*p));
        QByteArray s = QJsonDocument(wrap).toJson(QJsonDocument::Compact);
        return view_of(s.mid(1, s.size() - 2));
    });
}
QTC_API qtc_view qtc_val_to_string(qtc_val v) {
    return guarded<qtc_view>(qtc_view{"", 0}, [&]() -> qtc_view {
        QVariant *p = val_or_fail(v, "qtc_val_to_string");
        if (!p) return view_of(QByteArray());
        if (p->canConvert<QString>() && variant_kind(*p) != QTC_T_LIST && variant_kind(*p) != QTC_T_MAP)
            return view_of(p->toString().toUtf8());
        QJsonArray wrap;
        wrap.append(to_json(*p));
        QByteArray s = QJsonDocument(wrap).toJson(QJsonDocument::Compact);
        QByteArray body = s.mid(1, s.size() - 2);
        if (variant_kind(*p) == QTC_T_VALUE) body = QByteArray(p->typeName()) + "(" + body + ")";
        return view_of(body);
    });
}

// A list to change: a string list or a typed list becomes a QVariantList first.
static bool as_list(QVariant *p, const char *what) {
    if (p->metaType().id() == QMetaType::QVariantList) return true;
    if (p->canConvert<QVariantList>() && (p->metaType().id() == QMetaType::QStringList || native_list(*p))) {
        *p = QVariant(p->toList());
        return true;
    }
    fail(QTC_ERR_WRONG_TYPE, QStringLiteral("%1: the value is a %2, not a list").arg(QLatin1StringView(what), QLatin1StringView(p->typeName())));
    return false;
}
static bool as_map(QVariant *p, const char *what) {
    if (p->metaType().id() == QMetaType::QVariantMap) return true;
    if (p->metaType().id() == QMetaType::QVariantHash) {
        *p = QVariant(p->toMap());
        return true;
    }
    fail(QTC_ERR_WRONG_TYPE, QStringLiteral("%1: the value is a %2, not a map").arg(QLatin1StringView(what), QLatin1StringView(p->typeName())));
    return false;
}

QTC_API int64_t qtc_list_len(qtc_val list) {
    return guarded<int64_t>(QTC_ERR_STATE, [&]() -> int64_t {
        QVariant *p = val_or_fail(list, "qtc_list_len");
        if (!p) return t_err;
        if (native_list(*p)) return int64_t(seq_size(*p));   // read in place
        if (!as_list(p, "qtc_list_len")) return t_err;
        return int64_t(static_cast<const QVariantList *>(p->constData())->size());
    });
}
QTC_API qtc_val qtc_list_get(qtc_val list, int64_t index) {
    return guarded<qtc_val>(0, [&]() -> qtc_val {
        QVariant *p = val_or_fail(list, "qtc_list_get");
        if (p && native_list(*p)) {
            const qsizetype n = seq_size(*p);
            if (index < 0 || index >= n) {
                fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("index %1 of a list of %2").arg(index).arg(n));
                return 0;
            }
            return reg().new_value(seq_at(*p, qsizetype(index)));
        }
        if (!p || !as_list(p, "qtc_list_get")) return 0;
        const QVariantList *l = static_cast<const QVariantList *>(p->constData());
        if (index < 0 || index >= l->size()) { fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("index %1 of a list of %2").arg(index).arg(l->size())); return 0; }
        return reg().new_value(l->at(qsizetype(index)));
    });
}
QTC_API qtc_status qtc_list_append(qtc_val list, qtc_val item) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        QVariant *p = val_or_fail(list, "qtc_list_append");
        if (!p) return t_err;
        if (!as_list(p, "qtc_list_append")) return t_err;
        qtc_status err = 0;
        QVariant item_v;
        if (QVariant *q = reg().value(item, &err)) item_v = *q;
        else if (err == QTC_ERR_WRONG_TYPE && reg().object(item, &err)) item_v = QVariant::fromValue(ObjRef{item});
        else return fail(err, "qtc_list_append item");
        static_cast<QVariantList *>(p->data())->append(item_v);
        return QTC_OK;
    });
}
QTC_API qtc_status qtc_list_set(qtc_val list, int64_t index, qtc_val item) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        QVariant *p = val_or_fail(list, "qtc_list_set");
        if (!p) return t_err;
        if (!as_list(p, "qtc_list_set")) return t_err;
        QVariantList *l = static_cast<QVariantList *>(p->data());
        if (index < 0 || index >= l->size()) return fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("index %1 of a list of %2").arg(index).arg(l->size()));
        qtc_status err = 0;
        QVariant item_v;
        if (QVariant *q = reg().value(item, &err)) item_v = *q;
        else if (err == QTC_ERR_WRONG_TYPE && reg().object(item, &err)) item_v = QVariant::fromValue(ObjRef{item});
        else return fail(err, "qtc_list_set item");
        (*l)[qsizetype(index)] = item_v;
        return QTC_OK;
    });
}
QTC_API qtc_status qtc_list_remove(qtc_val list, int64_t index) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        QVariant *p = val_or_fail(list, "qtc_list_remove");
        if (!p) return t_err;
        if (!as_list(p, "qtc_list_remove")) return t_err;
        QVariantList *l = static_cast<QVariantList *>(p->data());
        if (index < 0 || index >= l->size()) return fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("index %1 of a list of %2").arg(index).arg(l->size()));
        l->removeAt(qsizetype(index));
        return QTC_OK;
    });
}
QTC_API qtc_status qtc_list_clear(qtc_val list) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        QVariant *p = val_or_fail(list, "qtc_list_clear");
        if (!p) return t_err;
        if (!as_list(p, "qtc_list_clear")) return t_err;
        static_cast<QVariantList *>(p->data())->clear();
        return QTC_OK;
    });
}
QTC_API int64_t qtc_map_len(qtc_val map) {
    return guarded<int64_t>(QTC_ERR_STATE, [&]() -> int64_t {
        QVariant *p = val_or_fail(map, "qtc_map_len");
        if (!p) return t_err;
        if (!as_map(p, "qtc_map_len")) return t_err;
        return int64_t(static_cast<const QVariantMap *>(p->constData())->size());
    });
}
QTC_API qtc_val qtc_map_get(qtc_val map, qtc_str key) {
    return guarded<qtc_val>(0, [&]() -> qtc_val {
        QVariant *p = val_or_fail(map, "qtc_map_get");
        if (!p || !as_map(p, "qtc_map_get")) return 0;
        QString k;
        if (!read_str(key, k, "key")) return 0;
        const QVariantMap *m = static_cast<const QVariantMap *>(p->constData());
        auto it = m->constFind(k);
        if (it == m->constEnd()) { fail(QTC_ERR_NOT_FOUND, QStringLiteral("no key '%1'").arg(k)); return 0; }
        return reg().new_value(it.value());
    });
}
QTC_API qtc_status qtc_map_set(qtc_val map, qtc_str key, qtc_val item) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        QVariant *p = val_or_fail(map, "qtc_map_set");
        if (!p) return t_err;
        if (!as_map(p, "qtc_map_set")) return t_err;
        QString k;
        if (!read_str(key, k, "key")) return t_err;
        qtc_status err = 0;
        QVariant item_v;
        if (QVariant *q = reg().value(item, &err)) item_v = *q;
        else if (err == QTC_ERR_WRONG_TYPE && reg().object(item, &err)) item_v = QVariant::fromValue(ObjRef{item});
        else return fail(err, "qtc_map_set item");
        static_cast<QVariantMap *>(p->data())->insert(k, item_v);
        return QTC_OK;
    });
}
QTC_API qtc_status qtc_map_remove(qtc_val map, qtc_str key) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        QVariant *p = val_or_fail(map, "qtc_map_remove");
        if (!p) return t_err;
        if (!as_map(p, "qtc_map_remove")) return t_err;
        QString k;
        if (!read_str(key, k, "key")) return t_err;
        if (!static_cast<QVariantMap *>(p->data())->remove(k)) return fail(QTC_ERR_NOT_FOUND, QStringLiteral("no key '%1'").arg(k));
        return QTC_OK;
    });
}
QTC_API bool qtc_map_has(qtc_val map, qtc_str key) {
    return guarded<bool>(false, [&]() -> bool {
        QVariant *p = val_or_fail(map, "qtc_map_has");
        if (!p || !as_map(p, "qtc_map_has")) return false;
        QString k;
        if (!read_str(key, k, "key")) return false;
        return static_cast<const QVariantMap *>(p->constData())->contains(k);
    });
}
QTC_API qtc_val qtc_map_keys(qtc_val map) {
    return guarded<qtc_val>(0, [&]() -> qtc_val {
        QVariant *p = val_or_fail(map, "qtc_map_keys");
        if (!p || !as_map(p, "qtc_map_keys")) return 0;
        QVariantList keys;
        for (const QString &k : static_cast<const QVariantMap *>(p->constData())->keys()) keys.append(k);
        return reg().new_value(QVariant(keys));
    });
}

// ----------------------------------------------------------- introspection

QTC_API bool qtc_class_exists(qtc_str class_name) {
    return guarded<bool>(false, [&]() -> bool {
        QByteArray cn;
        if (!read_bytes(class_name, cn, "class_name")) return false;
        return find_class(cn.trimmed()) != nullptr;
    });
}
QTC_API int64_t qtc_class_count(void) { return guarded<int64_t>(0, [] { return int64_t(class_names().size()); }); }
QTC_API qtc_view qtc_class_at(int64_t index) {
    return guarded<qtc_view>(qtc_view{"", 0}, [&]() -> qtc_view {
        if (index < 0 || index >= class_names().size()) { fail(QTC_ERR_OUT_OF_RANGE, "class index"); return view_of(QByteArray()); }
        return view_of(class_names().at(qsizetype(index)));
    });
}

static QJsonArray params_json(const MethodDef &m) {
    QJsonArray a;
    for (int i = 0; i < m.nparams; i++) {
        QJsonObject p;
        p.insert(QStringLiteral("type"), QString::fromLatin1(m.params[i].type ? m.params[i].type : ""));
        p.insert(QStringLiteral("kind"), int(m.params[i].kind));
        if ((m.params[i].kind == PK_ENUM || m.params[i].kind == PK_FLAGS) && m.params[i].enum_id >= 0 && m.params[i].enum_id < g_enum_count)
            p.insert(QStringLiteral("enum"), QString::fromLatin1(g_enums[m.params[i].enum_id].name));
        p.insert(QStringLiteral("optional"), i >= m.min_args);
        a.append(p);
    }
    return a;
}

QTC_API qtc_view qtc_describe(uint64_t handle, qtc_str class_name) {
    return guarded<qtc_view>(qtc_view{"", 0}, [&]() -> qtc_view {
        QJsonObject root;
        const QMetaObject *mo = nullptr;
        const ClassDef *c = nullptr;
        QByteArray cn;
        if (handle) {
            int k = reg().kind_of(handle);
            qtc_status err = 0;
            if (k == SK_VAL) {
                QVariant *v = reg().value(handle, &err);
                if (!v) { fail(err, "qtc_describe"); return view_of(QByteArray()); }
                cn = QByteArray(v->typeName() ? v->typeName() : "");
            } else {
                QObject *o = reg().object(handle, &err);
                if (!o) { fail(err, "qtc_describe"); return view_of(QByteArray()); }
                mo = o->metaObject();
                cn = mo->className();
            }
        } else {
            if (!read_bytes(class_name, cn, "class_name")) return view_of(QByteArray());
            cn = cn.trimmed();
        }
        c = find_class(cn);
        if (c && c->meta && !mo) mo = c->meta;
        if (!c && !mo) { fail(QTC_ERR_NOT_FOUND, QStringLiteral("no class named %1").arg(QString::fromUtf8(cn))); return view_of(QByteArray()); }
        root.insert(QStringLiteral("class"), QString::fromLatin1(cn));
        if (c) {
            root.insert(QStringLiteral("module"), QString::fromLatin1(c->module));
            root.insert(QStringLiteral("kind"), c->kind == CK_OBJECT ? QStringLiteral("object") : QStringLiteral("value"));
        }
        QJsonArray bases;
        if (mo) for (const QMetaObject *m = mo->superClass(); m; m = m->superClass()) bases.append(QString::fromLatin1(m->className()));
        else if (c && c->bases) for (int i = 0; c->bases[i]; i++) bases.append(QString::fromLatin1(c->bases[i]));
        root.insert(QStringLiteral("bases"), bases);
        if (mo) {
            QJsonArray props, sigs, table;
            for (int i = 0; i < mo->propertyCount(); i++) {
                QMetaProperty p = mo->property(i);
                QJsonObject j;
                j.insert(QStringLiteral("name"), QString::fromLatin1(p.name()));
                j.insert(QStringLiteral("type"), QString::fromLatin1(p.typeName() ? p.typeName() : ""));
                j.insert(QStringLiteral("writable"), p.isWritable());
                j.insert(QStringLiteral("notify"), p.hasNotifySignal() ? QString::fromLatin1(p.notifySignal().methodSignature()) : QString());
                props.append(j);
            }
            for (int i = 0; i < mo->methodCount(); i++) {
                QMetaMethod m = mo->method(i);
                if (m.access() != QMetaMethod::Public) continue;
                QString s = QString::fromLatin1(m.methodSignature());
                if (m.methodType() == QMetaMethod::Signal) sigs.append(s);
                else if (m.methodType() != QMetaMethod::Constructor) table.append(s);
            }
            root.insert(QStringLiteral("properties"), props);
            root.insert(QStringLiteral("signals"), sigs);
            root.insert(QStringLiteral("meta_methods"), table);
        }
        QJsonArray methods, ctors;
        auto add_methods = [&](const ClassDef *cc) {
            for (uint32_t i = 0; i < cc->nmethods; i++) {
                const MethodDef &m = cc->methods[i];
                QJsonObject j;
                j.insert(QStringLiteral("sig"), QString::fromLatin1(m.sig));
                j.insert(QStringLiteral("class"), QString::fromLatin1(cc->name));
                j.insert(QStringLiteral("returns"), QString::fromLatin1(m.ret_type ? m.ret_type : "void"));
                j.insert(QStringLiteral("static"), bool(m.flags & MF_STATIC));
                j.insert(QStringLiteral("blocking"), bool(m.flags & MF_BLOCKING));
                j.insert(QStringLiteral("nofuzz"), bool(m.flags & MF_NOFUZZ));
                j.insert(QStringLiteral("min"), int(m.min_args));
                j.insert(QStringLiteral("params"), params_json(m));
                methods.append(j);
            }
        };
        if (mo) {
            for (const QMetaObject *m = mo; m; m = m->superClass())
                if (const ClassDef *cc = find_class(QByteArray(m->className()))) add_methods(cc);
        } else if (c) {
            add_methods(c);
            if (c->bases) for (int i = 0; c->bases[i]; i++) if (const ClassDef *b = find_class(QByteArray(c->bases[i]))) add_methods(b);
        }
        if (c)
            for (uint32_t i = 0; i < c->nctors; i++) {
                QJsonObject j;
                j.insert(QStringLiteral("sig"), QString::fromLatin1(c->ctors[i].sig));
                j.insert(QStringLiteral("nofuzz"), bool(c->ctors[i].flags & MF_NOFUZZ));
                j.insert(QStringLiteral("params"), params_json(c->ctors[i]));
                ctors.append(j);
            }
        root.insert(QStringLiteral("methods"), methods);
        root.insert(QStringLiteral("constructors"), ctors);
        return view_of(QJsonDocument(root).toJson(QJsonDocument::Compact));
    });
}

static QHash<QByteArray, int64_t> &enum_index() {
    static auto *h = new QHash<QByteArray, int64_t>;
    if (h->isEmpty() && g_enum_count > 0) {
        for (int i = 0; i < g_enum_count; i++) {
            const EnumInfo &e = g_enums[i];
            QByteArray full(e.name);
            int p = full.lastIndexOf("::");
            QByteArray scope = p >= 0 ? full.left(p) : QByteArray();
            for (int k = 0; k < e.count; k++) {
                QByteArray key(e.keys[k]);
                h->insert(full + "::" + key, e.values[k]);
                if (!scope.isEmpty() && !h->contains(scope + "::" + key)) h->insert(scope + "::" + key, e.values[k]);
            }
        }
    }
    return *h;
}

QTC_API qtc_status qtc_enum_value(qtc_str qualified_name, int64_t *out) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        if (!out) return fail(QTC_ERR_INVALID_ARG, "out is NULL");
        QByteArray n;
        if (!read_bytes(qualified_name, n, "name")) return t_err;
        auto &idx = enum_index();
        auto it = idx.constFind(n.trimmed());
        if (it == idx.constEnd()) return fail(QTC_ERR_NOT_FOUND, QStringLiteral("no enum value %1").arg(QString::fromUtf8(n)));
        *out = *it;
        return QTC_OK;
    });
}
QTC_API qtc_view qtc_enum_key(qtc_str enum_name, int64_t value) {
    return guarded<qtc_view>(qtc_view{"", 0}, [&]() -> qtc_view {
        QByteArray n;
        if (!read_bytes(enum_name, n, "enum_name")) return view_of(QByteArray());
        n = n.trimmed();
        for (int i = 0; i < g_enum_count; i++)
            if (n == g_enums[i].name)
                for (int k = 0; k < g_enums[i].count; k++)
                    if (g_enums[i].values[k] == value) return view_of(QByteArray(g_enums[i].keys[k]));
        fail(QTC_ERR_NOT_FOUND, QStringLiteral("%1 has no key for %2").arg(QString::fromUtf8(n)).arg(value));
        return view_of(QByteArray());
    });
}

QTC_API qtc_view qtc_enum_describe(qtc_str enum_name) {
    return guarded<qtc_view>(qtc_view{"", 0}, [&]() -> qtc_view {
        QByteArray n;
        if (!read_bytes(enum_name, n, "enum_name")) return view_of(QByteArray());
        n = n.trimmed();
        for (int i = 0; i < g_enum_count; i++) {
            const EnumInfo &e = g_enums[i];
            if (n != e.name) continue;
            QJsonObject j;
            j.insert(QStringLiteral("name"), QString::fromLatin1(e.name));
            j.insert(QStringLiteral("flags"), bool(e.flags & 1));
            j.insert(QStringLiteral("open"), bool(e.flags & 2));
            QJsonArray vals;
            for (int k = 0; k < e.count; k++) vals.append(QJsonArray{QString::fromLatin1(e.keys[k]), qint64(e.values[k])});
            j.insert(QStringLiteral("values"), vals);
            return view_of(QJsonDocument(j).toJson(QJsonDocument::Compact));
        }
        fail(QTC_ERR_NOT_FOUND, QStringLiteral("no enum named %1").arg(QString::fromUtf8(n)));
        return view_of(QByteArray());
    });
}

QTC_API qtc_view qtc_stats(void) {
    return guarded<qtc_view>(qtc_view{"", 0}, []() -> qtc_view {
        int64_t nm = 0, nc = 0, objs = 0, vals = 0;
        QJsonObject mods;
        for (const QByteArray &n : class_names()) {
            const ClassDef *c = find_class(n);
            nm += c->nmethods;
            nc += c->nctors;
            (c->kind == CK_OBJECT ? objs : vals)++;
            QJsonObject m = mods.value(QString::fromLatin1(c->module)).toObject();
            m.insert(QStringLiteral("classes"), m.value(QStringLiteral("classes")).toInt() + 1);
            m.insert(QStringLiteral("methods"), m.value(QStringLiteral("methods")).toInt() + int(c->nmethods));
            mods.insert(QString::fromLatin1(c->module), m);
        }
        QJsonObject j;
        j.insert(QStringLiteral("qtc"), QStringLiteral(QTC_VERSION_STRING));
        j.insert(QStringLiteral("qt_runtime"), QString::fromLatin1(qVersion()));
        j.insert(QStringLiteral("qt_build"), QStringLiteral(QT_VERSION_STR));
        j.insert(QStringLiteral("classes"), qint64(class_names().size()));
        j.insert(QStringLiteral("object_classes"), qint64(objs));
        j.insert(QStringLiteral("value_classes"), qint64(vals));
        j.insert(QStringLiteral("methods"), qint64(nm));
        j.insert(QStringLiteral("constructors"), qint64(nc));
        j.insert(QStringLiteral("enums"), g_enum_count);
        j.insert(QStringLiteral("modules"), mods);
        // Qt modules this build links that are available under the GPL-3.0 only (docs/LICENSING.md);
        // empty for the default build, whose Qt modules are all available under the LGPL-3.0
        QJsonArray gpl;
        for (const char *m : {"Charts", "NetworkAuth", "HttpServer", "Mqtt", "Coap"})
            if (mods.contains(QLatin1StringView(m))) gpl.append(QLatin1StringView(m));
        j.insert(QStringLiteral("gpl_only_modules"), gpl);
        return view_of(QJsonDocument(j).toJson(QJsonDocument::Compact));
    });
}

}  // extern "C"

// ------------------------------------------------------- message handler

static void message_handler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg) {
    if (g_prev_handler) {
        g_prev_handler(type, ctx, msg);
        return;
    }
    QByteArray m = qFormatLogMessage(type, ctx, msg).toLocal8Bit();
    std::fprintf(stderr, "%s\n", m.constData());
    std::fflush(stderr);
}
