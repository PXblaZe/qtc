/* qtc concurrency tests: promises, futures, tasks, parallel map, worker threads and queued
   calls. C against the public API only. */
#define _POSIX_C_SOURCE 200809L
#include "qtc.h"

#include "test_thread.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

#if defined(_WIN32)
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
static int64_t now_ms(void) { return (int64_t)GetTickCount64(); }
#else
static void sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}
static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif
static int spin_until(atomic_int *v, int want, int timeout_ms) {
    int64_t end = now_ms() + timeout_ms;
    while (atomic_load(v) != want) {
        if (now_ms() > end) return 0;
        qtc_wait(1);
    }
    return 1;
}

/* Pumps events until one of `kind` with `tag` arrives; counts every event of that tag by kind. */
typedef struct {
    int n[64];
    qtc_event last[64];
} Seen;
static int pump_until(uint64_t tag, int32_t kind, int timeout_ms, Seen *seen) {
    int64_t end = now_ms() + timeout_ms;
    qtc_event e;
    for (;;) {
        while (qtc_poll(&e)) {
            if (e.tag != tag) continue;
            if (seen && e.kind >= 0 && e.kind < 64) {
                seen->n[e.kind]++;
                seen->last[e.kind] = e;
            }
            if (e.kind == kind) return 1;
        }
        int64_t left = end - now_ms();
        if (left <= 0) return 0;
        qtc_wait(left > 50 ? 50 : left);
    }
}
static void drain(void) {
    qtc_event e;
    qtc_process_events();
    while (qtc_poll(&e)) {}
    qtc_clear_error();
}

static atomic_int g_cleanups, g_ran, g_release, g_processed;
static void count_cleanup(void *u) { (void)u; atomic_fetch_add(&g_cleanups, 1); }

/* ------------------------------------------------------------------ tests */

static void sum_task(qtc_future f, void *user) {
    int64_t n = (int64_t)(intptr_t)user, s = 0;
    qtc_promise_progress_range(f, 0, (int32_t)n);
    for (int64_t i = 1; i <= n; i++) {
        s += i;
        if (i % 100 == 0) qtc_promise_progress(f, (int32_t)i, NOSTR);
    }
    qtc_promise_add_int(f, s);
}

static void test_before_init(void) {
    CHECK_EQ(qtc_promise_new(), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_NOT_INITIALIZED);
    CHECK_EQ(qtc_task_start(sum_task, NULL, NULL), 0);
    CHECK_EQ(qtc_task_map(10, NULL, NULL, NULL), 0);
    CHECK_EQ(qtc_future_state(1), QTC_ERR_NOT_INITIALIZED);
    CHECK(qtc_promise_canceled(1));
    CHECK_EQ(qtc_future_release(1), QTC_ERR_NOT_INITIALIZED);
    CHECK_EQ(qtc_live_futures(), 0);
}

