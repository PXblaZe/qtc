// Qt Network's part of the ownership roles (see qtc_roles.cpp and qtc_module.h).
#include "../qtc_module.h"

#include <QtNetwork/QAbstractNetworkCache>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkCookieJar>
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
#include <QtNetwork/QRestAccessManager>
#endif

namespace qtc {
namespace {

uint64_t kinds(QObject *o) {
    uint64_t k = 0;
    if (qobject_cast<QNetworkAccessManager *>(o)) k |= OK_NAM;
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    if (qobject_cast<QRestAccessManager *>(o)) k |= OK_REST;
#endif
    return k;
}

void visit(QObject *owner, uint64_t k, VisitCtx &c) {
    if (k & OK_NAM) {
        auto *n = static_cast<QNetworkAccessManager *>(owner);
        if (c.hit(n->cache()) && c.held()) n->setCache(nullptr);   // deletes it: the manager owns its cache
        // cookieJar() makes a jar when there is none, so only ask when the victim could be one
        if (c.could_be<QNetworkCookieJar>() && c.hit(n->cookieJar()) && c.held()) n->setCookieJar(nullptr);
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    if (k & OK_REST) {
        auto *r = static_cast<QRestAccessManager *>(owner);
        if (c.hit(r->networkAccessManager())) c.pin();
    }
#endif
}

// owners found by the type of what they hold: a manager's cookie jar, a QML engine's manager
uint64_t typed(QObject *o) {
    uint64_t t = 0;
    if (qobject_cast<QNetworkCookieJar *>(o)) t |= OK_NAM;
    if (qobject_cast<QNetworkAccessManager *>(o)) t |= OK_QMLENGINE;
    return t;
}

const ModuleExt kExt = [] {
    ModuleExt e;
    e.module = "Network";
    e.class_kinds = kinds;
    e.visit = visit;
    e.typed = typed;
    return e;
}();

}  // namespace
}  // namespace qtc

QTC_MODULE_EXT(Network) { qtc::register_module_ext(&qtc::kExt); }
