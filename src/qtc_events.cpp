// qtc runtime: the event queue, signal capture, watches, timers and the loop.
#include "qtc_events.h"

#include <QtCore/QCoreApplication>
#include <cstring>
#include <QtCore/QEventLoop>
#include <QtCore/QMimeData>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#ifdef QTC_HAS_GUI
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtGui/QWindow>
#endif
#ifdef QTC_HAS_WIDGETS
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>
#endif

#include <deque>

namespace qtc {

static constexpr int kRecent = 64;
static constexpr int kWakeEvent = QEvent::User + 0x5100;
static constexpr int kReleaseEvent = QEvent::User + 0x5101;

// A signal's scalar arguments (bool, integers, floating point), kept as values: the QVariants
// qtc_event_arg_* reads are made only if it is asked (most handlers read the payload fields).
struct ScalarArg {
    int type = 0;
    int64_t i = 0;
    double d = 0;
};

struct QueuedEvent {
    qtc_event e{};
    QList<QVariant> args;
    ScalarArg sargs[4];
    int nsargs = 0;   // > 0: args is empty and these are the arguments
    QByteArray text;
    QByteArray signal;
    uint32_t conn_slot = UINT32_MAX;
};

struct Connection {
    uint32_t gen = 1;
    bool used = false;
    bool forward = false;
    bool destroyed_signal = false;
    bool far = false;                // the sender lives in another thread (its emissions arrive queued)
    QObject *sender = nullptr;
    QObject *receiver = nullptr;     // forward connections (qtc_connect_to)
    uint64_t receiver_handle = 0;
    QByteArray method;               // forwards: the receiver method's signature
    int forward_n = 0;               // forwards: how many of the signal's arguments it takes
    uint64_t sender_handle = 0;
    QMetaMethod signal;
    QByteArray signature;
    uint64_t tag = 0;
    uint32_t flags = 0;
    int32_t kind = QTC_EV_SIGNAL;
    QMetaObject::Connection qconn;
    uint64_t pending_seq = 0;
    uint64_t batch = 0;
};

struct Watch {
    uint32_t gen = 1;
    bool used = false;
    QObject *obj = nullptr;
    uint64_t obj_handle = 0;
    uint64_t mask = 0, consume = 0, tag = 0;
};

class Spy : public QObject {   // no Q_OBJECT: its method offsets begin right after QObject's
public:
    int qt_metacall(QMetaObject::Call call, int id, void **a) override;
    bool event(QEvent *e) override;
};
class Filter : public QObject {
public:
    bool eventFilter(QObject *o, QEvent *e) override;
};
class AppFilter : public QObject {
public:
    bool eventFilter(QObject *o, QEvent *e) override;
};

struct Events {
    std::mutex mu;   // queue + counters + spy pointer (posts come from any thread)
    std::deque<QueuedEvent> q;
    uint64_t next_seq = 1;
    int64_t limit = 1000000;
    int64_t dropped = 0;
    bool open = false;
    Spy *spy = nullptr;
    // GUI thread only
    std::vector<QueuedEvent> recent = std::vector<QueuedEvent>(kRecent);
    std::vector<Connection> conns;
    std::vector<uint32_t> conn_free;
    std::vector<uint32_t> quarantine;
    uint64_t batch = 0;
    std::vector<Watch> watches;
    std::vector<uint32_t> watch_free;
    QHash<QObject *, QList<uint32_t>> watches_by_obj;
    QHash<QObject *, QList<uint32_t>> conns_by_obj;
    Filter *filter = nullptr;
    AppFilter *appfilter = nullptr;
    QEventLoop *wait_loop = nullptr;
    bool waiting = false;
    bool woken = false;
    bool quit = false;
    int exit_code = 0;
    bool in_handler = false;
    bool window_shown = false;
    bool check_pending = false;
    qtc_handler handler = nullptr;
    void *handler_user = nullptr;
    QByteArray text_store;
};
static Events &E() { static Events *e = new Events; return *e; }

// ------------------------------------------------------------------ queue

static void wake(bool gui) {
    Events &ev = E();
    if (gui) {
        if (ev.wait_loop) {
            ev.woken = true;
            ev.wait_loop->exit(1);
        }
        return;
    }
    std::lock_guard<std::mutex> lk(ev.mu);
    if (ev.spy) QCoreApplication::postEvent(ev.spy, new QEvent(QEvent::Type(kWakeEvent)));
}

// Returns the new event's sequence number, or 0 if it was dropped.
static uint64_t enqueue(QueuedEvent &&qe, bool gui) {
    Events &ev = E();
    uint64_t seq = 0;
    bool was_empty = false;
    {
        std::lock_guard<std::mutex> lk(ev.mu);
        if (!ev.open) return 0;
        if (ev.limit > 0 && int64_t(ev.q.size()) >= ev.limit) {
            ev.dropped++;
            return 0;
        }
        seq = ev.next_seq++;
        qe.e.seq = seq;
        if (qe.e.count == 0) qe.e.count = 1;
        was_empty = ev.q.empty();
        ev.q.push_back(std::move(qe));
    }
    // qtc_wait checks the queue before it blocks, so only the first event of a batch needs a wake-up.
    if (gui || was_empty) wake(gui);
    return seq;
}

// For the other layers (futures): queues an event built elsewhere. GUI thread.
uint64_t events_push(const qtc_event &e, const QByteArray &text) {
    QueuedEvent qe;
    qe.e = e;
    qe.text = text;
    return enqueue(std::move(qe), true);
}

static bool within(const QObject *o, const QObject *root) {
    for (int d = 0; o && d < 10000; o = o->parent(), d++)
        if (o == root) return true;
    return false;
}

// qtc_move_to_thread: whether a qtc_connect_to forward starts or ends inside root (a forward calls
// its receiver inside the emission, which must not cross threads). GUI thread.
bool events_forwards_below(QObject *root) {
    // forwards only join objects of the GUI thread, whose deaths clear these pointers at once
    for (const Connection &c : E().conns)
        if (c.used && c.forward && !c.far && (within(c.sender, root) || within(c.receiver, root))) return true;
    return false;
}

// root moved to another thread: the signals of what is inside it now come from there.
void events_mark_far(QObject *root) {
    // a far sender's pointer may dangle until its release arrives: only near ones are looked at
    for (Connection &c : E().conns)
        if (c.used && !c.forward && !c.far && within(c.sender, root)) c.far = true;
}

// Whether the object or one of its children has a qtc_watch (an event filter that lives in the
// GUI thread, which Qt refuses across threads). GUI thread.
bool events_watched_below(QObject *o) {
    Events &ev = E();
    if (ev.watches_by_obj.contains(o)) return true;
    const QList<QObject *> kids = o->findChildren<QObject *>();
    for (QObject *k : kids)
        if (ev.watches_by_obj.contains(k)) return true;
    return false;
}

static void fill_payload(QueuedEvent &qe) {
    int64_t *is[4] = {&qe.e.i0, &qe.e.i1, &qe.e.i2, &qe.e.i3};
    double *ds[2] = {&qe.e.d0, &qe.e.d1};
    int ii = 0, di = 0;
    bool have_text = false;
    for (const QVariant &v : qe.args) {
        switch (variant_kind(v)) {
        case QTC_T_BOOL: case QTC_T_INT: if (ii < 4) *is[ii++] = v.toLongLong(); break;
        case QTC_T_UINT: if (ii < 4) *is[ii++] = int64_t(v.toULongLong()); break;
        case QTC_T_OBJ: if (ii < 4) *is[ii++] = int64_t(v.value<ObjRef>().handle); break;
        case QTC_T_DOUBLE: if (di < 2) *ds[di++] = v.toDouble(); break;
        case QTC_T_STR:
            if (!have_text) { qe.text = v.toString().toUtf8(); have_text = true; }
            break;
        default: break;
        }
    }
}

static bool try_coalesce(Connection &c, QueuedEvent &qe) {
    Events &ev = E();
    std::lock_guard<std::mutex> lk(ev.mu);
    if (!c.pending_seq || ev.q.empty()) return false;
    uint64_t front = ev.q.front().e.seq;
    if (c.pending_seq < front) return false;
    size_t idx = size_t(c.pending_seq - front);
    if (idx >= ev.q.size() || ev.q[idx].e.seq != c.pending_seq) return false;
    QueuedEvent &t = ev.q[idx];
    int32_t count = t.e.count + 1;
    uint64_t seq = t.e.seq;
    t.args = std::move(qe.args);
    for (int i = 0; i < qe.nsargs; i++) t.sargs[i] = qe.sargs[i];
    t.nsargs = qe.nsargs;
    t.text = std::move(qe.text);
    qtc_event e = qe.e;
    e.seq = seq;
    e.count = count;
    t.e = e;
    return true;
}

// ------------------------------------------------------------ connections

static uint32_t alloc_conn() {
    Events &ev = E();
    if (!ev.conn_free.empty()) {
        uint32_t i = ev.conn_free.back();
        ev.conn_free.pop_back();
        return i;
    }
    ev.conns.emplace_back();
    return uint32_t(ev.conns.size() - 1);
}

// A disconnected slot is reused only after the event loop has drained the
// queued calls that may still name it.
static void release_conn(uint32_t slot) {
    Events &ev = E();
    Connection &c = ev.conns[slot];
    if (!c.used) return;
    QObject::disconnect(c.qconn);
    for (QObject *o : {c.sender, c.receiver}) {
        if (!o) continue;
        auto it = ev.conns_by_obj.find(o);
        if (it != ev.conns_by_obj.end()) {
            it->removeAll(slot);
            if (it->isEmpty()) ev.conns_by_obj.erase(it);
        }
    }
    c.used = false;
    c.far = false;
    c.sender = c.receiver = nullptr;
    c.signal = QMetaMethod();
    c.qconn = QMetaObject::Connection();
    c.gen++;
    c.batch = ++ev.batch;
    ev.quarantine.push_back(slot);
    std::lock_guard<std::mutex> lk(ev.mu);
    if (ev.spy) {
        QEvent *re = new QEvent(QEvent::Type(kReleaseEvent));
        QCoreApplication::postEvent(ev.spy, re, Qt::LowEventPriority);
    }
}

static void release_quarantine(uint64_t upto) {
    Events &ev = E();
    std::vector<uint32_t> keep;
    for (uint32_t s : ev.quarantine) {
        if (ev.conns[s].batch <= upto && ev.conns[s].gen < 0xfffffff0u) ev.conn_free.push_back(s);
        else if (ev.conns[s].batch > upto) keep.push_back(s);
    }
    ev.quarantine.swap(keep);
}

// A signal argument as a call argument, the way qtc_arg_* would push it.
static void arg_from_signal(Arg &x, QMetaType t, void *p) {
    x.reset();
    if (!t.isValid() || !p) { x.kind = AK_NULL; return; }
    if (t.flags() & QMetaType::PointerToQObject) {
        QObject *o = *reinterpret_cast<QObject **>(p);
        if (!o) { x.kind = AK_NULL; return; }
        x.kind = AK_OBJ;
        x.h = handle_for_signal_arg(o);
        return;
    }
    switch (t.id()) {
    case QMetaType::Bool: x.kind = AK_BOOL; x.b = *static_cast<const bool *>(p); return;
    case QMetaType::Char: x.kind = AK_INT; x.i = *static_cast<const char *>(p); return;
    case QMetaType::SChar: x.kind = AK_INT; x.i = *static_cast<const signed char *>(p); return;
    case QMetaType::UChar: x.kind = AK_UINT; x.u = *static_cast<const unsigned char *>(p); return;
    case QMetaType::Short: x.kind = AK_INT; x.i = *static_cast<const short *>(p); return;
    case QMetaType::UShort: x.kind = AK_UINT; x.u = *static_cast<const unsigned short *>(p); return;
    case QMetaType::Int: x.kind = AK_INT; x.i = *static_cast<const int *>(p); return;
    case QMetaType::UInt: x.kind = AK_UINT; x.u = *static_cast<const unsigned int *>(p); return;
    case QMetaType::Long: x.kind = AK_INT; x.i = *static_cast<const long *>(p); return;
    case QMetaType::ULong: x.kind = AK_UINT; x.u = *static_cast<const unsigned long *>(p); return;
    case QMetaType::LongLong: x.kind = AK_INT; x.i = *static_cast<const qlonglong *>(p); return;
    case QMetaType::ULongLong: x.kind = AK_UINT; x.u = *static_cast<const qulonglong *>(p); return;
    case QMetaType::Float: x.kind = AK_DOUBLE; x.d = *static_cast<const float *>(p); return;
    case QMetaType::Double: x.kind = AK_DOUBLE; x.d = *static_cast<const double *>(p); return;
    case QMetaType::QString: x.kind = AK_STR; x.s = *static_cast<const QString *>(p); return;
    case QMetaType::QByteArray: x.kind = AK_BYTES; x.bytes = *static_cast<const QByteArray *>(p); return;
    default: break;
    }
    QVariant v = t == QMetaType::fromType<QVariant>() ? *static_cast<const QVariant *>(p) : QVariant(t, p);
    if (t.flags() & QMetaType::IsEnumeration) {
        x.kind = AK_INT;
        x.i = v.toLongLong();
        return;
    }
    x.kind = AK_VAL;
    x.v = sanitize(v);
}

// A qtc_connect_to forward: the receiver's method is called the way qtc_call calls it, with the
// checks qtc makes (preconditions, ownership, the methods qtc leaves out), inside the emission
// as a direct Qt connection would. Forwards that set each other off stop at a depth limit
// instead of overflowing the stack.
static int g_forward_depth = 0;
constexpr int kMaxForwardDepth = 32;

static void forward_signal(uint32_t slot, void **a) {
    Events &ev = E();
    Connection &c = ev.conns[slot];
    qtc_status gone = 0;
    if (!c.receiver || !reg().object(c.receiver_handle, &gone)) return;   // destroyed, or being destroyed (qtc_destroy)
    if (g_forward_depth >= kMaxForwardDepth) {
        fail(QTC_ERR_LIMIT, QStringLiteral("qtc_connect_to: %1 -> %2 nested more than %3 forwards deep (forwards that set each other off?); this one was dropped")
                                .arg(QString::fromLatin1(c.signature), QString::fromLatin1(c.method)).arg(kMaxForwardDepth));
        return;
    }
    const uint64_t receiver = c.receiver_handle;
    const QByteArray method = c.method;   // copies: the call may release the connection
    const int n = c.forward_n;
    Stash keep;   // the program's pending arguments and last result stay as they were
    ArgStack &g = args();
    for (int i = 0; i < n; i++) arg_from_signal(g.a[i], c.signal.parameterMetaType(i), a[i + 1]);
    g.n = n;
    g_forward_depth++;
    do_call(receiver, qtc_sn(method.constData(), method.size()));
    g_forward_depth--;
}

// A signal argument of a plain number type: its payload slot read directly (the commonest case,
// valueChanged(int), toggled(bool), ..., with no conversion through QVariant). 1 integer, 2
// floating point, 0 any other type.
static int scalar_arg(QMetaType t, const void *p, int64_t *i, double *d) {
    switch (t.id()) {
    case QMetaType::Bool: *i = *static_cast<const bool *>(p); return 1;
    case QMetaType::Int: *i = *static_cast<const int *>(p); return 1;
    case QMetaType::UInt: *i = *static_cast<const unsigned *>(p); return 1;
    case QMetaType::LongLong: *i = *static_cast<const qlonglong *>(p); return 1;
    case QMetaType::Long: *i = *static_cast<const long *>(p); return 1;
    case QMetaType::Short: *i = *static_cast<const short *>(p); return 1;
    case QMetaType::UShort: *i = *static_cast<const unsigned short *>(p); return 1;
    case QMetaType::Double: *d = *static_cast<const double *>(p); return 2;
    case QMetaType::Float: *d = *static_cast<const float *>(p); return 2;
    default: return 0;
    }
}

// whether every argument of the emission is a scalar scalar_arg reads
static bool scalar_signal(const QMetaMethod &m, void **a) {
    int64_t i;
    double d;
    for (int k = 0; k < m.parameterCount(); k++) {
        const QMetaType t = m.parameterMetaType(k);
        if (!t.isValid() || !a[k + 1]) return false;
        switch (t.id()) {
        case QMetaType::Bool: case QMetaType::Int: case QMetaType::UInt: case QMetaType::LongLong: case QMetaType::Long:
        case QMetaType::Short: case QMetaType::UShort: case QMetaType::Double: case QMetaType::Float: break;
        default: return false;
        }
    }
    (void)i; (void)d;
    return true;
}

// the QVariants of kept scalar arguments, as the general path would have made them
static void materialize_args(QueuedEvent &qe) {
    if (qe.nsargs <= 0) return;
    QList<QVariant> v;
    for (int k = 0; k < qe.nsargs; k++) {
        const ScalarArg &x = qe.sargs[k];
        const QMetaType t(x.type);
        switch (x.type) {
        case QMetaType::Bool: { bool b = x.i != 0; v.append(QVariant(t, &b)); break; }
        case QMetaType::Int: { int y = int(x.i); v.append(QVariant(t, &y)); break; }
        case QMetaType::UInt: { unsigned y = unsigned(x.i); v.append(QVariant(t, &y)); break; }
        case QMetaType::LongLong: { qlonglong y = x.i; v.append(QVariant(t, &y)); break; }
        case QMetaType::Long: { long y = long(x.i); v.append(QVariant(t, &y)); break; }
        case QMetaType::Short: { short y = short(x.i); v.append(QVariant(t, &y)); break; }
        case QMetaType::UShort: { unsigned short y = static_cast<unsigned short>(x.i); v.append(QVariant(t, &y)); break; }
        case QMetaType::Double: { double y = x.d; v.append(QVariant(t, &y)); break; }
        case QMetaType::Float: { float y = float(x.d); v.append(QVariant(t, &y)); break; }
        default: v.append(QVariant()); break;
        }
    }
    qe.args = std::move(v);
    qe.nsargs = 0;
}

static void on_signal(uint32_t slot, void **a) {
    Events &ev = E();
    if (slot >= ev.conns.size()) return;
    Connection &c = ev.conns[slot];
    if (!c.used) return;
    if (c.forward) {
        forward_signal(slot, a);
        return;
    }
    QueuedEvent qe;
    qe.e.kind = c.kind;
    qe.e.tag = c.tag;
    qe.e.source = c.sender_handle;
    qe.e.id = make_handle(slot, c.gen);
    qe.conn_slot = slot;
    qe.signal = c.signature;
    const int n = c.signal.parameterCount();
    qe.e.argc = (c.kind == QTC_EV_SIGNAL && !(c.flags & QTC_CONN_NO_ARGS)) ? n : 0;
    if (c.kind == QTC_EV_SIGNAL && !(c.flags & QTC_CONN_NO_ARGS) && n > 0 && n <= 4 && scalar_signal(c.signal, a)) {
        // only scalars: kept as values, the payload fields filled in order
        int64_t *is[4] = {&qe.e.i0, &qe.e.i1, &qe.e.i2, &qe.e.i3};
        double *ds[2] = {&qe.e.d0, &qe.e.d1};
        int ii = 0, di = 0;
        for (int i = 0; i < n; i++) {
            const QMetaType t = c.signal.parameterMetaType(i);
            ScalarArg &x = qe.sargs[i];
            x.type = t.id();
            const int sk = scalar_arg(t, a[i + 1], &x.i, &x.d);
            if (sk == 1 && ii < 4) *is[ii++] = x.i;
            if (sk == 2 && di < 2) *ds[di++] = x.d;
        }
        qe.nsargs = n;
    } else if (c.kind == QTC_EV_SIGNAL && !(c.flags & QTC_CONN_NO_ARGS)) {
        qe.args.reserve(n);
        bool all_scalar = true;
        int64_t *is[4] = {&qe.e.i0, &qe.e.i1, &qe.e.i2, &qe.e.i3};
        double *ds[2] = {&qe.e.d0, &qe.e.d1};
        int ii = 0, di = 0;
        for (int i = 0; i < n; i++) {
            QMetaType t = c.signal.parameterMetaType(i);
            if (!t.isValid() || !a[i + 1]) {
                qe.args.append(QVariant());
                continue;
            }
            int64_t iv = 0;
            double dv = 0;
            if (const int sk = all_scalar ? scalar_arg(t, a[i + 1], &iv, &dv) : 0) {   // nothing to sanitize
                qe.args.append(QVariant(t, a[i + 1]));
                if (sk == 1 && ii < 4) *is[ii++] = iv;
                if (sk == 2 && di < 2) *ds[di++] = dv;
                continue;
            }
            all_scalar = false;
            // From another thread an object argument may be gone by now (or be another object at
            // the same address): only objects qtc knows and that still live get their handles there.
            if (t.flags() & QMetaType::PointerToQObject) {
                QObject *o = *reinterpret_cast<QObject **>(a[i + 1]);
                uint64_t h = c.destroyed_signal ? c.sender_handle : c.far ? reg().known_handle(o) : handle_for_signal_arg(o);
                qe.args.append(QVariant::fromValue(ObjRef{h}));
            } else if (t == QMetaType::fromType<QVariant>()) {
                const QVariant &v = *reinterpret_cast<const QVariant *>(a[i + 1]);   // not a variant inside a variant
                qe.args.append(c.far ? sanitize_known(v) : sanitize(v));
            } else {
                qe.args.append(c.far ? sanitize_known(QVariant(t, a[i + 1])) : sanitize(QVariant(t, a[i + 1])));
            }
        }
        if (!all_scalar) {
            qe.e.i0 = qe.e.i1 = qe.e.i2 = qe.e.i3 = 0;
            qe.e.d0 = qe.e.d1 = 0;
            fill_payload(qe);
        }
    }
    if (c.flags & QTC_CONN_COALESCE) {
        if (try_coalesce(c, qe)) return;
    }
    uint64_t seq = enqueue(std::move(qe), true);
    if (seq && (c.flags & QTC_CONN_COALESCE)) c.pending_seq = seq;
    if (c.flags & QTC_CONN_ONCE) release_conn(slot);
}

int Spy::qt_metacall(QMetaObject::Call call, int id, void **a) {
    id = QObject::qt_metacall(call, id, a);
    if (id < 0 || call != QMetaObject::InvokeMetaMethod) return id;
    on_signal(uint32_t(id), a);
    return -1;
}

bool Spy::event(QEvent *e) {
    if (e->type() == kWakeEvent) {
        wake(true);
        return true;
    }
    if (e->type() == kReleaseEvent) {
        release_quarantine(E().batch);
        return true;
    }
    return QObject::event(e);
}

static int find_signal(QObject *o, const QByteArray &spec, QString *err) {
    const QMetaObject *mo = o->metaObject();
    QByteArray name = spec.trimmed();
    if (name.contains('(')) {
        QByteArray norm = QMetaObject::normalizedSignature(name.constData());
        int idx = mo->indexOfSignal(norm.constData());
        if (idx < 0) *err = QStringLiteral("%1 has no signal %2").arg(QLatin1StringView(mo->className()), QString::fromLatin1(norm));
        return idx;
    }
    QList<int> found;
    for (int i = 0; i < mo->methodCount(); i++) {
        QMetaMethod m = mo->method(i);
        if (m.methodType() != QMetaMethod::Signal || m.name() != name) continue;
        if (m.attributes() & QMetaMethod::Cloned) continue;
        found.append(i);
    }
    if (found.isEmpty()) {
        *err = QStringLiteral("%1 has no signal named %2").arg(QLatin1StringView(mo->className()), QString::fromLatin1(name));
        return -1;
    }
    if (found.size() > 1) {
        QStringList sigs;
        for (int i : found) sigs << QString::fromLatin1(mo->method(i).methodSignature());
        *err = QStringLiteral("several signals are named %1: %2; pass a signature").arg(QString::fromLatin1(name), sigs.join(QStringLiteral(", ")));
        return -2;
    }
    return found.first();
}

static uint64_t make_connection(QObject *o, uint64_t sender, int sidx, uint64_t tag, uint32_t flags, int32_t kind) {
    Events &ev = E();
    uint32_t slot = alloc_conn();
    Connection &c = ev.conns[slot];
    c.used = true;
    c.forward = false;
    c.far = o->thread() != QThread::currentThread();
    c.sender = o;
    c.receiver = nullptr;
    c.sender_handle = sender;
    c.signal = o->metaObject()->method(sidx);
    c.signature = c.signal.methodSignature();
    c.destroyed_signal = (c.signature == "destroyed(QObject*)" || c.signature == "destroyed()");
    c.tag = tag;
    c.flags = flags;
    c.kind = kind;
    c.pending_seq = 0;
    c.qconn = QMetaObject::connect(o, sidx, ev.spy, QObject::staticMetaObject.methodCount() + int(slot), Qt::AutoConnection, nullptr);
    if (!c.qconn) {
        c.used = false;
        ev.conn_free.push_back(slot);
        fail(QTC_ERR_REJECTED, QStringLiteral("Qt refused the connection to %1").arg(QString::fromLatin1(c.signature)));
        return 0;
    }
    ev.conns_by_obj[o].append(slot);
    return make_handle(slot, c.gen);
}

static Connection *conn_of(uint64_t id, qtc_status *err) {
    Events &ev = E();
    if (!id) { *err = QTC_ERR_NULL_HANDLE; return nullptr; }
    uint32_t slot = handle_index(id);
    if (slot >= ev.conns.size()) { *err = QTC_ERR_BAD_HANDLE; return nullptr; }
    Connection &c = ev.conns[slot];
    if (c.gen != handle_gen(id)) { *err = handle_gen(id) < c.gen ? QTC_ERR_STALE_HANDLE : QTC_ERR_BAD_HANDLE; return nullptr; }
    if (!c.used) { *err = QTC_ERR_STALE_HANDLE; return nullptr; }
    *err = 0;
    return &c;
}

// ---------------------------------------------------------------- watches

#ifdef QTC_HAS_GUI
static int kind_of_event(QEvent::Type t) {
    switch (t) {
    case QEvent::MouseButtonPress: return QTC_EV_MOUSE_PRESS;
    case QEvent::MouseButtonRelease: return QTC_EV_MOUSE_RELEASE;
    case QEvent::MouseButtonDblClick: return QTC_EV_MOUSE_DOUBLE_CLICK;
    case QEvent::MouseMove: return QTC_EV_MOUSE_MOVE;
    case QEvent::Wheel: return QTC_EV_WHEEL;
    case QEvent::KeyPress: return QTC_EV_KEY_PRESS;
    case QEvent::KeyRelease: return QTC_EV_KEY_RELEASE;
    case QEvent::FocusIn: return QTC_EV_FOCUS_IN;
    case QEvent::FocusOut: return QTC_EV_FOCUS_OUT;
    case QEvent::Enter: return QTC_EV_ENTER;
    case QEvent::Leave: return QTC_EV_LEAVE;
    case QEvent::Resize: return QTC_EV_RESIZE;
    case QEvent::Move: return QTC_EV_MOVE;
    case QEvent::Show: return QTC_EV_SHOW;
    case QEvent::Hide: return QTC_EV_HIDE;
    case QEvent::Close: return QTC_EV_CLOSE;
    case QEvent::ContextMenu: return QTC_EV_CONTEXT_MENU;
    case QEvent::DragEnter: return QTC_EV_DRAG_ENTER;
    case QEvent::DragMove: return QTC_EV_DRAG_MOVE;
    case QEvent::DragLeave: return QTC_EV_DRAG_LEAVE;
    case QEvent::Drop: return QTC_EV_DROP;
    case QEvent::Paint: return QTC_EV_PAINT;
    case QEvent::WindowStateChange: return QTC_EV_WINDOW_STATE;
    default: return 0;
    }
}

static void fill_from_qevent(QueuedEvent &qe, int kind, QEvent *e, QObject *o) {
    switch (kind) {
    case QTC_EV_MOUSE_PRESS: case QTC_EV_MOUSE_RELEASE: case QTC_EV_MOUSE_DOUBLE_CLICK: case QTC_EV_MOUSE_MOVE: {
        auto *me = static_cast<QMouseEvent *>(e);
        qe.e.i0 = int64_t(me->button());
        qe.e.i1 = int64_t(me->buttons().toInt());
        qe.e.i2 = int64_t(me->modifiers().toInt());
        qe.e.d0 = me->position().x();
        qe.e.d1 = me->position().y();
        break;
    }
    case QTC_EV_WHEEL: {
        auto *we = static_cast<QWheelEvent *>(e);
        qe.e.i0 = we->angleDelta().y();
        qe.e.i1 = we->angleDelta().x();
        qe.e.i2 = int64_t(we->modifiers().toInt());
        qe.e.d0 = we->position().x();
        qe.e.d1 = we->position().y();
        break;
    }
    case QTC_EV_KEY_PRESS: case QTC_EV_KEY_RELEASE: {
        auto *ke = static_cast<QKeyEvent *>(e);
        qe.e.i0 = ke->key();
        qe.e.i1 = int64_t(ke->modifiers().toInt());
        qe.e.i2 = ke->isAutoRepeat() ? 1 : 0;
        qe.e.i3 = int64_t(ke->nativeScanCode());
        qe.text = ke->text().toUtf8();
        break;
    }
    case QTC_EV_FOCUS_IN: case QTC_EV_FOCUS_OUT:
        qe.e.i0 = int64_t(static_cast<QFocusEvent *>(e)->reason());
        break;
    case QTC_EV_RESIZE: {
        auto *re = static_cast<QResizeEvent *>(e);
        qe.e.i0 = re->size().width();
        qe.e.i1 = re->size().height();
        qe.e.i2 = re->oldSize().width();
        qe.e.i3 = re->oldSize().height();
        break;
    }
    case QTC_EV_MOVE: {
        auto *me = static_cast<QMoveEvent *>(e);
        qe.e.i0 = me->pos().x();
        qe.e.i1 = me->pos().y();
        qe.e.i2 = me->oldPos().x();
        qe.e.i3 = me->oldPos().y();
        break;
    }
    case QTC_EV_CONTEXT_MENU: {
        auto *ce = static_cast<QContextMenuEvent *>(e);
        qe.e.i0 = int64_t(ce->reason());
        qe.e.d0 = ce->pos().x();
        qe.e.d1 = ce->pos().y();
        break;
    }
    case QTC_EV_DRAG_ENTER: case QTC_EV_DRAG_MOVE: case QTC_EV_DROP: {
        auto *de = static_cast<QDropEvent *>(e);
        qe.e.i0 = int64_t(de->proposedAction());
        qe.e.d0 = de->position().x();
        qe.e.d1 = de->position().y();
        if (const QMimeData *md = de->mimeData()) {
            if (md->hasUrls()) {
                QStringList l;
                for (const QUrl &u : md->urls()) l << u.toString();
                qe.text = l.join(QLatin1Char('\n')).toUtf8();
            } else if (md->hasText()) {
                qe.text = md->text().toUtf8();
            }
        }
        break;
    }
    case QTC_EV_WINDOW_STATE: {
        auto *we = static_cast<QWindowStateChangeEvent *>(e);
        qe.e.i1 = int64_t(we->oldState().toInt());
#ifdef QTC_HAS_WIDGETS
        if (o->isWidgetType()) qe.e.i0 = int64_t(static_cast<QWidget *>(o)->windowState().toInt());
#endif
        if (o->isWindowType()) qe.e.i0 = int64_t(static_cast<QWindow *>(o)->windowStates().toInt());
        break;
    }
    default: break;
    }
}
#endif

// An event an override receives (qtc_ov_arg_event), described like a watched one: the kind (0
// for events watches do not deliver), the payload and the text. GUI thread or not.
void events_describe(QEvent *e, QObject *o, qtc_event *out, QByteArray *text) {
#ifdef QTC_HAS_GUI
    QueuedEvent qe;
    const int kind = kind_of_event(e->type());
    qe.e.kind = kind;
    if (kind) fill_from_qevent(qe, kind, e, o);
    *out = qe.e;
    out->kind = kind;
    out->flags = 0;
    out->count = 1;
    out->i3 = kind ? qe.e.i3 : 0;
    if (!kind) out->i0 = int64_t(e->type());   // the QEvent::Type of anything else
    *text = qe.text;
#else
    std::memset(out, 0, sizeof *out);
    out->i0 = int64_t(e->type());
    out->count = 1;
    text->clear();
    (void)o;
#endif
}

static bool consumable(int kind) {
    switch (kind) {
    case QTC_EV_MOUSE_PRESS: case QTC_EV_MOUSE_RELEASE: case QTC_EV_MOUSE_DOUBLE_CLICK: case QTC_EV_MOUSE_MOVE:
    case QTC_EV_WHEEL: case QTC_EV_KEY_PRESS: case QTC_EV_KEY_RELEASE: case QTC_EV_CLOSE: case QTC_EV_CONTEXT_MENU:
    case QTC_EV_DRAG_ENTER: case QTC_EV_DRAG_MOVE: case QTC_EV_DROP: return true;
    default: return false;
    }
}

bool Filter::eventFilter(QObject *o, QEvent *e) {
#ifdef QTC_HAS_GUI
    Events &ev = E();
    auto it = ev.watches_by_obj.constFind(o);
    if (it == ev.watches_by_obj.constEnd()) return false;
    int kind = kind_of_event(e->type());
    if (!kind) return false;
    const uint64_t bit = QTC_WATCH(kind);
    bool consume = false, want = false, wants_drop = false;
    const QList<uint32_t> ids = *it;
    for (uint32_t s : ids) {
        const Watch &w = ev.watches[s];
        if (w.mask & bit) want = true;
        if (w.mask & (QTC_WATCH(QTC_EV_DROP) | QTC_WATCH(QTC_EV_DRAG_ENTER) | QTC_WATCH(QTC_EV_DRAG_MOVE))) wants_drop = true;
    }
    if ((kind == QTC_EV_DRAG_ENTER || kind == QTC_EV_DRAG_MOVE) && wants_drop) static_cast<QDragMoveEvent *>(e)->acceptProposedAction();
    if (kind == QTC_EV_DROP && wants_drop) static_cast<QDropEvent *>(e)->acceptProposedAction();
    if (!want) return false;
    QueuedEvent proto;
    proto.e.kind = kind;
    fill_from_qevent(proto, kind, e, o);
    for (uint32_t s : ids) {
        const Watch &w = ev.watches[s];
        if (!(w.mask & bit)) continue;
        QueuedEvent qe = proto;
        qe.e.tag = w.tag;
        qe.e.source = w.obj_handle;
        qe.e.id = make_handle(s, w.gen);
        enqueue(std::move(qe), true);
        if ((w.consume & bit) && consumable(kind)) consume = true;
    }
    if (consume) {
        if (kind == QTC_EV_CLOSE) e->ignore();
        return true;
    }
#else
    Q_UNUSED(o);
    Q_UNUSED(e);
#endif
    return false;
}

// ------------------------------------------------ last-window-closed watch

static void check_last_window() {
    Events &ev = E();
    ev.check_pending = false;
#ifdef QTC_HAS_GUI
    if (!ev.window_shown || !qobject_cast<QGuiApplication *>(QCoreApplication::instance())) return;
    for (QWindow *w : QGuiApplication::topLevelWindows()) {
        if (!w->isVisible() || w->inherits("QWidgetWindow")) continue;
        Qt::WindowType t = w->type();
        if (t == Qt::Popup || t == Qt::ToolTip || t == Qt::SplashScreen || t == Qt::SubWindow) continue;
        if (w->transientParent()) continue;
        return;
    }
#ifdef QTC_HAS_WIDGETS
    if (qobject_cast<QApplication *>(QCoreApplication::instance())) {
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (!w->isVisible() || !w->isWindow() || w->parentWidget()) continue;
            if (!w->testAttribute(Qt::WA_QuitOnClose)) continue;
            Qt::WindowType t = w->windowType();
            if (t == Qt::Popup || t == Qt::ToolTip || t == Qt::SplashScreen || t == Qt::SubWindow) continue;
            return;
        }
    }
#endif
    ev.window_shown = false;
    QueuedEvent qe;
    qe.e.kind = QTC_EV_LAST_WINDOW_CLOSED;
    enqueue(std::move(qe), true);
    if (QGuiApplication::quitOnLastWindowClosed()) {
        ev.quit = true;
        ev.exit_code = 0;
        wake(true);
    }
#endif
}

