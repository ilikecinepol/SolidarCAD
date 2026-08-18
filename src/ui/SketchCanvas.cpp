#include "ui/SketchCanvas.h"

#include <QKeyEvent>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QSignalBlocker>
#include <QVariant>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace solidar {
namespace {

constexpr double kRulerTop = 30.0;
constexpr double kRulerLeft = 44.0;

double niceRulerStep(double pixelsPerMm) {
  const double targetMm = 75.0 / std::max(0.01, pixelsPerMm);
  const double magnitude = std::pow(10.0, std::floor(std::log10(targetMm)));
  const double normalized = targetMm / magnitude;
  const double factor = normalized <= 1.0 ? 1.0 :
                        normalized <= 2.0 ? 2.0 :
                        normalized <= 5.0 ? 5.0 : 10.0;
  return factor * magnitude;
}

double pointSegmentDistance(QPointF point, QPointF start, QPointF end) {
  const QPointF segment = end - start;
  const double lengthSquared = QPointF::dotProduct(segment, segment);
  if (lengthSquared == 0.0) return QLineF(point, start).length();
  const double t = std::clamp(
      QPointF::dotProduct(point - start, segment) / lengthSquared, 0.0, 1.0);
  return QLineF(point, start + segment * t).length();
}

std::optional<sketch::Point> intersectLines(const sketch::Line& first,
                                             const sketch::Line& second) {
  const double ax = first.end.xMm - first.start.xMm;
  const double ay = first.end.yMm - first.start.yMm;
  const double bx = second.end.xMm - second.start.xMm;
  const double by = second.end.yMm - second.start.yMm;
  const double determinant = ax * by - ay * bx;
  if (std::abs(determinant) < 1e-9) return std::nullopt;
  const double dx = second.start.xMm - first.start.xMm;
  const double dy = second.start.yMm - first.start.yMm;
  const double t = (dx * by - dy * bx) / determinant;
  return sketch::Point{first.start.xMm + t * ax,
                       first.start.yMm + t * ay};
}

std::optional<std::pair<sketch::Point, double>> circleThroughThreePoints(
    sketch::Point first, sketch::Point second, sketch::Point third) {
  const double determinant = 2.0 *
      (first.xMm * (second.yMm - third.yMm) +
       second.xMm * (third.yMm - first.yMm) +
       third.xMm * (first.yMm - second.yMm));
  if (std::abs(determinant) < 1e-9) return std::nullopt;
  const double a = first.xMm * first.xMm + first.yMm * first.yMm;
  const double b = second.xMm * second.xMm + second.yMm * second.yMm;
  const double c = third.xMm * third.xMm + third.yMm * third.yMm;
  sketch::Point center{
      (a * (second.yMm - third.yMm) + b * (third.yMm - first.yMm) +
       c * (first.yMm - second.yMm)) / determinant,
      (a * (third.xMm - second.xMm) + b * (first.xMm - third.xMm) +
       c * (second.xMm - first.xMm)) / determinant};
  return std::pair{center, std::hypot(center.xMm - first.xMm,
                                      center.yMm - first.yMm)};
}

double pointLineDistance(sketch::Point point, const sketch::Line& line) {
  const QPointF p(point.xMm, point.yMm);
  return pointSegmentDistance(p, QPointF(line.start.xMm, line.start.yMm),
                              QPointF(line.end.xMm, line.end.yMm));
}

double infiniteLineDistance(sketch::Point point, const sketch::Line& line) {
  const double dx = line.end.xMm - line.start.xMm;
  const double dy = line.end.yMm - line.start.yMm;
  const double length = std::hypot(dx, dy);
  if (length < 1e-9) return std::numeric_limits<double>::max();
  return std::abs(dy * point.xMm - dx * point.yMm +
                  line.end.xMm * line.start.yMm -
                  line.end.yMm * line.start.xMm) / length;
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
  circlePoints_.clear();
  circleGuideLines_.clear();
  rectanglePoints_.clear();
  hideDimensionEditor();
  setCursor(tool == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
  emit toolChanged(tool_);
  update();
}

void SketchCanvas::setRectangleMode(RectangleMode mode) {
  rectangleMode_ = mode;
  anchor_.reset();
  rectanglePoints_.clear();
  hideDimensionEditor();
  update();
}

void SketchCanvas::setCircleMode(CircleMode mode) {
  circleMode_ = mode;
  anchor_.reset();
  circlePoints_.clear();
  circleGuideLines_.clear();
  hideDimensionEditor();
  update();
}

void SketchCanvas::setCircleDiameter(double diameterMm) {
  circleDiameterMm_ = std::max(0.01, diameterMm);
  if (tool_ == Tool::Circle && circleMode_ == CircleMode::CenterRadius &&
      anchor_ && primaryDimension_->isVisible())
    setPrimaryDimension(circleDiameterMm_);
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
  emit lineStyleSelectionChanged(false, false);
  anchor_.reset();
  rectanglePoints_.clear();
  circlePoints_.clear();
  circleGuideLines_.clear();
  notifyGeometryChanged();
}

void SketchCanvas::resetSketch() {
  sketch_.clear();
  undoStack_.clear();
  selectionKind_ = SelectionKind::None;
  emit lineStyleSelectionChanged(false, false);
  anchor_.reset();
  rectanglePoints_.clear();
  circlePoints_.clear();
  circleGuideLines_.clear();
  setProperty("sketchPanX", 0.0);
  setProperty("sketchPanY", 0.0);
  setProperty("sketchPanning", false);
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
  setProperty("sketchPanX", 0.0);
  setProperty("sketchPanY", 0.0);
  setProperty("sketchPanning", false);
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
  emit lineStyleSelectionChanged(false, false);
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
  emit lineStyleSelectionChanged(false, false);
  emit selectionChanged(QString::fromUtf8("Ничего не выбрано"));
  notifyGeometryChanged();
}

QPointF SketchCanvas::mapPoint(sketch::Point point) const {
  const double centerX = kRulerLeft + (width() - kRulerLeft) * 0.5 +
                         property("sketchPanX").toDouble();
  const double centerY = kRulerTop + (height() - kRulerTop) * 0.5 +
                         property("sketchPanY").toDouble();
  return {centerX + point.xMm * pixelsPerMm_,
          centerY - point.yMm * pixelsPerMm_};
}

sketch::Point SketchCanvas::unmapPoint(QPointF point) const {
  const double centerX = kRulerLeft + (width() - kRulerLeft) * 0.5 +
                         property("sketchPanX").toDouble();
  const double centerY = kRulerTop + (height() - kRulerTop) * 0.5 +
                         property("sketchPanY").toDouble();
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

  const QPointF origin = mapPoint({0, 0});
  if (gridVisible_) {
    const double minorGrid = snapStepMm_ * pixelsPerMm_;
    painter.setPen(QPen(QColor("#e8edf5"), 1.0));
    double firstX = kRulerLeft +
                    std::fmod(origin.x() - kRulerLeft, minorGrid);
    if (firstX < kRulerLeft) firstX += minorGrid;
    double firstY = kRulerTop +
                    std::fmod(origin.y() - kRulerTop, minorGrid);
    if (firstY < kRulerTop) firstY += minorGrid;
    for (double x = firstX; x < width(); x += minorGrid)
      painter.drawLine(QPointF(x, kRulerTop), QPointF(x, height()));
    for (double y = firstY; y < height(); y += minorGrid)
      painter.drawLine(QPointF(kRulerLeft, y), QPointF(width(), y));
  }

  painter.fillRect(QRectF(0, 0, width(), kRulerTop), QColor("#f3f6fb"));
  painter.fillRect(QRectF(0, 0, kRulerLeft, height()), QColor("#f3f6fb"));
  painter.setPen(QPen(QColor("#cbd6e6"), 1.0));
  painter.drawLine(QPointF(kRulerLeft, kRulerTop), QPointF(width(), kRulerTop));
  painter.drawLine(QPointF(kRulerLeft, kRulerTop), QPointF(kRulerLeft, height()));
  painter.setPen(QColor("#637797"));
  const double rulerStepMm = niceRulerStep(pixelsPerMm_);
  const double visibleLeftMm = unmapPoint({kRulerLeft, origin.y()}).xMm;
  const double visibleRightMm = unmapPoint({static_cast<double>(width()),
                                             origin.y()}).xMm;
  const double firstHorizontalMm =
      std::ceil(visibleLeftMm / rulerStepMm) * rulerStepMm;
  for (double mm = firstHorizontalMm; mm <= visibleRightMm;
       mm += rulerStepMm) {
    const QPointF xMark = mapPoint({mm, 0});
    painter.drawLine(QPointF(xMark.x(), 20), QPointF(xMark.x(), kRulerTop));
    painter.drawText(QRectF(xMark.x() - 24, 2, 48, 17), Qt::AlignCenter,
                     QString::number(mm, 'f', 0));
  }
  const double visibleTopMm = unmapPoint({origin.x(), kRulerTop}).yMm;
  const double visibleBottomMm =
      unmapPoint({origin.x(), static_cast<double>(height())}).yMm;
  const double firstVerticalMm =
      std::ceil(visibleBottomMm / rulerStepMm) * rulerStepMm;
  for (double mm = firstVerticalMm; mm <= visibleTopMm;
       mm += rulerStepMm) {
    const QPointF yMark = mapPoint({0, mm});
    painter.drawLine(QPointF(34, yMark.y()), QPointF(kRulerLeft, yMark.y()));
    painter.save();
    painter.translate(3, yMark.y() + 22);
    painter.rotate(-90);
    painter.drawText(QRectF(0, 0, 44, 17), Qt::AlignCenter,
                     QString::number(mm, 'f', 0));
    painter.restore();
  }

  painter.save();
  painter.setClipRect(QRectF(kRulerLeft, kRulerTop,
                             width() - kRulerLeft,
                             height() - kRulerTop));

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
      painter.setPen(QPen(QColor("#596570"), 1.6,
                          circle.dashed ? Qt::DashLine : Qt::SolidLine));
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
                        selected ? 3.0 : 2.0,
                        line.dashed ? Qt::DashLine : Qt::SolidLine));
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
                        selected ? 3.0 : 2.0,
                        circle.dashed ? Qt::DashLine : Qt::SolidLine));
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
      if (rectangleMode_ == RectangleMode::FromCenter) {
        const QPointF delta = current - first;
        painter.drawRect(QRectF(first - delta, first + delta).normalized());
      } else {
        painter.drawRect(QRectF(first, current).normalized());
      }
    } else if (tool_ == Tool::Circle) {
      const double radius = QLineF(first, current).length();
      painter.drawEllipse(first, radius, radius);
    }
  }

  if (tool_ == Tool::Circle && circleMode_ != CircleMode::CenterRadius) {
    painter.setPen(QPen(QColor("#0a72ff"), 1.6, Qt::DashLine));
    painter.setBrush(QColor(10, 114, 255, 24));
    for (const auto& line : circleGuideLines_) {
      painter.setPen(QPen(QColor("#ff8a24"), 3.0));
      painter.drawLine(mapPoint(line.start), mapPoint(line.end));
    }
    painter.setPen(QPen(QColor("#0a72ff"), 1.6, Qt::DashLine));
    if (circleMode_ == CircleMode::TwoPoints && circlePoints_.size() == 1) {
      const auto first = circlePoints_.front();
      const sketch::Point center{(first.xMm + hoverPoint_.xMm) * 0.5,
                                 (first.yMm + hoverPoint_.yMm) * 0.5};
      const double radius = std::hypot(hoverPoint_.xMm - first.xMm,
                                       hoverPoint_.yMm - first.yMm) * 0.5 * pixelsPerMm_;
      painter.drawEllipse(mapPoint(center), radius, radius);
    } else if (circleMode_ == CircleMode::ThreePoints &&
               circlePoints_.size() == 2) {
      const auto preview = circleThroughThreePoints(
          circlePoints_[0], circlePoints_[1], hoverPoint_);
      if (preview) {
        const double radius = preview->second * pixelsPerMm_;
        painter.drawEllipse(mapPoint(preview->first), radius, radius);
      }
    }
    painter.setBrush(QColor("#0a72ff"));
    for (const auto& point : circlePoints_)
      painter.drawEllipse(mapPoint(point), 4.0, 4.0);
  }

  if (tool_ == Tool::Rectangle && rectangleMode_ == RectangleMode::ThreePoints &&
      !rectanglePoints_.empty()) {
    painter.setPen(QPen(QColor("#0a72ff"), 1.6, Qt::DashLine));
    painter.setBrush(QColor(10, 114, 255, 24));
    const auto first = rectanglePoints_[0];
    if (rectanglePoints_.size() == 1) {
      painter.drawLine(mapPoint(first), mapPoint(hoverPoint_));
    } else {
      const auto second = rectanglePoints_[1];
      const double dx = second.xMm - first.xMm;
      const double dy = second.yMm - first.yMm;
      const double length = std::hypot(dx, dy);
      if (length > 1e-9) {
        const double nx = -dy / length;
        const double ny = dx / length;
        const double height = (hoverPoint_.xMm-first.xMm)*nx +
                              (hoverPoint_.yMm-first.yMm)*ny;
        const sketch::Point third{second.xMm+nx*height, second.yMm+ny*height};
        const sketch::Point fourth{first.xMm+nx*height, first.yMm+ny*height};
        QPolygonF polygon;
        polygon << mapPoint(first) << mapPoint(second) << mapPoint(third)
                << mapPoint(fourth);
        painter.drawPolygon(polygon);
      }
    }
  }

  painter.setPen(QColor("#536985"));
  painter.drawText(QRectF(kRulerLeft + 12, height() - 30, width() - 70, 22),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QString::fromUtf8("Шаг сетки: %1 мм   •   Привязка: %2   •   Масштаб: %3%")
                       .arg(snapStepMm_)
                       .arg(snapEnabled_ ? QString::fromUtf8("ВКЛ")
                                         : QString::fromUtf8("ВЫКЛ"))
                       .arg(qRound(pixelsPerMm_ / 5.0 * 100.0)));
  painter.restore();
}

