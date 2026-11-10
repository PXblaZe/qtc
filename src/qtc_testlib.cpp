// libqtc_test: QtTest-based input simulation for qtc handles.
#include "qtc_test.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QThread>
#include <QtGui/QWheelEvent>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

static QWidget *widget_of(qtc_obj h, qtc_status *err) {
    *err = QTC_OK;
    void *p = qtc_unsafe_pointer(h);
    if (!p) {
        *err = qtc_last_error();
        if (*err == QTC_OK) *err = QTC_ERR_BAD_HANDLE;
        return nullptr;
    }
    QObject *o = static_cast<QObject *>(p);
    QWidget *w = qobject_cast<QWidget *>(o);
    if (!w) *err = QTC_ERR_WRONG_TYPE;
    return w;
}

static bool valid_button(int32_t b) {
    return b == Qt::LeftButton || b == Qt::RightButton || b == Qt::MiddleButton || b == Qt::BackButton ||
           b == Qt::ForwardButton;
}
static bool valid_mods(int32_t m) {
    const int32_t all = Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier |
                        Qt::KeypadModifier | Qt::GroupSwitchModifier;
    return (m & ~all) == 0;
}
static QPoint pos_of(QWidget *w, int32_t x, int32_t y) {
    if (x < 0 || y < 0) return w->rect().center();
    return QPoint(x, y);
}

#define WIDGET_OR_RETURN(w, h)                  \
    qtc_status err_ = QTC_OK;                   \
    QWidget *w = widget_of(h, &err_);           \
    if (!w) return err_;

extern "C" {

QTC_TEST_API qtc_status qtct_click(qtc_obj h, int32_t button, int32_t mods, int32_t x, int32_t y) {
    WIDGET_OR_RETURN(w, h);
    if (!valid_button(button) || !valid_mods(mods)) return QTC_ERR_OUT_OF_RANGE;
    QTest::mouseClick(w, Qt::MouseButton(button), Qt::KeyboardModifiers(mods), pos_of(w, x, y));
    return QTC_OK;
}
QTC_TEST_API qtc_status qtct_double_click(qtc_obj h, int32_t button, int32_t mods, int32_t x, int32_t y) {
    WIDGET_OR_RETURN(w, h);
    if (!valid_button(button) || !valid_mods(mods)) return QTC_ERR_OUT_OF_RANGE;
    QTest::mouseDClick(w, Qt::MouseButton(button), Qt::KeyboardModifiers(mods), pos_of(w, x, y));
    return QTC_OK;
}
QTC_TEST_API qtc_status qtct_press(qtc_obj h, int32_t button, int32_t mods, int32_t x, int32_t y) {
    WIDGET_OR_RETURN(w, h);
    if (!valid_button(button) || !valid_mods(mods)) return QTC_ERR_OUT_OF_RANGE;
    QTest::mousePress(w, Qt::MouseButton(button), Qt::KeyboardModifiers(mods), pos_of(w, x, y));
    return QTC_OK;
}
QTC_TEST_API qtc_status qtct_release(qtc_obj h, int32_t button, int32_t mods, int32_t x, int32_t y) {
    WIDGET_OR_RETURN(w, h);
    if (!valid_button(button) || !valid_mods(mods)) return QTC_ERR_OUT_OF_RANGE;
    QTest::mouseRelease(w, Qt::MouseButton(button), Qt::KeyboardModifiers(mods), pos_of(w, x, y));
    return QTC_OK;
}
QTC_TEST_API qtc_status qtct_move(qtc_obj h, int32_t x, int32_t y) {
    WIDGET_OR_RETURN(w, h);
    QTest::mouseMove(w, pos_of(w, x, y));
    return QTC_OK;
}
QTC_TEST_API qtc_status qtct_wheel(qtc_obj h, int32_t dy, int32_t dx, int32_t x, int32_t y) {
    WIDGET_OR_RETURN(w, h);
    QPoint p = pos_of(w, x, y);
    QWheelEvent ev(QPointF(p), QPointF(w->mapToGlobal(p)), QPoint(), QPoint(dx, dy), Qt::NoButton, Qt::NoModifier,
                   Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(w, &ev);
    return QTC_OK;
}
QTC_TEST_API qtc_status qtct_key(qtc_obj h, int32_t key, int32_t mods) {
    WIDGET_OR_RETURN(w, h);
    if (key <= 0 || !valid_mods(mods)) return QTC_ERR_OUT_OF_RANGE;
    QTest::keyClick(w, Qt::Key(key), Qt::KeyboardModifiers(mods));
    return QTC_OK;
}
QTC_TEST_API qtc_status qtct_type(qtc_obj h, qtc_str text) {
    WIDGET_OR_RETURN(w, h);
    if (text.len > 0 && !text.data) return QTC_ERR_INVALID_ARG;
    QString s = text.len < 0 ? QString::fromUtf8(text.data ? text.data : "") : QString::fromUtf8(text.data, qsizetype(text.len));
    QTest::keyClicks(w, s);
    return QTC_OK;
}
QTC_TEST_API qtc_status qtct_wait(int64_t ms) {
    if (!qtc_is_initialized()) return QTC_ERR_NOT_INITIALIZED;
    if (ms < 0 || ms > 3600000) return QTC_ERR_OUT_OF_RANGE;
    QTest::qWait(int(ms));
    return QTC_OK;
}
QTC_TEST_API qtc_status qtct_grab(qtc_obj h, qtc_str path) {
    WIDGET_OR_RETURN(w, h);
    if (path.len > 0 && !path.data) return QTC_ERR_INVALID_ARG;
    QString p = path.len < 0 ? QString::fromUtf8(path.data ? path.data : "") : QString::fromUtf8(path.data, qsizetype(path.len));
    if (p.isEmpty()) return QTC_ERR_INVALID_ARG;
    return w->grab().save(p, "PNG") ? QTC_OK : QTC_ERR_IO;
}
QTC_TEST_API qtc_status qtct_show(qtc_obj h) {
    WIDGET_OR_RETURN(w, h);
    w->show();
    if (!QTest::qWaitForWindowExposed(w, 2000)) return QTC_ERR_STATE;
    return QTC_OK;
}

}  // extern "C"
