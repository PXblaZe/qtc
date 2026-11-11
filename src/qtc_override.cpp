// Overridable objects: registration, dispatch to the program's handler, and the frame API the
// handler uses (qtc_ov_*). See qtc_override.h.
#include "qtc_override.h"
#include "qtc_module.h"
#include "qtc_builtin.h"
#include "qtc_events.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QThread>
#ifdef QTC_HAS_GUI
#include <QtGui/QPainter>
#endif

#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace qtc {

namespace {

constexpr int kMaxOvDepth = 64;

struct OvState {
    QHash<QByteArray, const OvClassDef *> classes;   // written at registration, read-only after
    std::mutex mu;                                    // hosts
    std::unordered_map<const QObject *, OvHost *> hosts;
    std::atomic<int64_t> live{0};
    std::atomic<qtc_override_handler> handler{nullptr};
    std::atomic<void *> user{nullptr};
};
OvState &S() {
    static OvState *s = new OvState;
    return *s;
}

struct Frame {
    OvHost *host = nullptr;
    QObject *self = nullptr;
    const OvMethod *m = nullptr;
    uint64_t tag = 0;
    void **argv = nullptr;
    void *ret = nullptr;
    bool ret_set = false;
    bool gui = false;
    std::vector<QByteArray> keep;    // bytes behind the views handed out during the call
    std::vector<std::pair<void (*)(uint64_t), uint64_t>> at_return;   // run when the handler returns
    Frame *prev = nullptr;
};
thread_local Frame *t_top = nullptr;
thread_local int t_depth = 0;

Frame *frame(const char *what, qtc_status *err) {
    if (!t_top) {
        *err = fail(QTC_ERR_STATE, QStringLiteral("%1: only inside an override handler").arg(QLatin1StringView(what)));
        return nullptr;
    }
    *err = 0;
    return t_top;
}

const OvType *param(Frame *f, int32_t i, const char *what, qtc_status *err) {
    if (i < 0 || i >= f->m->nparams) {
        *err = fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("%1: %2 has %3 parameters").arg(QLatin1StringView(what), QLatin1StringView(f->m->sig)).arg(f->m->nparams));
        return nullptr;
    }
    *err = 0;
    return &f->m->params[i];
}

void warn_once(const char *key, const QString &msg) {
    static std::mutex mu;
    static QSet<QByteArray> said;
    std::lock_guard<std::mutex> lk(mu);
    if (said.contains(key)) return;
    said.insert(key);
    std::fprintf(stderr, "qtc: %s\n", msg.toUtf8().constData());
}

// An integer of `size` bytes at p.
int64_t read_int(const void *p, int size, bool sign) {
    switch (size) {
    case 1: return sign ? int64_t(*static_cast<const int8_t *>(p)) : int64_t(*static_cast<const uint8_t *>(p));
    case 2: return sign ? int64_t(*static_cast<const int16_t *>(p)) : int64_t(*static_cast<const uint16_t *>(p));
    case 4: return sign ? int64_t(*static_cast<const int32_t *>(p)) : int64_t(*static_cast<const uint32_t *>(p));
    default: { int64_t v; std::memcpy(&v, p, 8); return v; }
    }
}
bool fits(int64_t v, int size, bool sign) {
    if (size >= 8) return true;
    const int bits = size * 8;
    if (sign) return v >= -(int64_t(1) << (bits - 1)) && v < (int64_t(1) << (bits - 1));
    return v >= 0 && v < (int64_t(1) << bits);
}
void write_int(void *p, int size, int64_t v) {
    switch (size) {
    case 1: { int8_t x = int8_t(v); std::memcpy(p, &x, 1); break; }
    case 2: { int16_t x = int16_t(v); std::memcpy(p, &x, 2); break; }
    case 4: { int32_t x = int32_t(v); std::memcpy(p, &x, 4); break; }
    default: std::memcpy(p, &v, 8); break;
    }
}

// A readable argument as a variant (a copy).
QVariant arg_variant(const OvType &t, void *p) {
    switch (t.kind) {
    case OT_BOOL: return QVariant(*static_cast<bool *>(p));
    case OT_SINT: case OT_ENUM: return QVariant(qlonglong(read_int(p, t.size, true)));
    case OT_UINT: return QVariant(qulonglong(read_int(p, t.size, false)));
    case OT_FLOAT: return QVariant(double(*static_cast<float *>(p)));
    case OT_DOUBLE: return QVariant(*static_cast<double *>(p));
    case OT_META: case OT_REF: {
        const QMetaType mt = ov_metatype(t);
        if (mt == QMetaType::fromType<QVariant>()) return *static_cast<const QVariant *>(p);
        return QVariant(mt, p);
    }
    default: return QVariant();
    }
}