bool AppFilter::eventFilter(QObject *o, QEvent *e) {
    // sent to an object (and each one inside it) before it moves to another thread: the
    // registry stops reading its slot without the lock (SF_GUI)
    if (e->type() == QEvent::ThreadChange) reg().note_thread_change(o);
#ifdef QTC_HAS_GUI
    const QEvent::Type t = e->type();
    if (t != QEvent::Show && t != QEvent::Close && t != QEvent::Hide) return false;
    bool is_window = o->isWindowType();
#ifdef QTC_HAS_WIDGETS
    if (o->isWidgetType() && static_cast<QWidget *>(o)->isWindow()) is_window = true;
#endif
    if (!is_window) return false;
    Events &ev = E();
    if (t == QEvent::Show) {
        ev.window_shown = true;
        return false;
    }
    if (!ev.check_pending && ev.spy) {
        ev.check_pending = true;
        QMetaObject::invokeMethod(ev.spy, [] { check_last_window(); }, Qt::QueuedConnection);
    }
#else
    Q_UNUSED(o);
    Q_UNUSED(e);
#endif
    return false;
}

// -------------------------------------------------- object destruction hook

// An object qtc_move_to_thread sent away died in its own thread: its connections are released on
// the GUI thread, found by handle (its address may already be reused). It has no watches.
static void forget_far_object(uint64_t handle) {
    Events &ev = E();
    for (uint32_t s = 0; s < ev.conns.size(); s++) {
        Connection &c = ev.conns[s];
        if (c.used && (c.sender_handle == handle || (c.receiver && c.receiver_handle == handle))) release_conn(s);
    }
}

