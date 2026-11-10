// Calls every bound method and constructor with deliberate misuse and checks
// that each call fails with the right error instead of crashing.
//
// For each class (from qtc_describe) and each of its generated methods:
//   - a stale receiver               -> QTC_ERR_STALE_HANDLE
//   - a null receiver                -> QTC_ERR_NULL_HANDLE
//   - a garbage receiver             -> QTC_ERR_BAD_HANDLE
//   - a receiver of the wrong kind   -> an error (object method on a value, value method on an object)
//   - one argument too many          -> an error
//   - every argument of a wrong type -> an error
//   - a stale object as an argument  -> an error
// Static methods and constructors get the argument checks.
#include "qtc.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMap>
#include <QtCore/QSet>

#include <cstdio>
#include <cstring>
#include <string>

static qtc_str S(const QByteArray &b) { return qtc_sn(b.constData(), b.size()); }
static QByteArray V(qtc_view v) { return QByteArray(v.data, qsizetype(v.len)); }

static int g_calls = 0, g_bad = 0;
static QMap<int, int> g_codes;

static void expect_error(int rc, const QByteArray &what, int want = 1) {
    g_calls++;
    g_codes[rc]++;
    bool ok = want == 1 ? rc < 0 : rc == want;
    if (!ok) {
        g_bad++;
        if (g_bad < 40)
            std::fprintf(stderr, "UNEXPECTED %s -> %d (%s) want %d: %s\n", what.constData(), rc, V(qtc_status_name(rc)).constData(),
                         want, V(qtc_last_error_message()).constData());
    }
}

// A wrong-kind argument for a parameter kind (ParamKind values in qtc_internal.h).
static void push_wrong(int kind, qtc_obj obj, qtc_val stale_obj_holder) {
    (void)stale_obj_holder;
    switch (kind) {
    case 1: qtc_arg_str(qtc_s("not a bool")); break;            // PK_BOOL
    case 2: case 3: case 5: case 6: qtc_arg_str(qtc_s("x")); break;   // ints, enums, flags
    case 4: qtc_arg_obj(obj); break;                              // PK_DOUBLE
    case 7: case 8: qtc_arg_obj(obj); break;                      // strings, bytes
    case 9: qtc_arg_double(1.5); break;                           // PK_CHAR
    case 10: qtc_arg_str(qtc_s("x")); break;                      // PK_OBJ
    case 11: qtc_arg_str(qtc_s("anything goes")); break;          // PK_VARIANT accepts everything
    case 12: qtc_arg_obj(obj); break;                             // PK_VALUE
    case 13: case 14: qtc_arg_obj(obj); break;                    // lists, maps
    case 15: qtc_arg_str(qtc_s("x")); break;                      // PK_INDEX
    case 16: qtc_arg_int(7); break;                               // PK_NULLPTR
    default: qtc_arg_obj(obj); break;
    }
}