static void test_promise_basics(void) {
    int64_t base = qtc_live_futures();
    qtc_future f = qtc_promise_new();
    CHECK(f != 0);
    CHECK_EQ(qtc_live_futures(), base + 1);
    int32_t st = qtc_future_state(f);
    CHECK((st & QTC_FUT_STARTED) && (st & QTC_FUT_RUNNING) && !(st & QTC_FUT_FINISHED));
    CHECK_EQ(qtc_promise_add_bool(f, true), QTC_OK);
    CHECK_EQ(qtc_promise_add_int(f, -5), QTC_OK);
    CHECK_EQ(qtc_promise_add_double(f, 2.5), QTC_OK);
    CHECK_EQ(qtc_promise_add_str(f, S("h\xc3\xa9llo")), QTC_OK);
    CHECK_EQ(qtc_promise_add_bytes(f, qtc_sn("\0\1", 2)), QTC_OK);
    const double xs[3] = {1, 2, 3};
    CHECK_EQ(qtc_promise_add_f64s(f, (qtc_f64s){xs, 3}), QTC_OK);
    CHECK_EQ(qtc_promise_add_json(f, S("{\"a\":[1,2]}")), QTC_OK);
    CHECK_EQ(qtc_promise_add_json(f, S("[1,\"x\"]")), QTC_OK);
    CHECK_EQ(qtc_promise_add_json(f, S("{bad")), QTC_ERR_INVALID_ARG);
    CHECK_EQ(qtc_promise_add_json(f, S("42")), QTC_ERR_INVALID_ARG);
    CHECK_EQ(qtc_promise_add_f64s(f, (qtc_f64s){NULL, 3}), QTC_ERR_INVALID_ARG);
    CHECK_EQ(qtc_promise_add_f64s(f, (qtc_f64s){xs, -1}), QTC_ERR_INVALID_ARG);
    CHECK_EQ(qtc_promise_add_str(f, qtc_sn(NULL, 4)), QTC_ERR_INVALID_ARG);
    qtc_val v = qtc_val_str(S("abc"));
    CHECK_EQ(qtc_promise_add_val(f, v), QTC_OK);
    qtc_val_free(v);
    CHECK_EQ(qtc_promise_add_val(f, v), QTC_ERR_STALE_HANDLE);
    CHECK_EQ(qtc_future_result_count(f), 9);

    CHECK_EQ(qtc_future_result(f, 0), QTC_OK);
    CHECK(qtc_ret_kind() == QTC_T_BOOL && qtc_ret_bool());
    CHECK_EQ(qtc_future_result(f, 1), QTC_OK);
    CHECK(qtc_ret_kind() == QTC_T_INT && qtc_ret_int() == -5);
    CHECK_EQ(qtc_future_result(f, 2), QTC_OK);
    CHECK(qtc_ret_kind() == QTC_T_DOUBLE && qtc_ret_double() == 2.5);
    CHECK_EQ(qtc_future_result(f, 3), QTC_OK);
    CHECK_STR(qtc_ret_str(), "h\xc3\xa9llo");
    CHECK_EQ(qtc_future_result(f, 4), QTC_OK);
    CHECK(qtc_ret_kind() == QTC_T_BYTES && qtc_ret_str().len == 2);
    CHECK_EQ(qtc_future_result(f, 5), QTC_OK);
    CHECK_EQ(qtc_ret_kind(), QTC_T_LIST);
    qtc_val l = qtc_ret_val();
    CHECK_EQ(qtc_list_len(l), 3);
    qtc_val_free(l);
    CHECK_EQ(qtc_future_result(f, 6), QTC_OK);
    CHECK_EQ(qtc_ret_kind(), QTC_T_MAP);
    CHECK_EQ(qtc_future_result(f, 7), QTC_OK);
    CHECK_EQ(qtc_ret_kind(), QTC_T_LIST);
    CHECK_EQ(qtc_future_result(f, 8), QTC_OK);
    CHECK_STR(qtc_ret_str(), "abc");
    CHECK_EQ(qtc_future_result(f, 9), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_future_result(f, -1), QTC_ERR_OUT_OF_RANGE);

    CHECK_EQ(qtc_promise_progress_range(f, 5, 1), QTC_ERR_INVALID_ARG);
    CHECK_EQ(qtc_promise_progress_range(f, 0, 10), QTC_OK);
    CHECK_EQ(qtc_promise_progress(f, 3, S("three")), QTC_OK);
    int32_t pv = -1, pmin = -1, pmax = -1;
    CHECK_EQ(qtc_future_progress(f, &pv, &pmin, &pmax), QTC_OK);
    CHECK(pv == 3 && pmin == 0 && pmax == 10);
    CHECK_EQ(qtc_future_progress(f, NULL, NULL, NULL), QTC_OK);
    CHECK_EQ(qtc_future_wait(f, 0), QTC_WAIT_TIMEOUT);

    CHECK_EQ(qtc_promise_finish(f), QTC_OK);
    CHECK(qtc_future_state(f) & QTC_FUT_FINISHED);
    CHECK(!(qtc_future_state(f) & (QTC_FUT_CANCELED | QTC_FUT_FAILED)));
    CHECK_EQ(qtc_promise_add_int(f, 1), QTC_ERR_STATE);
    CHECK_EQ(qtc_promise_finish(f), QTC_ERR_STATE);
    CHECK_EQ(qtc_promise_fail(f, S("late")), QTC_ERR_STATE);
    CHECK_EQ(qtc_promise_progress(f, 4, NOSTR), QTC_ERR_STATE);
    CHECK_EQ(qtc_future_wait(f, 0), QTC_WAIT_FINISHED);
    CHECK_EQ(qtc_future_wait(f, -1), QTC_WAIT_FINISHED);
    CHECK_EQ(qtc_future_error(f).len, 0);
    CHECK_EQ(qtc_future_result_count(f), 9);

    CHECK_EQ(qtc_future_release(f), QTC_OK);
    CHECK_EQ(qtc_live_futures(), base);
    CHECK_EQ(qtc_future_state(f), QTC_ERR_STALE_HANDLE);
    CHECK_EQ(qtc_future_release(f), QTC_ERR_STALE_HANDLE);
    CHECK_EQ(qtc_future_release(0), QTC_ERR_NULL_HANDLE);
    CHECK_EQ(qtc_future_release((uint64_t)1 << 40), QTC_ERR_BAD_HANDLE);
    CHECK_EQ(qtc_promise_add_int(f, 1), QTC_ERR_STALE_HANDLE);
    CHECK(qtc_promise_canceled(f));
    CHECK_EQ(qtc_future_result(f, 0), QTC_ERR_STALE_HANDLE);
    CHECK_EQ(qtc_future_cancel(f), QTC_ERR_STALE_HANDLE);
    CHECK_EQ(qtc_future_wait(f, 0), QTC_ERR_STALE_HANDLE);
    CHECK_EQ(qtc_future_error(f).len, 0);
}

