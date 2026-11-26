/* The threads the C tests start: POSIX threads, or on Windows the part of them the tests use
   (pthread_create, pthread_join), over Win32 threads. */
#ifndef QTC_TEST_THREAD_H
#define QTC_TEST_THREAD_H

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdlib.h>

typedef struct qtc_test_thread {
    HANDLE handle;
    void *(*fn)(void *);
    void *arg;
    void *result;
} *pthread_t;

static DWORD WINAPI qtc_test_thread_main(LPVOID p) {
    pthread_t t = (pthread_t)p;
    t->result = t->fn(t->arg);
    return 0;
}

static int pthread_create(pthread_t *out, const void *attr, void *(*fn)(void *), void *arg) {
    (void)attr;
    pthread_t t = (pthread_t)calloc(1, sizeof *t);
    if (!t) return -1;
    t->fn = fn;
    t->arg = arg;
    t->handle = CreateThread(NULL, 0, qtc_test_thread_main, t, 0, NULL);
    if (!t->handle) {
        free(t);
        return -1;
    }
    *out = t;
    return 0;
}

static int pthread_join(pthread_t t, void **result) {
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    if (result) *result = t->result;
    free(t);
    return 0;
}
#else
#include <pthread.h>
#endif

#endif