void SketchCanvas::mousePressEvent(QMouseEvent* event) {
  setFocus();
  if (event->button() == Qt::MiddleButton) {
    setProperty("sketchPanning", true);
    setProperty("sketchPanLastX", event->position().x());
    setProperty("sketchPanLastY", event->position().y());
    setCursor(Qt::ClosedHandCursor);
    event->accept();
    return;
  }
  if (event->button() == Qt::RightButton) {
    anchor_.reset();
    rectanglePoints_.clear();
    circlePoints_.clear();
    circleGuideLines_.clear();
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
  if (property("sketchPanning").toBool() &&
      (event->buttons() & Qt::MiddleButton)) {
    const QPointF current = event->position();
    const QPointF last(property("sketchPanLastX").toDouble(),
                       property("sketchPanLastY").toDouble());
    const QPointF delta = current - last;
    setProperty("sketchPanX", property("sketchPanX").toDouble() + delta.x());
    setProperty("sketchPanY", property("sketchPanY").toDouble() + delta.y());
    setProperty("sketchPanLastX", current.x());
    setProperty("sketchPanLastY", current.y());
    update();
    event->accept();
    return;
  }
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
  } else if (tool_ == Tool::Circle && circleMode_ != CircleMode::CenterRadius) {
    if (circleMode_ == CircleMode::TwoPoints && circlePoints_.size() == 1) {
      const auto first = circlePoints_.front();
      emit primaryDimensionChanged(std::hypot(hoverPoint_.xMm - first.xMm,
                                               hoverPoint_.yMm - first.yMm));
    } else if (circleMode_ == CircleMode::ThreePoints &&
               circlePoints_.size() == 2) {
      const auto preview = circleThroughThreePoints(
          circlePoints_[0], circlePoints_[1], hoverPoint_);
      if (preview) emit primaryDimensionChanged(preview->second * 2.0);
    }
    update();
  } else if (tool_ == Tool::Rectangle &&
             rectangleMode_ == RectangleMode::ThreePoints &&
             !rectanglePoints_.empty()) {
    update();
  }
}