static void test_promise_fail_cancel(void) {
    qtc_future f = qtc_promise_new();
    CHECK_EQ(qtc_promise_fail(f, S("boom")), QTC_OK);
    int32_t st = qtc_future_state(f);
    CHECK((st & QTC_FUT_FINISHED) && (st & QTC_FUT_FAILED) && !(st & QTC_FUT_CANCELED));
    CHECK_STR(qtc_future_error(f), "boom");
    CHECK_EQ(qtc_future_release(f), QTC_OK);

    f = qtc_promise_new();
    CHECK_EQ(qtc_promise_fail(f, NOSTR), QTC_OK);
    CHECK_STR(qtc_future_error(f), "failed");
    qtc_future_release(f);

    f = qtc_promise_new();
    CHECK(!qtc_promise_canceled(f));
    CHECK_EQ(qtc_future_cancel(f), QTC_OK);
    CHECK(qtc_promise_canceled(f));
    CHECK(qtc_future_state(f) & QTC_FUT_CANCELED);
    CHECK(!(qtc_future_state(f) & QTC_FUT_FINISHED));   /* the producer still finishes it */
    CHECK_EQ(qtc_promise_add_int(f, 1), QTC_OK);        /* accepted and dropped, as Qt does */
    CHECK_EQ(qtc_future_result_count(f), 0);
    CHECK_EQ(qtc_promise_finish(f), QTC_OK);
    CHECK_EQ(qtc_future_wait(f, 0), QTC_WAIT_FINISHED);
    qtc_future_release(f);

    /* releasing an unfinished future cancels and finishes it */
    f = qtc_promise_new();
    qtc_future g = qtc_future_all(f, f);
    CHECK(g != 0);
    CHECK_EQ(qtc_future_release(f), QTC_OK);
    CHECK_EQ(qtc_future_wait(g, 2000), QTC_WAIT_FINISHED);
    qtc_future_release(g);
}

static void test_watch(void) {
    drain();
    qtc_future f = qtc_promise_new();
    uint64_t w = qtc_future_watch(f, 77);
    CHECK(w != 0);
    qtc_promise_progress_range(f, 0, 100);
    qtc_promise_progress(f, 10, S("ten"));
    qtc_promise_add_int(f, 1);
    qtc_promise_add_int(f, 2);
    qtc_promise_finish(f);
    Seen s;
    memset(&s, 0, sizeof s);
    CHECK(pump_until(77, QTC_EV_FUTURE_FINISHED, 3000, &s));
    CHECK_EQ(s.n[QTC_EV_FUTURE_STARTED], 1);
    CHECK(s.n[QTC_EV_FUTURE_RESULTS] >= 1);
    int64_t got = 0;
    (void)got;
    qtc_event fin = s.last[QTC_EV_FUTURE_FINISHED];
    CHECK_EQ(fin.source, f);
    CHECK_EQ(fin.id, w);
    CHECK(fin.i0 & QTC_FUT_FINISHED);
    CHECK_EQ(fin.i1, 2);
    CHECK_EQ(qtc_future_unwatch(w), QTC_ERR_STALE_HANDLE);   /* it ended with FINISHED */

    /* results events add up to the results */
    qtc_future f2 = qtc_promise_new();
    uint64_t w2 = qtc_future_watch(f2, 78);
    for (int i = 0; i < 50; i++) qtc_promise_add_int(f2, i);
    qtc_promise_fail(f2, S("bad luck"));
    memset(&s, 0, sizeof s);
    int64_t total = 0;
    qtc_event e;
    int64_t end = now_ms() + 3000;
    int done = 0;
    while (!done && now_ms() < end) {
        qtc_wait(20);
        while (qtc_poll(&e)) {
            if (e.tag != 78) continue;
            if (e.kind == QTC_EV_FUTURE_RESULTS) total += e.i1;
            if (e.kind == QTC_EV_FUTURE_FINISHED) {
                done = 1;
                CHECK(e.i0 & QTC_FUT_FAILED);
                CHECK_STR(qtc_event_text(e.seq), "bad luck");
            }
        }
    }
    CHECK(done);
    CHECK_EQ(total, 50);
    (void)w2;
    qtc_future_release(f2);

    /* a watch on a finished future replays it; unwatch before the end */
    uint64_t w3 = qtc_future_watch(f, 79);
    memset(&s, 0, sizeof s);
    CHECK(pump_until(79, QTC_EV_FUTURE_FINISHED, 3000, &s));
    qtc_future f3 = qtc_promise_new();
    uint64_t w4 = qtc_future_watch(f3, 80);
    CHECK_EQ(qtc_future_unwatch(w4), QTC_OK);
    CHECK_EQ(qtc_future_unwatch(w4), QTC_ERR_STALE_HANDLE);
    CHECK_EQ(qtc_future_unwatch(0), QTC_ERR_NULL_HANDLE);
    qtc_promise_finish(f3);
    memset(&s, 0, sizeof s);
    CHECK(!pump_until(80, QTC_EV_FUTURE_FINISHED, 200, &s));
    CHECK_EQ(qtc_future_watch(0, 1), 0);
    CHECK_EQ(qtc_future_watch((uint64_t)1 << 40, 1), 0);
    (void)w3;
    qtc_future_release(f);
    qtc_future_release(f3);
    drain();
}

static void test_tasks(void) {
    enum { N = 200 };
    qtc_future fs[N];
    atomic_store(&g_cleanups, 0);
    for (int i = 0; i < N; i++) {
        fs[i] = qtc_task_start(sum_task, (void *)(intptr_t)(1000 + i), count_cleanup);
        CHECK(fs[i] != 0);
    }
    for (int i = 0; i < N; i++) {
        CHECK_EQ(qtc_future_wait(fs[i], 10000), QTC_WAIT_FINISHED);
        CHECK_EQ(qtc_future_result_count(fs[i]), 1);
        CHECK_EQ(qtc_future_result(fs[i], 0), QTC_OK);
        int64_t n = 1000 + i;
        CHECK_EQ(qtc_ret_int(), n * (n + 1) / 2);
        CHECK(!(qtc_future_state(fs[i]) & QTC_FUT_CANCELED));
        CHECK_EQ(qtc_future_release(fs[i]), QTC_OK);
    }
    CHECK(spin_until(&g_cleanups, N, 5000));
    CHECK_EQ(qtc_task_start(NULL, NULL, NULL), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_INVALID_ARG);
}

