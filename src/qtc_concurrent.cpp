// Futures, promises, tasks and worker threads for C (and Goose).
//
// One future per id. Its producer side (a promise) may be used from any thread; its results are
// kept here, in order, and a QPromise<QVariant> mirrors the future's state, progress and result
// count for Qt: QFutureWatcher turns those into events (qtc_future_watch), and QtFuture::whenAll /
// whenAny combine futures. Tasks run C functions on a QThreadPool through QRunnable::create, as
// QtConcurrent::run does. Every way a future can be left behind (a task the pool never runs, a
// queued call whose object dies first, a release, the end of the program) still finishes it, so
// nothing waits forever on a future nobody will finish.
#include "qtc_events.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QFuture>
#include <QtCore/QFutureWatcher>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMetaMethod>
#include <QtCore/QPointer>
#include <QtCore/QPromise>
#include <QtCore/QRunnable>
#include <QtCore/QThread>
#include <QtCore/QThreadPool>
#include <QtCore/QTimer>
#ifdef QTC_HAS_GUI
#include <QtGui/QCursor>
#include <QtGui/QIcon>
#include <QtGui/QPixmap>
#endif

#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace qtc {

namespace {

struct Job {
    uint64_t id = 0;
    std::mutex mu;                       // everything below but `future` and `failed`
    std::condition_variable cv;          // done
    QPromise<QVariant> promise;          // mirrors the state for Qt (one empty result per result)
    bool open = true;                    // results may still be added
    bool done = false;                   // finished (after promise.finish() returned)
    QList<QVariant> results;
    QByteArray error;
    QFuture<QVariant> future;            // set before the id is handed out, never changed
    std::atomic<bool> failed{false};
};
using JobPtr = std::shared_ptr<Job>;

struct WatchRec {
    QFutureWatcher<QVariant> *w;
    uint64_t job;
};

struct Conc {
    std::mutex mu;                       // jobs, next_id
    std::unordered_map<uint64_t, JobPtr> jobs;
    uint64_t next_id = 1;
    std::atomic<bool> open{false};
    // GUI thread only
    std::unordered_map<uint64_t, WatchRec> watches;
    uint64_t next_watch = 1;
};
Conc &C() {
    static Conc *c = new Conc;
    return *c;
}

qtc_status not_open() { return fail(QTC_ERR_NOT_INITIALIZED, "qtc_init has not been called"); }

JobPtr find_job(uint64_t id, qtc_status *err) {
    Conc &c = C();
    if (!id) {
        *err = QTC_ERR_NULL_HANDLE;
        return nullptr;
    }
    std::lock_guard<std::mutex> lk(c.mu);
    auto it = c.jobs.find(id);
    if (it == c.jobs.end()) {
        *err = id < c.next_id ? QTC_ERR_STALE_HANDLE : QTC_ERR_BAD_HANDLE;
        return nullptr;
    }
    *err = 0;
    return it->second;
}

JobPtr new_job() {
    Conc &c = C();
    auto j = std::make_shared<Job>();
    j->promise.start();
    j->future = j->promise.future();
    std::lock_guard<std::mutex> lk(c.mu);
    j->id = c.next_id++;
    c.jobs.emplace(j->id, j);
    return j;
}

// Finishes the job once. Never called with j.mu held: finishing runs Qt's continuations (the
// combinations of qtc_future_all / _any) right here, and they lock the futures they finish.
void finish_job(Job &j, const QByteArray *failure = nullptr) {
    {
        std::lock_guard<std::mutex> lk(j.mu);
        if (!j.open) return;
        j.open = false;
        if (failure) {
            j.error = failure->isEmpty() ? QByteArray("failed") : *failure;
            j.failed.store(true);
        }
    }
    j.promise.finish();
    {
        std::lock_guard<std::mutex> lk(j.mu);
        j.done = true;
    }
    j.cv.notify_all();
}

void cancel_and_finish(Job &j, const char *why = nullptr) {
    j.future.cancel();
    if (why) {
        const QByteArray w(why);
        finish_job(j, &w);
    } else {
        finish_job(j);
    }
}

// False once the future is finished. Results of a canceled future are dropped, as Qt drops them.
bool add_result_job(Job &j, QVariant v) {
    std::lock_guard<std::mutex> lk(j.mu);
    if (!j.open) return false;
    if (j.future.isCanceled()) return true;
    j.results.append(std::move(v));
    j.promise.addResult(QVariant(), -1);   // for watchers (resultsReadyAt) and combinations
    return true;
}

qtc_status add_result(uint64_t id, QVariant v, const char *what) {
    if (!C().open.load()) return not_open();
    qtc_status err = 0;
    JobPtr j = find_job(id, &err);
    if (!j) return fail(err, what);
    if (!add_result_job(*j, std::move(v)))
        return fail(QTC_ERR_STATE, QStringLiteral("%1: the future is finished").arg(QLatin1StringView(what)));
    return QTC_OK;
}

int32_t state_bits(const Job &j) {
    const QFuture<QVariant> &f = j.future;
    int32_t s = 0;
    if (f.isStarted()) s |= QTC_FUT_STARTED;
    if (f.isRunning()) s |= QTC_FUT_RUNNING;
    if (f.isFinished()) s |= QTC_FUT_FINISHED;
    if (f.isCanceled()) s |= QTC_FUT_CANCELED;
    if (f.isSuspended() || f.isSuspending()) s |= QTC_FUT_SUSPENDED;
    if (j.failed.load()) s |= QTC_FUT_FAILED;
    return s;
}

// --------------------------------------------------------------- tasks

// One task on a pool. Whatever happens to the runnable (run, or dropped unrun by
// QThreadPool::clear() or at exit), the future finishes and cleanup runs exactly once.
struct TaskRun {
    JobPtr j;
    qtc_task_fn fn;
    void *user;
    void (*cleanup)(void *);
    bool ran = false;
    void run() {
        ran = true;
        if (!j->future.isCanceled()) fn(j->id, user);
        finish_job(*j);
        if (cleanup) cleanup(user);
    }
    ~TaskRun() {
        if (ran) return;
        cancel_and_finish(*j);
        if (cleanup) cleanup(user);
    }
};

struct MapState {
    JobPtr j;
    int64_t count = 0, chunk = 1, chunks = 0;
    std::atomic<int64_t> next{0}, done{0};
    std::atomic<int> workers{0};
    qtc_map_fn fn = nullptr;
    void *user = nullptr;
    void (*cleanup)(void *) = nullptr;
    // the last runnable to go (run or dropped) ends the map
    void worker_gone() {
        if (workers.fetch_sub(1) != 1) return;
        if (done.load() == chunks) finish_job(*j);
        else cancel_and_finish(*j);   // canceled, or runnables dropped unrun
        if (cleanup) cleanup(user);
    }
};

struct MapWorker {
    std::shared_ptr<MapState> m;
    ~MapWorker() { m->worker_gone(); }
    void run() {
        Job &j = *m->j;
        for (;;) {
            if (j.future.isCanceled()) return;
            j.promise.suspendIfRequested();   // QPromise is thread-safe; blocks while suspended
            if (j.future.isCanceled()) return;
            const int64_t c = m->next.fetch_add(1);
            if (c >= m->chunks) return;
            const int64_t b = c * m->chunk, e = std::min(m->count, b + m->chunk);
            for (int64_t i = b; i < e; i++) m->fn(i, m->user);
            const int64_t d = m->done.fetch_add(1) + 1;
            std::lock_guard<std::mutex> lk(j.mu);
            if (j.open) j.promise.setProgressValue(int(d));
        }
    }
};

QThreadPool *pool_of(uint64_t pool, qtc_status *err) {
    *err = 0;
    if (!pool) return QThreadPool::globalInstance();
    QObject *o = reg().object(pool, err);
    if (!o) return nullptr;
    auto *tp = qobject_cast<QThreadPool *>(o);
    if (!tp) {
        *err = QTC_ERR_WRONG_TYPE;
        return nullptr;
    }
    return tp;
}

qtc_future start_task(QThreadPool *tp, int priority, qtc_task_fn fn, void *user, void (*cleanup)(void *)) {
    JobPtr j = new_job();
    auto t = std::make_shared<TaskRun>();
    t->j = j;
    t->fn = fn;
    t->user = user;
    t->cleanup = cleanup;
    tp->start(QRunnable::create([t] { t->run(); }), priority);
    return j->id;
}

// --------------------------------------------------------------- watches

void post_future_event(uint64_t watch, uint64_t tag, uint64_t job, int32_t kind, int64_t i0 = 0, int64_t i1 = 0,
                       int64_t i2 = 0, const QByteArray &text = QByteArray()) {
    qtc_event e{};
    e.kind = kind;
    e.tag = tag;
    e.source = job;
    e.id = watch;
    e.i0 = i0;
    e.i1 = i1;
    e.i2 = i2;
    events_push(e, text);
}

void end_watch(uint64_t wid) {
    Conc &c = C();
    auto it = c.watches.find(wid);
    if (it == c.watches.end()) return;
    it->second.w->disconnect();
    it->second.w->deleteLater();   // it may be emitting right now
    c.watches.erase(it);
}

// --------------------------------------------------------------- queued calls

#ifdef QTC_HAS_GUI
bool gui_only_type(QMetaType t) {
    return t == QMetaType::fromType<QPixmap>() || t == QMetaType::fromType<QIcon>() || t == QMetaType::fromType<QCursor>() ||
           t.id() == QMetaType::QBitmap;
}
#else
bool gui_only_type(QMetaType) { return false; }
#endif

// What a queued call may carry in a value: no objects inside it (a pointer may die before the
// call runs, or live in another thread), no model indexes (rows may go meanwhile), and across
// threads no GUI-thread-only values (pixmaps, icons, cursors).
bool queueable(const QVariant &v, bool far, int depth = 0) {
    const QMetaType mt = v.metaType();
    if (!mt.isValid()) return true;
    if (depth > 64 || holds_object(v)) return false;
    const int id = mt.id();
    if (id == QMetaType::QModelIndex || id == QMetaType::QPersistentModelIndex || (far && gui_only_type(mt))) return false;
    if (id == QMetaType::QVariantList) {
        for (const QVariant &e : v.toList())
            if (!queueable(e, far, depth + 1)) return false;
    } else if (id == QMetaType::QVariantMap) {
        const QVariantMap m = v.toMap();
        for (auto it = m.cbegin(); it != m.cend(); ++it)
            if (!queueable(it.value(), far, depth + 1)) return false;
    } else if (id == QMetaType::QVariantHash) {
        const QVariantHash m = v.toHash();
        for (auto it = m.cbegin(); it != m.cend(); ++it)
            if (!queueable(it.value(), far, depth + 1)) return false;
    }
    return true;
}

struct CallArg {
    QVariant v;           // the converted argument (objects: resolved again when the call runs)
    uint64_t obj = 0;     // an object parameter: its handle
    bool is_obj = false;
};

// One queued call. If its event is dropped before it runs (the object died), the future fails.
struct InvokeRun {
    QPointer<QObject> target;
    int method = -1;
    QByteArray sig;
    std::vector<CallArg> args;
    bool far = false;     // the object lives in another thread
    JobPtr j;             // null for qtc_invoke_queued
    bool ran = false;
    void fail_job(const QByteArray &why) {
        if (j) finish_job(*j, &why);
    }
    void run() {
        ran = true;
        QObject *x = target.data();
        if (!x) return fail_job("the object was destroyed before the call");
        if (j && j->future.isCanceled()) return finish_job(*j);
        const QMetaMethod m = x->metaObject()->method(method);
        // objects are resolved now, in the thread that owns them all (this is the GUI thread:
        // calls with object arguments are refused for other threads)
        std::vector<QVariant> vals(args.size());
        void *argv[1 + kMaxArgs] = {nullptr};
        for (size_t i = 0; i < args.size(); i++) {
            const QMetaType pt = m.parameterMetaType(int(i));
            if (!args[i].is_obj) {
                vals[i] = args[i].v;
            } else {
                QObject *o = nullptr;
                if (args[i].obj) {
                    qtc_status err = 0;
                    o = reg().object(args[i].obj, &err);
                    if (!o)
                        return fail_job(QByteArray("argument ") + QByteArray::number(qulonglong(i + 1)) +
                                        " is no longer usable here (destroyed or moved) when the call runs");
                }
                vals[i] = pt == QMetaType::fromType<QVariant>() ? QVariant::fromValue(o) : QVariant(pt, &o);
            }
            // a QVariant parameter takes the variant itself (see invoke_meta in qtc_core.cpp)
            argv[i + 1] = pt == QMetaType::fromType<QVariant>() ? static_cast<void *>(&vals[i]) : vals[i].data();
        }
        const QMetaType rt = m.returnMetaType();
        const bool has_ret = rt.isValid() && rt.id() != QMetaType::Void;
        QVariant r = has_ret ? QVariant(rt) : QVariant();   // a QVariant return: a variant holding one
        if (has_ret) argv[0] = r.data();
        QMetaObject::metacall(x, QMetaObject::InvokeMetaMethod, method, argv);
        if (!j) return;
        if (has_ret) {
            const QVariant out = rt == QMetaType::fromType<QVariant>() ? *static_cast<const QVariant *>(r.constData()) : r;
            if (far) {
                if (!queueable(out, true)) return fail_job("the result holds an object or a GUI-thread value, which cannot cross threads");
                add_result_job(*j, out);
            } else {
                add_result_job(*j, sanitize(out));
            }
        }
        finish_job(*j);
    }
    ~InvokeRun() {
        if (!ran) fail_job("the object was destroyed before the call");
    }
};

qtc_future invoke_queued(qtc_obj obj, qtc_str method, bool want_future, const char *what) {
    ArgStack &st = args();
    struct Clear {
        ArgStack &s;
        ~Clear() { s.clear(); }
    } clear{st};
    if (st.overflow) {
        fail(QTC_ERR_BAD_ARGS, QStringLiteral("%1: more than %2 arguments").arg(QLatin1StringView(what)).arg(kMaxArgs));
        return 0;
    }
    QByteArray spec;
    if (!read_bytes(method, spec, "method")) return 0;
    spec = spec.trimmed();
    // the registry lock keeps an object of another thread from finishing dying while it is looked at
    // and posted to (see Registry::destroy_far)
    std::lock_guard<std::recursive_mutex> lk(reg().mu);
    qtc_status err = 0;
    QObject *o = reg().object(obj, &err);
    bool far = false;
    if (!o && err == QTC_ERR_WRONG_THREAD) {
        o = reg().object_anywhere(obj, &err);
        far = true;
    }
    if (!o) {
        fail(err, what);
        return 0;
    }
    if (far) {
        // a call posted to a thread that has finished would never run
        QThread *t = o->thread();
        if (t && t->isFinished()) {
            fail(QTC_ERR_STATE, QStringLiteral("%1: the object's thread has finished").arg(QLatin1StringView(what)));
            return 0;
        }
    }
    const int n = st.n;
    std::vector<QMetaMethod> cands = meta_candidates(o, spec);
    if (!spec.contains('('))
        cands.erase(std::remove_if(cands.begin(), cands.end(), [n](const QMetaMethod &m) { return m.parameterCount() != n; }),
                    cands.end());
    if (cands.empty()) {
        fail(QTC_ERR_NOT_FOUND, QStringLiteral("%1: %2 has no slot, signal or invokable %3 taking %4 arguments that qtc reaches "
                                               "(Qt classes qtc binds are called with qtc_call)")
                                    .arg(QLatin1StringView(what), QLatin1StringView(o->metaObject()->className()),
                                         QString::fromUtf8(spec))
                                    .arg(n));
        return 0;
    }
    if (cands.size() > 1) {
        fail(QTC_ERR_AMBIGUOUS, QStringLiteral("%1: several methods named %2 take %3 arguments; pass a signature")
                                    .arg(QLatin1StringView(what), QString::fromUtf8(spec))
                                    .arg(n));
        return 0;
    }
    const QMetaMethod mm = cands.front();
    if (mm.parameterCount() != n) {
        fail(QTC_ERR_BAD_ARGS, QStringLiteral("%1: %2 takes %3 arguments, got %4")
                                   .arg(QLatin1StringView(what), QLatin1StringView(mm.methodSignature()))
                                   .arg(mm.parameterCount())
                                   .arg(n));
        return 0;
    }
    if (want_future && far) {
        const QMetaType rt = mm.returnMetaType();
        if ((rt.flags() & QMetaType::PointerToQObject) || gui_only_type(rt)) {
            fail(QTC_ERR_UNSUPPORTED, QStringLiteral("%1: %2 returns a value that cannot cross threads")
                                          .arg(QLatin1StringView(what), QLatin1StringView(mm.methodSignature())));
            return 0;
        }
    }
    auto run = std::make_shared<InvokeRun>();
    run->target = o;
    run->method = mm.methodIndex();
    run->sig = mm.methodSignature();
    run->far = far;
    run->args.resize(size_t(n));
    for (int i = 0; i < n; i++) {
        const QMetaType pt = mm.parameterMetaType(i);
        CallArg &ca = run->args[size_t(i)];
        const bool obj_in_variant = pt == QMetaType::fromType<QVariant>() && (st.a[i].kind == AK_OBJ);
        if ((pt.flags() & QMetaType::PointerToQObject) || obj_in_variant) {
            if (far) {
                fail(QTC_ERR_UNSUPPORTED, QStringLiteral("%1: argument %2 of %3 is an object, which cannot be passed to another thread")
                                              .arg(QLatin1StringView(what))
                                              .arg(i + 1)
                                              .arg(QLatin1StringView(mm.methodSignature())));
                return 0;
            }
            // the object must live now (and, for a typed parameter, be of its class); it is looked
            // up again when the call runs
            QVariant v;
            int e = 0;
            if (obj_in_variant) {
                qtc_status oe = 0;
                if (!reg().object(st.a[i].h, &oe)) e = oe;
            } else {
                e = arg_to_metatype(st.a[i], pt, v, i);
            }
            if (e) {
                fail(e, QStringLiteral("%1: argument %2 of %3").arg(QLatin1StringView(what)).arg(i + 1).arg(QLatin1StringView(mm.methodSignature())));
                return 0;
            }
            ca.is_obj = true;
            ca.obj = st.a[i].kind == AK_OBJ ? st.a[i].h : 0;
            continue;
        }
        if (int e = arg_to_metatype(st.a[i], pt, ca.v, i)) {
            fail(e, QStringLiteral("%1: argument %2 of %3").arg(QLatin1StringView(what)).arg(i + 1).arg(QLatin1StringView(mm.methodSignature())));
            return 0;
        }
        if (!queueable(ca.v, far)) {
            fail(QTC_ERR_UNSUPPORTED, QStringLiteral("%1: argument %2 of %3 holds an object or a model index; pass objects "
                                                     "as object parameters")
                                          .arg(QLatin1StringView(what))
                                          .arg(i + 1)
                                          .arg(QLatin1StringView(mm.methodSignature())));
            return 0;
        }
    }
    uint64_t id = 0;
    if (want_future) {
        run->j = new_job();
        id = run->j->id;
    }
    QMetaObject::invokeMethod(o, [run] { run->run(); }, Qt::QueuedConnection);
    return want_future ? id : 1;
}

}  // namespace

