# Using qtc

A guide to [include/qtc.h](../include/qtc.h), which has the full reference. All
snippets assume `#include "qtc.h"`; `QTC_S("literal")` makes a `qtc_str` from a
string literal and `qtc_s(ptr)` from a NUL-terminated string.

## Starting and stopping

```c
qtc_init_argv(QTC_APP_WIDGETS, argc, (const char *const *)argv);   /* or QTC_APP_GUI, QTC_APP_CORE */
qtc_set_error_policy(QTC_ERRORS_LOG);                              /* print every failure */
/* ... */
qtc_shutdown();
```

`QTC_APP_WIDGETS` creates a `QApplication` (widgets, QML, everything),
`QTC_APP_GUI` a `QGuiApplication` (QML and painting, no widgets) and
`QTC_APP_CORE` a `QCoreApplication`. Without a display on Linux, set
`QT_QPA_PLATFORM=offscreen` (qtc says so instead of letting Qt abort).

The thread that calls `qtc_init` is the GUI thread; every other function must be
called from it, except `qtc_post`.

## Objects

```c
qtc_obj win = qtc_new(QTC_S("QWidget"));
qtc_arg_str(QTC_S("Hello"));
qtc_arg_obj(win);                               /* QLabel(const QString &, QWidget *parent) */
qtc_obj label = qtc_new(QTC_S("QLabel"));
qtc_destroy(win);                               /* the label goes with it */
qtc_alive(label);                               /* false once it is gone */
```

Constructor arguments are pushed first. A new object without a parent belongs to
qtc until it gets one, and `qtc_shutdown` destroys what qtc still owns.
`qtc_destroy` refuses objects Qt owns or that another object holds by a plain
pointer it cannot let go of; the message names the owner to destroy instead.
`qtc_find_child`, `qtc_class_name` and `qtc_inherits` look objects up.

## Calling methods

```c
qtc_arg_int(10);
qtc_arg_int(20);
qtc_call(win, QTC_S("resize"));                 /* by name: the overload is chosen from the arguments */
qtc_arg_str(QTC_S("Title"));
qtc_call(win, QTC_S("setWindowTitle(QString)"));/* or by signature */
qtc_call(label, QTC_S("text"));
qtc_view t = qtc_ret_str();                     /* the result: qtc_ret_int, _bool, _double, _obj, _val ... */
qtc_call_static(QTC_S("QDateTime"), QTC_S("currentDateTime"));
qtc_val now = qtc_ret_val();
```

Arguments are matched to parameters by kind: `qtc_arg_int` fits any integer or
enum parameter (with a range check), `qtc_arg_str` a `QString`, `QByteArray`
(UTF-8), `QUrl` and similar, `qtc_arg_val` any Qt value type, `qtc_arg_obj` a
`QObject *` of the right class, `qtc_arg_null` a null pointer. If two overloads fit
equally, the call fails with `QTC_ERR_AMBIGUOUS` and names both: pass a signature.
Trailing parameters with defaults may be left out, also in a signature.

Out-parameters such as `bool *ok` are left out of the call and read afterwards:

```c
qtc_val s = qtc_val_str(QTC_S("ff"));
qtc_arg_int(16);
qtc_call(s, QTC_S("toInt"));                    /* QString::toInt(bool *ok, int base) */
bool ok = qtc_out_bool(0);
int64_t n = qtc_ret_int();                      /* 255 */
```

Every call returns `QTC_OK` or a negative `QTC_ERR_*`, and records it for
`qtc_last_error` and `qtc_last_error_message`.

## Properties

```c
qtc_set_int(slider, QTC_S("value"), 42);
int64_t v = qtc_get_int(slider, QTC_S("value"));
qtc_set_str(label, QTC_S("text"), QTC_S("hi"));
qtc_arg_val(color);                             /* any type: push it and qtc_prop_set */
qtc_prop_set(widget, QTC_S("palette"));
qtc_prop_get(widget, QTC_S("geometry"));        /* then qtc_ret_* */
qtc_dynprop_set(obj, QTC_S("myTag"));           /* dynamic properties */
```

A property with a bound setter (`text` → `setText`) is written through it, with
the setter's checks. Enum properties take integers and are validated.

## Values

A `qtc_val` holds any Qt value: numbers, strings, lists, maps, and every value
class (`QColor`, `QFont`, `QImage`, `QDateTime`, `QModelIndex`, …).