static void blocker(qtc_future f, void *u) {
    (void)u;
    while (!atomic_load(&g_release) && !qtc_promise_canceled(f)) sleep_ms(1);
}
static void counting(qtc_future f, void *u) {
    (void)f;
    (void)u;
    atomic_fetch_add(&g_ran, 1);
}

static void test_pool(void) {
    qtc_obj pool = qtc_new(S("QThreadPool"));
    CHECK(pool != 0);
    qtc_arg_int(1);
    CHECK_EQ(qtc_call(pool, S("setMaxThreadCount")), QTC_OK);
    qtc_arg_int(5000);
    CHECK_EQ(qtc_call(pool, S("setMaxThreadCount")), QTC_ERR_LIMIT);
    qtc_arg_int(1000);
    CHECK_EQ(qtc_call(pool, S("setStackSize")), QTC_ERR_OUT_OF_RANGE);

    /* a canceled task that has not started finishes without running */
    atomic_store(&g_release, 0);
    atomic_store(&g_ran, 0);
    atomic_store(&g_cleanups, 0);
    qtc_future f1 = qtc_task_start_in(pool, 0, blocker, NULL, count_cleanup);
    qtc_future f2 = qtc_task_start_in(pool, 0, counting, NULL, count_cleanup);
    CHECK(f1 && f2);
    sleep_ms(30);
    CHECK(qtc_future_state(f1) & QTC_FUT_RUNNING);
    CHECK_EQ(qtc_future_cancel(f2), QTC_OK);
    atomic_store(&g_release, 1);
    CHECK_EQ(qtc_future_wait(f2, 5000), QTC_WAIT_FINISHED);
    CHECK(qtc_future_state(f2) & QTC_FUT_CANCELED);
    CHECK_EQ(atomic_load(&g_ran), 0);
    CHECK_EQ(qtc_future_wait(f1, 5000), QTC_WAIT_FINISHED);
    CHECK(spin_until(&g_cleanups, 2, 5000));
    qtc_future_release(f1);
    qtc_future_release(f2);

    /* QThreadPool::clear drops queued tasks: their futures finish canceled, cleanups run */
    atomic_store(&g_release, 0);
    atomic_store(&g_cleanups, 0);
    qtc_future b = qtc_task_start_in(pool, 0, blocker, NULL, count_cleanup);
    sleep_ms(30);
    qtc_future q1 = qtc_task_start_in(pool, 0, counting, NULL, count_cleanup);
    qtc_future q2 = qtc_task_start_in(pool, 5, counting, NULL, count_cleanup);
    CHECK_EQ(qtc_call(pool, S("clear")), QTC_OK);
    CHECK_EQ(atomic_load(&g_cleanups), 2);
    CHECK_EQ(qtc_future_wait(q1, 0), QTC_WAIT_FINISHED);
    CHECK_EQ(qtc_future_wait(q2, 0), QTC_WAIT_FINISHED);
    CHECK(qtc_future_state(q1) & QTC_FUT_CANCELED);
    CHECK_EQ(atomic_load(&g_ran), 0);
    atomic_store(&g_release, 1);
    CHECK_EQ(qtc_future_wait(b, 5000), QTC_WAIT_FINISHED);
    CHECK(spin_until(&g_cleanups, 3, 5000));
    qtc_future_release(b);
    qtc_future_release(q1);
    qtc_future_release(q2);

    /* the global pool (0), wrong pools, the global pool is Qt's */
    qtc_future g = qtc_task_start_in(0, 0, counting, NULL, NULL);
    CHECK(g != 0);
    CHECK_EQ(qtc_future_wait(g, 5000), QTC_WAIT_FINISHED);
    qtc_future_release(g);
    qtc_obj timer = qtc_new(S("QTimer"));
    CHECK_EQ(qtc_task_start_in(timer, 0, counting, NULL, NULL), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_WRONG_TYPE);
    CHECK_EQ(qtc_task_start_in(pool, 0, NULL, NULL, NULL), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_INVALID_ARG);
    CHECK_EQ(qtc_call_static(S("QThreadPool"), S("globalInstance")), QTC_OK);
    qtc_obj gp = qtc_ret_obj();
    CHECK(gp != 0);
    CHECK_EQ(qtc_destroy(gp), QTC_ERR_STATE);
    CHECK_EQ(qtc_destroy(timer), QTC_OK);
    CHECK_EQ(qtc_destroy(pool), QTC_OK);
    drain();
    CHECK(!qtc_alive(pool));
}

typedef struct {
    int64_t *arr;
    int sleep;
} MapArg;
static void square(int64_t i, void *u) {
    MapArg *m = u;
    m->arr[i] = i * 2;
    atomic_fetch_add(&g_processed, 1);
    if (m->sleep) sleep_ms(m->sleep);
}