void SketchCanvas::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::MiddleButton &&
      property("sketchPanning").toBool()) {
    setProperty("sketchPanning", false);
    setCursor(tool_ == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
    event->accept();
    return;
  }
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
  bool dashed = false;
  if (selectionKind_ == SelectionKind::Line) {
    const auto found = std::find_if(
        sketch_.lines().begin(), sketch_.lines().end(), [this](const auto& line) {
          return line.elementId == selectionElementId_;
        });
    dashed = found != sketch_.lines().end() && found->dashed;
  } else if (selectionKind_ == SelectionKind::Circle &&
             selectionIndex_ < sketch_.circles().size()) {
    dashed = sketch_.circles()[selectionIndex_].dashed;
  }
  emit lineStyleSelectionChanged(selectionKind_ != SelectionKind::None, dashed);
  update();
}

void SketchCanvas::commitPoint(sketch::Point point) {
  if (tool_ == Tool::Rectangle && rectangleMode_ == RectangleMode::ThreePoints) {
    commitRectanglePoint(point);
    return;
  }
  if (tool_ == Tool::Circle && circleMode_ != CircleMode::CenterRadius) {
    commitCirclePoint(point);
    return;
  }
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
  else if (tool_ == Tool::Rectangle) {
    if (rectangleMode_ == RectangleMode::FromCenter) {
      const double dx = point.xMm - anchor_->xMm;
      const double dy = point.yMm - anchor_->yMm;
      sketch_.addRectangle({anchor_->xMm-dx, anchor_->yMm-dy},
                           {anchor_->xMm+dx, anchor_->yMm+dy});
    } else {
      sketch_.addRectangle(*anchor_, point);
    }
  }
  else if (tool_ == Tool::Circle) {
    const double radius = std::hypot(point.xMm - anchor_->xMm,
                                     point.yMm - anchor_->yMm);
    sketch_.addCircle(*anchor_, radius);
    circleDiameterMm_ = 2.0 * radius;
    emit primaryDimensionChanged(circleDiameterMm_);
  }
  anchor_.reset();
  hideDimensionEditor();
  notifyGeometryChanged();
}

