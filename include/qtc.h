/*
 * qtc — Qt 6 for C.
 *
 * A C API over Qt 6 built so that using it wrongly fails with an error code
 * instead of crashing (docs/LIMITS.md lists what that covers and what it cannot):
 *
 *   - Qt objects and values are handles, never pointers. A handle to an object
 *     that has been destroyed (by you, by its parent, by Qt) is detected and the
 *     call fails with QTC_ERR_STALE_HANDLE instead of touching freed memory.
 *   - Every call checks the receiver's class, the argument count, every argument's
 *     type, every enum value and every integer range before Qt sees it, and the
 *     preconditions Qt itself only asserts in debug builds (indexes, sizes,
 *     ownership, required states).
 *   - Signals and input events are queued and delivered from the top of the
 *     event loop, never from inside a Qt call stack, so a handler may destroy
 *     anything, including the object that sent the signal.
 *   - Destruction is deferred (deleteLater), so nothing Qt is still using is
 *     freed under it, and objects Qt or another object owns cannot be destroyed.
 *
 * Reach: every public method, constructor, static function, signal and slot of
 * the bound Qt modules whose argument types can cross a C boundary safely
 * (generated from the Qt headers; Qt 6.12 LTS), every property through Qt's
 * meta-object system, and every method, signal and property of QML objects and
 * of QObject classes qtc does not bind (the program's own, through their
 * meta-objects).
 *
 * Threading: all calls must come from the thread that called qtc_init (Qt's GUI
 * thread), except these, which are callable from any thread: qtc_post,
 * qtc_version, qtc_last_error*, qtc_clear_error, qtc_status_name, the promise
 * functions (qtc_promise_*, but qtc_promise_add_val), qtc_task_start,
 * qtc_task_map and the future functions marked "any thread" below. Work runs
 * off the GUI thread as tasks on a thread pool (qtc_task_*), or in objects moved
 * to a QThread (qtc_move_to_thread); its results reach the GUI thread through
 * futures and as events.
 *
 * Strings: input strings are qtc_str (UTF-8 bytes + length, no NUL needed).
 * Output strings are qtc_view: library-owned UTF-8, also NUL-terminated, valid
 * until the next call that returns a qtc_view on the same thread (error
 * messages and event texts have their own storage, see each function).
 */
#ifndef QTC_H
#define QTC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  if defined(QTC_BUILDING)
#    define QTC_API __declspec(dllexport)
#  else
#    define QTC_API __declspec(dllimport)
#  endif
#else
#  define QTC_API __attribute__((visibility("default")))
#endif

#define QTC_VERSION_MAJOR 1
#define QTC_VERSION_MINOR 3
#define QTC_VERSION_PATCH 0
#define QTC_VERSION_STRING "1.3.0"

/* ------------------------------------------------------------------ types */

typedef uint64_t qtc_obj;    /* handle to a QObject; 0 means "no object" */
typedef uint64_t qtc_val;    /* handle to a value (a QVariant); 0 means "no value" */
typedef int32_t qtc_status;  /* QTC_OK (0) or a negative QTC_ERR_* code */

/* Input string: UTF-8 bytes. len < 0 means data is NUL-terminated. */
typedef struct qtc_str { const char *data; int64_t len; } qtc_str;
/* Output string: library-owned UTF-8 (NUL-terminated as well). */
typedef struct qtc_view { const char *data; int64_t len; } qtc_view;
/* A run of doubles, for point lists: x0, y0, x1, y1, ... */
typedef struct qtc_f64s { const double *data; int64_t len; } qtc_f64s;

static inline qtc_str qtc_s(const char *nul_terminated) { qtc_str s; s.data = nul_terminated; s.len = -1; return s; }
static inline qtc_str qtc_sn(const char *data, int64_t len) { qtc_str s; s.data = data; s.len = len; return s; }
#ifndef __cplusplus
#  define QTC_S(literal) ((qtc_str){ (literal), (int64_t)(sizeof(literal) - 1) })
#endif

/* ----------------------------------------------------------------- status */

#define QTC_OK                    0
#define QTC_ERR_NOT_INITIALIZED  -1   /* qtc_init has not been called (or qtc_shutdown has) */
#define QTC_ERR_WRONG_THREAD     -2   /* called from a thread other than the GUI thread */
#define QTC_ERR_NULL_HANDLE      -3   /* handle 0 where an object or value is required */
#define QTC_ERR_STALE_HANDLE     -4   /* the object or value no longer exists */
#define QTC_ERR_BAD_HANDLE       -5   /* not a handle qtc ever issued */
#define QTC_ERR_WRONG_TYPE       -6   /* the handle's class does not support the operation */
#define QTC_ERR_NOT_FOUND        -7   /* no such class, method, property, signal, enum or child */
#define QTC_ERR_AMBIGUOUS        -8   /* several overloads match equally; pass a full signature */
#define QTC_ERR_BAD_ARGS         -9   /* wrong number or types of arguments */
#define QTC_ERR_OUT_OF_RANGE    -10   /* an index, enum value or integer is out of range */
#define QTC_ERR_READ_ONLY       -11   /* the property cannot be written */
#define QTC_ERR_CONVERSION      -12   /* a value cannot be converted to the requested type */
#define QTC_ERR_UNSUPPORTED     -13   /* not available in this Qt build, or not bindable */
#define QTC_ERR_EXCEPTION       -14   /* a C++ exception was raised and contained */
#define QTC_ERR_STATE           -15   /* not allowed in the current state */
#define QTC_ERR_LIMIT           -16   /* a resource limit was reached */
#define QTC_ERR_IO              -17   /* a file could not be read or written */
#define QTC_ERR_INVALID_ARG     -18   /* an argument is malformed (e.g. NULL data with a length) */
#define QTC_ERR_REJECTED        -19   /* Qt refused the operation (it returned false or warned) */

/* What happens when a call fails, besides returning its status. */
#define QTC_ERRORS_RETURN 0   /* nothing else (default) */
#define QTC_ERRORS_LOG    1   /* print the error to stderr */
#define QTC_ERRORS_ABORT  2   /* print it and abort(); for development */

QTC_API void qtc_set_error_policy(int32_t policy);
QTC_API int32_t qtc_error_policy(void);
/* The status of the most recent failed call on this thread, until qtc_clear_error. */
QTC_API qtc_status qtc_last_error(void);
/* Its message. Valid until the next error on this thread. */
QTC_API qtc_view qtc_last_error_message(void);
QTC_API void qtc_clear_error(void);
/* "QTC_ERR_STALE_HANDLE" etc. Static storage. */
QTC_API qtc_view qtc_status_name(qtc_status status);

/* -------------------------------------------------------------- lifecycle */

#define QTC_APP_WIDGETS 0   /* QApplication: widgets, QML, everything (default) */
#define QTC_APP_GUI     1   /* QGuiApplication: QML/Quick and painting, no widgets */
#define QTC_APP_CORE    2   /* QCoreApplication: no GUI at all */

/* Starts Qt. args is the command line as NUL-separated arguments, e.g.
   "myapp\0-platform\0offscreen" (pass {0, 0} for a default). */
QTC_API qtc_status qtc_init(int32_t app_kind, qtc_str args);
QTC_API qtc_status qtc_init_argv(int32_t app_kind, int32_t argc, const char *const *argv);
/* Destroys every object qtc created and still owns, every value, every
   connection, then the application. qtc_init may be called again afterwards. */
QTC_API qtc_status qtc_shutdown(void);
QTC_API bool qtc_is_initialized(void);
QTC_API qtc_view qtc_version(void);          /* this library, static */
QTC_API qtc_view qtc_qt_version(void);       /* the Qt it runs on, static */
QTC_API qtc_view qtc_qt_build_version(void); /* the Qt it was built against, static */
QTC_API qtc_obj qtc_app(void);               /* the application object */

/* ------------------------------------------------------------- event loop */

typedef struct qtc_event {
    uint64_t seq;     /* unique, increasing; names this event in qtc_event_* calls */
    uint64_t tag;     /* the tag given to qtc_connect / qtc_watch / qtc_timer / qtc_post */
    uint64_t source;  /* the object it came from (0 for posts) */
    uint64_t id;      /* the connection, watch or timer that produced it */
    int64_t i0, i1, i2, i3;   /* integer payload, see the event kinds */
    double d0, d1;            /* floating payload, see the event kinds */
    int32_t kind;     /* QTC_EV_* */
    int32_t argc;     /* signal events: number of signal arguments captured (0 with QTC_CONN_NO_ARGS) */
    int32_t count;    /* how many emissions were coalesced into this event (>= 1) */
    int32_t flags;    /* QTC_EVF_* */
} qtc_event;

