// Qt CoAP's part of the ownership roles (see qtc_roles.cpp and qtc_module.h).
#include "../qtc_module.h"

#include <QtCoap/QCoapReply>

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

const ModuleExt kExt = [] {
    ModuleExt e;
    e.module = "Coap";
    e.qt_owned_class = owned_class;
    e.qt_owned_by_class = owned_by;
    return e;
}();

}  // namespace
}  // namespace qtc

QTC_MODULE_EXT(Coap) { qtc::register_module_ext(&qtc::kExt); }
