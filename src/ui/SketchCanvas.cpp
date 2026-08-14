#include "ui/SketchCanvas.h"

#include <QKeyEvent>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace solidar {
namespace {

constexpr double kRulerTop = 30.0;
constexpr double kRulerLeft = 44.0;

double pointSegmentDistance(QPointF point, QPointF start, QPointF end) {
  const QPointF segment = end - start;
  const double lengthSquared = QPointF::dotProduct(segment, segment);
  if (lengthSquared == 0.0) return QLineF(point, start).length();
  const double t = std::clamp(
      QPointF::dotProduct(point - start, segment) / lengthSquared, 0.0, 1.0);
  return QLineF(point, start + segment * t).length();
}

}  // namespace

SketchCanvas::SketchCanvas(QWidget* parent) : QWidget(parent) {
  setMinimumSize(560, 380);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  setCursor(Qt::CrossCursor);
  auto makeDimension = [this]() {
    auto* input = new QDoubleSpinBox(this);
    input->setRange(-100000.0, 100000.0);
    input->setDecimals(2);
    input->setSuffix(QString::fromUtf8(" мм"));
    input->setFixedWidth(122);
    input->setStyleSheet(
        "QDoubleSpinBox{background:white;border:2px solid #0a72ff;"
        "border-radius:6px;padding:5px;color:#12356b;}"
        "QDoubleSpinBox:focus{border-color:#ff8a24;}");
    input->installEventFilter(this);
    input->hide();
    return input;
  };
  primaryDimension_ = makeDimension();
  secondaryDimension_ = makeDimension();
}

void SketchCanvas::setRectangle(double widthMm, double heightMm) {
  pushUndoState();
  sketch_.setRectangle(widthMm, heightMm);
  selectionKind_ = SelectionKind::None;
  anchor_.reset();
  update();
}