static void object_destroyed(QObject *o, uint64_t handle) {
    if (!gui_thread_ok()) {
        Events &ev = E();
        std::lock_guard<std::mutex> lk(ev.mu);
        if (ev.spy) QMetaObject::invokeMethod(ev.spy, [handle] { forget_far_object(handle); }, Qt::QueuedConnection);
        return;
    }
    Events &ev = E();
    auto wit = ev.watches_by_obj.find(o);
    if (wit != ev.watches_by_obj.end()) {
        const QList<uint32_t> ids = *wit;
        ev.watches_by_obj.erase(wit);
        for (uint32_t s : ids) {
            Watch &w = ev.watches[s];
            if (!w.used) continue;
            if (w.mask & QTC_WATCH(QTC_EV_DESTROYED)) {
                QueuedEvent qe;
                qe.e.kind = QTC_EV_DESTROYED;
                qe.e.tag = w.tag;
                qe.e.source = handle;
                qe.e.id = make_handle(s, w.gen);
                qe.e.flags = QTC_EVF_STALE_SOURCE;
                enqueue(std::move(qe), true);
            }
            w.used = false;
            w.obj = nullptr;
            w.gen++;
            if (w.gen < 0xfffffff0u) ev.watch_free.push_back(s);
        }
    }
    auto cit = ev.conns_by_obj.find(o);
    if (cit != ev.conns_by_obj.end()) {
        const QList<uint32_t> ids = *cit;
        ev.conns_by_obj.erase(cit);
        for (uint32_t s : ids) {
            Connection &c = ev.conns[s];
            if (c.sender == o) c.sender = nullptr;
            if (c.receiver == o) c.receiver = nullptr;
            // Qt is still emitting o's signals (destroyed() among them): release the record afterwards.
            const uint32_t gen = c.gen;
            if (ev.spy)
                QMetaObject::invokeMethod(ev.spy, [s, gen] {
                    Events &e2 = E();
                    if (s < e2.conns.size() && e2.conns[s].used && e2.conns[s].gen == gen) release_conn(s);
                }, Qt::QueuedConnection);
        }
    }
}