/* Event kinds.
   SIGNAL: i0..i3 and d0..d1 hold the first integer-like (bool, integer, enum,
     object handle) and floating arguments in order; every argument is
     available through qtc_event_arg_*.
   POST: i0, i1 = the values given to qtc_post.
   TIMER: i0 = number of timeouts coalesced.
   LAST_WINDOW_CLOSED: the last visible top-level window was closed.
   Watched events (qtc_watch), all positions in the widget's coordinates:
   MOUSE_*: i0 = button, i1 = buttons held, i2 = keyboard modifiers, d0, d1 = x, y.
   WHEEL: i0 = vertical angle delta, i1 = horizontal angle delta, i2 = modifiers,
     d0, d1 = x, y.
   KEY_*: i0 = Qt::Key, i1 = modifiers, i2 = auto-repeat (0/1), i3 = native
     scan code; the typed text is qtc_event_text.
   RESIZE: i0, i1 = new width, height; i2, i3 = old width, height.
   MOVE: i0, i1 = new x, y; i2, i3 = old x, y.
   DROP / DRAG_*: d0, d1 = x, y; i0 = proposed action; qtc_event_text = the
     dropped text, or the URLs one per line.
   CLOSE: a close request (consume it to keep the window open).
   DESTROYED: the watched object was destroyed (source is now stale).
   FUTURE_* (qtc_future_watch): source = the future, id = the watch.
     FUTURE_PROGRESS: i0 = value, i1 = minimum, i2 = maximum; qtc_event_text =
       the progress text. FUTURE_RESULTS: i0 = index of the first new result,
       i1 = how many. FUTURE_FINISHED: i0 = QTC_FUT_* state, i1 = result count;
       qtc_event_text = the failure message (QTC_FUT_FAILED). The watch ends
       with this event. */
#define QTC_EV_SIGNAL              1
#define QTC_EV_POST                2
#define QTC_EV_TIMER               3
#define QTC_EV_LAST_WINDOW_CLOSED  4
#define QTC_EV_FUTURE_STARTED      5
#define QTC_EV_FUTURE_PROGRESS     6
#define QTC_EV_FUTURE_RESULTS      7
#define QTC_EV_FUTURE_FINISHED     8
#define QTC_EV_FUTURE_CANCELED     9
#define QTC_EV_FUTURE_SUSPENDED   10
#define QTC_EV_FUTURE_RESUMED     11
#define QTC_EV_WATCH_FIRST        16
#define QTC_EV_MOUSE_PRESS        16
#define QTC_EV_MOUSE_RELEASE      17
#define QTC_EV_MOUSE_DOUBLE_CLICK 18
#define QTC_EV_MOUSE_MOVE         19
#define QTC_EV_WHEEL              20
#define QTC_EV_KEY_PRESS          21
#define QTC_EV_KEY_RELEASE        22
#define QTC_EV_FOCUS_IN           23
#define QTC_EV_FOCUS_OUT          24
#define QTC_EV_ENTER              25
#define QTC_EV_LEAVE              26
#define QTC_EV_RESIZE             27
#define QTC_EV_MOVE               28
#define QTC_EV_SHOW               29
#define QTC_EV_HIDE               30
#define QTC_EV_CLOSE              31
#define QTC_EV_CONTEXT_MENU       32
#define QTC_EV_DRAG_ENTER         33
#define QTC_EV_DRAG_MOVE          34
#define QTC_EV_DRAG_LEAVE         35
#define QTC_EV_DROP               36
#define QTC_EV_PAINT              37
#define QTC_EV_WINDOW_STATE       38
#define QTC_EV_DESTROYED          39
#define QTC_EV_WATCH_LAST         39
#define QTC_WATCH(kind) (1ull << ((kind) - QTC_EV_WATCH_FIRST))
#define QTC_WATCH_MOUSE (QTC_WATCH(QTC_EV_MOUSE_PRESS) | QTC_WATCH(QTC_EV_MOUSE_RELEASE) | \
                         QTC_WATCH(QTC_EV_MOUSE_DOUBLE_CLICK) | QTC_WATCH(QTC_EV_MOUSE_MOVE) | \
                         QTC_WATCH(QTC_EV_WHEEL))
#define QTC_WATCH_KEYS  (QTC_WATCH(QTC_EV_KEY_PRESS) | QTC_WATCH(QTC_EV_KEY_RELEASE))

#define QTC_EVF_STALE_SOURCE 1   /* the source was destroyed after the event was queued */

#define QTC_WAIT_TIMEOUT  0
#define QTC_WAIT_EVENTS   1
#define QTC_WAIT_QUIT     2
#define QTC_WAIT_FINISHED 1   /* qtc_future_wait: the future finished */

/* Runs Qt's event loop until a qtc event is queued, the timeout passes or the
   application is asked to quit. timeout_ms < 0 waits forever, 0 only processes
   what is pending. Returns QTC_WAIT_* or a negative status. */
QTC_API int32_t qtc_wait(int64_t timeout_ms);
/* Takes the oldest queued event; false when there is none. */
QTC_API bool qtc_poll(qtc_event *ev);
QTC_API int64_t qtc_pending(void);
/* Processes pending Qt events without blocking (queued qtc events are kept). */
QTC_API qtc_status qtc_process_events(void);
/* Asks the application to quit: qtc_wait returns QTC_WAIT_QUIT from now on. */
QTC_API qtc_status qtc_quit(int32_t exit_code);
QTC_API bool qtc_quit_requested(void);
QTC_API int32_t qtc_exit_code(void);
QTC_API qtc_status qtc_cancel_quit(void);
/* Quit automatically when the last window closes (default true). */
QTC_API qtc_status qtc_set_quit_on_last_window_closed(bool on);

/* Callback style: qtc_run waits and hands every event to the handler until the
   application quits, and returns the exit code. The handler runs at the top of
   the loop, so it may do anything, including destroying the event's source. */
typedef void (*qtc_handler)(const qtc_event *ev, void *user);
QTC_API qtc_status qtc_set_handler(qtc_handler handler, void *user);
QTC_API int32_t qtc_run(void);

/* Thread-safe: queues a QTC_EV_POST event and wakes qtc_wait. */
QTC_API qtc_status qtc_post(uint64_t tag, int64_t a, int64_t b);
/* Upper bound on queued events (0 = unlimited, the default is 1,000,000).
   When full, new events are dropped and counted. */
QTC_API qtc_status qtc_set_queue_limit(int64_t max_events);
QTC_API int64_t qtc_dropped_events(void);

/* Arguments of a signal event, valid while it is one of the 64 most recently
   polled events (or during its handler). */
QTC_API int32_t qtc_event_arg_kind(uint64_t seq, int32_t index);   /* QTC_T_* or < 0 */
QTC_API qtc_status qtc_event_arg_bool(uint64_t seq, int32_t index, bool *out);
QTC_API qtc_status qtc_event_arg_int(uint64_t seq, int32_t index, int64_t *out);
QTC_API qtc_status qtc_event_arg_double(uint64_t seq, int32_t index, double *out);
QTC_API qtc_view qtc_event_arg_str(uint64_t seq, int32_t index);
QTC_API qtc_obj qtc_event_arg_obj(uint64_t seq, int32_t index);
QTC_API qtc_val qtc_event_arg_val(uint64_t seq, int32_t index);   /* a new value; free it */
/* Text of a key or drop event; valid until the next qtc_event_text call. */
QTC_API qtc_view qtc_event_text(uint64_t seq);
/* The signal's signature, e.g. "valueChanged(int)". */
QTC_API qtc_view qtc_event_signal(uint64_t seq);

/* ------------------------------------------------------ signals and events */

#define QTC_CONN_COALESCE 1u   /* while an event of this connection is queued, update it instead of queueing more */
#define QTC_CONN_ONCE     2u   /* disconnect after the first emission */
#define QTC_CONN_NO_ARGS  4u   /* do not capture the signal's arguments (fastest) */

/* signal: a name ("clicked") or a signature ("clicked(bool)"). Returns a
   connection id, or 0 on failure. Works for QML-declared signals too. */
QTC_API uint64_t qtc_connect(qtc_obj sender, qtc_str signal, uint64_t tag, uint32_t flags);
QTC_API qtc_status qtc_disconnect(uint64_t connection);
/* Calls a method of another object whenever the signal is emitted, inside the
   emission like a direct Qt connection, without a round trip through C.
   method: a slot or invokable the signal can call by Qt's rules ("setValue"
   or "setValue(int)"), or any bound method whose leading parameter types are
   the signal's ("setFixedWidth" for valueChanged(int)). The call is made the
   way qtc_call makes it, with the same checks; a failure is reported by the
   error policy (and qtc_last_error) and the emission goes on. Forwards that
   set each other off stop 32 deep. The pending arguments and the last result
   of the program are left as they were. Disconnect with qtc_disconnect; the
   connection ends by itself when either object is destroyed. */
