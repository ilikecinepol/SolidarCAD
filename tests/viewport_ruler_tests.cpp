#include "TestAssertions.h"

#include <QApplication>
#include <QImage>
#include <QPainter>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "ui/ThemeColors.h"
#include "ui/ViewportRuler.h"

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);

  const solidar::ViewportCameraState camera{
      0.0F, 0.0F, 1.0F, {}, QSize(1000, 800), 1.0F, {}, 100.0};
  const std::vector<solidar::RenderTriangle> triangles{{
      {-10.0, -10.0, 0.0}, {10.0, -10.0, 0.0}, {0.0, 10.0, 0.0}}};
  solidar::RenderEdge bottom;
  bottom.points = {{-10.0, -10.0, 0.0}, {10.0, -10.0, 0.0}};
  const std::vector<solidar::RenderEdge> edges{bottom};

  const QPointF surfaceScreen = camera.worldToScreen({0.0, 0.0, 0.0});
  const auto surface = solidar::ViewportRuler::pickPoint(
      triangles, edges, camera, surfaceScreen);
  CHECK(surface.has_value());
  CHECK(surface->snap == solidar::RulerSnapKind::Surface);
  CHECK(std::abs(surface->world.x) < 1e-5);
  CHECK(std::abs(surface->world.y) < 1e-5);

  const QPointF vertexScreen = camera.worldToScreen({-10.0, -10.0, 0.0});
  const auto vertex = solidar::ViewportRuler::pickPoint(
      triangles, edges, camera, vertexScreen + QPointF(2.0, 1.0));
  CHECK(vertex.has_value());
  CHECK(vertex->snap == solidar::RulerSnapKind::Vertex);
  CHECK(std::abs(vertex->world.x + 10.0) < 1e-9);

  const QPointF edgeScreen = camera.worldToScreen({0.0, -10.0, 0.0});
  const auto edge = solidar::ViewportRuler::pickPoint(
      triangles, edges, camera, edgeScreen + QPointF(0.0, 3.0));
  CHECK(edge.has_value());
  CHECK(edge->snap == solidar::RulerSnapKind::Edge);
  CHECK(std::abs(edge->world.y + 10.0) < 1e-9);

  solidar::ViewportRuler ruler;
  ruler.begin();
  CHECK(ruler.active());
  CHECK(ruler.commitPoint({{0.0, 0.0, 0.0}, {}, 0.0,
                           solidar::RulerSnapKind::Vertex}) ==
        solidar::RulerClickResult::FirstPoint);
  CHECK(ruler.commitPoint({{3.0, 4.0, 12.0}, {}, 0.0,
                           solidar::RulerSnapKind::Surface}) ==
        solidar::RulerClickResult::Completed);
  CHECK(ruler.measuredDistanceMm().has_value());
  CHECK(std::abs(*ruler.measuredDistanceMm() - 13.0) < 1e-9);
  CHECK(ruler.commitPoint({{1.0, 2.0, 3.0}, {}, 0.0,
                           solidar::RulerSnapKind::Edge}) ==
        solidar::RulerClickResult::Restarted);
  CHECK(!ruler.measuredDistanceMm().has_value());

  QImage image(1000, 800, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  ruler.paint(painter, camera, solidar::darkThemeColors());
  painter.end();
  ruler.cancel();
  CHECK(!ruler.active());
  CHECK(!ruler.firstPoint().has_value());
  return EXIT_SUCCESS;
}
