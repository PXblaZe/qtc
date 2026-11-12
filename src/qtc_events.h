// Helpers shared by the qtc runtime's translation units.
#pragma once
#include "qtc_internal.h"

#include <QtCore/QThread>

namespace qtc {

bool is_inited();
// the thread qtc_init ran on (it stays set after qtc_shutdown, as objects die later)
extern std::atomic<Qt::HANDLE> g_gui_thread;
inline bool gui_thread_ok() { return QThread::currentThreadId() == g_gui_thread.load(std::memory_order_relaxed); }
qtc_status last_error_code();
qtc_view view_of(const QByteArray &b);
bool read_str(qtc_str s, QString &out, const char *what);
bool read_bytes(qtc_str s, QByteArray &out, const char *what);
uint64_t handle_for_signal_arg(QObject *o);
void top_level_point();
qtc_status do_call(uint64_t receiver, qtc_str method);
uint64_t events_push(const qtc_event &e, const QByteArray &text);
bool events_watched_below(QObject *o);
bool events_forwards_below(QObject *root);
void events_mark_far(QObject *root);
void events_describe(QEvent *e, QObject *o, qtc_event *out, QByteArray *text);
int ov_depth();   // override calls in progress on this thread (qtc_override.cpp)
// Runs fn(ctx) as a call from Qt into the program, as an override handler runs: the program's
// pending arguments and last result are kept aside, deletes are deferred, the event loop is
// refused. False (fn not run) when calls are already 64 deep. GUI thread.
bool ov_run_external(void (*fn)(void *), void *ctx);
void concurrent_init();
void concurrent_shutdown();

// Runs an API call on the GUI thread after qtc_init, containing C++ exceptions.
// Status-returning calls (signed integers) return the failure's own code; others return def.
template <class R>
inline R failed_with(R def, qtc_status code) {
    if constexpr (std::is_integral_v<R> && std::is_signed_v<R> && sizeof(R) >= 4) return R(code);
    else return def;
}
template <class R, class F>
R guarded(R def, F &&f) {
    if (Q_UNLIKELY(!ready_here())) {
        if (!is_inited()) return failed_with(def, fail(QTC_ERR_NOT_INITIALIZED, "qtc_init has not been called"));
        if (!gui_thread_ok())
            return failed_with(def, fail(QTC_ERR_WRONG_THREAD, "qtc functions must be called from the thread that called qtc_init"));
    }
    try {
        return f();
    } catch (const std::exception &e) {
        return failed_with(def, fail(QTC_ERR_EXCEPTION, QString::fromUtf8(e.what())));
    } catch (...) {
        return failed_with(def, fail(QTC_ERR_EXCEPTION, "unknown C++ exception"));
    }
}

}  // namespace qtc