QTC_API uint64_t qtc_connect_to(qtc_obj sender, qtc_str signal, qtc_obj receiver, qtc_str method);

/* Delivers the object's events selected by event_mask (QTC_WATCH bits) as
   qtc events; those also in consume_mask are filtered out before the object
   sees them (key, mouse, wheel, close, context menu and drop events). */
QTC_API uint64_t qtc_watch(qtc_obj obj, uint64_t event_mask, uint64_t consume_mask, uint64_t tag);
QTC_API qtc_status qtc_unwatch(uint64_t watch);

/* A started QTimer that delivers QTC_EV_TIMER events with this tag (coalesced).
   Destroy it with qtc_destroy. */
QTC_API qtc_obj qtc_timer(int64_t interval_ms, bool single_shot, uint64_t tag);

/* ---------------------------------------------------------------- objects */

/* Creates an object of the named class with the constructor arguments on the
   argument stack (see qtc_arg_*). The object is owned by qtc until it gets a
   parent: qtc_shutdown destroys it. Returns 0 on failure. */
QTC_API qtc_obj qtc_new(qtc_str class_name);
/* Hides it if it is a widget, invalidates the handle at once and deletes the
   object when control returns to the event loop. Children go with it. */
QTC_API qtc_status qtc_destroy(qtc_obj obj);
/* Makes qtc own an object Qt handed out without an owner (e.g. what
   QQmlComponent::create or QUiLoader::load return), so qtc_shutdown frees it. */
QTC_API qtc_status qtc_adopt(qtc_obj obj);
QTC_API bool qtc_alive(uint64_t handle);
QTC_API bool qtc_is_object(uint64_t handle);
QTC_API bool qtc_is_value(uint64_t handle);
QTC_API qtc_view qtc_class_name(qtc_obj obj);              /* the most derived class */
QTC_API bool qtc_inherits(qtc_obj obj, qtc_str class_name);
QTC_API qtc_obj qtc_find_child(qtc_obj parent, qtc_str object_name, qtc_str class_name, bool recursive);
QTC_API int64_t qtc_live_objects(void);
QTC_API int64_t qtc_live_values(void);

/* ----------------------------------------------------------------- values */

#define QTC_T_NONE   0
#define QTC_T_BOOL   1
#define QTC_T_INT    2
#define QTC_T_UINT   3
#define QTC_T_DOUBLE 4
#define QTC_T_STR    5
#define QTC_T_BYTES  6
#define QTC_T_OBJ    7
#define QTC_T_LIST   8
#define QTC_T_MAP    9
#define QTC_T_VALUE 10   /* any other Qt value: QColor, QFont, QImage, QDateTime, ... */
#define QTC_T_NULL  11

QTC_API qtc_val qtc_val_bool(bool v);
QTC_API qtc_val qtc_val_int(int64_t v);
QTC_API qtc_val qtc_val_uint(uint64_t v);
QTC_API qtc_val qtc_val_double(double v);
QTC_API qtc_val qtc_val_str(qtc_str v);
QTC_API qtc_val qtc_val_bytes(qtc_str v);
QTC_API qtc_val qtc_val_obj(qtc_obj v);
QTC_API qtc_val qtc_val_list(void);
QTC_API qtc_val qtc_val_map(void);
/* A value of a Qt value type, e.g. "QColor", built with the constructor
   arguments on the argument stack (none = default-constructed). */
QTC_API qtc_val qtc_val_new(qtc_str type_name);
QTC_API qtc_val qtc_val_from_json(qtc_str json);
QTC_API qtc_status qtc_val_free(qtc_val v);
QTC_API qtc_val qtc_val_copy(qtc_val v);
QTC_API int32_t qtc_val_kind(qtc_val v);          /* QTC_T_*, < 0 on error */
QTC_API qtc_view qtc_val_type_name(qtc_val v);    /* "int", "QString", "QColor", ... */
QTC_API qtc_status qtc_val_get_bool(qtc_val v, bool *out);
QTC_API qtc_status qtc_val_get_int(qtc_val v, int64_t *out);
QTC_API qtc_status qtc_val_get_uint(qtc_val v, uint64_t *out);
QTC_API qtc_status qtc_val_get_double(qtc_val v, double *out);
QTC_API qtc_view qtc_val_get_str(qtc_val v);
QTC_API qtc_obj qtc_val_get_obj(qtc_val v);
QTC_API qtc_val qtc_val_convert(qtc_val v, qtc_str type_name);   /* a new value */
QTC_API bool qtc_val_equal(qtc_val a, qtc_val b);
QTC_API qtc_view qtc_val_to_json(qtc_val v);
QTC_API qtc_view qtc_val_to_string(qtc_val v);    /* human-readable, for logs */
QTC_API int64_t qtc_list_len(qtc_val list);
QTC_API qtc_val qtc_list_get(qtc_val list, int64_t index);       /* a new value */
QTC_API qtc_status qtc_list_append(qtc_val list, qtc_val item);  /* copies item */
QTC_API qtc_status qtc_list_set(qtc_val list, int64_t index, qtc_val item);
QTC_API qtc_status qtc_list_remove(qtc_val list, int64_t index);
QTC_API qtc_status qtc_list_clear(qtc_val list);
QTC_API int64_t qtc_map_len(qtc_val map);
QTC_API qtc_val qtc_map_get(qtc_val map, qtc_str key);            /* a new value */
QTC_API qtc_status qtc_map_set(qtc_val map, qtc_str key, qtc_val item);
QTC_API qtc_status qtc_map_remove(qtc_val map, qtc_str key);
QTC_API bool qtc_map_has(qtc_val map, qtc_str key);
QTC_API qtc_val qtc_map_keys(qtc_val map);                        /* a new list */

/* ---------------------------------------------------------------- calling */

/* The argument stack: push a call's arguments, then qtc_call / qtc_new /
   qtc_val_new / qtc_prop_set consume them (even when they fail). Up to 16. */
QTC_API void qtc_args_clear(void);
QTC_API int32_t qtc_args_count(void);
QTC_API void qtc_arg_bool(bool v);
QTC_API void qtc_arg_int(int64_t v);
QTC_API void qtc_arg_uint(uint64_t v);
QTC_API void qtc_arg_double(double v);
QTC_API void qtc_arg_str(qtc_str v);
QTC_API void qtc_arg_bytes(qtc_str v);
QTC_API void qtc_arg_obj(qtc_obj v);
QTC_API void qtc_arg_val(qtc_val v);
QTC_API void qtc_arg_null(void);

/* Calls a method on an object or a value. method: a name ("setText"), resolved
   against the arguments' types, or a signature ("setText(QString)"). Covers
   generated thunks for public methods and Qt's meta-object methods (slots,
   signals, Q_INVOKABLEs, QML functions). The result goes to the return
   register. */
QTC_API qtc_status qtc_call(uint64_t receiver, qtc_str method);
QTC_API qtc_status qtc_call_static(qtc_str class_name, qtc_str method);

/* The return register: the result of the last successful call, get or new. */
QTC_API int32_t qtc_ret_kind(void);
QTC_API bool qtc_ret_bool(void);
QTC_API int64_t qtc_ret_int(void);
QTC_API uint64_t qtc_ret_uint(void);
QTC_API double qtc_ret_double(void);
QTC_API qtc_view qtc_ret_str(void);
QTC_API qtc_obj qtc_ret_obj(void);
QTC_API qtc_val qtc_ret_val(void);   /* a new value; free it */

/* Out-parameters. Methods with primitive pointer parameters (bool *ok, int *x,
   qreal *y, qint64 *pid, ...) get storage for them from qtc; after the call the
   values written there are read here, in parameter order. Leave those
   parameters out of the arguments (or pass null in their place):
       qtc_arg_int(16); qtc_call(str, QTC_S("toInt"));   value: qtc_ret_int()
       bool ok = qtc_out_bool(0);                        the `bool *ok`
   Every call resets them; qtc_out_count() says how many the last call filled. */
QTC_API int32_t qtc_out_count(void);
QTC_API bool qtc_out_bool(int32_t index);
QTC_API int64_t qtc_out_int(int32_t index);
QTC_API double qtc_out_double(int32_t index);