void SketchCanvas::commitRectanglePoint(sketch::Point point) {
  rectanglePoints_.push_back(point);
  if (rectanglePoints_.size() < 3) { update(); return; }
  const auto first = rectanglePoints_[0];
  const auto second = rectanglePoints_[1];
  const double dx = second.xMm - first.xMm;
  const double dy = second.yMm - first.yMm;
  const double length = std::hypot(dx, dy);
  if (length > 1e-9) {
    const double nx = -dy / length;
    const double ny = dx / length;
    const double height = (point.xMm-first.xMm)*nx +
                          (point.yMm-first.yMm)*ny;
    if (std::abs(height) > 1e-9) {
      const sketch::Point third{second.xMm+nx*height, second.yMm+ny*height};
      const sketch::Point fourth{first.xMm+nx*height, first.yMm+ny*height};
      pushUndoState();
      sketch_.addRectangle(first, second, third, fourth);
      rectanglePoints_.clear();
      notifyGeometryChanged();
      return;
    }
  }
  rectanglePoints_.clear();
  update();
}

void SketchCanvas::commitCirclePoint(sketch::Point point) {
  auto finishCircle = [this](sketch::Point center, double radius) {
    if (!std::isfinite(radius) || radius < 1e-6) return false;
    pushUndoState();
    sketch_.addCircle(center, radius);
    circleDiameterMm_ = 2.0 * radius;
    emit primaryDimensionChanged(circleDiameterMm_);
    circlePoints_.clear();
    circleGuideLines_.clear();
    notifyGeometryChanged();
    return true;
  };

  if (circleMode_ == CircleMode::TwoPoints) {
    circlePoints_.push_back(point);
    if (circlePoints_.size() == 2) {
      const auto first = circlePoints_[0];
      const auto second = circlePoints_[1];
      finishCircle({(first.xMm + second.xMm) * 0.5,
                    (first.yMm + second.yMm) * 0.5},
                   std::hypot(second.xMm - first.xMm,
                              second.yMm - first.yMm) * 0.5);
    }
    update();
    return;
  }
  if (circleMode_ == CircleMode::ThreePoints) {
    circlePoints_.push_back(point);
    if (circlePoints_.size() == 3) {
      const auto result = circleThroughThreePoints(
          circlePoints_[0], circlePoints_[1], circlePoints_[2]);
      if (result) finishCircle(result->first, result->second);
      else circlePoints_.clear();
    }
    update();
    return;
  }

  const auto nearestLine = [this](sketch::Point target)
      -> std::optional<sketch::Line> {
    double best = std::numeric_limits<double>::max();
    std::optional<sketch::Line> result;
    for (const auto& line : sketch_.lines()) {
      if (line.dashed) continue;
      const double distance = pointLineDistance(target, line);
      if (distance < best) {
        best = distance;
        result = line;
      }
    }
    return best <= std::max(3.0, 12.0 / pixelsPerMm_) ? result : std::nullopt;
  };

  if ((circleMode_ == CircleMode::ThreeTangents && circleGuideLines_.size() < 3) ||
      (circleMode_ == CircleMode::TwoTangentsRadius && circleGuideLines_.size() < 2)) {
    const auto line = nearestLine(point);
    if (!line) return;
    const bool duplicate = std::any_of(circleGuideLines_.begin(), circleGuideLines_.end(),
        [&](const auto& existing) { return existing.elementId == line->elementId; });
    if (!duplicate) circleGuideLines_.push_back(*line);
    update();
    if (circleMode_ == CircleMode::TwoTangentsRadius) return;
  }

  if (circleMode_ == CircleMode::ThreeTangents && circleGuideLines_.size() == 3) {
    const auto p0 = intersectLines(circleGuideLines_[0], circleGuideLines_[1]);
    const auto p1 = intersectLines(circleGuideLines_[1], circleGuideLines_[2]);
    const auto p2 = intersectLines(circleGuideLines_[2], circleGuideLines_[0]);
    if (!p0 || !p1 || !p2) { circleGuideLines_.clear(); update(); return; }
    const double a = std::hypot(p1->xMm - p2->xMm, p1->yMm - p2->yMm);
    const double b = std::hypot(p0->xMm - p2->xMm, p0->yMm - p2->yMm);
    const double c = std::hypot(p0->xMm - p1->xMm, p0->yMm - p1->yMm);
    const double sum = a + b + c;
    if (sum > 1e-9) {
      const sketch::Point center{(a*p0->xMm+b*p1->xMm+c*p2->xMm)/sum,
                                 (a*p0->yMm+b*p1->yMm+c*p2->yMm)/sum};
      finishCircle(center, infiniteLineDistance(center, circleGuideLines_[0]));
    }
    return;
  }

  if (circleMode_ == CircleMode::TwoTangentsRadius && circleGuideLines_.size() == 2) {
    circlePoints_.push_back(point);
    const double radius = circleDiameterMm_ * 0.5;
    struct Equation { double nx, ny, c; } equations[2];
    for (int i = 0; i < 2; ++i) {
      const auto& line = circleGuideLines_[i];
      const double dx = line.end.xMm - line.start.xMm;
      const double dy = line.end.yMm - line.start.yMm;
      const double length = std::hypot(dx, dy);
      equations[i] = {-dy / length, dx / length, 0.0};
      equations[i].c = equations[i].nx * line.start.xMm +
                       equations[i].ny * line.start.yMm;
    }
    double best = std::numeric_limits<double>::max();
    std::optional<sketch::Point> center;
    const double det = equations[0].nx * equations[1].ny -
                       equations[0].ny * equations[1].nx;
    if (std::abs(det) > 1e-9) {
      for (double s0 : {-1.0, 1.0}) for (double s1 : {-1.0, 1.0}) {
        const double r0 = equations[0].c + s0 * radius;
        const double r1 = equations[1].c + s1 * radius;
        sketch::Point candidate{(r0*equations[1].ny-equations[0].ny*r1)/det,
                                (equations[0].nx*r1-r0*equations[1].nx)/det};
        const double distance = std::hypot(candidate.xMm-point.xMm,
                                           candidate.yMm-point.yMm);
        if (distance < best) { best = distance; center = candidate; }
      }
    }
    if (center) finishCircle(*center, radius);
    else { circleGuideLines_.clear(); circlePoints_.clear(); update(); }
  }
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
    primaryDimension_->setValue(std::max(
        0.01, std::abs(dx) *
                  (rectangleMode_ == RectangleMode::FromCenter ? 2.0 : 1.0)));
  else
    primaryDimension_->setValue(std::max(0.01, 2.0 * std::hypot(dx, dy)));
  if (secondaryDimension_->isVisible()) {
    secondaryDimension_->setValue(tool_ == Tool::Line
                                      ? std::atan2(dy, dx) * 180.0 / 3.141592653589793
                                      : std::max(
                                            0.01, std::abs(dy) *
                                                      (rectangleMode_ == RectangleMode::FromCenter
                                                           ? 2.0 : 1.0)));
  }
  positionDimensionEditor();
  emit primaryDimensionChanged(primaryDimension_->value());
  // QDoubleSpinBox clears its text selection whenever setValue() changes the
  // displayed number. Keep the active dimension ready for immediate typing.
  if (primaryFocused)
    primaryDimension_->selectAll();
  else if (secondaryFocused)
    secondaryDimension_->selectAll();
}

