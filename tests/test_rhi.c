/* qtc graphics tests: the RHI (qtc_rhi_*) on every backend this machine has (Null always; OpenGL
   and Vulkan when a driver is there, e.g. Mesa's llvmpipe and lavapipe under Xvfb), swapchains on
   windows, QRhiWidget overrides, Qt Quick's API choice and the native OpenGL / Vulkan interop.
   C against the public API only. Rendering is checked by reading pixels back. */
#define _POSIX_C_SOURCE 200809L
#include "qtc.h"

#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed = 0, g_passed = 0;
#define CHECK(cond)                                                                                       \
    do {                                                                                                  \
        if (cond) g_passed++;                                                                             \
        else {                                                                                            \
            g_failed++;                                                                                   \
            fprintf(stderr, "FAIL %s:%d: %s  [last error: %.*s]\n", __FILE__, __LINE__, #cond,            \
                    (int)qtc_last_error_message().len, qtc_last_error_message().data);                    \
        }                                                                                                 \
    } while (0)
#define CHECK_EQ(a, b) CHECK((a) == (b))
#define S(lit) QTC_S(lit)
#define NOSTR qtc_sn(NULL, 0)

static int view_has(qtc_view v, const char *needle) {
    size_t n = strlen(needle);
    for (int64_t i = 0; i + (int64_t)n <= v.len; i++)
        if (memcmp(v.data + i, needle, n) == 0) return 1;
    return 0;
}
static qtc_str bytes_of(const void *p, size_t n) { return qtc_sn((const char *)p, (int64_t)n); }
static void drain(void) {
    qtc_process_events();
    qtc_event e;
    while (qtc_poll(&e)) {}
    qtc_cancel_quit();
    qtc_clear_error();
}

/* ------------------------------------------------------------------ shaders */

static const char *VS_COLOR =
    "#version 440\n"
    "layout(location = 0) in vec2 pos;\n"
    "layout(location = 1) in vec3 color;\n"
    "layout(location = 0) out vec3 v_color;\n"
    "layout(std140, binding = 0) uniform buf { vec4 tint; };\n"
    "void main() { v_color = color * tint.rgb; gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *FS_COLOR =
    "#version 440\n"
    "layout(location = 0) in vec3 v_color;\n"
    "layout(location = 0) out vec4 frag;\n"
    "void main() { frag = vec4(v_color, 1.0); }\n";
static const char *VS_INST =
    "#version 440\n"
    "layout(location = 0) in vec2 pos;\n"
    "layout(location = 1) in vec2 offs;\n"
    "layout(location = 0) out vec3 v_color;\n"
    "layout(std140, binding = 0) uniform buf { vec4 tint; };\n"
    "void main() { v_color = tint.rgb; gl_Position = vec4(pos + offs, 0.0, 1.0); }\n";
static const char *VS_UV =
    "#version 440\n"
    "layout(location = 0) in vec2 pos;\n"
    "layout(location = 0) out vec2 v_uv;\n"
    "void main() { v_uv = vec2((pos.x + 1.0) * 0.5, (pos.y + 1.0) * 0.5); gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *FS_TEX =
    "#version 440\n"
    "layout(location = 0) in vec2 v_uv;\n"
    "layout(location = 0) out vec4 frag;\n"
    "layout(binding = 1) uniform sampler2D tex;\n"
    "void main() { frag = texture(tex, v_uv); }\n";
static const char *VS_POINTS_NO_SIZE =
    "#version 440\n"
    "layout(location = 0) in vec2 pos;\n"
    "layout(location = 1) in vec3 color;\n"
    "layout(location = 0) out vec3 v_color;\n"
    "void main() { v_color = color; gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *VS_INT =
    "#version 440\n"
    "layout(location = 0) in vec2 pos;\n"
    "layout(location = 1) in int which;\n"
    "layout(location = 0) out vec3 v_color;\n"
    "void main() { v_color = vec3(float(which)); gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *FS_OTHER_INPUT =
    "#version 440\n"
    "layout(location = 3) in vec3 nothing;\n"
    "layout(location = 0) out vec4 frag;\n"
    "void main() { frag = vec4(nothing, 1.0); }\n";
static const char *FS_BIG_UBUF =
    "#version 440\n"
    "layout(location = 0) in vec3 v_color;\n"
    "layout(location = 0) out vec4 frag;\n"
    "layout(std140, binding = 0) uniform buf { vec4 tint; vec4 more[8]; };\n"
    "void main() { frag = vec4(v_color, 1.0) * tint + more[3]; }\n";
static const char *CS_SQUARES =
    "#version 440\n"
    "layout(local_size_x = 64) in;\n"
    "layout(std430, binding = 0) buffer Data { uint vals[]; };\n"
    "void main() { uint i = gl_GlobalInvocationID.x; vals[i] = i * i + 7u; }\n";
static const char *CS_IMAGE =
    "#version 440\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(rgba8, binding = 0) uniform writeonly image2D img;\n"
    "void main() { imageStore(img, ivec2(gl_GlobalInvocationID.xy), vec4(0.0, 0.0, 1.0, 1.0)); }\n";

/* ------------------------------------------------------------------ geometry */

/* x, y, r, g, b */
static const float LEFT_GREEN[] = {-1, -1, 0, 1, 0, 0, -1, 0, 1, 0, 0, 1, 0, 1, 0,
                                   -1, -1, 0, 1, 0, 0, 1, 0, 1, 0, -1, 1, 0, 1, 0};
static const float RIGHT_BLUE4[] = {0, -1, 0, 0, 1, 1, -1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 1, 0, 0, 1};
static const uint16_t QUAD_IDX[] = {0, 1, 2, 0, 2, 3};
static const float FULL_QUAD[] = {-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1};
static const float LEFT_STRIP[] = {-1, -1, -0.5f, -1, -0.5f, 1, -1, -1, -0.5f, 1, -1, 1};   /* x in [-1, -0.5] */
static const float OFFSETS[] = {0, 0, 1.5f, 0};                                               /* + [0.5, 1] */
static const float WHITE[4] = {1, 1, 1, 1};

#define W 64
#define H 64

static const uint8_t *pixel(qtc_view v, int w, int x, int y) { return (const uint8_t *)v.data + ((int64_t)y * w + x) * 4; }
static int near(int a, int b) { return a - b <= 3 && b - a <= 3; }
static int is_rgba(const uint8_t *p, int r, int g, int b) { return near(p[0], r) && near(p[1], g) && near(p[2], b) && near(p[3], 255); }

static qtc_rhi bake(uint32_t stage, const char *src) {
    qtc_rhi s = qtc_rhi_shader_bake(stage, qtc_sn(src, (int64_t)strlen(src)));
    if (!s) fprintf(stderr, "bake failed: %.*s\n", (int)qtc_last_error_message().len, qtc_last_error_message().data);
    return s;
}

/* A color pipeline (pos, color; uniform tint) into `target`. */
static qtc_rhi color_pipeline(qtc_rhi r, qtc_rhi vs, qtc_rhi fs, qtc_rhi srb, qtc_rhi target) {
    qtc_rhi p = qtc_rhi_pipeline(r);
    CHECK(p != 0);
    CHECK_EQ(qtc_rhi_pipeline_shaders(p, vs, fs), QTC_OK);
    CHECK_EQ(qtc_rhi_pipeline_vertex_binding(p, 20, false), QTC_OK);
    CHECK_EQ(qtc_rhi_pipeline_vertex_attribute(p, 0, 0, QTC_RHI_FLOAT2, 0), QTC_OK);
    CHECK_EQ(qtc_rhi_pipeline_vertex_attribute(p, 0, 1, QTC_RHI_FLOAT3, 8), QTC_OK);
    CHECK_EQ(qtc_rhi_pipeline_create(p, srb, target), QTC_OK);
    return p;
}

/* ------------------------------------------------------------------ one backend */

