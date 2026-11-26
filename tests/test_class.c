/* qtc runtime class tests: QObject classes defined from C (qtc_class_*). C, public API only. */
#define _POSIX_C_SOURCE 200809L
#include "qtc.h"

#include "test_thread.h"
#include <stdatomic.h>
#include <stdint.h>
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
#define CHECK_STR(view, lit) CHECK((view).len == (int64_t)strlen(lit) && memcmp((view).data, lit, strlen(lit)) == 0)
#define S(lit) QTC_S(lit)
#define NOSTR qtc_sn(NULL, 0)

static int view_has(qtc_view v, const char *needle) {
    size_t n = strlen(needle);
    for (int64_t i = 0; i + (int64_t)n <= v.len; i++)
        if (memcmp(v.data + i, needle, n) == 0) return 1;
    return 0;
}
static int pump_until(uint64_t tag, int timeout_steps, qtc_event *out) {
    qtc_event e;
    for (int i = 0; i < timeout_steps; i++) {
        while (qtc_poll(&e))
            if (e.tag == tag) {
                if (out) *out = e;
                return 1;
            }
        qtc_wait(10);
    }
    return 0;
}
static void drain(void) {
    qtc_event e;
    qtc_process_events();
    while (qtc_poll(&e)) {}
    qtc_cancel_quit();
    qtc_clear_error();
}

enum { T_ADD = 1, T_DESCRIBE, T_RESET, T_TAKE, T_WORK, T_SQUARE, T_BOARD_PAINT, T_ROWS };
static int g_resets;
static atomic_int g_work_gui = -1;
static atomic_int g_work_arg;

static int32_t handler(const qtc_override_call *c, void *user) {
    (void)user;
    switch (c->tag) {
    case T_ADD:
        qtc_ov_ret_int(qtc_ov_arg_int(0) + qtc_ov_arg_int(1));
        return QTC_OV_DONE;
    case T_DESCRIBE:
        qtc_ov_ret_str(S("a counter"));
        return QTC_OV_DONE;
    case T_RESET:
        g_resets++;
        return QTC_OV_DONE;
    case T_TAKE: {
        qtc_obj o = qtc_ov_arg_obj(0);
        if (o) qtc_ov_ret_str(qtc_get_str(o, S("objectName")).data ? (qtc_str){qtc_get_str(o, S("objectName")).data, qtc_get_str(o, S("objectName")).len} : NOSTR);
        else qtc_ov_ret_str(S("null"));
        return QTC_OV_DONE;
    }
    case T_WORK:
        atomic_store(&g_work_gui, c->gui_thread);
        atomic_store(&g_work_arg, (int)qtc_ov_arg_int(0));
        qtc_post(55, qtc_ov_arg_int(0) * 2, 0);
        return QTC_OV_DONE;
    case T_SQUARE: {
        int64_t x = qtc_ov_arg_int(0);
        qtc_ov_ret_int(x * x);
        return QTC_OV_DONE;
    }
    case T_BOARD_PAINT:
        qtc_draw_begin(c->self);
        qtc_draw_clear(0xff123456u);
        qtc_draw_end();
        return QTC_OV_DONE;
    case T_ROWS:
        qtc_ov_ret_int(3);
        return QTC_OV_DONE;
    default:
        return QTC_OV_BASE;
    }
}

