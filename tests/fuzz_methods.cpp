// Calls every bound method on live objects and values with boundary arguments.
//
//   fuzz_methods --list [--classes RE]                one work item per line
//   fuzz_methods --start N [--end M] --state FILE     run items N..M-1; writes the current index to FILE first
//                [--classes RE]                      only classes whose name matches RE
//   fuzz_methods --order FILE --state FILE [--seed S]  run the items listed in FILE (one index per line)
//                                                    in that order; the state file counts positions in the
//                                                    list (fuzz_minimize.py shrinks failures with it)
//                [--seed S]                          S > 0: the items in an order of their own, and every
//                                                    method also gets random argument combinations drawn
//                                                    from a wider table of values (see push_arg)
//
// fuzz_driver.py runs it, notices crashes and hangs from the state file, records the
// item, and resumes after it. Before each call one line goes to stderr ("CALL ..."),
// so the log of a crashed run ends with the call that crashed. Blocking methods and
// methods that touch files, processes or application-wide state are flagged by the
// generator and skipped here.
//
// A method declared in a class that cannot be instantiated (QBoxLayout, QAbstractButton)
// runs on an instance of the nearest class that inherits it (QHBoxLayout, QPushButton).
#include "qtc.h"

#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QSet>
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QRegularExpression>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <unistd.h>
#if defined(__GLIBC__)
#include <execinfo.h>
#endif

// Sanitizers print their own report of a crash; elsewhere the fuzzer prints a stack itself
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define FUZZ_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define FUZZ_SANITIZED 1
#endif
#endif

static void on_crash(int sig, siginfo_t *info, void *) {
    char head[96];
    const int n = std::snprintf(head, sizeof head, "CRASH: fatal signal %d at address %p, stack:\n", sig,
                                info ? info->si_addr : nullptr);
    (void)!::write(2, head, size_t(n > 0 ? n : 0));
#if defined(__GLIBC__)
    void *frames[64];
    backtrace_symbols_fd(frames, backtrace(frames, 64), 2);
#endif
    std::raise(sig);   // the handler was reset: this ends the process as the signal would have
}

static void install_crash_handler() {
#if !defined(FUZZ_SANITIZED)
#if defined(__GLIBC__)
    void *warm[1];
    (void)backtrace(warm, 1);   // loads what backtrace() needs now, not inside the handler
#endif
    struct sigaction sa = {};
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) sigaction(sig, &sa, nullptr);
#endif
}

static qtc_str S(const QByteArray &b) { return qtc_sn(b.constData(), b.size()); }
static QByteArray V(qtc_view v) { return QByteArray(v.data, qsizetype(v.len)); }

struct Param { int kind; QByteArray type; QByteArray enum_name; };
struct Item {
    QByteArray cls;
    bool is_obj;
    bool is_ctor;
    bool is_static;
    QByteArray sig;
    int min;
    QList<Param> params;
    QByteArray ret = {};
};
struct ClassInfo {
    bool is_obj = false;
    QList<QByteArray> bases;
    QList<QList<Param>> ctors;
};
static QHash<QByteArray, ClassInfo> g_classes;
static QList<QByteArray> g_class_order;
static QHash<QByteArray, QList<QByteArray>> g_noarg;   // class -> its own methods callable without arguments
static qint64 g_slow_ms = 0;                           // --slow MS: report calls that take at least that long
static uint64_t g_seed = 0;                            // --seed S: 0 runs the fixed patterns only
static uint64_t g_rng = 0;
static uint64_t rnd() {   // xorshift64*
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1DULL;
}
static int rnd_below(int n) { return n > 0 ? int(rnd() % uint64_t(n)) : 0; }
constexpr int kRandomRounds = 6;   // per method, with --seed

static QList<Param> params_of(const QJsonArray &a) {
    QList<Param> ps;
    for (const QJsonValue &p : a) {
        QJsonObject o = p.toObject();
        ps.append({o.value(QStringLiteral("kind")).toInt(), o.value(QStringLiteral("type")).toString().toUtf8(),
                   o.value(QStringLiteral("enum")).toString().toUtf8()});
    }
    return ps;
}

static QList<Item> build_items(const QRegularExpression &only) {
    QList<Item> items;
    int64_t n = qtc_class_count();
    for (int64_t i = 0; i < n; i++) {
        QByteArray cls = V(qtc_class_at(i));
        QJsonObject d = QJsonDocument::fromJson(V(qtc_describe(0, S(cls)))).object();
        bool is_obj = d.value(QStringLiteral("kind")).toString() == QStringLiteral("object");
        ClassInfo &ci = g_classes[cls];
        g_class_order.append(cls);
        ci.is_obj = is_obj;
        for (const QJsonValue &b : d.value(QStringLiteral("bases")).toArray()) ci.bases.append(b.toString().toUtf8());
        const bool wanted = only.match(QString::fromLatin1(cls)).hasMatch();
        for (const QJsonValue &cv : d.value(QStringLiteral("constructors")).toArray()) {
            QJsonObject c = cv.toObject();
            QList<Param> ps = params_of(c.value(QStringLiteral("params")).toArray());
            ci.ctors.append(ps);
            int mn = 0;
            for (const QJsonValue &p : c.value(QStringLiteral("params")).toArray())
                if (!p.toObject().value(QStringLiteral("optional")).toBool()) mn++;
            if (wanted && !c.value(QStringLiteral("nofuzz")).toBool())
                items.append({cls, is_obj, true, false, c.value(QStringLiteral("sig")).toString().toUtf8(), mn, ps});
        }
        for (const QJsonValue &mv : d.value(QStringLiteral("methods")).toArray()) {
            QJsonObject m = mv.toObject();
            if (m.value(QStringLiteral("class")).toString().toUtf8() != cls) continue;
            if (m.value(QStringLiteral("blocking")).toBool() || m.value(QStringLiteral("nofuzz")).toBool()) continue;
            const QByteArray sig = m.value(QStringLiteral("sig")).toString().toUtf8();
            const bool is_static = m.value(QStringLiteral("static")).toBool();
            const int mn = m.value(QStringLiteral("min")).toInt();
            if (!is_static && mn == 0) {
                QByteArray bare = sig.left(sig.indexOf('(')) + "()";
                QList<QByteArray> &na = g_noarg[cls];
                if (!na.contains(bare)) na.append(bare);
            }
            if (!wanted) continue;
            items.append({cls, is_obj, false, is_static, sig, mn, params_of(m.value(QStringLiteral("params")).toArray()),
                          m.value(QStringLiteral("returns")).toString().toUtf8()});
        }
    }
    return items;
}

