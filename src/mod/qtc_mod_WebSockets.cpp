// Qt WebSockets' part of the ownership roles (see qtc_roles.cpp and qtc_module.h).
#include "../qtc_module.h"

#include <QtWebSockets/QMaskGenerator>
#include <QtWebSockets/QWebSocket>

namespace qtc {
namespace {

uint64_t kinds(QObject *o) { return qobject_cast<QWebSocket *>(o) ? uint64_t(OK_WEBSOCKET) : 0; }

void visit(QObject *owner, uint64_t k, VisitCtx &c) {
    if (k & OK_WEBSOCKET) {
        // setMaskGenerator(nullptr) returns to the built-in generator, which itself cannot go
        auto *w = static_cast<QWebSocket *>(owner);
        const QObject *g = w->maskGenerator();
        if (c.hit(g) && c.held()) w->setMaskGenerator(nullptr);
    }
}

const ModuleExt kExt = [] {
    ModuleExt e;
    e.module = "WebSockets";
    e.class_kinds = kinds;
    e.visit = visit;
    return e;
}();

}  // namespace
}  // namespace qtc

QTC_MODULE_EXT(WebSockets) { qtc::register_module_ext(&qtc::kExt); }