```c
qtc_arg_str(QTC_S("#3388ff"));
qtc_val c = qtc_val_new(QTC_S("QColor"));      /* constructors take pushed arguments */
qtc_call(c, QTC_S("lighter"));                 /* values have methods */
qtc_val light = qtc_ret_val();
qtc_val list = qtc_val_from_json(QTC_S("[1, \"two\", {\"three\": 3}]"));
qtc_view json = qtc_val_to_json(list);
qtc_val_free(light);
qtc_val_free(c);
qtc_val_free(list);
```

`qtc_list_*` and `qtc_map_*` read and build lists and maps; `qtc_val_get_*`
reads scalars; `qtc_val_convert` converts between types. Values are copied into
calls, so freeing one after pushing it is fine. A value never holds a pointer to an
object, only its handle, so it stays safe after the object is gone.

## Strings

Input strings are `qtc_str`: UTF-8 bytes and a length (`-1` for NUL-terminated).
`QTC_S` measures a literal, so literals may contain NUL bytes. Output strings are
`qtc_view`: UTF-8, also NUL-terminated, owned by qtc and valid until the next
call that returns a view. Copy what you keep.

## Signals and the event loop

```c
qtc_connect(button, QTC_S("clicked"), TAG_CLICKED, 0);
qtc_connect(edit, QTC_S("textChanged"), TAG_TEXT, QTC_CONN_COALESCE);
qtc_watch(canvas, QTC_WATCH_MOUSE | QTC_WATCH(QTC_EV_CLOSE), QTC_WATCH(QTC_EV_CLOSE), TAG_INPUT);
qtc_obj timer = qtc_timer(1000, false, TAG_TICK);

qtc_event ev;
while (qtc_wait(-1) == QTC_WAIT_EVENTS) {
    while (qtc_poll(&ev)) {
        switch (ev.tag) {
        case TAG_CLICKED: /* ... */ break;
        case TAG_TEXT: {
            qtc_view text = qtc_event_arg_str(ev.seq, 0);   /* the signal's arguments */
            break;
        }
        case TAG_INPUT:
            if (ev.kind == QTC_EV_MOUSE_PRESS) { /* ev.d0, ev.d1: the position */ }
            if (ev.kind == QTC_EV_CLOSE) { /* consumed: the window stays open */ }
            break;
        }
    }
}
```

`qtc_wait(timeout_ms)` returns `QTC_WAIT_EVENTS` when something is queued,
`QTC_WAIT_TIMEOUT`, or `QTC_WAIT_QUIT` after `qtc_quit` or when the last window
closes (`qtc_set_quit_on_last_window_closed`). Events carry the connection's tag,
the source object, small integer and floating payloads (`i0`..`i3`, `d0`, `d1`, see
the event kinds in `qtc.h`), and every argument through `qtc_event_arg_*` for the
64 most recent events. A handler may destroy anything, including the sender;
later events from it are flagged `QTC_EVF_STALE_SOURCE`.

For a callback style, `qtc_set_handler(fn, user)` and `qtc_run()`.

`qtc_post(tag, a, b)` queues an event from any thread and wakes `qtc_wait`: it is
how worker threads report to the GUI.

`qtc_connect_to(sender, "valueChanged", receiver, "setValue")` calls a method
whenever a signal fires, inside the emission and without a trip through C. The
call is checked like `qtc_call`; a failure is reported and the emission goes on.

## Enums

```c
int64_t align = 0;
qtc_enum_value(QTC_S("Qt::AlignCenter"), &align);
qtc_view key = qtc_enum_key(QTC_S("Qt::AlignmentFlag"), 4);   /* "AlignHCenter" */
qtc_view all = qtc_enum_describe(QTC_S("QImage::Format"));     /* JSON */
```

## Drawing

```c
qtc_obj canvas = qtc_new(QTC_S("QtcCanvas"));      /* a widget that shows the last recording */
qtc_draw_begin(canvas);
qtc_draw_clear(0xff202020);
qtc_draw_pen(0xffffffff, 2, 1, 0, 0);
qtc_draw_line(0, 0, 100, 100);
qtc_draw_end();

qtc_arg_int(64); qtc_arg_int(64); qtc_arg_int(5 /* QImage::Format_ARGB32 */);
qtc_val img = qtc_val_new(QTC_S("QImage"));
qtc_draw_begin(img);                               /* or straight into an image */
qtc_draw_ellipse(8, 8, 48, 48);
qtc_draw_end();
```

Colours are `0xAARRGGBB`. There are pens, brushes, gradients, fonts, transforms,
clipping, paths, text and images; see the drawing section of `qtc.h`.

## QML