/* Largest single allocation, in bytes, that arguments may make Qt perform: a
   string or list size, a reserve, padding past the end, rows x columns of a
   model, the cells of a table. Calls over it fail with QTC_ERR_LIMIT instead of
   exhausting memory. Default 256 MiB; 0 restores the default. */
QTC_API qtc_status qtc_set_alloc_limit(int64_t bytes);
QTC_API int64_t qtc_alloc_limit(void);

/* Properties (Q_PROPERTY, including QML properties). qtc_prop_set takes the
   value from the argument stack; qtc_prop_get leaves it in the return register. */
QTC_API qtc_status qtc_prop_set(qtc_obj obj, qtc_str name);
QTC_API qtc_status qtc_prop_get(qtc_obj obj, qtc_str name);
/* Dynamic properties (QObject::setProperty on a name that is not declared). */
QTC_API qtc_status qtc_dynprop_set(qtc_obj obj, qtc_str name);
/* Shortcuts. */
QTC_API qtc_status qtc_set_bool(qtc_obj obj, qtc_str prop, bool v);
QTC_API qtc_status qtc_set_int(qtc_obj obj, qtc_str prop, int64_t v);
QTC_API qtc_status qtc_set_double(qtc_obj obj, qtc_str prop, double v);
QTC_API qtc_status qtc_set_str(qtc_obj obj, qtc_str prop, qtc_str v);
QTC_API bool qtc_get_bool(qtc_obj obj, qtc_str prop);
QTC_API int64_t qtc_get_int(qtc_obj obj, qtc_str prop);
QTC_API double qtc_get_double(qtc_obj obj, qtc_str prop);
QTC_API qtc_view qtc_get_str(qtc_obj obj, qtc_str prop);

/* ----------------------------------------------------------- introspection */

QTC_API bool qtc_class_exists(qtc_str class_name);
QTC_API int64_t qtc_class_count(void);
QTC_API qtc_view qtc_class_at(int64_t index);
/* JSON describing a class or a live object's class: bases, properties,
   signals, methods (generated and meta-object), constructors. */
QTC_API qtc_view qtc_describe(uint64_t handle, qtc_str class_name);
/* "Qt::AlignLeft", "QSizePolicy::Expanding", "Qt::AlignmentFlag::AlignLeft". */
QTC_API qtc_status qtc_enum_value(qtc_str qualified_name, int64_t *out);
QTC_API qtc_view qtc_enum_key(qtc_str enum_name, int64_t value);
/* {"name":"Qt::AlignmentFlag","flags":true,"open":false,"values":[["AlignLeft",1],...]} */
QTC_API qtc_view qtc_enum_describe(qtc_str enum_name);
/* {"classes":..,"methods":..,"unsupported":..,...} for this build. */
QTC_API qtc_view qtc_stats(void);

/* --------------------------------------------------------------- overrides */

/* Subclassing from C. qtc_new_overridable makes an object of a Qt class whose
   virtual methods the program overrides, one object at a time: an event
   handler (paintEvent, mousePressEvent, ...), a model's rowCount and data, a
   validator's validate, a thread's run, ... qtc_overridable_classes lists the
   classes, qtc_overridable_methods(class) their methods. Such an object is an
   ordinary object of its class otherwise, and its protected methods (a model's
   beginInsertRows and createIndex, a highlighter's setFormat, ...) are callable
   with qtc_call. */
QTC_API qtc_obj qtc_new_overridable(qtc_str class_name);   /* constructor arguments on the stack */
QTC_API bool qtc_is_overridable(qtc_obj obj);
QTC_API qtc_view qtc_overridable_classes(void);             /* one name per line */
/* JSON: {"class":..,"methods":[{"name","sig","params":[{"type","kind"}],
   "returns":{..},"pure","thread":"gui"|"any","paints"}],"protected":[sigs]} */
QTC_API qtc_view qtc_overridable_methods(qtc_str class_name);
/* From now on Qt's calls of method (a name, or a signature for an overload) on
   obj go to the override handler, with this tag; tag 0 ends the override. */
QTC_API qtc_status qtc_override(qtc_obj obj, qtc_str method, uint64_t tag);

typedef struct qtc_override_call {
    qtc_obj self;          /* the object */
    uint64_t tag;          /* the tag given to qtc_override */
    const char *method;    /* its signature, e.g. "rowCount(QModelIndex)" (static) */
    int32_t nargs;
    int32_t depth;         /* override calls in progress on this thread (1: this one alone) */
    bool gui_thread;       /* false for methods Qt calls from other threads (QThread::run) */
} qtc_override_call;

#define QTC_OV_DONE 0   /* the handler set the result (or there is none) */
#define QTC_OV_BASE 1   /* run the class's own implementation instead */

/* The handler runs whenever Qt calls an overridden method, on the thread Qt
   calls it from, in the middle of Qt's work: it may call qtc as usual, but not
   run the event loop (qtc_wait, qtc_run, qtc_process_events, exec(), ...) or
   shut qtc down, which fail with QTC_ERR_STATE. Methods other than "any"-thread
   ones are dispatched only on the GUI thread; there the handler may read and
   set the call with qtc_ov_*, and on other threads it may use the qtc_ov_*
   calls and the functions callable from any thread. Overrides calling each
   other stop 64 deep (Qt's own implementation runs beyond). */
typedef int32_t (*qtc_override_handler)(const qtc_override_call *call, void *user);
QTC_API qtc_status qtc_set_override_handler(qtc_override_handler handler, void *user);
/* For languages without function pointers: the handler is the function the
   program exports under this name (a dynamic symbol: link with -rdynamic). */
QTC_API qtc_status qtc_set_override_handler_symbol(qtc_str symbol);

/* Inside a handler, about the call being handled (any thread):
   the arguments, by position. */
QTC_API int32_t qtc_ov_arg_kind(int32_t index);      /* QTC_T_*; QTC_T_NONE for events, painters, ... */
QTC_API qtc_view qtc_ov_arg_type(int32_t index);     /* the C++ type, e.g. "const QModelIndex &" */
QTC_API bool qtc_ov_arg_bool(int32_t index);
QTC_API int64_t qtc_ov_arg_int(int32_t index);
QTC_API double qtc_ov_arg_double(int32_t index);
QTC_API qtc_view qtc_ov_arg_str(int32_t index);      /* valid until the handler returns */
QTC_API qtc_obj qtc_ov_arg_obj(int32_t index);
QTC_API qtc_val qtc_ov_arg_val(int32_t index);       /* GUI thread: a new value (free it) */
/* An event argument (QMouseEvent *, ...) described as qtc_wait would deliver a
   watched one (kind 0 and i0 = its QEvent::Type for others); its text; and
   whether it is accepted (what a handler's caller sees, e.g. a close event). */
QTC_API qtc_status qtc_ov_arg_event(int32_t index, qtc_event *out);
QTC_API qtc_view qtc_ov_event_text(int32_t index);
QTC_API qtc_status qtc_ov_accept_event(int32_t index, bool accepted);
/* A QModelIndex argument (models' overrides): its row, column and internal id
   (any of them may be NULL); true when it is valid. */
QTC_API bool qtc_ov_arg_index(int32_t index, int32_t *row, int32_t *column, uint64_t *id);
/* Out-parameters (QString &, int &): write them. */
QTC_API qtc_status qtc_ov_set_arg_int(int32_t index, int64_t v);
QTC_API qtc_status qtc_ov_set_arg_str(int32_t index, qtc_str v);
/* Buffers: a char * parameter to fill (its length is the next parameter):
   copies bytes in, at most that many, and returns how many; a const char *
   parameter to read. */
QTC_API int64_t qtc_ov_buffer_write(int32_t index, qtc_str bytes);
QTC_API qtc_view qtc_ov_buffer_read(int32_t index);
/* The result. Converted to the method's return type (QTC_ERR_CONVERSION or
   QTC_ERR_OUT_OF_RANGE when it does not fit). Not set: a default value (0,
   false, an empty string, an invalid index) for methods Qt has no
   implementation of, the class's own result after qtc_ov_call_base. */
QTC_API qtc_status qtc_ov_ret_bool(bool v);
QTC_API qtc_status qtc_ov_ret_int(int64_t v);
QTC_API qtc_status qtc_ov_ret_uint(uint64_t v);
QTC_API qtc_status qtc_ov_ret_double(double v);
QTC_API qtc_status qtc_ov_ret_str(qtc_str v);
QTC_API qtc_status qtc_ov_ret_bytes(qtc_str v);
QTC_API qtc_status qtc_ov_ret_val(qtc_val v);         /* GUI thread: a copy of v */
/* A model's index, parent, sibling or buddy override: an index of this model
   (createIndex(row, column, id)); row < 0 gives an invalid index. */