int kind_of(const OvType &t, void *p) {
    switch (t.kind) {
    case OT_BOOL: return QTC_T_BOOL;
    case OT_SINT: case OT_ENUM: return QTC_T_INT;
    case OT_UINT: return QTC_T_UINT;
    case OT_FLOAT: case OT_DOUBLE: return QTC_T_DOUBLE;
    case OT_META: case OT_REF: return variant_kind(sanitize_known(arg_variant(t, p)));
    case OT_OBJ: return QTC_T_OBJ;
    default: return QTC_T_NONE;
    }
}

// Stores v into a value of type t at p (a return slot, or an out-parameter).
qtc_status store(const OvType &t, void *p, const QVariant &v, const char *what) {
    auto bad = [&](const QString &why) {
        return fail(QTC_ERR_CONVERSION, QStringLiteral("%1: %2 (%3)").arg(QLatin1StringView(what), why, QLatin1StringView(t.name)));
    };
    switch (t.kind) {
    case OT_BOOL:
        if (!v.canConvert<bool>()) return bad(QStringLiteral("not a bool"));
        *static_cast<bool *>(p) = v.toBool();
        return QTC_OK;
    case OT_SINT: case OT_UINT: case OT_ENUM: {
        bool ok = false;
        int64_t x;
        if (v.metaType().id() == QMetaType::ULongLong) {
            const qulonglong u = v.toULongLong(&ok);
            if (u > uint64_t(std::numeric_limits<int64_t>::max()) && !(t.kind == OT_UINT && t.size == 8))
                return fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("%1: %2 does not fit %3").arg(QLatin1StringView(what)).arg(u).arg(QLatin1StringView(t.name)));
            x = int64_t(u);
        } else if (v.metaType().id() == QMetaType::Double) {
            const double d = v.toDouble();
            if (!(d >= -9.2e18 && d <= 9.2e18) || d != double(int64_t(d))) return bad(QStringLiteral("not a whole number"));
            x = int64_t(d);
            ok = true;
        } else {
            x = v.toLongLong(&ok);
        }
        if (!ok) return bad(QStringLiteral("not an integer"));
        const bool sign = t.kind != OT_UINT;
        if (!(t.kind == OT_UINT && t.size == 8) && !fits(x, t.size, sign))
            return fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("%1: %2 does not fit %3").arg(QLatin1StringView(what)).arg(x).arg(QLatin1StringView(t.name)));
        write_int(p, t.size, x);
        return QTC_OK;
    }
    case OT_FLOAT: case OT_DOUBLE: {
        bool ok = false;
        const double d = v.toDouble(&ok);
        if (!ok) return bad(QStringLiteral("not a number"));
        if (t.kind == OT_FLOAT) *static_cast<float *>(p) = float(d);
        else *static_cast<double *>(p) = d;
        return QTC_OK;
    }
    case OT_META: case OT_REF: {
        const QMetaType mt = ov_metatype(t);
        if (mt == QMetaType::fromType<QVariant>()) {
            *static_cast<QVariant *>(p) = v;
            return QTC_OK;
        }
        QVariant x = v;
        if (x.metaType() != mt) {
            if (mt == QMetaType::fromType<QHash<int, QByteArray>>() && v.canConvert<QVariantMap>()) {
                // role names: {"257": "name", ...}
                QHash<int, QByteArray> h;
                const QVariantMap m = v.toMap();
                for (auto it = m.cbegin(); it != m.cend(); ++it) {
                    bool ok = false;
                    const int k = it.key().toInt(&ok);
                    if (!ok) return bad(QStringLiteral("role names need integer keys"));
                    h.insert(k, it.value().toByteArray());
                }
                x = QVariant::fromValue(h);
            } else if (mt == QMetaType::fromType<QModelIndex>() && v.metaType() == QMetaType::fromType<QPersistentModelIndex>()) {
                x = QVariant::fromValue(QModelIndex(v.value<QPersistentModelIndex>()));
            } else if (!x.convert(mt)) {
                return bad(QStringLiteral("a %1 does not convert").arg(QLatin1StringView(v.metaType().name() ? v.metaType().name() : "null")));
            }
        }
        mt.destruct(p);
        mt.construct(p, x.constData());
        return QTC_OK;
    }
    default:
        return fail(QTC_ERR_UNSUPPORTED, QStringLiteral("%1: %2 cannot be set").arg(QLatin1StringView(what), QLatin1StringView(t.name)));
    }
}

// The result as the class's own implementation or the handler left it, checked: indexes of
// another model and negative counts would mislead views into undefined behaviour.
void check_result(Frame &f) {
    if (!f.ret) return;
    if ((f.m->flags & OVF_NONNEG) && (f.m->ret.kind == OT_SINT) && read_int(f.ret, f.m->ret.size, true) < 0)
        write_int(f.ret, f.m->ret.size, 0);
    if ((f.m->flags & OVF_OWN_INDEX) && f.m->ret.kind == OT_META) {
        auto *idx = static_cast<QModelIndex *>(f.ret);
        if (idx->isValid() && idx->model() != qobject_cast<const QAbstractItemModel *>(f.self)) {
            *idx = QModelIndex();
            warn_once(f.m->sig, QStringLiteral("%1 of a %2 returned an index of another model; it became invalid")
                                    .arg(QLatin1StringView(f.m->sig), QLatin1StringView(f.self->metaObject()->className())));
        }
    }
}