void SketchCanvas::setTool(Tool tool) {
  tool_ = tool;
  anchor_.reset();
  hideDimensionEditor();
  setCursor(tool == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
  emit toolChanged(tool_);
  update();
}

SketchCanvas::Tool SketchCanvas::tool() const noexcept { return tool_; }
const sketch::Sketch& SketchCanvas::sketch() const noexcept { return sketch_; }

void SketchCanvas::setReferenceBody(BoxParameters box, const QString& support,
                                    bool visible) {
  referenceBox_ = box;
  referenceSupport_ = support;
  referenceBodyVisible_ = visible;
  update();
}

void SketchCanvas::setReferenceProfile(const sketch::Sketch& profile,
                                       bool visible) {
  referenceProfile_ = profile;
  referenceProfileVisible_ = visible;
  update();
}

void SketchCanvas::clearSketch() {
  if (sketch_.lines().empty() && sketch_.circles().empty()) return;
  pushUndoState();
  sketch_.clear();
  selectionKind_ = SelectionKind::None;
  anchor_.reset();
  notifyGeometryChanged();
}

void SketchCanvas::resetSketch() {
  sketch_.clear();
  undoStack_.clear();
  selectionKind_ = SelectionKind::None;
  anchor_.reset();
  hideDimensionEditor();
  emit undoAvailable(false);
  notifyGeometryChanged();
}

void SketchCanvas::loadSketch(const sketch::Sketch& sketch) {
  hideDimensionEditor();
  sketch_ = sketch;
  undoStack_.clear();
  selectionKind_ = SelectionKind::None;
  anchor_.reset();
  dragging_ = false;
  setTool(Tool::Select);
  notifyGeometryChanged();
  emit undoAvailable(false);
  update();
}

bool SketchCanvas::canUndo() const noexcept { return !undoStack_.empty(); }

void SketchCanvas::undo() {
  if (undoStack_.empty()) return;
  sketch_ = undoStack_.back();
  undoStack_.pop_back();
  selectionKind_ = SelectionKind::None;
  anchor_.reset();
  hideDimensionEditor();
  emit selectionChanged(QString::fromUtf8("Ничего не выбрано"));
  emit undoAvailable(canUndo());
  notifyGeometryChanged();
}

void SketchCanvas::deleteSelection() {
  if (selectionKind_ == SelectionKind::None) return;
  pushUndoState();
  if (selectionKind_ == SelectionKind::Line)
    sketch_.removeElement(selectionElementId_);
  else if (selectionKind_ == SelectionKind::Circle)
    sketch_.removeCircle(selectionIndex_);
  else return;
  selectionKind_ = SelectionKind::None;
  emit selectionChanged(QString::fromUtf8("Ничего не выбрано"));
  notifyGeometryChanged();
}

QPointF SketchCanvas::mapPoint(sketch::Point point) const {
  const double centerX = kRulerLeft + (width() - kRulerLeft) * 0.5;
  const double centerY = kRulerTop + (height() - kRulerTop) * 0.5;
  return {centerX + point.xMm * pixelsPerMm_,
          centerY - point.yMm * pixelsPerMm_};
}

sketch::Point SketchCanvas::unmapPoint(QPointF point) const {
  const double centerX = kRulerLeft + (width() - kRulerLeft) * 0.5;
  const double centerY = kRulerTop + (height() - kRulerTop) * 0.5;
  return {(point.x() - centerX) / pixelsPerMm_,
          (centerY - point.y()) / pixelsPerMm_};
}

sketch::Point SketchCanvas::snappedPoint(QPointF point) const {
  auto result = unmapPoint(point);
  if (snapEnabled_) {
    result.xMm = std::round(result.xMm / snapStepMm_) * snapStepMm_;
    result.yMm = std::round(result.yMm / snapStepMm_) * snapStepMm_;
  }
  return result;
}

void SketchCanvas::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.fillRect(rect(), QColor("#fbfcff"));

  const double minorGrid = snapStepMm_ * pixelsPerMm_;
  const QPointF origin = mapPoint({0, 0});
  painter.setPen(QPen(QColor("#e8edf5"), 1.0));
  for (double x = origin.x(); x < width(); x += minorGrid)
    painter.drawLine(QPointF(x, kRulerTop), QPointF(x, height()));
  for (double x = origin.x() - minorGrid; x > kRulerLeft; x -= minorGrid)
    painter.drawLine(QPointF(x, kRulerTop), QPointF(x, height()));
  for (double y = origin.y(); y < height(); y += minorGrid)
    painter.drawLine(QPointF(kRulerLeft, y), QPointF(width(), y));
  for (double y = origin.y() - minorGrid; y > kRulerTop; y -= minorGrid)
    painter.drawLine(QPointF(kRulerLeft, y), QPointF(width(), y));

  painter.fillRect(QRectF(0, 0, width(), kRulerTop), QColor("#f3f6fb"));
  painter.fillRect(QRectF(0, 0, kRulerLeft, height()), QColor("#f3f6fb"));
  painter.setPen(QPen(QColor("#cbd6e6"), 1.0));
  painter.drawLine(QPointF(kRulerLeft, kRulerTop), QPointF(width(), kRulerTop));
  painter.drawLine(QPointF(kRulerLeft, kRulerTop), QPointF(kRulerLeft, height()));
  painter.setPen(QColor("#637797"));
  for (double mm = -100.0; mm <= 100.0; mm += 20.0) {
    const QPointF xMark = mapPoint({mm, 0});
    painter.drawLine(QPointF(xMark.x(), 20), QPointF(xMark.x(), kRulerTop));
    painter.drawText(QRectF(xMark.x() - 24, 2, 48, 17), Qt::AlignCenter,
                     QString::number(mm, 'f', 0));
    const QPointF yMark = mapPoint({0, mm});
    painter.drawLine(QPointF(34, yMark.y()), QPointF(kRulerLeft, yMark.y()));
    painter.save();
    painter.translate(3, yMark.y() + 22);
    painter.rotate(-90);
    painter.drawText(QRectF(0, 0, 44, 17), Qt::AlignCenter,
                     QString::number(mm, 'f', 0));
    painter.restore();
  }

  painter.setPen(QPen(QColor("#e35c64"), 1.1));
  painter.drawLine(QPointF(kRulerLeft, origin.y()), QPointF(width(), origin.y()));
  painter.setPen(QPen(QColor("#34a26b"), 1.1));
  painter.drawLine(QPointF(origin.x(), kRulerTop), QPointF(origin.x(), height()));

  if (referenceBodyVisible_ && !referenceProfileVisible_) {
    double bodyWidth = referenceBox_.widthMm;
    double bodyHeight = referenceBox_.depthMm;
    if (referenceSupport_.contains("XZ") ||
        referenceSupport_.contains(QString::fromUtf8("Передняя")) ||
        referenceSupport_.contains(QString::fromUtf8("Задняя"))) {
      bodyHeight = referenceBox_.heightMm;
    } else if (referenceSupport_.contains("YZ") ||
               referenceSupport_.contains(QString::fromUtf8("Правая")) ||
               referenceSupport_.contains(QString::fromUtf8("Левая"))) {
      bodyWidth = referenceBox_.depthMm;
      bodyHeight = referenceBox_.heightMm;
    }
    const QRectF bodyRect(mapPoint({-bodyWidth * 0.5, bodyHeight * 0.5}),
                          mapPoint({bodyWidth * 0.5, -bodyHeight * 0.5}));
    painter.setBrush(QColor(126, 138, 150, 75));
    painter.setPen(QPen(QColor("#596570"), 1.6));
    painter.drawRect(bodyRect.normalized());
  }
  if (referenceBodyVisible_ && referenceProfileVisible_) {
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor("#596570"), 1.6));
    for (const auto& line : referenceProfile_.lines())
      painter.drawLine(mapPoint(line.start), mapPoint(line.end));
    for (const auto& circle : referenceProfile_.circles()) {
      const QPointF center = mapPoint(circle.center);
      const double radius = circle.radiusMm * pixelsPerMm_;
      painter.drawEllipse(center, radius, radius);
    }
  }

  for (std::size_t index = 0; index < sketch_.lines().size(); ++index) {
    const auto& line = sketch_.lines()[index];
    const bool selected = selectionKind_ == SelectionKind::Line &&
                          selectionElementId_ == line.elementId;
    painter.setPen(QPen(selected ? QColor("#ff8a24") : QColor("#1469d7"),
                        selected ? 3.0 : 2.0));
    painter.drawLine(mapPoint(line.start), mapPoint(line.end));
    painter.setBrush(Qt::white);
    painter.drawEllipse(mapPoint(line.start), 3.5, 3.5);
    painter.drawEllipse(mapPoint(line.end), 3.5, 3.5);
  }
  for (std::size_t index = 0; index < sketch_.circles().size(); ++index) {
    const auto& circle = sketch_.circles()[index];
    const bool selected = selectionKind_ == SelectionKind::Circle &&
                          selectionIndex_ == index;
    painter.setPen(QPen(selected ? QColor("#ff8a24") : QColor("#1469d7"),
                        selected ? 3.0 : 2.0));
    painter.setBrush(Qt::NoBrush);
    const QPointF center = mapPoint(circle.center);
    const double radius = circle.radiusMm * pixelsPerMm_;
    painter.drawEllipse(center, radius, radius);
    painter.setBrush(Qt::white);
    painter.drawEllipse(center, 3.5, 3.5);
  }

  if (anchor_) {
    painter.setPen(QPen(QColor("#0a72ff"), 1.5, Qt::DashLine));
    painter.setBrush(QColor(10, 114, 255, 25));
    const QPointF first = mapPoint(*anchor_);
    const QPointF current = mapPoint(hoverPoint_);
    if (tool_ == Tool::Line) {
      painter.drawLine(first, current);
    } else if (tool_ == Tool::Rectangle) {
      painter.drawRect(QRectF(first, current).normalized());
    } else if (tool_ == Tool::Circle) {
      const double radius = QLineF(first, current).length();
      painter.drawEllipse(first, radius, radius);
    }
  }

  painter.setPen(QColor("#536985"));
  painter.drawText(QRectF(kRulerLeft + 12, height() - 30, width() - 70, 22),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QString::fromUtf8("Шаг сетки: %1 мм   •   Привязка: ВКЛ   •   Масштаб: %2%")
                       .arg(snapStepMm_)
                       .arg(qRound(pixelsPerMm_ / 5.0 * 100.0)));
}