QTC_API qtc_status qtc_ov_ret_index(int32_t row, int32_t column, uint64_t id);
/* Runs the class's own implementation with the call's arguments (as out-
   parameters stand now); its result becomes the call's result and, on the GUI
   thread, the return register's (qtc_ret_*). */
QTC_API qtc_status qtc_ov_call_base(void);
QTC_API int32_t qtc_ov_depth(void);                   /* override calls in progress on this thread */

/* ---------------------------------------------------------- runtime classes */

/* New QObject classes, defined while the program runs: signals, slots,
   invokables and properties that Qt, QML (as a context property, say) and
   qtc_call see like a C++ class's. Define one between qtc_class_begin and
   qtc_class_end, then make objects with qtc_class_new (or qtc_new). base:
   "QObject" (or empty), or any overridable class (qtc_overridable_classes): the
   objects are overridable objects too (qtc_override).
   Types in signatures: the names Qt's meta-type system knows ("int", "double",
   "QString", "QVariant", "QVariantList", "QColor", ...) and QObject* for objects.
   Slots and invokables run the override handler (qtc_set_override_handler)
   with the tag given here, on the thread Qt calls them from: an object moved to
   a worker thread (qtc_move_to_thread) runs them there. The handler reads the
   arguments with qtc_ov_arg_* and sets the result with qtc_ov_ret_*.
   Properties keep their values in the object; a write that changes one emits
   its notify signal ("<name>Changed(<type>)", added unless the class declares
   one taking nothing or the value). */
#define QTC_PROP_READ     0u   /* every property is readable */
#define QTC_PROP_WRITE    1u
#define QTC_PROP_NOTIFY   2u
#define QTC_PROP_CONSTANT 4u
QTC_API qtc_status qtc_class_begin(qtc_str name, qtc_str base);
QTC_API qtc_status qtc_class_signal(qtc_str signature);                         /* "progress(int,QString)" */
QTC_API qtc_status qtc_class_slot(qtc_str signature, qtc_str return_type, uint64_t tag);   /* return_type {0,0}: void */
QTC_API qtc_status qtc_class_invokable(qtc_str signature, qtc_str return_type, uint64_t tag);
QTC_API qtc_status qtc_class_property(qtc_str name, qtc_str type, uint32_t flags);
QTC_API qtc_status qtc_class_end(void);      /* the class exists from now on (until qtc_shutdown) */
QTC_API qtc_status qtc_class_cancel(void);   /* drops the definition being made */
QTC_API bool qtc_class_exists_runtime(qtc_str class_name);
/* An object of a runtime class, made with its base's constructor arguments
   (pushed as for qtc_new). */
QTC_API qtc_obj qtc_class_new(qtc_str class_name);
/* Emits a signal of a runtime class's object with the pushed arguments: the
   name, or the signature for an overload. (qtc_call emits it too.) */
QTC_API qtc_status qtc_emit(qtc_obj obj, qtc_str signal);

/* -------------------------------------------- tasks, futures and threads */

/* A future: the results of work that runs elsewhere (a task on a thread pool,
   a promise any thread fills, a queued call). Results are values kept in order
   until the future is released. Futures are not object handles. */
typedef uint64_t qtc_future;

#define QTC_FUT_STARTED   1
#define QTC_FUT_RUNNING   2
#define QTC_FUT_FINISHED  4
#define QTC_FUT_CANCELED  8
#define QTC_FUT_SUSPENDED 16   /* suspended, or asked to suspend */
#define QTC_FUT_FAILED    32   /* finished with qtc_promise_fail */

/* Promises (any thread, but qtc_promise_add_val): the producing side of a
   future. qtc_promise_new returns a started future that any thread may fill;
   pass its id to the thread doing the work. Every promise call fails with
   QTC_ERR_STATE once the future is finished, QTC_ERR_STALE_HANDLE once released. */
QTC_API qtc_future qtc_promise_new(void);
QTC_API qtc_status qtc_promise_progress_range(qtc_future f, int32_t min, int32_t max);
/* text {0, 0}: leave the progress text as it is */
QTC_API qtc_status qtc_promise_progress(qtc_future f, int32_t value, qtc_str text);
QTC_API qtc_status qtc_promise_add_bool(qtc_future f, bool v);
QTC_API qtc_status qtc_promise_add_int(qtc_future f, int64_t v);
QTC_API qtc_status qtc_promise_add_double(qtc_future f, double v);
QTC_API qtc_status qtc_promise_add_str(qtc_future f, qtc_str v);
QTC_API qtc_status qtc_promise_add_bytes(qtc_future f, qtc_str v);
QTC_API qtc_status qtc_promise_add_f64s(qtc_future f, qtc_f64s v);   /* a list of doubles */
/* JSON object or array: the result is a map or a list */
QTC_API qtc_status qtc_promise_add_json(qtc_future f, qtc_str json);
QTC_API qtc_status qtc_promise_add_val(qtc_future f, qtc_val v);     /* GUI thread: a copy of v */
/* Finishes the future as failed, with a message (qtc_future_error). */
QTC_API qtc_status qtc_promise_fail(qtc_future f, qtc_str message);
QTC_API qtc_status qtc_promise_finish(qtc_future f);
/* Whether the consumer canceled it (also true once it is released or finished
   canceled): long work checks this and stops. */
QTC_API bool qtc_promise_canceled(qtc_future f);
/* Blocks while the consumer has the future suspended (qtc_future_suspend).
   Not on the GUI thread (QTC_ERR_WRONG_THREAD). */
QTC_API qtc_status qtc_promise_suspend_if_requested(qtc_future f);

/* Tasks: C functions run on Qt's global thread pool. fn(f, user) runs on a pool
   thread and fills the future with the promise functions; when it returns, the
   future finishes (unless it already did). A future canceled before its task
   starts finishes without running it. cleanup(user), if given, runs exactly
   once after fn returns (on the pool thread), or instead of it when the task
   never runs. Any thread. Returns 0 on failure. */
typedef void (*qtc_task_fn)(qtc_future f, void *user);
typedef void (*qtc_map_fn)(int64_t index, void *user);
QTC_API qtc_future qtc_task_start(qtc_task_fn fn, void *user, void (*cleanup)(void *user));
/* The same on a QThreadPool object (0: the global pool), with Qt's priority
   (higher runs sooner). GUI thread. */
QTC_API qtc_future qtc_task_start_in(qtc_obj pool, int32_t priority, qtc_task_fn fn, void *user,
                                     void (*cleanup)(void *user));
/* Calls fn(i, user) for every i in [0, count), in parallel chunks on the global
   pool. Progress counts finished chunks; cancel stops it between chunks and
   suspend pauses it there. fn writes its results where user points (no results
   are added to the future). Any thread. */
QTC_API qtc_future qtc_task_map(int64_t count, qtc_map_fn fn, void *user, void (*cleanup)(void *user));

/* Futures: the consuming side. */
QTC_API int32_t qtc_future_state(qtc_future f);           /* QTC_FUT_* bits, < 0 on error; any thread */
QTC_API int64_t qtc_future_result_count(qtc_future f);    /* any thread */
QTC_API qtc_status qtc_future_progress(qtc_future f, int32_t *value, int32_t *min, int32_t *max);  /* any thread */
/* Result index into the return register (qtc_ret_*). GUI thread. */
QTC_API qtc_status qtc_future_result(qtc_future f, int64_t index);
/* The qtc_promise_fail message ("" if none). Any thread; valid until the next
   qtc_future_error on the same thread. */
QTC_API qtc_view qtc_future_error(qtc_future f);
QTC_API qtc_status qtc_future_cancel(qtc_future f);                 /* any thread */
QTC_API qtc_status qtc_future_suspend(qtc_future f, bool suspend);  /* any thread */
/* Waits until the future finishes: QTC_WAIT_FINISHED, or QTC_WAIT_TIMEOUT when
   timeout_ms (< 0: forever) passes first. On the GUI thread it runs Qt's event
   loop meanwhile, queueing qtc events as qtc_wait does (and returns early if
   the application quits); on other threads it blocks. Any thread. */
QTC_API int32_t qtc_future_wait(qtc_future f, int64_t timeout_ms);
/* Frees it, canceling and finishing it if it is not finished yet: a task still
   running sees qtc_promise_canceled. Any thread. */
QTC_API qtc_status qtc_future_release(qtc_future f);
/* A future that finishes when both have finished / when either has (its one
   result is the id of the one that finished first). Release it too. */
QTC_API qtc_future qtc_future_all(qtc_future a, qtc_future b);
QTC_API qtc_future qtc_future_any(qtc_future a, qtc_future b);
/* Delivers the future's progress as QTC_EV_FUTURE_* events with this tag,
   until it finishes. Returns a watch id (0 on failure). GUI thread. */
