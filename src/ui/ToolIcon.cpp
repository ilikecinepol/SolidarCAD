#include "ui/ToolIcon.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <QIconEngine>

#include <algorithm>
#include <array>
#include <cmath>

#include "ui/ThemeManager.h"

namespace solidar {
namespace {

constexpr qreal kCanvas = 48.0;
bool isConstraint(ToolIconKind kind) {
  return kind >= ToolIconKind::OrthogonalConstraint &&
         kind <= ToolIconKind::LockConstraint;
}

void point(QPainter& painter, QPointF center, qreal radius = 2.3) {
  const QBrush previous = painter.brush();
  painter.setBrush(painter.pen().color());
  painter.drawEllipse(center, radius, radius);
  painter.setBrush(previous);
}

void arrowHead(QPainter& painter, QPointF tip, QPointF direction,
               qreal size = 4.0) {
  const qreal length = std::hypot(direction.x(), direction.y());
  if (length <= 0.0) return;
  const QPointF unit = direction / length;
  const QPointF normal(-unit.y(), unit.x());
  painter.drawLine(tip, tip - unit * size + normal * size * 0.65);
  painter.drawLine(tip, tip - unit * size - normal * size * 0.65);
}

void dimensionLine(QPainter& painter, QPointF from, QPointF to) {
  painter.drawLine(from, to);
  arrowHead(painter, from, from - to, 3.5);
  arrowHead(painter, to, to - from, 3.5);
}

void drawCube(QPainter& painter, QPointF offset = {}) {
  const QPolygonF top{{10, 16}, {23, 9}, {37, 16}, {24, 23}};
  const QPolygonF left{{10, 16}, {24, 23}, {24, 38}, {10, 31}};
  const QPolygonF right{{24, 23}, {37, 16}, {37, 31}, {24, 38}};
  painter.save();
  painter.translate(offset);
  painter.drawPolygon(top);
  painter.drawPolygon(left);
  painter.drawPolygon(right);
  painter.restore();
}

void drawTool(QPainter& p, ToolIconKind kind) {
  switch (kind) {
    case ToolIconKind::CreateSketch:
      p.drawPolygon(QPolygonF{{7, 33}, {27, 33}, {39, 25}, {19, 25}});
      p.drawLine(10, 28, 22, 28);
      p.drawLine(28, 20, 40, 8);
      p.drawLine(34, 8, 40, 8);
      p.drawLine(40, 8, 40, 14);
      break;
    case ToolIconKind::Extrude:
      p.drawRect(QRectF(7, 25, 23, 15));
      p.drawPolygon(QPolygonF{{7, 25}, {17, 18}, {40, 18}, {30, 25}});
      p.drawPolygon(QPolygonF{{30, 25}, {40, 18}, {40, 33}, {30, 40}});
      p.drawLine(23, 18, 23, 7);
      arrowHead(p, {23, 6}, {0, -1});
      break;
    case ToolIconKind::Pocket:
      drawCube(p);
      p.drawRect(QRectF(18, 9, 12, 10));
      p.drawLine(24, 7, 24, 20);
      arrowHead(p, {24, 21}, {0, 1});
      break;
    case ToolIconKind::Revolve:
      p.drawLine(24, 7, 24, 41);
      p.drawPolygon(QPolygonF{{21, 14}, {12, 18}, {12, 35}, {21, 39}});
      p.drawArc(QRectF(6, 6, 36, 36), 35 * 16, 275 * 16);
      arrowHead(p, {38, 13}, {1, -1});
      break;
    case ToolIconKind::Fillet: {
      QPainterPath path;
      path.moveTo(8, 40);
      path.lineTo(8, 25);
      path.cubicTo(8, 15, 15, 8, 25, 8);
      path.lineTo(40, 8);
      p.drawPath(path);
      p.drawLine(13, 38, 13, 27);
      p.drawArc(QRectF(13, 13, 22, 22), 90 * 16, 90 * 16);
      p.drawLine(24, 13, 38, 13);
      break;
    }
    case ToolIconKind::Chamfer:
      p.drawPolyline(QPolygonF{{8, 40}, {8, 24}, {24, 8}, {40, 8}});
      p.drawPolyline(QPolygonF{{13, 38}, {13, 27}, {27, 13}, {38, 13}});
      dimensionLine(p, {9, 18}, {18, 9});
      break;
    case ToolIconKind::Move:
      point(p, {19, 29}, 2.0);
      p.drawLine(19, 29, 39, 29);
      arrowHead(p, {40, 29}, {1, 0});
      p.drawLine(19, 29, 19, 8);
      arrowHead(p, {19, 7}, {0, -1});
      p.drawLine(19, 29, 8, 40);
      arrowHead(p, {7, 41}, {-1, 1});
      break;
    case ToolIconKind::Ruler:
      p.save();
      p.translate(24, 24);
      p.rotate(-38.0);
      p.translate(-24, -24);
      p.drawRoundedRect(QRectF(6, 17, 36, 14), 2.0, 2.0);
      for (int index = 0; index < 7; ++index) {
        const qreal x = 10.0 + index * 4.5;
        const qreal height = index % 2 == 0 ? 7.0 : 4.5;
        p.drawLine(QPointF(x, 17), QPointF(x, 17 + height));
      }
      p.restore();
      break;
    case ToolIconKind::Shell:
      drawCube(p);
      p.drawPolygon(QPolygonF{{15, 18}, {24, 13}, {32, 17}, {24, 21}});
      p.drawLine(24, 21, 24, 34);
      break;
    case ToolIconKind::Draft:
      p.drawPolygon(QPolygonF{{10, 39}, {17, 9}, {35, 9}, {39, 39}});
      p.drawLine(8, 39, 41, 39);
      p.drawArc(QRectF(7, 8, 18, 18), 270 * 16, 65 * 16);
      arrowHead(p, {23, 13}, {1, -1});
      break;
    case ToolIconKind::Mirror:
    case ToolIconKind::SketchMirror:
      p.drawLine(24, 5, 24, 43);
      p.drawPolygon(QPolygonF{{7, 36}, {19, 12}, {19, 36}});
      p.drawPolygon(QPolygonF{{41, 36}, {29, 12}, {29, 36}});
      break;
    case ToolIconKind::LinearPattern:
      for (qreal x : {7.0, 21.0, 35.0}) p.drawRect(QRectF(x, 17, 7, 14));
      p.drawLine(9, 38, 40, 38);
      arrowHead(p, {41, 38}, {1, 0});
      break;
    case ToolIconKind::CircularPattern:
      for (const QPointF center : std::array<QPointF, 4>{
               QPointF(24, 7), QPointF(41, 24), QPointF(24, 41),
               QPointF(7, 24)})
        p.drawRect(QRectF(center.x() - 3, center.y() - 3, 6, 6));
      p.drawArc(QRectF(9, 9, 30, 30), 20 * 16, 285 * 16);
      arrowHead(p, {38, 17}, {1, -1});
      break;
    case ToolIconKind::Line:
      p.drawLine(9, 38, 39, 10);
      point(p, {9, 38});
      point(p, {39, 10});
      break;
    case ToolIconKind::Rectangle:
      p.drawRect(QRectF(8, 11, 32, 26));
      point(p, {8, 37});
      point(p, {40, 11});
      break;
    case ToolIconKind::Circle:
    case ToolIconKind::CircleCenterRadius:
      p.drawEllipse(QRectF(8, 8, 32, 32));
      p.drawLine(24, 24, 36, 13);
      point(p, {24, 24});
      point(p, {36, 13});
      break;
    case ToolIconKind::Arc:
      p.drawArc(QRectF(8, 8, 32, 32), 15 * 16, 215 * 16);
      point(p, {39, 20});
      point(p, {12, 34});
      point(p, {26, 8});
      break;
    case ToolIconKind::Projection:
      p.drawLine(8, 12, 40, 12);
      p.drawLine(8, 36, 40, 36);
      p.drawLine(14, 19, 14, 29);
      p.drawLine(24, 19, 24, 29);
      p.drawLine(34, 19, 34, 29);
      arrowHead(p, {14, 32}, {0, 1});
      arrowHead(p, {24, 32}, {0, 1});
      arrowHead(p, {34, 32}, {0, 1});
      break;
    case ToolIconKind::Polygon:
      p.drawPolygon(QPolygonF{{24, 6}, {40, 15}, {40, 33}, {24, 42},
                              {8, 33}, {8, 15}});
      point(p, {24, 24});
      p.drawLine(24, 24, 24, 6);
      break;
    case ToolIconKind::Slot:
      p.drawRoundedRect(QRectF(6, 15, 36, 18), 9, 9);
      p.drawLine(15, 24, 33, 24);
      point(p, {15, 24}, 1.8);
      point(p, {33, 24}, 1.8);
      break;
    case ToolIconKind::Text:
      p.drawLine(9, 9, 39, 9);
      p.drawLine(24, 9, 24, 39);
      p.drawLine(15, 39, 33, 39);
      break;
    case ToolIconKind::Trim:
      p.drawEllipse(QRectF(7, 29, 10, 10));
      p.drawEllipse(QRectF(31, 29, 10, 10));
      p.drawLine(15, 31, 35, 10);
      p.drawLine(33, 31, 13, 10);
      p.drawLine(12, 10, 21, 19);
      p.drawLine(36, 10, 27, 19);
      break;
    case ToolIconKind::Delete:
      p.drawRect(QRectF(14, 15, 20, 25));
      p.drawLine(11, 15, 37, 15);
      p.drawLine(19, 10, 29, 10);
      p.drawLine(20, 21, 20, 34);
      p.drawLine(28, 21, 28, 34);
      break;
    case ToolIconKind::Clear:
      p.drawRect(QRectF(9, 9, 30, 30));
      p.drawLine(14, 14, 34, 34);
      p.drawLine(34, 14, 14, 34);
      break;
    case ToolIconKind::AutoDimension:
      p.drawLine(8, 10, 8, 38);
      p.drawLine(40, 10, 40, 38);
      dimensionLine(p, {11, 24}, {37, 24});
      p.drawText(QRectF(10, 5, 28, 13), Qt::AlignCenter,
                 QString::fromUtf8("АВТО"));
      break;
    case ToolIconKind::OrthogonalConstraint:
      p.drawLine(10, 9, 10, 37);
      p.drawLine(10, 37, 39, 37);
      p.drawRect(QRectF(10, 27, 10, 10));
      break;
    case ToolIconKind::CoincidentConstraint:
      p.drawLine(8, 38, 40, 10);
      point(p, {24, 24}, 4.0);
      arrowHead(p, {20, 20}, {-1, -1});
      arrowHead(p, {28, 28}, {1, 1});
      break;
    case ToolIconKind::PerpendicularConstraint:
      p.drawLine(9, 38, 39, 9);
      p.drawLine(17, 8, 40, 31);
      p.drawRect(QRectF(20, 20, 8, 8));
      break;
    case ToolIconKind::ParallelConstraint:
      p.drawLine(9, 34, 35, 8);
      p.drawLine(15, 40, 41, 14);
      p.drawLine(18, 25, 25, 18);
      p.drawLine(25, 31, 32, 24);
      break;
    case ToolIconKind::EqualConstraint:
      p.drawLine(8, 17, 40, 17);
      p.drawLine(8, 31, 40, 31);
      break;
    case ToolIconKind::TangentConstraint:
      p.drawEllipse(QRectF(11, 7, 26, 26));
      p.drawLine(7, 38, 41, 27);
      point(p, {32, 30}, 2.0);
      break;
    case ToolIconKind::LockConstraint:
      p.drawRoundedRect(QRectF(11, 21, 26, 20), 3, 3);
      p.drawArc(QRectF(16, 7, 16, 24), 0, 180 * 16);
      point(p, {24, 30}, 2.1);
      p.drawLine(24, 32, 24, 36);
      break;
    case ToolIconKind::CircleTwoPoints:
    case ToolIconKind::CircleThreePoints:
    case ToolIconKind::CircleThreeTangents:
    case ToolIconKind::CircleTwoTangentsRadius:
      p.drawEllipse(QRectF(8, 8, 32, 32));
      if (kind == ToolIconKind::CircleTwoPoints ||
          kind == ToolIconKind::CircleThreePoints) {
        point(p, {8, 24});
        point(p, {40, 24});
        if (kind == ToolIconKind::CircleThreePoints) point(p, {24, 8});
      } else {
        p.drawLine(5, 8, 41, 4);
        p.drawLine(7, 41, 43, 36);
        if (kind == ToolIconKind::CircleThreeTangents)
          p.drawLine(4, 34, 10, 5);
        else {
          p.drawLine(24, 24, 38, 12);
          point(p, {24, 24}, 1.8);
        }
      }
      break;
    case ToolIconKind::RectangleTwoPoints:
      p.drawRect(QRectF(8, 11, 32, 26));
      point(p, {8, 37});
      point(p, {40, 11});
      break;
    case ToolIconKind::RectangleThreePoints:
      p.drawPolygon(QPolygonF{{8, 31}, {16, 10}, {40, 19}, {32, 40}});
      point(p, {8, 31});
      point(p, {16, 10});
      point(p, {40, 19});
      break;
    case ToolIconKind::RectangleFromCenter:
      p.drawRect(QRectF(8, 11, 32, 26));
      point(p, {24, 24});
      p.drawLine(24, 24, 40, 11);
      point(p, {40, 11});
      break;
  }
}

class ToolIconEngine final : public QIconEngine {
 public:
  explicit ToolIconEngine(ToolIconKind kind) : kind_(kind) {}

