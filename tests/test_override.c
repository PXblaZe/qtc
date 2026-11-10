/* qtc override tests: subclassing Qt classes from C (qtc_new_overridable, qtc_override and the
   qtc_ov_* frame API). C against the public API only. */
#define _POSIX_C_SOURCE 200809L
#include "qtc.h"
#include "qtc_test.h"

#include <pthread.h>
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

static int view_eq(qtc_view v, const char *lit) { return v.len == (int64_t)strlen(lit) && memcmp(v.data, lit, strlen(lit)) == 0; }
static int view_has(qtc_view v, const char *needle) {
    size_t n = strlen(needle);
    for (int64_t i = 0; i + (int64_t)n <= v.len; i++)
        if (memcmp(v.data + i, needle, n) == 0) return 1;
    return 0;
}
static void drain(void) {
    qtc_event e;
    qtc_process_events();
    while (qtc_poll(&e)) {}
    qtc_cancel_quit();
    qtc_clear_error();
}

/* ---------------------------------------------------------- the handler */

enum {
    T_ROWS = 1, T_DATA, T_TABLE_ROWS, T_TABLE_COLS, T_TABLE_INDEX, T_TABLE_PARENT, T_TABLE_DATA, T_FILTER,
    T_PAINT, T_MOUSE, T_SIZEHINT, T_VALIDATE, T_HIGHLIGHT, T_READ, T_WRITE, T_SEQ, T_RUN, T_TIMER,
    T_RECURSE, T_BAD_INDEX, T_HUGE, T_INSIDE, T_DESTROY_SELF, T_SCENE_BG, T_CLOSE, T_BASE_FLAGS
};

static int g_rows = 5;
static int g_calls[64];
static qtc_event g_last_event;
static int g_last_depth, g_run_gui = -1, g_run_depth;
static qtc_status g_inside[8];
static char g_device_data[64] = "hello from C";
static char g_written[64];
static int64_t g_written_len;
static qtc_obj g_other_model;
static atomic_int g_run_done;