qtc_override_handler symbol_handler(const QByteArray &name) {
#if defined(_WIN32)
    HMODULE self = GetModuleHandleW(nullptr);
    return reinterpret_cast<qtc_override_handler>(GetProcAddress(self, name.constData()));
#else
    return reinterpret_cast<qtc_override_handler>(dlsym(RTLD_DEFAULT, name.constData()));
#endif
}

}  // namespace

// ------------------------------------------------------------- hosts

OvHost::OvHost() = default;
OvHost::~OvHost() = default;

void register_overridables(const OvClassDef *defs, int count) {
    for (int i = 0; i < count; i++) S().classes.insert(QByteArray(defs[i].name), &defs[i]);
}

const OvClassDef *find_overridable(const QByteArray &name) {
    if (const OvClassDef *c = S().classes.value(name)) return c;
    if (load_module_of_class(name)) return S().classes.value(name);   // a module library not loaded yet
    return nullptr;
}

void ov_link(const QObject *o, OvHost *h) {
    OvState &s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    s.hosts[o] = h;
    s.live.fetch_add(1);
}

void ov_unlink(const QObject *o) {
    OvState &s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.hosts.erase(o)) s.live.fetch_sub(1);
}

OvHost *ov_host(const QObject *o) {
    OvState &s = S();
    if (!o || s.live.load(std::memory_order_relaxed) == 0) return nullptr;
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.hosts.find(o);
    return it == s.hosts.end() ? nullptr : it->second;
}

const MethodDef *ov_extras(const QObject *o, uint32_t *count) {
    OvHost *h = ov_host(o);
    if (!h || !h->ov_class) {
        *count = 0;
        return nullptr;
    }
    *count = h->ov_class->nextra;
    return h->ov_class->extra;
}

int ov_depth() { return t_depth; }

// ------------------------------------------------------------- dispatch

bool ov_dispatch(OvHost *h, QObject *self, int id, void **argv, void *ret) {
    const OvClassDef *cd = h->ov_class;
    if (!cd || id < 0 || uint32_t(id) >= cd->nmethods) return false;
    return ov_dispatch_method(h, self, &cd->methods[id], h->ov_tags[size_t(id)], argv, ret);
}

bool ov_dispatch_method(OvHost *h, QObject *self, const OvMethod *mp, uint64_t tag, void **argv, void *ret) {
    const OvMethod &m = *mp;
    const bool gui = gui_thread_ok() && is_inited();
    if (!gui && !(m.flags & OVF_ANY_THREAD)) {
        warn_once(m.sig, QStringLiteral("%1::%2 was called from a thread other than the GUI thread: its override "
                                        "did not run there").arg(QLatin1StringView(self->metaObject()->className()), QLatin1StringView(m.sig)));
        return false;
    }
    const qtc_override_handler fn = S().handler.load();
    if (!fn) return false;
    if (t_depth >= kMaxOvDepth) {
        warn_once("depth", QStringLiteral("overrides called each other %1 deep: the innermost ran Qt's own implementation").arg(kMaxOvDepth));
        return false;
    }
    Frame f;
    f.host = h;
    f.self = self;
    f.m = &m;
    f.tag = tag;
    f.argv = argv;
    f.ret = ret;
    f.gui = gui;
    f.prev = t_top;
    qtc_override_call call{};
    call.self = gui ? reg().handle_for(self) : reg().known_handle(self);
    call.tag = f.tag;
    call.method = m.sig;
    call.nargs = m.nparams;
    call.gui_thread = gui;
    int32_t r;
    {
        // the program's pending arguments, its last result and an open drawing are its own
        std::optional<Stash> stash;
        if (gui) {
            stash.emplace();
            draw_push();
        }
        t_top = &f;
        call.depth = ++t_depth;
        r = fn(&call, S().user.load());
        for (size_t i = 0; i < f.at_return.size(); i++) f.at_return[i].first(f.at_return[i].second);
        if (gui) draw_pop();   // a drawing left open ends while the call's painter is still there
        t_top = f.prev;
        t_depth--;
    }
    if (r == QTC_OV_BASE && !f.ret_set) {
        if (m.base) return false;   // the trampoline runs the class's own implementation
        // a pure virtual: the default result stands
    }
    check_result(f);
    return true;
}

bool ov_painting(const QObject *o) {
    return t_top && t_top->gui && (t_top->m->flags & OVF_PAINT) && t_top->self == o;
}

