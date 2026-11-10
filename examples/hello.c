/* hello.c: a window with a label and a button that counts its clicks.
 *
 *   ./hello              opens the window; close it to quit
 *   ./hello --selftest   clicks the button three times itself and checks the label
 *
 * Objects are handles (qtc_obj), arguments go on an argument stack (qtc_arg_*)
 * before qtc_new or qtc_call, and signals arrive as events from qtc_wait/qtc_poll
 * at the top of the loop, never in the middle of a Qt call.
 */
#include "qtc.h"

#include <stdio.h>
#include <string.h>

enum { TAG_CLICKED = 1, TAG_SELFTEST_DONE = 2 };

int main(int argc, char **argv) {
    const bool selftest = argc > 1 && strcmp(argv[1], "--selftest") == 0;
    if (qtc_init_argv(QTC_APP_WIDGETS, argc, (const char *const *)argv) != QTC_OK) {
        fprintf(stderr, "qtc_init: %s\n", qtc_last_error_message().data);
        return 1;
    }
    qtc_set_error_policy(QTC_ERRORS_LOG);   /* a wrong call prints why, and the program goes on */

    qtc_obj window = qtc_new(QTC_S("QWidget"));
    qtc_set_str(window, QTC_S("windowTitle"), QTC_S("Hello from C"));
    qtc_arg_obj(window);                    /* QVBoxLayout(QWidget *parent) */
    qtc_obj layout = qtc_new(QTC_S("QVBoxLayout"));
    qtc_arg_str(QTC_S("Not clicked yet"));
    qtc_obj label = qtc_new(QTC_S("QLabel"));
    qtc_arg_str(QTC_S("Click me"));
    qtc_obj button = qtc_new(QTC_S("QPushButton"));
    qtc_arg_obj(label);
    qtc_call(layout, QTC_S("addWidget"));   /* the layout gives both a parent: the window */
    qtc_arg_obj(button);
    qtc_call(layout, QTC_S("addWidget"));
    qtc_arg_int(320);
    qtc_arg_int(120);
    qtc_call(window, QTC_S("resize"));

    qtc_connect(button, QTC_S("clicked"), TAG_CLICKED, 0);
    qtc_call(window, QTC_S("show"));

    if (selftest) {
        for (int i = 0; i < 3; i++) qtc_call(button, QTC_S("click"));
        qtc_post(TAG_SELFTEST_DONE, 0, 0);  /* arrives after the three clicks */
    }

    int clicks = 0;
    qtc_event ev;
    while (qtc_wait(-1) == QTC_WAIT_EVENTS) {   /* QTC_WAIT_QUIT once the window closes */
        while (qtc_poll(&ev)) {
            if (ev.tag == TAG_CLICKED) {
                char text[64];
                clicks++;
                snprintf(text, sizeof text, "Clicked %d time%s", clicks, clicks == 1 ? "" : "s");
                qtc_set_str(label, QTC_S("text"), qtc_s(text));
            } else if (ev.tag == TAG_SELFTEST_DONE) {
                qtc_quit(0);
            }
        }
    }

    int rc = 0;
    if (selftest) {
        qtc_view text = qtc_get_str(label, QTC_S("text"));
        rc = strcmp(text.data, "Clicked 3 times") == 0 ? 0 : 1;
        printf("hello: label says \"%s\": %s\n", text.data, rc ? "FAIL" : "ok");
    }
    qtc_shutdown();   /* destroys the window, and with it everything inside */
    return rc;
}
