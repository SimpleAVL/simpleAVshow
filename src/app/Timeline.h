#pragma once
#include "app/Controller.h"
#include <QAbstractScrollArea>

namespace sav {
class Timeline : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit Timeline(Controller& controller, QWidget* parent = nullptr);
    QString selectedClip, selectedMarker;
    int selectedTrack = 0;
    bool snapping = true;
    void setZoom(double pixelsPerSecond);
    void selectClip(QString id);
    void selectMarker(QString id);
    void refresh();
signals:
    void selectionChanged();
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dropEvent(QDropEvent*) override;
signals:
    void assetDropped(QString asset, int track, sav::Frame time);
private:
    Controller& c_;
    static constexpr int header = 190, ruler = 58, row = 76;
    double zoom_ = 22;
    double scrollScale_ = 1;
    int dragMode_ = 0, dragClip_ = -1, dragTrack_ = 0;
    Frame dragStart_ = 0, dragDuration_ = 0, dragOffset_ = 0, dragTime_ = 0;
    QPoint press_;
    bool dragging_ = false;
    double xAt(Frame time) const;
    Frame timeAt(double x) const;
    int trackAt(double y) const;
    QRectF clipRect(const Clip& clip) const;
    Frame snap(Frame time, const std::string& ignore = {}) const;
};
}