static void define_counter(void) {
    CHECK_EQ(qtc_class_begin(S("Counter"), NOSTR), QTC_OK);
    CHECK_EQ(qtc_class_property(S("value"), S("int"), QTC_PROP_WRITE | QTC_PROP_NOTIFY), QTC_OK);
    CHECK_EQ(qtc_class_property(S("label"), S("QString"), QTC_PROP_WRITE), QTC_OK);
    CHECK_EQ(qtc_class_property(S("kind"), S("QString"), QTC_PROP_CONSTANT), QTC_OK);
    CHECK_EQ(qtc_class_signal(S("fired(int,QString)")), QTC_OK);
    CHECK_EQ(qtc_class_slot(S("add(int,int)"), S("int"), T_ADD), QTC_OK);
    CHECK_EQ(qtc_class_invokable(S("describe()"), S("QString"), T_DESCRIBE), QTC_OK);
    CHECK_EQ(qtc_class_slot(S("reset()"), NOSTR, T_RESET), QTC_OK);
    CHECK_EQ(qtc_class_slot(S("take(QObject*)"), S("QString"), T_TAKE), QTC_OK);
    /* refused while defining */
    CHECK_EQ(qtc_class_begin(S("Other"), NOSTR), QTC_ERR_STATE);
    CHECK_EQ(qtc_class_slot(S("add(int,int)"), S("int"), 9), QTC_ERR_STATE);                 /* twice */
    CHECK_EQ(qtc_class_slot(S("deleteLater()"), NOSTR, 9), QTC_ERR_STATE);                  /* QObject's */
    CHECK_EQ(qtc_class_property(S("objectName"), S("QString"), 0), QTC_ERR_STATE);
    CHECK_EQ(qtc_class_property(S("value"), S("int"), 0), QTC_ERR_STATE);
    CHECK_EQ(qtc_class_property(S("add"), S("int"), 0), QTC_ERR_STATE);                    /* a method's name */
    CHECK_EQ(qtc_class_signal(S("bad(NoSuchType)")), QTC_ERR_INVALID_ARG);
    CHECK_EQ(qtc_class_signal(S("bad(QWidget*)")), QTC_ERR_INVALID_ARG);
    CHECK_EQ(qtc_class_signal(S("no parens")), QTC_ERR_INVALID_ARG);
    CHECK_EQ(qtc_class_slot(S("9bad()"), NOSTR, 1), QTC_ERR_INVALID_ARG);
    CHECK_EQ(qtc_class_property(S("p"), S("int"), QTC_PROP_CONSTANT | QTC_PROP_WRITE), QTC_ERR_INVALID_ARG);
    CHECK_EQ(qtc_class_property(S("p"), S("QObject*"), 0), QTC_ERR_INVALID_ARG);
    CHECK_EQ(qtc_class_property(S("p"), S("int"), 64), QTC_ERR_INVALID_ARG);
    CHECK_EQ(qtc_class_end(), QTC_OK);
    CHECK_EQ(qtc_class_end(), QTC_ERR_STATE);
}

static void test_definition_errors(void) {
    CHECK_EQ(qtc_class_signal(S("x()")), QTC_ERR_STATE);   /* nothing open */
    CHECK_EQ(qtc_class_begin(S("QWidget"), NOSTR), QTC_ERR_STATE);
    CHECK_EQ(qtc_class_begin(S("int"), NOSTR), QTC_ERR_STATE);
    CHECK_EQ(qtc_class_begin(S("bad name"), NOSTR), QTC_ERR_INVALID_ARG);
    CHECK_EQ(qtc_class_begin(S("Fine"), S("QStringListModel")), QTC_ERR_NOT_FOUND);
    CHECK_EQ(qtc_class_begin(S("Dropped"), NOSTR), QTC_OK);
    CHECK_EQ(qtc_class_cancel(), QTC_OK);
    CHECK_EQ(qtc_class_cancel(), QTC_ERR_STATE);
    CHECK(!qtc_class_exists_runtime(S("Dropped")));
    CHECK_EQ(qtc_class_new(S("Dropped")), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_NOT_FOUND);
}

