#pragma once
#include "app/Controller.h"
#include <QAbstractScrollArea>
#include <QElapsedTimer>

namespace sav {
class Timeline : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit Timeline(Controller& controller, QWidget* parent = nullptr);
    QString selectedClip, selectedMarker;
    int selectedTrack = 0;
    bool snapping = true, follow = true;
    // Keeps the playhead (or the viewport centre when it is off-screen) under the same x.
    void setZoom(double pixelsPerSecond);
    double zoom() const { return zoom_; }
    void zoomToFit();
    void selectClip(QString id);
    void selectMarker(QString id);
    void refresh();
    // The automation property drawn and edited on the selected clip (other clips show volume/opacity).
    void setCurveProperty(int property);
    // Spoken summary of the current selection (also the accessible description).
    QString describe() const;
signals:
    void selectionChanged();
    void curvePropertyRequested(int property);
    void zoomChanged(double pixelsPerSecond);
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void focusInEvent(QFocusEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dropEvent(QDropEvent*) override;
signals:
    void assetDropped(QString asset, int track, sav::Frame time);
private:
    Controller& c_;
    static constexpr int header = 190, ruler = 58, row = 56;
    double zoom_ = 22;
    double scrollScale_ = 1;
    int dragMode_ = 0, dragClip_ = -1, dragMarker_ = -1, dragTrack_ = 0;
    Frame dragStart_ = 0, dragDuration_ = 0, dragOffset_ = 0, dragTime_ = 0;
    QPoint press_;
    bool dragging_ = false, autoScroll_ = false;
    Frame playhead_ = 0;
    // Keyframe editing: the clip and property being edited, its working copy, and the frozen value scale.
    enum class CurveDrag { None, Point, Bend };
    CurveDrag curveDrag_ = CurveDrag::None;
    int curveProperty_ = -1, curveClip_ = -1, curveEdit_ = 0, curvePoint_ = -1;
    bool curveInserted_ = false;
    Envelope curve_;
    double curveLow_ = 0, curveHigh_ = 1, curveBend_ = 0;
    Frame curveOrigin_ = 0;
    std::string lastInsertedClip_; Frame lastInsertedTime_ = -1; QElapsedTimer inserted_;
    struct CurveHit { int point = -1, segment = -1; bool line = false; };
    State drawnState_ = State::Stopped;
    QElapsedTimer manual_;
    void onTick();
    void reveal(Frame time);
    void zoomAround(double pixelsPerSecond, double x);
    double offset() const;
    void setOffset(double pixels);
    QRect strip(Frame time) const;
    Frame sourceLength(const Clip& clip) const;
    double xAt(Frame time) const;
    Frame timeAt(double x) const;
    int trackAt(double y) const;
    QRectF clipRect(const Clip& clip) const;
    Frame snap(Frame time, const std::string& ignore = {}) const;
    int curvePropertyFor(const Clip& clip) const;
    std::pair<double, double> curveRange(const Clip& clip, int property) const;
    static QRectF curveBand(const QRectF& rect) { return rect.adjusted(0, 18, 0, -4); }
    double curveY(const QRectF& band, double low, double high, double value) const;
    // Point within reach of pos, else the segment under it and whether pos is on the line.
    CurveHit curveHit(const Clip& clip, QPointF pos) const;
    int clipAt(QPointF pos) const;
    void commitCurve(const std::string& id, int property, const Envelope& curve);
};
}
