// Qt Spatial Audio's part of the ownership roles (see qtc_roles.cpp and qtc_module.h).
#include "../qtc_module.h"

#include <QtSpatialAudio/QAmbientSound>
#include <QtSpatialAudio/QAudioEngine>
#include <QtSpatialAudio/QAudioListener>
#include <QtSpatialAudio/QAudioRoom>
#include <QtSpatialAudio/QSpatialSound>

namespace qtc {
namespace {

uint64_t kinds(QObject *o) {
    return (qobject_cast<QSpatialSound *>(o) || qobject_cast<QAmbientSound *>(o) || dynamic_cast<QAudioListener *>(o))
               ? uint64_t(OK_SPATIAL)
               : 0;
}

void visit(QObject *owner, uint64_t k, VisitCtx &c) {
    if (k & OK_SPATIAL) {
        QAudioEngine *e = nullptr;
        if (auto *s = qobject_cast<QSpatialSound *>(owner)) e = s->engine();
        else if (auto *a = qobject_cast<QAmbientSound *>(owner)) e = a->engine();
        else if (auto *l = dynamic_cast<QAudioListener *>(owner)) e = l->engine();
        if (c.hit(e)) c.pin();
    }
}

void adopt_new(QObject *o, const QList<QObject *> &made_with) {
    // a room uses its engine until it dies and has no getter for it (sounds and listeners are
    // pinned through engine()); as the engine's child it would die after the engine's own
    // destructor ran, which Qt 6.4 does not survive
    if (qobject_cast<QAudioRoom *>(o))
        for (QObject *a : made_with)
            if (qobject_cast<QAudioEngine *>(a)) { roles_pin_made(o, a); break; }
}

const ModuleExt kExt = [] {
    ModuleExt e;
    e.module = "SpatialAudio";
    e.class_kinds = kinds;
    e.visit = visit;
    e.adopt_new = adopt_new;
    return e;
}();

}  // namespace
}  // namespace qtc

QTC_MODULE_EXT(SpatialAudio) { qtc::register_module_ext(&qtc::kExt); }
