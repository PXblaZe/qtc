// Ownership roles: what qtc_destroy must check and do before deleting an object.
//
// Qt objects point at each other with plain pointers in many places, and deleting the
// wrong object crashes the program later. Before qtc_destroy deletes an object it goes
// through four checks, in this order:
//
// 1. Objects that belong to Qt itself are refused (QTC_ERR_STATE): screens, the style
//    hints, input method, clipboard and its data, the application style, the main
//    thread's QThread and any running thread, event dispatchers, the native window of
//    a widget, a Quick window's content item, QML contexts made by the QML engine, the
//    driver of an open database connection, text frames, tables and lists of a
//    document, a document's current layout, and chart legends.
//
// 2. Owners that hold the object (or something inside it) and have no way to let go are
//    asked first: a QRestAccessManager's network manager, the engine of spatial sounds
//    and listeners, the device of a QWaveDecoder or an NMEA source, the window of a
//    QQuickRenderControl, the network manager of a QQmlEngine, the video sink of a
//    video widget, the transport of an MQTT client, the built-in reply handler of an
//    OAuth object. The object cannot be destroyed before its user.
//
// 3. Owners that can let go do so through their own API, then the object is deleted:
//    a scroll area's viewport, corner widget and scroll bars, table and tree headers, a
//    text edit's document, QMessageBox's check box, QProgressDialog's label, button and
//    bar, QWizard's buttons, side widget and pages, a QComboBox's line edit and view, a
//    QCompleter's popup, tab buttons, a QTabWidget's corner widgets, a layout's menu
//    bar, the proxy model of a QFileDialog, the delegate of a QDataWidgetMapper, the
//    style of a graphics scene or widget, a QDrag's mime data, a QMovie's device, the
//    share context of an OpenGL context, source models of a QConcatenateTablesProxyModel,
//    the cache and cookie jar of a QNetworkAccessManager, a QML context's context
//    object, a QQmlFileSelector's selector, a QWebSocket's mask generator, the series of
//    an area series and the sets and slices of chart series.
//
// 4. Internal parts are refused: objects a constructor made inside the object it built
//    (a QFileDialog's widgets, a QMenu's menu action, a QDockWidget's toggle action, a
//    QSvgWidget's renderer...), objects whose parent is a private Qt class, objects
//    named "qt_*", a QTabWidget's tab bar, a QSplitter's handles and legend markers.
//
// Objects that qtc_new made are never "internal". Layouts without a parent widget do
// not notice when a widget in them is deleted or moves elsewhere (Qt takes it out only
// through the parent widget): qtc takes destroyed widgets and widgets that get another
// parent out of such layouts itself, refuses to put one widget into two places of them,
// and owners that keep a widget by a plain pointer (a tab widget's corner, a scroll
// area's viewport ...) refuse a widget that is still in one.
#include "qtc_internal.h"
#include "qtc_module.h"

#include <functional>

#include <QtCore/QAbstractEventDispatcher>
#include <QtCore/QAbstractProxyModel>
#include <QtCore/QConcatenateTablesProxyModel>
#include <QtCore/QFileSelector>
#include <QtCore/QSet>
#include <QtCore/QThread>
#include <QtCore/QThreadPool>
#include <QtCore/QAbstractAnimation>
#include <QtCore/QTranslator>
#include <QtCore/QMimeData>
#include <cstring>

#ifdef QTC_HAS_GUI
#include <QtGui/qtguiglobal.h>
#include <QtGui/QAbstractTextDocumentLayout>
#include <QtGui/QClipboard>
#include <QtGui/QDrag>
#include <QtGui/QGuiApplication>
#include <QtGui/QInputMethod>
#include <QtCore/QMimeData>
#include <QtGui/QMovie>
#include <QtGui/QScreen>
#include <QtGui/QStyleHints>
#include <QtGui/QTextDocument>
#include <QtGui/QTextObject>
#include <QtGui/QWindow>
#if QT_CONFIG(opengl)
#include <QtGui/QOpenGLContext>
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
#include <QtGui/QAccessibilityHints>
#endif
#endif

#ifdef QTC_HAS_WIDGETS
#include <QtWidgets/QAbstractScrollArea>
#include <QtWidgets/QApplication>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QColumnView>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QCompleter>
#include <QtWidgets/QDataWidgetMapper>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QGraphicsProxyWidget>
#include <QtWidgets/QGraphicsScene>
#include <QtWidgets/QGraphicsWidget>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListView>
#include <QtGui/QBackingStore>
#include <QtGui/QStandardItemModel>
#include <QtGui/QImage>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFocusFrame>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QProgressDialog>
#include <QtWidgets/QProxyStyle>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyledItemDelegate>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTableView>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QTreeView>
#include <QtWidgets/QWizard>
#endif