void SketchCanvas::positionDimensionEditor() {
  if (!anchor_ || !primaryDimension_->isVisible()) return;
  const QPointF anchorPosition = mapPoint(*anchor_);
  const QPointF tipPosition = mapPoint(hoverPoint_);
  QPointF direction = tipPosition - anchorPosition;
  const double length = std::hypot(direction.x(), direction.y());
  direction = length < 1.0 ? QPointF(1.0, 0.5) : direction / length;

  QPoint position =
      (tipPosition + direction * 24.0 + QPointF(8.0, 8.0)).toPoint();
  const int editorHeight = secondaryDimension_->isVisible() ? 82 : 40;
  position.setX(std::clamp(position.x(), static_cast<int>(kRulerLeft + 6),
                           std::max(static_cast<int>(kRulerLeft + 6),
                                    width() - primaryDimension_->width() - 8)));
  position.setY(std::clamp(position.y(), static_cast<int>(kRulerTop + 6),
                           std::max(static_cast<int>(kRulerTop + 6),
                                    height() - editorHeight - 8)));
  primaryDimension_->move(position);
  secondaryDimension_->move(position + QPoint(0, 42));
}

void SketchCanvas::setPrimaryDimension(double value) {
  if (!anchor_ || !primaryDimension_->isVisible() || value <= 0.0) return;
  const double dx = hoverPoint_.xMm - anchor_->xMm;
  const double dy = hoverPoint_.yMm - anchor_->yMm;
  const double angle = std::atan2(dy, dx);
  if (tool_ == Tool::Circle) {
    hoverPoint_ = {anchor_->xMm + value * 0.5 * std::cos(angle),
                   anchor_->yMm + value * 0.5 * std::sin(angle)};
  } else if (tool_ == Tool::Line) {
    hoverPoint_ = {anchor_->xMm + value * std::cos(angle),
                   anchor_->yMm + value * std::sin(angle)};
  } else if (tool_ == Tool::Rectangle) {
    const double extent = rectangleMode_ == RectangleMode::FromCenter
                              ? value * 0.5 : value;
    hoverPoint_.xMm = anchor_->xMm + (dx < 0.0 ? -extent : extent);
  }
  {
    const QSignalBlocker blocker(primaryDimension_);
    primaryDimension_->setValue(value);
  }
  positionDimensionEditor();
  update();
}