// ------------------------------------------------------------------ init

void events_init() {
    Events &ev = E();
    ev.spy = nullptr;
    {
        Spy *spy = new Spy;
        std::lock_guard<std::mutex> lk(ev.mu);
        ev.spy = spy;
        ev.open = true;
        ev.q.clear();
        ev.next_seq = 1;
        ev.dropped = 0;
    }
    ev.filter = new Filter;
    ev.appfilter = new AppFilter;
    ev.quit = false;
    ev.exit_code = 0;
    ev.in_handler = false;
    ev.window_shown = false;
    ev.check_pending = false;
    ev.handler = nullptr;
    ev.handler_user = nullptr;
    QCoreApplication::instance()->installEventFilter(ev.appfilter);
    on_object_destroyed_hook = object_destroyed;
}

void events_shutdown() {
    Events &ev = E();
    on_object_destroyed_hook = nullptr;
    for (uint32_t i = 0; i < ev.conns.size(); i++)
        if (ev.conns[i].used) QObject::disconnect(ev.conns[i].qconn);
    for (auto it = ev.watches_by_obj.begin(); it != ev.watches_by_obj.end(); ++it) it.key()->removeEventFilter(ev.filter);
    if (QCoreApplication::instance()) QCoreApplication::instance()->removeEventFilter(ev.appfilter);
    ev.conns.clear();
    ev.conn_free.clear();
    ev.quarantine.clear();
    ev.watches.clear();
    ev.watch_free.clear();
    ev.watches_by_obj.clear();
    ev.conns_by_obj.clear();
    for (auto &r : ev.recent) r = QueuedEvent();
    Spy *spy = nullptr;
    {
        std::lock_guard<std::mutex> lk(ev.mu);
        ev.open = false;
        ev.q.clear();
        spy = ev.spy;
        ev.spy = nullptr;
    }
    QCoreApplication::removePostedEvents(spy);
    delete spy;
    delete ev.filter;
    ev.filter = nullptr;
    delete ev.appfilter;
    ev.appfilter = nullptr;
    ev.wait_loop = nullptr;
    ev.waiting = false;
}

