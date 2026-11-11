// Qt Charts' part of the ownership roles (see qtc_roles.cpp and qtc_module.h), and the state
// checks the generated Charts bindings ask of it.
#include "../qtc_module.h"

#include <functional>

#include <QtCharts/QAbstractBarSeries>
#include <QtCharts/QAreaSeries>
#include <QtCharts/QBarSet>
#include <QtCharts/QBoxPlotSeries>
#include <QtCharts/QBoxSet>
#include <QtCharts/QCandlestickModelMapper>
#include <QtCharts/QCandlestickSeries>
#include <QtCharts/QCandlestickSet>
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QHBarModelMapper>
#include <QtCharts/QHBoxPlotModelMapper>
#include <QtCharts/QHPieModelMapper>
#include <QtCharts/QHXYModelMapper>
#include <QtCharts/QLegend>
#include <QtCharts/QLegendMarker>
#include <QtCharts/QLineSeries>
#include <QtCharts/QPieSeries>
#include <QtCharts/QPieSlice>
#include <QtCharts/QVBarModelMapper>
#include <QtCharts/QVBoxPlotModelMapper>
#include <QtCharts/QVPieModelMapper>
#include <QtCharts/QVXYModelMapper>
#include <QtGui/QStandardItemModel>

namespace qtc {
namespace {

uint64_t kinds(QObject *o) {
    uint64_t k = 0;
    if (qobject_cast<QAbstractBarSeries *>(o) || qobject_cast<QPieSeries *>(o) || qobject_cast<QBoxPlotSeries *>(o) ||
        qobject_cast<QCandlestickSeries *>(o))
        k |= OK_CHARTSERIES;
    if (qobject_cast<QAreaSeries *>(o)) k |= OK_AREA;
    if (qobject_cast<QXYModelMapper *>(o) || qobject_cast<QBarModelMapper *>(o) || qobject_cast<QPieModelMapper *>(o) ||
        qobject_cast<QBoxPlotModelMapper *>(o) || qobject_cast<QCandlestickModelMapper *>(o))
        k |= OK_CHARTMAPPER;
    return k;
}

void visit(QObject *owner, uint64_t k, VisitCtx &c) {
    if (k & OK_SCROLLAREA) {
        if (auto *v = qobject_cast<QChartView *>(owner))
            if (c.hit(v->chart()) && c.held()) v->setChart(new QChart);
    }
    if (k & OK_CHARTSERIES) {
        if (auto *s = qobject_cast<QAbstractBarSeries *>(owner))
            for (QBarSet *x : s->barSets())
                if (c.hit(x) && c.held()) s->take(x);
        if (auto *s = qobject_cast<QPieSeries *>(owner))
            for (QPieSlice *x : s->slices())
                if (c.hit(x) && c.held()) s->take(x);
        if (auto *s = qobject_cast<QBoxPlotSeries *>(owner))
            for (QBoxSet *x : s->boxSets())
                if (c.hit(x) && c.held()) s->take(x);
        if (auto *s = qobject_cast<QCandlestickSeries *>(owner))
            for (QCandlestickSet *x : s->sets())
                if (c.hit(x) && c.held()) s->take(x);
    }
    if (k & OK_AREA) {
        auto *a = static_cast<QAreaSeries *>(owner);
        if (c.hit(a->upperSeries()) && c.held()) a->setUpperSeries(nullptr);
        if (c.hit(a->lowerSeries()) && c.held()) a->setLowerSeries(nullptr);
    }
    if (k & OK_CHARTMAPPER) {
        // A model mapper writes its series' changes into its model without checking that the
        // model is still there (Qt clears the pointer when the model goes). Before its model is
        // destroyed it gets an empty model of its own instead, and the series follows that one.
        // (setSeries(nullptr) would not do: the XY, bar, pie and box plot mappers only stop
        // following the series and keep pointing at it.)
        QObject *model = nullptr;
        std::function<void()> swap;
        auto take = [&](auto *m) {
            if (!m || swap || !m->series()) return;   // without a series it writes nothing
            model = m->model();
            swap = [m] { m->setModel(new QStandardItemModel(m)); };
        };
        take(qobject_cast<QVXYModelMapper *>(owner));
        take(qobject_cast<QHXYModelMapper *>(owner));
        take(qobject_cast<QVBarModelMapper *>(owner));
        take(qobject_cast<QHBarModelMapper *>(owner));
        take(qobject_cast<QVPieModelMapper *>(owner));
        take(qobject_cast<QHPieModelMapper *>(owner));
        take(qobject_cast<QVBoxPlotModelMapper *>(owner));
        take(qobject_cast<QHBoxPlotModelMapper *>(owner));
        take(qobject_cast<QCandlestickModelMapper *>(owner));
        if (swap && c.hit(model) && c.held()) swap();
    }
}

bool owned_class(QObject *o) { return qobject_cast<QLegend *>(o) || qobject_cast<QLegendMarker *>(o); }

QString owned_by(QObject *x) {
    if (qobject_cast<QLegend *>(x)) return QStringLiteral("the legend of a chart");
    if (qobject_cast<QLegendMarker *>(x)) return QStringLiteral("a marker of a chart legend");
    return QString();
}

const ModuleExt kExt = [] {
    ModuleExt e;
    e.module = "Charts";
    e.class_kinds = kinds;
    e.visit = visit;
    e.qt_owned_class = owned_class;
    e.qt_owned_by_class = owned_by;
    return e;
}();

}  // namespace

// Qt Charts crashes when an area series in a chart has a boundary line whose point label
// clipping is off (the default is on): the boundary's chart item updates a graphics item that
// is not set up yet when it is built, and again when the clipping changes, also after the area
// series left the chart (its chart item goes only with the next deferred delete). So the
// boundary lines of an area series keep their clipping on.
bool bound_clipping_ok(const QObject *series) {
    for (const auto &[o, k] : roles_candidates(const_cast<QObject *>(series))) {
        if (!(k & OK_AREA)) continue;
        auto *a = static_cast<QAreaSeries *>(o);
        if (a->upperSeries() == series || a->lowerSeries() == series) return false;
    }
    return true;
}

// An area series also needs its upper series when it goes into a chart: its chart item is built
// without one then, and animating it reads through the missing upper line.
bool area_clipping_ok(const QObject *series) {
    if (auto *a = qobject_cast<const QAreaSeries *>(series)) {
        if (!a->upperSeries()) return false;
        for (const QLineSeries *l : {a->upperSeries(), a->lowerSeries()})
            if (l && !l->pointLabelsClipping()) return false;
    }
    return true;
}

}  // namespace qtc

QTC_MODULE_EXT(Charts) { qtc::register_module_ext(&qtc::kExt); }
