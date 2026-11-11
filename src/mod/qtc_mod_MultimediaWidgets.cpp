// Qt Multimedia Widgets' part of the ownership roles (see qtc_roles.cpp and qtc_module.h).
#include "../qtc_module.h"

#include <QtMultimedia/QVideoSink>
#include <QtMultimediaWidgets/QGraphicsVideoItem>
#include <QtMultimediaWidgets/QVideoWidget>

namespace qtc {
namespace {

uint64_t kinds(QObject *o) {
    return (qobject_cast<QVideoWidget *>(o) || qobject_cast<QGraphicsVideoItem *>(o)) ? uint64_t(OK_VIDEO) : 0;
}

void visit(QObject *owner, uint64_t k, VisitCtx &c) {
    if (k & OK_VIDEO) {
        QVideoSink *sink = nullptr;
        if (auto *v = qobject_cast<QVideoWidget *>(owner)) sink = v->videoSink();
        else if (auto *g = qobject_cast<QGraphicsVideoItem *>(owner)) sink = g->videoSink();
        if (c.hit(sink)) c.pin();
    }
}

const ModuleExt kExt = [] {
    ModuleExt e;
    e.module = "MultimediaWidgets";
    e.class_kinds = kinds;
    e.visit = visit;
    return e;
}();

}  // namespace
}  // namespace qtc

QTC_MODULE_EXT(MultimediaWidgets) { qtc::register_module_ext(&qtc::kExt); }