```c
qtc_obj engine = qtc_new(QTC_S("QQmlApplicationEngine"));
qtc_obj bridge = qtc_new(QTC_S("QtcBridge"));
qtc_call(engine, QTC_S("rootContext"));
qtc_obj ctx = qtc_ret_obj();
qtc_arg_str(QTC_S("bridge")); qtc_arg_obj(bridge);
qtc_call(ctx, QTC_S("setContextProperty"));
qtc_connect(bridge, QTC_S("sent"), TAG_FROM_QML, 0);
qtc_arg_str(QTC_S("main.qml"));
qtc_call(engine, QTC_S("load"));
```

In QML, `bridge.send("name", {any: "json-like value"})` arrives as a `sent`
event with the name and payload as arguments; `bridge.post(name, value)` from C
emits `message(name, payload)` in QML. QML objects' properties, functions and
signals are reached with the ordinary calls (`qtc_get_int(window, "clicks")`,
`qtc_call(window, "press")`, `qtc_connect(item, "clicked", ...)`).

## Subclassing Qt classes (overridable objects)

```c
enum { ROWS = 1, DATA };
static int32_t handler(const qtc_override_call *c, void *user) {
    if (c->tag == ROWS) { qtc_ov_ret_int(100); return QTC_OV_DONE; }
    if (c->tag == DATA) {
        int32_t row, col; uint64_t id;
        if (qtc_ov_arg_index(0, &row, &col, &id) && qtc_ov_arg_int(1) == 0 /* DisplayRole */) {
            char buf[32]; int n = snprintf(buf, sizeof buf, "row %d", row);
            qtc_ov_ret_str(qtc_sn(buf, n));
            return QTC_OV_DONE;
        }
    }
    return QTC_OV_BASE;   /* the class's own implementation */
}

qtc_set_override_handler(handler, NULL);
qtc_obj model = qtc_new_overridable(QTC_S("QAbstractListModel"));
qtc_override(model, QTC_S("rowCount"), ROWS);
qtc_override(model, QTC_S("data"), DATA);
```

`qtc_overridable_classes()` lists the classes (widgets, windows, item models and
proxies, validators, highlighters, devices, threads, servers, animations, scenes,
delegates, `QOpenGLWidget`, `QRhiWidget`) and `qtc_overridable_methods(class)`
their methods. A handler reads the call with `qtc_ov_arg_*` (events, model
indexes and device buffers included), sets the result with `qtc_ov_ret_*`, may
run the base first with `qtc_ov_call_base`, and draws in a `paintEvent` or
`paint()` with `qtc_draw_begin(self)` or `qtc_draw_begin_painter(arg)`. Protected
methods a subclass uses (`beginInsertRows`, `createIndex`, ...) are callable on
overridable objects. Languages without function pointers export the handler and
name it with `qtc_set_override_handler_symbol`.

## Classes defined at run time

```c
qtc_class_begin(QTC_S("Counter"), QTC_S("QObject"));
qtc_class_property(QTC_S("count"), QTC_S("int"), QTC_PROP_WRITE | QTC_PROP_NOTIFY);
qtc_class_signal(QTC_S("overflow(int)"));
qtc_class_invokable(QTC_S("add(int)"), QTC_S("int"), TAG_ADD);   /* runs the handler */
qtc_class_end();
qtc_obj c = qtc_class_new(QTC_S("Counter"));
qtc_set_int(c, QTC_S("count"), 3);              /* emits countChanged(int) */
qtc_arg_int(7); qtc_emit(c, QTC_S("overflow"));
```

Such objects are ordinary objects to Qt and QML: a `Counter` set as a context
property is used from QML like a C++ class.

## Futures, promises and worker threads

```c
static void work(qtc_future f, void *user) {      /* on a pool thread */
    qtc_promise_progress_range(f, 0, 100);
    for (int i = 0; i < 100 && !qtc_promise_canceled(f); i++) {
        qtc_promise_add_int(f, i * i);
        qtc_promise_progress(f, i + 1, qtc_sn(NULL, 0));
    }
}
qtc_future f = qtc_task_start(work, NULL, NULL);
qtc_future_watch(f, TAG_WORK);                     /* QTC_EV_FUTURE_* events */
if (qtc_future_wait(f, 5000) == QTC_WAIT_FINISHED) {
    qtc_future_result(f, 99);
    int64_t last = qtc_ret_int();
}
qtc_future_release(f);
```

Any thread can fill a promise (`qtc_promise_new`); `qtc_task_map` runs a
function over a range in parallel; `qtc_move_to_thread` moves a worker object to
a `QThread`, where `qtc_invoke_queued` and `qtc_invoke_future` call it.

## Graphics