static void test_backend(int32_t backend, const char *name) {
    const int64_t live0 = qtc_rhi_live();
    qtc_rhi r = qtc_rhi_create(backend, 0);
    if (!r) {
        printf("  %s: not available here (%.*s)\n", name, (int)qtc_last_error_message().len, qtc_last_error_message().data);
        CHECK_EQ(qtc_last_error(), QTC_ERR_UNSUPPORTED);
        return;
    }
    const int real = backend != QTC_RHI_BACKEND_NULL;
    qtc_view info = qtc_rhi_info(r);
    printf("  %s: %.*s\n", name, (int)info.len, info.data);
    CHECK(view_has(info, "\"backend\""));
    CHECK(view_has(info, "\"ubuf_alignment\""));
    qtc_rhi_native_handles nh;
    CHECK_EQ(qtc_rhi_native(r, &nh), QTC_OK);
    CHECK_EQ(nh.backend, backend);
    if (backend == QTC_RHI_BACKEND_VULKAN) CHECK(nh.instance && nh.device && nh.physical_device && nh.queue);
    if (backend == QTC_RHI_BACKEND_OPENGL) CHECK(nh.device != NULL);
    CHECK_EQ(qtc_rhi_native(r, NULL), QTC_ERR_INVALID_ARG);
    CHECK_EQ(qtc_rhi_supports(r, 999), false);

    /* resources */
    qtc_rhi tex = qtc_rhi_texture(r, QTC_RHI_RGBA8, W, H, QTC_RHI_RENDER_TARGET | QTC_RHI_READABLE);
    CHECK(tex != 0);
    qtc_rhi rt = qtc_rhi_render_target(tex, true);
    CHECK(rt != 0);
    qtc_rhi ubuf = qtc_rhi_buffer(r, QTC_RHI_DYNAMIC, QTC_RHI_UNIFORM_BUFFER, 256);
    CHECK(ubuf != 0);
    CHECK_EQ(qtc_rhi_upload(ubuf, 0, bytes_of(WHITE, sizeof WHITE)), QTC_OK);
    qtc_rhi vb_left = qtc_rhi_buffer(r, QTC_RHI_IMMUTABLE, QTC_RHI_VERTEX_BUFFER, sizeof LEFT_GREEN);
    CHECK_EQ(qtc_rhi_upload(vb_left, 0, bytes_of(LEFT_GREEN, sizeof LEFT_GREEN)), QTC_OK);
    qtc_rhi vb_right = qtc_rhi_buffer(r, QTC_RHI_STATIC, QTC_RHI_VERTEX_BUFFER, sizeof RIGHT_BLUE4);
    CHECK_EQ(qtc_rhi_upload(vb_right, 0, bytes_of(RIGHT_BLUE4, sizeof RIGHT_BLUE4)), QTC_OK);
    qtc_rhi ib = qtc_rhi_buffer(r, QTC_RHI_STATIC, QTC_RHI_INDEX_BUFFER, 64);
    CHECK_EQ(qtc_rhi_upload(ib, 0, bytes_of(QUAD_IDX, sizeof QUAD_IDX)), QTC_OK);
    qtc_rhi srb = qtc_rhi_bindings(r);
    CHECK_EQ(qtc_rhi_bind_uniform(srb, 0, QTC_RHI_VERTEX | QTC_RHI_FRAGMENT, ubuf, 0, 16), QTC_OK);
    CHECK_EQ(qtc_rhi_bindings_create(srb), QTC_OK);
    qtc_rhi vs = bake(QTC_RHI_VERTEX, VS_COLOR), fs = bake(QTC_RHI_FRAGMENT, FS_COLOR);
    CHECK(vs && fs);
    qtc_rhi pipe = color_pipeline(r, vs, fs, srb, rt);

    /* a frame: red clear, green left half, blue right half (indexed) */
    CHECK_EQ(qtc_rhi_draw(r, 3, 1, 0, 0), QTC_ERR_STATE);   /* no frame */
    CHECK_EQ(qtc_rhi_begin_frame(r, 0), QTC_OK);
    CHECK_EQ(qtc_rhi_begin_frame(r, 0), QTC_ERR_STATE);
    CHECK_EQ(qtc_rhi_set_pipeline(r, pipe), QTC_ERR_STATE); /* no pass */
    CHECK_EQ(qtc_rhi_begin_pass(r, rt, 0xFFFF0000u, 1.0, 0), QTC_OK);
    CHECK_EQ(qtc_rhi_begin_pass(r, rt, 0xFFFF0000u, 1.0, 0), QTC_ERR_STATE);
    CHECK_EQ(qtc_rhi_draw(r, 6, 1, 0, 0), QTC_ERR_STATE);   /* no pipeline */
    CHECK_EQ(qtc_rhi_set_pipeline(r, pipe), QTC_OK);
    CHECK_EQ(qtc_rhi_draw(r, 6, 1, 0, 0), QTC_ERR_STATE);   /* no bindings */
    CHECK_EQ(qtc_rhi_set_bindings(r, srb), QTC_OK);
    CHECK_EQ(qtc_rhi_draw(r, 6, 1, 0, 0), QTC_ERR_STATE);   /* no vertex buffer */
    CHECK_EQ(qtc_rhi_set_vertex_buffer(r, 0, vb_left, 0), QTC_OK);
    CHECK_EQ(qtc_rhi_draw(r, 7, 1, 0, 0), QTC_ERR_OUT_OF_RANGE);   /* one vertex too many */
    CHECK_EQ(qtc_rhi_draw(r, 6, 1, 1, 0), QTC_ERR_OUT_OF_RANGE);   /* from vertex 1 */
    CHECK_EQ(qtc_rhi_draw(r, -1, 1, 0, 0), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_draw(r, 6, 0, 0, 0), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_set_vertex_buffer(r, 0, vb_left, 4), QTC_OK);
    CHECK_EQ(qtc_rhi_draw(r, 6, 1, 0, 0), QTC_ERR_OUT_OF_RANGE);   /* the offset eats a vertex */
    CHECK_EQ(qtc_rhi_set_vertex_buffer(r, 0, vb_left, 0), QTC_OK);
    CHECK_EQ(qtc_rhi_draw(r, 6, 1, 0, 0), QTC_OK);
    CHECK_EQ(qtc_rhi_draw(r, 0, 1, 0, 0), QTC_OK);
    CHECK_EQ(qtc_rhi_set_vertex_buffer(r, 0, vb_right, 0), QTC_OK);
    CHECK_EQ(qtc_rhi_draw_indexed(r, 6, 1, 0, 0, 0), QTC_ERR_STATE);   /* no index buffer */
    CHECK_EQ(qtc_rhi_set_index_buffer(r, ib, 1, false), QTC_ERR_OUT_OF_RANGE);   /* misaligned */
    CHECK_EQ(qtc_rhi_set_index_buffer(r, ib, 0, false), QTC_OK);
    CHECK_EQ(qtc_rhi_draw_indexed(r, 6, 1, 0, 0, 0), QTC_OK);
    /* the zeros after the six indices are in range; past the buffer's end is not */
    CHECK_EQ(qtc_rhi_draw_indexed(r, 32, 1, 0, 0, 0), QTC_OK);
    CHECK_EQ(qtc_rhi_draw_indexed(r, 33, 1, 0, 0, 0), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_draw_indexed(r, 6, 1, 30, 0, 0), QTC_ERR_OUT_OF_RANGE);
    if (qtc_rhi_supports(r, QTC_RHI_FEATURE_BASE_VERTEX)) {
        CHECK_EQ(qtc_rhi_draw_indexed(r, 6, 1, 0, 1, 0), QTC_ERR_OUT_OF_RANGE);   /* vertex 3 + 1 */
    }
    /* an upload in the pass reaches the GPU after it: draws here still see the old indices */
    const uint16_t bad[] = {0, 1, 200};
    CHECK_EQ(qtc_rhi_upload(ib, 12, bytes_of(bad, sizeof bad)), QTC_OK);
    CHECK_EQ(qtc_rhi_draw_indexed(r, 9, 1, 0, 0, 0), QTC_OK);
    qtc_rhi rb = qtc_rhi_read_texture(tex);
    CHECK(rb != 0);
    CHECK_EQ(qtc_rhi_readback_ready(rb), false);
    CHECK_EQ(qtc_rhi_readback_bytes(rb).len, 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_STATE);
    CHECK_EQ(qtc_rhi_end_pass(r), QTC_OK);
    CHECK_EQ(qtc_rhi_end_pass(r), QTC_ERR_STATE);
    CHECK_EQ(qtc_rhi_destroy(r), QTC_ERR_STATE);   /* in a frame */
    CHECK_EQ(qtc_rhi_end_frame(r), QTC_OK);
    CHECK_EQ(qtc_rhi_end_frame(r), QTC_ERR_STATE);
    CHECK_EQ(qtc_rhi_readback_ready(rb), true);
    qtc_view px = qtc_rhi_readback_bytes(rb);
    CHECK_EQ(px.len, (int64_t)W * H * 4);
    if (real && px.len == (int64_t)W * H * 4) {
        CHECK(is_rgba(pixel(px, W, 5, H / 2), 0, 255, 0));
        CHECK(is_rgba(pixel(px, W, W - 5, H / 2), 0, 0, 255));
    }
    qtc_val img = qtc_rhi_readback_image(rb);
    CHECK(img != 0);
    if (img) {
        qtc_arg_int(5);
        qtc_arg_int(H / 2);
        CHECK_EQ(qtc_call(img, S("pixel")), QTC_OK);
        if (real) CHECK_EQ((uint32_t)qtc_ret_uint(), 0xFF00FF00u);
    }

    /* the next pass sees the upload: index 200 is past the four vertices */
    CHECK_EQ(qtc_rhi_begin_frame(r, 0), QTC_OK);
    CHECK_EQ(qtc_rhi_begin_pass(r, rt, 0xFF000000u, 1.0, 0), QTC_OK);
    CHECK_EQ(qtc_rhi_set_pipeline(r, pipe), QTC_OK);
    CHECK_EQ(qtc_rhi_set_bindings(r, srb), QTC_OK);
    CHECK_EQ(qtc_rhi_set_vertex_buffer(r, 0, vb_right, 0), QTC_OK);
    CHECK_EQ(qtc_rhi_set_index_buffer(r, ib, 0, false), QTC_OK);
    CHECK_EQ(qtc_rhi_draw_indexed(r, 6, 1, 0, 0, 0), QTC_OK);
    CHECK_EQ(qtc_rhi_draw_indexed(r, 9, 1, 0, 0, 0), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_set_viewport(r, 0, 0, 0, 10), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_set_viewport(r, 0, 0, 1e9, 10), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_set_viewport(r, 32768, 0, 1, 1), QTC_ERR_OUT_OF_RANGE);   /* past Vulkan's bounds */
    CHECK_EQ(qtc_rhi_set_viewport(r, 0, NAN, 1, 1), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_set_viewport(r, -W, -H, 3 * W, 3 * H), QTC_OK);
    CHECK_EQ(qtc_rhi_set_viewport(r, 0, 0, W / 2, H), QTC_OK);
    /* a buffer destroyed mid-pass: commands recorded keep it until the frame ends */
    CHECK_EQ(qtc_rhi_destroy(vb_right), QTC_OK);
    CHECK_EQ(qtc_rhi_draw_indexed(r, 6, 1, 0, 0, 0), QTC_ERR_STATE);
    CHECK_EQ(qtc_rhi_set_vertex_buffer(r, 0, vb_right, 0), QTC_ERR_STALE_HANDLE);
    /* the pass and the frame end whatever was left open */
    CHECK_EQ(qtc_rhi_end_frame(r), QTC_OK);

    /* textures: sampling an uploaded 2x1 texture (blue | yellow) */
    {
        qtc_rhi t2 = qtc_rhi_texture(r, QTC_RHI_RGBA8, 2, 1, 0);
        const uint8_t pix[] = {0, 0, 255, 255, 255, 255, 0, 255};
        CHECK_EQ(qtc_rhi_upload_pixels(t2, bytes_of(pix, 7)), QTC_ERR_BAD_ARGS);
        CHECK_EQ(qtc_rhi_upload_pixels(t2, bytes_of(pix, sizeof pix)), QTC_OK);
        qtc_rhi smp = qtc_rhi_sampler(r, false, false);
        qtc_rhi s2 = qtc_rhi_bindings(r);
        CHECK_EQ(qtc_rhi_bind_texture(s2, 1, QTC_RHI_FRAGMENT, t2, smp), QTC_OK);
        CHECK_EQ(qtc_rhi_bindings_create(s2), QTC_OK);
        qtc_rhi vuv = bake(QTC_RHI_VERTEX, VS_UV), ftex = bake(QTC_RHI_FRAGMENT, FS_TEX);
        qtc_rhi p2 = qtc_rhi_pipeline(r);
        CHECK_EQ(qtc_rhi_pipeline_shaders(p2, vuv, ftex), QTC_OK);
        CHECK_EQ(qtc_rhi_pipeline_vertex_binding(p2, 8, false), QTC_OK);
        CHECK_EQ(qtc_rhi_pipeline_vertex_attribute(p2, 0, 0, QTC_RHI_FLOAT2, 0), QTC_OK);
        CHECK_EQ(qtc_rhi_pipeline_create(p2, s2, rt), QTC_OK);
        CHECK_EQ(qtc_rhi_pipeline_options(p2, QTC_RHI_TRIANGLES, 0, false, false, false), QTC_ERR_STATE);   /* created */
        qtc_rhi vfull = qtc_rhi_buffer(r, QTC_RHI_IMMUTABLE, QTC_RHI_VERTEX_BUFFER, sizeof FULL_QUAD);
        CHECK_EQ(qtc_rhi_upload(vfull, 0, bytes_of(FULL_QUAD, sizeof FULL_QUAD)), QTC_OK);
        CHECK_EQ(qtc_rhi_destroy(t2), QTC_ERR_STATE);    /* the bindings use it */
        CHECK_EQ(qtc_rhi_destroy(s2), QTC_ERR_STATE);    /* the pipeline was made with it */
        CHECK_EQ(qtc_rhi_begin_frame(r, 0), QTC_OK);
        CHECK_EQ(qtc_rhi_begin_pass(r, rt, 0xFF000000u, 1.0, 0), QTC_OK);
        CHECK_EQ(qtc_rhi_set_pipeline(r, p2), QTC_OK);
        CHECK_EQ(qtc_rhi_set_bindings(r, srb), QTC_ERR_BAD_ARGS);   /* another layout */
        CHECK_EQ(qtc_rhi_set_bindings(r, s2), QTC_OK);
        CHECK_EQ(qtc_rhi_set_vertex_buffer(r, 0, vfull, 0), QTC_OK);
        CHECK_EQ(qtc_rhi_draw(r, 6, 1, 0, 0), QTC_OK);
        CHECK_EQ(qtc_rhi_end_pass(r), QTC_OK);
        qtc_rhi rb2 = qtc_rhi_read_texture(tex);
        CHECK_EQ(qtc_rhi_end_frame(r), QTC_OK);
        qtc_view p = qtc_rhi_readback_bytes(rb2);
        if (real && p.len == (int64_t)W * H * 4) {
            CHECK(is_rgba(pixel(p, W, 5, H / 2), 0, 0, 255));
            CHECK(is_rgba(pixel(p, W, W - 5, H / 2), 255, 255, 0));
        }
        /* the readback's image goes back into a texture */
        qtc_val back = qtc_rhi_readback_image(rb2);
        qtc_rhi t3 = qtc_rhi_texture(r, QTC_RHI_RGBA8, W, H, 0);
        CHECK_EQ(qtc_rhi_upload_image(t3, back), QTC_OK);
        CHECK_EQ(qtc_rhi_upload_image(t2, back), QTC_ERR_BAD_ARGS);   /* another size */
        CHECK_EQ(qtc_rhi_upload_image(t3, qtc_val_int(1)), QTC_ERR_WRONG_TYPE);
        qtc_val_free(back);
        /* destroyed in order: pipeline, bindings, then what they used */
        CHECK_EQ(qtc_rhi_destroy(p2), QTC_OK);
        CHECK_EQ(qtc_rhi_destroy(s2), QTC_OK);
        CHECK_EQ(qtc_rhi_destroy(t2), QTC_OK);
        CHECK_EQ(qtc_rhi_destroy(t2), QTC_ERR_STALE_HANDLE);
        CHECK_EQ(qtc_rhi_destroy(rb2), QTC_OK);
    }

    /* instancing: a strip on the left, drawn again 1.5 to the right */
    if (qtc_rhi_supports(r, QTC_RHI_FEATURE_INSTANCING)) {
        qtc_rhi vin = bake(QTC_RHI_VERTEX, VS_INST);
        qtc_rhi p3 = qtc_rhi_pipeline(r);
        CHECK_EQ(qtc_rhi_pipeline_shaders(p3, vin, fs), QTC_OK);
        CHECK_EQ(qtc_rhi_pipeline_vertex_binding(p3, 8, false), QTC_OK);
        CHECK_EQ(qtc_rhi_pipeline_vertex_binding(p3, 8, true), QTC_OK);
        CHECK_EQ(qtc_rhi_pipeline_vertex_attribute(p3, 0, 0, QTC_RHI_FLOAT2, 0), QTC_OK);
        CHECK_EQ(qtc_rhi_pipeline_vertex_attribute(p3, 1, 1, QTC_RHI_FLOAT2, 0), QTC_OK);
        CHECK_EQ(qtc_rhi_pipeline_create(p3, srb, rt), QTC_OK);
        qtc_rhi vstrip = qtc_rhi_buffer(r, QTC_RHI_IMMUTABLE, QTC_RHI_VERTEX_BUFFER, sizeof LEFT_STRIP);
        qtc_rhi voff = qtc_rhi_buffer(r, QTC_RHI_STATIC, QTC_RHI_VERTEX_BUFFER, sizeof OFFSETS);
        CHECK_EQ(qtc_rhi_upload(vstrip, 0, bytes_of(LEFT_STRIP, sizeof LEFT_STRIP)), QTC_OK);
        CHECK_EQ(qtc_rhi_upload(voff, 0, bytes_of(OFFSETS, sizeof OFFSETS)), QTC_OK);
        const float magenta[4] = {1, 0, 1, 1};
        CHECK_EQ(qtc_rhi_upload(ubuf, 0, bytes_of(magenta, sizeof magenta)), QTC_OK);
        CHECK_EQ(qtc_rhi_begin_frame(r, 0), QTC_OK);
        CHECK_EQ(qtc_rhi_begin_pass(r, rt, 0xFF000000u, 1.0, 0), QTC_OK);
        CHECK_EQ(qtc_rhi_set_pipeline(r, p3), QTC_OK);
        CHECK_EQ(qtc_rhi_set_bindings(r, srb), QTC_OK);
        CHECK_EQ(qtc_rhi_set_vertex_buffer(r, 0, vstrip, 0), QTC_OK);
        CHECK_EQ(qtc_rhi_draw(r, 6, 2, 0, 0), QTC_ERR_STATE);   /* binding 1 has no buffer */
        CHECK_EQ(qtc_rhi_set_vertex_buffer(r, 1, voff, 0), QTC_OK);
        CHECK_EQ(qtc_rhi_draw(r, 6, 3, 0, 0), QTC_ERR_OUT_OF_RANGE);   /* two instances' offsets */
        CHECK_EQ(qtc_rhi_draw(r, 6, 2, 0, 0), QTC_OK);
        CHECK_EQ(qtc_rhi_end_pass(r), QTC_OK);
        qtc_rhi rb3 = qtc_rhi_read_texture(tex);
        CHECK_EQ(qtc_rhi_end_frame(r), QTC_OK);
        qtc_view p = qtc_rhi_readback_bytes(rb3);
        if (real && p.len == (int64_t)W * H * 4) {
            CHECK(is_rgba(pixel(p, W, 3, H / 2), 255, 0, 255));
            CHECK(is_rgba(pixel(p, W, W / 2, H / 2), 0, 0, 0));
            CHECK(is_rgba(pixel(p, W, W - 3, H / 2), 255, 0, 255));
        }
        CHECK_EQ(qtc_rhi_upload(ubuf, 0, bytes_of(WHITE, sizeof WHITE)), QTC_OK);
    }

    /* compute: a storage buffer of squares, an image */
    if (qtc_rhi_supports(r, QTC_RHI_FEATURE_COMPUTE)) {
        qtc_rhi cs = bake(QTC_RHI_COMPUTE, CS_SQUARES);
        CHECK(cs != 0);
        qtc_rhi sb = qtc_rhi_buffer(r, QTC_RHI_STATIC, QTC_RHI_STORAGE_BUFFER, 64 * 4);
        qtc_rhi s_load = qtc_rhi_bindings(r);
        CHECK_EQ(qtc_rhi_bind_storage(s_load, 0, QTC_RHI_COMPUTE, sb, QTC_RHI_LOAD), QTC_OK);
        CHECK_EQ(qtc_rhi_bindings_create(s_load), QTC_OK);
        CHECK_EQ(qtc_rhi_compute_pipeline(r, cs, s_load), 0);   /* the shader writes it */
        CHECK_EQ(qtc_last_error(), QTC_ERR_BAD_ARGS);
        qtc_rhi s_ls = qtc_rhi_bindings(r);
        CHECK_EQ(qtc_rhi_bind_storage(s_ls, 0, QTC_RHI_COMPUTE, sb, QTC_RHI_LOAD_STORE), QTC_OK);
        CHECK_EQ(qtc_rhi_bindings_create(s_ls), QTC_OK);
        qtc_rhi cp = qtc_rhi_compute_pipeline(r, cs, s_ls);
        CHECK(cp != 0);
        CHECK_EQ(qtc_rhi_begin_frame(r, 0), QTC_OK);
        CHECK_EQ(qtc_rhi_dispatch(r, 1, 1, 1), QTC_ERR_STATE);
        CHECK_EQ(qtc_rhi_begin_compute(r, 0), QTC_OK);
        CHECK_EQ(qtc_rhi_set_pipeline(r, pipe), QTC_ERR_STATE);   /* a graphics pipeline */
        CHECK_EQ(qtc_rhi_set_pipeline(r, cp), QTC_OK);
        CHECK_EQ(qtc_rhi_dispatch(r, 1, 1, 1), QTC_ERR_STATE);    /* no bindings */
        CHECK_EQ(qtc_rhi_set_bindings(r, s_ls), QTC_OK);
        CHECK_EQ(qtc_rhi_dispatch(r, 0, 1, 1), QTC_ERR_OUT_OF_RANGE);
        CHECK_EQ(qtc_rhi_dispatch(r, 1, 1, 1), QTC_OK);
        CHECK_EQ(qtc_rhi_end_pass(r), QTC_OK);
        qtc_rhi rbb = 0;
        if (qtc_rhi_supports(r, QTC_RHI_FEATURE_READ_BACK_BUFFERS)) rbb = qtc_rhi_read_buffer(sb, 0, 64 * 4);
        CHECK_EQ(qtc_rhi_end_frame(r), QTC_OK);
        if (rbb) {
            qtc_view v = qtc_rhi_readback_bytes(rbb);
            CHECK_EQ(v.len, 256);
            if (real && v.len == 256) {
                uint32_t vals[64];
                memcpy(vals, v.data, sizeof vals);
                int ok = 1;
                for (uint32_t i = 0; i < 64; i++) ok &= vals[i] == i * i + 7;
                CHECK(ok);
            }
            CHECK(qtc_rhi_readback_image(rbb) == 0);
            CHECK_EQ(qtc_last_error(), QTC_ERR_WRONG_TYPE);
        }
        /* an image written by a compute shader */
        qtc_rhi ci = bake(QTC_RHI_COMPUTE, CS_IMAGE);
        qtc_rhi img_t = qtc_rhi_texture(r, QTC_RHI_RGBA8, 16, 16, QTC_RHI_STORAGE_IMAGE | QTC_RHI_READABLE);
        if (img_t) {
            qtc_rhi wrong = qtc_rhi_texture(r, QTC_RHI_R32F, 16, 16, QTC_RHI_STORAGE_IMAGE);
            qtc_rhi s_img = qtc_rhi_bindings(r);
            CHECK_EQ(qtc_rhi_bind_image(s_img, 0, QTC_RHI_COMPUTE, tex, QTC_RHI_STORE), QTC_ERR_BAD_ARGS);   /* no storage flag */
            CHECK_EQ(qtc_rhi_bind_image(s_img, 0, QTC_RHI_COMPUTE, img_t, QTC_RHI_STORE), QTC_OK);
            CHECK_EQ(qtc_rhi_bindings_create(s_img), QTC_OK);
            if (wrong) {
                qtc_rhi s_wrong = qtc_rhi_bindings(r);
                CHECK_EQ(qtc_rhi_bind_image(s_wrong, 0, QTC_RHI_COMPUTE, wrong, QTC_RHI_STORE), QTC_OK);
                CHECK_EQ(qtc_rhi_bindings_create(s_wrong), QTC_OK);
                CHECK_EQ(qtc_rhi_compute_pipeline(r, ci, s_wrong), 0);   /* r32f is not rgba8 */
                CHECK_EQ(qtc_last_error(), QTC_ERR_BAD_ARGS);
            }
            qtc_rhi cpi = qtc_rhi_compute_pipeline(r, ci, s_img);
            CHECK(cpi != 0);
            CHECK_EQ(qtc_rhi_begin_frame(r, 0), QTC_OK);
            CHECK_EQ(qtc_rhi_begin_compute(r, 0), QTC_OK);
            CHECK_EQ(qtc_rhi_set_pipeline(r, cpi), QTC_OK);
            CHECK_EQ(qtc_rhi_set_bindings(r, s_img), QTC_OK);
            CHECK_EQ(qtc_rhi_dispatch(r, 2, 2, 1), QTC_OK);
            CHECK_EQ(qtc_rhi_end_pass(r), QTC_OK);
            qtc_rhi rbi = qtc_rhi_read_texture(img_t);
            CHECK_EQ(qtc_rhi_end_frame(r), QTC_OK);
            qtc_view v = qtc_rhi_readback_bytes(rbi);
            CHECK_EQ(v.len, 16 * 16 * 4);
            if (real && v.len == 16 * 16 * 4) {
                CHECK(is_rgba(pixel(v, 16, 0, 0), 0, 0, 255));
                CHECK(is_rgba(pixel(v, 16, 15, 15), 0, 0, 255));
            }
        }
    }

    /* native commands inside a pass */
    {
        void *ncb = (void *)1;
        CHECK_EQ(qtc_rhi_begin_frame(r, 0), QTC_OK);
        CHECK_EQ(qtc_rhi_begin_pass(r, rt, 0xFF000000u, 1.0, 0), QTC_OK);
        CHECK_EQ(qtc_rhi_begin_external(r, &ncb), QTC_ERR_STATE);   /* not an external pass */
        CHECK(ncb == NULL);
        CHECK_EQ(qtc_rhi_end_pass(r), QTC_OK);
        CHECK_EQ(qtc_rhi_begin_pass(r, rt, 0xFF000000u, 1.0, QTC_RHI_PASS_EXTERNAL), QTC_OK);
        CHECK_EQ(qtc_rhi_set_pipeline(r, pipe), QTC_OK);
        CHECK_EQ(qtc_rhi_begin_external(r, &ncb), QTC_OK);
        if (backend == QTC_RHI_BACKEND_VULKAN) CHECK(ncb != NULL);
        CHECK_EQ(qtc_rhi_draw(r, 3, 1, 0, 0), QTC_ERR_STATE);
        CHECK_EQ(qtc_rhi_upload(ubuf, 0, bytes_of(WHITE, 16)), QTC_ERR_STATE);
        CHECK_EQ(qtc_rhi_end_external(r), QTC_OK);
        CHECK_EQ(qtc_rhi_end_external(r), QTC_ERR_STATE);
        CHECK_EQ(qtc_rhi_set_bindings(r, srb), QTC_ERR_STATE);   /* the pipeline is set again first */
        CHECK_EQ(qtc_rhi_set_pipeline(r, pipe), QTC_OK);
        CHECK_EQ(qtc_rhi_set_bindings(r, srb), QTC_OK);
        CHECK_EQ(qtc_rhi_begin_external(r, &ncb), QTC_OK);
        CHECK_EQ(qtc_rhi_end_frame(r), QTC_OK);   /* ends the native section and the pass too */
    }

    /* native objects; an OpenGL context made current outside frames */
    {
        int32_t layout = -1;
        uint64_t o = qtc_rhi_native_object(tex, &layout);
        if (real) CHECK(o != 0);
        CHECK(qtc_rhi_native_object(vb_left, NULL) != 0 || !real);   /* (OpenGL keeps uniform buffers in CPU memory) */
        CHECK_EQ(qtc_rhi_native_object(srb, NULL), 0u);
        CHECK_EQ(qtc_last_error(), QTC_ERR_WRONG_TYPE);
        if (backend == QTC_RHI_BACKEND_OPENGL) {
            CHECK_EQ(qtc_rhi_make_current(r), QTC_OK);
            typedef const unsigned char *(*get_string_fn)(unsigned int);
            get_string_fn gs = (get_string_fn)qtc_gl_proc_address(S("glGetString"));
            CHECK(gs != NULL);
            if (gs) {
                const unsigned char *ver = gs(0x1F02u);   /* GL_VERSION */
                CHECK(ver != NULL && ver[0]);
            }
        }
        CHECK_EQ(qtc_rhi_finish(r), QTC_OK);
    }

    /* readbacks outlive their handles; the RHI keeps the results until the GPU is done */
    {
        CHECK_EQ(qtc_rhi_begin_frame(r, 0), QTC_OK);
        qtc_rhi gone = qtc_rhi_read_texture(tex);
        CHECK_EQ(qtc_rhi_destroy(gone), QTC_OK);
        CHECK_EQ(qtc_rhi_end_frame(r), QTC_OK);
        /* a readback queued with no frame after it stays pending until the RHI goes */
        qtc_rhi never = qtc_rhi_read_texture(tex);
        CHECK(never != 0);
        CHECK_EQ(qtc_rhi_readback_ready(never), false);
    }

    /* everything goes with the RHI */
    CHECK_EQ(qtc_rhi_destroy(r), QTC_OK);
    CHECK_EQ(qtc_rhi_destroy(tex), QTC_ERR_STALE_HANDLE);
    CHECK_EQ(qtc_rhi_begin_frame(r, 0), QTC_ERR_STALE_HANDLE);
    /* shaders are not the RHI's */
    CHECK(qtc_rhi_shader_reflect(vs).len > 0);
    CHECK_EQ(qtc_rhi_destroy(vs), QTC_OK);
    CHECK_EQ(qtc_rhi_destroy(fs), QTC_OK);
    (void)live0;
}

