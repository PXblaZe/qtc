/* fuzz_rhi: random sequences of graphics calls (qtc_rhi_*), with handles of every kind (live,
   destroyed, of another RHI, made up) and edge-value arguments, on every backend this machine
   has. Nothing may crash, hang or trip a sanitizer; every call returns. (The shaders check their
   own indexes: what a shader computes is the program's, see docs/LIMITS.md.) Each run checks that
   destroying everything leaves no handle behind.

     fuzz_rhi [seed] [steps]          (defaults: seed 1, 20000 steps per backend) */
#define _POSIX_C_SOURCE 200809L
#include "qtc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t g_rng;
static uint64_t rnd(void) {   /* splitmix64 */
    uint64_t z = (g_rng += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static int64_t pick(int64_t n) { return n > 0 ? (int64_t)(rnd() % (uint64_t)n) : 0; }

static const int64_t EDGE_I64[] = {0, 1, -1, 2, 3, 4, 6, 7, 8, 12, 16, 20, 64, 255, 256, 4096, 65535, 65536,
                                   (1 << 20), (1 << 30), INT32_MAX, INT32_MIN, INT64_MAX, INT64_MIN};
static int64_t ival(void) { return (rnd() & 3) ? EDGE_I64[pick(sizeof EDGE_I64 / sizeof EDGE_I64[0])] : (int64_t)rnd(); }
static int32_t i32(void) { return (int32_t)(ival() & 0xffffffff) % ((rnd() & 1) ? 8 : 70000); }
static double dval(void) {
    static const double d[] = {0, 1, -1, 0.5, 2, 1e9, -1e9, 1.0 / 0.0, -1.0 / 0.0, 0.0 / 0.0, 32768, 32769, 64};
    return d[pick(sizeof d / sizeof d[0])];
}

/* handle pools, by kind: every handle a call returned (live or not), plus made-up ones */
enum { K_ANY, K_BUF, K_UBUF, K_IBUF, K_SBUF, K_TEX, K_RT, K_SRB, K_PIPE, K_CPIPE, K_RB, K_SHADER, K_SC, K_KINDS };
#define POOL 128
static qtc_rhi g_h[K_KINDS][POOL];
static int g_n[K_KINDS];
static void keep_k(qtc_rhi h, int k) {
    if (!h) return;
    for (int kk = 0; kk < 2; kk++) {
        const int kind = kk ? K_ANY : k;
        if (g_n[kind] < POOL) g_h[kind][g_n[kind]++] = h;
        else g_h[kind][pick(POOL)] = h;
    }
}
static void keep(qtc_rhi h) { keep_k(h, K_ANY); }
static qtc_rhi any_k(int k) {
    switch (pick(12)) {
    case 0: return 0;
    case 1: return rnd();
    case 2: return (rnd() & 0xffffffff00000000ull) | 0x52000001ull;   /* RHI-tagged, made up */
    case 3: case 4: return g_n[K_ANY] ? g_h[K_ANY][pick(g_n[K_ANY])] : 0;
    default: return g_n[k] ? g_h[k][pick(g_n[k])] : (g_n[K_ANY] ? g_h[K_ANY][pick(g_n[K_ANY])] : 0);
    }
}
static qtc_rhi any_h(void) { return any_k(K_ANY); }

static const char *SHADERS[] = {
    "#version 440\nlayout(location = 0) in vec2 pos;\nlayout(location = 1) in vec3 color;\nlayout(location = 0) out vec3 v_color;\n"
    "layout(std140, binding = 0) uniform buf { vec4 tint; };\nvoid main() { v_color = color * tint.rgb; gl_Position = vec4(pos, 0.0, 1.0); }\n",
    "#version 440\nlayout(location = 0) in vec3 v_color;\nlayout(location = 0) out vec4 frag;\nvoid main() { frag = vec4(v_color, 1.0); }\n",
    "#version 440\nlayout(location = 0) in vec2 pos;\nlayout(location = 0) out vec2 v_uv;\nvoid main() { v_uv = pos; gl_PointSize = 2.0; gl_Position = vec4(pos, 0.0, 1.0); }\n",
    "#version 440\nlayout(location = 0) in vec2 v_uv;\nlayout(location = 0) out vec4 frag;\nlayout(binding = 1) uniform sampler2D tex;\nvoid main() { frag = texture(tex, v_uv); }\n",
    "#version 440\nlayout(local_size_x = 16) in;\nlayout(std430, binding = 0) buffer Data { uint vals[]; };\nvoid main() { uint i = gl_GlobalInvocationID.x; if (i < uint(vals.length())) vals[i] = 1u; }\n",
    "#version 440\nlayout(local_size_x = 4, local_size_y = 4) in;\nlayout(rgba8, binding = 0) uniform writeonly image2D img;\n"
    "void main() { ivec2 p = ivec2(gl_GlobalInvocationID.xy); if (all(lessThan(p, imageSize(img)))) imageStore(img, p, vec4(1.0)); }\n",
};
static qtc_rhi g_shaders[6];
static int g_trace;
static qtc_rhi g_all[100000];   /* every shader made, to destroy at the end */
static int g_nall;
static int g_ok_draw, g_ok_indexed, g_ok_dispatch, g_ok_pipe, g_ok_frame;

/* A valid setup to mutate from: a target, buffers, bindings and pipelines. */
typedef struct { qtc_rhi rt, p, srb, vb, ib, cp, csrb; } Scene;
static Scene g_scenes[64];
static int g_nscenes;
static void build(qtc_rhi r) {
    static const float quad[] = {-1, -1, 1, 0, 0, 1, -1, 0, 1, 0, 1, 1, 0, 0, 1, -1, 1, 1, 1, 1};
    static const uint16_t idx[] = {0, 1, 2, 0, 2, 3};
    static const float tint[4] = {1, 1, 1, 1};
    qtc_rhi tex = qtc_rhi_texture(r, QTC_RHI_RGBA8, 16, 16, QTC_RHI_RENDER_TARGET | QTC_RHI_READABLE);
    keep_k(tex, K_TEX);
    qtc_rhi rt = qtc_rhi_render_target(tex, rnd() & 1);
    keep_k(rt, K_RT);
    qtc_rhi vb = qtc_rhi_buffer(r, QTC_RHI_STATIC, QTC_RHI_VERTEX_BUFFER, sizeof quad);
    qtc_rhi_upload(vb, 0, qtc_sn((const char *)quad, sizeof quad));
    keep_k(vb, K_BUF);
    qtc_rhi ib = qtc_rhi_buffer(r, QTC_RHI_STATIC, QTC_RHI_INDEX_BUFFER, sizeof idx);
    qtc_rhi_upload(ib, 0, qtc_sn((const char *)idx, sizeof idx));
    keep_k(ib, K_IBUF);
    qtc_rhi ub = qtc_rhi_buffer(r, QTC_RHI_DYNAMIC, QTC_RHI_UNIFORM_BUFFER, 256);
    qtc_rhi_upload(ub, 0, qtc_sn((const char *)tint, sizeof tint));
    keep_k(ub, K_UBUF);
    qtc_rhi srb = qtc_rhi_bindings(r);
    qtc_rhi_bind_uniform(srb, 0, QTC_RHI_VERTEX | QTC_RHI_FRAGMENT, ub, 0, 16);
    qtc_rhi_bindings_create(srb);
    keep_k(srb, K_SRB);
    /* the shared shaders may have been destroyed by a random step: made again then */
    for (int i = 0; i < 6; i++)
        if (qtc_rhi_shader_reflect(g_shaders[i]).len == 0) {
            g_shaders[i] = qtc_rhi_shader_bake(i >= 4 ? QTC_RHI_COMPUTE : (i % 2 ? QTC_RHI_FRAGMENT : QTC_RHI_VERTEX),
                                               qtc_sn(SHADERS[i], (int64_t)strlen(SHADERS[i])));
            keep_k(g_shaders[i], K_SHADER);
            if (g_nall < (int)(sizeof g_all / sizeof g_all[0])) g_all[g_nall++] = g_shaders[i];
        }
    qtc_rhi p = qtc_rhi_pipeline(r);
    qtc_rhi_pipeline_shaders(p, g_shaders[0], g_shaders[1]);
    qtc_rhi_pipeline_vertex_binding(p, 20, false);
    qtc_rhi_pipeline_vertex_attribute(p, 0, 0, QTC_RHI_FLOAT2, 0);
    qtc_rhi_pipeline_vertex_attribute(p, 0, 1, QTC_RHI_FLOAT3, 8);
    if (rnd() & 1) qtc_rhi_pipeline_options(p, (int32_t)pick(4), (int32_t)pick(3), rnd() & 1, rnd() & 1, rnd() & 1);
    if (qtc_rhi_pipeline_create(p, srb, rt) == QTC_OK) g_ok_pipe++;
    else if (g_trace) fprintf(stderr, "build: %.*s\n", (int)qtc_last_error_message().len, qtc_last_error_message().data);
    keep_k(p, K_PIPE);
    if (qtc_rhi_supports(r, QTC_RHI_FEATURE_COMPUTE)) {
        qtc_rhi sb = qtc_rhi_buffer(r, QTC_RHI_STATIC, QTC_RHI_STORAGE_BUFFER, 16 * 4 * (1 + pick(4)));
        keep_k(sb, K_SBUF);
        qtc_rhi cs = qtc_rhi_bindings(r);
        qtc_rhi_bind_storage(cs, 0, QTC_RHI_COMPUTE, sb, QTC_RHI_LOAD_STORE);
        qtc_rhi_bindings_create(cs);
        keep_k(cs, K_SRB);
        qtc_rhi cp = qtc_rhi_compute_pipeline(r, g_shaders[4], cs);
        keep_k(cp, K_CPIPE);
        g_scenes[g_nscenes % 64].cp = cp;
        g_scenes[g_nscenes % 64].csrb = cs;
    }
    Scene *sc = &g_scenes[g_nscenes++ % 64];
    sc->rt = rt;
    sc->p = p;
    sc->srb = srb;
    sc->vb = vb;
    sc->ib = ib;
}

static void step(qtc_rhi rhis[2], qtc_obj window) {
    const qtc_rhi r = (rnd() & 7) ? rhis[0] : rhis[pick(2)];
    uint8_t bytes[256];
    for (size_t i = 0; i < sizeof bytes; i++) bytes[i] = (uint8_t)rnd();
    /* indices that are usually small, so that some draws pass */
    if (rnd() & 1)
        for (size_t i = 0; i < sizeof bytes; i++) bytes[i] = (uint8_t)(i & 1 ? 0 : rnd() % 8);
    const int64_t nb = pick(sizeof bytes + 1);
    const int64_t op = pick(50);
    if (g_trace) fprintf(stderr, "op %lld\n", (long long)op);
    switch (op) {
    case 0: keep(qtc_rhi_buffer(r, (int32_t)pick(4) - 1 + (rnd() & 1), (uint32_t)(1u << pick(5)) | (uint32_t)(rnd() & 1 ? 0 : 1u << pick(5)), (rnd() & 1) ? 16 * (1 + pick(16)) : ival())); break;
    case 1: keep(qtc_rhi_texture(r, (int32_t)pick(8), (rnd() & 3) ? 1 + (int32_t)pick(64) : i32(), (rnd() & 3) ? 1 + (int32_t)pick(64) : i32(), (uint32_t)pick(8))); break;
    case 2: keep(qtc_rhi_sampler(r, rnd() & 1, rnd() & 1)); break;
    case 3: keep_k(qtc_rhi_render_target(any_k(K_TEX), rnd() & 1), K_RT); break;
    case 4: if (rnd() & 1) build(r); else keep_k(qtc_rhi_bindings(r), K_SRB); break;
    case 5: qtc_rhi_bind_uniform(any_k(K_SRB), (int32_t)pick(4), (uint32_t)pick(8), any_k(K_UBUF), (rnd() & 1) ? 0 : ival(), (rnd() & 1) ? 0 : ival()); break;
    case 6: qtc_rhi_bind_texture(any_k(K_SRB), (int32_t)pick(4), (uint32_t)pick(8), any_k(K_TEX), any_h()); break;
    case 7: qtc_rhi_bind_storage(any_k(K_SRB), (int32_t)pick(4), (uint32_t)pick(8), any_k(K_SBUF), (int32_t)pick(5)); break;
    case 8: qtc_rhi_bind_image(any_k(K_SRB), (int32_t)pick(4), (uint32_t)pick(8), any_k(K_TEX), (int32_t)pick(5)); break;
    case 9: qtc_rhi_bindings_create(any_k(K_SRB)); break;
    case 10: keep_k(qtc_rhi_pipeline(r), K_PIPE); break;
    case 11: qtc_rhi_pipeline_shaders(any_k(K_PIPE), (rnd() & 1) ? g_shaders[pick(6)] : any_h(), (rnd() & 1) ? g_shaders[pick(6)] : any_h()); break;
    case 12: qtc_rhi_pipeline_vertex_binding(any_k(K_PIPE), (rnd() & 1) ? 4 * (1 + (int32_t)pick(8)) : i32(), rnd() & 1); break;
    case 13: qtc_rhi_pipeline_vertex_attribute(any_k(K_PIPE), (int32_t)pick(3), (int32_t)pick(4), (int32_t)pick(9), (rnd() & 1) ? 4 * (int32_t)pick(6) : i32()); break;
    case 14: qtc_rhi_pipeline_options(any_k(K_PIPE), (int32_t)pick(7) - 1, (int32_t)pick(5) - 1, rnd() & 1, rnd() & 1, rnd() & 1); break;
    case 15: g_ok_pipe += qtc_rhi_pipeline_create(any_k(K_PIPE), any_k(K_SRB), (rnd() & 3) ? any_k(K_RT) : any_k(K_SC)) == QTC_OK; break;
    case 16: keep_k(qtc_rhi_compute_pipeline(r, (rnd() & 1) ? g_shaders[4 + pick(2)] : any_h(), any_k(K_SRB)), K_CPIPE); break;
    case 17: qtc_rhi_upload((rnd() & 1) ? any_k(K_IBUF) : any_k(K_BUF), (rnd() & 1) ? 2 * pick(16) : ival(), qtc_sn((const char *)bytes, nb)); break;
    case 18: qtc_rhi_upload_pixels(any_k(K_TEX), qtc_sn((const char *)bytes, nb)); break;
    case 19: keep_k(qtc_rhi_read_texture(any_k(K_TEX)), K_RB); break;
    case 20: keep_k(qtc_rhi_read_buffer((rnd() & 1) ? any_k(K_SBUF) : any_k(K_UBUF), (rnd() & 1) ? 0 : ival(), (rnd() & 1) ? 16 : ival()), K_RB); break;
    case 21: qtc_rhi_readback_ready(any_k(K_RB)); qtc_rhi_readback_bytes(any_k(K_RB)); break;
    case 22: {
        qtc_val v = qtc_rhi_readback_image(any_k(K_RB));
        if (v && (rnd() & 1)) qtc_rhi_upload_image(any_k(K_TEX), v);
        qtc_val_free(v);
        break;
    }
    case 23: case 24: qtc_rhi_begin_frame(r, (rnd() & 3) ? 0 : any_k(K_SC)); break;
    case 25: g_ok_frame += qtc_rhi_end_frame(r) == QTC_OK; break;
    case 26: case 27: qtc_rhi_begin_pass(r, (rnd() & 7) ? any_k(K_RT) : any_k(K_SC), (uint32_t)rnd(), (rnd() & 3) ? 1.0 : dval(), (uint32_t)pick(3)); break;
    case 28: qtc_rhi_begin_compute(r, (uint32_t)pick(3)); break;
    case 29: qtc_rhi_end_pass(r); break;
    case 30: case 31: qtc_rhi_set_pipeline(r, (rnd() & 3) ? any_k(K_PIPE) : any_k(K_CPIPE)); break;
    case 32: case 33: qtc_rhi_set_bindings(r, any_k(K_SRB)); break;
    case 34: qtc_rhi_set_viewport(r, dval(), dval(), dval(), dval()); break;
    case 35: qtc_rhi_set_vertex_buffer(r, (rnd() & 1) ? 0 : (int32_t)pick(4) - (rnd() & 1), any_k(K_BUF), (rnd() & 1) ? 0 : ival()); break;
    case 36: qtc_rhi_set_index_buffer(r, any_k(K_IBUF), (rnd() & 1) ? 0 : ival(), rnd() & 1); break;
    case 37: case 38: g_ok_draw += qtc_rhi_draw(r, (rnd() & 1) ? pick(8) : ival(), (rnd() & 1) ? 1 : ival(), (rnd() & 1) ? 0 : ival(), (rnd() & 1) ? 0 : ival()) == QTC_OK; break;
    case 39: g_ok_indexed += qtc_rhi_draw_indexed(r, (rnd() & 1) ? pick(64) : ival(), (rnd() & 1) ? 1 : ival(), (rnd() & 1) ? 0 : ival(), (rnd() & 3) ? 0 : ival(), 0) == QTC_OK; break;
    case 40: g_ok_dispatch += qtc_rhi_dispatch(r, (rnd() & 1) ? 1 : i32(), 1, (rnd() & 1) ? 1 : i32()) == QTC_OK; break;
    case 41: {
        void *ncb = NULL;
        if (qtc_rhi_begin_external(r, &ncb) == QTC_OK && (rnd() & 1)) qtc_rhi_end_external(r);
        break;
    }
    case 42: qtc_rhi_end_external(r); break;
    case 43: {
        int32_t layout;
        qtc_rhi_native_object((rnd() & 1) ? any_k(K_TEX) : any_k(K_BUF), &layout);
        qtc_rhi_native_handles nh;
        qtc_rhi_native(any_h(), &nh);
        qtc_rhi_supports(any_h(), (int32_t)pick(9));
        break;
    }
    case 44:   /* destruction, of anything, at any time */
        if (rnd() & 1) qtc_rhi_destroy(any_h());
        break;
    case 46: case 47: case 48: case 49: {
        /* a draw sequence, mostly with one scene's parts, else from the pools: whatever of it is
           valid goes through */
        Scene sc = {0};
        if (g_nscenes && (rnd() & 3)) sc = g_scenes[pick(g_nscenes < 64 ? g_nscenes : 64)];
#define PART(field, kind) ((sc.field && (rnd() & 7)) ? sc.field : any_k(kind))
        if (rnd() & 1) qtc_rhi_begin_frame(r, 0);
        if (rnd() & 3) qtc_rhi_begin_pass(r, PART(rt, K_RT), 0xff000000u, 1.0, (rnd() & 7) ? 0 : QTC_RHI_PASS_EXTERNAL);
        const bool compute = (rnd() & 3) == 0;
        if (compute) {
            qtc_rhi_end_pass(r);
            qtc_rhi_begin_compute(r, 0);
            qtc_rhi_set_pipeline(r, PART(cp, K_CPIPE));
            qtc_rhi_set_bindings(r, PART(csrb, K_SRB));
            g_ok_dispatch += qtc_rhi_dispatch(r, 1 + (int32_t)pick(8), 1, 1) == QTC_OK;
        } else {
            qtc_rhi_set_pipeline(r, PART(p, K_PIPE));
            qtc_rhi_set_bindings(r, PART(srb, K_SRB));
            qtc_rhi_set_vertex_buffer(r, 0, PART(vb, K_BUF), (rnd() & 3) ? 0 : 4 * pick(8));
            if (rnd() & 1) qtc_rhi_set_viewport(r, 0, 0, 1 + pick(32), 1 + pick(32));
            for (int k = 0; k < 4; k++) {
                if (rnd() & 1) {
                    g_ok_draw += qtc_rhi_draw(r, pick(6), 1 + pick(2), pick(3), 0) == QTC_OK;
                } else {
                    qtc_rhi_set_index_buffer(r, PART(ib, K_IBUF), (rnd() & 3) ? 0 : 2 * pick(4), (rnd() & 7) == 0);
                    g_ok_indexed += qtc_rhi_draw_indexed(r, pick(8), 1 + pick(2), pick(3), (rnd() & 3) ? 0 : pick(3), 0) == QTC_OK;
                }
            }
        }
        if (rnd() & 1) qtc_rhi_end_pass(r);
        if (rnd() & 1) g_ok_frame += qtc_rhi_end_frame(r) == QTC_OK;
        break;
    }
    case 45:
        if (window && (rnd() & 7) == 0) keep_k(qtc_rhi_swapchain(r, window, (uint32_t)pick(8)), K_SC);
        else qtc_rhi_finish(r);
        break;
    }
}

int main(int argc, char **argv) {
    const uint64_t seed = argc > 1 ? strtoull(argv[1], NULL, 10) : 1;
    const int steps = argc > 2 ? atoi(argv[2]) : 20000;
    if (qtc_init(QTC_APP_WIDGETS, qtc_sn(NULL, 0)) != QTC_OK) return 2;
    qtc_set_error_policy(QTC_ERRORS_RETURN);
    qtc_set_quit_on_last_window_closed(false);
    int failures = 0;
    /* FUZZ_RHI_BACKEND=n: that backend only; FUZZ_RHI_DEBUG=1: the backends' validation layers;
       FUZZ_RHI_TRACE=1: each step's operation on stderr (to find what a crash came after) */
    const char *only = getenv("FUZZ_RHI_BACKEND");
    const uint32_t flags = getenv("FUZZ_RHI_DEBUG") ? QTC_RHI_DEBUG : 0;
    g_trace = getenv("FUZZ_RHI_TRACE") != NULL;
    for (int32_t backend = QTC_RHI_BACKEND_NULL; backend <= QTC_RHI_BACKEND_VULKAN; backend++) {
        if (only && atoi(only) != backend) continue;
        g_rng = seed * 1000003u + (uint64_t)backend;
        memset(g_n, 0, sizeof g_n);
        g_nscenes = 0;
        const int64_t live0 = qtc_rhi_live();
        qtc_rhi rhis[2] = {qtc_rhi_create(backend, flags), qtc_rhi_create(backend, flags)};
        if (!rhis[0] || !rhis[1]) {
            printf("fuzz_rhi: backend %d not available here\n", backend);
            qtc_rhi_destroy(rhis[0]);
            qtc_rhi_destroy(rhis[1]);
            continue;
        }
        for (int i = 0; i < 6; i++) g_shaders[i] = qtc_rhi_shader_bake(i == 4 || i == 5 ? QTC_RHI_COMPUTE : (i % 2 ? QTC_RHI_FRAGMENT : QTC_RHI_VERTEX), qtc_sn(SHADERS[i], (int64_t)strlen(SHADERS[i])));
        for (int i = 0; i < 6; i++) keep_k(g_shaders[i], K_SHADER);
        qtc_obj window = qtc_new(QTC_S("QWindow"));
        qtc_arg_int(32);
        qtc_arg_int(32);
        qtc_call(window, QTC_S("resize"));
        for (int i = 0; i < steps; i++) {
            step(rhis, window);
            if (i % 500 == 0) {
                qtc_process_events();
                qtc_event e;
                while (qtc_poll(&e)) {}
            }
            if (i == steps / 2) {
                qtc_call(window, QTC_S("show"));   /* swapchains made before and after it shows */
                qtc_process_events();
            }
            if (i == steps * 3 / 4) {
                qtc_destroy(window);   /* swapchains lose their window */
                window = 0;
            }
        }
        /* end whatever is open, then everything goes with the RHIs */
        for (int k = 0; k < 2; k++) {
            qtc_rhi_end_frame(rhis[k]);
            if (qtc_rhi_destroy(rhis[k]) != QTC_OK) {
                printf("fuzz_rhi: backend %d: an RHI could not be destroyed: %.*s\n", backend,
                       (int)qtc_last_error_message().len, qtc_last_error_message().data);
                failures++;
            }
        }
        for (int i = 0; i < g_nall; i++) qtc_rhi_destroy(g_all[i]);   /* the destroyed ones fail */
        g_nall = 0;
        for (int i = 0; i < 6; i++) qtc_rhi_destroy(g_shaders[i]);
        if (window) qtc_destroy(window);
        const int64_t left = qtc_rhi_live() - live0;
        if (left != 0) {
            printf("fuzz_rhi: backend %d: %lld handles left behind\n", backend, (long long)left);
            failures++;
        }
        printf("fuzz_rhi: backend %d: %d steps, seed %llu: %d pipelines, %d frames, %d draws, %d indexed, %d dispatches went through\n",
               backend, steps, (unsigned long long)seed, g_ok_pipe, g_ok_frame, g_ok_draw, g_ok_indexed, g_ok_dispatch);
        g_ok_draw = g_ok_indexed = g_ok_dispatch = g_ok_pipe = g_ok_frame = 0;
    }
    if (qtc_shutdown() != QTC_OK) failures++;
    printf("fuzz_rhi: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