QPainter *ov_painter_arg(int index) {
    if (!t_top || !t_top->gui || index < 0 || index >= t_top->m->nparams) return nullptr;
    if (t_top->m->params[index].kind != OT_PAINTER) return nullptr;
    return *static_cast<QPainter **>(t_top->argv[index]);
}

// ------------------------------------------------------------- model begin*/end*

bool model_idle(const QObject *m) {
    OvHost *h = ov_host(m);
    return !h || h->model_ops.empty();
}
bool model_pending(const QObject *m, int op) {
    OvHost *h = ov_host(m);
    return h && !h->model_ops.empty() && h->model_ops.back() == op;
}
void model_begun(QObject *m, int op) {
    if (OvHost *h = ov_host(m)) h->model_ops.push_back(uint8_t(op));
}
void model_ended(QObject *m, int op) {
    OvHost *h = ov_host(m);
    if (h && !h->model_ops.empty() && h->model_ops.back() == op) h->model_ops.pop_back();
}
bool indexes_of(const QAbstractItemModel *m, const QModelIndexList &l) {
    for (const QModelIndex &i : l)
        if (!index_of(m, i)) return false;
    return true;
}

}  // namespace qtc

using namespace qtc;

// ------------------------------------------------------------- the C API

QTC_API qtc_obj qtc_new_overridable(qtc_str class_name) {
    return guarded<qtc_obj>(0, [&]() -> qtc_obj {
        QByteArray cn;
        if (!read_bytes(class_name, cn, "class_name")) {
            args().clear();
            return 0;
        }
        const OvClassDef *c = find_overridable(cn.trimmed());
        if (!c) {
            args().clear();
            fail(QTC_ERR_NOT_FOUND, QStringLiteral("%1 is not an overridable class (qtc_overridable_classes lists them)").arg(QString::fromUtf8(cn)));
            return 0;
        }
        const qtc_obj h = new_with_ctors(c->name, c->ctors, c->nctors);
        if (!h) return 0;
        qtc_status err = 0;
        QObject *o = reg().object(h, &err);
        OvHost *host = ov_host(o);
        if (!host) {   // cannot happen: the constructors are the QtcOv_ class's
            fail(QTC_ERR_STATE, "qtc_new_overridable: the object has no override table");
            return 0;
        }
        host->ov_class = c;
        host->ov_tags.assign(c->nmethods, 0);
        return h;
    });
}

QTC_API bool qtc_is_overridable(qtc_obj obj) {
    return guarded<bool>(false, [&]() -> bool {
        qtc_status err = 0;
        QObject *o = reg().object(obj, &err);
        return o && ov_host(o);
    });
}

static int find_method(const OvClassDef *c, const QByteArray &spec, QString *why) {
    const bool sig = spec.contains('(');
    const QByteArray norm = sig ? QMetaObject::normalizedSignature(spec.constData()) : spec;
    int found = -1, n = 0;
    for (uint32_t i = 0; i < c->nmethods; i++) {
        const OvMethod &m = c->methods[i];
        if (sig ? norm == QMetaObject::normalizedSignature(m.sig) : spec == m.name) {
            found = int(i);
            n++;
        }
    }
    if (n == 1) return found;
    if (n == 0) *why = QStringLiteral("%1 has no overridable method %2").arg(QLatin1StringView(c->name), QString::fromUtf8(spec));
    else *why = QStringLiteral("%1 has several overridable methods named %2: pass a signature").arg(QLatin1StringView(c->name), QString::fromUtf8(spec));
    return n == 0 ? -1 : -2;
}

QTC_API qtc_status qtc_override(qtc_obj obj, qtc_str method, uint64_t tag) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        QObject *o = reg().object(obj, &err);
        if (!o) return fail(err, "qtc_override");
        OvHost *h = ov_host(o);
        if (!h || !h->ov_class)
            return fail(QTC_ERR_WRONG_TYPE, QStringLiteral("qtc_override: this %1 was not made with qtc_new_overridable").arg(QLatin1StringView(o->metaObject()->className())));
        QByteArray spec;
        if (!read_bytes(method, spec, "method")) return last_error_code();
        QString why;
        const int id = find_method(h->ov_class, spec.trimmed(), &why);
        if (id < 0) return fail(id == -2 ? QTC_ERR_AMBIGUOUS : QTC_ERR_NOT_FOUND, "qtc_override: " + why);
        h->ov_tags[size_t(id)] = tag;
        return QTC_OK;
    });
}

QTC_API qtc_status qtc_set_override_handler(qtc_override_handler handler, void *user) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        if (t_depth > 0) return fail(QTC_ERR_STATE, "qtc_set_override_handler: not from inside an override handler");
        S().user.store(user);
        S().handler.store(handler);
        return QTC_OK;
    });
}