void SketchCanvas::mousePressEvent(QMouseEvent* event) {
  setFocus();
  if (event->button() == Qt::RightButton) {
    anchor_.reset();
    hideDimensionEditor();
    update();
    return;
  }
  if (event->button() != Qt::LeftButton) return;
  if (tool_ == Tool::Select) {
    selectAt(event->position());
    if (selectionKind_ != SelectionKind::None) {
      pushUndoState();
      dragging_ = true;
      dragPoint_ = snappedPoint(event->position());
    }
  } else
    commitPoint(snappedPoint(event->position()));
}

void SketchCanvas::mouseMoveEvent(QMouseEvent* event) {
  hoverPoint_ = snappedPoint(event->position());
  if (dragging_ && (event->buttons() & Qt::LeftButton)) {
    const auto current = snappedPoint(event->position());
    const double dx = current.xMm - dragPoint_.xMm;
    const double dy = current.yMm - dragPoint_.yMm;
    if (selectionKind_ == SelectionKind::Line)
      sketch_.translateElement(selectionElementId_, dx, dy);
    else if (selectionKind_ == SelectionKind::Circle)
      sketch_.translateCircle(selectionIndex_, dx, dy);
    dragPoint_ = current;
    update();
  } else if (anchor_) {
    updateDimensionEditor();
    update();
  }
}