QTC_API uint64_t qtc_future_watch(qtc_future f, uint64_t tag);
QTC_API qtc_status qtc_future_unwatch(uint64_t watch);
QTC_API int64_t qtc_live_futures(void);

/* Worker objects. Moves an object (with its children) to a QThread's event
   loop. Allowed for objects with no parent that nothing else uses: not widgets,
   windows, QML or Qt Quick objects, models, animations, or Qt classes outside
   Qt Core and the network modules; nothing in the GUI thread may hold it, and
   it may not have qtc_watch watches or qtc_connect_to forwards. Afterwards
   qtc_call refuses it (QTC_ERR_WRONG_THREAD): reach it with qtc_invoke_queued /
   qtc_invoke_future; its signals (qtc_connect) arrive as events as before, and
   qtc_destroy deletes it in its thread (directly once that thread has
   finished). When its QThread is destroyed, it comes back to the GUI thread. */
QTC_API qtc_status qtc_move_to_thread(qtc_obj obj, qtc_obj thread);
/* Calls a slot, signal or invokable the way qtc_call reaches meta-object
   methods (the program's own classes, qtc runtime classes, QML objects), with
   the pushed arguments, from the target's event loop: later on the GUI thread,
   or in the thread the object lives in. Objects cannot be passed to (or
   returned from) another thread. qtc_invoke_future returns a future that gets
   the method's return value as its result. GUI thread. */
QTC_API qtc_status qtc_invoke_queued(qtc_obj obj, qtc_str method);
QTC_API qtc_future qtc_invoke_future(qtc_obj obj, qtc_str method);

/* ---------------------------------------------------------------- drawing */

/* Records drawing commands for a QtcCanvas object (a widget that replays the
   last finished recording whenever it paints) or draws them straight into a
   QImage or QPixmap value. Coordinates are doubles in the target's pixels. */
QTC_API qtc_status qtc_draw_begin(uint64_t target);
/* Inside an override that paints: qtc_draw_begin(self) in a widget's or
   window's paintEvent (or paintGL) override draws on it when the recording
   ends; qtc_draw_begin_painter(i) draws with the override's QPainter argument
   i (QGraphicsScene::drawBackground, ...). A recording still open when the
   handler returns is ended then. */
QTC_API qtc_status qtc_draw_begin_painter(int32_t arg_index);
QTC_API qtc_status qtc_draw_end(void);
QTC_API qtc_status qtc_draw_clear(uint32_t argb);
QTC_API qtc_status qtc_draw_pen(uint32_t argb, double width, int32_t style, int32_t cap, int32_t join);
QTC_API qtc_status qtc_draw_no_pen(void);
QTC_API qtc_status qtc_draw_brush(uint32_t argb, int32_t style);
QTC_API qtc_status qtc_draw_no_brush(void);
QTC_API qtc_status qtc_draw_linear_gradient(double x1, double y1, double x2, double y2,
                                            uint32_t argb1, uint32_t argb2);
QTC_API qtc_status qtc_draw_font(qtc_str family, double point_size, int32_t weight, bool italic);
QTC_API qtc_status qtc_draw_antialias(bool on);
QTC_API qtc_status qtc_draw_opacity(double opacity);
QTC_API qtc_status qtc_draw_save(void);
QTC_API qtc_status qtc_draw_restore(void);
QTC_API qtc_status qtc_draw_translate(double dx, double dy);
QTC_API qtc_status qtc_draw_rotate(double degrees);
QTC_API qtc_status qtc_draw_scale(double sx, double sy);
QTC_API qtc_status qtc_draw_clip_rect(double x, double y, double w, double h);
QTC_API qtc_status qtc_draw_point(double x, double y);
QTC_API qtc_status qtc_draw_line(double x1, double y1, double x2, double y2);
QTC_API qtc_status qtc_draw_rect(double x, double y, double w, double h);
QTC_API qtc_status qtc_draw_rounded_rect(double x, double y, double w, double h, double rx, double ry);
QTC_API qtc_status qtc_draw_ellipse(double x, double y, double w, double h);
/* Angles in degrees, counter-clockwise from 3 o'clock. */
QTC_API qtc_status qtc_draw_arc(double x, double y, double w, double h, double start, double span);
QTC_API qtc_status qtc_draw_pie(double x, double y, double w, double h, double start, double span);
QTC_API qtc_status qtc_draw_polyline(qtc_f64s xy);
QTC_API qtc_status qtc_draw_polygon(qtc_f64s xy);
QTC_API qtc_status qtc_draw_path_move(double x, double y);
QTC_API qtc_status qtc_draw_path_line(double x, double y);
QTC_API qtc_status qtc_draw_path_quad(double cx, double cy, double x, double y);
QTC_API qtc_status qtc_draw_path_cubic(double c1x, double c1y, double c2x, double c2y, double x, double y);
QTC_API qtc_status qtc_draw_path_close(void);
QTC_API qtc_status qtc_draw_path_end(void);   /* draws the path built since the last end */
QTC_API qtc_status qtc_draw_text(double x, double y, qtc_str text);
/* flags: Qt::AlignmentFlag | Qt::TextFlag */
QTC_API qtc_status qtc_draw_text_in(double x, double y, double w, double h, int32_t flags, qtc_str text);
QTC_API qtc_status qtc_draw_image(qtc_val image, double x, double y, double w, double h);
/* Number of commands recorded so far (for tests and budgeting). */
QTC_API int64_t qtc_draw_count(void);

/* --------------------------------------------------------------- graphics */

/* Qt Quick's graphics API, for the windows made after the call (set it before
   the first Qt Quick window): software, OpenGL, Vulkan, Direct3D 11 or 12,
   Metal, or none (Null). */
#define QTC_GAPI_UNKNOWN  0
#define QTC_GAPI_SOFTWARE 1
#define QTC_GAPI_OPENGL   2
#define QTC_GAPI_VULKAN   3
#define QTC_GAPI_D3D11    4
#define QTC_GAPI_D3D12    5
#define QTC_GAPI_METAL    6
#define QTC_GAPI_NULL     7
QTC_API qtc_status qtc_quick_set_graphics_api(int32_t api);
/* The API a Qt Quick window renders with (QTC_GAPI_UNKNOWN until its scene
   graph starts); window 0: the one requested for new windows. */
QTC_API int32_t qtc_quick_graphics_api(qtc_obj window);

/* An OpenGL function of the context current on this thread (in initializeGL,
   paintGL and resizeGL overrides, in an RHI frame on OpenGL, or after
   qtc_rhi_make_current), to call through a function pointer of its type. */
QTC_API void *qtc_gl_proc_address(qtc_str name);

/* Qt's rendering hardware interface (QRhi): one API over Vulkan, OpenGL (ES),
   Direct3D 11 and 12 and Metal. An RHI renders offscreen into textures, into a
   window through a swapchain, or into a QRhiWidget inside its render override.

   qtc_rhi handles name an RHI, the resources made with it (buffers, textures,
   samplers, render targets, swapchains, bindings, pipelines, readbacks) and
   shaders; they are not object handles. Destroying an RHI destroys its
   resources; a resource something else still uses (a texture a render target
   draws into, a buffer in created bindings, the bindings a pipeline was made
   with) cannot be destroyed before that user. GUI thread only.

   Every command is checked before the GPU sees it: the frame and pass it needs;
   a pipeline's vertex inputs, interface and bindings against the shaders'
   reflection; every draw against the sizes of the buffers it reads (indexed
   draws against the indices uploaded into the index buffer); dispatches against
   the device's limits. What a shader computes is not checked: a shader that
   indexes outside its buffers or images is undefined behaviour on the GPU. */
typedef uint64_t qtc_rhi;

#define QTC_RHI_BACKEND_NULL    0   /* no GPU: records and checks, draws nothing */
#define QTC_RHI_BACKEND_OPENGL  1   /* OpenGL 2.1+ or OpenGL ES 2.0+ */
#define QTC_RHI_BACKEND_VULKAN  2
#define QTC_RHI_BACKEND_D3D11   3   /* Windows */
#define QTC_RHI_BACKEND_D3D12   4   /* Windows */
#define QTC_RHI_BACKEND_METAL   5   /* macOS, iOS */
#define QTC_RHI_DEBUG 1u            /* debug markers, and the backend's validation layer when installed */
/* QTC_ERR_UNSUPPORTED when the backend is not in this build or has no device here. */
QTC_API qtc_rhi qtc_rhi_create(int32_t backend, uint32_t flags);
/* Destroys an RHI (with everything made with it), a resource, a readback or a
   shader. Resources used by the frame being recorded go when it ends. */
