// Graphics: Qt's RHI (QRhi) for C, Qt Quick's choice of graphics API, and the native objects a
// program needs to call OpenGL, Vulkan, Direct3D or Metal itself.
//
// RHI objects (an RHI, its buffers, textures, samplers, render targets, swapchains, shader
// resource bindings, pipelines, readbacks) and shaders are handles of their own: generation-checked
// like qtc's object handles, typed, and owned by the RHI they were made with (destroying it
// destroys them). Users keep what they use alive (bindings their resources, a render target its
// texture, a pipeline the bindings it was made with), so a QRhi resource is never deleted while a
// QRhi object still points at it. A handle is invalid before anything that may run program code
// happens while it is freed.
//
// Every command is checked before it reaches the GPU: the frame and pass it needs; a pipeline's
// vertex layout, interface and resource bindings against the shaders' reflection data; every draw
// against the sizes of the buffers it reads (an out-of-bounds fetch is undefined behaviour on the
// GPU, which software drivers turn into a crash), indexed draws against a CPU copy of what the GPU
// will see in the index buffer at that point of the frame.
#include "qtc_override.h"
#include "qtc_events.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QVarLengthArray>
#ifdef QTC_HAS_GUI
#include <QtGui/QColor>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QMatrix4x4>
#include <QtGui/QOffscreenSurface>
#include <QtGui/QPlatformSurfaceEvent>
#include <QtGui/QWindow>
#include <QtGui/qtgui-config.h>
#if QT_CONFIG(opengl)
#include <QtGui/QOpenGLContext>
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0) && __has_include(<rhi/qrhi.h>)
#define QTC_RHI 1
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <rhi/qshader.h>
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
#define QTC_VULKAN 1
#include <QtGui/QVulkanInstance>
#endif
#if defined(QTC_HAS_SHADERTOOLS) && __has_include(<rhi/qshaderbaker.h>)
#define QTC_BAKER 1
#include <rhi/qshaderbaker.h>
#endif
#endif
#endif
#ifdef QTC_HAS_MODULE_Quick
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>
#endif
#if defined(QTC_RHI) && defined(QTC_HAS_WIDGETS)
#include <QtWidgets/QRhiWidget>
#define QTC_RHI_WIDGET 1
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace qtc {

#ifdef QTC_RHI
// qtc_override.cpp: the QRhiCommandBuffer argument of the override call being handled for `self`,
// and a function to run when that call returns.
QRhiCommandBuffer *ov_rhi_cb_arg(const QObject *self);
void ov_at_return(void (*fn)(uint64_t), uint64_t arg);

namespace {

enum RKind : uint8_t {
    RK_FREE = 0, RK_RHI, RK_BUFFER, RK_TEXTURE, RK_SAMPLER, RK_RT, RK_SC, RK_SRB, RK_GPIPE, RK_CPIPE,
    RK_SHADER, RK_READBACK, RK_VKI, RK_LAST
};
const char *kind_name(int k) {
    static const char *const n[] = {"object", "RHI", "buffer", "texture", "sampler", "render target", "swapchain",
                                    "bindings", "graphics pipeline", "compute pipeline", "shader", "readback",
                                    "Vulkan instance"};
    return k >= 0 && k < RK_LAST ? n[k] : "?";
}

// The result type QRhi gives buffer readbacks (QRhiReadbackResult in recent versions).
template <class F> struct ReadbackArg;
template <class C, class R> struct ReadbackArg<void (C::*)(QRhiBuffer *, quint32, quint32, R *)> {
    using type = R;
};
using BufReadbackResult = ReadbackArg<decltype(&QRhiResourceUpdateBatch::readBackBuffer)>::type;

// A readback's result. Qt writes into it when the GPU is done, which may be after its handle is
// gone: the RHI's context keeps it until then.
struct RbBox {
    QRhiReadbackResult tex;
    BufReadbackResult buf;
    bool done = false;
};

// Members that newer Qt versions have: checked where they exist.
template <class B> int block_qualifiers(const B &b) {
    if constexpr (requires { b.qualifierFlags.toInt(); }) return int(b.qualifierFlags.toInt());
    else return -1;
}
template <class D> int writes_point_size(const D &d) {
    if constexpr (requires { d.outputBuiltinVariables(); }) {
        for (const auto &b : d.outputBuiltinVariables())
            if (b.type == D::PointSizeBuiltin) return 1;
        return 0;
    } else {
        return -1;
    }
}

// RHI handles carry a tag in their index half, so that they never match an object, value or
// other handle of the same index and generation (and those never match them).
constexpr uint32_t kRhiTag = 0x52000000u;
inline uint64_t rh_make(uint32_t index, uint32_t gen) { return (uint64_t(gen) << 32) | (uint64_t(index) + 1 + kRhiTag); }
inline uint32_t rh_index(uint64_t h) {
    const uint32_t low = uint32_t(h & 0xffffffffu);
    return low > kRhiTag ? low - 1 - kRhiTag : UINT32_MAX;
}
inline uint32_t rh_gen(uint64_t h) { return uint32_t(h >> 32); }

struct Bind {
    int binding = 0;
    uint32_t stages = 0;
    int type = 0;                 // 1 uniform buffer, 2 sampled texture, 3 storage buffer, 4 storage image
    uint64_t res = 0;             // buffer or texture
    uint64_t sampler = 0;
    int64_t offset = 0, size = 0;
    int access = 0;               // storage: QTC_RHI_LOAD, _STORE, _LOAD_STORE
};

struct VAttr {
    int binding = 0, location = 0, format = 0, offset = 0;
};

#ifdef QTC_VULKAN
// A Vulkan instance and the windows that use it (their surfaces must go before it does).
struct VkHolder {
    QVulkanInstance *inst = nullptr;
    std::vector<QPointer<QWindow>> windows;
};
#endif

struct Ctx;

struct RObj {
    uint32_t gen = 1;
    uint8_t kind = RK_FREE;
    uint64_t rhi = 0;                     // the RHI it belongs to (resources, readbacks)
    int refs = 0;                         // users that keep it alive
    std::vector<uint64_t> holds;          // what it keeps alive
    QRhiResource *res = nullptr;
    Ctx *ctx = nullptr;                   // RK_RHI
    // RK_SHADER; pipelines: copies of their shaders
    QShader shader, shader2;
    // RK_BUFFER
    int btype = 0;
    uint32_t usage = 0;
    int64_t size = 0;
    QByteArray shadow;                    // index buffers: what the GPU sees from the next draw on
    QByteArray staged;                    // ... and after the uploads queued since
    bool shadow_valid = false;
    int64_t mx_key[4] = {-1, -1, -1, -1};   // the last max_index query and its answer
    uint64_t mx_val = 0;
    // RK_TEXTURE, RK_RT
    int format = 0;
    int w = 0, h = 0;
    uint32_t tflags = 0;
    // RK_RT, RK_SC
    QRhiRenderBuffer *depth = nullptr;
    QRhiRenderPassDescriptor *rp = nullptr;
    bool has_depth = false;
    bool external = false;                // a QRhiWidget's target: not ours, valid during one call
    // RK_SC
    QPointer<QWindow> window;
    QObject *filter = nullptr;
    uint32_t scflags = 0;
    bool built = false;
    // RK_SRB
    std::vector<Bind> binds;
    bool created = false;
    // RK_GPIPE, RK_CPIPE
    uint64_t vs = 0, fs = 0;
    std::vector<int> strides;
    std::vector<bool> per_instance;
    std::vector<VAttr> attrs;
    int topology = 0, cull = 0;
    bool blend = false, depth_test = false, depth_write = false;
    QRhiRenderPassDescriptor *rp_own = nullptr;   // what it draws into: compatible targets alike
    uint64_t srb = 0;                              // the bindings it was made with
    // RK_READBACK
    std::shared_ptr<RbBox> rb;
    bool rb_image = false;
#ifdef QTC_VULKAN
    // RK_VKI
    VkHolder *vkh = nullptr;
#endif
};

struct Ctx {
    QRhi *rhi = nullptr;
    int backend = 0;
    bool owned = true;                    // false: a QRhiWidget's RHI (its window's)
    bool in_cleanup = false;              // the widget's QRhi is being destroyed
    QOffscreenSurface *fallback = nullptr;
#ifdef QTC_VULKAN
    VkHolder *vk = nullptr;
#endif
    bool in_frame = false;
    bool external_frame = false;          // borrowed from a QRhiWidget's override call
    bool external_cmds = false;           // between qtc_rhi_begin_external and _end_external
    uint64_t frame_sc = 0;                // the swapchain of the frame (0: offscreen)
    QRhiCommandBuffer *cb = nullptr;
    int pass = 0;                         // 0 none, 1 render, 2 compute
    uint32_t pass_flags = 0;
    QRhiRenderPassDescriptor *pass_rp = nullptr;
    QSize pass_size;
    uint64_t pipeline = 0, srb = 0;
    struct VB {
        uint64_t buf = 0;
        int64_t offset = 0;
    };
    std::vector<VB> vbufs;
    uint64_t ibuf = 0;
    int64_t ioffset = 0;
    int iformat = 0;                      // 0: 16-bit, 1: 32-bit
    QRhiResourceUpdateBatch *pending = nullptr;
    std::vector<uint64_t> staged_bufs;    // index buffers with uploads in `pending`
    std::vector<QRhiResource *> zombies;  // destroyed while `pending` may still refer to them
    std::vector<std::shared_ptr<RbBox>> inflight;
    std::vector<uint64_t> children;
};

struct Table {
    std::vector<RObj> t;
    std::vector<uint32_t> free;
    int64_t live = 0;
};
Table &T() {
    static Table *t = new Table;
    return *t;
}

// Readback results a destroyed RHI may still write into: kept for good.
std::vector<std::shared_ptr<RbBox>> &graveyard() {
    static auto *g = new std::vector<std::shared_ptr<RbBox>>;
    return *g;
}

uint64_t alloc(uint8_t kind, uint64_t rhi) {
    Table &tb = T();
    uint32_t idx;
    if (!tb.free.empty()) {
        idx = tb.free.back();
        tb.free.pop_back();
    } else {
        if (tb.t.size() >= (1u << 22)) return 0;
        tb.t.emplace_back();
        idx = uint32_t(tb.t.size() - 1);
    }
    RObj &o = tb.t[idx];
    const uint32_t gen = o.gen;
    o = RObj();
    o.gen = gen;
    o.kind = kind;
    o.rhi = rhi;
    tb.live++;
    return rh_make(idx, gen);
}

void free_slot(uint64_t h) {
    Table &tb = T();
    RObj &slot = tb.t[rh_index(h)];
    const uint32_t gen = slot.gen + 1;
    slot = RObj();
    slot.gen = gen ? gen : 1;
    tb.free.push_back(rh_index(h));
    tb.live--;
}

// Quiet lookup (kind 0: any). The pointer is valid until the next allocation.
RObj *find(uint64_t h, int kind = 0) {
    if (!h) return nullptr;
    Table &tb = T();
    const uint32_t idx = rh_index(h);
    if (idx >= tb.t.size()) return nullptr;
    RObj &o = tb.t[idx];
    if (o.gen != rh_gen(h) || o.kind == RK_FREE || (kind && o.kind != kind)) return nullptr;
    return &o;
}

RObj *get(uint64_t h, int kind, const char *what, qtc_status *err) {
    *err = 0;
    const QLatin1StringView w(what);
    if (!h) {
        *err = fail(QTC_ERR_NULL_HANDLE, QStringLiteral("%1: handle 0").arg(w));
        return nullptr;
    }
    Table &tb = T();
    const uint32_t idx = rh_index(h);
    if (idx >= tb.t.size() || rh_gen(h) == 0) {
        *err = fail(QTC_ERR_BAD_HANDLE, QStringLiteral("%1: not an RHI handle").arg(w));
        return nullptr;
    }
    RObj &o = tb.t[idx];
    if (o.gen != rh_gen(h) || o.kind == RK_FREE) {
        *err = fail(rh_gen(h) < o.gen ? QTC_ERR_STALE_HANDLE : QTC_ERR_BAD_HANDLE,
                    QStringLiteral("%1: the %2 is gone").arg(w, QLatin1StringView(kind_name(kind))));
        return nullptr;
    }
    if (kind && o.kind != kind) {
        *err = fail(QTC_ERR_WRONG_TYPE, QStringLiteral("%1: a %2, not a %3").arg(w, QLatin1StringView(kind_name(o.kind)),
                                                                               QLatin1StringView(kind_name(kind))));
        return nullptr;
    }
    return &o;
}

Ctx *rhi_ctx(uint64_t h, const char *what, qtc_status *err) {
    RObj *r = get(h, RK_RHI, what, err);
    return r ? r->ctx : nullptr;
}
Ctx *ctx_quiet(uint64_t h) {
    RObj *r = find(h, RK_RHI);
    return r ? r->ctx : nullptr;
}

void hold(uint64_t user, uint64_t target) {
    RObj *t = find(target);
    RObj *u = find(user);
    if (!t || !u) return;
    t->refs++;
    u->holds.push_back(target);
}

QRhiResourceUpdateBatch *pending(Ctx *c) {
    if (!c->pending) c->pending = c->rhi->nextResourceUpdateBatch();
    return c->pending;
}

// Hands the queued uploads and readbacks to the command buffer: from here on the GPU sees the
// index buffers' new contents.
QRhiResourceUpdateBatch *take_pending(Ctx *c) {
    QRhiResourceUpdateBatch *b = c->pending;
    c->pending = nullptr;
    // resources destroyed while the batch waited go once the frame it is recorded into is submitted
    for (QRhiResource *z : c->zombies) z->deleteLater();
    c->zombies.clear();
    for (uint64_t h : c->staged_bufs)
        if (RObj *o = find(h, RK_BUFFER)) {
            o->shadow = o->staged;
            o->shadow_valid = true;
            o->mx_key[0] = -1;
        }
    c->staged_bufs.clear();
    return b;
}

void prune(Ctx *c) {
    auto &v = c->inflight;
    v.erase(std::remove_if(v.begin(), v.end(), [](const std::shared_ptr<RbBox> &b) { return b->done; }), v.end());
}

void drop_resource(Ctx *c, QRhiResource *res) {
    if (!res) return;
    // a frame being recorded may use it: it goes when the frame has been submitted; queued
    // uploads and readbacks may refer to it: it goes once they are in a frame
    if (c && c->rhi && c->rhi->isRecordingFrame()) res->deleteLater();
    else if (c && c->pending) c->zombies.push_back(res);
    else delete res;
}

void reset_pass_state(Ctx *c) {
    c->pass = 0;
    c->pass_flags = 0;
    c->pass_rp = nullptr;
    c->pass_size = QSize();
    c->pipeline = c->srb = 0;
    c->vbufs.clear();
    c->ibuf = 0;
}

void reset_frame_state(Ctx *c) {
    reset_pass_state(c);
    c->in_frame = false;
    c->external_frame = false;
    c->external_cmds = false;
    c->frame_sc = 0;
    c->cb = nullptr;
}

// Ends what is open in a frame: native commands, then the pass.
void close_pass(Ctx *c) {
    if (c->external_cmds) {
        c->cb->endExternal();
        c->external_cmds = false;
    }
    if (c->pass == 1) c->cb->endPass(take_pending(c));
    else if (c->pass == 2) c->cb->endComputePass(take_pending(c));
    reset_pass_state(c);
}

// Ends a frame of our own whatever its state (destruction, a window losing its surface).
void abandon_frame(Ctx *c) {
    if (!c->in_frame || c->external_frame) return;
    close_pass(c);
    if (QRhiResourceUpdateBatch *b = take_pending(c)) c->cb->resourceUpdate(b);
    if (c->frame_sc) {
        if (RObj *sc = find(c->frame_sc, RK_SC)) c->rhi->endFrame(static_cast<QRhiSwapChain *>(sc->res), QRhi::SkipPresent);
    } else {
        c->rhi->endOffscreenFrame();
    }
    reset_frame_state(c);
    prune(c);
}

#ifdef QTC_VULKAN
void vk_free(VkHolder *v) {
    if (!v) return;
    for (QPointer<QWindow> &w : v->windows) {
        if (!w || w->vulkanInstance() != v->inst) continue;
        if (w->handle()) w->destroy();   // its surface goes while the instance is still there
        if (w) w->setVulkanInstance(nullptr);
    }
    delete v->inst;
    delete v;
}
#endif

#ifdef QTC_RHI_WIDGET
QHash<QRhi *, uint64_t> &widget_rhis() {
    static auto *m = new QHash<QRhi *, uint64_t>;
    return *m;
}
#endif

void release(uint64_t h);

void release_rhi(uint64_t h) {
    RObj *o = find(h, RK_RHI);
    if (!o) return;
    Ctx *c = o->ctx;
    abandon_frame(c);
    // users before what they use, while the QRhi is there
    static const uint8_t order[] = {RK_READBACK, RK_GPIPE, RK_CPIPE, RK_SRB, RK_RT, RK_SC, RK_SAMPLER, RK_TEXTURE, RK_BUFFER};
    for (uint8_t k : order) {
        const std::vector<uint64_t> kids = c->children;
        for (uint64_t x : kids)
            if (RObj *ko = find(x); ko && ko->kind == k) release(x);
    }
    const std::vector<uint64_t> rest = c->children;
    for (uint64_t x : rest) release(x);
    free_slot(h);   // gone for everyone before anything below may run program code
    if (c->pending) {
        c->pending->release();
        c->pending = nullptr;
    }
    for (QRhiResource *z : c->zombies) delete z;
    c->zombies.clear();
    if (c->owned) {
        // the QRhi completes (or drops) what is in flight while the results still exist
        delete c->rhi;
        c->rhi = nullptr;
        delete c->fallback;
#ifdef QTC_VULKAN
        vk_free(c->vk);
#endif
    } else {
#ifdef QTC_RHI_WIDGET
        widget_rhis().remove(c->rhi);
#endif
        if (!c->in_cleanup) c->rhi->removeCleanupCallback(c);   // never while Qt runs the callbacks
        if (!c->rhi->isRecordingFrame()) c->rhi->finish();
        prune(c);
        for (auto &b : c->inflight) graveyard().push_back(b);   // a frame still refers to them
    }
    delete c;
}

void release(uint64_t h) {
    RObj *po = find(h);
    if (!po) return;
    if (po->kind == RK_RHI) {
        release_rhi(h);
        return;
    }
    RObj o = std::move(*po);
    free_slot(h);   // gone for everyone before anything below may run program code
    Ctx *c = o.rhi ? ctx_quiet(o.rhi) : nullptr;
    for (uint64_t x : o.holds)
        if (RObj *t = find(x)) t->refs--;
    if (c) {
        auto &v = c->children;
        v.erase(std::remove(v.begin(), v.end(), h), v.end());
        if (c->pipeline == h) c->pipeline = 0;
        if (c->srb == h) c->srb = 0;
        if (c->ibuf == h) c->ibuf = 0;
        for (Ctx::VB &vb : c->vbufs)
            if (vb.buf == h) vb = Ctx::VB{};
    }
    switch (o.kind) {
    case RK_RT:
        if (!o.external) {
            drop_resource(c, o.res);
            drop_resource(c, o.depth);
            drop_resource(c, o.rp);
        }
        break;
    case RK_SC:
        if (o.filter) {
            if (o.window) o.window->removeEventFilter(o.filter);
            delete o.filter;
        }
        drop_resource(c, o.res);
        drop_resource(c, o.depth);
        drop_resource(c, o.rp);
        break;
    case RK_GPIPE:
        drop_resource(c, o.res);
        drop_resource(c, o.rp_own);
        break;
    case RK_SHADER:
    case RK_READBACK:
        break;
#ifdef QTC_VULKAN
    case RK_VKI:
        vk_free(o.vkh);
        break;
#endif
    default:
        drop_resource(c, o.res);
        break;
    }
}

uint64_t child(uint64_t rhi_h, uint8_t kind) {
    if (!find(rhi_h, RK_RHI)) return 0;
    const uint64_t h = alloc(kind, rhi_h);
    if (!h) {
        fail(QTC_ERR_LIMIT, "too many RHI objects");
        return 0;
    }
    find(rhi_h, RK_RHI)->ctx->children.push_back(h);   // the table may have moved
    return h;
}

// ------------------------------------------------------------------ enums

bool tex_format(int f, QRhiTexture::Format *out) {
    switch (f) {
    case QTC_RHI_RGBA8: *out = QRhiTexture::RGBA8; return true;
    case QTC_RHI_BGRA8: *out = QRhiTexture::BGRA8; return true;
    case QTC_RHI_R8: *out = QRhiTexture::R8; return true;
    case QTC_RHI_RGBA16F: *out = QRhiTexture::RGBA16F; return true;
    case QTC_RHI_RGBA32F: *out = QRhiTexture::RGBA32F; return true;
    case QTC_RHI_R32F: *out = QRhiTexture::R32F; return true;
    default: return false;
    }
}
int bytes_per_pixel(int f) {
    switch (f) {
    case QTC_RHI_R8: return 1;
    case QTC_RHI_RGBA16F: return 8;
    case QTC_RHI_RGBA32F: return 16;
    default: return 4;
    }
}
struct AttrInfo {
    QRhiVertexInputAttribute::Format f;
    int bytes;
    int kind;   // 0 float, 1 signed int, 2 unsigned int
};
bool attr_info(int f, AttrInfo *a) {
    switch (f) {
    case QTC_RHI_FLOAT: *a = {QRhiVertexInputAttribute::Float, 4, 0}; return true;
    case QTC_RHI_FLOAT2: *a = {QRhiVertexInputAttribute::Float2, 8, 0}; return true;
    case QTC_RHI_FLOAT3: *a = {QRhiVertexInputAttribute::Float3, 12, 0}; return true;
    case QTC_RHI_FLOAT4: *a = {QRhiVertexInputAttribute::Float4, 16, 0}; return true;
    case QTC_RHI_UNORM_BYTE4: *a = {QRhiVertexInputAttribute::UNormByte4, 4, 0}; return true;
    case QTC_RHI_INT: *a = {QRhiVertexInputAttribute::SInt, 4, 1}; return true;
    case QTC_RHI_UINT: *a = {QRhiVertexInputAttribute::UInt, 4, 2}; return true;
    default: return false;
    }
}
// 0 float, 1 signed, 2 unsigned; -1 for what a vertex input cannot be here (matrices, doubles, ...)
int var_kind(QShaderDescription::VariableType t) {
    switch (t) {
    case QShaderDescription::Float: case QShaderDescription::Vec2: case QShaderDescription::Vec3: case QShaderDescription::Vec4:
        return 0;
    case QShaderDescription::Int: case QShaderDescription::Int2: case QShaderDescription::Int3: case QShaderDescription::Int4:
        return 1;
    case QShaderDescription::Uint: case QShaderDescription::Uint2: case QShaderDescription::Uint3: case QShaderDescription::Uint4:
        return 2;
    default:
        return -1;
    }
}
QRhiShaderResourceBinding::StageFlags stages_of(uint32_t s) {
    QRhiShaderResourceBinding::StageFlags f;
    if (s & QTC_RHI_VERTEX) f |= QRhiShaderResourceBinding::VertexStage;
    if (s & QTC_RHI_FRAGMENT) f |= QRhiShaderResourceBinding::FragmentStage;
    if (s & QTC_RHI_COMPUTE) f |= QRhiShaderResourceBinding::ComputeStage;
    return f;
}
QShaderDescription::ImageFormat image_format_of(int f) {
    switch (f) {
    case QTC_RHI_RGBA8: return QShaderDescription::ImageFormatRgba8;
    case QTC_RHI_R8: return QShaderDescription::ImageFormatR8;
    case QTC_RHI_RGBA16F: return QShaderDescription::ImageFormatRgba16f;
    case QTC_RHI_RGBA32F: return QShaderDescription::ImageFormatRgba32f;
    case QTC_RHI_R32F: return QShaderDescription::ImageFormatR32f;
    default: return QShaderDescription::ImageFormatUnknown;
    }
}

int limit(QRhi *rhi, QRhi::ResourceLimit l, int def) {
    const int v = rhi->resourceLimit(l);
    return v > 0 ? v : def;
}

// ------------------------------------------------------------------ checks

qtc_status need_frame(Ctx *c, const char *what) {
    if (c->in_frame) return QTC_OK;
    return fail(QTC_ERR_STATE, c->owned ? QStringLiteral("%1: no frame is being recorded (qtc_rhi_begin_frame)").arg(QLatin1StringView(what))
                                        : QStringLiteral("%1: a widget's RHI records inside the widget's initialize or render override").arg(QLatin1StringView(what)));
}

Ctx *frame_ctx(uint64_t rhi, const char *what, qtc_status *err) {
    Ctx *c = rhi_ctx(rhi, what, err);
    if (!c) return nullptr;
    if ((*err = need_frame(c, what))) return nullptr;
    if (c->external_cmds) {
        *err = fail(QTC_ERR_STATE, QStringLiteral("%1: native commands are being recorded (qtc_rhi_end_external first)").arg(QLatin1StringView(what)));
        return nullptr;
    }
    return c;
}

// The largest index in [first, first + count) of the indices the GPU will see.
bool max_index(RObj *b, int64_t byte_offset, int format, int64_t first, int64_t count, uint64_t *out) {
    const int isz = format ? 4 : 2;
    if (first < 0 || count < 0 || byte_offset < 0) return false;
    const int64_t begin = byte_offset + first * isz, end = begin + count * isz;
    if (begin < 0 || end > b->shadow.size()) return false;
    if (b->mx_key[0] == begin && b->mx_key[1] == end && b->mx_key[2] == isz) {
        *out = b->mx_val;
        return true;
    }
    uint64_t m = 0;
    const char *d = b->shadow.constData();
    if (isz == 2) {
        for (int64_t p = begin; p < end; p += 2) {
            uint16_t x;
            std::memcpy(&x, d + p, 2);
            if (x > m) m = x;
        }
    } else {
        for (int64_t p = begin; p < end; p += 4) {
            uint32_t x;
            std::memcpy(&x, d + p, 4);
            if (x > m) m = x;
        }
    }
    b->mx_key[0] = begin;
    b->mx_key[1] = end;
    b->mx_key[2] = isz;
    b->mx_val = m;
    *out = m;
    return true;
}

// The resources the shader declares are bound, of the kind, size, format and access it needs, and
// visible to its stage.
qtc_status check_bindings(const QShader &sh, uint32_t stage, const RObj *s, const char *what) {
    const QShaderDescription d = sh.description();
    const QLatin1StringView w(what);
    auto bound = [&](int binding) -> const Bind * {
        for (const Bind &b : s->binds)
            if (b.binding == binding) return &b;
        return nullptr;
    };
    auto visible = [&](const Bind *b) { return (b->stages & stage) != 0; };
    if (!d.pushConstantBlocks().isEmpty())
        return fail(QTC_ERR_UNSUPPORTED, QStringLiteral("%1: push constants are not supported (use a uniform block)").arg(w));
    if (!d.separateImages().isEmpty() || !d.separateSamplers().isEmpty())
        return fail(QTC_ERR_UNSUPPORTED, QStringLiteral("%1: separate textures and samplers are not supported (use sampler2D)").arg(w));
    for (const QShaderDescription::UniformBlock &u : d.uniformBlocks()) {
        if (u.descriptorSet > 0 || u.binding < 0)
            return fail(QTC_ERR_UNSUPPORTED, QStringLiteral("%1: uniform block %2 needs set 0 and a binding number").arg(w, QString::fromUtf8(u.blockName)));
        const Bind *b = bound(u.binding);
        if (!b || b->type != 1)
            return fail(QTC_ERR_BAD_ARGS, QStringLiteral("%1: the shader's uniform block %2 (binding %3) has no uniform buffer bound").arg(w, QString::fromUtf8(u.blockName)).arg(u.binding));
        if (!visible(b)) return fail(QTC_ERR_BAD_ARGS, QStringLiteral("%1: binding %2 is not visible to the shader's stage").arg(w).arg(u.binding));
        if (b->size < u.size)
            return fail(QTC_ERR_BAD_ARGS, QStringLiteral("%1: binding %2 has %3 bytes, the shader reads %4").arg(w).arg(u.binding).arg(b->size).arg(u.size));
    }
    for (const QShaderDescription::InOutVariable &v : d.combinedImageSamplers()) {
        if (v.descriptorSet > 0 || v.binding < 0 || !v.arrayDims.isEmpty())
            return fail(QTC_ERR_UNSUPPORTED, QStringLiteral("%1: sampler %2: arrays, and sets other than 0, are not supported").arg(w, QString::fromUtf8(v.name)));
        if (v.type != QShaderDescription::Sampler2D)
            return fail(QTC_ERR_UNSUPPORTED, QStringLiteral("%1: sampler %2: only sampler2D is supported").arg(w, QString::fromUtf8(v.name)));
        const Bind *b = bound(v.binding);
        if (!b || b->type != 2)
            return fail(QTC_ERR_BAD_ARGS, QStringLiteral("%1: the shader's sampler %2 (binding %3) has no texture bound").arg(w, QString::fromUtf8(v.name)).arg(v.binding));
        if (!visible(b)) return fail(QTC_ERR_BAD_ARGS, QStringLiteral("%1: binding %2 is not visible to the shader's stage").arg(w).arg(v.binding));
    }
    for (const QShaderDescription::StorageBlock &sb : d.storageBlocks()) {
        if (sb.descriptorSet > 0 || sb.binding < 0)
            return fail(QTC_ERR_UNSUPPORTED, QStringLiteral("%1: storage block %2 needs set 0 and a binding number").arg(w, QString::fromUtf8(sb.blockName)));
        const Bind *b = bound(sb.binding);
        if (!b || b->type != 3)
            return fail(QTC_ERR_BAD_ARGS, QStringLiteral("%1: the shader's storage block %2 (binding %3) has no storage buffer bound").arg(w, QString::fromUtf8(sb.blockName)).arg(sb.binding));
        if (!visible(b)) return fail(QTC_ERR_BAD_ARGS, QStringLiteral("%1: binding %2 is not visible to the shader's stage").arg(w).arg(sb.binding));
        if (b->size < sb.knownSize)
            return fail(QTC_ERR_BAD_ARGS, QStringLiteral("%1: binding %2 has %3 bytes, the shader's block needs at least %4").arg(w).arg(sb.binding).arg(b->size).arg(sb.knownSize));
        const int q = block_qualifiers(sb);
        if (q >= 0) {
            if (!(q & QShaderDescription::QualifierReadOnly) && b->access == QTC_RHI_LOAD)
                return fail(QTC_ERR_BAD_ARGS, QStringLiteral("%1: the shader may write binding %2 (declare it readonly, or bind it with QTC_RHI_STORE or QTC_RHI_LOAD_STORE)").arg(w).arg(sb.binding));
            if (!(q & QShaderDescription::QualifierWriteOnly) && b->access == QTC_RHI_STORE)
                return fail(QTC_ERR_BAD_ARGS, QStringLiteral("%1: the shader may read binding %2 (declare it writeonly, or bind it with QTC_RHI_LOAD or QTC_RHI_LOAD_STORE)").arg(w).arg(sb.binding));
        }
    }
    for (const QShaderDescription::InOutVariable &v : d.storageImages()) {
        if (v.descriptorSet > 0 || v.binding < 0 || !v.arrayDims.isEmpty())
            return fail(QTC_ERR_UNSUPPORTED, QStringLiteral("%1: image %2: arrays, and sets other than 0, are not supported").arg(w, QString::fromUtf8(v.name)));
        if (v.type != QShaderDescription::Image2D)
            return fail(QTC_ERR_UNSUPPORTED, QStringLiteral("%1: image %2: only image2D is supported").arg(w, QString::fromUtf8(v.name)));
        const Bind *b = bound(v.binding);
        if (!b || b->type != 4)
            return fail(QTC_ERR_BAD_ARGS, QStringLiteral("%1: the shader's image %2 (binding %3) has no image bound").arg(w, QString::fromUtf8(v.name)).arg(v.binding));
        if (!visible(b)) return fail(QTC_ERR_BAD_ARGS, QStringLiteral("%1: binding %2 is not visible to the shader's stage").arg(w).arg(v.binding));
        const RObj *t = find(b->res, RK_TEXTURE);
        if (!t) return fail(QTC_ERR_STALE_HANDLE, QStringLiteral("%1: the texture of binding %2 is gone").arg(w).arg(v.binding));
        if (image_format_of(t->format) != v.imageFormat)
            return fail(QTC_ERR_BAD_ARGS, QStringLiteral("%1: image %2: the texture's format is not the shader's").arg(w, QString::fromUtf8(v.name)));
        if (!v.imageFlags.testFlag(QShaderDescription::ReadOnlyImage) && b->access == QTC_RHI_LOAD)
            return fail(QTC_ERR_BAD_ARGS, QStringLiteral("%1: the shader may write image binding %2 (readonly, or QTC_RHI_STORE / _LOAD_STORE)").arg(w).arg(v.binding));
        if (!v.imageFlags.testFlag(QShaderDescription::WriteOnlyImage) && b->access == QTC_RHI_STORE)
            return fail(QTC_ERR_BAD_ARGS, QStringLiteral("%1: the shader may read image binding %2 (writeonly, or QTC_RHI_LOAD / _LOAD_STORE)").arg(w).arg(v.binding));
    }
    return QTC_OK;
}

// Every resource the bindings name still exists (a destroyed one would leave Qt a dangling pointer).
qtc_status check_alive(const RObj *s, const char *what) {
    for (const Bind &b : s->binds) {
        if (!find(b.res) || (b.type == 2 && !find(b.sampler, RK_SAMPLER)))
            return fail(QTC_ERR_STALE_HANDLE, QStringLiteral("%1: a resource of binding %2 is gone").arg(QLatin1StringView(what)).arg(b.binding));
    }
    return QTC_OK;
}

// A swapchain's window is losing its native surface: the swapchain lets go of it first.
class ScFilter : public QObject {
public:
    uint64_t sc = 0;
    bool eventFilter(QObject *, QEvent *e) override {
        if (e->type() != QEvent::PlatformSurface ||
            static_cast<QPlatformSurfaceEvent *>(e)->surfaceEventType() != QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
            return false;
        RObj *o = find(sc, RK_SC);
        if (!o) return false;
        if (Ctx *c = ctx_quiet(o->rhi); c && c->in_frame && c->frame_sc == sc) abandon_frame(c);
        if ((o = find(sc, RK_SC))) {
            static_cast<QRhiSwapChain *>(o->res)->destroy();
            o->built = false;
        }
        return false;
    }
};

bool draws_itself(QWindow *w) {
#ifdef QTC_HAS_MODULE_Quick
    if (qobject_cast<QQuickWindow *>(w)) return true;
#endif
    return w->inherits("QWidgetWindow") || w->inherits("QPaintDeviceWindow");
}

}  // namespace
#endif  // QTC_RHI

}  // namespace qtc

using namespace qtc;

// ================================================================= Qt Quick

QTC_API qtc_status qtc_quick_set_graphics_api(int32_t api) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
#ifdef QTC_HAS_MODULE_Quick
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
#else
        (void)api;
        return fail(QTC_ERR_UNSUPPORTED, "Qt Quick is not bound in this build");
#endif
    });
}