static int32_t handler(const qtc_override_call *c, void *user) {
    (void)user;
    if (c->tag < 64) g_calls[c->tag]++;
    g_last_depth = c->depth;
    switch (c->tag) {
    case T_ROWS:
        qtc_ov_ret_int(g_rows);
        return QTC_OV_DONE;
    case T_DATA: {
        int32_t row = -1;
        CHECK(qtc_ov_arg_index(0, &row, NULL, NULL));
        if (qtc_ov_arg_int(1) != 0) return QTC_OV_DONE;   /* Qt::DisplayRole only */
        char buf[32];
        snprintf(buf, sizeof buf, "item %d", row);
        qtc_ov_ret_str(qtc_s(buf));
        return QTC_OV_DONE;
    }
    case T_TABLE_ROWS: {
        /* a flat table: children of the root only */
        if (qtc_ov_arg_index(0, NULL, NULL, NULL)) qtc_ov_ret_int(0);
        else qtc_ov_ret_int(3);
        return QTC_OV_DONE;
    }
    case T_TABLE_COLS:
        qtc_ov_ret_int(qtc_ov_arg_index(0, NULL, NULL, NULL) ? 0 : 2);
        return QTC_OV_DONE;
    case T_TABLE_INDEX: {
        int64_t r = qtc_ov_arg_int(0), col = qtc_ov_arg_int(1);
        if (qtc_ov_arg_index(2, NULL, NULL, NULL) || r < 0 || r >= 3 || col < 0 || col >= 2) return QTC_OV_DONE;   /* invalid */
        qtc_ov_ret_index((int32_t)r, (int32_t)col, (uint64_t)(r * 10 + col));
        return QTC_OV_DONE;
    }
    case T_TABLE_PARENT:
        qtc_ov_ret_index(-1, -1, 0);
        return QTC_OV_DONE;
    case T_TABLE_DATA: {
        int32_t r, col;
        uint64_t id;
        if (!qtc_ov_arg_index(0, &r, &col, &id) || qtc_ov_arg_int(1) != 0) return QTC_OV_DONE;
        char buf[32];
        snprintf(buf, sizeof buf, "%d,%d:%llu", r, col, (unsigned long long)id);
        qtc_ov_ret_str(qtc_s(buf));
        return QTC_OV_DONE;
    }
    case T_FILTER:
        qtc_ov_ret_bool(qtc_ov_arg_int(0) % 2 == 0);   /* even source rows */
        return QTC_OV_DONE;
    case T_PAINT:
        CHECK_EQ(qtc_draw_begin(c->self), QTC_OK);
        qtc_draw_clear(0xff00ff00u);   /* opaque green */
        qtc_draw_brush(0xffff0000u, 1);
        qtc_draw_no_pen();
        qtc_draw_rect(0, 0, 10, 10);   /* red corner */
        CHECK_EQ(qtc_draw_end(), QTC_OK);
        return QTC_OV_DONE;
    case T_MOUSE:
        CHECK_EQ(qtc_ov_arg_event(0, &g_last_event), QTC_OK);
        CHECK_EQ(qtc_ov_arg_kind(0), QTC_T_NONE);
        return QTC_OV_DONE;
    case T_SIZEHINT:
        CHECK_EQ(qtc_ov_call_base(), QTC_OK);
        qtc_arg_int(321);
        qtc_arg_int(123);
        CHECK_EQ(qtc_ov_ret_val(qtc_val_new(S("QSize"))), QTC_OK);
        return QTC_OV_DONE;
    case T_VALIDATE: {
        /* uppercase the input; Acceptable once it has 3 characters */
        qtc_view in = qtc_ov_arg_str(0);
        char up[64];
        int64_t n = in.len < 63 ? in.len : 63;
        for (int64_t i = 0; i < n; i++) up[i] = (in.data[i] >= 'a' && in.data[i] <= 'z') ? (char)(in.data[i] - 32) : in.data[i];
        up[n] = 0;
        CHECK_EQ(qtc_ov_set_arg_str(0, qtc_s(up)), QTC_OK);
        CHECK_EQ(qtc_ov_set_arg_int(1, n), QTC_OK);
        qtc_ov_ret_int(n >= 3 ? 2 : 1);   /* QValidator::Acceptable : Intermediate */
        return QTC_OV_DONE;
    }
    case T_HIGHLIGHT: {
        qtc_view text = qtc_ov_arg_str(0);
        qtc_arg_int(0);
        qtc_arg_int(text.len > 2 ? 2 : text.len);
        qtc_arg_val(qtc_val_str(S("#ff0000")));
        g_inside[0] = qtc_call(c->self, S("setFormat(int,int,QColor)"));
        return QTC_OV_DONE;
    }
    case T_READ: {
        int64_t want = qtc_ov_arg_int(1);
        int64_t have = (int64_t)strlen(g_device_data);
        int64_t n = qtc_ov_buffer_write(0, qtc_sn(g_device_data, have < want ? have : want));
        memmove(g_device_data, g_device_data + n, (size_t)(have - n + 1));
        qtc_ov_ret_int(n > 0 ? n : -1);
        return QTC_OV_DONE;
    }
    case T_WRITE: {
        qtc_view v = qtc_ov_buffer_read(0);
        int64_t n = v.len < 63 - g_written_len ? v.len : 63 - g_written_len;
        memcpy(g_written + g_written_len, v.data, (size_t)n);
        g_written_len += n;
        qtc_ov_ret_int(n);
        return QTC_OV_DONE;
    }
    case T_SEQ:
        qtc_ov_ret_bool(true);
        return QTC_OV_DONE;
    case T_RUN:
        g_run_gui = c->gui_thread;
        g_run_depth = c->depth;
        g_inside[1] = qtc_ov_ret_int(1);   /* run() returns nothing */
        qtc_post(77, 1, 2);
        atomic_store(&g_run_done, 1);
        return QTC_OV_DONE;
    case T_TIMER:
        CHECK_EQ(qtc_ov_arg_event(0, &g_last_event), QTC_OK);
        return QTC_OV_DONE;
    case T_RECURSE:
        qtc_call(c->self, S("rowCount"));   /* calls itself again: stops 64 deep */
        qtc_ov_ret_int(1);
        return QTC_OV_DONE;
    case T_BAD_INDEX:
        /* an index of another model: replaced by an invalid one */
        qtc_arg_int(0);
        qtc_arg_int(0);
        qtc_call(g_other_model, S("index"));
        qtc_ov_ret_val(qtc_ret_val());
        return QTC_OV_DONE;
    case T_HUGE:
        g_inside[2] = qtc_ov_ret_int((int64_t)1 << 40);   /* does not fit an int */
        return QTC_OV_DONE;
    case T_INSIDE:
        g_inside[3] = qtc_wait(0);
        g_inside[4] = qtc_process_events();
        g_inside[5] = qtc_shutdown();
        g_inside[6] = qtc_set_override_handler(handler, NULL);
        qtc_ov_ret_int(2);
        return QTC_OV_DONE;
    case T_DESTROY_SELF:
        g_inside[7] = qtc_destroy(c->self);   /* deferred: Qt is in the middle of calling it */
        qtc_ov_ret_int(4);
        return QTC_OV_DONE;
    case T_SCENE_BG:
        CHECK_EQ(qtc_draw_begin_painter(0), QTC_OK);
        qtc_draw_brush(0xff0000ffu, 1);   /* blue */
        qtc_draw_no_pen();
        qtc_draw_rect(-10000, -10000, 20000, 20000);
        return QTC_OV_DONE;   /* left open: ended when the handler returns */
    case T_CLOSE:
        qtc_ov_accept_event(0, false);   /* keep the window open */
        return QTC_OV_DONE;
    case T_BASE_FLAGS:
        return QTC_OV_BASE;
    default:
        return QTC_OV_BASE;
    }
}