static void test_map(void) {
    enum { N = 1000000 };
    MapArg m = {calloc(N, sizeof(int64_t)), 0};
    atomic_store(&g_processed, 0);
    atomic_store(&g_cleanups, 0);
    qtc_future f = qtc_task_map(N, square, &m, count_cleanup);
    CHECK(f != 0);
    CHECK_EQ(qtc_future_wait(f, 30000), QTC_WAIT_FINISHED);
    CHECK(!(qtc_future_state(f) & QTC_FUT_CANCELED));
    int bad = 0;
    for (int64_t i = 0; i < N; i++)
        if (m.arr[i] != i * 2) bad++;
    CHECK_EQ(bad, 0);
    CHECK_EQ(atomic_load(&g_processed), N);
    int32_t v, lo, hi;
    CHECK_EQ(qtc_future_progress(f, &v, &lo, &hi), QTC_OK);
    CHECK(lo == 0 && hi > 0 && v == hi);
    CHECK(spin_until(&g_cleanups, 1, 5000));
    qtc_future_release(f);

    f = qtc_task_map(0, square, &m, count_cleanup);
    CHECK(f != 0);
    CHECK_EQ(qtc_future_wait(f, 5000), QTC_WAIT_FINISHED);
    CHECK(spin_until(&g_cleanups, 2, 5000));
    qtc_future_release(f);
    CHECK_EQ(qtc_task_map(-1, square, &m, NULL), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_OUT_OF_RANGE);
    CHECK_EQ(qtc_task_map(5, NULL, &m, NULL), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_INVALID_ARG);

    /* cancel stops it between chunks */
    atomic_store(&g_processed, 0);
    MapArg slow = {m.arr, 1};
    f = qtc_task_map(4000, square, &slow, count_cleanup);
    sleep_ms(40);
    CHECK_EQ(qtc_future_cancel(f), QTC_OK);
    CHECK_EQ(qtc_future_wait(f, 10000), QTC_WAIT_FINISHED);
    CHECK(qtc_future_state(f) & QTC_FUT_CANCELED);
    CHECK(atomic_load(&g_processed) < 4000);
    CHECK(spin_until(&g_cleanups, 3, 5000));
    qtc_future_release(f);

    /* suspend pauses it between chunks; resume finishes it */
    atomic_store(&g_processed, 0);
    f = qtc_task_map(600, square, &slow, NULL);
    sleep_ms(20);
    CHECK_EQ(qtc_future_suspend(f, true), QTC_OK);
    /* the chunks already running finish first, which on a machine with many threads (or
       Windows, where a 1 ms sleep takes a timer tick) can take a while: wait until no more
       items get done, then check that none do */
    int p1 = -1, p2 = atomic_load(&g_processed);
    for (int waited = 0; waited < 10000 && p1 != p2; waited += 150) {
        p1 = p2;
        sleep_ms(150);
        p2 = atomic_load(&g_processed);
    }
    CHECK(qtc_future_state(f) & QTC_FUT_SUSPENDED);
    CHECK_EQ(p1, p2);
    CHECK(p1 < 600);
    CHECK_EQ(qtc_future_suspend(f, false), QTC_OK);
    CHECK_EQ(qtc_future_wait(f, 20000), QTC_WAIT_FINISHED);
    CHECK_EQ(atomic_load(&g_processed), 600);
    CHECK(!(qtc_future_state(f) & QTC_FUT_CANCELED));
    qtc_future_release(f);
    free(m.arr);
}

static void *producer(void *arg) {
    qtc_future f = *(qtc_future *)arg;
    for (int i = 0; i < 1000; i++) qtc_promise_add_int(f, i);
    qtc_promise_finish(f);
    return NULL;
}
static void *adder(void *arg) {
    qtc_future f = *(qtc_future *)arg;
    for (int i = 0; i < 1000; i++) qtc_promise_add_int(f, i);
    return NULL;
}
typedef struct {
    qtc_future f;
    int64_t timeout;
    int32_t got;
    qtc_status worker_result, worker_add_val, worker_suspend;
} WaitArg;
static void *waiter(void *arg) {
    WaitArg *w = arg;
    w->worker_result = qtc_future_result(w->f, 0);
    w->worker_add_val = qtc_promise_add_val(w->f, 1);
    w->worker_suspend = qtc_promise_suspend_if_requested(w->f);
    w->got = qtc_future_wait(w->f, w->timeout);
    return NULL;
}