#ifdef QTC_HAS_MODULE_Quick
static int32_t gapi_of(QSGRendererInterface::GraphicsApi g) {
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
#endif

QTC_API int32_t qtc_quick_graphics_api(qtc_obj window) {
    return guarded<int32_t>(QTC_ERR_STATE, [&]() -> int32_t {
#ifdef QTC_HAS_MODULE_Quick
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
#else
        (void)window;
        return fail(QTC_ERR_UNSUPPORTED, "Qt Quick is not bound in this build");
#endif
    });
}

// ================================================================= OpenGL

#ifdef QTC_HAS_GUI
#if QT_CONFIG(opengl)
#define QTC_GL 1
#endif
#endif

QTC_API void *qtc_gl_proc_address(qtc_str name) {
    return guarded<void *>(nullptr, [&]() -> void * {
#ifdef QTC_GL
        QByteArray n;
        if (!read_bytes(name, n, "name")) return nullptr;
        if (n.isEmpty() || n.size() > 256 || n.contains('\0')) {
            fail(QTC_ERR_INVALID_ARG, "qtc_gl_proc_address: a function name");
            return nullptr;
        }
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        if (!ctx) {
            fail(QTC_ERR_STATE, "qtc_gl_proc_address: no OpenGL context is current on this thread");
            return nullptr;
        }
        void *p = reinterpret_cast<void *>(ctx->getProcAddress(n));
        if (!p) fail(QTC_ERR_NOT_FOUND, QStringLiteral("qtc_gl_proc_address: no OpenGL function %1").arg(QString::fromLatin1(n)));
        return p;
#else
        (void)name;
        fail(QTC_ERR_UNSUPPORTED, "OpenGL is not in this build");
        return nullptr;
#endif
    });
}

#ifdef QTC_RHI
// ================================================================= RHI

QTC_API qtc_rhi qtc_rhi_create(int32_t backend, uint32_t flags) {
    return guarded<qtc_rhi>(0, [&]() -> qtc_rhi {
        if (app_kind() == QTC_APP_CORE) {
            fail(QTC_ERR_STATE, "qtc_rhi_create needs a GUI application");
            return 0;
        }
        if (backend < QTC_RHI_BACKEND_NULL || backend > QTC_RHI_BACKEND_METAL) {
            fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_create: not a QTC_RHI_BACKEND_ value");
            return 0;
        }
        if (flags & ~uint32_t(QTC_RHI_DEBUG)) {
            fail(QTC_ERR_INVALID_ARG, "qtc_rhi_create: unknown flags");
            return 0;
        }
        const bool debug = flags & QTC_RHI_DEBUG;
        auto c = std::make_unique<Ctx>();
        c->backend = backend;
        QRhi::Flags rf;
        if (debug) rf |= QRhi::EnableDebugMarkers;
        const char *missing = nullptr;
        switch (backend) {
        case QTC_RHI_BACKEND_NULL: {
            QRhiNullInitParams p;
            c->rhi = QRhi::create(QRhi::Null, &p, rf);
            break;
        }
        case QTC_RHI_BACKEND_OPENGL: {
#ifdef QTC_GL
            QRhiGles2InitParams p;
            c->fallback = QRhiGles2InitParams::newFallbackSurface();
            p.fallbackSurface = c->fallback;
            c->rhi = QRhi::create(QRhi::OpenGLES2, &p, rf);
            if (!c->rhi) {
                delete c->fallback;
                c->fallback = nullptr;
            }
#else
            missing = "OpenGL is not in this build";
#endif
            break;
        }
        case QTC_RHI_BACKEND_VULKAN: {
#ifdef QTC_VULKAN
            auto *v = new VkHolder;
            v->inst = new QVulkanInstance;
            // 1.1 or newer when the loader has it (some device extensions Qt asks for need it)
            v->inst->setApiVersion(v->inst->supportedApiVersion());
            v->inst->setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
            if (debug && v->inst->supportedLayers().contains("VK_LAYER_KHRONOS_validation"))
                v->inst->setLayers({"VK_LAYER_KHRONOS_validation"});
            if (v->inst->create()) {
                QRhiVulkanInitParams p;
                p.inst = v->inst;
                c->rhi = QRhi::create(QRhi::Vulkan, &p, rf);
            }
            if (c->rhi) c->vk = v;
            else vk_free(v);
#else
            missing = "Vulkan is not in this build";
#endif
            break;
        }
        case QTC_RHI_BACKEND_D3D11: {
#ifdef Q_OS_WIN
            QRhiD3D11InitParams p;
            p.enableDebugLayer = debug;
            c->rhi = QRhi::create(QRhi::D3D11, &p, rf);
#else
            missing = "Direct3D exists on Windows only";
#endif
            break;
        }
        case QTC_RHI_BACKEND_D3D12: {
#ifdef Q_OS_WIN
            QRhiD3D12InitParams p;
            p.enableDebugLayer = debug;
            c->rhi = QRhi::create(QRhi::D3D12, &p, rf);
#else
            missing = "Direct3D exists on Windows only";
#endif
            break;
        }
        case QTC_RHI_BACKEND_METAL: {
#if QT_CONFIG(metal)
            QRhiMetalInitParams p;
            c->rhi = QRhi::create(QRhi::Metal, &p, rf);
#else
            missing = "Metal exists on Apple platforms only";
#endif
            break;
        }
        }
        if (missing) {
            fail(QTC_ERR_UNSUPPORTED, QStringLiteral("qtc_rhi_create: %1").arg(QLatin1StringView(missing)));
            return 0;
        }
        if (!c->rhi) {
            fail(QTC_ERR_UNSUPPORTED, QStringLiteral("qtc_rhi_create: no device for the backend here (backend %1)").arg(backend));
            return 0;
        }
        const uint64_t h = alloc(RK_RHI, 0);
        if (!h) {
            delete c->rhi;
            delete c->fallback;
#ifdef QTC_VULKAN
            vk_free(c->vk);
#endif
            fail(QTC_ERR_LIMIT, "too many RHI objects");
            return 0;
        }
        find(h, RK_RHI)->ctx = c.release();
        return h;
    });
}

QTC_API qtc_status qtc_rhi_destroy(qtc_rhi h) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        RObj *o = get(h, 0, "qtc_rhi_destroy", &err);
        if (!o) return err;
        if (o->kind == RK_RHI) {
            if (!o->ctx->owned) return fail(QTC_ERR_STATE, "qtc_rhi_destroy: a widget's RHI goes with its window");
            if (o->ctx->in_frame) return fail(QTC_ERR_STATE, "qtc_rhi_destroy: end the frame first");
        } else {
            if (o->refs > 0)
                return fail(QTC_ERR_STATE, QStringLiteral("qtc_rhi_destroy: the %1 is still used by %2 bindings, render targets or pipelines (destroy them first)")
                                               .arg(QLatin1StringView(kind_name(o->kind))).arg(o->refs));
            if (o->kind == RK_SC) {
                Ctx *c = ctx_quiet(o->rhi);
                if (c && c->in_frame && c->frame_sc == h) return fail(QTC_ERR_STATE, "qtc_rhi_destroy: a frame on the swapchain is being recorded");
            }
        }
        release(h);
        return QTC_OK;
    });
}

