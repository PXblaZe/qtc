/* End-to-end: a small form application built with qtc, driven by simulated input. */
#include "qtc.h"
#include "qtc_test.h"

#include <stdio.h>
#include <string.h>

static int failed = 0, passed = 0;
#define CHECK(c)                                                                                                    \
    do {                                                                                                            \
        if (c) passed++;                                                                                            \
        else {                                                                                                      \
            failed++;                                                                                               \
            fprintf(stderr, "FAIL %s:%d: %s [%.*s]\n", __FILE__, __LINE__, #c, (int)qtc_last_error_message().len,   \
                    qtc_last_error_message().data);                                                                 \
        }                                                                                                           \
    } while (0)
#define S(x) QTC_S(x)

enum { T_SUBMIT = 1, T_NEW, T_QUIT, T_TAB, T_CANVAS, T_NAME };

static qtc_obj child(const char *cls, qtc_obj parent) {
    qtc_arg_obj(parent);
    return qtc_new(qtc_s(cls));
}
static qtc_obj call_obj(qtc_obj o, const char *method) {
    qtc_call(o, qtc_s(method));
    return qtc_ret_obj();
}
static qtc_obj call_obj_s(qtc_obj o, const char *method, const char *arg) {
    qtc_arg_str(qtc_s(arg));
    qtc_call(o, qtc_s(method));
    return qtc_ret_obj();
}
static void add_row(qtc_obj form, const char *label, qtc_obj w) {
    qtc_arg_str(qtc_s(label));
    qtc_arg_obj(w);
    CHECK(qtc_call(form, S("addRow(QString,QWidget*)")) == QTC_OK);
}

