// Classes qtc adds to Qt: a canvas for retained drawing and a QML/C message bridge.
#pragma once
#include <QtCore/QObject>
#include <QtCore/QVariant>
#include <QtCore/QString>
#include <memory>

#ifdef QTC_HAS_GUI
#include <QtGui/QFont>
#include <QtGui/QImage>
#include <QtGui/QPainterPath>
#include <QtGui/QPixmap>
#include <QtCore/QPointF>
#include <vector>

namespace qtc {
struct DrawCmd {
    uint8_t op = 0;
    int32_t i = 0;
    uint32_t c1 = 0, c2 = 0;   // colours, ARGB
    double a[6] = {0, 0, 0, 0, 0, 0};
};
struct DrawList {
    std::vector<DrawCmd> cmds;
    std::vector<QString> texts;
    std::vector<QImage> images;
    std::vector<QPixmap> pixmaps;
    std::vector<QPointF> points;
    std::vector<QPainterPath> paths;
    std::vector<QFont> fonts;
};
void replay(class QPainter &p, const DrawList &list, const QRect &bounds);
}  // namespace qtc
#endif

#ifdef QTC_HAS_WIDGETS
#include <QtWidgets/QWidget>

class QtcCanvas : public QWidget {
    Q_OBJECT
    Q_PROPERTY(int commandCount READ commandCount)
    Q_PROPERTY(int paintCount READ paintCount)
public:
    explicit QtcCanvas(QWidget *parent = nullptr);
    void setList(std::shared_ptr<const qtc::DrawList> list);
    int commandCount() const;
    int paintCount() const { return paints_; }
    Q_INVOKABLE void clearDrawing();

protected:
    void paintEvent(class QPaintEvent *e) override;

private:
    std::shared_ptr<const qtc::DrawList> list_;
    int paints_ = 0;
};
#endif

class QtcBridge : public QObject {
    Q_OBJECT
public:
    explicit QtcBridge(QObject *parent = nullptr) : QObject(parent) {}
    // Called from QML (or anywhere): reaches C as the signal sent(name, payload).
    Q_INVOKABLE void send(const QString &name, const QVariant &payload = QVariant()) { Q_EMIT sent(name, payload); }
    // Called from C: reaches QML as the signal message(name, payload).
    Q_INVOKABLE void post(const QString &name, const QVariant &payload = QVariant()) { Q_EMIT message(name, payload); }

Q_SIGNALS:
    void sent(const QString &name, const QVariant &payload);
    void message(const QString &name, const QVariant &payload);
};
