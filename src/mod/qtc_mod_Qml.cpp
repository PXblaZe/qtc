// Qt QML's part of the runtime: ownership roles (see qtc_roles.cpp), which objects QML made, and
// JavaScript values as the plain values they hold (see qtc_module.h).
#include "../qtc_module.h"

#include <QtCore/QFileSelector>
#include <QtNetwork/QNetworkAccessManager>
#include <QtQml/QJSValue>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlFileSelector>

namespace qtc {
namespace {

uint64_t kinds(QObject *o) {
    uint64_t k = 0;
    if (qobject_cast<QQmlContext *>(o)) k |= OK_QMLCONTEXT;
    if (qobject_cast<QQmlFileSelector *>(o)) k |= OK_FILESELECTOR;
    if (qobject_cast<QQmlEngine *>(o)) k |= OK_QMLENGINE;
    if (qobject_cast<QQmlComponent *>(o)) k |= OK_QMLCOMPONENT;
    return k;
}

void visit(QObject *owner, uint64_t k, VisitCtx &c) {
    if (k & OK_QMLCONTEXT) {
        auto *x = static_cast<QQmlContext *>(owner);
        if (c.hit(x->contextObject()) && c.held()) x->setContextObject(nullptr);
        // a context made for an engine uses it until it dies, even as the child of something else
        // (the engine's own contexts die with it)
        if (roles_made_by_qtc(x) && c.hit(x->engine())) c.pin();
    }
    if (k & OK_QMLCOMPONENT) {
        auto *x = static_cast<QQmlComponent *>(owner);
        if (roles_made_by_qtc(x) && c.hit(x->engine())) c.pin();
    }
    if (k & OK_FILESELECTOR) {
        // a fresh selector of its own takes over (setSelector(nullptr) makes one that Qt 6.4
        // deletes twice); taking over deletes the built-in one, which is its release
        auto *f = static_cast<QQmlFileSelector *>(owner);
        if (c.hit(f->selector()) && c.held()) f->setSelector(new QFileSelector(f));
    }
    if ((k & OK_QMLENGINE) && c.could_be<QNetworkAccessManager>()) {
        auto *e = static_cast<QQmlEngine *>(owner);
        if (QObject *nam = e->networkAccessManager(); nam && (nam == c.victim || roles_inside(nam, c.victim))) c.pin();
    }
}

bool owned_class(QObject *o) { return qobject_cast<QQmlContext *>(o) != nullptr; }

QString owned_by_class(QObject *x) {
    if (qobject_cast<QQmlContext *>(x) && !roles_made_by_qtc(x)) return QStringLiteral("a context made by the QML engine");
    return QString();
}

QString not_movable(QObject *x) {
    if (qmlContext(x) || qmlEngine(x)) return QStringLiteral("belongs to a QML engine");
    return QString();
}

// A component still loading asynchronously is called back by Qt's type loader when the load
// completes, so it must stay alive until then.
bool defer_delete(QObject *o) {
    auto *c = qobject_cast<QQmlComponent *>(o);
    if (!c || !c->isLoading()) return false;
    QObject::connect(c, &QQmlComponent::statusChanged, c, [c](QQmlComponent::Status s) {
        if (s != QQmlComponent::Loading) c->deleteLater();
    });
    return true;
}

void adopt_new(QObject *o, const QList<QObject *> &made_with) {
    // a QQmlFileSelector calls its engine when it is destroyed (Qt 6.4 through a plain pointer)
    if (qobject_cast<QQmlFileSelector *>(o))
        for (QObject *a : made_with)
            if (qobject_cast<QQmlEngine *>(a)) { roles_pin_made(o, a); break; }
    // a component keeps a plain pointer to its engine
    if (auto *c = qobject_cast<QQmlComponent *>(o); c && !c->parent() && c->engine()) c->setParent(c->engine());
}

bool qml_object(const QObject *o) { return qmlContext(o) != nullptr; }

// a JavaScript value (a QML var, an object QML passes as a QVariant: older Qt keeps these as
// QJSValue) becomes the maps, lists and scalars it holds
bool plain_variant(const QVariant &v, QVariant *out) {
    if (v.metaType() != QMetaType::fromType<QJSValue>()) return false;
    *out = v.value<QJSValue>().toVariant();
    return true;
}

const ModuleExt kExt = [] {
    ModuleExt e;
    e.module = "Qml";
    e.class_kinds = kinds;
    e.visit = visit;
    e.qt_owned_class = owned_class;
    e.qt_owned_by_class = owned_by_class;
    e.not_movable = not_movable;
    e.defer_delete = defer_delete;
    e.adopt_new = adopt_new;
    return e;
}();

}  // namespace
}  // namespace qtc

QTC_MODULE_EXT(Qml) {
    qtc::g_module_hooks.qml_object = qtc::qml_object;
    qtc::g_module_hooks.plain_variant = qtc::plain_variant;
    qtc::register_module_ext(&qtc::kExt);
}