// ------------------------------------------------------------- instances

static QHash<QByteArray, uint64_t> g_inst;     // one live instance per class
static QHash<QByteArray, bool> g_no_inst;
static QHash<QByteArray, QByteArray> g_receiver_class;
static qtc_obj g_parent = 0;
static QList<uint64_t> g_temps;

static QByteArray class_of_param(const Param &p) {
    QByteArray cls = p.type;
    cls.replace("const ", "");
    cls.replace("*", "");
    cls.replace("&", "");
    return cls.trimmed();
}

static uint64_t instance(const QByteArray &cls, bool is_obj, int depth = 0);
static uint64_t receiver_for(const QByteArray &cls, bool is_obj, int depth = 0);

// An instance for an object or value parameter: of the class, or of a subclass when the class
// cannot be made itself (an abstract QIODevice gets a QBuffer).
static uint64_t param_instance(const QByteArray &cls, bool is_obj, int depth) {
    if (!g_classes.contains(cls)) return 0;
    return receiver_for(cls, is_obj, depth);
}

// Arguments for a constructor: plain values, objects and values made first (making them
// runs constructors, which would consume a half-pushed argument list).
static bool push_ctor_defaults(const QList<Param> &ps, int depth) {
    QList<uint64_t> made;
    for (const Param &p : ps) {
        uint64_t h = 0;
        if (p.kind == 10) {
            h = param_instance(class_of_param(p), true, depth + 1);
            if (!h) return false;
        } else if (p.kind == 12) {
            h = param_instance(p.type, false, depth + 1);
            if (!h) return false;
        }
        made.append(h);
    }
    for (int i = 0; i < ps.size(); i++) {
        const Param &p = ps[i];
        switch (p.kind) {
        case 1: qtc_arg_bool(false); break;
        case 2: case 3: qtc_arg_int(1); break;
        case 4: qtc_arg_double(1.0); break;
        case 5: case 6: {
            QJsonObject e = QJsonDocument::fromJson(V(qtc_enum_describe(S(p.enum_name)))).object();
            QJsonArray vals = e.value(QStringLiteral("values")).toArray();
            qtc_arg_int(vals.isEmpty() ? 0 : vals.at(0).toArray().at(1).toInteger());
            break;
        }
        case 7: case 9: qtc_arg_str(qtc_s("a")); break;
        case 8: qtc_arg_bytes(qtc_s("a")); break;
        case 10: qtc_arg_obj(made[i]); break;
        case 12: qtc_arg_val(made[i]); break;
        default: qtc_arg_null(); break;
        }
    }
    return true;
}

// Classes without public constructors that Qt hands out from elsewhere.
static uint64_t call_ret(uint64_t recv, const char *method) {
    if (qtc_call(recv, qtc_s(method)) != QTC_OK) return 0;
    if (qtc_ret_kind() == QTC_T_OBJ) return qtc_ret_obj();
    if (qtc_ret_kind() == QTC_T_VALUE) return qtc_ret_val();
    return 0;
}
static uint64_t static_ret(const char *cls, const char *method) {
    if (qtc_call_static(qtc_s(cls), qtc_s(method)) != QTC_OK) return 0;
    if (qtc_ret_kind() == QTC_T_OBJ) return qtc_ret_obj();
    if (qtc_ret_kind() == QTC_T_VALUE) return qtc_ret_val();
    return 0;
}
static uint64_t first_of_list(uint64_t list_val) {
    if (!list_val) return 0;
    uint64_t out = 0;
    if (qtc_list_len(list_val) > 0) {
        qtc_val e = qtc_list_get(list_val, 0);
        out = qtc_val_get_obj(e);
        qtc_val_free(e);
    }
    qtc_val_free(list_val);
    return out;
}
static uint64_t factory(const QByteArray &cls, int depth) {
    if (cls == "QCoreApplication" || cls == "QGuiApplication" || cls == "QApplication") return qtc_app();
    if (cls == "QStyleHints") return static_ret("QGuiApplication", "styleHints");
    if (cls == "QScreen") return static_ret("QGuiApplication", "primaryScreen");
    if (cls == "QInputMethod") return static_ret("QGuiApplication", "inputMethod");
    if (cls == "QClipboard") return static_ret("QGuiApplication", "clipboard");
    if (cls == "QColormap") return static_ret("QColormap", "instance");
    if (cls == "QSplitterHandle") {   // a splitter makes its handles: one between two widgets
        uint64_t sp = qtc_new(S("QSplitter"));
        if (!sp) return 0;
        for (int i = 0; i < 2; i++) {
            qtc_arg_obj(sp);
            qtc_new(S("QWidget"));
        }
        qtc_arg_int(1);
        return call_ret(sp, "handle");
    }
    if (cls == "QScroller") {
        uint64_t w = instance("QWidget", true, depth + 1);
        if (!w) return 0;
        qtc_arg_obj(w);
        return static_ret("QScroller", "scroller(QObject*)");
    }
    if (cls == "QGeoAreaMonitorSource") {
        qtc_arg_obj(g_parent);
        return static_ret("QGeoAreaMonitorSource", "createDefaultSource(QObject*)");
    }
    if (cls == "QOpenGLContextGroup") {
        uint64_t c = instance("QOpenGLContext", true, depth + 1);
        return c ? call_ret(c, "shareGroup") : 0;
    }
    if (cls == "QCollatorSortKey") {
        uint64_t c = instance("QCollator", false, depth + 1);
        if (!c) return 0;
        qtc_arg_str(qtc_s("key"));
        return call_ret(c, "sortKey");
    }
    if (cls == "QTextFrame" || cls == "QTextObject" || cls == "QTextTable" || cls == "QTextList") {
        uint64_t doc = qtc_new(qtc_s("QTextDocument"));
        if (!doc) return 0;
        if (cls == "QTextFrame" || cls == "QTextObject") return call_ret(doc, "rootFrame");
        qtc_arg_obj(doc);
        qtc_val cur = qtc_val_new(qtc_s("QTextCursor"));
        if (!cur) return 0;
        uint64_t out;
        if (cls == "QTextTable") {
            qtc_arg_int(3);
            qtc_arg_int(3);
            out = call_ret(cur, "insertTable(int,int)");
        } else {
            qtc_arg_int(-1);   // QTextListFormat::ListDisc
            out = call_ret(cur, "insertList(QTextListFormat::Style)");
        }
        qtc_val_free(cur);
        return out;
    }
    if (cls == "QLegend" || cls.endsWith("LegendMarker")) {
        uint64_t chart = qtc_new(qtc_s("QChart"));
        if (!chart) return 0;
        uint64_t legend = call_ret(chart, "legend");
        if (cls == "QLegend") return legend;
        const char *series = cls == "QAreaLegendMarker" ? "QAreaSeries" : cls == "QBarLegendMarker" ? "QBarSeries"
                           : cls == "QBoxPlotLegendMarker" ? "QBoxPlotSeries" : cls == "QCandlestickLegendMarker" ? "QCandlestickSeries"
                           : cls == "QPieLegendMarker" ? "QPieSeries" : "QLineSeries";
        uint64_t s = qtc_new(qtc_s(series));
        if (!s) return 0;
        if (cls == "QPieLegendMarker") { qtc_arg_str(qtc_s("slice")); qtc_arg_double(1); qtc_call(s, qtc_s("append(QString,qreal)")); }
        if (cls == "QBarLegendMarker") {
            qtc_arg_str(qtc_s("set"));
            uint64_t set = qtc_new(qtc_s("QBarSet"));
            qtc_arg_obj(set);
            qtc_call(s, qtc_s("append(QBarSet*)"));
        }
        qtc_arg_obj(s);
        qtc_call(chart, qtc_s("addSeries"));
        qtc_arg_obj(s);
        return first_of_list(call_ret(legend, "markers(QAbstractSeries*)"));
    }
    return 0;
}