void concurrent_init() { C().open.store(true); }

void concurrent_shutdown() {
    Conc &c = C();
    for (auto &[id, w] : c.watches) {
        w.w->disconnect();
        delete w.w;
    }
    c.watches.clear();
    std::vector<JobPtr> all;
    {
        std::lock_guard<std::mutex> lk(c.mu);
        for (auto &[id, j] : c.jobs) all.push_back(j);
        c.jobs.clear();
    }
    // tasks still running see their futures canceled (and unknown from now on) and wind down
    for (JobPtr &j : all) cancel_and_finish(*j);
    // worker threads qtc knows of stop their event loops; objects in one that does not stop in
    // time stay where they are (qtc_shutdown leaves them)
    std::vector<QPointer<QThread>> threads;
    {
        std::lock_guard<std::recursive_mutex> lk(reg().mu);
        for (const Slot &s : reg().table)
            if (s.kind == SK_OBJ && s.alive())
                if (auto *t = qobject_cast<QThread *>(s.obj); t && t != QThread::currentThread() && t->isRunning())
                    threads.push_back(t);
    }
    for (QPointer<QThread> &t : threads)
        if (t) {
            t->requestInterruption();
            t->quit();
        }
    for (QPointer<QThread> &t : threads)
        if (t) t->wait(QDeadlineTimer(2000));
    // QCoreApplication's destructor waits for the global pool too; finishing it here keeps the
    // tasks' last calls (cleanups, posts) before the event queue closes
    QThreadPool::globalInstance()->waitForDone();
    c.open.store(false);
}

}  // namespace qtc

