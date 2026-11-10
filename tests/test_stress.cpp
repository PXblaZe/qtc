// Stress and edge cases: handle churn, generation reuse, event floods, deep trees,
// many connections, cross-thread floods, big payloads. Each case has a time budget,
// so a performance cliff fails the test just like a wrong result.
#include "qtc.h"

#include <QtCore/QByteArray>
#include <QtCore/QElapsedTimer>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

static int g_fail = 0;
#define CHECK(c)                                                                                              \
    do {                                                                                                      \
        if (!(c)) {                                                                                           \
            g_fail++;                                                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s [%.*s]\n", __FILE__, __LINE__, #c, int(qtc_last_error_message().len), \
                         qtc_last_error_message().data);                                                      \
        }                                                                                                     \
    } while (0)

// The budgets are for an optimized build. Sanitizers slow qtc's own code down several times
// (Qt's libraries are not instrumented), so a sanitizer build gets four times the time.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define QTC_STRESS_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define QTC_STRESS_SANITIZED 1
#endif
#endif
#ifdef QTC_STRESS_SANITIZED
static const double kBudgetScale = 4.0;
#else
static const double kBudgetScale = 1.0;
#endif

struct Timer {
    const char *name;
    double budget_ms;
    QElapsedTimer t;
    Timer(const char *n, double b) : name(n), budget_ms(b * kBudgetScale) { t.start(); }
    ~Timer() {
        double ms = t.nsecsElapsed() / 1e6;
        bool ok = ms <= budget_ms;
        std::printf("  %-52s %9.1f ms  (budget %.0f)%s\n", name, ms, budget_ms, ok ? "" : "  OVER BUDGET");
        if (!ok) g_fail++;
    }
};

static qtc_str S(const char *s) { return qtc_s(s); }

int main() {
    if (qtc_init(QTC_APP_WIDGETS, qtc_sn("stress\0-platform\0offscreen", 27)) != QTC_OK) return 2;
    qtc_set_quit_on_last_window_closed(false);
    std::printf("stress (Qt %s):\n", qtc_qt_version().data);

    {
        Timer t("1,000,000 value handles created and freed", 4000);
        for (int i = 0; i < 1000000; i++) {
            qtc_val v = qtc_val_int(i);
            if (!v || qtc_val_free(v) != QTC_OK) { CHECK(false); break; }
        }
        CHECK(qtc_live_values() == 0);
    }
    {
        Timer t("200,000 live values at once, then freed", 3000);
        std::vector<qtc_val> vs(200000);
        for (auto &v : vs) v = qtc_val_str(S("payload"));
        CHECK(qtc_live_values() == 200000);
        for (auto &v : vs) qtc_val_free(v);
        CHECK(qtc_live_values() == 0);
        // every old handle is stale, never reused for a new value
        qtc_val fresh = qtc_val_int(1);
        int stale_ok = 0;
        for (auto &v : vs) stale_ok += (!qtc_alive(v) && v != fresh);
        CHECK(stale_ok == 200000);
        qtc_val_free(fresh);
    }
    {
        Timer t("100,000 QObjects created and destroyed", 8000);
        for (int round = 0; round < 10; round++) {
            std::vector<qtc_obj> os(10000);
            for (auto &o : os) o = qtc_new(S("QObject"));
            for (auto &o : os) qtc_destroy(o);
            qtc_process_events();
        }
        CHECK(qtc_live_objects() <= 2);
    }
    {
        Timer t("widget tree 1,000 deep, destroyed from the root", 5000);
        qtc_obj root = qtc_new(S("QWidget"));
        qtc_obj p = root;
        std::vector<qtc_obj> chain;
        for (int i = 0; i < 1000; i++) {
            qtc_arg_obj(p);
            p = qtc_new(S("QWidget"));
            chain.push_back(p);
        }
        CHECK(p != 0);
        qtc_destroy(root);
        qtc_process_events();
        int dead = 0;
        for (auto o : chain) dead += !qtc_alive(o);
        CHECK(dead == 1000);
    }
    {
        Timer t("1,000,000 emissions through a coalesced connection", 6000);
        qtc_obj s = qtc_new(S("QSlider"));
        qtc_set_int(s, S("maximum"), 2000000);
        qtc_connect(s, S("valueChanged"), 1, QTC_CONN_COALESCE);
        for (int i = 1; i <= 1000000; i++) qtc_set_int(s, S("value"), i);
        CHECK(qtc_pending() == 1);
        qtc_event e;
        CHECK(qtc_poll(&e) && e.count == 1000000 && e.i0 == 1000000);
        qtc_destroy(s);
        qtc_process_events();
    }
    {
        Timer t("200,000 queued signal events captured and polled", 6000);
        qtc_obj b = qtc_new(S("QPushButton"));
        qtc_connect(b, S("clicked"), 2, 0);
        for (int i = 0; i < 200000; i++) qtc_call(b, S("click"));
        CHECK(qtc_pending() == 200000);
        qtc_event e;
        int n = 0;
        while (qtc_poll(&e)) n++;
        CHECK(n == 200000);
        qtc_destroy(b);
        qtc_process_events();
    }
    {
        Timer t("20,000 connections on one object, then disconnected", 4000);
        qtc_obj b = qtc_new(S("QPushButton"));
        std::vector<uint64_t> cs;
        for (int i = 0; i < 20000; i++) cs.push_back(qtc_connect(b, S("clicked"), uint64_t(i), QTC_CONN_NO_ARGS));
        qtc_call(b, S("click"));
        CHECK(qtc_pending() == 20000);
        qtc_event e;
        while (qtc_poll(&e)) {}
        for (auto c : cs) CHECK(qtc_disconnect(c) == QTC_OK);
        qtc_call(b, S("click"));
        CHECK(qtc_pending() == 0);
        qtc_destroy(b);
        qtc_process_events();
        qtc_process_events();
    }
    {
        Timer t("8 threads x 100,000 posts, consumed concurrently", 15000);
        std::atomic<int> done{0};
        std::vector<std::thread> th;
        for (int k = 0; k < 8; k++)
            th.emplace_back([k, &done] {
                for (int i = 0; i < 100000; i++)
                    while (qtc_post(uint64_t(k), i, 0) == QTC_ERR_LIMIT) std::this_thread::yield();
                done++;
            });
        int64_t got = 0;
        while (got < 800000) {
            if (qtc_wait(5000) != QTC_WAIT_EVENTS) break;
            qtc_event e;
            while (qtc_poll(&e)) got++;
        }
        for (auto &t2 : th) t2.join();
        CHECK(got == 800000);
    }
    {
        Timer t("queue flood with a 1,000 event limit: drops counted, memory bounded", 3000);
        qtc_set_queue_limit(1000);
        int64_t d0 = qtc_dropped_events();
        for (int i = 0; i < 500000; i++) qtc_post(9, i, 0);
        CHECK(qtc_pending() == 1000);
        CHECK(qtc_dropped_events() - d0 == 499000);
        qtc_event e;
        while (qtc_poll(&e)) {}
        qtc_set_queue_limit(1000000);
    }
    {
        Timer t("64 MB string through a property and back", 4000);
        QByteArray big(64 << 20, 'z');
        qtc_obj o = qtc_new(S("QObject"));
        CHECK(qtc_set_str(o, S("objectName"), qtc_sn(big.constData(), big.size())) == QTC_OK);
        CHECK(qtc_get_str(o, S("objectName")).len == big.size());
        qtc_destroy(o);
        qtc_process_events();
    }
    {
        Timer t("list value with 1,000,000 elements", 5000);
        qtc_val l = qtc_val_list();
        qtc_val one = qtc_val_int(1);
        for (int i = 0; i < 1000000; i++) qtc_list_append(l, one);
        CHECK(qtc_list_len(l) == 1000000);
        qtc_val x = qtc_list_get(l, 999999);
        CHECK(x != 0);
        qtc_val_free(x);
        qtc_val_free(one);
        qtc_val_free(l);
    }
    {
        Timer t("100,000 items into a QListWidget through qtc_call", 8000);
        qtc_obj lw = qtc_new(S("QListWidget"));
        for (int i = 0; i < 100000; i++) {
            qtc_arg_str(S("item"));
            qtc_call(lw, S("addItem(QString)"));
        }
        qtc_call(lw, S("count"));
        CHECK(qtc_ret_int() == 100000);
        qtc_destroy(lw);
        qtc_process_events();
    }
    {
        Timer t("drawing 200,000 commands onto a 1024x1024 image", 6000);
        qtc_arg_int(1024);
        qtc_arg_int(1024);
        int64_t fmt = 0;
        qtc_enum_value(S("QImage::Format_ARGB32_Premultiplied"), &fmt);
        qtc_arg_int(fmt);
        qtc_val img = qtc_val_new(S("QImage"));
        CHECK(qtc_draw_begin(img) == QTC_OK);
        qtc_draw_antialias(true);
        for (int i = 0; i < 200000; i++) qtc_draw_line(i % 1024, 0, 1023 - i % 1024, 1023);
        CHECK(qtc_draw_end() == QTC_OK);
        qtc_val_free(img);
    }
    {
        Timer t("handles stay valid across 50 shutdown/init cycles", 30000);
        for (int i = 0; i < 50; i++) {
            qtc_obj w = qtc_new(S("QWidget"));
            qtc_val v = qtc_val_str(S("x"));
            CHECK(qtc_shutdown() == QTC_OK);
            CHECK(qtc_init(QTC_APP_WIDGETS, qtc_sn("stress\0-platform\0offscreen", 27)) == QTC_OK);
            CHECK(!qtc_alive(w) && !qtc_alive(v));
        }
    }
    qtc_shutdown();
    std::printf("stress: %s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
