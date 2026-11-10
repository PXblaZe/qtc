// QtcCanvas, QtcBridge and the qtc_draw_* API.
#include "qtc_builtin.h"
#include "qtc_events.h"

#ifdef QTC_HAS_GUI
#include <QtGui/QLinearGradient>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QTextTable>
#include <QtGui/QTransform>
#include <QtCore/QLineF>
#endif
#ifdef QTC_HAS_WIDGETS
#include <QtWidgets/QApplication>
#endif

namespace qtc {

#ifdef QTC_HAS_GUI
enum DrawOp : uint8_t {
    OP_CLEAR = 1, OP_PEN, OP_NOPEN, OP_BRUSH, OP_NOBRUSH, OP_LINGRAD, OP_FONT, OP_AA, OP_OPACITY, OP_SAVE,
    OP_RESTORE, OP_TRANSLATE, OP_ROTATE, OP_SCALE, OP_CLIP, OP_POINT, OP_LINE, OP_RECT, OP_RRECT, OP_ELLIPSE,
    OP_ARC, OP_PIE, OP_POLYLINE, OP_POLYGON, OP_PATH, OP_TEXT, OP_TEXTIN, OP_IMAGE, OP_PIXMAP
};

static QColor argb(uint32_t c) { return QColor::fromRgba(QRgb(c)); }

void replay(QPainter &p, const DrawList &l, const QRect &bounds) {
    for (const DrawCmd &c : l.cmds) {
        const double *a = c.a;
        switch (c.op) {
        case OP_CLEAR: {
            p.save();
            p.resetTransform();
            p.setClipping(false);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.fillRect(bounds, argb(c.c1));
            p.restore();
            break;
        }
        case OP_PEN: {
            QPen pen(argb(c.c1));
            pen.setWidthF(a[0]);
            pen.setStyle(Qt::PenStyle(int(a[1])));
            pen.setCapStyle(Qt::PenCapStyle(int(a[2])));
            pen.setJoinStyle(Qt::PenJoinStyle(int(a[3])));
            p.setPen(pen);
            break;
        }
        case OP_NOPEN: p.setPen(Qt::NoPen); break;
        case OP_BRUSH: p.setBrush(QBrush(argb(c.c1), Qt::BrushStyle(int(a[0])))); break;
        case OP_NOBRUSH: p.setBrush(Qt::NoBrush); break;
        case OP_LINGRAD: {
            QLinearGradient g(a[0], a[1], a[2], a[3]);
            g.setColorAt(0, argb(c.c1));
            g.setColorAt(1, argb(c.c2));
            p.setBrush(QBrush(g));
            break;
        }
        case OP_FONT: p.setFont(l.fonts[size_t(c.i)]); break;
        case OP_AA:
            p.setRenderHint(QPainter::Antialiasing, a[0] != 0);
            p.setRenderHint(QPainter::TextAntialiasing, a[0] != 0);
            p.setRenderHint(QPainter::SmoothPixmapTransform, a[0] != 0);
            break;
        case OP_OPACITY: p.setOpacity(a[0]); break;
        case OP_SAVE: p.save(); break;
        case OP_RESTORE: p.restore(); break;
        case OP_TRANSLATE: p.translate(a[0], a[1]); break;
        case OP_ROTATE: p.rotate(a[0]); break;
        case OP_SCALE: p.scale(a[0], a[1]); break;
        case OP_CLIP: p.setClipRect(QRectF(a[0], a[1], a[2], a[3]), Qt::IntersectClip); break;
        case OP_POINT: p.drawPoint(QPointF(a[0], a[1])); break;
        case OP_LINE: p.drawLine(QPointF(a[0], a[1]), QPointF(a[2], a[3])); break;
        case OP_RECT: p.drawRect(QRectF(a[0], a[1], a[2], a[3])); break;
        case OP_RRECT: p.drawRoundedRect(QRectF(a[0], a[1], a[2], a[3]), a[4], a[5]); break;
        case OP_ELLIPSE: p.drawEllipse(QRectF(a[0], a[1], a[2], a[3])); break;
        case OP_ARC: p.drawArc(QRectF(a[0], a[1], a[2], a[3]), int(a[4] * 16), int(a[5] * 16)); break;
        case OP_PIE: p.drawPie(QRectF(a[0], a[1], a[2], a[3]), int(a[4] * 16), int(a[5] * 16)); break;
        case OP_POLYLINE: p.drawPolyline(l.points.data() + size_t(a[0]), int(a[1])); break;
        case OP_POLYGON: p.drawPolygon(l.points.data() + size_t(a[0]), int(a[1])); break;
        case OP_PATH: p.drawPath(l.paths[size_t(c.i)]); break;
        case OP_TEXT: p.drawText(QPointF(a[0], a[1]), l.texts[size_t(c.i)]); break;
        case OP_TEXTIN: p.drawText(QRectF(a[0], a[1], a[2], a[3]), int(a[4]), l.texts[size_t(c.i)]); break;
        case OP_IMAGE: p.drawImage(QRectF(a[0], a[1], a[2], a[3]), l.images[size_t(c.i)]); break;
        case OP_PIXMAP: {
            const QPixmap &pm = l.pixmaps[size_t(c.i)];
            p.drawPixmap(QRectF(a[0], a[1], a[2], a[3]), pm, QRectF(pm.rect()));
            break;
        }
        default: break;
        }
    }
}

// ------------------------------------------------------------ the recorder

struct Recorder {
    bool active = false;
    uint64_t target = 0;
    bool is_value = false;
    std::shared_ptr<DrawList> list;
    int depth = 0;
    QPainterPath path;
    bool path_open = false;
};
static Recorder &rec() { static Recorder *r = new Recorder; return *r; }

static constexpr double kMaxCoord = 1e9;

static bool finite_all(std::initializer_list<double> v) {
    for (double x : v)
        if (!std::isfinite(x) || std::fabs(x) > kMaxCoord) return false;
    return true;
}

static qtc_status need_rec(const char *what) {
    if (!rec().active) return fail(QTC_ERR_STATE, QStringLiteral("%1: call qtc_draw_begin first").arg(QLatin1StringView(what)));
    return QTC_OK;
}

static qtc_status add(uint8_t op, std::initializer_list<double> vals, const char *what, int32_t i = 0,
                      uint32_t c1 = 0, uint32_t c2 = 0) {
    if (qtc_status e = need_rec(what)) return e;
    if (!finite_all(vals)) return fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("%1: coordinates must be finite and within +/-1e9").arg(QLatin1StringView(what)));
    if (rec().list->cmds.size() >= 50'000'000) return fail(QTC_ERR_LIMIT, "too many drawing commands in one recording");
    DrawCmd c;
    c.op = op;
    c.i = i;
    c.c1 = c1;
    c.c2 = c2;
    int k = 0;
    for (double v : vals) c.a[k++] = v;
    rec().list->cmds.push_back(c);
    return QTC_OK;
}
#endif

}  // namespace qtc