/* ------------------------------------------------------------------ validation (Null backend) */

static void test_validation(void) {
    const int64_t live0 = qtc_rhi_live();
    CHECK_EQ(qtc_rhi_create(-1, 0), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_create(QTC_RHI_BACKEND_NULL, 0x80), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_INVALID_ARG);
#ifndef _WIN32
    CHECK_EQ(qtc_rhi_create(QTC_RHI_BACKEND_D3D11, 0), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_UNSUPPORTED);
#endif
    qtc_rhi r = qtc_rhi_create(QTC_RHI_BACKEND_NULL, QTC_RHI_DEBUG);
    qtc_rhi r2 = qtc_rhi_create(QTC_RHI_BACKEND_NULL, 0);
    CHECK(r && r2);
    CHECK_EQ(qtc_rhi_live(), live0 + 2);

    /* handles */
    CHECK_EQ(qtc_rhi_destroy(0), QTC_ERR_NULL_HANDLE);
    CHECK_EQ(qtc_rhi_destroy(0x7fffffffffffull), QTC_ERR_BAD_HANDLE);
    CHECK_EQ(qtc_rhi_info(12345).len, 0);
    qtc_obj notrhi = qtc_new(S("QObject"));
    CHECK_EQ(qtc_rhi_destroy(notrhi), QTC_ERR_BAD_HANDLE);
    qtc_destroy(notrhi);

    /* buffers */
    CHECK_EQ(qtc_rhi_buffer(r, QTC_RHI_STATIC, QTC_RHI_UNIFORM_BUFFER, 64), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_INVALID_ARG);
    CHECK_EQ(qtc_rhi_buffer(r, QTC_RHI_DYNAMIC, QTC_RHI_UNIFORM_BUFFER | QTC_RHI_VERTEX_BUFFER, 64), 0);
    CHECK_EQ(qtc_rhi_buffer(r, QTC_RHI_DYNAMIC, QTC_RHI_INDEX_BUFFER, 64), 0);
    CHECK_EQ(qtc_rhi_buffer(r, QTC_RHI_STATIC, QTC_RHI_INDEX_BUFFER | QTC_RHI_STORAGE_BUFFER, 64), 0);
    CHECK_EQ(qtc_rhi_buffer(r, QTC_RHI_DYNAMIC, QTC_RHI_STORAGE_BUFFER, 64), 0);
    CHECK_EQ(qtc_rhi_buffer(r, QTC_RHI_STATIC, 0, 64), 0);
    CHECK_EQ(qtc_rhi_buffer(r, QTC_RHI_STATIC, 0x100, 64), 0);
    CHECK_EQ(qtc_rhi_buffer(r, 7, QTC_RHI_VERTEX_BUFFER, 64), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_buffer(r, QTC_RHI_STATIC, QTC_RHI_VERTEX_BUFFER, 0), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_buffer(r, QTC_RHI_STATIC, QTC_RHI_VERTEX_BUFFER, (int64_t)1 << 31), 0);
    CHECK_EQ(qtc_rhi_buffer(r, QTC_RHI_DYNAMIC, QTC_RHI_UNIFORM_BUFFER, (int64_t)1 << 28), 0);
    qtc_rhi vb = qtc_rhi_buffer(r, QTC_RHI_STATIC, QTC_RHI_VERTEX_BUFFER, 64);
    CHECK_EQ(qtc_rhi_upload(vb, 60, bytes_of("12345", 5)), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_upload(vb, -1, bytes_of("1", 1)), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_upload(vb, 64, bytes_of("", 0)), QTC_OK);
    CHECK_EQ(qtc_rhi_read_buffer(vb, 0, 65), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_OUT_OF_RANGE);

    /* textures, samplers, targets */
    CHECK_EQ(qtc_rhi_texture(r, 99, 4, 4, 0), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_texture(r, QTC_RHI_RGBA8, 0, 4, 0), 0);
    CHECK_EQ(qtc_rhi_texture(r, QTC_RHI_RGBA8, 4, 1 << 20, 0), 0);
    CHECK_EQ(qtc_rhi_texture(r, QTC_RHI_RGBA8, 4, 4, 0x40), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_INVALID_ARG);
    CHECK_EQ(qtc_rhi_texture(r, QTC_RHI_BGRA8, 4, 4, QTC_RHI_STORAGE_IMAGE), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_UNSUPPORTED);
    qtc_rhi plain = qtc_rhi_texture(r, QTC_RHI_RGBA8, 4, 4, 0);
    CHECK_EQ(qtc_rhi_render_target(plain, false), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_STATE);
    CHECK_EQ(qtc_rhi_read_texture(plain), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_STATE);
    CHECK_EQ(qtc_rhi_render_target(vb, false), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_WRONG_TYPE);
    qtc_rhi ct = qtc_rhi_texture(r, QTC_RHI_RGBA8, 8, 8, QTC_RHI_RENDER_TARGET);
    qtc_rhi rt = qtc_rhi_render_target(ct, false);
    CHECK(rt != 0);
    CHECK_EQ(qtc_rhi_destroy(ct), QTC_ERR_STATE);   /* the target draws into it */
    qtc_rhi other_ct = qtc_rhi_texture(r2, QTC_RHI_RGBA8, 8, 8, QTC_RHI_RENDER_TARGET);
    qtc_rhi other_rt = qtc_rhi_render_target(other_ct, false);

    /* shaders */
    CHECK_EQ(qtc_rhi_shader_bake(8, S("void main(){}")), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_shader_bake(QTC_RHI_VERTEX, NOSTR), 0);
    CHECK_EQ(qtc_rhi_shader_bake(QTC_RHI_VERTEX, S("#version 440\nvoid main() { this is not glsl; }\n")), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_INVALID_ARG);
    CHECK(view_has(qtc_last_error_message(), "qtc_rhi_shader_bake"));
    qtc_rhi vs = bake(QTC_RHI_VERTEX, VS_COLOR), fs = bake(QTC_RHI_FRAGMENT, FS_COLOR);
    CHECK(vs && fs);
    qtc_view refl = qtc_rhi_shader_reflect(vs);
    CHECK(view_has(refl, "\"inputs\"") && view_has(refl, "tint"));
    /* .qsb round trip */
    qtc_view q = qtc_rhi_shader_save(vs);
    CHECK(q.len > 16);
    char *qcopy = (char *)malloc((size_t)q.len);
    memcpy(qcopy, q.data, (size_t)q.len);
    const int64_t qlen = q.len;
    qtc_rhi vs2 = qtc_rhi_shader_load(qtc_sn(qcopy, qlen));
    CHECK(vs2 != 0);
    /* truncated and corrupted packages are refused, not crashed on */
    for (int64_t cut = 0; cut < qlen; cut += qlen / 23 + 1) {
        qtc_rhi s = qtc_rhi_shader_load(qtc_sn(qcopy, cut));
        if (s) qtc_rhi_destroy(s);
    }
    for (int i = 0; i < 200; i++) {
        char *bad = (char *)malloc((size_t)qlen);
        memcpy(bad, qcopy, (size_t)qlen);
        uint32_t seed = (uint32_t)i * 2654435761u;
        for (int k = 0; k < 4; k++) {
            seed = seed * 1103515245u + 12345u;
            bad[(seed >> 8) % (uint32_t)qlen] = (char)(seed >> 3);
        }
        qtc_rhi s = qtc_rhi_shader_load(qtc_sn(bad, qlen));
        if (s) qtc_rhi_destroy(s);
        free(bad);
    }
    free(qcopy);
    CHECK_EQ(qtc_rhi_shader_load(S("garbage")), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_INVALID_ARG);
    qtc_clear_error();

    /* bindings */
    qtc_rhi ub = qtc_rhi_buffer(r, QTC_RHI_DYNAMIC, QTC_RHI_UNIFORM_BUFFER, 512);
    qtc_rhi ub_other = qtc_rhi_buffer(r2, QTC_RHI_DYNAMIC, QTC_RHI_UNIFORM_BUFFER, 64);
    qtc_rhi s = qtc_rhi_bindings(r);
    CHECK_EQ(qtc_rhi_bind_uniform(s, 0, QTC_RHI_VERTEX, vb, 0, 16), QTC_ERR_BAD_ARGS);   /* not uniform */
    CHECK_EQ(qtc_rhi_bind_uniform(s, 0, QTC_RHI_VERTEX, ub_other, 0, 16), QTC_ERR_BAD_ARGS);   /* other RHI */
    CHECK_EQ(qtc_rhi_bind_uniform(s, 0, QTC_RHI_VERTEX, ub, 3, 16), QTC_ERR_OUT_OF_RANGE);  /* misaligned */
    CHECK_EQ(qtc_rhi_bind_uniform(s, 0, QTC_RHI_VERTEX, ub, 0, 513), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_bind_uniform(s, 0, QTC_RHI_VERTEX, ub, INT64_MIN, 0), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_bind_uniform(s, 0, QTC_RHI_VERTEX, ub, INT64_MAX, 0), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_bind_uniform(s, 0, QTC_RHI_VERTEX, ub, 0, INT64_MIN), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_bind_uniform(s, 64, QTC_RHI_VERTEX, ub, 0, 16), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_bind_uniform(s, 0, 0, ub, 0, 16), QTC_ERR_INVALID_ARG);
    CHECK_EQ(qtc_rhi_bind_uniform(s, 0, QTC_RHI_VERTEX, ub, 0, 8), QTC_OK);   /* smaller than the block */
    CHECK_EQ(qtc_rhi_bind_uniform(s, 0, QTC_RHI_VERTEX, ub, 0, 16), QTC_ERR_STATE);   /* binding 0 again */
    CHECK_EQ(qtc_rhi_bind_storage(s, 1, QTC_RHI_COMPUTE, ub, QTC_RHI_LOAD), QTC_ERR_BAD_ARGS);
    CHECK_EQ(qtc_rhi_bindings_create(s), QTC_OK);
    CHECK_EQ(qtc_rhi_bindings_create(s), QTC_ERR_STATE);
    CHECK_EQ(qtc_rhi_bind_uniform(s, 1, QTC_RHI_VERTEX, ub, 0, 16), QTC_ERR_STATE);   /* created */
    CHECK_EQ(qtc_rhi_destroy(ub), QTC_ERR_STATE);   /* the bindings use it */
    qtc_rhi s_ok = qtc_rhi_bindings(r);
    CHECK_EQ(qtc_rhi_bind_uniform(s_ok, 0, QTC_RHI_VERTEX, ub, 256, 0), QTC_OK);   /* 256 to the end */
    CHECK_EQ(qtc_rhi_bindings_create(s_ok), QTC_OK);
    qtc_rhi s_frag = qtc_rhi_bindings(r);
    CHECK_EQ(qtc_rhi_bind_uniform(s_frag, 0, QTC_RHI_FRAGMENT, ub, 0, 16), QTC_OK);
    CHECK_EQ(qtc_rhi_bindings_create(s_frag), QTC_OK);
    qtc_rhi s_open = qtc_rhi_bindings(r);   /* never created */

    /* pipelines */
    qtc_rhi p = qtc_rhi_pipeline(r);
    CHECK_EQ(qtc_rhi_pipeline_create(p, s_ok, rt), QTC_ERR_STATE);   /* no shaders */
    CHECK_EQ(qtc_rhi_pipeline_shaders(p, fs, vs), QTC_ERR_BAD_ARGS);   /* swapped */
    CHECK_EQ(qtc_rhi_pipeline_shaders(p, vs, fs), QTC_OK);
    CHECK_EQ(qtc_rhi_pipeline_vertex_attribute(p, 0, 0, QTC_RHI_FLOAT2, 0), QTC_ERR_OUT_OF_RANGE);   /* no binding */
    CHECK_EQ(qtc_rhi_pipeline_vertex_binding(p, 6, false), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_pipeline_vertex_binding(p, 20, false), QTC_OK);
    CHECK_EQ(qtc_rhi_pipeline_vertex_attribute(p, 0, 0, QTC_RHI_FLOAT2, 0), QTC_OK);
    CHECK_EQ(qtc_rhi_pipeline_vertex_attribute(p, 0, 0, QTC_RHI_FLOAT3, 8), QTC_ERR_STATE);   /* location 0 again */
    CHECK_EQ(qtc_rhi_pipeline_vertex_attribute(p, 0, 1, QTC_RHI_FLOAT4, 8), QTC_ERR_OUT_OF_RANGE);   /* past the stride */
    CHECK_EQ(qtc_rhi_pipeline_vertex_attribute(p, 0, 1, 99, 8), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_pipeline_create(p, s_ok, rt), QTC_ERR_BAD_ARGS);   /* input 1 has no attribute */
    CHECK_EQ(qtc_rhi_pipeline_vertex_attribute(p, 0, 1, QTC_RHI_INT, 8), QTC_OK);
    CHECK_EQ(qtc_rhi_pipeline_create(p, s_ok, rt), QTC_ERR_BAD_ARGS);   /* int for a vec3 */
    CHECK_EQ(qtc_rhi_destroy(p), QTC_OK);
    p = qtc_rhi_pipeline(r);
    CHECK_EQ(qtc_rhi_pipeline_shaders(p, vs, fs), QTC_OK);
    CHECK_EQ(qtc_rhi_pipeline_vertex_binding(p, 20, false), QTC_OK);
    CHECK_EQ(qtc_rhi_pipeline_vertex_attribute(p, 0, 0, QTC_RHI_FLOAT2, 0), QTC_OK);
    CHECK_EQ(qtc_rhi_pipeline_vertex_attribute(p, 0, 1, QTC_RHI_FLOAT3, 8), QTC_OK);
    CHECK_EQ(qtc_rhi_pipeline_options(p, 9, 0, false, false, false), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_pipeline_options(p, QTC_RHI_TRIANGLES, 5, false, false, false), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_pipeline_create(p, s_open, rt), QTC_ERR_STATE);    /* bindings not created */
    CHECK_EQ(qtc_rhi_pipeline_create(p, s, rt), QTC_ERR_BAD_ARGS);       /* 8 bytes for a 16-byte block */
    CHECK_EQ(qtc_rhi_pipeline_create(p, s_frag, rt), QTC_ERR_BAD_ARGS);  /* invisible to the vertex stage */
    CHECK_EQ(qtc_rhi_pipeline_create(p, s_ok, other_rt), QTC_ERR_BAD_ARGS);   /* another RHI's target */
    CHECK_EQ(qtc_rhi_pipeline_create(p, s_ok, vb), QTC_ERR_WRONG_TYPE);
    CHECK_EQ(qtc_rhi_pipeline_options(p, QTC_RHI_TRIANGLE_STRIP, QTC_RHI_CULL_BACK, true, true, true), QTC_OK);
    CHECK_EQ(qtc_rhi_pipeline_create(p, s_ok, rt), QTC_OK);
    CHECK_EQ(qtc_rhi_pipeline_create(p, s_ok, rt), QTC_ERR_STATE);
    CHECK_EQ(qtc_rhi_destroy(s_ok), QTC_ERR_STATE);   /* the pipeline was made with it */
    /* points need gl_PointSize; inputs need outputs; uniform blocks need their size */
    {
        qtc_rhi vnp = bake(QTC_RHI_VERTEX, VS_POINTS_NO_SIZE);
        qtc_rhi pp = qtc_rhi_pipeline(r);
        qtc_rhi_pipeline_shaders(pp, vnp, fs);
        qtc_rhi_pipeline_vertex_binding(pp, 20, false);
        qtc_rhi_pipeline_vertex_attribute(pp, 0, 0, QTC_RHI_FLOAT2, 0);
        qtc_rhi_pipeline_vertex_attribute(pp, 0, 1, QTC_RHI_FLOAT3, 8);
        qtc_rhi_pipeline_options(pp, QTC_RHI_POINTS, 0, false, false, false);
        qtc_rhi s_none = qtc_rhi_bindings(r);
        CHECK_EQ(qtc_rhi_bindings_create(s_none), QTC_OK);
        CHECK_EQ(qtc_rhi_pipeline_create(pp, s_none, rt), QTC_ERR_BAD_ARGS);
        CHECK_EQ(qtc_rhi_pipeline_options(pp, QTC_RHI_LINES, 0, false, false, false), QTC_OK);
        CHECK_EQ(qtc_rhi_pipeline_create(pp, s_none, rt), QTC_OK);
        qtc_rhi fo = bake(QTC_RHI_FRAGMENT, FS_OTHER_INPUT);
        qtc_rhi p2 = qtc_rhi_pipeline(r);
        qtc_rhi_pipeline_shaders(p2, vnp, fo);
        qtc_rhi_pipeline_vertex_binding(p2, 20, false);
        qtc_rhi_pipeline_vertex_attribute(p2, 0, 0, QTC_RHI_FLOAT2, 0);
        qtc_rhi_pipeline_vertex_attribute(p2, 0, 1, QTC_RHI_FLOAT3, 8);
        CHECK_EQ(qtc_rhi_pipeline_create(p2, s_none, rt), QTC_ERR_BAD_ARGS);
        qtc_rhi fb = bake(QTC_RHI_FRAGMENT, FS_BIG_UBUF);
        qtc_rhi p3 = qtc_rhi_pipeline(r);
        qtc_rhi_pipeline_shaders(p3, vs, fb);
        qtc_rhi_pipeline_vertex_binding(p3, 20, false);
        qtc_rhi_pipeline_vertex_attribute(p3, 0, 0, QTC_RHI_FLOAT2, 0);
        qtc_rhi_pipeline_vertex_attribute(p3, 0, 1, QTC_RHI_FLOAT3, 8);
        qtc_rhi s_both = qtc_rhi_bindings(r);
        qtc_rhi_bind_uniform(s_both, 0, QTC_RHI_VERTEX | QTC_RHI_FRAGMENT, ub, 0, 16);
        qtc_rhi_bindings_create(s_both);
        CHECK_EQ(qtc_rhi_pipeline_create(p3, s_both, rt), QTC_ERR_BAD_ARGS);   /* the block is 144 bytes */
        qtc_rhi vi = bake(QTC_RHI_VERTEX, VS_INT);
        qtc_rhi p4 = qtc_rhi_pipeline(r);
        qtc_rhi_pipeline_shaders(p4, vi, fs);
        qtc_rhi_pipeline_vertex_binding(p4, 12, false);
        qtc_rhi_pipeline_vertex_attribute(p4, 0, 0, QTC_RHI_FLOAT2, 0);
        qtc_rhi_pipeline_vertex_attribute(p4, 0, 1, QTC_RHI_UINT, 8);
        CHECK_EQ(qtc_rhi_pipeline_create(p4, s_none, rt), QTC_ERR_BAD_ARGS);   /* uint for an int */
        CHECK(vi != 0);
    }

    /* frames on another RHI's objects; a destroyed RHI */
    CHECK_EQ(qtc_rhi_begin_frame(r, 0), QTC_OK);
    CHECK_EQ(qtc_rhi_begin_pass(r, other_rt, 0, 1.0, 0), QTC_ERR_BAD_ARGS);
    CHECK_EQ(qtc_rhi_begin_pass(r, rt, 0, 2.0, 0), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_begin_pass(r, rt, 0, 1.0, 0x8), QTC_ERR_INVALID_ARG);
    CHECK_EQ(qtc_rhi_begin_pass(r, rt, 0, 1.0, 0), QTC_OK);
    CHECK_EQ(qtc_rhi_set_pipeline(r, p), QTC_OK);
    CHECK_EQ(qtc_rhi_set_bindings(r, s_open), QTC_ERR_BAD_ARGS);
    CHECK_EQ(qtc_rhi_set_bindings(r, s), QTC_ERR_BAD_ARGS);   /* too small for the shader */
    CHECK_EQ(qtc_rhi_set_bindings(r, s_ok), QTC_OK);
    CHECK_EQ(qtc_rhi_set_vertex_buffer(r, 0, ub, 0), QTC_ERR_BAD_ARGS);   /* not a vertex buffer */
    CHECK_EQ(qtc_rhi_set_vertex_buffer(r, 16, vb, 0), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_set_vertex_buffer(r, 0, vb, 2), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_rhi_set_vertex_buffer(r, 0, vb, 0), QTC_OK);
    CHECK_EQ(qtc_rhi_draw(r, 3, 1, 0, 0), QTC_OK);                 /* 3 x 20 bytes fit in 64 */
    CHECK_EQ(qtc_rhi_draw(r, 4, 1, 0, 0), QTC_ERR_OUT_OF_RANGE);   /* the fourth reaches 80 */
    CHECK_EQ(qtc_rhi_draw(r, 1, 1, 1 << 30, 0), QTC_ERR_OUT_OF_RANGE);
    {
        qtc_rhi ib = qtc_rhi_buffer(r, QTC_RHI_STATIC, QTC_RHI_INDEX_BUFFER, 12);
        CHECK_EQ(qtc_rhi_set_index_buffer(r, ib, 0, false), QTC_OK);
        CHECK_EQ(qtc_rhi_draw_indexed(r, 3, 1, 0, 0, 0), QTC_ERR_STATE);   /* its data comes with the next pass */
        CHECK_EQ(qtc_rhi_end_pass(r), QTC_OK);
        CHECK_EQ(qtc_rhi_begin_pass(r, rt, 0, 1.0, 0), QTC_OK);
        CHECK_EQ(qtc_rhi_set_pipeline(r, p), QTC_OK);
        CHECK_EQ(qtc_rhi_set_bindings(r, s_ok), QTC_OK);
        CHECK_EQ(qtc_rhi_set_vertex_buffer(r, 0, vb, 0), QTC_OK);
        CHECK_EQ(qtc_rhi_set_index_buffer(r, ib, 0, false), QTC_OK);
        CHECK_EQ(qtc_rhi_draw_indexed(r, 6, 1, 0, 0, 0), QTC_OK);   /* six zeros */
        CHECK_EQ(qtc_rhi_draw_indexed(r, 7, 1, 0, 0, 0), QTC_ERR_OUT_OF_RANGE);
        CHECK_EQ(qtc_rhi_set_index_buffer(r, ib, 0, true), QTC_OK);
        CHECK_EQ(qtc_rhi_draw_indexed(r, 3, 1, 0, 0, 0), QTC_OK);
        CHECK_EQ(qtc_rhi_draw_indexed(r, 4, 1, 0, 0, 0), QTC_ERR_OUT_OF_RANGE);
    }
    CHECK_EQ(qtc_rhi_set_pipeline(r, s), QTC_ERR_WRONG_TYPE);
    CHECK_EQ(qtc_rhi_swapchain(r, 0, 0), 0);
    CHECK_EQ(qtc_rhi_end_frame(r), QTC_OK);

    /* the dependency order of destruction */
    CHECK_EQ(qtc_rhi_destroy(p), QTC_OK);
    CHECK_EQ(qtc_rhi_destroy(s_ok), QTC_OK);
    CHECK_EQ(qtc_rhi_destroy(s), QTC_OK);
    CHECK_EQ(qtc_rhi_destroy(s_frag), QTC_OK);
    CHECK_EQ(qtc_rhi_destroy(rt), QTC_OK);
    CHECK_EQ(qtc_rhi_destroy(ct), QTC_OK);

    /* many objects come and go: the table reuses its slots and handles never come back */
    {
        qtc_rhi first = qtc_rhi_buffer(r, QTC_RHI_STATIC, QTC_RHI_VERTEX_BUFFER, 16);
        CHECK_EQ(qtc_rhi_destroy(first), QTC_OK);
        int ok = 1;
        for (int i = 0; i < 5000; i++) {
            qtc_rhi b = qtc_rhi_buffer(r, QTC_RHI_STATIC, QTC_RHI_VERTEX_BUFFER, 16);
            ok &= b != 0 && b != first;
            ok &= qtc_rhi_destroy(b) == QTC_OK;
        }
        CHECK(ok);
        CHECK_EQ(qtc_rhi_destroy(first), QTC_ERR_STALE_HANDLE);
    }

    /* widgets' functions on other objects */
    qtc_obj o = qtc_new(S("QObject"));
    CHECK_EQ(qtc_rhi_widget(o), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_WRONG_TYPE);
    CHECK_EQ(qtc_rhi_widget_target(o), 0);
    CHECK_EQ(qtc_rhi_swapchain(r, o, 0), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_WRONG_TYPE);
    qtc_obj widget = qtc_new(S("QWidget"));
    qtc_call(widget, S("winId"));   /* a QWidgetWindow behind it */
    qtc_call(widget, S("windowHandle"));
    qtc_obj wh = qtc_ret_obj();
    if (wh) {
        CHECK_EQ(qtc_rhi_swapchain(r, wh, 0), 0);   /* it draws itself */
        CHECK_EQ(qtc_last_error(), QTC_ERR_WRONG_TYPE);
    }
    qtc_destroy(widget);
    qtc_destroy(o);

    /* RHIs go with what they own, whatever state it was left in */
    CHECK_EQ(qtc_rhi_destroy(r), QTC_OK);
    CHECK_EQ(qtc_rhi_destroy(vb), QTC_ERR_STALE_HANDLE);
    CHECK_EQ(qtc_rhi_destroy(r2), QTC_OK);
    CHECK_EQ(qtc_rhi_destroy(vs2), QTC_OK);
    /* shaders remain until destroyed (or qtc_shutdown) */
    CHECK(qtc_rhi_live() >= live0);
}

