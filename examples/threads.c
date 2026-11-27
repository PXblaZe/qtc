/* threads.c: worker threads reporting to the GUI through qtc_post.
 *
 *   ./threads            four workers fill a progress bar; close the window to quit
 *   ./threads --selftest checks that every report arrived, then quits
 *
 * Qt objects belong to the GUI thread: every qtc call must come from it, and a
 * call from another thread fails with QTC_ERR_WRONG_THREAD instead of racing.
 * qtc_post is the exception: any thread may call it, and the event it queues
 * wakes qtc_wait in the GUI thread.
 */
#include "qtc.h"

#include <stdio.h>
#include <string.h>

enum { WORKERS = 4, STEPS = 250 };
enum { TAG_PROGRESS = 1, TAG_DONE = 2 };

static void *work(void *arg) {
    const int64_t id = (int64_t)(intptr_t)arg;
    int64_t checksum = 0;
    for (int64_t step = 1; step <= STEPS; step++) {
        for (int k = 0; k < 2000; k++) checksum += (step * k) % 7;   /* some work */
        qtc_post(TAG_PROGRESS, id, step);
    }
    qtc_post(TAG_DONE, id, checksum);
    return NULL;
}

/* Starting and joining a thread: Win32 threads on Windows, POSIX threads elsewhere. */
#if defined(_WIN32)
#include <windows.h>
typedef HANDLE thread;
static DWORD WINAPI work_win32(LPVOID arg) {
    work(arg);
    return 0;
}
static void start(thread *t, intptr_t id) { *t = CreateThread(NULL, 0, work_win32, (void *)id, 0, NULL); }
static void join(thread t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
#else
#include <pthread.h>
typedef pthread_t thread;
static void start(thread *t, intptr_t id) { pthread_create(t, NULL, work, (void *)id); }
static void join(thread t) { pthread_join(t, NULL); }
#endif

int main(int argc, char **argv) {
    const bool selftest = argc > 1 && strcmp(argv[1], "--selftest") == 0;
    if (qtc_init_argv(QTC_APP_WIDGETS, argc, (const char *const *)argv) != QTC_OK) return 1;
    qtc_set_error_policy(QTC_ERRORS_LOG);

    qtc_obj bar = qtc_new(QTC_S("QProgressBar"));
    qtc_set_str(bar, QTC_S("windowTitle"), QTC_S("Workers"));
    qtc_set_int(bar, QTC_S("maximum"), WORKERS * STEPS);
    qtc_arg_int(360);
    qtc_arg_int(60);
    qtc_call(bar, QTC_S("resize"));
    qtc_call(bar, QTC_S("show"));

    thread threads[WORKERS];
    for (intptr_t i = 0; i < WORKERS; i++) start(&threads[i], i);

    int64_t reports = 0, done = 0;
    qtc_event ev;
    while (qtc_wait(-1) == QTC_WAIT_EVENTS) {
        while (qtc_poll(&ev)) {
            if (ev.tag == TAG_PROGRESS) {
                reports++;
                qtc_set_int(bar, QTC_S("value"), reports);
            } else if (ev.tag == TAG_DONE) {
                printf("worker %lld done (checksum %lld)\n", (long long)ev.i0, (long long)ev.i1);
                if (++done == WORKERS && selftest) qtc_quit(0);
            }
        }
    }
    for (int i = 0; i < WORKERS; i++) join(threads[i]);

    int rc = 0;
    if (selftest) {
        const int64_t value = qtc_get_int(bar, QTC_S("value"));
        rc = (reports == WORKERS * STEPS && value == WORKERS * STEPS) ? 0 : 1;
        printf("threads: %lld reports, bar at %lld: %s\n", (long long)reports, (long long)value, rc ? "FAIL" : "ok");
    }
    qtc_shutdown();
    return rc;
}