static void test_threads(void) {
    qtc_future fs[4];
    pthread_t th[4];
    for (int i = 0; i < 4; i++) {
        fs[i] = qtc_promise_new();
        pthread_create(&th[i], NULL, producer, &fs[i]);
    }
    for (int i = 0; i < 4; i++) {
        CHECK_EQ(qtc_future_wait(fs[i], -1), QTC_WAIT_FINISHED);
        CHECK_EQ(qtc_future_result_count(fs[i]), 1000);
        int ordered = 1;
        for (int k = 0; k < 1000; k += 97) {
            qtc_future_result(fs[i], k);
            if (qtc_ret_int() != k) ordered = 0;
        }
        CHECK(ordered);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
        qtc_future_release(fs[i]);
    }

    /* many threads, one promise */
    qtc_future one = qtc_promise_new();
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, adder, &one);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    CHECK_EQ(qtc_future_result_count(one), 4000);
    qtc_promise_finish(one);
    qtc_future_release(one);

    /* a worker waits for a future the GUI thread finishes; GUI-thread-only calls fail there */
    WaitArg w = {qtc_promise_new(), -1, -1, 0, 0, 0};
    pthread_t wt;
    pthread_create(&wt, NULL, waiter, &w);
    sleep_ms(50);
    qtc_promise_add_int(w.f, 7);
    qtc_promise_finish(w.f);
    pthread_join(wt, NULL);
    CHECK_EQ(w.got, QTC_WAIT_FINISHED);
    CHECK_EQ(w.worker_result, QTC_ERR_WRONG_THREAD);
    CHECK_EQ(w.worker_add_val, QTC_ERR_WRONG_THREAD);
    CHECK_EQ(w.worker_suspend, QTC_OK);
    qtc_future_release(w.f);

    WaitArg w2 = {qtc_promise_new(), 30, -1, 0, 0, 0};
    int64_t t0 = now_ms();
    pthread_create(&wt, NULL, waiter, &w2);
    pthread_join(wt, NULL);
    CHECK_EQ(w2.got, QTC_WAIT_TIMEOUT);
    CHECK(now_ms() - t0 >= 25);
    qtc_future_release(w2.f);

    CHECK_EQ(qtc_promise_suspend_if_requested(qtc_promise_new()), QTC_ERR_WRONG_THREAD);   /* on the GUI thread */
}

static void *late_finisher(void *arg) {
    sleep_ms(40);
    qtc_promise_finish(*(qtc_future *)arg);
    return NULL;
}

static void test_combine_and_wait(void) {
    qtc_future a = qtc_promise_new(), b = qtc_promise_new();
    qtc_future g = qtc_future_all(a, b);
    CHECK(g != 0);
    qtc_promise_finish(a);
    CHECK_EQ(qtc_future_wait(g, 50), QTC_WAIT_TIMEOUT);
    qtc_promise_finish(b);
    CHECK_EQ(qtc_future_wait(g, 2000), QTC_WAIT_FINISHED);

    qtc_future c = qtc_promise_new(), d = qtc_promise_new();
    qtc_future h = qtc_future_any(c, d);
    CHECK(h != 0);
    qtc_promise_finish(d);
    CHECK_EQ(qtc_future_wait(h, 2000), QTC_WAIT_FINISHED);
    CHECK_EQ(qtc_future_result(h, 0), QTC_OK);
    CHECK_EQ(qtc_ret_uint(), d);
    CHECK_EQ(qtc_future_all(a, 0), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_NULL_HANDLE);
    qtc_future_release(c);
    CHECK_EQ(qtc_future_any(c, d), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_STALE_HANDLE);
    qtc_future_release(a);
    qtc_future_release(b);
    qtc_future_release(d);
    qtc_future_release(g);
    qtc_future_release(h);

    /* the GUI thread keeps Qt running while it waits; a worker finishes it */
    qtc_future f = qtc_promise_new();
    int64_t t0 = now_ms();
    CHECK_EQ(qtc_future_wait(f, 50), QTC_WAIT_TIMEOUT);
    CHECK(now_ms() - t0 >= 45);
    pthread_t t;
    pthread_create(&t, NULL, late_finisher, &f);
    CHECK_EQ(qtc_future_wait(f, 5000), QTC_WAIT_FINISHED);
    pthread_join(t, NULL);
    qtc_future_release(f);
}

static void until_canceled(qtc_future f, void *u) {
    (void)u;
    while (!qtc_promise_canceled(f)) sleep_ms(1);
}

static void test_release_running(void) {
    atomic_store(&g_cleanups, 0);
    qtc_future f = qtc_task_start(until_canceled, NULL, count_cleanup);
    sleep_ms(20);
    CHECK(qtc_future_state(f) & QTC_FUT_RUNNING);
    CHECK_EQ(qtc_future_release(f), QTC_OK);
    CHECK(spin_until(&g_cleanups, 1, 5000));
}

static qtc_obj qml_object(const char *src) {
    qtc_obj eng = qtc_new(S("QQmlEngine"));
    qtc_arg_obj(eng);
    qtc_obj comp = qtc_new(S("QQmlComponent"));
    qtc_arg_bytes(qtc_s(src));
    qtc_arg_val(qtc_val_str(S("file:///inline.qml")));
    if (qtc_call(comp, S("setData")) != QTC_OK || qtc_call(comp, S("create()")) != QTC_OK) return 0;
    qtc_obj o = qtc_ret_obj();
    if (o) qtc_adopt(o);
    return o;
}

