/* form.c: a small form with a list of entries, built from Qt widgets.
 *
 *   ./form               fill it in and press Submit; close the window to quit
 *   ./form --selftest    fills it in and submits it itself, then checks the list
 *
 * Shows properties (qtc_set_* / qtc_get_*), enums by name (qtc_enum_value),
 * several signals in one loop, and coalescing: a burst of textEdited signals
 * arrives as one event whose count says how many there were.
 */
#include "qtc.h"

#include <stdio.h>
#include <string.h>

enum { TAG_SUBMIT = 1, TAG_EDITED = 2, TAG_SELFTEST_DONE = 3 };

static qtc_obj make(const char *cls) { return qtc_new(qtc_s(cls)); }

static void add_row(qtc_obj form, const char *label, qtc_obj field) {
    qtc_arg_str(qtc_s(label));
    qtc_arg_obj(field);
    qtc_call(form, QTC_S("addRow"));   /* QFormLayout::addRow(QString, QWidget*) */
}

int main(int argc, char **argv) {
    const bool selftest = argc > 1 && strcmp(argv[1], "--selftest") == 0;
    if (qtc_init_argv(QTC_APP_WIDGETS, argc, (const char *const *)argv) != QTC_OK) return 1;
    qtc_set_error_policy(QTC_ERRORS_LOG);

    qtc_obj window = make("QWidget");
    qtc_set_str(window, QTC_S("windowTitle"), QTC_S("Sign-up"));
    qtc_arg_obj(window);
    qtc_obj form = qtc_new(QTC_S("QFormLayout"));

    qtc_obj name = make("QLineEdit");
    qtc_set_str(name, QTC_S("placeholderText"), QTC_S("your name"));
    qtc_obj age = make("QSpinBox");
    qtc_arg_int(0);
    qtc_arg_int(130);
    qtc_call(age, QTC_S("setRange"));
    qtc_set_int(age, QTC_S("value"), 30);
    qtc_obj country = make("QComboBox");
    const char *countries[] = {"India", "Netherlands", "Brazil", "Japan"};
    for (int i = 0; i < 4; i++) {
        qtc_arg_str(qtc_s(countries[i]));
        qtc_call(country, QTC_S("addItem"));
    }
    qtc_arg_str(QTC_S("Subscribe to the newsletter"));
    qtc_obj subscribe = qtc_new(QTC_S("QCheckBox"));
    qtc_arg_str(QTC_S("Submit"));
    qtc_obj submit = qtc_new(QTC_S("QPushButton"));
    qtc_obj entries = make("QListWidget");

    add_row(form, "Name", name);
    add_row(form, "Age", age);
    add_row(form, "Country", country);
    qtc_arg_obj(subscribe);
    qtc_call(form, QTC_S("addRow"));
    qtc_arg_obj(submit);
    qtc_call(form, QTC_S("addRow"));
    add_row(form, "Entries", entries);

    /* enums by name: Qt::AlignRight | Qt::AlignVCenter for the labels */
    int64_t right = 0, vcenter = 0;
    qtc_enum_value(QTC_S("Qt::AlignRight"), &right);
    qtc_enum_value(QTC_S("Qt::AlignVCenter"), &vcenter);
    qtc_arg_int(right | vcenter);
    qtc_call(form, QTC_S("setLabelAlignment"));

    qtc_connect(submit, QTC_S("clicked"), TAG_SUBMIT, 0);
    qtc_connect(name, QTC_S("textEdited"), TAG_EDITED, QTC_CONN_COALESCE);
    qtc_call(window, QTC_S("show"));

    if (selftest) {
        const char *keys[] = {"A", "d", "a"};   /* insert() counts as editing: three textEdited signals */
        for (int i = 0; i < 3; i++) {
            qtc_arg_str(qtc_s(keys[i]));
            qtc_call(name, QTC_S("insert"));
        }
        qtc_set_int(age, QTC_S("value"), 36);
        qtc_set_int(country, QTC_S("currentIndex"), 1);
        qtc_set_bool(subscribe, QTC_S("checked"), true);
        qtc_call(submit, QTC_S("click"));
        qtc_post(TAG_SELFTEST_DONE, 0, 0);
    }

    int edit_events = 0, keystrokes = 0;
    qtc_event ev;
    while (qtc_wait(-1) == QTC_WAIT_EVENTS) {
        while (qtc_poll(&ev)) {
            if (ev.tag == TAG_SUBMIT) {
                char line[512];
                /* each qtc_view is valid until the next call that returns one: copy it first */
                char who[200];
                snprintf(who, sizeof who, "%s", qtc_get_str(name, QTC_S("text")).data);
                snprintf(line, sizeof line, "%s, %lld, %s%s", who[0] ? who : "(no name)",
                         (long long)qtc_get_int(age, QTC_S("value")), qtc_get_str(country, QTC_S("currentText")).data,
                         qtc_get_bool(subscribe, QTC_S("checked")) ? ", subscribed" : "");
                qtc_arg_str(qtc_s(line));
                qtc_call(entries, QTC_S("addItem"));
                qtc_call(name, QTC_S("clear"));
            } else if (ev.tag == TAG_EDITED) {
                edit_events++;
                keystrokes += ev.count;
                printf("name edited (%d keystroke%s)\n", ev.count, ev.count == 1 ? "" : "s");
            } else if (ev.tag == TAG_SELFTEST_DONE) {
                qtc_quit(0);
            }
        }
    }

    int rc = 0;
    if (selftest) {
        qtc_call(entries, QTC_S("count"));
        const int64_t n = qtc_ret_int();
        /* list items are not QObjects, so read the entry through the list's model */
        char first[512] = "";
        qtc_obj model = qtc_call(entries, QTC_S("model")) == QTC_OK ? qtc_ret_obj() : 0;
        if (model) {
            qtc_arg_int(0);
            qtc_arg_int(0);
            qtc_call(model, QTC_S("index"));   /* a QModelIndex value */
            qtc_val index = qtc_ret_val();
            qtc_arg_val(index);
            qtc_call(model, QTC_S("data"));    /* data(index), role defaults to DisplayRole */
            qtc_val data = qtc_ret_val();
            snprintf(first, sizeof first, "%s", qtc_val_get_str(data).data);
            qtc_val_free(data);
            qtc_val_free(index);
        }
        rc = (n == 1 && strcmp(first, "Ada, 36, Netherlands, subscribed") == 0 && edit_events == 1 && keystrokes == 3) ? 0 : 1;
        printf("form: %lld entr%s, first \"%s\", %d edit event%s for %d keystrokes: %s\n", (long long)n, n == 1 ? "y" : "ies",
               first, edit_events, edit_events == 1 ? "" : "s", keystrokes, rc ? "FAIL" : "ok");
    }
    qtc_shutdown();
    return rc;
}