void SketchCanvas::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && dragging_) {
    dragging_ = false;
    notifyGeometryChanged();
  }
}

void SketchCanvas::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Escape) {
    setTool(Tool::Select);
  } else if (event->key() == Qt::Key_Delete) {
    deleteSelection();
  } else {
    QWidget::keyPressEvent(event);
  }
}

bool SketchCanvas::eventFilter(QObject* watched, QEvent* event) {
  if ((watched == primaryDimension_ || watched == secondaryDimension_) &&
      event->type() == QEvent::KeyPress) {
    const auto* keyEvent = static_cast<QKeyEvent*>(event);
    if (keyEvent->key() == Qt::Key_Tab ||
        keyEvent->key() == Qt::Key_Backtab) {
      if (secondaryDimension_->isVisible()) {
        auto* target = watched == primaryDimension_ ? secondaryDimension_
                                                    : primaryDimension_;
        target->setFocus();
        target->selectAll();
      } else {
        primaryDimension_->setFocus();
        primaryDimension_->selectAll();
      }
      return true;
    }
    if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
      commitDimensionEditor();
      return true;
    }
    if (keyEvent->key() == Qt::Key_Escape) {
      setTool(Tool::Select);
      return true;
    }
  }
  return QWidget::eventFilter(watched, event);
}

void SketchCanvas::wheelEvent(QWheelEvent* event) {
  pixelsPerMm_ = std::clamp(
      pixelsPerMm_ * (event->angleDelta().y() > 0 ? 1.12 : 0.89), 0.5, 30.0);
  update();
}

void SketchCanvas::selectAt(QPointF position) {
  double bestDistance = 9.0;
  selectionKind_ = SelectionKind::None;
  for (std::size_t index = 0; index < sketch_.lines().size(); ++index) {
    const auto& line = sketch_.lines()[index];
    const double distance = pointSegmentDistance(
        position, mapPoint(line.start), mapPoint(line.end));
    if (distance < bestDistance) {
      bestDistance = distance;
      selectionKind_ = SelectionKind::Line;
      selectionIndex_ = index;
      selectionElementId_ = line.elementId;
    }
  }
  for (std::size_t index = 0; index < sketch_.circles().size(); ++index) {
    const auto& circle = sketch_.circles()[index];
    const double distance = std::abs(
        QLineF(position, mapPoint(circle.center)).length() -
        circle.radiusMm * pixelsPerMm_);
    if (distance < bestDistance) {
      bestDistance = distance;
      selectionKind_ = SelectionKind::Circle;
      selectionIndex_ = index;
    }
  }
  if (selectionKind_ == SelectionKind::Line)
    emit selectionChanged(QString::fromUtf8("Объект: Линия"));
  else if (selectionKind_ == SelectionKind::Circle)
    emit selectionChanged(QString::fromUtf8("Объект: Окружность"));
  else
    emit selectionChanged(QString::fromUtf8("Ничего не выбрано"));
  update();
}

void SketchCanvas::commitPoint(sketch::Point point) {
  if (!anchor_) {
    anchor_ = point;
    hoverPoint_ = point;
    showDimensionEditor(mapPoint(point).toPoint() + QPoint(18, 18));
    update();
    return;
  }
  if (tool_ == Tool::Line)
    pushUndoState();
  else if (tool_ == Tool::Rectangle)
    pushUndoState();
  else if (tool_ == Tool::Circle)
    pushUndoState();
  if (tool_ == Tool::Line)
    sketch_.addLine(*anchor_, point);
  else if (tool_ == Tool::Rectangle)
    sketch_.addRectangle(*anchor_, point);
  else if (tool_ == Tool::Circle)
    sketch_.addCircle(*anchor_, std::hypot(point.xMm - anchor_->xMm,
                                           point.yMm - anchor_->yMm));
  anchor_.reset();
  hideDimensionEditor();
  notifyGeometryChanged();
}