namespace qtc {

namespace {


struct Owner {
    QPointer<QObject> p;
    uint64_t kinds = 0;
    QList<QObject *> held;   // what it held when the index last looked
};
using Owners = QHash<QObject *, Owner>;

// Window containers made through QWidget::createWindowContainer, and the window each embeds
// (Qt has no public way to ask a container for it)
QHash<QObject *, QPointer<QObject>> &container_windows() {
    static auto *m = new QHash<QObject *, QPointer<QObject>>;
    return *m;
}
Owners &owners_map() {
    static Owners *s = new Owners;
    return *s;
}

// Keyed by address, checked through a QPointer before use: an object deleted while its
// signals were blocked leaves its entry behind until the registry notices.
using PtrMap = QHash<QObject *, QPointer<QObject>>;
PtrMap &layouts_map() {
    static PtrMap *s = new PtrMap;
    return *s;
}
// focus frames with a handle (see widget_parent_changed)
PtrMap &frames_map() {
    static PtrMap *s = new PtrMap;
    return *s;
}
QList<QObject *> live(PtrMap &t) {
    QList<QObject *> out;
    for (auto it = t.begin(); it != t.end();) {
        if (it.value()) { out.append(it.value().data()); ++it; }
        else it = t.erase(it);
    }
    return out;
}

// Parts a constructor made: part -> (the part, the object being constructed).
struct Part {
    QPointer<QObject> self;
    QPointer<QObject> root;
};
QHash<QObject *, Part> &parts_map() {
    static QHash<QObject *, Part> *s = new QHash<QObject *, Part>;
    return *s;
}
qsizetype g_parts_swept = 0;

// user -> what it was made with and uses until it dies (see roles_adopt_new)
struct MadePin {
    QPointer<QObject> user, used;
};
QList<MadePin> &made_pins() {
    static QList<MadePin> *s = new QList<MadePin>;
    return *s;
}

bool inside(const QObject *o, const QObject *root) {
    for (int d = 0; o && d < 10000; o = o->parent(), d++)
        if (o == root) return true;
    return false;
}

QString label(const QObject *o) { return QString::fromLatin1(o->metaObject()->className()); }

static uint64_t class_kinds(QObject *o);
namespace {
struct KindsPeek : QObject {   // QObject's protected d_ptr, as qtc_core.cpp's DPtrPeek reads it
    static const QObjectData *of(const QObject *o) {
        auto member = &KindsPeek::d_ptr;
        return (o->*member).data();
    }
};
}  // namespace

// The roles an object can play. All but one depend on its class alone, so they are worked out
// once per class (a few dozen casts each time cost more than the rest of making an object).
QHash<const QMetaObject *, uint64_t> &kinds_memo() { static auto *m = new QHash<const QMetaObject *, uint64_t>; return *m; }
QHash<const QMetaObject *, uint64_t> &typed_memo() { static auto *m = new QHash<const QMetaObject *, uint64_t>; return *m; }
QHash<const QMetaObject *, bool> &owned_memo() { static auto *m = new QHash<const QMetaObject *, bool>; return *m; }

uint64_t kinds_of(QObject *o) {
    uint64_t k = 0;
    const QObjectData *d = KindsPeek::of(o);
    if (d && d->metaObject) {
        load_modules_of(o);
        k = class_kinds(o);   // a dynamic meta-object: it can change, and its address can be reused
    } else {
        QHash<const QMetaObject *, uint64_t> *memo = &kinds_memo();
        const QMetaObject *mo = o->metaObject();
        auto it = memo->constFind(mo);
        if (it == memo->constEnd()) {
            load_modules_of(o);   // the module of its class knows its roles
            memo = &kinds_memo();
            it = memo->insert(mo, class_kinds(o));
        }
        k = *it;
    }
#ifdef QTC_HAS_WIDGETS
    if (container_windows().contains(o)) k |= OK_WINDOWCONTAINER;
#endif
    return k;
}

static uint64_t class_kinds(QObject *o) {
    uint64_t k = 0;
#ifdef QTC_HAS_WIDGETS
    if (qobject_cast<QAbstractScrollArea *>(o)) k |= OK_SCROLLAREA;
    if (qobject_cast<QMessageBox *>(o)) k |= OK_MESSAGEBOX;
    if (qobject_cast<QFocusFrame *>(o)) k |= OK_FOCUSFRAME;
    if (qobject_cast<QDialogButtonBox *>(o)) k |= OK_BUTTONBOX;
    if (qobject_cast<QProgressDialog *>(o)) k |= OK_PROGRESS;
    if (qobject_cast<QWizard *>(o)) k |= OK_WIZARD;
    if (qobject_cast<QComboBox *>(o)) k |= OK_COMBO;
    if (qobject_cast<QCompleter *>(o)) k |= OK_COMPLETER;
    if (qobject_cast<QTabBar *>(o)) k |= OK_TABBAR;
    if (qobject_cast<QTabWidget *>(o)) k |= OK_TABWIDGET;
    if (qobject_cast<QLayout *>(o)) k |= OK_LAYOUT;
    if (qobject_cast<QFileDialog *>(o)) k |= OK_FILEDIALOG;
    if (qobject_cast<QDataWidgetMapper *>(o)) k |= OK_MAPPER;
    if (qobject_cast<QGraphicsScene *>(o)) k |= OK_SCENE;
    if (qobject_cast<QGraphicsWidget *>(o)) k |= OK_GWIDGET;
#endif
#ifdef QTC_HAS_GUI
    if (qobject_cast<QDrag *>(o)) k |= OK_DRAG;
    if (qobject_cast<QMovie *>(o)) k |= OK_MOVIE;
#if QT_CONFIG(opengl)
    if (qobject_cast<QOpenGLContext *>(o)) k |= OK_GLCONTEXT;
#endif
#endif
    if (qobject_cast<QConcatenateTablesProxyModel *>(o)) k |= OK_CONCAT;
    for (const ModuleExt *e : module_exts())
        if (e->class_kinds) k |= e->class_kinds(o);
    return k;
}


// Does owner hold victim, or something inside it? With release set, let go of what it can.
// the victim, or something that dies with it (a child), could be a T
template <class T> bool vcast(const QPointer<QObject> &vp) {
    if (!vp) return false;
    return qobject_cast<T *>(vp.data()) || vp->findChild<T *>() != nullptr;
}
bool made_by_qtc(const QObject *o);

// With collect set (and no victim), lists what owner holds instead: the held-object index.
// Roles looked up by the victim's type (vcast) are not listed; see candidates().
int visit(QObject *owner, uint64_t kinds, QObject *victim, bool release, QList<QObject *> *collect = nullptr) {
    VisitCtx ctx;
    ctx.victim = victim;
    ctx.vp = victim;   // a release may delete the victim (an owner that owns what it holds): look at it only while it lives
    ctx.release = release;
    ctx.collect = collect;
    const QPointer<QObject> &vp = ctx.vp;
    int &st = ctx.st;
    auto hit = [&](const QObject *c) { return ctx.hit(c); };
    auto held = [&]() { return ctx.held(); };
    auto pin = [&]() { ctx.pin(); };
    (void)hit;
    (void)held;
    (void)pin;
    (void)owner;
    (void)kinds;
    (void)release;
#ifdef QTC_HAS_WIDGETS
    if (kinds & OK_SCROLLAREA) {
        auto *a = static_cast<QAbstractScrollArea *>(owner);
        if (hit(a->viewport()) && held()) a->setViewport(nullptr);
        if (hit(a->cornerWidget()) && held()) a->setCornerWidget(nullptr);
        if (hit(a->verticalScrollBar()) && held()) a->setVerticalScrollBar(new QScrollBar(Qt::Vertical));
        if (hit(a->horizontalScrollBar()) && held()) a->setHorizontalScrollBar(new QScrollBar(Qt::Horizontal));
        if (auto *t = qobject_cast<QTableView *>(owner)) {
            if (hit(t->horizontalHeader()) && held()) t->setHorizontalHeader(new QHeaderView(Qt::Horizontal, t));
            if (hit(t->verticalHeader()) && held()) t->setVerticalHeader(new QHeaderView(Qt::Vertical, t));
        }
        if (auto *t = qobject_cast<QTreeView *>(owner))
            if (hit(t->header()) && held()) t->setHeader(new QHeaderView(Qt::Horizontal, t));
        if (auto *c = qobject_cast<QColumnView *>(owner))   // older Qt 6 crashes on a null preview
            if (hit(c->previewWidget()) && held()) c->setPreviewWidget(new QWidget);
        if (auto *e = qobject_cast<QTextEdit *>(owner))
            if (hit(e->document()) && held()) e->setDocument(nullptr);
        if (auto *e = qobject_cast<QPlainTextEdit *>(owner))
            if (hit(e->document()) && held()) e->setDocument(nullptr);
    }
    if (kinds & OK_MESSAGEBOX) {
        auto *m = static_cast<QMessageBox *>(owner);
        if (hit(m->checkBox()) && held()) m->setCheckBox(nullptr);
        // it keeps its custom buttons in a list of plain pointers besides the button box, and
        // buttonText(), setButtonText() and exec() read that list: a button leaves it first
        const QList<QAbstractButton *> buttons = m->buttons();
        for (QAbstractButton *b : buttons)
            if (hit(b) && held()) m->removeButton(b);
    }
    if (kinds & OK_BUTTONBOX) {
        // it keeps its buttons in lists of plain pointers, and clear() and setStandardButtons()
        // delete them wherever they have moved since: a button stays in its box until it leaves
        auto *bb = static_cast<QDialogButtonBox *>(owner);
        const QList<QAbstractButton *> buttons = bb->buttons();
        for (QAbstractButton *b : buttons)
            if (hit(b) && held()) bb->removeButton(b);
    }
    if (kinds & OK_WINDOWCONTAINER) {
        // it uses its window without checking that the window is still there whenever one of its
        // parents changes (QWindowContainer::parentWasChanged): the window stays with it
        if (hit(container_windows().value(owner).data())) pin();
    }
    if (kinds & OK_FOCUSFRAME) {
        // it follows its widget by mapping from the widget's parent to the parent the widget had,
        // which must stay one of its ancestors (mapTo walks up to it): the widget stays put while
        // a frame watches it, and leaves the frame first when it is destroyed
        auto *f = static_cast<QFocusFrame *>(owner);
        if (hit(f->widget()) && held()) f->setWidget(nullptr);
    }
    if (kinds & OK_PROGRESS) {
        auto *d = static_cast<QProgressDialog *>(owner);
        // no getters: its label, cancel button and bar are its only children of those types
        QList<QPointer<QObject>> kids;
        for (QObject *c : d->children()) kids.append(c);
        for (const QPointer<QObject> &c : kids) {
            if (!c || !hit(c)) continue;
            if (qobject_cast<QLabel *>(c)) { if (held()) d->setLabel(new QLabel(d->labelText())); }
            else if (qobject_cast<QPushButton *>(c)) { if (held()) d->setCancelButton(nullptr); }
            else if (qobject_cast<QProgressBar *>(c)) {
                if (held()) {
                    auto *bar = new QProgressBar;
                    bar->setRange(d->minimum(), d->maximum());
                    bar->setValue(d->value());
                    d->setBar(bar);
                }
            }
        }
    }
    if (kinds & OK_WIZARD) {
        auto *w = static_cast<QWizard *>(owner);
        for (int b = 0; b < QWizard::NButtons; b++)
            if (hit(w->button(QWizard::WizardButton(b))) && held()) w->setButton(QWizard::WizardButton(b), nullptr);
        if (hit(w->sideWidget()) && held()) w->setSideWidget(nullptr);
        const QList<int> ids = w->pageIds();
        for (int id : ids)
            if (hit(w->page(id)) && held()) w->removePage(id);
    }
    if (kinds & OK_COMBO) {
        auto *c = static_cast<QComboBox *>(owner);
        if (hit(c->lineEdit()) && held()) c->setEditable(false);
        // view() makes the popup when there is none, so only ask when the victim could be one
        if (vcast<QAbstractItemView>(vp) && hit(c->view()) && held()) c->setView(new QListView);
    }
    if (kinds & OK_COMPLETER) {
        auto *c = static_cast<QCompleter *>(owner);
        // popup() makes a popup when there is none, so only ask when the victim could be one
        if (vcast<QAbstractItemView>(vp) && hit(c->popup()) && held()) c->setPopup(new QListView);
    }
    if (kinds & OK_TABBAR) {
        auto *t = static_cast<QTabBar *>(owner);
        for (int i = 0; i < t->count(); i++)
            for (QTabBar::ButtonPosition side : {QTabBar::LeftSide, QTabBar::RightSide})
                if (hit(t->tabButton(i, side)) && held()) t->setTabButton(i, side, nullptr);
    }
    if (kinds & OK_TABWIDGET) {
        auto *t = static_cast<QTabWidget *>(owner);
        for (Qt::Corner c : {Qt::TopLeftCorner, Qt::TopRightCorner, Qt::BottomLeftCorner, Qt::BottomRightCorner})
            if (hit(t->cornerWidget(c)) && held()) t->setCornerWidget(nullptr, c);
    }
    if (kinds & OK_LAYOUT) {
        auto *l = static_cast<QLayout *>(owner);
        if (hit(l->menuBar()) && held()) l->setMenuBar(nullptr);
    }
    if (kinds & OK_FILEDIALOG) {
        auto *f = static_cast<QFileDialog *>(owner);
        if (hit(f->proxyModel()) && held()) f->setProxyModel(nullptr);
    }
    if (kinds & OK_MAPPER) {
        auto *m = static_cast<QDataWidgetMapper *>(owner);
        // it works only with a delegate: a fresh default one takes over
        if (hit(m->itemDelegate()) && held()) m->setItemDelegate(new QStyledItemDelegate(m));
    }
    if ((kinds & OK_SCENE) && vcast<QStyle>(vp)) {
        auto *s = static_cast<QGraphicsScene *>(owner);
        if (hit(s->style()) && held()) s->setStyle(nullptr);   // deletes it: the scene owns its style
    }
    if ((kinds & OK_GWIDGET) && vcast<QStyle>(vp)) {
        auto *g = static_cast<QGraphicsWidget *>(owner);
        if (hit(g->style()) && held()) g->setStyle(nullptr);
    }
#endif
#ifdef QTC_HAS_GUI
    if (kinds & OK_DRAG) {
        auto *d = static_cast<QDrag *>(owner);
        if (hit(d->mimeData()) && held()) d->setMimeData(nullptr);   // deletes it: the drag owns it
    }
    if (kinds & OK_MOVIE) {
        auto *m = static_cast<QMovie *>(owner);
        if (hit(m->device()) && held()) m->setDevice(nullptr);
    }
#if QT_CONFIG(opengl)
    if (kinds & OK_GLCONTEXT) {
        auto *c = static_cast<QOpenGLContext *>(owner);
        if (hit(c->shareContext()) && held()) c->setShareContext(nullptr);
    }
#endif
#endif
    if (kinds & OK_CONCAT) {
        auto *c = static_cast<QConcatenateTablesProxyModel *>(owner);
        const QList<QAbstractItemModel *> ms = c->sourceModels();
        for (QAbstractItemModel *m : ms)
            if (hit(m) && held()) c->removeSourceModel(m);
    }
    for (const ModuleExt *e : module_exts())
        if (e->visit) e->visit(owner, kinds, ctx);
    return st;
}

bool made_by_qtc(const QObject *o) { return reg().made(o); }

// Held object -> the owners holding it. An owner something was called on is looked at again
// (dirty); when roles_epoch moves on (Qt ran code of its own: event processing, a static call)
// every owner is.
QHash<QObject *, QList<QObject *>> &held_index() {
    static QHash<QObject *, QList<QObject *>> *s = new QHash<QObject *, QList<QObject *>>;
    return *s;
}
QSet<QObject *> &dirty_owners() {
    static QSet<QObject *> *s = new QSet<QObject *>;
    return *s;
}
uint64_t g_index_epoch = 0;

void unindex(QObject *o, Owner &ow) {
    QHash<QObject *, QList<QObject *>> &ix = held_index();
    for (QObject *h : ow.held) {
        auto it = ix.find(h);
        if (it == ix.end()) continue;
        it->removeAll(o);
        if (it->isEmpty()) ix.erase(it);
    }
    ow.held.clear();
}

void reindex(QObject *o, Owner &ow) {
    unindex(o, ow);
    QList<QObject *> got;
    visit(o, ow.kinds, nullptr, false, &got);
    QHash<QObject *, QList<QObject *>> &ix = held_index();
    for (QObject *h : got) {
        if (ow.held.contains(h)) continue;
        ow.held.append(h);
        ix[h].append(o);
    }
}

void ensure_index() {
    Owners &m = owners_map();
    if (g_index_epoch != roles_epoch) {
        held_index().clear();
        dirty_owners().clear();
        for (auto it = m.begin(); it != m.end();) {
            if (!it->p) { it = m.erase(it); continue; }
            it->held.clear();
            reindex(it.key(), *it);
            ++it;
        }
        g_index_epoch = roles_epoch;
        return;
    }
    if (dirty_owners().isEmpty()) return;
    const QSet<QObject *> d = std::exchange(dirty_owners(), {});
    for (QObject *o : d) {
        auto it = m.find(o);
        if (it == m.end()) continue;
        if (!it->p) { unindex(o, *it); m.erase(it); continue; }
        reindex(o, *it);
    }
}

// The owners that may hold x or something inside it (check each with visit()).
QList<QPair<QObject *, uint64_t>> candidates(QObject *x) {
    ensure_index();
    QList<QPair<QObject *, uint64_t>> out;
    QSet<QObject *> seen;
    const Owners &m = owners_map();
    auto take = [&](QObject *o) {
        if (seen.contains(o)) return;
        seen.insert(o);
        auto oi = m.constFind(o);
        if (oi != m.constEnd() && oi->p) out.append({o, oi->kinds});
    };
    const QHash<QObject *, QList<QObject *>> &ix = held_index();
    const QList<QObject *> kids = x->children().isEmpty() ? QList<QObject *>() : x->findChildren<QObject *>();
    if (!ix.isEmpty()) {
        auto look = [&](QObject *h) {
            auto it = ix.constFind(h);
            if (it != ix.constEnd())
                for (QObject *o : *it) take(o);
        };
        look(x);
        for (QObject *k : kids) look(k);
    }
    // roles whose getters would make what they return: asked by the type of the victim and of
    // everything that dies with it (a style made a child of the object destroyed)
    uint64_t typed = 0;
    auto type_of_uncached = [](QObject *o) -> uint64_t {
        uint64_t typed = 0;
#ifdef QTC_HAS_WIDGETS
        if (qobject_cast<QAbstractItemView *>(o)) typed |= OK_COMPLETER | OK_COMBO;
        if (qobject_cast<QStyle *>(o)) typed |= OK_SCENE | OK_GWIDGET;
#endif
        for (const ModuleExt *e : module_exts())
            if (e->typed) typed |= e->typed(o);
        Q_UNUSED(o);
        return typed;
    };
    // the casts depend on the class alone: once per meta-object (dynamic ones each time)
    auto type_of = [&typed, &type_of_uncached](QObject *o) {
        const QObjectData *d = KindsPeek::of(o);
        if (d && d->metaObject) { typed |= type_of_uncached(o); return; }
        QHash<const QMetaObject *, uint64_t> &memo = typed_memo();
        const QMetaObject *mo = o->metaObject();
        auto it = memo.constFind(mo);
        if (it == memo.constEnd()) it = memo.insert(mo, type_of_uncached(o));
        typed |= *it;
    };
    type_of(x);
    for (QObject *k : kids) type_of(k);
    if (typed)
        for (auto it = m.constBegin(); it != m.constEnd(); ++it)
            if ((it->kinds & typed) && it->p) take(it->p.data());
    return out;
}

// 1. Objects that belong to Qt itself.
QString qt_owned_by_class(QObject *x);

// Whether qt_owned can say anything about objects of x's class beyond the application object:
// most classes are none of the ones it knows, and asking all its casts of every object destroyed
// cost more than the rest of destroying it. Kept per class; keep it in step with
// qt_owned_by_class (a debug build checks that it is).
static bool qt_owned_class(QObject *x) {
    const auto relevant = [](QObject *o) {
        if (qobject_cast<QThread *>(o) || qobject_cast<QAbstractEventDispatcher *>(o) || qobject_cast<QThreadPool *>(o)) return true;
#ifdef QTC_HAS_GUI
        if (qobject_cast<QScreen *>(o) || qobject_cast<QStyleHints *>(o) || qobject_cast<QInputMethod *>(o) ||
            qobject_cast<QClipboard *>(o) || qobject_cast<QMimeData *>(o) || o->inherits("QWidgetWindow") ||
            o->inherits("QQuickWidgetOffscreenWindow") || qobject_cast<QTextObject *>(o) ||
            qobject_cast<QAbstractTextDocumentLayout *>(o))
            return true;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        if (qobject_cast<QAccessibilityHints *>(o)) return true;
#endif
#endif
#ifdef QTC_HAS_WIDGETS
        if (qobject_cast<QStyle *>(o)) return true;
#endif
        for (const ModuleExt *e : module_exts())
            if (e->qt_owned_class && e->qt_owned_class(o)) return true;
        return false;
    };
    const QObjectData *d = KindsPeek::of(x);
    if (d && d->metaObject) return relevant(x);   // a dynamic meta-object: asked every time
    QHash<const QMetaObject *, bool> &memo = owned_memo();
    const QMetaObject *mo = x->metaObject();
    auto it = memo.constFind(mo);
    if (it == memo.constEnd()) it = memo.insert(mo, relevant(x));
    return *it;
}

QString qt_owned(QObject *x) {
    if (x == QCoreApplication::instance()) return QStringLiteral("the application object (qtc_shutdown destroys it)");
    if (!qt_owned_class(x)) {
        Q_ASSERT(qt_owned_by_class(x).isEmpty());
        return QString();
    }
    return qt_owned_by_class(x);
}

QString qt_owned_by_class(QObject *x) {
    if (auto *t = qobject_cast<QThread *>(x)) {
        if (t == QCoreApplication::instance()->thread() || t == QThread::currentThread())
            return QStringLiteral("the QThread of the main thread");
        if (t->isRunning()) return QStringLiteral("a running thread; quit() and wait() for it first");
    }
    if (qobject_cast<QAbstractEventDispatcher *>(x)) return QStringLiteral("the event dispatcher of a thread");
    if (qobject_cast<QThreadPool *>(x) && x == QThreadPool::globalInstance()) return QStringLiteral("Qt's global thread pool");
#ifdef QTC_HAS_GUI
    if (qobject_cast<QScreen *>(x)) return QStringLiteral("a screen (screens belong to the windowing system)");
    if (qobject_cast<QStyleHints *>(x)) return QStringLiteral("the application's style hints");
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    if (qobject_cast<QAccessibilityHints *>(x)) return QStringLiteral("the application's accessibility hints");
#endif
    if (qobject_cast<QInputMethod *>(x)) return QStringLiteral("the application's input method");
    if (qobject_cast<QClipboard *>(x)) return QStringLiteral("the application's clipboard");
    if (auto *m = qobject_cast<QMimeData *>(x)) {
        if (QClipboard *cb = QGuiApplication::clipboard())
            for (QClipboard::Mode mode : {QClipboard::Clipboard, QClipboard::Selection, QClipboard::FindBuffer})
                if (cb->mimeData(mode) == m) return QStringLiteral("data of the clipboard");
    }
    if (x->inherits("QWidgetWindow")) return QStringLiteral("the native window of a widget");
    if (x->inherits("QQuickWidgetOffscreenWindow")) return QStringLiteral("the window inside a QQuickWidget");
    if (qobject_cast<QTextObject *>(x) && static_cast<QTextObject *>(x)->document())
        return QStringLiteral("a frame, table or list of a document; remove its text instead");
    if (auto *l = qobject_cast<QAbstractTextDocumentLayout *>(x))
        if (QTextDocument *d = l->document(); d && d->documentLayout() == l)
            return QStringLiteral("the layout of a document; replace it with setDocumentLayout()");
#endif
#ifdef QTC_HAS_WIDGETS
    if (auto *st = qobject_cast<QStyle *>(x); st && qobject_cast<QApplication *>(QCoreApplication::instance())) {
        for (QStyle *s = QApplication::style(); s; ) {
            if (s == st) return QStringLiteral("the application style");
            auto *p = qobject_cast<QProxyStyle *>(s);
            s = p ? p->baseStyle() : nullptr;
            if (s == p) break;
        }
    }
#endif
    for (const ModuleExt *e : module_exts())
        if (e->qt_owned_by_class)
            if (QString why = e->qt_owned_by_class(x); !why.isEmpty()) return why;
    return QString();
}

// 4. Parts of Qt objects that cannot be deleted while their owner lives (empty: may be deleted).
QString internal_part(QObject *x) {
    QObject *p = x->parent();
    // the child of a private implementation object, whoever made it (QGeoAreaMonitorPollingPrivate
    // takes over the position source all polling area monitors share)
    if (p && QByteArray(p->metaObject()->className()).endsWith("Private"))
        return QStringLiteral("an internal object of %1").arg(label(p));
    if (made_by_qtc(x)) return QString();
#ifdef QTC_HAS_WIDGETS
    if (qobject_cast<QTabBar *>(x) && qobject_cast<QTabWidget *>(p)) return QStringLiteral("the tab bar of a QTabWidget");
    if (qobject_cast<QSplitterHandle *>(x) && qobject_cast<QSplitter *>(p)) return QStringLiteral("a handle of a QSplitter");
    // a widget of a class qtc does not bind, inside another widget: a part Qt made for itself,
    // maybe long after construction (QColumnView makes a new preview column for each preview
    // widget, and keeps a plain pointer to it)
    if (x->isWidgetType() && p && p->isWidgetType() && !find_class(QByteArray(x->metaObject()->className())))
        return QStringLiteral("an internal part (%1) of %2").arg(label(x), label(p));
#endif
    auto pit = parts_map().constFind(x);
    if (pit != parts_map().constEnd() && pit->self == x && pit->root && pit->root != x && inside(x, pit->root))
        return QStringLiteral("an internal part (%1) of %2").arg(label(x), label(pit->root));
    if (p && x->objectName().startsWith(QLatin1StringView("qt_")))
        return QStringLiteral("an internal part (%1) of %2").arg(x->objectName(), label(p));
    return QString();
}

#ifdef QTC_HAS_WIDGETS
bool holds(const QLayout *l, const QObject *w, int depth) {
    if (depth > 64) return false;
    for (int i = 0; i < l->count(); i++) {
        QLayoutItem *it = l->itemAt(i);
        if (!it) break;
        if (static_cast<const QObject *>(it->widget()) == w) return true;
        if (const QLayout *sub = it->layout())
            if (holds(sub, w, depth + 1)) return true;
    }
    return false;
}

void drop_widget(QLayout *l, const QObject *w, int depth) {
    if (depth > 64) return;
    for (int i = 0; i < l->count();) {
        QLayoutItem *it = l->itemAt(i);
        if (!it) break;
        // the item still points at the widget, or Qt already cleared it: either way it is dead
        bool dead = static_cast<const QObject *>(it->widget()) == w ||
                    (!it->widget() && !it->layout() && !it->spacerItem() && dynamic_cast<QWidgetItem *>(it));
        if (dead) {
            delete l->takeAt(i);
            continue;
        }
        if (QLayout *sub = it->layout()) drop_widget(sub, w, depth + 1);
        i++;
    }
}

// a layout without a parent widget holds the widget
bool in_loose_layout(const QObject *w) {
    for (QObject *lo : live(layouts_map())) {
        auto *l = qobject_cast<QLayout *>(lo);
        if (l && !l->parentWidget() && holds(l, w, 0)) return true;
    }
    return false;
}

// A widget leaves its layout when it moves to another parent, but Qt sees that only through
// the layout's parent widget: a layout without one would keep the widget wherever it went, and
// delete it there (QFormLayout::removeRow) or take it back (once the layout gets a parent
// widget). qtc watches the widgets it puts into such layouts and takes them out when they move.
// (A layout given a parent widget is no longer one of these when its widgets move into it.)
class LooseLayoutWatch : public QObject {
public:
    bool eventFilter(QObject *o, QEvent *e) override {
        if (e->type() == QEvent::ParentChange && o->isWidgetType())
            for (QObject *lo : live(layouts_map())) {
                auto *l = qobject_cast<QLayout *>(lo);
                if (l && !l->parentWidget() && holds(l, o, 0)) drop_widget(l, o, 0);
            }
        return false;
    }
};
QPointer<LooseLayoutWatch> &loose_watch() {
    static QPointer<LooseLayoutWatch> *s = new QPointer<LooseLayoutWatch>;
    return *s;
}
#endif

}  // namespace

uint64_t roles_epoch = 1;

bool roles_is_owner(const QObject *o) { return owners_map().contains(const_cast<QObject *>(o)); }
// Per class: whether objects of o's class can ever hold others (window containers become holders
// one by one, as createWindowContainer makes them, so the class counts).
bool roles_may_own(QObject *o) { return kinds_of(o) != 0 || o->inherits("QWindowContainer"); }

void roles_note_call(const QObject *o) {
    auto *x = const_cast<QObject *>(o);
    if (owners_map().contains(x)) dirty_owners().insert(x);
}

void roles_register(QObject *o) {
    const uint64_t k = kinds_of(o);
    if (k) {
        Owners &m = owners_map();
        auto it = m.find(o);
        if (it != m.end()) unindex(o, *it);   // an earlier object at this address
        m.insert(o, Owner{o, k, {}});
        dirty_owners().insert(o);
    }
#ifdef QTC_HAS_WIDGETS
    if (k & OK_LAYOUT) layouts_map().insert(o, o);           // qobject_cast<QLayout *>
    if (k & OK_FOCUSFRAME) frames_map().insert(o, o);        // qobject_cast<QFocusFrame *>
#endif
}

void roles_note_container(QObject *container, QObject *window) {
    if (!container || !window) return;
    container_windows().insert(container, window);
    if (auto it = owners_map().find(container); it != owners_map().end()) it->kinds |= OK_WINDOWCONTAINER;
    else roles_register(container);
    dirty_owners().insert(container);
}

void roles_forget(QObject *o, bool readable) {
    container_windows().remove(o);
    if (auto it = owners_map().find(o); it != owners_map().end()) {
        unindex(o, *it);
        owners_map().erase(it);
    }
    dirty_owners().remove(o);
    layouts_map().remove(o);
    frames_map().remove(o);
#ifdef QTC_HAS_WIDGETS
    // o is being destroyed (or is gone): layouts without a parent widget must let go of it now.
    // Only widgets sit in layouts; o's flags can be read while it is destroyed, not once it is gone.
    if (layouts_map().isEmpty() || (readable && !widget_flag(o))) return;
    for (auto it = layouts_map().begin(); it != layouts_map().end();) {
        if (!it.value()) { it = layouts_map().erase(it); continue; }
        auto *l = qobject_cast<QLayout *>(it.value().data());
        ++it;
        if (l && !l->parentWidget()) drop_widget(l, o, 0);
    }
#endif
}

#ifdef QTC_HAS_WIDGETS
namespace {
// widgets given a style by qtc, by style; Qt deletes some styles itself (the application style
// replaced by another, a proxy's base style), so the style is kept by QPointer too
struct StyleUse {
    QPointer<QObject> style;
    QList<QPointer<QWidget>> users;
};
QHash<QObject *, StyleUse> &style_users();
}
#endif

void roles_drop_delete_watch();

void roles_clear() {
    owners_map().clear();
    container_windows().clear();
    made_pins().clear();
    held_index().clear();
    dirty_owners().clear();
#ifdef QTC_HAS_WIDGETS
    style_users().clear();
#endif
    layouts_map().clear();
    frames_map().clear();
#ifdef QTC_HAS_WIDGETS
    delete loose_watch().data();
#endif
    roles_drop_delete_watch();
    parts_map().clear();
    g_parts_swept = 0;
}

void roles_constructed(QObject *root) {
    const QList<QObject *> kids = root->findChildren<QObject *>();
    if (kids.isEmpty()) return;
    QHash<QObject *, Part> &m = parts_map();
    for (QObject *k : kids) m.insert(k, Part{k, root});
    // forget parts that died: keeps the table proportional to the live ones
    if (m.size() > 2 * g_parts_swept + 4096) {
        for (auto it = m.begin(); it != m.end();) {
            if (!it->self || !it->root) it = m.erase(it);
            else ++it;
        }
        g_parts_swept = m.size();
    }
}

namespace {
#ifdef QTC_HAS_WIDGETS
// Widgets given a style with QWidget::setStyle: the widget does not own it, and with a style
// sheet Qt wraps it in a style that keeps a plain pointer to it. Before such a style is
// destroyed its widgets go back to the application style.
QHash<QObject *, StyleUse> &style_users() {
    static QHash<QObject *, StyleUse> *s = new QHash<QObject *, StyleUse>;
    return *s;
}

void release_style_users(QObject *victim) {
    QHash<QObject *, StyleUse> &m = style_users();
    if (m.isEmpty()) return;
    for (auto it = m.begin(); it != m.end();) {
        if (!it->style) { it = m.erase(it); continue; }   // Qt deleted the style itself
        if (!inside(it->style.data(), victim)) { ++it; continue; }
        const QList<QPointer<QWidget>> users = it->users;
        const QObject *style = it.key();
        it = m.erase(it);
        for (const QPointer<QWidget> &w : users)
            if (w && w->style() != nullptr && (w->style() == style || w->testAttribute(Qt::WA_SetStyle)))
                w->setStyle(nullptr);
    }
}
#endif
}  // namespace

bool style_unshared(const QObject *style) {
#ifdef QTC_HAS_WIDGETS
    if (!style) return true;
    const auto it = style_users().constFind(const_cast<QObject *>(style));
    if (it == style_users().constEnd() || it->style != style) return true;   // none, or a dead style's
    for (const QPointer<QWidget> &w : it->users)
        if (w) return false;
#else
    (void)style;
#endif
    return true;
}

bool in_no_scroll_area(const QObject *widget, const QObject *except) {
#ifdef QTC_HAS_WIDGETS
    if (!widget) return true;
    const QWidgetList all = QApplication::allWidgets();
    for (QWidget *x : all)
        if (auto *a = qobject_cast<QScrollArea *>(x); a && a != except && a->widget() == widget) return false;
#else
    (void)widget;
    (void)except;
#endif
    return true;
}

bool scene_style_swappable(const QObject *scene) {
#ifdef QTC_HAS_WIDGETS
    auto *s = qobject_cast<const QGraphicsScene *>(scene);
    if (!s) return true;
    const QStyle *own = s->style();
    if (!own || own->parent() != s) return true;   // nothing of its own to delete
    const QList<QGraphicsItem *> items = s->items();
    for (const QGraphicsItem *i : items)
        if (i->isWidget() && static_cast<const QGraphicsWidget *>(i)->inherits("QGraphicsProxyWidget")) return false;
#else
    (void)scene;
#endif
    return true;
}

bool note_style_user(QObject *widget, QObject *style) {
#ifdef QTC_HAS_WIDGETS
    auto *w = qobject_cast<QWidget *>(widget);
    if (!w) return true;
    // the widget leaves the style it had for this one
    QHash<QObject *, StyleUse> &m = style_users();
    for (auto it = m.begin(); it != m.end();) {
        it->users.removeAll(w);
        it->users.removeAll(nullptr);
        if (it->users.isEmpty() || !it->style) it = m.erase(it);
        else ++it;
    }
    if (style) {
        StyleUse &u = m[style];
        if (u.style != style) {   // new, or left by a dead style at the same address
            u.style = style;
            u.users.clear();
        }
        u.users.append(w);
    }
#else
    (void)widget;
    (void)style;
#endif
    return true;
}

QString made_pin_reason(QObject *victim);

namespace {
#ifdef QTC_HAS_WIDGETS
// QWidget::destroy is protected
struct WidgetAccess : QWidget {
    static void destroy_window(QWidget *w) { (w->*&WidgetAccess::destroy)(true, false); }
};

// A widget changed parent (QEvent::ParentChange reaches the application's filters before the
// widget's own). Two Qt bugs follow from such a change.
void widget_parent_changed(QWidget *w) {
    // A focus frame follows its widget into a new parent by mapping from that parent, and has no
    // case for a widget left without one: Qt takes widgets out itself (QWidgetAction::
    // releaseWidget when a tool bar or menu lets an action go, also while being destroyed), so a
    // frame lets go of a widget that became a window before it sees the change. A frame being
    // destroyed is a plain QWidget by then (it no longer filters events) and is left alone.
    if (!w->parentWidget() && !frames_map().isEmpty()) {
        for (QObject *o : live(frames_map()))
            if (auto *f = qobject_cast<QFocusFrame *>(o); f && f->widget() == w) f->setWidget(nullptr);
    }
    // A window keeps its native window when it is given new flags or no parent (Qt makes it
    // again when the window is shown), and Qt destroys the native window of a window that goes
    // into a parent only if it is made: one that went into a parent without being shown in
    // between stays a plain child with a native window. Qt then takes it for the native parent
    // of what goes into it, and making that over for a QOpenGLWidget leaves none
    // (QWidget::setParent, crash). Such a native window goes now, as Qt lets go of one: windows
    // inside it move to the next native parent first.
    if (!w->isWindow() && !w->testAttribute(Qt::WA_NativeWindow) && !w->testAttribute(Qt::WA_WState_Created)) {
        if (QWindow *stale = w->windowHandle()) {
            QWidget *p = w->parentWidget();
            while (p && !p->windowHandle()) p = p->parentWidget();
            QWindow *to = p ? p->windowHandle() : nullptr;
            const QObjectList kids = stale->children();
            for (QObject *c : kids)
                if (auto *cw = qobject_cast<QWindow *>(c)) cw->setParent(to);
            WidgetAccess::destroy_window(w);
        }
    }
}
#endif

// Qt deletes objects itself too: a widget or graphics widget closed with Qt::WA_DeleteOnClose, a
// closed MDI subwindow, a message box's old check box. Those deletions are deferred, so before
// each one the owners that hold the object (or something inside it) by a plain pointer let go of
// it, as they do before qtc_destroy. A release may delete the object itself (setViewport deletes
// the old viewport): then the event is consumed, there is nothing left to delete.
class DeferredDeleteWatch : public QObject {
public:
    bool eventFilter(QObject *o, QEvent *e) override {
        // A window larger than the allocation limit is not painted: its backing store (or
        // software render target) would be allocated at its size, and Qt paints on with no
        // paint engine when that fails. qtc refuses such sizes when they are asked for; a
        // window can still grow there through its layout.
        if (e->type() == QEvent::UpdateRequest || e->type() == QEvent::Expose) {
#ifdef QTC_HAS_WIDGETS
            if (o->isWidgetType()) {
                auto *w = static_cast<QWidget *>(o);
                if (!w->isWindow()) return false;
                if (!window_size_ok(w, w->width(), w->height())) return true;
                // Qt paints a widget with a graphics effect without checking that the backing
                // store has an image (it checks for the others): a store that kept no image at
                // the window's size is not painted into. Only an exposed window with a platform
                // window has a store to ask: on Wayland, asking the store of a window whose
                // platform window is gone (a closed window, a dismissed or refused popup)
                // crashes in the platform plugin, and those still get Expose and UpdateRequest.
                QWindow *wh = w->windowHandle();
                if (!wh || !wh->handle() || !wh->isExposed()) return false;
                if (QBackingStore *bs = w->backingStore(); bs && bs->size() == w->size() && !w->size().isEmpty()) {
                    QPaintDevice *pd = bs->paintDevice();
                    if (pd && pd->devType() == QInternal::Image && static_cast<QImage *>(pd)->isNull()) return true;
                }
                return false;
            }
#endif
#ifdef QTC_HAS_GUI
            if (o->isWindowType()) {
                auto *w = static_cast<QWindow *>(o);
                return !qwindow_size_ok(w, w->width(), w->height());
            }
#endif
            return false;
        }
#ifdef QTC_HAS_WIDGETS
        // A layout whose sizes add up past what Qt's layout engine counts in int is not laid
        // out (its space distribution would not finish). qtc refuses the values that get there
        // one by one, and refuses show() and setGeometry() on such a layout; resizes and layout
        // requests reach it here.
        if (e->type() == QEvent::LayoutRequest || e->type() == QEvent::Resize) {
            if (!o->isWidgetType()) return false;
            QLayout *l = static_cast<QWidget *>(o)->layout();
            return l && !layout_sums_ok(l);
        }
        if (e->type() == QEvent::ParentChange) {
            if (o->isWidgetType()) widget_parent_changed(static_cast<QWidget *>(o));
            return false;
        }
#endif
        if (e->type() != QEvent::DeferredDelete || owners_map().isEmpty()) return false;
        QPointer<QObject> v(o);
        QList<QPair<QPointer<QObject>, uint64_t>> holders;
        for (const auto &[h, kinds] : candidates(o)) {
            if (h == o || inside(h, o)) continue;
            if (visit(h, kinds, o, false) == RELEASABLE) holders.append({h, kinds});
        }
        for (const auto &[h, kinds] : holders) {
            if (h && v) visit(h, kinds, o, true);
            if (h) dirty_owners().insert(h.data());
        }
#ifdef QTC_HAS_WIDGETS
        if (v) release_style_users(o);
#endif
        return !v;
    }
};
QPointer<DeferredDeleteWatch> &delete_watch() {
    static QPointer<DeferredDeleteWatch> *s = new QPointer<DeferredDeleteWatch>;
    return *s;
}
}  // namespace

void roles_drop_delete_watch() { delete delete_watch().data(); }

void roles_init() {
    if (!delete_watch() && QCoreApplication::instance()) {
        delete_watch() = new DeferredDeleteWatch;
        QCoreApplication::instance()->installEventFilter(delete_watch().data());
    }
}

#ifdef QTC_HAS_WIDGETS
namespace {
// An item view's index widgets (setIndexWidget, QTableWidget::setCellWidget, QTreeWidget::
// setItemWidget, persistent editors) are children of its viewport. Qt keeps them by pointer and
// shows and places them on each layout: one deleted another way leaves the view reading through
// a null pointer, one moved elsewhere is still placed in the view.
QAbstractItemView *index_widget_view(QObject *x) {
    if (!x->isWidgetType()) return nullptr;
    QWidget *p = static_cast<QWidget *>(x)->parentWidget();
    if (!p) return nullptr;
    auto *v = qobject_cast<QAbstractItemView *>(p->parentWidget());
    return v && v->viewport() == p ? v : nullptr;
}
// the index whose widget w is (invalid when none); looked up where the widget sits first, then
// over the model (up to a bound: beyond it the widget counts as one, to be safe)
QModelIndex index_of_widget(QAbstractItemView *v, QWidget *w, bool *unsure = nullptr) {
    QModelIndex i = v->indexAt(w->geometry().center());
    if (i.isValid() && v->indexWidget(i) == w) return i;
    QAbstractItemModel *m = v->model();
    if (!m) return {};
    qint64 budget = 200000;
    std::function<QModelIndex(const QModelIndex &)> walk = [&](const QModelIndex &parent) -> QModelIndex {
        const int rows = m->rowCount(parent), cols = m->columnCount(parent);
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < cols; c++) {
                if (--budget < 0) return {};
                const QModelIndex k = m->index(r, c, parent);
                if (v->indexWidget(k) == w) return k;
                if (c == 0 && m->hasChildren(k))
                    if (QModelIndex f = walk(k); f.isValid() || budget < 0) return f;
            }
        return {};
    };
    QModelIndex f = walk(v->rootIndex());
    if (unsure) *unsure = !f.isValid() && budget < 0;
    return f;
}
}  // namespace
#endif