static void test_invoke(void) {
    if (!qtc_class_exists(S("QQmlEngine"))) {
        fprintf(stderr, "skip: QML module not bound\n");
        return;
    }
    drain();
    qtc_obj o = qml_object("import QtQml\nQtObject { objectName: \"q\"; signal ping(int x);"
                           " function add(a, b) { return a + b }"
                           " function name(x) { return x.objectName }"
                           " function w(r) { return r.width } }");
    CHECK(o != 0);
    if (!o) return;
    qtc_arg_int(2);
    qtc_arg_int(40);
    qtc_future f = qtc_invoke_future(o, S("add"));
    CHECK(f != 0);
    CHECK_EQ(qtc_future_wait(f, 0), QTC_WAIT_TIMEOUT);   /* queued: runs from the event loop */
    CHECK_EQ(qtc_future_wait(f, 3000), QTC_WAIT_FINISHED);
    CHECK_EQ(qtc_future_result(f, 0), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 42);
    qtc_future_release(f);

    /* an object argument in a QVariant parameter is looked up when the call runs */
    qtc_obj t = qtc_new(S("QTimer"));
    qtc_set_str(t, S("objectName"), S("tick"));
    qtc_arg_obj(t);
    f = qtc_invoke_future(o, S("name"));
    CHECK_EQ(qtc_future_wait(f, 3000), QTC_WAIT_FINISHED);
    CHECK_EQ(qtc_future_result(f, 0), QTC_OK);
    CHECK_STR(qtc_ret_str(), "tick");
    qtc_future_release(f);
    qtc_arg_obj(t);
    f = qtc_invoke_future(o, S("name"));
    qtc_destroy(t);
    CHECK_EQ(qtc_future_wait(f, 3000), QTC_WAIT_FINISHED);
    CHECK(qtc_future_state(f) & QTC_FUT_FAILED);
    qtc_future_release(f);

    /* a large value in a QVariant parameter */
    qtc_arg_double(0);
    qtc_arg_double(0);
    qtc_arg_double(30);
    qtc_arg_double(40);
    qtc_val r = qtc_val_new(S("QRectF"));
    qtc_arg_val(r);
    f = qtc_invoke_future(o, S("w"));
    CHECK_EQ(qtc_future_wait(f, 3000), QTC_WAIT_FINISHED);
    qtc_future_result(f, 0);
    CHECK(qtc_ret_double() == 30.0);
    qtc_future_release(f);
    qtc_val_free(r);

    /* a queued signal */
    uint64_t c = qtc_connect(o, S("ping"), 91, 0);
    CHECK(c != 0);
    qtc_arg_int(5);
    CHECK_EQ(qtc_invoke_queued(o, S("ping")), QTC_OK);
    Seen s;
    memset(&s, 0, sizeof s);
    CHECK(pump_until(91, QTC_EV_SIGNAL, 3000, &s));
    CHECK_EQ(s.last[QTC_EV_SIGNAL].i0, 5);

    /* destroying the object after the call was queued: the call was posted first and runs */
    qtc_obj o2 = qml_object("import QtQml\nQtObject { function one() { return 1 } }");
    f = qtc_invoke_future(o2, S("one"));
    CHECK(f != 0);
    qtc_destroy(o2);
    CHECK_EQ(qtc_future_wait(f, 3000), QTC_WAIT_FINISHED);
    CHECK(!(qtc_future_state(f) & QTC_FUT_FAILED));
    CHECK_EQ(qtc_future_result(f, 0), QTC_OK);
    CHECK_EQ(qtc_ret_int(), 1);
    qtc_future_release(f);

    /* what qtc_call reaches through the meta-object only */
    CHECK_EQ(qtc_invoke_future(o, S("nosuch")), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_NOT_FOUND);
    qtc_arg_int(1);
    CHECK_EQ(qtc_invoke_queued(o, S("add")), QTC_ERR_NOT_FOUND);   /* add takes two */
    CHECK_EQ(qtc_args_count(), 0);
    qtc_obj timer = qtc_new(S("QTimer"));
    CHECK_EQ(qtc_invoke_queued(timer, S("start")), QTC_ERR_NOT_FOUND);   /* bound classes: qtc_call */
    CHECK_EQ(qtc_invoke_queued(timer, S("deleteLater")), QTC_ERR_NOT_FOUND);
    CHECK_EQ(qtc_invoke_queued(0, S("start")), QTC_ERR_NULL_HANDLE);
    qtc_destroy(timer);
    drain();
}

