// Runtime classes: QObject subclasses (or subclasses of an overridable Qt class) defined while
// the program runs, with signals, slots, invokables and properties (qtc_class_*).
//
// An object of one is an overridable object (qtc_override.h) whose host points to its class: the
// QtcOv_ class answers metaObject(), qt_metacast() and qt_metacall() with the runtime class's
// meta-object, built with Qt's QMetaObjectBuilder (a private Qt Core API: like the generated
// code, qtc is built for one Qt release). Slots and invokables go to the override handler with
// the tag given at definition, on whatever thread Qt calls them from (an object moved to a worker
// thread runs them there); properties keep their values in the object and emit their notify
// signal when a write changes them.
#include "qtc_override.h"
#include "qtc_events.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMetaMethod>
#include <QtCore/QRegularExpression>
#ifndef QTC_NO_RUNTIME_CLASSES
#include <QtCore/private/qmetaobjectbuilder_p.h>
#endif

#include <deque>
#include <memory>

namespace qtc {

struct DynMethod {
    QByteArray sig, name, ret;     // ret: empty for void
    QList<QByteArray> params;
    bool is_signal = false;
    bool invokable = false;        // Q_INVOKABLE (a method) rather than a slot
    uint64_t tag = 0;
    std::vector<OvType> types;     // the handler's view of the parameters
    OvMethod om{};
};

struct DynProp {
    QByteArray name, type;
    QMetaType meta;
    uint32_t flags = 0;
    int notify = -1;               // local index of the notify signal
    int notify_args = 0;           // how many arguments it takes (0 or 1)
};

struct DynClass {
    QByteArray name;
    const OvClassDef *base = nullptr;
    const QMetaObject *base_meta = nullptr;
    std::deque<DynMethod> sigs, meths;   // local method indices: signals, then the others
    std::deque<DynProp> props;
    QMetaObject *mo = nullptr;
    ClassDef cdef{};
};

namespace {

struct ClassState {
    QHash<QByteArray, DynClass *> classes;   // defined this session
    std::vector<std::unique_ptr<DynClass>> all;   // never freed: objects outliving a session keep their meta-object
    DynClass *building = nullptr;
};
ClassState &CS() {
    static ClassState *s = new ClassState;
    return *s;
}

bool identifier(const QByteArray &n) {
    static const QRegularExpression rx(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    return rx.match(QString::fromLatin1(n)).hasMatch();
}

// A type a runtime class's signature or property may use: one Qt's meta-type system knows (and
// can copy), or QObject* for objects.
bool type_ok(const QByteArray &t, QString *why) {
    if (t == "QObject*") return true;
    if (t.endsWith('*')) {
        *why = QStringLiteral("%1: object parameters are QObject*").arg(QString::fromLatin1(t));
        return false;
    }
    const QMetaType mt = QMetaType::fromName(t);
    if (!mt.isValid() || mt.id() == QMetaType::Void) {
        *why = QStringLiteral("%1 is not a type Qt's meta-type system knows").arg(QString::fromLatin1(t));
        return false;
    }
    if (!(mt.flags() & QMetaType::NeedsCopyConstruction) && !mt.isCopyConstructible()) {
        *why = QStringLiteral("%1 cannot be copied").arg(QString::fromLatin1(t));
        return false;
    }
    return true;
}

OvType ov_type_for(const QByteArray &t) {
    if (t == "QObject*") return OvType{OT_OBJ, 0, nullptr, t.constData(), QMetaType::fromType<QObject *>().iface()};
    return OvType{OT_META, 0, nullptr, t.constData(), QMetaType::fromName(t).iface()};
}

// "name(T1,T2)" -> name and the normalized parameter types; false (with why) when malformed.
bool parse_sig(const QByteArray &spec, QByteArray *sig, QByteArray *name, QList<QByteArray> *params, QString *why) {
    QByteArray s = QMetaObject::normalizedSignature(spec.trimmed().constData());
    const int open = s.indexOf('('), close = s.lastIndexOf(')');
    if (open <= 0 || close != s.size() - 1) {
        *why = QStringLiteral("%1 is not a signature like name(int,QString)").arg(QString::fromUtf8(spec));
        return false;
    }
    *name = s.left(open);
    if (!identifier(*name)) {
        *why = QStringLiteral("%1 is not a valid name").arg(QString::fromUtf8(*name));
        return false;
    }
    const QByteArray inner = s.mid(open + 1, close - open - 1);
    params->clear();
    if (!inner.isEmpty()) {
        int depth = 0;
        QByteArray cur;
        for (char ch : inner) {
            if (ch == '<') depth++;
            else if (ch == '>') depth--;
            if (ch == ',' && depth == 0) {
                params->append(cur);
                cur.clear();
            } else {
                cur.append(ch);
            }
        }
        params->append(cur);
    }
    if (params->size() > kMaxArgs - 1) {
        *why = QStringLiteral("at most %1 parameters").arg(kMaxArgs - 1);
        return false;
    }
    for (const QByteArray &p : *params)
        if (!type_ok(p, why)) return false;
    *sig = s;
    return true;
}

DynClass *open_class(const char *what, qtc_status *err) {
    DynClass *c = CS().building;
    if (!c) *err = fail(QTC_ERR_STATE, QStringLiteral("%1: no class definition is open (qtc_class_begin)").arg(QLatin1StringView(what)));
    else *err = 0;
    return c;
}

bool has_method(const DynClass *c, const QByteArray &sig) {
    for (const DynMethod &m : c->sigs)
        if (m.sig == sig) return true;
    for (const DynMethod &m : c->meths)
        if (m.sig == sig) return true;
    return false;
}

// Is this name a member of the base class already (a signal, method or property)? A runtime
// member of the same name would shadow it confusingly; qtc refuses it.
bool base_has(const DynClass *c, const QByteArray &name) {
    const QMetaObject *mo = c->base_meta;
    for (int i = 0; i < mo->methodCount(); i++)
        if (mo->method(i).name() == name) return true;
    return mo->indexOfProperty(name.constData()) >= 0;
}

void local_call(OvHost *h, QObject *self, QMetaObject::Call call, int id, void **a);

void static_metacall(QObject *o, QMetaObject::Call call, int id, void **a) {
    OvHost *h = ov_host(o);
    if (!h || !h->ov_dyn) return;
    local_call(h, o, call, id, a);
}

void emit_notify(OvHost *h, QObject *self, const DynProp &p, int index) {
    if (p.notify < 0) return;
    QVariant &v = h->dyn_props[size_t(index)];
    void *args[2] = {nullptr, nullptr};
    if (p.notify_args == 1) args[1] = p.meta == QMetaType::fromType<QVariant>() ? static_cast<void *>(&v) : v.data();
    QMetaObject::activate(self, h->ov_dyn->mo, p.notify, args);
}

void local_call(OvHost *h, QObject *self, QMetaObject::Call call, int id, void **a) {
    const DynClass *c = h->ov_dyn;
    const int nsig = int(c->sigs.size()), nm = nsig + int(c->meths.size());
    switch (call) {
    case QMetaObject::InvokeMetaMethod:
        if (id < 0 || id >= nm) return;
        if (id < nsig) {
            QMetaObject::activate(self, c->mo, id, a);
        } else {
            const DynMethod &m = c->meths[size_t(id - nsig)];
            ov_dispatch_method(h, self, &m.om, m.tag, a + 1, a[0]);
        }
        return;
    case QMetaObject::RegisterMethodArgumentMetaType: {
        if (id < 0 || id >= nm) return;
        const DynMethod &m = id < nsig ? c->sigs[size_t(id)] : c->meths[size_t(id - nsig)];
        const int arg = *static_cast<int *>(a[1]);
        *static_cast<QMetaType *>(a[0]) = (arg >= 0 && arg < m.params.size()) ? QMetaType::fromName(m.params[arg]) : QMetaType();
        return;
    }
    case QMetaObject::ReadProperty: {
        if (id < 0 || id >= int(c->props.size())) return;
        const DynProp &p = c->props[size_t(id)];
        const QVariant &v = h->dyn_props[size_t(id)];
        if (p.meta == QMetaType::fromType<QVariant>()) {
            *static_cast<QVariant *>(a[0]) = v;
        } else {
            p.meta.destruct(a[0]);
            p.meta.construct(a[0], v.constData());
        }
        return;
    }
    case QMetaObject::WriteProperty: {
        if (id < 0 || id >= int(c->props.size())) return;
        const DynProp &p = c->props[size_t(id)];
        if (!(p.flags & QTC_PROP_WRITE)) return;
        QVariant nv = p.meta == QMetaType::fromType<QVariant>() ? *static_cast<const QVariant *>(a[0]) : QVariant(p.meta, a[0]);
        QVariant &cur = h->dyn_props[size_t(id)];
        if (cur == nv && cur.metaType() == nv.metaType()) return;
        cur = std::move(nv);
        emit_notify(h, self, p, id);
        return;
    }
    case QMetaObject::ResetProperty: {
        if (id < 0 || id >= int(c->props.size())) return;
        const DynProp &p = c->props[size_t(id)];
        if (!(p.flags & QTC_PROP_WRITE)) return;
        QVariant fresh = p.meta == QMetaType::fromType<QVariant>() ? QVariant() : QVariant(p.meta);
        QVariant &cur = h->dyn_props[size_t(id)];
        if (cur == fresh) return;
        cur = std::move(fresh);
        emit_notify(h, self, p, id);
        return;
    }
    case QMetaObject::RegisterPropertyMetaType:
        if (id >= 0 && id < int(c->props.size())) *static_cast<QMetaType *>(a[0]) = c->props[size_t(id)].meta;
        return;
    default:
        return;
    }
}

}  // namespace

const QMetaObject *dyn_meta(const DynClass *d) { return d->mo; }

bool dyn_is(const DynClass *d, const char *class_name) { return class_name && d->name == class_name; }

int dyn_metacall(OvHost *h, QObject *self, QMetaObject::Call call, int id, void **a) {
    const DynClass *c = h->ov_dyn;
    const int nm = int(c->sigs.size() + c->meths.size()), np = int(c->props.size());
    switch (call) {
    case QMetaObject::InvokeMetaMethod:
    case QMetaObject::RegisterMethodArgumentMetaType:
        if (id < nm) local_call(h, self, call, id, a);
        return id - nm;
    case QMetaObject::ReadProperty:
    case QMetaObject::WriteProperty:
    case QMetaObject::ResetProperty:
    case QMetaObject::RegisterPropertyMetaType:
    case QMetaObject::BindableProperty:
        if (id < np && call != QMetaObject::BindableProperty) local_call(h, self, call, id, a);
        return id - np;
    default:
        return id;
    }
}

bool dyn_class_named(const QByteArray &name) { return CS().classes.contains(name); }

uint64_t dyn_new(const QByteArray &name, bool *handled) {
    DynClass *c = CS().classes.value(name);
    *handled = c != nullptr;
    if (!c) return 0;
    const uint64_t h = new_with_ctors(c->name.constData(), c->base->ctors, c->base->nctors);
    if (!h) return 0;
    qtc_status err = 0;
    QObject *o = reg().object(h, &err);
    OvHost *host = ov_host(o);
    if (!host) {
        fail(QTC_ERR_STATE, "the object has no override table");
        return 0;
    }
    host->ov_class = c->base;
    host->ov_tags.assign(c->base->nmethods, 0);
    host->dyn_props.clear();
    for (const DynProp &p : c->props)
        host->dyn_props.push_back(p.meta == QMetaType::fromType<QVariant>() ? QVariant() : QVariant(p.meta));
    host->ov_dyn = c;
    return h;
}

void classes_shutdown() {
    ClassState &s = CS();
    for (auto it = s.classes.begin(); it != s.classes.end(); ++it) unregister_class(it.key());
    s.classes.clear();
    s.building = nullptr;
}

}  // namespace qtc

using namespace qtc;

QTC_API qtc_status qtc_class_begin(qtc_str name, qtc_str base) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
#ifdef QTC_NO_RUNTIME_CLASSES
        return fail(QTC_ERR_UNSUPPORTED, "runtime classes need Qt's private QMetaObjectBuilder, which this build of qtc lacks");
#else
        ClassState &s = CS();
        if (s.building) return fail(QTC_ERR_STATE, QStringLiteral("qtc_class_begin: %1 is still being defined (qtc_class_end)").arg(QString::fromUtf8(s.building->name)));
        QByteArray n, b;
        if (!read_bytes(name, n, "name") || !read_bytes(base, b, "base")) return last_error_code();
        n = n.trimmed();
        b = b.trimmed();
        if (b.isEmpty()) b = "QObject";
        if (!identifier(n)) return fail(QTC_ERR_INVALID_ARG, QStringLiteral("qtc_class_begin: %1 is not a valid class name").arg(QString::fromUtf8(n)));
        if (find_class(n) || s.classes.contains(n) || QMetaType::fromName(n).isValid())
            return fail(QTC_ERR_STATE, QStringLiteral("qtc_class_begin: a class named %1 exists").arg(QString::fromUtf8(n)));
        const OvClassDef *ov = find_overridable(b);
        const ClassDef *bc = find_class(b);
        if (!ov || !bc || !bc->meta)
            return fail(QTC_ERR_NOT_FOUND, QStringLiteral("qtc_class_begin: the base %1 is not an overridable class (qtc_overridable_classes)").arg(QString::fromUtf8(b)));
        auto c = std::make_unique<DynClass>();
        c->name = n;
        c->base = ov;
        c->base_meta = bc->meta;
        s.building = c.get();
        s.all.push_back(std::move(c));
        return QTC_OK;
#endif
    });
}

