#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

#include "model/TopologyReferenceResolver.h"
#include "ui/Viewport.h"
#include "ui/ViewportCamera.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);
  // Exercise widget events without showing a QOpenGLWidget or needing a GPU.
  const auto mouse = [](solidar::Viewport& view, QEvent::Type type,
                        QPointF position, Qt::MouseButton button,
                        Qt::MouseButtons buttons) {
    QMouseEvent event(type, position, position, button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&view, &event);
  };
  const std::vector<solidar::SketchPlacement> placements{
      {{35, 20, 65}, {1, 0, 0}, {0, 1, 0}},
      {{45, 20, 35}, {0, 1, 0}, {0, 0, 1}},
      {{35, 20, 45}, {1, 0, 0}, {0, 0.6, 0.8}}};
  const QString support = QStringLiteral(
      "\u0413\u0440\u0430\u043d\u044c \u0442\u0435\u043b\u0430 #7");
  for (const auto& placement : placements) {
    for (const bool circle : {false, true}) {
      for (const bool multiple : {false, true}) {
        solidar::Viewport view;
        view.resize(800, 600);
        view.setBodyPosition({7, -4});
        solidar::sketch::Sketch profile;
        if (circle) profile.addCircle({0, 0}, 6);
        else profile.addRectangle({-6, -6}, {6, 6});
        // A disjoint second contour forces the screen-to-plane region path.
        if (multiple) profile.addRectangle({18, -6}, {30, 6});
        view.addSketch(profile, support, placement);
        const solidar::ViewportCameraState camera{
            view.cameraYawDegrees(), view.cameraPitchDegrees(), 1.0F, {},
            view.size()};
        const auto screen = [&](double u, double v) {
          auto world = placement.toWorld(u, v);
          world.x += 7;
          world.y -= 4;
          return camera.worldToScreen(world);
        };
        int picks = 0;
        QObject::connect(&view, &solidar::Viewport::extrusionSurfacePicked,
                         &view, [&](const QString&) { ++picks; });
        view.beginExtrusionSurfaceSelection();
        // No preceding move: the click itself must resolve placement and index.
        mouse(view, QEvent::MouseButtonPress, screen(0, 0),
              Qt::LeftButton, Qt::LeftButton);
        CHECK(picks == 1);
        CHECK(view.extrusionCandidateSketchIndex() == 0);
        CHECK(view.extrusionCandidateSupport() == support);
        CHECK(!view.extrusionCandidateOnBodyCap());
        const auto& candidate = view.extrusionCandidateSketch();
        if (circle && !multiple) {
          CHECK(candidate.circles().size() == 1);
          CHECK(std::abs(candidate.circles().front().radiusMm - 6) < 1e-6);
          CHECK(std::abs(candidate.circles().front().center.xMm) < 1e-6);
          CHECK(std::abs(candidate.circles().front().center.yMm) < 1e-6);
        } else {
          CHECK(candidate.lines().size() >= 4);
          double minX = 1000, minY = 1000, maxX = -1000, maxY = -1000;
          for (const auto& line : candidate.lines()) {
            minX = std::min(minX, line.start.xMm);
            minY = std::min(minY, line.start.yMm);
            maxX = std::max(maxX, line.start.xMm);
            maxY = std::max(maxY, line.start.yMm);
          }
          CHECK(std::abs(minX + 6) < 0.02 && std::abs(maxX - 6) < 0.02);
          CHECK(std::abs(minY + 6) < 0.02 && std::abs(maxY - 6) < 0.02);
        }
        view.showExtrusionManipulator(12);
        const auto normal = placement.normal();
        const QPointF axis = camera.worldToScreen({normal.x, normal.y, normal.z}) -
                             camera.worldToScreen({0, 0, 0});
        double draggedLength = 0;
        QObject::connect(&view, &solidar::Viewport::extrusionPreviewLengthChanged,
                         &view, [&](double value) { draggedLength = value; });
        mouse(view, QEvent::MouseButtonPress, screen(0, 0) + axis * 12,
              Qt::LeftButton, Qt::LeftButton);
        mouse(view, QEvent::MouseMove, screen(0, 0) + axis * 18,
              Qt::NoButton, Qt::LeftButton);
        mouse(view, QEvent::MouseButtonRelease, screen(0, 0) + axis * 18,
              Qt::LeftButton, Qt::NoButton);
        CHECK(std::abs(draggedLength - 18) < 0.02);
        view.beginExtrusionSurfaceSelection();
        CHECK(view.findChild<QDoubleSpinBox*>()->isHidden());
        CHECK(view.extrusionCandidateSketchIndex() == static_cast<std::size_t>(-1));
        CHECK(view.extrusionCandidateSupport().isEmpty());
        CHECK(view.extrusionCandidateSketch().lines().empty());
        CHECK(view.extrusionCandidateSketch().circles().empty());
        // A stale valid hover must not turn an outside click into a selection.
        mouse(view, QEvent::MouseMove, screen(0, 0), Qt::NoButton, Qt::NoButton);
        mouse(view, QEvent::MouseButtonPress, {10, 10},
              Qt::LeftButton, Qt::LeftButton);
        CHECK(picks == 1);
        // Nor may the old raw-XY location select a translated face sketch.
        mouse(view, QEvent::MouseButtonPress, camera.worldToScreen({7, -4, 0}),
              Qt::LeftButton, Qt::LeftButton);
        CHECK(picks == 1);
        mouse(view, QEvent::MouseButtonPress, screen(0, 0),
              Qt::LeftButton, Qt::LeftButton);
        CHECK(picks == 2);
      }
    }
  }

  constexpr solidar::BodyId bodyId = 41;
  constexpr solidar::FeatureId sourceFeatureId = 73;
  constexpr solidar::FeatureId previewFeatureId = 99;
  const auto source = std::make_shared<TopoDS_Shape>(
      BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape());
  const auto previewA = std::make_shared<TopoDS_Shape>(
      BRepPrimAPI_MakeCylinder(14.0, 25.0).Shape());
  const auto previewB = std::make_shared<TopoDS_Shape>(
      BRepPrimAPI_MakeCylinder(12.0, 35.0).Shape());

  const auto edgeA = solidar::makeEdgeReference(
      *source, bodyId, sourceFeatureId, 0);
  const auto edgeB = solidar::makeEdgeReference(
      *source, bodyId, sourceFeatureId, 1);
  const auto edgeC = solidar::makeEdgeReference(
      *source, bodyId, sourceFeatureId, 2);
  CHECK(edgeA.signature && edgeB.signature && edgeC.signature);

  solidar::Viewport viewport;
  viewport.setBodyShape(source, bodyId, sourceFeatureId);
  viewport.setSelectedBodyEdges({edgeA});
  viewport.setToolPreviewShape(bodyId, previewFeatureId, previewA);
  CHECK(viewport.selectedBodyEdges() ==
        std::vector<solidar::EdgeReference>{edgeA});

  viewport.setSelectedBodyEdges({edgeA, edgeB});
  viewport.setToolPreviewShape(bodyId, previewFeatureId, previewB);
  const auto selected = viewport.selectedBodyEdges();
  CHECK(selected == std::vector<solidar::EdgeReference>({edgeA, edgeB}));
  CHECK(selected[0].featureId == sourceFeatureId);
  CHECK(selected[1].featureId == sourceFeatureId);

  viewport.setSelectedBodyEdges({edgeA, edgeB, edgeC});
  viewport.setToolPreviewShape(bodyId, previewFeatureId, previewA);
  CHECK(viewport.selectedBodyEdges() ==
        std::vector<solidar::EdgeReference>({edgeA, edgeB, edgeC}));

  // Parameter changes rebuild preview repeatedly; toggling B leaves the same
  // source references A+C and never adopts previewFeatureId.
  viewport.setToolPreviewShape(bodyId, previewFeatureId, previewB);
  viewport.setToolPreviewShape(bodyId, previewFeatureId, previewA);
  viewport.setSelectedBodyEdges({edgeA, edgeC});
  const auto afterDeselect = viewport.selectedBodyEdges();
  CHECK(afterDeselect ==
        std::vector<solidar::EdgeReference>({edgeA, edgeC}));
  for (const auto& edge : afterDeselect) {
    CHECK(edge.bodyId == bodyId);
    CHECK(edge.featureId == sourceFeatureId);
    CHECK(edge.topology().kind == solidar::TopologyKind::Edge);
    CHECK(edge.signature);
  }

  viewport.clearToolPreviewShape();
  CHECK(viewport.selectedBodyEdges() ==
        std::vector<solidar::EdgeReference>({edgeA, edgeC}));

  // Regression: the Extrusion tool must still select a real B-Rep body face
  // even when no sketch contour lies under the cursor. This was broken by an
  // early return in updateExtrusionHover that skipped the body-face path and
  // by the legacy box/cap reconstruction, which ignored the actual B-Rep mesh.
  {
    solidar::Viewport body;
    body.resize(800, 600);
    const auto boxShape = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape());
    body.setBodyShape(boxShape, bodyId, sourceFeatureId);
    body.setSolidVisible(true);
    const solidar::ViewportCameraState camera{
        body.cameraYawDegrees(), body.cameraPitchDegrees(), 1.0F, {},
        body.size(), 1.0F, {20.0, 15.0, 10.0}, 1.0};
    // Centre of the top face (z = 20 plane), well inside the front-facing cap.
    const QPointF topFace = camera.worldToScreen({20.0, 15.0, 20.0});
    // A point clearly off the body (must not select any face).
    const QPointF outside = camera.worldToScreen({90.0, 90.0, 20.0});
    int facePicks = 0;
    QString lastSurface;
    QObject::connect(&body, &solidar::Viewport::extrusionSurfacePicked, &body,
                     [&](const QString& surface) {
                       ++facePicks;
                       lastSurface = surface;
                     });
    body.beginExtrusionSurfaceSelection();
    mouse(body, QEvent::MouseButtonPress, topFace,
          Qt::LeftButton, Qt::LeftButton);
    CHECK(facePicks == 1);
    CHECK(lastSurface.startsWith(
        QString::fromUtf8("\u0413\u0440\u0430\u043d\u044c \u0442\u0435\u043b\u0430")));
    CHECK(body.extrusionCandidateSketchIndex() ==
          static_cast<std::size_t>(-1));
    CHECK(!body.extrusionCandidateOnBodyCap());
    // The picked face resolves to a launch-side placement and its outline
    // becomes the bare-face extrusion profile (extruded along the face normal).
    CHECK(body.extrusionFacePlacement().has_value());
    CHECK(!body.extrusionCandidateSketch().lines().empty());
    body.beginExtrusionSurfaceSelection();
    mouse(body, QEvent::MouseButtonPress, outside,
          Qt::LeftButton, Qt::LeftButton);
    CHECK(facePicks == 1);
  }
  return EXIT_SUCCESS;
}