using namespace qtc;

// ------------------------------------------------------------- promises (any thread)

QTC_API qtc_future qtc_promise_new(void) {
    if (!C().open.load()) {
        not_open();
        return 0;
    }
    return new_job()->id;
}

template <class F>
static qtc_status with_open(qtc_future f, const char *what, F &&fn) {
    if (!C().open.load()) return not_open();
    qtc_status err = 0;
    JobPtr j = find_job(f, &err);
    if (!j) return fail(err, what);
    std::lock_guard<std::mutex> lk(j->mu);
    if (!j->open) return fail(QTC_ERR_STATE, QStringLiteral("%1: the future is finished").arg(QLatin1StringView(what)));
    fn(*j);
    return QTC_OK;
}

QTC_API qtc_status qtc_promise_progress_range(qtc_future f, int32_t min, int32_t max) {
    if (min > max) return fail(QTC_ERR_INVALID_ARG, "qtc_promise_progress_range: min > max");
    return with_open(f, "qtc_promise_progress_range", [&](Job &j) { j.promise.setProgressRange(min, max); });
}

QTC_API qtc_status qtc_promise_progress(qtc_future f, int32_t value, qtc_str text) {
    QString t;
    if (text.data && !read_str(text, t, "text")) return last_error_code();
    return with_open(f, "qtc_promise_progress", [&](Job &j) {
        if (text.data) j.promise.setProgressValueAndText(value, t);
        else j.promise.setProgressValue(value);
    });
}

