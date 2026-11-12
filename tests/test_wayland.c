/* Widgets on Wayland (run by run_wayland.sh under a headless weston): windows whose platform
 * window is gone, a closed window, a dismissed or refused popup, still get Expose and
 * UpdateRequest events, and nothing qtc does with those may ask their backing store for a
 * paint device (Qt's Wayland plugin crashes there). An earlier version crashed in each case below. */
#include "qtc.h"

#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s [%.*s]\n", __FILE__, __LINE__, #c, \
    (int)qtc_last_error_message().len, qtc_last_error_message().data); } } while (0)

static void spin(int ms) { qtc_wait(ms); qtc_process_events(); qtc_event ev; while (qtc_poll(&ev)) {} }

static int exposed(qtc_obj w) {
    if (qtc_call(w, QTC_S("windowHandle")) != QTC_OK) return 0;
    qtc_obj h = qtc_ret_obj();
    if (!h || qtc_call(h, QTC_S("isExposed")) != QTC_OK) return 0;
    return qtc_ret_bool();
}

static qtc_obj shown_window(const char *cls) {
    qtc_obj w = qtc_new(qtc_s(cls));
    qtc_arg_int(320); qtc_arg_int(200);
    qtc_call(w, QTC_S("resize"));
    qtc_call(w, QTC_S("show"));
    for (int i = 0; i < 100 && !exposed(w); i++) spin(20);
    return w;
}

int main(void) {
    if (qtc_init(QTC_APP_WIDGETS, QTC_S("test_wayland")) != QTC_OK) { printf("no Qt\n"); return 1; }
    qtc_set_error_policy(QTC_ERRORS_RETURN);
    qtc_set_quit_on_last_window_closed(false);
    qtc_call(qtc_app(), QTC_S("platformName"));
    qtc_view platform = qtc_ret_str();
    printf("platform: %.*s\n", (int)platform.len, platform.data);
    if (platform.len != 7 || memcmp(platform.data, "wayland", 7) != 0) { printf("SKIP: not on Wayland\n"); return 77; }

    /* a main window with a menu: the popup is shown (the compositor refuses it without input
       here) and hidden again, then the window is closed */
    qtc_obj win = shown_window("QMainWindow");
    CHECK(exposed(win));
    qtc_call(win, QTC_S("menuBar"));
    qtc_obj bar = qtc_ret_obj();
    qtc_arg_str(QTC_S("&View"));
    qtc_call(bar, QTC_S("addMenu"));
    qtc_obj menu = qtc_ret_obj();
    qtc_arg_str(QTC_S("&Preview"));
    qtc_call(menu, QTC_S("addAction"));
    qtc_arg_int(40); qtc_arg_int(40);
    qtc_val at = qtc_val_new(QTC_S("QPoint"));
    qtc_arg_val(at);
    CHECK(qtc_call(menu, QTC_S("popup")) == QTC_OK);
    qtc_val_free(at);
    spin(300);
    CHECK(qtc_call(menu, QTC_S("hide")) == QTC_OK);
    spin(300);
    CHECK(qtc_call(win, QTC_S("close")) == QTC_OK);
    spin(300);
    CHECK(qtc_alive(win));

    /* a window closed while it has repaints pending */
    qtc_obj w2 = shown_window("QWidget");
    CHECK(exposed(w2));
    for (int i = 0; i < 5; i++) qtc_call(w2, QTC_S("update"));
    qtc_call(w2, QTC_S("close"));
    spin(300);
    /* shown again, then hidden with repaints pending */
    qtc_call(w2, QTC_S("show"));
    spin(300);
    qtc_call(w2, QTC_S("update"));
    qtc_call(w2, QTC_S("hide"));
    spin(300);
    CHECK(qtc_destroy(w2) == QTC_OK);
    spin(100);

    /* a window destroyed while shown */
    qtc_obj w3 = shown_window("QLabel");
    qtc_call(w3, QTC_S("update"));
    CHECK(qtc_destroy(w3) == QTC_OK);
    spin(300);

    CHECK(qtc_shutdown() == QTC_OK);
    printf("test_wayland: %d passed, %d failed\n", checks - failures, failures);
    return failures ? 1 : 0;
}