QString roles_prepare_destroy(QObject *victim) {
    if (QString why = qt_owned(victim); !why.isEmpty()) return why;
    if (QString why = made_pin_reason(victim); !why.isEmpty()) return why;
#ifdef QTC_HAS_WIDGETS
    // an index widget: its view lets go of it first (and deletes it later itself)
    if (QAbstractItemView *v = index_widget_view(victim))
        if (QModelIndex i = index_of_widget(v, static_cast<QWidget *>(victim)); i.isValid())
            v->setIndexWidget(i, nullptr);
#endif
    QList<QPair<QPointer<QObject>, uint64_t>> holders;
    for (const auto &[o, kinds] : candidates(victim)) {
        if (o == victim || inside(o, victim)) continue;
        int st = visit(o, kinds, victim, false);
        if (st == PINNED) return QStringLiteral("used by %1, which cannot let go of it; destroy that first").arg(label(o));
        if (st == RELEASABLE) holders.append({o, kinds});
    }
    if (holders.isEmpty()) {
        QString why = internal_part(victim);
#ifdef QTC_HAS_WIDGETS
        if (why.isEmpty()) release_style_users(victim);
#endif
        return why;
    }
#ifdef QTC_HAS_WIDGETS
    release_style_users(victim);
#endif
    QPointer<QObject> v(victim);
    for (const auto &[o, kinds] : holders) {
        if (o && v) visit(o, kinds, victim, true);
        if (o) dirty_owners().insert(o.data());   // it holds something new now (a fresh viewport, ...)
    }
    if (!v) return QString();   // releasing it deleted it
    // an owner that went back to a built-in object it cannot do without still holds it
    for (const auto &[o, kinds] : holders)
        if (o && visit(o, kinds, victim, false) != NOT_HELD)
            return QStringLiteral("a built-in part of %1").arg(label(o));
    return QString();
}