static uint64_t make(const QByteArray &cls, bool is_obj, int depth) {
    uint64_t h = is_obj ? qtc_new(S(cls)) : qtc_val_new(S(cls));
    if (!h && is_obj && g_parent) {
        qtc_arg_obj(g_parent);
        h = qtc_new(S(cls));
    }
    if (!h) {
        for (const QList<Param> &ps : g_classes.value(cls).ctors) {
            if (!push_ctor_defaults(ps, depth)) {
                qtc_args_clear();
                continue;
            }
            h = is_obj ? qtc_new(S(cls)) : qtc_val_new(S(cls));
            if (h) break;
        }
    }
    if (!h) h = factory(cls, depth);
    return h;
}

static uint64_t instance(const QByteArray &cls, bool is_obj, int depth) {
    auto it = g_inst.constFind(cls);
    if (it != g_inst.constEnd() && qtc_alive(*it)) return *it;
    if (g_no_inst.value(cls) || depth > 3) return 0;
    uint64_t h = is_obj ? qtc_new(S(cls)) : qtc_val_new(S(cls));
    if (!h && is_obj && g_parent) {
        qtc_arg_obj(g_parent);
        h = qtc_new(S(cls));
    }
    if (!h) {
        // try each constructor with plain arguments
        for (const QList<Param> &ps : g_classes.value(cls).ctors) {
            if (!push_ctor_defaults(ps, depth)) {
                qtc_args_clear();
                continue;
            }
            h = is_obj ? qtc_new(S(cls)) : qtc_val_new(S(cls));
            if (h) break;
        }
    }
    if (!h) h = factory(cls, depth);
    if (!h) {
        g_no_inst.insert(cls, true);
        return 0;
    }
    g_inst.insert(cls, h);
    return h;
}

// The receiver for methods declared in cls: an instance of cls, else of the nearest subclass.
static uint64_t receiver_for(const QByteArray &cls, bool is_obj, int depth) {
    if (uint64_t h = instance(cls, is_obj, depth)) return h;
    QByteArray sub = g_receiver_class.value(cls);
    if (sub.isEmpty()) {
        int best = 1 << 30;
        for (const QByteArray &c : g_class_order) {
            const ClassInfo &ci = g_classes[c];
            int d = int(ci.bases.indexOf(cls));
            if (d < 0 || d >= best) continue;
            if (!instance(c, ci.is_obj, depth)) continue;
            best = d;
            sub = c;
            if (d == 0) break;
        }
        g_receiver_class.insert(cls, sub.isEmpty() ? QByteArray("-") : sub);
    }
    if (sub.isEmpty() || sub == "-") return 0;
    return instance(sub, g_classes[sub].is_obj, depth);
}

static QHash<QByteArray, QList<int64_t>> g_enum_cache;
static QList<int64_t> enum_values(const QByteArray &name, bool *flags) {
    static QHash<QByteArray, bool> fl;
    if (!g_enum_cache.contains(name)) {
        QJsonObject e = QJsonDocument::fromJson(V(qtc_enum_describe(S(name)))).object();
        QList<int64_t> vals;
        for (const QJsonValue &kv : e.value(QStringLiteral("values")).toArray()) vals.append(kv.toArray().at(1).toInteger());
        g_enum_cache.insert(name, vals);
        fl.insert(name, e.value(QStringLiteral("flags")).toBool());
    }
    *flags = fl.value(name);
    return g_enum_cache.value(name);
}

