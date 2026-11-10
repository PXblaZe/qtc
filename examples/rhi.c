/* rhi.c: a spinning triangle in a window, drawn with Qt's RHI (Vulkan, OpenGL,
 * Direct3D or Metal, whichever this machine has; the Null backend otherwise).
 *
 *   ./rhi                 draws until the window is closed
 *   ./rhi --selftest      draws a few frames offscreen, reads one back, checks it
 *
 * Shaders are Vulkan-style GLSL, compiled at run time for the backend in use.
 */
#include "qtc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *VS =
    "#version 440\n"
    "layout(location = 0) in vec2 pos;\n"
    "layout(location = 1) in vec3 color;\n"
    "layout(location = 0) out vec3 v_color;\n"
    "layout(std140, binding = 0) uniform buf { mat4 mvp; };\n"
    "void main() { v_color = color; gl_Position = mvp * vec4(pos, 0.0, 1.0); }\n";
static const char *FS =
    "#version 440\n"
    "layout(location = 0) in vec3 v_color;\n"
    "layout(location = 0) out vec4 frag;\n"
    "void main() { frag = vec4(v_color, 1.0); }\n";

static const float TRIANGLE[] = {
    0.0f, 0.8f, 1.0f, 0.2f, 0.2f,     /* x, y, r, g, b */
    -0.7f, -0.6f, 0.2f, 1.0f, 0.2f,
    0.7f, -0.6f, 0.2f, 0.4f, 1.0f,
};

static qtc_str src(const char *s) { return qtc_sn(s, (int64_t)strlen(s)); }

/* A rotation about z, as a column-major mat4. */
static void rotation(float out[16], double angle) {
    memset(out, 0, 16 * sizeof(float));
    out[0] = (float)cos(angle);
    out[1] = (float)sin(angle);
    out[4] = (float)-sin(angle);
    out[5] = (float)cos(angle);
    out[10] = 1;
    out[15] = 1;
}