#ifndef QTC_NO_RUNTIME_CLASSES
static qtc_status add_method(qtc_str signature, qtc_str return_type, uint64_t tag, bool is_signal, bool invokable, const char *what) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        DynClass *c = open_class(what, &err);
        if (!c) return err;
        QByteArray spec, ret;
        if (!read_bytes(signature, spec, "signature")) return last_error_code();
        if (return_type.data && !read_bytes(return_type, ret, "return_type")) return last_error_code();
        ret = QMetaObject::normalizedType(ret.trimmed().constData());
        if (ret == "void") ret.clear();
        QByteArray sig, name;
        QList<QByteArray> params;
        QString why;
        if (!parse_sig(spec, &sig, &name, &params, &why)) return fail(QTC_ERR_INVALID_ARG, QStringLiteral("%1: %2").arg(QLatin1StringView(what), why));
        if (!ret.isEmpty() && !type_ok(ret, &why)) return fail(QTC_ERR_INVALID_ARG, QStringLiteral("%1: %2").arg(QLatin1StringView(what), why));
        if (has_method(c, sig)) return fail(QTC_ERR_STATE, QStringLiteral("%1: %2 is defined already").arg(QLatin1StringView(what), QString::fromLatin1(sig)));
        if (base_has(c, name)) return fail(QTC_ERR_STATE, QStringLiteral("%1: %2 has a member named %3").arg(QLatin1StringView(what), QLatin1StringView(c->base_meta->className()), QString::fromLatin1(name)));
        for (const DynProp &p : c->props)
            if (p.name == name) return fail(QTC_ERR_STATE, QStringLiteral("%1: a property is named %2").arg(QLatin1StringView(what), QString::fromLatin1(name)));
        std::deque<DynMethod> &list = is_signal ? c->sigs : c->meths;
        if (int(c->sigs.size() + c->meths.size()) >= 4096) return fail(QTC_ERR_LIMIT, QStringLiteral("%1: at most 4096 methods").arg(QLatin1StringView(what)));
        list.emplace_back();
        DynMethod &m = list.back();
        m.sig = sig;
        m.name = name;
        m.ret = is_signal ? QByteArray() : ret;
        m.params = params;
        m.is_signal = is_signal;
        m.invokable = invokable;
        m.tag = tag;
        return QTC_OK;
    });
}
#endif