// Pushes one boundary argument and describes it for the trace.
// v picks one of the fixed patterns; wide (random rounds) picks from a table of wider values instead.
static QByteArray push_arg(const Param &p, int v, uint64_t self, bool wide = false) {
    static const QByteArray longstr(4096, 'a');
    const int w = v;
    switch (p.kind) {
    case 1: qtc_arg_bool(v % 2); return v % 2 ? "true" : "false";
    case 2: {
        const int64_t vals[] = {0, 1, -1, 2147483647, 3, 100};
        const int64_t wvals[] = {-2147483647 - 1, -2, 65535, 1 << 20, 4096, 255};
        const int64_t x = wide ? wvals[w % 6] : vals[v % 6];
        qtc_arg_int(x);
        return QByteArray::number(x);
    }
    case 3: {
        const int64_t vals[] = {0, 1, 255, 4294967295LL, 7, 65536};
        const int64_t wvals[] = {2, 1 << 20, 4096, 2147483648LL, 16, 4294967294LL};
        const int64_t x = wide ? wvals[w % 6] : vals[v % 6];
        qtc_arg_int(x);
        return QByteArray::number(x);
    }
    case 4: {
        const double vals[] = {0.0, 1.0, -1.0, 1e9, 0.5, NAN};
        const double wvals[] = {-1e9, INFINITY, -INFINITY, 1e-300, 1e300, 2.5};
        const double x = wide ? wvals[w % 6] : vals[v % 6];
        qtc_arg_double(x);
        return QByteArray::number(x);
    }
    case 5: case 6: {
        bool fl = false;
        QList<int64_t> vals = enum_values(p.enum_name, &fl);
        if (vals.isEmpty()) { qtc_arg_int(0); return "enum 0"; }
        int64_t x;
        if (wide) {
            // any declared value, an undeclared one just past the largest, or -1
            int64_t mx = vals[0];
            for (int64_t e : vals) mx = std::max(mx, e);
            x = w % 3 == 0 ? mx + 1 : (w % 3 == 1 ? -1 : vals[rnd_below(int(vals.size()))]);
        } else if (fl && v == 4) {
            x = 0;
            for (int64_t e : vals) x |= e;
        } else {
            x = vals[(v * 7) % vals.size()];
        }
        qtc_arg_int(x);
        return "enum " + QByteArray::number(x);
    }
    case 7: case 8: {
        // no absolute path: whatever a method does with a path stays in the driver's sandbox
        const char *vals[] = {"", "x", "qtc \xf0\x9f\x98\x80 fuzz", nullptr, "%s%n%x", "a/../b/"};
        const char *wvals[] = {"\n\t\r", "\x01\x7f\x1b[0m", "a,b;c=d&e", "../../x/../..", "<b>x</b>%1%2", "\xff\xfe invalid"};
        if (wide) {
            if (p.kind == 8) qtc_arg_bytes(qtc_s(wvals[w % 6]));
            else qtc_arg_str(qtc_s(wvals[w % 6]));
            return "wide string " + QByteArray::number(w % 6);
        }
        if (v % 6 == 3) { qtc_arg_str(S(longstr)); return "\"a\"x4096"; }
        if (p.kind == 8) qtc_arg_bytes(qtc_s(vals[v % 6]));
        else qtc_arg_str(qtc_s(vals[v % 6]));
        return "\"" + QByteArray(vals[v % 6]) + "\"";
    }
    case 9: qtc_arg_str(qtc_s(v % 2 ? "a" : "\xe2\x82\xac")); return v % 2 ? "'a'" : "'euro'";
    case 10: {
        QByteArray cls = class_of_param(p);
        if (v % 3 == 0) { qtc_arg_null(); return "null"; }
        uint64_t o = (v % 3 == 1) ? param_instance(cls, true, 0) : self;
        if (o && qtc_is_object(o)) {
            qtc_arg_obj(o);
            return (v % 3 == 1 ? "instance " : "self ") + cls;
        }
        qtc_arg_null();
        return "null";
    }
    case 11: {
        if (wide) {
            switch (w % 4) {
            case 0: qtc_arg_double(1.5); return "variant 1.5";
            case 1: qtc_arg_bool(true); return "variant true";
            case 2: { qtc_val m = qtc_val_from_json(qtc_s("{\"a\": [1, {\"b\": null}], \"c\": \"d\"}")); qtc_arg_val(m); g_temps.append(m); return "variant {...}"; }
            default: qtc_arg_int(-1); return "variant -1";
            }
        }
        switch (v % 4) {
        case 0: qtc_arg_int(0); return "variant 0";
        case 1: qtc_arg_str(qtc_s("x")); return "variant \"x\"";
        case 2: { qtc_val l = qtc_val_list(); qtc_arg_val(l); g_temps.append(l); return "variant []"; }
        default: qtc_arg_null(); return "variant null";
        }
    }
    case 12: {
        QByteArray t = p.type;
        uint64_t val = param_instance(t, false, 0);
        if (v % 3 == 2 || !val) { qtc_arg_str(qtc_s("1")); return "\"1\" for " + t; }
        qtc_arg_val(val);
        return "instance " + t;
    }
    case 13: {
        if (wide) {
            qtc_val l = qtc_val_from_json(qtc_s(w % 2 ? "[[1, 2], [], null, \"x\", 3.5]"
                                                      : "[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31]"));
            qtc_arg_val(l);
            g_temps.append(l);
            return w % 2 ? "[mixed]" : "[32 ints]";
        }
        qtc_val l = qtc_val_list();
        if (v % 2) {
            qtc_val a = qtc_val_int(1), b = qtc_val_str(qtc_s("two"));
            qtc_list_append(l, a);
            qtc_list_append(l, b);
            qtc_val_free(a);
            qtc_val_free(b);
        }
        qtc_arg_val(l);
        g_temps.append(l);
        return v % 2 ? "[1, \"two\"]" : "[]";
    }
    case 14: {
        qtc_val m = qtc_val_map();
        if (v % 2) {
            qtc_val a = qtc_val_int(1);
            qtc_map_set(m, qtc_s("a"), a);
            qtc_val_free(a);
        }
        qtc_arg_val(m);
        g_temps.append(m);
        return v % 2 ? "{a: 1}" : "{}";
    }
    case 15: {
        uint64_t idx = instance("QPersistentModelIndex", false);
        if (idx) qtc_arg_val(idx);
        else qtc_arg_null();
        return "index";
    }
    case 16: case 17: qtc_arg_null(); return "null";
    default: qtc_arg_null(); return "null";
    }
}

static void drain() {
    qtc_event e;
    while (qtc_poll(&e)) {}
    for (uint64_t t : g_temps) qtc_val_free(t);
    g_temps.clear();
}