QTC_API int64_t qtc_rhi_live(void) {
    return guarded<int64_t>(QTC_ERR_STATE, [&]() -> int64_t { return T().live; });
}

QTC_API qtc_view qtc_rhi_info(qtc_rhi h) {
    return guarded<qtc_view>(qtc_view{"", 0}, [&]() -> qtc_view {
        qtc_status err = 0;
        Ctx *c = rhi_ctx(h, "qtc_rhi_info", &err);
        if (!c) return qtc_view{"", 0};
        QRhi *rhi = c->rhi;
        QJsonObject j;
        j.insert(QStringLiteral("backend"), QLatin1StringView(rhi->backendName()));
        j.insert(QStringLiteral("driver"), QString::fromUtf8(rhi->driverInfo().deviceName));
        j.insert(QStringLiteral("compute"), rhi->isFeatureSupported(QRhi::Compute));
        j.insert(QStringLiteral("instancing"), rhi->isFeatureSupported(QRhi::Instancing));
        j.insert(QStringLiteral("index32"), rhi->isFeatureSupported(QRhi::ElementIndexUint));
        j.insert(QStringLiteral("base_vertex"), rhi->isFeatureSupported(QRhi::BaseVertex));
        j.insert(QStringLiteral("base_instance"), rhi->isFeatureSupported(QRhi::BaseInstance));
        j.insert(QStringLiteral("int_attributes"), rhi->isFeatureSupported(QRhi::IntAttributes));
        j.insert(QStringLiteral("max_texture_size"), rhi->resourceLimit(QRhi::TextureSizeMax));
        j.insert(QStringLiteral("max_uniform_buffer"), rhi->resourceLimit(QRhi::MaxUniformBufferRange));
        j.insert(QStringLiteral("max_vertex_inputs"), rhi->resourceLimit(QRhi::MaxVertexInputs));
        j.insert(QStringLiteral("max_compute_groups"), rhi->resourceLimit(QRhi::MaxThreadGroupsPerDimension));
        j.insert(QStringLiteral("ubuf_alignment"), rhi->ubufAlignment());
        j.insert(QStringLiteral("y_up_in_ndc"), rhi->isYUpInNDC());
        j.insert(QStringLiteral("y_up_in_framebuffer"), rhi->isYUpInFramebuffer());
        QJsonArray m;
        const QMatrix4x4 cm = rhi->clipSpaceCorrMatrix();
        for (int i = 0; i < 16; i++) m.append(double(cm.constData()[i]));
        j.insert(QStringLiteral("clip_space_corr"), m);
        return view_of(QJsonDocument(j).toJson(QJsonDocument::Compact));
    });
}

QTC_API bool qtc_rhi_supports(qtc_rhi h, int32_t feature) {
    return guarded<bool>(false, [&]() -> bool {
        qtc_status err = 0;
        Ctx *c = rhi_ctx(h, "qtc_rhi_supports", &err);
        if (!c) return false;
        switch (feature) {
        case QTC_RHI_FEATURE_COMPUTE: return c->rhi->isFeatureSupported(QRhi::Compute);
        case QTC_RHI_FEATURE_INSTANCING: return c->rhi->isFeatureSupported(QRhi::Instancing);
        case QTC_RHI_FEATURE_INDEX32: return c->rhi->isFeatureSupported(QRhi::ElementIndexUint);
        case QTC_RHI_FEATURE_BASE_VERTEX: return c->rhi->isFeatureSupported(QRhi::BaseVertex);
        case QTC_RHI_FEATURE_BASE_INSTANCE: return c->rhi->isFeatureSupported(QRhi::BaseInstance);
        case QTC_RHI_FEATURE_INT_ATTRIBUTES: return c->rhi->isFeatureSupported(QRhi::IntAttributes);
        case QTC_RHI_FEATURE_READ_BACK_BUFFERS: return c->rhi->isFeatureSupported(QRhi::ReadBackNonUniformBuffer);
        default: fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_supports: not a QTC_RHI_FEATURE_ value"); return false;
        }
    });
}

QTC_API qtc_status qtc_rhi_finish(qtc_rhi h) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        Ctx *c = rhi_ctx(h, "qtc_rhi_finish", &err);
        if (!c) return err;
        if (c->in_frame || c->rhi->isRecordingFrame()) return fail(QTC_ERR_STATE, "qtc_rhi_finish: outside frames");
        if (c->rhi->finish() != QRhi::FrameOpSuccess) return fail(QTC_ERR_REJECTED, "qtc_rhi_finish: the device was lost");
        prune(c);
        return QTC_OK;
    });
}

// ------------------------------------------------------------------ shaders

static qtc_rhi shader_handle(const QShader &s) {
    const uint64_t h = alloc(RK_SHADER, 0);
    if (!h) {
        fail(QTC_ERR_LIMIT, "too many RHI objects");
        return 0;
    }
    find(h, RK_SHADER)->shader = s;
    return h;
}

QTC_API qtc_rhi qtc_rhi_shader_bake(uint32_t stage, qtc_str glsl) {
    return guarded<qtc_rhi>(0, [&]() -> qtc_rhi {
#ifdef QTC_BAKER
        QShader::Stage st;
        switch (stage) {
        case QTC_RHI_VERTEX: st = QShader::VertexStage; break;
        case QTC_RHI_FRAGMENT: st = QShader::FragmentStage; break;
        case QTC_RHI_COMPUTE: st = QShader::ComputeStage; break;
        default: fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_shader_bake: stage is QTC_RHI_VERTEX, _FRAGMENT or _COMPUTE"); return 0;
        }
        QByteArray src;
        if (!read_bytes(glsl, src, "glsl")) return 0;
        if (src.isEmpty() || src.size() > (1 << 22)) {
            fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_shader_bake: the source is empty or over 4 MiB");
            return 0;
        }
        // every backend of this platform: SPIR-V for Vulkan, GLSL for OpenGL (ES), and HLSL or MSL
        // where Direct3D or Metal exist; the legacy GLSL versions go when the shader needs more
        QList<QShaderBaker::GeneratedShader> extra;
#ifdef Q_OS_WIN
        extra.append({QShader::HlslShader, QShaderVersion(50)});
#endif
#if defined(Q_OS_MACOS) || defined(Q_OS_IOS)
        extra.append({QShader::MslShader, QShaderVersion(12)});
#endif
        QList<QList<QShaderBaker::GeneratedShader>> attempts;
        if (st == QShader::ComputeStage) {
            attempts.append({{QShader::SpirvShader, QShaderVersion(100)}, {QShader::GlslShader, QShaderVersion(310, QShaderVersion::GlslEs)},
                             {QShader::GlslShader, QShaderVersion(430)}});
            attempts.append({{QShader::SpirvShader, QShaderVersion(100)}, {QShader::GlslShader, QShaderVersion(430)}});
        } else {
            attempts.append({{QShader::SpirvShader, QShaderVersion(100)}, {QShader::GlslShader, QShaderVersion(100, QShaderVersion::GlslEs)},
                             {QShader::GlslShader, QShaderVersion(120)}, {QShader::GlslShader, QShaderVersion(150)},
                             {QShader::GlslShader, QShaderVersion(330)}});
            attempts.append({{QShader::SpirvShader, QShaderVersion(100)}, {QShader::GlslShader, QShaderVersion(300, QShaderVersion::GlslEs)},
                             {QShader::GlslShader, QShaderVersion(330)}});
        }
        QString first_error;
        for (QList<QShaderBaker::GeneratedShader> targets : attempts) {
            targets += extra;
            QShaderBaker b;
            b.setSourceString(src, st);
            b.setGeneratedShaders(targets);
            b.setGeneratedShaderVariants({QShader::StandardShader});
            const QShader s = b.bake();
            if (s.isValid()) return shader_handle(s);
            if (first_error.isEmpty()) first_error = b.errorMessage();
        }
        fail(QTC_ERR_INVALID_ARG, "qtc_rhi_shader_bake: " + first_error.left(4000));
        return 0;
#else
        (void)stage, (void)glsl;
        fail(QTC_ERR_UNSUPPORTED, "qtc_rhi_shader_bake needs Qt Shader Tools (load packages made by Qt's qsb tool with qtc_rhi_shader_load)");
        return 0;
#endif
    });
}

QTC_API qtc_rhi qtc_rhi_shader_load(qtc_str qsb) {
    return guarded<qtc_rhi>(0, [&]() -> qtc_rhi {
        QByteArray b;
        if (!read_bytes(qsb, b, "qsb")) return 0;
        if (b.size() > (1 << 26)) {
            fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_shader_load: over 64 MiB");
            return 0;
        }
        const QShader s = QShader::fromSerialized(b);
        const QShader::Stage st = s.stage();
        if (!s.isValid() || (st != QShader::VertexStage && st != QShader::FragmentStage && st != QShader::ComputeStage)) {
            fail(QTC_ERR_INVALID_ARG, "qtc_rhi_shader_load: not a .qsb package of a vertex, fragment or compute shader");
            return 0;
        }
        return shader_handle(s);
    });
}

QTC_API qtc_view qtc_rhi_shader_save(qtc_rhi shader) {
    return guarded<qtc_view>(qtc_view{"", 0}, [&]() -> qtc_view {
        qtc_status err = 0;
        RObj *s = get(shader, RK_SHADER, "qtc_rhi_shader_save", &err);
        if (!s) return qtc_view{"", 0};
        return view_of(s->shader.serialized());
    });
}

QTC_API qtc_view qtc_rhi_shader_reflect(qtc_rhi shader) {
    return guarded<qtc_view>(qtc_view{"", 0}, [&]() -> qtc_view {
        qtc_status err = 0;
        RObj *s = get(shader, RK_SHADER, "qtc_rhi_shader_reflect", &err);
        if (!s) return qtc_view{"", 0};
        return view_of(s->shader.description().toJson());
    });
}

// ------------------------------------------------------------------ resources

QTC_API qtc_rhi qtc_rhi_buffer(qtc_rhi rhi, int32_t type, uint32_t usage, int64_t size) {
    return guarded<qtc_rhi>(0, [&]() -> qtc_rhi {
        qtc_status err = 0;
        Ctx *c = rhi_ctx(rhi, "qtc_rhi_buffer", &err);
        if (!c) return 0;
        if (type < QTC_RHI_IMMUTABLE || type > QTC_RHI_DYNAMIC) { fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_buffer: type"); return 0; }
        const uint32_t all = QTC_RHI_VERTEX_BUFFER | QTC_RHI_INDEX_BUFFER | QTC_RHI_UNIFORM_BUFFER | QTC_RHI_STORAGE_BUFFER;
        if (!usage || (usage & ~all)) { fail(QTC_ERR_INVALID_ARG, "qtc_rhi_buffer: usage"); return 0; }
        if ((usage & QTC_RHI_UNIFORM_BUFFER) && (usage != QTC_RHI_UNIFORM_BUFFER || type != QTC_RHI_DYNAMIC)) {
            fail(QTC_ERR_INVALID_ARG, "qtc_rhi_buffer: a uniform buffer is QTC_RHI_DYNAMIC, with no other usage");
            return 0;
        }
        if ((usage & QTC_RHI_INDEX_BUFFER) && (type == QTC_RHI_DYNAMIC || (usage & QTC_RHI_STORAGE_BUFFER))) {
            fail(QTC_ERR_INVALID_ARG, "qtc_rhi_buffer: an index buffer is QTC_RHI_IMMUTABLE or QTC_RHI_STATIC, and not a storage buffer");
            return 0;
        }
        if ((usage & QTC_RHI_STORAGE_BUFFER) && type == QTC_RHI_DYNAMIC) {
            fail(QTC_ERR_INVALID_ARG, "qtc_rhi_buffer: a storage buffer is QTC_RHI_IMMUTABLE or QTC_RHI_STATIC");
            return 0;
        }
        if ((usage & QTC_RHI_STORAGE_BUFFER) && !c->rhi->isFeatureSupported(QRhi::Compute)) {
            fail(QTC_ERR_UNSUPPORTED, "qtc_rhi_buffer: storage buffers need compute support");
            return 0;
        }
        const bool index = usage & QTC_RHI_INDEX_BUFFER;
        const int64_t cap = index ? (int64_t(1) << 28) : (int64_t(1) << 30);
        if (size <= 0 || size > cap || !alloc_ok(size, index ? 3 : 1)) {
            fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("qtc_rhi_buffer: size must be in (0, %1] and within the allocation limit").arg(cap));
            return 0;
        }
        const int maxu = limit(c->rhi, QRhi::MaxUniformBufferRange, 65536);
        if ((usage & QTC_RHI_UNIFORM_BUFFER) && size > maxu) {
            fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("qtc_rhi_buffer: uniform buffers hold at most %1 bytes here").arg(maxu));
            return 0;
        }
        QRhiBuffer::UsageFlags u;
        if (usage & QTC_RHI_VERTEX_BUFFER) u |= QRhiBuffer::VertexBuffer;
        if (index) u |= QRhiBuffer::IndexBuffer;
        if (usage & QTC_RHI_UNIFORM_BUFFER) u |= QRhiBuffer::UniformBuffer;
        if (usage & QTC_RHI_STORAGE_BUFFER) u |= QRhiBuffer::StorageBuffer;
        const QRhiBuffer::Type t = type == QTC_RHI_IMMUTABLE ? QRhiBuffer::Immutable : type == QTC_RHI_STATIC ? QRhiBuffer::Static : QRhiBuffer::Dynamic;
        QRhiResourceUpdateBatch *batch = index ? pending(c) : nullptr;
        if (index && !batch) { fail(QTC_ERR_LIMIT, "qtc_rhi_buffer: no resource update batch is free"); return 0; }
        QRhiBuffer *b = c->rhi->newBuffer(t, u, quint32(size));
        if (!b->create()) {
            delete b;
            fail(QTC_ERR_REJECTED, "qtc_rhi_buffer: the backend refused the buffer");
            return 0;
        }
        const uint64_t h = child(rhi, RK_BUFFER);
        if (!h) { delete b; return 0; }
        RObj *o = find(h, RK_BUFFER);
        o->res = b;
        o->btype = type;
        o->usage = usage;
        o->size = size;
        if (index) {
            // the GPU's contents start undefined: zeros reach it with the next pass, and indexed
            // draws before that are refused
            o->staged = QByteArray(qsizetype(size), '\0');
            batch->uploadStaticBuffer(b, 0, quint32(size), o->staged.constData());
            c->staged_bufs.push_back(h);
        }
        return h;
    });
}

QTC_API qtc_rhi qtc_rhi_texture(qtc_rhi rhi, int32_t format, int32_t w, int32_t h, uint32_t flags) {
    return guarded<qtc_rhi>(0, [&]() -> qtc_rhi {
        qtc_status err = 0;
        Ctx *c = rhi_ctx(rhi, "qtc_rhi_texture", &err);
        if (!c) return 0;
        QRhiTexture::Format f;
        if (!tex_format(format, &f)) { fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_texture: format"); return 0; }
        const int maxs = limit(c->rhi, QRhi::TextureSizeMax, 4096);
        if (w <= 0 || h <= 0 || w > maxs || h > maxs || !alloc_ok(int64_t(w) * h, bytes_per_pixel(format) * 2)) {
            fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("qtc_rhi_texture: the size must be in [1, %1] and within the allocation limit").arg(maxs));
            return 0;
        }
        if (flags & ~uint32_t(QTC_RHI_RENDER_TARGET | QTC_RHI_READABLE | QTC_RHI_STORAGE_IMAGE)) { fail(QTC_ERR_INVALID_ARG, "qtc_rhi_texture: flags"); return 0; }
        QRhiTexture::Flags tf;
        if (flags & QTC_RHI_RENDER_TARGET) tf |= QRhiTexture::RenderTarget;
        if (flags & QTC_RHI_READABLE) tf |= QRhiTexture::UsedAsTransferSource;
        if (flags & QTC_RHI_STORAGE_IMAGE) {
            if (format == QTC_RHI_BGRA8) { fail(QTC_ERR_UNSUPPORTED, "qtc_rhi_texture: a BGRA8 texture cannot be a storage image"); return 0; }
            if (!c->rhi->isFeatureSupported(QRhi::Compute)) { fail(QTC_ERR_UNSUPPORTED, "qtc_rhi_texture: storage images need compute support"); return 0; }
            tf |= QRhiTexture::UsedWithLoadStore;
        }
        if (!c->rhi->isTextureFormatSupported(f, tf)) { fail(QTC_ERR_UNSUPPORTED, "qtc_rhi_texture: the backend lacks the format (with these flags)"); return 0; }
        QRhiTexture *t = c->rhi->newTexture(f, QSize(w, h), 1, tf);
        if (!t->create()) {
            delete t;
            fail(QTC_ERR_REJECTED, "qtc_rhi_texture: the backend refused the texture");
            return 0;
        }
        const uint64_t hd = child(rhi, RK_TEXTURE);
        if (!hd) { delete t; return 0; }
        RObj *o = find(hd, RK_TEXTURE);
        o->res = t;
        o->format = format;
        o->w = w;
        o->h = h;
        o->tflags = flags;
        return hd;
    });
}

