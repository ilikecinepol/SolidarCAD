#include "drawing/EskdRenderer.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <GeomAbs_CurveType.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Elips.hxx>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QTransform>
#include <QList>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <vector>

namespace solidar::drawing {
namespace {

constexpr double kPageWidthMm = 210.0;
constexpr double kPageHeightMm = 297.0;
constexpr double kLeftMarginMm = 20.0;
constexpr double kOtherMarginMm = 5.0;
constexpr double kTitleWidthMm = 185.0;
constexpr double kTitleHeightMm = 55.0;

void line(QPainter& painter, double x1, double y1, double x2, double y2) {
  painter.drawLine(QPointF(x1, y1), QPointF(x2, y2));
}

void text(QPainter& painter, const QRectF& bounds, const QString& value,
          double heightMm, Qt::Alignment alignment = Qt::AlignCenter) {
  QFont font("GOST type B");
  font.setFamilies({"GOST type B", "ISOCPEUR", "Arial", "DejaVu Sans"});
  font.setPointSizeF(heightMm * 2.35);
  painter.setFont(font);
  painter.drawText(bounds, alignment | Qt::TextWordWrap, value);
}

void arrow(QPainter& painter, QPointF tip, bool pointsRight) {
  const double direction = pointsRight ? 1.0 : -1.0;
  QPolygonF shape;
  shape << tip << QPointF(tip.x() - direction * 3.0, tip.y() - 0.7)
        << QPointF(tip.x() - direction * 3.0, tip.y() + 0.7);
  painter.save();
  painter.setBrush(Qt::black);
  painter.drawPolygon(shape);
  painter.restore();
}

void horizontalDimension(QPainter& painter, double left, double right,
                         double objectY, double dimensionY, double value) {
  painter.setPen(QPen(Qt::black, 0.18));
  line(painter, left, objectY - 1.0, left, dimensionY - 2.0);
  line(painter, right, objectY - 1.0, right, dimensionY - 2.0);
  line(painter, left - 2.0, dimensionY, right + 2.0, dimensionY);
  arrow(painter, {left, dimensionY}, true);
  arrow(painter, {right, dimensionY}, false);
  text(painter, {left, dimensionY - 5.0, right - left, 4.0},
       QString::number(value, 'f', value == static_cast<int>(value) ? 0 : 2),
       3.5);
}

void verticalDimension(QPainter& painter, double bottom, double top,
                       double objectX, double dimensionX, double value) {
  painter.setPen(QPen(Qt::black, 0.18));
  line(painter, objectX + 1.0, bottom, dimensionX + 2.0, bottom);
  line(painter, objectX + 1.0, top, dimensionX + 2.0, top);
  line(painter, dimensionX, bottom + 2.0, dimensionX, top - 2.0);
  painter.save();
  painter.translate(dimensionX - 1.0, (bottom + top) * 0.5);
  painter.rotate(-90.0);
  text(painter, {-12.0, -4.5, 24.0, 4.0},
       QString::number(value, 'f', value == static_cast<int>(value) ? 0 : 2),
       3.5);
  painter.restore();
  QPolygonF bottomArrow{{dimensionX, bottom}, {dimensionX - 0.7, bottom + 3.0},
                        {dimensionX + 0.7, bottom + 3.0}};
  QPolygonF topArrow{{dimensionX, top}, {dimensionX - 0.7, top - 3.0},
                     {dimensionX + 0.7, top - 3.0}};
  painter.save();
  painter.setBrush(Qt::black);
  painter.drawPolygon(bottomArrow);
  painter.drawPolygon(topArrow);
  painter.restore();
}

void titleBlock(QPainter& painter, const TitleBlockData& data) {
  const double left = kPageWidthMm - kOtherMarginMm - kTitleWidthMm;
  const double top = kPageHeightMm - kOtherMarginMm - kTitleHeightMm;
  painter.setPen(QPen(Qt::black, 0.5));
  painter.drawRect(QRectF(left, top, kTitleWidthMm, kTitleHeightMm));
  painter.setPen(QPen(Qt::black, 0.18));

  line(painter, left, top + 15, left + 185, top + 15);
  line(painter, left, top + 30, left + 185, top + 30);
  line(painter, left, top + 40, left + 185, top + 40);
  line(painter, left, top + 45, left + 185, top + 45);
  line(painter, left + 65, top, left + 65, top + 55);
  line(painter, left + 135, top, left + 135, top + 55);
  line(painter, left + 150, top + 30, left + 150, top + 55);
  line(painter, left + 165, top + 30, left + 165, top + 55);
  line(painter, left + 7, top + 30, left + 7, top + 55);
  line(painter, left + 24, top + 30, left + 24, top + 55);
  line(painter, left + 47, top + 30, left + 47, top + 55);

  text(painter, {left + 65, top, 70, 15}, data.productName, 5.0);
  text(painter, {left + 135, top, 50, 30}, data.designation, 4.0);
  text(painter, {left + 65, top + 15, 70, 15}, data.material, 3.5);
  text(painter, {left + 65, top + 30, 70, 25}, data.organization, 3.5);
  text(painter, {left + 135, top + 30, 15, 10}, "Лит.", 2.5);
  text(painter, {left + 150, top + 30, 15, 10}, "Масса", 2.5);
  text(painter, {left + 165, top + 30, 20, 10}, "Масштаб", 2.5);
  text(painter, {left + 165, top + 40, 20, 15}, data.scale, 3.5);

  const QStringList roles{"Изм.", "Лист", "№ докум.", "Подп.", "Дата"};
  const QList<QRectF> roleCells{{left, top + 30, 7, 10}, {left + 7, top + 30, 17, 10},
                                {left + 24, top + 30, 23, 10}, {left + 47, top + 30, 18, 10},
                                {left, top + 40, 65, 5}};
  for (qsizetype i = 0; i < roles.size(); ++i)
    text(painter, roleCells[i], roles[i], 2.5);
  text(painter, {left, top + 45, 24, 5}, "Разраб.", 2.5, Qt::AlignLeft | Qt::AlignVCenter);
  text(painter, {left, top + 50, 24, 5}, "Пров.", 2.5, Qt::AlignLeft | Qt::AlignVCenter);
}

struct ProjectedEdge final {
  QPainterPath path;
  QRectF bounds;
};

std::vector<ProjectedEdge> projectEdges(const TopoDS_Shape& shape) {
  std::vector<ProjectedEdge> result;
  for (TopExp_Explorer explorer(shape, TopAbs_EDGE); explorer.More();
       explorer.Next()) {
    const TopoDS_Edge edgeShape = TopoDS::Edge(explorer.Current());
    if (BRep_Tool::Degenerated(edgeShape)) continue;
    try {
      BRepAdaptor_Curve curve(edgeShape);
      const double first = curve.FirstParameter();
      const double last = curve.LastParameter();
      if (!std::isfinite(first) || !std::isfinite(last) || last < first)
        continue;

      QPainterPath path;
      if (curve.GetType() == GeomAbs_Line) {
        const gp_Pnt start = curve.Value(first);
        const gp_Pnt end = curve.Value(last);
        path.moveTo(start.X(), -start.Y());
        path.lineTo(end.X(), -end.Y());
      } else if (curve.GetType() == GeomAbs_Circle) {
        // Preserve analytic circles/arcs. A 3D circle projected onto XY is an
        // affine ellipse, which QPainter represents exactly under a transform.
        const gp_Circ circle = curve.Circle();
        const gp_Ax2 axes = circle.Position();
        const gp_Dir x = axes.XDirection();
        const gp_Dir y = axes.YDirection();
        const gp_Pnt center = circle.Location();
        const double radius = circle.Radius();
        QPainterPath local;
        const QRectF bounds(-radius, -radius, 2.0 * radius, 2.0 * radius);
        const double startDegrees = -first * 180.0 / std::numbers::pi;
        const double sweepDegrees = -(last - first) * 180.0 / std::numbers::pi;
        local.arcMoveTo(bounds, startDegrees);
        local.arcTo(bounds, startDegrees, sweepDegrees);
        const QTransform projection(x.X(), -x.Y(), y.X(), -y.Y(),
                                    center.X(), -center.Y());
        path = projection.map(local);
      } else if (curve.GetType() == GeomAbs_Ellipse) {
        const gp_Elips ellipse = curve.Ellipse();
        const gp_Ax2 axes = ellipse.Position();
        const gp_Dir x = axes.XDirection();
        const gp_Dir y = axes.YDirection();
        const gp_Pnt center = ellipse.Location();
        QPainterPath local;
        const QRectF bounds(-ellipse.MajorRadius(), -ellipse.MinorRadius(),
                            2.0 * ellipse.MajorRadius(),
                            2.0 * ellipse.MinorRadius());
        const double startDegrees = -first * 180.0 / std::numbers::pi;
        const double sweepDegrees = -(last - first) * 180.0 / std::numbers::pi;
        local.arcMoveTo(bounds, startDegrees);
        local.arcTo(bounds, startDegrees, sweepDegrees);
        const QTransform projection(x.X(), -x.Y(), y.X(), -y.Y(),
                                    center.X(), -center.Y());
        path = projection.map(local);
      } else {
        // General spline/conic display is presentation-only. Use a bounded
        // adaptive-looking tessellation here; analytic line/circle/ellipse
        // paths above are never degraded to polylines.
        constexpr int kSamples = 65;
        for (int index = 0; index < kSamples; ++index) {
          const double t = first + (last - first) * index / (kSamples - 1);
          const gp_Pnt point = curve.Value(t);
          if (index == 0)
            path.moveTo(point.X(), -point.Y());
          else
            path.lineTo(point.X(), -point.Y());
        }
      }
      if (!path.isEmpty()) result.push_back({path, path.boundingRect()});
    } catch (const Standard_Failure&) {
      // Some otherwise valid B-Reps contain edges without an evaluable 3D
      // curve. A drawing preview must skip them, never unwind through paintEvent.
      continue;
    }
  }
  return result;
}

struct ProjectedDrawingBounds {
  QRectF pageBounds;
  double modelWidth = 0.0;
  double modelHeight = 0.0;
};

std::optional<ProjectedDrawingBounds> drawProjectedShape(
    QPainter& painter, const QRectF& area, const TopoDS_Shape* sourceShape) {
  if (!sourceShape || sourceShape->IsNull()) return std::nullopt;
  const auto edges = projectEdges(*sourceShape);
  if (edges.empty()) return std::nullopt;

  double minX = std::numeric_limits<double>::max();
  double minY = std::numeric_limits<double>::max();
  double maxX = std::numeric_limits<double>::lowest();
  double maxY = std::numeric_limits<double>::lowest();
  for (const auto& edge : edges) {
    minX = std::min(minX, edge.bounds.left());
    minY = std::min(minY, edge.bounds.top());
    maxX = std::max(maxX, edge.bounds.right());
    maxY = std::max(maxY, edge.bounds.bottom());
  }
  const double width = std::max(maxX - minX, 1e-6);
  const double height = std::max(maxY - minY, 1e-6);
  const double scale = std::min(area.width() / width, area.height() / height);
  const QPointF modelCenter((minX + maxX) * 0.5, (minY + maxY) * 0.5);
  const QPointF pageCenter = area.center();

  painter.setPen(QPen(Qt::black, 0.5));
  QTransform fit;
  fit.translate(pageCenter.x(), pageCenter.y());
  fit.scale(scale, scale);
  fit.translate(-modelCenter.x(), -modelCenter.y());
  for (const auto& edge : edges) painter.drawPath(fit.map(edge.path));
  return ProjectedDrawingBounds{
      fit.mapRect(QRectF(QPointF(minX, minY), QPointF(maxX, maxY))),
      width, height};
}

}  // namespace

void EskdRenderer::renderA4(QPainter& painter, const QRectF& target,
                            const sketch::Sketch& sketch,
                            const TitleBlockData& title,
                            const TopoDS_Shape* sourceShape) {
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing);
  painter.fillRect(target, Qt::white);
  painter.translate(target.topLeft());
  painter.scale(target.width() / kPageWidthMm, target.height() / kPageHeightMm);
  painter.setBrush(Qt::NoBrush);