#ifdef QTC_HAS_WIDGETS
QtcCanvas::QtcCanvas(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}
void QtcCanvas::setList(std::shared_ptr<const qtc::DrawList> list) {
    list_ = std::move(list);
    update();
}
int QtcCanvas::commandCount() const { return list_ ? int(list_->cmds.size()) : 0; }
void QtcCanvas::clearDrawing() {
    list_.reset();
    update();
}
void QtcCanvas::paintEvent(QPaintEvent *) {
    paints_++;
    if (!list_) return;
    QPainter p(this);
    qtc::replay(p, *list_, rect());
}
#endif

namespace qtc {

// ------------------------------------------------ registration of builtins

#ifdef QTC_HAS_WIDGETS
static const ParamDesc canvas_ctor_params[] = {{PK_OBJ, -1, "QWidget*", &QWidget::staticMetaObject}};
static int canvas_ctor(void *, Arg *a, int n, Ret *r) {
    QWidget *parent = nullptr;
    if (n >= 1) {
        H<QWidget *> h;
        if (int e = cv(a[0], h, 0)) return e;
        parent = h.get();
    }
    r->set_obj(new QtcCanvas(parent));
    return 0;
}
static const MethodDef canvas_ctors[] = {{"QtcCanvas", "QtcCanvas(QWidget*)", canvas_ctor, canvas_ctor_params, 1, 0, 0, QTC_T_OBJ, "QtcCanvas*"}};
#endif
static const ParamDesc bridge_ctor_params[] = {{PK_OBJ, -1, "QObject*", &QObject::staticMetaObject}};
static int bridge_ctor(void *, Arg *a, int n, Ret *r) {
    QObject *parent = nullptr;
    if (n >= 1) {
        H<QObject *> h;
        if (int e = cv(a[0], h, 0)) return e;
        parent = h.get();
    }
    r->set_obj(new QtcBridge(parent));
    return 0;
}
static const MethodDef bridge_ctors[] = {{"QtcBridge", "QtcBridge(QObject*)", bridge_ctor, bridge_ctor_params, 1, 0, 0, QTC_T_OBJ, "QtcBridge*"}};

static const ClassDef builtin_defs[] = {
#ifdef QTC_HAS_WIDGETS
    {"QtcCanvas", "qtc", CK_OBJECT, NEED_WIDGETS, 0, &QtcCanvas::staticMetaObject, nullptr, nullptr, nullptr, nullptr, 0, canvas_ctors, 1},
#endif
    {"QtcBridge", "qtc", CK_OBJECT, NEED_CORE, 0, &QtcBridge::staticMetaObject, nullptr, nullptr, nullptr, nullptr, 0, bridge_ctors, 1},
};

void register_builtin_classes() { register_classes(builtin_defs, int(sizeof(builtin_defs) / sizeof(builtin_defs[0]))); }

}  // namespace qtc