QTC_API qtc_status qtc_class_signal(qtc_str signature) {
#ifdef QTC_NO_RUNTIME_CLASSES
    (void)signature;
    return fail(QTC_ERR_UNSUPPORTED, "runtime classes are not available in this build");
#else
    return add_method(signature, qtc_sn(nullptr, 0), 0, true, false, "qtc_class_signal");
#endif
}

QTC_API qtc_status qtc_class_slot(qtc_str signature, qtc_str return_type, uint64_t tag) {
#ifdef QTC_NO_RUNTIME_CLASSES
    (void)signature, (void)return_type, (void)tag;
    return fail(QTC_ERR_UNSUPPORTED, "runtime classes are not available in this build");
#else
    return add_method(signature, return_type, tag, false, false, "qtc_class_slot");
#endif
}

QTC_API qtc_status qtc_class_invokable(qtc_str signature, qtc_str return_type, uint64_t tag) {
#ifdef QTC_NO_RUNTIME_CLASSES
    (void)signature, (void)return_type, (void)tag;
    return fail(QTC_ERR_UNSUPPORTED, "runtime classes are not available in this build");
#else
    return add_method(signature, return_type, tag, false, true, "qtc_class_invokable");
#endif
}

QTC_API qtc_status qtc_class_property(qtc_str name, qtc_str type, uint32_t flags) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
#ifdef QTC_NO_RUNTIME_CLASSES
        (void)name, (void)type, (void)flags;
        return fail(QTC_ERR_UNSUPPORTED, "runtime classes are not available in this build");