static void test_counter(void) {
    define_counter();
    CHECK(qtc_class_exists_runtime(S("Counter")));
    CHECK(qtc_class_exists(S("Counter")));
    CHECK_EQ(qtc_class_begin(S("Counter"), NOSTR), QTC_ERR_STATE);   /* exists */
    qtc_view d = qtc_describe(0, S("Counter"));
    (void)d;
    qtc_obj o = qtc_class_new(S("Counter"));
    CHECK(o != 0);
    CHECK_STR(qtc_class_name(o), "Counter");
    CHECK(qtc_inherits(o, S("QObject")));
    CHECK(qtc_inherits(o, S("Counter")));
    CHECK(qtc_is_overridable(o));
    qtc_obj o2 = qtc_new(S("Counter"));
    CHECK(o2 != 0);
    qtc_view dd = qtc_describe(o, NOSTR);
    CHECK(view_has(dd, "add(int,int)") && view_has(dd, "valueChanged(int)") && view_has(dd, "\"value\""));

    /* properties, with the notify signal */
    CHECK_EQ(qtc_get_int(o, S("value")), 0);
    uint64_t cv = qtc_connect(o, S("valueChanged"), 10, 0);
    CHECK(cv != 0);
    CHECK_EQ(qtc_set_int(o, S("value"), 5), QTC_OK);
    CHECK_EQ(qtc_get_int(o, S("value")), 5);
    qtc_event e;
    CHECK(pump_until(10, 50, &e));
    CHECK_EQ(e.i0, 5);
    CHECK_EQ(qtc_set_int(o, S("value"), 5), QTC_OK);   /* the same value: no signal */
    CHECK(!pump_until(10, 5, NULL));
    CHECK_EQ(qtc_set_str(o, S("label"), S("hi")), QTC_OK);
    CHECK_STR(qtc_get_str(o, S("label")), "hi");
    CHECK_EQ(qtc_set_str(o, S("kind"), S("x")), QTC_ERR_READ_ONLY);
    CHECK_EQ(qtc_get_int(o2, S("value")), 0);   /* per object */

    /* slots and invokables run the handler */
    qtc_arg_int(2);
    qtc_arg_int(40);
    CHECK_EQ(qtc_call(o, S("add")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 42);
    CHECK_EQ(qtc_call(o, S("describe")), QTC_OK);
    CHECK_STR(qtc_ret_str(), "a counter");
    g_resets = 0;
    CHECK_EQ(qtc_call(o, S("reset")), QTC_OK);
    CHECK_EQ(g_resets, 1);
    qtc_obj named = qtc_new(S("QObject"));
    qtc_set_str(named, S("objectName"), S("buddy"));
    qtc_arg_obj(named);
    CHECK_EQ(qtc_call(o, S("take")), QTC_OK);
    CHECK_STR(qtc_ret_str(), "buddy");
    qtc_arg_null();
    CHECK_EQ(qtc_call(o, S("take")), QTC_OK);
    CHECK_STR(qtc_ret_str(), "null");
    qtc_arg_str(S("nope"));
    CHECK_EQ(qtc_call(o, S("take")), QTC_ERR_BAD_ARGS);

    /* signals */
    uint64_t cf = qtc_connect(o, S("fired"), 11, 0);
    CHECK(cf != 0);
    qtc_arg_int(7);
    qtc_arg_str(S("seven"));
    CHECK_EQ(qtc_emit(o, S("fired")), QTC_OK);
    CHECK(pump_until(11, 50, &e));
    CHECK_EQ(e.i0, 7);
    CHECK_STR(qtc_event_text(e.seq), "seven");
    qtc_arg_int(8);
    qtc_arg_str(S("eight"));
    CHECK_EQ(qtc_call(o, S("fired")), QTC_OK);
    CHECK(pump_until(11, 50, &e));
    CHECK_EQ(e.i0, 8);
    CHECK_EQ(qtc_emit(o, S("nosuch")), QTC_ERR_NOT_FOUND);
    qtc_arg_int(1);
    CHECK_EQ(qtc_emit(o, S("fired")), QTC_ERR_NOT_FOUND);   /* takes two */
    CHECK_EQ(qtc_emit(named, S("destroyed")), QTC_ERR_WRONG_TYPE);

    /* a Qt signal forwarded to a runtime slot */
    qtc_obj timer = qtc_new(S("QTimer"));
    qtc_set_bool(timer, S("singleShot"), true);
    uint64_t fw = qtc_connect_to(timer, S("timeout"), o, S("reset"));
    CHECK(fw != 0);
    g_resets = 0;
    qtc_arg_int(1);
    qtc_call(timer, S("start(int)"));
    for (int i = 0; i < 50 && g_resets == 0; i++) qtc_wait(10);
    CHECK_EQ(g_resets, 1);

    /* QML sees it: a context property */
    if (qtc_class_exists(S("QQmlEngine"))) {
        qtc_obj eng = qtc_new(S("QQmlEngine"));
        qtc_call(eng, S("rootContext"));
        qtc_obj ctx = qtc_ret_obj();
        qtc_arg_str(S("backend"));
        qtc_arg_obj(o);
        CHECK_EQ(qtc_call(ctx, S("setContextProperty(QString,QObject*)")), QTC_OK);
        qtc_arg_obj(eng);
        qtc_obj comp = qtc_new(S("QQmlComponent"));
        qtc_arg_bytes(S("import QtQml\nQtObject { property int v: backend.value; property int sum: backend.add(20, 22);"
                        " property string d: backend.describe() }"));
        qtc_arg_val(qtc_val_str(S("file:///inline.qml")));
        CHECK_EQ(qtc_call(comp, S("setData")), QTC_OK);
        CHECK_EQ(qtc_call(comp, S("create()")), QTC_OK);
        qtc_obj q = qtc_ret_obj();
        CHECK(q != 0);
        if (q) {
            CHECK_EQ(qtc_get_int(q, S("v")), 5);
            CHECK_EQ(qtc_get_int(q, S("sum")), 42);
            CHECK_STR(qtc_get_str(q, S("d")), "a counter");
            qtc_set_int(o, S("value"), 9);   /* the binding follows the notify signal */
            CHECK_EQ(qtc_get_int(q, S("v")), 9);
            qtc_destroy(q);
        }
        qtc_destroy(comp);
        qtc_destroy(eng);
    }
    qtc_destroy(timer);
    qtc_destroy(named);
    qtc_destroy(o);
    qtc_destroy(o2);
    drain();
}

static void test_worker(void) {
    CHECK_EQ(qtc_class_begin(S("Worker"), S("QObject")), QTC_OK);
    CHECK_EQ(qtc_class_slot(S("work(int)"), NOSTR, T_WORK), QTC_OK);
    CHECK_EQ(qtc_class_invokable(S("square(int)"), S("int"), T_SQUARE), QTC_OK);
    CHECK_EQ(qtc_class_end(), QTC_OK);
    qtc_obj th = qtc_new(S("QThread"));
    qtc_call(th, S("start"));
    qtc_obj w = qtc_class_new(S("Worker"));
    CHECK(w != 0);
    CHECK_EQ(qtc_move_to_thread(w, th), QTC_OK);
    qtc_arg_int(21);
    CHECK_EQ(qtc_invoke_queued(w, S("work")), QTC_OK);
    qtc_event e;
    CHECK(pump_until(55, 300, &e));
    CHECK_EQ(e.i0, 42);
    CHECK_EQ(atomic_load(&g_work_gui), 0);   /* ran in the worker thread */
    qtc_arg_int(21);
    qtc_future f = qtc_invoke_future(w, S("square"));
    CHECK(f != 0);
    CHECK_EQ(qtc_future_wait(f, 5000), QTC_WAIT_FINISHED);
    CHECK_EQ(qtc_future_result(f, 0), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 441);
    qtc_future_release(f);
    qtc_arg_int(2);
    CHECK_EQ(qtc_call(w, S("square")), QTC_ERR_WRONG_THREAD);
    CHECK_EQ(qtc_destroy(w), QTC_OK);
    qtc_call(th, S("quit"));
    qtc_arg_int(5000);
    qtc_call(th, S("wait(ulong)"));
    qtc_destroy(th);
    drain();
}

static void test_widget_base(void) {
    CHECK_EQ(qtc_class_begin(S("Board"), S("QWidget")), QTC_OK);
    CHECK_EQ(qtc_class_property(S("score"), S("int"), QTC_PROP_WRITE | QTC_PROP_NOTIFY), QTC_OK);
    CHECK_EQ(qtc_class_signal(S("won()")), QTC_OK);
    CHECK_EQ(qtc_class_end(), QTC_OK);
    qtc_obj b = qtc_class_new(S("Board"));
    CHECK(b != 0);
    CHECK(qtc_inherits(b, S("QWidget")));
    CHECK_EQ(qtc_set_int(b, S("score"), 3), QTC_OK);
    CHECK_EQ(qtc_get_int(b, S("score")), 3);
    CHECK_EQ(qtc_set_int(b, S("minimumWidth"), 50), QTC_OK);   /* QWidget's own property */
    CHECK_EQ(qtc_get_int(b, S("minimumWidth")), 50);
    CHECK_EQ(qtc_override(b, S("paintEvent"), T_BOARD_PAINT), QTC_OK);   /* overridable too */
    qtc_arg_int(40);
    qtc_arg_int(30);
    qtc_call(b, S("resize"));
    qtc_call(b, S("show"));
    qtc_wait(30);
    CHECK_EQ(qtc_call(b, S("grab()")), QTC_OK);
    qtc_val pm = qtc_ret_val();
    qtc_val img = qtc_val_convert(pm, S("QImage"));
    qtc_arg_int(20);
    qtc_arg_int(15);
    qtc_call(img, S("pixel(int,int)"));
    CHECK_EQ(qtc_ret_int() & 0xffffff, 0x123456);
    qtc_val_free(pm);
    qtc_val_free(img);
    CHECK_EQ(qtc_move_to_thread(b, b), QTC_ERR_WRONG_TYPE);
    qtc_destroy(b);

    /* a model with a runtime invokable */
    CHECK_EQ(qtc_class_begin(S("Rows"), S("QAbstractListModel")), QTC_OK);
    CHECK_EQ(qtc_class_invokable(S("total()"), S("int"), T_ROWS), QTC_OK);
    CHECK_EQ(qtc_class_end(), QTC_OK);
    qtc_obj m = qtc_class_new(S("Rows"));
    CHECK(m != 0);
    CHECK_EQ(qtc_override(m, S("rowCount"), T_ROWS), QTC_OK);
    CHECK_EQ(qtc_call(m, S("rowCount")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 3);
    CHECK_EQ(qtc_call(m, S("total")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 3);
    qtc_destroy(m);
    drain();
}

int main(void) {
    if (qtc_init(QTC_APP_WIDGETS, NOSTR) != QTC_OK) return 2;
    qtc_set_quit_on_last_window_closed(false);
    CHECK_EQ(qtc_set_override_handler(handler, NULL), QTC_OK);
    test_definition_errors();
    test_counter();
    test_worker();
    test_widget_base();
    /* a new session starts without them */
    CHECK_EQ(qtc_shutdown(), QTC_OK);
    CHECK_EQ(qtc_init(QTC_APP_WIDGETS, NOSTR), QTC_OK);
    CHECK(!qtc_class_exists_runtime(S("Counter")));
    CHECK(!qtc_class_exists(S("Counter")));
    qtc_set_override_handler(handler, NULL);
    define_counter();
    qtc_obj o = qtc_class_new(S("Counter"));
    qtc_arg_int(1);
    qtc_arg_int(2);
    CHECK_EQ(qtc_call(o, S("add")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 3);
    CHECK_EQ(qtc_shutdown(), QTC_OK);
    printf("test_class: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
