#include "drawing/EskdRenderer.h"

#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QList>
#include <QStringList>

#include <algorithm>

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

}  // namespace

void EskdRenderer::renderA4(QPainter& painter, const QRectF& target,
                            const sketch::Sketch& sketch,
                            const TitleBlockData& title) {
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
  const double scale = std::min(drawingArea.width() / (sketch.widthMm() + 30.0),
                                drawingArea.height() / (sketch.heightMm() + 30.0));
  const QPointF center = drawingArea.center();
  auto map = [&](sketch::Point point) {
    return QPointF(center.x() + point.xMm * scale,
                   center.y() - point.yMm * scale);
  };

  painter.setPen(QPen(Qt::black, 0.7));
  for (const auto& segment : sketch.lines())
    painter.drawLine(map(segment.start), map(segment.end));
  for (const auto& circle : sketch.circles()) {
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
  painter.restore();
}

}  // namespace solidar::drawing