QTC_API qtc_rhi qtc_rhi_sampler(qtc_rhi rhi, bool linear, bool repeat) {
    return guarded<qtc_rhi>(0, [&]() -> qtc_rhi {
        qtc_status err = 0;
        Ctx *c = rhi_ctx(rhi, "qtc_rhi_sampler", &err);
        if (!c) return 0;
        const QRhiSampler::Filter f = linear ? QRhiSampler::Linear : QRhiSampler::Nearest;
        const QRhiSampler::AddressMode a = repeat ? QRhiSampler::Repeat : QRhiSampler::ClampToEdge;
        QRhiSampler *s = c->rhi->newSampler(f, f, QRhiSampler::None, a, a);
        if (!s->create()) { delete s; fail(QTC_ERR_REJECTED, "qtc_rhi_sampler: refused"); return 0; }
        const uint64_t h = child(rhi, RK_SAMPLER);
        if (!h) { delete s; return 0; }
        find(h, RK_SAMPLER)->res = s;
        return h;
    });
}

QTC_API qtc_rhi qtc_rhi_render_target(qtc_rhi texture, bool depth) {
    return guarded<qtc_rhi>(0, [&]() -> qtc_rhi {
        qtc_status err = 0;
        RObj *t = get(texture, RK_TEXTURE, "qtc_rhi_render_target", &err);
        if (!t) return 0;
        if (!(t->tflags & QTC_RHI_RENDER_TARGET)) { fail(QTC_ERR_STATE, "qtc_rhi_render_target: the texture was not made with QTC_RHI_RENDER_TARGET"); return 0; }
        const uint64_t rhi_h = t->rhi;
        const int tw = t->w, th = t->h;
        auto *qt = static_cast<QRhiTexture *>(t->res);
        QRhi *rhi = ctx_quiet(rhi_h)->rhi;
        QRhiTextureRenderTargetDescription desc{QRhiColorAttachment(qt)};
        QRhiRenderBuffer *ds = nullptr;
        if (depth) {
            ds = rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, QSize(tw, th));
            if (!ds->create()) { delete ds; fail(QTC_ERR_REJECTED, "qtc_rhi_render_target: the depth buffer was refused"); return 0; }
            desc.setDepthStencilBuffer(ds);
        }
        QRhiTextureRenderTarget *rt = rhi->newTextureRenderTarget(desc);
        QRhiRenderPassDescriptor *rp = rt->newCompatibleRenderPassDescriptor();
        rt->setRenderPassDescriptor(rp);
        if (!rt->create()) {
            delete rt;
            delete rp;
            delete ds;
            fail(QTC_ERR_REJECTED, "qtc_rhi_render_target: refused");
            return 0;
        }
        const uint64_t h = child(rhi_h, RK_RT);
        if (!h) { delete rt; delete rp; delete ds; return 0; }
        RObj *o = find(h, RK_RT);
        o->res = rt;
        o->rp = rp;
        o->depth = ds;
        o->has_depth = ds != nullptr;
        o->w = tw;
        o->h = th;
        hold(h, texture);
        return h;
    });
}

QTC_API qtc_rhi qtc_rhi_swapchain(qtc_rhi rhi, qtc_obj window, uint32_t flags) {
    return guarded<qtc_rhi>(0, [&]() -> qtc_rhi {
        qtc_status err = 0;
        Ctx *c = rhi_ctx(rhi, "qtc_rhi_swapchain", &err);
        if (!c) return 0;
        if (!c->owned) { fail(QTC_ERR_STATE, "qtc_rhi_swapchain: a widget's RHI draws into its widget"); return 0; }
        if (flags & ~uint32_t(QTC_RHI_SC_DEPTH | QTC_RHI_SC_READABLE | QTC_RHI_SC_NO_VSYNC)) { fail(QTC_ERR_INVALID_ARG, "qtc_rhi_swapchain: flags"); return 0; }
        QObject *wo = reg().object(window, &err);
        if (!wo) { fail(err, "qtc_rhi_swapchain"); return 0; }
        auto *w = qobject_cast<QWindow *>(wo);
        if (!w) { fail(QTC_ERR_WRONG_TYPE, "qtc_rhi_swapchain: not a QWindow"); return 0; }
        if (draws_itself(w)) { fail(QTC_ERR_WRONG_TYPE, "qtc_rhi_swapchain: the window draws itself (widgets, Qt Quick, QRasterWindow, QOpenGLWindow): use a plain QWindow"); return 0; }
        for (const RObj &x : T().t)
            if (x.kind == RK_SC && x.window == w) { fail(QTC_ERR_STATE, "qtc_rhi_swapchain: the window has a swapchain already"); return 0; }
        QSurface::SurfaceType need = w->surfaceType();
        switch (c->backend) {
        case QTC_RHI_BACKEND_OPENGL: need = QSurface::OpenGLSurface; break;
        case QTC_RHI_BACKEND_VULKAN: need = QSurface::VulkanSurface; break;
        case QTC_RHI_BACKEND_D3D11:
        case QTC_RHI_BACKEND_D3D12: need = QSurface::Direct3DSurface; break;
        case QTC_RHI_BACKEND_METAL: need = QSurface::MetalSurface; break;
        default: break;
        }
        if (w->handle()) {
            if (w->surfaceType() != need) { fail(QTC_ERR_STATE, "qtc_rhi_swapchain: the window exists with another surface type (make the swapchain before showing it)"); return 0; }
#ifdef QTC_VULKAN
            if (c->backend == QTC_RHI_BACKEND_VULKAN && w->vulkanInstance() != c->vk->inst) {
                fail(QTC_ERR_STATE, "qtc_rhi_swapchain: the window exists with another Vulkan instance (make the swapchain before showing it)");
                return 0;
            }
#endif
        } else {
            w->setSurfaceType(need);
#ifdef QTC_VULKAN
            if (c->backend == QTC_RHI_BACKEND_VULKAN) {
                w->setVulkanInstance(c->vk->inst);
                c->vk->windows.push_back(w);
            }
#endif
        }
        // the native window (hidden until shown) gives the surface whose format the swapchain's
        // render pass needs
        if (!w->handle()) w->create();
        if (!w->handle()) { fail(QTC_ERR_REJECTED, "qtc_rhi_swapchain: the window could not be created"); return 0; }
        QRhiSwapChain *sc = c->rhi->newSwapChain();
        sc->setWindow(w);
        QRhiSwapChain::Flags sf;
        if (flags & QTC_RHI_SC_READABLE) sf |= QRhiSwapChain::UsedAsTransferSource;
        if (flags & QTC_RHI_SC_NO_VSYNC) sf |= QRhiSwapChain::NoVSync;
        sc->setFlags(sf);
        QRhiRenderBuffer *ds = nullptr;
        if (flags & QTC_RHI_SC_DEPTH) {
            ds = c->rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, QSize(), 1, QRhiRenderBuffer::UsedWithSwapChainOnly);
            sc->setDepthStencil(ds);
        }
        QRhiRenderPassDescriptor *rp = sc->newCompatibleRenderPassDescriptor();
        if (!rp) {
            delete sc;
            delete ds;
            fail(QTC_ERR_REJECTED, "qtc_rhi_swapchain: no surface for the window here");
            return 0;
        }
        sc->setRenderPassDescriptor(rp);
        const uint64_t h = child(rhi, RK_SC);
        if (!h) { delete sc; delete ds; delete rp; return 0; }
        auto *filter = new ScFilter;
        filter->sc = h;
        w->installEventFilter(filter);
        RObj *o = find(h, RK_SC);
        o->res = sc;
        o->depth = ds;
        o->rp = rp;
        o->has_depth = ds != nullptr;
        o->window = w;
        o->filter = filter;
        o->scflags = flags;
        return h;   // made for the window at the first frame, when it can show one
    });
}

// ------------------------------------------------------------------ bindings

QTC_API qtc_rhi qtc_rhi_bindings(qtc_rhi rhi) {
    return guarded<qtc_rhi>(0, [&]() -> qtc_rhi {
        qtc_status err = 0;
        if (!rhi_ctx(rhi, "qtc_rhi_bindings", &err)) return 0;
        return child(rhi, RK_SRB);
    });
}

static qtc_status add_bind(qtc_rhi srb, const Bind &b, const char *what) {
    qtc_status err = 0;
    RObj *s = get(srb, RK_SRB, what, &err);
    if (!s) return err;
    const QLatin1StringView w(what);
    if (s->created) return fail(QTC_ERR_STATE, QStringLiteral("%1: the bindings are created already").arg(w));
    if (b.binding < 0 || b.binding > 63) return fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("%1: binding must be in [0, 63]").arg(w));
    if (!b.stages || (b.stages & ~uint32_t(QTC_RHI_VERTEX | QTC_RHI_FRAGMENT | QTC_RHI_COMPUTE)))
        return fail(QTC_ERR_INVALID_ARG, QStringLiteral("%1: stages are QTC_RHI_VERTEX, _FRAGMENT and _COMPUTE bits").arg(w));
    if (s->binds.size() >= 64) return fail(QTC_ERR_LIMIT, QStringLiteral("%1: at most 64 bindings").arg(w));
    for (const Bind &x : s->binds)
        if (x.binding == b.binding) return fail(QTC_ERR_STATE, QStringLiteral("%1: binding %2 is used already").arg(w).arg(b.binding));
    s->binds.push_back(b);
    hold(srb, b.res);
    if (b.sampler) hold(srb, b.sampler);
    return QTC_OK;
}

QTC_API qtc_status qtc_rhi_bind_uniform(qtc_rhi srb, int32_t binding, uint32_t stages, qtc_rhi buffer, int64_t offset, int64_t size) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        RObj *s = get(srb, RK_SRB, "qtc_rhi_bind_uniform", &err);
        if (!s) return err;
        const uint64_t srb_rhi = s->rhi;
        RObj *b = get(buffer, RK_BUFFER, "qtc_rhi_bind_uniform", &err);
        if (!b) return err;
        if (b->rhi != srb_rhi) return fail(QTC_ERR_BAD_ARGS, "qtc_rhi_bind_uniform: the buffer belongs to another RHI");
        if (!(b->usage & QTC_RHI_UNIFORM_BUFFER)) return fail(QTC_ERR_BAD_ARGS, "qtc_rhi_bind_uniform: not a uniform buffer");
        if (size == 0 && offset >= 0 && offset <= b->size) size = b->size - offset;   // 0: to the end
        const int align = ctx_quiet(srb_rhi)->rhi->ubufAlignment();
        if (offset < 0 || size <= 0 || offset > b->size || size > b->size - offset || (align > 0 && offset % align))
            return fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("qtc_rhi_bind_uniform: the range must lie in the buffer, at an offset that is a multiple of %1").arg(align));
        Bind x;
        x.binding = binding;
        x.stages = stages;
        x.type = 1;
        x.res = buffer;
        x.offset = offset;
        x.size = size;
        return add_bind(srb, x, "qtc_rhi_bind_uniform");
    });
}

QTC_API qtc_status qtc_rhi_bind_texture(qtc_rhi srb, int32_t binding, uint32_t stages, qtc_rhi texture, qtc_rhi sampler) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        RObj *s = get(srb, RK_SRB, "qtc_rhi_bind_texture", &err);
        if (!s) return err;
        const uint64_t srb_rhi = s->rhi;
        RObj *t = get(texture, RK_TEXTURE, "qtc_rhi_bind_texture", &err);
        if (!t) return err;
        const uint64_t t_rhi = t->rhi;
        RObj *sm = get(sampler, RK_SAMPLER, "qtc_rhi_bind_texture", &err);
        if (!sm) return err;
        if (t_rhi != srb_rhi || sm->rhi != srb_rhi) return fail(QTC_ERR_BAD_ARGS, "qtc_rhi_bind_texture: the texture or sampler belongs to another RHI");
        Bind x;
        x.binding = binding;
        x.stages = stages;
        x.type = 2;
        x.res = texture;
        x.sampler = sampler;
        return add_bind(srb, x, "qtc_rhi_bind_texture");
    });
}

QTC_API qtc_status qtc_rhi_bind_storage(qtc_rhi srb, int32_t binding, uint32_t stages, qtc_rhi buffer, int32_t access) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        RObj *s = get(srb, RK_SRB, "qtc_rhi_bind_storage", &err);
        if (!s) return err;
        const uint64_t srb_rhi = s->rhi;
        RObj *b = get(buffer, RK_BUFFER, "qtc_rhi_bind_storage", &err);
        if (!b) return err;
        if (b->rhi != srb_rhi) return fail(QTC_ERR_BAD_ARGS, "qtc_rhi_bind_storage: the buffer belongs to another RHI");
        if (!(b->usage & QTC_RHI_STORAGE_BUFFER)) return fail(QTC_ERR_BAD_ARGS, "qtc_rhi_bind_storage: not a storage buffer");
        if (access < QTC_RHI_LOAD || access > QTC_RHI_LOAD_STORE) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_bind_storage: access is QTC_RHI_LOAD, _STORE or _LOAD_STORE");
        Bind x;
        x.binding = binding;
        x.stages = stages;
        x.type = 3;
        x.res = buffer;
        x.size = b->size;
        x.access = access;
        return add_bind(srb, x, "qtc_rhi_bind_storage");
    });
}

QTC_API qtc_status qtc_rhi_bind_image(qtc_rhi srb, int32_t binding, uint32_t stages, qtc_rhi texture, int32_t access) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        RObj *s = get(srb, RK_SRB, "qtc_rhi_bind_image", &err);
        if (!s) return err;
        const uint64_t srb_rhi = s->rhi;
        RObj *t = get(texture, RK_TEXTURE, "qtc_rhi_bind_image", &err);
        if (!t) return err;
        if (t->rhi != srb_rhi) return fail(QTC_ERR_BAD_ARGS, "qtc_rhi_bind_image: the texture belongs to another RHI");
        if (!(t->tflags & QTC_RHI_STORAGE_IMAGE)) return fail(QTC_ERR_BAD_ARGS, "qtc_rhi_bind_image: the texture was not made with QTC_RHI_STORAGE_IMAGE");
        if (access < QTC_RHI_LOAD || access > QTC_RHI_LOAD_STORE) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_bind_image: access is QTC_RHI_LOAD, _STORE or _LOAD_STORE");
        Bind x;
        x.binding = binding;
        x.stages = stages;
        x.type = 4;
        x.res = texture;
        x.access = access;
        return add_bind(srb, x, "qtc_rhi_bind_image");
    });
}

QTC_API qtc_status qtc_rhi_bindings_create(qtc_rhi srb) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        RObj *s = get(srb, RK_SRB, "qtc_rhi_bindings_create", &err);
        if (!s) return err;
        if (s->created) return fail(QTC_ERR_STATE, "qtc_rhi_bindings_create: created already");
        if (qtc_status e = check_alive(s, "qtc_rhi_bindings_create")) return e;
        Ctx *c = ctx_quiet(s->rhi);
        QVarLengthArray<QRhiShaderResourceBinding, 8> bs;
        for (const Bind &b : s->binds) {
            const auto st = stages_of(b.stages);
            switch (b.type) {
            case 1:
                bs.append(QRhiShaderResourceBinding::uniformBuffer(b.binding, st, static_cast<QRhiBuffer *>(find(b.res)->res), quint32(b.offset), quint32(b.size)));
                break;
            case 2:
                bs.append(QRhiShaderResourceBinding::sampledTexture(b.binding, st, static_cast<QRhiTexture *>(find(b.res)->res),
                                                                    static_cast<QRhiSampler *>(find(b.sampler)->res)));
                break;
            case 3: {
                auto *qb = static_cast<QRhiBuffer *>(find(b.res)->res);
                if (b.access == QTC_RHI_LOAD) bs.append(QRhiShaderResourceBinding::bufferLoad(b.binding, st, qb));
                else if (b.access == QTC_RHI_STORE) bs.append(QRhiShaderResourceBinding::bufferStore(b.binding, st, qb));
                else bs.append(QRhiShaderResourceBinding::bufferLoadStore(b.binding, st, qb));
                break;
            }
            case 4: {
                auto *qt = static_cast<QRhiTexture *>(find(b.res)->res);
                if (b.access == QTC_RHI_LOAD) bs.append(QRhiShaderResourceBinding::imageLoad(b.binding, st, qt, 0));
                else if (b.access == QTC_RHI_STORE) bs.append(QRhiShaderResourceBinding::imageStore(b.binding, st, qt, 0));
                else bs.append(QRhiShaderResourceBinding::imageLoadStore(b.binding, st, qt, 0));
                break;
            }
            }
        }
        QRhiShaderResourceBindings *srbo = c->rhi->newShaderResourceBindings();
        srbo->setBindings(bs.cbegin(), bs.cend());
        if (!srbo->create()) {
            delete srbo;
            return fail(QTC_ERR_REJECTED, "qtc_rhi_bindings_create: refused");
        }
        s = find(srb, RK_SRB);
        s->res = srbo;
        s->created = true;
        return QTC_OK;
    });
}

// ------------------------------------------------------------------ pipelines

QTC_API qtc_rhi qtc_rhi_pipeline(qtc_rhi rhi) {
    return guarded<qtc_rhi>(0, [&]() -> qtc_rhi {
        qtc_status err = 0;
        if (!rhi_ctx(rhi, "qtc_rhi_pipeline", &err)) return 0;
        return child(rhi, RK_GPIPE);
    });
}

static RObj *open_pipeline(qtc_rhi p, const char *what, qtc_status *err) {
    RObj *o = get(p, RK_GPIPE, what, err);
    if (o && o->res) {
        *err = fail(QTC_ERR_STATE, QStringLiteral("%1: the pipeline is created already").arg(QLatin1StringView(what)));
        return nullptr;
    }
    return o;
}

QTC_API qtc_status qtc_rhi_pipeline_shaders(qtc_rhi p, qtc_rhi vs, qtc_rhi fs) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        RObj *o = open_pipeline(p, "qtc_rhi_pipeline_shaders", &err);
        if (!o) return err;
        RObj *v = get(vs, RK_SHADER, "qtc_rhi_pipeline_shaders: vertex", &err);
        if (!v) return err;
        RObj *f = get(fs, RK_SHADER, "qtc_rhi_pipeline_shaders: fragment", &err);
        if (!f) return err;
        if (v->shader.stage() != QShader::VertexStage || f->shader.stage() != QShader::FragmentStage)
            return fail(QTC_ERR_BAD_ARGS, "qtc_rhi_pipeline_shaders: a vertex shader, then a fragment shader");
        o->vs = vs;
        o->fs = fs;
        return QTC_OK;
    });
}