QTC_API qtc_status qtc_promise_add_bool(qtc_future f, bool v) { return add_result(f, QVariant(v), "qtc_promise_add_bool"); }
QTC_API qtc_status qtc_promise_add_int(qtc_future f, int64_t v) {
    return add_result(f, QVariant(qlonglong(v)), "qtc_promise_add_int");
}
QTC_API qtc_status qtc_promise_add_double(qtc_future f, double v) { return add_result(f, QVariant(v), "qtc_promise_add_double"); }

QTC_API qtc_status qtc_promise_add_str(qtc_future f, qtc_str v) {
    QString s;
    if (!read_str(v, s, "value")) return last_error_code();
    return add_result(f, QVariant(s), "qtc_promise_add_str");
}

QTC_API qtc_status qtc_promise_add_bytes(qtc_future f, qtc_str v) {
    QByteArray b;
    if (!read_bytes(v, b, "value")) return last_error_code();
    return add_result(f, QVariant(b), "qtc_promise_add_bytes");
}

QTC_API qtc_status qtc_promise_add_f64s(qtc_future f, qtc_f64s v) {
    if (v.len < 0 || (v.len > 0 && !v.data)) return fail(QTC_ERR_INVALID_ARG, "qtc_promise_add_f64s: NULL data or a negative length");
    if (!alloc_ok(v.len, int64_t(sizeof(double)))) return fail(QTC_ERR_LIMIT, "qtc_promise_add_f64s: over the allocation limit");
    QList<double> l(v.data, v.data + v.len);
    return add_result(f, QVariant::fromValue(l), "qtc_promise_add_f64s");
}