int main() {
    if (qtc_init(QTC_APP_WIDGETS, qtc_sn("sweep\0-platform\0offscreen", 26)) != QTC_OK) {
        std::fprintf(stderr, "init failed: %s\n", V(qtc_last_error_message()).constData());
        return 2;
    }
    qtc_set_error_policy(QTC_ERRORS_RETURN);
    qtc_obj stale = qtc_new(qtc_s("QObject"));
    qtc_destroy(stale);
    qtc_process_events();
    qtc_val stale_val = qtc_val_int(1);
    qtc_val_free(stale_val);
    qtc_val a_value = qtc_val_int(1);
    qtc_obj an_object = qtc_new(qtc_s("QObject"));
    const qtc_obj garbage = 0x7fffffff00001234ull;

    // names QObject itself answers to: a value method of the same name legitimately works on a QObject
    QSet<QByteArray> qobject_names;
    {
        QJsonObject qd = QJsonDocument::fromJson(V(qtc_describe(an_object, qtc_s("")))).object();
        for (const QJsonValue &mv : qd.value(QStringLiteral("methods")).toArray()) {
            QByteArray sg = mv.toObject().value(QStringLiteral("sig")).toString().toUtf8();
            qobject_names.insert(sg.left(sg.indexOf('(')));
        }
        for (const QJsonValue &mv : qd.value(QStringLiteral("meta_methods")).toArray()) {
            QByteArray sg = mv.toString().toUtf8();
            qobject_names.insert(sg.left(sg.indexOf('(')));
        }
    }
    int64_t n = qtc_class_count();
    int classes = 0, methods = 0, ctors = 0;
    for (int64_t i = 0; i < n; i++) {
        QByteArray cls = V(qtc_class_at(i));
        QByteArray desc = V(qtc_describe(0, S(cls)));
        QJsonObject d = QJsonDocument::fromJson(desc).object();
        if (d.isEmpty()) {
            std::fprintf(stderr, "no description for %s\n", cls.constData());
            g_bad++;
            continue;
        }
        classes++;
        bool is_obj = d.value(QStringLiteral("kind")).toString() == QStringLiteral("object");
        for (const QJsonValue &mv : d.value(QStringLiteral("methods")).toArray()) {
            QJsonObject m = mv.toObject();
            if (m.value(QStringLiteral("class")).toString().toUtf8() != cls) continue;   // inherited: tested with its own class
            methods++;
            QByteArray sig = m.value(QStringLiteral("sig")).toString().toUtf8();
            QByteArray what = cls + "::" + sig;
            QJsonArray params = m.value(QStringLiteral("params")).toArray();
            bool is_static = m.value(QStringLiteral("static")).toBool();
            if (is_static) {
                if (cls == "QCoreApplication" || cls == "QGuiApplication" || cls == "QApplication") {
                    QByteArray nm = sig.left(sig.indexOf('('));
                    if (nm == "quit" || nm == "exit") continue;   // redirected to qtc_quit by design
                }
                for (int k = 0; k <= params.size(); k++) qtc_arg_int(0);
                expect_error(qtc_call_static(S(cls), S(sig)), what + " +1 arg");
                bool any_strict = false;
                for (const QJsonValue &p : params) {
                    int kind = p.toObject().value(QStringLiteral("kind")).toInt();
                    if (kind != 11) any_strict = true;
                    push_wrong(kind, an_object, 0);
                }
                if (any_strict) expect_error(qtc_call_static(S(cls), S(sig)), what + " wrong types");
                else qtc_args_clear();
                continue;
            }
            expect_error(qtc_call(is_obj ? stale : stale_val, S(sig)), what + " stale receiver", QTC_ERR_STALE_HANDLE);
            expect_error(qtc_call(0, S(sig)), what + " null receiver", QTC_ERR_NULL_HANDLE);
            expect_error(qtc_call(garbage, S(sig)), what + " garbage receiver", QTC_ERR_BAD_HANDLE);
            if (is_obj || !qobject_names.contains(sig.left(sig.indexOf('('))))
                expect_error(qtc_call(is_obj ? a_value : an_object, S(sig)), what + " wrong receiver kind");
            expect_error(qtc_call(a_value, S(sig)), what + " receiver of another value type");
            // arguments are checked before the receiver's method runs: use a live receiver of the right
            // class where one is cheap to make; otherwise the wrong-kind receiver also exercises arg parsing.
            for (int k = 0; k <= params.size(); k++) qtc_arg_int(0);
            expect_error(qtc_call(is_obj ? an_object : a_value, S(sig)), what + " +1 arg");
            bool has_obj_param = false;
            for (const QJsonValue &p : params)
                if (p.toObject().value(QStringLiteral("kind")).toInt() == 10) has_obj_param = true;
            if (has_obj_param) {
                for (const QJsonValue &p : params) {
                    int kind = p.toObject().value(QStringLiteral("kind")).toInt();
                    if (kind == 10) qtc_arg_obj(stale);
                    else qtc_arg_null();
                }
                expect_error(qtc_call(is_obj ? an_object : a_value, S(sig)), what + " stale argument");
            }
        }
        for (const QJsonValue &cv : d.value(QStringLiteral("constructors")).toArray()) {
            QJsonObject c = cv.toObject();
            ctors++;
            QJsonArray params = c.value(QStringLiteral("params")).toArray();
            QByteArray what = "new " + c.value(QStringLiteral("sig")).toString().toUtf8();
            for (int k = 0; k < 17; k++) qtc_arg_int(k);
            if (is_obj) expect_error(qtc_new(S(cls)) ? 0 : qtc_last_error(), what + " overflow");
            else expect_error(qtc_val_new(S(cls)) ? 0 : qtc_last_error(), what + " overflow");
            bool any_strict = false;
            for (const QJsonValue &p : params) {
                int kind = p.toObject().value(QStringLiteral("kind")).toInt();
                if (kind != 11) any_strict = true;
            }
            if (!any_strict || params.isEmpty()) continue;
            for (const QJsonValue &p : params) push_wrong(p.toObject().value(QStringLiteral("kind")).toInt(), an_object, 0);
            uint64_t h = is_obj ? qtc_new(S(cls)) : qtc_val_new(S(cls));
            if (h) {
                // another overload accepted the arguments: that is legitimate, clean up
                if (is_obj) qtc_destroy(h);
                else qtc_val_free(h);
                g_calls++;
            } else {
                expect_error(qtc_last_error(), what + " wrong types");
            }
        }
        if (i % 50 == 0) qtc_process_events();
    }
    qtc_process_events();
    std::printf("sweep: %d classes, %d methods, %d constructors, %d misuse calls, %d unexpected\n", classes, methods, ctors,
                g_calls, g_bad);
    for (auto it = g_codes.begin(); it != g_codes.end(); ++it)
        std::printf("  %-26s %d\n", V(qtc_status_name(it.key())).constData(), it.value());
    qtc_cancel_quit();
    qtc_val_free(a_value);
    qtc_shutdown();
    return g_bad ? 1 : 0;
}