QTC_API qtc_status qtc_rhi_pipeline_vertex_binding(qtc_rhi p, int32_t stride, bool per_instance) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        RObj *o = open_pipeline(p, "qtc_rhi_pipeline_vertex_binding", &err);
        if (!o) return err;
        if (stride <= 0 || stride > 2048 || stride % 4) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_pipeline_vertex_binding: stride must be a multiple of 4 in [4, 2048]");
        if (o->strides.size() >= 16) return fail(QTC_ERR_LIMIT, "qtc_rhi_pipeline_vertex_binding: at most 16 bindings");
        o->strides.push_back(stride);
        o->per_instance.push_back(per_instance);
        return QTC_OK;
    });
}

QTC_API qtc_status qtc_rhi_pipeline_vertex_attribute(qtc_rhi p, int32_t binding, int32_t location, int32_t format, int32_t offset) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        RObj *o = open_pipeline(p, "qtc_rhi_pipeline_vertex_attribute", &err);
        if (!o) return err;
        if (binding < 0 || binding >= int(o->strides.size())) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_pipeline_vertex_attribute: no such vertex binding (add it first)");
        if (location < 0 || location > 15) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_pipeline_vertex_attribute: location must be in [0, 15]");
        AttrInfo a{};
        if (!attr_info(format, &a)) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_pipeline_vertex_attribute: format");
        if (offset < 0 || offset % 4 || offset + a.bytes > o->strides[size_t(binding)])
            return fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_pipeline_vertex_attribute: the attribute must lie inside the stride, at an offset that is a multiple of 4");
        for (const VAttr &x : o->attrs)
            if (x.location == location) return fail(QTC_ERR_STATE, "qtc_rhi_pipeline_vertex_attribute: the location is used already");
        o->attrs.push_back(VAttr{binding, location, format, offset});
        return QTC_OK;
    });
}

QTC_API qtc_status qtc_rhi_pipeline_options(qtc_rhi p, int32_t topology, int32_t cull, bool blend, bool depth_test, bool depth_write) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        RObj *o = open_pipeline(p, "qtc_rhi_pipeline_options", &err);
        if (!o) return err;
        if (topology < QTC_RHI_TRIANGLES || topology > QTC_RHI_POINTS) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_pipeline_options: topology");
        if (cull < QTC_RHI_CULL_NONE || cull > QTC_RHI_CULL_BACK) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_pipeline_options: cull");
        o->topology = topology;
        o->cull = cull;
        o->blend = blend;
        o->depth_test = depth_test;
        o->depth_write = depth_write;
        return QTC_OK;
    });
}

// The render pass descriptor and depth of a target handle: a render target, a swapchain, or a
// widget's target.
static bool target_rp(uint64_t target, uint64_t rhi, QRhiRenderPassDescriptor **rp, bool *has_depth, const char *what, qtc_status *err) {
    RObj *t = get(target, 0, what, err);
    if (!t) return false;
    if (t->kind != RK_RT && t->kind != RK_SC) {
        *err = fail(QTC_ERR_WRONG_TYPE, QStringLiteral("%1: a render target or a swapchain").arg(QLatin1StringView(what)));
        return false;
    }
    if (t->rhi != rhi) {
        *err = fail(QTC_ERR_BAD_ARGS, QStringLiteral("%1: the target belongs to another RHI").arg(QLatin1StringView(what)));
        return false;
    }
    if (!t->rp) {
        *err = fail(QTC_ERR_STATE, QStringLiteral("%1: the target has no render pass").arg(QLatin1StringView(what)));
        return false;
    }
    *rp = t->rp;
    *has_depth = t->has_depth;
    return true;
}

QTC_API qtc_status qtc_rhi_pipeline_create(qtc_rhi p, qtc_rhi bindings, qtc_rhi target) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        RObj *o = open_pipeline(p, "qtc_rhi_pipeline_create", &err);
        if (!o) return err;
        const uint64_t prhi = o->rhi;
        RObj *s = get(bindings, RK_SRB, "qtc_rhi_pipeline_create: bindings", &err);
        if (!s) return err;
        if (!s->created) return fail(QTC_ERR_STATE, "qtc_rhi_pipeline_create: create the bindings first");
        if (s->rhi != prhi) return fail(QTC_ERR_BAD_ARGS, "qtc_rhi_pipeline_create: the bindings belong to another RHI");
        if (qtc_status e = check_alive(s, "qtc_rhi_pipeline_create")) return e;
        QRhiRenderPassDescriptor *trp = nullptr;
        bool tdepth = false;
        if (!target_rp(target, prhi, &trp, &tdepth, "qtc_rhi_pipeline_create", &err)) return err;
        if (!o->vs || !o->fs) return fail(QTC_ERR_STATE, "qtc_rhi_pipeline_create: set the shaders first (qtc_rhi_pipeline_shaders)");
        RObj *v = get(o->vs, RK_SHADER, "qtc_rhi_pipeline_create: vertex shader", &err);
        if (!v) return err;
        RObj *f = get(o->fs, RK_SHADER, "qtc_rhi_pipeline_create: fragment shader", &err);
        if (!f) return err;
        const QShader vsh = v->shader, fsh = f->shader;
        QRhi *rhi = ctx_quiet(prhi)->rhi;
        const QShaderDescription vd = vsh.description(), fd = fsh.description();
        // vertex inputs: an attribute of the shader's kind at each location
        const int max_inputs = limit(rhi, QRhi::MaxVertexInputs, 16);
        for (const VAttr &a : o->attrs)
            if (a.location >= max_inputs) return fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("qtc_rhi_pipeline_create: location %1: the device has %2 vertex inputs").arg(a.location).arg(max_inputs));
        for (const QShaderDescription::InOutVariable &in : vd.inputVariables()) {
            const int k = var_kind(in.type);
            if (k < 0 || !in.arrayDims.isEmpty())
                return fail(QTC_ERR_UNSUPPORTED, QStringLiteral("qtc_rhi_pipeline_create: vertex input %1: matrices, doubles, booleans and arrays are not supported (use vectors)").arg(QString::fromUtf8(in.name)));
            const VAttr *a = nullptr;
            for (const VAttr &x : o->attrs)
                if (x.location == in.location) a = &x;
            if (!a) return fail(QTC_ERR_BAD_ARGS, QStringLiteral("qtc_rhi_pipeline_create: the vertex shader's input %1 (location %2) has no attribute").arg(QString::fromUtf8(in.name)).arg(in.location));
            AttrInfo ai{};
            attr_info(a->format, &ai);
            if (ai.kind != k)
                return fail(QTC_ERR_BAD_ARGS, QStringLiteral("qtc_rhi_pipeline_create: input %1 is %2 in the shader").arg(QString::fromUtf8(in.name),
                            k == 0 ? QStringLiteral("float") : k == 1 ? QStringLiteral("int (QTC_RHI_INT)") : QStringLiteral("uint (QTC_RHI_UINT)")));
            if (k && !rhi->isFeatureSupported(QRhi::IntAttributes))
                return fail(QTC_ERR_UNSUPPORTED, "qtc_rhi_pipeline_create: integer vertex inputs are not supported here");
        }
        // the fragment shader's inputs are the vertex shader's outputs
        const QList<QShaderDescription::InOutVariable> outs = vd.outputVariables();
        for (const QShaderDescription::InOutVariable &in : fd.inputVariables()) {
            const QShaderDescription::InOutVariable *out = nullptr;
            for (const QShaderDescription::InOutVariable &x : outs)
                if (x.location == in.location) out = &x;
            if (!out) return fail(QTC_ERR_BAD_ARGS, QStringLiteral("qtc_rhi_pipeline_create: the fragment shader's input %1 (location %2) is no output of the vertex shader").arg(QString::fromUtf8(in.name)).arg(in.location));
            if (out->type != in.type || out->arrayDims != in.arrayDims)
                return fail(QTC_ERR_BAD_ARGS, QStringLiteral("qtc_rhi_pipeline_create: location %1: the vertex shader's output and the fragment shader's input differ in type").arg(in.location));
        }
        if (o->topology == QTC_RHI_POINTS && writes_point_size(vd) == 0)
            return fail(QTC_ERR_BAD_ARGS, "qtc_rhi_pipeline_create: drawing points, the vertex shader writes gl_PointSize");
        if (qtc_status e = check_bindings(vsh, QTC_RHI_VERTEX, s, "qtc_rhi_pipeline_create")) return e;
        if (qtc_status e = check_bindings(fsh, QTC_RHI_FRAGMENT, s, "qtc_rhi_pipeline_create")) return e;
        QRhiGraphicsPipeline *ps = rhi->newGraphicsPipeline();
        ps->setShaderStages({{QRhiShaderStage::Vertex, vsh}, {QRhiShaderStage::Fragment, fsh}});
        QRhiVertexInputLayout layout;
        QVarLengthArray<QRhiVertexInputBinding, 4> vb;
        for (size_t i = 0; i < o->strides.size(); i++)
            vb.append(QRhiVertexInputBinding(quint32(o->strides[i]), o->per_instance[i] ? QRhiVertexInputBinding::PerInstance : QRhiVertexInputBinding::PerVertex));
        QVarLengthArray<QRhiVertexInputAttribute, 8> va;
        for (const VAttr &a : o->attrs) {
            AttrInfo ai{};
            attr_info(a.format, &ai);
            va.append(QRhiVertexInputAttribute(a.binding, a.location, ai.f, quint32(a.offset)));
        }
        layout.setBindings(vb.cbegin(), vb.cend());
        layout.setAttributes(va.cbegin(), va.cend());
        ps->setVertexInputLayout(layout);
        static const QRhiGraphicsPipeline::Topology tops[] = {QRhiGraphicsPipeline::Triangles, QRhiGraphicsPipeline::TriangleStrip,
                                                              QRhiGraphicsPipeline::Lines, QRhiGraphicsPipeline::LineStrip,
                                                              QRhiGraphicsPipeline::Points};
        ps->setTopology(tops[o->topology]);
        ps->setCullMode(o->cull == QTC_RHI_CULL_FRONT ? QRhiGraphicsPipeline::Front : o->cull == QTC_RHI_CULL_BACK ? QRhiGraphicsPipeline::Back : QRhiGraphicsPipeline::None);
        if (o->blend) {
            QRhiGraphicsPipeline::TargetBlend tb;
            tb.enable = true;   // premultiplied alpha: One, OneMinusSrcAlpha
            ps->setTargetBlends({tb});
        }
        ps->setDepthTest(o->depth_test && tdepth);
        ps->setDepthWrite(o->depth_write && tdepth);
        ps->setShaderResourceBindings(static_cast<QRhiShaderResourceBindings *>(s->res));
        QRhiRenderPassDescriptor *own = trp->newCompatibleRenderPassDescriptor();
        if (!own) {
            delete ps;
            return fail(QTC_ERR_REJECTED, "qtc_rhi_pipeline_create: the backend could not describe the target's render pass");
        }
        ps->setRenderPassDescriptor(own);
        if (!ps->create()) {
            delete ps;
            delete own;
            return fail(QTC_ERR_REJECTED, "qtc_rhi_pipeline_create: the backend refused the pipeline (do the shaders have a variant for it?)");
        }
        o = find(p, RK_GPIPE);
        o->res = ps;
        o->rp_own = own;
        o->srb = bindings;
        o->shader = vsh;
        o->shader2 = fsh;
        hold(p, bindings);
        return QTC_OK;
    });
}

QTC_API qtc_rhi qtc_rhi_compute_pipeline(qtc_rhi rhi, qtc_rhi cs, qtc_rhi bindings) {
    return guarded<qtc_rhi>(0, [&]() -> qtc_rhi {
        qtc_status err = 0;
        Ctx *c = rhi_ctx(rhi, "qtc_rhi_compute_pipeline", &err);
        if (!c) return 0;
        if (!c->rhi->isFeatureSupported(QRhi::Compute)) { fail(QTC_ERR_UNSUPPORTED, "qtc_rhi_compute_pipeline: the backend has no compute shaders"); return 0; }
        RObj *sh = get(cs, RK_SHADER, "qtc_rhi_compute_pipeline", &err);
        if (!sh) return 0;
        if (sh->shader.stage() != QShader::ComputeStage) { fail(QTC_ERR_BAD_ARGS, "qtc_rhi_compute_pipeline: not a compute shader"); return 0; }
        const QShader shader = sh->shader;
        RObj *s = get(bindings, RK_SRB, "qtc_rhi_compute_pipeline", &err);
        if (!s) return 0;
        if (!s->created || s->rhi != rhi) { fail(QTC_ERR_BAD_ARGS, "qtc_rhi_compute_pipeline: created bindings of this RHI"); return 0; }
        if (check_alive(s, "qtc_rhi_compute_pipeline")) return 0;
        if (check_bindings(shader, QTC_RHI_COMPUTE, s, "qtc_rhi_compute_pipeline")) return 0;
        const std::array<uint, 3> ls = shader.description().computeShaderLocalSize();
        const uint64_t threads = uint64_t(ls[0]) * ls[1] * ls[2];
        if (!threads || int64_t(ls[0]) > limit(c->rhi, QRhi::MaxThreadGroupX, 1024) || int64_t(ls[1]) > limit(c->rhi, QRhi::MaxThreadGroupY, 1024) ||
            int64_t(ls[2]) > limit(c->rhi, QRhi::MaxThreadGroupZ, 64) || threads > uint64_t(limit(c->rhi, QRhi::MaxThreadsPerThreadGroup, 1024))) {
            fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_compute_pipeline: the shader's local size is over the device's limits");
            return 0;
        }
        QRhiComputePipeline *ps = c->rhi->newComputePipeline();
        ps->setShaderStage({QRhiShaderStage::Compute, shader});
        ps->setShaderResourceBindings(static_cast<QRhiShaderResourceBindings *>(s->res));
        if (!ps->create()) {
            delete ps;
            fail(QTC_ERR_REJECTED, "qtc_rhi_compute_pipeline: the backend refused the pipeline");
            return 0;
        }
        const uint64_t h = child(rhi, RK_CPIPE);
        if (!h) { delete ps; return 0; }
        RObj *o = find(h, RK_CPIPE);
        o->res = ps;
        o->srb = bindings;
        o->shader = shader;
        hold(h, bindings);
        return h;
    });
}

// ------------------------------------------------------------------ data

QTC_API qtc_status qtc_rhi_upload(qtc_rhi buffer, int64_t offset, qtc_str bytes) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        RObj *b = get(buffer, RK_BUFFER, "qtc_rhi_upload", &err);
        if (!b) return err;
        QByteArray d;
        if (!read_bytes(bytes, d, "bytes")) return last_error_code();
        if (offset < 0 || offset > b->size || d.size() > b->size - offset) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_upload: the data must fit the buffer");
        if (d.isEmpty()) return QTC_OK;
        Ctx *c = ctx_quiet(b->rhi);
        if (c->external_cmds) return fail(QTC_ERR_STATE, "qtc_rhi_upload: native commands are being recorded");
        QRhiResourceUpdateBatch *batch = pending(c);
        if (!batch) return fail(QTC_ERR_LIMIT, "qtc_rhi_upload: no resource update batch is free");
        auto *qb = static_cast<QRhiBuffer *>(b->res);
        if (b->btype == QTC_RHI_DYNAMIC) batch->updateDynamicBuffer(qb, quint32(offset), quint32(d.size()), d.constData());
        else batch->uploadStaticBuffer(qb, quint32(offset), quint32(d.size()), d.constData());
        if (b->usage & QTC_RHI_INDEX_BUFFER) {
            std::memcpy(b->staged.data() + offset, d.constData(), size_t(d.size()));
            if (std::find(c->staged_bufs.begin(), c->staged_bufs.end(), buffer) == c->staged_bufs.end()) c->staged_bufs.push_back(buffer);
        }
        return QTC_OK;
    });
}

QTC_API qtc_status qtc_rhi_upload_image(qtc_rhi texture, qtc_val image) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        RObj *t = get(texture, RK_TEXTURE, "qtc_rhi_upload_image", &err);
        if (!t) return err;
        const int tw = t->w, th = t->h, tf = t->format;
        auto *qt = static_cast<QRhiTexture *>(t->res);
        Ctx *c = ctx_quiet(t->rhi);
        QVariant *v = reg().value(image, &err);
        if (!v) return fail(err, "qtc_rhi_upload_image");
        if (v->metaType() != QMetaType::fromType<QImage>()) return fail(QTC_ERR_WRONG_TYPE, "qtc_rhi_upload_image: a QImage value");
        QImage img = v->value<QImage>();
        if (img.size() != QSize(tw, th)) return fail(QTC_ERR_BAD_ARGS, QStringLiteral("qtc_rhi_upload_image: the image is %1x%2, the texture %3x%4").arg(img.width()).arg(img.height()).arg(tw).arg(th));
        if (tf == QTC_RHI_RGBA8) img = img.convertToFormat(QImage::Format_RGBA8888);
        else if (tf == QTC_RHI_BGRA8) img = img.convertToFormat(QImage::Format_ARGB32);
        else if (tf == QTC_RHI_R8) img = img.convertToFormat(QImage::Format_Grayscale8);
        else return fail(QTC_ERR_UNSUPPORTED, "qtc_rhi_upload_image: images go into RGBA8, BGRA8 or R8 textures (others take qtc_rhi_upload_pixels)");
        if (img.isNull()) return fail(QTC_ERR_LIMIT, "qtc_rhi_upload_image: the conversion failed (out of memory?)");
        if (c->external_cmds) return fail(QTC_ERR_STATE, "qtc_rhi_upload_image: native commands are being recorded");
        QRhiResourceUpdateBatch *batch = pending(c);
        if (!batch) return fail(QTC_ERR_LIMIT, "qtc_rhi_upload_image: no resource update batch is free");
        batch->uploadTexture(qt, img);
        return QTC_OK;
    });
}

QTC_API qtc_status qtc_rhi_upload_pixels(qtc_rhi texture, qtc_str pixels) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        RObj *t = get(texture, RK_TEXTURE, "qtc_rhi_upload_pixels", &err);
        if (!t) return err;
        QByteArray d;
        if (!read_bytes(pixels, d, "pixels")) return last_error_code();
        const int64_t need = int64_t(t->w) * t->h * bytes_per_pixel(t->format);
        if (d.size() != need) return fail(QTC_ERR_BAD_ARGS, QStringLiteral("qtc_rhi_upload_pixels: %1 bytes, the whole texture with rows tightly packed").arg(need));
        Ctx *c = ctx_quiet(t->rhi);
        if (c->external_cmds) return fail(QTC_ERR_STATE, "qtc_rhi_upload_pixels: native commands are being recorded");
        QRhiResourceUpdateBatch *batch = pending(c);
        if (!batch) return fail(QTC_ERR_LIMIT, "qtc_rhi_upload_pixels: no resource update batch is free");
        QRhiTextureSubresourceUploadDescription sub(d);
        batch->uploadTexture(static_cast<QRhiTexture *>(t->res), QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, sub)));
        return QTC_OK;
    });
}

// A readback handle, its result kept by the RHI until the GPU has written it.
static qtc_rhi new_readback(uint64_t rhi_h, bool image, RbBox **box_out) {
    const uint64_t h = child(rhi_h, RK_READBACK);
    if (!h) return 0;
    auto box = std::make_shared<RbBox>();
    RbBox *raw = box.get();
    if (image) box->tex.completed = [raw] { raw->done = true; };
    else box->buf.completed = [raw] { raw->done = true; };
    RObj *o = find(h, RK_READBACK);
    o->rb = box;
    o->rb_image = image;
    ctx_quiet(rhi_h)->inflight.push_back(box);
    *box_out = raw;
    return h;
}