using namespace qtc;

// ------------------------------------------------------------------- C API

#ifdef QTC_HAS_GUI
#define DRAW_GUARD(body) return guarded<qtc_status>(QTC_ERR_STATE, [&]() -> qtc_status { body });

extern "C" {

QTC_API qtc_status qtc_draw_begin(uint64_t target) {
    DRAW_GUARD(
        Recorder &r = rec();
        if (r.active) return fail(QTC_ERR_STATE, "qtc_draw_begin: a recording is already open; call qtc_draw_end");
        if (app_kind() == QTC_APP_CORE) return fail(QTC_ERR_STATE, "drawing needs a GUI application");
        int k = reg().kind_of(target);
        qtc_status err = 0;
        if (k == SK_VAL) {
            QVariant *v = reg().value(target, &err);
            if (!v) return fail(err, "qtc_draw_begin");
            QMetaType mt = v->metaType();
            if (mt != QMetaType::fromType<QImage>() && mt != QMetaType::fromType<QPixmap>())
                return fail(QTC_ERR_WRONG_TYPE, QStringLiteral("qtc_draw_begin: a %1 value is not drawable (use QImage or QPixmap)").arg(QLatin1StringView(mt.name())));
            r.is_value = true;
        } else {
            QObject *o = reg().object(target, &err);
            if (!o) return fail(err, "qtc_draw_begin");
#ifdef QTC_HAS_WIDGETS
            if (!qobject_cast<QtcCanvas *>(o)) return fail(QTC_ERR_WRONG_TYPE, QStringLiteral("qtc_draw_begin: %1 is not a QtcCanvas").arg(QLatin1StringView(o->metaObject()->className())));
#else
            return fail(QTC_ERR_UNSUPPORTED, "QtcCanvas needs QtWidgets");
#endif
            r.is_value = false;
        }
        r.active = true;
        r.target = target;
        r.list = std::make_shared<DrawList>();
        r.depth = 0;
        r.path = QPainterPath();
        r.path_open = false;
        return QTC_OK;)
}

QTC_API qtc_status qtc_draw_end(void) {
    DRAW_GUARD(
        Recorder &r = rec();
        if (!r.active) return fail(QTC_ERR_STATE, "qtc_draw_end: no recording is open");
        r.active = false;
        while (r.depth > 0) {
            DrawCmd c;
            c.op = OP_RESTORE;
            r.list->cmds.push_back(c);
            r.depth--;
        }
        std::shared_ptr<DrawList> list = std::move(r.list);
        qtc_status err = 0;
        if (r.is_value) {
            QVariant *v = reg().value(r.target, &err);
            if (!v) return fail(err, "qtc_draw_end: the target");
            if (v->metaType() == QMetaType::fromType<QImage>()) {
                QImage *img = static_cast<QImage *>(v->data());
                if (img->isNull()) return fail(QTC_ERR_STATE, "qtc_draw_end: the image is empty");
                QPainter p;
                if (!p.begin(img)) return fail(QTC_ERR_REJECTED, "QPainter cannot draw on this image format");
                replay(p, *list, img->rect());
                p.end();
            } else {
                QPixmap *pm = static_cast<QPixmap *>(v->data());
                if (pm->isNull()) return fail(QTC_ERR_STATE, "qtc_draw_end: the pixmap is empty");
                QPainter p;
                if (!p.begin(pm)) return fail(QTC_ERR_REJECTED, "QPainter cannot draw on this pixmap");
                replay(p, *list, pm->rect());
                p.end();
            }
            return QTC_OK;
        }
#ifdef QTC_HAS_WIDGETS
        QObject *o = reg().object(r.target, &err);
        if (!o) return fail(err, "qtc_draw_end: the canvas");
        static_cast<QtcCanvas *>(o)->setList(std::move(list));
#endif
        return QTC_OK;)
}

QTC_API int64_t qtc_draw_count(void) {
    return guarded<int64_t>(0, []() -> int64_t { return rec().active ? int64_t(rec().list->cmds.size()) : 0; });
}

QTC_API qtc_status qtc_draw_clear(uint32_t c) { DRAW_GUARD(return add(OP_CLEAR, {}, "qtc_draw_clear", 0, c);) }
QTC_API qtc_status qtc_draw_pen(uint32_t c, double width, int32_t style, int32_t cap, int32_t join) {
    DRAW_GUARD(
        if (!std::isfinite(width) || width < 0 || width > 100000) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_draw_pen: width must be 0 .. 100000");
        if (style < Qt::NoPen || style > Qt::DashDotDotLine) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_draw_pen: style must be a Qt::PenStyle 0 .. 5");
        if (cap != Qt::FlatCap && cap != Qt::SquareCap && cap != Qt::RoundCap) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_draw_pen: cap must be a Qt::PenCapStyle");
        if (join != Qt::MiterJoin && join != Qt::BevelJoin && join != Qt::RoundJoin && join != Qt::SvgMiterJoin) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_draw_pen: join must be a Qt::PenJoinStyle");
        return add(OP_PEN, {width, double(style), double(cap), double(join)}, "qtc_draw_pen", 0, c);)
}
QTC_API qtc_status qtc_draw_no_pen(void) { DRAW_GUARD(return add(OP_NOPEN, {}, "qtc_draw_no_pen");) }
QTC_API qtc_status qtc_draw_brush(uint32_t c, int32_t style) {
    DRAW_GUARD(
        if (style < Qt::NoBrush || style > Qt::DiagCrossPattern) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_draw_brush: style must be a Qt::BrushStyle 0 .. 14");
        return add(OP_BRUSH, {double(style)}, "qtc_draw_brush", 0, c);)
}
QTC_API qtc_status qtc_draw_no_brush(void) { DRAW_GUARD(return add(OP_NOBRUSH, {}, "qtc_draw_no_brush");) }
QTC_API qtc_status qtc_draw_linear_gradient(double x1, double y1, double x2, double y2, uint32_t c1, uint32_t c2) {
    DRAW_GUARD(return add(OP_LINGRAD, {x1, y1, x2, y2}, "qtc_draw_linear_gradient", 0, c1, c2);)
}
QTC_API qtc_status qtc_draw_font(qtc_str family, double pt, int32_t weight, bool italic) {
    DRAW_GUARD(
        if (qtc_status e = need_rec("qtc_draw_font")) return e;
        QString fam;
        if (!read_str(family, fam, "family")) return last_error_code();
        if (!std::isfinite(pt) || pt <= 0 || pt > 10000) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_draw_font: point size must be > 0 and <= 10000");
        if (weight < 1 || weight > 1000) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_draw_font: weight must be 1 .. 1000");
        QFont f(fam);
        f.setPointSizeF(pt);
        f.setWeight(QFont::Weight(weight));
        f.setItalic(italic);
        rec().list->fonts.push_back(f);
        return add(OP_FONT, {}, "qtc_draw_font", int32_t(rec().list->fonts.size() - 1));)
}
QTC_API qtc_status qtc_draw_antialias(bool on) { DRAW_GUARD(return add(OP_AA, {on ? 1.0 : 0.0}, "qtc_draw_antialias");) }
QTC_API qtc_status qtc_draw_opacity(double o) {
    DRAW_GUARD(
        if (!std::isfinite(o) || o < 0 || o > 1) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_draw_opacity: 0 .. 1");
        return add(OP_OPACITY, {o}, "qtc_draw_opacity");)
}
QTC_API qtc_status qtc_draw_save(void) {
    DRAW_GUARD(
        if (qtc_status e = need_rec("qtc_draw_save")) return e;
        if (rec().depth >= 10000) return fail(QTC_ERR_LIMIT, "qtc_draw_save: nested too deeply");
        rec().depth++;
        return add(OP_SAVE, {}, "qtc_draw_save");)
}
QTC_API qtc_status qtc_draw_restore(void) {
    DRAW_GUARD(
        if (qtc_status e = need_rec("qtc_draw_restore")) return e;
        if (rec().depth <= 0) return fail(QTC_ERR_STATE, "qtc_draw_restore without a matching qtc_draw_save");
        rec().depth--;
        return add(OP_RESTORE, {}, "qtc_draw_restore");)
}
QTC_API qtc_status qtc_draw_translate(double dx, double dy) { DRAW_GUARD(return add(OP_TRANSLATE, {dx, dy}, "qtc_draw_translate");) }
QTC_API qtc_status qtc_draw_rotate(double deg) { DRAW_GUARD(return add(OP_ROTATE, {deg}, "qtc_draw_rotate");) }
QTC_API qtc_status qtc_draw_scale(double sx, double sy) {
    DRAW_GUARD(
        if (sx == 0 || sy == 0 || std::fabs(sx) > 1e6 || std::fabs(sy) > 1e6) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_draw_scale: factors must be non-zero and within +/-1e6");
        return add(OP_SCALE, {sx, sy}, "qtc_draw_scale");)
}
QTC_API qtc_status qtc_draw_clip_rect(double x, double y, double w, double h) { DRAW_GUARD(return add(OP_CLIP, {x, y, w, h}, "qtc_draw_clip_rect");) }
QTC_API qtc_status qtc_draw_point(double x, double y) { DRAW_GUARD(return add(OP_POINT, {x, y}, "qtc_draw_point");) }
QTC_API qtc_status qtc_draw_line(double x1, double y1, double x2, double y2) { DRAW_GUARD(return add(OP_LINE, {x1, y1, x2, y2}, "qtc_draw_line");) }
QTC_API qtc_status qtc_draw_rect(double x, double y, double w, double h) { DRAW_GUARD(return add(OP_RECT, {x, y, w, h}, "qtc_draw_rect");) }
QTC_API qtc_status qtc_draw_rounded_rect(double x, double y, double w, double h, double rx, double ry) {
    DRAW_GUARD(return add(OP_RRECT, {x, y, w, h, rx, ry}, "qtc_draw_rounded_rect");)
}
QTC_API qtc_status qtc_draw_ellipse(double x, double y, double w, double h) { DRAW_GUARD(return add(OP_ELLIPSE, {x, y, w, h}, "qtc_draw_ellipse");) }
QTC_API qtc_status qtc_draw_arc(double x, double y, double w, double h, double s, double sp) {
    DRAW_GUARD(
        if (std::fabs(s) > 1e6 || std::fabs(sp) > 1e6) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_draw_arc: angles within +/-1e6 degrees");
        return add(OP_ARC, {x, y, w, h, s, sp}, "qtc_draw_arc");)
}
QTC_API qtc_status qtc_draw_pie(double x, double y, double w, double h, double s, double sp) {
    DRAW_GUARD(
        if (std::fabs(s) > 1e6 || std::fabs(sp) > 1e6) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_draw_pie: angles within +/-1e6 degrees");
        return add(OP_PIE, {x, y, w, h, s, sp}, "qtc_draw_pie");)
}
static qtc_status add_points(uint8_t op, qtc_f64s xy, const char *what) {
    if (qtc_status e = need_rec(what)) return e;
    if (xy.len < 0 || (xy.len > 0 && !xy.data)) return fail(QTC_ERR_INVALID_ARG, QStringLiteral("%1: bad point array").arg(QLatin1StringView(what)));
    if (xy.len % 2) return fail(QTC_ERR_INVALID_ARG, QStringLiteral("%1: the array holds x, y pairs, so its length must be even").arg(QLatin1StringView(what)));
    if (xy.len / 2 > 10'000'000) return fail(QTC_ERR_LIMIT, QStringLiteral("%1: at most 10,000,000 points").arg(QLatin1StringView(what)));
    for (int64_t i = 0; i < xy.len; i++)
        if (!std::isfinite(xy.data[i]) || std::fabs(xy.data[i]) > kMaxCoord) return fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("%1: point %2 is not finite or beyond +/-1e9").arg(QLatin1StringView(what)).arg(i / 2));
    DrawList &l = *rec().list;
    size_t off = l.points.size();
    for (int64_t i = 0; i + 1 < xy.len; i += 2) l.points.emplace_back(xy.data[i], xy.data[i + 1]);
    return add(op, {double(off), double(xy.len / 2)}, what);
}
QTC_API qtc_status qtc_draw_polyline(qtc_f64s xy) { DRAW_GUARD(return add_points(OP_POLYLINE, xy, "qtc_draw_polyline");) }
QTC_API qtc_status qtc_draw_polygon(qtc_f64s xy) { DRAW_GUARD(return add_points(OP_POLYGON, xy, "qtc_draw_polygon");) }
QTC_API qtc_status qtc_draw_path_move(double x, double y) {
    DRAW_GUARD(
        if (qtc_status e = need_rec("qtc_draw_path_move")) return e;
        if (!finite_all({x, y})) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_draw_path_move");
        rec().path.moveTo(x, y);
        rec().path_open = true;
        return QTC_OK;)
}
QTC_API qtc_status qtc_draw_path_line(double x, double y) {
    DRAW_GUARD(
        if (qtc_status e = need_rec("qtc_draw_path_line")) return e;
        if (!finite_all({x, y})) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_draw_path_line");
        rec().path.lineTo(x, y);
        rec().path_open = true;
        return QTC_OK;)
}
QTC_API qtc_status qtc_draw_path_quad(double cx, double cy, double x, double y) {
    DRAW_GUARD(
        if (qtc_status e = need_rec("qtc_draw_path_quad")) return e;
        if (!finite_all({cx, cy, x, y})) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_draw_path_quad");
        rec().path.quadTo(cx, cy, x, y);
        rec().path_open = true;
        return QTC_OK;)
}
QTC_API qtc_status qtc_draw_path_cubic(double c1x, double c1y, double c2x, double c2y, double x, double y) {
    DRAW_GUARD(
        if (qtc_status e = need_rec("qtc_draw_path_cubic")) return e;
        if (!finite_all({c1x, c1y, c2x, c2y, x, y})) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_draw_path_cubic");
        rec().path.cubicTo(c1x, c1y, c2x, c2y, x, y);
        rec().path_open = true;
        return QTC_OK;)
}
QTC_API qtc_status qtc_draw_path_close(void) {
    DRAW_GUARD(
        if (qtc_status e = need_rec("qtc_draw_path_close")) return e;
        rec().path.closeSubpath();
        return QTC_OK;)
}
QTC_API qtc_status qtc_draw_path_end(void) {
    DRAW_GUARD(
        if (qtc_status e = need_rec("qtc_draw_path_end")) return e;
        if (!rec().path_open) return fail(QTC_ERR_STATE, "qtc_draw_path_end: the path is empty");
        rec().list->paths.push_back(rec().path);
        rec().path = QPainterPath();
        rec().path_open = false;
        return add(OP_PATH, {}, "qtc_draw_path_end", int32_t(rec().list->paths.size() - 1));)
}
QTC_API qtc_status qtc_draw_text(double x, double y, qtc_str text) {
    DRAW_GUARD(
        if (qtc_status e = need_rec("qtc_draw_text")) return e;
        QString t;
        if (!read_str(text, t, "text")) return last_error_code();
        rec().list->texts.push_back(t);
        return add(OP_TEXT, {x, y}, "qtc_draw_text", int32_t(rec().list->texts.size() - 1));)
}
QTC_API qtc_status qtc_draw_text_in(double x, double y, double w, double h, int32_t flags, qtc_str text) {
    DRAW_GUARD(
        if (qtc_status e = need_rec("qtc_draw_text_in")) return e;
        if (flags & ~0x3ffff) return fail(QTC_ERR_OUT_OF_RANGE, "qtc_draw_text_in: flags must be Qt::AlignmentFlag | Qt::TextFlag bits");
        QString t;
        if (!read_str(text, t, "text")) return last_error_code();
        rec().list->texts.push_back(t);
        return add(OP_TEXTIN, {x, y, w, h, double(flags)}, "qtc_draw_text_in", int32_t(rec().list->texts.size() - 1));)
}
QTC_API qtc_status qtc_draw_image(qtc_val image, double x, double y, double w, double h) {
    DRAW_GUARD(
        if (qtc_status e = need_rec("qtc_draw_image")) return e;
        qtc_status err = 0;
        QVariant *v = reg().value(image, &err);
        if (!v) return fail(err, "qtc_draw_image");
        if (v->metaType() == QMetaType::fromType<QImage>()) {
            rec().list->images.push_back(v->value<QImage>());
            return add(OP_IMAGE, {x, y, w, h}, "qtc_draw_image", int32_t(rec().list->images.size() - 1));
        }
        if (v->metaType() == QMetaType::fromType<QPixmap>()) {
            rec().list->pixmaps.push_back(v->value<QPixmap>());
            return add(OP_PIXMAP, {x, y, w, h}, "qtc_draw_image", int32_t(rec().list->pixmaps.size() - 1));
        }
        return fail(QTC_ERR_WRONG_TYPE, QStringLiteral("qtc_draw_image: a %1 is not an image").arg(QLatin1StringView(v->typeName())));)
}

}  // extern "C"
#endif