QTC_API qtc_status qtc_rhi_destroy(qtc_rhi h);
/* JSON: {"backend", "driver", "compute", "instancing", "index32", "base_vertex",
   "base_instance", "int_attributes", "max_texture_size", "max_uniform_buffer",
   "max_vertex_inputs", "max_compute_groups", "ubuf_alignment", "y_up_in_ndc",
   "y_up_in_framebuffer", "clip_space_corr" (16 doubles, column-major)}. */
QTC_API qtc_view qtc_rhi_info(qtc_rhi rhi);
#define QTC_RHI_FEATURE_COMPUTE           1
#define QTC_RHI_FEATURE_INSTANCING        2
#define QTC_RHI_FEATURE_INDEX32           3   /* 32-bit indices */
#define QTC_RHI_FEATURE_BASE_VERTEX       4   /* vertex_offset in indexed draws */
#define QTC_RHI_FEATURE_BASE_INSTANCE     5   /* first_instance */
#define QTC_RHI_FEATURE_INT_ATTRIBUTES    6   /* QTC_RHI_INT and QTC_RHI_UINT vertex inputs */
#define QTC_RHI_FEATURE_READ_BACK_BUFFERS 7   /* readbacks of buffers other than uniform buffers */
QTC_API bool qtc_rhi_supports(qtc_rhi rhi, int32_t feature);
QTC_API int64_t qtc_rhi_live(void);   /* RHI handles alive: RHIs, resources, readbacks, shaders */
/* Waits until the GPU has done everything submitted, completing readbacks.
   Outside frames. */
QTC_API qtc_status qtc_rhi_finish(qtc_rhi rhi);

/* Shaders. Stages (also visibility bits for bindings): */
#define QTC_RHI_VERTEX   1u
#define QTC_RHI_FRAGMENT 2u
#define QTC_RHI_COMPUTE  4u
/* Compiles Vulkan-style GLSL (#version 440, layout(binding = n) on every
   resource, layout(location = n) on every input and output) for every backend
   this platform has, with Qt Shader Tools: QTC_ERR_UNSUPPORTED without it (load
   packages made by Qt's qsb tool instead). Compile errors: QTC_ERR_INVALID_ARG
   with the compiler's message. */
QTC_API qtc_rhi qtc_rhi_shader_bake(uint32_t stage, qtc_str glsl);
QTC_API qtc_rhi qtc_rhi_shader_load(qtc_str qsb);         /* a .qsb shader package */
QTC_API qtc_view qtc_rhi_shader_save(qtc_rhi shader);      /* its .qsb bytes, for caching */
QTC_API qtc_view qtc_rhi_shader_reflect(qtc_rhi shader);   /* the reflection data, JSON */

/* Buffers. Uniform buffers are QTC_RHI_DYNAMIC, with no other usage; index and
   storage buffers are QTC_RHI_IMMUTABLE or QTC_RHI_STATIC. size: (0, 1 GiB]. */
#define QTC_RHI_IMMUTABLE 0   /* uploaded rarely (stays on the GPU) */
#define QTC_RHI_STATIC    1   /* uploaded now and then */
#define QTC_RHI_DYNAMIC   2   /* rewritten every frame */
#define QTC_RHI_VERTEX_BUFFER  1u
#define QTC_RHI_INDEX_BUFFER   2u
#define QTC_RHI_UNIFORM_BUFFER 4u
#define QTC_RHI_STORAGE_BUFFER 8u
QTC_API qtc_rhi qtc_rhi_buffer(qtc_rhi rhi, int32_t type, uint32_t usage, int64_t size);

/* Textures (2D, one mip level) and samplers. */
#define QTC_RHI_RGBA8   1
#define QTC_RHI_BGRA8   2
#define QTC_RHI_R8      3
#define QTC_RHI_RGBA16F 4
#define QTC_RHI_RGBA32F 5
#define QTC_RHI_R32F    6
#define QTC_RHI_RENDER_TARGET 1u   /* texture flags: qtc_rhi_render_target draws into it */
#define QTC_RHI_READABLE      2u   /* qtc_rhi_read_texture */
#define QTC_RHI_STORAGE_IMAGE 4u   /* qtc_rhi_bind_image */
QTC_API qtc_rhi qtc_rhi_texture(qtc_rhi rhi, int32_t format, int32_t w, int32_t h, uint32_t flags);
QTC_API qtc_rhi qtc_rhi_sampler(qtc_rhi rhi, bool linear, bool repeat);
/* A target drawing into a texture made with QTC_RHI_RENDER_TARGET, with a
   depth-stencil buffer of its size when depth is true. */
QTC_API qtc_rhi qtc_rhi_render_target(qtc_rhi texture, bool depth);
/* A swapchain presenting into a QWindow (not a widget's, a Qt Quick or a
   QPaintDeviceWindow, which draw themselves). Make it before the window is
   first shown: it sets the window's surface type (and Vulkan instance) to the
   RHI's. Frames on it (qtc_rhi_begin_frame) draw into the window; it follows
   the window's size. */
#define QTC_RHI_SC_DEPTH     1u   /* a depth-stencil buffer */
#define QTC_RHI_SC_READABLE  2u   /* qtc_rhi_read_swapchain */
#define QTC_RHI_SC_NO_VSYNC  4u
QTC_API qtc_rhi qtc_rhi_swapchain(qtc_rhi rhi, qtc_obj window, uint32_t flags);

/* Bindings: the resources shaders see, by binding number, and the stages that
   see each (QTC_RHI_VERTEX | ...). Add them, then create; created bindings do
   not change. */
QTC_API qtc_rhi qtc_rhi_bindings(qtc_rhi rhi);
/* size 0: from offset to the end. offset: a multiple of the device's uniform
   buffer alignment ("ubuf_alignment"). */
QTC_API qtc_status qtc_rhi_bind_uniform(qtc_rhi bindings, int32_t binding, uint32_t stages, qtc_rhi buffer,
                                        int64_t offset, int64_t size);
QTC_API qtc_status qtc_rhi_bind_texture(qtc_rhi bindings, int32_t binding, uint32_t stages, qtc_rhi texture,
                                        qtc_rhi sampler);
#define QTC_RHI_LOAD       1   /* storage access */
#define QTC_RHI_STORE      2
#define QTC_RHI_LOAD_STORE 3
QTC_API qtc_status qtc_rhi_bind_storage(qtc_rhi bindings, int32_t binding, uint32_t stages, qtc_rhi buffer,
                                        int32_t access);
/* A texture made with QTC_RHI_STORAGE_IMAGE, as a compute shader's image. */
QTC_API qtc_status qtc_rhi_bind_image(qtc_rhi bindings, int32_t binding, uint32_t stages, qtc_rhi texture,
                                      int32_t access);
QTC_API qtc_status qtc_rhi_bindings_create(qtc_rhi bindings);

/* Graphics pipelines: configure, then create. */
#define QTC_RHI_FLOAT       1   /* vertex attribute formats */
#define QTC_RHI_FLOAT2      2
#define QTC_RHI_FLOAT3      3
#define QTC_RHI_FLOAT4      4
#define QTC_RHI_UNORM_BYTE4 5   /* 4 bytes as floats in [0, 1] */
#define QTC_RHI_INT         6
#define QTC_RHI_UINT        7
#define QTC_RHI_TRIANGLES      0   /* topologies */
#define QTC_RHI_TRIANGLE_STRIP 1
#define QTC_RHI_LINES          2
#define QTC_RHI_LINE_STRIP     3
#define QTC_RHI_POINTS         4   /* the vertex shader writes gl_PointSize */
#define QTC_RHI_CULL_NONE  0
#define QTC_RHI_CULL_FRONT 1
#define QTC_RHI_CULL_BACK  2
QTC_API qtc_rhi qtc_rhi_pipeline(qtc_rhi rhi);
QTC_API qtc_status qtc_rhi_pipeline_shaders(qtc_rhi pipeline, qtc_rhi vertex, qtc_rhi fragment);
/* Vertex bindings are numbered in the order they are added (stride: bytes per
   vertex or instance, a multiple of 4). */
QTC_API qtc_status qtc_rhi_pipeline_vertex_binding(qtc_rhi pipeline, int32_t stride, bool per_instance);
QTC_API qtc_status qtc_rhi_pipeline_vertex_attribute(qtc_rhi pipeline, int32_t binding, int32_t location,
                                                     int32_t format, int32_t offset);