QTC_API qtc_status qtc_set_override_handler_symbol(qtc_str symbol) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        if (t_depth > 0) return fail(QTC_ERR_STATE, "qtc_set_override_handler_symbol: not from inside an override handler");
        QByteArray name;
        if (!read_bytes(symbol, name, "symbol")) return last_error_code();
        if (name.isEmpty() || name.contains('\0')) return fail(QTC_ERR_INVALID_ARG, "qtc_set_override_handler_symbol: an empty name");
        const qtc_override_handler fn = symbol_handler(name);
        if (!fn)
            return fail(QTC_ERR_NOT_FOUND, QStringLiteral("no exported function named %1 (export it from the program: "
                                                          "linked with -rdynamic, or from a shared library)").arg(QString::fromUtf8(name)));
        S().user.store(nullptr);
        S().handler.store(fn);
        return QTC_OK;
    });
}

static QJsonObject type_json(const OvType &t) {
    static const char *const kinds[] = {"void", "bool", "int", "uint", "float", "double", "enum", "value", "object",
                                        "event", "painter", "out", "buffer", "const buffer", "opaque"};
    QJsonObject o;
    o.insert(QStringLiteral("type"), QLatin1StringView(t.name));
    o.insert(QStringLiteral("kind"), QLatin1StringView(t.kind < sizeof(kinds) / sizeof(kinds[0]) ? kinds[t.kind] : "?"));
    return o;
}

QTC_API qtc_view qtc_overridable_methods(qtc_str class_name) {
    return guarded<qtc_view>(qtc_view{"", 0}, [&]() -> qtc_view {
        QByteArray cn;
        if (!read_bytes(class_name, cn, "class_name")) return qtc_view{"", 0};
        const OvClassDef *c = find_overridable(cn.trimmed());
        if (!c) {
            fail(QTC_ERR_NOT_FOUND, QStringLiteral("%1 is not an overridable class").arg(QString::fromUtf8(cn)));
            return qtc_view{"", 0};
        }
        QJsonArray ms;
        for (uint32_t i = 0; i < c->nmethods; i++) {
            const OvMethod &m = c->methods[i];
            QJsonObject j;
            j.insert(QStringLiteral("name"), QLatin1StringView(m.name));
            j.insert(QStringLiteral("sig"), QLatin1StringView(m.sig));
            QJsonArray ps;
            for (int k = 0; k < m.nparams; k++) ps.append(type_json(m.params[k]));
            j.insert(QStringLiteral("params"), ps);
            j.insert(QStringLiteral("returns"), type_json(m.ret));
            j.insert(QStringLiteral("pure"), bool(m.flags & OVF_PURE));
            j.insert(QStringLiteral("thread"), QLatin1StringView((m.flags & OVF_ANY_THREAD) ? "any" : "gui"));
            j.insert(QStringLiteral("paints"), bool(m.flags & OVF_PAINT));
            ms.append(j);
        }
        QJsonArray ex;
        for (uint32_t i = 0; i < c->nextra; i++) ex.append(QLatin1StringView(c->extra[i].sig));
        QJsonObject o;
        o.insert(QStringLiteral("class"), QLatin1StringView(c->name));
        o.insert(QStringLiteral("module"), QLatin1StringView(c->module));
        o.insert(QStringLiteral("methods"), ms);
        o.insert(QStringLiteral("protected"), ex);
        return view_of(QJsonDocument(o).toJson(QJsonDocument::Compact));
    });
}

QTC_API qtc_view qtc_overridable_classes(void) {
    return guarded<qtc_view>(qtc_view{"", 0}, []() -> qtc_view {
        QStringList names;
        for (auto it = S().classes.cbegin(); it != S().classes.cend(); ++it)
            if (app_kind() == QTC_APP_WIDGETS || it.value()->need == NEED_CORE || (it.value()->need == NEED_GUI && app_kind() == QTC_APP_GUI))
                names << QString::fromLatin1(it.key());
        names.sort();
        return view_of(names.join(QLatin1Char('\n')).toUtf8());
    });
}

// ------------------------------------------------------------- inside a handler (any thread)

#define OV_FRAME(what, failv)                         \
    qtc_status err_ = 0;                              \
    Frame *f = frame(what, &err_);                    \
    if (!f) return failv;

QTC_API int32_t qtc_ov_arg_kind(int32_t index) {
    OV_FRAME("qtc_ov_arg_kind", err_)
    const OvType *t = param(f, index, "qtc_ov_arg_kind", &err_);
    if (!t) return err_;
    return kind_of(*t, f->argv[index]);
}

QTC_API qtc_view qtc_ov_arg_type(int32_t index) {
    OV_FRAME("qtc_ov_arg_type", (qtc_view{"", 0}))
    const OvType *t = param(f, index, "qtc_ov_arg_type", &err_);
    if (!t) return qtc_view{"", 0};
    return qtc_view{t->name, int64_t(std::strlen(t->name))};
}

