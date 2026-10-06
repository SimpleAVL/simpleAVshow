#include "app/ProgramView.h"
#include <QPainter>

namespace sav {
ProgramView::ProgramView(QWidget* parent) : QWidget(parent) { setMinimumSize(160, 90); setAutoFillBackground(false); }
void ProgramView::setPicture(QImage picture) { if (picture.cacheKey() != picture_.cacheKey()) { picture_ = std::move(picture); update(); } }
void ProgramView::paintEvent(QPaintEvent*) {
    QPainter p(this); p.fillRect(rect(), Qt::black);
    if (picture_.isNull()) return;
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QSize size = picture_.size().scaled(this->size(), Qt::KeepAspectRatio);
    p.drawImage(QRect((width() - size.width()) / 2, (height() - size.height()) / 2, size.width(), size.height()), picture_);
}
}