/* blend: premultiplied alpha. Defaults: triangles, no culling, no blending, no depth. */
QTC_API qtc_status qtc_rhi_pipeline_options(qtc_rhi pipeline, int32_t topology, int32_t cull, bool blend,
                                            bool depth_test, bool depth_write);
/* bindings: created bindings with the layout the shaders use (any bindings of
   the same layout serve its draws later). target: a render target, a swapchain
   or a QRhiWidget's target; the pipeline draws into every target compatible
   with it. */
QTC_API qtc_status qtc_rhi_pipeline_create(qtc_rhi pipeline, qtc_rhi bindings, qtc_rhi target);
QTC_API qtc_rhi qtc_rhi_compute_pipeline(qtc_rhi rhi, qtc_rhi shader, qtc_rhi bindings);

/* Data. Uploads and readbacks are queued, and run when the next pass begins or
   ends, or the frame ends: draws in a pass see what was uploaded before it. */
QTC_API qtc_status qtc_rhi_upload(qtc_rhi buffer, int64_t offset, qtc_str bytes);
/* A QImage value of the texture's size into an RGBA8, BGRA8 or R8 texture. */
QTC_API qtc_status qtc_rhi_upload_image(qtc_rhi texture, qtc_val image);
/* The whole texture, rows tightly packed, in its format. */
QTC_API qtc_status qtc_rhi_upload_pixels(qtc_rhi texture, qtc_str pixels);
/* Readbacks: handles that complete when the frame that runs them has finished
   on the GPU (offscreen frames: when qtc_rhi_end_frame returns; window and
   widget frames: a frame or two later, or at qtc_rhi_finish). */
QTC_API qtc_rhi qtc_rhi_read_texture(qtc_rhi texture);          /* made with QTC_RHI_READABLE */
QTC_API qtc_rhi qtc_rhi_read_buffer(qtc_rhi buffer, int64_t offset, int64_t size);
QTC_API qtc_rhi qtc_rhi_read_swapchain(qtc_rhi swapchain);      /* in a frame on it, QTC_RHI_SC_READABLE */
QTC_API bool qtc_rhi_readback_ready(qtc_rhi readback);
QTC_API qtc_view qtc_rhi_readback_bytes(qtc_rhi readback);
/* RGBA8 and BGRA8 readbacks as a QImage value (rows top to bottom). */
QTC_API qtc_val qtc_rhi_readback_image(qtc_rhi readback);

/* Frames. swapchain 0: an offscreen frame; else a frame drawing into the
   swapchain's window, presented by qtc_rhi_end_frame. QTC_RHI_SKIPPED (not an
   error): the window cannot be drawn into now (hidden, minimized, size 0); try
   again on its next expose. */
#define QTC_RHI_SKIPPED 1
QTC_API qtc_status qtc_rhi_begin_frame(qtc_rhi rhi, qtc_rhi swapchain);
QTC_API qtc_status qtc_rhi_end_frame(qtc_rhi rhi);
/* Passes. target: a render target, the swapchain of the frame, or a QRhiWidget's
   target (qtc_rhi_widget_target). The viewport starts as the whole target.
   QTC_RHI_PASS_EXTERNAL: native commands may be recorded in it
   (qtc_rhi_begin_external). */
#define QTC_RHI_PASS_EXTERNAL 1u
QTC_API qtc_status qtc_rhi_begin_pass(qtc_rhi rhi, qtc_rhi target, uint32_t clear_argb, double depth,
                                      uint32_t flags);
QTC_API qtc_status qtc_rhi_begin_compute(qtc_rhi rhi, uint32_t flags);
QTC_API qtc_status qtc_rhi_end_pass(qtc_rhi rhi);   /* a render or a compute pass */
QTC_API qtc_status qtc_rhi_set_pipeline(qtc_rhi rhi, qtc_rhi pipeline);
QTC_API qtc_status qtc_rhi_set_bindings(qtc_rhi rhi, qtc_rhi bindings);
QTC_API qtc_status qtc_rhi_set_viewport(qtc_rhi rhi, double x, double y, double w, double h);
QTC_API qtc_status qtc_rhi_set_vertex_buffer(qtc_rhi rhi, int32_t binding, qtc_rhi buffer, int64_t offset);
QTC_API qtc_status qtc_rhi_set_index_buffer(qtc_rhi rhi, qtc_rhi buffer, int64_t offset, bool index32);
QTC_API qtc_status qtc_rhi_draw(qtc_rhi rhi, int64_t vertices, int64_t instances, int64_t first_vertex,
                                int64_t first_instance);
QTC_API qtc_status qtc_rhi_draw_indexed(qtc_rhi rhi, int64_t indices, int64_t instances, int64_t first_index,
                                        int64_t vertex_offset, int64_t first_instance);
QTC_API qtc_status qtc_rhi_dispatch(qtc_rhi rhi, int32_t x, int32_t y, int32_t z);

/* QRhiWidget: make one overridable (qtc_new_overridable "QRhiWidget") and
   override initialize(QRhiCommandBuffer*) and render(QRhiCommandBuffer*). In
   those overrides, qtc_rhi_widget(self) is the widget's RHI with a frame open
   (record passes, uploads and draws as in a frame of your own; the frame ends
   with the call), and qtc_rhi_widget_target(self) its target for passes. The
   target handle is valid during that call only; resources made with the
   widget's RHI live until it goes (releaseResources is called before that). */
QTC_API qtc_rhi qtc_rhi_widget(qtc_obj widget);
QTC_API qtc_rhi qtc_rhi_widget_target(qtc_obj widget);

/* Native interop: the objects behind an RHI, for code that calls Vulkan,
   OpenGL, Direct3D or Metal itself. */
typedef struct qtc_rhi_native_handles {
    int32_t backend;           /* QTC_RHI_BACKEND_* */
    void *instance;            /* VkInstance */
    void *physical_device;     /* VkPhysicalDevice */
    void *device;              /* VkDevice, ID3D11Device*, ID3D12Device*, id<MTLDevice>, QOpenGLContext* */
    void *queue;               /* VkQueue, ID3D11DeviceContext*, ID3D12CommandQueue*, id<MTLCommandQueue> */
    uint32_t queue_family;     /* Vulkan */
    uint32_t queue_index;      /* Vulkan */
} qtc_rhi_native_handles;
QTC_API qtc_status qtc_rhi_native(qtc_rhi rhi, qtc_rhi_native_handles *out);
/* A buffer's or texture's native object (VkBuffer, VkImage, GLuint, ID3D11Buffer*,
   ...; 0 if none); layout: a texture's current VkImageLayout (or 0). */
QTC_API uint64_t qtc_rhi_native_object(qtc_rhi resource, int32_t *layout);
/* Makes an OpenGL RHI's context current on this thread, outside frames. */
QTC_API qtc_status qtc_rhi_make_current(qtc_rhi rhi);
/* Inside a pass begun with QTC_RHI_PASS_EXTERNAL: native commands may be
   recorded until qtc_rhi_end_external, into *native_cb (VkCommandBuffer,
   ID3D12GraphicsCommandList*, id<MTLRenderCommandEncoder>; null for OpenGL and
   Direct3D 11, whose commands go to the current context). The pipeline and
   bindings must be set again afterwards. */
QTC_API qtc_status qtc_rhi_begin_external(qtc_rhi rhi, void **native_cb);
QTC_API qtc_status qtc_rhi_end_external(qtc_rhi rhi);

/* Vulkan without an RHI: a Vulkan instance (extensions and layers: JSON arrays
   of names; {0, 0}: none), its functions, and surfaces for windows. Destroy it
   with qtc_rhi_destroy; windows using it lose their native window first. */
QTC_API qtc_rhi qtc_vk_create(qtc_str extensions, qtc_str layers);
/* h: an instance from qtc_vk_create, or a Vulkan RHI. */
QTC_API void *qtc_vk_instance(qtc_rhi h);                     /* VkInstance */
QTC_API void *qtc_vk_proc_address(qtc_rhi h, qtc_str name);   /* vkGetInstanceProcAddr */
/* Before the window is first shown: makes it a Vulkan surface of the instance. */
QTC_API qtc_status qtc_vk_attach(qtc_rhi h, qtc_obj window);
QTC_API uint64_t qtc_vk_surface(qtc_rhi h, qtc_obj window);  /* VkSurfaceKHR, once the window exists */

/* ---------------------------------------------------------------- interop */

/* For C++ code that shares objects with qtc. The pointer is only valid while
   the object lives; prefer handles. */
QTC_API void *qtc_unsafe_pointer(qtc_obj obj);
QTC_API qtc_obj qtc_wrap_pointer(void *qobject);

#ifdef __cplusplus
}
#endif

#endif /* QTC_H */