/* ------------------------------------------------------------------ windows: swapchains */

static void test_swapchain(int32_t backend, const char *name) {
    qtc_rhi r = qtc_rhi_create(backend, 0);
    if (!r) return;
    const int real = backend != QTC_RHI_BACKEND_NULL;
    qtc_obj win = qtc_new(S("QWindow"));
    qtc_arg_int(80);
    qtc_arg_int(60);
    qtc_call(win, S("resize"));
    qtc_rhi sc = qtc_rhi_swapchain(r, win, QTC_RHI_SC_DEPTH | QTC_RHI_SC_READABLE);
    CHECK(sc != 0);
    CHECK_EQ(qtc_rhi_swapchain(r, win, 0), 0);   /* one per window */
    CHECK_EQ(qtc_last_error(), QTC_ERR_STATE);
    CHECK_EQ(qtc_rhi_swapchain(r, win, 0x100), 0);
    /* a pipeline for the swapchain before its first frame */
    qtc_rhi ub = qtc_rhi_buffer(r, QTC_RHI_DYNAMIC, QTC_RHI_UNIFORM_BUFFER, 64);
    qtc_rhi_upload(ub, 0, bytes_of(WHITE, sizeof WHITE));
    qtc_rhi s = qtc_rhi_bindings(r);
    qtc_rhi_bind_uniform(s, 0, QTC_RHI_VERTEX, ub, 0, 16);
    qtc_rhi_bindings_create(s);
    qtc_rhi vs = bake(QTC_RHI_VERTEX, VS_COLOR), fs = bake(QTC_RHI_FRAGMENT, FS_COLOR);
    qtc_rhi p = color_pipeline(r, vs, fs, s, sc);
    qtc_rhi vb = qtc_rhi_buffer(r, QTC_RHI_IMMUTABLE, QTC_RHI_VERTEX_BUFFER, sizeof LEFT_GREEN);
    qtc_rhi_upload(vb, 0, bytes_of(LEFT_GREEN, sizeof LEFT_GREEN));

    /* hidden: nothing to draw into */
    CHECK_EQ(qtc_rhi_begin_frame(r, sc), QTC_RHI_SKIPPED);
    qtc_call(win, S("show"));
    int frames = 0, skipped = 0;
    qtc_rhi rb = 0;
    for (int i = 0; i < 200 && frames < 3; i++) {
        qtc_process_events();
        qtc_status st = qtc_rhi_begin_frame(r, sc);
        if (st == QTC_RHI_SKIPPED) {
            skipped++;
            qtc_wait(10);
            continue;
        }
        CHECK_EQ(st, QTC_OK);
        if (st != QTC_OK) break;
        CHECK_EQ(qtc_rhi_begin_pass(r, sc, 0xFFFF0000u, 1.0, 0), QTC_OK);
        CHECK_EQ(qtc_rhi_set_pipeline(r, p), QTC_OK);
        CHECK_EQ(qtc_rhi_set_bindings(r, s), QTC_OK);
        CHECK_EQ(qtc_rhi_set_vertex_buffer(r, 0, vb, 0), QTC_OK);
        CHECK_EQ(qtc_rhi_draw(r, 6, 1, 0, 0), QTC_OK);
        CHECK_EQ(qtc_rhi_end_pass(r), QTC_OK);
        if (frames == 0) rb = qtc_rhi_read_swapchain(sc);
        CHECK_EQ(qtc_rhi_destroy(sc), QTC_ERR_STATE);   /* its frame is being recorded */
        CHECK_EQ(qtc_rhi_end_frame(r), QTC_OK);
        frames++;
    }
    printf("  %s swapchain: %d frames (%d skipped while the window was not exposed)\n", name, frames, skipped);
    CHECK_EQ(frames, 3);
    CHECK_EQ(qtc_rhi_read_swapchain(sc), 0);   /* outside its frames */
    if (rb) {
        CHECK_EQ(qtc_rhi_finish(r), QTC_OK);
        CHECK_EQ(qtc_rhi_readback_ready(rb), true);
        qtc_val img = qtc_rhi_readback_image(rb);
        CHECK(img != 0);
        if (img && real) {
            qtc_call(img, S("height"));
            int64_t h = qtc_ret_int();
            qtc_call(img, S("width"));
            int64_t w = qtc_ret_int();
            CHECK(w >= 80 && h >= 60);
            qtc_arg_int(3);
            qtc_arg_int(h / 2);
            qtc_call(img, S("pixel"));
            CHECK_EQ((uint32_t)qtc_ret_uint(), 0xFF00FF00u);
            qtc_arg_int(w - 3);
            qtc_arg_int(h / 2);
            qtc_call(img, S("pixel"));
            CHECK_EQ((uint32_t)qtc_ret_uint(), 0xFFFF0000u);
        }
        qtc_val_free(img);
    }
    /* the window goes first: its surface is let go of, the swapchain can draw no more */
    qtc_destroy(win);
    drain();
    CHECK_EQ(qtc_rhi_begin_frame(r, sc), QTC_ERR_STATE);
    CHECK_EQ(qtc_rhi_destroy(sc), QTC_OK);   /* pipelines keep their own copy of its render pass */
    CHECK_EQ(qtc_rhi_destroy(p), QTC_OK);
    /* a window destroyed in the middle of a frame on it: the frame is dropped, nothing reaches
       the gone surface */
    {
        qtc_obj w2 = qtc_new(S("QWindow"));
        qtc_arg_int(40);
        qtc_arg_int(40);
        qtc_call(w2, S("resize"));
        qtc_rhi sc2 = qtc_rhi_swapchain(r, w2, 0);
        qtc_rhi p2 = sc2 ? color_pipeline(r, vs, fs, s, sc2) : 0;
        qtc_call(w2, S("show"));
        qtc_status st = QTC_RHI_SKIPPED;
        for (int i = 0; i < 200 && st == QTC_RHI_SKIPPED; i++) {
            qtc_process_events();
            st = qtc_rhi_begin_frame(r, sc2);
            if (st == QTC_RHI_SKIPPED) qtc_wait(10);
        }
        CHECK_EQ(st, QTC_OK);
        if (st == QTC_OK) {
            CHECK_EQ(qtc_rhi_begin_pass(r, sc2, 0xFF000000u, 1.0, 0), QTC_OK);
            CHECK_EQ(qtc_rhi_set_pipeline(r, p2), QTC_OK);
            qtc_destroy(w2);   /* deleted (and its surface let go of) once events are processed */
            qtc_process_events();
            CHECK_EQ(qtc_rhi_draw(r, 3, 1, 0, 0), QTC_ERR_STATE);
            CHECK(strstr(qtc_last_error_message().data, "no frame") != NULL);
            CHECK_EQ(qtc_rhi_end_frame(r), QTC_ERR_STATE);
            CHECK_EQ(qtc_rhi_begin_frame(r, sc2), QTC_ERR_STATE);   /* the window is gone */
            CHECK_EQ(qtc_rhi_begin_frame(r, 0), QTC_OK);            /* the RHI goes on */
            CHECK_EQ(qtc_rhi_end_frame(r), QTC_OK);
        } else {
            qtc_destroy(w2);
        }
        drain();
    }
    CHECK_EQ(qtc_rhi_destroy(r), QTC_OK);
    qtc_rhi_destroy(vs);
    qtc_rhi_destroy(fs);
}

