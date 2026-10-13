#include "app/Timeline.h"
#include <QPainter>
#include <QMouseEvent>
#include <QScrollBar>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QPainterPath>
#include <QWheelEvent>
#include <cmath>
#include <limits>

namespace sav {
namespace {
QColor trackColor(const Track& track) {
    if (track.visual) return QColor("#648fef");
    if (track.async) return QColor("#45bfa6");
    return QColor("#ad8cde");
}
}
Timeline::Timeline(Controller& c, QWidget* parent) : QAbstractScrollArea(parent), c_(c) {
    setMinimumHeight(240); setFrameShape(QFrame::NoFrame); setAcceptDrops(true); setMouseTracking(true);
    viewport()->setAcceptDrops(true); viewport()->setMouseTracking(true);
    connect(&c, &Controller::changed, this, &Timeline::refresh);
    connect(&c, &Controller::tick, this, &Timeline::onTick);
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this] { if (!autoScroll_) manual_.start(); viewport()->update(); });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { viewport()->update(); });
    refresh();
}
void Timeline::setZoom(double value) {
    const double x = xAt(c_.position());
    zoomAround(value, x >= header && x <= viewport()->width() ? x : (header + viewport()->width()) / 2.);
}
void Timeline::zoomAround(double value, double x) {
    value = std::clamp(value, .01, 1000.); if (value == zoom_) return;
    const double time = (x - header + offset()) / zoom_;
    zoom_ = value; refresh(); setOffset(time * zoom_ - (x - header)); manual_.start();
}
void Timeline::zoomToFit() {
    zoomAround(std::max(1., viewport()->width() - header - 80.) / std::max(.001, seconds(c_.project.show.duration)), header);
    setOffset(0); emit zoomChanged(zoom_);
}
void Timeline::wheelEvent(QWheelEvent* event) {
    if (!(event->modifiers() & Qt::ControlModifier)) { QAbstractScrollArea::wheelEvent(event); return; }
    zoomAround(zoom_ * std::pow(1.0015, event->angleDelta().y()), std::max<double>(header, event->position().x()));
    emit zoomChanged(zoom_); event->accept();
}
double Timeline::offset() const { return horizontalScrollBar()->value() * scrollScale_; }
void Timeline::setOffset(double pixels) {
    const bool was = std::exchange(autoScroll_, true);
    horizontalScrollBar()->setValue(static_cast<int>(std::lround(std::clamp(pixels / scrollScale_, 0., static_cast<double>(horizontalScrollBar()->maximum())))));
    autoScroll_ = was;
}
QRect Timeline::strip(Frame time) const { return QRect(static_cast<int>(std::floor(xAt(time))) - 7, 0, 15, viewport()->height()); }
// Playhead-only ticks repaint two narrow strips; follow pages while playing unless the user just scrolled.
void Timeline::onTick() {
    const Frame now = c_.position(); const State state = c_.transport.state();
    if (follow && (state == State::Playing || state == State::Held) && !dragging_ && !(manual_.isValid() && !manual_.hasExpired(3000))) {
        const double x = xAt(now), width = viewport()->width() - header; const int before = horizontalScrollBar()->value();
        if (width > 0 && (x < header || x > header + width * .85)) setOffset(seconds(now) * zoom_ - width * .15);
        if (horizontalScrollBar()->value() != before) { playhead_ = now; drawnState_ = state; return; }
    }
    if (now == playhead_ && state == drawnState_) return;
    viewport()->update(strip(playhead_)); viewport()->update(strip(now)); playhead_ = now; drawnState_ = state;
}
Frame Timeline::sourceLength(const Clip& clip) const {
    if (clip.loop || !clip.text.empty()) return std::numeric_limits<Frame>::max();
    const auto asset = c_.project.assets.value(QString::fromStdString(clip.assetId));
    Frame length = asset.video && asset.duration > 0 ? frames(asset.duration) : 0;
    if (clip.audio) length = std::max(length, clip.audio->length());
    return length > 0 ? length : std::numeric_limits<Frame>::max();
}
void Timeline::refresh() {
    const bool was = std::exchange(autoScroll_, true);
    const double width = std::max(0., seconds(c_.project.show.duration) * zoom_ - viewport()->width() + header + 80);
    const double oldOffset = offset();
    scrollScale_ = std::max(1., width / (std::numeric_limits<int>::max() - 1.));
    horizontalScrollBar()->setRange(0, static_cast<int>(width / scrollScale_));
    setOffset(std::min(width, oldOffset));
    horizontalScrollBar()->setPageStep(std::max(1, static_cast<int>((viewport()->width() - header) / scrollScale_)));
    const auto height = static_cast<std::int64_t>(c_.project.show.tracks.size()) * row + ruler - viewport()->height();
    verticalScrollBar()->setRange(0, static_cast<int>(std::clamp(height, std::int64_t{0}, static_cast<std::int64_t>(std::numeric_limits<int>::max()))));
    verticalScrollBar()->setPageStep(std::max(1, viewport()->height() - ruler)); autoScroll_ = was;
    playhead_ = c_.position(); drawnState_ = c_.transport.state();
    selectedTrack = std::clamp(selectedTrack, 0, static_cast<int>(c_.project.show.tracks.size()) - 1);
    if (!selectedClip.isEmpty() && std::none_of(c_.project.show.clips.begin(), c_.project.show.clips.end(), [this](const Clip& clip) { return clip.id == selectedClip.toStdString(); })) selectedClip.clear();
    if (!selectedMarker.isEmpty() && std::none_of(c_.project.show.markers.begin(), c_.project.show.markers.end(), [this](const Marker& m) { return m.id == selectedMarker.toStdString(); })) selectedMarker.clear();
    viewport()->update();
}
void Timeline::resizeEvent(QResizeEvent* event) { QAbstractScrollArea::resizeEvent(event); refresh(); }
double Timeline::xAt(Frame t) const { return header + seconds(t) * zoom_ - horizontalScrollBar()->value() * scrollScale_; }
Frame Timeline::timeAt(double x) const { return std::clamp(frames((x - header + horizontalScrollBar()->value() * scrollScale_) / zoom_), Frame{0}, c_.project.show.duration); }
int Timeline::trackAt(double y) const { return static_cast<int>(std::floor((y - ruler + verticalScrollBar()->value()) / row)); }
QRectF Timeline::clipRect(const Clip& clip) const {
    return QRectF(xAt(clip.start), ruler + clip.track * row - verticalScrollBar()->value() + 5, seconds(clip.duration) * zoom_, row - 10);
}
Frame Timeline::snap(Frame time, const std::string& ignore) const {
    if (!snapping) return time;
    Frame snapped = frames(std::round(seconds(time) * c_.project.fps) / c_.project.fps);
    Frame distance = frames(8 / zoom_);
    auto consider = [&](Frame t) { const Frame d = std::abs(t - time); if (d < distance) { distance = d; snapped = t; } };
    consider(c_.position()); consider(0);
    for (const auto& m : c_.project.show.markers) if (m.id != ignore) consider(m.time);
    for (const auto& clip : c_.project.show.clips) if (clip.id != ignore) { consider(clip.start); consider(clip.end()); }
    return std::clamp(snapped, Frame{0}, c_.project.show.duration);
}
void Timeline::selectClip(QString id) {
    selectedClip = std::move(id); selectedMarker.clear();
    for (const auto& c : c_.project.show.clips) if (c.id == selectedClip.toStdString()) selectedTrack = c.track;
    emit selectionChanged(); viewport()->update();
}
void Timeline::selectMarker(QString id) { selectedMarker = std::move(id); selectedClip.clear(); emit selectionChanged(); viewport()->update(); }
void Timeline::paintEvent(QPaintEvent* event) {
    QPainter p(viewport()); p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(viewport()->rect(), QColor("#111720"));
    const auto& show = c_.project.show; const QRect area = event->rect();
    // 1-2-5 seconds/minutes spacing keeps ruler labels at least 70 px apart.
    double interval = 1;
    for (double step : {1., 2., 5., 10., 20., 30., 60., 120., 300., 600., 1200., 1800., 3600.}) if ((interval = step) * zoom_ >= 70) break;
    while (interval * zoom_ < 70) interval *= 2;
    const double first = std::floor(seconds(timeAt(header)) / interval) * interval;
    p.setClipRect(QRect(header, ruler, viewport()->width() - header, viewport()->height() - ruler));
    for (std::size_t i = 0; i < show.tracks.size(); ++i) {
        const int y = ruler + static_cast<int>(i) * row - verticalScrollBar()->value();
        p.fillRect(header, y, viewport()->width() - header, row, QColor(i % 2 ? "#131a24" : "#151d28"));
        p.setPen(QColor("#263140")); p.drawLine(header, y + row - 1, viewport()->width(), y + row - 1);
    }
    for (double t = first; xAt(frames(t)) < viewport()->width(); t += interval) {
        p.setPen(QColor("#212a37")); p.drawLine(QPointF(xAt(frames(t)), ruler), QPointF(xAt(frames(t)), viewport()->height()));
    }
    for (const auto& marker : show.markers) {
        p.setPen(QPen(QColor(223, 182, 91, 80), 1, Qt::DashLine));
        p.drawLine(QPointF(xAt(marker.time), ruler), QPointF(xAt(marker.time), viewport()->height()));
    }
    for (std::size_t index = 0; index < show.clips.size(); ++index) {
        const auto& clip = show.clips[index]; QRectF rect = clipRect(clip);
        if (dragging_ && dragClip_ == static_cast<int>(index)) {
            Clip ghost = clip; ghost.start = dragStart_; ghost.duration = dragDuration_; ghost.track = dragTrack_; rect = clipRect(ghost);
        } else if (dragging_ && dragClip_ >= 0 && !clip.link.empty() && clip.link == show.clips[dragClip_].link) {
            // Linked partners follow the drag on their own tracks.
            const auto& dragged = show.clips[dragClip_];
            Clip ghost = clip; ghost.start += dragStart_ - dragged.start; ghost.duration += dragDuration_ - dragged.duration; rect = clipRect(ghost);
        }
        if (!rect.intersects(QRectF(header, ruler, viewport()->width() - header, viewport()->height() - ruler) & QRectF(area))) continue;
        const auto& track = show.tracks[clip.track]; const QColor color = trackColor(track);
        const bool selected = clip.id == selectedClip.toStdString();
        p.setPen(QPen(selected ? QColor("#dce8ff") : color.darker(140), selected ? 2 : 1));
        p.setBrush(track.muted ? QColor("#29313c") : color.darker(240)); p.drawRoundedRect(rect, 5, 5);
        p.save(); p.setClipRect(rect.adjusted(2, 2, -2, -2));
        QString name = clip.text.empty() ? c_.project.assets.value(QString::fromStdString(clip.assetId)).name : QString::fromStdString(clip.text).section('\n', 0, 0);
        p.setPen(QColor("#e3eaf5")); QFont font = p.font(); font.setPointSize(10); font.setBold(true); p.setFont(font);
        p.drawText(rect.adjusted(10, 3, -10, -3), Qt::AlignTop | Qt::AlignLeft, (clip.loop ? "↻  " : "") + name + (clip.link.empty() ? "" : "  · linked"));
        if (clip.audio) {
            p.setPen(QPen(QColor(color.red(), color.green(), color.blue(), 150), 1));
            const int base = static_cast<int>(std::max(static_cast<double>(header), rect.left() + 6));
            const int left = base + std::max(0, (area.left() - base - 1) / 3) * 3;
            const int right = static_cast<int>(std::min({static_cast<double>(viewport()->width()), rect.right() - 6, area.right() + 3.}));
            for (int x = left; x < right; x += 3) {
                Frame sample = clip.offset + frames((x - rect.left()) / zoom_);
                if (clip.loop && clip.audio->length() > clip.offset) sample = clip.offset + (sample - clip.offset) % (clip.audio->length() - clip.offset);
                float amplitude = 0;
                amplitude = clip.audio->peakAt(sample);
                const double height = std::min(10., 1.5 + amplitude * 18.);
                p.drawLine(QPointF(x, rect.bottom() - 14 - height), QPointF(x, rect.bottom() - 14 + height));
            }
        }
        const auto& curve = track.visual ? clip.alpha : clip.volume;
        if (!curve.points.empty()) {
            p.setPen(QPen(QColor("#e7c982"), 1.5)); QPainterPath path;
            const double maximum = track.visual ? 1 : std::max(1., std::max_element(curve.points.begin(), curve.points.end(), [](const Keyframe& a, const Keyframe& b) { return a.value < b.value; })->value);
            for (std::size_t i = 0; i < curve.points.size(); ++i) {
                const QPointF point(rect.left() + seconds(curve.points[i].time) * zoom_, rect.bottom() - 4 - curve.points[i].value / maximum * 20);
                if (i == 0) path.moveTo(point); else path.lineTo(point);
                p.setBrush(QColor("#e7c982")); p.drawEllipse(point, 2.5, 2.5);
            }
            p.setBrush(Qt::NoBrush); p.drawPath(path);
        }
        p.restore();
    }
    p.setClipping(false);
    // Fixed headers, independent of horizontal scrolling.
    p.fillRect(0, 0, header, viewport()->height(), QColor("#19222f"));
    p.fillRect(0, 0, viewport()->width(), ruler, QColor("#1c2634"));
    p.setPen(QColor("#93a4ba")); p.drawText(QRect(15, 0, header - 20, ruler), Qt::AlignVCenter, "TRACKS / OUTPUTS");
    p.save(); p.setClipRect(QRect(0, ruler, header, viewport()->height() - ruler));
    for (std::size_t i = 0; i < show.tracks.size(); ++i) {
        const auto& track = show.tracks[i]; const int y = ruler + static_cast<int>(i) * row - verticalScrollBar()->value();
        if (static_cast<int>(i) == selectedTrack) p.fillRect(0, y, header, row, QColor("#26364b"));
        p.fillRect(0, y + 8, 3, row - 16, trackColor(track));
        p.setPen(track.muted ? QColor("#6e7c8d") : QColor("#e1e8f2"));
        QFont font = p.font(); font.setBold(true); font.setPointSize(11); p.setFont(font);
        p.drawText(QRect(15, y + 7, header - 24, 22), Qt::AlignLeft | Qt::AlignVCenter, QString::fromStdString(track.name));
        font.setBold(false); font.setPointSize(9); p.setFont(font); p.setPen(QColor("#8fa3bc"));
        const QString route = track.visual ? "PICTURE" : track.left < 0 && track.right < 0 ? "OUTPUT OFF" :
            QString("OUT %1 / %2").arg(track.left < 0 ? "—" : QString::number(track.left + 1)).arg(track.right < 0 ? "—" : QString::number(track.right + 1));
        p.drawText(QRect(15, y + 29, header - 24, 18), Qt::AlignLeft | Qt::AlignVCenter, (track.async ? "ASYNC  ·  " : "") + route + (track.muted ? "  ·  M" : ""));
        p.setPen(QColor("#2b3747")); p.drawLine(0, y + row - 1, header, y + row - 1);
    }
    p.restore(); p.save(); p.setClipRect(QRect(header, 0, viewport()->width() - header, ruler));
    QFont font = p.font(); font.setPointSize(9); p.setFont(font);
    for (double t = first; xAt(frames(t)) < viewport()->width(); t += interval) {
        const double x = xAt(frames(t)); p.setPen(QColor("#8395ad"));
        p.drawText(QRectF(x + 5, 2, 80, 22), Qt::AlignLeft | Qt::AlignVCenter, QString("%1:%2").arg(static_cast<qint64>(t) / 60, 2, 10, QChar('0')).arg(static_cast<qint64>(t) % 60, 2, 10, QChar('0')));
        p.setPen(QColor("#40516b")); p.drawLine(QPointF(x, 22), QPointF(x, 29));
    }
    for (std::size_t i = 0; i < show.markers.size(); ++i) {
        const auto& marker = show.markers[i];
        const double x = xAt(dragging_ && dragMarker_ == static_cast<int>(i) ? dragStart_ : marker.time); const bool selected = marker.id == selectedMarker.toStdString();
        p.setPen(Qt::NoPen); p.setBrush(selected ? QColor("#ffe3a4") : QColor("#d8b668"));
        QPolygonF flag{QPointF(x, 32), QPointF(x + 10, 32), QPointF(x + 10, 42), QPointF(x, 50)}; p.drawPolygon(flag);
        p.setPen(QColor("#e4c785")); p.drawText(QRectF(x + 15, 30, 150, 22), Qt::AlignLeft | Qt::AlignVCenter, QString::fromStdString(marker.name));
    }
    p.restore();
    const double playhead = xAt(playhead_);
    if (playhead >= header && playhead <= viewport()->width()) {
        const QColor color = drawnState_ == State::Held ? QColor("#e7c982") : QColor("#f3a285");
        p.setPen(QPen(color, 1.5)); p.drawLine(QPointF(playhead, 0), QPointF(playhead, viewport()->height()));
        p.setBrush(color); p.drawPolygon(QPolygonF{QPointF(playhead - 5, 0), QPointF(playhead + 5, 0), QPointF(playhead, 7)});
    }
}
void Timeline::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    press_ = event->position().toPoint(); dragTime_ = timeAt(press_.x()); dragging_ = false; dragClip_ = -1; dragMarker_ = -1;
    if (press_.x() < header) {
        const int track = trackAt(press_.y());
        if (track >= 0 && static_cast<std::size_t>(track) < c_.project.show.tracks.size()) {
            selectedTrack = track; selectedClip.clear(); selectedMarker.clear(); emit selectionChanged(); viewport()->update();
        }
        return;
    }
    if (press_.y() >= 29 && press_.y() < ruler) {
        for (std::size_t i = 0; i < c_.project.show.markers.size(); ++i) {
            const auto& marker = c_.project.show.markers[i];
            if (std::abs(xAt(marker.time) - press_.x()) >= 12) continue;
            selectMarker(QString::fromStdString(marker.id));
            if (c_.canEdit(false)) { dragMarker_ = static_cast<int>(i); dragStart_ = marker.time; }
            return;
        }
    }
    if (press_.y() < ruler) { c_.seek(snap(dragTime_)); return; }
    for (int i = static_cast<int>(c_.project.show.clips.size()) - 1; i >= 0; --i) {
        const auto& clip = c_.project.show.clips[i]; const QRectF rect = clipRect(clip);
        if (!rect.contains(press_)) continue;
        selectClip(QString::fromStdString(clip.id));
        if (!c_.canEdit(false)) return;
        dragClip_ = i; dragStart_ = clip.start; dragDuration_ = clip.duration; dragOffset_ = clip.offset; dragTrack_ = clip.track;
        dragMode_ = press_.x() < rect.left() + 7 ? 1 : press_.x() > rect.right() - 7 ? 2 : 0;
        return;
    }
    const int track = trackAt(press_.y());
    if (track >= 0 && static_cast<std::size_t>(track) < c_.project.show.tracks.size()) selectedTrack = track;
    selectedClip.clear(); selectedMarker.clear(); emit selectionChanged(); c_.seek(snap(dragTime_));
}
void Timeline::mouseMoveEvent(QMouseEvent* event) {
    if ((event->buttons() & Qt::LeftButton) && dragMarker_ >= 0 && c_.canEdit(false)) {
        if ((event->position().toPoint() - press_).manhattanLength() < 4 && !dragging_) return;
        dragging_ = true; const auto& marker = c_.project.show.markers[dragMarker_];
        dragStart_ = std::clamp(snap(marker.time + timeAt(event->position().x()) - dragTime_, marker.id), Frame{0}, c_.project.show.duration);
        viewport()->update(); return;
    }
    if (!(event->buttons() & Qt::LeftButton) || dragClip_ < 0 || !c_.canEdit(false)) return;
    if ((event->position().toPoint() - press_).manhattanLength() < 4 && !dragging_) return;
    dragging_ = true; const auto& original = c_.project.show.clips[dragClip_];
    const Frame delta = timeAt(event->position().x()) - dragTime_;
    const Frame minimum = std::min(original.duration, frames(1. / c_.project.fps));
    if (dragMode_ == 1) {
        dragStart_ = std::clamp(snap(original.start + delta, original.id), std::max(Frame{0}, original.start - original.offset), original.end() - minimum);
        dragDuration_ = original.end() - dragStart_; dragOffset_ = original.offset + dragStart_ - original.start;
    } else if (dragMode_ == 2) {
        // Non-looping media cannot run past its source end.
        const Frame limit = std::min(c_.project.show.duration - original.start, sourceLength(original) - original.offset);
        dragDuration_ = std::clamp(snap(original.end() + delta, original.id) - original.start, minimum, std::max(minimum, limit));
    }
    else {
        dragStart_ = std::clamp(snap(original.start + delta, original.id), Frame{0}, c_.project.show.duration - original.duration);
        const int nextTrack = trackAt(event->position().y());
        if (nextTrack >= 0 && static_cast<std::size_t>(nextTrack) < c_.project.show.tracks.size() &&
            c_.project.show.tracks[nextTrack].visual == c_.project.show.tracks[original.track].visual) dragTrack_ = nextTrack;
    }
    viewport()->update();
}
void Timeline::mouseReleaseEvent(QMouseEvent*) {
    if (dragging_ && dragMarker_ >= 0) {
        const int index = dragMarker_; const Frame time = dragStart_; dragging_ = false; dragMarker_ = -1;
        c_.edit([=](Project& p) { p.show.markers[index].time = time; }); emit selectionChanged();
    }
    if (dragging_ && dragClip_ >= 0) {
        const int index = dragClip_; const Frame start = dragStart_, duration = dragDuration_, offset = dragOffset_; const int track = dragTrack_;
        dragging_ = false; dragClip_ = -1;
        c_.edit([=](Project& p) {
            const auto id = p.show.clips[index].id; p.show.clips[index].track = track;
            retimeLinked(p, id, start, duration, offset);
        }); emit selectionChanged();
    }
    dragging_ = false; dragClip_ = -1; dragMarker_ = -1; viewport()->update();
}
void Timeline::mouseDoubleClickEvent(QMouseEvent* event) { if (event->position().x() >= header && !c_.showMode) c_.seek(snap(timeAt(event->position().x()))); }
void Timeline::dragEnterEvent(QDragEnterEvent* event) { if (event->mimeData()->hasFormat("application/x-simpleavshow-asset") && c_.canEdit(false)) event->acceptProposedAction(); }
void Timeline::dragMoveEvent(QDragMoveEvent* event) { if (event->mimeData()->hasFormat("application/x-simpleavshow-asset") && c_.canEdit(false)) event->acceptProposedAction(); }
void Timeline::dropEvent(QDropEvent* event) {
    const int track = trackAt(event->position().y());
    if (event->position().x() < header || track < 0 || static_cast<std::size_t>(track) >= c_.project.show.tracks.size() || !c_.canEdit()) return;
    emit assetDropped(QString::fromUtf8(event->mimeData()->data("application/x-simpleavshow-asset")), track, snap(timeAt(event->position().x())));
    event->acceptProposedAction();
}
}