QTC_API qtc_status qtc_promise_add_json(qtc_future f, qtc_str json) {
    QByteArray b;
    if (!read_bytes(json, b, "json")) return last_error_code();
    QJsonParseError pe{};
    const QJsonDocument d = QJsonDocument::fromJson(b, &pe);
    if (pe.error != QJsonParseError::NoError || d.isNull())
        return fail(QTC_ERR_INVALID_ARG, "qtc_promise_add_json: " + pe.errorString());
    return add_result(f, d.isArray() ? QVariant(d.array().toVariantList()) : QVariant(d.object().toVariantMap()),
                      "qtc_promise_add_json");
}

QTC_API qtc_status qtc_promise_add_val(qtc_future f, qtc_val v) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        QVariant *x = reg().value(v, &err);
        if (!x) return fail(err, "qtc_promise_add_val");
        return add_result(f, *x, "qtc_promise_add_val");
    });
}

QTC_API qtc_status qtc_promise_fail(qtc_future f, qtc_str message) {
    if (!C().open.load()) return not_open();
    QByteArray m;
    if (message.data && !read_bytes(message, m, "message")) return last_error_code();
    qtc_status err = 0;
    JobPtr j = find_job(f, &err);
    if (!j) return fail(err, "qtc_promise_fail");
    {
        std::lock_guard<std::mutex> lk(j->mu);
        if (!j->open) return fail(QTC_ERR_STATE, "qtc_promise_fail: the future is finished");
    }
    finish_job(*j, &m);
    return QTC_OK;
}

QTC_API qtc_status qtc_promise_finish(qtc_future f) {
    if (!C().open.load()) return not_open();
    qtc_status err = 0;
    JobPtr j = find_job(f, &err);
    if (!j) return fail(err, "qtc_promise_finish");
    {
        std::lock_guard<std::mutex> lk(j->mu);
        if (!j->open) return fail(QTC_ERR_STATE, "qtc_promise_finish: the future is finished");
    }
    finish_job(*j);
    return QTC_OK;
}

