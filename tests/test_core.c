/* qtc core tests, written in C against the public API only. */
#define _POSIX_C_SOURCE 200809L
#include "qtc.h"
#include "qtc_direct.h"
#include "qtc_inline.h"
#include "qtc_test.h"

#include <math.h>
#include <pthread.h>
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

static qtc_obj make(const char *cls) { return qtc_new(qtc_s(cls)); }
static qtc_obj make_child(const char *cls, qtc_obj parent) {
    qtc_arg_obj(parent);
    return qtc_new(qtc_s(cls));
}
static int drain(qtc_event *last, uint64_t tag) {
    int n = 0;
    qtc_event e;
    while (qtc_poll(&e))
        if (!tag || e.tag == tag) {
            n++;
            if (last) *last = e;
        }
    return n;
}
static void pump(int ms) { qtct_wait(ms); }
static void reset(void) {
    qtc_process_events();
    drain(NULL, 0);
    qtc_cancel_quit();
    qtc_clear_error();
}

/* ------------------------------------------------------------------ tests */

static void test_before_init(void) {
    CHECK(!qtc_is_initialized());
    CHECK_EQ(qtc_new(S("QWidget")), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_NOT_INITIALIZED);
    CHECK_EQ(qtc_shutdown(), QTC_ERR_NOT_INITIALIZED);
    CHECK_EQ(qtc_post(1, 2, 3), QTC_ERR_NOT_INITIALIZED);
    CHECK_EQ(qtc_wait(0), QTC_ERR_NOT_INITIALIZED);
    CHECK(!qtc_alive(12345));
    CHECK_STR(qtc_version(), QTC_VERSION_STRING);
    CHECK(qtc_qt_version().len > 0);
    CHECK_STR(qtc_status_name(QTC_ERR_STALE_HANDLE), "QTC_ERR_STALE_HANDLE");
    CHECK_EQ(qtc_init(99, S("x")), QTC_ERR_INVALID_ARG);
    CHECK_EQ(qtc_init(QTC_APP_WIDGETS, qtc_sn(NULL, 5)), QTC_ERR_INVALID_ARG);
}

static void test_init(void) {
    CHECK_EQ(qtc_init(QTC_APP_WIDGETS, S("test_core\0-platform\0offscreen")), QTC_OK);
    CHECK(qtc_is_initialized());
    CHECK_EQ(qtc_init(QTC_APP_WIDGETS, S("again")), QTC_ERR_STATE);
    CHECK(qtc_app() != 0);
    CHECK(qtc_inherits(qtc_app(), S("QApplication")));
    CHECK(qtc_class_count() > 400);
    qtc_view st = qtc_stats();
    CHECK(st.len > 10 && strstr(st.data, "\"methods\"") != NULL);
}

static void test_handles(void) {
    reset();
    qtc_obj w = make("QWidget");
    CHECK(w != 0);
    CHECK(qtc_alive(w));
    CHECK(qtc_is_object(w));
    CHECK(!qtc_is_value(w));
    CHECK_STR(qtc_class_name(w), "QWidget");
    qtc_obj b = make_child("QPushButton", w);
    CHECK(b != 0);
    CHECK(qtc_inherits(b, S("QAbstractButton")));
    CHECK(!qtc_inherits(b, S("QLabel")));
    /* the same object always has the same handle */
    CHECK_EQ(qtc_call(b, S("parentWidget")), QTC_OK);
    CHECK_EQ(qtc_ret_obj(), w);

    /* destroy: the handle is stale at once, children follow when deleted */
    CHECK_EQ(qtc_destroy(w), QTC_OK);
    CHECK(!qtc_alive(w));
    CHECK_EQ(qtc_set_str(w, S("windowTitle"), S("x")), QTC_ERR_STALE_HANDLE);
    CHECK_EQ(qtc_destroy(w), QTC_ERR_STALE_HANDLE);
    CHECK_EQ(qtc_process_events(), QTC_OK);
    CHECK(!qtc_alive(b));
    CHECK_EQ(qtc_call(b, S("click")), QTC_ERR_STALE_HANDLE);
    CHECK_EQ(qtc_call(b, S("text")), QTC_ERR_STALE_HANDLE);

    /* garbage and null handles */
    CHECK_EQ(qtc_call(0, S("show")), QTC_ERR_NULL_HANDLE);
    CHECK_EQ(qtc_call(0xdeadbeefcafeull, S("show")), QTC_ERR_BAD_HANDLE);
    CHECK_EQ(qtc_call(0xffffffffffffffffull, S("show")), QTC_ERR_BAD_HANDLE);
    CHECK_EQ(qtc_destroy(0x100000001ull + 999999), QTC_ERR_BAD_HANDLE);
    qtc_val v = qtc_val_int(5);
    CHECK_EQ(qtc_destroy(v), QTC_ERR_WRONG_TYPE);
    CHECK_EQ(qtc_set_int(v, S("x"), 1), QTC_ERR_WRONG_TYPE);
    CHECK_EQ(qtc_val_free(v), QTC_OK);
    CHECK_EQ(qtc_val_free(v), QTC_ERR_STALE_HANDLE);
    CHECK_EQ(qtc_destroy(qtc_app()), QTC_ERR_STATE);

    /* deleted by Qt: a parent deleted through Qt itself */
    qtc_obj p = make("QWidget");
    qtc_obj c = make_child("QLabel", p);
    CHECK_EQ(qtc_call(p, S("deleteLater")), QTC_ERR_UNSUPPORTED);
    CHECK_EQ(qtc_set_bool(p, S("visible"), false), QTC_OK);
    CHECK_EQ(qtc_destroy(p), QTC_OK);
    pump(1);
    CHECK(!qtc_alive(c));

    /* handles are never reused for another object */
    qtc_obj first = make("QObject");
    qtc_destroy(first);
    qtc_process_events();
    qtc_obj second = make("QObject");
    CHECK(second != first);
    CHECK(!qtc_alive(first));
    CHECK(qtc_alive(second));
    qtc_destroy(second);
    qtc_process_events();
}

static void test_errors(void) {
    reset();
    qtc_clear_error();
    CHECK_EQ(qtc_last_error(), QTC_OK);
    qtc_obj l = make("QLabel");
    CHECK_EQ(qtc_call(l, S("noSuchMethod")), QTC_ERR_NOT_FOUND);
    CHECK_EQ(qtc_last_error(), QTC_ERR_NOT_FOUND);
    CHECK(strstr(qtc_last_error_message().data, "noSuchMethod") != NULL);
    CHECK_EQ(qtc_new(S("NoSuchClass")), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_NOT_FOUND);
    CHECK_EQ(qtc_new(S("QAbstractButton")), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_UNSUPPORTED);
    CHECK_EQ(qtc_new(S("QColor")), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_WRONG_TYPE);
    CHECK_EQ(qtc_call(l, qtc_sn(NULL, 3)), QTC_ERR_INVALID_ARG);
    CHECK_EQ(qtc_call(l, S("")), QTC_ERR_INVALID_ARG);
    /* argument stack overflow */
    for (int i = 0; i < 17; i++) qtc_arg_int(i);
    CHECK_EQ(qtc_call(l, S("setText")), QTC_ERR_BAD_ARGS);
    CHECK_EQ(qtc_args_count(), 0);
    /* wrong argument types and counts */
    qtc_arg_int(5);
    CHECK_EQ(qtc_call(l, S("setText")), QTC_ERR_BAD_ARGS);
    qtc_arg_str(S("a"));
    qtc_arg_str(S("b"));
    qtc_arg_str(S("c"));
    CHECK_EQ(qtc_call(l, S("setText")), QTC_ERR_BAD_ARGS);
    /* an object of the wrong class as an argument */
    qtc_obj w = make("QWidget");
    qtc_obj sl = make_child("QSlider", w);
    qtc_arg_obj(sl);
    CHECK_EQ(qtc_call(l, S("setBuddy(QWidget*)")), QTC_OK);
    qtc_arg_obj(qtc_app());
    CHECK_EQ(qtc_call(l, S("setBuddy(QWidget*)")), QTC_ERR_BAD_ARGS);
    /* a stale object as an argument */
    qtc_obj gone = make("QWidget");
    qtc_destroy(gone);
    qtc_arg_obj(gone);
    CHECK_EQ(qtc_call(l, S("setBuddy")), QTC_ERR_STALE_HANDLE);
    /* parent cycles are refused */
    qtc_arg_obj(sl);
    CHECK_EQ(qtc_call(w, S("setParent(QWidget*)")), QTC_ERR_REJECTED);
    qtc_arg_obj(w);
    CHECK_EQ(qtc_call(w, S("setParent(QWidget*)")), QTC_ERR_REJECTED);
    /* refused calls */
    CHECK_EQ(qtc_call(w, S("moveToThread")), QTC_ERR_UNSUPPORTED);
    CHECK_EQ(qtc_call(qtc_app(), S("exec")), QTC_ERR_STATE);
    CHECK_EQ(qtc_call_static(S("QApplication"), S("exec")), QTC_ERR_STATE);
    qtc_destroy(w);
    qtc_destroy(l);
    qtc_process_events();
}

static void test_properties(void) {
    reset();
    qtc_obj w = make("QWidget");
    CHECK_EQ(qtc_set_str(w, S("windowTitle"), S("Hello")), QTC_OK);
    CHECK_STR(qtc_get_str(w, S("windowTitle")), "Hello");
    CHECK_EQ(qtc_set_bool(w, S("enabled"), false), QTC_OK);
    CHECK(!qtc_get_bool(w, S("enabled")));
    CHECK_EQ(qtc_set_int(w, S("minimumWidth"), 120), QTC_OK);
    CHECK_EQ(qtc_get_int(w, S("minimumWidth")), 120);
    CHECK_EQ(qtc_set_int(w, S("noSuchProperty"), 1), QTC_ERR_NOT_FOUND);
    CHECK_EQ(qtc_set_int(w, S("isActiveWindow"), 1), QTC_ERR_READ_ONLY);
    CHECK_EQ(qtc_set_str(w, S("minimumWidth"), S("wide")), QTC_ERR_BAD_ARGS);
    qtc_obj s = make_child("QSlider", w);
    CHECK_EQ(qtc_set_int(s, S("maximum"), 1000), QTC_OK);
    CHECK_EQ(qtc_set_int(s, S("value"), 250), QTC_OK);
    CHECK_EQ(qtc_get_int(s, S("value")), 250);
    CHECK_EQ(qtc_set_double(s, S("value"), 3.5), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_set_int(s, S("value"), 5000000000LL), QTC_ERR_OUT_OF_RANGE);
    /* enum properties are validated against the enum */
    qtc_obj l = make_child("QLabel", w);
    int64_t right = 0, vcenter = 0;
    CHECK_EQ(qtc_enum_value(S("Qt::AlignRight"), &right), QTC_OK);
    CHECK_EQ(qtc_enum_value(S("Qt::AlignVCenter"), &vcenter), QTC_OK);
    CHECK_EQ(right, 2);
    CHECK_EQ(qtc_set_int(l, S("alignment"), right | vcenter), QTC_OK);
    CHECK_EQ(qtc_get_int(l, S("alignment")), right | vcenter);
    CHECK_EQ(qtc_set_int(l, S("alignment"), 0x40000000), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_set_int(l, S("textFormat"), 77), QTC_ERR_OUT_OF_RANGE);
    /* dynamic properties */
    qtc_arg_str(S("payload"));
    CHECK_EQ(qtc_dynprop_set(w, S("myData")), QTC_OK);
    CHECK_EQ(qtc_prop_get(w, S("myData")), QTC_OK);
    CHECK_STR(qtc_ret_str(), "payload");
    qtc_arg_int(1);
    CHECK_EQ(qtc_dynprop_set(w, S("windowTitle")), QTC_ERR_STATE);
    qtc_destroy(w);
    qtc_process_events();
}