static bool arg_number(int32_t index, const char *what, QVariant *out) {
    qtc_status err = 0;
    Frame *f = frame(what, &err);
    if (!f) return false;
    const OvType *t = param(f, index, what, &err);
    if (!t) return false;
    if (t->kind == OT_OBJ || t->kind == OT_EVENT || t->kind == OT_PAINTER || t->kind == OT_OPAQUE || t->kind == OT_BUF || t->kind == OT_CBUF) {
        fail(QTC_ERR_WRONG_TYPE, QStringLiteral("%1: parameter %2 of %3 is a %4").arg(QLatin1StringView(what)).arg(index).arg(QLatin1StringView(f->m->sig), QLatin1StringView(t->name)));
        return false;
    }
    *out = arg_variant(*t, f->argv[index]);
    return true;
}

QTC_API bool qtc_ov_arg_bool(int32_t index) {
    QVariant v;
    return arg_number(index, "qtc_ov_arg_bool", &v) && v.toBool();
}

QTC_API int64_t qtc_ov_arg_int(int32_t index) {
    QVariant v;
    if (!arg_number(index, "qtc_ov_arg_int", &v)) return 0;
    return v.toLongLong();
}

QTC_API double qtc_ov_arg_double(int32_t index) {
    QVariant v;
    if (!arg_number(index, "qtc_ov_arg_double", &v)) return 0;
    return v.toDouble();
}

QTC_API qtc_view qtc_ov_arg_str(int32_t index) {
    QVariant v;
    if (!arg_number(index, "qtc_ov_arg_str", &v)) return qtc_view{"", 0};
    QByteArray b = v.metaType().id() == QMetaType::QByteArray ? v.toByteArray() : v.toString().toUtf8();
    t_top->keep.push_back(b);
    const QByteArray &k = t_top->keep.back();
    return qtc_view{k.constData(), int64_t(k.size())};
}

QTC_API qtc_obj qtc_ov_arg_obj(int32_t index) {
    OV_FRAME("qtc_ov_arg_obj", 0)
    const OvType *t = param(f, index, "qtc_ov_arg_obj", &err_);
    if (!t) return 0;
    if (t->kind != OT_OBJ) {
        fail(QTC_ERR_WRONG_TYPE, QStringLiteral("qtc_ov_arg_obj: parameter %1 of %2 is a %3").arg(index).arg(QLatin1StringView(f->m->sig), QLatin1StringView(t->name)));
        return 0;
    }
    QObject *o = *static_cast<QObject **>(f->argv[index]);
    return f->gui ? reg().handle_for(o) : reg().known_handle(o);
}

QTC_API qtc_val qtc_ov_arg_val(int32_t index) {
    OV_FRAME("qtc_ov_arg_val", 0)
    if (!f->gui) {
        fail(QTC_ERR_WRONG_THREAD, "qtc_ov_arg_val: values are made on the GUI thread");
        return 0;
    }
    QVariant v;
    if (!arg_number(index, "qtc_ov_arg_val", &v)) return 0;
    return reg().new_value(sanitize(v));
}

QTC_API qtc_status qtc_ov_arg_event(int32_t index, qtc_event *out) {
    OV_FRAME("qtc_ov_arg_event", err_)
    const OvType *t = param(f, index, "qtc_ov_arg_event", &err_);
    if (!t) return err_;
    if (t->kind != OT_EVENT) return fail(QTC_ERR_WRONG_TYPE, QStringLiteral("qtc_ov_arg_event: parameter %1 of %2 is not an event").arg(index).arg(QLatin1StringView(f->m->sig)));
    if (!out) return fail(QTC_ERR_INVALID_ARG, "qtc_ov_arg_event: out is NULL");
    QEvent *e = *static_cast<QEvent **>(f->argv[index]);
    std::memset(out, 0, sizeof *out);
    QByteArray text;
    if (e) events_describe(e, f->self, out, &text);
    out->tag = f->tag;
    out->source = f->gui ? reg().handle_for(f->self) : reg().known_handle(f->self);
    f->keep.push_back(text);
    return QTC_OK;
}

QTC_API qtc_view qtc_ov_event_text(int32_t index) {
    OV_FRAME("qtc_ov_event_text", (qtc_view{"", 0}))
    const OvType *t = param(f, index, "qtc_ov_event_text", &err_);
    if (!t || t->kind != OT_EVENT) return qtc_view{"", 0};
    QEvent *e = *static_cast<QEvent **>(f->argv[index]);
    qtc_event tmp;
    QByteArray text;
    if (e) events_describe(e, f->self, &tmp, &text);
    f->keep.push_back(text);
    const QByteArray &k = f->keep.back();
    return qtc_view{k.constData(), int64_t(k.size())};
}