static void trace(const Item &it, const QByteArray &sig, const QByteArray &args, const QByteArray &recv) {
    QByteArray line = "CALL " + it.cls + (it.is_ctor ? " new " : (it.is_static ? " static " : " ")) + sig;
    if (!recv.isEmpty() && recv != it.cls) line += " on " + recv;
    line += " (" + args + ")\n";
    (void)!::write(2, line.constData(), size_t(line.size()));
}

static QByteArray sig_prefix(const QByteArray &full, int k) {
    QByteArray inner = full.mid(full.indexOf('(') + 1);
    inner.chop(1);
    QList<QByteArray> parts;   // split at the commas outside <> and ()
    int depth = 0;
    QByteArray cur;
    for (char c : inner) {
        if (c == ',' && depth == 0) {
            parts.append(cur);
            cur.clear();
            continue;
        }
        if (c == '<' || c == '(') depth++;
        else if (c == '>' || c == ')') depth--;
        cur.append(c);
    }
    if (!cur.isEmpty()) parts.append(cur);
    QByteArray sig = full.left(full.indexOf('(')) + "(";
    for (int i = 0; i < k && i < parts.size(); i++) sig += (i ? "," : "") + parts[i];
    return sig + ")";
}

static int64_t g_calls = 0;
static FILE *g_skipped = nullptr;   // items without a receiver, for the coverage report
static bool g_by_name = false;      // --by-name: report the overloads a call by name cannot tell apart
static QSet<QByteArray> g_ambiguous;

// --by-name: call the method by name with arguments of its parameters' own kinds (pattern 1), as
// a program would, and report it when qtc cannot pick the overload. Calls that fail otherwise are
// fine here: the method mode tests them.
static void run_item_by_name(const Item &it) {
    if (it.params.size() > 12) return;
    const int name_end = it.sig.indexOf('(');
    const QByteArray name = it.sig.left(name_end);
    uint64_t self = 0;
    if (!it.is_ctor && !it.is_static) {
        self = receiver_for(it.cls, it.is_obj);
        if (!self) return;
    }
    for (int k = it.min; k <= it.params.size(); k++) {
        bool simple = true;
        for (int i = 0; i < k; i++) {
            const Param &p = it.params[i];
            if (p.kind == 10) (void)param_instance(class_of_param(p), true, 0);
            else if (p.kind == 12) (void)param_instance(p.type, false, 0);
            else if (p.kind == 15) (void)instance("QPersistentModelIndex", false);
            else if (p.kind == 0) simple = false;
        }
        if (!simple) continue;
        if (!it.is_ctor && !it.is_static && !qtc_alive(self)) return;
        for (int i = 0; i < k; i++) push_arg(it.params[i], 1, self);
        g_calls++;
        int rc;
        if (it.is_ctor) {
            uint64_t h = it.is_obj ? qtc_new(S(it.cls)) : qtc_val_new(S(it.cls));
            rc = qtc_last_error();
            if (h) {
                if (it.is_obj) qtc_destroy(h);
                else qtc_val_free(h);
                rc = 0;
            }
        } else if (it.is_static) {
            rc = qtc_call_static(S(it.cls), S(name));
        } else {
            rc = qtc_call(self, S(name));
        }
        if (rc == QTC_ERR_AMBIGUOUS) {
            const QByteArray key = it.cls + "::" + (it.is_ctor ? QByteArray("<ctor>") : name) + " with " +
                                   QByteArray::number(k) + " argument(s): " + qtc_last_error_message().data;
            if (!g_ambiguous.contains(key)) {
                g_ambiguous.insert(key);
                std::printf("AMBIGUOUS %s\n", key.constData());
                std::fflush(stdout);
            }
        }
        drain();
    }
}