static void test_workers(void) {
    drain();
    qtc_obj th = qtc_new(S("QThread"));
    CHECK(th != 0);
    CHECK_EQ(qtc_call(th, S("start")), QTC_OK);

    /* a timer started here and moved there: its timeouts come from the worker thread */
    qtc_obj timer = qtc_new(S("QTimer"));
    qtc_arg_int(5);
    CHECK_EQ(qtc_call(timer, S("start")), QTC_OK);
    uint64_t c = qtc_connect(timer, S("timeout"), 92, 0);
    CHECK(c != 0);
    CHECK_EQ(qtc_move_to_thread(timer, th), QTC_OK);
    CHECK_EQ(qtc_call(timer, S("stop")), QTC_ERR_WRONG_THREAD);
    CHECK_EQ(qtc_set_int(timer, S("interval"), 7), QTC_ERR_WRONG_THREAD);
    CHECK(qtc_alive(timer));
    Seen s;
    memset(&s, 0, sizeof s);
    CHECK(pump_until(92, QTC_EV_SIGNAL, 3000, &s));
    CHECK(pump_until(92, QTC_EV_SIGNAL, 3000, &s));
    CHECK_EQ(qtc_watch(timer, QTC_WATCH(QTC_EV_DESTROYED), 0, 1), 0);
    CHECK_EQ(qtc_last_error(), QTC_ERR_WRONG_THREAD);
    /* destroyed in its own thread; the connection goes with it */
    CHECK_EQ(qtc_destroy(timer), QTC_OK);
    CHECK(!qtc_alive(timer));
    for (int i = 0; i < 20; i++) qtc_wait(5);
    drain();
    memset(&s, 0, sizeof s);
    CHECK(!pump_until(92, QTC_EV_SIGNAL, 100, &s));
    CHECK_EQ(qtc_destroy(timer), QTC_ERR_STALE_HANDLE);

    /* refusals */
    qtc_obj w = qtc_new(S("QWidget"));
    CHECK_EQ(qtc_move_to_thread(w, th), QTC_ERR_STATE);
    qtc_obj parent = qtc_new(S("QObject"));
    qtc_arg_obj(parent);
    qtc_obj child = qtc_new(S("QObject"));
    CHECK_EQ(qtc_move_to_thread(child, th), QTC_ERR_STATE);
    qtc_obj model = qtc_new(S("QStringListModel"));
    CHECK_EQ(qtc_move_to_thread(model, th), QTC_ERR_STATE);
    CHECK_EQ(qtc_move_to_thread(th, th), QTC_ERR_STATE);
    qtc_obj watched = qtc_new(S("QObject"));
    uint64_t wid = qtc_watch(watched, QTC_WATCH(QTC_EV_DESTROYED), 0, 3);
    CHECK(wid != 0);
    CHECK_EQ(qtc_move_to_thread(watched, th), QTC_ERR_STATE);
    qtc_unwatch(wid);
    qtc_obj fwd = qtc_new(S("QObject"));
    qtc_obj receiver = qtc_new(S("QTimer"));
    uint64_t fc = qtc_connect_to(fwd, S("objectNameChanged"), receiver, S("stop"));
    CHECK(fc != 0);
    CHECK_EQ(qtc_move_to_thread(fwd, th), QTC_ERR_STATE);
    qtc_disconnect(fc);
    CHECK_EQ(qtc_move_to_thread(fwd, th), QTC_OK);
    CHECK_EQ(qtc_move_to_thread(w, timer), QTC_ERR_STALE_HANDLE);
    CHECK_EQ(qtc_move_to_thread(watched, parent), QTC_ERR_WRONG_TYPE);
    CHECK_EQ(qtc_move_to_thread(watched, 0), QTC_ERR_NULL_HANDLE);

    /* a thread that finished: objects in it are deleted directly; destroying the QThread
       brings the others back to the GUI thread */
    qtc_obj keep = qtc_new(S("QObject"));
    CHECK_EQ(qtc_move_to_thread(keep, th), QTC_OK);
    CHECK_EQ(qtc_call(keep, S("objectName")), QTC_ERR_WRONG_THREAD);
    CHECK_EQ(qtc_destroy(th), QTC_ERR_STATE);   /* still running */
    CHECK_EQ(qtc_call(th, S("quit")), QTC_OK);
    qtc_arg_int(5000);
    CHECK_EQ(qtc_call(th, S("wait(ulong)")), QTC_OK);
    CHECK(qtc_ret_bool());
    CHECK_EQ(qtc_invoke_queued(fwd, S("deleteLater")), QTC_ERR_STATE);   /* its thread has finished */
    CHECK_EQ(qtc_destroy(fwd), QTC_OK);
    CHECK(!qtc_alive(fwd));
    CHECK_EQ(qtc_destroy(th), QTC_OK);
    drain();
    CHECK(!qtc_alive(th));
    CHECK_EQ(qtc_set_str(keep, S("objectName"), S("home")), QTC_OK);
    CHECK_STR(qtc_get_str(keep, S("objectName")), "home");
    CHECK_EQ(qtc_destroy(keep), QTC_OK);
    qtc_destroy(w);
    qtc_destroy(receiver);
    qtc_destroy(parent);
    qtc_destroy(model);
    qtc_destroy(watched);
    drain();
}

static void test_shutdown_with_work(void) {
    /* tasks still running and a thread still running: qtc_shutdown cancels, stops and returns */
    atomic_store(&g_cleanups, 0);
    qtc_future f = qtc_task_start(until_canceled, NULL, count_cleanup);
    qtc_future p = qtc_promise_new();
    uint64_t w = qtc_future_watch(p, 5);
    (void)w;
    qtc_obj th = qtc_new(S("QThread"));
    qtc_call(th, S("start"));
    qtc_obj o = qtc_new(S("QObject"));
    CHECK_EQ(qtc_move_to_thread(o, th), QTC_OK);
    sleep_ms(20);
    (void)f;
    CHECK_EQ(qtc_shutdown(), QTC_OK);
    CHECK_EQ(atomic_load(&g_cleanups), 1);
    CHECK_EQ(qtc_live_futures(), 0);
    CHECK_EQ(qtc_future_state(p), QTC_ERR_NOT_INITIALIZED);
    CHECK_EQ(qtc_init(QTC_APP_WIDGETS, NOSTR), QTC_OK);
    qtc_future again = qtc_promise_new();
    CHECK(again != 0 && again != p);
    CHECK_EQ(qtc_future_state(p), QTC_ERR_STALE_HANDLE);
    qtc_future_release(again);
}

int main(void) {
    test_before_init();
    if (qtc_init(QTC_APP_WIDGETS, NOSTR) != QTC_OK) {
        fprintf(stderr, "qtc_init failed\n");
        return 2;
    }
    test_promise_basics();
    test_promise_fail_cancel();
    test_watch();
    test_tasks();
    test_pool();
    test_map();
    test_threads();
    test_combine_and_wait();
    test_release_running();
    test_invoke();
    test_workers();
    test_shutdown_with_work();
    CHECK_EQ(qtc_shutdown(), QTC_OK);
    printf("test_concurrent: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