// Roles that break when the held object moves elsewhere (widgets placed inside their owner,
// chart sets the series deletes): such objects stay where they are.
constexpr uint64_t kPlaceKinds = OK_SCROLLAREA | OK_MESSAGEBOX | OK_PROGRESS | OK_WIZARD | OK_COMBO | OK_COMPLETER |
                                 OK_TABBAR | OK_TABWIDGET | OK_LAYOUT | OK_CHARTSERIES | OK_AREA | OK_GEO | OK_FOCUSFRAME |
                                 OK_WINDOWCONTAINER | OK_BUTTONBOX;


QObject *roles_holder(QObject *x) {
#ifdef QTC_HAS_WIDGETS
    if (QAbstractItemView *v = index_widget_view(x)) {
        bool unsure = false;
        if (index_of_widget(v, static_cast<QWidget *>(x), &unsure).isValid() || unsure) return v;
    }
    // a widget embedded in a graphics scene: its proxy keeps it wherever it moves (Qt does not
    // look at its parent changes) and deletes it with itself, also when the scene is cleared
    if (x->isWidgetType())
        if (QGraphicsProxyWidget *px = static_cast<QWidget *>(x)->graphicsProxyWidget()) return px;
#endif
    for (const auto &[o, kinds] : candidates(x))
        if ((kinds & kPlaceKinds) && o != x && !inside(o, x) && visit(o, kinds & kPlaceKinds, x, false) != NOT_HELD) return o;
    return nullptr;
}

