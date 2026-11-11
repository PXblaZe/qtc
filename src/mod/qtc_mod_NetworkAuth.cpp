// Qt Network Authorization's part of the ownership roles (see qtc_roles.cpp and qtc_module.h).
#include "../qtc_module.h"

#include <QtNetworkAuth/QAbstractOAuth>
#include <QtNetworkAuth/QAbstractOAuthReplyHandler>

namespace qtc {
namespace {

uint64_t kinds(QObject *o) { return qobject_cast<QAbstractOAuth *>(o) ? uint64_t(OK_OAUTH) : 0; }

void visit(QObject *owner, uint64_t k, VisitCtx &c) {
    if (k & OK_OAUTH) {
        // the built-in reply handler (what replyHandler() returns when none was set) belongs to
        // the OAuth object, which deletes it; one set with setReplyHandler is watched and may go
        auto *a = static_cast<QAbstractOAuth *>(owner);
        QAbstractOAuthReplyHandler *cur = a->replyHandler();
        a->setReplyHandler(nullptr);
        QAbstractOAuthReplyHandler *def = a->replyHandler();
        if (cur != def) a->setReplyHandler(cur);
        if (c.hit(def)) c.pin();
    }
}

const ModuleExt kExt = [] {
    ModuleExt e;
    e.module = "NetworkAuth";
    e.class_kinds = kinds;
    e.visit = visit;
    return e;
}();

}  // namespace
}  // namespace qtc

QTC_MODULE_EXT(NetworkAuth) { qtc::register_module_ext(&qtc::kExt); }