QTC_API bool qtc_promise_canceled(qtc_future f) {
    if (!C().open.load()) return true;
    qtc_status err = 0;
    JobPtr j = find_job(f, &err);
    return !j || j->future.isCanceled();
}

QTC_API qtc_status qtc_promise_suspend_if_requested(qtc_future f) {
    if (!C().open.load()) return not_open();
    if (gui_thread_ok()) return fail(QTC_ERR_WRONG_THREAD, "qtc_promise_suspend_if_requested would block the GUI thread");
    qtc_status err = 0;
    JobPtr j = find_job(f, &err);
    if (!j) return fail(err, "qtc_promise_suspend_if_requested");
    j->promise.suspendIfRequested();   // returns at once when not suspended, canceled or finished
    return QTC_OK;
}

// ------------------------------------------------------------- tasks

QTC_API qtc_future qtc_task_start(qtc_task_fn fn, void *user, void (*cleanup)(void *)) {
    if (!C().open.load()) {
        not_open();
        return 0;
    }
    if (!fn) {
        fail(QTC_ERR_INVALID_ARG, "qtc_task_start: no function");
        return 0;
    }
    return start_task(QThreadPool::globalInstance(), 0, fn, user, cleanup);
}

QTC_API qtc_future qtc_task_start_in(qtc_obj pool, int32_t priority, qtc_task_fn fn, void *user, void (*cleanup)(void *)) {
    return guarded<qtc_future>(0, [&]() -> qtc_future {
        if (!fn) {
            fail(QTC_ERR_INVALID_ARG, "qtc_task_start_in: no function");
            return 0;
        }
        qtc_status err = 0;
        QThreadPool *tp = pool_of(pool, &err);
        if (!tp) {
            fail(err, "qtc_task_start_in: pool");
            return 0;
        }
        return start_task(tp, priority, fn, user, cleanup);
    });
}

QTC_API qtc_future qtc_task_map(int64_t count, qtc_map_fn fn, void *user, void (*cleanup)(void *)) {
    if (!C().open.load()) {
        not_open();
        return 0;
    }
    if (!fn) {
        fail(QTC_ERR_INVALID_ARG, "qtc_task_map: no function");
        return 0;
    }
    if (count < 0) {
        fail(QTC_ERR_OUT_OF_RANGE, "qtc_task_map: count < 0");
        return 0;
    }
    QThreadPool *pool = QThreadPool::globalInstance();
    auto m = std::make_shared<MapState>();
    m->j = new_job();
    m->count = count;
    m->fn = fn;
    m->user = user;
    m->cleanup = cleanup;
    // a few chunks per thread: progress to watch, cancel and suspend between them, little overhead
    const int threads = std::max(1, pool->maxThreadCount());
    m->chunk = std::max<int64_t>(1, count / (int64_t(threads) * 8));
    m->chunks = count ? (count - 1) / m->chunk + 1 : 0;
    if (m->chunks > INT_MAX) {   // progress values are ints
        m->chunk = (count - 1) / INT_MAX + 1;
        m->chunks = (count - 1) / m->chunk + 1;
    }
    {
        std::lock_guard<std::mutex> lk(m->j->mu);
        m->j->promise.setProgressRange(0, int(m->chunks));
    }
    // more runnables than threads only wait in the queue
    const int workers = int(std::max<int64_t>(1, std::min<int64_t>({int64_t(threads), m->chunks, 1024})));
    m->workers.store(workers);
    const qtc_future id = m->j->id;
    for (int w = 0; w < workers; w++) {
        auto wk = std::make_shared<MapWorker>();
        wk->m = m;
        pool->start(QRunnable::create([wk] { wk->run(); }));
    }
    return id;
}

// ------------------------------------------------------------- futures

QTC_API int32_t qtc_future_state(qtc_future f) {
    if (!C().open.load()) return not_open();
    qtc_status err = 0;
    JobPtr j = find_job(f, &err);
    if (!j) return fail(err, "qtc_future_state");
    return state_bits(*j);
}

QTC_API int64_t qtc_future_result_count(qtc_future f) {
    if (!C().open.load()) return not_open();
    qtc_status err = 0;
    JobPtr j = find_job(f, &err);
    if (!j) return fail(err, "qtc_future_result_count");
    std::lock_guard<std::mutex> lk(j->mu);
    return j->results.size();
}

QTC_API qtc_status qtc_future_progress(qtc_future f, int32_t *value, int32_t *min, int32_t *max) {
    if (!C().open.load()) return not_open();
    qtc_status err = 0;
    JobPtr j = find_job(f, &err);
    if (!j) return fail(err, "qtc_future_progress");
    if (value) *value = j->future.progressValue();
    if (min) *min = j->future.progressMinimum();
    if (max) *max = j->future.progressMaximum();
    return QTC_OK;
}

QTC_API qtc_status qtc_future_result(qtc_future f, int64_t index) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        JobPtr j = find_job(f, &err);
        if (!j) return fail(err, "qtc_future_result");
        QVariant v;
        {
            std::lock_guard<std::mutex> lk(j->mu);
            if (index < 0 || index >= j->results.size())
                return fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("qtc_future_result: there is no result %1 (%2 so far)")
                                                      .arg(index)
                                                      .arg(j->results.size()));
            v = j->results.at(index);
        }
        ret().set_variant(v);
        return QTC_OK;
    });
}

