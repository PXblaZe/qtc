// Qt CoAP's part of the ownership roles (see qtc_roles.cpp and qtc_module.h).
#include "../qtc_module.h"

#include <memory>

#include <QtCoap/QCoapClient>
#include <QtCoap/QCoapReply>
#include <QtCore/QCoreApplication>
#include <QtCore/QSemaphore>
#include <QtCore/QThread>
#if __has_include(<QtCoap/private/qcoapclient_p.h>)
#include <QtCoap/private/qcoapclient_p.h>
#include <QtCoap/private/qcoapprotocol_p.h>
#define QTC_COAP_PRIVATE 1
#endif

namespace qtc {
namespace {

bool owned_class(QObject *o) { return qobject_cast<QCoapReply *>(o) != nullptr; }

QString owned_by(QObject *x) {
    // the client's protocol object, on a thread of its own, keeps a running reply by a plain pointer
    if (auto *r = qobject_cast<QCoapReply *>(x); r && !r->isFinished())
        return QStringLiteral("a CoAP reply still running (the client's protocol thread uses it); abort() it, "
                              "and destroy it once it has finished");
    return QString();
}

#ifdef QTC_COAP_PRIVATE
// A request the client sent is queued to its protocol thread (QCoapProtocol::sendRequest), which
// reads the reply there without checking that it still exists: a client destroyed before that
// event ran took its replies along, and the protocol thread crashed on them. Before a client
// goes, its protocol thread works through what is queued for it (a bounded wait: the protocol
// never waits for the GUI thread).
#if defined(__GNUC__)
#if defined(__GNUC__)
__attribute__((no_sanitize("vptr")))   // UBSan's type checks would need QCoapClientPrivate's typeinfo
#endif
#endif
void drain(QCoapClient *c) {
    auto *d = static_cast<QCoapClientPrivate *>(QObjectPrivate::get(c));
    QObject *proto = d ? d->protocol : nullptr;
    QThread *t = d ? d->workerThread : nullptr;
    if (!proto || !t || !t->isRunning() || t == QThread::currentThread() || proto->thread() != t) return;
    auto done = std::make_shared<QSemaphore>(0);
    QMetaObject::invokeMethod(proto, [done] { done->release(); }, Qt::QueuedConnection);
    done->tryAcquire(1, 5000);
}

// not deferring anything: draining, before qtc_destroy deletes the client or what holds it
bool before_delete(QObject *o) {
    if (auto *c = qobject_cast<QCoapClient *>(o)) drain(c);
    for (QCoapClient *c : o->findChildren<QCoapClient *>()) drain(c);
    return false;
}
#endif

const ModuleExt kExt = [] {
    ModuleExt e;
    e.module = "Coap";
#ifdef QTC_COAP_PRIVATE
    e.defer_delete = before_delete;
#endif
    e.qt_owned_class = owned_class;
    e.qt_owned_by_class = owned_by;
    return e;
}();

}  // namespace
}  // namespace qtc

QTC_MODULE_EXT(Coap) { qtc::register_module_ext(&qtc::kExt); }