// qtc_move_to_thread. Qt classes of these modules may live in a worker thread; the others (GUI,
// widgets, QML, multimedia, SQL connections, positioning backends, qtc's own) belong to the GUI
// thread. Classes qtc does not bind (the program's own) count by the Qt class they derive from.
static bool movable_module(const char *m) {
    static const char *const ok[] = {"Core", "Network", "WebSockets", "Mqtt", "Coap", "SerialPort", "NetworkAuth", "HttpServer"};
    for (const char *x : ok)
        if (std::strcmp(m, x) == 0) return true;
    return false;
}

QString roles_movable(QObject *root) {
    QList<QObject *> tree = root->findChildren<QObject *>();
    tree.prepend(root);
    for (QObject *x : tree) {
        if (QString why = qt_owned(x); !why.isEmpty()) return QStringLiteral("%1 is %2").arg(label(x), why);
        if (x->isWidgetType() || x->isWindowType()) return QStringLiteral("%1 is a widget or a window").arg(label(x));
        for (const ModuleExt *e : module_exts())
            if (e->not_movable)
                if (QString why = e->not_movable(x); !why.isEmpty()) return QStringLiteral("%1 %2").arg(label(x), why);
        if (qobject_cast<QThread *>(x) || qobject_cast<QThreadPool *>(x) || qobject_cast<QAbstractItemModel *>(x) ||
            qobject_cast<QAbstractAnimation *>(x) || qobject_cast<QTranslator *>(x) || qobject_cast<QMimeData *>(x) ||
            x->inherits("QNetworkInformation"))
            return QStringLiteral("%1 stays in the GUI thread").arg(label(x));
        const ClassDef *c = bound_class_of(x);
        if (!c || !movable_module(c->module))
            return QStringLiteral("%1 (Qt %2) stays in the GUI thread").arg(label(x), QLatin1StringView(c ? c->module : "?"));
        if (QObject *b = bound_parent(x); b && !inside(b, root)) return QStringLiteral("%1 belongs to %2").arg(label(x), label(b));
        // what it holds must go with it ...
        if (uint64_t k = kinds_of(x)) {
            QList<QObject *> held;
            visit(x, k, nullptr, false, &held);
            for (QObject *h : held)
                if (h && !inside(h, root)) return QStringLiteral("%1 uses %2, which stays in the GUI thread").arg(label(x), label(h));
        }
        // ... and nothing that stays may hold it
        for (const auto &[o, kinds] : candidates(x)) {
            if (o == x || inside(o, root)) continue;
            if (visit(o, kinds, x, false) != NOT_HELD) return QStringLiteral("%1 is used by %2").arg(label(x), label(o));
        }
    }
    for (const MadePin &p : made_pins()) {
        if (!p.user || !p.used) continue;
        if (inside(p.user, root) != inside(p.used, root))
            return QStringLiteral("%1 was made with %2 and uses it while it lives").arg(label(p.user), label(p.used));
    }
    return QString();
}