#else
        qtc_status err = 0;
        DynClass *c = open_class("qtc_class_property", &err);
        if (!c) return err;
        if (flags & ~uint32_t(QTC_PROP_WRITE | QTC_PROP_NOTIFY | QTC_PROP_CONSTANT))
            return fail(QTC_ERR_INVALID_ARG, "qtc_class_property: unknown flags");
        if ((flags & QTC_PROP_CONSTANT) && (flags & (QTC_PROP_WRITE | QTC_PROP_NOTIFY)))
            return fail(QTC_ERR_INVALID_ARG, "qtc_class_property: a constant property is neither writable nor notifying");
        QByteArray n, t;
        if (!read_bytes(name, n, "name") || !read_bytes(type, t, "type")) return last_error_code();
        n = n.trimmed();
        t = QMetaObject::normalizedType(t.trimmed().constData());
        QString why;
        if (!identifier(n)) return fail(QTC_ERR_INVALID_ARG, QStringLiteral("qtc_class_property: %1 is not a valid name").arg(QString::fromUtf8(n)));
        if (t == "QObject*" || !type_ok(t, &why)) return fail(QTC_ERR_INVALID_ARG, QStringLiteral("qtc_class_property: %1").arg(t == "QObject*" ? QStringLiteral("object properties are not supported") : why));
        for (const DynProp &p : c->props)
            if (p.name == n) return fail(QTC_ERR_STATE, QStringLiteral("qtc_class_property: %1 is defined already").arg(QString::fromUtf8(n)));
        for (const DynMethod &m : c->meths)
            if (m.name == n) return fail(QTC_ERR_STATE, QStringLiteral("qtc_class_property: a method is named %1").arg(QString::fromUtf8(n)));
        if (base_has(c, n)) return fail(QTC_ERR_STATE, QStringLiteral("qtc_class_property: %1 has a member named %2").arg(QLatin1StringView(c->base_meta->className()), QString::fromUtf8(n)));
        if (c->props.size() >= 1024) return fail(QTC_ERR_LIMIT, "qtc_class_property: at most 1024 properties");
        DynProp p;
        p.name = n;
        p.type = t;
        p.meta = QMetaType::fromName(t);
        p.flags = flags;
        c->props.push_back(p);
        return QTC_OK;
