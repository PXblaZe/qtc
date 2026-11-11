// Qt Positioning's part of the ownership roles (see qtc_roles.cpp and qtc_module.h).
#include "../qtc_module.h"

#include <QtCore/QIODevice>

#include <QtPositioning/QGeoAreaMonitorSource>
#include <QtPositioning/QGeoPositionInfoSource>
#include <QtPositioning/QNmeaPositionInfoSource>
#include <QtPositioning/QNmeaSatelliteInfoSource>

namespace qtc {
namespace {

uint64_t kinds(QObject *o) {
    uint64_t k = 0;
    if (qobject_cast<QGeoAreaMonitorSource *>(o)) k |= OK_GEO;
    if (qobject_cast<QNmeaPositionInfoSource *>(o) || qobject_cast<QNmeaSatelliteInfoSource *>(o)) k |= OK_NMEA;
    return k;
}

QIODevice *nmea_device(QObject *o) {
    if (auto *n = qobject_cast<QNmeaPositionInfoSource *>(o)) return n->device();
    if (auto *n = qobject_cast<QNmeaSatelliteInfoSource *>(o)) return n->device();
    return nullptr;
}

void visit(QObject *owner, uint64_t k, VisitCtx &c) {
    if (k & OK_GEO) {
        auto *m = static_cast<QGeoAreaMonitorSource *>(owner);
        if (c.hit(m->positionInfoSource()) && c.held()) m->setPositionInfoSource(nullptr);
    }
    if (k & OK_NMEA) {
        if (c.hit(nmea_device(owner))) c.pin();   // the device can be set only once
    }
}

// an NMEA source reads the device
bool nmea_reads(const QObject *device) {
    for (const auto &[o, k] : roles_candidates(const_cast<QObject *>(device)))
        if ((k & OK_NMEA) && static_cast<const QObject *>(nmea_device(o)) == device) return true;
    return false;
}

const ModuleExt kExt = [] {
    ModuleExt e;
    e.module = "Positioning";
    e.class_kinds = kinds;
    e.visit = visit;
    return e;
}();

}  // namespace
}  // namespace qtc

QTC_MODULE_EXT(Positioning) {
    qtc::g_module_hooks.nmea_reads = qtc::nmea_reads;
    qtc::register_module_ext(&qtc::kExt);
}