/* The same handler, exported for qtc_set_override_handler_symbol. */
int32_t qtc_test_override_handler(const qtc_override_call *c, void *user);
int32_t qtc_test_override_handler(const qtc_override_call *c, void *user) { return handler(c, user); }

/* ------------------------------------------------------------------ tests */

static void test_introspection(void) {
    qtc_view cl = qtc_overridable_classes();
    CHECK(view_has(cl, "QWidget\n") && view_has(cl, "QAbstractListModel") && view_has(cl, "QValidator"));
    qtc_view m = qtc_overridable_methods(S("QAbstractListModel"));
    CHECK(view_has(m, "\"rowCount\"") && view_has(m, "\"pure\":true") && view_has(m, "beginInsertRows"));
    CHECK(!view_has(m, "\"name\":\"columnCount\""));   /* QAbstractListModel implements it privately */
    CHECK_EQ(qtc_overridable_methods(S("QStringListModel")).len, 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_NOT_FOUND);
    CHECK_EQ(qtc_new_overridable(S("QStringListModel")), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_NOT_FOUND);
    CHECK_EQ(qtc_args_count(), 0);
    qtc_obj w = qtc_new(S("QWidget"));
    CHECK(!qtc_is_overridable(w));
    CHECK_EQ(qtc_override(w, S("paintEvent"), 1), QTC_ERR_WRONG_TYPE);
    qtc_destroy(w);
    /* outside a handler the frame API refuses */
    CHECK_EQ(qtc_ov_ret_int(1), QTC_ERR_STATE);
    CHECK_EQ(qtc_ov_arg_int(0), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_STATE);
    CHECK_EQ(qtc_ov_call_base(), QTC_ERR_STATE);
    CHECK_EQ(qtc_ov_depth(), 0);
}