static void run_item(const Item &it) {
    int maxa = int(it.params.size());
    uint64_t self = 0;
    QByteArray recv;
    if (!it.is_ctor && !it.is_static) {
        self = receiver_for(it.cls, it.is_obj);
        if (!self) {
            if (g_skipped) {
                std::fprintf(g_skipped, "%s\t%s\n", it.cls.constData(), it.sig.constData());
                std::fflush(g_skipped);
            }
            return;
        }
        recv = V(qtc_class_name(self));
    }
    for (int k = it.min; k <= maxa; k++) {
        QByteArray sig = sig_prefix(it.sig, k);
        for (int v = 0; v < 6; v++) {
            if (!it.is_ctor && !it.is_static && !qtc_alive(self)) {
                self = receiver_for(it.cls, it.is_obj);
                if (!self) return;
                recv = V(qtc_class_name(self));
            }
            // make the object and value arguments first: making one runs a constructor, which
            // would consume the arguments already pushed
            for (int i = 0; i < k; i++) {
                const Param &p = it.params[i];
                if (p.kind == 10 && (v + i) % 3 == 1) (void)param_instance(class_of_param(p), true, 0);
                else if (p.kind == 12) (void)param_instance(p.type, false, 0);
                else if (p.kind == 15) (void)instance("QPersistentModelIndex", false);
            }
            if (!it.is_ctor && !it.is_static && !qtc_alive(self)) {
                self = receiver_for(it.cls, it.is_obj);
                if (!self) return;
                recv = V(qtc_class_name(self));
            }
            QByteArray args;
            for (int i = 0; i < k; i++) args += (i ? ", " : "") + push_arg(it.params[i], v + i, self);
            trace(it, sig, args, recv);
            g_calls++;
            QElapsedTimer slow;
            slow.start();
            struct Slow {
                QElapsedTimer &t;
                const Item &it;
                const QByteArray &sig, &args;
                ~Slow() {
                    if (g_slow_ms > 0 && t.elapsed() >= g_slow_ms) {
                        QByteArray l = "SLOW " + QByteArray::number(t.elapsed()) + " ms " + it.cls + " " + sig + " (" + args + ")\n";
                        (void)!::write(2, l.constData(), size_t(l.size()));
                    }
                }
            } slow_guard{slow, it, sig, args};
            if (it.is_ctor) {
                uint64_t h = it.is_obj ? qtc_new(S(it.cls)) : qtc_val_new(S(it.cls));
                if (h) {
                    if (it.is_obj) qtc_destroy(h);
                    else qtc_val_free(h);
                }
            } else if (it.is_static) {
                qtc_call_static(S(it.cls), S(sig));
            } else {
                qtc_call(self, S(sig));
                if (qtc_ret_kind() == QTC_T_VALUE || qtc_ret_kind() == QTC_T_LIST || qtc_ret_kind() == QTC_T_MAP) {
                    qtc_val r = qtc_ret_val();
                    if (r) {
                        qtc_val_to_string(r);
                        qtc_val_free(r);
                    }
                }
            }
            drain();
        }
    }
    // random rounds: an arity, and for each argument any of the patterns, the wider ones included
    for (int round = 0; g_seed && round < kRandomRounds; round++) {
        const int k = it.min + rnd_below(maxa - it.min + 1);
        int vs[32];
        bool wide[32];
        for (int i = 0; i < k && i < 32; i++) {
            vs[i] = rnd_below(6);
            wide[i] = rnd_below(2) == 1;
        }
        if (!it.is_ctor && !it.is_static && !qtc_alive(self)) {
            self = receiver_for(it.cls, it.is_obj);
            if (!self) return;
            recv = V(qtc_class_name(self));
        }
        for (int i = 0; i < k && i < 32; i++) {
            const Param &p = it.params[i];
            if (p.kind == 10 && vs[i] % 3 == 1) (void)param_instance(class_of_param(p), true, 0);
            else if (p.kind == 12) (void)param_instance(p.type, false, 0);
            else if (p.kind == 15) (void)instance("QPersistentModelIndex", false);
        }
        if (!it.is_ctor && !it.is_static && !qtc_alive(self)) {
            self = receiver_for(it.cls, it.is_obj);
            if (!self) return;
            recv = V(qtc_class_name(self));
        }
        const QByteArray sig = sig_prefix(it.sig, k);
        QByteArray args;
        for (int i = 0; i < k && i < 32; i++) args += (i ? ", " : "") + push_arg(it.params[i], vs[i], self, wide[i]);
        trace(it, sig, args, recv);
        g_calls++;
        if (it.is_ctor) {
            uint64_t h = it.is_obj ? qtc_new(S(it.cls)) : qtc_val_new(S(it.cls));
            if (h) {
                if (it.is_obj) qtc_destroy(h);
                else qtc_val_free(h);
            }
        } else if (it.is_static) {
            qtc_call_static(S(it.cls), S(sig));
        } else {
            qtc_call(self, S(sig));
            if (qtc_ret_kind() == QTC_T_VALUE || qtc_ret_kind() == QTC_T_LIST || qtc_ret_kind() == QTC_T_MAP) {
                qtc_val r = qtc_ret_val();
                if (r) {
                    qtc_val_to_string(r);
                    qtc_val_free(r);
                }
            }
        }
        drain();
    }
}


// ------------------------------------------------------------- lifetime mode
//
// Qt objects that keep a plain pointer to an object they were given crash when that object
// dies first, and parts that Qt hands out crash their owner when deleted. In this mode:
//   arg items   a method or constructor that takes objects runs on a fresh receiver with
//               fresh objects; the objects are destroyed and the receiver is used; then
//               again with the receiver destroyed first and the objects used
//   ret items   a method that returns objects runs on a fresh receiver; what it returns is
//               destroyed and the receiver (and the application) is used
// "Used" means every method callable without arguments, of the class and its bases, then
// show and repaint for widgets, then destruction.

struct LItem { int item; bool ret; };

static bool returns_objects(const QByteArray &ret) {
    static const QRegularExpression re(QStringLiteral(R"(^(const )?Q\w+ ?\*$|^QList<Q\w+ ?\*>$|^QObjectList$|^QWidgetList$|^QWindowList$)"));
    return re.match(QString::fromLatin1(ret)).hasMatch() && !ret.startsWith("QString") && ret != "QChar*";
}

static QList<LItem> lifetime_items(const QList<Item> &items) {
    QList<LItem> out;
    for (int i = 0; i < items.size(); i++) {
        const Item &it = items[i];
        bool has_obj = false;
        for (const Param &p : it.params) has_obj = has_obj || p.kind == 10;
        if (has_obj) out.append({i, false});
        if (!it.is_ctor && it.min == 0 && returns_objects(it.ret)) out.append({i, true});
    }
    return out;
}

static void tline(const QByteArray &s) {
    QByteArray line = "CALL " + s + "\n";
    (void)!::write(2, line.constData(), size_t(line.size()));
}

static QByteArray class_of(uint64_t h, const QByteArray &fallback) {
    QByteArray c = qtc_is_object(h) ? V(qtc_class_name(h)) : V(qtc_val_type_name(h));
    return g_classes.contains(c) ? c : fallback;
}

// A new object or value of cls, or of the nearest class inheriting it that can be made.
static uint64_t fresh(const QByteArray &cls) {
    if (!g_classes.contains(cls)) return 0;
    const bool is_obj = g_classes[cls].is_obj;
    if (!g_no_inst.value(cls))
        if (uint64_t h = make(cls, is_obj, 0)) return h;
    (void)receiver_for(cls, is_obj, 0);
    const QByteArray sub = g_receiver_class.value(cls);
    if (sub.isEmpty() || sub == "-" || sub == cls) return 0;
    return make(sub, g_classes[sub].is_obj, 0);
}

static void kill(uint64_t h) {
    if (!qtc_alive(h)) return;
    if (qtc_is_object(h)) {
        tline("destroy " + V(qtc_class_name(h)));
        qtc_destroy(h);
    } else {
        qtc_val_free(h);
    }
}

static void settle() {
    tline("(queued events)");   // a failure from here on happened while Qt ran queued events and timers
    qtc_process_events();
    drain();
    qtc_cancel_quit();
}

