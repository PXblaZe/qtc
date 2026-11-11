// Loading the module libraries (see qtc_module.h).
//
// Every Qt module beyond Core, Gui and Widgets is bound by a library of its own: libqtc_<Module>
// beside libqtc. The generated index (qtc_gen_register.cpp) says which module binds which class
// and which modules a module needs; the first time a class of a module is looked for, the module
// is loaded (after the modules it needs) and registers its classes, its overridable classes and
// its part of the ownership roles. Built with QTC_STATIC_MODULES, the module code is part of
// libqtc and loading it only registers it.
#include "qtc_module.h"
#include "qtc_events.h"   // gui_thread_ok

#include <cstring>

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QSet>

#ifndef QTC_STATIC_MODULES
#include <dlfcn.h>
#endif

namespace qtc {

ModuleHooks g_module_hooks;

namespace {

enum LoadState : int8_t { UNTRIED = 0, LOADING = 1, LOADED = 2, MISSING = -1 };

QHash<QByteArray, int8_t> &states() {
    static auto *s = new QHash<QByteArray, int8_t>;
    return *s;
}

}  // namespace
}  // namespace qtc
#if defined(QTC_STATIC_MODULES) && defined(QTC_HAS_SHADERTOOLS)
QTC_MODULE_INIT(ShaderTools);
#define QTC_SHADERTOOLS_INIT qtc_module_init_ShaderTools
#else
#define QTC_SHADERTOOLS_INIT nullptr
#endif
namespace qtc {
namespace {

// Modules qtc binds without a class of their own: helpers the runtime loads for a feature.
const ModuleDef kHelpers[] = {
    {"ShaderTools", "Gui", QTC_SHADERTOOLS_INIT},   // qtc_rhi_shader_bake
};

const ModuleDef *module_def(const char *name) {
    for (int i = 0; i < g_module_count; i++)
        if (std::strcmp(g_modules[i].name, name) == 0) return &g_modules[i];
    for (const ModuleDef &h : kHelpers)
        if (std::strcmp(h.name, name) == 0) return &h;
    return nullptr;
}

#ifdef QTC_STATIC_MODULES
using InitFn = void (*)();
InitFn find_init(const ModuleDef *m) { return m->init; }
#else
// the directory libqtc was loaded from, with a trailing slash
QByteArray own_dir() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(&load_module), &info) && info.dli_fname) {
        QByteArray p(info.dli_fname);
        const qsizetype slash = p.lastIndexOf('/');
        if (slash >= 0) return p.left(slash + 1);
    }
    return QByteArray();
}

using InitFn = void (*)();
InitFn find_init(const ModuleDef *m) {
    const char *ext = ".so";   // a CMake MODULE library, macOS included
    static const QByteArray dir = own_dir();
    const QByteArray file = "libqtc_" + QByteArray(m->name) + ext;
    // beside libqtc first (an installed or a build tree), then wherever the loader looks
    void *h = dir.isEmpty() ? nullptr : dlopen((dir + file).constData(), RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen(file.constData(), RTLD_NOW | RTLD_LOCAL);
    if (!h) return nullptr;
    const QByteArray sym = "qtc_module_init_" + QByteArray(m->name);
    return reinterpret_cast<InitFn>(dlsym(h, sym.constData()));
}
#endif

}  // namespace

bool load_module(const char *name) {
    if (!name) return false;
    const ModuleDef *m = module_def(name);
    if (!m) return true;   // part of libqtc (or nothing qtc binds)
    const QByteArray key(name);
    int8_t &s = states()[key];
    if (s == LOADED || s == LOADING) return true;
    if (s == MISSING) return false;
    // registering touches tables only the GUI thread reads (before qtc_init: the thread that calls it)
    if (app_kind() >= 0 && !gui_thread_ok()) return false;
    s = LOADING;
    bool ok = true;
    for (const QByteArray &d : QByteArray(m->deps).split(' '))
        if (!d.isEmpty() && !load_module(d.constData())) ok = false;
    InitFn init = ok ? find_init(m) : nullptr;
    // states() may have grown while the dependencies loaded: look the entry up again
    if (!init) {
        states()[key] = MISSING;
        return false;
    }
    init();
    states()[key] = LOADED;
    return true;
}

bool module_loaded(const char *name) {
    const ModuleDef *m = module_def(name);
    return !m || states().value(QByteArray(name)) == LOADED;
}

const char *module_of_class(const QByteArray &cls) {
    int lo = 0, hi = g_module_class_count;
    const char *key = cls.constData();
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        const int c = std::strcmp(g_module_classes[mid].cls, key);
        if (c == 0) return g_modules[g_module_classes[mid].module].name;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return nullptr;
}

bool load_module_of_class(const QByteArray &cls) {
    const char *m = module_of_class(cls);
    if (!m || module_loaded(m)) return false;
    return load_module(m);
}

void load_all_modules() {
    static bool done = false;   // each one loaded, or found missing
    if (done) return;
    for (int i = 0; i < g_module_count; i++) load_module(g_modules[i].name);
    done = true;
    for (int i = 0; i < g_module_count; i++)
        if (states().value(QByteArray(g_modules[i].name)) == UNTRIED) done = false;   // not on the GUI thread
}

namespace {
struct DPeek : QObject {
    static const QObjectData *of(const QObject *o) {
        auto member = &DPeek::d_ptr;
        return (o->*member).data();
    }
};
}  // namespace

void load_modules_of(const QObject *o) {
    if (!o || g_module_count == 0) return;
    // the classes looked at, by meta-object: static meta-objects only (a dynamic one, the first
    // of an object whose QObjectData has one, can be freed and its address reused)
    static auto *seen = new QSet<const QMetaObject *>;
    const QMetaObject *mo = o->metaObject();
    const QObjectData *d = DPeek::of(o);
    bool dynamic = d && d->metaObject;
    for (; mo; mo = mo->superClass(), dynamic = false) {
        if (!dynamic && seen->contains(mo)) break;   // and so were its bases
        if (const char *m = module_of_class(QByteArray::fromRawData(mo->className(), qsizetype(std::strlen(mo->className())))))
            load_module(m);
        if (!dynamic) seen->insert(mo);
    }
}

}  // namespace qtc
