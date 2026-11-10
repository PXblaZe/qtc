/* qml.c: a Qt Quick window talking to C through a QtcBridge.
 *
 *   ./qml                click the square; close the window to quit
 *   ./qml --selftest     drives it itself and checks both directions
 *
 * QML calls bridge.send(name, payload): C gets a "sent" signal event with a
 * structured payload (any JSON-like value). C calls bridge.post(name, payload):
 * QML gets the bridge's message(name, payload) signal. QML properties and
 * functions are reached like C++ ones: qtc_get_*, qtc_set_*, qtc_call.
 */
#include "qtc.h"

#include <stdio.h>
#include <string.h>

static const char QML[] =
    "import QtQuick\n"
    "Window {\n"
    "    id: win\n"
    "    width: 320; height: 220; visible: true\n"
    "    title: \"QML from C\"\n"
    "    property int clicks: 0\n"
    "    property string status: \"waiting\"\n"
    "    function press() { clicks++; bridge.send(\"clicked\", { clicks: clicks, at: Date.now() > 0 }) }\n"
    "    Rectangle {\n"
    "        anchors.centerIn: parent; width: 120; height: 120; radius: 12\n"
    "        color: win.clicks % 2 ? \"#ffcc33\" : \"#3388ff\"\n"
    "        Text { anchors.centerIn: parent; text: win.clicks; font.pixelSize: 40 }\n"
    "        MouseArea { anchors.fill: parent; onClicked: win.press() }\n"
    "    }\n"
    "    Text { anchors.bottom: parent.bottom; anchors.horizontalCenter: parent.horizontalCenter; text: win.status }\n"
    "    Connections {\n"
    "        target: bridge\n"
    "        function onMessage(name, payload) { win.status = name + \": \" + payload.text }\n"
    "    }\n"
    "}\n";

enum { TAG_SENT = 1, TAG_SELFTEST_DONE = 2 };

int main(int argc, char **argv) {
    const bool selftest = argc > 1 && strcmp(argv[1], "--selftest") == 0;
    if (qtc_init_argv(QTC_APP_GUI, argc, (const char *const *)argv) != QTC_OK) return 1;
    qtc_set_error_policy(QTC_ERRORS_LOG);

    qtc_obj engine = qtc_new(QTC_S("QQmlApplicationEngine"));
    qtc_obj bridge = qtc_new(QTC_S("QtcBridge"));
    qtc_call(engine, QTC_S("rootContext"));
    qtc_obj context = qtc_ret_obj();
    qtc_arg_str(QTC_S("bridge"));
    qtc_arg_obj(bridge);
    qtc_call(context, QTC_S("setContextProperty"));
    qtc_connect(bridge, QTC_S("sent"), TAG_SENT, 0);

    qtc_arg_bytes(qtc_sn(QML, (int64_t)sizeof QML - 1));
    qtc_call(engine, QTC_S("loadData"));
    qtc_call(engine, QTC_S("rootObjects"));
    qtc_val roots = qtc_ret_val();
    qtc_val first = qtc_list_len(roots) > 0 ? qtc_list_get(roots, 0) : 0;
    qtc_obj window = first ? qtc_val_get_obj(first) : 0;
    qtc_val_free(first);
    qtc_val_free(roots);
    if (!window) {
        fprintf(stderr, "the QML did not load\n");
        qtc_shutdown();
        return 1;
    }

    if (selftest) {
        qtc_call(window, QTC_S("press"));   /* what a click on the square does */
        qtc_call(window, QTC_S("press"));
        qtc_post(TAG_SELFTEST_DONE, 0, 0);
    }

    int64_t last_clicks = 0;
    qtc_event ev;
    while (qtc_wait(-1) == QTC_WAIT_EVENTS) {
        while (qtc_poll(&ev)) {
            if (ev.tag == TAG_SENT) {
                /* sent(QString name, QVariant payload) */
                char name[64];
                snprintf(name, sizeof name, "%s", qtc_event_arg_str(ev.seq, 0).data);
                qtc_val payload = qtc_event_arg_val(ev.seq, 1);
                printf("QML sent %s %s\n", name, qtc_val_to_json(payload).data);
                qtc_val clicks = qtc_map_get(payload, QTC_S("clicks"));
                qtc_val_get_int(clicks, &last_clicks);
                qtc_val_free(clicks);
                qtc_val_free(payload);
                /* answer: bridge.post(name, payload) reaches QML's onMessage */
                char json[96];
                snprintf(json, sizeof json, "{\"text\": \"C saw %lld\"}", (long long)last_clicks);
                qtc_val answer = qtc_val_from_json(qtc_s(json));
                qtc_arg_str(QTC_S("reply"));
                qtc_arg_val(answer);
                qtc_call(bridge, QTC_S("post"));
                qtc_val_free(answer);
            } else if (ev.tag == TAG_SELFTEST_DONE) {
                qtc_quit(0);
            }
        }
    }

    int rc = 0;
    if (selftest) {
        char status[128];
        snprintf(status, sizeof status, "%s", qtc_get_str(window, QTC_S("status")).data);
        const int64_t clicks = qtc_get_int(window, QTC_S("clicks"));
        rc = (clicks == 2 && last_clicks == 2 && strcmp(status, "reply: C saw 2") == 0) ? 0 : 1;
        printf("qml: clicks %lld, C saw %lld, status \"%s\": %s\n", (long long)clicks, (long long)last_clicks, status,
               rc ? "FAIL" : "ok");
    }
    qtc_shutdown();
    return rc;
}