#endif
    });
}

QTC_API qtc_status qtc_class_end(void) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
#ifdef QTC_NO_RUNTIME_CLASSES
        return fail(QTC_ERR_UNSUPPORTED, "runtime classes are not available in this build");
#else
        ClassState &s = CS();
        qtc_status err = 0;
        DynClass *c = open_class("qtc_class_end", &err);
        if (!c) return err;
        s.building = nullptr;
        // notify signals: "<name>Changed(<type>)" unless the class declares one with that name
        for (DynProp &p : c->props) {
            if (!(p.flags & QTC_PROP_NOTIFY)) continue;
            const QByteArray sn = p.name + "Changed";
            int found = -1;
            for (size_t i = 0; i < c->sigs.size(); i++)
                if (c->sigs[i].name == sn) {
                    if (found >= 0) return fail(QTC_ERR_AMBIGUOUS, QStringLiteral("qtc_class_end: several signals named %1").arg(QString::fromLatin1(sn)));
                    found = int(i);
                }
            if (found < 0) {
                if (base_has(c, sn)) return fail(QTC_ERR_STATE, QStringLiteral("qtc_class_end: %1 has a member named %2").arg(QLatin1StringView(c->base_meta->className()), QString::fromLatin1(sn)));
                c->sigs.emplace_back();
                DynMethod &m = c->sigs.back();
                m.name = sn;
                m.sig = sn + "(" + p.type + ")";
                m.params = {p.type};
                m.is_signal = true;
                found = int(c->sigs.size() - 1);
            }
            const DynMethod &ns = c->sigs[size_t(found)];
            if (ns.params.size() > 1 || (ns.params.size() == 1 && ns.params[0] != p.type))
                return fail(QTC_ERR_BAD_ARGS, QStringLiteral("qtc_class_end: the notify signal %1 must take nothing or one %2").arg(QString::fromLatin1(ns.sig), QString::fromLatin1(p.type)));
            p.notify = found;
            p.notify_args = int(ns.params.size());
        }
        QMetaObjectBuilder b;
        b.setClassName(c->name);
        b.setSuperClass(c->base_meta);
        b.setStaticMetacallFunction(static_metacall);
        for (const DynMethod &m : c->sigs) b.addSignal(m.sig);
        for (const DynMethod &m : c->meths) {
            QMetaMethodBuilder mb = m.invokable ? b.addMethod(m.sig) : b.addSlot(m.sig);
            mb.setReturnType(m.ret.isEmpty() ? QByteArray("void") : m.ret);
        }
        for (const DynProp &p : c->props) {
            QMetaPropertyBuilder pb = b.addProperty(p.name, p.type, p.notify);
            pb.setReadable(true);
            pb.setWritable(p.flags & QTC_PROP_WRITE);
            pb.setResettable(p.flags & QTC_PROP_WRITE);
            pb.setScriptable(true);
            pb.setStored(true);
            pb.setDesignable(true);
            pb.setConstant(p.flags & QTC_PROP_CONSTANT);
            if (p.notify >= 0) pb.setNotifySignal(b.method(p.notify));
        }
        c->mo = b.toMetaObject();
        if (!c->mo) return fail(QTC_ERR_REJECTED, "qtc_class_end: Qt could not build the meta-object");
        // the handler's view of every slot and invokable
        for (DynMethod &m : c->meths) {
            m.types.clear();
            for (const QByteArray &t : m.params) m.types.push_back(ov_type_for(t));
            m.om.name = m.name.constData();
            m.om.sig = m.sig.constData();
            m.om.nparams = uint8_t(m.params.size());
            m.om.params = m.types.empty() ? nullptr : m.types.data();
            m.om.ret = m.ret.isEmpty() ? OvType{OT_VOID, 0, nullptr, "void"} : ov_type_for(m.ret);
            m.om.base = nullptr;
            m.om.flags = OVF_PURE | OVF_ANY_THREAD;
        }
        c->cdef = ClassDef{c->name.constData(), "qtc", CK_OBJECT, c->base->need, 0, c->mo, nullptr, nullptr, nullptr,
                           nullptr, 0, nullptr, 0};
        register_classes(&c->cdef, 1);
        s.classes.insert(c->name, c);
        return QTC_OK;
