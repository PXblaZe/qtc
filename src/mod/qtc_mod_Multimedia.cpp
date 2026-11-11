// Qt Multimedia's part of the ownership roles (see qtc_roles.cpp and qtc_module.h).
#include "../qtc_module.h"

#include <QtMultimedia/QWaveDecoder>

namespace qtc {
namespace {

uint64_t kinds(QObject *o) { return qobject_cast<QWaveDecoder *>(o) ? uint64_t(OK_WAVE) : 0; }

void visit(QObject *owner, uint64_t k, VisitCtx &c) {
    if (k & OK_WAVE) {
        auto *w = static_cast<QWaveDecoder *>(owner);
        if (c.hit(w->getDevice())) c.pin();
    }
}

const ModuleExt kExt = [] {
    ModuleExt e;
    e.module = "Multimedia";
    e.class_kinds = kinds;
    e.visit = visit;
    return e;
}();

}  // namespace
}  // namespace qtc

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QTC_MODULE_EXT(Multimedia) { qtc::register_module_ext(&qtc::kExt); }
QT_WARNING_POP
