#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QMouseEvent>
#include <QWheelEvent>

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
  const auto wheel = [](solidar::Viewport& view, QPointF position) {
    QWheelEvent event(position, position, {}, {0, 120}, Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
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
  viewport.resize(800, 600);
  viewport.setBodyShape(source, bodyId, sourceFeatureId);
  viewport.setSolidVisible(true);
  viewport.setSelectionFilter(solidar::SelectionFilter::Edge);
  const solidar::ViewportCameraState sourceCamera{
      viewport.cameraYawDegrees(), viewport.cameraPitchDegrees(), 1.0F, {},
      viewport.size(), 1.0F, {20.0, 15.0, 10.0}, 1.0};
  // Hover remains source-topology based even before a tool has selected an
  // edge, which is the state Chamfer and Fillet start in.
  mouse(viewport, QEvent::MouseMove, sourceCamera.worldToScreen({0, 0, 0}),
        Qt::NoButton, Qt::NoButton);
  CHECK(viewport.hoveredBodyEdgeIndex());
  const std::size_t sourceHoverIndex = *viewport.hoveredBodyEdgeIndex();
  const auto preselectedEdge = solidar::makeEdgeReference(
      *source, bodyId, sourceFeatureId, (sourceHoverIndex + 1) % 12);
  CHECK(preselectedEdge.signature);
  CHECK(preselectedEdge.edgeIndex != sourceHoverIndex);
  const std::vector<solidar::EdgeReference> preselection{preselectedEdge};
  viewport.setSelectedBodyEdges(preselection);

  int edgeSelectionSignals = 0;
  int generalSelectionSignals = 0;
  QObject::connect(&viewport, &solidar::Viewport::bodyEdgeSelectionChanged,
                   &viewport, [&] { ++edgeSelectionSignals; });
  QObject::connect(&viewport, &solidar::Viewport::selectionChanged, &viewport,
                   [&](const QString&) { ++generalSelectionSignals; });

  // A live Fillet/Chamfer preview must keep picking the source topology. Hover
  // is transient and must not replace the already committed source edge.
  viewport.setToolPreviewShape(bodyId, previewFeatureId, previewA);
  mouse(viewport, QEvent::MouseMove, sourceCamera.worldToScreen({0, 0, 0}),
        Qt::NoButton, Qt::NoButton);
  CHECK(viewport.hoveredBodyEdgeIndex() == sourceHoverIndex);
  CHECK(viewport.selectedBodyEdges() == preselection);
  CHECK(viewport.selectedBodyEdges().front().bodyId == bodyId);
  CHECK(viewport.selectedBodyEdges().front().featureId == sourceFeatureId);

  QEvent edgeLeave(QEvent::Leave);
  QApplication::sendEvent(&viewport, &edgeLeave);
  CHECK(!viewport.hoveredBodyEdgeIndex());
  CHECK(viewport.selectedBodyEdges() == preselection);
  CHECK(edgeSelectionSignals == 0);
  CHECK(generalSelectionSignals == 0);

  viewport.setToolManipulator({{}, {0, 0, 1}, 0.0, 0.0, 100000.0});
  const auto* distanceHud = viewport.findChild<QDoubleSpinBox*>("distance");
  CHECK(distanceHud && std::abs(distanceHud->minimum()) < 1e-12);
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

  enum class CameraGesture { Orbit, Pan, Zoom };
  for (const auto gesture : {CameraGesture::Orbit, CameraGesture::Pan,
                             CameraGesture::Zoom}) {
    solidar::Viewport gestureViewport;
    gestureViewport.resize(800, 600);
    gestureViewport.setBodyShape(source, bodyId, sourceFeatureId);
    gestureViewport.setSolidVisible(true);
    gestureViewport.setSelectionFilter(solidar::SelectionFilter::Edge);
    gestureViewport.setToolPreviewShape(bodyId, previewFeatureId, previewA);
    gestureViewport.setSelectedBodyEdges(preselection);
    const solidar::ViewportCameraState gestureCamera{
        gestureViewport.cameraYawDegrees(),
        gestureViewport.cameraPitchDegrees(), 1.0F, {},
        gestureViewport.size(), 1.0F, {20.0, 15.0, 10.0}, 1.0};
    const QPointF edgePoint = gestureCamera.worldToScreen({0, 0, 0});

    int gestureEdgeSelectionSignals = 0;
    int gestureGeneralSelectionSignals = 0;
    QObject::connect(&gestureViewport,
                     &solidar::Viewport::bodyEdgeSelectionChanged,
                     &gestureViewport,
                     [&] { ++gestureEdgeSelectionSignals; });
    QObject::connect(&gestureViewport, &solidar::Viewport::selectionChanged,
                     &gestureViewport,
                     [&](const QString&) { ++gestureGeneralSelectionSignals; });

    mouse(gestureViewport, QEvent::MouseMove, edgePoint,
          Qt::NoButton, Qt::NoButton);
    CHECK(gestureViewport.hoveredBodyEdgeIndex());

    if (gesture == CameraGesture::Orbit) {
      mouse(gestureViewport, QEvent::MouseButtonPress, edgePoint,
            Qt::RightButton, Qt::RightButton);
      mouse(gestureViewport, QEvent::MouseMove, edgePoint + QPointF(12, 8),
            Qt::NoButton, Qt::RightButton);
      mouse(gestureViewport, QEvent::MouseButtonRelease,
            edgePoint + QPointF(12, 8), Qt::RightButton, Qt::NoButton);
    } else if (gesture == CameraGesture::Pan) {
      mouse(gestureViewport, QEvent::MouseButtonPress, edgePoint,
            Qt::MiddleButton, Qt::MiddleButton);
      mouse(gestureViewport, QEvent::MouseMove, edgePoint + QPointF(15, -9),
            Qt::NoButton, Qt::MiddleButton);
      mouse(gestureViewport, QEvent::MouseButtonRelease,
            edgePoint + QPointF(15, -9), Qt::MiddleButton, Qt::NoButton);
    } else {
      wheel(gestureViewport, edgePoint);
    }

    CHECK(!gestureViewport.hoveredBodyEdgeIndex());
    CHECK(gestureViewport.selectedBodyEdges() == preselection);
    CHECK(gestureEdgeSelectionSignals == 0);
    CHECK(gestureGeneralSelectionSignals == 0);
  }

  // Face hover is independent from committed face selection, emits no
  // selection signal, and is invalidated by both leave and camera changes.
  {
    solidar::Viewport faceViewport;
    faceViewport.resize(800, 600);
    faceViewport.setBodyShape(source, bodyId, sourceFeatureId);
    faceViewport.setSolidVisible(true);
    faceViewport.setSelectionFilter(solidar::SelectionFilter::Face);
    const solidar::ViewportCameraState faceCamera{
        faceViewport.cameraYawDegrees(), faceViewport.cameraPitchDegrees(),
        1.0F, {}, faceViewport.size(), 1.0F, {20.0, 15.0, 10.0}, 1.0};
    const QPointF topFace = faceCamera.worldToScreen({20.0, 15.0, 20.0});

    int faceSelectionSignals = 0;
    int faceGeneralSelectionSignals = 0;
    QObject::connect(&faceViewport,
                     &solidar::Viewport::bodyFaceSelectionChanged,
                     &faceViewport, [&] { ++faceSelectionSignals; });
    QObject::connect(&faceViewport, &solidar::Viewport::selectionChanged,
                     &faceViewport,
                     [&](const QString&) { ++faceGeneralSelectionSignals; });

    mouse(faceViewport, QEvent::MouseMove, topFace,
          Qt::NoButton, Qt::NoButton);
    CHECK(faceViewport.hoveredBodyFaceIndex());
    const std::size_t hoveredFace = *faceViewport.hoveredBodyFaceIndex();
    const auto selectedFace = solidar::makeFaceReference(
        *source, bodyId, sourceFeatureId, (hoveredFace + 1) % 6);
    CHECK(selectedFace.signature);
    const std::vector<solidar::FaceReference> selectedFaces{selectedFace};
    faceViewport.setSelectedBodyFaces(selectedFaces);

    mouse(faceViewport, QEvent::MouseMove, topFace,
          Qt::NoButton, Qt::NoButton);
    CHECK(faceViewport.hoveredBodyFaceIndex() == hoveredFace);
    CHECK(faceViewport.selectedBodyFaces() == selectedFaces);
    CHECK(faceViewport.selectedBodyFaceIndex() !=
          faceViewport.hoveredBodyFaceIndex());
    CHECK(faceSelectionSignals == 0);
    CHECK(faceGeneralSelectionSignals == 0);

    faceViewport.setSolidVisible(false);
    CHECK(!faceViewport.hoveredBodyFaceIndex());
    CHECK(faceViewport.selectedBodyFaces() == selectedFaces);
    CHECK(faceSelectionSignals == 0);
    CHECK(faceGeneralSelectionSignals == 0);
    faceViewport.setSolidVisible(true);
    mouse(faceViewport, QEvent::MouseMove, topFace,
          Qt::NoButton, Qt::NoButton);
    CHECK(faceViewport.hoveredBodyFaceIndex() == hoveredFace);

    QEvent faceLeave(QEvent::Leave);
    QApplication::sendEvent(&faceViewport, &faceLeave);
    CHECK(!faceViewport.hoveredBodyFaceIndex());
    CHECK(faceViewport.selectedBodyFaces() == selectedFaces);

    mouse(faceViewport, QEvent::MouseMove, topFace,
          Qt::NoButton, Qt::NoButton);
    CHECK(faceViewport.hoveredBodyFaceIndex());
    faceViewport.viewTop();
    CHECK(!faceViewport.hoveredBodyFaceIndex());
    CHECK(faceViewport.selectedBodyFaces() == selectedFaces);
    CHECK(faceSelectionSignals == 0);
    CHECK(faceGeneralSelectionSignals == 0);
  }

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
    body.beginExtrusionSurfaceSelection();
    mouse(body, QEvent::MouseButtonPress, outside,
          Qt::LeftButton, Qt::LeftButton);
    CHECK(facePicks == 1);
  }
  return EXIT_SUCCESS;
}