void roles_forget_tree(QObject *root) {
    QList<QObject *> tree = root->findChildren<QObject *>();
    tree.prepend(root);
    for (QObject *x : tree) {
        roles_forget(x, true);
        parts_map().remove(x);
    }
    QList<MadePin> &pins = made_pins();
    for (qsizetype i = 0; i < pins.size();) {
        if (!pins[i].user || !pins[i].used || inside(pins[i].user, root) || inside(pins[i].used, root)) pins.removeAt(i);
        else i++;
    }
    roles_epoch++;   // the held-object index looks again
}

void roles_register_tree(QObject *root) {
    QList<QObject *> tree = root->findChildren<QObject *>();
    tree.prepend(root);
    for (QObject *x : tree)
        if (reg().known_handle(x)) roles_register(x);
    roles_epoch++;
}

// Also when the receiver is the holder: Qt deletes the old object before taking the new one in
// several setters (QComboBox::setView), so handing an owner what it holds already destroys it.
// an object that keeps a plain pointer to the one it was made in, and so must stay inside it
QObject *bound_parent(const QObject *o) {
#ifdef QTC_HAS_WIDGETS
    if (auto *h = qobject_cast<const QSplitterHandle *>(o)) return h->splitter();
#else
    (void)o;
#endif
    return nullptr;
}