// --------------------------------------------------------- QPainterPath limits

#ifdef QTC_HAS_GUI
namespace qtc {

static constexpr int64_t kMaxPathElements = int64_t(1) << 22;

bool path_room(const QPainterPath &p, int64_t extra) {
    return extra <= 0 || int64_t(p.elementCount()) + extra <= kMaxPathElements;
}

// Flattening (QBezier::addToPolygon) splits each cubic until it is flat to half a pixel,
// at most nine times deep: up to 512 points per curve, about sqrt(2 * length) in practice.
bool flatten_ok(const QPainterPath &p, const QTransform &t) {
    const int n = p.elementCount();
    if (n == 0) return true;
    long double points = 0;
    const long double budget = (long double)alloc_limit() / sizeof(QPointF);
    QPointF prev = t.map(QPointF(p.elementAt(0).x, p.elementAt(0).y));
    for (int i = 0; i < n; i++) {
        const QPainterPath::Element e = p.elementAt(i);
        const QPointF pt = t.map(QPointF(e.x, e.y));
        if (e.type == QPainterPath::CurveToElement && i + 2 < n) {
            const QPointF c2 = t.map(QPointF(p.elementAt(i + 1).x, p.elementAt(i + 1).y));
            const QPointF end = t.map(QPointF(p.elementAt(i + 2).x, p.elementAt(i + 2).y));
            const double len = QLineF(prev, pt).length() + QLineF(pt, c2).length() + QLineF(c2, end).length();
            points += std::isfinite(len) ? std::min(512.0, std::sqrt(2.0 * len) + 2.0) : 512.0;
            prev = end;
            i += 2;
        } else {
            points += 1;
            prev = pt;
        }
        if (points > budget) return false;
    }
    return true;
}


// Qt's path clipper (united, intersected, subtracted, simplified) slows down with the number of
// places the paths cross, about as its square: two paths of 256 random points cross some 17,000
// times and take seconds, of 512 points minutes. The crossings are counted first with a sweep
// over x, which stops early once there are too many to be quick, and so do the segment pairs it
// would have to test.
// Qt measures a curve by splitting it until each piece is straight to within 0.01: about sqrt(length)
// pieces, and percentAtLength() measures again for each step of its search. Paths of many long
// curves (or curves far out, where doubles cannot get within 0.01) take minutes.
bool path_length_ok(const QPainterPath &p) {
    long double pieces = 0;
    for (int i = 0, n = p.elementCount(); i < n; i++) {
        const QPainterPath::Element e = p.elementAt(i);
        if (e.type == QPainterPath::CurveToElement && i > 0 && i + 2 < n) {
            const QPointF a = p.elementAt(i - 1), c = p.elementAt(i + 1), d = p.elementAt(i + 2);
            const double len = QLineF(a, QPointF(e)).length() + QLineF(QPointF(e), c).length() + QLineF(c, d).length();
            if (!std::isfinite(len) || std::abs(d.x()) > 1e12 || std::abs(d.y()) > 1e12) return false;
            pieces += std::sqrt(len) + 1;
            i += 2;
        } else {
            pieces += 1;
        }
        if (pieces > (1 << 20)) return false;
    }
    return true;
}

// no cell of the table spans more than one row or column
bool table_unmerged(const QTextTable *t) {
    for (int r = 0, rows = t->rows(); r < rows; r++)
        for (int c = 0, cols = t->columns(); c < cols; c++) {
            const QTextTableCell cell = t->cellAt(r, c);
            if (cell.isValid() && (cell.rowSpan() > 1 || cell.columnSpan() > 1)) return false;
        }
    return true;
}

// no cell of the table spans more than one row and more than one column
bool table_columns_addable(const QTextTable *t) {
    for (int r = 0, rows = t->rows(); r < rows; r++)
        for (int c = 0, cols = t->columns(); c < cols; c++) {
            const QTextTableCell cell = t->cellAt(r, c);
            if (cell.isValid() && cell.rowSpan() > 1 && cell.columnSpan() > 1) return false;
        }
    return true;
}

// A perspective (projective) transform maps a path's curves as line segments: up to 512 for each
// curve, however small (the flattening threshold shrinks with the transform's scale).
bool project_ok(const QTransform &t, const QPainterPath &p) {
    if (t.type() != QTransform::TxProject) return true;
    int64_t points = 0;
    for (int i = 0, n = p.elementCount(); i < n; i++)
        if ((points += p.elementAt(i).type == QPainterPath::CurveToElement ? 512 : 1) > (int64_t(1) << 20)) return false;
    return true;
}

bool clip_ok(const QPainterPath &a, const QPainterPath &b, bool count_crossings) {
    // its arithmetic (products of coordinates, intersection points) overflows long before Qt's
    // own limit on coordinates (1e128), and it crashes on what comes out: keep them to +-2^30.
    // Point by point: controlPointRect() passes over NaN, which crashes the clipper too.
    for (const QPainterPath *p : {&a, &b}) {
        for (int i = 0, n = p->elementCount(); i < n; i++) {
            const QPainterPath::Element e = p->elementAt(i);
            if (!(std::abs(e.x) <= 1073741824.0 && std::abs(e.y) <= 1073741824.0)) return false;
        }
    }
    if (!count_crossings) return true;
    struct Seg { double x0, y0, x1, y1, lo, hi; };
    std::vector<Seg> segs;
    auto collect = [&](const QPainterPath &p) {
        for (const QPolygonF &poly : p.toSubpathPolygons()) {
            for (qsizetype i = 0; i + 1 < poly.size(); i++) {
                const QPointF &u = poly[i], &v = poly[i + 1];
                segs.push_back({u.x(), u.y(), v.x(), v.y(), std::min(u.x(), v.x()), std::max(u.x(), v.x())});
            }
            if (poly.size() > 2 && poly.first() != poly.last())   // the clipper closes each subpath
                segs.push_back({poly.last().x(), poly.last().y(), poly.first().x(), poly.first().y(),
                                std::min(poly.last().x(), poly.first().x()), std::max(poly.last().x(), poly.first().x())});
            if (segs.size() > (size_t(1) << 15)) return false;
        }
        return true;
    };
    if (!collect(a) || !collect(b)) return false;
    // The clipper merges coincident points through a k-d tree that goes one level deeper for
    // each copy of a point: thousands of copies take seconds, and the recursion overflows the
    // stack (QKdPointTree::build).
    {
        QHash<std::pair<double, double>, int> copies;
        copies.reserve(qsizetype(segs.size()));
        for (const Seg &g : segs)
            if (++copies[{g.x0, g.y0}] > 1024) return false;
    }
    std::sort(segs.begin(), segs.end(), [](const Seg &l, const Seg &r) { return l.lo < r.lo; });
    auto side = [](double ax, double ay, double bx, double by, double cx, double cy) {
        const double d = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        return d > 0 ? 1 : (d < 0 ? -1 : 0);
    };
    int64_t crossings = 0, pairs = 0;
    std::vector<const Seg *> active;
    for (const Seg &s : segs) {
        size_t keep = 0;
        for (const Seg *t : active)
            if (t->hi >= s.lo) active[keep++] = t;
        active.resize(keep);
        for (const Seg *t : active) {
            if (++pairs > (int64_t(1) << 22)) return false;
            if (std::max(std::min(s.y0, s.y1), std::min(t->y0, t->y1)) > std::min(std::max(s.y0, s.y1), std::max(t->y0, t->y1)))
                continue;
            const int d1 = side(t->x0, t->y0, t->x1, t->y1, s.x0, s.y0), d2 = side(t->x0, t->y0, t->x1, t->y1, s.x1, s.y1);
            const int d3 = side(s.x0, s.y0, s.x1, s.y1, t->x0, t->y0), d4 = side(s.x0, s.y0, s.x1, s.y1, t->x1, t->y1);
            if (d1 * d2 < 0 && d3 * d4 < 0 && ++crossings > 4096) return false;
        }
        active.push_back(&s);
    }
    return true;
}
}  // namespace qtc
#endif  // QTC_HAS_GUI