void SketchCanvas::showDimensionEditor(QPoint position) {
  primaryDimension_->move(position);
  secondaryDimension_->move(position + QPoint(0, 42));
  primaryDimension_->setRange(0.01, 100000.0);
  secondaryDimension_->setRange(-360.0, 100000.0);
  if (tool_ == Tool::Line) {
    primaryDimension_->setPrefix(QString::fromUtf8("L: "));
    secondaryDimension_->setPrefix(QString::fromUtf8("Угол: "));
    secondaryDimension_->setSuffix(QString::fromUtf8(" °"));
    secondaryDimension_->show();
  } else if (tool_ == Tool::Rectangle) {
    primaryDimension_->setPrefix(QString::fromUtf8("W: "));
    secondaryDimension_->setPrefix(QString::fromUtf8("H: "));
    secondaryDimension_->setSuffix(QString::fromUtf8(" мм"));
    secondaryDimension_->setRange(0.01, 100000.0);
    secondaryDimension_->show();
  } else {
    primaryDimension_->setPrefix(QString::fromUtf8("Ø: "));
    secondaryDimension_->hide();
  }
  primaryDimension_->show();
  updateDimensionEditor();
  primaryDimension_->setFocus();
  primaryDimension_->selectAll();
}

void SketchCanvas::updateDimensionEditor() {
  if (!anchor_ || !primaryDimension_->isVisible()) return;
  const double dx = hoverPoint_.xMm - anchor_->xMm;
  const double dy = hoverPoint_.yMm - anchor_->yMm;
  const bool primaryFocused = primaryDimension_->hasFocus();
  const bool secondaryFocused = secondaryDimension_->hasFocus();
  if (tool_ == Tool::Line)
    primaryDimension_->setValue(std::max(0.01, std::hypot(dx, dy)));
  else if (tool_ == Tool::Rectangle)
    primaryDimension_->setValue(std::max(0.01, std::abs(dx)));
  else
    primaryDimension_->setValue(std::max(0.01, 2.0 * std::hypot(dx, dy)));
  if (secondaryDimension_->isVisible()) {
    secondaryDimension_->setValue(tool_ == Tool::Line
                                      ? std::atan2(dy, dx) * 180.0 / 3.141592653589793
                                      : std::max(0.01, std::abs(dy)));
  }
  // QDoubleSpinBox clears its text selection whenever setValue() changes the
  // displayed number. Keep the active dimension ready for immediate typing.
  if (primaryFocused)
    primaryDimension_->selectAll();
  else if (secondaryFocused)
    secondaryDimension_->selectAll();
}

void SketchCanvas::commitDimensionEditor() {
  if (!anchor_) return;
  pushUndoState();
  const auto start = *anchor_;
  if (tool_ == Tool::Line) {
    const double angle = secondaryDimension_->value() * 3.141592653589793 / 180.0;
    sketch_.addLine(start, {start.xMm + primaryDimension_->value() * std::cos(angle),
                            start.yMm + primaryDimension_->value() * std::sin(angle)});
  } else if (tool_ == Tool::Rectangle) {
    const double sx = hoverPoint_.xMm < start.xMm ? -1.0 : 1.0;
    const double sy = hoverPoint_.yMm < start.yMm ? -1.0 : 1.0;
    sketch_.addRectangle(start, {start.xMm + sx * primaryDimension_->value(),
                                 start.yMm + sy * secondaryDimension_->value()});
  } else if (tool_ == Tool::Circle) {
    sketch_.addCircle(start, primaryDimension_->value() * 0.5);
  }
  anchor_.reset();
  hideDimensionEditor();
  setFocus();
  notifyGeometryChanged();
}

void SketchCanvas::hideDimensionEditor() {
  primaryDimension_->hide();
  secondaryDimension_->hide();
}

void SketchCanvas::notifyGeometryChanged() {
  emit geometryChanged(sketch_.widthMm(), sketch_.heightMm());
  update();
}

void SketchCanvas::pushUndoState() {
  undoStack_.push_back(sketch_);
  constexpr std::size_t maxUndoSteps = 100;
  if (undoStack_.size() > maxUndoSteps) undoStack_.erase(undoStack_.begin());
  emit undoAvailable(true);
}

}  // namespace solidar