/* ------------------------------------------------------------------ QRhiWidget */

enum { T_W_INIT = 1, T_W_RENDER, T_W_RELEASE };
static int g_w_calls[4];
static qtc_rhi g_w_rhi, g_w_pipe, g_w_srb, g_w_vb, g_w_ub, g_w_vs, g_w_fs, g_w_target_seen;
static qtc_status g_w_in[8];
static int g_w_leave_open, g_w_rebuilt;
static qtc_rhi g_w_pipe_rhi;
static void *g_w_gl_fn;

static int32_t handler(const qtc_override_call *c, void *user) {
    (void)user;
    if (c->tag < 1 || c->tag > 3) return QTC_OV_BASE;
    g_w_calls[c->tag]++;
    if (c->tag == T_W_RELEASE) return QTC_OV_BASE;
    qtc_rhi rh = qtc_rhi_widget(c->self);
    qtc_rhi t = qtc_rhi_widget_target(c->self);
    if (!rh || !t) return QTC_OV_DONE;
    g_w_rhi = rh;
    g_w_target_seen = t;
    g_w_in[0] = qtc_rhi_begin_frame(rh, 0);   /* the widget's frame */
    g_w_in[1] = qtc_rhi_end_frame(rh);
    g_w_in[2] = qtc_rhi_destroy(rh);
    /* the resources are the RHI's: made again when the widget renders with another one (as when
       grabFramebuffer makes a QRhi of its own) */
    if (c->tag == T_W_INIT || rh != g_w_pipe_rhi) {
        if (rh != g_w_pipe_rhi) {
            g_w_pipe_rhi = rh;
            g_w_rebuilt++;
            g_w_ub = qtc_rhi_buffer(rh, QTC_RHI_DYNAMIC, QTC_RHI_UNIFORM_BUFFER, 64);
            qtc_rhi_upload(g_w_ub, 0, bytes_of(WHITE, sizeof WHITE));
            g_w_srb = qtc_rhi_bindings(rh);
            qtc_rhi_bind_uniform(g_w_srb, 0, QTC_RHI_VERTEX, g_w_ub, 0, 16);
            qtc_rhi_bindings_create(g_w_srb);
            g_w_pipe = color_pipeline(rh, g_w_vs, g_w_fs, g_w_srb, t);
            g_w_vb = qtc_rhi_buffer(rh, QTC_RHI_IMMUTABLE, QTC_RHI_VERTEX_BUFFER, sizeof LEFT_GREEN);
            qtc_rhi_upload(g_w_vb, 0, bytes_of(LEFT_GREEN, sizeof LEFT_GREEN));
        }
        if (c->tag == T_W_INIT) return QTC_OV_DONE;
    }
    qtc_rhi_native_handles nh;
    if (!g_w_gl_fn && qtc_rhi_native(rh, &nh) == QTC_OK && nh.backend == QTC_RHI_BACKEND_OPENGL)
        g_w_gl_fn = qtc_gl_proc_address(S("glGetString"));   /* the frame's context is current */
    g_w_in[3] = qtc_rhi_begin_pass(rh, t, 0xFF0000FFu, 1.0, 0);
    g_w_in[4] = qtc_rhi_set_pipeline(rh, g_w_pipe);
    g_w_in[5] = qtc_rhi_set_bindings(rh, g_w_srb);
    qtc_rhi_set_vertex_buffer(rh, 0, g_w_vb, 0);
    g_w_in[6] = qtc_rhi_draw(rh, 6, 1, 0, 0);
    if (!g_w_leave_open) g_w_in[7] = qtc_rhi_end_pass(rh);   /* else: the call's end closes it */
    return QTC_OV_DONE;
}