QTC_API qtc_view qtc_future_error(qtc_future f) {
    static thread_local QByteArray keep;
    if (!C().open.load()) {
        not_open();
        return qtc_view{"", 0};
    }
    qtc_status err = 0;
    JobPtr j = find_job(f, &err);
    if (!j) {
        fail(err, "qtc_future_error");
        return qtc_view{"", 0};
    }
    {
        std::lock_guard<std::mutex> lk(j->mu);
        keep = j->error;
    }
    return view_of(keep);
}

QTC_API qtc_status qtc_future_cancel(qtc_future f) {
    if (!C().open.load()) return not_open();
    qtc_status err = 0;
    JobPtr j = find_job(f, &err);
    if (!j) return fail(err, "qtc_future_cancel");
    j->future.cancel();
    return QTC_OK;
}

QTC_API qtc_status qtc_future_suspend(qtc_future f, bool suspend) {
    if (!C().open.load()) return not_open();
    qtc_status err = 0;
    JobPtr j = find_job(f, &err);
    if (!j) return fail(err, "qtc_future_suspend");
    j->future.setSuspended(suspend);
    return QTC_OK;
}

QTC_API int32_t qtc_future_wait(qtc_future f, int64_t timeout_ms) {
    if (!C().open.load()) return not_open();
    qtc_status err = 0;
    JobPtr j = find_job(f, &err);
    if (!j) return fail(err, "qtc_future_wait");
    if (j->future.isFinished()) return QTC_WAIT_FINISHED;
    if (timeout_ms == 0) return QTC_WAIT_TIMEOUT;
    if (gui_thread_ok()) {
        if (ov_depth() > 0) return fail(QTC_ERR_STATE, "qtc_future_wait: the GUI thread cannot wait inside an override handler");
        // the GUI thread keeps Qt running meanwhile: a queued call into it may be what finishes f
        QEventLoop loop;
        QFutureWatcher<QVariant> w;
        QObject::connect(&w, &QFutureWatcherBase::finished, &loop, &QEventLoop::quit);
        QTimer t;
        t.setSingleShot(true);
        QObject::connect(&t, &QTimer::timeout, &loop, &QEventLoop::quit);
        w.setFuture(j->future);
        if (timeout_ms > 0) t.start(int(std::min<int64_t>(timeout_ms, INT_MAX)));
        if (!j->future.isFinished()) loop.exec();
        return j->future.isFinished() ? QTC_WAIT_FINISHED : QTC_WAIT_TIMEOUT;
    }
    std::unique_lock<std::mutex> lk(j->mu);
    if (timeout_ms < 0) {
        j->cv.wait(lk, [&] { return j->done; });
        return QTC_WAIT_FINISHED;
    }
    return j->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return j->done; }) ? QTC_WAIT_FINISHED
                                                                                                : QTC_WAIT_TIMEOUT;
}

QTC_API qtc_status qtc_future_release(qtc_future f) {
    if (!C().open.load()) return not_open();
    Conc &c = C();
    JobPtr j;
    {
        std::lock_guard<std::mutex> lk(c.mu);
        auto it = c.jobs.find(f);
        if (it == c.jobs.end())
            return fail(!f ? QTC_ERR_NULL_HANDLE : f < c.next_id ? QTC_ERR_STALE_HANDLE : QTC_ERR_BAD_HANDLE,
                        "qtc_future_release");
        j = it->second;
        c.jobs.erase(it);
    }
    // nobody can read it any more: an unfinished one is canceled and finished (its watches end)
    if (!j->future.isFinished()) cancel_and_finish(*j);
    std::lock_guard<std::mutex> lk(j->mu);
    j->results.clear();
    return QTC_OK;
}

static qtc_future combine(qtc_future a, qtc_future b, bool all, const char *what) {
    if (!C().open.load()) {
        not_open();
        return 0;
    }
    qtc_status err = 0;
    JobPtr ja = find_job(a, &err);
    if (!ja) {
        fail(err, what);
        return 0;
    }
    JobPtr jb = find_job(b, &err);
    if (!jb) {
        fail(err, what);
        return 0;
    }
    JobPtr g = new_job();
    QList<QFuture<QVariant>> in{ja->future, jb->future};   // QtFuture attaches continuations through these
    if (all) {
        QtFuture::whenAll(in.begin(), in.end()).then(QtFuture::Launch::Sync, [g](const QList<QFuture<QVariant>> &) { finish_job(*g); });
    } else {
        QtFuture::whenAny(in.begin(), in.end()).then(QtFuture::Launch::Sync, [g, a, b](const QtFuture::WhenAnyResult<QVariant> &r) {
            add_result_job(*g, QVariant(qulonglong(r.index == 0 ? a : b)));
            finish_job(*g);
        });
    }
    return g->id;
}

QTC_API qtc_future qtc_future_all(qtc_future a, qtc_future b) { return combine(a, b, true, "qtc_future_all"); }
QTC_API qtc_future qtc_future_any(qtc_future a, qtc_future b) { return combine(a, b, false, "qtc_future_any"); }