```c
qtc_rhi r = qtc_rhi_create(QTC_RHI_BACKEND_VULKAN, 0);      /* or _OPENGL, _D3D11, _METAL, _NULL */
qtc_rhi tex = qtc_rhi_texture(r, QTC_RHI_RGBA8, 256, 256, QTC_RHI_RENDER_TARGET | QTC_RHI_READABLE);
qtc_rhi rt = qtc_rhi_render_target(tex, true);
qtc_rhi vbuf = qtc_rhi_buffer(r, QTC_RHI_IMMUTABLE, QTC_RHI_VERTEX_BUFFER, sizeof verts);
qtc_rhi_upload(vbuf, 0, qtc_sn((const char *)verts, sizeof verts));
qtc_rhi vs = qtc_rhi_shader_bake(QTC_RHI_VERTEX, QTC_S(vertex_glsl));   /* Vulkan-style GLSL 440 */
qtc_rhi fs = qtc_rhi_shader_bake(QTC_RHI_FRAGMENT, QTC_S(fragment_glsl));
qtc_rhi srb = qtc_rhi_bindings(r);
qtc_rhi_bindings_create(srb);
qtc_rhi p = qtc_rhi_pipeline(r);
qtc_rhi_pipeline_shaders(p, vs, fs);
qtc_rhi_pipeline_vertex_binding(p, 5 * sizeof(float), false);
qtc_rhi_pipeline_vertex_attribute(p, 0, 0, QTC_RHI_FLOAT2, 0);
qtc_rhi_pipeline_vertex_attribute(p, 0, 1, QTC_RHI_FLOAT3, 2 * sizeof(float));
qtc_rhi_pipeline_create(p, srb, rt);

qtc_rhi_begin_frame(r, 0);                                   /* 0: offscreen; or a swapchain */
qtc_rhi_begin_pass(r, rt, 0xff000000, 1.0, 0);
qtc_rhi_set_pipeline(r, p);
qtc_rhi_set_bindings(r, srb);
qtc_rhi_set_vertex_buffer(r, 0, vbuf, 0);
qtc_rhi_draw(r, 3, 1, 0, 0);
qtc_rhi_end_pass(r);
qtc_rhi rb = qtc_rhi_read_texture(tex);
qtc_rhi_end_frame(r);
qtc_val image = qtc_rhi_readback_image(rb);                  /* a QImage */
```

A window draws through a swapchain (`qtc_rhi_swapchain(r, window, flags)`, then
`qtc_rhi_begin_frame(r, swapchain)` each frame; `QTC_RHI_SKIPPED` while it is not
shown); a `QRhiWidget` draws in its `render` override with `qtc_rhi_widget(self)`
and `qtc_rhi_widget_target(self)`. Compute pipelines, storage buffers and images,
instancing and indexed draws are there too. Every command is checked before the
GPU sees it; see `qtc.h` and [LIMITS.md](LIMITS.md#graphics) for what is left out.
Code that calls Vulkan, OpenGL, Direct3D or Metal itself gets the native objects
(`qtc_rhi_native`, `qtc_rhi_native_object`), records native commands inside a
pass (`qtc_rhi_begin_external`), looks up OpenGL functions (`qtc_gl_proc_address`)
or makes its own Vulkan instance and window surfaces (`qtc_vk_*`).
`qtc_quick_set_graphics_api` picks the API Qt Quick renders with.

## Limits on what arguments may make Qt do

Arguments that would make Qt allocate more than a limit (256 MiB by default) in
one go are refused with `QTC_ERR_LIMIT`: a string resized to 2^31 characters, an
image of 100,000 x 100,000 pixels, a model with a billion rows.
`qtc_set_alloc_limit(bytes)` changes the limit.

## Introspection

`qtc_describe(0, "QSlider")` returns JSON with the class's properties, signals,
methods (with their parameters) and constructors; `qtc_describe(obj, "")` does the
same for a live object, QML objects included. `qtc_class_count` and
`qtc_class_at` list the bound classes. `manifest/qtc_api.json` has the same data
for all of them, for generating bindings in other languages.

## Testing your program

`libqtc_test` (`qtc_test.h`) simulates input with Qt Test: `qtct_click`,
`qtct_type`, `qtct_key`, `qtct_wait`, `qtct_grab` and more. Link it into test
programs only. Run them with `QT_QPA_PLATFORM=offscreen`.

## Interop with C++

`qtc_wrap_pointer(QObject *)` gives a handle to an object C++ code made, and
`qtc_unsafe_pointer(handle)` the object behind a handle. Both are for code that
mixes C++ and C; the pointer is only valid while the object lives.