static void test_calls(void) {
    reset();
    qtc_obj w = make("QWidget");
    qtc_obj l = make_child("QLabel", w);
    qtc_arg_str(S("text from C"));
    CHECK_EQ(qtc_call(l, S("setText")), QTC_OK);
    CHECK_EQ(qtc_call(l, S("text")), QTC_OK);
    CHECK_EQ(qtc_ret_kind(), QTC_T_STR);
    CHECK_STR(qtc_ret_str(), "text from C");
    /* overloads: addItem(QString) vs addItem(QIcon, QString) vs addItem(QString, QVariant) */
    qtc_obj cb = make_child("QComboBox", w);
    qtc_arg_str(S("one"));
    CHECK_EQ(qtc_call(cb, S("addItem")), QTC_OK);
    qtc_arg_str(S("two"));
    qtc_val data = qtc_val_int(42);
    qtc_arg_val(data);
    CHECK_EQ(qtc_call(cb, S("addItem")), QTC_OK);
    qtc_val icon = qtc_val_new(S("QIcon"));
    CHECK(icon != 0);
    qtc_arg_val(icon);
    qtc_arg_str(S("three"));
    CHECK_EQ(qtc_call(cb, S("addItem")), QTC_OK);
    CHECK_EQ(qtc_call(cb, S("count")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 3);
    qtc_arg_int(1);
    CHECK_EQ(qtc_call(cb, S("itemData(int)")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 42);
    qtc_arg_int(2);
    CHECK_EQ(qtc_call(cb, S("itemText")), QTC_OK);
    CHECK_STR(qtc_ret_str(), "three");
    qtc_arg_int(99);
    CHECK_EQ(qtc_call(cb, S("itemText")), QTC_OK);     /* Qt returns "" for an index out of range */
    CHECK_STR(qtc_ret_str(), "");
    /* method strings are cached by their bytes: a buffer the program reuses for another
       method must reach that method, and one that differs only in spacing the same one */
    {
        char buf[32];
        strcpy(buf, "itemText(int)");
        qtc_arg_int(0);
        CHECK_EQ(qtc_call(cb, qtc_sn(buf, (int64_t)strlen(buf))), QTC_OK);
        CHECK_STR(qtc_ret_str(), "one");
        strcpy(buf, "itemData(int)");
        qtc_arg_int(1);
        CHECK_EQ(qtc_call(cb, qtc_sn(buf, (int64_t)strlen(buf))), QTC_OK);
        CHECK_EQ(qtc_ret_int(), 42);
        strcpy(buf, " itemText( int ) ");
        qtc_arg_int(2);
        CHECK_EQ(qtc_call(cb, qtc_sn(buf, (int64_t)strlen(buf))), QTC_OK);
        CHECK_STR(qtc_ret_str(), "three");
        strcpy(buf, "itemTextX(int)");
        qtc_arg_int(0);
        CHECK_EQ(qtc_call(cb, qtc_sn(buf, (int64_t)strlen(buf))), QTC_ERR_NOT_FOUND);
        /* the same with NUL-terminated strings (qtc_s), which are measured and cached too */
        strcpy(buf, "itemText(int)");
        qtc_arg_int(0);
        CHECK_EQ(qtc_call(cb, qtc_s(buf)), QTC_OK);
        CHECK_STR(qtc_ret_str(), "one");
        strcpy(buf, "itemData");
        qtc_arg_int(1);
        CHECK_EQ(qtc_call(cb, qtc_s(buf)), QTC_OK);
        CHECK_EQ(qtc_ret_int(), 42);
        strcpy(buf, "itemDat");
        qtc_arg_int(1);
        CHECK_EQ(qtc_call(cb, qtc_s(buf)), QTC_ERR_NOT_FOUND);
        CHECK_EQ(qtc_call(cb, qtc_s("")), QTC_ERR_INVALID_ARG);
        qtc_obj l2 = qtc_new(qtc_s("QLabel"));
        CHECK(l2 != 0);
        strcpy(buf, "QLabe");
        CHECK_EQ(qtc_new(qtc_s(buf)), 0);
        qtc_destroy(l2);
    }
    /* full signature */
    qtc_arg_str(S("four"));
    CHECK_EQ(qtc_call(cb, S("addItem(QString,QVariant)")), QTC_OK);
    CHECK_EQ(qtc_call(cb, S("addItem(QString, QNoSuchType)")), QTC_ERR_NOT_FOUND);
    /* slots through the meta-object system */
    CHECK_EQ(qtc_call(cb, S("clear")), QTC_OK);
    CHECK_EQ(qtc_call(cb, S("count")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 0);
    /* integer ranges */
    qtc_obj sp = make_child("QSpinBox", w);
    qtc_arg_int(10000000000LL);
    CHECK_EQ(qtc_call(sp, S("setMaximum")), QTC_ERR_OUT_OF_RANGE);
    qtc_arg_int(500);
    CHECK_EQ(qtc_call(sp, S("setMaximum")), QTC_OK);
    qtc_arg_double(42.0);
    CHECK_EQ(qtc_call(sp, S("setValue")), QTC_OK);
    CHECK_EQ(qtc_get_int(sp, S("value")), 42);
    qtc_arg_double(42.5);
    CHECK_EQ(qtc_call(sp, S("setValue")), QTC_ERR_BAD_ARGS);
    /* enums and flags as arguments */
    qtc_arg_int(1 | 0x80);
    CHECK_EQ(qtc_call(l, S("setAlignment")), QTC_OK);
    qtc_arg_int(0x7fff0000);
    CHECK_EQ(qtc_call(l, S("setAlignment")), QTC_ERR_OUT_OF_RANGE);
    qtc_arg_int(12345);
    CHECK_EQ(qtc_call(l, S("setTextFormat")), QTC_ERR_OUT_OF_RANGE);
    qtc_arg_int(12345);
    CHECK_EQ(qtc_call(l, S("setTextFormat(Qt::TextFormat)")), QTC_ERR_OUT_OF_RANGE);
    /* statics */
    qtc_arg_str(S("qtc-tests"));
    CHECK_EQ(qtc_call_static(S("QCoreApplication"), S("setApplicationName")), QTC_OK);
    CHECK_EQ(qtc_call_static(S("QCoreApplication"), S("applicationName")), QTC_OK);
    CHECK_STR(qtc_ret_str(), "qtc-tests");
    CHECK_EQ(qtc_call_static(S("QDateTime"), S("currentDateTime")), QTC_OK);
    CHECK_EQ(qtc_ret_kind(), QTC_T_VALUE);
    qtc_val now = qtc_ret_val();
    CHECK(now != 0);
    CHECK_EQ(qtc_call(now, S("isValid")), QTC_OK);
    CHECK(qtc_ret_bool());
    qtc_val_free(now);
    /* optional out-parameters accept only null */
    qtc_val text = qtc_val_str(S("123"));
    CHECK_EQ(qtc_call(text, S("toInt")), QTC_OK);      /* QString's own methods work on string values */
    CHECK_EQ(qtc_ret_int(), 123);
    qtc_arg_null();
    qtc_arg_int(16);
    CHECK_EQ(qtc_call(text, S("toInt")), QTC_OK);      /* the bool *ok out-parameter takes null */
    CHECK_EQ(qtc_ret_int(), 0x123);
    qtc_arg_int(1);
    qtc_arg_int(16);
    CHECK_EQ(qtc_call(text, S("toInt")), QTC_ERR_BAD_ARGS);
    qtc_val_free(text);
    /* objects returned as lists */
    CHECK_EQ(qtc_call(w, S("children")), QTC_OK);
    CHECK_EQ(qtc_ret_kind(), QTC_T_LIST);
    qtc_val kids = qtc_ret_val();
    CHECK_EQ(qtc_list_len(kids), 3);
    qtc_val first = qtc_list_get(kids, 0);
    CHECK_EQ(qtc_val_get_obj(first), l);
    qtc_val_free(first);
    qtc_val_free(kids);
    qtc_val_free(data);
    qtc_val_free(icon);
    /* find_child */
    CHECK_EQ(qtc_set_str(sp, S("objectName"), S("spinner")), QTC_OK);
    CHECK_EQ(qtc_find_child(w, S("spinner"), S("QSpinBox"), true), sp);
    CHECK_EQ(qtc_find_child(w, S("spinner"), S("QLabel"), true), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_NOT_FOUND);
    qtc_destroy(w);
    qtc_process_events();
}

static void test_values(void) {
    reset();
    qtc_val i = qtc_val_int(-7), d = qtc_val_double(2.5), b = qtc_val_bool(true), s = qtc_val_str(S("h\xc3\xa9llo"));
    int64_t iv = 0;
    double dv = 0;
    bool bv = false;
    CHECK_EQ(qtc_val_get_int(i, &iv), QTC_OK);
    CHECK_EQ(iv, -7);
    CHECK_EQ(qtc_val_get_double(d, &dv), QTC_OK);
    CHECK(dv == 2.5);
    CHECK_EQ(qtc_val_get_bool(b, &bv), QTC_OK);
    CHECK(bv);
    CHECK_STR(qtc_val_get_str(s), "h\xc3\xa9llo");
    CHECK_EQ(qtc_val_kind(s), QTC_T_STR);
    CHECK_STR(qtc_val_type_name(d), "double");
    CHECK_EQ(qtc_val_get_int(s, &iv), QTC_ERR_CONVERSION);
    CHECK_EQ(qtc_val_get_int(i, NULL), QTC_ERR_INVALID_ARG);
    /* lists and maps */
    qtc_val list = qtc_val_list();
    CHECK_EQ(qtc_list_append(list, i), QTC_OK);
    CHECK_EQ(qtc_list_append(list, s), QTC_OK);
    CHECK_EQ(qtc_list_len(list), 2);
    CHECK_EQ(qtc_list_get(list, 2), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_list_get(list, -1), 0);
    qtc_val map = qtc_val_map();
    CHECK_EQ(qtc_map_set(map, S("list"), list), QTC_OK);
    CHECK_EQ(qtc_map_set(map, S("pi"), d), QTC_OK);
    CHECK(qtc_map_has(map, S("pi")));
    CHECK(!qtc_map_has(map, S("tau")));
    CHECK_EQ(qtc_map_len(map), 2);
    CHECK_STR(qtc_val_to_json(map), "{\"list\":[-7,\"h\xc3\xa9llo\"],\"pi\":2.5}");
    CHECK_EQ(qtc_list_len(map), QTC_ERR_WRONG_TYPE);
    CHECK_EQ(qtc_map_len(list), QTC_ERR_WRONG_TYPE);
    qtc_val j = qtc_val_from_json(S("{\"a\": [1, 2, {\"b\": null}], \"c\": true}"));
    CHECK(j != 0);
    CHECK_EQ(qtc_map_len(j), 2);
    CHECK_EQ(qtc_val_from_json(S("{oops")), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_CONVERSION);
    /* Qt value types */
    qtc_arg_int(255);
    qtc_arg_int(0);
    qtc_arg_int(0);
    qtc_val red = qtc_val_new(S("QColor"));
    CHECK(red != 0);
    CHECK_STR(qtc_val_type_name(red), "QColor");
    CHECK_EQ(qtc_call(red, S("name")), QTC_OK);
    CHECK_STR(qtc_ret_str(), "#ff0000");
    qtc_arg_int(128);
    CHECK_EQ(qtc_call(red, S("setGreen")), QTC_OK);
    CHECK_EQ(qtc_call(red, S("green")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 128);
    qtc_arg_int(999);
    CHECK_EQ(qtc_call(red, S("setGreen")), QTC_OK);  /* Qt clamps / warns but does not crash */
    qtc_val c2 = qtc_val_copy(red);
    CHECK(qtc_val_equal(red, c2));
    qtc_val str = qtc_val_str(S("#00ff00"));
    qtc_val green = qtc_val_convert(str, S("QColor"));
    CHECK(green != 0);
    CHECK_EQ(qtc_call(green, S("green")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 255);
    CHECK_EQ(qtc_val_convert(str, S("QNoSuchType")), 0);
    /* a QColor value where a widget wants a QColor (implicit string conversion too) */
    qtc_val sz = qtc_val_new(S("QSize"));
    CHECK(sz != 0);
    qtc_arg_int(3);
    qtc_arg_int(4);
    qtc_val rect = qtc_val_new(S("QSize"));
    CHECK_EQ(qtc_call(rect, S("width")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 3);
    CHECK_EQ(qtc_val_new(S("QWidget")), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_WRONG_TYPE);
    int64_t before = qtc_live_values();
    qtc_val tmp[100];
    for (int k = 0; k < 100; k++) tmp[k] = qtc_val_int(k);
    CHECK_EQ(qtc_live_values(), before + 100);
    for (int k = 0; k < 100; k++) qtc_val_free(tmp[k]);
    CHECK_EQ(qtc_live_values(), before);
    qtc_val all[] = {i, d, b, s, list, map, j, red, c2, str, green, sz, rect};
    for (size_t k = 0; k < sizeof all / sizeof all[0]; k++) CHECK_EQ(qtc_val_free(all[k]), QTC_OK);
}

static void test_strings(void) {
    reset();
    qtc_obj l = make("QLabel");
    const char emoji[] = "Goose \xf0\x9f\xa6\xa2 \xe4\xbd\xa0\xe5\xa5\xbd";
    CHECK_EQ(qtc_set_str(l, S("text"), qtc_s(emoji)), QTC_OK);
    CHECK_STR(qtc_get_str(l, S("text")), emoji);
    /* embedded NUL survives because lengths are explicit */
    CHECK_EQ(qtc_set_str(l, S("text"), qtc_sn("a\0b", 3)), QTC_OK);
    qtc_view v = qtc_get_str(l, S("text"));
    CHECK(v.len == 3 && v.data[1] == '\0' && v.data[2] == 'b');
    /* invalid UTF-8 becomes U+FFFD, never a crash */
    CHECK_EQ(qtc_set_str(l, S("text"), qtc_sn("\xff\xfe\x80", 3)), QTC_OK);
    v = qtc_get_str(l, S("text"));
    CHECK(v.len > 0);
    CHECK_EQ(qtc_set_str(l, S("text"), qtc_sn(NULL, 0)), QTC_OK);
    CHECK_STR(qtc_get_str(l, S("text")), "");
    CHECK_EQ(qtc_set_str(l, S("text"), qtc_sn(NULL, 10)), QTC_ERR_BAD_ARGS);
    /* a 20 MB string */
    size_t big = 20u << 20;
    char *buf = malloc(big);
    memset(buf, 'x', big);
    CHECK_EQ(qtc_set_str(l, S("objectName"), qtc_sn(buf, (int64_t)big)), QTC_OK);
    CHECK_EQ(qtc_get_str(l, S("objectName")).len, (int64_t)big);
    free(buf);
    qtc_destroy(l);
    qtc_process_events();
}

static void test_signals(void) {
    reset();
    qtc_obj w = make("QWidget");
    qtc_obj b = make_child("QPushButton", w);
    uint64_t c = qtc_connect(b, S("clicked"), 77, 0);
    CHECK(c != 0);
    CHECK_EQ(qtc_call(b, S("click")), QTC_OK);
    qtc_event e;
    CHECK(qtc_poll(&e));
    CHECK_EQ(e.kind, QTC_EV_SIGNAL);
    CHECK_EQ(e.tag, 77u);
    CHECK_EQ(e.source, b);
    CHECK_EQ(e.id, c);
    CHECK_EQ(e.argc, 1);
    CHECK_EQ(e.i0, 0); /* checked = false */
    CHECK_STR(qtc_event_signal(e.seq), "clicked(bool)");
    CHECK_EQ(qtc_event_arg_kind(e.seq, 0), QTC_T_BOOL);
    bool checked = true;
    CHECK_EQ(qtc_event_arg_bool(e.seq, 0, &checked), QTC_OK);
    CHECK(!checked);
    CHECK_EQ(qtc_event_arg_bool(e.seq, 5, &checked), QTC_ERR_OUT_OF_RANGE);
    CHECK(!qtc_poll(&e));
    /* string arguments */
    qtc_obj le = make_child("QLineEdit", w);
    uint64_t tc = qtc_connect(le, S("textChanged"), 5, 0);
    CHECK(tc != 0);
    CHECK_EQ(qtc_set_str(le, S("text"), S("typed")), QTC_OK);
    CHECK(qtc_poll(&e));
    CHECK_STR(qtc_event_text(e.seq), "typed");
    CHECK_STR(qtc_event_arg_str(e.seq, 0), "typed");
    /* coalescing: 1000 changes, one event with the last value */
    qtc_obj s = make_child("QSlider", w);
    qtc_set_int(s, S("maximum"), 2000);
    uint64_t vc = qtc_connect(s, S("valueChanged"), 9, QTC_CONN_COALESCE);
    for (int k = 1; k <= 1000; k++) qtc_set_int(s, S("value"), k);
    CHECK_EQ(qtc_pending(), 1);
    CHECK(qtc_poll(&e));
    CHECK_EQ(e.count, 1000);
    CHECK_EQ(e.i0, 1000);
    qtc_set_int(s, S("value"), 5);
    CHECK(qtc_poll(&e));
    CHECK_EQ(e.count, 1);
    CHECK_EQ(e.i0, 5);
    /* ONCE */
    uint64_t once = qtc_connect(b, S("clicked()"), 8, QTC_CONN_ONCE);
    CHECK(once != 0);
    qtc_call(b, S("click"));
    qtc_call(b, S("click"));
    CHECK_EQ(drain(NULL, 8), 1);
    CHECK_EQ(qtc_disconnect(once), QTC_ERR_STALE_HANDLE);
    /* disconnect */
    CHECK_EQ(qtc_disconnect(c), QTC_OK);
    CHECK_EQ(qtc_disconnect(c), QTC_ERR_STALE_HANDLE);
    qtc_call(b, S("click"));
    CHECK_EQ(drain(NULL, 77), 0);
    /* bad connections */
    CHECK_EQ(qtc_connect(b, S("noSuchSignal"), 1, 0), 0u);
    CHECK_EQ(qtc_last_error(), QTC_ERR_NOT_FOUND);
    CHECK_EQ(qtc_connect(b, S("click"), 1, 0), 0u);
    CHECK_EQ(qtc_connect(b, S("clicked"), 1, 0x80), 0u);
    CHECK_EQ(qtc_last_error(), QTC_ERR_INVALID_ARG);
    /* Qt-to-Qt connection: the slider drives a progress bar without C */
    qtc_obj pb = make_child("QProgressBar", w);
    qtc_set_int(pb, S("maximum"), 2000);
    uint64_t fwd = qtc_connect_to(s, S("valueChanged"), pb, S("setValue"));
    CHECK(fwd != 0);
    qtc_set_int(s, S("value"), 321);
    CHECK_EQ(qtc_get_int(pb, S("value")), 321);
    CHECK_EQ(qtc_disconnect(fwd), QTC_OK);
    qtc_set_int(s, S("value"), 10);
    CHECK_EQ(qtc_get_int(pb, S("value")), 321);
    drain(NULL, 0);
    /* destroying the sender: queued events keep a stale source, flagged */
    uint64_t dc = qtc_connect(b, S("clicked"), 11, 0);
    qtc_call(b, S("click"));
    qtc_destroy(b);
    CHECK(qtc_poll(&e));
    CHECK_EQ(e.tag, 11u);
    CHECK(e.flags & QTC_EVF_STALE_SOURCE);
    qtc_process_events();
    CHECK_EQ(qtc_disconnect(dc), QTC_ERR_STALE_HANDLE);
    CHECK_EQ(qtc_disconnect(vc), QTC_OK);
    CHECK_EQ(qtc_disconnect(tc), QTC_OK);
    /* the destroyed signal itself */
    qtc_obj o = make("QObject");
    uint64_t dsig = qtc_connect(o, S("destroyed"), 12, 0);
    CHECK(dsig != 0);
    qtc_destroy(o);
    qtc_process_events();
    CHECK(qtc_poll(&e));
    CHECK_EQ(e.tag, 12u);
    CHECK_EQ(e.source, o);
    /* old events' arguments age out */
    qtc_obj b2 = make_child("QPushButton", w);
    qtc_connect(b2, S("clicked"), 13, 0);
    qtc_call(b2, S("click"));
    CHECK(qtc_poll(&e));
    uint64_t old = e.seq;
    for (int k = 0; k < 70; k++) qtc_call(b2, S("click"));
    while (qtc_poll(&e)) {}
    CHECK_EQ(qtc_event_arg_kind(old, 0), QTC_ERR_STALE_HANDLE);
    qtc_destroy(w);
    qtc_process_events();
    drain(NULL, 0);
}

/* qtc_connect_to calls through qtc's checks, keeps the program's pending
   arguments and result, and survives forwards that set each other off */
static void test_forwards(void) {
    reset();
    qtc_obj w = make("QWidget");
    /* a bound method that is not a slot, matched by the signal's parameter types */
    qtc_obj slider = make_child("QSlider", w);
    qtc_set_int(slider, S("maximum"), 500);
    qtc_obj box = make_child("QWidget", w);
    uint64_t f1 = qtc_connect_to(slider, S("valueChanged"), box, S("setFixedWidth"));
    CHECK(f1 != 0);
    qtc_set_int(slider, S("value"), 123);
    qtc_call(box, S("width"));
    CHECK_EQ(qtc_ret_int(), 123);
    /* the receiver's preconditions apply: a BSP tree depth outside [0, 20] is refused */
    qtc_obj scene = make("QGraphicsScene");
    qtc_obj depth = make_child("QSpinBox", w);
    qtc_arg_int(0);
    qtc_arg_int(1000);
    qtc_call(depth, S("setRange"));
    uint64_t f2 = qtc_connect_to(depth, S("valueChanged"), scene, S("setBspTreeDepth"));
    CHECK(f2 != 0);
    qtc_set_int(depth, S("value"), 7);
    qtc_call(scene, S("bspTreeDepth"));
    CHECK_EQ(qtc_ret_int(), 7);
    qtc_clear_error();
    CHECK_EQ(qtc_set_int(depth, S("value"), 900), QTC_OK);   /* the spin box takes it ... */
    CHECK_EQ(qtc_last_error(), QTC_ERR_OUT_OF_RANGE);         /* ... the forward is refused */
    qtc_call(scene, S("bspTreeDepth"));
    CHECK_EQ(qtc_ret_int(), 7);
    /* methods qtc leaves out cannot be forwarded to either */
    CHECK_EQ(qtc_connect_to(slider, S("sliderPressed"), box, S("deleteLater")), 0u);
    CHECK_EQ(qtc_last_error(), QTC_ERR_UNSUPPORTED);
    CHECK_EQ(qtc_connect_to(slider, S("valueChanged"), box, S("noSuchMethod")), 0u);
    CHECK_EQ(qtc_last_error(), QTC_ERR_NOT_FOUND);
    /* a forward inside a call: the outer call's string argument is untouched */
    qtc_obj edit = make_child("QLineEdit", w);
    qtc_obj label = make_child("QLabel", w);
    uint64_t f3 = qtc_connect_to(edit, S("textChanged"), label, S("setText"));
    CHECK(f3 != 0);
    qtc_arg_str(S("forwarded text"));
    CHECK_EQ(qtc_call(edit, S("setText")), QTC_OK);
    CHECK_STR(qtc_get_str(edit, S("text")), "forwarded text");
    CHECK_STR(qtc_get_str(label, S("text")), "forwarded text");
    /* the result of the program's call is the one left, not the forward's */
    qtc_arg_str(S("abc"));
    qtc_call(edit, S("setText"));
    qtc_call(edit, S("text"));
    CHECK_STR(qtc_ret_str(), "abc");
    /* a forward from the event loop leaves pending arguments alone */
    qtc_obj timer = qtc_timer(1, true, 0);
    uint64_t f4 = qtc_connect_to(timer, S("timeout"), slider, S("triggerAction"));
    CHECK_EQ(f4, 0u);   /* triggerAction needs an argument timeout() does not carry */
    uint64_t f5 = qtc_connect_to(timer, S("timeout"), depth, S("stepUp"));
    CHECK(f5 != 0);
    qtc_set_int(depth, S("value"), 3);
    qtc_arg_int(42);
    qtc_arg_str(S("still here"));
    for (int k = 0; k < 50 && qtc_wait(10) != QTC_WAIT_QUIT; k++) {}
    CHECK_EQ(qtc_args_count(), 2);
    qtc_args_clear();
    CHECK_EQ(qtc_get_int(depth, S("value")), 4);
    qtc_destroy(timer);
    /* forwards that set each other off stop at the depth limit, not with a stack overflow */
    qtc_obj spin = make_child("QSpinBox", w);
    qtc_arg_int(0);
    qtc_arg_int(100000);
    qtc_call(spin, S("setRange"));
    uint64_t f6 = qtc_connect_to(spin, S("valueChanged"), spin, S("stepUp"));
    CHECK(f6 != 0);
    qtc_clear_error();
    qtc_set_int(spin, S("value"), 1);
    CHECK_EQ(qtc_last_error(), QTC_ERR_LIMIT);
    CHECK_EQ(qtc_get_int(spin, S("value")), 1 + 32);
    /* the receiver dies: the forward goes with it, quietly */
    qtc_destroy(box);
    qtc_clear_error();
    qtc_set_int(slider, S("value"), 7);
    CHECK_EQ(qtc_last_error(), QTC_OK);
    qtc_process_events();
    CHECK_EQ(qtc_disconnect(f1), QTC_ERR_STALE_HANDLE);
    CHECK_EQ(qtc_disconnect(f2), QTC_OK);
    CHECK_EQ(qtc_disconnect(f3), QTC_OK);
    CHECK_EQ(qtc_disconnect(f6), QTC_OK);
    qtc_destroy(scene);
    qtc_destroy(w);
    reset();
}

/* Methods only the meta-object has: for classes qtc binds, what the bound
   API leaves out stays out (private signals, denied slots) */
static void test_meta_only_methods(void) {
    reset();
    qtc_obj model = make("QStringListModel");
    qtc_obj view = make("QListView");
    qtc_arg_obj(model);
    qtc_call(view, S("setModel"));
    qtc_val root = qtc_val_new(S("QModelIndex"));
    qtc_arg_val(root);
    qtc_arg_int(0);
    qtc_arg_int(1000);
    CHECK_EQ(qtc_call(model, S("rowsInserted")), QTC_ERR_NOT_FOUND);   /* a private signal */
    qtc_val_free(root);
    qtc_obj o = make("QObject");
    CHECK(qtc_call(o, S("destroyed")) != QTC_OK);
    CHECK(qtc_alive(o));
    CHECK(qtc_call(o, S("objectNameChanged")) != QTC_OK);
    qtc_obj t = make("QThread");
    CHECK(qtc_call(t, S("terminate")) != QTC_OK);
    /* what the bound API has still works, by name or by signature */
    CHECK_EQ(qtc_call(model, S("rowCount")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 0);
    qtc_arg_int(0);
    qtc_arg_int(2);
    CHECK_EQ(qtc_call(model, S("insertRows(int,int)")), QTC_OK);
    CHECK(qtc_ret_bool());
    /* qtc's own classes are reached through their meta-objects */
    qtc_obj bridge = make("QtcBridge");
    uint64_t c = qtc_connect(bridge, S("message"), 31, 0);
    qtc_arg_str(S("ping"));
    CHECK_EQ(qtc_call(bridge, S("post")), QTC_OK);
    qtc_event e;
    CHECK_EQ(drain(&e, 31), 1);
    qtc_disconnect(c);
    qtc_destroy(bridge);
    qtc_destroy(t);
    qtc_destroy(o);
    qtc_destroy(view);
    qtc_destroy(model);
    reset();
}

/* callback style: the handler destroys the very button that sent the event */
static int g_handled = 0;
static qtc_obj g_win = 0;
static void handler(const qtc_event *e, void *user) {
    (void)user;
    g_handled++;
    if (e->tag == 100) {
        CHECK_EQ(qtc_destroy(e->source), QTC_OK);
        CHECK_EQ(qtc_destroy(g_win), QTC_OK);
        qtc_post(101, 0, 0);
    } else if (e->tag == 101) {
        CHECK_EQ(qtc_wait(0), QTC_ERR_STATE);
        CHECK_EQ(qtc_run(), QTC_ERR_STATE);
        qtc_quit(7);
    }
}
static void test_handler_destroys_sender(void) {
    reset();
    g_win = make("QWidget");
    qtc_obj b = make_child("QPushButton", g_win);
    qtc_connect(b, S("clicked"), 100, 0);
    CHECK_EQ(qtc_set_handler(handler, NULL), QTC_OK);
    qtc_call(b, S("click"));
    CHECK_EQ(qtc_run(), 7);
    CHECK_EQ(g_handled, 2);
    CHECK(!qtc_alive(b));
    CHECK(qtc_quit_requested());
    CHECK_EQ(qtc_exit_code(), 7);
    CHECK_EQ(qtc_wait(10), QTC_WAIT_QUIT);
    CHECK_EQ(qtc_cancel_quit(), QTC_OK);
    CHECK_EQ(qtc_wait(0), QTC_WAIT_TIMEOUT);
    qtc_set_handler(NULL, NULL);
    CHECK_EQ(qtc_run(), QTC_ERR_STATE);
}

static void test_watch_and_input(void) {
    reset();
    qtc_obj w = make("QWidget");
    qtc_obj le = make_child("QLineEdit", w);
    CHECK_EQ(qtct_show(w), QTC_OK);
    qtc_call(le, S("setFocus"));
    uint64_t wk = qtc_watch(le, QTC_WATCH_KEYS, QTC_WATCH(QTC_EV_KEY_PRESS), 20);
    CHECK(wk != 0);
    CHECK_EQ(qtct_type(le, S("ab")), QTC_OK);
    qtc_event e;
    int presses = 0;
    while (qtc_poll(&e))
        if (e.kind == QTC_EV_KEY_PRESS) {
            presses++;
            CHECK(qtc_event_text(e.seq).len == 1);
        }
    CHECK_EQ(presses, 2);
    CHECK_STR(qtc_get_str(le, S("text")), "");    /* consumed: the line edit never saw them */
    CHECK_EQ(qtc_unwatch(wk), QTC_OK);
    CHECK_EQ(qtc_unwatch(wk), QTC_ERR_STALE_HANDLE);
    CHECK_EQ(qtct_type(le, S("cd")), QTC_OK);
    CHECK_STR(qtc_get_str(le, S("text")), "cd");
    /* mouse */
    qtc_obj cv = make_child("QtcCanvas", w);
    qtc_arg_int(200);
    qtc_arg_int(100);
    CHECK_EQ(qtc_call(cv, S("resize(int,int)")), QTC_OK);
    qtc_call(cv, S("show"));
    uint64_t wm = qtc_watch(cv, QTC_WATCH_MOUSE, 0, 21);
    CHECK_EQ(qtct_click(cv, 1, 0, 30, 40), QTC_OK);
    int press = 0, release = 0;
    while (qtc_poll(&e)) {
        if (e.kind == QTC_EV_MOUSE_PRESS) {
            press++;
            CHECK_EQ(e.i0, 1);
            CHECK(fabs(e.d0 - 30) < 0.01 && fabs(e.d1 - 40) < 0.01);
        }
        if (e.kind == QTC_EV_MOUSE_RELEASE) release++;
    }
    CHECK(press == 1 && release == 1);
    CHECK_EQ(qtct_click(cv, 99, 0, 1, 1), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtct_click(qtc_val_int(1), 1, 0, 1, 1), QTC_ERR_WRONG_TYPE);
    /* close requests can be refused */
    uint64_t wc = qtc_watch(w, QTC_WATCH(QTC_EV_CLOSE) | QTC_WATCH(QTC_EV_DESTROYED), QTC_WATCH(QTC_EV_CLOSE), 22);
    qtc_set_quit_on_last_window_closed(false);
    CHECK_EQ(qtc_call(w, S("close")), QTC_OK);
    CHECK(qtc_get_bool(w, S("visible")));
    int closes = 0;
    while (qtc_poll(&e)) if (e.kind == QTC_EV_CLOSE) closes++;
    CHECK_EQ(closes, 1);
    /* bad watch requests */
    CHECK_EQ(qtc_watch(w, 0, 0, 1), 0u);
    CHECK_EQ(qtc_watch(w, QTC_WATCH(QTC_EV_RESIZE), QTC_WATCH(QTC_EV_RESIZE), 1), 0u);
    CHECK_EQ(qtc_watch(w, QTC_WATCH(QTC_EV_RESIZE), QTC_WATCH(QTC_EV_CLOSE), 1), 0u);
    CHECK_EQ(qtc_watch(w, 1ull << 60, 0, 1), 0u);
    (void)wm;
    (void)wc;
    qtc_destroy(w);
    qtc_process_events();
    int destroyed = 0;
    while (qtc_poll(&e)) if (e.kind == QTC_EV_DESTROYED && e.tag == 22) destroyed++;
    CHECK_EQ(destroyed, 1);
    qtc_set_quit_on_last_window_closed(true);
}

static void test_timers(void) {
    reset();
    qtc_obj t = qtc_timer(5, false, 30);
    CHECK(t != 0);
    pump(60);
    qtc_event e;
    int n = 0;
    while (qtc_poll(&e))
        if (e.kind == QTC_EV_TIMER) {
            n++;
            CHECK(e.i0 >= 2);       /* coalesced: one event for all timeouts */
        }
    CHECK_EQ(n, 1);
    CHECK_EQ(qtc_wait(100), QTC_WAIT_EVENTS);
    CHECK(qtc_poll(&e));
    CHECK_EQ(e.tag, 30u);
    qtc_destroy(t);
    qtc_process_events();
    drain(NULL, 0);
    qtc_obj one = qtc_timer(1, true, 31);
    CHECK_EQ(qtc_wait(1000), QTC_WAIT_EVENTS);
    CHECK_EQ(drain(NULL, 31), 1);
    CHECK_EQ(qtc_wait(30), QTC_WAIT_TIMEOUT);
    qtc_destroy(one);
    CHECK_EQ(qtc_timer(-1, false, 0), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_OUT_OF_RANGE);
}

#define THREADS 4
#define PER_THREAD 20000
static void *poster(void *arg) {
    intptr_t id = (intptr_t)arg;
    for (int k = 0; k < PER_THREAD; k++) qtc_post(500 + (uint64_t)id, k, id);
    /* calls that are not thread-safe are refused, not raced */
    if (qtc_new(S("QObject")) != 0) return (void *)1;
    if (qtc_last_error() != QTC_ERR_WRONG_THREAD) return (void *)2;
    return NULL;
}
static void test_threads(void) {
    reset();
    pthread_t th[THREADS];
    for (intptr_t i = 0; i < THREADS; i++) pthread_create(&th[i], NULL, poster, (void *)i);
    int got = 0;
    int64_t last[THREADS];
    for (int i = 0; i < THREADS; i++) last[i] = -1;
    int ordered = 1;
    while (got < THREADS * PER_THREAD) {
        int w = qtc_wait(2000);
        if (w != QTC_WAIT_EVENTS) break;
        qtc_event e;
        while (qtc_poll(&e))
            if (e.kind == QTC_EV_POST) {
                int id = (int)e.i1;
                if (e.i0 <= last[id]) ordered = 0;
                last[id] = e.i0;
                got++;
            }
    }
    for (int i = 0; i < THREADS; i++) {
        void *r;
        pthread_join(th[i], &r);
        CHECK(r == NULL);
    }
    CHECK_EQ(got, THREADS * PER_THREAD);
    CHECK(ordered);
    /* queue limit */
    CHECK_EQ(qtc_set_queue_limit(10), QTC_OK);
    int64_t dropped0 = qtc_dropped_events();
    for (int k = 0; k < 100; k++) qtc_post(1, k, 0);
    CHECK_EQ(qtc_pending(), 10);
    CHECK_EQ(qtc_dropped_events() - dropped0, 90);
    CHECK_EQ(qtc_post(1, 0, 0), QTC_ERR_LIMIT);
    CHECK_EQ(qtc_set_queue_limit(1000000), QTC_OK);
    CHECK_EQ(qtc_set_queue_limit(-1), QTC_ERR_OUT_OF_RANGE);
    drain(NULL, 0);
}

static void test_last_window(void) {
    reset();
    qtc_obj w = make("QWidget");
    CHECK_EQ(qtct_show(w), QTC_OK);
    CHECK_EQ(qtc_call(w, S("close")), QTC_OK);
    int r = qtc_wait(1000);
    CHECK_EQ(r, QTC_WAIT_QUIT);
    qtc_event e;
    int lwc = 0;
    while (qtc_poll(&e)) if (e.kind == QTC_EV_LAST_WINDOW_CLOSED) lwc++;
    CHECK_EQ(lwc, 1);
    CHECK_EQ(qtc_exit_code(), 0);
    qtc_cancel_quit();
    /* quit through Qt's own API is redirected */
    CHECK_EQ(qtc_call(qtc_app(), S("quit")), QTC_OK);
    CHECK(qtc_quit_requested());
    qtc_cancel_quit();
    qtc_arg_int(3);
    CHECK_EQ(qtc_call_static(S("QCoreApplication"), S("exit")), QTC_OK);
    CHECK_EQ(qtc_exit_code(), 3);
    qtc_cancel_quit();
    qtc_destroy(w);
    qtc_process_events();
}

static void test_drawing(void) {
    reset();
    qtc_arg_int(64);
    qtc_arg_int(32);
    int64_t fmt = 0;
    CHECK_EQ(qtc_enum_value(S("QImage::Format_ARGB32"), &fmt), QTC_OK);
    qtc_arg_int(fmt);
    qtc_val img = qtc_val_new(S("QImage"));
    CHECK(img != 0);
    CHECK_EQ(qtc_draw_begin(img), QTC_OK);
    CHECK_EQ(qtc_draw_begin(img), QTC_ERR_STATE);
    CHECK_EQ(qtc_draw_clear(0xff0000ffu), QTC_OK);
    CHECK_EQ(qtc_draw_no_pen(), QTC_OK);
    CHECK_EQ(qtc_draw_brush(0xffff0000u, 1), QTC_OK);
    CHECK_EQ(qtc_draw_rect(0, 0, 32, 32), QTC_OK);
    CHECK_EQ(qtc_draw_line(NAN, 0, 1, 1), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_draw_line(1e300, 0, 1, 1), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_draw_pen(0xff000000u, -1, 1, 0, 0), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_draw_pen(0xff000000u, 1, 99, 0, 0), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_draw_restore(), QTC_ERR_STATE);
    CHECK_EQ(qtc_draw_save(), QTC_OK);
    double pts[] = {40, 2, 60, 2, 50, 30};
    qtc_f64s xy = {pts, 6};
    CHECK_EQ(qtc_draw_polygon(xy), QTC_OK);
    qtc_f64s odd = {pts, 5};
    CHECK_EQ(qtc_draw_polygon(odd), QTC_ERR_INVALID_ARG);
    qtc_f64s nul = {NULL, 4};
    CHECK_EQ(qtc_draw_polygon(nul), QTC_ERR_INVALID_ARG);
    CHECK_EQ(qtc_draw_text(2, 30, S("hi")), QTC_OK);
    CHECK_EQ(qtc_draw_path_end(), QTC_ERR_STATE);
    CHECK_EQ(qtc_draw_path_move(0, 0), QTC_OK);
    CHECK_EQ(qtc_draw_path_cubic(10, 0, 20, 30, 30, 30), QTC_OK);
    CHECK_EQ(qtc_draw_path_end(), QTC_OK);
    CHECK(qtc_draw_count() >= 6);
    CHECK_EQ(qtc_draw_end(), QTC_OK);   /* the unmatched save is closed for you */
    CHECK_EQ(qtc_draw_end(), QTC_ERR_STATE);
    qtc_arg_int(5);
    qtc_arg_int(5);
    CHECK_EQ(qtc_call(img, S("pixelColor(int,int)")), QTC_OK);
    qtc_val px = qtc_ret_val();
    CHECK_EQ(qtc_call(px, S("name")), QTC_OK);
    CHECK_STR(qtc_ret_str(), "#ff0000");
    qtc_val_free(px);
    qtc_arg_int(62);
    qtc_arg_int(30);
    CHECK_EQ(qtc_call(img, S("pixelColor(int,int)")), QTC_OK);
    px = qtc_ret_val();
    CHECK_EQ(qtc_call(px, S("name")), QTC_OK);
    CHECK_STR(qtc_ret_str(), "#0000ff");
    qtc_val_free(px);
    /* drawing on a canvas widget and a non-drawable target */
    qtc_obj w = make("QWidget");
    CHECK_EQ(qtc_draw_begin(w), QTC_ERR_WRONG_TYPE);
    qtc_obj cv = make_child("QtcCanvas", w);
    qtc_arg_int(100);
    qtc_arg_int(80);
    qtc_call(cv, S("resize(int,int)"));
    CHECK_EQ(qtc_draw_begin(cv), QTC_OK);
    for (int k = 0; k < 1000; k++) qtc_draw_ellipse(k % 100, k % 80, 5, 5);
    CHECK_EQ(qtc_draw_image(img, 0, 0, 16, 16), QTC_OK);
    CHECK_EQ(qtc_draw_end(), QTC_OK);
    CHECK_EQ(qtc_get_int(cv, S("commandCount")), 1001);
    CHECK_EQ(qtct_show(w), QTC_OK);
    pump(20);
    CHECK(qtc_get_int(cv, S("paintCount")) >= 1);
    CHECK_EQ(qtct_grab(w, S("canvas.png")), QTC_OK);
    CHECK_EQ(qtc_draw_line(0, 0, 1, 1), QTC_ERR_STATE);
    qtc_destroy(w);
    qtc_val_free(img);
    qtc_process_events();
}

static void test_models(void) {
    reset();
    qtc_arg_int(3);
    qtc_arg_int(2);
    qtc_obj m = qtc_new(S("QStandardItemModel"));
    CHECK(m != 0);
    qtc_arg_int(1);
    qtc_arg_int(1);
    CHECK_EQ(qtc_call(m, S("index")), QTC_OK);
    qtc_val idx = qtc_ret_val();
    CHECK(idx != 0);
    qtc_arg_val(idx);
    qtc_arg_str(S("cell"));
    CHECK_EQ(qtc_call(m, S("setData")), QTC_OK);
    CHECK(qtc_ret_bool());
    qtc_arg_val(idx);
    CHECK_EQ(qtc_call(m, S("data")), QTC_OK);
    CHECK_STR(qtc_ret_str(), "cell");
    CHECK_EQ(qtc_call(idx, S("row")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 1);
    /* a view on the model */
    qtc_obj view = make("QTableView");
    qtc_arg_obj(m);
    CHECK_EQ(qtc_call(view, S("setModel")), QTC_OK);
    /* remove the row: the index follows the model and becomes invalid */
    qtc_arg_int(1);
    qtc_arg_int(1);
    CHECK_EQ(qtc_call(m, S("removeRows(int,int)")), QTC_OK);
    CHECK_EQ(qtc_call(idx, S("isValid")), QTC_OK);
    CHECK(!qtc_ret_bool());
    /* destroying the model leaves the index safe to use */
    qtc_arg_int(0);
    qtc_arg_int(0);
    CHECK_EQ(qtc_call(m, S("index")), QTC_OK);
    qtc_val idx2 = qtc_ret_val();
    qtc_destroy(m);
    qtc_process_events();
    CHECK_EQ(qtc_call(idx2, S("isValid")), QTC_OK);
    CHECK(!qtc_ret_bool());
    CHECK_EQ(qtc_call(idx2, S("data")), QTC_OK);
    CHECK_EQ(qtc_ret_kind(), QTC_T_NONE);
    qtc_val_free(idx);
    qtc_val_free(idx2);
    qtc_destroy(view);
    qtc_process_events();
}

static void test_qml(void) {
    reset();
    if (!qtc_class_exists(S("QQmlApplicationEngine"))) {
        fprintf(stderr, "skip: QML module not bound\n");
        return;
    }
    qtc_obj eng = qtc_new(S("QQmlEngine"));
    CHECK(eng != 0);
    qtc_arg_obj(eng);
    qtc_obj comp = qtc_new(S("QQmlComponent"));
    CHECK(comp != 0);
    qtc_arg_bytes(S("import QtQml\nQtObject { property int x: 5; property string name: \"qml\"; signal ping(string s, int n);"
                    " function add(a, b) { return a + b } function w(r) { return r.width } }"));
    qtc_arg_val(qtc_val_str(S("file:///inline.qml")));
    CHECK_EQ(qtc_call(comp, S("setData")), QTC_OK);
    CHECK_EQ(qtc_call(comp, S("create()")), QTC_OK);
    qtc_obj o = qtc_ret_obj();
    CHECK(o != 0);
    if (!o) {
        qtc_call(comp, S("errorString"));
        fprintf(stderr, "QML error: %.*s\n", (int)qtc_ret_str().len, qtc_ret_str().data);
        return;
    }
    CHECK_EQ(qtc_adopt(o), QTC_OK);
    CHECK_EQ(qtc_get_int(o, S("x")), 5);
    CHECK_EQ(qtc_set_int(o, S("x"), 9), QTC_OK);
    CHECK_EQ(qtc_get_int(o, S("x")), 9);
    CHECK_STR(qtc_get_str(o, S("name")), "qml");
    qtc_arg_int(2);
    qtc_arg_int(40);
    CHECK_EQ(qtc_call(o, S("add")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 42);
    /* a value stored outside its variant (QRectF is 32 bytes) reaches an untyped QML parameter
       as itself (1.0.0 passed the address of the value, read as a variant: a crash) */
    qtc_arg_double(1);
    qtc_arg_double(2);
    qtc_arg_double(30);
    qtc_arg_double(40);
    qtc_val rect = qtc_val_new(S("QRectF"));
    CHECK(rect != 0);
    qtc_arg_val(rect);
    CHECK_EQ(qtc_call(o, S("w")), QTC_OK);
    CHECK(qtc_ret_double() == 30.0);
    qtc_val_free(rect);
    uint64_t c = qtc_connect(o, S("ping"), 40, 0);
    CHECK(c != 0);
    qtc_arg_str(S("hello"));
    qtc_arg_int(3);
    CHECK_EQ(qtc_call(o, S("ping")), QTC_OK);
    qtc_event e;
    CHECK(qtc_poll(&e));
    CHECK_EQ(e.tag, 40u);
    CHECK_STR(qtc_event_text(e.seq), "hello");
    CHECK_EQ(e.i0, 3);
    /* the bridge */
    qtc_obj br = qtc_new(S("QtcBridge"));
    uint64_t bc = qtc_connect(br, S("sent"), 41, 0);
    qtc_arg_str(S("clicked"));
    qtc_arg_val(qtc_val_from_json(S("{\"x\": 1}")));
    CHECK_EQ(qtc_call(br, S("send")), QTC_OK);
    CHECK(qtc_poll(&e));
    CHECK_EQ(e.tag, 41u);
    CHECK_STR(qtc_event_text(e.seq), "clicked");
    qtc_val payload = qtc_event_arg_val(e.seq, 1);
    CHECK_STR(qtc_val_to_json(payload), "{\"x\":1}");
    qtc_val_free(payload);
    (void)bc;
    qtc_destroy(o);
    qtc_destroy(comp);
    qtc_destroy(eng);
    qtc_destroy(br);
    qtc_process_events();
    drain(NULL, 0);
}

static void *wrong_thread(void *arg) {
    qtc_obj w = (qtc_obj)(uintptr_t)arg;
    int ok = qtc_set_str(w, S("windowTitle"), S("x")) == QTC_ERR_WRONG_THREAD && qtc_last_error() == QTC_ERR_WRONG_THREAD &&
             qtc_destroy(w) == QTC_ERR_WRONG_THREAD && qtc_wait(0) == QTC_ERR_WRONG_THREAD;
    return ok ? NULL : (void *)1;
}
/* Objects Qt deletes while their signals are blocked never emit destroyed(): their handles
   must still go stale, and the watches and connections on them must be released safely. */
static void test_blocked_signals_lifetime(void) {
    reset();
    qtc_obj parent = make("QWidget");
    qtc_obj child = make_child("QPushButton", parent);
    qtc_obj grandchild = make_child("QLabel", child);
    uint64_t w = qtc_watch(child, QTC_WATCH(QTC_EV_MOUSE_PRESS), 0, 7);
    uint64_t c = qtc_connect(child, S("clicked"), 8, 0);
    CHECK(w != 0 && c != 0);
    qtc_arg_bool(true);
    CHECK_EQ(qtc_call(child, S("blockSignals")), QTC_OK);
    qtc_arg_bool(true);
    CHECK_EQ(qtc_call(grandchild, S("blockSignals")), QTC_OK);
    CHECK_EQ(qtc_destroy(parent), QTC_OK);
    qtc_process_events();   /* the parent's deferred delete runs; the blocked children go silently */
    CHECK(!qtc_alive(child));
    CHECK(!qtc_alive(grandchild));
    CHECK_EQ(qtc_call(child, S("show")), QTC_ERR_STALE_HANDLE);
    CHECK_EQ(qtc_call(grandchild, S("text")), QTC_ERR_STALE_HANDLE);
    CHECK_EQ(qtc_unwatch(w), QTC_ERR_STALE_HANDLE);   /* released with the object, as for any destroyed one */
    qtc_disconnect(c);   /* released already or now: either way no crash */
    /* a new object at a recycled address gets a fresh handle */
    for (int i = 0; i < 50; i++) {
        qtc_obj x = make("QPushButton");
        CHECK(x != child && x != grandchild);
        qtc_destroy(x);
    }
    qtc_process_events();
    /* a blocked object destroyed through qtc behaves normally */
    qtc_obj b = make("QPushButton");
    qtc_arg_bool(true);
    qtc_call(b, S("blockSignals"));
    CHECK_EQ(qtc_destroy(b), QTC_OK);
    qtc_process_events();
    CHECK(!qtc_alive(b));
}

static qtc_obj static_obj(const char *cls, const char *method) {
    if (qtc_call_static(qtc_s(cls), qtc_s(method)) != QTC_OK || qtc_ret_kind() != QTC_T_OBJ) return 0;
    return qtc_ret_obj();
}
static qtc_obj call_obj(qtc_obj o, const char *method) {
    if (qtc_call(o, qtc_s(method)) != QTC_OK || qtc_ret_kind() != QTC_T_OBJ) return 0;
    return qtc_ret_obj();
}

/* What qtc_destroy refuses, releases first or must wait for (qtc_roles.cpp), and the checks
   that keep one object from having two owners. */
static void test_ownership_rules(void) {
    reset();
    /* objects that belong to Qt itself */
    qtc_obj screen = static_obj("QGuiApplication", "primaryScreen");
    CHECK(screen != 0);
    CHECK_EQ(qtc_destroy(screen), QTC_ERR_STATE);
    CHECK_EQ(qtc_destroy(static_obj("QGuiApplication", "styleHints")), QTC_ERR_STATE);
    CHECK_EQ(qtc_destroy(static_obj("QGuiApplication", "clipboard")), QTC_ERR_STATE);
    CHECK_EQ(qtc_destroy(static_obj("QApplication", "style")), QTC_ERR_STATE);
    CHECK_EQ(qtc_destroy(static_obj("QThread", "currentThread")), QTC_ERR_STATE);
    qtc_obj win = make("QWidget");
    CHECK_EQ(qtc_destroy(call_obj(win, "style")), QTC_ERR_STATE);   /* the application style */
    qtc_obj doc = make("QTextDocument");
    CHECK_EQ(qtc_destroy(call_obj(doc, "rootFrame")), QTC_ERR_STATE);
    CHECK_EQ(qtc_destroy(call_obj(doc, "documentLayout")), QTC_ERR_STATE);
    CHECK(qtc_alive(screen));
    /* parts a constructor made inside the object */
    qtc_obj menu = make("QMenu");
    qtc_obj dock = make("QDockWidget");
    CHECK_EQ(qtc_destroy(call_obj(menu, "menuAction")), QTC_ERR_STATE);
    CHECK_EQ(qtc_destroy(call_obj(dock, "toggleViewAction")), QTC_ERR_STATE);
    qtc_obj act = make_child("QAction", menu);   /* made by qtc: an ordinary child */
    CHECK_EQ(qtc_destroy(act), QTC_OK);
    /* owners that let go first */
    qtc_obj tabs = make("QTabWidget");
    qtc_obj corner = make("QPushButton");
    qtc_arg_obj(corner);
    CHECK_EQ(qtc_call(tabs, S("setCornerWidget")), QTC_OK);
    CHECK_EQ(qtc_destroy(corner), QTC_OK);
    qtc_process_events();
    CHECK_EQ(call_obj(tabs, "cornerWidget"), 0);
    qtc_call(tabs, S("show"));
    qtc_process_events();
    qtc_obj nam = make("QNetworkAccessManager");
    qtc_obj cache = make("QNetworkDiskCache");
    qtc_arg_obj(cache);
    CHECK_EQ(qtc_call(nam, S("setCache")), QTC_OK);
    qtc_obj nam2 = make("QNetworkAccessManager");
    qtc_arg_obj(cache);
    CHECK_EQ(qtc_call(nam2, S("setCache")), QTC_ERR_STATE);   /* one owner only */
    CHECK_EQ(qtc_destroy(cache), QTC_OK);                      /* the manager deletes it itself */
    CHECK(!qtc_alive(cache));
    CHECK_EQ(call_obj(nam, "cache"), 0);
    /* an owner that cannot let go: its user goes first (QRestAccessManager is Qt 6.7+) */
    if (qtc_class_exists(S("QRestAccessManager"))) {
        qtc_arg_obj(nam);
        qtc_obj rest = qtc_new(S("QRestAccessManager"));
        CHECK(rest != 0);
        CHECK_EQ(qtc_destroy(nam), QTC_ERR_STATE);
        CHECK_EQ(qtc_destroy(rest), QTC_OK);
        qtc_process_events();
    }
    CHECK_EQ(qtc_destroy(nam), QTC_OK);
    /* property writes go through the setter, with its checks */
    qtc_obj proxy = make("QSortFilterProxyModel");
    qtc_arg_obj(proxy);
    CHECK_EQ(qtc_prop_set(proxy, S("sourceModel")), QTC_ERR_BAD_ARGS);
    qtc_obj model = make("QStandardItemModel");
    qtc_arg_obj(model);
    CHECK_EQ(qtc_prop_set(proxy, S("sourceModel")), QTC_OK);
    CHECK_EQ(call_obj(proxy, "sourceModel"), model);
    /* a read channel outside the device's channels */
    qtc_obj buf = make("QBuffer");
    qtc_arg_int(3);   /* QIODevice::ReadWrite */
    CHECK_EQ(qtc_call(buf, S("open")), QTC_OK);
    qtc_arg_int(1);
    CHECK_EQ(qtc_call(buf, S("setCurrentReadChannel")), QTC_ERR_OUT_OF_RANGE);
    qtc_arg_int(-1);
    CHECK_EQ(qtc_call(buf, S("setCurrentReadChannel")), QTC_ERR_OUT_OF_RANGE);
    qtc_arg_int(0);
    CHECK_EQ(qtc_call(buf, S("setCurrentReadChannel")), QTC_OK);
    /* writing past the end grows the buffer to the position: seeking a step further each time
       must not grow it past the allocation limit */
    {
        int64_t lim = qtc_alloc_limit();
        qtc_arg_int(lim / 2);
        CHECK_EQ(qtc_call(buf, S("seek")), QTC_OK);
        qtc_arg_int(lim + 1);
        CHECK_EQ(qtc_call(buf, S("seek")), QTC_ERR_LIMIT);
        qtc_arg_int(0);
        CHECK_EQ(qtc_call(buf, S("seek")), QTC_OK);
    }
    /* enum values outside the values C++ allows for the enum: not a Qt::Key, nor modifiers */
    qtc_arg_int(0x2000001);
    CHECK_EQ(qtc_val_new(S("QKeyCombination")), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_OUT_OF_RANGE);
    /* a value one overload's enum has picks that overload: QKeyCombination(Qt::ShiftModifier) */
    qtc_arg_int(0x2000000);
    qtc_val kc = qtc_val_new(S("QKeyCombination"));
    CHECK(kc != 0);
    qtc_call(kc, S("keyboardModifiers"));
    CHECK_EQ(qtc_ret_int(), 0x2000000);
    qtc_val_free(kc);
    /* overloads that differ in string views, integer widths or float/double resolve by name */
    qtc_arg_str(S("#3388ff"));
    qtc_val col = qtc_val_new(S("QColor"));   /* QColor(QString) over QColor(QStringView) */
    CHECK(col != 0);
    qtc_call(col, S("name"));
    CHECK_STR(qtc_ret_str(), "#3388ff");
    qtc_val_free(col);
    qtc_arg_int(-42);
    CHECK_EQ(qtc_call_static(S("QString"), S("number")), QTC_OK);   /* number(qlonglong) */
    CHECK_STR(qtc_ret_str(), "-42");
    qtc_arg_double(0.5);
    CHECK_EQ(qtc_call_static(S("QString"), S("number")), QTC_OK);   /* number(double) */
    CHECK_STR(qtc_ret_str(), "0.5");
    /* slots named by strings are not bound: signals come through qtc_connect */
    qtc_arg_int(0);
    qtc_arg_obj(win);
    qtc_arg_str(S("1close()"));
    CHECK_EQ(qtc_call_static(S("QTimer"), S("singleShot")), QTC_ERR_NOT_FOUND);
    qtc_destroy(win);
    qtc_destroy(menu);
    qtc_destroy(dock);
    qtc_destroy(tabs);
    qtc_destroy(nam2);
    qtc_destroy(proxy);
    qtc_destroy(model);
    qtc_destroy(buf);
    qtc_destroy(doc);
    qtc_process_events();
}

/* Crashes and hangs inside Qt that the fuzzers found: each is refused before it happens. */
static void test_fuzz_findings(void) {
    reset();
    /* a widget in a layout without a parent widget leaves that layout when it moves elsewhere,
       so the layout cannot delete it later (QFormLayout::removeRow deletes a row's widgets) */
    qtc_obj form = make("QFormLayout");
    qtc_obj label = make("QLabel");
    qtc_arg_obj(label);
    CHECK_EQ(qtc_call(form, S("addRow(QWidget*)")), QTC_OK);
    qtc_obj split = make("QSplitter");
    qtc_arg_obj(label);
    CHECK_EQ(qtc_call(split, S("addWidget")), QTC_OK);
    CHECK_EQ(qtc_call(form, S("count")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 0);
    qtc_arg_int(0);
    qtc_call(form, S("removeRow(int)"));
    qtc_process_events();
    CHECK(qtc_alive(label));
    CHECK_EQ(qtc_call(split, S("count")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 1);
    /* ... and an owner that keeps a widget by a plain pointer refuses one still in such a layout
       (a tab widget's own child would not even move: the layout could delete it under the tab widget) */
    qtc_obj tabs = make("QTabWidget");
    qtc_obj inner = make_child("QLabel", tabs);
    qtc_arg_obj(inner);
    CHECK_EQ(qtc_call(form, S("addRow(QWidget*)")), QTC_OK);
    qtc_arg_obj(inner);
    qtc_arg_int(1);   /* Qt::TopRightCorner */
    CHECK_EQ(qtc_call(tabs, S("setCornerWidget")), QTC_ERR_STATE);
    /* a layout given a parent widget keeps its widgets when they move into that widget */
    qtc_obj box = make("QVBoxLayout");
    qtc_obj b1 = make("QPushButton");
    qtc_arg_obj(b1);
    CHECK_EQ(qtc_call(box, S("addWidget(QWidget*)")), QTC_OK);
    qtc_obj host = make("QWidget");
    qtc_arg_obj(box);
    CHECK_EQ(qtc_call(host, S("setLayout")), QTC_OK);
    CHECK_EQ(qtc_call(box, S("count")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 1);

    /* insertions Qt makes in quadratic time */
    qtc_obj slm = make("QStringListModel");
    qtc_arg_int(0);
    qtc_arg_int(100);
    CHECK_EQ(qtc_call(slm, S("insertRows(int,int)")), QTC_OK);
    qtc_arg_int(100);
    qtc_arg_int(1 << 20);
    CHECK_EQ(qtc_call(slm, S("insertRows(int,int)")), QTC_ERR_LIMIT);   /* one row at a time, in the middle */
    qtc_arg_int(0);
    qtc_arg_int(1 << 16);
    CHECK_EQ(qtc_call(slm, S("insertRows(int,int)")), QTC_OK);          /* at the front: cheap */
    qtc_obj idp = make("QIdentityProxyModel");
    qtc_arg_obj(slm);
    CHECK_EQ(qtc_call(idp, S("setSourceModel")), QTC_OK);
    qtc_arg_int(5);
    qtc_arg_int(1 << 16);
    CHECK_EQ(qtc_call(idp, S("insertRows(int,int)")), QTC_ERR_LIMIT);   /* through a proxy too */
    qtc_obj table = make("QTableWidget");
    qtc_arg_int(1 << 16);
    CHECK_EQ(qtc_call(table, S("setRowCount")), QTC_OK);
    qtc_arg_int(4);
    CHECK_EQ(qtc_call(table, S("setColumnCount")), QTC_ERR_LIMIT);      /* rows x rows x columns */
    qtc_obj sim = make("QStandardItemModel");
    qtc_arg_int(1 << 16);
    CHECK_EQ(qtc_call(sim, S("setRowCount")), QTC_OK);
    qtc_val labels = qtc_val_from_json(S("[\"a\", \"b\", \"c\"]"));
    qtc_arg_val(labels);
    CHECK_EQ(qtc_call(sim, S("setHorizontalHeaderLabels")), QTC_ERR_LIMIT);
    qtc_val_free(labels);
    /* count() with a regular expression counts overlapping matches: size x size steps */
    qtc_arg_int(10000);
    qtc_arg_str(S("x"));
    qtc_val text = qtc_val_new(S("QString"));   /* QString(qsizetype, QChar) */
    CHECK(text != 0);
    if (text) {
        qtc_arg_str(S(".*"));
        qtc_val re = qtc_val_new(S("QRegularExpression"));
        qtc_arg_val(re);
        CHECK_EQ(qtc_call(text, S("count(QRegularExpression)")), QTC_ERR_LIMIT);
        qtc_val_free(re);
        qtc_val_free(text);
    }
    /* a grid layout lays out from a rows x columns array it clears on every pass */
    qtc_obj grid = make("QGridLayout");
    qtc_arg_int(4096);
    qtc_arg_int(2);   /* Qt::Vertical */
    CHECK_EQ(qtc_call(grid, S("setDefaultPositioning")), QTC_OK);
    qtc_arg_int(65535);
    qtc_arg_int(1);
    CHECK_EQ(qtc_call(grid, S("setColumnStretch")), QTC_ERR_LIMIT);
    qtc_arg_int(255);
    qtc_arg_int(1);
    CHECK_EQ(qtc_call(grid, S("setColumnStretch")), QTC_OK);
    CHECK_EQ(qtc_call(grid, S("maximumSize")), QTC_OK);

    /* images and windows stay within the allocation limit: Qt allocates them, the system kills
       the process when they are touched */
    qtc_arg_int(100000);
    qtc_arg_int(100000);
    qtc_arg_int(5);   /* QImage::Format_ARGB32 */
    CHECK_EQ(qtc_val_new(S("QImage")), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_LIMIT);
    qtc_arg_int(64);
    qtc_arg_int(64);
    qtc_arg_int(5);
    qtc_val small = qtc_val_new(S("QImage"));
    CHECK(small != 0);
    qtc_arg_int(1 << 20);
    qtc_arg_int(1 << 20);
    CHECK_EQ(qtc_call(small, S("scaled(int,int)")), QTC_ERR_LIMIT);
    qtc_arg_int(128);
    qtc_arg_int(128);
    CHECK_EQ(qtc_call(small, S("scaled(int,int)")), QTC_OK);
    qtc_val_free(small);
    qtc_obj win = make("QWidget");
    qtc_arg_int(16777215);
    qtc_arg_int(1000);
    CHECK_EQ(qtc_call(win, S("resize(int,int)")), QTC_ERR_LIMIT);   /* a window */
    qtc_obj child = make_child("QWidget", win);
    qtc_arg_int(16777215);
    qtc_arg_int(1000);
    CHECK_EQ(qtc_call(child, S("resize(int,int)")), QTC_OK);       /* a child paints into its window */
    qtc_arg_int(8);
    qtc_arg_int(8);
    CHECK_EQ(qtc_call(child, S("resize(int,int)")), QTC_OK);
    qtc_arg_int(1 << 20);
    qtc_arg_int(1 << 20);
    CHECK_EQ(qtc_call(win, S("setMinimumSize(int,int)")), QTC_ERR_LIMIT);
    qtc_destroy(win);

    /* Qt's own deletions: a held widget closed with Qt::WA_DeleteOnClose; its holder lets go first */
    qtc_obj tw2 = make("QTabWidget");
    qtc_obj cw = make("QPushButton");
    qtc_arg_obj(cw);
    CHECK_EQ(qtc_call(tw2, S("setCornerWidget")), QTC_OK);
    qtc_arg_int(55);   /* Qt::WA_DeleteOnClose */
    qtc_arg_bool(1);
    CHECK_EQ(qtc_call(cw, S("setAttribute")), QTC_OK);
    CHECK_EQ(qtc_call(cw, S("close")), QTC_OK);
    qtc_process_events();
    CHECK(!qtc_alive(cw));
    CHECK_EQ(call_obj(tw2, "cornerWidget"), 0);
    CHECK_EQ(qtc_call(tw2, S("show")), QTC_OK);
    qtc_process_events();
    qtc_destroy(tw2);
    if (qtc_class_exists(S("QChartView"))) {
        qtc_obj view = make("QChartView");
        qtc_obj ch = make("QChart");
        qtc_arg_obj(ch);
        CHECK_EQ(qtc_call(view, S("setChart")), QTC_OK);
        qtc_arg_int(55);
        qtc_arg_bool(1);
        CHECK_EQ(qtc_call(ch, S("setAttribute")), QTC_OK);
        CHECK_EQ(qtc_call(ch, S("close")), QTC_OK);
        qtc_process_events();
        CHECK(!qtc_alive(ch));
        qtc_obj now = call_obj(view, "chart");   /* a fresh chart, not the closed one */
        CHECK(now != 0 && now != ch);
        qtc_obj vscene = call_obj(view, "scene");   /* the view keeps its chart in its own scene */
        CHECK(vscene != 0);
        CHECK_EQ(qtc_call(vscene, S("clear")), QTC_ERR_STATE);
        qtc_arg_obj(now);                            /* another view would move it into its own scene */
        CHECK_EQ(qtc_new(S("QChartView")), 0);
        CHECK_EQ(qtc_last_error(), QTC_ERR_STATE);
        qtc_obj lone = make("QAreaSeries");          /* no upper series: Qt crashes animating it */
        qtc_arg_obj(lone);
        CHECK_EQ(qtc_call(now, S("addSeries")), QTC_ERR_STATE);
        qtc_destroy(lone);
        qtc_destroy(view);
    }

    /* a splitter makes its own handles (one made apart would be taken in as a content widget,
       and replaceWidget would then take it out); a handle reads its splitter on every size
       hint, so it stays in it */
    qtc_obj spl = make("QSplitter");
    qtc_arg_int(1);
    qtc_arg_obj(spl);
    CHECK_EQ(qtc_new(S("QSplitterHandle")), 0);
    make_child("QLabel", spl);
    make_child("QLabel", spl);
    qtc_arg_int(1);
    CHECK_EQ(qtc_call(spl, S("handle")), QTC_OK);
    qtc_obj hdl = qtc_ret_obj();
    CHECK(hdl != 0);
    qtc_obj elsewhere = make("QWidget");
    qtc_arg_obj(elsewhere);
    CHECK_EQ(qtc_call(hdl, S("setParent(QWidget*)")), QTC_ERR_STATE);
    qtc_arg_int(0);
    qtc_arg_obj(hdl);
    CHECK_EQ(qtc_call(spl, S("replaceWidget")), QTC_ERR_STATE);
    CHECK_EQ(qtc_call(hdl, S("sizeHint")), QTC_OK);
    qtc_destroy(elsewhere);
    qtc_destroy(spl);
    if (qtc_class_exists(S("QWebChannel"))) {
        qtc_obj chan = make("QWebChannel");
        qtc_arg_null();
        CHECK_EQ(qtc_call(chan, S("connectTo")), QTC_ERR_BAD_ARGS);   /* published to later */
        qtc_destroy(chan);
    }

    /* a chart model mapper writes its series' changes into its model without checking it */
    if (qtc_class_exists(S("QVXYModelMapper"))) {
        qtc_obj mapper = make("QVXYModelMapper");
        qtc_obj line = make("QLineSeries");
        qtc_arg_obj(line);
        CHECK_EQ(qtc_call(mapper, S("setSeries")), QTC_ERR_STATE);   /* no model yet */
        qtc_obj tmodel = make("QStandardItemModel");
        qtc_arg_obj(tmodel);
        CHECK_EQ(qtc_call(mapper, S("setModel")), QTC_OK);
        qtc_arg_obj(line);
        CHECK_EQ(qtc_call(mapper, S("setSeries")), QTC_OK);
        qtc_arg_null();
        CHECK_EQ(qtc_call(mapper, S("setModel")), QTC_ERR_STATE);
        CHECK_EQ(qtc_destroy(tmodel), QTC_OK);   /* the mapper gets an empty model of its own first */
        qtc_process_events();
        CHECK(call_obj(mapper, "model") != 0);
        qtc_arg_double(1);
        qtc_arg_double(2);
        CHECK_EQ(qtc_call(line, S("append")), QTC_OK);
        qtc_arg_null();
        CHECK_EQ(qtc_call(mapper, S("setSeries")), QTC_ERR_BAD_ARGS);   /* Qt would keep pointing at it */
        qtc_destroy(mapper);
        qtc_destroy(line);
    }

    /* a focus frame maps from its widget's parent: the widget stays put while a frame watches it */
    {
        qtc_obj ffp = make("QWidget");
        qtc_obj ffw = make_child("QLineEdit", ffp);
        qtc_obj ff = make_child("QFocusFrame", ffp);
        qtc_arg_obj(ffw);
        CHECK_EQ(qtc_call(ff, S("setWidget")), QTC_OK);
        qtc_arg_null();
        CHECK_EQ(qtc_call(ffw, S("setParent(QWidget*)")), QTC_ERR_STATE);
        qtc_obj wact = make_child("QWidgetAction", ffp);
        qtc_arg_obj(ffw);
        CHECK_EQ(qtc_call(wact, S("setDefaultWidget")), QTC_ERR_STATE);
        qtc_arg_null();
        CHECK_EQ(qtc_call(ff, S("setWidget")), QTC_OK);
        qtc_arg_null();
        CHECK_EQ(qtc_call(ffw, S("setParent(QWidget*)")), QTC_OK);
        qtc_destroy(ffw);
        qtc_destroy(ffp);
    }
    /* Qt takes a widget action's widget out of a tool bar itself (removeAction, the tool bar
       being destroyed with its window): a frame watching that widget lets go first */
    {
        qtc_obj mw = make("QMainWindow");
        qtc_arg_str(S("tools"));
        CHECK_EQ(qtc_call(mw, S("addToolBar(QString)")), QTC_OK);
        qtc_obj tb = qtc_ret_obj();
        qtc_obj wlbl = make("QLabel");
        qtc_obj wa = make_child("QWidgetAction", mw);
        qtc_arg_obj(wlbl);
        CHECK_EQ(qtc_call(wa, S("setDefaultWidget")), QTC_OK);
        qtc_arg_obj(wa);
        CHECK_EQ(qtc_call(tb, S("addAction(QAction*)")), QTC_OK);
        qtc_obj ff = make_child("QFocusFrame", mw);
        qtc_arg_obj(wlbl);
        CHECK_EQ(qtc_call(ff, S("setWidget")), QTC_OK);
        qtc_arg_obj(wa);
        CHECK_EQ(qtc_call(tb, S("removeAction")), QTC_OK);
        CHECK_EQ(qtc_call(ff, S("widget")), QTC_OK);
        CHECK(qtc_ret_obj() == 0);
        qtc_arg_obj(wa);
        CHECK_EQ(qtc_call(tb, S("addAction(QAction*)")), QTC_OK);
        qtc_arg_obj(wlbl);
        CHECK_EQ(qtc_call(ff, S("setWidget")), QTC_OK);
        CHECK_EQ(qtc_destroy(mw), QTC_OK);   /* the tool bar goes before the frame */
        qtc_process_events();
        CHECK(!qtc_alive(ff));
    }
    /* a window given new flags after it was made, then put in a parent before being shown again,
       keeps a native window Qt took for the native parent of what goes in it (a QOpenGLWidget
       placed there crashed Qt): it loses that window */
    {
        qtc_obj sw = make("QWidget");
        qtc_arg_int(2); /* Qt::WindowMaximized */
        CHECK_EQ(qtc_call(sw, S("setWindowState")), QTC_OK);
        qtc_arg_null();
        qtc_arg_int(0xd); /* Qt::ToolTip */
        CHECK_EQ(qtc_call(sw, S("setParent(QWidget*,Qt::WindowFlags)")), QTC_OK);
        qtc_obj stack = make("QStackedWidget");
        qtc_arg_obj(sw);
        CHECK_EQ(qtc_call(stack, S("addWidget")), QTC_OK);
        CHECK_EQ(qtc_call(sw, S("windowHandle")), QTC_OK);
        CHECK(qtc_ret_obj() == 0);
        if (qtc_class_exists(S("QOpenGLWidget"))) {
            qtc_obj glw = make_child("QOpenGLWidget", sw);
            CHECK(glw != 0);
        }
        CHECK_EQ(qtc_call(stack, S("show")), QTC_OK);
        qtc_process_events();
        CHECK_EQ(qtc_destroy(stack), QTC_OK);
    }
    /* a widget embedded in a graphics scene stays with its proxy, which deletes it with itself
       (QGraphicsScene::clear): it is not placed elsewhere meanwhile */
    {
        qtc_obj scene = make("QGraphicsScene");
        qtc_obj ew = make("QLabel");
        qtc_arg_obj(ew);
        CHECK_EQ(qtc_call(scene, S("addWidget(QWidget*)")), QTC_OK);
        qtc_obj px = qtc_ret_obj();
        CHECK(px != 0);
        qtc_obj colv = make("QColumnView");
        qtc_arg_obj(ew);
        CHECK_EQ(qtc_call(colv, S("setPreviewWidget")), QTC_ERR_STATE);
        qtc_obj host = make("QWidget");
        qtc_arg_obj(host);
        CHECK_EQ(qtc_call(ew, S("setParent(QWidget*)")), QTC_ERR_STATE);
        CHECK_EQ(qtc_call(scene, S("clear")), QTC_OK);
        CHECK(!qtc_alive(ew));
        CHECK_EQ(qtc_call(colv, S("previewWidget")), QTC_OK);
        /* replacing the viewport of the preview column Qt made (it holds the preview widget)
           leaves the view a preview widget of its own first */
        qtc_obj pw = make("QWidget");
        qtc_arg_obj(pw);
        CHECK_EQ(qtc_call(colv, S("setPreviewWidget")), QTC_OK);
        CHECK_EQ(qtc_call(pw, S("parentWidget")), QTC_OK);
        qtc_obj pvp = qtc_ret_obj();
        CHECK_EQ(qtc_call(pvp, S("parentWidget")), QTC_OK);
        qtc_obj pcol = qtc_ret_obj();
        qtc_obj nvp = make("QWidget");
        qtc_arg_obj(nvp);
        CHECK_EQ(qtc_call(pcol, S("setViewport")), QTC_OK);
        CHECK(!qtc_alive(pw));
        CHECK_EQ(qtc_call(colv, S("previewWidget")), QTC_OK);
        CHECK(qtc_ret_obj() != 0);
        qtc_process_events();
        /* a widget of a class qtc does not bind, inside another widget, is a part Qt made for
           itself, whenever it made it: the popup container a combo box makes on demand */
        qtc_obj cbx = make("QComboBox");
        CHECK_EQ(qtc_call(cbx, S("view")), QTC_OK);
        qtc_obj cview = qtc_ret_obj();
        CHECK_EQ(qtc_call(cview, S("parentWidget")), QTC_OK);
        qtc_obj ccont = qtc_ret_obj();
        CHECK(ccont != 0);
        CHECK_EQ(qtc_destroy(ccont), QTC_ERR_STATE);
        qtc_destroy(cbx);
        qtc_destroy(host);
        qtc_destroy(colv);
        qtc_destroy(scene);
        qtc_process_events();
    }
    /* a hidden menu's cached action rectangles outlived clear(): actionAt handed out a deleted
       action */
    {
        qtc_obj menu = make("QMenu");
        for (int i = 0; i < 3; i++) {
            qtc_arg_str(S("item"));
            CHECK_EQ(qtc_call(menu, S("addAction(QString)")), QTC_OK);
        }
        CHECK_EQ(qtc_call(menu, S("sizeHint")), QTC_OK);
        CHECK_EQ(qtc_call(menu, S("clear")), QTC_OK);
        qtc_val pt = qtc_val_new(S("QPoint"));
        qtc_arg_int(5);
        CHECK_EQ(qtc_call(pt, S("setX")), QTC_OK);
        qtc_arg_int(30);
        CHECK_EQ(qtc_call(pt, S("setY")), QTC_OK);
        qtc_arg_val(pt);
        CHECK_EQ(qtc_call(menu, S("actionAt")), QTC_OK);
        CHECK(qtc_ret_obj() == 0);
        qtc_val_free(pt);
        qtc_destroy(menu);
    }
    /* Qt's NMEA sources read their device while it says bytes are available; open without
       reading it says so while every read fails, and the simulation reader spun forever */
    if (qtc_class_exists(S("QNmeaPositionInfoSource"))) {
        qtc_obj nbuf = make("QBuffer");
        qtc_arg_int(2); /* WriteOnly */
        CHECK_EQ(qtc_call(nbuf, S("open")), QTC_OK);
        qtc_arg_str(S("abc"));
        CHECK_EQ(qtc_call(nbuf, S("write(QByteArray)")), QTC_OK);
        qtc_arg_int(2); /* SimulationMode */
        qtc_obj nsrc = qtc_new(S("QNmeaPositionInfoSource"));
        CHECK(nsrc != 0);
        qtc_arg_obj(nbuf);
        CHECK_EQ(qtc_call(nsrc, S("setDevice")), QTC_ERR_STATE);
        CHECK_EQ(qtc_call(nbuf, S("close")), QTC_OK);
        qtc_arg_int(3); /* ReadWrite */
        CHECK_EQ(qtc_call(nbuf, S("open")), QTC_OK);
        qtc_arg_obj(nbuf);
        CHECK_EQ(qtc_call(nsrc, S("setDevice")), QTC_OK);
        CHECK_EQ(qtc_call(nbuf, S("close")), QTC_OK);
        qtc_arg_int(2);
        CHECK_EQ(qtc_call(nbuf, S("open")), QTC_ERR_STATE);
        qtc_arg_int(1); /* ReadOnly */
        CHECK_EQ(qtc_call(nbuf, S("open")), QTC_OK);
        CHECK_EQ(qtc_call(nsrc, S("startUpdates")), QTC_OK);
        qtc_process_events();
        qtc_destroy(nsrc);
        qtc_process_events();
        qtc_destroy(nbuf);
    }
    /* an item view keeps its index widgets by pointer and shows them on each layout: one
       destroyed is let go by the view first, and one is not moved elsewhere */
    {
        qtc_obj tw = make("QTableWidget");
        qtc_arg_int(4);
        CHECK_EQ(qtc_call(tw, S("setRowCount")), QTC_OK);
        qtc_arg_int(4);
        CHECK_EQ(qtc_call(tw, S("setColumnCount")), QTC_OK);
        qtc_obj cw = make("QLabel");
        qtc_arg_int(1);
        qtc_arg_int(0);
        qtc_arg_obj(cw);
        CHECK_EQ(qtc_call(tw, S("setCellWidget")), QTC_OK);
        qtc_obj fhost = make("QWidget");
        qtc_obj fl = make_child("QFormLayout", fhost);
        qtc_arg_obj(cw);
        CHECK_EQ(qtc_call(fl, S("addRow(QWidget*)")), QTC_ERR_STATE);
        CHECK_EQ(qtc_destroy(cw), QTC_OK);
        qtc_process_events();
        qtc_arg_int(1);
        qtc_arg_int(0);
        CHECK_EQ(qtc_call(tw, S("cellWidget")), QTC_OK);
        CHECK(qtc_ret_obj() == 0);
        qtc_arg_str(S("QLabel { color: red }"));
        CHECK_EQ(qtc_call(qtc_app(), S("setStyleSheet")), QTC_OK);
        CHECK_EQ(qtc_call(tw, S("show")), QTC_OK);
        qtc_process_events();
        qtc_arg_str(S(""));
        CHECK_EQ(qtc_call(qtc_app(), S("setStyleSheet")), QTC_OK);
        qtc_destroy(fhost);
        qtc_destroy(tw);
        qtc_process_events();
    }
    /* a style that dies with the object destroyed (its child) is let go by the graphics
       widgets and scenes using it first (they keep a plain pointer) */
    {
        qtc_obj sowner = make("QObject");
        qtc_obj cst = make("QCommonStyle");
        qtc_arg_obj(sowner);
        CHECK_EQ(qtc_call(cst, S("setParent")), QTC_OK);
        qtc_obj gw = make("QGraphicsWidget");
        qtc_arg_obj(cst);
        CHECK_EQ(qtc_call(gw, S("setStyle")), QTC_OK);
        CHECK_EQ(qtc_destroy(sowner), QTC_OK);
        qtc_process_events();
        CHECK(!qtc_alive(cst));
        CHECK_EQ(qtc_call(gw, S("style")), QTC_OK);
        CHECK(qtc_ret_obj() != cst);
        qtc_destroy(gw);
    }
    /* the clipper merges coincident points through a k-d tree one level deeper per copy */
    {
        qtc_val cp = qtc_val_new(S("QPainterPath"));
        int added = 0;
        for (int i = 0; i < 1500; i++) {
            qtc_arg_double(0);
            qtc_arg_double(0);
            added += qtc_call(cp, S("lineTo(qreal,qreal)")) == QTC_OK;
            qtc_arg_double(1);
            qtc_arg_double(1);
            added += qtc_call(cp, S("lineTo(qreal,qreal)")) == QTC_OK;
        }
        CHECK(added == 3000);
        CHECK(qtc_call(cp, S("simplified")) != QTC_OK);
        qtc_val_free(cp);
    }
    /* QWizard takes its side widget in only when it next restyles, in the middle of
       QApplication::setStyle: a splitter the widget left then deleted a handle Qt was restyling */
    {
        qtc_obj spl = make("QSplitter");
        qtc_obj sw2 = make("QWidget");
        qtc_arg_int(0);
        qtc_arg_obj(sw2);
        CHECK_EQ(qtc_call(spl, S("insertWidget")), QTC_OK);
        qtc_obj wiz = make("QWizard");
        qtc_arg_obj(sw2);
        CHECK_EQ(qtc_call(wiz, S("setSideWidget")), QTC_ERR_STATE);
        qtc_arg_null();
        CHECK_EQ(qtc_call(sw2, S("setParent(QWidget*)")), QTC_OK);
        qtc_arg_obj(sw2);
        CHECK_EQ(qtc_call(wiz, S("setSideWidget")), QTC_OK);
        qtc_destroy(wiz);
        qtc_destroy(spl);
        qtc_process_events();
    }
    /* a dock widget tabified with itself (after floating out of no area) left a tab group no
       close reached: QApplication::closeAllWindows looped forever */
    {
        qtc_obj dmw = make("QMainWindow");
        qtc_obj dock = make_child("QDockWidget", dmw);
        qtc_arg_obj(dock);
        CHECK_EQ(qtc_call(dmw, S("removeDockWidget")), QTC_OK);
        qtc_arg_int(8); /* Qt::BottomDockWidgetArea */
        CHECK_EQ(qtc_call(dock, S("setDockLocation")), QTC_OK);
        qtc_arg_int(0);
        CHECK_EQ(qtc_call(dock, S("setDockLocation")), QTC_OK);
        qtc_arg_obj(dock);
        qtc_arg_obj(dock);
        CHECK_EQ(qtc_call(dmw, S("tabifyDockWidget")), QTC_ERR_BAD_ARGS);
        qtc_arg_obj(dock);
        qtc_arg_obj(dock);
        qtc_arg_int(1);
        CHECK_EQ(qtc_call(dmw, S("splitDockWidget")), QTC_ERR_BAD_ARGS);
        qtc_destroy(dmw);
        qtc_process_events();
    }
    /* attributes Qt keeps itself are not set from outside: a window marked visible without a
       native window crashed QApplication::closeAllWindows */
    {
        qtc_obj aw = make("QWidget");
        qtc_arg_int(15); /* Qt::WA_WState_Visible */
        qtc_arg_bool(true);
        CHECK_EQ(qtc_call(aw, S("setAttribute")), QTC_ERR_OUT_OF_RANGE);
        qtc_arg_int(60); /* Qt::WA_WState_Created */
        qtc_arg_bool(true);
        CHECK_EQ(qtc_call(aw, S("setAttribute")), QTC_ERR_OUT_OF_RANGE);
        qtc_arg_int(55); /* Qt::WA_DeleteOnClose */
        qtc_arg_bool(true);
        CHECK_EQ(qtc_call(aw, S("setAttribute")), QTC_OK);
        qtc_arg_int(15);
        CHECK_EQ(qtc_call(aw, S("testAttribute")), QTC_OK);
        CHECK(!qtc_ret_bool());
        qtc_destroy(aw);
    }
    /* Qt's path clipper crashes on a non-finite point; Qt keeps them out of a path everywhere
       but setElementPositionAt */
    {
        qtc_val path = qtc_val_new(S("QPainterPath"));
        qtc_val rect = qtc_val_new(S("QRectF"));
        qtc_arg_double(0);
        qtc_arg_double(0);
        qtc_arg_double(10);
        qtc_arg_double(10);
        CHECK_EQ(qtc_call(rect, S("setRect")), QTC_OK);
        qtc_arg_val(rect);
        CHECK_EQ(qtc_call(path, S("addRect(QRectF)")), QTC_OK);
        qtc_arg_int(3);
        qtc_arg_double(NAN);
        qtc_arg_double(0);
        CHECK_EQ(qtc_call(path, S("setElementPositionAt")), QTC_ERR_OUT_OF_RANGE);
        qtc_arg_int(3);
        qtc_arg_double(2);
        qtc_arg_double(INFINITY);
        CHECK_EQ(qtc_call(path, S("setElementPositionAt")), QTC_ERR_OUT_OF_RANGE);
        CHECK_EQ(qtc_call(path, S("simplified")), QTC_OK);
        qtc_val_free(rect);
        qtc_val_free(path);
    }
    /* a window container uses its window whenever one of its parents changes: the window
       stays with it */
    {
        qtc_obj cw = make("QWindow");
        qtc_arg_obj(cw);
        CHECK_EQ(qtc_call_static(S("QWidget"), S("createWindowContainer")), QTC_OK);
        qtc_obj cont = qtc_ret_obj();
        CHECK(cont != 0);
        CHECK_EQ(qtc_destroy(cw), QTC_ERR_STATE);
        qtc_arg_null();
        CHECK_EQ(qtc_call(cw, S("setParent")), QTC_ERR_STATE);
        qtc_obj host = make("QWidget");
        qtc_arg_obj(host);
        CHECK_EQ(qtc_call(cont, S("setParent(QWidget*)")), QTC_OK);   /* the container moves freely */
        qtc_destroy(host);                                             /* and takes its window along */
        qtc_process_events();
        CHECK(!qtc_alive(cw));
    }
    /* a button box deletes the buttons in its lists on clear(), wherever they have moved since:
       a button stays in its box until it leaves */
    {
        qtc_obj box = make("QDialogButtonBox");
        qtc_obj pb = make("QPushButton");
        qtc_arg_obj(pb);
        qtc_arg_int(0);
        CHECK_EQ(qtc_call(box, S("addButton(QAbstractButton*,QDialogButtonBox::ButtonRole)")), QTC_OK);
        qtc_obj wz2 = make("QWizard");
        qtc_arg_int(6);   /* CustomButton1 */
        qtc_arg_obj(pb);
        CHECK_EQ(qtc_call(wz2, S("setButton")), QTC_ERR_STATE);
        qtc_arg_obj(pb);
        CHECK_EQ(qtc_call(box, S("removeButton")), QTC_OK);
        qtc_arg_int(6);
        qtc_arg_obj(pb);
        CHECK_EQ(qtc_call(wz2, S("setButton")), QTC_OK);
        CHECK_EQ(qtc_call(box, S("clear")), QTC_OK);
        qtc_arg_int(1);
        CHECK_EQ(qtc_call(wz2, S("setWizardStyle")), QTC_OK);
        CHECK(qtc_alive(pb));
        qtc_destroy(wz2);
        qtc_destroy(box);
    }
    /* a focused widget moved into a window of its own: the old parent forgets it as its focus child */
    {
        qtc_obj sbar = make("QStatusBar");
        qtc_obj fwid = make("QLineEdit");
        qtc_arg_obj(fwid);
        CHECK_EQ(qtc_call(sbar, S("addPermanentWidget")), QTC_OK);
        CHECK_EQ(qtc_call(fwid, S("setFocus()")), QTC_OK);
        qtc_arg_null();
        qtc_arg_int(13);   /* Qt::ToolTip, a window type */
        CHECK_EQ(qtc_call(fwid, S("setParent(QWidget*,Qt::WindowFlags)")), QTC_OK);
        qtc_destroy(fwid);
        qtc_process_events();
        qtc_obj mwin = make("QMainWindow");
        qtc_arg_obj(sbar);
        CHECK_EQ(qtc_call(mwin, S("setStatusBar")), QTC_OK);
        qtc_destroy(mwin);
    }
    /* a disk cache deletes every file ending in .d under its directory */
    if (qtc_class_exists(S("QNetworkDiskCache"))) {
        qtc_obj dcache = make("QNetworkDiskCache");
        qtc_arg_str(S("/"));
        CHECK_EQ(qtc_call(dcache, S("setCacheDirectory")), QTC_ERR_BAD_ARGS);
        qtc_destroy(dcache);
    }
    /* QWizard keeps its side widget by a plain pointer: it lets go of it first */
    {
        qtc_obj wz = make("QWizard");
        qtc_obj side = make("QLabel");
        qtc_arg_obj(side);
        CHECK_EQ(qtc_call(wz, S("setSideWidget")), QTC_OK);
        CHECK_EQ(qtc_destroy(side), QTC_OK);
        qtc_process_events();
        CHECK_EQ(qtc_call(wz, S("sideWidget")), QTC_OK);
        CHECK_EQ(qtc_ret_obj(), 0);
        qtc_destroy(wz);
    }
    /* an ellipse region whose far edge wraps takes a polygon from an empty path */
    qtc_arg_int(1048576); qtc_arg_int(0); qtc_arg_int(1); qtc_arg_int(-2147483647 - 1); qtc_arg_int(1);
    CHECK_EQ(qtc_val_new(S("QRegion")), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_OUT_OF_RANGE);

    /* a file dialog's proxy model keeps the dialog's model as its source */
    qtc_obj fdlg = make("QFileDialog");
    qtc_obj fproxy = make("QSortFilterProxyModel");
    qtc_arg_obj(fproxy);
    CHECK_EQ(qtc_call(fdlg, S("setProxyModel")), QTC_OK);
    qtc_obj fother = make("QStandardItemModel");
    qtc_arg_obj(fother);
    CHECK_EQ(qtc_call(fproxy, S("setSourceModel")), QTC_ERR_STATE);
    qtc_destroy(fdlg);
    qtc_destroy(fother);
    /* a combo box's popup must be a list, table or tree */
    qtc_obj cbox = make("QComboBox");
    qtc_obj colv = make("QColumnView");
    qtc_arg_obj(colv);
    CHECK_EQ(qtc_call(cbox, S("setView")), QTC_ERR_BAD_ARGS);
    qtc_destroy(colv);
    qtc_destroy(cbox);

    /* a date edit's popup deletes its old calendar first: setting the same one again uses it after that */
    qtc_obj dte = make("QDateTimeEdit");
    qtc_arg_bool(1);
    CHECK_EQ(qtc_call(dte, S("setCalendarPopup")), QTC_OK);
    qtc_obj cal = make("QCalendarWidget");
    qtc_arg_obj(cal);
    CHECK_EQ(qtc_call(dte, S("setCalendarWidget")), QTC_OK);
    qtc_arg_obj(cal);
    CHECK_EQ(qtc_call(dte, S("setCalendarWidget")), QTC_ERR_STATE);
    qtc_destroy(dte);
    /* merged cells of a rich text table: no rows or columns removed across them */
    qtc_obj tdoc = make("QTextDocument");
    qtc_arg_obj(tdoc);
    qtc_val tcur = qtc_val_new(S("QTextCursor"));
    qtc_arg_int(3);
    qtc_arg_int(3);
    CHECK_EQ(qtc_call(tcur, S("insertTable(int,int)")), QTC_OK);
    qtc_obj ttab = qtc_ret_obj();
    qtc_arg_int(1); qtc_arg_int(0); qtc_arg_int(2); qtc_arg_int(3);
    CHECK_EQ(qtc_call(ttab, S("mergeCells(int,int,int,int)")), QTC_OK);
    qtc_arg_int(3); qtc_arg_int(2);
    CHECK_EQ(qtc_call(ttab, S("resize")), QTC_ERR_STATE);
    qtc_arg_int(0); qtc_arg_int(1);
    CHECK_EQ(qtc_call(ttab, S("removeColumns")), QTC_ERR_STATE);
    qtc_arg_int(4); qtc_arg_int(3);
    CHECK_EQ(qtc_call(ttab, S("resize")), QTC_OK);   /* growing by rows is fine */
    /* columns added next to a cell merged across rows and columns break the table */
    qtc_arg_int(1);
    CHECK_EQ(qtc_call(ttab, S("appendColumns")), QTC_ERR_STATE);
    qtc_arg_int(0); qtc_arg_int(1);
    CHECK_EQ(qtc_call(ttab, S("insertColumns")), QTC_ERR_STATE);
    qtc_arg_int(4); qtc_arg_int(5);
    CHECK_EQ(qtc_call(ttab, S("resize")), QTC_ERR_STATE);
    qtc_arg_int(1);
    CHECK_EQ(qtc_call(ttab, S("appendRows")), QTC_OK);   /* rows are fine */
    qtc_val_free(tcur);
    qtc_destroy(tdoc);
    /* a negative stretch, or sizes adding up past int, can keep a layout from ever finishing */
    qtc_obj hbox = make("QHBoxLayout");
    qtc_arg_int(-5);
    CHECK_EQ(qtc_call(hbox, S("addStretch")), QTC_ERR_OUT_OF_RANGE);
    qtc_arg_int(2147483647);
    CHECK_EQ(qtc_call(hbox, S("addStretch")), QTC_ERR_OUT_OF_RANGE);
    qtc_arg_int(65535);
    CHECK_EQ(qtc_call(hbox, S("addStretch")), QTC_OK);
    qtc_arg_int(2147483647);
    CHECK_EQ(qtc_call(hbox, S("addSpacing")), QTC_ERR_OUT_OF_RANGE);
    qtc_arg_int(-3);
    CHECK_EQ(qtc_call(hbox, S("addSpacing")), QTC_ERR_OUT_OF_RANGE);
    qtc_arg_int(2147483647);
    CHECK_EQ(qtc_call(hbox, S("setSpacing")), QTC_ERR_OUT_OF_RANGE);
    qtc_arg_int(-1);
    CHECK_EQ(qtc_call(hbox, S("setSpacing")), QTC_OK);   /* the style's spacing */
    qtc_arg_int(2147483647); qtc_arg_int(0); qtc_arg_int(0); qtc_arg_int(0);
    CHECK_EQ(qtc_call(hbox, S("setContentsMargins(int,int,int,int)")), QTC_ERR_OUT_OF_RANGE);
    qtc_destroy(hbox);
    {   /* many spacers of the largest size add up past what the engine counts */
        qtc_obj lw = make("QWidget");
        qtc_obj lay = make_child("QHBoxLayout", lw);
        int refused = 0, added = 0;
        for (int i = 0; i < 140; i++) {
            qtc_arg_int(16777215);
            added += qtc_call(lay, S("addSpacing")) == QTC_OK;
        }
        CHECK_EQ(added, 140);
        qtc_arg_int(0); qtc_arg_int(0); qtc_arg_int(400); qtc_arg_int(300);
        qtc_val rect = qtc_val_new(S("QRect"));
        qtc_arg_val(rect);
        refused = qtc_call(lay, S("setGeometry(QRect)")) == QTC_ERR_LIMIT;
        qtc_val_free(rect);
        CHECK(refused);
        CHECK_EQ(qtc_call(lw, S("show")), QTC_ERR_LIMIT);
        qtc_arg_int(500); qtc_arg_int(300);
        CHECK_EQ(qtc_call(lw, S("resize(int,int)")), QTC_OK);   /* the resize is not laid out */
        pump(20);
        CHECK(qtc_alive(lw));
        qtc_destroy(lw);
    }

    /* a corner near INT_MAX wraps the far edge, and painting the window then crashes */
    qtc_obj farwin = make("QWidget");
    qtc_arg_int(2147483637); qtc_arg_int(2147483637); qtc_arg_int(100); qtc_arg_int(100);
    CHECK_EQ(qtc_call(farwin, S("setGeometry(int,int,int,int)")), QTC_ERR_OUT_OF_RANGE);
    qtc_arg_int(16777215); qtc_arg_int(-16777215);
    CHECK_EQ(qtc_call(farwin, S("move(int,int)")), QTC_OK);
    qtc_destroy(farwin);

    /* a window that grows past the allocation limit through its layout is not painted */
    qtc_obj bigwin = make("QWidget");
    qtc_obj biglay = make_child("QVBoxLayout", bigwin);
    CHECK_EQ(qtc_call(bigwin, S("show")), QTC_OK);
    pump(20);
    qtc_obj toplabel = make("QLabel");   /* no parent: a window, so its size is checked */
    qtc_arg_int(30000);
    qtc_arg_int(30000);
    CHECK_EQ(qtc_call(toplabel, S("setMinimumSize(int,int)")), QTC_ERR_LIMIT);
    qtc_destroy(toplabel);
    qtc_obj huge = make_child("QLabel", bigwin);
    qtc_arg_int(30000);
    qtc_arg_int(30000);
    CHECK_EQ(qtc_call(huge, S("setMinimumSize(int,int)")), QTC_OK);   /* not a window: allowed */
    qtc_arg_obj(huge);
    CHECK_EQ(qtc_call(biglay, S("addWidget(QWidget*)")), QTC_OK);
    pump(50);
    CHECK(qtc_alive(bigwin));
    qtc_destroy(bigwin);
    qtc_process_events();

    /* found in the same round */
    qtc_obj lcd = make("QLCDNumber");
    qtc_arg_int(0);
    CHECK_EQ(qtc_call(lcd, S("setDigitCount")), QTC_ERR_OUT_OF_RANGE);   /* display() then writes past the end */
    qtc_destroy(lcd);
    qtc_val pal = qtc_val_new(S("QPalette"));
    CHECK(pal != 0);
    qtc_arg_int(5);   /* QPalette::All */
    CHECK_EQ(qtc_call(pal, S("setCurrentColorGroup")), QTC_ERR_OUT_OF_RANGE);
    qtc_arg_int(2);   /* Inactive */
    CHECK_EQ(qtc_call(pal, S("setCurrentColorGroup")), QTC_OK);
    qtc_val_free(pal);
    qtc_obj page = make("QWizardPage");
    qtc_arg_int(9);   /* QWizard::Stretch */
    qtc_arg_str(S("x"));
    CHECK_EQ(qtc_call(page, S("setButtonText")), QTC_ERR_OUT_OF_RANGE);
    qtc_destroy(page);
    qtc_obj g2 = make("QGridLayout");
    qtc_arg_int(65535);
    CHECK_EQ(qtc_call(g2, S("rowStretch")), QTC_ERR_OUT_OF_RANGE);   /* read past the list */
    qtc_destroy(g2);
    qtc_obj wide = make("QStandardItemModel");
    qtc_arg_int(4096);
    CHECK_EQ(qtc_call(wide, S("setColumnCount")), QTC_OK);
    qtc_arg_int(0);
    qtc_arg_int(1 << 20);
    CHECK_EQ(qtc_call(wide, S("insertRows(int,int)")), QTC_ERR_LIMIT);   /* 4096 x 2^20 item pointers */
    qtc_destroy(wide);
    qtc_val bytes = qtc_val_from_json(S("\"abc\""));
    qtc_val_free(bytes);
    CHECK_EQ(qtc_call_static(S("QByteArray"), S("erase")), QTC_ERR_NOT_FOUND);   /* iterators are not bound */

    /* QMessageBox keeps plain pointers to its clicked, default and escape buttons */
    qtc_obj mb = make("QMessageBox");
    qtc_obj ok = make("QPushButton");
    qtc_arg_obj(ok);
    qtc_arg_int(0);   /* AcceptRole */
    CHECK_EQ(qtc_call(mb, S("addButton(QAbstractButton*,QMessageBox::ButtonRole)")), QTC_OK);
    qtc_arg_obj(ok);
    CHECK_EQ(qtc_call(mb, S("setDefaultButton(QPushButton*)")), QTC_OK);
    CHECK_EQ(qtc_call(ok, S("click")), QTC_OK);
    CHECK_EQ(call_obj(mb, "clickedButton"), ok);
    CHECK_EQ(qtc_destroy(ok), QTC_OK);   /* the box lets go of it first (removeButton) */
    qtc_process_events();
    CHECK_EQ(qtc_call(mb, S("clickedButton")), QTC_ERR_STATE);   /* Qt still points at it */
    CHECK_EQ(qtc_call(mb, S("defaultButton")), QTC_OK);
    CHECK_EQ(qtc_ret_obj(), 0);
    /* and its custom buttons in a list of plain pointers that buttonText() reads */
    qtc_arg_str(S("custom"));
    qtc_arg_int(0);
    CHECK_EQ(qtc_call(mb, S("addButton(QString,QMessageBox::ButtonRole)")), QTC_OK);
    qtc_obj custom = qtc_ret_obj();
    CHECK(custom != 0);
    CHECK_EQ(qtc_destroy(custom), QTC_OK);
    qtc_process_events();
    qtc_arg_int(0);
    CHECK_EQ(qtc_call(mb, S("buttonText(int)")), QTC_OK);
    qtc_arg_int(0);
    qtc_arg_str(S("x"));
    CHECK_EQ(qtc_call(mb, S("setButtonText(int,QString)")), QTC_OK);

    /* QMdiSubWindow makes a menu that is not its child a child widget; showing it then crashes */
    qtc_obj sub = make("QMdiSubWindow");
    qtc_obj loose = make("QMenu");
    qtc_arg_obj(loose);
    CHECK_EQ(qtc_call(sub, S("setSystemMenu")), QTC_ERR_STATE);
    qtc_obj own = make_child("QMenu", sub);
    qtc_arg_obj(own);
    CHECK_EQ(qtc_call(sub, S("setSystemMenu")), QTC_OK);
    CHECK_EQ(qtc_call(sub, S("showSystemMenu")), QTC_OK);
    qtc_call(own, S("hide"));

    /* Qt Charts: an axis is needed, and boundary lines of an area series in a chart keep their
       point label clipping */
    if (qtc_class_exists(S("QChart"))) {
        qtc_obj chart = make("QChart");
        qtc_obj area = make("QAreaSeries");
        qtc_obj upper = make("QLineSeries");
        qtc_arg_obj(upper);
        CHECK_EQ(qtc_call(area, S("setUpperSeries")), QTC_OK);
        qtc_arg_obj(area);
        CHECK_EQ(qtc_call(chart, S("addSeries")), QTC_OK);
        qtc_arg_null();
        CHECK_EQ(qtc_call(area, S("attachAxis")), QTC_ERR_BAD_ARGS);
        qtc_arg_bool(0);
        CHECK_EQ(qtc_call(upper, S("setPointLabelsClipping")), QTC_ERR_STATE);
        qtc_arg_bool(1);
        CHECK_EQ(qtc_call(upper, S("setPointLabelsClipping")), QTC_OK);   /* unchanged: fine */
        qtc_arg_obj(area);
        CHECK_EQ(qtc_call(chart, S("removeSeries")), QTC_OK);
        qtc_arg_bool(0);   /* the area's chart item lives on until the next deferred delete */
        CHECK_EQ(qtc_call(upper, S("setPointLabelsClipping")), QTC_ERR_STATE);
        qtc_obj other = make("QLineSeries");
        qtc_arg_bool(0);
        CHECK_EQ(qtc_call(other, S("setPointLabelsClipping")), QTC_OK);   /* not a boundary */
        qtc_arg_obj(other);
        CHECK_EQ(qtc_call(area, S("setLowerSeries")), QTC_ERR_STATE);
        qtc_destroy(other);
        qtc_destroy(chart);
        qtc_destroy(area);
        qtc_destroy(upper);

        /* removeAllSeries deletes an area series while its chart item lives on and follows the
           boundary lines; removeSeries and destroying it afterwards is safe */
        chart = make("QChart");
        area = make("QAreaSeries");
        upper = make("QLineSeries");
        qtc_arg_obj(upper);
        CHECK_EQ(qtc_call(area, S("setUpperSeries")), QTC_OK);
        qtc_arg_obj(area);
        CHECK_EQ(qtc_call(chart, S("addSeries")), QTC_OK);
        CHECK_EQ(qtc_call(chart, S("removeAllSeries")), QTC_ERR_STATE);
        qtc_arg_obj(area);
        CHECK_EQ(qtc_call(chart, S("removeSeries")), QTC_OK);
        CHECK_EQ(qtc_destroy(area), QTC_OK);
        qtc_process_events();
        CHECK_EQ(qtc_call(chart, S("removeAllSeries")), QTC_OK);
        qtc_arg_bool(1);
        CHECK_EQ(qtc_call(upper, S("setPointsVisible")), QTC_OK);
        qtc_destroy(chart);
        qtc_destroy(upper);

        /* point signals emitted by hand while the series is in a chart */
        chart = make("QChart");
        qtc_obj ls = make("QLineSeries");
        for (int i = 0; i < 3; i++) {
            qtc_arg_double(i);
            qtc_arg_double(i);
            CHECK_EQ(qtc_call(ls, S("append(qreal,qreal)")), QTC_OK);
        }
        qtc_arg_obj(ls);
        CHECK_EQ(qtc_call(chart, S("addSeries")), QTC_OK);
        qtc_arg_int(-1);
        CHECK_EQ(qtc_call(ls, S("pointAdded")), QTC_ERR_STATE);
        qtc_arg_int(5);
        CHECK_EQ(qtc_call(ls, S("pointReplaced")), QTC_ERR_OUT_OF_RANGE);
        qtc_arg_int(1);
        CHECK_EQ(qtc_call(ls, S("pointReplaced")), QTC_OK);
        qtc_destroy(chart);
        qtc_process_events();

        /* index + count wrapped past INT_MAX */
        qtc_arg_str(S("set"));
        qtc_obj bs = make("QBarSet");
        for (int i = 0; i < 3; i++) {
            qtc_arg_double(i);
            CHECK_EQ(qtc_call(bs, S("append(qreal)")), QTC_OK);
        }
        qtc_arg_int(1);
        qtc_arg_int(INT32_MAX);
        CHECK_EQ(qtc_call(bs, S("remove")), QTC_ERR_OUT_OF_RANGE);
        qtc_arg_int(1);
        qtc_arg_int(5);
        CHECK_EQ(qtc_call(bs, S("remove")), QTC_OK);
        CHECK_EQ(qtc_call(bs, S("count")), QTC_OK);
        CHECK_EQ(qtc_ret_int(), 1);
        qtc_destroy(bs);
        qtc_process_events();
    }
    /* scrolling by offsets past any widget size overflowed the backing store's arithmetic */
    {
        qtc_obj sw = make("QWidget");
        qtc_arg_int(INT32_MIN);
        qtc_arg_int(INT32_MIN);
        CHECK_EQ(qtc_call(sw, S("scroll")), QTC_ERR_OUT_OF_RANGE);
        qtc_arg_int(5);
        qtc_arg_int(-5);
        CHECK_EQ(qtc_call(sw, S("scroll")), QTC_OK);
        /* grab() with one negative size grabs from the given top left to the widget's bottom
           right in both directions: here 10^8 pixels wide, which Qt cannot allocate (and then
           paints into the null pixmap) */
        qtc_arg_int(-100000000);
        qtc_arg_int(0);
        qtc_arg_int(10);
        qtc_arg_int(-1);
        qtc_val gr = qtc_val_new(S("QRect"));
        qtc_arg_val(gr);
        CHECK_EQ(qtc_call(sw, S("grab")), QTC_ERR_LIMIT);
        qtc_val_free(gr);
        /* an empty rectangle that is not a null one still "intersects" the widget, and a graphics
           effect then draws into the null pixmap */
        qtc_obj eff = make("QGraphicsOpacityEffect");
        qtc_arg_obj(eff);
        CHECK_EQ(qtc_call(sw, S("setGraphicsEffect")), QTC_OK);
        qtc_arg_int(0);
        qtc_arg_int(1);
        qtc_arg_int(2);
        qtc_arg_int(0);
        gr = qtc_val_new(S("QRect"));
        qtc_arg_val(gr);
        CHECK_EQ(qtc_call(sw, S("grab")), QTC_ERR_BAD_ARGS);
        qtc_val_free(gr);
        qtc_arg_int(0);
        qtc_arg_int(0);
        qtc_arg_int(8);
        qtc_arg_int(8);
        gr = qtc_val_new(S("QRect"));
        qtc_arg_val(gr);
        CHECK_EQ(qtc_call(sw, S("grab")), QTC_OK);
        qtc_val_free(gr);
        qtc_destroy(sw);
    }
    /* a grid's row minimum heights count toward the sums the layout engine adds up in int (they
       wrapped and its space distribution never finished) */
    {
        qtc_obj gl = make("QGridLayout");
        qtc_arg_int(100);
        qtc_arg_int(INT32_MIN);
        CHECK_EQ(qtc_call(gl, S("setRowMinimumHeight")), QTC_ERR_OUT_OF_RANGE);
        for (int r = 0; r < 70; r++) {
            qtc_arg_int(r);
            qtc_arg_int(16777215);
            CHECK_EQ(qtc_call(gl, S("setRowMinimumHeight")), QTC_OK);
        }
        qtc_arg_int(0);
        qtc_arg_int(0);
        qtc_arg_int(100);
        qtc_arg_int(100);
        qtc_val gr = qtc_val_new(S("QRect"));
        qtc_arg_val(gr);
        CHECK_EQ(qtc_call(gl, S("setGeometry")), QTC_ERR_LIMIT);
        qtc_val_free(gr);
        qtc_destroy(gl);
    }
    /* a scroll area whose widget is also its viewport resizes each to the other without end */
    {
        qtc_obj sa = make("QScrollArea");
        qtc_obj sw = make("QWidget");
        qtc_arg_obj(sw);
        CHECK_EQ(qtc_call(sa, S("setWidget")), QTC_OK);
        qtc_arg_obj(sw);
        CHECK_EQ(qtc_call(sa, S("setViewport")), QTC_ERR_BAD_ARGS);
        qtc_arg_obj(sa);
        CHECK_EQ(qtc_call(sa, S("setViewport")), QTC_ERR_BAD_ARGS);
        CHECK_EQ(qtc_call(sa, S("viewport")), QTC_OK);
        qtc_obj vp = qtc_ret_obj();
        qtc_arg_obj(vp);
        CHECK(qtc_call(sa, S("setWidget")) != QTC_OK);
        /* another scroll area cannot take the widget while the first still has it */
        qtc_obj sb = make("QScrollArea");
        qtc_arg_obj(sw);
        CHECK_EQ(qtc_call(sb, S("setWidget")), QTC_ERR_STATE);
        CHECK_EQ(qtc_call(sa, S("takeWidget")), QTC_OK);
        qtc_arg_obj(sw);
        CHECK_EQ(qtc_call(sb, S("setWidget")), QTC_OK);
        /* a tool box's pages sit in scroll areas of their own */
        qtc_obj tb = make("QToolBox");
        qtc_arg_obj(sw);
        qtc_arg_str(S("page"));
        CHECK_EQ(qtc_call(tb, S("addItem(QWidget*,QString)")), QTC_ERR_STATE);
        qtc_obj page = make("QWidget");
        qtc_arg_obj(page);
        qtc_arg_str(S("page"));
        CHECK_EQ(qtc_call(tb, S("addItem(QWidget*,QString)")), QTC_OK);
        qtc_arg_obj(page);
        CHECK_EQ(qtc_call(sb, S("setWidget")), QTC_ERR_STATE);
        qtc_destroy(tb);
        qtc_destroy(sb);
        qtc_destroy(sa);
        qtc_process_events();
    }
    /* a path moved by a non-finite offset gets NaN points, and every hit test then walks all of it */
    {
        qtc_val pp = qtc_val_new(S("QPainterPath"));
        qtc_arg_double(NAN);
        qtc_arg_double(0);
        CHECK_EQ(qtc_call(pp, S("translate(qreal,qreal)")), QTC_ERR_BAD_ARGS);
        qtc_arg_double(1);
        qtc_arg_double(2);
        CHECK_EQ(qtc_call(pp, S("translate(qreal,qreal)")), QTC_OK);
        qtc_val_free(pp);
    }
    /* a standard item model removes a column row by row, moving everything after it each time */
    {
        qtc_obj sm = make("QStandardItemModel");
        qtc_arg_int(32);
        CHECK_EQ(qtc_call(sm, S("setColumnCount")), QTC_OK);
        qtc_arg_int(65536);
        CHECK_EQ(qtc_call(sm, S("setRowCount")), QTC_OK);
        qtc_arg_int(0);
        qtc_arg_int(1);
        CHECK_EQ(qtc_call(sm, S("removeColumns")), QTC_ERR_LIMIT);
        qtc_arg_int(8);
        CHECK_EQ(qtc_call(sm, S("setColumnCount")), QTC_ERR_LIMIT);
        qtc_arg_int(16);
        CHECK_EQ(qtc_call(sm, S("setRowCount")), QTC_OK);
        qtc_arg_int(0);
        qtc_arg_int(1);
        CHECK_EQ(qtc_call(sm, S("removeColumns")), QTC_OK);
        qtc_destroy(sm);
    }
    /* normalizing orders each run of combining marks in time growing as its square */
    {
        char *marks = malloc(2 * 20000 + 2);
        marks[0] = 'a';
        for (int i = 0; i < 20000; i++) {
            marks[1 + 2 * i] = (char)0xcc;   /* U+0301 COMBINING ACUTE ACCENT */
            marks[2 + 2 * i] = (char)0x81;
        }
        marks[1 + 2 * 20000] = 0;
        qtc_arg_str(qtc_s(marks));
        qtc_val qs = qtc_val_new(S("QString"));
        qtc_arg_int(3);   /* NormalizationForm_KC */
        CHECK_EQ(qtc_call(qs, S("normalized")), QTC_ERR_LIMIT);
        qtc_val_free(qs);
        marks[1 + 2 * 1000] = 0;
        qtc_arg_str(qtc_s(marks));
        qs = qtc_val_new(S("QString"));
        qtc_arg_int(3);
        CHECK_EQ(qtc_call(qs, S("normalized")), QTC_OK);
        qtc_val_free(qs);
        free(marks);
    }
    /* a backward whole-word search with a regular expression is quadratic in the block length */
    {
        qtc_obj pe = make("QPlainTextEdit");
        char *big = malloc(20001);
        memset(big, 'a', 20000);
        big[20000] = 0;
        qtc_arg_str(qtc_s(big));
        CHECK_EQ(qtc_call(pe, S("insertPlainText")), QTC_OK);
        free(big);
        qtc_val rx = qtc_val_new(S("QRegularExpression"));
        qtc_arg_val(rx);
        qtc_arg_int(1 | 4);   /* FindBackward | FindWholeWords */
        CHECK_EQ(qtc_call(pe, S("find(QRegularExpression,QTextDocument::FindFlags)")), QTC_ERR_LIMIT);
        qtc_arg_val(rx);
        qtc_arg_int(1);   /* backward alone finds the first match at once */
        CHECK_EQ(qtc_call(pe, S("find(QRegularExpression,QTextDocument::FindFlags)")), QTC_OK);
        qtc_val_free(rx);
        qtc_destroy(pe);
    }
    /* a scene's own style is deleted before the widgets in its proxy widgets let go of it */
    {
        qtc_obj scene = make("QGraphicsScene");
        qtc_obj cst = make("QCommonStyle");
        qtc_arg_obj(cst);
        CHECK_EQ(qtc_call(scene, S("setStyle")), QTC_OK);
        qtc_arg_null();
        CHECK_EQ(qtc_call(scene, S("setStyle")), QTC_OK);   /* no proxy widgets: fine */
        CHECK(!qtc_alive(cst));   /* the scene deleted its old style */
        cst = make("QCommonStyle");
        qtc_arg_obj(cst);
        CHECK_EQ(qtc_call(scene, S("setStyle")), QTC_OK);
        qtc_obj pb = make("QPushButton");
        qtc_arg_obj(pb);
        CHECK_EQ(qtc_call(scene, S("addWidget")), QTC_OK);
        qtc_arg_null();
        CHECK_EQ(qtc_call(scene, S("setStyle")), QTC_ERR_STATE);
        CHECK(qtc_alive(cst));
        qtc_destroy(scene);
        qtc_process_events();
    }
    /* the channel count is a short inside QAudioFormat (negative frame sizes then divide) */
    {
        qtc_val af = qtc_val_new(S("QAudioFormat"));
        if (af) {
            qtc_arg_int(40000);
            CHECK_EQ(qtc_call(af, S("setChannelCount")), QTC_ERR_OUT_OF_RANGE);
            qtc_arg_int(2);
            CHECK_EQ(qtc_call(af, S("setChannelCount")), QTC_OK);
            qtc_val_free(af);
        }
        qtc_clear_error();
    }
    /* GPL-3.0-only add-ons, when built: an MQTT client's transport and an OAuth object's built-in
       reply handler stay as long as their owner; transports must suit their type */
    {
        qtc_obj mq = make("QMqttClient");
        if (mq) {
            qtc_obj buf = make("QBuffer");
            qtc_arg_obj(buf);
            qtc_arg_int(1);   /* AbstractSocket */
            CHECK_EQ(qtc_call(mq, S("setTransport")), QTC_ERR_BAD_ARGS);
            qtc_arg_null();
            qtc_arg_int(0);
            CHECK_EQ(qtc_call(mq, S("setTransport")), QTC_ERR_BAD_ARGS);
            qtc_arg_obj(buf);
            qtc_arg_int(0);   /* IODevice */
            CHECK_EQ(qtc_call(mq, S("setTransport")), QTC_OK);
            CHECK_EQ(qtc_destroy(buf), QTC_ERR_STATE);
            CHECK_EQ(qtc_set_int(mq, S("state"), 2), QTC_ERR_STATE);   /* Connected */
            qtc_obj ws = make("QWebSocket");
            if (ws) {
                qtc_arg_obj(ws);
                CHECK_EQ(qtc_call(mq, S("connectToHostWebSocket")), QTC_ERR_BAD_ARGS);
                qtc_destroy(ws);
            }
            qtc_destroy(mq);
            qtc_process_events();
            CHECK_EQ(qtc_destroy(buf), QTC_OK);
        }
        /* a running CoAP reply is used by the client's protocol thread */
        qtc_obj coap = make("QCoapClient");
        if (coap) {
            qtc_arg_str(S("coap://127.0.0.1:5683/x"));
            CHECK_EQ(qtc_call(coap, S("get(QUrl)")), QTC_OK);
            qtc_obj rep = qtc_ret_obj();
            CHECK(rep != 0);
            CHECK_EQ(qtc_destroy(rep), QTC_ERR_STATE);
            qtc_destroy(coap);
            qtc_process_events();
            /* a client destroyed at once while its protocol thread has the request queued
               (Qt's protocol reads the reply there unchecked: qtc drains that thread first) */
            int sent = 0;
            for (int i = 0; i < 60; i++) {
                qtc_obj c2 = make("QCoapClient");
                qtc_arg_str(S("coap://127.0.0.1:5683/x"));
                if (qtc_call(c2, S("get(QUrl)")) == QTC_OK) sent++;
                CHECK_EQ(qtc_destroy(c2), QTC_OK);
                if (i % 3 == 0) qtc_process_events();
            }
            CHECK_EQ(sent, 60);
            qtc_process_events();
        }
        qtc_obj oa = make("QOAuth1");
        if (oa) {
            CHECK_EQ(qtc_call(oa, S("replyHandler")), QTC_OK);
            qtc_obj rh = qtc_ret_obj();
            CHECK(rh != 0);
            CHECK_EQ(qtc_destroy(rh), QTC_ERR_STATE);
            qtc_arg_int(1);   /* Rsa_Sha1: Qt aborts on it */
            CHECK_EQ(qtc_call(oa, S("setSignatureMethod")), QTC_ERR_BAD_ARGS);
            qtc_destroy(oa);
        }
        qtc_process_events();
        qtc_clear_error();
    }
    qtc_destroy(sub);
    qtc_destroy(loose);
    qtc_destroy(mb);
    qtc_destroy(grid);
    qtc_destroy(sim);
    qtc_destroy(table);
    qtc_destroy(idp);
    qtc_destroy(slm);
    qtc_destroy(host);
    qtc_destroy(form);
    qtc_destroy(split);
    qtc_destroy(tabs);
    qtc_process_events();
    reset();
}

/* Direct signals (QTC_CONN_DIRECT): the handler runs inside the emission, may destroy the
 * sender (deleted after Qt's call), cannot run the event loop, and nests. */
static struct { int calls, nested, wait_refused, arg_ok, depth_max, destroyed, same_event; qtc_obj slider; } g_ds;
static void direct_handler(const qtc_event *e, void *user) {
    (void)user;
    g_ds.calls++;
    qtc_event cur;
    if (qtc_direct_event(&cur) && cur.seq == e->seq && cur.tag == e->tag) g_ds.same_event++;
    int32_t d = qtc_ov_depth();
    if (d > g_ds.depth_max) g_ds.depth_max = d;
    if (e->tag == 700) {                       /* clicked(bool) */
        bool checked = true;
        if (e->argc == 1 && qtc_event_arg_bool(e->seq, 0, &checked) == QTC_OK && !checked) g_ds.arg_ok++;
        if (qtc_wait(0) == QTC_ERR_STATE) g_ds.wait_refused++;
        qtc_arg_int(42);
        qtc_call(g_ds.slider, S("setValue"));  /* emits valueChanged: a nested direct call */
    } else if (e->tag == 701) {                /* valueChanged(int) */
        g_ds.nested++;
        if (e->i0 == 42) g_ds.arg_ok++;
    } else if (e->tag == 702) {                /* the sender destroys itself */
        if (qtc_destroy(e->source) == QTC_OK) g_ds.destroyed++;
    }
}
static void test_direct_signals(void) {
    reset();
    memset(&g_ds, 0, sizeof g_ds);
    qtc_obj w = make("QWidget");
    qtc_obj b = make_child("QPushButton", w);
    g_ds.slider = make_child("QSlider", w);
    CHECK_EQ(qtc_set_signal_handler(direct_handler, NULL), QTC_OK);
    CHECK(qtc_connect(b, S("clicked"), 700, QTC_CONN_DIRECT) != 0);
    CHECK(qtc_connect(g_ds.slider, S("valueChanged"), 701, QTC_CONN_DIRECT) != 0);
    CHECK_EQ(qtc_call(b, S("click")), QTC_OK);
    CHECK_EQ(g_ds.calls, 2);                   /* clicked, then valueChanged inside it */
    CHECK_EQ(g_ds.nested, 1);
    CHECK_EQ(g_ds.arg_ok, 2);
    CHECK_EQ(g_ds.wait_refused, 1);
    CHECK_EQ(g_ds.depth_max, 2);
    CHECK_EQ(g_ds.same_event, 2);
    qtc_event none;
    CHECK(!qtc_direct_event(&none));             /* only inside the handler */
    CHECK_EQ(drain(NULL, 700) + drain(NULL, 701), 0);   /* nothing was queued */
    /* the sender destroyed by its own handler: deleted once Qt's call returns */
    qtc_obj b2 = make_child("QPushButton", w);
    CHECK(qtc_connect(b2, S("clicked"), 702, QTC_CONN_DIRECT | QTC_CONN_ONCE) != 0);
    CHECK_EQ(qtc_call(b2, S("click")), QTC_OK);
    qtc_process_events();
    CHECK(!qtc_alive(b2));
    /* without a handler, a direct connection queues as before */
    CHECK_EQ(qtc_set_signal_handler(NULL, NULL), QTC_OK);
    int before = g_ds.calls;
    qtc_call(b, S("click"));
    qtc_process_events();
    CHECK_EQ(g_ds.calls, before);
    CHECK_EQ(drain(NULL, 700), 1);
    drain(NULL, 0);
    CHECK_EQ(qtc_set_signal_handler(direct_handler, NULL), QTC_OK);
    /* churn: many direct emissions, each sender destroyed by its handler */
    for (int i = 0; i < 200; i++) {
        qtc_obj x = make_child("QPushButton", w);
        qtc_connect(x, S("clicked"), 702, QTC_CONN_DIRECT);
        qtc_call(x, S("click"));
    }
    qtc_process_events();
    CHECK_EQ(g_ds.destroyed, 201);
    qtc_set_signal_handler(NULL, NULL);
    qtc_destroy(w);
    reset();
}

/* Objects made and destroyed in a mix: plain ones (no event or ownership state, the fast path),
 * ones with a connection or a watch (the event layer cleans up), owners (the ownership roles
 * forget them). Addresses get reused all the time: a new object must never inherit anything of
 * the dead one at its address. Sanitizer builds also cross-check that the objects the fast paths
 * skipped had nothing to clean up (QTC_CROSSCHECK). */
static void test_lifetime_fast_paths(void) {
    reset();
    uint32_t rnd = 12345;
    int stale_ok = 0, quiet_ok = 0, loud_ok = 0;
    for (int round = 0; round < 300; round++) {
        enum { N = 8 };
        qtc_obj o[N];
        int kind[N];
        for (int i = 0; i < N; i++) {
            rnd = rnd * 1103515245u + 12345u;
            kind[i] = (int)((rnd >> 16) % 5);
            switch (kind[i]) {
            case 0: o[i] = qtcn_QObject(); break;                                  /* plain */
            case 1: o[i] = qtcn_QObject(); qtc_connect(o[i], S("objectNameChanged"), 900 + (uint64_t)i, 0); break;
            case 2: o[i] = make("QWidget"); qtc_watch(o[i], QTC_WATCH_KEYS, 0, 950); break;
            case 3: o[i] = make("QComboBox"); break;                              /* an owner */
            default: o[i] = make("QHBoxLayout"); break;                            /* an owner, a layout */
            }
        }
        /* destroy in a random order */
        for (int k = 0; k < N; k++) {
            rnd = rnd * 1103515245u + 12345u;
            int i = (int)((rnd >> 16) % N);
            while (!qtc_alive(o[i])) i = (i + 1) % N;
            if (qtc_destroy(o[i]) == QTC_OK && !qtc_alive(o[i])) stale_ok++;
        }
        /* new plain objects, likely at the same addresses: no old connection may fire for them */
        qtc_obj fresh[N];
        for (int i = 0; i < N; i++) {
            fresh[i] = qtcn_QObject();
            qtc_arg_str(S("renamed"));
            qtc_call(fresh[i], S("setObjectName"));
        }
        qtc_process_events();
        qtc_event e;
        int stray = 0;
        while (qtc_poll(&e))
            if (e.tag >= 900 && e.tag < 1000) stray++;
        if (stray == 0) quiet_ok++;
        /* and a connection made now works */
        qtc_connect(fresh[0], S("objectNameChanged"), 990, 0);
        qtc_arg_str(S("again"));
        qtc_call(fresh[0], S("setObjectName"));
        qtc_process_events();
        if (drain(NULL, 990) == 1) loud_ok++;
        for (int i = 0; i < N; i++) qtc_destroy(fresh[i]);
    }
    CHECK_EQ(stale_ok, 300 * 8);
    CHECK_EQ(quiet_ok, 300);
    CHECK_EQ(loud_ok, 300);
    reset();
}

static void test_wrong_thread(void) {
    reset();
    qtc_obj w = make("QWidget");
    pthread_t t;
    pthread_create(&t, NULL, wrong_thread, (void *)(uintptr_t)w);
    void *r;
    pthread_join(t, &r);
    CHECK(r == NULL);
    qtc_destroy(w);
    qtc_process_events();
}

static void *direct_wrong_thread(void *arg) {
    qtc_obj l = (qtc_obj)(uintptr_t)arg;
    int ok = qtcm_setText__QString(l, S("x")) == QTC_ERR_WRONG_THREAD && qtcm_text(l) == QTC_ERR_WRONG_THREAD &&
             qtcn_QObject() == 0 && qtc_last_error() == QTC_ERR_WRONG_THREAD;
    return ok ? NULL : (void *)1;
}

/* Direct entry points (qtc_direct.h): the same results and errors as qtc_call. */
/* qtc_inline.h: value types as structs, and the qtcmi_ entry points that take them */
static void test_inline(void) {
    reset();
    qtc_QRect r = qtci_QRect__int_int_int_int(1, 2, 3, 4);
    CHECK_EQ(qtc_last_error(), QTC_OK);
    CHECK(r.x1 == 1 && r.y1 == 2 && r.x2 == 3 && r.y2 == 5);   /* Qt keeps the far corner */
    CHECK_EQ(qtci_QRect_right(r), 3);
    qtci_QRect_moveTo__int_int(&r, 10, 20);
    CHECK(r.x1 == 10 && r.y1 == 20 && r.x2 == 12 && r.y2 == 23);
    qtc_QRect t = qtci_QRect_translated__int_int(r, 1, 1);
    CHECK(t.x1 == 11 && r.x1 == 10);
    CHECK(qtci_QRect_intersects__QRect(r, t));
    qtc_QRectF rf = qtci_QRectF__QRect(r);
    CHECK(rf.xp == 10.0 && rf.w == 3.0 && rf.h == 4.0);
    qtc_QPoint p = {3, -4};
    CHECK_EQ(qtci_QPoint_manhattanLength(p), 7);
    /* colors: made by qtc, changed in place, converted */
    qtc_QColor c = qtci_QColor__int_int_int(255, 10, 20);
    CHECK_EQ(qtci_QColor_red(c), 255);
    CHECK_EQ(qtci_QColor_green(c), 10);
    qtci_QColor_setRed__int(&c, 7);
    CHECK_EQ(qtci_QColor_red(c), 7);
    qtc_QColor h = qtci_QColor_fromHsv__int_int_int(120, 255, 255);
    CHECK_EQ(qtci_QColor_hue(h), 120);
    CHECK_EQ(qtci_QColor_green(h), 255);
    /* checks: integers out of range, null pointers, colors qtc did not make */
    qtc_QRect bad = qtci_QRect__int_int_int_int(1LL << 40, 0, 1, 1);
    CHECK_EQ(qtc_last_error(), QTC_ERR_OUT_OF_RANGE);
    CHECK(bad.x1 == 0 && bad.x2 == 0);
    qtci_QRect_moveTo__int_int(NULL, 1, 1);
    CHECK_EQ(qtc_last_error(), QTC_ERR_NULL_HANDLE);
    qtc_QColor junk = c;
    junk.spec = 99;
    CHECK_EQ(qtci_QColor_red(junk), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_INVALID_ARG);
    junk = h;
    junk.c1 = 40000;   /* an HSV hue past 359.99 degrees */
    CHECK_EQ(qtci_QColor_red(junk), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_INVALID_ARG);
    CHECK_EQ(qtci_QColor_red(c), 7);
    CHECK_EQ(qtc_last_error(), QTC_OK);
    /* objects take them through qtcmi_ and the dynamic API */
    qtc_obj w = qtcn_QWidget();
    qtc_QRect g = qtci_QRect__int_int_int_int(5, 6, 70, 80);
    CHECK_EQ(qtcmi_setGeometry__QRect(w, g), QTC_OK);
    CHECK_EQ(qtc_call(w, S("geometry")), QTC_OK);
    qtc_QRect back = qtc_ret_QRect();
    CHECK(memcmp(&back, &g, sizeof g) == 0);
    qtc_QPoint at = {9, 8};
    CHECK_EQ(qtcmi_move__QPoint(w, at), QTC_OK);
    CHECK_EQ(qtc_call(w, S("pos")), QTC_OK);
    qtc_QPoint pos = qtc_ret_QPoint();
    CHECK(pos.xp == 9 && pos.yp == 8);
    CHECK_EQ(qtcmi_setGeometry__QRect(0, g), QTC_ERR_NULL_HANDLE);
    CHECK_EQ(qtcmi_fill__QColor(w, c), QTC_ERR_NOT_FOUND);   /* a QWidget has no fill(QColor) */
    CHECK_EQ(qtcmi_fill__QColor(w, junk), QTC_ERR_INVALID_ARG);
    qtc_arg_QRect(g);
    CHECK_EQ(qtc_call(w, S("setGeometry")), QTC_OK);
    qtc_arg_QColor(junk);
    CHECK(qtc_call(w, S("setGeometry")) != QTC_OK);   /* the bad argument fails the call */
    qtc_args_clear();
    CHECK_EQ(qtc_call(w, S("width")), QTC_OK);
    qtc_QRect none = qtc_ret_QRect();   /* an int is not a QRect */
    CHECK_EQ(qtc_last_error(), QTC_ERR_WRONG_TYPE);
    CHECK(none.x1 == 0 && none.x2 == 0);
    /* to and from handles */
    qtc_val v = qtc_val_from_QColor(c);
    CHECK(v != 0 && qtc_is_value(v));
    CHECK_EQ(qtcm_red(v), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 7);
    qtc_QColor c2;
    CHECK_EQ(qtc_val_to_QColor(v, &c2), QTC_OK);
    CHECK(memcmp(&c2, &c, sizeof c) == 0);
    CHECK_EQ(qtc_val_to_QRect(v, &back), QTC_ERR_WRONG_TYPE);
    CHECK_EQ(qtc_val_from_QColor(junk), 0);
    qtc_val_free(v);
    qtc_destroy(w);
}

static void test_direct(void) {
    reset();
    qtc_obj l = qtcn_QLabel__QString(S("start"));
    CHECK(l != 0 && qtc_is_object(l));
    CHECK_EQ(qtcm_text(l), QTC_OK);
    CHECK_STR(qtc_ret_str(), "start");
    CHECK_EQ(qtcm_setText__QString(l, S("hello")), QTC_OK);
    CHECK_EQ(qtc_call(l, S("text")), QTC_OK);
    CHECK_STR(qtc_ret_str(), "hello");
    for (int i = 0; i < 3; i++) {   /* the second call on the class takes the cached path */
        CHECK_EQ(qtcm_setText__QString(l, S("again")), QTC_OK);
        CHECK_EQ(qtcm_text(l), QTC_OK);
        CHECK_STR(qtc_ret_str(), "again");
    }
    /* pushed arguments are neither used nor cleared */
    qtc_arg_int(7);
    CHECK_EQ(qtcm_text(l), QTC_OK);
    CHECK_EQ(qtc_args_count(), 1);
    qtc_args_clear();
    /* errors */
    CHECK_EQ(qtcm_text(0), QTC_ERR_NULL_HANDLE);
    CHECK_EQ(qtcm_setValue__int(l, 3), QTC_ERR_NOT_FOUND);   /* a QLabel has no setValue(int) */
    CHECK_EQ(qtcm_setText__QString(l, (qtc_str){NULL, 3}), QTC_ERR_INVALID_ARG);
    qtc_obj s = qtcn_QSlider();
    CHECK_EQ(qtcm_setValue__int(s, 1LL << 40), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtcm_setMaximum__int(s, 100), QTC_OK);
    CHECK_EQ(qtcm_setValue__int(s, 42), QTC_OK);
    CHECK_EQ(qtcm_value(s), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 42);
    /* the same signature on another class of the same name */
    qtc_obj b = qtcn_QPushButton__QString(S("b"));
    CHECK_EQ(qtcm_setText__QString(b, S("button")), QTC_OK);
    CHECK_EQ(qtcm_text(b), QTC_OK);
    CHECK_STR(qtc_ret_str(), "button");
    /* values */
    qtc_val c = qtcn_QColor__int_int_int(255, 10, 20);
    CHECK(c != 0 && qtc_is_value(c));
    CHECK_EQ(qtcm_red(c), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 255);
    CHECK_EQ(qtcm_green(c), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 10);
    CHECK_EQ(qtcm_red(l), QTC_ERR_NOT_FOUND);
    qtc_val_free(c);
    CHECK_EQ(qtcm_red(c), QTC_ERR_STALE_HANDLE);
    /* what qtc_call treats specially still is: a parent cycle is refused */
    qtc_obj w = qtcn_QWidget();
    qtc_obj inner = qtcn_QWidget__QWidgetPtr(w);
    CHECK_EQ(qtcm_setParent__QWidgetPtr(w, inner), QTC_ERR_REJECTED);
    /* objects qtc makes: a child goes with its parent (a widget with events still queued for it,
       here a polish request, goes once they are delivered) */
    CHECK_EQ(qtc_destroy(w), QTC_OK);
    CHECK(!qtc_alive(w));
    qtc_process_events();
    CHECK(!qtc_alive(inner));
    CHECK_EQ(qtcm_show(inner), QTC_ERR_STALE_HANDLE);
    qtc_obj parent = qtcn_QObject();
    qtc_obj kid = qtcn_QObject__QObjectPtr(parent);
    qtc_obj kid2 = qtc_new(S("QObject"));
    CHECK_EQ(qtcm_setParent__QObjectPtr(kid2, parent), QTC_OK);
    int64_t before = qtc_live_objects();
    CHECK_EQ(qtc_destroy(parent), QTC_OK);
    CHECK(!qtc_alive(kid) && !qtc_alive(kid2));
    CHECK_EQ(qtc_live_objects(), before - 3);
    /* deleted by Qt itself: a QTimer::singleShot to deleteLater is not reachable, a parent's is */
    qtc_obj holder = qtcn_QObject();
    qtc_obj held = qtcn_QTimer__QObjectPtr(holder);
    CHECK_EQ(qtcm_setSingleShot__bool(held, true), QTC_OK);
    CHECK_EQ(qtc_destroy(holder), QTC_OK);
    CHECK(!qtc_alive(held));
    /* a constructor given the wrong kind of object */
    qtc_obj plain = qtcn_QObject();
    qtc_clear_error();
    CHECK_EQ(qtcn_QLabel__QString_QWidgetPtr(S("x"), plain), 0);
    CHECK(qtc_last_error() != QTC_OK);
    qtc_destroy(plain);
    qtc_clear_error();
    /* from another thread */
    pthread_t th;
    pthread_create(&th, NULL, direct_wrong_thread, (void *)(uintptr_t)l);
    void *r;
    pthread_join(th, &r);
    CHECK(r == NULL);
    qtc_destroy(l);
    qtc_destroy(s);
    qtc_destroy(b);
    reset();
}

static void test_describe(void) {
    reset();
    qtc_view d = qtc_describe(0, S("QPushButton"));
    CHECK(d.len > 100);
    CHECK(strstr(d.data, "\"setFlat(bool)\"") != NULL);
    CHECK(strstr(d.data, "\"clicked(bool)\"") != NULL);
    CHECK_EQ(qtc_describe(0, S("Nope")).len, 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_NOT_FOUND);
    int64_t v = 0;
    CHECK_EQ(qtc_enum_value(S("Qt::AlignmentFlag::AlignHCenter"), &v), QTC_OK);
    CHECK_EQ(v, 4);
    CHECK_STR(qtc_enum_key(S("Qt::AlignmentFlag"), 4), "AlignHCenter");
    CHECK_EQ(qtc_enum_value(S("Qt::NoSuchThing"), &v), QTC_ERR_NOT_FOUND);
}

static void test_shutdown_and_reinit(void) {
    reset();
    qtc_obj w = make("QWidget");
    qtc_obj b = make_child("QPushButton", w);
    qtc_val v = qtc_val_str(S("x"));
    qtc_obj t = qtc_timer(1000, false, 1);
    qtc_connect(b, S("clicked"), 1, 0);
    qtc_watch(w, QTC_WATCH_MOUSE, 0, 1);
    CHECK_EQ(qtc_shutdown(), QTC_OK);
    CHECK(!qtc_is_initialized());
    CHECK(!qtc_alive(w));
    CHECK(!qtc_alive(v));
    CHECK(!qtc_alive(t));
    CHECK_EQ(qtc_shutdown(), QTC_ERR_NOT_INITIALIZED);
    CHECK_EQ(qtc_init(QTC_APP_WIDGETS, S("again\0-platform\0offscreen")), QTC_OK);
    CHECK(!qtc_alive(w));
    CHECK_EQ(qtc_call(w, S("show")), QTC_ERR_STALE_HANDLE);
    qtc_obj w2 = make("QWidget");
    CHECK(w2 != 0 && w2 != w);
    CHECK_EQ(qtc_live_values(), 0);
    CHECK_EQ(qtc_shutdown(), QTC_OK);
}

int main(void) {
    test_before_init();
    test_init();
    test_handles();
    test_errors();
    test_properties();
    test_calls();
    test_values();
    test_strings();
    test_signals();
    test_forwards();
    test_meta_only_methods();
    test_handler_destroys_sender();
    test_watch_and_input();
    test_timers();
    test_threads();
    test_last_window();
    test_drawing();
    test_models();
    test_qml();
    test_blocked_signals_lifetime();
    test_ownership_rules();
    test_fuzz_findings();
    test_lifetime_fast_paths();
    test_direct_signals();
    test_wrong_thread();
    test_direct();
    test_inline();
    test_describe();
    test_shutdown_and_reinit();
    printf("test_core: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