int main(void) {
    CHECK(qtc_init(QTC_APP_WIDGETS, S("app\0-platform\0offscreen")) == QTC_OK);
    qtc_obj win = qtc_new(S("QMainWindow"));
    qtc_set_str(win, S("windowTitle"), S("qtc form"));
    qtc_arg_int(520);
    qtc_arg_int(380);
    qtc_call(win, S("resize(int,int)"));

    /* menus, toolbar, status bar */
    qtc_obj bar = call_obj(win, "menuBar");
    qtc_obj file = call_obj_s(bar, "addMenu(QString)", "&File");
    qtc_obj act_new = call_obj_s(file, "addAction(QString)", "&New");
    qtc_obj act_quit = call_obj_s(file, "addAction(QString)", "&Quit");
    CHECK(act_new && act_quit);
    qtc_obj tools = call_obj_s(win, "addToolBar(QString)", "main");
    qtc_arg_obj(act_new);
    CHECK(qtc_call(tools, S("addAction(QAction*)")) == QTC_OK);
    qtc_obj status = call_obj(win, "statusBar");
    qtc_connect(act_new, S("triggered"), T_NEW, 0);
    qtc_connect(act_quit, S("triggered"), T_QUIT, 0);

    /* central widget: tabs with a form and a canvas */
    qtc_obj tabs = qtc_new(S("QTabWidget"));
    qtc_arg_obj(tabs);
    CHECK(qtc_call(win, S("setCentralWidget")) == QTC_OK);
    qtc_obj page = qtc_new(S("QWidget"));
    qtc_obj form = child("QFormLayout", page);
    qtc_obj name = qtc_new(S("QLineEdit"));
    qtc_set_str(name, S("placeholderText"), S("your name"));
    qtc_obj age = qtc_new(S("QSpinBox"));
    qtc_set_int(age, S("maximum"), 130);
    qtc_set_int(age, S("value"), 30);
    qtc_obj country = qtc_new(S("QComboBox"));
    const char *countries[] = {"India", "Netherlands", "Brazil"};
    for (int i = 0; i < 3; i++) {
        qtc_arg_str(qtc_s(countries[i]));
        qtc_call(country, S("addItem(QString)"));
    }
    qtc_obj subscribe = qtc_new(S("QCheckBox"));
    qtc_set_str(subscribe, S("text"), S("Subscribe"));
    qtc_obj submit = qtc_new(S("QPushButton"));
    qtc_set_str(submit, S("text"), S("Submit"));
    qtc_obj entries = qtc_new(S("QListWidget"));
    add_row(form, "Name", name);
    add_row(form, "Age", age);
    add_row(form, "Country", country);
    add_row(form, "", subscribe);
    add_row(form, "", submit);
    add_row(form, "Entries", entries);
    qtc_arg_obj(page);
    qtc_arg_str(S("Form"));
    CHECK(qtc_call(tabs, S("addTab(QWidget*,QString)")) == QTC_OK);
    qtc_obj canvas = qtc_new(S("QtcCanvas"));
    qtc_arg_obj(canvas);
    qtc_arg_str(S("Canvas"));
    CHECK(qtc_call(tabs, S("addTab(QWidget*,QString)")) == QTC_OK);
    CHECK(qtc_call(page, S("parentWidget")) == QTC_OK && qtc_ret_obj() != 0);   /* reparented by Qt */

    qtc_connect(submit, S("clicked"), T_SUBMIT, 0);
    qtc_connect(tabs, S("currentChanged"), T_TAB, 0);
    qtc_connect(name, S("textEdited"), T_NAME, QTC_CONN_COALESCE);
    qtc_watch(canvas, QTC_WATCH(QTC_EV_MOUSE_PRESS), 0, T_CANVAS);

    CHECK(qtct_show(win) == QTC_OK);

    /* the user fills in the form */
    qtc_call(name, S("setFocus"));
    CHECK(qtct_type(name, S("Phoenix")) == QTC_OK);
    for (int i = 0; i < 3; i++) qtct_key(age, 0x01000013 /* Qt::Key_Up */, 0);
    qtc_arg_int(1);
    qtc_call(country, S("setCurrentIndex"));
    CHECK(qtct_click(subscribe, 1, 0, 8, 8) == QTC_OK);   /* on the indicator */
    CHECK(qtct_click(submit, 1, 0, -1, -1) == QTC_OK);
    qtc_call(act_new, S("trigger"));
    qtc_arg_int(1);
    qtc_call(tabs, S("setCurrentIndex"));
    qtct_wait(10);
    CHECK(qtct_click(canvas, 1, 0, 50, 60) == QTC_OK);
    CHECK(qtct_click(canvas, 1, 0, 120, 80) == QTC_OK);
    qtc_post(T_QUIT, 0, 0);

    /* the application's event loop */
    int submits = 0, news = 0, tabs_changed = 0, clicks = 0, edits = 0, edit_count = 0;
    char line[256] = "";
    double dots[16];
    int ndots = 0;
    for (;;) {
        int w = qtc_wait(2000);
        if (w == QTC_WAIT_QUIT || w == QTC_WAIT_TIMEOUT) break;
        qtc_event e;
        while (qtc_poll(&e)) {
            switch (e.tag) {
            case T_SUBMIT: {
                submits++;
                qtc_view n = qtc_get_str(name, S("text"));
                char nm[64];
                snprintf(nm, sizeof nm, "%.*s", (int)n.len, n.data);
                qtc_call(country, S("currentText"));
                qtc_view c = qtc_ret_str();
                snprintf(line, sizeof line, "%s, %lld, %.*s%s", nm, (long long)qtc_get_int(age, S("value")), (int)c.len, c.data,
                         qtc_get_bool(subscribe, S("checked")) ? ", subscribed" : "");
                qtc_arg_str(qtc_s(line));
                qtc_call(entries, S("addItem(QString)"));
                qtc_arg_str(S("saved"));
                qtc_arg_int(0);
                qtc_call(status, S("showMessage"));
                break;
            }
            case T_NEW:
                news++;
                qtc_call(name, S("clear"));
                break;
            case T_TAB: tabs_changed++; break;
            case T_NAME:
                edits++;
                edit_count += e.count;
                break;
            case T_CANVAS:
                clicks++;
                if (ndots < 16) {
                    dots[ndots++] = e.d0;
                    dots[ndots++] = e.d1;
                }
                qtc_draw_begin(canvas);
                qtc_draw_clear(0xffffffffu);
                qtc_draw_antialias(true);
                qtc_draw_brush(0xff2266ccu, 1);
                qtc_draw_no_pen();
                for (int i = 0; i + 1 < ndots; i += 2) qtc_draw_ellipse(dots[i] - 6, dots[i + 1] - 6, 12, 12);
                qtc_draw_end();
                break;
            case T_QUIT: qtc_quit(0); break;
            }
        }
    }
    CHECK(submits == 1);
    CHECK(strcmp(line, "Phoenix, 33, Netherlands, subscribed") == 0);
    CHECK(news == 1);
    CHECK(qtc_get_str(name, S("text")).len == 0);
    CHECK(tabs_changed == 1);
    CHECK(clicks == 2);
    CHECK(edits == 1 && edit_count == 7);   /* 7 keystrokes coalesced into one event */
    CHECK(qtc_get_int(canvas, S("commandCount")) == 6);
    qtc_call(entries, S("count"));
    CHECK(qtc_ret_int() == 1);
    qtc_arg_int(0);
    qtc_call(entries, S("item"));
    CHECK(qtc_last_error() == QTC_ERR_NOT_FOUND || qtc_ret_kind() == QTC_T_NONE);   /* QListWidgetItem* is not bound */
    qtc_call(status, S("currentMessage"));
    CHECK(qtc_ret_str().len == 5);
    qtct_wait(20);
    CHECK(qtct_grab(win, S("app.png")) == QTC_OK);
    if (line[0]) printf("entry: %s\n", line);
    CHECK(qtc_shutdown() == QTC_OK);
    printf("test_app: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