void SketchCanvas::setSelectedDashed(bool dashed) {
  if (selectionKind_ == SelectionKind::None) return;
  pushUndoState();
  if (selectionKind_ == SelectionKind::Line)
    sketch_.setElementDashed(selectionElementId_, dashed);
  else
    sketch_.setCircleDashed(selectionIndex_, dashed);
  emit lineStyleSelectionChanged(true, dashed);
  notifyGeometryChanged();
}

void SketchCanvas::commitCurrentDimension() { commitDimensionEditor(); }

void SketchCanvas::setGridVisible(bool visible) {
  gridVisible_ = visible;
  update();
}

void SketchCanvas::setSnapEnabled(bool enabled) {
  snapEnabled_ = enabled;
  update();
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
    if (rectangleMode_ == RectangleMode::FromCenter) {
      const double halfWidth = primaryDimension_->value() * 0.5;
      const double halfHeight = secondaryDimension_->value() * 0.5;
      sketch_.addRectangle({start.xMm-sx*halfWidth, start.yMm-sy*halfHeight},
                           {start.xMm+sx*halfWidth, start.yMm+sy*halfHeight});
    } else {
      sketch_.addRectangle(start, {start.xMm + sx * primaryDimension_->value(),
                                   start.yMm + sy * secondaryDimension_->value()});
    }
  } else if (tool_ == Tool::Circle) {
    sketch_.addCircle(start, primaryDimension_->value() * 0.5);
    circleDiameterMm_ = primaryDimension_->value();
    emit primaryDimensionChanged(circleDiameterMm_);
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