static void top_level() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    reg().sweep_blocked();   // objects deleted while their signals were blocked
    top_level_point();
}

}  // namespace qtc

using namespace qtc;

extern "C" {

QTC_API int32_t qtc_wait(int64_t timeout_ms) {
    return guarded<int32_t>(QTC_ERR_STATE, [&]() -> int32_t {
        Events &ev = E();
        if (ov_depth() > 0) return fail(QTC_ERR_STATE, "qtc_wait cannot run inside an override handler (Qt is in the middle of a call)");
        if (ev.in_handler) return fail(QTC_ERR_STATE, "qtc_wait cannot be called from inside a qtc_run handler");
        if (ev.waiting) return fail(QTC_ERR_STATE, "qtc_wait is already running");
        auto pending = [&] {
            std::lock_guard<std::mutex> lk(ev.mu);
            return !ev.q.empty();
        };
        top_level();
        roles_epoch++;   // Qt runs code of its own while it processes events
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        top_level();
        if (ev.quit) return QTC_WAIT_QUIT;
        if (pending()) return QTC_WAIT_EVENTS;
        if (timeout_ms == 0) return QTC_WAIT_TIMEOUT;
        QEventLoop loop;
        QTimer timer;
        bool timed_out = false;
        if (timeout_ms > 0) {
            timer.setSingleShot(true);
            timer.setTimerType(Qt::PreciseTimer);
            QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
                timed_out = true;
                loop.exit(2);
            });
            timer.start(int(std::min<int64_t>(timeout_ms, INT32_MAX)));
        }
        ev.wait_loop = &loop;
        ev.waiting = true;
        ev.woken = false;
        int r = 1;
        if (!pending() && !ev.quit) r = loop.exec();
        ev.wait_loop = nullptr;
        ev.waiting = false;
        roles_epoch++;
        timer.stop();
        if (!ev.woken && !timed_out && r != 1 && r != 2) {
            ev.quit = true;   // QCoreApplication::exit() ended the loop
            ev.exit_code = r;
        }
        top_level();
        if (ev.quit) return QTC_WAIT_QUIT;
        if (pending()) return QTC_WAIT_EVENTS;
        return QTC_WAIT_TIMEOUT;
    });
}

