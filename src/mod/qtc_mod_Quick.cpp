// Qt Quick's part of the runtime: ownership roles (see qtc_roles.cpp) and the graphics API of
// Quick windows (qtc_quick_set_graphics_api, qtc_quick_graphics_api; see qtc_module.h).
#include "../qtc_module.h"

#include <QtGui/QGuiApplication>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickRenderControl>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>

namespace qtc {
namespace {

bool owned_class(QObject *o) {
    return qobject_cast<QQuickItem *>(o) || o->inherits("QQuickRootItem") || qobject_cast<QQuickRenderControl *>(o);
}

QString owned_by_class(QObject *x) {
    if (auto *item = qobject_cast<QQuickItem *>(x))
        if (item->window() && item->window()->contentItem() == item) return QStringLiteral("the content item of a Quick window");
    if (x->inherits("QQuickRootItem")) return QStringLiteral("the content item of a Quick window");
    if (auto *rc = qobject_cast<QQuickRenderControl *>(x))
        if (rc->window()) return QStringLiteral("the render control of a live QQuickWindow; destroy the window first");
    return QString();
}

qtc_status set_graphics_api(int32_t api) {
    QSGRendererInterface::GraphicsApi g;
    switch (api) {
    case QTC_GAPI_SOFTWARE: g = QSGRendererInterface::Software; break;
    case QTC_GAPI_OPENGL: g = QSGRendererInterface::OpenGL; break;
    case QTC_GAPI_VULKAN: g = QSGRendererInterface::Vulkan; break;
    case QTC_GAPI_D3D11: g = QSGRendererInterface::Direct3D11; break;
    case QTC_GAPI_D3D12: g = QSGRendererInterface::Direct3D12; break;
    case QTC_GAPI_METAL: g = QSGRendererInterface::Metal; break;
    case QTC_GAPI_NULL: g = QSGRendererInterface::Null; break;
    default: return fail(QTC_ERR_OUT_OF_RANGE, "qtc_quick_set_graphics_api: not a QTC_GAPI_ value");
    }
    if (app_kind() == QTC_APP_CORE) return fail(QTC_ERR_STATE, "qtc_quick_set_graphics_api needs a GUI application");
    // Qt reads it as windows are made: a later change would leave windows on different APIs
    for (QWindow *w : QGuiApplication::allWindows())
        if (qobject_cast<QQuickWindow *>(w))
            return fail(QTC_ERR_STATE, "qtc_quick_set_graphics_api: a Qt Quick window exists already (set it before making any)");
    QQuickWindow::setGraphicsApi(g);
    return QTC_OK;
}

int32_t gapi_of(QSGRendererInterface::GraphicsApi g) {
    switch (g) {
    case QSGRendererInterface::Software: return QTC_GAPI_SOFTWARE;
    case QSGRendererInterface::OpenGL: return QTC_GAPI_OPENGL;
    case QSGRendererInterface::Vulkan: return QTC_GAPI_VULKAN;
    case QSGRendererInterface::Direct3D11: return QTC_GAPI_D3D11;
    case QSGRendererInterface::Direct3D12: return QTC_GAPI_D3D12;
    case QSGRendererInterface::Metal: return QTC_GAPI_METAL;
    case QSGRendererInterface::Null: return QTC_GAPI_NULL;
    default: return QTC_GAPI_UNKNOWN;
    }
}

int32_t graphics_api(qtc_obj window) {
    if (!window) {
        // Qt reports the RHI's API even when the software adaptation is asked for
        if (QQuickWindow::sceneGraphBackend() == QLatin1StringView("software")) return QTC_GAPI_SOFTWARE;
        return gapi_of(QQuickWindow::graphicsApi());
    }
    qtc_status err = 0;
    QObject *o = reg().object(window, &err);
    if (!o) return fail(err, "qtc_quick_graphics_api");
    auto *w = qobject_cast<QQuickWindow *>(o);
    if (!w) return fail(QTC_ERR_WRONG_TYPE, "qtc_quick_graphics_api: not a QQuickWindow");
    QSGRendererInterface *ri = w->rendererInterface();
    return ri ? gapi_of(ri->graphicsApi()) : QTC_GAPI_UNKNOWN;
}

const ModuleExt kExt = [] {
    ModuleExt e;
    e.module = "Quick";
    e.qt_owned_class = owned_class;
    e.qt_owned_by_class = owned_by_class;
    return e;
}();

}  // namespace
}  // namespace qtc

QTC_MODULE_EXT(Quick) {
    qtc::g_module_hooks.quick_set_graphics_api = qtc::set_graphics_api;
    qtc::g_module_hooks.quick_graphics_api = qtc::graphics_api;
    qtc::register_module_ext(&qtc::kExt);
}