static void test_widget(int api, const char *name) {
    memset(g_w_calls, 0, sizeof g_w_calls);
    memset(g_w_in, 0x7f, sizeof g_w_in);
    g_w_rhi = g_w_pipe = g_w_srb = g_w_vb = g_w_ub = g_w_target_seen = g_w_pipe_rhi = 0;
    g_w_rebuilt = 0;
    g_w_gl_fn = NULL;
    g_w_vs = bake(QTC_RHI_VERTEX, VS_COLOR);
    g_w_fs = bake(QTC_RHI_FRAGMENT, FS_COLOR);
    qtc_obj w = qtc_new_overridable(S("QRhiWidget"));
    CHECK(w != 0);
    if (!w) return;
    CHECK_EQ(qtc_rhi_widget(w), 0);   /* no RHI before it renders */
    CHECK_EQ(qtc_last_error(), QTC_ERR_STATE);
    CHECK_EQ(qtc_rhi_widget_target(w), 0);
    qtc_arg_int(api);
    CHECK_EQ(qtc_call(w, S("setApi")), QTC_OK);
    CHECK_EQ(qtc_override(w, S("initialize"), T_W_INIT), QTC_OK);
    CHECK_EQ(qtc_override(w, S("render"), T_W_RENDER), QTC_OK);
    CHECK_EQ(qtc_override(w, S("releaseResources"), T_W_RELEASE), QTC_OK);
    qtc_arg_int(W);
    qtc_arg_int(H);
    qtc_call(w, S("resize"));
    for (int round = 0; round < 2; round++) {
        g_w_leave_open = round;
        if (round == 1) {   /* shown: it renders with its window's RHI */
            qtc_call(w, S("show"));
            for (int i = 0; i < 20; i++) {
                qtc_process_events();
                qtc_wait(5);
            }
        }
        CHECK_EQ(qtc_call(w, S("grabFramebuffer")), QTC_OK);
        qtc_val img = qtc_ret_val();
        if (g_w_calls[T_W_RENDER] == 0) {
            printf("  QRhiWidget (%s): did not render here\n", name);
            qtc_val_free(img);
            break;
        }
        CHECK_EQ(g_w_in[0], QTC_ERR_STATE);
        CHECK_EQ(g_w_in[1], QTC_ERR_STATE);
        CHECK_EQ(g_w_in[2], QTC_ERR_STATE);
        CHECK_EQ(g_w_in[3], QTC_OK);
        CHECK_EQ(g_w_in[4], QTC_OK);
        CHECK_EQ(g_w_in[5], QTC_OK);
        CHECK_EQ(g_w_in[6], QTC_OK);
        if (!round) CHECK_EQ(g_w_in[7], QTC_OK);
        if (api != 0 && img) {
            qtc_arg_int(3);
            qtc_arg_int(H / 2);
            qtc_call(img, S("pixel"));
            const uint32_t left = (uint32_t)qtc_ret_uint() | 0xFF000000u;
            if (left != 0xFF00FF00u) fprintf(stderr, "QRhiWidget %s round %d: left pixel %08x\n", name, round, left);
            CHECK_EQ(left, 0xFF00FF00u);
            qtc_arg_int(W - 3);
            qtc_arg_int(H / 2);
            qtc_call(img, S("pixel"));
            CHECK_EQ((uint32_t)qtc_ret_uint() | 0xFF000000u, 0xFF0000FFu);
        }
        qtc_val_free(img);
    }
    printf("  QRhiWidget (%s): initialize %d, render %d, resources made for %d RHI(s)\n", name, g_w_calls[T_W_INIT],
           g_w_calls[T_W_RENDER], g_w_rebuilt);
    /* outside the call: its frame and target are gone, the RHI stays */
    if (g_w_rhi) {
        CHECK_EQ(qtc_rhi_draw(g_w_rhi, 3, 1, 0, 0), QTC_ERR_STATE);
        CHECK_EQ(qtc_rhi_begin_pass(g_w_rhi, g_w_target_seen, 0, 1.0, 0), QTC_ERR_STATE);
        CHECK_EQ(qtc_rhi_destroy(g_w_target_seen), QTC_ERR_STALE_HANDLE);
        CHECK(qtc_rhi_info(g_w_rhi).len > 0);
        CHECK_EQ(qtc_rhi_widget(w), g_w_rhi);
        if (api == 1) CHECK(g_w_gl_fn != NULL);
    }
    /* the widget (and its RHI) goes: what was made with that RHI goes with it */
    qtc_destroy(w);
    drain();
    if (g_w_rhi) {
        CHECK_EQ(qtc_rhi_destroy(g_w_pipe), QTC_ERR_STALE_HANDLE);
        CHECK_EQ(qtc_rhi_info(g_w_rhi).len, 0);
    }
    qtc_rhi_destroy(g_w_vs);
    qtc_rhi_destroy(g_w_fs);
    qtc_clear_error();
}