QTC_API bool qtc_poll(qtc_event *out) {
    return guarded<bool>(false, [&]() -> bool {
        if (!out) {
            fail(QTC_ERR_INVALID_ARG, "qtc_poll(NULL)");
            return false;
        }
        Events &ev = E();
        QueuedEvent qe;
        {
            std::lock_guard<std::mutex> lk(ev.mu);
            if (ev.q.empty()) return false;
            qe = std::move(ev.q.front());
            ev.q.pop_front();
        }
        if (qe.conn_slot < ev.conns.size()) {
            Connection &c = ev.conns[qe.conn_slot];
            if (c.pending_seq == qe.e.seq) c.pending_seq = 0;
        }
        if (qe.e.kind == QTC_EV_TIMER) qe.e.i0 = qe.e.count;
        if (qe.e.source && !reg().alive(qe.e.source)) qe.e.flags |= QTC_EVF_STALE_SOURCE;
        *out = qe.e;
        ev.recent[size_t(qe.e.seq % kRecent)] = std::move(qe);
        return true;
    });
}

QTC_API int64_t qtc_pending(void) {
    return guarded<int64_t>(0, []() -> int64_t {
        std::lock_guard<std::mutex> lk(E().mu);
        return int64_t(E().q.size());
    });
}

QTC_API qtc_status qtc_process_events(void) {
    return guarded<qtc_status>(QTC_ERR_STATE, []() -> qtc_status {
        if (ov_depth() > 0) return fail(QTC_ERR_STATE, "qtc_process_events cannot run inside an override handler (Qt is in the middle of a call)");
        if (E().waiting) return fail(QTC_ERR_STATE, "qtc_process_events cannot run inside qtc_wait");
        top_level();
        roles_epoch++;
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        top_level();
        return QTC_OK;
    });
}

QTC_API qtc_status qtc_quit(int32_t code) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        E().quit = true;
        E().exit_code = code;
        wake(true);
        return QTC_OK;
    });
}
QTC_API bool qtc_quit_requested(void) { return guarded<bool>(false, [] { return E().quit; }); }
QTC_API int32_t qtc_exit_code(void) { return guarded<int32_t>(0, [] { return E().exit_code; }); }
QTC_API qtc_status qtc_cancel_quit(void) {
    return guarded<qtc_status>(QTC_ERR_STATE, []() -> qtc_status {
        E().quit = false;
        E().exit_code = 0;
        return QTC_OK;
    });
}
QTC_API qtc_status qtc_set_quit_on_last_window_closed(bool on) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
#ifdef QTC_HAS_GUI
        if (qobject_cast<QGuiApplication *>(QCoreApplication::instance())) QGuiApplication::setQuitOnLastWindowClosed(on);
#endif
        Q_UNUSED(on);
        return QTC_OK;
    });
}

QTC_API qtc_status qtc_set_handler(qtc_handler fn, void *user) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        E().handler = fn;
        E().handler_user = user;
        return QTC_OK;
    });
}

QTC_API int32_t qtc_run(void) {
    return guarded<int32_t>(QTC_ERR_STATE, []() -> int32_t {
        Events &ev = E();
        if (ov_depth() > 0) return fail(QTC_ERR_STATE, "qtc_run cannot run inside an override handler");
        if (!ev.handler) return fail(QTC_ERR_STATE, "qtc_run needs a handler: call qtc_set_handler first");
        if (ev.in_handler || ev.waiting) return fail(QTC_ERR_STATE, "qtc_run is already running");
        for (;;) {
            int32_t w = qtc_wait(-1);
            if (w < 0) return w;
            if (w == QTC_WAIT_QUIT) break;
            qtc_event e;
            while (!ev.quit && qtc_poll(&e)) {
                ev.in_handler = true;
                roles_epoch++;   // Qt may have run in between: the handler sees fresh roles
                try {
                    ev.handler(&e, ev.handler_user);
                } catch (...) {
                    fail(QTC_ERR_EXCEPTION, "the handler threw");
                }
                ev.in_handler = false;
            }
            if (ev.quit) break;
        }
        return ev.exit_code;
    });
}