QTC_API qtc_status qtc_ov_accept_event(int32_t index, bool accepted) {
    OV_FRAME("qtc_ov_accept_event", err_)
    const OvType *t = param(f, index, "qtc_ov_accept_event", &err_);
    if (!t) return err_;
    if (t->kind != OT_EVENT) return fail(QTC_ERR_WRONG_TYPE, "qtc_ov_accept_event: not an event parameter");
    if (QEvent *e = *static_cast<QEvent **>(f->argv[index])) e->setAccepted(accepted);
    return QTC_OK;
}

static qtc_status set_out(int32_t index, const QVariant &v, const char *what) {
    OV_FRAME(what, err_)
    const OvType *t = param(f, index, what, &err_);
    if (!t) return err_;
    if (t->kind != OT_REF)
        return fail(QTC_ERR_READ_ONLY, QStringLiteral("%1: parameter %2 of %3 is not an out-parameter").arg(QLatin1StringView(what)).arg(index).arg(QLatin1StringView(f->m->sig)));
    return store(*t, f->argv[index], v, what);
}

QTC_API qtc_status qtc_ov_set_arg_int(int32_t index, int64_t v) { return set_out(index, QVariant(qlonglong(v)), "qtc_ov_set_arg_int"); }
QTC_API qtc_status qtc_ov_set_arg_str(int32_t index, qtc_str v) {
    QString s;
    if (!read_str(v, s, "value")) return last_error_code();
    return set_out(index, QVariant(s), "qtc_ov_set_arg_str");
}

static int64_t buffer_len(Frame *f, int32_t index) {
    if (index + 1 >= f->m->nparams) return -1;
    const OvType &lt = f->m->params[index + 1];
    if (lt.kind != OT_SINT && lt.kind != OT_UINT) return -1;
    return read_int(f->argv[index + 1], lt.size, lt.kind == OT_SINT);
}

QTC_API int64_t qtc_ov_buffer_write(int32_t index, qtc_str bytes) {
    OV_FRAME("qtc_ov_buffer_write", err_)
    const OvType *t = param(f, index, "qtc_ov_buffer_write", &err_);
    if (!t) return err_;
    if (t->kind != OT_BUF) return fail(QTC_ERR_WRONG_TYPE, "qtc_ov_buffer_write: not a buffer to fill");
    QByteArray b;
    if (!read_bytes(bytes, b, "bytes")) return last_error_code();
    const int64_t cap = buffer_len(f, index);
    char *data = *static_cast<char **>(f->argv[index]);
    if (cap < 0 || !data) return fail(QTC_ERR_STATE, "qtc_ov_buffer_write: the buffer has no length");
    const int64_t n = std::min<int64_t>(cap, b.size());
    if (n > 0) std::memcpy(data, b.constData(), size_t(n));
    return n;
}

QTC_API qtc_view qtc_ov_buffer_read(int32_t index) {
    OV_FRAME("qtc_ov_buffer_read", (qtc_view{"", 0}))
    const OvType *t = param(f, index, "qtc_ov_buffer_read", &err_);
    if (!t) return qtc_view{"", 0};
    if (t->kind != OT_CBUF) {
        fail(QTC_ERR_WRONG_TYPE, "qtc_ov_buffer_read: not a buffer to read");
        return qtc_view{"", 0};
    }
    const int64_t len = buffer_len(f, index);
    const char *data = *static_cast<const char **>(f->argv[index]);
    if (len < 0 || (!data && len > 0)) {
        fail(QTC_ERR_STATE, "qtc_ov_buffer_read: the buffer has no length");
        return qtc_view{"", 0};
    }
    return qtc_view{data ? data : "", len};
}

static qtc_status set_ret(const QVariant &v, const char *what) {
    OV_FRAME(what, err_)
    if (f->m->ret.kind == OT_VOID) return fail(QTC_ERR_STATE, QStringLiteral("%1: %2 returns nothing").arg(QLatin1StringView(what), QLatin1StringView(f->m->sig)));
    if (!f->ret) return QTC_OK;   // the caller does not take the result (a slot called by a signal)
    if (qtc_status e = store(f->m->ret, f->ret, v, what)) return e;
    f->ret_set = true;
    return QTC_OK;
}

