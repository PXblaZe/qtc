// Modules loaded on demand. libqtc holds the runtime and Qt Core, Gui and Widgets; every other Qt
// module qtc binds is a library of its own (libqtc_<Module>.so, next to libqtc), loaded the first
// time a program names one of its classes. A widgets program never maps Qt Quick, Qt Multimedia
// or the rest, which is most of what starting it used to cost.
//
// A module library registers its classes (the generated tables), its overridable classes, and
// what qtc's runtime needs to know about its classes beyond their methods: the ownership roles
// (qtc_roles.cpp) and a few hooks. This header is that interface.
#pragma once

#include "qtc_internal.h"

#include <QtCore/QList>
#include <QtCore/QPair>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QVariant>

#pragma GCC visibility push(default)

namespace qtc {

// --------------------------------------------------------------- loading

// A module library: its name, the modules it needs (space-separated) and, built with
// QTC_STATIC_MODULES, its init function (otherwise libqtc_<name> exports qtc_module_init_<name>).
struct ModuleDef {
    const char *name;
    const char *deps;
    void (*init)();
};
struct ModuleClass {
    const char *cls;   // sorted by strcmp
    uint16_t module;   // index into g_modules
};
// generated (qtc_gen_register.cpp): the module libraries and the classes each binds
extern const ModuleDef g_modules[];
extern const int g_module_count;
extern const ModuleClass g_module_classes[];
extern const int g_module_class_count;

// Loads a module's library (and those of the modules it depends on) and registers what it binds.
// GUI thread; true when the module is part of libqtc or is loaded now.
bool load_module(const char *name);
bool module_loaded(const char *name);
// The module that binds a class, or null when no module library does (or the class is in libqtc).
const char *module_of_class(const QByteArray &cls);
// Loads the module that binds cls when it is not loaded yet: true when it was loaded now.
bool load_module_of_class(const QByteArray &cls);
// Loads every module library there is (qtc_describe and other listings of everything bound).
void load_all_modules();
// Loads the modules of o's class and its bases (an object Qt made, of a module not loaded yet).
void load_modules_of(const QObject *o);

// ------------------------------------------------------- ownership roles
//
// What an owner may hold, worked out once per class (see qtc_roles.cpp).
enum OwnerKind : uint64_t {
    OK_SCROLLAREA = 1ull << 0, OK_MESSAGEBOX = 1ull << 1, OK_PROGRESS = 1ull << 2, OK_WIZARD = 1ull << 3,
    OK_COMBO = 1ull << 4, OK_COMPLETER = 1ull << 5, OK_TABBAR = 1ull << 6, OK_TABWIDGET = 1ull << 7,
    OK_LAYOUT = 1ull << 8, OK_FILEDIALOG = 1ull << 9, OK_MAPPER = 1ull << 10, OK_SCENE = 1ull << 11,
    OK_GWIDGET = 1ull << 12, OK_DRAG = 1ull << 13, OK_MOVIE = 1ull << 14, OK_GLCONTEXT = 1ull << 15,
    OK_CONCAT = 1ull << 16, OK_NAM = 1ull << 17, OK_REST = 1ull << 18, OK_QMLCONTEXT = 1ull << 19,
    OK_FILESELECTOR = 1ull << 20, OK_QMLENGINE = 1ull << 21, OK_WAVE = 1ull << 22, OK_VIDEO = 1ull << 23,
    OK_SPATIAL = 1ull << 24, OK_CHARTSERIES = 1ull << 25, OK_AREA = 1ull << 26, OK_GEO = 1ull << 27,
    OK_NMEA = 1ull << 28, OK_WEBSOCKET = 1ull << 29, OK_QMLCOMPONENT = 1ull << 30, OK_CHARTMAPPER = 1ull << 31,
    OK_FOCUSFRAME = 1ull << 32, OK_WINDOWCONTAINER = 1ull << 33, OK_BUTTONBOX = 1ull << 34,
    OK_MQTT = 1ull << 35, OK_OAUTH = 1ull << 36,
};

enum Hold { NOT_HELD = 0, RELEASABLE = 1, PINNED = 2 };

bool roles_inside(const QObject *o, const QObject *root);   // o is root or inside it
QString roles_label(const QObject *o);
bool roles_made_by_qtc(const QObject *o);                   // made with qtc_new
QList<QPair<QObject *, uint64_t>> roles_candidates(QObject *x);   // owners that may hold x
void roles_pin_made(QObject *user, QObject *used);          // user keeps a plain pointer to used

// One owner looked at for one victim (see visit() in qtc_roles.cpp): hit(c) says whether c is
// the victim or inside it (or, collecting, records c); held() marks the owner as holding it and
// says whether to let go now; pin() marks it as holding it for good.
struct VisitCtx {
    QObject *victim = nullptr;
    QPointer<QObject> vp;
    bool release = false;
    QList<QObject *> *collect = nullptr;
    int st = NOT_HELD;
    bool hit(const QObject *c) {
        if (collect) {
            if (c) collect->append(const_cast<QObject *>(c));
            return false;
        }
        return vp && c && roles_inside(c, victim);
    }
    bool held() {
        if (st < RELEASABLE) st = RELEASABLE;
        return release;
    }
    void pin() { st = PINNED; }
    // the victim, or something that dies with it (a child), could be a T
    template <class T> bool could_be() const {
        if (!vp) return false;
        return qobject_cast<T *>(vp.data()) || vp->template findChild<T *>() != nullptr;
    }
};

// What a module adds to the roles. Every function may be null.
struct ModuleExt {
    const char *module = nullptr;
    uint64_t (*class_kinds)(QObject *o) = nullptr;                             // the OwnerKind bits of o's class
    void (*visit)(QObject *owner, uint64_t kinds, VisitCtx &c) = nullptr;     // the roles of those kinds
    uint64_t (*typed)(QObject *o) = nullptr;   // kinds whose holders are found by the type of what they hold
    bool (*qt_owned_class)(QObject *o) = nullptr;      // qt_owned_by_class may say something about o's class
    QString (*qt_owned_by_class)(QObject *o) = nullptr;   // non-empty: Qt owns o
    QString (*not_movable)(QObject *o) = nullptr;     // non-empty: o cannot move to a worker thread
    bool (*defer_delete)(QObject *o) = nullptr;       // true: qtc_destroy leaves deleting o to the module
    void (*adopt_new)(QObject *o, const QList<QObject *> &made_with) = nullptr;   // after qtc_new
};
void register_module_ext(const ModuleExt *e);
const QList<const ModuleExt *> &module_exts();

// ----------------------------------------------------------------- hooks

// What the runtime asks of a module's code (null while the module is not loaded).
struct ModuleHooks {
    bool (*qml_object)(const QObject *o) = nullptr;                    // Qml: o belongs to a QML context
    bool (*plain_variant)(const QVariant &v, QVariant *out) = nullptr; // Qml: a QJSValue as the plain value it holds
    qtc_status (*quick_set_graphics_api)(int32_t api) = nullptr;       // Quick: qtc_quick_set_graphics_api
    int32_t (*quick_graphics_api)(qtc_obj window) = nullptr;           // Quick: qtc_quick_graphics_api
    // ShaderTools: bakes a GLSL shader for every backend (see qtc_rhi_shader_bake); the result is a
    // serialized QShader, or empty with *error set
    QByteArray (*bake_shader)(int stage, const QByteArray &glsl, QString *error) = nullptr;
    bool (*nmea_reads)(const QObject *device) = nullptr;               // Positioning: an NMEA source reads the device
};
extern ModuleHooks g_module_hooks;

}  // namespace qtc

#pragma GCC visibility pop

// What a module library exports: qtc_module_init_<Module>, called once it is loaded, and
// qtc_mod_ext_<Module> (src/mod), its part of the roles and hooks, which the init calls.
#if defined(_WIN32)
#define QTC_MOD_EXPORT
#else
#define QTC_MOD_EXPORT __attribute__((visibility("default")))
#endif
#define QTC_MODULE_INIT(M) extern "C" QTC_MOD_EXPORT void qtc_module_init_##M()
#define QTC_MODULE_EXT(M) extern "C" QTC_MOD_EXPORT void qtc_mod_ext_##M()