int main(int argc, char **argv) {
    const bool selftest = argc > 1 && strcmp(argv[1], "--selftest") == 0;
    if (qtc_init_argv(QTC_APP_GUI, argc, (const char *const *)argv) != QTC_OK) return 1;
    qtc_set_error_policy(QTC_ERRORS_LOG);

    /* the first backend this machine has (failures here are expected: quiet them) */
    qtc_set_error_policy(QTC_ERRORS_RETURN);
    static const int32_t backends[] = {QTC_RHI_BACKEND_VULKAN, QTC_RHI_BACKEND_METAL, QTC_RHI_BACKEND_D3D11,
                                       QTC_RHI_BACKEND_OPENGL, QTC_RHI_BACKEND_NULL};
    qtc_rhi r = 0;
    int32_t backend = 0;
    for (size_t i = 0; i < sizeof backends / sizeof backends[0] && !r; i++) r = qtc_rhi_create(backend = backends[i], 0);
    qtc_set_error_policy(QTC_ERRORS_LOG);
    if (!r) return 1;
    char info[4096] = {0};   /* views last until the next call: keep a copy */
    {
        qtc_view v = qtc_rhi_info(r);
        memcpy(info, v.data, v.len < (int64_t)sizeof info - 1 ? (size_t)v.len : sizeof info - 1);
    }
    printf("rhi: %s\n", info);

    /* where it draws: a window, or (self-test) a texture we can read back */
    qtc_obj window = 0;
    qtc_rhi target = 0, tex = 0;
    if (!selftest) {
        window = qtc_new(QTC_S("QWindow"));
        qtc_set_str(window, QTC_S("title"), QTC_S("qtc RHI"));
        qtc_arg_int(480);
        qtc_arg_int(480);
        qtc_call(window, QTC_S("resize"));
        target = qtc_rhi_swapchain(r, window, QTC_RHI_SC_DEPTH);
        if (!target) return 1;
        qtc_call(window, QTC_S("show"));
    } else {
        tex = qtc_rhi_texture(r, QTC_RHI_RGBA8, 128, 128, QTC_RHI_RENDER_TARGET | QTC_RHI_READABLE);
        target = qtc_rhi_render_target(tex, true);
    }

    qtc_rhi vbuf = qtc_rhi_buffer(r, QTC_RHI_IMMUTABLE, QTC_RHI_VERTEX_BUFFER, sizeof TRIANGLE);
    qtc_rhi_upload(vbuf, 0, qtc_sn((const char *)TRIANGLE, sizeof TRIANGLE));
    qtc_rhi ubuf = qtc_rhi_buffer(r, QTC_RHI_DYNAMIC, QTC_RHI_UNIFORM_BUFFER, 64);
    qtc_rhi srb = qtc_rhi_bindings(r);
    qtc_rhi_bind_uniform(srb, 0, QTC_RHI_VERTEX, ubuf, 0, 64);
    qtc_rhi_bindings_create(srb);
    qtc_rhi vs = qtc_rhi_shader_bake(QTC_RHI_VERTEX, src(VS));
    qtc_rhi fs = qtc_rhi_shader_bake(QTC_RHI_FRAGMENT, src(FS));
    qtc_rhi pipe = qtc_rhi_pipeline(r);
    qtc_rhi_pipeline_shaders(pipe, vs, fs);
    qtc_rhi_pipeline_vertex_binding(pipe, 5 * sizeof(float), false);
    qtc_rhi_pipeline_vertex_attribute(pipe, 0, 0, QTC_RHI_FLOAT2, 0);
    qtc_rhi_pipeline_vertex_attribute(pipe, 0, 1, QTC_RHI_FLOAT3, 2 * sizeof(float));
    if (qtc_rhi_pipeline_create(pipe, srb, target) != QTC_OK) return 1;

    /* the backend's clip space (Vulkan's y points down): its correction matrix, times ours */
    float corr[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    {
        const char *p = strstr(info, "\"clip_space_corr\":[");
        if (p) {
            p += strlen("\"clip_space_corr\":[");
            for (int i = 0; i < 16; i++) {
                corr[i] = (float)strtod(p, (char **)&p);
                if (*p == ',') p++;
            }
        }
    }

    int frames = 0, ok = 1;
    qtc_rhi readback = 0;
    for (;;) {
        if (!selftest) {
            if (qtc_wait(16) == QTC_WAIT_QUIT) break;
            qtc_event e;
            while (qtc_poll(&e)) {}
        }
        float rot[16], mvp[16];
        rotation(rot, selftest ? 0.0 : frames * 0.02);
        for (int c = 0; c < 4; c++)
            for (int rr = 0; rr < 4; rr++) {
                float v = 0;
                for (int k = 0; k < 4; k++) v += corr[k * 4 + rr] * rot[c * 4 + k];
                mvp[c * 4 + rr] = v;
            }
        qtc_status st = qtc_rhi_begin_frame(r, selftest ? 0 : target);
        if (st == QTC_RHI_SKIPPED) continue;   /* the window is hidden or minimized */
        if (st != QTC_OK) { ok = 0; break; }
        qtc_rhi_upload(ubuf, 0, qtc_sn((const char *)mvp, sizeof mvp));
        qtc_rhi_begin_pass(r, target, 0xff181820u, 1.0, 0);
        qtc_rhi_set_pipeline(r, pipe);
        qtc_rhi_set_bindings(r, srb);
        qtc_rhi_set_vertex_buffer(r, 0, vbuf, 0);
        qtc_rhi_draw(r, 3, 1, 0, 0);
        qtc_rhi_end_pass(r);
        if (selftest && frames == 2) readback = qtc_rhi_read_texture(tex);
        if (qtc_rhi_end_frame(r) != QTC_OK) { ok = 0; break; }
        frames++;
        if (selftest && frames == 3) break;
    }

    if (selftest) {
        /* the triangle's middle is drawn (on a backend that draws), a corner is the clear color */
        qtc_val img = qtc_rhi_readback_image(readback);
        ok = ok && img != 0;
        if (img && backend != QTC_RHI_BACKEND_NULL) {
            qtc_arg_int(64);
            qtc_arg_int(64);
            qtc_call(img, QTC_S("pixel"));
            const uint32_t mid = (uint32_t)qtc_ret_uint();
            qtc_arg_int(2);
            qtc_arg_int(2);
            qtc_call(img, QTC_S("pixel"));
            const uint32_t corner = (uint32_t)qtc_ret_uint();
            ok = ok && mid != 0xff181820u && corner == 0xff181820u;
        }
        printf("rhi: %d frames, %s\n", frames, ok ? "ok" : "FAILED");
    }
    qtc_shutdown();   /* everything made with the RHI goes with it */
    return ok ? 0 : 1;
}
