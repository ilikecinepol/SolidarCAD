#include <array>
#include "ui/SketchCanvas.h"
#include "sketch/SketchSolver.h"

#include <QKeySequence>
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

#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>
#include <limits>

namespace solidar {
namespace {
struct SketchClipboardElement {
  enum class Kind { Line, Rectangle, Circle };

  Kind kind{Kind::Line};
  sketch::Point lineStart{};
  sketch::Point lineEnd{};
  std::array<sketch::Point, 4> rectanglePoints{};
  sketch::Point circleCenter{};
  double circleRadiusMm{0.0};
};

struct SketchClipboardData {
  std::vector<SketchClipboardElement> elements;
  int pasteGeneration{0};

  [[nodiscard]] bool empty() const noexcept { return elements.empty(); }

  void clear() {
    elements.clear();
    pasteGeneration = 0;
  }
};

SketchClipboardData gSketchClipboard;


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

double lineAngleDegrees(const sketch::Line& first,
                        const sketch::Line& second) {
  const auto samePoint = [](sketch::Point a, sketch::Point b) {
    return std::hypot(a.xMm - b.xMm, a.yMm - b.yMm) <= 1e-7;
  };

  double ax = first.end.xMm - first.start.xMm;
  double ay = first.end.yMm - first.start.yMm;
  double bx = second.end.xMm - second.start.xMm;
  double by = second.end.yMm - second.start.yMm;

  // For connected segments, an angular dimension must describe the two
  // visible rays leaving their common CAD vertex, regardless of each line's
  // internal start/end order.
  if (samePoint(first.start, second.start)) {
    ax = first.end.xMm - first.start.xMm;
    ay = first.end.yMm - first.start.yMm;
    bx = second.end.xMm - second.start.xMm;
    by = second.end.yMm - second.start.yMm;
  } else if (samePoint(first.start, second.end)) {
    ax = first.end.xMm - first.start.xMm;
    ay = first.end.yMm - first.start.yMm;
    bx = second.start.xMm - second.end.xMm;
    by = second.start.yMm - second.end.yMm;
  } else if (samePoint(first.end, second.start)) {
    ax = first.start.xMm - first.end.xMm;
    ay = first.start.yMm - first.end.yMm;
    bx = second.end.xMm - second.start.xMm;
    by = second.end.yMm - second.start.yMm;
  } else if (samePoint(first.end, second.end)) {
    ax = first.start.xMm - first.end.xMm;
    ay = first.start.yMm - first.end.yMm;
    bx = second.start.xMm - second.end.xMm;
    by = second.start.yMm - second.end.yMm;
  }

  const double al = std::hypot(ax, ay);
  const double bl = std::hypot(bx, by);
  if (al <= 1e-9 || bl <= 1e-9) return 0.0;

  const double cosine =
      std::clamp((ax * bx + ay * by) / (al * bl), -1.0, 1.0);
  return std::acos(cosine) * 180.0 / 3.14159265358979323846;
}

template <typename MapPointFn>
std::optional<QPointF> lineIntersectionScreen(const sketch::Line& first,
                                              const sketch::Line& second,
                                              MapPointFn&& mapPointFn) {
  const double ax = first.end.xMm - first.start.xMm;
  const double ay = first.end.yMm - first.start.yMm;
  const double bx = second.end.xMm - second.start.xMm;
  const double by = second.end.yMm - second.start.yMm;
  const double determinant = ax * by - ay * bx;
  if (std::abs(determinant) <= 1e-9) return std::nullopt;

  const double dx = second.start.xMm - first.start.xMm;
  const double dy = second.start.yMm - first.start.yMm;
  const double t = (dx * by - dy * bx) / determinant;

  return mapPointFn(sketch::Point{
      first.start.xMm + t * ax,
      first.start.yMm + t * ay});
}
// ANGLE DIMENSION RAY TOWARD SEGMENT
QPointF angleRayTowardSegment(QPointF intersection,
                              QPointF start,
                              QPointF end) {
  const QPointF toStart = start - intersection;
  const QPointF toEnd = end - intersection;

  const double startLength =
      std::hypot(toStart.x(), toStart.y());
  const double endLength =
      std::hypot(toEnd.x(), toEnd.y());

  constexpr double epsilon = 1e-6;

  if (startLength <= epsilon && endLength <= epsilon)
    return {};
  if (startLength <= epsilon)
    return toEnd;
  if (endLength <= epsilon)
    return toStart;

  const double dot = QPointF::dotProduct(toStart, toEnd);

  if (dot >= 0.0)
    return startLength <= endLength ? toStart : toEnd;

  return startLength >= endLength ? toStart : toEnd;
}

// VISIBLE ANGLE BETWEEN FINITE SEGMENTS
double visibleLineAngleDegrees(const sketch::Line& first,
                               const sketch::Line& second) {
  const auto samePoint = [](sketch::Point a, sketch::Point b) {
    return std::hypot(a.xMm - b.xMm,
                      a.yMm - b.yMm) <= 1e-7;
  };

  const double ax = first.end.xMm - first.start.xMm;
  const double ay = first.end.yMm - first.start.yMm;
  const double bx = second.end.xMm - second.start.xMm;
  const double by = second.end.yMm - second.start.yMm;

  const double determinant = ax * by - ay * bx;
  if (std::abs(determinant) <= 1e-9)
    return 0.0;

  const double dx = second.start.xMm - first.start.xMm;
  const double dy = second.start.yMm - first.start.yMm;
  const double t = (dx * by - dy * bx) / determinant;

  const sketch::Point intersection{
      first.start.xMm + t * ax,
      first.start.yMm + t * ay};

  const auto rayTowardSegment =
      [intersection](sketch::Point start,
                     sketch::Point end) {
        QPointF toStart(start.xMm - intersection.xMm,
                        start.yMm - intersection.yMm);
        QPointF toEnd(end.xMm - intersection.xMm,
                      end.yMm - intersection.yMm);

        const double startLength =
            std::hypot(toStart.x(), toStart.y());
        const double endLength =
            std::hypot(toEnd.x(), toEnd.y());

        constexpr double epsilon = 1e-9;

        if (startLength <= epsilon)
          return toEnd;
        if (endLength <= epsilon)
          return toStart;

        const double dot =
            QPointF::dotProduct(toStart, toEnd);

        if (dot >= 0.0)
          return startLength <= endLength ? toStart : toEnd;

        return startLength >= endLength ? toStart : toEnd;
      };

  QPointF firstDirection =
      rayTowardSegment(first.start, first.end);
  QPointF secondDirection =
      rayTowardSegment(second.start, second.end);

  if (samePoint(first.start, second.start)) {
    firstDirection =
        QPointF(first.end.xMm - first.start.xMm,
                first.end.yMm - first.start.yMm);
    secondDirection =
        QPointF(second.end.xMm - second.start.xMm,
                second.end.yMm - second.start.yMm);
  } else if (samePoint(first.start, second.end)) {
    firstDirection =
        QPointF(first.end.xMm - first.start.xMm,
                first.end.yMm - first.start.yMm);
    secondDirection =
        QPointF(second.start.xMm - second.end.xMm,
                second.start.yMm - second.end.yMm);
  } else if (samePoint(first.end, second.start)) {
    firstDirection =
        QPointF(first.start.xMm - first.end.xMm,
                first.start.yMm - first.end.yMm);
    secondDirection =
        QPointF(second.end.xMm - second.start.xMm,
                second.end.yMm - second.start.yMm);
  } else if (samePoint(first.end, second.end)) {
    firstDirection =
        QPointF(first.start.xMm - first.end.xMm,
                first.start.yMm - first.end.yMm);
    secondDirection =
        QPointF(second.start.xMm - second.end.xMm,
                second.start.yMm - second.end.yMm);
  }

  const double firstLength =
      std::hypot(firstDirection.x(), firstDirection.y());
  const double secondLength =
      std::hypot(secondDirection.x(), secondDirection.y());

  if (firstLength <= 1e-9 || secondLength <= 1e-9)
    return 0.0;

  const double cosine =
      std::clamp(
          QPointF::dotProduct(firstDirection, secondDirection) /
              (firstLength * secondLength),
          -1.0,
          1.0);

  return std::acos(cosine) *
         180.0 / 3.14159265358979323846;
}
std::optional<QPointF> nearestSegmentEndpointTo(
    QPointF point, QPointF start, QPointF end) {
  const double firstDistance = QLineF(point, start).length();
  const double secondDistance = QLineF(point, end).length();

  if (firstDistance <= 0.5 || secondDistance <= 0.5)
    return std::nullopt;

  return firstDistance <= secondDistance
             ? std::optional<QPointF>{start}
             : std::optional<QPointF>{end};
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

struct TwoTangentCirclePreview {
  sketch::Point center;
  double radiusMm{};
};

std::optional<TwoTangentCirclePreview>
twoTangentCircleForRadius(
    const sketch::Line& first,
    const sketch::Line& second,
    double radiusMm,
    sketch::Point hint) {
  if (!std::isfinite(radiusMm) ||
      radiusMm <= 1e-6)
    return std::nullopt;

  const double firstDx =
      first.end.xMm - first.start.xMm;
  const double firstDy =
      first.end.yMm - first.start.yMm;
  const double secondDx =
      second.end.xMm - second.start.xMm;
  const double secondDy =
      second.end.yMm - second.start.yMm;

  const double firstLengthSquared =
      firstDx * firstDx + firstDy * firstDy;
  const double secondLengthSquared =
      secondDx * secondDx + secondDy * secondDy;

  if (firstLengthSquared <= 1e-12 ||
      secondLengthSquared <= 1e-12)
    return std::nullopt;

  const double firstLength =
      std::sqrt(firstLengthSquared);
  const double secondLength =
      std::sqrt(secondLengthSquared);

  const double n1x = -firstDy / firstLength;
  const double n1y = firstDx / firstLength;
  const double n2x = -secondDy / secondLength;
  const double n2y = secondDx / secondLength;

  const double determinant =
      n1x * n2y - n1y * n2x;

  if (std::abs(determinant) <= 1e-10)
    return std::nullopt;

  const double c1 =
      n1x * first.start.xMm +
      n1y * first.start.yMm;
  const double c2 =
      n2x * second.start.xMm +
      n2y * second.start.yMm;

  std::optional<TwoTangentCirclePreview> best;
  double bestMovement =
      std::numeric_limits<double>::max();

  for (const double s1 : {-1.0, 1.0}) {
    for (const double s2 : {-1.0, 1.0}) {
      const double r1 = c1 + s1 * radiusMm;
      const double r2 = c2 + s2 * radiusMm;

      const sketch::Point center{
          (r1 * n2y - n1y * r2) / determinant,
          (n1x * r2 - r1 * n2x) / determinant};

      if (!std::isfinite(center.xMm) ||
          !std::isfinite(center.yMm))
        continue;

      // TWO-TANGENT FINITE SEGMENT DIAMETER LIMIT
      const double firstT =
          ((center.xMm - first.start.xMm) * firstDx +
           (center.yMm - first.start.yMm) * firstDy) /
          firstLengthSquared;

      const double secondT =
          ((center.xMm - second.start.xMm) * secondDx +
           (center.yMm - second.start.yMm) * secondDy) /
          secondLengthSquared;

      constexpr double finiteTolerance = 1e-8;

      if (firstT < -finiteTolerance ||
          firstT > 1.0 + finiteTolerance ||
          secondT < -finiteTolerance ||
          secondT > 1.0 + finiteTolerance)
        continue;

      const double movement =
          std::hypot(
              center.xMm - hint.xMm,
              center.yMm - hint.yMm);

      if (movement < bestMovement) {
        bestMovement = movement;
        best = TwoTangentCirclePreview{
            center,
            radiusMm};
      }
    }
  }

  return best;
}

std::optional<TwoTangentCirclePreview>
clampedTwoTangentCircleForRadius(
    const sketch::Line& first,
    const sketch::Line& second,
    double requestedRadiusMm,
    sketch::Point hint) {
  const double requested =
      std::max(0.01, requestedRadiusMm);

  if (const auto exact =
          twoTangentCircleForRadius(
              first,
              second,
              requested,
              hint))
    return exact;

  double low = 0.01;
  double high = requested;

  std::optional<TwoTangentCirclePreview> best =
      twoTangentCircleForRadius(
          first,
          second,
          low,
          hint);

  if (!best)
    return std::nullopt;

  for (int iteration = 0;
       iteration < 48;
       ++iteration) {
    const double mid =
        (low + high) * 0.5;

    const auto candidate =
        twoTangentCircleForRadius(
            first,
            second,
            mid,
            hint);

    if (candidate) {
      low = mid;
      best = candidate;
    } else {
      high = mid;
    }
  }

  return best;
}
std::optional<TwoTangentCirclePreview>
twoTangentCircleFromCursor(
    const sketch::Line& first,
    const sketch::Line& second,
    sketch::Point cursor) {
  const double firstDx =
      first.end.xMm - first.start.xMm;
  const double firstDy =
      first.end.yMm - first.start.yMm;
  const double secondDx =
      second.end.xMm - second.start.xMm;
  const double secondDy =
      second.end.yMm - second.start.yMm;

  const double firstLength =
      std::hypot(firstDx, firstDy);
  const double secondLength =
      std::hypot(secondDx, secondDy);

  if (firstLength <= 1e-9 ||
      secondLength <= 1e-9)
    return std::nullopt;

  const double n1x = -firstDy / firstLength;
  const double n1y = firstDx / firstLength;
  const double n2x = -secondDy / secondLength;
  const double n2y = secondDx / secondLength;

  const double determinant =
      n1x * n2y - n1y * n2x;

  if (std::abs(determinant) <= 1e-10)
    return std::nullopt;

  const double c1 =
      n1x * first.start.xMm +
      n1y * first.start.yMm;
  const double c2 =
      n2x * second.start.xMm +
      n2y * second.start.yMm;

  const sketch::Point base{
      (c1 * n2y - n1y * c2) /
          determinant,
      (n1x * c2 - c1 * n2x) /
          determinant};

  std::optional<TwoTangentCirclePreview> best;
  double bestScore =
      std::numeric_limits<double>::max();

  for (const double s1 : {-1.0, 1.0}) {
    for (const double s2 : {-1.0, 1.0}) {
      const sketch::Point direction{
          (s1 * n2y - n1y * s2) /
              determinant,
          (n1x * s2 - s1 * n2x) /
              determinant};

      const double directionSquared =
          direction.xMm * direction.xMm +
          direction.yMm * direction.yMm;

      if (directionSquared <= 1e-12)
        continue;

      const double cursorX =
          cursor.xMm - base.xMm;
      const double cursorY =
          cursor.yMm - base.yMm;

      double radiusMm =
          (cursorX * direction.xMm +
           cursorY * direction.yMm) /
          directionSquared;

      radiusMm =
          std::max(0.01, radiusMm);

      const sketch::Point center{
          base.xMm +
              direction.xMm * radiusMm,
          base.yMm +
              direction.yMm * radiusMm};

      double score =
          std::hypot(
              center.xMm - cursor.xMm,
              center.yMm - cursor.yMm);

      const auto finitePenalty =
          [center](const sketch::Line& line) {
            const double dx =
                line.end.xMm - line.start.xMm;
            const double dy =
                line.end.yMm - line.start.yMm;
            const double lengthSquared =
                dx * dx + dy * dy;

            if (lengthSquared <= 1e-12)
              return 1000000.0;

            const double t =
                ((center.xMm -
                      line.start.xMm) *
                     dx +
                 (center.yMm -
                      line.start.yMm) *
                     dy) /
                lengthSquared;

            if (t < 0.0)
              return -t *
                     std::sqrt(lengthSquared);

            if (t > 1.0)
              return (t - 1.0) *
                     std::sqrt(lengthSquared);

            return 0.0;
          };

      score +=
          0.25 *
          (finitePenalty(first) +
           finitePenalty(second));

      if (score < bestScore) {
        bestScore = score;
        best =
            TwoTangentCirclePreview{
                center,
                radiusMm};
      }
    }
  }

  return best;
}

// TWO-TANGENT LIVE DIAMETER PREVIEW

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
  clearGeometrySelection();
  anchor_.reset();
  update();
}

void SketchCanvas::setTool(Tool tool) {
  setProperty("twoTangentRadiusPreviewActive", false);
  tool_ = tool;
  anchor_.reset();
  setProperty("dragPointLineId", QVariant());
  setProperty("dragPointStart", QVariant());
  setProperty("draggingDimensionLine", QVariant());
  setProperty("draggingDimensionLabel", QVariant());
  coincidentFirstPoint_.reset();
  circlePoints_.clear();
  circleGuideLines_.clear();
  rectanglePoints_.clear();
  selectionBoxActive_ = false;
  setProperty("autoDimensionTarget", QVariant());
  setProperty("autoDimensionFirstLine", QVariant());
  setProperty("autoDimensionFirstStart", QVariant());
  setProperty(
      "autoDimensionFirstElementCenter",
      static_cast<qulonglong>(0));
  setProperty(
      "autoDimensionSecondElementCenter",
      static_cast<qulonglong>(0));
  setProperty(
      "autoDimensionFirstCircle",
      static_cast<qulonglong>(
          sketch::kInvalidGeometryId));
  setProperty(
      "autoDimensionSecondCircle",
      static_cast<qulonglong>(
          sketch::kInvalidGeometryId));
  setProperty("autoDimensionOffsetMm", QVariant());
  setProperty("autoDimensionAngleRad", QVariant());
  setProperty("autoDimensionPointMode", QVariant());
  setProperty("autoDimensionDirectLineId", QVariant());
  setProperty("autoDimensionAngleFirstLine", QVariant());
  setProperty("autoDimensionAngleSecondLine", QVariant());
  setProperty("pointOnLineCarrier", QVariant());
  setProperty("pointOnCircleCarrier", QVariant());
  setProperty("perpendicularFirstLine", QVariant());
  setProperty("parallelFirstLine", QVariant());
  setProperty("equalFirstGeometry", QVariant());
  setProperty("equalFirstKind", QVariant());
  setProperty("tangentFirstGeometry", QVariant());
  setProperty("tangentFirstKind", QVariant());
  setProperty("editingDimensionIndex", QVariant());
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
  setProperty("twoTangentRadiusPreviewActive", false);
  circleMode_ = mode;
  anchor_.reset();
  coincidentFirstPoint_.reset();
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

std::vector<SketchCanvas::ConstraintPanelEntry>
SketchCanvas::selectedConstraintPanelEntries() const {
  sketch::GeometryId selectedId = sketch::kInvalidGeometryId;
  if (selectionKind_ == SelectionKind::Line)
    selectedId = selectionLineId_;
  else if (selectionKind_ == SelectionKind::Circle)
    selectedId = selectionCircleId_;

  if (selectedId == sketch::kInvalidGeometryId) return {};

  const auto typeName = [](sketch::ConstraintType type) -> QString {
    switch (type) {
      case sketch::ConstraintType::Horizontal:
        return QString::fromUtf8("Горизонтально");
      case sketch::ConstraintType::Vertical:
        return QString::fromUtf8("Вертикально");
      case sketch::ConstraintType::Coincident:
        return QString::fromUtf8("Совпадение");
      case sketch::ConstraintType::PointOnLine:
        return QString::fromUtf8("Принадлежность");
      case sketch::ConstraintType::Distance:
        return QString::fromUtf8("Расстояние");
      case sketch::ConstraintType::DistanceX:
        return QString::fromUtf8("Расстояние X");
      case sketch::ConstraintType::DistanceY:
        return QString::fromUtf8("Расстояние Y");
      case sketch::ConstraintType::Length:
        return QString::fromUtf8("Длина");
      case sketch::ConstraintType::Radius:
        return QString::fromUtf8("Радиус");
      case sketch::ConstraintType::Diameter:
        return QString::fromUtf8("Диаметр");
      case sketch::ConstraintType::Parallel:
        return QString::fromUtf8("Параллельно");
      case sketch::ConstraintType::Perpendicular:
        return QString::fromUtf8("Перпендикулярно");
      case sketch::ConstraintType::Equal:
        return QString::fromUtf8("Равенство");
      case sketch::ConstraintType::Angle:
        return QString::fromUtf8("Угол");
      case sketch::ConstraintType::Tangent:
        return QString::fromUtf8("\xD0\x9A\xD0\xB0\xD1\x81\xD0\xB0\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xBD\xD0\xBE");
    }
    return QString::fromUtf8("Ограничение");
  };

  const auto samePoint = [](sketch::PointReference first,
                            sketch::PointReference second) {
    // CRASH-FREE 05: COMPLETE POINTREFERENCE IDENTITY
    if (first.elementCenterId != 0 ||
        second.elementCenterId != 0) {
      return first.elementCenterId != 0 &&
             second.elementCenterId != 0 &&
             first.elementCenterId == second.elementCenterId;
    }

    if (first.circleId != sketch::kInvalidGeometryId ||
        second.circleId != sketch::kInvalidGeometryId) {
      return first.circleId != sketch::kInvalidGeometryId &&
             second.circleId != sketch::kInvalidGeometryId &&
             first.circleId == second.circleId;
    }

    if (first.lineId == sketch::kInvalidGeometryId ||
        second.lineId == sketch::kInvalidGeometryId)
      return false;

    return first.lineId == second.lineId &&
           first.start == second.start;
  };

  const auto samePointPair = [&samePoint](
                                 const sketch::Constraint& constraint,
                                 const sketch::Dimension& dimension) {
    const bool sameOrder =
        samePoint(constraint.firstPoint, dimension.firstPoint) &&
        samePoint(constraint.secondPoint, dimension.secondPoint);
    const bool reverseOrder =
        samePoint(constraint.firstPoint, dimension.secondPoint) &&
        samePoint(constraint.secondPoint, dimension.firstPoint);
    return sameOrder || reverseOrder;
  };

  const auto constraintMatchesDimension =
      [&samePointPair](const sketch::Constraint& constraint,
                       const sketch::Dimension& dimension) {
        switch (dimension.kind) {
          case sketch::DimensionKind::LineLength:
            return constraint.type == sketch::ConstraintType::Length &&
                   constraint.firstGeometry == dimension.geometryId;
          case sketch::DimensionKind::CircleDiameter:
            return constraint.type == sketch::ConstraintType::Diameter &&
                   constraint.firstGeometry == dimension.geometryId;
          case sketch::DimensionKind::PointDistance:
            return constraint.type == sketch::ConstraintType::Distance &&
                   samePointPair(constraint, dimension);
          case sketch::DimensionKind::PointDistanceX:
            return constraint.type == sketch::ConstraintType::DistanceX &&
                   samePointPair(constraint, dimension);
          case sketch::DimensionKind::PointDistanceY:
            return constraint.type == sketch::ConstraintType::DistanceY &&
                   samePointPair(constraint, dimension);
          case sketch::DimensionKind::LineAngle: {
            if (constraint.type != sketch::ConstraintType::Angle) return false;
            const bool sameOrder =
                constraint.firstGeometry == dimension.geometryId &&
                constraint.secondGeometry == dimension.secondPoint.lineId;
            const bool reverseOrder =
                constraint.firstGeometry == dimension.secondPoint.lineId &&
                constraint.secondGeometry == dimension.geometryId;
            return sameOrder || reverseOrder;
          }
        }
        return false;
      };

  const auto dimensionReferencesSelected =
      [selectedId](const sketch::Dimension& dimension) {
        if (dimension.kind == sketch::DimensionKind::LineLength ||
            dimension.kind == sketch::DimensionKind::CircleDiameter)
          return dimension.geometryId == selectedId;
        if (dimension.kind == sketch::DimensionKind::LineAngle)
          return dimension.geometryId == selectedId ||
                 dimension.secondPoint.lineId == selectedId;

        return dimension.firstPoint.lineId == selectedId ||
               dimension.secondPoint.lineId == selectedId ||
               dimension.firstPoint.circleId == selectedId ||
               dimension.secondPoint.circleId == selectedId;
      };

  const auto currentDimensionValue =
      [this](const sketch::Dimension& dimension) -> std::optional<double> {
        if (dimension.kind == sketch::DimensionKind::CircleDiameter) {
          const auto index = sketch_.circleIndex(dimension.geometryId);
          if (!index) return std::nullopt;
          return sketch_.circles()[*index].radiusMm * 2.0;
        }

        if (dimension.kind == sketch::DimensionKind::LineAngle) {
          const auto firstIndex = sketch_.lineIndex(dimension.geometryId);
          const auto secondIndex =
              sketch_.lineIndex(dimension.secondPoint.lineId);
          if (!firstIndex || !secondIndex) return std::nullopt;
          return lineAngleDegrees(sketch_.lines()[*firstIndex],
                                  sketch_.lines()[*secondIndex]);
        }

        sketch::Point first;
        sketch::Point second;

        if (dimension.kind == sketch::DimensionKind::LineLength) {
          const auto index = sketch_.lineIndex(dimension.geometryId);
          if (!index) return std::nullopt;
          first = sketch_.lines()[*index].start;
          second = sketch_.lines()[*index].end;
        } else {
          const auto firstPoint = sketch_.referencedPoint(dimension.firstPoint);
          const auto secondPoint =
              sketch_.referencedPoint(dimension.secondPoint);
          if (!firstPoint || !secondPoint) return std::nullopt;
          first = *firstPoint;
          second = *secondPoint;
        }

        if (dimension.kind == sketch::DimensionKind::PointDistanceX)
          return std::abs(second.xMm - first.xMm);
        if (dimension.kind == sketch::DimensionKind::PointDistanceY)
          return std::abs(second.yMm - first.yMm);

        return std::hypot(second.xMm - first.xMm, second.yMm - first.yMm);
      };

  std::vector<ConstraintPanelEntry> result;
  std::vector<sketch::ConstraintId> dimensionConstraintIds;

  for (std::size_t dimensionIndex = 0;
       dimensionIndex < sketch_.dimensions().size(); ++dimensionIndex) {
    const auto& dimension = sketch_.dimensions()[dimensionIndex];
    if (!dimensionReferencesSelected(dimension)) continue;

    sketch::ConstraintId activeConstraint = sketch::kInvalidConstraintId;
    for (const auto& constraint : sketch_.constraints()) {
      if (constraintMatchesDimension(constraint, dimension)) {
        activeConstraint = constraint.id;
        dimensionConstraintIds.push_back(constraint.id);
        break;
      }
    }

    QString name;
    switch (dimension.kind) {
      case sketch::DimensionKind::LineLength:
        name = typeName(sketch::ConstraintType::Length);
        break;
      case sketch::DimensionKind::CircleDiameter:
        name = typeName(sketch::ConstraintType::Diameter);
        break;
      case sketch::DimensionKind::PointDistance:
        name = typeName(sketch::ConstraintType::Distance);
        break;
      case sketch::DimensionKind::PointDistanceX:
        name = typeName(sketch::ConstraintType::DistanceX);
        break;
      case sketch::DimensionKind::PointDistanceY:
        name = typeName(sketch::ConstraintType::DistanceY);
        break;
      case sketch::DimensionKind::LineAngle:
        name = typeName(sketch::ConstraintType::Angle);
        break;
    }

    if (const auto value = currentDimensionValue(dimension)) {
      if (dimension.kind == sketch::DimensionKind::LineAngle)
        name += QStringLiteral(": %1").arg(*value, 0, 'f', 2) + QChar(0x00B0);
      else
      name += QString::fromUtf8(": %1 мм").arg(*value, 0, 'f', 2);
    }
    ConstraintPanelEntry entry;
    entry.description = name;
    entry.constraintId = activeConstraint;
    entry.dimensionIndex = dimensionIndex;
    entry.checked = activeConstraint != sketch::kInvalidConstraintId;
    result.push_back(std::move(entry));
  }

  for (const auto& constraint : sketch_.constraints()) {
    if (std::find(dimensionConstraintIds.begin(), dimensionConstraintIds.end(),
                  constraint.id) != dimensionConstraintIds.end())
      continue;

    const bool referencesSelected =
        constraint.firstGeometry == selectedId ||
        constraint.secondGeometry == selectedId ||
        constraint.firstPoint.lineId == selectedId ||
        constraint.secondPoint.lineId == selectedId ||
        constraint.firstPoint.circleId == selectedId ||
        constraint.secondPoint.circleId == selectedId;
    if (!referencesSelected) continue;

    QString text = typeName(constraint.type);
    if ((constraint.type == sketch::ConstraintType::Distance ||
         constraint.type == sketch::ConstraintType::DistanceX ||
         constraint.type == sketch::ConstraintType::DistanceY ||
         constraint.type == sketch::ConstraintType::Length ||
         constraint.type == sketch::ConstraintType::Radius ||
         constraint.type == sketch::ConstraintType::Diameter) &&
        constraint.value > 0.0)
      text += QString::fromUtf8(": %1 мм").arg(constraint.value, 0, 'f', 2);
    else if (constraint.type == sketch::ConstraintType::Angle &&
             constraint.value != 0.0)
      text += QString::fromUtf8(": %1°").arg(constraint.value, 0, 'f', 2);

    ConstraintPanelEntry entry;
    entry.description = text;
    entry.constraintId = constraint.id;
    entry.checked = true;
    result.push_back(std::move(entry));
  }

  return result;
}

bool SketchCanvas::setDimensionDriving(std::size_t dimensionIndex,
                                       bool driving) {
  if (dimensionIndex >= sketch_.dimensions().size()) return false;

  const auto dimension = sketch_.dimensions()[dimensionIndex];

  const auto samePoint = [](sketch::PointReference first,
                            sketch::PointReference second) {
    // CRASH-FREE 05: COMPLETE POINTREFERENCE IDENTITY
    if (first.elementCenterId != 0 ||
        second.elementCenterId != 0) {
      return first.elementCenterId != 0 &&
             second.elementCenterId != 0 &&
             first.elementCenterId == second.elementCenterId;
    }

    if (first.circleId != sketch::kInvalidGeometryId ||
        second.circleId != sketch::kInvalidGeometryId) {
      return first.circleId != sketch::kInvalidGeometryId &&
             second.circleId != sketch::kInvalidGeometryId &&
             first.circleId == second.circleId;
    }

    if (first.lineId == sketch::kInvalidGeometryId ||
        second.lineId == sketch::kInvalidGeometryId)
      return false;

    return first.lineId == second.lineId &&
           first.start == second.start;
  };

  const auto samePointPair =
      [&samePoint, &dimension](const sketch::Constraint& constraint) {
        const bool sameOrder =
            samePoint(constraint.firstPoint, dimension.firstPoint) &&
            samePoint(constraint.secondPoint, dimension.secondPoint);
        const bool reverseOrder =
            samePoint(constraint.firstPoint, dimension.secondPoint) &&
            samePoint(constraint.secondPoint, dimension.firstPoint);
        return sameOrder || reverseOrder;
      };

  const auto matches = [&dimension, &samePointPair](
                           const sketch::Constraint& constraint) {
    switch (dimension.kind) {
      case sketch::DimensionKind::LineLength:
        return constraint.type == sketch::ConstraintType::Length &&
               constraint.firstGeometry == dimension.geometryId;
      case sketch::DimensionKind::CircleDiameter:
        return constraint.type == sketch::ConstraintType::Diameter &&
               constraint.firstGeometry == dimension.geometryId;
      case sketch::DimensionKind::PointDistance:
        return constraint.type == sketch::ConstraintType::Distance &&
               samePointPair(constraint);
      case sketch::DimensionKind::PointDistanceX:
        return constraint.type == sketch::ConstraintType::DistanceX &&
               samePointPair(constraint);
      case sketch::DimensionKind::PointDistanceY:
        return constraint.type == sketch::ConstraintType::DistanceY &&
               samePointPair(constraint);
      case sketch::DimensionKind::LineAngle: {
        if (constraint.type != sketch::ConstraintType::Angle) return false;
        const bool sameOrder =
            constraint.firstGeometry == dimension.geometryId &&
            constraint.secondGeometry == dimension.secondPoint.lineId;
        const bool reverseOrder =
            constraint.firstGeometry == dimension.secondPoint.lineId &&
            constraint.secondGeometry == dimension.geometryId;
        return sameOrder || reverseOrder;
      }
    }
    return false;
  };

  std::vector<sketch::ConstraintId> existing;
  for (const auto& constraint : sketch_.constraints()) {
    if (matches(constraint)) existing.push_back(constraint.id);
  }

  if (!driving) {
    if (existing.empty()) return true;

    pushUndoState();
    for (const auto id : existing)
      sketch_.removeConstraint(id);

    notifyGeometryChanged();
    update();
    return true;
  }

  if (!existing.empty()) return true;

  double value = 0.0;
  sketch::Constraint constraint;

  switch (dimension.kind) {
    case sketch::DimensionKind::LineLength: {
      const auto index = sketch_.lineIndex(dimension.geometryId);
      if (!index) return false;
      const auto& line = sketch_.lines()[*index];
      value = std::hypot(line.end.xMm - line.start.xMm,
                         line.end.yMm - line.start.yMm);
      constraint.type = sketch::ConstraintType::Length;
      constraint.firstGeometry = dimension.geometryId;
      break;
    }

    case sketch::DimensionKind::CircleDiameter: {
      const auto index = sketch_.circleIndex(dimension.geometryId);
      if (!index) return false;
      value = sketch_.circles()[*index].radiusMm * 2.0;
      constraint.type = sketch::ConstraintType::Diameter;
      constraint.firstGeometry = dimension.geometryId;
      break;
    }

    case sketch::DimensionKind::PointDistance:
    case sketch::DimensionKind::PointDistanceX:
    case sketch::DimensionKind::PointDistanceY: {
      const auto first = sketch_.referencedPoint(dimension.firstPoint);
      const auto second = sketch_.referencedPoint(dimension.secondPoint);
      if (!first || !second) return false;

      if (dimension.kind == sketch::DimensionKind::PointDistanceX) {
        value = std::abs(second->xMm - first->xMm);
        constraint.type = sketch::ConstraintType::DistanceX;
      } else if (dimension.kind == sketch::DimensionKind::PointDistanceY) {
        value = std::abs(second->yMm - first->yMm);
        constraint.type = sketch::ConstraintType::DistanceY;
      } else {
        value = std::hypot(second->xMm - first->xMm,
                           second->yMm - first->yMm);
        constraint.type = sketch::ConstraintType::Distance;
      }

      constraint.firstPoint = dimension.firstPoint;
      constraint.secondPoint = dimension.secondPoint;
      break;
    }

    case sketch::DimensionKind::LineAngle: {
      const auto firstIndex = sketch_.lineIndex(dimension.geometryId);
      const auto secondIndex =
          sketch_.lineIndex(dimension.secondPoint.lineId);
      if (!firstIndex || !secondIndex) return false;

      const double primitiveAngle =
          lineAngleDegrees(
              sketch_.lines()[*firstIndex],
              sketch_.lines()[*secondIndex]);

      const double visibleAngle =
          visibleLineAngleDegrees(
              sketch_.lines()[*firstIndex],
              sketch_.lines()[*secondIndex]);

      value = visibleAngle;

      double solverAngle =
          visibleAngle;

      const double supplement =
          180.0 - primitiveAngle;

      if (std::abs(
              visibleAngle -
              supplement) <
          std::abs(
              visibleAngle -
              primitiveAngle))
        solverAngle =
            180.0 - visibleAngle;

      constraint.type =
          sketch::ConstraintType::Angle;
      constraint.firstGeometry =
          dimension.geometryId;
      constraint.secondGeometry =
          dimension.secondPoint.lineId;
      constraint.value =
          solverAngle;
      break;
    }
  }

  if (value <= 1e-9) return false;

  pushUndoState();
  if (dimension.kind !=
      sketch::DimensionKind::LineAngle)
    constraint.value = value;

  sketch_.addConstraint(constraint);
  sketch_.setDimensionValue(
      dimensionIndex,
      value);

  notifyGeometryChanged();
  update();
  return true;
}
bool SketchCanvas::removeConstraintById(sketch::ConstraintId id) {
  if (id == sketch::kInvalidConstraintId) return false;

  const auto found = std::find_if(
      sketch_.constraints().begin(), sketch_.constraints().end(),
      [id](const auto& constraint) { return constraint.id == id; });
  if (found == sketch_.constraints().end()) return false;

  pushUndoState();
  if (!sketch_.removeConstraint(id)) return false;

  emit selectionChanged(QString::fromUtf8("Ограничение удалено"));
  notifyGeometryChanged();
  update();
  return true;
}
void SketchCanvas::setReferenceBody(BoxParameters box, const QString& support,
                                    bool visible) {
  referenceBox_ = box;
  referenceSupport_ = support;
  referenceBodyVisible_ = visible;
  update();
}

void SketchCanvas::setSketchEditContext(const SketchEditContext& context) {
  clearSketchEditContext();
  referencePlacement_ = context.placement;
  if (!context.supportShape || context.supportShape->IsNull() ||
      !context.supportFace)
    return;

  referenceBodyMesh_.rebuild(*context.supportShape);
  std::size_t faceIndex = 0;
  for (TopExp_Explorer faces(*context.supportShape, TopAbs_FACE); faces.More();
       faces.Next(), ++faceIndex) {
    if (faceIndex != context.supportFace->faceIndex) continue;
    referenceFaceMesh_.rebuild(faces.Current());
    break;
  }
  realReferenceBodyVisible_ = !referenceBodyMesh_.triangles().empty();
  if (!realReferenceBodyVisible_) return;

  const BodyRenderMesh& fitMesh = referenceFaceMesh_.edges().empty()
                                      ? referenceBodyMesh_
                                      : referenceFaceMesh_;
  double minU = std::numeric_limits<double>::max();
  double minV = std::numeric_limits<double>::max();
  double maxU = std::numeric_limits<double>::lowest();
  double maxV = std::numeric_limits<double>::lowest();
  const auto includePoint = [&](Point3d point) {
    const auto local = referencePlacement_.toLocal(point);
    minU = std::min(minU, local.x);
    minV = std::min(minV, local.y);
    maxU = std::max(maxU, local.x);
    maxV = std::max(maxV, local.y);
  };
  for (const auto& edge : fitMesh.edges())
    for (const auto& point : edge.points) includePoint(point);
  if (minU <= maxU && minV <= maxV) {
    const double spanU = std::max(1.0, maxU - minU);
    const double spanV = std::max(1.0, maxV - minV);
    const double availableWidth = std::max(100.0, width() - kRulerLeft - 50.0);
    const double availableHeight = std::max(100.0, height() - kRulerTop - 50.0);
    pixelsPerMm_ = std::clamp(0.82 * std::min(availableWidth / spanU,
                                             availableHeight / spanV),
                              0.05, 50.0);
    setProperty("sketchPanX", -(minU + maxU) * 0.5 * pixelsPerMm_);
    setProperty("sketchPanY", (minV + maxV) * 0.5 * pixelsPerMm_);
  }
  update();
}

void SketchCanvas::clearSketchEditContext() {
  referenceBodyMesh_.clear();
  referenceFaceMesh_.clear();
  realReferenceBodyVisible_ = false;
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
  setProperty("dimensionLabelAlongMm", QVariantList{});
  setProperty("dimensionLabelOffsetMm", QVariantList{});
  clearGeometrySelection();
  emit lineStyleSelectionChanged(false, false);
  anchor_.reset();
  rectanglePoints_.clear();
  circlePoints_.clear();
  circleGuideLines_.clear();
  notifyGeometryChanged();
}

void SketchCanvas::resetSketch() {
  sketch_.clear();
  setProperty("dimensionLabelAlongMm", QVariantList{});
  setProperty("dimensionLabelOffsetMm", QVariantList{});
  undoStack_.clear();
  clearGeometrySelection();
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
  setProperty("dimensionLabelAlongMm", QVariantList{});
  setProperty("dimensionLabelOffsetMm", QVariantList{});
  undoStack_.clear();
  clearGeometrySelection();
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

bool SketchCanvas::hasRealReferenceBody() const noexcept {
  return realReferenceBodyVisible_;
}

std::size_t SketchCanvas::referenceFaceEdgeCount() const noexcept {
  return referenceFaceMesh_.edges().size();
}

void SketchCanvas::undo() {
  if (undoStack_.empty()) return;
  sketch_ = undoStack_.back();
  setProperty("dimensionLabelAlongMm", QVariantList{});
  setProperty("dimensionLabelOffsetMm", QVariantList{});
  undoStack_.pop_back();
  clearGeometrySelection();
  emit lineStyleSelectionChanged(false, false);
  anchor_.reset();
  hideDimensionEditor();
  emit selectionChanged(QString::fromUtf8("Ничего не выбрано"));
  emit undoAvailable(canUndo());
  notifyGeometryChanged();
}

void SketchCanvas::deleteSelection() {
  const bool hasMultiSelection =
      !selectedElementIds_.empty() || !selectedCircleIds_.empty();

  if (!hasMultiSelection && selectionKind_ == SelectionKind::None) return;

  pushUndoState();

  if (hasMultiSelection) {
    // Element IDs are group IDs: deleting one ID also deletes all primitive
    // lines belonging to a composite element such as a rectangle.
    const auto elementIds = selectedElementIds_;
    const auto circleIds = selectedCircleIds_;

    for (const auto elementId : elementIds)
      sketch_.removeElement(elementId);

    for (const auto circleId : circleIds) {
      const auto index = sketch_.circleIndex(circleId);
      if (index) sketch_.removeCircle(*index);
    }
  } else if (selectionKind_ == SelectionKind::Line) {
    sketch_.removeElement(selectionElementId_);
  } else if (selectionKind_ == SelectionKind::Circle) {
    const auto index = sketch_.circleIndex(selectionCircleId_);
    if (index) sketch_.removeCircle(*index);
  }

  clearGeometrySelection();
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

  if (realReferenceBodyVisible_) {
    struct ProjectedTriangle {
      QPolygonF polygon;
      double depth{};
      double facing{};
    };
    const Vector3d normal = referencePlacement_.normal();
    const auto depthOf = [&](Point3d point) {
      return (point.x - referencePlacement_.origin.x) * normal.x +
             (point.y - referencePlacement_.origin.y) * normal.y +
             (point.z - referencePlacement_.origin.z) * normal.z;
    };
    const auto projected = [&](Point3d point) {
      const auto local = referencePlacement_.toLocal(point);
      return mapPoint({local.x, local.y});
    };
    std::vector<ProjectedTriangle> triangles;
    triangles.reserve(referenceBodyMesh_.triangles().size());
    for (const auto& triangle : referenceBodyMesh_.triangles()) {
      triangles.push_back(
          {{projected(triangle.a), projected(triangle.b), projected(triangle.c)},
           (depthOf(triangle.a) + depthOf(triangle.b) + depthOf(triangle.c)) /
               3.0,
           std::abs(triangle.normal.x * normal.x +
                    triangle.normal.y * normal.y +
                    triangle.normal.z * normal.z)});
    }
    std::sort(triangles.begin(), triangles.end(),
              [](const auto& first, const auto& second) {
                return first.depth < second.depth;
              });
    painter.setPen(Qt::NoPen);
    for (const auto& triangle : triangles) {
      const int shade = static_cast<int>(150 + triangle.facing * 35.0);
      painter.setBrush(QColor(shade, shade + 3, shade + 7, 72));
      painter.drawPolygon(triangle.polygon);
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(82, 94, 108, 105), 1.0));
    for (const auto& edge : referenceBodyMesh_.edges()) {
      QPolygonF curve;
      for (const auto& point : edge.points) curve << projected(point);
      painter.drawPolyline(curve);
    }
    painter.setBrush(QColor(205, 218, 232, 48));
    painter.setPen(Qt::NoPen);
    for (const auto& triangle : referenceFaceMesh_.triangles()) {
      QPolygonF polygon{projected(triangle.a), projected(triangle.b),
                        projected(triangle.c)};
      painter.drawPolygon(polygon);
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(58, 75, 94, 205), 2.0));
    for (const auto& edge : referenceFaceMesh_.edges()) {
      QPolygonF boundary;
      for (const auto& point : edge.points) boundary << projected(point);
      painter.drawPolyline(boundary);
    }
  }
  if (!realReferenceBodyVisible_ && referenceBodyVisible_ &&
      !referenceProfileVisible_) {
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
  if (!realReferenceBodyVisible_ && referenceBodyVisible_ &&
      referenceProfileVisible_) {
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
    const bool selected =
        lineElementSelected(line.elementId) ||
        (selectedElementIds_.empty() &&
         selectionKind_ == SelectionKind::Line &&
         selectionElementId_ == line.elementId);
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
    const auto circleId = sketch_.circleId(index);
    const bool selected =
        circleSelected(circleId) ||
        (selectedCircleIds_.empty() &&
         selectionKind_ == SelectionKind::Circle &&
         selectionCircleId_ == circleId);
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

  const auto drawArrow = [&painter](QPointF tip, QPointF direction) {
    const double length = std::hypot(direction.x(), direction.y());
    if (length < 1e-6) return;
    direction /= length;
    const QPointF normal(-direction.y(), direction.x());
    QPolygonF arrow;
    arrow << tip << tip - direction * 8.0 + normal * 3.5
          << tip - direction * 8.0 - normal * 3.5;
    painter.drawPolygon(arrow);
  };
  for (const auto& dimension : sketch_.dimensions()) {
    const std::size_t dimensionIndex =
        static_cast<std::size_t>(&dimension - sketch_.dimensions().data());

    if (dimension.kind == sketch::DimensionKind::LineAngle) {
      const auto firstIndex = sketch_.lineIndex(dimension.geometryId);
      const auto secondIndex =
          sketch_.lineIndex(dimension.secondPoint.lineId);
      if (!firstIndex || !secondIndex) continue;

      const auto& firstLine = sketch_.lines()[*firstIndex];
      const auto& secondLine = sketch_.lines()[*secondIndex];
      const auto center = lineIntersectionScreen(
          firstLine, secondLine,
          [this](sketch::Point point) { return mapPoint(point); });
      if (!center) continue;

      QPointF firstDirection =
          angleRayTowardSegment(
              *center,
              mapPoint(firstLine.start),
              mapPoint(firstLine.end));
      QPointF secondDirection =
          angleRayTowardSegment(
              *center,
              mapPoint(secondLine.start),
              mapPoint(secondLine.end));

      const auto sameScreenVertex = [this](sketch::Point a,
                                           sketch::Point b) {
        return QLineF(mapPoint(a), mapPoint(b)).length() <= 0.5;
      };

      if (sameScreenVertex(firstLine.start, secondLine.start)) {
        firstDirection =
            mapPoint(firstLine.end) - mapPoint(firstLine.start);
        secondDirection =
            mapPoint(secondLine.end) - mapPoint(secondLine.start);
      } else if (sameScreenVertex(firstLine.start, secondLine.end)) {
        firstDirection =
            mapPoint(firstLine.end) - mapPoint(firstLine.start);
        secondDirection =
            mapPoint(secondLine.start) - mapPoint(secondLine.end);
      } else if (sameScreenVertex(firstLine.end, secondLine.start)) {
        firstDirection =
            mapPoint(firstLine.start) - mapPoint(firstLine.end);
        secondDirection =
            mapPoint(secondLine.end) - mapPoint(secondLine.start);
      } else if (sameScreenVertex(firstLine.end, secondLine.end)) {
        firstDirection =
            mapPoint(firstLine.start) - mapPoint(firstLine.end);
        secondDirection =
            mapPoint(secondLine.start) - mapPoint(secondLine.end);
      }
      const double firstLength =
          std::hypot(firstDirection.x(), firstDirection.y());
      const double secondLength =
          std::hypot(secondDirection.x(), secondDirection.y());
      if (firstLength <= 1.0 || secondLength <= 1.0) continue;
      firstDirection /= firstLength;
      secondDirection /= secondLength;

      const double radius =
          std::max(16.0, std::abs(dimension.offsetMm) * pixelsPerMm_);
      const QPointF arcFirst = *center + firstDirection * radius;
      const QPointF arcSecond = *center + secondDirection * radius;

      double startDeg =
          -std::atan2(firstDirection.y(), firstDirection.x()) *
          180.0 / 3.14159265358979323846;
      double endDeg =
          -std::atan2(secondDirection.y(), secondDirection.x()) *
          180.0 / 3.14159265358979323846;
      double spanDeg = endDeg - startDeg;
      while (spanDeg <= -180.0) spanDeg += 360.0;
      while (spanDeg > 180.0) spanDeg -= 360.0;

      const bool selectedDimension =
          property("selectedDimension").isValid() &&
          property("selectedDimension").toULongLong() == dimensionIndex;
      const QColor dimensionColor =
          selectedDimension ? QColor("#ff8a24") : QColor("#315e9d");

            // STORED ANGLE CARRIER EXTENSIONS
      const QPointF firstStartScreen = mapPoint(firstLine.start);
      const QPointF firstEndScreen = mapPoint(firstLine.end);
      const QPointF secondStartScreen = mapPoint(secondLine.start);
      const QPointF secondEndScreen = mapPoint(secondLine.end);

      painter.setBrush(Qt::NoBrush);
      painter.setPen(
          QPen(dimensionColor,
               selectedDimension ? 1.5 : 1.0,
               Qt::DashLine));

      if (pointSegmentDistance(*center,
                               firstStartScreen,
                               firstEndScreen) > 0.75) {
        if (const auto endpoint =
                nearestSegmentEndpointTo(*center,
                                         firstStartScreen,
                                         firstEndScreen))
          painter.drawLine(*center, *endpoint);
      }

      if (pointSegmentDistance(*center,
                               secondStartScreen,
                               secondEndScreen) > 0.75) {
        if (const auto endpoint =
                nearestSegmentEndpointTo(*center,
                                         secondStartScreen,
                                         secondEndScreen))
          painter.drawLine(*center, *endpoint);
      }

      painter.setPen(
          QPen(dimensionColor,
               selectedDimension ? 2.2 : 1.2));
      painter.drawLine(*center, arcFirst);
      painter.drawLine(*center, arcSecond);

      QRectF arcRect(center->x() - radius, center->y() - radius,
                     radius * 2.0, radius * 2.0);
      painter.drawArc(arcRect,
                      qRound(startDeg * 16.0),
                      qRound(spanDeg * 16.0));

      const double midRad =
          (startDeg + spanDeg * 0.5) *
          3.14159265358979323846 / 180.0;
      QPointF textCenter =
          *center + QPointF(std::cos(midRad), -std::sin(midRad)) *
                        (radius + 18.0);

      // Angular dimension labels use the same auxiliary arrays as linear
      // dimensions, but store free screen-X / screen-Y offsets in millimetres.
      const QVariantList angleLabelX =
          property("dimensionLabelAlongMm").toList();
      const QVariantList angleLabelY =
          property("dimensionLabelOffsetMm").toList();

      const double labelOffsetX =
          dimensionIndex < static_cast<std::size_t>(angleLabelX.size())
              ? angleLabelX[static_cast<int>(dimensionIndex)].toDouble()
              : 0.0;
      const double labelOffsetY =
          dimensionIndex < static_cast<std::size_t>(angleLabelY.size())
              ? angleLabelY[static_cast<int>(dimensionIndex)].toDouble()
              : 0.0;

      textCenter += QPointF(labelOffsetX * pixelsPerMm_,
                            labelOffsetY * pixelsPerMm_);

      const QString label =
          QString::fromUtf8("%1°")
              .arg(std::abs(spanDeg), 0, 'f', 2);

      const QRectF textRect(-42.0, -10.0, 84.0, 20.0);
      painter.save();
      painter.translate(textCenter);
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(251, 252, 255, 235));
      painter.drawRoundedRect(textRect, 4.0, 4.0);
      painter.setPen(selectedDimension ? QColor("#d76400")
                                       : QColor("#244a82"));
      painter.drawText(textRect, Qt::AlignCenter, label);
      painter.restore();
      continue;
    }
    QPointF first;
    QPointF second;
    QPointF geometryFirst;
    QPointF geometrySecond;
    QString label;
    const bool diameterDimension =
        dimension.kind == sketch::DimensionKind::CircleDiameter;

    if (diameterDimension) {
      const auto circleIndex = sketch_.circleIndex(dimension.geometryId);
      if (!circleIndex) continue;
      const auto& circle = sketch_.circles()[*circleIndex];
      const double dx = std::cos(dimension.angleRad) * circle.radiusMm;
      const double dy = std::sin(dimension.angleRad) * circle.radiusMm;
      first = mapPoint({circle.center.xMm - dx, circle.center.yMm - dy});
      second = mapPoint({circle.center.xMm + dx, circle.center.yMm + dy});
      geometryFirst = first;
      geometrySecond = second;
      label = QString::fromUtf8("Ø %1 мм").arg(circle.radiusMm * 2.0, 0, 'f', 2);
    } else {
      if (dimension.kind == sketch::DimensionKind::LineLength) {
        const auto lineIndex = sketch_.lineIndex(dimension.geometryId);
        if (!lineIndex) continue;
        const auto& line = sketch_.lines()[*lineIndex];
        geometryFirst = mapPoint(line.start);
        geometrySecond = mapPoint(line.end);
        first = geometryFirst;
        second = geometrySecond;
      } else {
        const auto firstPoint = sketch_.referencedPoint(dimension.firstPoint);
        const auto secondPoint = sketch_.referencedPoint(dimension.secondPoint);
        if (!firstPoint || !secondPoint) continue;

        geometryFirst = mapPoint(*firstPoint);
        geometrySecond = mapPoint(*secondPoint);
        first = geometryFirst;
        second = geometrySecond;

        if (dimension.kind == sketch::DimensionKind::PointDistanceX) {
          // Horizontal dimension line: keep the real X positions.
          second.setY(first.y());
        } else if (dimension.kind == sketch::DimensionKind::PointDistanceY) {
          // Vertical dimension line: keep the real Y positions.
          second.setX(first.x());
        }
      }

      label = QString::fromUtf8("%1 мм").arg(
          QLineF(first, second).length() / pixelsPerMm_, 0, 'f', 2);
    }

    QPointF direction = second - first;
    const double length = std::hypot(direction.x(), direction.y());
    if (length < 1.0) continue;
    direction /= length;

    const QPointF normal(-direction.y(), direction.x());
    const QPointF offset = diameterDimension
                               ? QPointF{}
                               : normal * dimension.offsetMm * pixelsPerMm_;

    const QPointF dimensionFirst = first + offset;
    const QPointF dimensionSecond = second + offset;
    const bool selectedDimension =
        property("selectedDimension").isValid() &&
        property("selectedDimension").toULongLong() == dimensionIndex;
    const QColor dimensionColor =
        selectedDimension ? QColor("#ff8a24") : QColor("#315e9d");
    painter.setPen(QPen(dimensionColor, selectedDimension ? 2.2 : 1.2));
    painter.setBrush(dimensionColor);
    if (!diameterDimension) {
      painter.drawLine(geometryFirst, dimensionFirst);
      painter.drawLine(geometrySecond, dimensionSecond);
    }
    painter.drawLine(dimensionFirst, dimensionSecond);
    drawArrow(dimensionFirst, direction);
    drawArrow(dimensionSecond, -direction);
    const QPointF textCenter = dimensionLabelCenter(
        dimensionIndex, dimensionFirst, dimensionSecond);
    double textAngle = std::atan2(direction.y(), direction.x()) *
                       180.0 / 3.141592653589793;
    if (textAngle > 90.0 || textAngle < -90.0) textAngle += 180.0;
    const QRectF textRect(-42.0, -10.0, 84.0, 20.0);
    painter.save();
    painter.translate(textCenter);
    painter.rotate(textAngle);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(251, 252, 255, 235));
    painter.drawRoundedRect(textRect, 4.0, 4.0);
    painter.setPen(selectedDimension ? QColor("#d76400")
                                     : QColor("#244a82"));
    painter.drawText(textRect, Qt::AlignCenter, label);
    painter.restore();
  }
  // RECTANGLE CENTER NODES
  //
  // Virtual CAD nodes owned by rectangles created with FromCenter.
  // Coordinates are derived from the current rectangle geometry, so the
  // node always follows move / resize / rotation.
  painter.save();
  painter.setPen(QPen(QColor("#1469d7"), 1.8));
  painter.setBrush(QColor("#fbfcff"));

  for (const auto elementId : sketch_.centerNodeElementIds()) {
    const auto center = sketch_.elementCenterPoint(elementId);
    if (!center) continue;

    const QPointF screenCenter = mapPoint(*center);

    painter.drawEllipse(screenCenter, 4.2, 4.2);
    painter.drawLine(screenCenter + QPointF(-6.0, 0.0),
                     screenCenter + QPointF(6.0, 0.0));
    painter.drawLine(screenCenter + QPointF(0.0, -6.0),
                     screenCenter + QPointF(0.0, 6.0));
  }

  painter.restore();
  // CRASH-FREE 13: NO DUPLICATE PREVIEW WHILE EDITING
  //
  // A stored dimension is already rendered by the permanent dimension loop
  // above. Drawing the transient AutoDimension preview at the same time
  // produces a second dimension line. Preview remains enabled for NEW
  // dimensions only.
  if (tool_ == Tool::AutoDimension &&
      primaryDimension_->isVisible() &&
      !property("editingDimensionIndex").isValid()) {
    const QString target = property("autoDimensionTarget").toString();

    if (target == "angle") {
      const auto firstId = static_cast<sketch::GeometryId>(
          property("autoDimensionAngleFirstLine").toULongLong());
      const auto secondId = static_cast<sketch::GeometryId>(
          property("autoDimensionAngleSecondLine").toULongLong());
      const auto firstIndex = sketch_.lineIndex(firstId);
      const auto secondIndex = sketch_.lineIndex(secondId);

      if (firstIndex && secondIndex) {
        const auto& firstLine = sketch_.lines()[*firstIndex];
        const auto& secondLine = sketch_.lines()[*secondIndex];
        const auto center = lineIntersectionScreen(
            firstLine, secondLine,
            [this](sketch::Point point) { return mapPoint(point); });

        if (center) {
          QPointF firstDirection =
          angleRayTowardSegment(
              *center,
              mapPoint(firstLine.start),
              mapPoint(firstLine.end));
      QPointF secondDirection =
          angleRayTowardSegment(
              *center,
              mapPoint(secondLine.start),
              mapPoint(secondLine.end));

          const auto sameScreenVertex = [this](sketch::Point a,
                                               sketch::Point b) {
            return QLineF(mapPoint(a), mapPoint(b)).length() <= 0.5;
          };

          // Preview must use the exact same sector semantics as the stored
          // angular dimension: both rays leave the common CAD vertex.
          if (sameScreenVertex(firstLine.start, secondLine.start)) {
            firstDirection =
                mapPoint(firstLine.end) - mapPoint(firstLine.start);
            secondDirection =
                mapPoint(secondLine.end) - mapPoint(secondLine.start);
          } else if (sameScreenVertex(firstLine.start, secondLine.end)) {
            firstDirection =
                mapPoint(firstLine.end) - mapPoint(firstLine.start);
            secondDirection =
                mapPoint(secondLine.start) - mapPoint(secondLine.end);
          } else if (sameScreenVertex(firstLine.end, secondLine.start)) {
            firstDirection =
                mapPoint(firstLine.start) - mapPoint(firstLine.end);
            secondDirection =
                mapPoint(secondLine.end) - mapPoint(secondLine.start);
          } else if (sameScreenVertex(firstLine.end, secondLine.end)) {
            firstDirection =
                mapPoint(firstLine.start) - mapPoint(firstLine.end);
            secondDirection =
                mapPoint(secondLine.start) - mapPoint(secondLine.end);
          }

          const double firstLength =
              std::hypot(firstDirection.x(), firstDirection.y());
          const double secondLength =
              std::hypot(secondDirection.x(), secondDirection.y());

          if (firstLength > 1.0 && secondLength > 1.0) {
            firstDirection /= firstLength;
            secondDirection /= secondLength;

            const double radius =
                std::max(16.0,
                         std::abs(property("autoDimensionOffsetMm").toDouble()) *
                             pixelsPerMm_);
            const QPointF arcFirst = *center + firstDirection * radius;
            const QPointF arcSecond = *center + secondDirection * radius;

            double startDeg =
                -std::atan2(firstDirection.y(), firstDirection.x()) *
                180.0 / 3.14159265358979323846;
            double endDeg =
                -std::atan2(secondDirection.y(), secondDirection.x()) *
                180.0 / 3.14159265358979323846;
            double spanDeg = endDeg - startDeg;
            while (spanDeg <= -180.0) spanDeg += 360.0;
            while (spanDeg > 180.0) spanDeg -= 360.0;

                        // PREVIEW ANGLE CARRIER EXTENSIONS
            const QPointF firstStartScreen = mapPoint(firstLine.start);
            const QPointF firstEndScreen = mapPoint(firstLine.end);
            const QPointF secondStartScreen = mapPoint(secondLine.start);
            const QPointF secondEndScreen = mapPoint(secondLine.end);

            painter.setBrush(Qt::NoBrush);
            painter.setPen(
                QPen(QColor("#0872f9"), 1.0, Qt::DashLine));

            if (pointSegmentDistance(*center,
                                     firstStartScreen,
                                     firstEndScreen) > 0.75) {
              if (const auto endpoint =
                      nearestSegmentEndpointTo(*center,
                                               firstStartScreen,
                                               firstEndScreen))
                painter.drawLine(*center, *endpoint);
            }

            if (pointSegmentDistance(*center,
                                     secondStartScreen,
                                     secondEndScreen) > 0.75) {
              if (const auto endpoint =
                      nearestSegmentEndpointTo(*center,
                                               secondStartScreen,
                                               secondEndScreen))
                painter.drawLine(*center, *endpoint);
            }

            painter.setPen(
                QPen(QColor("#0872f9"), 1.4, Qt::DashLine));
            painter.drawLine(*center, arcFirst);
            painter.drawLine(*center, arcSecond);
            QRectF arcRect(center->x() - radius, center->y() - radius,
                           radius * 2.0, radius * 2.0);
            painter.drawArc(arcRect,
                            qRound(startDeg * 16.0),
                            qRound(spanDeg * 16.0));
          }
        }
      }
    }

    const bool diameterDimension = target == "circle";

    std::optional<QPointF> first;
    std::optional<QPointF> second;

    if (target == "angle") {
      // Angular preview is painted above.
    } else if (target == "line") {
      const auto id = static_cast<sketch::GeometryId>(
          property("autoDimensionIndex").toULongLong());
      const auto index = sketch_.lineIndex(id);
      if (index) {
        first = mapPoint(sketch_.lines()[*index].start);
        second = mapPoint(sketch_.lines()[*index].end);
      }
    } else if (target == "circle") {
      const auto id = static_cast<sketch::GeometryId>(
          property("autoDimensionIndex").toULongLong());
      const auto index = sketch_.circleIndex(id);
      if (index) {
        const auto& circle = sketch_.circles()[*index];
        const double angle =
            property("autoDimensionAngleRad").toDouble();
        const double dx = std::cos(angle) * circle.radiusMm;
        const double dy = std::sin(angle) * circle.radiusMm;
        first =
            mapPoint({circle.center.xMm - dx, circle.center.yMm - dy});
        second =
            mapPoint({circle.center.xMm + dx, circle.center.yMm + dy});
      }
    } else if (target == "points") {
      const sketch::PointReference firstReference{
          static_cast<sketch::GeometryId>(
              property("autoDimensionFirstLine").toULongLong()),
          property("autoDimensionFirstStart").toBool(),
          static_cast<sketch::GeometryId>(
            property("autoDimensionFirstCircle").toULongLong()),
        static_cast<std::size_t>(
            property("autoDimensionFirstElementCenter").toULongLong())};
      const sketch::PointReference secondReference{
          static_cast<sketch::GeometryId>(
              property("autoDimensionSecondLine").toULongLong()),
          property("autoDimensionSecondStart").toBool(),
          static_cast<sketch::GeometryId>(
            property("autoDimensionSecondCircle").toULongLong()),
        static_cast<std::size_t>(
            property("autoDimensionSecondElementCenter").toULongLong())};

      const auto firstPoint =
          sketch_.referencedPoint(firstReference);
      const auto secondPoint =
          sketch_.referencedPoint(secondReference);

      if (firstPoint && secondPoint) {
        first = mapPoint(*firstPoint);
        second = mapPoint(*secondPoint);
      }
    }

    if (first && second) {
      const QPointF geometryFirst = *first;
      const QPointF geometrySecond = *second;

      QPointF baseFirst = geometryFirst;
      QPointF baseSecond = geometrySecond;

      const QString pointMode =
          target == "points"
              ? property("autoDimensionPointMode").toString()
              : QString();

      if (pointMode == QStringLiteral("x")) {
        QPointF projectedSecond = baseSecond;
        projectedSecond.setY(baseFirst.y());
        if (QLineF(baseFirst, projectedSecond).length() > 1.0)
          baseSecond = projectedSecond;
      } else if (pointMode == QStringLiteral("y")) {
        QPointF projectedSecond = baseSecond;
        projectedSecond.setX(baseFirst.x());
        if (QLineF(baseFirst, projectedSecond).length() > 1.0)
          baseSecond = projectedSecond;
      }

      QPointF direction = baseSecond - baseFirst;
      const double length =
          std::hypot(direction.x(), direction.y());

      if (length > 1.0) {
        direction /= length;
        const QPointF normal(-direction.y(), direction.x());

        const QPointF offset =
            diameterDimension
                ? QPointF{}
                : normal *
                      property("autoDimensionOffsetMm").toDouble() *
                      pixelsPerMm_;

        const QPointF dimensionFirst = baseFirst + offset;
        const QPointF dimensionSecond = baseSecond + offset;

        painter.setPen(
            QPen(QColor("#0872f9"), 1.4, Qt::DashLine));
        painter.setBrush(QColor("#0872f9"));

        if (!diameterDimension) {
          painter.drawLine(geometryFirst, dimensionFirst);
          painter.drawLine(geometrySecond, dimensionSecond);
        }

        painter.drawLine(dimensionFirst, dimensionSecond);
        drawArrow(dimensionFirst, direction);
        drawArrow(dimensionSecond, -direction);
      }
    }
  }  if (tool_ == Tool::AutoDimension &&
      property("autoDimensionFirstLine").isValid() &&
      property("autoDimensionTarget").toString().isEmpty()) {
    const sketch::PointReference first{
          static_cast<sketch::GeometryId>(
              property("autoDimensionFirstLine").toULongLong()),
          property("autoDimensionFirstStart").toBool(),
          static_cast<sketch::GeometryId>(
            property("autoDimensionFirstCircle").toULongLong()),
        static_cast<std::size_t>(
            property("autoDimensionFirstElementCenter").toULongLong())};
    if (const auto point = sketch_.referencedPoint(first)) {
      painter.setPen(QPen(QColor("#0872f9"), 2.0));
      painter.setBrush(QColor(8, 114, 249, 55));
      painter.drawEllipse(mapPoint(*point), 7.0, 7.0);
    }
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
    // TWO-TANGENT SEMITRANSPARENT PREVIEW
    if (circleMode_ ==
            CircleMode::TwoTangentsRadius &&
        circleGuideLines_.size() == 2 &&
        property(
            "twoTangentRadiusPreviewActive")
            .toBool()) {
      const double previewDiameter =
          primaryDimension_->isVisible()
              ? primaryDimension_->value()
              : circleDiameterMm_;

      const auto preview =
          clampedTwoTangentCircleForRadius(
              circleGuideLines_[0],
              circleGuideLines_[1],
              std::max(
                  0.01,
                  previewDiameter * 0.5),
              hoverPoint_);

      if (preview) {
        painter.save();

        painter.setPen(
            QPen(
                QColor(10, 114, 255, 190),
                1.8,
                Qt::DashLine));

        painter.setBrush(
            QColor(10, 114, 255, 48));

        const double radiusPx =
            preview->radiusMm *
            pixelsPerMm_;

        painter.drawEllipse(
            mapPoint(preview->center),
            radiusPx,
            radiusPx);

        painter.restore();
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

  // CONSTRAINT TOOL SELECTION HIGHLIGHT
  //
  // Constraint tools keep their first picked entity in existing transient
  // state/properties. Render that entity in yellow so the user can clearly
  // see what has already been selected before choosing the second object.
  {
    const QColor constraintHighlight("#ffc400");
    const QColor constraintHighlightFill(255, 196, 0, 42);

    const auto drawHighlightedLine =
        [this, &painter, &constraintHighlight](
            sketch::GeometryId id) {
          if (id == sketch::kInvalidGeometryId) return;

          const auto index = sketch_.lineIndex(id);
          if (!index) return;

          const auto& line = sketch_.lines()[*index];
          painter.save();
          painter.setPen(QPen(constraintHighlight, 4.0,
                              Qt::SolidLine, Qt::RoundCap));
          painter.setBrush(Qt::NoBrush);
          painter.drawLine(mapPoint(line.start), mapPoint(line.end));
          painter.restore();
        };

    const auto drawHighlightedPoint =
        [this, &painter, &constraintHighlight,
         &constraintHighlightFill](
            sketch::PointReference reference) {
          const auto point = sketch_.referencedPoint(reference);
          if (!point) return;

          painter.save();
          painter.setPen(QPen(constraintHighlight, 2.6));
          painter.setBrush(constraintHighlightFill);
          painter.drawEllipse(mapPoint(*point), 7.0, 7.0);
          painter.restore();
        };

    const auto drawHighlightedCircle =
        [this, &painter, &constraintHighlight](
            sketch::GeometryId id) {
          if (id == sketch::kInvalidGeometryId) return;

          const auto index = sketch_.circleIndex(id);
          if (!index) return;

          const auto& circle = sketch_.circles()[*index];
          painter.save();
          painter.setPen(QPen(constraintHighlight, 4.0));
          painter.setBrush(Qt::NoBrush);
          painter.drawEllipse(mapPoint(circle.center),
                              circle.radiusMm * pixelsPerMm_,
                              circle.radiusMm * pixelsPerMm_);
          painter.restore();
        };

    if (tool_ == Tool::CoincidentConstraint) {
      // point -> point
      if (coincidentFirstPoint_)
        drawHighlightedPoint(*coincidentFirstPoint_);

      // line body -> point (merged PointOnLine workflow)
      if (property("pointOnLineCarrier").isValid()) {
        drawHighlightedLine(
            static_cast<sketch::GeometryId>(
                property("pointOnLineCarrier").toULongLong()));
      }
      // POINT-ON-CIRCLE CARRIER HIGHLIGHT V3
      if (property("pointOnCircleCarrier").isValid()) {
        drawHighlightedCircle(
            static_cast<sketch::GeometryId>(
                property("pointOnCircleCarrier").toULongLong()));
      }
    }

    if (tool_ == Tool::PerpendicularConstraint &&
        property("perpendicularFirstLine").isValid()) {
      drawHighlightedLine(
          static_cast<sketch::GeometryId>(
              property("perpendicularFirstLine").toULongLong()));
    }

    if (tool_ == Tool::ParallelConstraint &&
        property("parallelFirstLine").isValid()) {
      drawHighlightedLine(
          static_cast<sketch::GeometryId>(
              property("parallelFirstLine").toULongLong()));
    }

    if (tool_ == Tool::EqualConstraint &&
        property("equalFirstGeometry").isValid()) {
      const auto id =
          static_cast<sketch::GeometryId>(
              property("equalFirstGeometry").toULongLong());

      const QString kind =
          property("equalFirstKind").toString();

      if (kind == QStringLiteral("circle"))
        drawHighlightedCircle(id);
      else
        drawHighlightedLine(id);
    }
    if (tool_ == Tool::TangentConstraint &&
        property("tangentFirstGeometry").isValid() &&
        property("tangentFirstKind").isValid()) {
      const auto id =
          static_cast<sketch::GeometryId>(
              property("tangentFirstGeometry").toULongLong());

      const int kind =
          property("tangentFirstKind").toInt();

      if (kind == 2)
        drawHighlightedCircle(id);
      else if (kind == 1)
        drawHighlightedLine(id);
    }
  }
  if (selectionBoxActive_) {
    const QRectF selectionRect(selectionBoxStart_, selectionBoxCurrent_);
    const QRectF normalized = selectionRect.normalized();

    painter.setPen(QPen(QColor("#ff8a24"), 1.4, Qt::DashLine));
    painter.setBrush(QColor(255, 138, 36, 32));
    painter.drawRect(normalized);
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
    selectionBoxActive_ = false;
    anchor_.reset();
    setProperty("dragPointLineId", QVariant());
    setProperty("dragPointStart", QVariant());

    // Cancel unfinished constraint-tool selections as well.
    coincidentFirstPoint_.reset();
    setProperty("pointOnLineCarrier", QVariant());
    setProperty("pointOnCircleCarrier", QVariant());
    setProperty("perpendicularFirstLine", QVariant());
    setProperty("parallelFirstLine", QVariant());
    setProperty("equalFirstGeometry", QVariant());
    setProperty("equalFirstKind", QVariant());
    setProperty("equalFirstElement", QVariant());
    setProperty("tangentFirstGeometry", QVariant());
    setProperty("tangentFirstKind", QVariant());

    rectanglePoints_.clear();
    circlePoints_.clear();
    circleGuideLines_.clear();
    hideDimensionEditor();
    setCursor(tool_ == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
    update();
    return;
  }
  if (event->button() != Qt::LeftButton) return;
  if (tool_ == Tool::AutoDimension && primaryDimension_->isVisible() &&
      !property("autoDimensionTarget").toString().isEmpty()) {
    const auto directLineId = static_cast<sketch::GeometryId>(
        property("autoDimensionDirectLineId").toULongLong());

    if (directLineId != sketch::kInvalidGeometryId &&
        property("autoDimensionTarget").toString() != QStringLiteral("angle")) {
      double bestDistance = 9.0;
      std::optional<std::size_t> secondLineIndex;

      for (std::size_t index = 0; index < sketch_.lines().size(); ++index) {
        const auto candidateId = sketch_.lineId(index);
        if (candidateId == directLineId) continue;

        const auto& line = sketch_.lines()[index];
        const double distance = pointSegmentDistance(
            event->position(), mapPoint(line.start), mapPoint(line.end));

        if (distance < bestDistance) {
          bestDistance = distance;
          secondLineIndex = index;
        }
      }

      if (secondLineIndex) {
        const auto firstIndex = sketch_.lineIndex(directLineId);
        if (firstIndex) {
          const auto secondId = sketch_.lineId(*secondLineIndex);
          // CRASH-FREE 12: INITIAL VISIBLE ANGLE VALUE
          const double angle =
              visibleLineAngleDegrees(
                  sketch_.lines()[*firstIndex],
                  sketch_.lines()[*secondLineIndex]);

          if (angle > 1e-6 && angle < 180.0 - 1e-6) {
            setProperty("autoDimensionTarget", "angle");
            setProperty("autoDimensionAngleFirstLine",
                        static_cast<qulonglong>(directLineId));
            setProperty("autoDimensionAngleSecondLine",
                        static_cast<qulonglong>(secondId));
            setProperty("autoDimensionOffsetMm", 12.0);

            primaryDimension_->setPrefix(QString());
            primaryDimension_->setSuffix(QString::fromUtf8("°"));
            primaryDimension_->setRange(0.01, 179.99);
            primaryDimension_->setValue(angle);
            primaryDimension_->move(
                (event->position() + QPointF(16, 16)).toPoint());
            primaryDimension_->show();
            primaryDimension_->setFocus();
            primaryDimension_->selectAll();

            update();
            event->accept();
            return;
          }
        }
      }
    }

    commitAutoDimension();
    event->accept();
    return;
  }
  // Installed dimensions are selectable/draggable only in Select mode.
  // While AutoDimension or a constraint tool is active, dimensions are
  // transparent to mouse hit-testing so geometry interaction cannot be
  // stolen by an existing annotation.
  if (tool_ == Tool::Select &&
      !event->modifiers().testFlag(Qt::ControlModifier) &&
      beginDimensionLabelDrag(event->position())) {
    event->accept();
    return;
  }
  if (tool_ == Tool::Select &&
      !event->modifiers().testFlag(Qt::ControlModifier) &&
      beginDimensionLineDrag(event->position())) {
    event->accept();
    return;
  }
  setProperty("selectedDimension", QVariant());
  if (tool_ == Tool::AutoDimension) {
    handleAutoDimensionClick(event->position());
  } else if (tool_ == Tool::OrthogonalConstraint) {
    handleOrthogonalConstraintClick(event->position());
  } else if (tool_ == Tool::CoincidentConstraint) {
    handleCoincidentConstraintClick(event->position());
  } else if (tool_ == Tool::PerpendicularConstraint) {
    handlePerpendicularConstraintClick(event->position());
  } else if (tool_ == Tool::ParallelConstraint) {
    handleParallelConstraintClick(event->position());
  } else if (tool_ == Tool::EqualConstraint) {
    handleEqualConstraintClick(event->position());
  } else if (tool_ == Tool::TangentConstraint) {
    handleTangentConstraintClick(event->position());
  } else if (tool_ == Tool::Select) {
    const bool additive =
        event->modifiers().testFlag(Qt::ControlModifier);

    // LMB drag on empty canvas starts the selection rectangle. A click on
    // existing geometry keeps the normal select/move behaviour.
    bool geometryHit = false;
    constexpr double geometryHitTolerance = 9.0;

    for (const auto& line : sketch_.lines()) {
      if (pointSegmentDistance(event->position(),
                               mapPoint(line.start),
                               mapPoint(line.end)) <
          geometryHitTolerance) {
        geometryHit = true;
        break;
      }
    }

    if (!geometryHit) {
      for (const auto& circle : sketch_.circles()) {
        const double distance = std::abs(
            QLineF(event->position(), mapPoint(circle.center)).length() -
            circle.radiusMm * pixelsPerMm_);

        if (distance < geometryHitTolerance) {
          geometryHit = true;
          break;
        }
      }
    }

    if (!geometryHit) {
      selectionBoxActive_ = true;
      selectionBoxStart_ = event->position();
      selectionBoxCurrent_ = event->position();
      selectionBoxAdditive_ = additive;

      if (!additive) {
        clearGeometrySelection();
        emit lineStyleSelectionChanged(false, false);
        emit selectionChanged(QString::fromUtf8("Ничего не выбрано"));
      }

      setCursor(Qt::CrossCursor);
      event->accept();
      update();
      return;
    }

    if (additive) {
      // Ctrl+LMB toggles complete CAD objects. Endpoint editing is deliberately
      // bypassed here so Ctrl always means selection-set modification.
      setProperty("dragPointLineId", QVariant());
      setProperty("dragPointStart", QVariant());
      selectAt(event->position(), true, false);
      event->accept();
      return;
    }

    constexpr double endpointTolerance = 9.0;
    double bestEndpointDistance = endpointTolerance;
    std::optional<sketch::PointReference> endpoint;
    std::size_t endpointElementId = 0;
    bool endpointDashed = false;

    for (std::size_t index = 0; index < sketch_.lines().size(); ++index) {
      const auto& line = sketch_.lines()[index];
      const auto lineId = sketch_.lineId(index);
      if (lineId == sketch::kInvalidGeometryId) continue;

      for (const bool start : {true, false}) {
        const auto point = start ? line.start : line.end;
        const double distance =
            QLineF(event->position(), mapPoint(point)).length();

        if (distance < bestEndpointDistance) {
          bestEndpointDistance = distance;
          endpoint = sketch::PointReference{lineId, start};
          endpointElementId = line.elementId;
          endpointDashed = line.dashed;
        }
      }
    }

    const bool hasSeveralSelected =
        selectedElementIds_.size() + selectedCircleIds_.size() > 1;

    // Once several objects are selected, clicking any selected object means
    // "move the selection". Endpoint editing still works normally for a
    // single object.
    if (endpoint &&
        !(hasSeveralSelected && lineElementSelected(endpointElementId))) {
      clearGeometrySelection();
      selectedElementIds_.push_back(endpointElementId);
      selectionKind_ = SelectionKind::Line;
      selectionLineId_ = endpoint->lineId;
      selectionElementId_ = endpointElementId;
      selectionCircleId_ = sketch::kInvalidGeometryId;

      setProperty("dragPointLineId",
                  static_cast<qulonglong>(endpoint->lineId));
      setProperty("dragPointStart", endpoint->start);

      pushUndoState();
      dragging_ = true;
      dragPoint_ = snappedPoint(event->position());

      emit lineStyleSelectionChanged(true, endpointDashed);
      emit selectionChanged(QString::fromUtf8("Выбрана точка линии"));
      update();
    } else {
      setProperty("dragPointLineId", QVariant());
      setProperty("dragPointStart", QVariant());

      selectAt(event->position(), false, true);
      if (selectionKind_ != SelectionKind::None) {
        pushUndoState();
        dragging_ = true;
        dragPoint_ = snappedPoint(event->position());
      }
    }  } else {
    sketch::Point constructionPoint =
        snappedPoint(event->position());

    // UNIFIED CONSTRUCTION CAD POINT SNAP
    //
    // Every geometry creation tool uses the same first-class CAD points:
    // line endpoints, circle centres and virtual rectangle centres.
    // The picked construction point is moved exactly onto the nearest
    // existing CAD point before commitPoint() sees it.
    if (snapEnabled_ &&
        (tool_ == Tool::Line ||
         tool_ == Tool::Rectangle ||
         tool_ == Tool::Circle)) {
      constexpr double cadPointTolerancePx = 10.0;
      double bestDistance = cadPointTolerancePx;

      const auto considerPoint =
          [this, &constructionPoint,
           &bestDistance, event](
              sketch::Point candidate) {
            const double distance =
                QLineF(
                    event->position(),
                    mapPoint(candidate)).length();

            if (distance < bestDistance) {
              bestDistance = distance;
              constructionPoint = candidate;
            }
          };

      for (const auto& line : sketch_.lines()) {
        considerPoint(line.start);
        considerPoint(line.end);
      }

      for (const auto& circle : sketch_.circles())
        considerPoint(circle.center);

      for (const auto elementId :
           sketch_.centerNodeElementIds()) {
        const auto center =
            sketch_.elementCenterPoint(elementId);

        if (center)
          considerPoint(*center);
      }
    }
    commitPoint(constructionPoint);
  }
}

void SketchCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    QWidget::mouseDoubleClickEvent(event);
    return;
  }

  const auto found = dimensionAt(event->position());

  if (!found ||
      *found >= sketch_.dimensions().size()) {
    QWidget::mouseDoubleClickEvent(event);
    return;
  }

  const auto dimension =
      sketch_.dimensions()[*found];

  setTool(Tool::AutoDimension);

  setProperty("editingDimensionIndex",
              static_cast<qulonglong>(*found));

  // CRASH-FREE 13: KEEP EDITED DIMENSION SELECTED
  setProperty("selectedDimension",
              static_cast<qulonglong>(*found));
  setProperty("autoDimensionOffsetMm",
              dimension.offsetMm);
  setProperty("autoDimensionAngleRad",
              dimension.angleRad);

  if (dimension.kind ==
      sketch::DimensionKind::LineLength) {
    setProperty("autoDimensionTarget", "line");
    setProperty("autoDimensionIndex",
                static_cast<qulonglong>(
                    dimension.geometryId));
    primaryDimension_->setPrefix(QString());

  } else if (dimension.kind ==
             sketch::DimensionKind::CircleDiameter) {
    setProperty("autoDimensionTarget", "circle");
    setProperty("autoDimensionIndex",
                static_cast<qulonglong>(
                    dimension.geometryId));
    primaryDimension_->setPrefix(
        QString::fromUtf8("Г: "));

  } else if (dimension.kind ==
             sketch::DimensionKind::LineAngle) {
    setProperty("autoDimensionTarget", "angle");
    setProperty("autoDimensionAngleFirstLine",
                static_cast<qulonglong>(
                    dimension.geometryId));
    setProperty("autoDimensionAngleSecondLine",
                static_cast<qulonglong>(
                    dimension.secondPoint.lineId));
    primaryDimension_->setPrefix(QString());

  } else {
    setProperty("autoDimensionTarget", "points");

    if (dimension.kind ==
        sketch::DimensionKind::PointDistanceX)
      setProperty("autoDimensionPointMode", "x");
    else if (dimension.kind ==
             sketch::DimensionKind::PointDistanceY)
      setProperty("autoDimensionPointMode", "y");
    else
      setProperty("autoDimensionPointMode",
                  "aligned");

    setProperty("autoDimensionFirstLine",
                static_cast<qulonglong>(
                    dimension.firstPoint.lineId));
    setProperty("autoDimensionFirstStart",
                dimension.firstPoint.start);
    setProperty(
        "autoDimensionFirstCircle",
        static_cast<qulonglong>(
            dimension.firstPoint.circleId));
    setProperty(
        "autoDimensionFirstElementCenter",
        static_cast<qulonglong>(
            dimension.firstPoint.elementCenterId));

    setProperty("autoDimensionSecondLine",
                static_cast<qulonglong>(
                    dimension.secondPoint.lineId));
    setProperty("autoDimensionSecondStart",
                dimension.secondPoint.start);
    setProperty(
        "autoDimensionSecondCircle",
        static_cast<qulonglong>(
            dimension.secondPoint.circleId));
    setProperty(
        "autoDimensionSecondElementCenter",
        static_cast<qulonglong>(
            dimension.secondPoint.elementCenterId));

    primaryDimension_->setPrefix(QString());
  }

  if (dimension.kind ==
      sketch::DimensionKind::LineAngle) {
    primaryDimension_->setSuffix(
        QString(QChar(0x00B0)));
    primaryDimension_->setRange(0.01, 179.99);

    const auto firstIndex =
        sketch_.lineIndex(dimension.geometryId);
    const auto secondIndex =
        sketch_.lineIndex(
            dimension.secondPoint.lineId);

    if (firstIndex && secondIndex) {
      primaryDimension_->setValue(
          visibleLineAngleDegrees(
              sketch_.lines()[*firstIndex],
              sketch_.lines()[*secondIndex]));
    } else {
      primaryDimension_->setValue(
          dimension.valueMm);
    }
  } else {
    primaryDimension_->setSuffix(QString::fromUtf8("\xD0\xBC\xD0\xBC"));
    primaryDimension_->setRange(
        0.01, 100000.0);
    primaryDimension_->setValue(
        dimension.valueMm);
  }

  secondaryDimension_->hide();

  primaryDimension_->move(
      (event->position() +
       QPointF(16, 16)).toPoint());

  primaryDimension_->show();
  primaryDimension_->setFocus();
  primaryDimension_->selectAll();

  event->accept();
  update();
}

void SketchCanvas::mouseMoveEvent(QMouseEvent* event) {
  if (selectionBoxActive_ &&
      (event->buttons() & Qt::LeftButton)) {
    selectionBoxCurrent_ = event->position();
    update();
    event->accept();
    return;
  }

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

  // Live center-node preview is restricted to creation tools.
  // Select/drag, AutoDimension and constraint tools retain their original
  // snapping and cannot be affected by virtual rectangle centers.
  if (snapEnabled_ &&
      (tool_ == Tool::Line ||
       tool_ == Tool::Rectangle ||
       tool_ == Tool::Circle)) {
    constexpr double centerNodeTolerancePx = 10.0;
    double bestDistance = centerNodeTolerancePx;

    for (const auto elementId : sketch_.centerNodeElementIds()) {
      const auto center =
          sketch_.elementCenterPoint(elementId);
      if (!center) continue;

      const double distance =
          QLineF(event->position(),
                 mapPoint(*center)).length();

      if (distance < bestDistance) {
        bestDistance = distance;
        hoverPoint_ = *center;
      }
    }
  }
  // TWO-TANGENT MOUSE DIAMETER DRIVE
  if (tool_ == Tool::Circle &&
      circleMode_ ==
          CircleMode::TwoTangentsRadius &&
      circleGuideLines_.size() == 2 &&
      property(
          "twoTangentRadiusPreviewActive")
          .toBool()) {
    primaryDimension_->setPrefix(
        QString::fromUtf8("Ø "));
    primaryDimension_->setSuffix(
        QString::fromUtf8(" мм"));
    primaryDimension_->setRange(
        0.02, 100000.0);

    if (!primaryDimension_->isVisible()) {
      primaryDimension_->show();
      primaryDimension_->raise();
    }

    if (!primaryDimension_->hasFocus()) {
      const auto cursorPreview =
          twoTangentCircleFromCursor(
              circleGuideLines_[0],
              circleGuideLines_[1],
              hoverPoint_);

      if (cursorPreview) {
        const auto finitePreview =
            clampedTwoTangentCircleForRadius(
                circleGuideLines_[0],
                circleGuideLines_[1],
                cursorPreview->radiusMm,
                hoverPoint_);

        if (finitePreview) {
          circleDiameterMm_ =
              finitePreview->radiusMm * 2.0;

          const QSignalBlocker blocker(
              primaryDimension_);

          primaryDimension_->setValue(
              circleDiameterMm_);

          primaryDimension_->move(
              (event->position() +
               QPointF(18.0, 18.0)).toPoint());

          emit primaryDimensionChanged(
              circleDiameterMm_);
        }
      }
    } else {
      const auto finitePreview =
          clampedTwoTangentCircleForRadius(
              circleGuideLines_[0],
              circleGuideLines_[1],
              primaryDimension_->value() * 0.5,
              hoverPoint_);

      if (finitePreview) {
        const double actualDiameter =
            finitePreview->radiusMm * 2.0;

        circleDiameterMm_ =
            actualDiameter;

        if (std::abs(
                primaryDimension_->value() -
                actualDiameter) > 1e-6) {
          const QSignalBlocker blocker(
              primaryDimension_);
          primaryDimension_->setValue(
              actualDiameter);
          primaryDimension_->selectAll();
        }
      }
    }

    update();
  }
  if (property("draggingDimensionLabel").isValid() &&
      (event->buttons() & Qt::LeftButton)) {
    const auto index = static_cast<std::size_t>(
        property("draggingDimensionLabel").toULongLong());

    if (index < sketch_.dimensions().size() &&
        sketch_.dimensions()[index].kind ==
            sketch::DimensionKind::LineAngle) {
      const auto& dimension = sketch_.dimensions()[index];
      const auto firstIndex = sketch_.lineIndex(dimension.geometryId);
      const auto secondIndex =
          sketch_.lineIndex(dimension.secondPoint.lineId);

      if (firstIndex && secondIndex) {
        const auto& firstLine = sketch_.lines()[*firstIndex];
        const auto& secondLine = sketch_.lines()[*secondIndex];
        const auto center = lineIntersectionScreen(
            firstLine, secondLine,
            [this](sketch::Point point) { return mapPoint(point); });

        if (center) {
          QPointF firstDirection =
              angleRayTowardSegment(
                  *center,
                  mapPoint(firstLine.start),
                  mapPoint(firstLine.end));

          QPointF secondDirection =
              angleRayTowardSegment(
                  *center,
                  mapPoint(secondLine.start),
                  mapPoint(secondLine.end));

          const double firstLength =
              std::hypot(firstDirection.x(), firstDirection.y());
          const double secondLength =
              std::hypot(secondDirection.x(), secondDirection.y());

          if (firstLength > 1.0 && secondLength > 1.0) {
            firstDirection /= firstLength;
            secondDirection /= secondLength;

            double startDeg =
                -std::atan2(firstDirection.y(), firstDirection.x()) *
                180.0 / 3.14159265358979323846;
            double endDeg =
                -std::atan2(secondDirection.y(), secondDirection.x()) *
                180.0 / 3.14159265358979323846;
            double spanDeg = endDeg - startDeg;
            while (spanDeg <= -180.0) spanDeg += 360.0;
            while (spanDeg > 180.0) spanDeg -= 360.0;

            const double radius =
                std::max(16.0,
                         std::abs(dimension.offsetMm) * pixelsPerMm_);
            const double midDeg = startDeg + spanDeg * 0.5;
            const double midRad =
                midDeg * 3.14159265358979323846 / 180.0;

            const QPointF defaultLabelCenter =
                *center +
                QPointF(std::cos(midRad), -std::sin(midRad)) *
                    (radius + 18.0);

            const QPointF delta =
                event->position() - defaultLabelCenter;

            QVariantList alongValues =
                property("dimensionLabelAlongMm").toList();
            QVariantList offsetValues =
                property("dimensionLabelOffsetMm").toList();

            while (alongValues.size() <= static_cast<int>(index))
              alongValues.push_back(0.0);
            while (offsetValues.size() <= static_cast<int>(index))
              offsetValues.push_back(0.0);

            alongValues[static_cast<int>(index)] =
                delta.x() / pixelsPerMm_;
            offsetValues[static_cast<int>(index)] =
                delta.y() / pixelsPerMm_;

            setProperty("dimensionLabelAlongMm", alongValues);
            setProperty("dimensionLabelOffsetMm", offsetValues);
            update();
          }
        }
      }

      event->accept();
      return;
    }

    QPointF first;
    QPointF second;
    if (dimensionSegment(index, first, second)) {
      QPointF direction = second - first;
      const double length = std::hypot(direction.x(), direction.y());
      if (length > 1.0) {
        direction /= length;
        const QPointF normal(-direction.y(), direction.x());
        const QPointF delta = event->position() - (first + second) * 0.5;
        QVariantList alongValues = property("dimensionLabelAlongMm").toList();
        QVariantList offsetValues = property("dimensionLabelOffsetMm").toList();
        while (alongValues.size() <= static_cast<int>(index))
          alongValues.push_back(0.0);
        while (offsetValues.size() <= static_cast<int>(index))
          offsetValues.push_back(2.0);
        alongValues[static_cast<int>(index)] =
            QPointF::dotProduct(delta, direction) / pixelsPerMm_;
        offsetValues[static_cast<int>(index)] =
            QPointF::dotProduct(delta, normal) / pixelsPerMm_;
        setProperty("dimensionLabelAlongMm", alongValues);
        setProperty("dimensionLabelOffsetMm", offsetValues);
        update();
      }
    }
    event->accept();
    return;
  }
  if (property("draggingDimensionLine").isValid() &&
      (event->buttons() & Qt::LeftButton)) {
    const auto index = static_cast<std::size_t>(
        property("draggingDimensionLine").toULongLong());
    if (index < sketch_.dimensions().size()) {
      const auto& dimension = sketch_.dimensions()[index];
      if (dimension.kind == sketch::DimensionKind::LineAngle) {
        const auto firstIndex = sketch_.lineIndex(dimension.geometryId);
        const auto secondIndex =
            sketch_.lineIndex(dimension.secondPoint.lineId);

        if (firstIndex && secondIndex) {
          const auto center = lineIntersectionScreen(
              sketch_.lines()[*firstIndex],
              sketch_.lines()[*secondIndex],
              [this](sketch::Point point) { return mapPoint(point); });

          if (center) {
            const double radiusPixels =
                QLineF(*center, event->position()).length();
            sketch_.setDimensionPlacement(
                index,
                std::max(3.0, radiusPixels / pixelsPerMm_),
                dimension.angleRad);
          }
        }
      } else if (dimension.kind == sketch::DimensionKind::CircleDiameter) {
        const auto circleIndex = sketch_.circleIndex(dimension.geometryId);
        if (circleIndex) {
          const QPointF center =
              mapPoint(sketch_.circles()[*circleIndex].center);
          const QPointF delta = event->position() - center;
          if (std::hypot(delta.x(), delta.y()) > 2.0)
            sketch_.setDimensionPlacement(
                index, dimension.offsetMm,
                std::atan2(-delta.y(), delta.x()));
        }
      } else {
        QPointF first;
        QPointF second;
        if (dimensionSegment(index, first, second)) {
          QPointF direction = second - first;
          const double length = std::hypot(direction.x(), direction.y());
          if (length > 1.0) {
            direction /= length;
            const QPointF normal(-direction.y(), direction.x());
            const QPointF rawFirst =
                first - normal * dimension.offsetMm * pixelsPerMm_;
            const double signedPixels =
                QPointF::dotProduct(event->position() - rawFirst, normal);
            sketch_.setDimensionPlacement(index, signedPixels / pixelsPerMm_,
                                          dimension.angleRad);
          }
        }
      }
      update();
    }
    event->accept();
    return;
  }
  if (tool_ == Tool::AutoDimension && primaryDimension_->isVisible() &&
      property("autoDimensionTarget").isValid()) {
    // CRASH-FREE 13: EXISTING DIMENSION EDIT LOCK
    //
    // Double-click restoration already sets the exact stored dimension kind:
    // aligned / X / Y / line / circle / angle. Cursor movement must not run
    // the new-dimension heuristic again, must not move the editor and must not
    // alter annotation placement. Only the typed numeric value is editable.
    if (property("editingDimensionIndex").isValid()) {
      update();
    } else {
    const QString target =
        property("autoDimensionTarget").toString();

    if (target == "angle") {
      const auto firstId = static_cast<sketch::GeometryId>(
          property("autoDimensionAngleFirstLine").toULongLong());
      const auto secondId = static_cast<sketch::GeometryId>(
          property("autoDimensionAngleSecondLine").toULongLong());
      const auto firstIndex = sketch_.lineIndex(firstId);
      const auto secondIndex = sketch_.lineIndex(secondId);

      if (firstIndex && secondIndex) {
        const auto center = lineIntersectionScreen(
            sketch_.lines()[*firstIndex], sketch_.lines()[*secondIndex],
            [this](sketch::Point point) { return mapPoint(point); });
        if (center) {
          const double radiusPixels =
              QLineF(*center, event->position()).length();
          setProperty("autoDimensionOffsetMm",
                      std::max(3.0, radiusPixels / pixelsPerMm_));
          primaryDimension_->move(
              (event->position() + QPointF(16, 16)).toPoint());
        }
      }
    } else if (target == "circle") {
      const auto id = static_cast<sketch::GeometryId>(
          property("autoDimensionIndex").toULongLong());
      const auto index = sketch_.circleIndex(id);
      if (index) {
        const auto center =
            mapPoint(sketch_.circles()[*index].center);
        const QPointF delta = event->position() - center;
        if (std::hypot(delta.x(), delta.y()) > 2.0)
          setProperty("autoDimensionAngleRad",
                      std::atan2(-delta.y(), delta.x()));
      }
    } else if (target == "line") {
      const auto id = static_cast<sketch::GeometryId>(
          property("autoDimensionIndex").toULongLong());
      const auto index = sketch_.lineIndex(id);
      if (index) {
        const QPointF first =
            mapPoint(sketch_.lines()[*index].start);
        const QPointF second =
            mapPoint(sketch_.lines()[*index].end);

        QPointF direction = second - first;
        const double length =
            std::hypot(direction.x(), direction.y());
        if (length > 1.0) {
          direction /= length;
          const QPointF normal(-direction.y(), direction.x());
          const double signedPixels =
              QPointF::dotProduct(event->position() - first,
                                  normal);
          setProperty("autoDimensionOffsetMm",
                      signedPixels / pixelsPerMm_);
        }
      }
    } else if (target == "points") {
      const sketch::PointReference firstReference{
          static_cast<sketch::GeometryId>(
              property("autoDimensionFirstLine").toULongLong()),
          property("autoDimensionFirstStart").toBool(),
          static_cast<sketch::GeometryId>(
            property("autoDimensionFirstCircle").toULongLong()),
        static_cast<std::size_t>(
            property("autoDimensionFirstElementCenter").toULongLong())};
      const sketch::PointReference secondReference{
          static_cast<sketch::GeometryId>(
              property("autoDimensionSecondLine").toULongLong()),
          property("autoDimensionSecondStart").toBool(),
          static_cast<sketch::GeometryId>(
            property("autoDimensionSecondCircle").toULongLong()),
        static_cast<std::size_t>(
            property("autoDimensionSecondElementCenter").toULongLong())};

      const auto firstPoint =
          sketch_.referencedPoint(firstReference);
      const auto secondPoint =
          sketch_.referencedPoint(secondReference);

      if (firstPoint && secondPoint) {
        const QPointF first = mapPoint(*firstPoint);
        const QPointF second = mapPoint(*secondPoint);
        const QPointF cursor = event->position();
        const QPointF midpoint = (first + second) * 0.5;
        const QPointF fromMid = cursor - midpoint;

        constexpr double axisBias = 2.20;
        QString mode = QStringLiteral("aligned");

        const double deltaX =
            std::abs(secondPoint->xMm - firstPoint->xMm);
        const double deltaY =
            std::abs(secondPoint->yMm - firstPoint->yMm);
        constexpr double projectionEpsilonMm = 1e-6;

        if (std::abs(fromMid.y()) >
                std::abs(fromMid.x()) * axisBias &&
            deltaX > projectionEpsilonMm) {
          mode = QStringLiteral("x");
        } else if (std::abs(fromMid.x()) >
                       std::abs(fromMid.y()) * axisBias &&
                   deltaY > projectionEpsilonMm) {
          mode = QStringLiteral("y");
        }

        setProperty("autoDimensionPointMode", mode);

        QPointF baseFirst = first;
        QPointF baseSecond = second;

        if (mode == QStringLiteral("x")) {
          baseSecond.setY(baseFirst.y());
          primaryDimension_->setValue(
              std::abs(secondPoint->xMm - firstPoint->xMm));
        } else if (mode == QStringLiteral("y")) {
          baseSecond.setX(baseFirst.x());
          primaryDimension_->setValue(
              std::abs(secondPoint->yMm - firstPoint->yMm));
        } else {
          primaryDimension_->setValue(
              std::hypot(secondPoint->xMm - firstPoint->xMm,
                         secondPoint->yMm - firstPoint->yMm));
        }

        QPointF direction = baseSecond - baseFirst;
        const double length =
            std::hypot(direction.x(), direction.y());

        if (length > 1.0) {
          direction /= length;
          const QPointF normal(-direction.y(), direction.x());
          const double signedPixels =
              QPointF::dotProduct(cursor - baseFirst, normal);
          setProperty("autoDimensionOffsetMm",
                      signedPixels / pixelsPerMm_);
        }

        // Keep the editor close to the cursor while previewing.
        primaryDimension_->move(
            (event->position() + QPointF(16, 16)).toPoint());
      }
    }

    update();
      }
}  if (dragging_ && (event->buttons() & Qt::LeftButton)) {
    const auto current = snappedPoint(event->position());
    const double dx = current.xMm - dragPoint_.xMm;
    const double dy = current.yMm - dragPoint_.yMm;

    if (property("dragPointLineId").isValid()) {
      const sketch::PointReference reference{
          static_cast<sketch::GeometryId>(
              property("dragPointLineId").toULongLong()),
          property("dragPointStart").toBool()};
      sketch_.translatePoint(reference, dx, dy);
    } else if (!selectedElementIds_.empty() ||
               !selectedCircleIds_.empty()) {
      sketch_.translateSelection(selectedElementIds_, selectedCircleIds_,
                                 dx, dy);
    } else if (selectionKind_ == SelectionKind::Line) {
      sketch_.translateElement(selectionElementId_, dx, dy);
    } else if (selectionKind_ == SelectionKind::Circle) {
      sketch_.translateCircleById(selectionCircleId_, dx, dy);
    }

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
  if (event->button() == Qt::LeftButton && selectionBoxActive_) {
    selectionBoxCurrent_ = event->position();
    const QRectF selectionRect(selectionBoxStart_, selectionBoxCurrent_);

    selectionBoxActive_ = false;
    setCursor(tool_ == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);

    if (selectionRect.normalized().width() >= 3.0 ||
        selectionRect.normalized().height() >= 3.0) {
      selectInRect(selectionRect.normalized(), selectionBoxAdditive_);
    } else if (!selectionBoxAdditive_) {
      clearGeometrySelection();
      emit lineStyleSelectionChanged(false, false);
      emit selectionChanged(QString::fromUtf8("Ничего не выбрано"));
    }

    event->accept();
    update();
    return;
  }

  if (event->button() == Qt::MiddleButton &&
      property("sketchPanning").toBool()) {
    setProperty("sketchPanning", false);
    setCursor(tool_ == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
    event->accept();
    return;
  }
  if (event->button() == Qt::LeftButton && dragging_) {
    dragging_ = false;
    setProperty("dragPointLineId", QVariant());
    setProperty("dragPointStart", QVariant());
    notifyGeometryChanged();
  }
  if (event->button() == Qt::LeftButton &&
      property("draggingDimensionLabel").isValid()) {
    setProperty("draggingDimensionLabel", QVariant());
    setCursor(tool_ == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
    event->accept();
  }
  if (event->button() == Qt::LeftButton &&
      property("draggingDimensionLine").isValid()) {
    setProperty("draggingDimensionLine", QVariant());
    setCursor(tool_ == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
    event->accept();
  }
}

void SketchCanvas::keyPressEvent(QKeyEvent* event) {
  const auto copyCurrentSelection = [this]() -> bool {
    gSketchClipboard.clear();

    std::vector<std::size_t> elementIds = selectedElementIds_;
    std::vector<sketch::GeometryId> circleIds = selectedCircleIds_;

    if (elementIds.empty() && circleIds.empty()) {
      if (selectionKind_ == SelectionKind::Line &&
          selectionElementId_ != 0) {
        elementIds.push_back(selectionElementId_);
      } else if (selectionKind_ == SelectionKind::Circle &&
                 selectionCircleId_ != sketch::kInvalidGeometryId) {
        circleIds.push_back(selectionCircleId_);
      }
    }

    for (const auto elementId : elementIds) {
      std::vector<std::size_t> indices;

      for (std::size_t index = 0;
           index < sketch_.lines().size();
           ++index) {
        if (sketch_.lines()[index].elementId == elementId)
          indices.push_back(index);
      }

      if (indices.empty()) continue;

      if (indices.size() == 4) {
        SketchClipboardElement item;
        item.kind = SketchClipboardElement::Kind::Rectangle;

        for (std::size_t side = 0; side < 4; ++side)
          item.rectanglePoints[side] =
              sketch_.lines()[indices[side]].start;

        gSketchClipboard.elements.push_back(item);
        continue;
      }

      for (const auto index : indices) {
        const auto& line = sketch_.lines()[index];

        SketchClipboardElement item;
        item.kind = SketchClipboardElement::Kind::Line;
        item.lineStart = line.start;
        item.lineEnd = line.end;
        gSketchClipboard.elements.push_back(item);
      }
    }

    for (const auto circleId : circleIds) {
      const auto index = sketch_.circleIndex(circleId);
      if (!index) continue;

      SketchClipboardElement item;
      item.kind = SketchClipboardElement::Kind::Circle;
      item.circleCenter = sketch_.circles()[*index].center;
      item.circleRadiusMm = sketch_.circles()[*index].radiusMm;
      gSketchClipboard.elements.push_back(item);
    }

    gSketchClipboard.pasteGeneration = 0;
    return !gSketchClipboard.empty();
  };

  const auto pasteClipboard = [this]() -> bool {
    if (gSketchClipboard.empty()) return false;

    pushUndoState();
    ++gSketchClipboard.pasteGeneration;

    const double offsetMm =
        6.0 * static_cast<double>(gSketchClipboard.pasteGeneration);

    const std::size_t oldLineCount = sketch_.lines().size();
    const std::size_t oldCircleCount = sketch_.circles().size();

    const auto shifted =
        [offsetMm](sketch::Point point) {
          point.xMm += offsetMm;
          point.yMm -= offsetMm;
          return point;
        };

    for (const auto& item : gSketchClipboard.elements) {
      switch (item.kind) {
        case SketchClipboardElement::Kind::Line:
          sketch_.addLine(
              shifted(item.lineStart),
              shifted(item.lineEnd));
          break;

        case SketchClipboardElement::Kind::Rectangle:
          sketch_.addRectangle(
              shifted(item.rectanglePoints[0]),
              shifted(item.rectanglePoints[1]),
              shifted(item.rectanglePoints[2]),
              shifted(item.rectanglePoints[3]));
          break;

        case SketchClipboardElement::Kind::Circle:
          sketch_.addCircle(
              shifted(item.circleCenter),
              item.circleRadiusMm);
          break;
      }
    }

    clearGeometrySelection();

    for (std::size_t index = oldLineCount;
         index < sketch_.lines().size();
         ++index) {
      const auto elementId = sketch_.lines()[index].elementId;

      if (std::find(selectedElementIds_.begin(),
                    selectedElementIds_.end(),
                    elementId) == selectedElementIds_.end())
        selectedElementIds_.push_back(elementId);
    }

    for (std::size_t index = oldCircleCount;
         index < sketch_.circles().size();
         ++index) {
      const auto id = sketch_.circleId(index);
      if (id != sketch::kInvalidGeometryId)
        selectedCircleIds_.push_back(id);
    }

    if (!selectedElementIds_.empty()) {
      const auto elementId = selectedElementIds_.back();

      for (std::size_t index = sketch_.lines().size();
           index > 0;
           --index) {
        const auto current = index - 1;
        if (sketch_.lines()[current].elementId != elementId)
          continue;

        selectionKind_ = SelectionKind::Line;
        selectionElementId_ = elementId;
        selectionLineId_ = sketch_.lineId(current);
        selectionCircleId_ = sketch::kInvalidGeometryId;
        break;
      }

      emit lineStyleSelectionChanged(true, false);
    } else if (!selectedCircleIds_.empty()) {
      selectionKind_ = SelectionKind::Circle;
      selectionCircleId_ = selectedCircleIds_.back();
      selectionLineId_ = sketch::kInvalidGeometryId;
      selectionElementId_ = 0;
      emit lineStyleSelectionChanged(false, false);
    }

    emit selectionChanged(QString::fromUtf8("Вставленные объекты"));
    notifyGeometryChanged();
    update();
    return true;
  };

  // StandardKey matching is layout-aware. Ctrl+A/C/X/V therefore works
  // with both English and Russian keyboard layouts.
  if (event->matches(QKeySequence::SelectAll)) {
    clearGeometrySelection();

    for (const auto& line : sketch_.lines()) {
      if (std::find(selectedElementIds_.begin(),
                    selectedElementIds_.end(),
                    line.elementId) == selectedElementIds_.end()) {
        selectedElementIds_.push_back(line.elementId);
      }
    }

    for (std::size_t index = 0;
         index < sketch_.circles().size();
         ++index) {
      const auto id = sketch_.circleId(index);
      if (id != sketch::kInvalidGeometryId)
        selectedCircleIds_.push_back(id);
    }

    if (!selectedElementIds_.empty()) {
      const auto elementId = selectedElementIds_.back();

      for (std::size_t index = sketch_.lines().size();
           index > 0;
           --index) {
        const auto current = index - 1;
        if (sketch_.lines()[current].elementId != elementId)
          continue;

        selectionKind_ = SelectionKind::Line;
        selectionElementId_ = elementId;
        selectionLineId_ = sketch_.lineId(current);
        selectionCircleId_ = sketch::kInvalidGeometryId;
        break;
      }

      emit lineStyleSelectionChanged(true, false);
    } else if (!selectedCircleIds_.empty()) {
      selectionKind_ = SelectionKind::Circle;
      selectionCircleId_ = selectedCircleIds_.back();
      selectionLineId_ = sketch::kInvalidGeometryId;
      selectionElementId_ = 0;
      emit lineStyleSelectionChanged(false, false);
    } else {
      emit lineStyleSelectionChanged(false, false);
    }

    emit selectionChanged(QString::fromUtf8("Выбраны все объекты"));
    update();

    event->accept();
    return;
  }

  if (event->matches(QKeySequence::Copy)) {
    if (copyCurrentSelection())
      emit selectionChanged(QString::fromUtf8("Объекты скопированы"));

    event->accept();
    return;
  }

  if (event->matches(QKeySequence::Cut)) {
    if (copyCurrentSelection()) {
      deleteSelection();
      emit selectionChanged(QString::fromUtf8("Объекты вырезаны"));
    }

    event->accept();
    return;
  }

  if (event->matches(QKeySequence::Paste)) {
    (void)pasteClipboard();

    event->accept();
    return;
  }

  if (event->key() == Qt::Key_Escape) {
    setTool(Tool::Select);
  } else if (event->key() == Qt::Key_Delete) {
    const QVariant selectedDimensionProperty =
        property("selectedDimension");

    if (selectedDimensionProperty.isValid()) {
      const auto index = static_cast<std::size_t>(
          selectedDimensionProperty.toULongLong());

      if (index < sketch_.dimensions().size()) {
        pushUndoState();

        const auto dimension = sketch_.dimensions()[index];

        const auto samePoint = [](sketch::PointReference first,
                                  sketch::PointReference second) {
          // CRASH-FREE 05: COMPLETE DELETE REFERENCE IDENTITY
          if (first.elementCenterId != 0 ||
              second.elementCenterId != 0) {
            return first.elementCenterId != 0 &&
                   second.elementCenterId != 0 &&
                   first.elementCenterId ==
                       second.elementCenterId;
          }

          if (first.circleId != sketch::kInvalidGeometryId ||
              second.circleId != sketch::kInvalidGeometryId) {
            return first.circleId != sketch::kInvalidGeometryId &&
                   second.circleId != sketch::kInvalidGeometryId &&
                   first.circleId == second.circleId;
          }

          if (first.lineId == sketch::kInvalidGeometryId ||
              second.lineId == sketch::kInvalidGeometryId)
            return false;

          return first.lineId == second.lineId &&
                 first.start == second.start;
        };

        const auto samePointPair =
            [&samePoint](const sketch::Constraint& constraint,
                         const sketch::Dimension& dimension) {
          const bool sameOrder =
              samePoint(constraint.firstPoint,
                        dimension.firstPoint) &&
              samePoint(constraint.secondPoint,
                        dimension.secondPoint);
          const bool reverseOrder =
              samePoint(constraint.firstPoint,
                        dimension.secondPoint) &&
              samePoint(constraint.secondPoint,
                        dimension.firstPoint);
          return sameOrder || reverseOrder;
        };

        std::vector<sketch::ConstraintId> constraintsToRemove;

        for (const auto& constraint : sketch_.constraints()) {
          bool matches = false;

          switch (dimension.kind) {
            case sketch::DimensionKind::LineLength:
              matches =
                  constraint.type == sketch::ConstraintType::Length &&
                  constraint.firstGeometry == dimension.geometryId;
              break;

            case sketch::DimensionKind::CircleDiameter:
              matches =
                  constraint.type == sketch::ConstraintType::Diameter &&
                  constraint.firstGeometry == dimension.geometryId;
              break;

            case sketch::DimensionKind::PointDistance:
              matches =
                  constraint.type == sketch::ConstraintType::Distance &&
                  samePointPair(constraint, dimension);
              break;

            case sketch::DimensionKind::PointDistanceX:
              matches =
                  constraint.type == sketch::ConstraintType::DistanceX &&
                  samePointPair(constraint, dimension);
              break;

            case sketch::DimensionKind::PointDistanceY:
              matches =
                  constraint.type == sketch::ConstraintType::DistanceY &&
                  samePointPair(constraint, dimension);
              break;

            case sketch::DimensionKind::LineAngle: {
              const bool sameOrder =
                  constraint.firstGeometry == dimension.geometryId &&
                  constraint.secondGeometry == dimension.secondPoint.lineId;
              const bool reverseOrder =
                  constraint.firstGeometry == dimension.secondPoint.lineId &&
                  constraint.secondGeometry == dimension.geometryId;
              matches =
                  constraint.type == sketch::ConstraintType::Angle &&
                  (sameOrder || reverseOrder);
              break;
            }
          }

          if (matches)
            constraintsToRemove.push_back(constraint.id);
        }

        for (const auto constraintId : constraintsToRemove)
          sketch_.removeConstraint(constraintId);

        // Sketch currently exposes clear/store operations for dimensions.
        // Rebuild the dimension list without the selected annotation.
        const auto dimensions = sketch_.dimensions();
        sketch_.clearDimensions();

        for (std::size_t current = 0;
             current < dimensions.size(); ++current) {
          if (current != index)
            sketch_.storeDimension(dimensions[current]);
        }

        // Keep the auxiliary label-placement arrays aligned with
        // the dimension vector.
        QVariantList alongValues =
            property("dimensionLabelAlongMm").toList();
        QVariantList offsetValues =
            property("dimensionLabelOffsetMm").toList();

        if (index < static_cast<std::size_t>(alongValues.size()))
          alongValues.removeAt(static_cast<qsizetype>(index));
        if (index < static_cast<std::size_t>(offsetValues.size()))
          offsetValues.removeAt(static_cast<qsizetype>(index));

        setProperty("dimensionLabelAlongMm", alongValues);
        setProperty("dimensionLabelOffsetMm", offsetValues);

        setProperty("selectedDimension", QVariant());
        setProperty("draggingDimensionLine", QVariant());
        setProperty("draggingDimensionLabel", QVariant());

        hideDimensionEditor();
        notifyGeometryChanged();
        update();

        event->accept();
        return;
      }

      setProperty("selectedDimension", QVariant());
    }

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
    if (keyEvent->key() == Qt::Key_Return ||
        keyEvent->key() == Qt::Key_Enter) {
      // TWO-TANGENT NUMERIC ENTER
      if (tool_ == Tool::Circle &&
          circleMode_ ==
              CircleMode::TwoTangentsRadius &&
          circleGuideLines_.size() == 2 &&
          property(
              "twoTangentRadiusPreviewActive")
              .toBool()) {
        const auto finitePreview =
            clampedTwoTangentCircleForRadius(
                circleGuideLines_[0],
                circleGuideLines_[1],
                primaryDimension_->value() * 0.5,
                hoverPoint_);

        if (finitePreview) {
          circleDiameterMm_ =
              finitePreview->radiusMm * 2.0;

          {
            const QSignalBlocker blocker(
                primaryDimension_);
            primaryDimension_->setValue(
                circleDiameterMm_);
          }

          commitCirclePoint(hoverPoint_);
        }
        return true;
      }

      if (tool_ == Tool::AutoDimension)
        commitAutoDimension();
      else
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

bool SketchCanvas::dimensionSegment(std::size_t index, QPointF& first,
                                    QPointF& second) const {
  if (index >= sketch_.dimensions().size()) return false;

  const auto& dimension = sketch_.dimensions()[index];
  const bool diameter =
      dimension.kind == sketch::DimensionKind::CircleDiameter;

  if (diameter) {
    const auto circleIndex = sketch_.circleIndex(dimension.geometryId);
    if (!circleIndex) return false;

    const auto& circle = sketch_.circles()[*circleIndex];
    const double dx = std::cos(dimension.angleRad) * circle.radiusMm;
    const double dy = std::sin(dimension.angleRad) * circle.radiusMm;

    first = mapPoint(
        {circle.center.xMm - dx, circle.center.yMm - dy});
    second = mapPoint(
        {circle.center.xMm + dx, circle.center.yMm + dy});
    return true;
  }

  sketch::Point firstPoint;
  sketch::Point secondPoint;

  if (dimension.kind == sketch::DimensionKind::LineLength) {
    const auto lineIndex = sketch_.lineIndex(dimension.geometryId);
    if (!lineIndex) return false;

    const auto& line = sketch_.lines()[*lineIndex];
    firstPoint = line.start;
    secondPoint = line.end;
  } else {
    const auto referencedFirst =
        sketch_.referencedPoint(dimension.firstPoint);
    const auto referencedSecond =
        sketch_.referencedPoint(dimension.secondPoint);

    if (!referencedFirst || !referencedSecond) return false;

    firstPoint = *referencedFirst;
    secondPoint = *referencedSecond;
  }

  const QPointF geometryFirst = mapPoint(firstPoint);
  const QPointF geometrySecond = mapPoint(secondPoint);

  first = geometryFirst;
  second = geometrySecond;

  // Hit testing must use exactly the same projected base line that is
  // painted for the stored dimension.
  if (dimension.kind == sketch::DimensionKind::PointDistanceX) {
    second.setY(first.y());
  } else if (dimension.kind == sketch::DimensionKind::PointDistanceY) {
    second.setX(first.x());
  }

  QPointF direction = second - first;
  const double length = std::hypot(direction.x(), direction.y());
  if (length < 1.0) return false;

  direction /= length;
  const QPointF normal(-direction.y(), direction.x());
  const QPointF offset =
      normal * dimension.offsetMm * pixelsPerMm_;

  first += offset;
  second += offset;

  return true;
}
QPointF SketchCanvas::dimensionLabelCenter(std::size_t index, QPointF first,
                                           QPointF second) const {
  QPointF direction = second - first;
  const double length = std::hypot(direction.x(), direction.y());
  if (length < 1.0) return (first + second) * 0.5;
  direction /= length;
  const QPointF normal(-direction.y(), direction.x());
  const QVariantList alongValues = property("dimensionLabelAlongMm").toList();
  const QVariantList offsetValues = property("dimensionLabelOffsetMm").toList();
  const double along = index < static_cast<std::size_t>(alongValues.size())
                           ? alongValues[static_cast<int>(index)].toDouble()
                           : 0.0;
  const double offset = index < static_cast<std::size_t>(offsetValues.size())
                            ? offsetValues[static_cast<int>(index)].toDouble()
                            : 2.0;
  return (first + second) * 0.5 +
         direction * along * pixelsPerMm_ + normal * offset * pixelsPerMm_;
}

bool SketchCanvas::beginDimensionLabelDrag(QPointF position) {
  for (std::size_t reverse = sketch_.dimensions().size();
       reverse > 0; --reverse) {
    const std::size_t index = reverse - 1;
    const auto& dimension = sketch_.dimensions()[index];

    if (dimension.kind == sketch::DimensionKind::LineAngle) {
      const auto firstIndex =
          sketch_.lineIndex(dimension.geometryId);
      const auto secondIndex =
          sketch_.lineIndex(dimension.secondPoint.lineId);

      if (!firstIndex || !secondIndex)
        continue;

      const auto& firstLine = sketch_.lines()[*firstIndex];
      const auto& secondLine = sketch_.lines()[*secondIndex];

      const auto center = lineIntersectionScreen(
          firstLine, secondLine,
          [this](sketch::Point point) {
            return mapPoint(point);
          });

      if (!center)
        continue;

      // ANGLE LABEL HIT GEOMETRY
      // Keep this exactly aligned with stored LineAngle rendering.
      QPointF firstDirection =
          angleRayTowardSegment(
              *center,
              mapPoint(firstLine.start),
              mapPoint(firstLine.end));

      QPointF secondDirection =
          angleRayTowardSegment(
              *center,
              mapPoint(secondLine.start),
              mapPoint(secondLine.end));

      const auto sameScreenVertex =
          [this](sketch::Point a, sketch::Point b) {
            return QLineF(mapPoint(a),
                          mapPoint(b)).length() <= 0.5;
          };

      if (sameScreenVertex(firstLine.start,
                           secondLine.start)) {
        firstDirection =
            mapPoint(firstLine.end) -
            mapPoint(firstLine.start);
        secondDirection =
            mapPoint(secondLine.end) -
            mapPoint(secondLine.start);
      } else if (sameScreenVertex(firstLine.start,
                                  secondLine.end)) {
        firstDirection =
            mapPoint(firstLine.end) -
            mapPoint(firstLine.start);
        secondDirection =
            mapPoint(secondLine.start) -
            mapPoint(secondLine.end);
      } else if (sameScreenVertex(firstLine.end,
                                  secondLine.start)) {
        firstDirection =
            mapPoint(firstLine.start) -
            mapPoint(firstLine.end);
        secondDirection =
            mapPoint(secondLine.end) -
            mapPoint(secondLine.start);
      } else if (sameScreenVertex(firstLine.end,
                                  secondLine.end)) {
        firstDirection =
            mapPoint(firstLine.start) -
            mapPoint(firstLine.end);
        secondDirection =
            mapPoint(secondLine.start) -
            mapPoint(secondLine.end);
      }

      const double firstLength =
          std::hypot(firstDirection.x(),
                     firstDirection.y());
      const double secondLength =
          std::hypot(secondDirection.x(),
                     secondDirection.y());

      if (firstLength <= 1.0 ||
          secondLength <= 1.0)
        continue;

      firstDirection /= firstLength;
      secondDirection /= secondLength;

      double startDeg =
          -std::atan2(firstDirection.y(),
                      firstDirection.x()) *
          180.0 / 3.14159265358979323846;

      double endDeg =
          -std::atan2(secondDirection.y(),
                      secondDirection.x()) *
          180.0 / 3.14159265358979323846;

      double spanDeg = endDeg - startDeg;

      while (spanDeg <= -180.0)
        spanDeg += 360.0;
      while (spanDeg > 180.0)
        spanDeg -= 360.0;

      const double radius =
          std::max(
              16.0,
              std::abs(dimension.offsetMm) *
                  pixelsPerMm_);

      const double midRad =
          (startDeg + spanDeg * 0.5) *
          3.14159265358979323846 /
          180.0;

      QPointF labelCenter =
          *center +
          QPointF(std::cos(midRad),
                  -std::sin(midRad)) *
              (radius + 18.0);

      const QVariantList labelX =
          property("dimensionLabelAlongMm").toList();
      const QVariantList labelY =
          property("dimensionLabelOffsetMm").toList();

      if (index <
          static_cast<std::size_t>(labelX.size()))
        labelCenter.rx() +=
            labelX[static_cast<int>(index)].toDouble() *
            pixelsPerMm_;

      if (index <
          static_cast<std::size_t>(labelY.size()))
        labelCenter.ry() +=
            labelY[static_cast<int>(index)].toDouble() *
            pixelsPerMm_;

      // Painted textRect is 84x20. Use a slightly more forgiving
      // 96x28 interaction box.
      if (std::abs(position.x() -
                   labelCenter.x()) <= 48.0 &&
          std::abs(position.y() -
                   labelCenter.y()) <= 14.0) {
        setProperty(
            "selectedDimension",
            static_cast<qulonglong>(index));
        setProperty(
            "draggingDimensionLabel",
            static_cast<qulonglong>(index));
        setCursor(Qt::ClosedHandCursor);
        update();
        return true;
      }

      continue;
    }

    QPointF first;
    QPointF second;

    if (!dimensionSegment(index, first, second))
      continue;

    const QPointF center =
        dimensionLabelCenter(index, first, second);

    const QPointF direction = second - first;
    double angle =
        std::atan2(direction.y(),
                   direction.x());

    if (angle >
            3.141592653589793 * 0.5 ||
        angle <
            -3.141592653589793 * 0.5)
      angle += 3.141592653589793;

    const QPointF delta =
        position - center;

    const double localX =
        std::cos(angle) * delta.x() +
        std::sin(angle) * delta.y();

    const double localY =
        -std::sin(angle) * delta.x() +
        std::cos(angle) * delta.y();

    if (std::abs(localX) <= 46.0 &&
        std::abs(localY) <= 13.0) {
      setProperty(
          "draggingDimensionLabel",
          static_cast<qulonglong>(index));
      setProperty(
          "selectedDimension",
          static_cast<qulonglong>(index));
      setCursor(Qt::ClosedHandCursor);
      update();
      return true;
    }
  }

  return false;
}

bool SketchCanvas::beginDimensionLineDrag(QPointF position) {
  if (const auto found = dimensionAt(position)) {
    if (*found < sketch_.dimensions().size() &&
        sketch_.dimensions()[*found].kind ==
            sketch::DimensionKind::LineAngle) {
      setProperty("selectedDimension",
                  static_cast<qulonglong>(*found));
      setProperty("draggingDimensionLine",
                  static_cast<qulonglong>(*found));
      setCursor(Qt::ClosedHandCursor);
      update();
      return true;
    }
  }

  constexpr double hitTolerance = 8.0;
  double bestDistance = hitTolerance;
  std::optional<std::size_t> bestIndex;
  for (std::size_t index = 0; index < sketch_.dimensions().size(); ++index) {
    QPointF first;
    QPointF second;
    if (!dimensionSegment(index, first, second)) continue;
    const double distance = pointSegmentDistance(position, first, second);
    if (distance < bestDistance) {
      bestDistance = distance;
      bestIndex = index;
    }
  }
  if (!bestIndex) return false;
  setProperty("selectedDimension", static_cast<qulonglong>(*bestIndex));
  setProperty("draggingDimensionLine", static_cast<qulonglong>(*bestIndex));
  setCursor(Qt::ClosedHandCursor);
  update();
  return true;
}

std::optional<std::size_t> SketchCanvas::dimensionAt(
    QPointF position) const {
  constexpr double hitTolerance = 8.0;

  for (std::size_t reverse = sketch_.dimensions().size();
       reverse > 0; --reverse) {
    const std::size_t index = reverse - 1;
    const auto& dimension = sketch_.dimensions()[index];

    if (dimension.kind == sketch::DimensionKind::LineAngle) {
      const auto firstIndex =
          sketch_.lineIndex(dimension.geometryId);
      const auto secondIndex =
          sketch_.lineIndex(dimension.secondPoint.lineId);

      if (!firstIndex || !secondIndex)
        continue;

      const auto& firstLine = sketch_.lines()[*firstIndex];
      const auto& secondLine = sketch_.lines()[*secondIndex];

      const auto center = lineIntersectionScreen(
          firstLine, secondLine,
          [this](sketch::Point point) {
            return mapPoint(point);
          });

      if (!center)
        continue;

      QPointF firstDirection =
          angleRayTowardSegment(
              *center,
              mapPoint(firstLine.start),
              mapPoint(firstLine.end));

      QPointF secondDirection =
          angleRayTowardSegment(
              *center,
              mapPoint(secondLine.start),
              mapPoint(secondLine.end));

      const auto sameScreenVertex =
          [this](sketch::Point a, sketch::Point b) {
            return QLineF(mapPoint(a),
                          mapPoint(b)).length() <= 0.5;
          };

      if (sameScreenVertex(firstLine.start,
                           secondLine.start)) {
        firstDirection =
            mapPoint(firstLine.end) -
            mapPoint(firstLine.start);
        secondDirection =
            mapPoint(secondLine.end) -
            mapPoint(secondLine.start);
      } else if (sameScreenVertex(firstLine.start,
                                  secondLine.end)) {
        firstDirection =
            mapPoint(firstLine.end) -
            mapPoint(firstLine.start);
        secondDirection =
            mapPoint(secondLine.start) -
            mapPoint(secondLine.end);
      } else if (sameScreenVertex(firstLine.end,
                                  secondLine.start)) {
        firstDirection =
            mapPoint(firstLine.start) -
            mapPoint(firstLine.end);
        secondDirection =
            mapPoint(secondLine.end) -
            mapPoint(secondLine.start);
      } else if (sameScreenVertex(firstLine.end,
                                  secondLine.end)) {
        firstDirection =
            mapPoint(firstLine.start) -
            mapPoint(firstLine.end);
        secondDirection =
            mapPoint(secondLine.start) -
            mapPoint(secondLine.end);
      }

      const double firstLength =
          std::hypot(firstDirection.x(),
                     firstDirection.y());
      const double secondLength =
          std::hypot(secondDirection.x(),
                     secondDirection.y());

      if (firstLength <= 1.0 ||
          secondLength <= 1.0)
        continue;

      firstDirection /= firstLength;
      secondDirection /= secondLength;

      double startDeg =
          -std::atan2(firstDirection.y(),
                      firstDirection.x()) *
          180.0 / 3.14159265358979323846;

      double endDeg =
          -std::atan2(secondDirection.y(),
                      secondDirection.x()) *
          180.0 / 3.14159265358979323846;

      double spanDeg = endDeg - startDeg;

      while (spanDeg <= -180.0)
        spanDeg += 360.0;
      while (spanDeg > 180.0)
        spanDeg -= 360.0;

      const double radius =
          std::max(16.0,
                   std::abs(dimension.offsetMm) *
                       pixelsPerMm_);

      const QPointF arcFirst =
          *center + firstDirection * radius;
      const QPointF arcSecond =
          *center + secondDirection * radius;

      if (pointSegmentDistance(
              position, *center, arcFirst) <= hitTolerance ||
          pointSegmentDistance(
              position, *center, arcSecond) <= hitTolerance)
        return index;

      const QPointF cursorDelta = position - *center;
      const double cursorRadius =
          std::hypot(cursorDelta.x(),
                     cursorDelta.y());

      double cursorDeg =
          -std::atan2(cursorDelta.y(),
                      cursorDelta.x()) *
          180.0 / 3.14159265358979323846;

      double relativeDeg = cursorDeg - startDeg;

      while (relativeDeg <= -180.0)
        relativeDeg += 360.0;
      while (relativeDeg > 180.0)
        relativeDeg -= 360.0;

      const bool insideSweep =
          spanDeg >= 0.0
              ? (relativeDeg >= -5.0 &&
                 relativeDeg <= spanDeg + 5.0)
              : (relativeDeg <= 5.0 &&
                 relativeDeg >= spanDeg - 5.0);

      if (insideSweep &&
          std::abs(cursorRadius - radius) <= hitTolerance)
        return index;

      const double midRad =
          (startDeg + spanDeg * 0.5) *
          3.14159265358979323846 /
          180.0;

      QPointF labelCenter =
          *center +
          QPointF(std::cos(midRad),
                  -std::sin(midRad)) *
              (radius + 18.0);

      const QVariantList labelX =
          property("dimensionLabelAlongMm").toList();
      const QVariantList labelY =
          property("dimensionLabelOffsetMm").toList();

      if (index <
          static_cast<std::size_t>(labelX.size()))
        labelCenter.rx() +=
            labelX[static_cast<int>(index)].toDouble() *
            pixelsPerMm_;

      if (index <
          static_cast<std::size_t>(labelY.size()))
        labelCenter.ry() +=
            labelY[static_cast<int>(index)].toDouble() *
            pixelsPerMm_;

      if (std::abs(position.x() - labelCenter.x()) <= 48.0 &&
          std::abs(position.y() - labelCenter.y()) <= 14.0)
        return index;

      continue;
    }

    QPointF first;
    QPointF second;

    if (!dimensionSegment(index, first, second))
      continue;

    const QPointF center =
        dimensionLabelCenter(index, first, second);
    const QPointF direction = second - first;

    double angle =
        std::atan2(direction.y(), direction.x());

    if (angle > 3.141592653589793 * 0.5 ||
        angle < -3.141592653589793 * 0.5)
      angle += 3.141592653589793;

    const QPointF delta = position - center;

    const double localX =
        std::cos(angle) * delta.x() +
        std::sin(angle) * delta.y();

    const double localY =
        -std::sin(angle) * delta.x() +
        std::cos(angle) * delta.y();

    if ((std::abs(localX) <= 46.0 &&
         std::abs(localY) <= 13.0) ||
        pointSegmentDistance(
            position, first, second) <= hitTolerance)
      return index;
  }

  return std::nullopt;
}
void SketchCanvas::handleCoincidentConstraintClick(QPointF position) {
  // POINT-ON-CIRCLE PRIORITY ROUTER V3
  //
  // Keep the requested workflow deliberately narrow and deterministic:
  //   line endpoint -> circle body
  //   circle body   -> line endpoint
  //
  // If neither side of this pair is involved, fall through untouched to the
  // existing Coincident / PointOnLine logic below.
  constexpr double pocEndpointTolerance = 11.0;
  constexpr double pocCircleBodyTolerance = 11.0;
  constexpr double pocCircleCenterExclusion = 12.0;

  const auto findLineEndpoint =
      [this, position]() -> std::optional<sketch::PointReference> {
        double bestDistance = pocEndpointTolerance;
        std::optional<sketch::PointReference> result;

        for (std::size_t index = 0;
             index < sketch_.lines().size(); ++index) {
          const auto id = sketch_.lineId(index);
          if (id == sketch::kInvalidGeometryId)
            continue;

          const auto& line = sketch_.lines()[index];

          for (const bool start : {true, false}) {
            const sketch::Point point =
                start ? line.start : line.end;

            const double distance =
                QLineF(position, mapPoint(point)).length();

            if (distance < bestDistance) {
              bestDistance = distance;
              result = sketch::PointReference{id, start};
            }
          }
        }

        return result;
      };

  const auto findCircleBody =
      [this, position]() -> sketch::GeometryId {
        double bestDistance = pocCircleBodyTolerance;
        sketch::GeometryId result =
            sketch::kInvalidGeometryId;

        for (std::size_t index = 0;
             index < sketch_.circles().size(); ++index) {
          const auto id = sketch_.circleId(index);
          if (id == sketch::kInvalidGeometryId)
            continue;

          const auto& circle = sketch_.circles()[index];
          const QPointF center = mapPoint(circle.center);

          const double centerDistance =
              QLineF(position, center).length();

          // Clicking near the centre must remain ordinary Coincident.
          if (centerDistance < pocCircleCenterExclusion)
            continue;

          const double radiusPx =
              circle.radiusMm * pixelsPerMm_;

          const double bodyDistance =
              std::abs(centerDistance - radiusPx);

          if (bodyDistance < bestDistance) {
            bestDistance = bodyDistance;
            result = id;
          }
        }

        return result;
      };

  const auto createPointOnCircle =
      [this](sketch::GeometryId circleId,
             sketch::PointReference endpoint) {
        if (circleId == sketch::kInvalidGeometryId ||
            endpoint.lineId == sketch::kInvalidGeometryId)
          return false;

        // Do not create the same relation twice.
        for (const auto& existing : sketch_.constraints()) {
          if (existing.type !=
              sketch::ConstraintType::PointOnCircle)
            continue;

          if (existing.firstGeometry == circleId &&
              existing.secondPoint.lineId == endpoint.lineId &&
              existing.secondPoint.start == endpoint.start &&
              existing.secondPoint.circleId ==
                  sketch::kInvalidGeometryId &&
              existing.secondPoint.elementCenterId == 0)
            return true;
        }

        pushUndoState();

        sketch::Constraint constraint;
        constraint.type =
            sketch::ConstraintType::PointOnCircle;
        constraint.firstGeometry = circleId;
        constraint.secondPoint = endpoint;
        sketch_.addConstraint(constraint);

        notifyGeometryChanged();
        return true;
      };

  // Circle was chosen first: now accept only a real line endpoint.
  if (property("pointOnCircleCarrier").isValid()) {
    const auto carrier =
        static_cast<sketch::GeometryId>(
            property("pointOnCircleCarrier").toULongLong());

    const auto endpoint = findLineEndpoint();

    if (endpoint) {
      (void)createPointOnCircle(carrier, *endpoint);

      setProperty("pointOnCircleCarrier", QVariant());
      coincidentFirstPoint_.reset();

      emit selectionChanged(QString::fromUtf8(
          "Ограничение: конец линии на окружности"));
      update();
      return;
    }

    // Clicking elsewhere cancels only this special pending pair and then lets
    // the normal Coincident tool handle the same click.
    setProperty("pointOnCircleCarrier", QVariant());
  }

  const auto endpointHit = findLineEndpoint();
  const auto circleHit = findCircleBody();

  // Endpoint was chosen previously by the ordinary Coincident first-point
  // picker. Give a circle-body second click priority over PointOnLine.
  if (coincidentFirstPoint_ &&
      coincidentFirstPoint_->lineId !=
          sketch::kInvalidGeometryId &&
      coincidentFirstPoint_->circleId ==
          sketch::kInvalidGeometryId &&
      coincidentFirstPoint_->elementCenterId == 0 &&
      circleHit != sketch::kInvalidGeometryId) {
    const auto endpoint = *coincidentFirstPoint_;

    (void)createPointOnCircle(circleHit, endpoint);

    coincidentFirstPoint_.reset();
    setProperty("pointOnCircleCarrier", QVariant());

    emit selectionChanged(QString::fromUtf8(
        "Ограничение: конец линии на окружности"));
    update();
    return;
  }

  // Circle body first. Store only the carrier and wait for an endpoint.
  //
  // Endpoint hit has priority so a point that already lies on/near the
  // circumference is still treated as a point rather than as circle body.
  if (!coincidentFirstPoint_ &&
      !endpointHit &&
      circleHit != sketch::kInvalidGeometryId) {
    setProperty(
        "pointOnCircleCarrier",
        static_cast<qulonglong>(circleHit));

    emit selectionChanged(QString::fromUtf8(
        "Окружность выбрана: укажите конец линии"));
    update();
    return;
  }

  // MERGED POINT-ON-LINE MODE
  //
  // The existing Coincident tool now handles two related workflows:
  //   point -> point       = Coincident
  //   line body -> point   = PointOnLine
  //
  // Clicking near a line endpoint deliberately falls through to the original
  // point-selection logic below.
  constexpr double lineTolerance = 9.0;
  constexpr double endpointExclusion = 11.0;
  constexpr double pointTolerance = 10.0;

  // POINT-ON-CIRCLE: ACTIVE CARRIER
  const QVariant circleCarrierProperty =
      property("pointOnCircleCarrier");

  if (circleCarrierProperty.isValid()) {
    const auto carrierCircleId =
        static_cast<sketch::GeometryId>(
            circleCarrierProperty.toULongLong());

    if (!sketch_.circleIndex(carrierCircleId)) {
      setProperty("pointOnCircleCarrier", QVariant());
      return;
    }

    double bestPointDistance = pointTolerance;
    std::optional<sketch::PointReference> clickedPoint;

    // Line endpoints.
    for (std::size_t index = 0;
         index < sketch_.lines().size(); ++index) {
      const auto id = sketch_.lineId(index);
      if (id == sketch::kInvalidGeometryId) continue;

      const auto& line = sketch_.lines()[index];

      for (const bool start : {true, false}) {
        const sketch::Point candidate =
            start ? line.start : line.end;

        const double distance =
            QLineF(position, mapPoint(candidate)).length();

        if (distance < bestPointDistance) {
          bestPointDistance = distance;
          clickedPoint =
              sketch::PointReference{id, start};
        }
      }
    }

    // Circle centres, except the carrier's own centre.
    for (std::size_t index = 0;
         index < sketch_.circles().size(); ++index) {
      const auto id = sketch_.circleId(index);

      if (id == sketch::kInvalidGeometryId ||
          id == carrierCircleId)
        continue;

      const double distance =
          QLineF(position,
                 mapPoint(sketch_.circles()[index].center))
              .length();

      if (distance < bestPointDistance) {
        bestPointDistance = distance;

        sketch::PointReference center;
        center.circleId = id;
        clickedPoint = center;
      }
    }

    // Virtual rectangle centres.
    for (const auto elementId :
         sketch_.centerNodeElementIds()) {
      const auto centerPoint =
          sketch_.elementCenterPoint(elementId);

      if (!centerPoint) continue;

      const double distance =
          QLineF(position, mapPoint(*centerPoint)).length();

      if (distance < bestPointDistance) {
        bestPointDistance = distance;

        sketch::PointReference center;
        center.elementCenterId = elementId;
        clickedPoint = center;
      }
    }

    if (!clickedPoint) {
      emit selectionChanged(QString::fromUtf8(
          "Принадлежность: выберите конечную точку"));
      return;
    }

    const auto sameReference =
        [](sketch::PointReference first,
           sketch::PointReference second) {
          if (first.elementCenterId != 0 ||
              second.elementCenterId != 0)
            return first.elementCenterId != 0 &&
                   first.elementCenterId ==
                       second.elementCenterId;

          if (first.circleId !=
                  sketch::kInvalidGeometryId ||
              second.circleId !=
                  sketch::kInvalidGeometryId)
            return first.circleId !=
                       sketch::kInvalidGeometryId &&
                   first.circleId == second.circleId;

          return first.lineId == second.lineId &&
                 first.start == second.start;
        };

    for (const auto& constraint :
         sketch_.constraints()) {
      if (constraint.type !=
          sketch::ConstraintType::PointOnCircle)
        continue;

      if (constraint.firstGeometry == carrierCircleId &&
          sameReference(constraint.secondPoint,
                        *clickedPoint)) {
        setProperty("pointOnCircleCarrier", QVariant());
        emit selectionChanged(QString::fromUtf8(
            "Эта точка уже принадлежит выбранной окружности"));
        update();
        return;
      }
    }

    pushUndoState();

    sketch::Constraint constraint;
    constraint.type =
        sketch::ConstraintType::PointOnCircle;
    constraint.firstGeometry = carrierCircleId;
    constraint.secondPoint = *clickedPoint;
    sketch_.addConstraint(constraint);

    setProperty("pointOnCircleCarrier", QVariant());
    coincidentFirstPoint_.reset();

    emit selectionChanged(QString::fromUtf8(
        "Ограничение: точка на окружности"));
    notifyGeometryChanged();
    update();
    return;
  }
  const QVariant carrierProperty = property("pointOnLineCarrier");

  if (carrierProperty.isValid()) {
    const auto carrierId = static_cast<sketch::GeometryId>(
        carrierProperty.toULongLong());

    if (!sketch_.lineIndex(carrierId)) {
      setProperty("pointOnLineCarrier", QVariant());
      return;
    }

    double bestPointDistance = pointTolerance;
    std::optional<sketch::PointReference> clickedPoint;

    // Line endpoints.
    for (std::size_t index = 0;
         index < sketch_.lines().size();
         ++index) {
      const auto id = sketch_.lineId(index);
      if (id == sketch::kInvalidGeometryId)
        continue;

      const auto& line = sketch_.lines()[index];

      for (const bool start : {true, false}) {
        const sketch::Point point =
            start ? line.start : line.end;

        const double distance =
            QLineF(position, mapPoint(point)).length();

        if (distance < bestPointDistance) {
          bestPointDistance = distance;
          clickedPoint = sketch::PointReference{id, start};
        }
      }
    }

    // Circle centers.
    for (std::size_t index = 0;
         index < sketch_.circles().size();
         ++index) {
      const auto id = sketch_.circleId(index);
      if (id == sketch::kInvalidGeometryId)
        continue;

      const double distance =
          QLineF(position,
                 mapPoint(sketch_.circles()[index].center)).length();

      if (distance < bestPointDistance) {
        bestPointDistance = distance;

        sketch::PointReference center;
        center.circleId = id;
        clickedPoint = center;
      }
    }


    // Rectangle center nodes.
    for (const auto elementId : sketch_.centerNodeElementIds()) {
      const auto centerPoint =
          sketch_.elementCenterPoint(elementId);
      if (!centerPoint) continue;

      const double distance =
          QLineF(position,
                 mapPoint(*centerPoint)).length();

      if (distance < bestPointDistance) {
        bestPointDistance = distance;

        sketch::PointReference center;
        center.elementCenterId = elementId;
        clickedPoint = center;
      }
    }
    if (!clickedPoint) {
      emit selectionChanged(QString::fromUtf8(
          "Принадлежность: выберите конечную точку или центр окружности"));
      return;
    }

    if (clickedPoint->elementCenterId == 0 &&
        clickedPoint->circleId == sketch::kInvalidGeometryId &&
        clickedPoint->lineId == carrierId) {
      emit selectionChanged(QString::fromUtf8(
          "Принадлежность: выберите точку другого объекта"));
      return;
    }

    const auto sameReference =
        [](sketch::PointReference first,
           sketch::PointReference second) {
          if (first.elementCenterId != 0 ||
              second.elementCenterId != 0) {
            return first.elementCenterId != 0 &&
                   first.elementCenterId == second.elementCenterId;
          }

          if (first.circleId != sketch::kInvalidGeometryId ||
              second.circleId != sketch::kInvalidGeometryId) {
            return first.circleId != sketch::kInvalidGeometryId &&
                   first.circleId == second.circleId;
          }

          return first.lineId == second.lineId &&
                 first.start == second.start;
        };

    for (const auto& constraint : sketch_.constraints()) {
      if (constraint.type != sketch::ConstraintType::PointOnLine)
        continue;

      if (constraint.firstGeometry == carrierId &&
          sameReference(constraint.secondPoint, *clickedPoint)) {
        setProperty("pointOnLineCarrier", QVariant());

        emit selectionChanged(QString::fromUtf8(
            "Эта точка уже принадлежит выбранной линии"));
        update();
        return;
      }
    }

    pushUndoState();

    sketch::Constraint constraint;
    constraint.type = sketch::ConstraintType::PointOnLine;
    constraint.firstGeometry = carrierId;
    constraint.secondPoint = *clickedPoint;
    sketch_.addConstraint(constraint);

    setProperty("pointOnLineCarrier", QVariant());

    emit selectionChanged(
        QString::fromUtf8("Ограничение: Принадлежность"));
    notifyGeometryChanged();
    update();
    return;
  }

  // POINT-FIRST -> LINE-BODY POINT-ON-LINE
  //
  // If a point was selected first, allow the second click to be the body of
  // a line. This makes the merged tool symmetrical:
  //   line -> point  and  point -> line.
  if (coincidentFirstPoint_) {
    double bestCarrierDistance = lineTolerance;
    std::optional<std::size_t> bestCarrierIndex;

    for (std::size_t index = 0;
         index < sketch_.lines().size();
         ++index) {
      const auto& line = sketch_.lines()[index];
      const QPointF start = mapPoint(line.start);
      const QPointF end = mapPoint(line.end);

      // A click near a vertex still means "point", not "line body".
      if (QLineF(position, start).length() < endpointExclusion ||
          QLineF(position, end).length() < endpointExclusion)
        continue;

      const double distance =
          pointSegmentDistance(position, start, end);

      if (distance < bestCarrierDistance) {
        bestCarrierDistance = distance;
        bestCarrierIndex = index;
      }
    }

    if (bestCarrierIndex) {
      const auto carrierId = sketch_.lineId(*bestCarrierIndex);
      const auto pointReference = *coincidentFirstPoint_;

      if (carrierId != sketch::kInvalidGeometryId &&
          !(pointReference.elementCenterId == 0 &&
            pointReference.circleId == sketch::kInvalidGeometryId &&
            pointReference.lineId == carrierId)) {
        const auto sameReference =
            [](sketch::PointReference first,
               sketch::PointReference second) {
              if (first.circleId != sketch::kInvalidGeometryId ||
                  second.circleId != sketch::kInvalidGeometryId) {
                return first.circleId != sketch::kInvalidGeometryId &&
                       first.circleId == second.circleId;
              }

              return first.lineId == second.lineId &&
                     first.start == second.start;
            };

        bool duplicate = false;

        for (const auto& constraint : sketch_.constraints()) {
          if (constraint.type != sketch::ConstraintType::PointOnLine)
            continue;

          if (constraint.firstGeometry == carrierId &&
              sameReference(constraint.secondPoint, pointReference)) {
            duplicate = true;
            break;
          }
        }

        if (!duplicate) {
          pushUndoState();

          sketch::Constraint constraint;
          constraint.type = sketch::ConstraintType::PointOnLine;
          constraint.firstGeometry = carrierId;
          constraint.secondPoint = pointReference;
          sketch_.addConstraint(constraint);

          emit selectionChanged(
              QString::fromUtf8("Ограничение: Принадлежность"));
          notifyGeometryChanged();
        } else {
          emit selectionChanged(QString::fromUtf8(
              "Эта точка уже принадлежит выбранной линии"));
        }

        coincidentFirstPoint_.reset();
        setProperty("pointOnLineCarrier", QVariant());
        update();
        return;
      }
    }
  }
  // POINT-ON-CIRCLE: POINT FIRST
  if (coincidentFirstPoint_) {
    constexpr double circleBodyTolerance = 9.0;
    constexpr double circleCenterExclusion = 11.0;

    sketch::GeometryId carrierCircleId =
        sketch::kInvalidGeometryId;
    double bestBodyDistance = circleBodyTolerance;

    for (std::size_t index = 0;
         index < sketch_.circles().size(); ++index) {
      const auto id = sketch_.circleId(index);
      if (id == sketch::kInvalidGeometryId) continue;

      const auto& circle = sketch_.circles()[index];
      const QPointF center = mapPoint(circle.center);

      const double centerDistance =
          QLineF(position, center).length();

      // Keep clicking the centre as normal Coincident point selection.
      if (centerDistance < circleCenterExclusion)
        continue;

      const double radiusPx =
          circle.radiusMm * pixelsPerMm_;

      const double bodyDistance =
          std::abs(centerDistance - radiusPx);

      if (bodyDistance < bestBodyDistance) {
        bestBodyDistance = bodyDistance;
        carrierCircleId = id;
      }
    }

    if (carrierCircleId !=
        sketch::kInvalidGeometryId) {
      const auto pointReference =
          *coincidentFirstPoint_;

      if (pointReference.circleId == carrierCircleId) {
        emit selectionChanged(QString::fromUtf8(
            "Центр окружности нельзя связать с её собственной окружностью"));
        return;
      }

      bool duplicate = false;

      for (const auto& constraint :
           sketch_.constraints()) {
        if (constraint.type !=
            sketch::ConstraintType::PointOnCircle)
          continue;

        if (constraint.firstGeometry != carrierCircleId)
          continue;

        const auto& existing = constraint.secondPoint;

        if (existing.elementCenterId != 0 ||
            pointReference.elementCenterId != 0) {
          duplicate =
              existing.elementCenterId != 0 &&
              existing.elementCenterId ==
                  pointReference.elementCenterId;
        } else if (
            existing.circleId != sketch::kInvalidGeometryId ||
            pointReference.circleId !=
                sketch::kInvalidGeometryId) {
          duplicate =
              existing.circleId != sketch::kInvalidGeometryId &&
              existing.circleId == pointReference.circleId;
        } else {
          duplicate =
              existing.lineId == pointReference.lineId &&
              existing.start == pointReference.start;
        }

        if (duplicate) break;
      }

      if (!duplicate) {
        pushUndoState();

        sketch::Constraint constraint;
        constraint.type =
            sketch::ConstraintType::PointOnCircle;
        constraint.firstGeometry = carrierCircleId;
        constraint.secondPoint = pointReference;
        sketch_.addConstraint(constraint);

        notifyGeometryChanged();
      }

      coincidentFirstPoint_.reset();
      setProperty("pointOnCircleCarrier", QVariant());

      emit selectionChanged(QString::fromUtf8(
          "Ограничение: точка на окружности"));
      update();
      return;
    }
  }
  // FIRST-CLICK POINT PRIORITY
  //
  // Endpoints and circle centres take precedence over a nearby segment body.
  // This prevents a point lying visually on/near another line from
  // accidentally starting line-first PointOnLine mode.
  if (!coincidentFirstPoint_ &&
      !property("pointOnLineCarrier").isValid()) {
    double firstPointDistance = pointTolerance;
    std::optional<sketch::PointReference> firstClickedPoint;

    for (std::size_t index = 0;
         index < sketch_.lines().size();
         ++index) {
      const auto id = sketch_.lineId(index);
      if (id == sketch::kInvalidGeometryId)
        continue;

      const auto& line = sketch_.lines()[index];

      for (const bool start : {true, false}) {
        const sketch::Point point =
            start ? line.start : line.end;

        const double distance =
            QLineF(position, mapPoint(point)).length();

        if (distance < firstPointDistance) {
          firstPointDistance = distance;
          firstClickedPoint =
              sketch::PointReference{id, start};
        }
      }
    }

    for (std::size_t index = 0;
         index < sketch_.circles().size();
         ++index) {
      const auto id = sketch_.circleId(index);
      if (id == sketch::kInvalidGeometryId)
        continue;

      const double distance =
          QLineF(position,
                 mapPoint(sketch_.circles()[index].center)).length();

      if (distance < firstPointDistance) {
        firstPointDistance = distance;

        sketch::PointReference center;
        center.circleId = id;
        firstClickedPoint = center;
      }
    }

    // CENTER NODE FIRST-CLICK
    for (const auto elementId : sketch_.centerNodeElementIds()) {
      const auto centerPoint =
          sketch_.elementCenterPoint(elementId);
      if (!centerPoint) continue;

      const double distance =
          QLineF(position,
                 mapPoint(*centerPoint)).length();

      if (distance < firstPointDistance) {
        firstPointDistance = distance;

        sketch::PointReference center;
        center.elementCenterId = elementId;
        firstClickedPoint = center;
      }
    }
    if (firstClickedPoint) {
      coincidentFirstPoint_ = *firstClickedPoint;

      emit selectionChanged(QString::fromUtf8(
          "Совпадение / Принадлежность: выберите вторую точку или тело линии"));
      update();
      return;
    }
  }
  // No first point has been selected for ordinary Coincident yet:
  // a click on the interior of a segment starts PointOnLine mode.
  // POINT-ON-CIRCLE: CIRCLE FIRST
  if (!coincidentFirstPoint_) {
    constexpr double circleBodyTolerance = 9.0;
    constexpr double circleCenterExclusion = 11.0;

    sketch::GeometryId carrierCircleId =
        sketch::kInvalidGeometryId;
    double bestBodyDistance = circleBodyTolerance;

    for (std::size_t index = 0;
         index < sketch_.circles().size(); ++index) {
      const auto id = sketch_.circleId(index);
      if (id == sketch::kInvalidGeometryId) continue;

      const auto& circle = sketch_.circles()[index];
      const QPointF center = mapPoint(circle.center);

      const double centerDistance =
          QLineF(position, center).length();

      if (centerDistance < circleCenterExclusion)
        continue;

      const double radiusPx =
          circle.radiusMm * pixelsPerMm_;
      const double bodyDistance =
          std::abs(centerDistance - radiusPx);

      if (bodyDistance < bestBodyDistance) {
        bestBodyDistance = bodyDistance;
        carrierCircleId = id;
      }
    }

    if (carrierCircleId !=
        sketch::kInvalidGeometryId) {
      setProperty(
          "pointOnCircleCarrier",
          static_cast<qulonglong>(carrierCircleId));

      emit selectionChanged(QString::fromUtf8(
          "Принадлежность: выберите конечную точку"));
      update();
      return;
    }
  }
  if (!coincidentFirstPoint_) {
    double bestDistance = lineTolerance;
    std::optional<std::size_t> bestLineIndex;

    for (std::size_t index = 0;
         index < sketch_.lines().size();
         ++index) {
      const auto& line = sketch_.lines()[index];
      const QPointF start = mapPoint(line.start);
      const QPointF end = mapPoint(line.end);

      if (QLineF(position, start).length() < endpointExclusion ||
          QLineF(position, end).length() < endpointExclusion)
        continue;

      const double distance =
          pointSegmentDistance(position, start, end);

      if (distance < bestDistance) {
        bestDistance = distance;
        bestLineIndex = index;
      }
    }

    if (bestLineIndex) {
      const auto carrierId = sketch_.lineId(*bestLineIndex);

      if (carrierId != sketch::kInvalidGeometryId) {
        setProperty("pointOnLineCarrier",
                    static_cast<qulonglong>(carrierId));

        emit selectionChanged(QString::fromUtf8(
            "Принадлежность: выберите точку или центр окружности"));
        update();
        return;
      }
    }
  }

  constexpr double hitTolerance = 10.0;
  double bestDistance = hitTolerance;
  std::optional<sketch::PointReference> clickedPoint;

  // Line endpoints.
  for (std::size_t index = 0; index < sketch_.lines().size(); ++index) {
    const auto& line = sketch_.lines()[index];
    const auto id = sketch_.lineId(index);
    if (id == sketch::kInvalidGeometryId) continue;

    for (const bool start : {true, false}) {
      const auto point = start ? line.start : line.end;
      const double distance =
          QLineF(position, mapPoint(point)).length();

      if (distance < bestDistance) {
        bestDistance = distance;
        clickedPoint = sketch::PointReference{id, start};
      }
    }
  }

  // Circle centers participate in the same point-reference system.
  for (std::size_t index = 0; index < sketch_.circles().size(); ++index) {
    const auto id = sketch_.circleId(index);
    if (id == sketch::kInvalidGeometryId) continue;

    const double distance =
        QLineF(position, mapPoint(sketch_.circles()[index].center)).length();

    if (distance < bestDistance) {
      bestDistance = distance;
      sketch::PointReference reference;
      reference.circleId = id;
      clickedPoint = reference;
    }
  }


  // CENTER NODE ORDINARY PICK
  for (const auto elementId : sketch_.centerNodeElementIds()) {
    const auto centerPoint =
        sketch_.elementCenterPoint(elementId);
    if (!centerPoint) continue;

    const double distance =
        QLineF(position,
               mapPoint(*centerPoint)).length();

    if (distance < bestDistance) {
      bestDistance = distance;

      sketch::PointReference center;
      center.elementCenterId = elementId;
      clickedPoint = center;
    }
  }
  if (!clickedPoint) {
    emit selectionChanged(QString::fromUtf8(
        "Совпадение: выберите конец линии или центр окружности"));
    return;
  }

  const auto sameReference =
      [](sketch::PointReference first,
         sketch::PointReference second) {
    if (first.elementCenterId != 0 ||
        second.elementCenterId != 0) {
      return first.elementCenterId != 0 &&
             first.elementCenterId == second.elementCenterId;
    }

    if (first.circleId != sketch::kInvalidGeometryId ||
        second.circleId != sketch::kInvalidGeometryId) {
      return first.circleId != sketch::kInvalidGeometryId &&
             first.circleId == second.circleId;
    }

    return first.lineId == second.lineId &&
           first.start == second.start;
  };

  if (!coincidentFirstPoint_) {
    coincidentFirstPoint_ = *clickedPoint;
    emit selectionChanged(QString::fromUtf8(
        "Совпадение: выберите вторую точку или центр"));
    update();
    return;
  }

  const auto first = *coincidentFirstPoint_;
  const auto second = *clickedPoint;

  if (sameReference(first, second)) {
    coincidentFirstPoint_.reset();
    emit selectionChanged(QString::fromUtf8(
        "Выбрана одна и та же точка"));
    update();
    return;
  }

  // Do not collapse a single line by coinciding its own two ends.
  if (first.elementCenterId == 0 &&
      second.elementCenterId == 0 &&
      first.circleId == sketch::kInvalidGeometryId &&
      second.circleId == sketch::kInvalidGeometryId &&
      first.lineId == second.lineId) {
    coincidentFirstPoint_.reset();
    emit selectionChanged(QString::fromUtf8(
        "Совпадение: выберите разные объекты"));
    update();
    return;
  }

  // Avoid duplicate Coincident constraints in either order.
  for (const auto& constraint : sketch_.constraints()) {
    if (constraint.type != sketch::ConstraintType::Coincident) continue;

    const bool sameOrder =
        sameReference(constraint.firstPoint, first) &&
        sameReference(constraint.secondPoint, second);
    const bool reverseOrder =
        sameReference(constraint.firstPoint, second) &&
        sameReference(constraint.secondPoint, first);

    if (sameOrder || reverseOrder) {
      coincidentFirstPoint_.reset();
      emit selectionChanged(QString::fromUtf8(
          "Эти точки уже имеют ограничение «Совпадение»"));
      update();
      return;
    }
  }

  pushUndoState();

  sketch::Constraint constraint;
  constraint.type = sketch::ConstraintType::Coincident;
  constraint.firstPoint = first;
  constraint.secondPoint = second;
  sketch_.addConstraint(constraint);

  coincidentFirstPoint_.reset();
  emit selectionChanged(QString::fromUtf8(
      "Ограничение: Совпадение"));
  notifyGeometryChanged();
  update();
}
void SketchCanvas::handleTangentConstraintClick(QPointF position) {
  constexpr double hitTolerance = 9.0;

  enum class HitKind {
    None = 0,
    Line = 1,
    Circle = 2
  };

  HitKind hitKind = HitKind::None;
  sketch::GeometryId hitId = sketch::kInvalidGeometryId;
  double bestDistance = hitTolerance;

  for (std::size_t index = 0;
       index < sketch_.lines().size();
       ++index) {
    const auto& line = sketch_.lines()[index];

    const double distance =
        pointSegmentDistance(
            position,
            mapPoint(line.start),
            mapPoint(line.end));

    if (distance >= bestDistance)
      continue;

    const auto id = sketch_.lineId(index);
    if (id == sketch::kInvalidGeometryId)
      continue;

    bestDistance = distance;
    hitKind = HitKind::Line;
    hitId = id;
  }

  for (std::size_t index = 0;
       index < sketch_.circles().size();
       ++index) {
    const auto& circle = sketch_.circles()[index];

    const double distance =
        std::abs(
            QLineF(
                position,
                mapPoint(circle.center)).length() -
            circle.radiusMm * pixelsPerMm_);

    if (distance >= bestDistance)
      continue;

    const auto id = sketch_.circleId(index);
    if (id == sketch::kInvalidGeometryId)
      continue;

    bestDistance = distance;
    hitKind = HitKind::Circle;
    hitId = id;
  }

  if (hitKind == HitKind::None ||
      hitId == sketch::kInvalidGeometryId)
    return;

  const QVariant firstGeometryProperty =
      property("tangentFirstGeometry");

  const QVariant firstKindProperty =
      property("tangentFirstKind");

  if (!firstGeometryProperty.isValid() ||
      !firstKindProperty.isValid()) {
    setProperty(
        "tangentFirstGeometry",
        static_cast<qulonglong>(hitId));

    setProperty(
        "tangentFirstKind",
        static_cast<int>(hitKind));

    if (hitKind == HitKind::Line) {
      emit selectionChanged(
          QString::fromUtf8(
              "\xD0\x9A\xD0\xB0\xD1\x81\xD0\xB0\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xBD\xD0\xB0\xD1\x8F: "
              "\xD0\xB2\xD1\x8B\xD0\xB1\xD0\xB5\xD1\x80\xD0\xB8\xD1\x82\xD0\xB5 "
              "\xD0\xBE\xD0\xBA\xD1\x80\xD1\x83\xD0\xB6\xD0\xBD\xD0\xBE\xD1\x81\xD1\x82\xD1\x8C"));
    }
    else {
      emit selectionChanged(
          QString::fromUtf8(
              "\xD0\x9A\xD0\xB0\xD1\x81\xD0\xB0\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xBD\xD0\xB0\xD1\x8F: "
              "\xD0\xB2\xD1\x8B\xD0\xB1\xD0\xB5\xD1\x80\xD0\xB8\xD1\x82\xD0\xB5 "
              "\xD0\xBF\xD1\x80\xD1\x8F\xD0\xBC\xD1\x83\xD1\x8E"));
    }

    update();
    return;
  }

  const auto firstId =
      static_cast<sketch::GeometryId>(
          firstGeometryProperty.toULongLong());

  const auto firstKind =
      static_cast<HitKind>(
          firstKindProperty.toInt());

  if (firstKind == hitKind) {
    emit selectionChanged(
        firstKind == HitKind::Line
            ? QString::fromUtf8(
                  "\xD0\x9A\xD0\xB0\xD1\x81\xD0\xB0\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xBD\xD0\xB0\xD1\x8F: "
                  "\xD0\xBD\xD1\x83\xD0\xB6\xD0\xBD\xD0\xBE "
                  "\xD0\xB2\xD1\x8B\xD0\xB1\xD1\x80\xD0\xB0\xD1\x82\xD1\x8C "
                  "\xD0\xBE\xD0\xBA\xD1\x80\xD1\x83\xD0\xB6\xD0\xBD\xD0\xBE\xD1\x81\xD1\x82\xD1\x8C")
            : QString::fromUtf8(
                  "\xD0\x9A\xD0\xB0\xD1\x81\xD0\xB0\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xBD\xD0\xB0\xD1\x8F: "
                  "\xD0\xBD\xD1\x83\xD0\xB6\xD0\xBD\xD0\xBE "
                  "\xD0\xB2\xD1\x8B\xD0\xB1\xD1\x80\xD0\xB0\xD1\x82\xD1\x8C "
                  "\xD0\xBF\xD1\x80\xD1\x8F\xD0\xBC\xD1\x83\xD1\x8E"));
    update();
    return;
  }

  sketch::GeometryId lineId =
      sketch::kInvalidGeometryId;

  sketch::GeometryId circleId =
      sketch::kInvalidGeometryId;

  if (firstKind == HitKind::Line &&
      hitKind == HitKind::Circle) {
    lineId = firstId;
    circleId = hitId;
  }
  else if (firstKind == HitKind::Circle &&
           hitKind == HitKind::Line) {
    lineId = hitId;
    circleId = firstId;
  }

  const auto resetState = [this]() {
    setProperty("tangentFirstGeometry", QVariant());
    setProperty("tangentFirstKind", QVariant());
  };

  if (lineId == sketch::kInvalidGeometryId ||
      circleId == sketch::kInvalidGeometryId ||
      !sketch_.lineIndex(lineId) ||
      !sketch_.circleIndex(circleId)) {
    resetState();
    update();
    return;
  }

  for (const auto& constraint :
       sketch_.constraints()) {
    if (constraint.type !=
        sketch::ConstraintType::Tangent)
      continue;

    if (constraint.firstGeometry == lineId &&
        constraint.secondGeometry == circleId) {
      resetState();

      emit selectionChanged(
          QString::fromUtf8(
              "\xD0\x9A\xD0\xB0\xD1\x81\xD0\xB0\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xBD\xD0\xB0\xD1\x8F "
              "\xD1\x83\xD0\xB6\xD0\xB5 "
              "\xD1\x83\xD1\x81\xD1\x82\xD0\xB0\xD0\xBD\xD0\xBE\xD0\xB2\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xB0"));

      update();
      return;
    }
  }

  pushUndoState();

  sketch::Constraint constraint;
  constraint.type =
      sketch::ConstraintType::Tangent;
  constraint.firstGeometry = lineId;
  constraint.secondGeometry = circleId;

  sketch_.addConstraint(constraint);

  resetState();

  emit selectionChanged(
      QString::fromUtf8(
          "\xD0\x9E\xD0\xB3\xD1\x80\xD0\xB0\xD0\xBD\xD0\xB8\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5: "
          "\xD0\x9A\xD0\xB0\xD1\x81\xD0\xB0\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xBD\xD0\xB0\xD1\x8F "
          "\xD0\xBA "
          "\xD0\xBE\xD0\xBA\xD1\x80\xD1\x83\xD0\xB6\xD0\xBD\xD0\xBE\xD1\x81\xD1\x82\xD0\xB8"));

  notifyGeometryChanged();
  update();
}
void SketchCanvas::handleEqualConstraintClick(QPointF position) {
  constexpr double hitTolerance = 9.0;

  enum class HitKind { None, Line, Rectangle, Circle };

  HitKind hitKind = HitKind::None;
  sketch::GeometryId hitId = sketch::kInvalidGeometryId;
  std::size_t hitElementId = 0;
  double bestDistance = hitTolerance;

  for (std::size_t index = 0; index < sketch_.lines().size(); ++index) {
    const auto& line = sketch_.lines()[index];
    const double distance = pointSegmentDistance(
        position, mapPoint(line.start), mapPoint(line.end));

    if (distance >= bestDistance) continue;

    const auto id = sketch_.lineId(index);
    if (id == sketch::kInvalidGeometryId) continue;

    std::size_t elementLineCount = 0;
    for (const auto& candidate : sketch_.lines()) {
      if (candidate.elementId == line.elementId)
        ++elementLineCount;
    }

    bestDistance = distance;
    hitKind = elementLineCount == 4
                  ? HitKind::Rectangle
                  : HitKind::Line;
    hitId = id;
    hitElementId = line.elementId;
  }

  for (std::size_t index = 0; index < sketch_.circles().size(); ++index) {
    const auto& circle = sketch_.circles()[index];
    const double distance = std::abs(
        QLineF(position, mapPoint(circle.center)).length() -
        circle.radiusMm * pixelsPerMm_);

    if (distance >= bestDistance) continue;

    const auto id = sketch_.circleId(index);
    if (id == sketch::kInvalidGeometryId) continue;

    bestDistance = distance;
    hitKind = HitKind::Circle;
    hitId = id;
    hitElementId = 0;
  }

  if (hitKind == HitKind::None ||
      hitId == sketch::kInvalidGeometryId)
    return;

  const auto kindName = [](HitKind kind) {
    switch (kind) {
      case HitKind::Line:
        return QStringLiteral("line");
      case HitKind::Rectangle:
        return QStringLiteral("rectangle");
      case HitKind::Circle:
        return QStringLiteral("circle");
      default:
        return QString();
    }
  };

  const QVariant firstGeometryProperty =
      property("equalFirstGeometry");
  const QVariant firstKindProperty =
      property("equalFirstKind");

  if (!firstGeometryProperty.isValid() ||
      !firstKindProperty.isValid()) {
    setProperty("equalFirstGeometry",
                static_cast<qulonglong>(hitId));
    setProperty("equalFirstKind", kindName(hitKind));
    setProperty("equalFirstElement",
                static_cast<qulonglong>(hitElementId));

    clearGeometrySelection();

    if (hitKind == HitKind::Line ||
        hitKind == HitKind::Rectangle) {
      const auto index = sketch_.lineIndex(hitId);
      if (index) {
        selectedElementIds_.push_back(
            sketch_.lines()[*index].elementId);
        selectionKind_ = SelectionKind::Line;
        selectionElementId_ =
            sketch_.lines()[*index].elementId;
        selectionLineId_ = hitId;
        selectionCircleId_ = sketch::kInvalidGeometryId;

        emit lineStyleSelectionChanged(
            true, sketch_.lines()[*index].dashed);
      }
    } else {
      selectedCircleIds_.push_back(hitId);
      selectionKind_ = SelectionKind::Circle;
      selectionCircleId_ = hitId;
      selectionLineId_ = sketch::kInvalidGeometryId;
      emit lineStyleSelectionChanged(false, false);
    }

    emit selectionChanged(
        QString::fromUtf8("Эквивалентность: выберите второй объект"));
    update();
    return;
  }

  const auto resetEqualState = [this]() {
    setProperty("equalFirstGeometry", QVariant());
    setProperty("equalFirstKind", QVariant());
    setProperty("equalFirstElement", QVariant());
  };

  const auto firstId = static_cast<sketch::GeometryId>(
      firstGeometryProperty.toULongLong());
  const QString firstKind = firstKindProperty.toString();
  const QString secondKind = kindName(hitKind);

  // CRASH-FREE 14: LINE-RECTANGLE EQUAL
  //
  // A click on a rectangle still resolves to one concrete perimeter line
  // (hitId). Equal between a standalone line and a rectangle therefore means
  // equality with THAT selected side. Rectangle + Rectangle keeps the
  // dedicated width/height mapping below.
  const bool firstLineLike =
      firstKind == QStringLiteral("line") ||
      firstKind == QStringLiteral("rectangle");
  const bool secondLineLike =
      secondKind == QStringLiteral("line") ||
      secondKind == QStringLiteral("rectangle");
  const bool mixedLineRectangle =
      firstLineLike &&
      secondLineLike &&
      firstKind != secondKind;

  if (firstKind != secondKind &&
      !mixedLineRectangle) {
    resetEqualState();
    emit selectionChanged(QString::fromUtf8(
        "Эквивалентность: выберите два объекта одного типа"));
    update();
    return;
  }

  if (firstKind == QStringLiteral("circle")) {
    if (firstId == hitId ||
        !sketch_.circleIndex(firstId)) {
      resetEqualState();
      emit selectionChanged(QString::fromUtf8(
          "Эквивалентность: выберите две разные окружности"));
      update();
      return;
    }

    for (const auto& constraint : sketch_.constraints()) {
      if (constraint.type != sketch::ConstraintType::Equal)
        continue;

      const bool sameOrder =
          constraint.firstGeometry == firstId &&
          constraint.secondGeometry == hitId;
      const bool reverseOrder =
          constraint.firstGeometry == hitId &&
          constraint.secondGeometry == firstId;

      if (sameOrder || reverseOrder) {
        resetEqualState();
        emit selectionChanged(
            QString::fromUtf8("Эти окружности уже эквивалентны"));
        update();
        return;
      }
    }

    pushUndoState();

    sketch::Constraint constraint;
    constraint.type = sketch::ConstraintType::Equal;
    constraint.firstGeometry = firstId;
    constraint.secondGeometry = hitId;
    sketch_.addConstraint(constraint);

    (void)sketch::BasicSketchSolver::solve(sketch_);
    resetEqualState();

    clearGeometrySelection();
    selectedCircleIds_.push_back(hitId);
    selectionKind_ = SelectionKind::Circle;
    selectionCircleId_ = hitId;
    selectionLineId_ = sketch::kInvalidGeometryId;
    emit lineStyleSelectionChanged(false, false);

    emit selectionChanged(
        QString::fromUtf8("Ограничение: Эквивалентность"));
    notifyGeometryChanged();
    update();
    return;
  }

  if (mixedLineRectangle) {
    if (firstId == hitId ||
        !sketch_.lineIndex(firstId) ||
        !sketch_.lineIndex(hitId)) {
      resetEqualState();
      emit selectionChanged(QString::fromUtf8(
          "Эквивалентность: выберите две разные линии"));
      update();
      return;
    }

    for (const auto& constraint : sketch_.constraints()) {
      if (constraint.type !=
          sketch::ConstraintType::Equal)
        continue;

      const bool sameOrder =
          constraint.firstGeometry == firstId &&
          constraint.secondGeometry == hitId;
      const bool reverseOrder =
          constraint.firstGeometry == hitId &&
          constraint.secondGeometry == firstId;

      if (sameOrder || reverseOrder) {
        resetEqualState();
        emit selectionChanged(
            QString::fromUtf8(
                "Эти линии уже эквивалентны"));
        update();
        return;
      }
    }

    pushUndoState();

    sketch::Constraint constraint;
    constraint.type =
        sketch::ConstraintType::Equal;
    constraint.firstGeometry = firstId;
    constraint.secondGeometry = hitId;
    sketch_.addConstraint(constraint);

    (void)sketch::BasicSketchSolver::solve(sketch_);

    resetEqualState();

    const auto hitIndex =
        sketch_.lineIndex(hitId);

    clearGeometrySelection();

    if (hitIndex) {
      selectedElementIds_.push_back(
          sketch_.lines()[*hitIndex].elementId);
      selectionKind_ = SelectionKind::Line;
      selectionElementId_ =
          sketch_.lines()[*hitIndex].elementId;
      selectionLineId_ = hitId;
      selectionCircleId_ =
          sketch::kInvalidGeometryId;

      emit lineStyleSelectionChanged(
          true,
          sketch_.lines()[*hitIndex].dashed);
    }

    emit selectionChanged(
        QString::fromUtf8(
            "Ограничение: Эквивалентность"));

    notifyGeometryChanged();
    update();
    return;
  }
  if (firstKind == QStringLiteral("line")) {
    if (firstId == hitId ||
        !sketch_.lineIndex(firstId) ||
        !sketch_.lineIndex(hitId)) {
      resetEqualState();
      emit selectionChanged(QString::fromUtf8(
          "Эквивалентность: выберите две разные линии"));
      update();
      return;
    }

    for (const auto& constraint : sketch_.constraints()) {
      if (constraint.type != sketch::ConstraintType::Equal)
        continue;

      const bool sameOrder =
          constraint.firstGeometry == firstId &&
          constraint.secondGeometry == hitId;
      const bool reverseOrder =
          constraint.firstGeometry == hitId &&
          constraint.secondGeometry == firstId;

      if (sameOrder || reverseOrder) {
        resetEqualState();
        emit selectionChanged(
            QString::fromUtf8("Эти линии уже эквивалентны"));
        update();
        return;
      }
    }

    pushUndoState();

    sketch::Constraint constraint;
    constraint.type = sketch::ConstraintType::Equal;
    constraint.firstGeometry = firstId;
    constraint.secondGeometry = hitId;
    sketch_.addConstraint(constraint);

    (void)sketch::BasicSketchSolver::solve(sketch_);
    resetEqualState();

    const auto hitIndex = sketch_.lineIndex(hitId);
    clearGeometrySelection();

    if (hitIndex) {
      selectedElementIds_.push_back(
          sketch_.lines()[*hitIndex].elementId);
      selectionKind_ = SelectionKind::Line;
      selectionElementId_ =
          sketch_.lines()[*hitIndex].elementId;
      selectionLineId_ = hitId;
      selectionCircleId_ = sketch::kInvalidGeometryId;

      emit lineStyleSelectionChanged(
          true, sketch_.lines()[*hitIndex].dashed);
    }

    emit selectionChanged(
        QString::fromUtf8("Ограничение: Эквивалентность"));
    notifyGeometryChanged();
    update();
    return;
  }

  // Rectangle + Rectangle.
  const auto firstElementId = static_cast<std::size_t>(
      property("equalFirstElement").toULongLong());
  const auto secondElementId = hitElementId;

  if (firstElementId == 0 ||
      secondElementId == 0 ||
      firstElementId == secondElementId) {
    resetEqualState();
    emit selectionChanged(QString::fromUtf8(
        "Эквивалентность: выберите два разных прямоугольника"));
    update();
    return;
  }

  std::vector<sketch::GeometryId> firstRectangleIds;
  std::vector<sketch::GeometryId> secondRectangleIds;

  for (std::size_t index = 0; index < sketch_.lines().size(); ++index) {
    const auto& line = sketch_.lines()[index];

    if (line.elementId == firstElementId)
      firstRectangleIds.push_back(sketch_.lineId(index));

    if (line.elementId == secondElementId)
      secondRectangleIds.push_back(sketch_.lineId(index));
  }

  if (firstRectangleIds.size() != 4 ||
      secondRectangleIds.size() != 4) {
    resetEqualState();
    emit selectionChanged(QString::fromUtf8(
        "Эквивалентность: составной объект не распознан как прямоугольник"));
    update();
    return;
  }

  const auto findIndex =
      [](const std::vector<sketch::GeometryId>& ids,
         sketch::GeometryId id) -> std::optional<std::size_t> {
        const auto found = std::find(ids.begin(), ids.end(), id);
        if (found == ids.end()) return std::nullopt;

        return static_cast<std::size_t>(
            std::distance(ids.begin(), found));
      };

  const auto firstSideIndex =
      findIndex(firstRectangleIds, firstId);

  if (!firstSideIndex) {
    resetEqualState();
    return;
  }

  const auto firstLineIndex = sketch_.lineIndex(firstId);
  if (!firstLineIndex) {
    resetEqualState();
    return;
  }

  const auto& firstSelectedLine =
      sketch_.lines()[*firstLineIndex];

  const double firstDx =
      firstSelectedLine.end.xMm - firstSelectedLine.start.xMm;
  const double firstDy =
      firstSelectedLine.end.yMm - firstSelectedLine.start.yMm;
  const double firstLength =
      std::hypot(firstDx, firstDy);

  if (firstLength <= 1e-9) {
    resetEqualState();
    return;
  }

  const double firstUx = firstDx / firstLength;
  const double firstUy = firstDy / firstLength;

  // The second click selects the rectangle, not the dimension mapping.
  // Match its side automatically by orientation so width cannot be
  // accidentally linked to height.
  std::optional<std::size_t> secondSideIndex;
  double bestParallelScore = -1.0;

  for (std::size_t sideIndex = 0;
       sideIndex < secondRectangleIds.size();
       ++sideIndex) {
    const auto candidateIndex =
        sketch_.lineIndex(secondRectangleIds[sideIndex]);
    if (!candidateIndex) continue;

    const auto& candidate =
        sketch_.lines()[*candidateIndex];

    const double dx =
        candidate.end.xMm - candidate.start.xMm;
    const double dy =
        candidate.end.yMm - candidate.start.yMm;
    const double length = std::hypot(dx, dy);
    if (length <= 1e-9) continue;

    const double ux = dx / length;
    const double uy = dy / length;

    // abs(dot) treats parallel and anti-parallel directions as equivalent.
    const double score =
        std::abs(firstUx * ux + firstUy * uy);

    if (score > bestParallelScore) {
      bestParallelScore = score;
      secondSideIndex = sideIndex;
    }
  }

  if (!secondSideIndex) {
    resetEqualState();
    return;
  }

  const auto matchedSecondId =
      secondRectangleIds[*secondSideIndex];

  const auto firstAdjacentId =
      firstRectangleIds[(*firstSideIndex + 1) % 4];
  const auto secondAdjacentId =
      secondRectangleIds[(*secondSideIndex + 1) % 4];

  const auto equalExists =
      [this](sketch::GeometryId first,
             sketch::GeometryId second) {
        return std::any_of(
            sketch_.constraints().begin(),
            sketch_.constraints().end(),
            [first, second](const sketch::Constraint& constraint) {
              if (constraint.type !=
                  sketch::ConstraintType::Equal)
                return false;

              return (constraint.firstGeometry == first &&
                      constraint.secondGeometry == second) ||
                     (constraint.firstGeometry == second &&
                      constraint.secondGeometry == first);
            });
      };

  const bool firstPairExists =
      equalExists(firstId, matchedSecondId);
  const bool secondPairExists =
      equalExists(firstAdjacentId, secondAdjacentId);

  if (firstPairExists && secondPairExists) {
    resetEqualState();
    emit selectionChanged(QString::fromUtf8(
        "Эти прямоугольники уже эквивалентны"));
    update();
    return;
  }

  pushUndoState();

  if (!firstPairExists) {
    sketch::Constraint firstEqual;
    firstEqual.type = sketch::ConstraintType::Equal;
    firstEqual.firstGeometry = firstId;
    firstEqual.secondGeometry = matchedSecondId;
    sketch_.addConstraint(firstEqual);
  }

  if (!secondPairExists) {
    sketch::Constraint secondEqual;
    secondEqual.type = sketch::ConstraintType::Equal;
    secondEqual.firstGeometry = firstAdjacentId;
    secondEqual.secondGeometry = secondAdjacentId;
    sketch_.addConstraint(secondEqual);
  }

  (void)sketch::BasicSketchSolver::solve(sketch_);
  resetEqualState();

  clearGeometrySelection();
  selectedElementIds_.push_back(secondElementId);
  selectionKind_ = SelectionKind::Line;
  selectionElementId_ = secondElementId;
  selectionLineId_ = hitId;
  selectionCircleId_ = sketch::kInvalidGeometryId;

  const auto hitIndex = sketch_.lineIndex(hitId);
  emit lineStyleSelectionChanged(
      true, hitIndex && sketch_.lines()[*hitIndex].dashed);

  emit selectionChanged(QString::fromUtf8(
      "Ограничение: Эквивалентность прямоугольников"));
  notifyGeometryChanged();
  update();
}
void SketchCanvas::handleParallelConstraintClick(QPointF position) {
  constexpr double hitTolerance = 9.0;
  double bestDistance = hitTolerance;
  std::optional<std::size_t> bestIndex;

  for (std::size_t index = 0; index < sketch_.lines().size(); ++index) {
    const auto& line = sketch_.lines()[index];
    const double distance = pointSegmentDistance(
        position, mapPoint(line.start), mapPoint(line.end));

    if (distance < bestDistance) {
      bestDistance = distance;
      bestIndex = index;
    }
  }

  if (!bestIndex) return;

  const auto clickedId = sketch_.lineId(*bestIndex);
  if (clickedId == sketch::kInvalidGeometryId) return;

  const QVariant firstProperty = property("parallelFirstLine");

  if (!firstProperty.isValid()) {
    setProperty("parallelFirstLine",
                static_cast<qulonglong>(clickedId));

    clearGeometrySelection();
    selectedElementIds_.push_back(
        sketch_.lines()[*bestIndex].elementId);
    selectionKind_ = SelectionKind::Line;
    selectionElementId_ = sketch_.lines()[*bestIndex].elementId;
    selectionLineId_ = clickedId;
    selectionCircleId_ = sketch::kInvalidGeometryId;

    emit selectionChanged(
        QString::fromUtf8("Параллельность: выберите вторую линию"));
    emit lineStyleSelectionChanged(
        true, sketch_.lines()[*bestIndex].dashed);
    update();
    return;
  }

  const auto firstId = static_cast<sketch::GeometryId>(
      firstProperty.toULongLong());

  if (firstId == sketch::kInvalidGeometryId ||
      !sketch_.lineIndex(firstId)) {
    setProperty("parallelFirstLine", QVariant());
    return;
  }

  if (firstId == clickedId) {
    setProperty("parallelFirstLine", QVariant());
    emit selectionChanged(
        QString::fromUtf8("Параллельность: выберите две разные линии"));
    update();
    return;
  }

  for (const auto& constraint : sketch_.constraints()) {
    if (constraint.type != sketch::ConstraintType::Parallel) continue;

    const bool sameOrder =
        constraint.firstGeometry == firstId &&
        constraint.secondGeometry == clickedId;
    const bool reverseOrder =
        constraint.firstGeometry == clickedId &&
        constraint.secondGeometry == firstId;

    if (sameOrder || reverseOrder) {
      setProperty("parallelFirstLine", QVariant());
      emit selectionChanged(
          QString::fromUtf8("Эти линии уже параллельны"));
      update();
      return;
    }
  }

  const auto orthogonalType =
      [this](sketch::GeometryId id)
          -> std::optional<sketch::ConstraintType> {
        for (const auto& constraint : sketch_.constraints()) {
          if (constraint.firstGeometry != id) continue;

          if (constraint.type == sketch::ConstraintType::Horizontal ||
              constraint.type == sketch::ConstraintType::Vertical)
            return constraint.type;
        }

        return std::nullopt;
      };

  const auto firstOrthogonal = orthogonalType(firstId);
  const auto secondOrthogonal = orthogonalType(clickedId);

  if (firstOrthogonal && secondOrthogonal &&
      *firstOrthogonal != *secondOrthogonal) {
    setProperty("parallelFirstLine", QVariant());
    emit selectionChanged(QString::fromUtf8(
        "Конфликт: одна линия горизонтальна, другая вертикальна"));
    update();
    return;
  }

  pushUndoState();

  // Parallel conflicts with an explicit angle/perpendicular relationship on
  // the same pair in the current sequential solver. Remove only that pair.
  std::vector<sketch::ConstraintId> conflicting;

  for (const auto& constraint : sketch_.constraints()) {
    if (constraint.type != sketch::ConstraintType::Angle &&
        constraint.type != sketch::ConstraintType::Perpendicular)
      continue;

    const bool sameOrder =
        constraint.firstGeometry == firstId &&
        constraint.secondGeometry == clickedId;
    const bool reverseOrder =
        constraint.firstGeometry == clickedId &&
        constraint.secondGeometry == firstId;

    if (sameOrder || reverseOrder)
      conflicting.push_back(constraint.id);
  }

  for (const auto id : conflicting)
    sketch_.removeConstraint(id);

  sketch::Constraint constraint;
  constraint.type = sketch::ConstraintType::Parallel;
  constraint.firstGeometry = firstId;
  constraint.secondGeometry = clickedId;
  sketch_.addConstraint(constraint);

  (void)sketch::BasicSketchSolver::solve(sketch_);

  setProperty("parallelFirstLine", QVariant());

  const auto clickedIndex = sketch_.lineIndex(clickedId);
  clearGeometrySelection();

  if (clickedIndex) {
    selectedElementIds_.push_back(
        sketch_.lines()[*clickedIndex].elementId);
    selectionKind_ = SelectionKind::Line;
    selectionElementId_ = sketch_.lines()[*clickedIndex].elementId;
    selectionLineId_ = clickedId;
    selectionCircleId_ = sketch::kInvalidGeometryId;

    emit lineStyleSelectionChanged(
        true, sketch_.lines()[*clickedIndex].dashed);
  }

  emit selectionChanged(
      QString::fromUtf8("Ограничение: Параллельность"));
  notifyGeometryChanged();
  update();
}
void SketchCanvas::handlePerpendicularConstraintClick(QPointF position) {
  constexpr double hitTolerance = 9.0;
  double bestDistance = hitTolerance;
  std::optional<std::size_t> bestIndex;

  for (std::size_t index = 0; index < sketch_.lines().size(); ++index) {
    const auto& line = sketch_.lines()[index];
    const double distance = pointSegmentDistance(
        position, mapPoint(line.start), mapPoint(line.end));

    if (distance < bestDistance) {
      bestDistance = distance;
      bestIndex = index;
    }
  }

  if (!bestIndex) return;

  const auto clickedId = sketch_.lineId(*bestIndex);
  if (clickedId == sketch::kInvalidGeometryId) return;

  const QVariant firstProperty = property("perpendicularFirstLine");

  if (!firstProperty.isValid()) {
    setProperty("perpendicularFirstLine",
                static_cast<qulonglong>(clickedId));

    clearGeometrySelection();
    selectedElementIds_.push_back(sketch_.lines()[*bestIndex].elementId);
    selectionKind_ = SelectionKind::Line;
    selectionElementId_ = sketch_.lines()[*bestIndex].elementId;
    selectionLineId_ = clickedId;
    selectionCircleId_ = sketch::kInvalidGeometryId;

    emit selectionChanged(
        QString::fromUtf8("Перпендикулярность: выберите вторую линию"));
    emit lineStyleSelectionChanged(
        true, sketch_.lines()[*bestIndex].dashed);
    update();
    return;
  }

  const auto firstId = static_cast<sketch::GeometryId>(
      firstProperty.toULongLong());

  if (firstId == sketch::kInvalidGeometryId ||
      !sketch_.lineIndex(firstId)) {
    setProperty("perpendicularFirstLine", QVariant());
    return;
  }

  if (firstId == clickedId) {
    setProperty("perpendicularFirstLine", QVariant());
    emit selectionChanged(
        QString::fromUtf8("Перпендикулярность: выберите две разные линии"));
    update();
    return;
  }

  // Do not create a duplicate in either order.
  for (const auto& constraint : sketch_.constraints()) {
    if (constraint.type != sketch::ConstraintType::Perpendicular) continue;

    const bool sameOrder =
        constraint.firstGeometry == firstId &&
        constraint.secondGeometry == clickedId;
    const bool reverseOrder =
        constraint.firstGeometry == clickedId &&
        constraint.secondGeometry == firstId;

    if (sameOrder || reverseOrder) {
      setProperty("perpendicularFirstLine", QVariant());
      emit selectionChanged(
          QString::fromUtf8("Эти линии уже перпендикулярны"));
      update();
      return;
    }
  }

  const auto orthogonalType =
      [this](sketch::GeometryId id)
          -> std::optional<sketch::ConstraintType> {
        for (const auto& constraint : sketch_.constraints()) {
          if (constraint.firstGeometry != id) continue;
          if (constraint.type == sketch::ConstraintType::Horizontal ||
              constraint.type == sketch::ConstraintType::Vertical)
            return constraint.type;
        }
        return std::nullopt;
      };

  const auto firstOrthogonal = orthogonalType(firstId);
  const auto secondOrthogonal = orthogonalType(clickedId);

  // Two H/V-fixed lines can only accept Perpendicular when their fixed
  // orientations are already opposite.
  if (firstOrthogonal && secondOrthogonal &&
      *firstOrthogonal == *secondOrthogonal) {
    setProperty("perpendicularFirstLine", QVariant());
    emit selectionChanged(QString::fromUtf8(
        "Конфликт: обе линии уже имеют одинаковую H/V-ориентацию"));
    update();
    return;
  }

  pushUndoState();

  // Perpendicular is an angular relationship. Remove an old explicit Angle
  // constraint for this exact pair so the simple solver is not overconstrained.
  std::vector<sketch::ConstraintId> oldAngles;

  for (const auto& constraint : sketch_.constraints()) {
    if (constraint.type != sketch::ConstraintType::Angle) continue;

    const bool sameOrder =
        constraint.firstGeometry == firstId &&
        constraint.secondGeometry == clickedId;
    const bool reverseOrder =
        constraint.firstGeometry == clickedId &&
        constraint.secondGeometry == firstId;

    if (sameOrder || reverseOrder)
      oldAngles.push_back(constraint.id);
  }

  for (const auto id : oldAngles)
    sketch_.removeConstraint(id);

  sketch::Constraint constraint;
  constraint.type = sketch::ConstraintType::Perpendicular;
  constraint.firstGeometry = firstId;
  constraint.secondGeometry = clickedId;
  sketch_.addConstraint(constraint);

  // addConstraint() already runs the sketch solver. Do not run the full
  // sequential solver a second time here: with interconnected constraints
  // that duplicate pass can move geometry twice.

  setProperty("perpendicularFirstLine", QVariant());

  const auto clickedIndex = sketch_.lineIndex(clickedId);
  clearGeometrySelection();

  if (clickedIndex) {
    selectedElementIds_.push_back(
        sketch_.lines()[*clickedIndex].elementId);
    selectionKind_ = SelectionKind::Line;
    selectionElementId_ = sketch_.lines()[*clickedIndex].elementId;
    selectionLineId_ = clickedId;
    selectionCircleId_ = sketch::kInvalidGeometryId;

    emit lineStyleSelectionChanged(
        true, sketch_.lines()[*clickedIndex].dashed);
  }

  emit selectionChanged(
      QString::fromUtf8("Ограничение: Перпендикулярность"));
  notifyGeometryChanged();
  update();
}
void SketchCanvas::handleOrthogonalConstraintClick(QPointF position) {
  constexpr double hitTolerance = 9.0;
  double bestDistance = hitTolerance;
  std::optional<std::size_t> bestIndex;

  for (std::size_t index = 0; index < sketch_.lines().size(); ++index) {
    const auto& line = sketch_.lines()[index];
    const double distance = pointSegmentDistance(
        position, mapPoint(line.start), mapPoint(line.end));
    if (distance < bestDistance) {
      bestDistance = distance;
      bestIndex = index;
    }
  }

  if (!bestIndex) return;

  const auto& line = sketch_.lines()[*bestIndex];
  const double dx = std::abs(line.end.xMm - line.start.xMm);
  const double dy = std::abs(line.end.yMm - line.start.yMm);
  const auto id = sketch_.lineId(*bestIndex);
  if (id == sketch::kInvalidGeometryId) return;

  const auto type = dy > dx ? sketch::ConstraintType::Vertical
                            : sketch::ConstraintType::Horizontal;

  // Avoid stacking duplicate H/V constraints on the same primitive.
  for (const auto& constraint : sketch_.constraints()) {
    if (constraint.firstGeometry == id && constraint.type == type) {
      emit selectionChanged(
          type == sketch::ConstraintType::Vertical
              ? QString::fromUtf8("Линия уже вертикальна")
              : QString::fromUtf8("Линия уже горизонтальна"));
      return;
    }
  }

  pushUndoState();

  // If the same line had the opposite orthogonal constraint, replace it.
  std::vector<sketch::ConstraintId> opposite;
  for (const auto& constraint : sketch_.constraints()) {
    if (constraint.firstGeometry != id) continue;
    if ((type == sketch::ConstraintType::Vertical &&
         constraint.type == sketch::ConstraintType::Horizontal) ||
        (type == sketch::ConstraintType::Horizontal &&
         constraint.type == sketch::ConstraintType::Vertical))
      opposite.push_back(constraint.id);
  }
  for (const auto constraintId : opposite)
    sketch_.removeConstraint(constraintId);

  sketch::Constraint constraint;
  constraint.type = type;
  constraint.firstGeometry = id;
  sketch_.addConstraint(constraint);

  selectionKind_ = SelectionKind::Line;
  selectionElementId_ = line.elementId;
  selectionLineId_ = id;
  emit selectionChanged(
      type == sketch::ConstraintType::Vertical
          ? QString::fromUtf8("Ограничение: вертикально")
          : QString::fromUtf8("Ограничение: горизонтально"));
  notifyGeometryChanged();
  update();
}

void SketchCanvas::handleAutoDimensionClick(QPointF position) {
  // AutoDimension owns mouse movement while it is active. Never allow a
  // stale installed-dimension drag state to intercept its live preview.
  setProperty("draggingDimensionLine", QVariant());
  setProperty("draggingDimensionLabel", QVariant());

  constexpr double pointTolerance = 8.0;
  std::optional<sketch::PointReference> clickedPoint;
  double pointDistance = pointTolerance;
  for (std::size_t index = 0; index < sketch_.lines().size(); ++index) {
    const auto& line = sketch_.lines()[index];
    for (const bool start : {true, false}) {
      const auto point = start ? line.start : line.end;
      const double distance = QLineF(position, mapPoint(point)).length();
      if (distance < pointDistance) {
        pointDistance = distance;
        clickedPoint = sketch::PointReference{sketch_.lineId(index), start};
      }
    }
  }
  // CRASH-FREE 06 V2: CIRCLE CENTER AUTODIMENSION
  //
  // Circle centres compete with line endpoints in the same point hit-test.
  // PointReference already supports circleId, so this keeps all geometry
  // references stable and lets the existing distance solver do the work.
  for (std::size_t index = 0;
       index < sketch_.circles().size();
       ++index) {
    const auto circleId =
        sketch_.circleId(index);

    if (circleId ==
        sketch::kInvalidGeometryId)
      continue;

    const double distance =
        QLineF(
            position,
            mapPoint(
                sketch_.circles()[index].center))
            .length();

    if (distance >= pointDistance)
      continue;

    pointDistance = distance;

    sketch::PointReference centerReference;
    centerReference.circleId = circleId;
    clickedPoint = centerReference;
  }
  // CRASH-FREE 08 V2: RECTANGLE CENTER AUTODIMENSION
  //
  // Virtual centres of composite elements are first-class CAD points.
  // They compete with line endpoints and circle centres in the same
  // point hit-test used by AutoDimension.
  for (const auto elementId :
       sketch_.centerNodeElementIds()) {
    const auto center =
        sketch_.elementCenterPoint(elementId);

    if (!center)
      continue;

    const double distance =
        QLineF(position, mapPoint(*center)).length();

    if (distance >= pointDistance)
      continue;

    pointDistance = distance;

    sketch::PointReference centerReference;
    centerReference.elementCenterId = elementId;
    clickedPoint = centerReference;
  }
  if (clickedPoint) {
    if (!property("autoDimensionFirstLine").isValid()) {
      setProperty("autoDimensionFirstLine",
                  static_cast<qulonglong>(clickedPoint->lineId));
      setProperty("autoDimensionFirstStart", clickedPoint->start);
      setProperty(
          "autoDimensionFirstCircle",
          static_cast<qulonglong>(
              clickedPoint->circleId));
      setProperty(
          "autoDimensionFirstElementCenter",
          static_cast<qulonglong>(
              clickedPoint->elementCenterId));
      update();
      return;
    }
    const sketch::PointReference first{
        static_cast<sketch::GeometryId>(
            property("autoDimensionFirstLine").toULongLong()),
        property("autoDimensionFirstStart").toBool(),
        static_cast<sketch::GeometryId>(
            property("autoDimensionFirstCircle").toULongLong()),
        static_cast<std::size_t>(
            property("autoDimensionFirstElementCenter").toULongLong())};
    const auto firstPoint = sketch_.referencedPoint(first);
    const auto secondPoint = sketch_.referencedPoint(*clickedPoint);
    if (!firstPoint || !secondPoint) return;
    const double distance = std::hypot(secondPoint->xMm - firstPoint->xMm,
                                       secondPoint->yMm - firstPoint->yMm);
    if (distance <= 1e-9) return;
    setProperty("autoDimensionTarget", "points");
    setProperty("autoDimensionPointMode", "aligned");
    setProperty("autoDimensionDirectLineId", QVariant());
    setProperty("autoDimensionOffsetMm", 4.0);
    setProperty("autoDimensionSecondLine",
                static_cast<qulonglong>(clickedPoint->lineId));
    setProperty("autoDimensionSecondStart", clickedPoint->start);
    setProperty(
        "autoDimensionSecondCircle",
        static_cast<qulonglong>(
            clickedPoint->circleId));
    setProperty(
        "autoDimensionSecondElementCenter",
        static_cast<qulonglong>(
            clickedPoint->elementCenterId));
    primaryDimension_->setPrefix(QString());
    primaryDimension_->setSuffix(QString::fromUtf8(" мм"));
    primaryDimension_->setRange(0.01, 100000.0);
    primaryDimension_->setValue(distance);
    secondaryDimension_->hide();
    primaryDimension_->move((position + QPointF(16, 16)).toPoint());
    primaryDimension_->show();
    primaryDimension_->setFocus();
    primaryDimension_->selectAll();
    update();
    return;
  }

  double bestDistance = 9.0;
  std::optional<std::size_t> lineIndex;
  for (std::size_t index = 0; index < sketch_.lines().size(); ++index) {
    const auto& line = sketch_.lines()[index];
    const double distance = pointSegmentDistance(
        position, mapPoint(line.start), mapPoint(line.end));
    if (distance < bestDistance) {
      bestDistance = distance;
      lineIndex = index;
    }
  }
  std::optional<std::size_t> circleIndex;
  for (std::size_t index = 0; index < sketch_.circles().size(); ++index) {
    const auto& circle = sketch_.circles()[index];
    const double distance = std::abs(
        QLineF(position, mapPoint(circle.center)).length() -
        circle.radiusMm * pixelsPerMm_);
    if (distance < bestDistance) {
      bestDistance = distance;
      lineIndex.reset();
      circleIndex = index;
    }
  }
  if (!lineIndex && !circleIndex) return;
  setProperty("autoDimensionFirstLine", QVariant());
  secondaryDimension_->hide();
  primaryDimension_->setRange(0.01, 100000.0);
  if (lineIndex) {
    const auto& line = sketch_.lines()[*lineIndex];
    const auto lineId = sketch_.lineId(*lineIndex);

    // Treat a direct line click exactly like selecting its two endpoints.
    // This gives the line the same three interactive dimension modes:
    // aligned length, X projection and Y projection.
    setProperty("autoDimensionTarget", "points");
    setProperty("autoDimensionPointMode", "aligned");
    setProperty("autoDimensionDirectLineId",
                static_cast<qulonglong>(lineId));
    setProperty("autoDimensionOffsetMm", 4.0);

    setProperty("autoDimensionFirstLine",
                static_cast<qulonglong>(lineId));
    setProperty("autoDimensionFirstStart", true);
    setProperty("autoDimensionSecondLine",
                static_cast<qulonglong>(lineId));
    setProperty("autoDimensionSecondStart", false);

    primaryDimension_->setPrefix(QString());
    primaryDimension_->setValue(
        std::hypot(line.end.xMm - line.start.xMm,
                   line.end.yMm - line.start.yMm));
  } else {
    setProperty("autoDimensionTarget", "circle");
    setProperty("autoDimensionIndex",
                static_cast<qulonglong>(sketch_.circleId(*circleIndex)));
    primaryDimension_->setPrefix(QString::fromUtf8("Ø: "));
    const QPointF center = mapPoint(sketch_.circles()[*circleIndex].center);
    const QPointF delta = position - center;
    setProperty("autoDimensionAngleRad", std::atan2(-delta.y(), delta.x()));
    primaryDimension_->setValue(sketch_.circles()[*circleIndex].radiusMm * 2.0);
  }
  primaryDimension_->setSuffix(QString::fromUtf8(" мм"));
  primaryDimension_->move((position + QPointF(16, 16)).toPoint());
  primaryDimension_->show();
  primaryDimension_->setFocus();
  primaryDimension_->selectAll();
  update();
}

void SketchCanvas::commitAutoDimension() {
  const QString target = property("autoDimensionTarget").toString();
  const double value = primaryDimension_->value();
  if (target.isEmpty() || value <= 0.0) return;
  const bool editingExisting = property("editingDimensionIndex").isValid();
  const auto editingIndex = static_cast<std::size_t>(
      property("editingDimensionIndex").toULongLong());
  pushUndoState();
  sketch::Dimension dimension;
  dimension.valueMm = value;
  dimension.offsetMm = property("autoDimensionOffsetMm").isValid()
                           ? property("autoDimensionOffsetMm").toDouble()
                           : 4.0;
  dimension.angleRad = property("autoDimensionAngleRad").toDouble();
  bool changed = false;
  if (target == "angle") {
    const auto firstId = static_cast<sketch::GeometryId>(
        property("autoDimensionAngleFirstLine").toULongLong());
    const auto secondId = static_cast<sketch::GeometryId>(
        property("autoDimensionAngleSecondLine").toULongLong());

    double solverAngle = value;

    // CRASH-FREE 12: VISIBLE ANGLE BRANCH MAPPING
    //
    // The UI dimension describes the visible finite-segment sector, while the
    // solver works with the primitive line directions. For disconnected lines
    // these can be supplementary (for example visible 30° vs primitive 150°).
    // Determine that branch BEFORE both initial creation and later editing.
    const auto firstIndex =
        sketch_.lineIndex(firstId);
    const auto secondIndex =
        sketch_.lineIndex(secondId);

    if (firstIndex && secondIndex) {
      const double currentPrimitive =
          lineAngleDegrees(
              sketch_.lines()[*firstIndex],
              sketch_.lines()[*secondIndex]);

      const double currentVisible =
          visibleLineAngleDegrees(
              sketch_.lines()[*firstIndex],
              sketch_.lines()[*secondIndex]);

      const double supplement =
          180.0 - currentPrimitive;

      const double primitiveError =
          std::abs(
              currentVisible -
              currentPrimitive);

      const double supplementError =
          std::abs(
              currentVisible -
              supplement);

      if (supplementError <
          primitiveError)
        solverAngle =
            180.0 - value;
    }

    changed =
        sketch_.setLineAngleByIds(
            firstId,
            secondId,
            solverAngle);
    dimension.kind = sketch::DimensionKind::LineAngle;
    dimension.geometryId = firstId;
    // LineAngle uses secondPoint.lineId as the second stable line reference.
    dimension.secondPoint.lineId = secondId;

    if (changed) {
      std::vector<sketch::ConstraintId> oldAngleConstraints;
      for (const auto& constraint : sketch_.constraints()) {
        if (constraint.type != sketch::ConstraintType::Angle) continue;

        const bool sameOrder =
            constraint.firstGeometry == firstId &&
            constraint.secondGeometry == secondId;
        const bool reverseOrder =
            constraint.firstGeometry == secondId &&
            constraint.secondGeometry == firstId;

        if (sameOrder || reverseOrder)
          oldAngleConstraints.push_back(constraint.id);
      }

      for (const auto constraintId : oldAngleConstraints)
        sketch_.removeConstraint(constraintId);

      sketch::Constraint angleConstraint;
      angleConstraint.type = sketch::ConstraintType::Angle;
      angleConstraint.firstGeometry = firstId;
      angleConstraint.secondGeometry = secondId;
      angleConstraint.value = solverAngle;
      sketch_.addConstraint(angleConstraint);
    }
  } else if (target == "line") {
    const auto id = static_cast<sketch::GeometryId>(
        property("autoDimensionIndex").toULongLong());
    changed = sketch_.setLineLengthById(id, value);
    dimension.kind = sketch::DimensionKind::LineLength;
    dimension.geometryId = id;

    if (changed) {
      // A displayed line length is a driving CAD constraint, not merely
      // an annotation. Keep at most one Length constraint per line.
      std::vector<sketch::ConstraintId> oldLengthConstraints;
      for (const auto& constraint : sketch_.constraints()) {
        if (constraint.type == sketch::ConstraintType::Length &&
            constraint.firstGeometry == id)
          oldLengthConstraints.push_back(constraint.id);
      }
      for (const auto constraintId : oldLengthConstraints)
        sketch_.removeConstraint(constraintId);

      sketch::Constraint lengthConstraint;
      lengthConstraint.type = sketch::ConstraintType::Length;
      lengthConstraint.firstGeometry = id;
      lengthConstraint.value = value;
      sketch_.addConstraint(lengthConstraint);

      // CRASH-FREE 14: PROPAGATE DRIVING LENGTH THROUGH EQUAL
      //
      // Direct dimension editing already resized this geometry. If it belongs
      // to an Equal relationship, immediately solve that relationship so its
      // follower receives the new driving size in the same user action.
      const bool participatesInEqual =
          std::any_of(
              sketch_.constraints().begin(),
              sketch_.constraints().end(),
              [id](const sketch::Constraint& item) {
                return item.type ==
                           sketch::ConstraintType::Equal &&
                       (item.firstGeometry == id ||
                        item.secondGeometry == id);
              });

      if (participatesInEqual)
        (void)sketch::BasicSketchSolver::solve(sketch_);
    }
  } else if (target == "circle") {
    const auto id = static_cast<sketch::GeometryId>(
        property("autoDimensionIndex").toULongLong());

    changed = sketch_.setCircleDiameterById(id, value);
    dimension.kind = sketch::DimensionKind::CircleDiameter;
    dimension.geometryId = id;

    if (changed) {
      // A displayed circle diameter is a driving CAD constraint.
      // Keep at most one Diameter constraint per circle.
      std::vector<sketch::ConstraintId> oldDiameterConstraints;

      for (const auto& constraint : sketch_.constraints()) {
        if (constraint.type == sketch::ConstraintType::Diameter &&
            constraint.firstGeometry == id) {
          oldDiameterConstraints.push_back(constraint.id);
        }
      }

      for (const auto constraintId : oldDiameterConstraints)
        sketch_.removeConstraint(constraintId);

      sketch::Constraint diameterConstraint;
      diameterConstraint.type = sketch::ConstraintType::Diameter;
      diameterConstraint.firstGeometry = id;
      diameterConstraint.value = value;
      sketch_.addConstraint(diameterConstraint);
    }
  } else if (target == "points") {
    const sketch::PointReference first{
          static_cast<sketch::GeometryId>(
              property("autoDimensionFirstLine").toULongLong()),
          property("autoDimensionFirstStart").toBool(),
          static_cast<sketch::GeometryId>(
            property("autoDimensionFirstCircle").toULongLong()),
        static_cast<std::size_t>(
            property("autoDimensionFirstElementCenter").toULongLong())};
    const sketch::PointReference second{
          static_cast<sketch::GeometryId>(
              property("autoDimensionSecondLine").toULongLong()),
          property("autoDimensionSecondStart").toBool(),
          static_cast<sketch::GeometryId>(
            property("autoDimensionSecondCircle").toULongLong()),
        static_cast<std::size_t>(
            property("autoDimensionSecondElementCenter").toULongLong())};

    const QString pointMode =
        property("autoDimensionPointMode").toString();

    sketch::ConstraintType constraintType = sketch::ConstraintType::Distance;
    if (pointMode == QStringLiteral("x")) {
      changed = sketch_.setPointDistanceX(first, second, value);
      dimension.kind = sketch::DimensionKind::PointDistanceX;
      constraintType = sketch::ConstraintType::DistanceX;
    } else if (pointMode == QStringLiteral("y")) {
      changed = sketch_.setPointDistanceY(first, second, value);
      dimension.kind = sketch::DimensionKind::PointDistanceY;
      constraintType = sketch::ConstraintType::DistanceY;
    } else {
      changed = sketch_.setPointDistance(first, second, value);
      dimension.kind = sketch::DimensionKind::PointDistance;
    }

    dimension.firstPoint = first;
    dimension.secondPoint = second;

    if (changed) {
      // The same point pair may have one driving distance mode at a time.
      std::vector<sketch::ConstraintId> oldDistanceConstraints;
      for (const auto& constraint : sketch_.constraints()) {
        if (constraint.type != sketch::ConstraintType::Distance &&
            constraint.type != sketch::ConstraintType::DistanceX &&
            constraint.type != sketch::ConstraintType::DistanceY)
          continue;

              // CRASH-FREE 09 V2: KEEP INDEPENDENT DISTANCE MODES
      //
      // Aligned, X and Y are separate driving constraints. Replacing an
      // existing dimension must remove only the SAME mode for the SAME pair.
      if (constraint.type != constraintType)
        continue;
const bool sameOrder =
            constraint.firstPoint.lineId == first.lineId &&
            constraint.firstPoint.start == first.start &&
            constraint.secondPoint.lineId == second.lineId &&
            constraint.secondPoint.start == second.start;
        const bool reverseOrder =
            constraint.firstPoint.lineId == second.lineId &&
            constraint.firstPoint.start == second.start &&
            constraint.secondPoint.lineId == first.lineId &&
            constraint.secondPoint.start == first.start;

        if (sameOrder || reverseOrder)
          oldDistanceConstraints.push_back(constraint.id);
      }

      for (const auto constraintId : oldDistanceConstraints)
        sketch_.removeConstraint(constraintId);

      sketch::Constraint distanceConstraint;
      distanceConstraint.type = constraintType;
      distanceConstraint.firstPoint = first;
      distanceConstraint.secondPoint = second;
      distanceConstraint.value = value;
      sketch_.addConstraint(distanceConstraint);
    }
  }
  if (changed && editingExisting) {
    sketch_.setDimensionValue(editingIndex, value);
    sketch_.setDimensionPlacement(editingIndex, dimension.offsetMm,
                                  dimension.angleRad);
  } else if (changed) {
    sketch_.storeDimension(dimension);
  }
  if (changed && !editingExisting) {
    QVariantList alongValues = property("dimensionLabelAlongMm").toList();
    QVariantList offsetValues = property("dimensionLabelOffsetMm").toList();
    alongValues.push_back(0.0);
    offsetValues.push_back(2.0);
    setProperty("dimensionLabelAlongMm", alongValues);
    setProperty("dimensionLabelOffsetMm", offsetValues);
  }
  setProperty("autoDimensionTarget", QVariant());
  setProperty("autoDimensionFirstLine", QVariant());
  setProperty("autoDimensionFirstStart", QVariant());
  // CRASH-FREE 08 V2: RESET ELEMENT CENTER CHANNELS
  setProperty(
      "autoDimensionFirstElementCenter",
      static_cast<qulonglong>(0));
  setProperty(
      "autoDimensionSecondElementCenter",
      static_cast<qulonglong>(0));
  // CRASH-FREE 06 V2: RESET CIRCLE POINT CHANNELS
  setProperty(
      "autoDimensionFirstCircle",
      static_cast<qulonglong>(
          sketch::kInvalidGeometryId));
  setProperty(
      "autoDimensionSecondCircle",
      static_cast<qulonglong>(
          sketch::kInvalidGeometryId));
  setProperty("autoDimensionPointMode", QVariant());
  setProperty("autoDimensionDirectLineId", QVariant());
  setProperty("autoDimensionAngleFirstLine", QVariant());
  setProperty("autoDimensionAngleSecondLine", QVariant());
  setProperty("editingDimensionIndex", QVariant());

  // CRASH-FREE 13: EDIT SESSION CLEANUP
  setProperty("selectedDimension", QVariant());

  hideDimensionEditor();
  setFocus();
  notifyGeometryChanged();
}

void SketchCanvas::wheelEvent(QWheelEvent* event) {
  pixelsPerMm_ = std::clamp(
      pixelsPerMm_ * (event->angleDelta().y() > 0 ? 1.12 : 0.89), 0.5, 30.0);
  update();
}

bool SketchCanvas::lineElementSelected(std::size_t elementId) const {
  return std::find(selectedElementIds_.begin(), selectedElementIds_.end(),
                   elementId) != selectedElementIds_.end();
}

bool SketchCanvas::circleSelected(sketch::GeometryId id) const {
  return std::find(selectedCircleIds_.begin(), selectedCircleIds_.end(),
                   id) != selectedCircleIds_.end();
}

void SketchCanvas::clearGeometrySelection() {
  selectedElementIds_.clear();
  selectedCircleIds_.clear();
  selectionKind_ = SelectionKind::None;
  selectionLineId_ = sketch::kInvalidGeometryId;
  selectionCircleId_ = sketch::kInvalidGeometryId;
  selectionElementId_ = 0;
}

void SketchCanvas::selectAt(QPointF position, bool additive,
                            bool preserveExistingIfHit) {
  double bestDistance = 9.0;

  SelectionKind hitKind = SelectionKind::None;
  std::size_t hitElementId = 0;
  sketch::GeometryId hitLineId = sketch::kInvalidGeometryId;
  sketch::GeometryId hitCircleId = sketch::kInvalidGeometryId;
  bool hitDashed = false;

  for (std::size_t index = 0; index < sketch_.lines().size(); ++index) {
    const auto& line = sketch_.lines()[index];
    const double distance = pointSegmentDistance(
        position, mapPoint(line.start), mapPoint(line.end));

    if (distance < bestDistance) {
      bestDistance = distance;
      hitKind = SelectionKind::Line;
      hitElementId = line.elementId;
      hitLineId = sketch_.lineId(index);
      hitCircleId = sketch::kInvalidGeometryId;
      hitDashed = line.dashed;
    }
  }

  for (std::size_t index = 0; index < sketch_.circles().size(); ++index) {
    const auto& circle = sketch_.circles()[index];
    const double distance = std::abs(
        QLineF(position, mapPoint(circle.center)).length() -
        circle.radiusMm * pixelsPerMm_);

    if (distance < bestDistance) {
      bestDistance = distance;
      hitKind = SelectionKind::Circle;
      hitElementId = 0;
      hitLineId = sketch::kInvalidGeometryId;
      hitCircleId = sketch_.circleId(index);
      hitDashed = circle.dashed;
    }
  }

  if (hitKind == SelectionKind::None) {
    if (!additive) clearGeometrySelection();
    emit lineStyleSelectionChanged(false, false);
    emit selectionChanged(QString::fromUtf8("Ничего не выбрано"));
    update();
    return;
  }

  const bool alreadySelected =
      hitKind == SelectionKind::Line
          ? lineElementSelected(hitElementId)
          : circleSelected(hitCircleId);

  if (additive) {
    if (hitKind == SelectionKind::Line) {
      const auto found =
          std::find(selectedElementIds_.begin(), selectedElementIds_.end(),
                    hitElementId);
      if (found != selectedElementIds_.end())
        selectedElementIds_.erase(found);
      else
        selectedElementIds_.push_back(hitElementId);
    } else {
      const auto found =
          std::find(selectedCircleIds_.begin(), selectedCircleIds_.end(),
                    hitCircleId);
      if (found != selectedCircleIds_.end())
        selectedCircleIds_.erase(found);
      else
        selectedCircleIds_.push_back(hitCircleId);
    }

    if (alreadySelected) {
      if (selectedElementIds_.empty() && selectedCircleIds_.empty()) {
        clearGeometrySelection();
        emit lineStyleSelectionChanged(false, false);
        emit selectionChanged(QString::fromUtf8("Ничего не выбрано"));
        update();
        return;
      }

      // Keep one remaining object as the active property-panel object.
      if (!selectedElementIds_.empty()) {
        const auto elementId = selectedElementIds_.back();
        const auto found = std::find_if(
            sketch_.lines().begin(), sketch_.lines().end(),
            [elementId](const auto& line) { return line.elementId == elementId; });

        if (found != sketch_.lines().end()) {
          const auto index =
              static_cast<std::size_t>(
                  std::distance(sketch_.lines().begin(), found));
          selectionKind_ = SelectionKind::Line;
          selectionElementId_ = elementId;
          selectionLineId_ = sketch_.lineId(index);
          selectionCircleId_ = sketch::kInvalidGeometryId;
          hitDashed = found->dashed;
        }
      } else {
        selectionKind_ = SelectionKind::Circle;
        selectionCircleId_ = selectedCircleIds_.back();
        selectionLineId_ = sketch::kInvalidGeometryId;
        const auto index = sketch_.circleIndex(selectionCircleId_);
        hitDashed = index && sketch_.circles()[*index].dashed;
      }
    } else {
      selectionKind_ = hitKind;
      selectionElementId_ = hitElementId;
      selectionLineId_ = hitLineId;
      selectionCircleId_ = hitCircleId;
    }
  } else {
    if (!(preserveExistingIfHit && alreadySelected)) {
      clearGeometrySelection();

      if (hitKind == SelectionKind::Line)
        selectedElementIds_.push_back(hitElementId);
      else
        selectedCircleIds_.push_back(hitCircleId);
    }

    selectionKind_ = hitKind;
    selectionElementId_ = hitElementId;
    selectionLineId_ = hitLineId;
    selectionCircleId_ = hitCircleId;
  }

  const std::size_t selectedCount =
      selectedElementIds_.size() + selectedCircleIds_.size();

  emit selectionChanged(
      selectedCount > 1
          ? QString::fromUtf8("Выбрано объектов: %1").arg(selectedCount)
          : hitKind == SelectionKind::Line
                ? QString::fromUtf8("Объект: линия")
                : QString::fromUtf8("Объект: окружность"));

  emit lineStyleSelectionChanged(selectedCount > 0, hitDashed);
  update();
}

void SketchCanvas::selectInRect(const QRectF& rect, bool additive) {
  if (!additive) clearGeometrySelection();

  const auto addElement = [this](std::size_t elementId) {
    if (!lineElementSelected(elementId))
      selectedElementIds_.push_back(elementId);
  };

  const auto lineTouchesRect = [](QPointF first, QPointF second,
                                  const QRectF& rect) {
    if (rect.contains(first) || rect.contains(second)) return true;

    const QLineF segment(first, second);
    const QLineF top(rect.topLeft(), rect.topRight());
    const QLineF right(rect.topRight(), rect.bottomRight());
    const QLineF bottom(rect.bottomRight(), rect.bottomLeft());
    const QLineF left(rect.bottomLeft(), rect.topLeft());

    QPointF intersection;
    return segment.intersects(top, &intersection) == QLineF::BoundedIntersection ||
           segment.intersects(right, &intersection) == QLineF::BoundedIntersection ||
           segment.intersects(bottom, &intersection) == QLineF::BoundedIntersection ||
           segment.intersects(left, &intersection) == QLineF::BoundedIntersection;
  };

  for (std::size_t index = 0; index < sketch_.lines().size(); ++index) {
    const auto& line = sketch_.lines()[index];
    if (lineTouchesRect(mapPoint(line.start), mapPoint(line.end), rect))
      addElement(line.elementId);
  }

  for (std::size_t index = 0; index < sketch_.circles().size(); ++index) {
    const auto& circle = sketch_.circles()[index];
    const auto id = sketch_.circleId(index);
    const QPointF center = mapPoint(circle.center);
    const double radius = circle.radiusMm * pixelsPerMm_;
    const QRectF bounds(center.x() - radius, center.y() - radius,
                        radius * 2.0, radius * 2.0);

    if (bounds.intersects(rect) || rect.contains(center)) {
      if (!circleSelected(id))
        selectedCircleIds_.push_back(id);
    }
  }

  const std::size_t selectedCount =
      selectedElementIds_.size() + selectedCircleIds_.size();

  if (!selectedElementIds_.empty()) {
    const auto elementId = selectedElementIds_.back();
    const auto found = std::find_if(
        sketch_.lines().begin(), sketch_.lines().end(),
        [elementId](const auto& line) { return line.elementId == elementId; });

    if (found != sketch_.lines().end()) {
      const auto index = static_cast<std::size_t>(
          std::distance(sketch_.lines().begin(), found));
      selectionKind_ = SelectionKind::Line;
      selectionElementId_ = elementId;
      selectionLineId_ = sketch_.lineId(index);
      selectionCircleId_ = sketch::kInvalidGeometryId;
      emit lineStyleSelectionChanged(true, found->dashed);
    }
  } else if (!selectedCircleIds_.empty()) {
    selectionKind_ = SelectionKind::Circle;
    selectionCircleId_ = selectedCircleIds_.back();
    selectionLineId_ = sketch::kInvalidGeometryId;
    const auto index = sketch_.circleIndex(selectionCircleId_);
    emit lineStyleSelectionChanged(
        true, index && sketch_.circles()[*index].dashed);
  } else {
    clearGeometrySelection();
    emit lineStyleSelectionChanged(false, false);
  }

  emit selectionChanged(
      selectedCount > 0
          ? QString::fromUtf8("Выбрано объектов: %1").arg(selectedCount)
          : QString::fromUtf8("Ничего не выбрано"));

  update();
}
namespace {

// Add persistent Coincident constraints from newly created reference points
// to CAD points that existed before the creation operation.
//
// Current PointReference supports line endpoints and circle centers, so this
// automatically covers line endpoints, rectangle vertices and circle centers.
void autoCoincidentNewGeometry(
    sketch::Sketch& sketch,
    std::size_t oldLineCount,
    std::size_t oldCircleCount,
    double toleranceMm) {
  struct ReferenceCandidate {
    sketch::PointReference reference;
    sketch::Point initialPoint;
  };

  const auto sameReference =
      [](sketch::PointReference first,
         sketch::PointReference second) {
        if (first.elementCenterId != 0 ||
            second.elementCenterId != 0) {
          return first.elementCenterId != 0 &&
                 first.elementCenterId ==
                     second.elementCenterId;
        }

        if (first.circleId != sketch::kInvalidGeometryId ||
            second.circleId != sketch::kInvalidGeometryId) {
          return first.circleId != sketch::kInvalidGeometryId &&
                 first.circleId == second.circleId;
        }

        return first.lineId == second.lineId &&
               first.start == second.start;
      };

  std::vector<sketch::PointReference> oldReferences;

  const auto addOldReference =
      [&oldReferences, &sameReference](
          sketch::PointReference reference) {
        const bool duplicate =
            std::any_of(
                oldReferences.begin(),
                oldReferences.end(),
                [reference, &sameReference](
                    sketch::PointReference existing) {
                  return sameReference(existing, reference);
                });

        if (!duplicate)
          oldReferences.push_back(reference);
      };

  for (std::size_t index = 0;
       index < std::min(oldLineCount, sketch.lines().size());
       ++index) {
    const auto id = sketch.lineId(index);
    if (id == sketch::kInvalidGeometryId)
      continue;

    addOldReference(sketch::PointReference{id, true});
    addOldReference(sketch::PointReference{id, false});
  }

  for (std::size_t index = 0;
       index < std::min(oldCircleCount, sketch.circles().size());
       ++index) {
    const auto id = sketch.circleId(index);
    if (id == sketch::kInvalidGeometryId)
      continue;

    sketch::PointReference center;
    center.circleId = id;
    addOldReference(center);
  }

  // Existing virtual centers of composite elements are CAD points too.
  // Only centers whose element already existed before this creation pass
  // belong in oldReferences.
  for (const auto elementId : sketch.centerNodeElementIds()) {
    bool belongsToOldGeometry = false;

    for (std::size_t index = 0;
         index < std::min(oldLineCount, sketch.lines().size());
         ++index) {
      if (sketch.lines()[index].elementId == elementId) {
        belongsToOldGeometry = true;
        break;
      }
    }

    if (!belongsToOldGeometry)
      continue;

    sketch::PointReference center;
    center.elementCenterId = elementId;
    addOldReference(center);
  }

  if (oldReferences.empty())
    return;

  std::vector<ReferenceCandidate> newReferences;

  const auto addNewReference =
      [&sketch, &newReferences](
          sketch::PointReference reference) {
        const auto point =
            sketch.referencedPoint(reference);

        if (!point)
          return;

        // Rectangle corners may be represented by two coincident
        // primitive endpoints. One external constraint per geometric
        // point is enough.
        const bool duplicatePoint =
            std::any_of(
                newReferences.begin(),
                newReferences.end(),
                [&point](const ReferenceCandidate& item) {
                  return std::hypot(
                             item.initialPoint.xMm -
                                 point->xMm,
                             item.initialPoint.yMm -
                                 point->yMm) <= 1e-7;
                });

        if (!duplicatePoint)
          newReferences.push_back({reference, *point});
      };

  for (std::size_t index = oldLineCount;
       index < sketch.lines().size();
       ++index) {
    const auto id = sketch.lineId(index);

    if (id == sketch::kInvalidGeometryId)
      continue;

    addNewReference(
        sketch::PointReference{id, true});
    addNewReference(
        sketch::PointReference{id, false});
  }

  for (std::size_t index = oldCircleCount;
       index < sketch.circles().size();
       ++index) {
    const auto id = sketch.circleId(index);

    if (id == sketch::kInvalidGeometryId)
      continue;

    sketch::PointReference center;
    center.circleId = id;
    addNewReference(center);
  }

  // Newly created center-based rectangles expose a virtual center node.
  // Add it as a new reference so a rectangle created from an existing
  // CAD point receives a real Coincident constraint at its center.
  for (const auto elementId : sketch.centerNodeElementIds()) {
    bool belongsToNewGeometry = false;

    for (std::size_t index = oldLineCount;
         index < sketch.lines().size();
         ++index) {
      if (sketch.lines()[index].elementId == elementId) {
        belongsToNewGeometry = true;
        break;
      }
    }

    if (!belongsToNewGeometry)
      continue;

    sketch::PointReference center;
    center.elementCenterId = elementId;
    addNewReference(center);
  }

  const auto constraintExists =
      [&sketch, &sameReference](
          sketch::PointReference first,
          sketch::PointReference second) {
        return std::any_of(
            sketch.constraints().begin(),
            sketch.constraints().end(),
            [first, second, &sameReference](
                const sketch::Constraint& constraint) {
              if (constraint.type !=
                  sketch::ConstraintType::Coincident)
                return false;

              return
                  (sameReference(
                       constraint.firstPoint, first) &&
                   sameReference(
                       constraint.secondPoint, second)) ||
                  (sameReference(
                       constraint.firstPoint, second) &&
                   sameReference(
                       constraint.secondPoint, first));
            });
      };

  for (const auto& candidate : newReferences) {
    const auto currentPoint =
        sketch.referencedPoint(candidate.reference);

    if (!currentPoint)
      continue;

    std::optional<sketch::PointReference> nearest;
    double bestDistance = toleranceMm;

    for (const auto oldReference : oldReferences) {
      const auto oldPoint =
          sketch.referencedPoint(oldReference);

      if (!oldPoint)
        continue;

      const double distance =
          std::hypot(
              currentPoint->xMm - oldPoint->xMm,
              currentPoint->yMm - oldPoint->yMm);

      if (distance < bestDistance) {
        bestDistance = distance;
        nearest = oldReference;
      }
    }

    if (!nearest ||
        sameReference(
            *nearest, candidate.reference) ||
        constraintExists(
            *nearest, candidate.reference))
      continue;

    sketch::Constraint constraint;
    constraint.type =
        sketch::ConstraintType::Coincident;

    // Existing geometry is the reference;
    // newly created geometry moves to it.
    constraint.firstPoint = *nearest;
    constraint.secondPoint =
        candidate.reference;

    sketch.addConstraint(constraint);
  }
}

}  // namespace
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
  const std::size_t oldLineCount = sketch_.lines().size();
  const std::size_t oldCircleCount = sketch_.circles().size();

  if (tool_ == Tool::Line)
    pushUndoState();
  else if (tool_ == Tool::Rectangle)
    pushUndoState();
  else if (tool_ == Tool::Circle)
    pushUndoState();
  if (tool_ == Tool::Line) {
    // Automatic CAD coincidence:
    // if the new line starts on an existing line endpoint, remember that
    // endpoint before creating the new primitive and store a real persistent
    // Coincident constraint to the new line start.
    std::optional<sketch::PointReference> connectedEndpoint;
    double bestEndpointDistanceMm =
        8.0 / std::max(0.001, pixelsPerMm_);

    for (std::size_t index = 0; index < sketch_.lines().size(); ++index) {
      const auto id = sketch_.lineId(index);
      if (id == sketch::kInvalidGeometryId) continue;

      const auto& existingLine = sketch_.lines()[index];

      for (const bool start : {true, false}) {
        const auto existingPoint =
            start ? existingLine.start : existingLine.end;
        const double distanceMm =
            std::hypot(anchor_->xMm - existingPoint.xMm,
                       anchor_->yMm - existingPoint.yMm);

        if (distanceMm < bestEndpointDistanceMm) {
          bestEndpointDistanceMm = distanceMm;
          connectedEndpoint = sketch::PointReference{id, start};
        }
      }
    }

    // Snap the new line start exactly onto the detected CAD endpoint.
    sketch::Point lineStart = *anchor_;
    if (connectedEndpoint) {
      if (const auto endpointPoint =
              sketch_.referencedPoint(*connectedEndpoint))
        lineStart = *endpointPoint;
    }

    const std::size_t newLineIndex = sketch_.lines().size();
    sketch_.addLine(lineStart, point);

    if (connectedEndpoint &&
        newLineIndex < sketch_.lines().size()) {
      const auto newLineId = sketch_.lineId(newLineIndex);

      if (newLineId != sketch::kInvalidGeometryId) {
        sketch::Constraint constraint;
        constraint.type = sketch::ConstraintType::Coincident;
        constraint.firstPoint = *connectedEndpoint;
        constraint.secondPoint =
            sketch::PointReference{newLineId, true};
        sketch_.addConstraint(constraint);
      }
    }
  }
  else if (tool_ == Tool::Rectangle) {
    if (rectangleMode_ == RectangleMode::FromCenter) {
      const double dx = point.xMm - anchor_->xMm;
      const double dy = point.yMm - anchor_->yMm;

      const std::size_t firstNewLine = sketch_.lines().size();

      sketch_.addRectangle({anchor_->xMm-dx, anchor_->yMm-dy},
                           {anchor_->xMm+dx, anchor_->yMm+dy});

      // addRectangle() creates four lines with one shared elementId.
      // Register that element as having a virtual CAD center node.
      if (firstNewLine < sketch_.lines().size())
        sketch_.markElementCenterNode(
            sketch_.lines()[firstNewLine].elementId);
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
  autoCoincidentNewGeometry(
      sketch_,
      oldLineCount,
      oldCircleCount,
      8.0 / std::max(0.001, pixelsPerMm_));

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

      const std::size_t oldLineCount = sketch_.lines().size();
      const std::size_t oldCircleCount = sketch_.circles().size();

      sketch_.addRectangle(first, second, third, fourth);

      autoCoincidentNewGeometry(
          sketch_,
          oldLineCount,
          oldCircleCount,
          8.0 / std::max(0.001, pixelsPerMm_));

      rectanglePoints_.clear();
      notifyGeometryChanged();
      return;
    }
  }
  rectanglePoints_.clear();
  update();
}

void SketchCanvas::commitCirclePoint(sketch::Point point) {
  constexpr double minRadiusMm = 1e-6;
  const double referenceToleranceMm =
      10.0 / std::max(0.001, pixelsPerMm_);

  // Resolve a picked construction point back to an existing CAD point.
  // This is used after the new circle is created to persist the relation
  // as PointOnCircle rather than merely keeping equal coordinates.
  const auto nearestExistingReference =
      [this, referenceToleranceMm](
          sketch::Point target)
          -> std::optional<sketch::PointReference> {
        double bestDistance =
            referenceToleranceMm;

        std::optional<sketch::PointReference> best;

        const auto consider =
            [this, target, &bestDistance, &best](
                sketch::PointReference reference) {
              const auto candidate =
                  sketch_.referencedPoint(reference);

              if (!candidate)
                return;

              const double distance =
                  std::hypot(
                      candidate->xMm - target.xMm,
                      candidate->yMm - target.yMm);

              if (distance < bestDistance) {
                bestDistance = distance;
                best = reference;
              }
            };

        for (std::size_t index = 0;
             index < sketch_.lines().size();
             ++index) {
          const auto id = sketch_.lineId(index);

          if (id == sketch::kInvalidGeometryId)
            continue;

          consider(
              sketch::PointReference{id, true});
          consider(
              sketch::PointReference{id, false});
        }

        for (std::size_t index = 0;
             index < sketch_.circles().size();
             ++index) {
          const auto id =
              sketch_.circleId(index);

          if (id == sketch::kInvalidGeometryId)
            continue;

          sketch::PointReference center;
          center.circleId = id;
          consider(center);
        }

        for (const auto elementId :
             sketch_.centerNodeElementIds()) {
          sketch::PointReference center;
          center.elementCenterId = elementId;
          consider(center);
        }

        return best;
      };

  const auto sameReference =
      [](sketch::PointReference first,
         sketch::PointReference second) {
        if (first.elementCenterId != 0 ||
            second.elementCenterId != 0) {
          return first.elementCenterId != 0 &&
                 first.elementCenterId ==
                     second.elementCenterId;
        }

        if (first.circleId !=
                sketch::kInvalidGeometryId ||
            second.circleId !=
                sketch::kInvalidGeometryId) {
          return first.circleId !=
                     sketch::kInvalidGeometryId &&
                 first.circleId ==
                     second.circleId;
        }

        return first.lineId == second.lineId &&
               first.start == second.start;
      };

  const auto finishCircle =
      [this, minRadiusMm,
       &nearestExistingReference,
       &sameReference](
          sketch::Point center,
          double radius,
          const std::vector<sketch::Point>& definingPoints,
          const std::vector<sketch::GeometryId>& tangentLineIds) {
        if (!std::isfinite(radius) ||
            radius < minRadiusMm)
          return false;

        // Capture references BEFORE creating the new circle so its own
        // centre can never be mistaken for one of the source points.
        std::vector<sketch::PointReference>
            sourceReferences;

        for (const auto definingPoint :
             definingPoints) {
          const auto reference =
              nearestExistingReference(
                  definingPoint);

          if (!reference)
            continue;

          const bool duplicate =
              std::any_of(
                  sourceReferences.begin(),
                  sourceReferences.end(),
                  [&sameReference, &reference](
                      sketch::PointReference existing) {
                    return sameReference(
                        existing, *reference);
                  });

          if (!duplicate)
            sourceReferences.push_back(*reference);
        }

        pushUndoState();

        const std::size_t oldLineCount =
            sketch_.lines().size();
        const std::size_t oldCircleCount =
            sketch_.circles().size();

        sketch_.addCircle(center, radius);

        if (sketch_.circles().size() <=
            oldCircleCount) {
          return false;
        }

        const auto newCircleId =
            sketch_.circleId(oldCircleCount);

        // Center-based automatic coincidence is still useful if the
        // newly computed centre itself happens to be an existing CAD point.
        autoCoincidentNewGeometry(
            sketch_,
            oldLineCount,
            oldCircleCount,
            8.0 /
                std::max(0.001, pixelsPerMm_));

        // Two-point and three-point circles are defined by points on
        // their circumference. Persist those relations explicitly.
        if (newCircleId !=
            sketch::kInvalidGeometryId) {
          for (const auto source :
               sourceReferences) {
            bool duplicate = false;

            for (const auto& existing :
                 sketch_.constraints()) {
              if (existing.type !=
                  sketch::ConstraintType::PointOnCircle)
                continue;

              if (existing.firstGeometry !=
                  newCircleId)
                continue;

              if (sameReference(
                      existing.secondPoint,
                      source)) {
                duplicate = true;
                break;
              }
            }

            if (duplicate)
              continue;

            sketch::Constraint constraint;
            constraint.type =
                sketch::ConstraintType::PointOnCircle;
            constraint.firstGeometry =
                newCircleId;
            constraint.secondPoint = source;

            sketch_.addConstraint(constraint);
          }
        }
        // AUTO TANGENT CONSTRAINTS
        if (newCircleId != sketch::kInvalidGeometryId) {
          for (const auto lineId : tangentLineIds) {
            if (lineId == sketch::kInvalidGeometryId ||
                !sketch_.lineIndex(lineId))
              continue;

            bool duplicate = false;

            for (const auto& existing : sketch_.constraints()) {
              if (existing.type != sketch::ConstraintType::Tangent)
                continue;

              if (existing.firstGeometry == lineId &&
                  existing.secondGeometry == newCircleId) {
                duplicate = true;
                break;
              }
            }

            if (duplicate)
              continue;

            sketch::Constraint tangent;
            tangent.type = sketch::ConstraintType::Tangent;
            tangent.firstGeometry = lineId;
            tangent.secondGeometry = newCircleId;
            sketch_.addConstraint(tangent);
          }
        }

        circleDiameterMm_ = 2.0 * radius;
        emit primaryDimensionChanged(
            circleDiameterMm_);

        circlePoints_.clear();
        circleGuideLines_.clear();

        notifyGeometryChanged();
        update();
        return true;
      };

  if (circleMode_ ==
      CircleMode::TwoPoints) {
    circlePoints_.push_back(point);

    if (circlePoints_.size() < 2) {
      hoverPoint_ = point;
      update();
      return;
    }

    const auto first = circlePoints_[0];
    const auto second = circlePoints_[1];

    const double diameter =
        std::hypot(
            second.xMm - first.xMm,
            second.yMm - first.yMm);

    if (diameter <= minRadiusMm * 2.0) {
      circlePoints_.clear();
      update();
      return;
    }

    const sketch::Point center{
        (first.xMm + second.xMm) * 0.5,
        (first.yMm + second.yMm) * 0.5};

    (void)finishCircle(
        center,
        diameter * 0.5,
        {first, second},
        {});

    return;
  }

  if (circleMode_ ==
      CircleMode::ThreePoints) {
    circlePoints_.push_back(point);

    if (circlePoints_.size() < 3) {
      hoverPoint_ = point;
      update();
      return;
    }

    const auto first = circlePoints_[0];
    const auto second = circlePoints_[1];
    const auto third = circlePoints_[2];

    const auto result =
        circleThroughThreePoints(
            first, second, third);

    if (!result) {
      circlePoints_.clear();
      update();
      return;
    }

    (void)finishCircle(
        result->first,
        result->second,
        {first, second, third},
        {});

    return;
  }

  const auto guideLineId =
      [this](const sketch::Line& guide)
          -> sketch::GeometryId {
        constexpr double epsilon = 1e-7;

        const auto samePoint =
            [epsilon](sketch::Point first,
                      sketch::Point second) {
              return std::hypot(
                         first.xMm - second.xMm,
                         first.yMm - second.yMm) <= epsilon;
            };

        for (std::size_t index = 0;
             index < sketch_.lines().size();
             ++index) {
          const auto& candidate = sketch_.lines()[index];

          if (candidate.elementId != guide.elementId)
            continue;

          const bool sameOrder =
              samePoint(candidate.start, guide.start) &&
              samePoint(candidate.end, guide.end);

          const bool reverseOrder =
              samePoint(candidate.start, guide.end) &&
              samePoint(candidate.end, guide.start);

          if (sameOrder || reverseOrder)
            return sketch_.lineId(index);
        }

        return sketch::kInvalidGeometryId;
      };
  const auto nearestLine =
      [this](sketch::Point target)
          -> std::optional<sketch::Line> {
        double best =
            std::numeric_limits<double>::max();

        std::optional<sketch::Line> result;

        for (const auto& line :
             sketch_.lines()) {
          if (line.dashed)
            continue;

          const double distance =
              pointLineDistance(target, line);

          if (distance < best) {
            best = distance;
            result = line;
          }
        }

        return
            best <=
                    std::max(
                        3.0,
                        12.0 / pixelsPerMm_)
                ? result
                : std::nullopt;
      };

  if ((circleMode_ ==
           CircleMode::ThreeTangents &&
       circleGuideLines_.size() < 3) ||
      (circleMode_ ==
           CircleMode::TwoTangentsRadius &&
       circleGuideLines_.size() < 2)) {
    const auto line =
        nearestLine(point);

    if (!line)
      return;

    const bool duplicate =
        std::any_of(
            circleGuideLines_.begin(),
            circleGuideLines_.end(),
            [&](const auto& existing) {
              return existing.elementId ==
                     line->elementId;
            });

    if (!duplicate)
      circleGuideLines_.push_back(*line);
    update();

    if (circleMode_ ==
        CircleMode::TwoTangentsRadius) {
      if (circleGuideLines_.size() == 2) {
        // TWO-TANGENT LIVE PREVIEW START
        setProperty(
            "twoTangentRadiusPreviewActive",
            true);

        primaryDimension_->setPrefix(
            QString::fromUtf8("Ø "));
        primaryDimension_->setSuffix(
            QString::fromUtf8(" мм"));
        primaryDimension_->setRange(
            0.02, 100000.0);
        primaryDimension_->setDecimals(2);
        primaryDimension_->setValue(
            std::max(0.02, circleDiameterMm_));

        const auto intersection =
            intersectLines(
                circleGuideLines_[0],
                circleGuideLines_[1]);

        const QPointF editorPoint =
            intersection
                ? mapPoint(*intersection)
                : mapPoint(point);

        primaryDimension_->move(
            (editorPoint +
             QPointF(18.0, 18.0)).toPoint());
        primaryDimension_->show();
        primaryDimension_->raise();
        primaryDimension_->clearFocus();

        emit selectionChanged(
            QString::fromUtf8(
                "Перемещайте мышь для изменения диаметра, "
                "щелкните для подтверждения или введите Ø и нажмите Enter"));
      }

      return;
    }
  }

  if (circleMode_ ==
          CircleMode::ThreeTangents &&
      circleGuideLines_.size() == 3) {
    // THREE TANGENTS FINITE-SEGMENT SOLUTION V2
    //
    // Three infinite lines have up to four tangent circles:
    // one incircle and three excircles. Select the solution that best
    // corresponds to the finite segments clicked by the user.

    const auto p0 =
        intersectLines(
            circleGuideLines_[0],
            circleGuideLines_[1]);

    const auto p1 =
        intersectLines(
            circleGuideLines_[1],
            circleGuideLines_[2]);

    const auto p2 =
        intersectLines(
            circleGuideLines_[2],
            circleGuideLines_[0]);

    if (!p0 || !p1 || !p2) {
      circleGuideLines_.clear();
      update();
      return;
    }

    const double a =
        std::hypot(
            p1->xMm - p2->xMm,
            p1->yMm - p2->yMm);

    const double b =
        std::hypot(
            p0->xMm - p2->xMm,
            p0->yMm - p2->yMm);

    const double c =
        std::hypot(
            p0->xMm - p1->xMm,
            p0->yMm - p1->yMm);

    const auto weightedCenter =
        [](sketch::Point first,
           sketch::Point second,
           sketch::Point third,
           double wa,
           double wb,
           double wc)
            -> std::optional<sketch::Point> {
          const double denominator = wa + wb + wc;

          if (std::abs(denominator) <= 1e-9)
            return std::nullopt;

          const sketch::Point center{
              (wa * first.xMm +
               wb * second.xMm +
               wc * third.xMm) /
                  denominator,
              (wa * first.yMm +
               wb * second.yMm +
               wc * third.yMm) /
                  denominator};

          if (!std::isfinite(center.xMm) ||
              !std::isfinite(center.yMm))
            return std::nullopt;

          return center;
        };

    std::vector<sketch::Point> candidates;

    const auto addCandidate =
        [&candidates](std::optional<sketch::Point> center) {
          if (!center)
            return;

          const bool duplicate =
              std::any_of(
                  candidates.begin(),
                  candidates.end(),
                  [&center](sketch::Point existing) {
                    return std::hypot(
                               existing.xMm - center->xMm,
                               existing.yMm - center->yMm) <= 1e-7;
                  });

          if (!duplicate)
            candidates.push_back(*center);
        };

    // Incenter.
    addCandidate(
        weightedCenter(
            *p0, *p1, *p2,
            a, b, c));

    // Three excenters.
    addCandidate(
        weightedCenter(
            *p0, *p1, *p2,
            -a, b, c));

    addCandidate(
        weightedCenter(
            *p0, *p1, *p2,
            a, -b, c));

    addCandidate(
        weightedCenter(
            *p0, *p1, *p2,
            a, b, -c));

    const auto tangentFootPenalty =
        [](sketch::Point center,
           const sketch::Line& line) {
          const double dx =
              line.end.xMm - line.start.xMm;
          const double dy =
              line.end.yMm - line.start.yMm;

          const double lengthSquared =
              dx * dx + dy * dy;

          if (lengthSquared <= 1e-12)
            return 1e12;

          const double t =
              ((center.xMm - line.start.xMm) * dx +
               (center.yMm - line.start.yMm) * dy) /
              lengthSquared;

          if (t >= 0.0 && t <= 1.0)
            return 0.0;

          const double segmentLength =
              std::sqrt(lengthSquared);

          const double outside =
              t < 0.0 ? -t : t - 1.0;

          return outside * outside *
                 segmentLength * segmentLength;
        };

    const auto endpointProximityScore =
        [](sketch::Point center,
           const sketch::Line& line) {
          const double d0 =
              std::hypot(
                  center.xMm - line.start.xMm,
                  center.yMm - line.start.yMm);

          const double d1 =
              std::hypot(
                  center.xMm - line.end.xMm,
                  center.yMm - line.end.yMm);

          return std::min(d0, d1);
        };

    std::optional<sketch::Point> bestCenter;
    double bestScore =
        std::numeric_limits<double>::max();

    for (const auto candidate : candidates) {
      const double r0 =
          infiniteLineDistance(
              candidate,
              circleGuideLines_[0]);

      const double r1 =
          infiniteLineDistance(
              candidate,
              circleGuideLines_[1]);

      const double r2 =
          infiniteLineDistance(
              candidate,
              circleGuideLines_[2]);

      if (!std::isfinite(r0) ||
          !std::isfinite(r1) ||
          !std::isfinite(r2))
        continue;

      const double radius =
          (r0 + r1 + r2) / 3.0;

      if (radius <= 1e-9)
        continue;

      const double residual =
          std::abs(r0 - radius) +
          std::abs(r1 - radius) +
          std::abs(r2 - radius);

      const double finitePenalty =
          tangentFootPenalty(
              candidate,
              circleGuideLines_[0]) +
          tangentFootPenalty(
              candidate,
              circleGuideLines_[1]) +
          tangentFootPenalty(
              candidate,
              circleGuideLines_[2]);

      // If several solutions have tangent feet on all finite segments,
      // prefer the geometrically nearer one instead of a huge remote circle.
      const double proximity =
          endpointProximityScore(
              candidate,
              circleGuideLines_[0]) +
          endpointProximityScore(
              candidate,
              circleGuideLines_[1]) +
          endpointProximityScore(
              candidate,
              circleGuideLines_[2]);

      const double score =
          finitePenalty * 1000000.0 +
          residual * 1000.0 +
          proximity;

      if (score < bestScore) {
        bestScore = score;
        bestCenter = candidate;
      }
    }

    if (!bestCenter) {
      circleGuideLines_.clear();
      update();
      return;
    }

    const double radius =
        infiniteLineDistance(
            *bestCenter,
            circleGuideLines_[0]);

    (void)finishCircle(
        *bestCenter,
        radius,
        {},
        std::vector<sketch::GeometryId>{
            guideLineId(circleGuideLines_[0]),
            guideLineId(circleGuideLines_[1]),
            guideLineId(circleGuideLines_[2])});

    return;
  }

  if (circleMode_ ==
          CircleMode::TwoTangentsRadius &&
      circleGuideLines_.size() == 2) {
    circlePoints_.push_back(point);

    const double requestedDiameter =
        primaryDimension_->isVisible()
            ? primaryDimension_->value()
            : circleDiameterMm_;

    circleDiameterMm_ =
        std::max(0.02, requestedDiameter);

    const auto finiteRequestedCircle =
        clampedTwoTangentCircleForRadius(
            circleGuideLines_[0],
            circleGuideLines_[1],
            circleDiameterMm_ * 0.5,
            point);

    if (!finiteRequestedCircle) {
      emit selectionChanged(
          QString::fromUtf8(
              "Для выбранных отрезков невозможно построить "
              "касательную окружность этого диаметра"));
      update();
      return;
    }

    circleDiameterMm_ =
        finiteRequestedCircle->radiusMm * 2.0;

    {
      const QSignalBlocker blocker(
          primaryDimension_);
      primaryDimension_->setValue(
          circleDiameterMm_);
    }

    const double radius =
        finiteRequestedCircle->radiusMm;

    struct Equation {
      double nx;
      double ny;
      double c;
    };

    Equation equations[2];

    for (int i = 0; i < 2; ++i) {
      const auto& line =
          circleGuideLines_[i];

      const double dx =
          line.end.xMm -
          line.start.xMm;

      const double dy =
          line.end.yMm -
          line.start.yMm;

      const double length =
          std::hypot(dx, dy);

      if (length <= 1e-9) {
        circleGuideLines_.clear();
        circlePoints_.clear();
        update();
        return;
      }

      equations[i] = {
          -dy / length,
          dx / length,
          0.0};

      equations[i].c =
          equations[i].nx *
              line.start.xMm +
          equations[i].ny *
              line.start.yMm;
    }

    double best =
        std::numeric_limits<double>::max();

    std::optional<sketch::Point> center;

    const double det =
        equations[0].nx *
            equations[1].ny -
        equations[0].ny *
            equations[1].nx;

    if (std::abs(det) > 1e-9) {
      for (double s0 :
           {-1.0, 1.0}) {
        for (double s1 :
             {-1.0, 1.0}) {
          const double r0 =
              equations[0].c +
              s0 * radius;

          const double r1 =
              equations[1].c +
              s1 * radius;

          const sketch::Point candidate{
              (r0 * equations[1].ny -
               equations[0].ny * r1) /
                  det,
              (equations[0].nx * r1 -
               r0 * equations[1].nx) /
                  det};

          const double distance =
              std::hypot(
                  candidate.xMm -
                      point.xMm,
                  candidate.yMm -
                      point.yMm);

          if (distance < best) {
            best = distance;
            center = candidate;
          }
        }
      }
    }

    if (finiteRequestedCircle) {

      const std::size_t circlesBefore =
          sketch_.circles().size();

      const bool created =
          finishCircle(
              finiteRequestedCircle->center,
              radius,
              {},
              std::vector<sketch::GeometryId>{
                  guideLineId(circleGuideLines_[0]),
                  guideLineId(circleGuideLines_[1])});

      if (created &&
          sketch_.circles().size() >
              circlesBefore) {
        const std::size_t createdIndex =
            sketch_.circles().size() - 1;

        const auto createdCircleId =
            sketch_.circleId(createdIndex);

        if (createdCircleId !=
            sketch::kInvalidGeometryId) {
          // AUTO DRIVING DIAMETER AFTER TWO TANGENTS
          sketch::Dimension diameterDimension;
          diameterDimension.kind =
              sketch::DimensionKind::CircleDiameter;
          diameterDimension.geometryId =
              createdCircleId;
          diameterDimension.valueMm =
              circleDiameterMm_;
          diameterDimension.offsetMm = 0.0;
          diameterDimension.angleRad = 0.0;

          sketch_.storeDimension(
              diameterDimension);

          sketch::Constraint diameterConstraint;
          diameterConstraint.type =
              sketch::ConstraintType::Diameter;
          diameterConstraint.firstGeometry =
              createdCircleId;
          diameterConstraint.value =
              circleDiameterMm_;

          sketch_.addConstraint(
              diameterConstraint);

          notifyGeometryChanged();
        }
      }
    }

    setProperty(
        "twoTangentRadiusPreviewActive",
        false);
    hideDimensionEditor();
    circlePoints_.clear();
    update();
    return;
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
  if (selectedElementIds_.empty() && selectedCircleIds_.empty() &&
      selectionKind_ == SelectionKind::None)
    return;

  pushUndoState();

  if (!selectedElementIds_.empty() || !selectedCircleIds_.empty()) {
    for (const auto elementId : selectedElementIds_)
      sketch_.setElementDashed(elementId, dashed);
    for (const auto circleId : selectedCircleIds_)
      sketch_.setCircleDashedById(circleId, dashed);
  } else if (selectionKind_ == SelectionKind::Line) {
    sketch_.setElementDashed(selectionElementId_, dashed);
  } else if (selectionKind_ == SelectionKind::Circle) {
    sketch_.setCircleDashedById(selectionCircleId_, dashed);
  }

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

  const std::size_t oldLineCount = sketch_.lines().size();
  const std::size_t oldCircleCount = sketch_.circles().size();

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
  autoCoincidentNewGeometry(
      sketch_,
      oldLineCount,
      oldCircleCount,
      8.0 / std::max(0.001, pixelsPerMm_));

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