  painter.setPen(QPen(Qt::black, 0.5));
  painter.drawRect(QRectF(kLeftMarginMm, kOtherMarginMm,
                          kPageWidthMm - kLeftMarginMm - kOtherMarginMm,
                          kPageHeightMm - 2.0 * kOtherMarginMm));
  titleBlock(painter, title);

  const QRectF drawingArea(kLeftMarginMm + 15.0, kOtherMarginMm + 20.0,
                           150.0, 190.0);
  // Reserve room for dimensions around the fitted model. When a model source
  // exists it is the single source of both geometry and overall dimensions;
  // drawing the support sketch as well would duplicate and misalign outlines.
  const QRectF geometryArea = drawingArea.adjusted(10.0, 10.0, -20.0, -20.0);
  const auto projection =
      drawProjectedShape(painter, geometryArea, sourceShape);
  if (projection) {
    const QRectF bounds = projection->pageBounds.normalized();
    if (projection->modelWidth > 1e-6)
      horizontalDimension(painter, bounds.left(), bounds.right(),
                          bounds.bottom(), bounds.bottom() + 12.0,
                          projection->modelWidth);
    if (projection->modelHeight > 1e-6)
      verticalDimension(painter, bounds.bottom(), bounds.top(), bounds.right(),
                        bounds.right() + 12.0, projection->modelHeight);
  } else {
    const double scale =
        std::min(drawingArea.width() / (sketch.widthMm() + 30.0),
                 drawingArea.height() / (sketch.heightMm() + 30.0));
    const QPointF center = drawingArea.center();
    auto map = [&](sketch::Point point) {
      return QPointF(center.x() + point.xMm * scale,
                     center.y() - point.yMm * scale);
    };

    for (const auto& segment : sketch.lines()) {
      painter.setPen(QPen(Qt::black, 0.7,
                          segment.dashed ? Qt::DashLine : Qt::SolidLine));
      painter.drawLine(map(segment.start), map(segment.end));
    }
    painter.setPen(QPen(Qt::black, 0.7));
    for (const auto& circle : sketch.circles()) {
      painter.setPen(QPen(Qt::black, 0.7,
                          circle.dashed ? Qt::DashLine : Qt::SolidLine));
      const QPointF circleCenter = map(circle.center);
      const double radius = circle.radiusMm * scale;
      painter.drawEllipse(circleCenter, radius, radius);
    }

    const double left = center.x() - sketch.widthMm() * scale * 0.5;
    const double right = center.x() + sketch.widthMm() * scale * 0.5;
    const double top = center.y() - sketch.heightMm() * scale * 0.5;
    const double bottom = center.y() + sketch.heightMm() * scale * 0.5;
    horizontalDimension(painter, left, right, bottom, bottom + 12.0,
                        sketch.widthMm());
    verticalDimension(painter, bottom, top, right, right + 12.0,
                      sketch.heightMm());
  }
  painter.restore();
}

}  // namespace solidar::drawing
