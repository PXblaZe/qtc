// Qt MQTT's part of the ownership roles (see qtc_roles.cpp and qtc_module.h).
#include "../qtc_module.h"

#include <QtMqtt/QMqttClient>

namespace qtc {
namespace {

uint64_t kinds(QObject *o) { return qobject_cast<QMqttClient *>(o) ? uint64_t(OK_MQTT) : 0; }

void visit(QObject *owner, uint64_t k, VisitCtx &c) {
    if (k & OK_MQTT) {
        // the client keeps its transport by a plain pointer (setTransport, or a socket of its own
        // that it deletes itself), and letting go of it (setTransport(nullptr)) crashes: the
        // transport goes with the client
        if (c.hit(static_cast<QMqttClient *>(owner)->transport())) c.pin();
    }
}

const ModuleExt kExt = [] {
    ModuleExt e;
    e.module = "Mqtt";
    e.class_kinds = kinds;
    e.visit = visit;
    return e;
}();

}  // namespace
}  // namespace qtc

QTC_MODULE_EXT(Mqtt) { qtc::register_module_ext(&qtc::kExt); }
