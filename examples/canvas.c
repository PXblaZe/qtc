/* canvas.c: custom drawing, on screen and into an image.
 *
 *   ./canvas             click in the window to drop dots; close it to quit
 *   ./canvas --selftest  draws without a user, saves canvas.png and checks pixels
 *
 * A QtcCanvas is a widget that repaints from the last recording made with
 * qtc_draw_begin(canvas) ... qtc_draw_end(): no paint callback runs inside Qt.
 * qtc_watch delivers its mouse presses as events. The same drawing calls
 * paint straight into a QImage value.
 */
#include "qtc.h"

#include <stdio.h>
#include <string.h>

enum { TAG_PRESS = 1, TAG_SELFTEST_DONE = 2 };
enum { MAX_DOTS = 256 };

static double dots[MAX_DOTS][2];
static int ndots = 0;

static void scene(void) {   /* the drawing calls, whatever the target */
    qtc_draw_clear(0xff1e2430);
    qtc_draw_antialias(true);
    qtc_draw_linear_gradient(0, 0, 400, 0, 0xff2a3a5c, 0xff5c2a4a);
    qtc_draw_no_pen();
    qtc_draw_rect(0, 0, 400, 40);
    qtc_draw_pen(0xffffffff, 1, 1, 0, 0);
    qtc_draw_font(QTC_S("Sans"), 12, 50, false);
    qtc_draw_text_in(0, 0, 400, 40, 0x0084 /* AlignCenter */, QTC_S("click to drop dots"));
    qtc_draw_no_pen();
    qtc_draw_brush(0xffffcc33, 1);
    for (int i = 0; i < ndots; i++) qtc_draw_ellipse(dots[i][0] - 8, dots[i][1] - 8, 16, 16);
}

static void redraw(qtc_obj canvas) {
    if (qtc_draw_begin(canvas) != QTC_OK) return;
    scene();
    qtc_draw_end();   /* the canvas repaints from this recording from now on */
}

static void add_dot(qtc_obj canvas, double x, double y) {
    if (ndots < MAX_DOTS) {
        dots[ndots][0] = x;
        dots[ndots][1] = y;
        ndots++;
    }
    redraw(canvas);
}

/* the colour name ("#rrggbb") of one pixel of an image value */
static void pixel(qtc_val image, int x, int y, char out[16]) {
    qtc_arg_int(x);
    qtc_arg_int(y);
    qtc_call(image, QTC_S("pixelColor"));
    qtc_val color = qtc_ret_val();
    qtc_call(color, QTC_S("name"));
    snprintf(out, 16, "%s", qtc_ret_str().data);
    qtc_val_free(color);
}

int main(int argc, char **argv) {
    const bool selftest = argc > 1 && strcmp(argv[1], "--selftest") == 0;
    if (qtc_init_argv(QTC_APP_WIDGETS, argc, (const char *const *)argv) != QTC_OK) return 1;
    qtc_set_error_policy(QTC_ERRORS_LOG);

    qtc_obj canvas = qtc_new(QTC_S("QtcCanvas"));
    qtc_set_str(canvas, QTC_S("windowTitle"), QTC_S("Canvas"));
    qtc_arg_int(400);
    qtc_arg_int(300);
    qtc_call(canvas, QTC_S("resize"));
    qtc_watch(canvas, QTC_WATCH(QTC_EV_MOUSE_PRESS), 0, TAG_PRESS);
    redraw(canvas);
    qtc_call(canvas, QTC_S("show"));

    if (selftest) {
        add_dot(canvas, 100, 150);   /* what three clicks would do */
        add_dot(canvas, 200, 200);
        add_dot(canvas, 300, 100);
        qtc_post(TAG_SELFTEST_DONE, 0, 0);
    }

    qtc_event ev;
    while (qtc_wait(-1) == QTC_WAIT_EVENTS) {
        while (qtc_poll(&ev)) {
            if (ev.tag == TAG_PRESS) add_dot(canvas, ev.d0, ev.d1);   /* d0, d1: the position */
            else if (ev.tag == TAG_SELFTEST_DONE) qtc_quit(0);
        }
    }

    int rc = 0;
    if (selftest) {
        /* the same scene into an image, saved as a PNG */
        int64_t argb32 = 0;
        qtc_enum_value(QTC_S("QImage::Format_ARGB32"), &argb32);
        qtc_arg_int(400);
        qtc_arg_int(300);
        qtc_arg_int(argb32);
        qtc_val image = qtc_val_new(QTC_S("QImage"));
        qtc_draw_begin(image);
        scene();
        qtc_draw_end();
        qtc_arg_str(QTC_S("canvas.png"));
        qtc_call(image, QTC_S("save"));
        const bool saved = qtc_ret_bool();
        char dot[16], back[16], shot[16];
        pixel(image, 200, 200, dot);
        pixel(image, 20, 280, back);
        /* and what the window shows: grab it */
        qtc_call(canvas, QTC_S("grab"));
        qtc_val pm = qtc_ret_val();
        qtc_call(pm, QTC_S("toImage"));
        qtc_val grabbed = qtc_ret_val();
        pixel(grabbed, 300, 100, shot);
        rc = (saved && strcmp(dot, "#ffcc33") == 0 && strcmp(back, "#1e2430") == 0 && strcmp(shot, "#ffcc33") == 0) ? 0 : 1;
        printf("canvas: saved %s, dot %s, background %s, window %s: %s\n", saved ? "canvas.png" : "nothing", dot, back, shot,
               rc ? "FAIL" : "ok");
        qtc_val_free(grabbed);
        qtc_val_free(pm);
        qtc_val_free(image);
    }
    qtc_shutdown();
    return rc;
}