/* ------------------------------------------------------------------ Vulkan, Qt Quick */

static void test_vulkan_interop(int windows) {
    qtc_rhi vk = qtc_vk_create(S("[\"VK_KHR_surface\"]"), NOSTR);
    if (!vk) {
        printf("  Vulkan instance: not available here\n");
        CHECK_EQ(qtc_last_error(), QTC_ERR_UNSUPPORTED);
        return;
    }
    CHECK_EQ(qtc_vk_create(S("{\"not\": \"an array\"}"), NOSTR), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_INVALID_ARG);
    void *inst = qtc_vk_instance(vk);
    CHECK(inst != NULL);
    typedef int32_t (*enum_fn)(void *, uint32_t *, void *);
    enum_fn en = (enum_fn)qtc_vk_proc_address(vk, S("vkEnumeratePhysicalDevices"));
    CHECK(en != NULL);
    if (en) {
        uint32_t n = 0;
        CHECK_EQ(en(inst, &n, NULL), 0);
        CHECK(n > 0);
        printf("  Vulkan instance: %u physical device(s)\n", n);
    }
    CHECK(qtc_vk_proc_address(vk, S("vkNoSuchFunction")) == NULL);
    CHECK_EQ(qtc_last_error(), QTC_ERR_NOT_FOUND);
    if (windows) {
        qtc_obj win = qtc_new(S("QWindow"));
        CHECK_EQ(qtc_vk_surface(vk, win), 0u);   /* not attached */
        CHECK_EQ(qtc_vk_attach(vk, win), QTC_OK);
        CHECK_EQ(qtc_vk_attach(vk, win), QTC_OK);
        CHECK_EQ(qtc_vk_surface(vk, win), 0u);   /* not created yet */
        qtc_arg_int(32);
        qtc_arg_int(32);
        qtc_call(win, S("resize"));
        CHECK_EQ(qtc_call(win, S("show")), QTC_OK);
        CHECK(qtc_vk_surface(vk, win) != 0);
        /* the instance goes first: the window lets go of its native window, then of the instance */
        CHECK_EQ(qtc_rhi_destroy(vk), QTC_OK);
        qtc_call(win, S("show"));   /* no instance: Qt warns and shows nothing; no crash */
        drain();
        qtc_destroy(win);
    } else {
        CHECK_EQ(qtc_rhi_destroy(vk), QTC_OK);
    }
    CHECK(qtc_vk_instance(vk) == NULL);
    CHECK_EQ(qtc_last_error(), QTC_ERR_STALE_HANDLE);
    qtc_clear_error();
}