static void use(uint64_t h, const QByteArray &static_cls) {
    if (!qtc_alive(h)) return;
    const QByteArray cls = class_of(h, static_cls);
    QList<QByteArray> chain{cls};
    chain += g_classes.value(cls).bases;
    for (const QByteArray &c : chain) {
        for (const QByteArray &sig : g_noarg.value(c)) {
            if (!qtc_alive(h)) return;
            tline("use " + c + " " + sig);
            g_calls++;
            if (qtc_call(h, S(sig)) == QTC_OK &&
                (qtc_ret_kind() == QTC_T_VALUE || qtc_ret_kind() == QTC_T_LIST || qtc_ret_kind() == QTC_T_MAP)) {
                qtc_val r = qtc_ret_val();
                if (r) {
                    qtc_val_to_string(r);
                    qtc_val_free(r);
                }
            }
            drain();
        }
    }
    if (qtc_is_object(h) && qtc_inherits(h, qtc_s("QWidget"))) {
        tline("use show + repaint");
        qtc_call(h, qtc_s("show()"));
        settle();
        if (qtc_alive(h)) qtc_call(h, qtc_s("repaint()"));
    }
    settle();
}

// The application after something it may own was destroyed.
static void use_globals() {
    tline("use globals");
    auto touch = [](const char *cls, const char *getter, const char *method) {
        if (qtc_call_static(qtc_s(cls), qtc_s(getter)) != QTC_OK || qtc_ret_kind() != QTC_T_OBJ) return;
        uint64_t o = qtc_ret_obj();
        if (o) qtc_call(o, qtc_s(method));
        if (qtc_ret_kind() == QTC_T_VALUE) qtc_val_free(qtc_ret_val());
    };
    touch("QGuiApplication", "primaryScreen", "geometry");
    touch("QGuiApplication", "styleHints", "mouseDoubleClickInterval");
    touch("QGuiApplication", "inputMethod", "isVisible");
    touch("QGuiApplication", "clipboard", "text");
    touch("QApplication", "style", "name");
    touch("QThread", "currentThread", "isRunning");
    touch("QCoreApplication", "instance", "applicationName");
    uint64_t w = qtc_new(qtc_s("QLineEdit"));
    if (w) {
        qtc_arg_str(qtc_s("text"));
        qtc_call(w, qtc_s("setText"));
        qtc_call(w, qtc_s("show"));
        settle();
        qtc_call(w, qtc_s("repaint"));
        qtc_destroy(w);
    }
    settle();
}

// Arguments: fresh objects for object parameters, plain values for the rest.
static bool push_fresh(const QList<Param> &ps, QList<QPair<uint64_t, QByteArray>> *objs) {
    QList<uint64_t> made;
    for (const Param &p : ps) {
        uint64_t h = 0;
        if (p.kind == 10) {
            h = fresh(class_of_param(p));
            if (h) objs->append({h, class_of_param(p)});
        } else if (p.kind == 12) {
            h = param_instance(p.type, false, 0);
        } else if (p.kind == 15) {
            h = instance("QPersistentModelIndex", false);
        }
        made.append(h);
    }
    for (int i = 0; i < ps.size(); i++) {
        const Param &p = ps[i];
        switch (p.kind) {
        case 1: qtc_arg_bool(true); break;
        case 2: case 3: qtc_arg_int(0); break;
        case 4: qtc_arg_double(1.0); break;
        case 5: case 6: {
            QJsonObject e = QJsonDocument::fromJson(V(qtc_enum_describe(S(p.enum_name)))).object();
            QJsonArray vals = e.value(QStringLiteral("values")).toArray();
            qtc_arg_int(vals.isEmpty() ? 0 : vals.at(0).toArray().at(1).toInteger());
            break;
        }
        case 7: case 9: qtc_arg_str(qtc_s("a")); break;
        case 8: qtc_arg_bytes(qtc_s("a")); break;
        case 10: case 12: case 15:
            if (made[i]) {
                if (p.kind == 10) qtc_arg_obj(made[i]);
                else qtc_arg_val(made[i]);
            } else {
                qtc_arg_null();
            }
            break;
        case 11: qtc_arg_int(0); break;
        case 13: { qtc_val l = qtc_val_list(); qtc_arg_val(l); g_temps.append(l); break; }
        case 14: { qtc_val m = qtc_val_map(); qtc_arg_val(m); g_temps.append(m); break; }
        default: qtc_arg_null(); break;
        }
    }
    return true;
}

// Runs the item once; returns the receiver (0 for statics or on failure).
static uint64_t invoke(const Item &it, QList<QPair<uint64_t, QByteArray>> *objs, bool *ok) {
    *ok = false;
    uint64_t self = 0;
    if (!it.is_ctor && !it.is_static) {
        self = fresh(it.cls);
        if (!self) {
            if (g_skipped) {
                std::fprintf(g_skipped, "%s\t%s\n", it.cls.constData(), it.sig.constData());
                std::fflush(g_skipped);
            }
            return 0;
        }
    }
    QList<QPair<uint64_t, QByteArray>> made;
    push_fresh(it.params, &made);
    if (!it.is_ctor && !it.is_static && !qtc_alive(self)) {
        qtc_args_clear();
        for (auto &m : made) kill(m.first);
        return 0;
    }
    tline((it.is_ctor ? "new " : (it.is_static ? "static " : "")) + it.cls + " " + it.sig);
    g_calls++;
    if (it.is_ctor) {
        self = it.is_obj ? qtc_new(S(it.cls)) : qtc_val_new(S(it.cls));
        *ok = self != 0;
    } else if (it.is_static) {
        *ok = qtc_call_static(S(it.cls), S(it.sig)) == QTC_OK;
    } else {
        *ok = qtc_call(self, S(it.sig)) == QTC_OK;
    }
    if (qtc_ret_kind() == QTC_T_VALUE || qtc_ret_kind() == QTC_T_LIST || qtc_ret_kind() == QTC_T_MAP)
        if (qtc_val r = qtc_ret_val()) g_temps.append(r);
    drain();
    *objs = made;
    return self;
}

static void run_arg_item(const Item &it) {
    // A: the objects die first, the receiver is used
    {
        QList<QPair<uint64_t, QByteArray>> objs;
        bool ok = false;
        uint64_t self = invoke(it, &objs, &ok);
        if (ok) {
            for (auto &o : objs) kill(o.first);
            settle();
            if (it.is_static) use_globals();
            else use(self, it.cls);
        }
        kill(self);
        for (auto &o : objs) kill(o.first);
        settle();
    }
    if (it.is_static) return;
    // B: the receiver dies first, the objects are used
    {
        QList<QPair<uint64_t, QByteArray>> objs;
        bool ok = false;
        uint64_t self = invoke(it, &objs, &ok);
        kill(self);
        settle();
        if (ok)
            for (auto &o : objs) use(o.first, o.second);
        for (auto &o : objs) kill(o.first);
        settle();
    }
}