bool not_held(const QObject *receiver, const QObject *arg) {
    if (!arg) return true;
    if (roles_holder(const_cast<QObject *>(arg))) return false;
    // not even into its own: QSplitter::replaceWidget would then take it out of the splitter
    if (bound_parent(arg)) return false;
#ifdef QTC_HAS_WIDGETS
    // such an owner takes a widget in without always moving it, so a layout without a parent
    // widget could still delete it (see LooseLayoutWatch)
    auto it = owners_map().constFind(const_cast<QObject *>(receiver));
    if (it != owners_map().cend() && (it->kinds & kPlaceKinds & ~OK_LAYOUT) && arg->isWidgetType() && in_loose_layout(arg))
        return false;
#else
    (void)receiver;
#endif
    return true;
}

// A setter that deletes the part it replaces (QAbstractScrollArea::setViewport, QScrollArea::
// setWidget, QComboBox::setView ...): other owners that hold something in that part let go of
// it first, as before qtc_destroy (a QColumnView's preview widget sits in its preview column's
// viewport). False when one cannot.
bool part_free(const QObject *receiver, const QObject *old) {
    if (!old) return true;
    auto *x = const_cast<QObject *>(old);
    QList<QPair<QPointer<QObject>, uint64_t>> holders;
    for (const auto &[o, kinds] : candidates(x)) {
        if (o == receiver || o == x || inside(o, x)) continue;
        int st = visit(o, kinds, x, false);
        if (st == PINNED) return false;
        if (st == RELEASABLE) holders.append({o, kinds});
    }
    const QPointer<QObject> v(x);
    for (const auto &[o, kinds] : holders) {
        if (o && v) visit(o, kinds, x, true);
        if (o) dirty_owners().insert(o.data());
    }
    if (!v) return false;   // a release took the part along: the receiver must not delete it again
    for (const auto &[o, kinds] : holders)
        if (o && visit(o, kinds, x, false) != NOT_HELD) return false;
    return true;
}