#endif
    });
}

QTC_API qtc_status qtc_class_cancel(void) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        if (!CS().building) return fail(QTC_ERR_STATE, "qtc_class_cancel: no class definition is open");
        CS().building = nullptr;
        return QTC_OK;
    });
}

QTC_API qtc_obj qtc_class_new(qtc_str class_name) {
    return guarded<qtc_obj>(0, [&]() -> qtc_obj {
        QByteArray n;
        if (!read_bytes(class_name, n, "class_name")) {
            args().clear();
            return 0;
        }
        bool handled = false;
        const uint64_t h = dyn_new(n.trimmed(), &handled);
        if (!handled) {
            args().clear();
            fail(QTC_ERR_NOT_FOUND, QStringLiteral("no runtime class named %1").arg(QString::fromUtf8(n)));
            return 0;
        }
        return h;
    });
}

QTC_API qtc_status qtc_emit(qtc_obj obj, qtc_str signal) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        QObject *o = reg().object(obj, &err);
        if (!o) {
            args().clear();
            return fail(err, "qtc_emit");
        }
        OvHost *h = ov_host(o);
        if (!h || !h->ov_dyn) {
            args().clear();
            return fail(QTC_ERR_WRONG_TYPE, "qtc_emit: only objects of runtime classes emit their signals from C (qtc_class_new)");
        }
        QByteArray spec;
        if (!read_bytes(signal, spec, "signal")) {
            args().clear();
            return last_error_code();
        }
        spec = spec.trimmed();
        const DynClass *c = h->ov_dyn;
        const int n = args().n;
        int found = -1, count = 0;
        const QByteArray norm = spec.contains('(') ? QMetaObject::normalizedSignature(spec.constData()) : QByteArray();
        for (size_t i = 0; i < c->sigs.size(); i++) {
            const DynMethod &m = c->sigs[i];
            if (norm.isEmpty() ? (m.name == spec && m.params.size() == n) : m.sig == norm) {
                found = int(i);
                count++;
            }
        }
        if (count != 1) {
            args().clear();
            return fail(count ? QTC_ERR_AMBIGUOUS : QTC_ERR_NOT_FOUND,
                        QStringLiteral("qtc_emit: %1 has %2 signal %3 taking %4 arguments").arg(QString::fromUtf8(c->name), count ? QStringLiteral("more than one") : QStringLiteral("no"), QString::fromUtf8(spec)).arg(n));
        }
        // through the meta-object, as qtc_call emits signals: the arguments are converted and checked there
        const QByteArray full = c->sigs[size_t(found)].sig;
        return do_call(obj, qtc_sn(full.constData(), full.size()));
    });
}

QTC_API bool qtc_class_exists_runtime(qtc_str class_name) {
    return guarded<bool>(false, [&]() -> bool {
        QByteArray n;
        if (!read_bytes(class_name, n, "class_name")) return false;
        return CS().classes.contains(n.trimmed());
    });
}