static QRhiResourceUpdateBatch *batch_for(Ctx *c, const char *what) {
    if (c->external_cmds) {
        fail(QTC_ERR_STATE, QStringLiteral("%1: native commands are being recorded").arg(QLatin1StringView(what)));
        return nullptr;
    }
    QRhiResourceUpdateBatch *b = pending(c);
    if (!b) fail(QTC_ERR_LIMIT, QStringLiteral("%1: no resource update batch is free").arg(QLatin1StringView(what)));
    return b;
}

QTC_API qtc_rhi qtc_rhi_read_texture(qtc_rhi texture) {
    return guarded<qtc_rhi>(0, [&]() -> qtc_rhi {
        qtc_status err = 0;
        RObj *t = get(texture, RK_TEXTURE, "qtc_rhi_read_texture", &err);
        if (!t) return 0;
        if (!(t->tflags & QTC_RHI_READABLE)) { fail(QTC_ERR_STATE, "qtc_rhi_read_texture: the texture was not made with QTC_RHI_READABLE"); return 0; }
        const uint64_t rhi_h = t->rhi;
        auto *qt = static_cast<QRhiTexture *>(t->res);
        QRhiResourceUpdateBatch *batch = batch_for(ctx_quiet(rhi_h), "qtc_rhi_read_texture");
        if (!batch) return 0;
        RbBox *box = nullptr;
        const uint64_t h = new_readback(rhi_h, true, &box);
        if (!h) return 0;
        batch->readBackTexture(QRhiReadbackDescription(qt), &box->tex);
        return h;
    });
}

QTC_API qtc_rhi qtc_rhi_read_buffer(qtc_rhi buffer, int64_t offset, int64_t size) {
    return guarded<qtc_rhi>(0, [&]() -> qtc_rhi {
        qtc_status err = 0;
        RObj *b = get(buffer, RK_BUFFER, "qtc_rhi_read_buffer", &err);
        if (!b) return 0;
        if (offset < 0 || size <= 0 || offset > b->size || size > b->size - offset) { fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_read_buffer: the range must lie in the buffer"); return 0; }
        const uint64_t rhi_h = b->rhi;
        Ctx *c = ctx_quiet(rhi_h);
        if (!(b->usage & QTC_RHI_UNIFORM_BUFFER)) {
            if (b->btype == QTC_RHI_DYNAMIC) { fail(QTC_ERR_UNSUPPORTED, "qtc_rhi_read_buffer: of the dynamic buffers, only uniform buffers can be read back"); return 0; }
            if (!c->rhi->isFeatureSupported(QRhi::ReadBackNonUniformBuffer)) { fail(QTC_ERR_UNSUPPORTED, "qtc_rhi_read_buffer: this backend reads back uniform buffers only"); return 0; }
        }
        auto *qb = static_cast<QRhiBuffer *>(b->res);
        QRhiResourceUpdateBatch *batch = batch_for(c, "qtc_rhi_read_buffer");
        if (!batch) return 0;
        RbBox *box = nullptr;
        const uint64_t h = new_readback(rhi_h, false, &box);
        if (!h) return 0;
        batch->readBackBuffer(qb, quint32(offset), quint32(size), &box->buf);
        return h;
    });
}

QTC_API qtc_rhi qtc_rhi_read_swapchain(qtc_rhi swapchain) {
    return guarded<qtc_rhi>(0, [&]() -> qtc_rhi {
        qtc_status err = 0;
        RObj *s = get(swapchain, RK_SC, "qtc_rhi_read_swapchain", &err);
        if (!s) return 0;
        if (!(s->scflags & QTC_RHI_SC_READABLE)) { fail(QTC_ERR_STATE, "qtc_rhi_read_swapchain: the swapchain was not made with QTC_RHI_SC_READABLE"); return 0; }
        const uint64_t rhi_h = s->rhi;
        Ctx *c = ctx_quiet(rhi_h);
        if (!c->in_frame || c->frame_sc != swapchain) { fail(QTC_ERR_STATE, "qtc_rhi_read_swapchain: inside a frame on the swapchain"); return 0; }
        QRhiResourceUpdateBatch *batch = batch_for(c, "qtc_rhi_read_swapchain");
        if (!batch) return 0;
        RbBox *box = nullptr;
        const uint64_t h = new_readback(rhi_h, true, &box);
        if (!h) return 0;
        batch->readBackTexture(QRhiReadbackDescription(), &box->tex);   // no texture: the back buffer
        return h;
    });
}

QTC_API bool qtc_rhi_readback_ready(qtc_rhi rb) {
    return guarded<bool>(false, [&]() -> bool {
        qtc_status err = 0;
        RObj *o = get(rb, RK_READBACK, "qtc_rhi_readback_ready", &err);
        return o && o->rb->done;
    });
}

QTC_API qtc_view qtc_rhi_readback_bytes(qtc_rhi rb) {
    return guarded<qtc_view>(qtc_view{"", 0}, [&]() -> qtc_view {
        qtc_status err = 0;
        RObj *o = get(rb, RK_READBACK, "qtc_rhi_readback_bytes", &err);
        if (!o) return qtc_view{"", 0};
        if (!o->rb->done) { fail(QTC_ERR_STATE, "qtc_rhi_readback_bytes: not complete yet (qtc_rhi_readback_ready)"); return qtc_view{"", 0}; }
        return view_of(o->rb_image ? o->rb->tex.data : o->rb->buf.data);
    });
}

QTC_API qtc_val qtc_rhi_readback_image(qtc_rhi rb) {
    return guarded<qtc_val>(0, [&]() -> qtc_val {
        qtc_status err = 0;
        RObj *o = get(rb, RK_READBACK, "qtc_rhi_readback_image", &err);
        if (!o) return 0;
        if (!o->rb->done) { fail(QTC_ERR_STATE, "qtc_rhi_readback_image: not complete yet (qtc_rhi_readback_ready)"); return 0; }
        if (!o->rb_image) { fail(QTC_ERR_WRONG_TYPE, "qtc_rhi_readback_image: a buffer's readback (qtc_rhi_readback_bytes)"); return 0; }
        const QRhiReadbackResult &r = o->rb->tex;
        QImage::Format f;
        if (r.format == QRhiTexture::RGBA8) f = QImage::Format_RGBA8888;
        else if (r.format == QRhiTexture::BGRA8) f = QImage::Format_ARGB32;
        else { fail(QTC_ERR_WRONG_TYPE, "qtc_rhi_readback_image: only RGBA8 and BGRA8 read back as images (qtc_rhi_readback_bytes reads any)"); return 0; }
        const QSize sz = r.pixelSize;
        if (sz.isEmpty() || r.data.size() < int64_t(sz.width()) * sz.height() * 4) { fail(QTC_ERR_STATE, "qtc_rhi_readback_image: incomplete data"); return 0; }
        const QImage img(reinterpret_cast<const uchar *>(r.data.constData()), sz.width(), sz.height(), sz.width() * 4, f);
        // rows top to bottom, whatever the backend's framebuffer orientation
        Ctx *c = ctx_quiet(o->rhi);
        QImage out;
        if (c && c->rhi && c->rhi->isYUpInFramebuffer()) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
            out = img.flipped(Qt::Vertical);
#else
            out = img.mirrored(false, true);
#endif
        } else {
            out = img.copy();
        }
        if (out.isNull()) { fail(QTC_ERR_LIMIT, "qtc_rhi_readback_image: out of memory"); return 0; }
        return reg().new_value(QVariant::fromValue(out));
    });
}

// ------------------------------------------------------------------ frames and commands

QTC_API qtc_status qtc_rhi_begin_frame(qtc_rhi rhi, qtc_rhi swapchain) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        Ctx *c = rhi_ctx(rhi, "qtc_rhi_begin_frame", &err);
        if (!c) return err;
        if (!c->owned) return fail(QTC_ERR_STATE, "qtc_rhi_begin_frame: a widget's RHI records inside the widget's render override");
        if (c->in_frame) return fail(QTC_ERR_STATE, "qtc_rhi_begin_frame: a frame is being recorded already");
        if (c->rhi->isRecordingFrame()) return fail(QTC_ERR_STATE, "qtc_rhi_begin_frame: the RHI is recording a frame");
        prune(c);
        QRhiCommandBuffer *cb = nullptr;
        if (!swapchain) {
            const QRhi::FrameOpResult r = c->rhi->beginOffscreenFrame(&cb);
            if (r == QRhi::FrameOpDeviceLost) return fail(QTC_ERR_REJECTED, "qtc_rhi_begin_frame: the device was lost (destroy the RHI and make another)");
            if (r != QRhi::FrameOpSuccess) return fail(QTC_ERR_REJECTED, "qtc_rhi_begin_frame: the backend could not begin a frame");
        } else {
            RObj *s = get(swapchain, RK_SC, "qtc_rhi_begin_frame", &err);
            if (!s) return err;
            if (s->rhi != rhi) return fail(QTC_ERR_BAD_ARGS, "qtc_rhi_begin_frame: the swapchain belongs to another RHI");
            if (!s->window) return fail(QTC_ERR_STATE, "qtc_rhi_begin_frame: the swapchain's window is gone");
            auto *sc = static_cast<QRhiSwapChain *>(s->res);
            if (!s->window->isExposed()) return QTC_RHI_SKIPPED;
            const QSize sz = sc->surfacePixelSize();
            if (sz.isEmpty()) return QTC_RHI_SKIPPED;
            if (!s->built || sc->currentPixelSize() != sz) {
                if (!sc->createOrResize()) return fail(QTC_ERR_REJECTED, "qtc_rhi_begin_frame: the swapchain could not be made for the window");
                if ((s = find(swapchain, RK_SC))) s->built = true;
            }
            QRhi::FrameOpResult r = c->rhi->beginFrame(sc);
            if (r == QRhi::FrameOpSwapChainOutOfDate) {
                if (!sc->createOrResize()) return QTC_RHI_SKIPPED;
                r = c->rhi->beginFrame(sc);
            }
            if (r == QRhi::FrameOpSwapChainOutOfDate) return QTC_RHI_SKIPPED;
            if (r == QRhi::FrameOpDeviceLost) return fail(QTC_ERR_REJECTED, "qtc_rhi_begin_frame: the device was lost (destroy the RHI and make another)");
            if (r != QRhi::FrameOpSuccess) return fail(QTC_ERR_REJECTED, "qtc_rhi_begin_frame: the backend could not begin a frame");
            cb = sc->currentFrameCommandBuffer();
            c->frame_sc = swapchain;
        }
        reset_pass_state(c);
        c->in_frame = true;
        c->cb = cb;
        return QTC_OK;
    });
}

QTC_API qtc_status qtc_rhi_end_frame(qtc_rhi rhi) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        Ctx *c = rhi_ctx(rhi, "qtc_rhi_end_frame", &err);
        if (!c) return err;
        if (qtc_status e = need_frame(c, "qtc_rhi_end_frame")) return e;
        if (c->external_frame) return fail(QTC_ERR_STATE, "qtc_rhi_end_frame: the widget's frame ends when its override returns");
        close_pass(c);
        if (QRhiResourceUpdateBatch *b = take_pending(c)) c->cb->resourceUpdate(b);
        QRhi::FrameOpResult r;
        if (c->frame_sc) {
            RObj *s = find(c->frame_sc, RK_SC);
            r = s ? c->rhi->endFrame(static_cast<QRhiSwapChain *>(s->res)) : QRhi::FrameOpError;
        } else {
            // an offscreen frame waits for the GPU: its readbacks are complete when it returns
            r = c->rhi->endOffscreenFrame();
        }
        reset_frame_state(c);
        prune(c);
        if (r == QRhi::FrameOpSuccess || r == QRhi::FrameOpSwapChainOutOfDate) return QTC_OK;   // out of date: remade at the next frame
        if (r == QRhi::FrameOpDeviceLost) return fail(QTC_ERR_REJECTED, "qtc_rhi_end_frame: the device was lost (destroy the RHI and make another)");
        return fail(QTC_ERR_REJECTED, "qtc_rhi_end_frame: the backend failed the frame");
    });
}

QTC_API qtc_status qtc_rhi_begin_pass(qtc_rhi rhi, qtc_rhi target, uint32_t clear_argb, double depth, uint32_t flags) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        Ctx *c = frame_ctx(rhi, "qtc_rhi_begin_pass", &err);
        if (!c) return err;
        if (c->pass) return fail(QTC_ERR_STATE, "qtc_rhi_begin_pass: a pass is open already");
        if (flags & ~uint32_t(QTC_RHI_PASS_EXTERNAL)) return fail(QTC_ERR_INVALID_ARG, "qtc_rhi_begin_pass: flags");
        if (!(depth >= 0.0 && depth <= 1.0)) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_begin_pass: depth must be in [0, 1]");
        RObj *t = get(target, 0, "qtc_rhi_begin_pass", &err);
        if (!t) return err;
        if (t->rhi != rhi) return fail(QTC_ERR_BAD_ARGS, "qtc_rhi_begin_pass: the target belongs to another RHI");
        QRhiRenderTarget *rt = nullptr;
        QSize size;
        if (t->kind == RK_RT) {
            rt = static_cast<QRhiRenderTarget *>(t->res);
            size = rt->pixelSize();
        } else if (t->kind == RK_SC) {
            if (c->frame_sc != target) return fail(QTC_ERR_STATE, "qtc_rhi_begin_pass: the frame is not on this swapchain");
            auto *sc = static_cast<QRhiSwapChain *>(t->res);
            rt = sc->currentFrameRenderTarget();
            size = sc->currentPixelSize();
        } else {
            return fail(QTC_ERR_WRONG_TYPE, "qtc_rhi_begin_pass: a render target or a swapchain");
        }
        QRhiRenderPassDescriptor *rp = t->rp;
        QRhiCommandBuffer::BeginPassFlags bf;
        if (flags & QTC_RHI_PASS_EXTERNAL) bf |= QRhiCommandBuffer::ExternalContent;
        c->cb->beginPass(rt, QColor::fromRgba(QRgb(clear_argb)), {float(depth), 0}, take_pending(c), bf);
        reset_pass_state(c);
        c->pass = 1;
        c->pass_flags = flags;
        c->pass_rp = rp;
        c->pass_size = size;
        return QTC_OK;
    });
}

QTC_API qtc_status qtc_rhi_begin_compute(qtc_rhi rhi, uint32_t flags) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        Ctx *c = frame_ctx(rhi, "qtc_rhi_begin_compute", &err);
        if (!c) return err;
        if (c->pass) return fail(QTC_ERR_STATE, "qtc_rhi_begin_compute: a pass is open already");
        if (flags & ~uint32_t(QTC_RHI_PASS_EXTERNAL)) return fail(QTC_ERR_INVALID_ARG, "qtc_rhi_begin_compute: flags");
        if (!c->rhi->isFeatureSupported(QRhi::Compute)) return fail(QTC_ERR_UNSUPPORTED, "qtc_rhi_begin_compute: no compute here");
        QRhiCommandBuffer::BeginPassFlags bf;
        if (flags & QTC_RHI_PASS_EXTERNAL) bf |= QRhiCommandBuffer::ExternalContent;
        c->cb->beginComputePass(take_pending(c), bf);
        reset_pass_state(c);
        c->pass = 2;
        c->pass_flags = flags;
        return QTC_OK;
    });
}

QTC_API qtc_status qtc_rhi_end_pass(qtc_rhi rhi) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        Ctx *c = rhi_ctx(rhi, "qtc_rhi_end_pass", &err);
        if (!c) return err;
        if (qtc_status e = need_frame(c, "qtc_rhi_end_pass")) return e;
        if (!c->pass) return fail(QTC_ERR_STATE, "qtc_rhi_end_pass: no pass is open");
        close_pass(c);
        return QTC_OK;
    });
}

QTC_API qtc_status qtc_rhi_set_pipeline(qtc_rhi rhi, qtc_rhi pipeline) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        Ctx *c = frame_ctx(rhi, "qtc_rhi_set_pipeline", &err);
        if (!c) return err;
        RObj *p = get(pipeline, 0, "qtc_rhi_set_pipeline", &err);
        if (!p) return err;
        if (p->rhi != rhi) return fail(QTC_ERR_BAD_ARGS, "qtc_rhi_set_pipeline: the pipeline belongs to another RHI");
        if (p->kind == RK_GPIPE) {
            if (c->pass != 1) return fail(QTC_ERR_STATE, "qtc_rhi_set_pipeline: a graphics pipeline needs a render pass (qtc_rhi_begin_pass)");
            if (!p->res) return fail(QTC_ERR_STATE, "qtc_rhi_set_pipeline: create the pipeline first");
            if (!p->rp_own->isCompatible(c->pass_rp)) return fail(QTC_ERR_BAD_ARGS, "qtc_rhi_set_pipeline: the pipeline was made for targets incompatible with the pass's");
            c->cb->setGraphicsPipeline(static_cast<QRhiGraphicsPipeline *>(p->res));
            // the whole target until the program says otherwise
            c->cb->setViewport(QRhiViewport(0, 0, float(c->pass_size.width()), float(c->pass_size.height())));
        } else if (p->kind == RK_CPIPE) {
            if (c->pass != 2) return fail(QTC_ERR_STATE, "qtc_rhi_set_pipeline: a compute pipeline needs a compute pass (qtc_rhi_begin_compute)");
            c->cb->setComputePipeline(static_cast<QRhiComputePipeline *>(p->res));
        } else {
            return fail(QTC_ERR_WRONG_TYPE, "qtc_rhi_set_pipeline: not a pipeline");
        }
        c->pipeline = pipeline;
        c->srb = 0;
        return QTC_OK;
    });
}

QTC_API qtc_status qtc_rhi_set_bindings(qtc_rhi rhi, qtc_rhi bindings) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        Ctx *c = frame_ctx(rhi, "qtc_rhi_set_bindings", &err);
        if (!c) return err;
        if (!c->pipeline) return fail(QTC_ERR_STATE, "qtc_rhi_set_bindings: set a pipeline first");
        RObj *s = get(bindings, RK_SRB, "qtc_rhi_set_bindings", &err);
        if (!s) return err;
        if (!s->created || s->rhi != rhi) return fail(QTC_ERR_BAD_ARGS, "qtc_rhi_set_bindings: created bindings of this RHI");
        if (qtc_status e = check_alive(s, "qtc_rhi_set_bindings")) return e;
        RObj *p = find(c->pipeline);
        RObj *ps = p ? find(p->srb, RK_SRB) : nullptr;
        if (!p || !ps) return fail(QTC_ERR_STALE_HANDLE, "qtc_rhi_set_bindings: the pipeline is gone");
        if (!static_cast<QRhiShaderResourceBindings *>(s->res)->isLayoutCompatible(static_cast<QRhiShaderResourceBindings *>(ps->res)))
            return fail(QTC_ERR_BAD_ARGS, "qtc_rhi_set_bindings: the bindings' layout differs from the pipeline's");
        // what the shaders need, against these bindings (sizes may differ from the pipeline's set)
        if (p->kind == RK_GPIPE) {
            if ((err = check_bindings(p->shader, QTC_RHI_VERTEX, s, "qtc_rhi_set_bindings"))) return err;
            if ((err = check_bindings(p->shader2, QTC_RHI_FRAGMENT, s, "qtc_rhi_set_bindings"))) return err;
        } else {
            if ((err = check_bindings(p->shader, QTC_RHI_COMPUTE, s, "qtc_rhi_set_bindings"))) return err;
        }
        c->cb->setShaderResources(static_cast<QRhiShaderResourceBindings *>(s->res));
        c->srb = bindings;
        return QTC_OK;
    });
}