static void test_list_model(void) {
    qtc_obj parent = qtc_new(S("QObject"));
    qtc_arg_obj(parent);
    qtc_obj m = qtc_new_overridable(S("QAbstractListModel"));
    CHECK(m != 0);
    CHECK(qtc_is_overridable(m));
    CHECK_STR(qtc_class_name(m), "QAbstractListModel");
    CHECK(qtc_inherits(m, S("QAbstractItemModel")));
    /* no overrides yet: Qt's own (pure: defaults) */
    CHECK_EQ(qtc_call(m, S("rowCount")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 0);
    CHECK_EQ(qtc_override(m, S("rowCount"), T_ROWS), QTC_OK);
    CHECK_EQ(qtc_override(m, S("data"), T_DATA), QTC_OK);
    CHECK_EQ(qtc_override(m, S("nosuch"), 1), QTC_ERR_NOT_FOUND);
    CHECK_EQ(qtc_override(m, S("rowCount(QModelIndex)"), T_ROWS), QTC_OK);
    CHECK_EQ(qtc_override(m, S("rowCount(int)"), T_ROWS), QTC_ERR_NOT_FOUND);
    g_rows = 5;
    CHECK_EQ(qtc_call(m, S("rowCount")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 5);
    qtc_arg_int(3);
    CHECK_EQ(qtc_call(m, S("index")), QTC_OK);   /* QAbstractListModel's own index() */
    qtc_val idx = qtc_ret_val();
    qtc_arg_val(idx);
    CHECK_EQ(qtc_call(m, S("data")), QTC_OK);
    CHECK_STR(qtc_ret_str(), "item 3");
    qtc_val_free(idx);

    /* a proxy on top sees it, and a filter written in C */
    qtc_obj proxy = qtc_new_overridable(S("QSortFilterProxyModel"));
    CHECK(proxy != 0);
    CHECK_EQ(qtc_override(proxy, S("filterAcceptsRow"), T_FILTER), QTC_OK);
    qtc_arg_obj(m);
    CHECK_EQ(qtc_call(proxy, S("setSourceModel")), QTC_OK);
    CHECK_EQ(qtc_call(proxy, S("rowCount")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 3);   /* rows 0, 2, 4 */

    /* inserting rows: protected begin/end, checked */
    CHECK_EQ(qtc_call(m, S("endInsertRows")), QTC_ERR_STATE);
    qtc_arg_null();
    qtc_arg_int(9);
    qtc_arg_int(9);
    CHECK_EQ(qtc_call(m, S("beginInsertRows")), QTC_ERR_OUT_OF_RANGE);   /* first > rowCount */
    qtc_val root = qtc_val_new(S("QModelIndex"));
    qtc_arg_val(root);
    qtc_arg_int(5);
    qtc_arg_int(6);
    CHECK_EQ(qtc_call(m, S("beginInsertRows")), QTC_OK);
    qtc_arg_val(root);
    qtc_arg_int(0);
    qtc_arg_int(0);
    CHECK_EQ(qtc_call(m, S("beginRemoveRows")), QTC_ERR_STATE);   /* one pair at a time */
    CHECK_EQ(qtc_call(m, S("endRemoveRows")), QTC_ERR_STATE);
    g_rows = 7;
    CHECK_EQ(qtc_call(m, S("endInsertRows")), QTC_OK);
    CHECK_EQ(qtc_call(m, S("endInsertRows")), QTC_ERR_STATE);
    CHECK_EQ(qtc_call(proxy, S("rowCount")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 4);   /* rows 0, 2, 4, 6 */
    qtc_arg_val(root);
    qtc_arg_int(6);
    qtc_arg_int(5);
    CHECK_EQ(qtc_call(m, S("beginRemoveRows")), QTC_ERR_OUT_OF_RANGE);   /* last < first */
    qtc_arg_val(root);
    qtc_arg_int(0);
    qtc_arg_int(1);
    CHECK_EQ(qtc_call(m, S("beginRemoveRows")), QTC_OK);
    g_rows = 5;
    CHECK_EQ(qtc_call(m, S("endRemoveRows")), QTC_OK);
    CHECK_EQ(qtc_call(m, S("beginResetModel")), QTC_OK);
    g_rows = 2;
    CHECK_EQ(qtc_call(m, S("endResetModel")), QTC_OK);
    CHECK_EQ(qtc_call(proxy, S("rowCount")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 1);
    /* the protected methods are the overridable object's alone */
    qtc_obj plain = qtc_new(S("QStringListModel"));
    CHECK_EQ(qtc_call(plain, S("beginResetModel")), QTC_ERR_NOT_FOUND);
    qtc_destroy(plain);

    /* a result that does not fit becomes an error in the handler, the default stays */
    CHECK_EQ(qtc_override(m, S("rowCount"), T_HUGE), QTC_OK);
    CHECK_EQ(qtc_call(m, S("rowCount")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 0);
    CHECK_EQ(g_inside[2], QTC_ERR_OUT_OF_RANGE);
    /* overrides calling each other stop 64 deep */
    CHECK_EQ(qtc_override(m, S("rowCount"), T_RECURSE), QTC_OK);
    g_calls[T_RECURSE] = 0;
    CHECK_EQ(qtc_call(m, S("rowCount")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 1);
    CHECK_EQ(g_calls[T_RECURSE], 64);
    /* what a handler may not do */
    CHECK_EQ(qtc_override(m, S("rowCount"), T_INSIDE), QTC_OK);
    CHECK_EQ(qtc_call(m, S("rowCount")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 2);
    CHECK_EQ(g_inside[3], QTC_ERR_STATE);
    CHECK_EQ(g_inside[4], QTC_ERR_STATE);
    CHECK_EQ(g_inside[5], QTC_ERR_STATE);
    CHECK_EQ(g_inside[6], QTC_ERR_STATE);
    CHECK(qtc_is_initialized());
    /* ending the override: Qt's own again */
    CHECK_EQ(qtc_override(m, S("rowCount"), 0), QTC_OK);
    CHECK_EQ(qtc_call(m, S("rowCount")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 0);
    /* destroying itself from inside: deferred */
    CHECK_EQ(qtc_override(m, S("rowCount"), T_DESTROY_SELF), QTC_OK);
    CHECK_EQ(qtc_call(m, S("rowCount")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 4);
    CHECK_EQ(g_inside[7], QTC_OK);
    CHECK(!qtc_alive(m));
    drain();
    CHECK_EQ(qtc_call(proxy, S("rowCount")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 0);
    qtc_val_free(root);
    qtc_destroy(proxy);
    qtc_destroy(parent);
    drain();
}

static void test_table_model(void) {
    qtc_obj m = qtc_new_overridable(S("QAbstractItemModel"));
    CHECK(m != 0);
    qtc_override(m, S("rowCount"), T_TABLE_ROWS);
    qtc_override(m, S("columnCount"), T_TABLE_COLS);
    qtc_override(m, S("index"), T_TABLE_INDEX);
    qtc_override(m, S("parent"), T_TABLE_PARENT);
    qtc_override(m, S("data"), T_TABLE_DATA);
    qtc_obj view = qtc_new(S("QTableView"));
    qtc_arg_obj(m);
    CHECK_EQ(qtc_call(view, S("setModel")), QTC_OK);
    qtc_arg_int(2);
    qtc_arg_int(1);
    CHECK_EQ(qtc_call(m, S("index")), QTC_OK);
    qtc_val idx = qtc_ret_val();
    qtc_arg_val(idx);
    CHECK_EQ(qtc_call(m, S("data")), QTC_OK);
    CHECK_STR(qtc_ret_str(), "2,1:21");
    qtc_val_free(idx);
    qtc_arg_int(5);
    qtc_arg_int(0);
    CHECK_EQ(qtc_call(m, S("index")), QTC_OK);
    idx = qtc_ret_val();
    qtc_arg_val(idx);
    CHECK_EQ(qtc_call(m, S("data")), QTC_OK);   /* invalid index: no data */
    CHECK(qtc_ret_kind() == QTC_T_NONE || qtc_ret_kind() == QTC_T_NULL);
    qtc_val_free(idx);
    qtc_call(view, S("show"));
    qtct_wait(20);
    /* an index of another model is never handed to Qt */
    g_other_model = qtc_new(S("QStringListModel"));
    qtc_arg_val(qtc_val_from_json(S("[\"a\",\"b\"]")));
    qtc_call(g_other_model, S("setStringList"));
    qtc_override(m, S("index"), T_BAD_INDEX);
    qtc_arg_int(0);
    qtc_arg_int(0);
    CHECK_EQ(qtc_call(m, S("index")), QTC_OK);
    idx = qtc_ret_val();
    qtc_arg_val(idx);
    CHECK_EQ(qtc_call(m, S("data")), QTC_OK);
    CHECK(qtc_ret_kind() == QTC_T_NONE || qtc_ret_kind() == QTC_T_NULL);
    qtc_val_free(idx);
    qtc_destroy(view);
    qtc_destroy(m);
    qtc_destroy(g_other_model);
    drain();
}

static int64_t pixel(qtc_obj widget, int x, int y) {
    CHECK_EQ(qtc_call(widget, S("grab()")), QTC_OK);
    qtc_val pm = qtc_ret_val();
    qtc_val img = qtc_val_convert(pm, S("QImage"));
    qtc_arg_int(x);
    qtc_arg_int(y);
    qtc_call(img, S("pixel(int,int)"));
    int64_t p = qtc_ret_int();
    qtc_val_free(pm);
    qtc_val_free(img);
    return p & 0xffffff;
}

static void test_widget(void) {
    qtc_obj w = qtc_new_overridable(S("QWidget"));
    CHECK(w != 0);
    qtc_arg_int(60);
    qtc_arg_int(40);
    qtc_call(w, S("resize"));
    CHECK_EQ(qtc_override(w, S("paintEvent"), T_PAINT), QTC_OK);
    CHECK_EQ(qtc_override(w, S("mousePressEvent"), T_MOUSE), QTC_OK);
    CHECK_EQ(qtc_override(w, S("sizeHint"), T_SIZEHINT), QTC_OK);
    CHECK_EQ(qtc_override(w, S("closeEvent"), T_CLOSE), QTC_OK);
    qtc_call(w, S("show"));
    qtct_wait(30);
    g_calls[T_PAINT] = 0;
    CHECK_EQ(pixel(w, 30, 30), 0x00ff00);
    CHECK_EQ(pixel(w, 2, 2), 0xff0000);
    CHECK(g_calls[T_PAINT] >= 1);
    /* drawing on a widget outside its paintEvent is refused */
    CHECK_EQ(qtc_draw_begin(w), QTC_ERR_WRONG_TYPE);
    CHECK_EQ(qtc_draw_begin_painter(0), QTC_ERR_STATE);
    /* events reach the override as watched events would */
    memset(&g_last_event, 0, sizeof g_last_event);
    CHECK_EQ(qtct_click(w, 1, 0, 12, 7), QTC_OK);
    CHECK_EQ(g_last_event.kind, QTC_EV_MOUSE_PRESS);
    CHECK_EQ(g_last_event.i0, 1);
    CHECK(g_last_event.d0 == 12 && g_last_event.d1 == 7);
    CHECK_EQ(g_last_event.tag, T_MOUSE);
    CHECK_EQ(g_last_event.source, w);
    /* the class's own result, adjusted */
    CHECK_EQ(qtc_call(w, S("sizeHint")), QTC_OK);
    qtc_val sz = qtc_ret_val();
    qtc_call(sz, S("width"));
    CHECK_EQ(qtc_ret_int(), 321);
    qtc_val_free(sz);
    /* a close event the override ignores keeps the window open */
    CHECK_EQ(qtc_call(w, S("close")), QTC_OK);
    CHECK(!qtc_ret_bool());
    CHECK(qtc_get_bool(w, S("visible")));
    /* protected methods of a widget */
    CHECK_EQ(qtc_call(w, S("focusNextChild")), QTC_OK);
    qtc_destroy(w);
    drain();
}

static void test_validator_highlighter(void) {
    qtc_obj v = qtc_new_overridable(S("QValidator"));
    CHECK(v != 0);
    CHECK_EQ(qtc_override(v, S("validate"), T_VALIDATE), QTC_OK);
    qtc_obj edit = qtc_new(S("QLineEdit"));
    qtc_arg_obj(v);
    CHECK_EQ(qtc_call(edit, S("setValidator")), QTC_OK);
    qtc_call(edit, S("show"));
    qtct_wait(10);
    CHECK_EQ(qtct_type(edit, S("abcd")), QTC_OK);
    CHECK_STR(qtc_get_str(edit, S("text")), "ABCD");
    CHECK(qtc_get_bool(edit, S("acceptableInput")));
    qtc_destroy(edit);
    qtc_destroy(v);

    qtc_obj doc = qtc_new(S("QTextDocument"));
    qtc_arg_obj(doc);
    qtc_obj h = qtc_new_overridable(S("QSyntaxHighlighter"));
    CHECK(h != 0);
    CHECK_EQ(qtc_override(h, S("highlightBlock"), T_HIGHLIGHT), QTC_OK);
    g_calls[T_HIGHLIGHT] = 0;
    g_inside[0] = -99;
    qtc_arg_str(S("one\ntwo\nthree"));
    CHECK_EQ(qtc_call(doc, S("setPlainText")), QTC_OK);
    CHECK_EQ(qtc_call(h, S("rehighlight")), QTC_OK);
    CHECK(g_calls[T_HIGHLIGHT] >= 3);
    CHECK_EQ(g_inside[0], QTC_OK);
    qtc_destroy(h);
    qtc_destroy(doc);
    drain();
}

static void test_device(void) {
    qtc_obj d = qtc_new_overridable(S("QIODevice"));
    CHECK(d != 0);
    CHECK_EQ(qtc_override(d, S("readData"), T_READ), QTC_OK);
    CHECK_EQ(qtc_override(d, S("writeData"), T_WRITE), QTC_OK);
    CHECK_EQ(qtc_override(d, S("isSequential"), T_SEQ), QTC_OK);
    qtc_arg_int(3);   /* ReadWrite */
    CHECK_EQ(qtc_call(d, S("open")), QTC_OK);
    CHECK(qtc_ret_bool());
    CHECK_EQ(qtc_call(d, S("readAll")), QTC_OK);
    CHECK_STR(qtc_ret_str(), "hello from C");
    qtc_arg_bytes(S("written"));
    CHECK_EQ(qtc_call(d, S("write(QByteArray)")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 7);
    CHECK(g_written_len == 7 && memcmp(g_written, "written", 7) == 0);
    CHECK_EQ(qtc_call(d, S("isSequential")), QTC_OK);
    CHECK(qtc_ret_bool());
    qtc_arg_str(S("custom failure"));
    CHECK_EQ(qtc_call(d, S("setErrorString")), QTC_OK);
    CHECK_EQ(qtc_call(d, S("errorString")), QTC_OK);
    CHECK_STR(qtc_ret_str(), "custom failure");
    qtc_call(d, S("close"));
    qtc_destroy(d);
}

static void test_thread_run(void) {
    qtc_obj th = qtc_new_overridable(S("QThread"));
    CHECK(th != 0);
    CHECK_EQ(qtc_override(th, S("run"), T_RUN), QTC_OK);
    atomic_store(&g_run_done, 0);
    CHECK_EQ(qtc_call(th, S("start")), QTC_OK);
    qtc_event e;
    int got = 0;
    for (int i = 0; i < 200 && !got; i++) {
        qtc_wait(10);
        while (qtc_poll(&e))
            if (e.tag == 77) got = 1;
    }
    CHECK(got);
    qtc_arg_int(5000);
    CHECK_EQ(qtc_call(th, S("wait(ulong)")), QTC_OK);
    CHECK_EQ(g_run_gui, 0);
    CHECK_EQ(g_run_depth, 1);
    CHECK_EQ(g_inside[1], QTC_ERR_STATE);   /* run() returns nothing */
    CHECK_EQ(qtc_destroy(th), QTC_OK);
    drain();
}

static void test_timer_scene(void) {
    qtc_obj o = qtc_new_overridable(S("QObject"));
    CHECK_EQ(qtc_override(o, S("timerEvent"), T_TIMER), QTC_OK);
    qtc_arg_int(5);
    CHECK_EQ(qtc_call(o, S("startTimer(int)")), QTC_OK);
    g_calls[T_TIMER] = 0;
    for (int i = 0; i < 100 && g_calls[T_TIMER] < 2; i++) qtc_wait(5);
    CHECK(g_calls[T_TIMER] >= 2);
    CHECK_EQ(g_last_event.kind, 0);
    CHECK_EQ(g_last_event.i0, 1);   /* QEvent::Timer */
    qtc_destroy(o);

    qtc_obj scene = qtc_new_overridable(S("QGraphicsScene"));
    CHECK(scene != 0);
    CHECK_EQ(qtc_override(scene, S("drawBackground"), T_SCENE_BG), QTC_OK);
    qtc_arg_obj(scene);
    qtc_obj view = qtc_new(S("QGraphicsView"));
    qtc_arg_int(80);
    qtc_arg_int(60);
    qtc_call(view, S("resize"));
    qtc_call(view, S("show"));
    qtct_wait(30);
    CHECK_EQ(pixel(view, 40, 30), 0x0000ff);
    qtc_destroy(view);
    qtc_destroy(scene);
    drain();
}

static void test_symbol_handler(void) {
    CHECK_EQ(qtc_set_override_handler_symbol(S("no_such_function_anywhere")), QTC_ERR_NOT_FOUND);
    CHECK_EQ(qtc_set_override_handler_symbol(S("qtc_test_override_handler")), QTC_OK);
    qtc_obj m = qtc_new_overridable(S("QAbstractListModel"));
    qtc_override(m, S("rowCount"), T_ROWS);
    g_rows = 11;
    CHECK_EQ(qtc_call(m, S("rowCount")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 11);
    CHECK_EQ(qtc_set_override_handler(NULL, NULL), QTC_OK);   /* no handler: Qt's own */
    CHECK_EQ(qtc_call(m, S("rowCount")), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 0);
    CHECK_EQ(qtc_set_override_handler(handler, NULL), QTC_OK);
    qtc_destroy(m);
    drain();
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (qtc_init(QTC_APP_WIDGETS, NOSTR) != QTC_OK) {
        fprintf(stderr, "qtc_init failed\n");
        return 2;
    }
    CHECK_EQ(qtc_set_override_handler(handler, NULL), QTC_OK);
    qtc_set_quit_on_last_window_closed(false);   /* windows come and go: qtc_wait keeps waiting */
    test_introspection();
    test_list_model();
    test_table_model();
    test_widget();
    test_validator_highlighter();
    test_device();
    test_thread_run();
    test_timer_scene();
    test_symbol_handler();
    CHECK_EQ(qtc_shutdown(), QTC_OK);
    printf("test_override: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
