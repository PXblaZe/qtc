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
 * thread), except qtc_post, qtc_version, qtc_last_error* and qtc_status_name,
 * which are callable from any thread.
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
#define QTC_VERSION_MINOR 0
#define QTC_VERSION_PATCH 0
#define QTC_VERSION_STRING "1.0.0"

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
   DESTROYED: the watched object was destroyed (source is now stale). */
#define QTC_EV_SIGNAL              1
#define QTC_EV_POST                2
#define QTC_EV_TIMER               3
#define QTC_EV_LAST_WINDOW_CLOSED  4
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

#define QTC_WAIT_TIMEOUT 0
#define QTC_WAIT_EVENTS  1
#define QTC_WAIT_QUIT    2

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

/* ---------------------------------------------------------------- drawing */

/* Records drawing commands for a QtcCanvas object (a widget that replays the
   last finished recording whenever it paints) or draws them straight into a
   QImage or QPixmap value. Coordinates are doubles in the target's pixels. */
QTC_API qtc_status qtc_draw_begin(uint64_t target);
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

/* ---------------------------------------------------------------- interop */

/* For C++ code that shares objects with qtc. The pointer is only valid while
   the object lives; prefer handles. */
QTC_API void *qtc_unsafe_pointer(qtc_obj obj);
QTC_API qtc_obj qtc_wrap_pointer(void *qobject);

#ifdef __cplusplus
}
#endif

#endif /* QTC_H */