QTC_API qtc_status qtc_rhi_set_viewport(qtc_rhi rhi, double x, double y, double w, double h) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        Ctx *c = frame_ctx(rhi, "qtc_rhi_set_viewport", &err);
        if (!c) return err;
        if (c->pass != 1 || !c->pipeline) return fail(QTC_ERR_STATE, "qtc_rhi_set_viewport: inside a render pass, after qtc_rhi_set_pipeline");
        // within what every device accepts: Vulkan guarantees viewports up to the largest
        // framebuffer and bounds of twice that either way (and Qt flips y for it)
        const double m = limit(c->rhi, QRhi::TextureSizeMax, 4096);
        if (!(std::isfinite(x) && std::isfinite(y) && w > 0 && h > 0 && w <= m && h <= m && std::fabs(x) <= m && std::fabs(y) <= m &&
              x + w <= 2 * m - 1 && y + h <= 2 * m - 1))
            return fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("qtc_rhi_set_viewport: a positive size of at most %1, with x and y within %1 of 0").arg(m));
        c->cb->setViewport(QRhiViewport(float(x), float(y), float(w), float(h)));
        return QTC_OK;
    });
}

QTC_API qtc_status qtc_rhi_set_vertex_buffer(qtc_rhi rhi, int32_t binding, qtc_rhi buffer, int64_t offset) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        Ctx *c = frame_ctx(rhi, "qtc_rhi_set_vertex_buffer", &err);
        if (!c) return err;
        if (c->pass != 1) return fail(QTC_ERR_STATE, "qtc_rhi_set_vertex_buffer: inside a render pass");
        if (binding < 0 || binding >= 16) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_set_vertex_buffer: binding in [0, 16)");
        RObj *b = get(buffer, RK_BUFFER, "qtc_rhi_set_vertex_buffer", &err);
        if (!b) return err;
        if (b->rhi != rhi || !(b->usage & QTC_RHI_VERTEX_BUFFER)) return fail(QTC_ERR_BAD_ARGS, "qtc_rhi_set_vertex_buffer: a vertex buffer of this RHI");
        if (offset < 0 || offset > b->size || offset % 4) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_set_vertex_buffer: offset (a multiple of 4 within the buffer)");
        if (int(c->vbufs.size()) <= binding) c->vbufs.resize(size_t(binding + 1));
        c->vbufs[size_t(binding)] = Ctx::VB{buffer, offset};
        return QTC_OK;
    });
}

QTC_API qtc_status qtc_rhi_set_index_buffer(qtc_rhi rhi, qtc_rhi buffer, int64_t offset, bool index32) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        Ctx *c = frame_ctx(rhi, "qtc_rhi_set_index_buffer", &err);
        if (!c) return err;
        if (c->pass != 1) return fail(QTC_ERR_STATE, "qtc_rhi_set_index_buffer: inside a render pass");
        RObj *b = get(buffer, RK_BUFFER, "qtc_rhi_set_index_buffer", &err);
        if (!b) return err;
        if (b->rhi != rhi || !(b->usage & QTC_RHI_INDEX_BUFFER)) return fail(QTC_ERR_BAD_ARGS, "qtc_rhi_set_index_buffer: an index buffer of this RHI");
        if (offset < 0 || offset > b->size || offset % (index32 ? 4 : 2)) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_set_index_buffer: offset (aligned to the index size, within the buffer)");
        if (index32 && !c->rhi->isFeatureSupported(QRhi::ElementIndexUint)) return fail(QTC_ERR_UNSUPPORTED, "qtc_rhi_set_index_buffer: 32-bit indices are not supported here");
        c->ibuf = buffer;
        c->ioffset = offset;
        c->iformat = index32 ? 1 : 0;
        return QTC_OK;
    });
}

// Binds the vertex (and index) buffers once the vertex buffers the pipeline reads are known to
// hold `vertices` vertices and `instances` instances.
static qtc_status bind_vertices(Ctx *c, RObj *p, int64_t vertices, int64_t instances, const char *what) {
    const QLatin1StringView w(what);
    QVarLengthArray<QRhiCommandBuffer::VertexInput, 8> in;
    for (size_t i = 0; i < p->strides.size(); i++) {
        if (i >= c->vbufs.size() || !c->vbufs[i].buf) return fail(QTC_ERR_STATE, QStringLiteral("%1: vertex binding %2 has no buffer (qtc_rhi_set_vertex_buffer)").arg(w).arg(i));
        RObj *b = find(c->vbufs[i].buf, RK_BUFFER);
        if (!b) return fail(QTC_ERR_STALE_HANDLE, QStringLiteral("%1: the buffer of vertex binding %2 is gone").arg(w).arg(i));
        int64_t reach = 0;   // the bytes one element of the binding reads
        for (const VAttr &a : p->attrs)
            if (a.binding == int(i)) {
                AttrInfo ai{};
                attr_info(a.format, &ai);
                reach = std::max<int64_t>(reach, a.offset + ai.bytes);
            }
        const int64_t count = p->per_instance[i] ? instances : vertices;
        const int64_t need = count > 0 && reach > 0 ? (count - 1) * p->strides[i] + reach : 0;
        const int64_t have = b->size - c->vbufs[i].offset;
        if (need > have)
            return fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("%1: vertex binding %2 would read %3 bytes, its buffer has %4 from the offset").arg(w).arg(i).arg(need).arg(have));
        in.append({static_cast<QRhiBuffer *>(b->res), quint32(c->vbufs[i].offset)});
    }
    QRhiBuffer *ib = nullptr;
    if (c->ibuf) {
        RObj *x = find(c->ibuf, RK_BUFFER);
        if (!x) return fail(QTC_ERR_STALE_HANDLE, QStringLiteral("%1: the index buffer is gone").arg(w));
        ib = static_cast<QRhiBuffer *>(x->res);
    }
    if (!in.isEmpty() || ib)
        c->cb->setVertexInput(0, int(in.size()), in.isEmpty() ? nullptr : in.constData(), ib, quint32(c->ioffset),
                              c->iformat ? QRhiCommandBuffer::IndexUInt32 : QRhiCommandBuffer::IndexUInt16);
    return QTC_OK;
}

static qtc_status draw_ready(Ctx *c, RObj **p, const char *what) {
    const QLatin1StringView w(what);
    if (c->pass != 1) return fail(QTC_ERR_STATE, QStringLiteral("%1: inside a render pass").arg(w));
    *p = c->pipeline ? find(c->pipeline, RK_GPIPE) : nullptr;
    if (!*p) return fail(QTC_ERR_STATE, QStringLiteral("%1: set a graphics pipeline first").arg(w));
    RObj *s = c->srb ? find(c->srb, RK_SRB) : nullptr;
    if (!s) return fail(QTC_ERR_STATE, QStringLiteral("%1: set the bindings first (qtc_rhi_set_bindings)").arg(w));
    return check_alive(s, what);
}

static qtc_status check_instancing(Ctx *c, int64_t instances, int64_t first_instance, const char *what) {
    if (instances > 1 && !c->rhi->isFeatureSupported(QRhi::Instancing))
        return fail(QTC_ERR_UNSUPPORTED, QStringLiteral("%1: no instancing here").arg(QLatin1StringView(what)));
    if (first_instance > 0 && !c->rhi->isFeatureSupported(QRhi::BaseInstance))
        return fail(QTC_ERR_UNSUPPORTED, QStringLiteral("%1: first_instance is not supported here").arg(QLatin1StringView(what)));
    return QTC_OK;
}

QTC_API qtc_status qtc_rhi_draw(qtc_rhi rhi, int64_t vertices, int64_t instances, int64_t first_vertex, int64_t first_instance) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        Ctx *c = frame_ctx(rhi, "qtc_rhi_draw", &err);
        if (!c) return err;
        RObj *p = nullptr;
        if (qtc_status e = draw_ready(c, &p, "qtc_rhi_draw")) return e;
        if (vertices < 0 || instances < 1 || first_vertex < 0 || first_instance < 0 || vertices > (int64_t(1) << 30) ||
            instances > (int64_t(1) << 24) || first_vertex > (int64_t(1) << 30) || first_instance > (int64_t(1) << 24))
            return fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_draw: counts");
        if (qtc_status e = check_instancing(c, instances, first_instance, "qtc_rhi_draw")) return e;
        if (qtc_status e = bind_vertices(c, p, first_vertex + vertices, first_instance + instances, "qtc_rhi_draw")) return e;
        if (vertices) c->cb->draw(quint32(vertices), quint32(instances), quint32(first_vertex), quint32(first_instance));
        return QTC_OK;
    });
}

QTC_API qtc_status qtc_rhi_draw_indexed(qtc_rhi rhi, int64_t indices, int64_t instances, int64_t first_index, int64_t vertex_offset, int64_t first_instance) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        Ctx *c = frame_ctx(rhi, "qtc_rhi_draw_indexed", &err);
        if (!c) return err;
        RObj *p = nullptr;
        if (qtc_status e = draw_ready(c, &p, "qtc_rhi_draw_indexed")) return e;
        if (!c->ibuf) return fail(QTC_ERR_STATE, "qtc_rhi_draw_indexed: set an index buffer first");
        if (indices < 0 || instances < 1 || first_index < 0 || first_instance < 0 || vertex_offset < 0 || indices > (int64_t(1) << 30) ||
            instances > (int64_t(1) << 24) || first_index > (int64_t(1) << 30) || vertex_offset > (int64_t(1) << 30) || first_instance > (int64_t(1) << 24))
            return fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_draw_indexed: counts");
        if (vertex_offset > 0 && !c->rhi->isFeatureSupported(QRhi::BaseVertex)) return fail(QTC_ERR_UNSUPPORTED, "qtc_rhi_draw_indexed: vertex_offset is not supported here");
        if (qtc_status e = check_instancing(c, instances, first_instance, "qtc_rhi_draw_indexed")) return e;
        RObj *ib = find(c->ibuf, RK_BUFFER);
        if (!ib) return fail(QTC_ERR_STALE_HANDLE, "qtc_rhi_draw_indexed: the index buffer is gone");
        if (!ib->shadow_valid) return fail(QTC_ERR_STATE, "qtc_rhi_draw_indexed: the index buffer's data reaches the GPU when the next pass begins (upload before qtc_rhi_begin_pass)");
        uint64_t mx = 0;
        if (!max_index(ib, c->ioffset, c->iformat, first_index, indices, &mx))
            return fail(QTC_ERR_OUT_OF_RANGE, "qtc_rhi_draw_indexed: the indices lie outside the index buffer");
        const int64_t reach = indices ? int64_t(mx) + vertex_offset + 1 : 0;
        if (qtc_status e = bind_vertices(c, p, reach, first_instance + instances, "qtc_rhi_draw_indexed")) return e;
        if (indices) c->cb->drawIndexed(quint32(indices), quint32(instances), quint32(first_index), qint32(vertex_offset), quint32(first_instance));
        return QTC_OK;
    });
}

QTC_API qtc_status qtc_rhi_dispatch(qtc_rhi rhi, int32_t x, int32_t y, int32_t z) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        Ctx *c = frame_ctx(rhi, "qtc_rhi_dispatch", &err);
        if (!c) return err;
        if (c->pass != 2 || !c->pipeline) return fail(QTC_ERR_STATE, "qtc_rhi_dispatch: inside a compute pass, with a compute pipeline set");
        RObj *s = c->srb ? find(c->srb, RK_SRB) : nullptr;
        if (!s) return fail(QTC_ERR_STATE, "qtc_rhi_dispatch: set the bindings first");
        if (qtc_status e = check_alive(s, "qtc_rhi_dispatch")) return e;
        const int mx = limit(c->rhi, QRhi::MaxThreadGroupsPerDimension, 65535);
        if (x < 1 || y < 1 || z < 1 || x > mx || y > mx || z > mx)
            return fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("qtc_rhi_dispatch: group counts in [1, %1]").arg(mx));
        c->cb->dispatch(x, y, z);
        return QTC_OK;
    });
}

// ------------------------------------------------------------------ native interop

QTC_API qtc_status qtc_rhi_native(qtc_rhi h, qtc_rhi_native_handles *out) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        if (!out) return fail(QTC_ERR_INVALID_ARG, "qtc_rhi_native: out is NULL");
        qtc_status err = 0;
        Ctx *c = rhi_ctx(h, "qtc_rhi_native", &err);
        if (!c) return err;
        std::memset(out, 0, sizeof *out);
        out->backend = c->backend;
        const QRhiNativeHandles *nh = c->rhi->nativeHandles();
        if (!nh) return QTC_OK;
        switch (c->rhi->backend()) {
#ifdef QTC_GL
        case QRhi::OpenGLES2:
            out->device = static_cast<const QRhiGles2NativeHandles *>(nh)->context;
            break;
#endif
#ifdef QTC_VULKAN
        case QRhi::Vulkan: {
            auto *v = static_cast<const QRhiVulkanNativeHandles *>(nh);
            out->instance = v->inst ? reinterpret_cast<void *>(v->inst->vkInstance()) : nullptr;
            out->physical_device = reinterpret_cast<void *>(v->physDev);
            out->device = reinterpret_cast<void *>(v->dev);
            out->queue = reinterpret_cast<void *>(v->gfxQueue);
            out->queue_family = v->gfxQueueFamilyIdx;
            out->queue_index = v->gfxQueueIdx;
            break;
        }
#endif
#ifdef Q_OS_WIN
        case QRhi::D3D11:
            out->device = static_cast<const QRhiD3D11NativeHandles *>(nh)->dev;
            out->queue = static_cast<const QRhiD3D11NativeHandles *>(nh)->context;
            break;
        case QRhi::D3D12:
            out->device = static_cast<const QRhiD3D12NativeHandles *>(nh)->dev;
            out->queue = static_cast<const QRhiD3D12NativeHandles *>(nh)->commandQueue;
            break;
#endif
#if QT_CONFIG(metal)
        case QRhi::Metal:
            out->device = reinterpret_cast<void *>(static_cast<const QRhiMetalNativeHandles *>(nh)->dev);
            out->queue = reinterpret_cast<void *>(static_cast<const QRhiMetalNativeHandles *>(nh)->cmdQueue);
            break;
#endif
        default:
            break;
        }
        return QTC_OK;
    });
}

QTC_API uint64_t qtc_rhi_native_object(qtc_rhi h, int32_t *layout) {
    return guarded<uint64_t>(0, [&]() -> uint64_t {
        if (layout) *layout = 0;
        qtc_status err = 0;
        RObj *o = get(h, 0, "qtc_rhi_native_object", &err);
        if (!o) return 0;
        if (o->kind == RK_TEXTURE) {
            const QRhiTexture::NativeTexture nt = static_cast<QRhiTexture *>(o->res)->nativeTexture();
            if (layout) *layout = nt.layout;
            return nt.object;
        }
        if (o->kind != RK_BUFFER) {
            fail(QTC_ERR_WRONG_TYPE, "qtc_rhi_native_object: a buffer or a texture");
            return 0;
        }
        Ctx *c = ctx_quiet(o->rhi);
        const QRhiBuffer::NativeBuffer nb = static_cast<QRhiBuffer *>(o->res)->nativeBuffer();
        if (nb.slotCount <= 0) return 0;
        const int slot = nb.slotCount > 1 ? std::clamp(c->rhi->currentFrameSlot(), 0, nb.slotCount - 1) : 0;
        const void *p = nb.objects[slot];
        if (!p) return 0;
        switch (c->rhi->backend()) {
        case QRhi::OpenGLES2: {
            quint32 id;   // GLuint
            std::memcpy(&id, p, sizeof id);
            return id;
        }
        case QRhi::Vulkan: {
            uint64_t v;   // VkBuffer: a 64-bit handle
            std::memcpy(&v, p, sizeof v);
            return v;
        }
        default: {
            void *ptr;    // ID3D11Buffer *, ID3D12Resource *, id<MTLBuffer>
            std::memcpy(&ptr, p, sizeof ptr);
            return uint64_t(quintptr(ptr));
        }
        }
    });
}

QTC_API qtc_status qtc_rhi_make_current(qtc_rhi h) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        Ctx *c = rhi_ctx(h, "qtc_rhi_make_current", &err);
        if (!c) return err;
        if (c->in_frame) return fail(QTC_ERR_STATE, "qtc_rhi_make_current: outside frames (in a frame the context is current already)");
        return c->rhi->makeThreadLocalNativeContextCurrent() ? QTC_OK : fail(QTC_ERR_REJECTED, "qtc_rhi_make_current: failed");
    });
}

QTC_API qtc_status qtc_rhi_begin_external(qtc_rhi h, void **native_cb) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        if (native_cb) *native_cb = nullptr;
        qtc_status err = 0;
        Ctx *c = frame_ctx(h, "qtc_rhi_begin_external", &err);
        if (!c) return err;
        if (!c->pass || !(c->pass_flags & QTC_RHI_PASS_EXTERNAL))
            return fail(QTC_ERR_STATE, "qtc_rhi_begin_external: inside a pass begun with QTC_RHI_PASS_EXTERNAL");
        c->cb->beginExternal();
        c->external_cmds = true;
        void *ncb = nullptr;
        const QRhiNativeHandles *nh = c->cb->nativeHandles();
        switch (c->rhi->backend()) {
#ifdef QTC_VULKAN
        case QRhi::Vulkan:
            if (nh) ncb = reinterpret_cast<void *>(static_cast<const QRhiVulkanCommandBufferNativeHandles *>(nh)->commandBuffer);
            break;
#endif
#ifdef Q_OS_WIN
        case QRhi::D3D12:
            if (nh) ncb = static_cast<const QRhiD3D12CommandBufferNativeHandles *>(nh)->commandList;
            break;
#endif
#if QT_CONFIG(metal)
        case QRhi::Metal:
            if (nh) ncb = reinterpret_cast<void *>(static_cast<const QRhiMetalCommandBufferNativeHandles *>(nh)->encoder);
            break;
#endif
        default:
            (void)nh;
            break;
        }
        if (native_cb) *native_cb = ncb;
        return QTC_OK;
    });
}

QTC_API qtc_status qtc_rhi_end_external(qtc_rhi h) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        Ctx *c = rhi_ctx(h, "qtc_rhi_end_external", &err);
        if (!c) return err;
        if (!c->external_cmds) return fail(QTC_ERR_STATE, "qtc_rhi_end_external: no native commands are being recorded");
        c->cb->endExternal();
        c->external_cmds = false;
        // Qt's state is gone: pipeline, bindings, buffers and viewport are set again
        c->pipeline = c->srb = 0;
        c->vbufs.clear();
        c->ibuf = 0;
        return QTC_OK;
    });
}

// ------------------------------------------------------------------ Vulkan

#ifdef QTC_VULKAN
static VkHolder *vk_of(uint64_t h, const char *what, qtc_status *err) {
    RObj *o = get(h, 0, what, err);
    if (!o) return nullptr;
    if (o->kind == RK_VKI) return o->vkh;
    if (o->kind == RK_RHI && o->ctx->owned && o->ctx->vk) return o->ctx->vk;
    *err = fail(QTC_ERR_WRONG_TYPE, QStringLiteral("%1: a Vulkan instance (qtc_vk_create) or a Vulkan RHI").arg(QLatin1StringView(what)));
    return nullptr;
}