QTC_API qtc_status qtc_post(uint64_t tag, int64_t a, int64_t b) {
    if (!is_inited()) return fail(QTC_ERR_NOT_INITIALIZED, "qtc_init has not been called");
    QueuedEvent qe;
    qe.e.kind = QTC_EV_POST;
    qe.e.tag = tag;
    qe.e.i0 = a;
    qe.e.i1 = b;
    if (!enqueue(std::move(qe), gui_thread_ok())) {
        std::lock_guard<std::mutex> lk(E().mu);
        return E().open ? fail(QTC_ERR_LIMIT, "the event queue is full") : fail(QTC_ERR_NOT_INITIALIZED, "qtc is shutting down");
    }
    return QTC_OK;
}

QTC_API qtc_status qtc_set_queue_limit(int64_t max_events) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        if (max_events < 0) return fail(QTC_ERR_OUT_OF_RANGE, "the limit must be >= 0");
        std::lock_guard<std::mutex> lk(E().mu);
        E().limit = max_events;
        return QTC_OK;
    });
}
QTC_API int64_t qtc_dropped_events(void) {
    return guarded<int64_t>(0, [] {
        std::lock_guard<std::mutex> lk(E().mu);
        return E().dropped;
    });
}

static QueuedEvent *recent_event(uint64_t seq) {
    Events &ev = E();
    QueuedEvent &qe = ev.recent[size_t(seq % kRecent)];
    if (seq == 0 || qe.e.seq != seq) {
        fail(QTC_ERR_STALE_HANDLE, QStringLiteral("event %1 is no longer available (only the last %2 polled events are kept)").arg(seq).arg(kRecent));
        return nullptr;
    }
    return &qe;
}
static const QVariant *event_arg(uint64_t seq, int32_t index) {
    QueuedEvent *qe = recent_event(seq);
    if (!qe) return nullptr;
    materialize_args(*qe);
    if (index < 0 || index >= qe->args.size()) {
        fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("event %1 has %2 captured arguments").arg(seq).arg(qe->args.size()));
        return nullptr;
    }
    return &qe->args[index];
}

QTC_API int32_t qtc_event_arg_kind(uint64_t seq, int32_t index) {
    return guarded<int32_t>(QTC_ERR_STATE, [&]() -> int32_t {
        const QVariant *v = event_arg(seq, index);
        return v ? variant_kind(*v) : last_error_code();
    });
}
QTC_API qtc_status qtc_event_arg_bool(uint64_t seq, int32_t index, bool *out) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        if (!out) return fail(QTC_ERR_INVALID_ARG, "out is NULL");
        const QVariant *v = event_arg(seq, index);
        if (!v) return last_error_code();
        if (!v->canConvert<bool>()) return fail(QTC_ERR_CONVERSION, "not a bool");
        *out = v->toBool();
        return QTC_OK;
    });
}
QTC_API qtc_status qtc_event_arg_int(uint64_t seq, int32_t index, int64_t *out) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        if (!out) return fail(QTC_ERR_INVALID_ARG, "out is NULL");
        const QVariant *v = event_arg(seq, index);
        if (!v) return last_error_code();
        if (v->metaType() == QMetaType::fromType<ObjRef>()) { *out = int64_t(v->value<ObjRef>().handle); return QTC_OK; }
        bool ok = false;
        qlonglong x = v->toLongLong(&ok);
        if (!ok) return fail(QTC_ERR_CONVERSION, "not an integer");
        *out = x;
        return QTC_OK;
    });
}
QTC_API qtc_status qtc_event_arg_double(uint64_t seq, int32_t index, double *out) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        if (!out) return fail(QTC_ERR_INVALID_ARG, "out is NULL");
        const QVariant *v = event_arg(seq, index);
        if (!v) return last_error_code();
        bool ok = false;
        double x = v->toDouble(&ok);
        if (!ok) return fail(QTC_ERR_CONVERSION, "not a number");
        *out = x;
        return QTC_OK;
    });
}
QTC_API qtc_view qtc_event_arg_str(uint64_t seq, int32_t index) {
    return guarded<qtc_view>(qtc_view{"", 0}, [&]() -> qtc_view {
        const QVariant *v = event_arg(seq, index);
        if (!v) return view_of(QByteArray());
        if (v->metaType().id() == QMetaType::QByteArray) return view_of(v->toByteArray());
        if (!v->canConvert<QString>()) { fail(QTC_ERR_CONVERSION, "not a string"); return view_of(QByteArray()); }
        return view_of(v->toString().toUtf8());
    });
}
QTC_API qtc_obj qtc_event_arg_obj(uint64_t seq, int32_t index) {
    return guarded<qtc_obj>(0, [&]() -> qtc_obj {
        const QVariant *v = event_arg(seq, index);
        if (!v) return 0;
        if (v->metaType() != QMetaType::fromType<ObjRef>()) { fail(QTC_ERR_CONVERSION, "not an object"); return 0; }
        return v->value<ObjRef>().handle;
    });
}
QTC_API qtc_val qtc_event_arg_val(uint64_t seq, int32_t index) {
    return guarded<qtc_val>(0, [&]() -> qtc_val {
        const QVariant *v = event_arg(seq, index);
        return v ? reg().new_value(*v) : 0;
    });
}
QTC_API qtc_view qtc_event_text(uint64_t seq) {
    return guarded<qtc_view>(qtc_view{"", 0}, [&]() -> qtc_view {
        QueuedEvent *qe = recent_event(seq);
        Events &ev = E();
        ev.text_store = qe ? qe->text : QByteArray();
        return qtc_view{ev.text_store.constData(), int64_t(ev.text_store.size())};
    });
}
QTC_API qtc_view qtc_event_signal(uint64_t seq) {
    return guarded<qtc_view>(qtc_view{"", 0}, [&]() -> qtc_view {
        QueuedEvent *qe = recent_event(seq);
        return view_of(qe ? qe->signal : QByteArray());
    });
}

QTC_API uint64_t qtc_connect(qtc_obj sender, qtc_str signal, uint64_t tag, uint32_t flags) {
    return guarded<uint64_t>(0, [&]() -> uint64_t {
        qtc_status err = 0;
        // a sender may live in a worker thread (qtc_move_to_thread): its signals reach the queue
        // through Qt's queued connections. The registry lock keeps such a sender from finishing
        // dying while it is connected to.
        std::lock_guard<std::recursive_mutex> lk(reg().mu);
        QObject *o = reg().object(sender, &err);
        if (!o && err == QTC_ERR_WRONG_THREAD) o = reg().object_anywhere(sender, &err);
        if (!o) { fail(err, "qtc_connect: sender"); return 0; }
        if (flags & ~uint32_t(QTC_CONN_COALESCE | QTC_CONN_ONCE | QTC_CONN_NO_ARGS)) { fail(QTC_ERR_INVALID_ARG, "unknown connection flags"); return 0; }
        QByteArray spec;
        if (!read_bytes(signal, spec, "signal")) return 0;
        QString why;
        int idx = find_signal(o, spec, &why);
        if (idx < 0) { fail(idx == -2 ? QTC_ERR_AMBIGUOUS : QTC_ERR_NOT_FOUND, why); return 0; }
        return make_connection(o, sender, idx, tag, flags, QTC_EV_SIGNAL);
    });
}

QTC_API qtc_status qtc_disconnect(uint64_t connection) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        Connection *c = conn_of(connection, &err);
        if (!c) return fail(err, "qtc_disconnect");
        release_conn(handle_index(connection));
        return QTC_OK;
    });
}

