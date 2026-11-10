/*
 * qtc_test — simulated input for testing qtc applications (built on QtTest).
 *
 * Link libqtc_test in test programs only. Every function takes a widget
 * handle; positions are in the widget's coordinates, x < 0 means its centre.
 */
#ifndef QTC_TEST_H
#define QTC_TEST_H

#include "qtc.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  if defined(QTC_TEST_BUILDING)
#    define QTC_TEST_API __declspec(dllexport)
#  else
#    define QTC_TEST_API __declspec(dllimport)
#  endif
#else
#  define QTC_TEST_API __attribute__((visibility("default")))
#endif

/* button: a Qt::MouseButton (1 = left, 2 = right, 4 = middle); modifiers: Qt::KeyboardModifier bits. */
QTC_TEST_API qtc_status qtct_click(qtc_obj widget, int32_t button, int32_t modifiers, int32_t x, int32_t y);
QTC_TEST_API qtc_status qtct_double_click(qtc_obj widget, int32_t button, int32_t modifiers, int32_t x, int32_t y);
QTC_TEST_API qtc_status qtct_press(qtc_obj widget, int32_t button, int32_t modifiers, int32_t x, int32_t y);
QTC_TEST_API qtc_status qtct_release(qtc_obj widget, int32_t button, int32_t modifiers, int32_t x, int32_t y);
QTC_TEST_API qtc_status qtct_move(qtc_obj widget, int32_t x, int32_t y);
QTC_TEST_API qtc_status qtct_wheel(qtc_obj widget, int32_t delta_y, int32_t delta_x, int32_t x, int32_t y);
/* key: a Qt::Key; a press and a release. */
QTC_TEST_API qtc_status qtct_key(qtc_obj widget, int32_t key, int32_t modifiers);
/* Types each character of text into the widget. */
QTC_TEST_API qtc_status qtct_type(qtc_obj widget, qtc_str text);
/* Processes events for ms milliseconds (qtc events stay queued). */
QTC_TEST_API qtc_status qtct_wait(int64_t ms);
/* Renders the widget into a PNG file. */
QTC_TEST_API qtc_status qtct_grab(qtc_obj widget, qtc_str png_path);
/* Shows the widget and waits until it is exposed. */
QTC_TEST_API qtc_status qtct_show(qtc_obj widget);

#ifdef __cplusplus
}
#endif
#endif