static void run_ret_item(const Item &it) {
    uint64_t self = 0;
    if (!it.is_static) {
        self = fresh(it.cls);
        if (!self) return;
    }
    tline((it.is_static ? "static " : "") + it.cls + " " + it.sig + " -> destroy the result");
    g_calls++;
    const QByteArray bare = it.sig.left(it.sig.indexOf('(')) + "()";
    qtc_status rc = it.is_static ? qtc_call_static(S(it.cls), S(bare)) : qtc_call(self, S(bare));
    QList<uint64_t> victims;
    if (rc == QTC_OK && qtc_ret_kind() == QTC_T_OBJ) {
        if (uint64_t o = qtc_ret_obj()) victims.append(o);
    } else if (rc == QTC_OK && qtc_ret_kind() == QTC_T_LIST) {
        qtc_val l = qtc_ret_val();
        for (int64_t i = 0, n = qtc_list_len(l); i < n && i < 64; i++) {
            qtc_val e = qtc_list_get(l, i);
            uint64_t o = qtc_val_get_obj(e);
            if (o) victims.append(o);
            qtc_val_free(e);
        }
        qtc_val_free(l);
    }
    drain();
    for (uint64_t v : victims) {
        if (v == self || v == g_parent) continue;
        kill(v);
        settle();
        if (self) use(self, it.cls);
        use_globals();
        if (self && !qtc_alive(self)) break;
    }
    kill(self);
    settle();
}

int main(int argc, char **argv) {
    install_crash_handler();
    bool list = false, lifetime = false;
    int start = 0, end = -1;
    const char *state = nullptr;
    const char *order_file = nullptr;
    QRegularExpression only(QStringLiteral(".*"));
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) list = true;
        else if (!strcmp(argv[i], "--lifetime")) lifetime = true;
        else if (!strcmp(argv[i], "--slow") && i + 1 < argc) g_slow_ms = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) g_seed = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--by-name")) g_by_name = true;
        else if (!strcmp(argv[i], "--start") && i + 1 < argc) start = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--end") && i + 1 < argc) end = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--state") && i + 1 < argc) state = argv[++i];
        else if (!strcmp(argv[i], "--order") && i + 1 < argc) order_file = argv[++i];
        else if (!strcmp(argv[i], "--skipped") && i + 1 < argc) g_skipped = std::fopen(argv[++i], "a");
        else if (!strcmp(argv[i], "--classes") && i + 1 < argc)
            only = QRegularExpression(QStringLiteral("^(?:%1)$").arg(QString::fromUtf8(argv[++i])));
    }
    if (qtc_init(QTC_APP_WIDGETS, qtc_sn("fuzz\0-platform\0offscreen", 25)) != QTC_OK) return 3;
    qtc_set_quit_on_last_window_closed(false);
    // keep the shared instances small: what grows them is tested, linear work on 100 MB is not interesting
    qtc_set_alloc_limit(16 << 20);
    QList<Item> items = build_items(only);
    QList<LItem> litems = lifetime ? lifetime_items(items) : QList<LItem>();
    if (g_seed) {
        // the same seed gives the same order in every process run, so the driver's indexes hold
        g_rng = g_seed * 0x9E3779B97F4A7C15ULL | 1;
        if (lifetime)
            for (int i = int(litems.size()) - 1; i > 0; i--) litems.swapItemsAt(i, rnd_below(i + 1));
        else
            for (int i = int(items.size()) - 1; i > 0; i--) items.swapItemsAt(i, rnd_below(i + 1));
    }
    const int count = lifetime ? int(litems.size()) : int(items.size());
    if (list) {
        for (int i = 0; i < count; i++) {
            const Item &it = items[lifetime ? litems[i].item : i];
            const char *kind = it.is_ctor ? "ctor" : (it.is_static ? "static" : "method");
            std::printf("%d\t%s\t%s\t%s%s\n", i, it.cls.constData(), it.sig.constData(), kind,
                        lifetime ? (litems[i].ret ? " ret" : " arg") : "");
        }
        qtc_shutdown();
        return 0;
    }
    g_parent = qtc_new(qtc_s("QWidget"));
    if (end < 0 || end > count) end = count;
    QList<int> order;   // the items to run, in order
    if (order_file) {
        QFile f(QString::fromLocal8Bit(order_file));
        if (!f.open(QIODevice::ReadOnly)) return 4;
        for (const QByteArray &l : f.readAll().split('\n'))
            if (!l.trimmed().isEmpty() && l.trimmed().toInt() >= 0 && l.trimmed().toInt() < count) order.append(l.trimmed().toInt());
    } else {
        for (int i = start; i < end; i++) order.append(i);
    }
    for (int pos = 0; pos < order.size(); pos++) {
        const int i = order[pos];
        if (state) {
            FILE *f = std::fopen(state, "w");
            if (f) {
                std::fprintf(f, "%d %lld\n", order_file ? pos : i, (long long)g_calls);
                std::fflush(f);
                std::fclose(f);
            }
        }
        if (g_seed) g_rng = (g_seed + uint64_t(i) * 0x9E3779B97F4A7C15ULL) | 1;   // each item's rounds, the same on a rerun
        if (g_by_name) run_item_by_name(items[i]);
        else if (!lifetime) run_item(items[i]);
        else if (litems[i].ret) run_ret_item(items[litems[i].item]);
        else run_arg_item(items[litems[i].item]);
        if (i % 25 == 0) {
            tline("(queued events)");
            qtc_process_events();
            drain();
            qtc_cancel_quit();
        }
        if (!qtc_alive(g_parent)) g_parent = qtc_new(qtc_s("QWidget"));
    }
    qtc_process_events();
    drain();
    qtc_shutdown();
    if (state) {
        FILE *f = std::fopen(state, "w");
        if (f) {
            std::fprintf(f, "done %lld\n", (long long)g_calls);
            std::fclose(f);
        }
    }
    return 0;
}