QTC_API uint64_t qtc_future_watch(qtc_future f, uint64_t tag) {
    return guarded<uint64_t>(0, [&]() -> uint64_t {
        qtc_status err = 0;
        JobPtr j = find_job(f, &err);
        if (!j) {
            fail(err, "qtc_future_watch");
            return 0;
        }
        Conc &c = C();
        const uint64_t wid = c.next_watch++;
        const uint64_t job = j->id;
        auto *w = new QFutureWatcher<QVariant>;
        QObject::connect(w, &QFutureWatcherBase::started, w, [wid, tag, job] { post_future_event(wid, tag, job, QTC_EV_FUTURE_STARTED); });
        QObject::connect(w, &QFutureWatcherBase::progressValueChanged, w, [w, wid, tag, job](int v) {
            post_future_event(wid, tag, job, QTC_EV_FUTURE_PROGRESS, v, w->progressMinimum(), w->progressMaximum(),
                              w->progressText().toUtf8());
        });
        QObject::connect(w, &QFutureWatcherBase::resultsReadyAt, w,
                         [wid, tag, job](int b, int e) { post_future_event(wid, tag, job, QTC_EV_FUTURE_RESULTS, b, e - b); });
        QObject::connect(w, &QFutureWatcherBase::canceled, w, [wid, tag, job] { post_future_event(wid, tag, job, QTC_EV_FUTURE_CANCELED); });
        QObject::connect(w, &QFutureWatcherBase::suspended, w, [wid, tag, job] { post_future_event(wid, tag, job, QTC_EV_FUTURE_SUSPENDED); });
        QObject::connect(w, &QFutureWatcherBase::resumed, w, [wid, tag, job] { post_future_event(wid, tag, job, QTC_EV_FUTURE_RESUMED); });
        // the job is held only weakly: a released future does not stay alive for its watch
        std::weak_ptr<Job> wj = j;
        QObject::connect(w, &QFutureWatcherBase::finished, w, [w, wj, wid, tag, job] {
            int32_t state = QTC_FUT_FINISHED | QTC_FUT_STARTED | (w->isCanceled() ? QTC_FUT_CANCELED : 0);
            int64_t count = 0;
            QByteArray error;
            if (JobPtr j = wj.lock()) {
                state = state_bits(*j);
                std::lock_guard<std::mutex> lk(j->mu);
                count = j->results.size();
                error = j->error;
            }
            post_future_event(wid, tag, job, QTC_EV_FUTURE_FINISHED, state, count, 0, error);
            end_watch(wid);
        });
        w->setFuture(j->future);
        c.watches.emplace(wid, WatchRec{w, job});
        return wid;
    });
}

QTC_API qtc_status qtc_future_unwatch(uint64_t watch) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        Conc &c = C();
        if (!watch) return fail(QTC_ERR_NULL_HANDLE, "qtc_future_unwatch");
        if (!c.watches.count(watch))
            return fail(watch < c.next_watch ? QTC_ERR_STALE_HANDLE : QTC_ERR_BAD_HANDLE, "qtc_future_unwatch");
        end_watch(watch);
        return QTC_OK;
    });
}

QTC_API int64_t qtc_live_futures(void) {
    Conc &c = C();
    std::lock_guard<std::mutex> lk(c.mu);
    return int64_t(c.jobs.size());
}

// ------------------------------------------------------------- worker objects

QTC_API qtc_status qtc_move_to_thread(qtc_obj obj, qtc_obj thread) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        qtc_status err = 0;
        QObject *o = reg().object(obj, &err);
        if (!o) return fail(err, "qtc_move_to_thread: object");
        QObject *t = reg().object(thread, &err);
        if (!t) return fail(err, "qtc_move_to_thread: thread");
        auto *th = qobject_cast<QThread *>(t);
        if (!th) return fail(QTC_ERR_WRONG_TYPE, "qtc_move_to_thread: not a QThread");
        if (th == QThread::currentThread()) return QTC_OK;
        if (o->parent()) return fail(QTC_ERR_STATE, "qtc_move_to_thread: the object has a parent (Qt moves only whole trees)");
        if (events_watched_below(o)) return fail(QTC_ERR_STATE, "qtc_move_to_thread: the object (or one inside it) has a qtc_watch");
        if (events_forwards_below(o))
            return fail(QTC_ERR_STATE, "qtc_move_to_thread: the object (or one inside it) has qtc_connect_to forwards");
        if (QString why = roles_movable(o); !why.isEmpty()) return fail(QTC_ERR_STATE, "qtc_move_to_thread: " + why);
        // from now on qtc reaches it only across threads: queued invocations, its signals, destroy
        roles_forget_tree(o);
        events_mark_far(o);
        o->moveToThread(th);
        if (o->thread() != th) {   // Qt refused (it warned): it is still an ordinary object here
            roles_register_tree(o);
            return fail(QTC_ERR_REJECTED, "qtc_move_to_thread: Qt refused the move");
        }
        return QTC_OK;
    });
}

QTC_API qtc_status qtc_invoke_queued(qtc_obj obj, qtc_str method) {
    return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status {
        return invoke_queued(obj, method, false, "qtc_invoke_queued") ? QTC_OK : last_error_code();
    });
}

QTC_API qtc_future qtc_invoke_future(qtc_obj obj, qtc_str method) {
    return guarded<qtc_future>(0, [&]() -> qtc_future { return invoke_queued(obj, method, true, "qtc_invoke_future"); });
}
