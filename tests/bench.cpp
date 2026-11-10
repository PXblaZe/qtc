// qtc overhead against direct C++ Qt calls on the same objects.
#include "qtc.h"

#include <QtCore/QElapsedTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSlider>

#include <cstdio>
#include <functional>

static qtc_str S(const char *s) { return qtc_s(s); }

static double bench(const char *what, int n, const std::function<void()> &f) {
    for (int i = 0; i < n / 10; i++) f();   // warm up
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < n; i++) f();
    double ns = double(t.nsecsElapsed()) / n;
    (void)what;
    return ns;
}

static void row(const char *what, double qtc_ns, double direct_ns) {
    if (direct_ns > 0)
        std::printf("| %-46s | %9.0f | %9.0f | %6.1fx |\n", what, qtc_ns, direct_ns, qtc_ns / direct_ns);
    else
        std::printf("| %-46s | %9.0f | %9s | %7s |\n", what, qtc_ns, "-", "-");
}

int main() {
    qtc_init(QTC_APP_WIDGETS, qtc_sn("bench\0-platform\0offscreen", 26));
    qtc_obj l = qtc_new(S("QLabel"));
    qtc_obj s = qtc_new(S("QSlider"));
    qtc_obj b = qtc_new(S("QPushButton"));
    qtc_obj cb = qtc_new(S("QComboBox"));
    auto *ql = static_cast<QLabel *>(qtc_unsafe_pointer(l));
    auto *qs = static_cast<QSlider *>(qtc_unsafe_pointer(s));
    auto *qb = static_cast<QPushButton *>(qtc_unsafe_pointer(b));
    auto *qcb = static_cast<QComboBox *>(qtc_unsafe_pointer(cb));
    qs->setMaximum(1 << 30);
    const int N = 200000;
    int k = 0;

    std::printf("qtc %s on Qt %s, ns per operation (lower is better)\n\n", qtc_version().data, qtc_qt_version().data);
    std::printf("| %-46s | %9s | %9s | %7s |\n", "operation", "qtc", "direct", "ratio");
    std::printf("|%s|%s|%s|%s|\n", "------------------------------------------------", "-----------", "-----------", "---------");

    row("QLabel::setText through a method call",
        bench("", N, [&] { qtc_arg_str(S("hello")); qtc_call(l, S("setText")); }),
        bench("", N, [&] { ql->setText(QStringLiteral("hello")); }));
    row("QLabel::setText with a full signature",
        bench("", N, [&] { qtc_arg_str(S("hello")); qtc_call(l, S("setText(QString)")); }), 0);
    row("QLabel text through the property system",
        bench("", N, [&] { qtc_set_str(l, S("text"), S("hello")); }),
        bench("", N, [&] { ql->setProperty("text", QStringLiteral("hello")); }));
    row("QSlider::setValue (int)",
        bench("", N, [&] { qtc_arg_int(++k & 0xffff); qtc_call(s, S("setValue")); }),
        bench("", N, [&] { qs->setValue(++k & 0xffff); }));
    row("QSlider::value() and read the result",
        bench("", N, [&] { qtc_call(s, S("value")); (void)qtc_ret_int(); }),
        bench("", N, [&] { (void)qs->value(); }));
    row("QSlider value property get",
        bench("", N, [&] { (void)qtc_get_int(s, S("value")); }),
        bench("", N, [&] { (void)qs->property("value").toInt(); }));
    row("QLabel::text() returning a string",
        bench("", N, [&] { qtc_call(l, S("text")); (void)qtc_ret_str(); }),
        bench("", N, [&] { (void)ql->text().toUtf8(); }));
    row("QComboBox::addItem, resolved among 3 overloads",
        bench("", N / 4, [&] { qtc_arg_str(S("x")); qtc_call(cb, S("addItem")); }),
        bench("", N / 4, [&] { qcb->addItem(QStringLiteral("x")); }));
    qcb->clear();
    uint64_t c = qtc_connect(b, S("clicked"), 1, 0);
    int hits = 0;
    QObject::connect(qb, &QPushButton::clicked, [&] { hits++; });
    row("signal to C: emit, capture, poll (with args)",
        bench("", N, [&] { qb->click(); qtc_event e; qtc_poll(&e); }),
        bench("", N, [&] { qb->click(); }));
    qtc_disconnect(c);
    c = qtc_connect(b, S("clicked"), 1, QTC_CONN_NO_ARGS);
    row("signal to C without argument capture",
        bench("", N, [&] { qb->click(); qtc_event e; qtc_poll(&e); }), 0);
    qtc_disconnect(c);
    row("value handle create + free",
        bench("", N, [&] { qtc_val v = qtc_val_int(1); qtc_val_free(v); }), 0);
    row("qtc_alive on a live handle", bench("", N * 5, [&] { (void)qtc_alive(l); }), 0);
    row("call on a stale handle (rejected)",
        bench("", N, [&] { qtc_call(0x500000001ull, S("show")); }), 0);
    qtc_shutdown();
    return 0;
}