QTC_API qtc_status qtc_ov_ret_bool(bool v) { return set_ret(QVariant(v), "qtc_ov_ret_bool"); }
QTC_API qtc_status qtc_ov_ret_int(int64_t v) { return set_ret(QVariant(qlonglong(v)), "qtc_ov_ret_int"); }
QTC_API qtc_status qtc_ov_ret_uint(uint64_t v) { return set_ret(QVariant(qulonglong(v)), "qtc_ov_ret_uint"); }
QTC_API qtc_status qtc_ov_ret_double(double v) { return set_ret(QVariant(v), "qtc_ov_ret_double"); }
QTC_API qtc_status qtc_ov_ret_str(qtc_str v) {
    QString s;
    if (!read_str(v, s, "value")) return last_error_code();
    return set_ret(QVariant(s), "qtc_ov_ret_str");
}
QTC_API qtc_status qtc_ov_ret_bytes(qtc_str v) {
    QByteArray b;
    if (!read_bytes(v, b, "value")) return last_error_code();
    return set_ret(QVariant(b), "qtc_ov_ret_bytes");
}
QTC_API qtc_status qtc_ov_ret_val(qtc_val v) {
    OV_FRAME("qtc_ov_ret_val", err_)
    if (!f->gui) return fail(QTC_ERR_WRONG_THREAD, "qtc_ov_ret_val: values live on the GUI thread");
    qtc_status err = 0;
    QVariant *x = reg().value(v, &err);
    if (!x) return fail(err, "qtc_ov_ret_val");
    if (holds_object(*x)) return fail(QTC_ERR_UNSUPPORTED, "qtc_ov_ret_val: a result cannot hold objects");
    return set_ret(*x, "qtc_ov_ret_val");
}

QTC_API bool qtc_ov_arg_index(int32_t index, int32_t *row, int32_t *column, uint64_t *id) {
    qtc_status err = 0;
    Frame *f = frame("qtc_ov_arg_index", &err);
    if (!f) return false;
    const OvType *t = param(f, index, "qtc_ov_arg_index", &err);
    if (!t) return false;
    if ((t->kind != OT_META && t->kind != OT_REF) || ov_metatype(*t) != QMetaType::fromType<QModelIndex>()) {
        fail(QTC_ERR_WRONG_TYPE, QStringLiteral("qtc_ov_arg_index: parameter %1 of %2 is not a QModelIndex").arg(index).arg(QLatin1StringView(f->m->sig)));
        return false;
    }
    const QModelIndex &i = *static_cast<const QModelIndex *>(f->argv[index]);
    if (row) *row = i.row();
    if (column) *column = i.column();
    if (id) *id = uint64_t(i.internalId());
    return i.isValid();
}

QTC_API qtc_status qtc_ov_ret_index(int32_t row, int32_t column, uint64_t id) {
    OV_FRAME("qtc_ov_ret_index", err_)
    const OvType &t = f->m->ret;
    if (t.kind != OT_META || ov_metatype(t) != QMetaType::fromType<QModelIndex>())
        return fail(QTC_ERR_WRONG_TYPE, QStringLiteral("qtc_ov_ret_index: %1 does not return a QModelIndex").arg(QLatin1StringView(f->m->sig)));
    const OvClassDef *c = f->host->ov_class;
    QModelIndex i;
    if (row >= 0 && column >= 0) {
        if (!c || !c->make_index) return fail(QTC_ERR_WRONG_TYPE, "qtc_ov_ret_index: the object is not a model");
        i = c->make_index(f->self, row, column, quintptr(id));
    }
    *static_cast<QModelIndex *>(f->ret) = i;
    f->ret_set = true;
    return QTC_OK;
}

QTC_API qtc_status qtc_ov_call_base(void) {
    OV_FRAME("qtc_ov_call_base", err_)
    if (!f->m->base) return fail(QTC_ERR_UNSUPPORTED, QStringLiteral("qtc_ov_call_base: %1 has no implementation of its own").arg(QLatin1StringView(f->m->sig)));
    f->m->base(f->self, f->argv, f->ret);
    f->ret_set = true;
    if (f->gui && f->ret) {
        // readable through qtc_ret_* (the handler's own register: the program's is stashed)
        const OvType &t = f->m->ret;
        if (t.kind == OT_META) ret().set_variant(arg_variant(t, f->ret));
        else if (t.kind != OT_VOID && t.kind != OT_OBJ && t.kind != OT_OPAQUE) ret().set_variant(arg_variant(t, f->ret));
    }
    return QTC_OK;
}

QTC_API int32_t qtc_ov_depth(void) { return t_depth; }

// ---------------------------------------------------------------- graphics (qtc_rhi.cpp)

namespace qtc {
// The QRhiCommandBuffer argument of the innermost override call when it is being handled for
// `self` on the GUI thread (QRhiWidget::initialize and render).
QRhiCommandBuffer *ov_rhi_cb_arg(const QObject *self) {
    Frame *f = t_top;
    if (!f || !f->gui || f->self != self || !f->argv) return nullptr;
    for (int i = 0; i < f->m->nparams; i++) {
        const OvType &t = f->m->params[i];
        if (t.kind != OT_OPAQUE || !t.name) continue;
        QByteArray n(t.name);
        n.replace(' ', "");
        if (n == "QRhiCommandBuffer*") return *static_cast<QRhiCommandBuffer **>(f->argv[i]);
    }
    return nullptr;
}

void ov_at_return(void (*fn)(uint64_t), uint64_t arg) {
    if (t_top) t_top->at_return.emplace_back(fn, arg);
}
}  // namespace qtc