static void test_quick(void) {
    CHECK_EQ(qtc_quick_set_graphics_api(99), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_quick_set_graphics_api(QTC_GAPI_SOFTWARE), QTC_OK);
    const int32_t gq = qtc_quick_graphics_api(0);
    printf("  Qt Quick graphics API after asking for software: %d\n", gq);
    CHECK_EQ(gq, QTC_GAPI_SOFTWARE);
    qtc_obj o = qtc_new(S("QObject"));
    CHECK_EQ(qtc_quick_graphics_api(o), QTC_ERR_WRONG_TYPE);
    qtc_destroy(o);
    qtc_obj qw = qtc_new(S("QQuickWindow"));
    if (qw) {
        CHECK_EQ(qtc_quick_set_graphics_api(QTC_GAPI_OPENGL), QTC_ERR_STATE);   /* a Quick window exists */
        int32_t g = qtc_quick_graphics_api(qw);
        CHECK(g == QTC_GAPI_UNKNOWN || g == QTC_GAPI_SOFTWARE);
        qtc_destroy(qw);
        drain();
    }
    CHECK_EQ(qtc_gl_proc_address(S("glGetString")), NULL);   /* no context current here */
    CHECK_EQ(qtc_last_error(), QTC_ERR_STATE);
    qtc_clear_error();
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (qtc_init(QTC_APP_WIDGETS, NOSTR) != QTC_OK) {
        fprintf(stderr, "qtc_init failed\n");
        return 2;
    }
    qtc_set_quit_on_last_window_closed(false);
    CHECK_EQ(qtc_set_override_handler(handler, NULL), QTC_OK);
    qtc_call_static(S("QGuiApplication"), S("platformName"));
    qtc_view pv = qtc_ret_str();
    char platform[32] = {0};
    memcpy(platform, pv.data, pv.len < 31 ? (size_t)pv.len : 31);
    const int windows = strcmp(platform, "offscreen") != 0 && strcmp(platform, "minimal") != 0;
    printf("platform: %s\n", platform);

    test_validation();
    test_backend(QTC_RHI_BACKEND_NULL, "Null");
    test_backend(QTC_RHI_BACKEND_OPENGL, "OpenGL");
    test_backend(QTC_RHI_BACKEND_VULKAN, "Vulkan");
    test_swapchain(QTC_RHI_BACKEND_NULL, "Null");
    if (windows) {
        test_swapchain(QTC_RHI_BACKEND_OPENGL, "OpenGL");
        test_swapchain(QTC_RHI_BACKEND_VULKAN, "Vulkan");
    }
    test_widget(0, "Null");
    if (windows) {
        test_widget(1, "OpenGL");
        test_widget(3, "Vulkan");
    }
    test_vulkan_interop(windows);
    test_quick();

    /* shutdown with RHIs, resources, open frames and shaders left alive */
    qtc_rhi left = qtc_rhi_create(QTC_RHI_BACKEND_NULL, 0);
    qtc_rhi lt = qtc_rhi_texture(left, QTC_RHI_RGBA8, 4, 4, QTC_RHI_RENDER_TARGET);
    qtc_rhi lrt = qtc_rhi_render_target(lt, false);
    qtc_rhi_begin_frame(left, 0);
    qtc_rhi_begin_pass(left, lrt, 0, 1.0, 0);
    bake(QTC_RHI_VERTEX, VS_COLOR);
    CHECK(qtc_rhi_live() > 0);
    CHECK_EQ(qtc_shutdown(), QTC_OK);
    printf("test_rhi: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