static bool name_list(qtc_str s, QByteArrayList *out, const char *what) {
    QByteArray j;
    if (!read_bytes(s, j, what)) return false;
    if (j.trimmed().isEmpty()) return true;
    QJsonParseError pe;
    const QJsonDocument d = QJsonDocument::fromJson(j, &pe);
    if (pe.error != QJsonParseError::NoError || !d.isArray() || d.array().size() > 256) {
        fail(QTC_ERR_INVALID_ARG, QStringLiteral("qtc_vk_create: %1 is a JSON array of at most 256 names").arg(QLatin1StringView(what)));
        return false;
    }
    for (const QJsonValue &v : d.array()) {
        if (!v.isString() || v.toString().isEmpty() || v.toString().size() > 256) {
            fail(QTC_ERR_INVALID_ARG, QStringLiteral("qtc_vk_create: %1 is a JSON array of names").arg(QLatin1StringView(what)));
            return false;
        }
        out->append(v.toString().toUtf8());
    }
    return true;
}
#endif

QTC_API qtc_rhi qtc_vk_create(qtc_str extensions, qtc_str layers) {
    return guarded<qtc_rhi>(0, [&]() -> qtc_rhi {
#ifdef QTC_VULKAN
        if (app_kind() == QTC_APP_CORE) { fail(QTC_ERR_STATE, "qtc_vk_create needs a GUI application"); return 0; }
        QByteArrayList ext, lay;
        if (!name_list(extensions, &ext, "extensions") || !name_list(layers, &lay, "layers")) return 0;
        auto *v = new VkHolder;
        v->inst = new QVulkanInstance;
        v->inst->setApiVersion(v->inst->supportedApiVersion());
        v->inst->setExtensions(ext);
        v->inst->setLayers(lay);
        if (!v->inst->create()) {
            const int code = int(v->inst->errorCode());
            vk_free(v);
            fail(QTC_ERR_UNSUPPORTED, QStringLiteral("qtc_vk_create: no Vulkan instance here (VkResult %1)").arg(code));
            return 0;
        }
        const uint64_t h = alloc(RK_VKI, 0);
        if (!h) { vk_free(v); fail(QTC_ERR_LIMIT, "too many RHI objects"); return 0; }
        find(h, RK_VKI)->vkh = v;
        return h;
#else
        (void)extensions, (void)layers;
        fail(QTC_ERR_UNSUPPORTED, "Vulkan is not in this build");
        return 0;
#endif
    });
}

QTC_API void *qtc_vk_instance(qtc_rhi h) {
    return guarded<void *>(nullptr, [&]() -> void * {
#ifdef QTC_VULKAN
        qtc_status err = 0;
        VkHolder *v = vk_of(h, "qtc_vk_instance", &err);
        return v ? reinterpret_cast<void *>(v->inst->vkInstance()) : nullptr;
#else
        (void)h;
        fail(QTC_ERR_UNSUPPORTED, "Vulkan is not in this build");
        return nullptr;
#endif
    });
}

QTC_API void *qtc_vk_proc_address(qtc_rhi h, qtc_str name) {
    return guarded<void *>(nullptr, [&]() -> void * {
#ifdef QTC_VULKAN
        qtc_status err = 0;
        VkHolder *v = vk_of(h, "qtc_vk_proc_address", &err);
        if (!v) return nullptr;
        QByteArray n;
        if (!read_bytes(name, n, "name")) return nullptr;
        if (n.isEmpty() || n.size() > 256 || n.contains('\0')) { fail(QTC_ERR_INVALID_ARG, "qtc_vk_proc_address: a function name"); return nullptr; }
        void *p = reinterpret_cast<void *>(v->inst->getInstanceProcAddr(n.constData()));
        if (!p) fail(QTC_ERR_NOT_FOUND, QStringLiteral("qtc_vk_proc_address: no Vulkan function %1").arg(QString::fromLatin1(n)));
        return p;
#else
        (void)h, (void)name;
        fail(QTC_ERR_UNSUPPORTED, "Vulkan is not in this build");
        return nullptr;
#endif
    });
}

QTC_API qtc_status qtc_vk_attach(qtc_rhi h, qtc_obj window) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
#ifdef QTC_VULKAN
        qtc_status err = 0;
        VkHolder *v = vk_of(h, "qtc_vk_attach", &err);
        if (!v) return err;
        QObject *wo = reg().object(window, &err);
        if (!wo) return fail(err, "qtc_vk_attach");
        auto *w = qobject_cast<QWindow *>(wo);
        if (!w) return fail(QTC_ERR_WRONG_TYPE, "qtc_vk_attach: not a QWindow");
        if (draws_itself(w)) return fail(QTC_ERR_WRONG_TYPE, "qtc_vk_attach: the window draws itself: use a plain QWindow");
        if (w->vulkanInstance() == v->inst && w->surfaceType() == QSurface::VulkanSurface) return QTC_OK;
        if (w->handle()) return fail(QTC_ERR_STATE, "qtc_vk_attach: the window exists already (attach before showing it)");
        w->setSurfaceType(QSurface::VulkanSurface);
        w->setVulkanInstance(v->inst);
        v->windows.push_back(w);
        return QTC_OK;
#else
        (void)h, (void)window;
        return fail(QTC_ERR_UNSUPPORTED, "Vulkan is not in this build");
#endif
    });
}

QTC_API uint64_t qtc_vk_surface(qtc_rhi h, qtc_obj window) {
    return guarded<uint64_t>(0, [&]() -> uint64_t {
#ifdef QTC_VULKAN
        qtc_status err = 0;
        VkHolder *v = vk_of(h, "qtc_vk_surface", &err);
        if (!v) return 0;
        QObject *wo = reg().object(window, &err);
        if (!wo) { fail(err, "qtc_vk_surface"); return 0; }
        auto *w = qobject_cast<QWindow *>(wo);
        if (!w) { fail(QTC_ERR_WRONG_TYPE, "qtc_vk_surface: not a QWindow"); return 0; }
        if (w->vulkanInstance() != v->inst) { fail(QTC_ERR_STATE, "qtc_vk_surface: the window is not attached to the instance (qtc_vk_attach)"); return 0; }
        if (!w->handle()) { fail(QTC_ERR_STATE, "qtc_vk_surface: the window does not exist yet (show it, or call create)"); return 0; }
        const VkSurfaceKHR s = QVulkanInstance::surfaceForWindow(w);
        if (!s) { fail(QTC_ERR_REJECTED, "qtc_vk_surface: no surface for the window"); return 0; }
        uint64_t out = 0;
        static_assert(sizeof(s) <= sizeof(out), "VkSurfaceKHR");
        std::memcpy(&out, &s, sizeof s);
        return out;
#else
        (void)h, (void)window;
        fail(QTC_ERR_UNSUPPORTED, "Vulkan is not in this build");
        return 0;
#endif
    });
}

// ------------------------------------------------------------------ QRhiWidget

#ifdef QTC_RHI_WIDGET
namespace qtc {
namespace {
// QRhiWidget's rhi() and renderTarget() are protected: reached through member pointers of a
// class that makes them public (no cast of the widget).
struct RhiWidgetAccess : QRhiWidget {
    using QRhiWidget::renderTarget;
    using QRhiWidget::rhi;
};
QRhi *widget_rhi(QRhiWidget *w) { return (w->*(&RhiWidgetAccess::rhi))(); }
QRhiRenderTarget *widget_target(QRhiWidget *w) { return (w->*(&RhiWidgetAccess::renderTarget))(); }

// The frame borrowed from a widget's override call ends with the call; its target handles go too.
void end_borrowed(uint64_t h) {
    Ctx *c = ctx_quiet(h);
    if (!c || !c->external_frame) return;
    close_pass(c);
    if (QRhiResourceUpdateBatch *b = take_pending(c)) c->cb->resourceUpdate(b);
    reset_frame_state(c);
    const std::vector<uint64_t> kids = c->children;
    for (uint64_t x : kids)
        if (RObj *t = find(x, RK_RT); t && t->external) release(x);
}

// The wrapper of a widget's RHI: one per QRhi (the widgets of a window share it), released
// before the QRhi goes.
uint64_t wrap_widget_rhi(QRhi *rhi) {
    if (!rhi) return 0;
    auto it = widget_rhis().constFind(rhi);
    if (it != widget_rhis().cend() && find(*it, RK_RHI)) return *it;
    const uint64_t h = alloc(RK_RHI, 0);
    if (!h) {
        fail(QTC_ERR_LIMIT, "too many RHI objects");
        return 0;
    }
    auto *c = new Ctx;
    c->rhi = rhi;
    c->owned = false;
    switch (rhi->backend()) {
    case QRhi::OpenGLES2: c->backend = QTC_RHI_BACKEND_OPENGL; break;
    case QRhi::Vulkan: c->backend = QTC_RHI_BACKEND_VULKAN; break;
    case QRhi::D3D11: c->backend = QTC_RHI_BACKEND_D3D11; break;
    case QRhi::D3D12: c->backend = QTC_RHI_BACKEND_D3D12; break;
    case QRhi::Metal: c->backend = QTC_RHI_BACKEND_METAL; break;
    default: c->backend = QTC_RHI_BACKEND_NULL; break;
    }
    find(h, RK_RHI)->ctx = c;
    widget_rhis().insert(rhi, h);
    rhi->addCleanupCallback(c, [h](QRhi *) {
        if (Ctx *cc = ctx_quiet(h)) {
            cc->in_cleanup = true;
            release(h);
        }
    });
    return h;
}
}  // namespace
}  // namespace qtc
#endif

QTC_API qtc_rhi qtc_rhi_widget(qtc_obj widget) {
    return guarded<qtc_rhi>(0, [&]() -> qtc_rhi {
#ifdef QTC_RHI_WIDGET
        qtc_status err = 0;
        QObject *o = reg().object(widget, &err);
        if (!o) { fail(err, "qtc_rhi_widget"); return 0; }
        auto *w = qobject_cast<QRhiWidget *>(o);
        if (!w) { fail(QTC_ERR_WRONG_TYPE, "qtc_rhi_widget: not a QRhiWidget"); return 0; }
        if (!widget_rhi(w)) { fail(QTC_ERR_STATE, "qtc_rhi_widget: the widget has no RHI yet (call it in its initialize or render override)"); return 0; }
        const uint64_t h = wrap_widget_rhi(widget_rhi(w));
        if (!h) return 0;
        Ctx *c = ctx_quiet(h);
        // inside the widget's initialize or render override: its frame records the commands
        if (QRhiCommandBuffer *cb = ov_rhi_cb_arg(w); cb && !c->in_frame) {
            reset_pass_state(c);
            c->in_frame = true;
            c->external_frame = true;
            c->cb = cb;
            ov_at_return(&end_borrowed, h);
        }
        return h;
#else
        (void)widget;
        fail(QTC_ERR_UNSUPPORTED, "QRhiWidget needs Qt Widgets 6.7 or later");
        return 0;
#endif
    });
}

QTC_API qtc_rhi qtc_rhi_widget_target(qtc_obj widget) {
    return guarded<qtc_rhi>(0, [&]() -> qtc_rhi {
#ifdef QTC_RHI_WIDGET
        qtc_status err = 0;
        QObject *o = reg().object(widget, &err);
        if (!o) { fail(err, "qtc_rhi_widget_target"); return 0; }
        auto *w = qobject_cast<QRhiWidget *>(o);
        if (!w) { fail(QTC_ERR_WRONG_TYPE, "qtc_rhi_widget_target: not a QRhiWidget"); return 0; }
        if (!ov_rhi_cb_arg(w)) { fail(QTC_ERR_STATE, "qtc_rhi_widget_target: inside the widget's initialize or render override"); return 0; }
        QRhiRenderTarget *rt = widget_target(w);
        if (!rt) { fail(QTC_ERR_STATE, "qtc_rhi_widget_target: the widget has no render target yet"); return 0; }
        const uint64_t rh = qtc_rhi_widget(widget);   // borrows the frame too
        if (!rh) return 0;
        for (uint64_t k : ctx_quiet(rh)->children)
            if (RObj *x = find(k, RK_RT); x && x->external && x->res == rt) return k;
        const uint64_t h = child(rh, RK_RT);
        if (!h) return 0;
        RObj *t = find(h, RK_RT);
        t->res = rt;
        t->rp = rt->renderPassDescriptor();
        t->external = true;
        if (rt->resourceType() == QRhiResource::TextureRenderTarget) {
            const QRhiTextureRenderTargetDescription d = static_cast<QRhiTextureRenderTarget *>(rt)->description();
            t->has_depth = d.depthStencilBuffer() || d.depthTexture();
        }
        const QSize sz = rt->pixelSize();
        t->w = sz.width();
        t->h = sz.height();
        return h;
#else
        (void)widget;
        fail(QTC_ERR_UNSUPPORTED, "QRhiWidget needs Qt Widgets 6.7 or later");
        return 0;
#endif
    });
}

namespace qtc {
void rhi_shutdown() {
    Table &tb = T();
    // RHIs first (with their resources), then the rest: shaders and Vulkan instances
    for (uint32_t i = 0; i < tb.t.size(); i++)
        if (tb.t[i].kind == RK_RHI) release(rh_make(i, tb.t[i].gen));
    for (uint32_t i = 0; i < tb.t.size(); i++)
        if (tb.t[i].kind != RK_FREE) release(rh_make(i, tb.t[i].gen));
}
}  // namespace qtc

#else   // no RHI in this Qt

#define RHI_STUB(rt, def, name, params)                                                   \
    QTC_API rt name params {                                                              \
        return guarded<rt>(def, [&]() -> rt {                                             \
            fail(QTC_ERR_UNSUPPORTED, "the RHI needs Qt GUI 6.7 or later in this build"); \
            return def;                                                                   \
        });                                                                               \
    }
#define QTC_NOVIEW (qtc_view{"", 0})
RHI_STUB(qtc_rhi, 0, qtc_rhi_create, (int32_t, uint32_t))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_destroy, (qtc_rhi))
RHI_STUB(qtc_view, QTC_NOVIEW, qtc_rhi_info, (qtc_rhi))
RHI_STUB(bool, false, qtc_rhi_supports, (qtc_rhi, int32_t))
RHI_STUB(int64_t, 0, qtc_rhi_live, (void))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_finish, (qtc_rhi))
RHI_STUB(qtc_rhi, 0, qtc_rhi_shader_bake, (uint32_t, qtc_str))
RHI_STUB(qtc_rhi, 0, qtc_rhi_shader_load, (qtc_str))
RHI_STUB(qtc_view, QTC_NOVIEW, qtc_rhi_shader_save, (qtc_rhi))
RHI_STUB(qtc_view, QTC_NOVIEW, qtc_rhi_shader_reflect, (qtc_rhi))
RHI_STUB(qtc_rhi, 0, qtc_rhi_buffer, (qtc_rhi, int32_t, uint32_t, int64_t))
RHI_STUB(qtc_rhi, 0, qtc_rhi_texture, (qtc_rhi, int32_t, int32_t, int32_t, uint32_t))
RHI_STUB(qtc_rhi, 0, qtc_rhi_sampler, (qtc_rhi, bool, bool))
RHI_STUB(qtc_rhi, 0, qtc_rhi_render_target, (qtc_rhi, bool))
RHI_STUB(qtc_rhi, 0, qtc_rhi_swapchain, (qtc_rhi, qtc_obj, uint32_t))
RHI_STUB(qtc_rhi, 0, qtc_rhi_bindings, (qtc_rhi))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_bind_uniform, (qtc_rhi, int32_t, uint32_t, qtc_rhi, int64_t, int64_t))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_bind_texture, (qtc_rhi, int32_t, uint32_t, qtc_rhi, qtc_rhi))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_bind_storage, (qtc_rhi, int32_t, uint32_t, qtc_rhi, int32_t))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_bind_image, (qtc_rhi, int32_t, uint32_t, qtc_rhi, int32_t))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_bindings_create, (qtc_rhi))
RHI_STUB(qtc_rhi, 0, qtc_rhi_pipeline, (qtc_rhi))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_pipeline_shaders, (qtc_rhi, qtc_rhi, qtc_rhi))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_pipeline_vertex_binding, (qtc_rhi, int32_t, bool))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_pipeline_vertex_attribute, (qtc_rhi, int32_t, int32_t, int32_t, int32_t))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_pipeline_options, (qtc_rhi, int32_t, int32_t, bool, bool, bool))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_pipeline_create, (qtc_rhi, qtc_rhi, qtc_rhi))
RHI_STUB(qtc_rhi, 0, qtc_rhi_compute_pipeline, (qtc_rhi, qtc_rhi, qtc_rhi))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_upload, (qtc_rhi, int64_t, qtc_str))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_upload_image, (qtc_rhi, qtc_val))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_upload_pixels, (qtc_rhi, qtc_str))
RHI_STUB(qtc_rhi, 0, qtc_rhi_read_texture, (qtc_rhi))
RHI_STUB(qtc_rhi, 0, qtc_rhi_read_buffer, (qtc_rhi, int64_t, int64_t))
RHI_STUB(qtc_rhi, 0, qtc_rhi_read_swapchain, (qtc_rhi))
RHI_STUB(bool, false, qtc_rhi_readback_ready, (qtc_rhi))
RHI_STUB(qtc_view, QTC_NOVIEW, qtc_rhi_readback_bytes, (qtc_rhi))
RHI_STUB(qtc_val, 0, qtc_rhi_readback_image, (qtc_rhi))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_begin_frame, (qtc_rhi, qtc_rhi))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_end_frame, (qtc_rhi))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_begin_pass, (qtc_rhi, qtc_rhi, uint32_t, double, uint32_t))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_begin_compute, (qtc_rhi, uint32_t))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_end_pass, (qtc_rhi))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_set_pipeline, (qtc_rhi, qtc_rhi))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_set_bindings, (qtc_rhi, qtc_rhi))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_set_viewport, (qtc_rhi, double, double, double, double))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_set_vertex_buffer, (qtc_rhi, int32_t, qtc_rhi, int64_t))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_set_index_buffer, (qtc_rhi, qtc_rhi, int64_t, bool))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_draw, (qtc_rhi, int64_t, int64_t, int64_t, int64_t))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_draw_indexed, (qtc_rhi, int64_t, int64_t, int64_t, int64_t, int64_t))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_dispatch, (qtc_rhi, int32_t, int32_t, int32_t))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_native, (qtc_rhi, qtc_rhi_native_handles *))
RHI_STUB(uint64_t, 0, qtc_rhi_native_object, (qtc_rhi, int32_t *))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_make_current, (qtc_rhi))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_begin_external, (qtc_rhi, void **))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_rhi_end_external, (qtc_rhi))
RHI_STUB(qtc_rhi, 0, qtc_rhi_widget, (qtc_obj))
RHI_STUB(qtc_rhi, 0, qtc_rhi_widget_target, (qtc_obj))
RHI_STUB(qtc_rhi, 0, qtc_vk_create, (qtc_str, qtc_str))
RHI_STUB(void *, nullptr, qtc_vk_instance, (qtc_rhi))
RHI_STUB(void *, nullptr, qtc_vk_proc_address, (qtc_rhi, qtc_str))
RHI_STUB(qtc_status, QTC_ERR_UNSUPPORTED, qtc_vk_attach, (qtc_rhi, qtc_obj))
RHI_STUB(uint64_t, 0, qtc_vk_surface, (qtc_rhi, qtc_obj))

namespace qtc {
void rhi_shutdown() {}
}  // namespace qtc

#endif  // QTC_RHI
