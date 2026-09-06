#include "ui/HistoryTimelineWidget.h"

#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace solidar {

int HistoryTimelineGeometry::nearestIndex(int x) const noexcept {
  if (stepCount <= 0) return -1;
  return std::clamp(static_cast<int>(std::lround(
      static_cast<double>(x - center(0)) / pitch())), 0, stepCount - 1);
}

HistoryTimelineWidget::HistoryTimelineWidget(QWidget* parent) : QWidget(parent) {
  setFixedHeight(58);
  layout_ = new QHBoxLayout(this);
  layout_->setContentsMargins(12, 3, 12, 27);
  layout_->setSpacing(8);
  layout_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  setMouseTracking(true);
}

QHBoxLayout* HistoryTimelineWidget::stepLayout() const noexcept { return layout_; }

void HistoryTimelineWidget::setStepCount(int count) {
  geometry_.stepCount = std::max(0, count);
  position_ = std::clamp(position_, 0, geometry_.stepCount);
  setMinimumWidth(geometry_.contentWidth());
  update();
}

void HistoryTimelineWidget::setPosition(int position) {
  position_ = std::clamp(position, 0, geometry_.stepCount);
  update();
}

int HistoryTimelineWidget::position() const noexcept { return position_; }

void HistoryTimelineWidget::paintEvent(QPaintEvent* event) {
  QWidget::paintEvent(event);
  if (!geometry_.stepCount) return;
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const int y = 43;
  painter.setPen(QPen(QColor("#9db3cf"), 2));
  painter.drawLine(geometry_.trackStart(), y, geometry_.trackEnd(), y);
  for (int i = 0; i < geometry_.stepCount; ++i)
    painter.drawEllipse(QPoint(geometry_.center(i), y), 2, 2);
  if (position_ > 0) {
    const int x = geometry_.center(position_ - 1);
    painter.setPen(Qt::NoPen); painter.setBrush(QColor("#1671e8"));
    painter.drawPolygon(QPolygon({QPoint(x, y - 8), QPoint(x - 5, y + 1),
                                  QPoint(x + 5, y + 1)}));
  }
}

void HistoryTimelineWidget::selectAt(int x) {
  const int index = geometry_.nearestIndex(x);
  if (index < 0) return;
  const int next = index + 1;
  if (next != position_) { position_ = next; emit positionChanged(next); }
  update();
}

void HistoryTimelineWidget::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    dragging_ = true; selectAt(event->position().x()); event->accept(); return;
  }
  QWidget::mousePressEvent(event);
}
void HistoryTimelineWidget::mouseMoveEvent(QMouseEvent* event) {
  if (dragging_) { selectAt(event->position().x()); event->accept(); return; }
  QWidget::mouseMoveEvent(event);
}
void HistoryTimelineWidget::mouseReleaseEvent(QMouseEvent* event) {
  if (dragging_ && event->button() == Qt::LeftButton) {
    selectAt(event->position().x()); dragging_ = false; event->accept(); return;
  }
  QWidget::mouseReleaseEvent(event);
}

}  // namespace solidar