  [[nodiscard]] QIconEngine* clone() const override {
    return new ToolIconEngine(kind_);
  }

  void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode,
             QIcon::State) override {
    if (!painter || rect.isEmpty()) return;
    const auto& colors = ThemeManager::instance().colors();
    const bool danger = isConstraint(kind_) || kind_ == ToolIconKind::Delete ||
                        kind_ == ToolIconKind::Clear;
    QColor line = danger ? colors.danger : colors.accent;
    if (mode == QIcon::Disabled) line = colors.textDisabled;
    if (mode == QIcon::Active && !danger) line = colors.accentHover;

    const qreal side = std::min(rect.width(), rect.height());
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->translate(rect.center());
    painter->scale(side / kCanvas, side / kCanvas);
    painter->translate(-kCanvas / 2.0, -kCanvas / 2.0);
    painter->setPen(
        QPen(line, 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QColor fill = line;
    fill.setAlpha(34);
    painter->setBrush(fill);
    QFont font = painter->font();
    font.setBold(true);
    font.setPixelSize(9);
    painter->setFont(font);
    drawTool(*painter, kind_);
    painter->restore();
  }

  [[nodiscard]] QPixmap pixmap(const QSize& size, QIcon::Mode mode,
                               QIcon::State state) override {
    QPixmap result(size);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    paint(&painter, result.rect(), mode, state);
    return result;
  }

 private:
  ToolIconKind kind_;
};

}  // namespace

QIcon toolIcon(ToolIconKind kind) {
  return QIcon(new ToolIconEngine(kind));
}

}  // namespace solidar