bool free_for(const QObject *receiver, const QObject *arg) {
    if (!arg) return true;
    auto *x = const_cast<QObject *>(arg);
    if (!qt_owned(x).isEmpty()) return false;
    for (const auto &[o, kinds] : candidates(x))
        if (o != receiver && o != x && !inside(o, x) && visit(o, kinds, x, false) != NOT_HELD) return false;
    return true;
}

// A QFileDialog sets its proxy model's source to its own file system model, and its completer
// takes whatever source the proxy has for one (static_cast): the source stays.
bool proxy_source_free(const QObject *proxy) {
#ifdef QTC_HAS_WIDGETS
    for (const auto &[o, kinds] : candidates(const_cast<QObject *>(proxy)))
        if ((kinds & OK_FILEDIALOG) && static_cast<QFileDialog *>(o)->proxyModel() == proxy) return false;
#else
    (void)proxy;
#endif
    return true;
}



bool doc_layout_ok(const QObject *doc, const QObject *layout) {
#ifdef QTC_HAS_WIDGETS
    // QPlainTextEdit works only with a QPlainTextDocumentLayout
    for (const auto &[o, kinds] : candidates(const_cast<QObject *>(doc))) {
        auto *e = (kinds & OK_SCROLLAREA) ? qobject_cast<QPlainTextEdit *>(o) : nullptr;
        if (e && e->document() == doc && !qobject_cast<const QPlainTextDocumentLayout *>(layout)) return false;
    }
#else
    (void)doc;
    (void)layout;
#endif
    return true;
}


// Objects that keep a plain pointer to an object they were made with, and have no getter
// to check it later, become its children: they go with it instead of dangling. Those that
// cannot be its children pin it instead: it cannot be destroyed before them.
QString made_pin_reason(QObject *victim) {
    QList<MadePin> &l = made_pins();
    for (qsizetype i = 0; i < l.size();) {
        if (!l[i].user || !l[i].used) { l.removeAt(i); continue; }
        if (l[i].user != victim && !inside(l[i].user, victim) && inside(l[i].used, victim))
            return QStringLiteral("used by %1, which cannot let go of it; destroy that first").arg(label(l[i].user));
        i++;
    }
    return QString();
}

void roles_adopt_new(QObject *o, const QList<QObject *> &made_with) {
    for (const ModuleExt *e : module_exts())
        if (e->adopt_new) e->adopt_new(o, made_with);
}

bool layout_accepts(const QObject *layout, const QObject *widget) {
#ifdef QTC_HAS_WIDGETS
    auto *l = qobject_cast<const QLayout *>(layout);
    if (!l || !widget) return true;
    if (l->parentWidget()) return true;   // Qt moves the widget out of its old place
    if (holds(l, widget, 0)) return false;
    for (QObject *lo : live(layouts_map())) {
        auto *x = qobject_cast<QLayout *>(lo);
        if (x && x != l && !x->parentWidget() && holds(x, widget, 0)) return false;
    }
    // it goes in: take it out again when it moves elsewhere
    if (!loose_watch()) loose_watch() = new LooseLayoutWatch;
    const_cast<QObject *>(widget)->installEventFilter(loose_watch().data());
#else
    (void)layout;
    (void)widget;
#endif
    return true;
}

// A module may need an object alive a while longer than qtc_destroy (a QML component still
// loading): its handle is gone at once, the object goes later.
bool roles_defer_delete(QObject *o) {
    for (const ModuleExt *e : module_exts())
        if (e->defer_delete && e->defer_delete(o)) return true;
    return false;
}

// ------------------------------------------------------------ for the modules (qtc_module.h)

static QList<const ModuleExt *> &exts() {
    static auto *l = new QList<const ModuleExt *>;
    return *l;
}
void register_module_ext(const ModuleExt *e) {
    if (!exts().contains(e)) exts().append(e);
    // what objects of a class can hold depends on the modules loaded: worked out again
    kinds_memo().clear();
    typed_memo().clear();
    owned_memo().clear();
}
const QList<const ModuleExt *> &module_exts() { return exts(); }

bool roles_inside(const QObject *o, const QObject *root) { return inside(o, root); }
QString roles_label(const QObject *o) { return label(o); }
bool roles_made_by_qtc(const QObject *o) { return made_by_qtc(o); }
QList<QPair<QObject *, uint64_t>> roles_candidates(QObject *x) { return candidates(x); }
void roles_pin_made(QObject *user, QObject *used) { made_pins().append(MadePin{user, used}); }

}  // namespace qtc

namespace qtc {
// Qt's NMEA sources read their device for as long as it says bytes are available, and a device
// open without reading says so while every read fails: the simulation reader spins forever. A
// device an NMEA source reads is reopened for reading only.
bool nmea_open_ok(const QObject *device, int mode) {
    if (mode & QIODevice::ReadOnly) return true;
    return !(g_module_hooks.nmea_reads && g_module_hooks.nmea_reads(device));
}
}  // namespace qtc