QTC_API uint64_t qtc_connect_to(qtc_obj sender, qtc_str signal, qtc_obj receiver, qtc_str method) {
    return guarded<uint64_t>(0, [&]() -> uint64_t {
        qtc_status err = 0;
        QObject *s = reg().object(sender, &err);
        if (!s) { fail(err, "qtc_connect_to: sender"); return 0; }
        QObject *r = reg().object(receiver, &err);
        if (!r) { fail(err, "qtc_connect_to: receiver"); return 0; }
        QByteArray sspec, mspec;
        if (!read_bytes(signal, sspec, "signal") || !read_bytes(method, mspec, "method")) return 0;
        QString why;
        int si = find_signal(s, sspec, &why);
        if (si < 0) { fail(si == -2 ? QTC_ERR_AMBIGUOUS : QTC_ERR_NOT_FOUND, why); return 0; }
        QMetaMethod sm = s->metaObject()->method(si);
        const QMetaObject *rmo = r->metaObject();
        mspec = mspec.trimmed();
        // The receiver's method: a slot or invokable method the signal can call by Qt's rules
        // (taking the most of its arguments), else a bound method whose leading parameter types
        // are the signal's.
        QByteArray target;
        int nargs = -1;
        if (mspec.contains('(')) {
            const QByteArray norm = QMetaObject::normalizedSignature(mspec.constData());
            const int mi = rmo->indexOfMethod(norm.constData());
            if (mi >= 0) {
                if (!QMetaObject::checkConnectArgs(sm, rmo->method(mi))) { fail(QTC_ERR_BAD_ARGS, "the signal and method arguments do not match"); return 0; }
                target = norm;
                nargs = rmo->method(mi).parameterCount();
            } else {
                const QByteArray name = norm.left(norm.indexOf('('));
                int k = -1;
                const QByteArray sig = bound_method_for_signal(r, name, sm.parameterTypes(), &k);
                if (sig == norm) {
                    target = sig;
                    nargs = k;
                }
            }
        } else {
            int best = -1;
            for (int i = 0; i < rmo->methodCount(); i++) {
                QMetaMethod m = rmo->method(i);
                if (m.name() != mspec || m.access() != QMetaMethod::Public || m.methodType() == QMetaMethod::Constructor) continue;
                if (!QMetaObject::checkConnectArgs(sm, m)) continue;
                if (m.parameterCount() > best) {
                    best = m.parameterCount();
                    target = m.methodSignature();
                    nargs = best;
                }
            }
            if (nargs < 0) target = bound_method_for_signal(r, mspec, sm.parameterTypes(), &nargs);
        }
        if (nargs < 0 || target.isEmpty()) {
            fail(QTC_ERR_NOT_FOUND, QStringLiteral("%1 has no method %2 that takes the arguments of %3")
                                        .arg(QLatin1StringView(rmo->className()), QString::fromLatin1(mspec), QString::fromLatin1(sm.methodSignature())));
            return 0;
        }
        if (!method_callable(r, target, &why)) { fail(QTC_ERR_UNSUPPORTED, why); return 0; }
        Events &ev = E();
        uint32_t slot = alloc_conn();
        Connection &c = ev.conns[slot];
        c.used = true;
        c.forward = true;
        c.far = false;   // both live here (object() above), and slots are reused
        c.sender = s;
        c.receiver = r;
        c.receiver_handle = receiver;
        c.method = target;
        c.forward_n = nargs;
        c.sender_handle = sender;
        c.signal = sm;
        c.signature = sm.methodSignature();
        c.destroyed_signal = false;
        c.tag = 0;
        c.flags = 0;
        c.kind = QTC_EV_SIGNAL;
        c.pending_seq = 0;
        // through the spy, which lives in the GUI thread: an emission from another thread is
        // queued to it, so the call always runs where calls may run
        c.qconn = QMetaObject::connect(s, si, ev.spy, QObject::staticMetaObject.methodCount() + int(slot), Qt::AutoConnection, nullptr);
        if (!c.qconn) {
            c.used = false;
            ev.conn_free.push_back(slot);
            fail(QTC_ERR_REJECTED, "Qt refused the connection");
            return 0;
        }
        ev.conns_by_obj[s].append(slot);
        if (r != s) ev.conns_by_obj[r].append(slot);
        return make_handle(slot, c.gen);
    });
}

QTC_API uint64_t qtc_watch(qtc_obj obj, uint64_t mask, uint64_t consume, uint64_t tag) {
    return guarded<uint64_t>(0, [&]() -> uint64_t {
        qtc_status err = 0;
        QObject *o = reg().object(obj, &err);
        if (!o) { fail(err, "qtc_watch"); return 0; }
        const uint64_t all = (QTC_WATCH(QTC_EV_WATCH_LAST) << 1) - 1;
        if (!mask || (mask & ~all)) { fail(QTC_ERR_INVALID_ARG, "the event mask has no valid QTC_WATCH bits"); return 0; }
        if (consume & ~mask) { fail(QTC_ERR_INVALID_ARG, "consume_mask must be a subset of event_mask"); return 0; }
        for (int k = QTC_EV_WATCH_FIRST; k <= QTC_EV_WATCH_LAST; k++)
            if ((consume & QTC_WATCH(k)) && !consumable(k)) {
                fail(QTC_ERR_INVALID_ARG, QStringLiteral("event kind %1 cannot be consumed").arg(k));
                return 0;
            }
        Events &ev = E();
        uint32_t slot;
        if (!ev.watch_free.empty()) {
            slot = ev.watch_free.back();
            ev.watch_free.pop_back();
        } else {
            ev.watches.emplace_back();
            slot = uint32_t(ev.watches.size() - 1);
        }
        Watch &w = ev.watches[slot];
        w.used = true;
        w.obj = o;
        w.obj_handle = obj;
        w.mask = mask;
        w.consume = consume;
        w.tag = tag;
        auto &list = ev.watches_by_obj[o];
        if (list.isEmpty()) o->installEventFilter(ev.filter);
        list.append(slot);
#ifdef QTC_HAS_WIDGETS
        if (o->isWidgetType()) {
            QWidget *wd = static_cast<QWidget *>(o);
            if (mask & (QTC_WATCH(QTC_EV_DROP) | QTC_WATCH(QTC_EV_DRAG_ENTER) | QTC_WATCH(QTC_EV_DRAG_MOVE))) wd->setAcceptDrops(true);
            if (mask & QTC_WATCH(QTC_EV_MOUSE_MOVE)) wd->setMouseTracking(true);
        }
#endif
        return make_handle(slot, w.gen);
    });
}

QTC_API qtc_status qtc_unwatch(uint64_t id) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        Events &ev = E();
        if (!id) return fail(QTC_ERR_NULL_HANDLE, "qtc_unwatch");
        uint32_t slot = handle_index(id);
        if (slot >= ev.watches.size()) return fail(QTC_ERR_BAD_HANDLE, "qtc_unwatch");
        Watch &w = ev.watches[slot];
        if (w.gen != handle_gen(id) || !w.used) return fail(QTC_ERR_STALE_HANDLE, "qtc_unwatch");
        qtc_status lerr = 0;
        if (!reg().object(w.obj_handle, &lerr, true)) {
            // gone (and, if it died without notice, forgotten just now, which released the watch)
            if (w.used && w.gen == handle_gen(id)) {
                ev.watches_by_obj.remove(w.obj);
                w.used = false;
                w.obj = nullptr;
                w.gen++;
                if (w.gen < 0xfffffff0u) ev.watch_free.push_back(slot);
            }
            return QTC_OK;
        }
        auto it = ev.watches_by_obj.find(w.obj);
        if (it != ev.watches_by_obj.end()) {
            it->removeAll(slot);
            if (it->isEmpty()) {
                w.obj->removeEventFilter(ev.filter);
                ev.watches_by_obj.erase(it);
            }
        }
        w.used = false;
        w.obj = nullptr;
        w.gen++;
        if (w.gen < 0xfffffff0u) ev.watch_free.push_back(slot);
        return QTC_OK;
    });
}

QTC_API qtc_obj qtc_timer(int64_t interval_ms, bool single_shot, uint64_t tag) {
    return guarded<qtc_obj>(0, [&]() -> qtc_obj {
        if (interval_ms < 0 || interval_ms > INT32_MAX) { fail(QTC_ERR_OUT_OF_RANGE, "interval_ms must be 0 .. 2147483647"); return 0; }
        QTimer *t = new QTimer;
        t->setInterval(int(interval_ms));
        t->setSingleShot(single_shot);
        uint64_t h = reg().adopt_new(t, true);
        if (!h) { delete t; return 0; }
        int si = t->metaObject()->indexOfSignal("timeout()");
        if (!make_connection(t, h, si, tag, QTC_CONN_COALESCE | QTC_CONN_NO_ARGS, QTC_EV_TIMER)) {
            delete t;
            return 0;
        }
        t->start();
        return h;
    });
}

}  // extern "C"
