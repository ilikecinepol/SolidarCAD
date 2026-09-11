#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QMouseEvent>

#include <gp_Pnt.hxx>

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
  const auto mouseMod = [](solidar::Viewport& view, QEvent::Type type,
                           QPointF position, Qt::MouseButton button,
                           Qt::MouseButtons buttons,
                           Qt::KeyboardModifiers modifiers) {
    QMouseEvent event(type, position, position, button, buttons, modifiers);
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

  // Rectangle marquee state machine. The body is a 40x30x20 box; with the
  // default camera (yaw -35, pitch 25) three faces are front-facing.
  const auto boxShape = std::make_shared<TopoDS_Shape>(
      BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape());
  const auto marqueeDrag = [&mouseMod](solidar::Viewport& view, QPointF start,
                                       QPointF end,
                                       Qt::KeyboardModifiers modifiers =
                                           Qt::NoModifier) {
    mouseMod(view, QEvent::MouseButtonPress, start, Qt::LeftButton,
             Qt::LeftButton, modifiers);
    mouseMod(view, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton,
             modifiers);
    mouseMod(view, QEvent::MouseButtonRelease, end, Qt::LeftButton,
             Qt::NoButton, modifiers);
  };

  // Empty-area drag spanning the body selects the spanned (front-facing) faces.
  {
    solidar::Viewport view;
    view.resize(800, 600);
    view.setBodyShape(boxShape, bodyId, sourceFeatureId);
    view.setSolidVisible(true);
    CHECK(!view.marqueeActive());
    marqueeDrag(view, {1.0, 1.0}, {799.0, 599.0});
    CHECK(!view.marqueeActive());
    const auto faces = view.selectedBodyFaces();
    CHECK(!faces.empty());
    for (const auto& face : faces) {
      CHECK(face.bodyId == bodyId);
      CHECK(face.featureId == sourceFeatureId);
      CHECK(face.signature.has_value());
    }
  }

  // A sub-threshold (<3px) drag is a click: marquee becomes active on press
  // and is cancelled on release without selecting anything.
  {
    solidar::Viewport view;
    view.resize(800, 600);
    view.setBodyShape(boxShape, bodyId, sourceFeatureId);
    view.setSolidVisible(true);
    mouseMod(view, QEvent::MouseButtonPress, {1.0, 1.0}, Qt::LeftButton,
             Qt::LeftButton, Qt::NoModifier);
    CHECK(view.marqueeActive());
    mouseMod(view, QEvent::MouseButtonRelease, {2.0, 2.0}, Qt::LeftButton,
             Qt::NoButton, Qt::NoModifier);
    CHECK(!view.marqueeActive());
    CHECK(view.selectedBodyFaces().empty());
  }

  // Edge filter selects edges only; Face filter selects faces only.
  {
    solidar::Viewport edgeView;
    edgeView.resize(800, 600);
    edgeView.setBodyShape(boxShape, bodyId, sourceFeatureId);
    edgeView.setSolidVisible(true);
    edgeView.setSelectionFilter(solidar::SelectionFilter::Edge);
    marqueeDrag(edgeView, {1.0, 1.0}, {799.0, 599.0});
    CHECK(edgeView.selectedBodyFaces().empty());
    const auto edges = edgeView.selectedBodyEdges();
    // Occlusion parity: the box has 12 unique edges, but BodyRenderMesh stores
    // 24 edge entries because TopExp_Explorer returns each shared edge twice
    // (once per the two faces it joins). Of these, the 3 fully-hidden edges
    // (x2 = 6 entries, meeting at the back corner) are rejected by the depth
    // occlusion test, so exactly 18 edges are selected. A broken occlusion
    // would select all 24; this exact count proves rear-edge rejection.
    CHECK(!edges.empty());
    CHECK(edges.size() == 18);
    for (const auto& edge : edges) {
      CHECK(edge.bodyId == bodyId);
      CHECK(edge.featureId == sourceFeatureId);
      CHECK(edge.signature.has_value());
    }
  }
  {
    solidar::Viewport faceView;
    faceView.resize(800, 600);
    faceView.setBodyShape(boxShape, bodyId, sourceFeatureId);
    faceView.setSolidVisible(true);
    faceView.setSelectionFilter(solidar::SelectionFilter::Face);
    marqueeDrag(faceView, {1.0, 1.0}, {799.0, 599.0});
    CHECK(!faceView.selectedBodyFaces().empty());
    CHECK(faceView.selectedBodyEdges().empty());
  }

  // Rect outside geometry: non-additive clears the selection, Ctrl preserves.
  {
    solidar::Viewport view;
    view.resize(800, 600);
    view.setBodyShape(boxShape, bodyId, sourceFeatureId);
    view.setSolidVisible(true);
    marqueeDrag(view, {1.0, 1.0}, {799.0, 599.0});
    CHECK(!view.selectedBodyFaces().empty());
    // Non-additive drag over empty area replaces (clears) the selection.
    marqueeDrag(view, {5.0, 5.0}, {60.0, 60.0});
    CHECK(view.selectedBodyFaces().empty());
    // Re-select, then a Ctrl-drag over empty area keeps it.
    marqueeDrag(view, {1.0, 1.0}, {799.0, 599.0});
    CHECK(!view.selectedBodyFaces().empty());
    marqueeDrag(view, {5.0, 5.0}, {60.0, 60.0}, Qt::ControlModifier);
    CHECK(!view.selectedBodyFaces().empty());
  }

  // Multi-body: a marquee over one body maps to that body's persistent refs.
  {
    solidar::Viewport view;
    view.resize(800, 600);
    const auto boxA = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape());
    const auto boxB = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(gp_Pnt(100.0, 0.0, 0.0), 40.0, 30.0, 20.0)
            .Shape());
    const solidar::BodyId bodyA = 41;
    const solidar::BodyId bodyB = 42;
    const solidar::FeatureId featA = 73;
    const solidar::FeatureId featB = 74;
    view.setBodyShapes({{bodyA, featA, boxA}, {bodyB, featB, boxB}});
    view.setSolidVisible(true);
    const solidar::ViewportCameraState camera{
        view.cameraYawDegrees(), view.cameraPitchDegrees(), 1.0F, {},
        view.size(), 1.0F, {70.0, 15.0, 10.0}, 1.0};
    // boxB top-face centre (boxB spans x 100..140, y 0..30, z 0..20).
    const QPointF boxBTop = camera.worldToScreen({120.0, 15.0, 20.0});
    // Drag from the gap between the two bodies (left of boxB) across boxB.
    marqueeDrag(view, boxBTop + QPointF(-150.0, 0.0),
                boxBTop + QPointF(60.0, 120.0));
    const auto faces = view.selectedBodyFaces();
    CHECK(!faces.empty());
    for (const auto& face : faces) {
      CHECK(face.bodyId == bodyB);
      CHECK(face.featureId == featB);
      CHECK(face.signature.has_value());
    }
  }

  // Ctrl+A (QKeySequence::SelectAll) sends the standard SelectAll key press
  // straight to the viewport, bypassing any MainWindow-level shortcut handling.
  const auto sendStandardKey = [](solidar::Viewport& view) {
    QKeyEvent event(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier);
    QApplication::sendEvent(&view, &event);
  };

  // Edge tool + multi-select on: SelectAll selects every eligible visible
  // edge (occlusion + single-body) and leaves the face selection empty.
  {
    solidar::Viewport view;
    view.resize(800, 600);
    view.setBodyShape(boxShape, bodyId, sourceFeatureId);
    view.setSolidVisible(true);
    view.setSelectionFilter(solidar::SelectionFilter::Edge);
    view.setEdgeMultiSelectionMode(true);
    sendStandardKey(view);
    const auto edges = view.selectedBodyEdges();
    CHECK(!edges.empty());
    // Occlusion parity: identical to the marquee edge count (18 of 24 edge
    // entries survive; the 3 hidden rear edges are rejected by depth).
    CHECK(edges.size() == 18);
    CHECK(view.selectedBodyFaces().empty());
    // Body (model-level) selection is only produced in true normal mode.
    CHECK(view.selectedBodies().empty());
    for (const auto& edge : edges) {
      CHECK(edge.bodyId == bodyId);
      CHECK(edge.featureId == sourceFeatureId);
      CHECK(edge.signature.has_value());
    }
  }

  // Face tool + multi-select on: SelectAll selects faces only.
  {
    solidar::Viewport view;
    view.resize(800, 600);
    view.setBodyShape(boxShape, bodyId, sourceFeatureId);
    view.setSolidVisible(true);
    view.setSelectionFilter(solidar::SelectionFilter::Face);
    view.setFaceMultiSelectionMode(true);
    sendStandardKey(view);
    CHECK(!view.selectedBodyFaces().empty());
    CHECK(view.selectedBodyEdges().empty());
  }

  // Normal mode (Any filter) + face multi-select on: the selection domain
  // remains faces — body selection only activates in true normal mode (no
  // multi-select mode enabled).
  {
    solidar::Viewport view;
    view.resize(800, 600);
    view.setBodyShape(boxShape, bodyId, sourceFeatureId);
    view.setSolidVisible(true);
    view.setFaceMultiSelectionMode(true);
    sendStandardKey(view);
    CHECK(!view.selectedBodyFaces().empty());
    CHECK(view.selectedBodyEdges().empty());
    CHECK(view.selectedBodies().empty());
  }

  // True normal mode (Any filter, no multi-select): Ctrl+A selects every
  // distinct visible body (model-level entity), not faces/edges.
  {
    solidar::Viewport view;
    view.resize(800, 600);
    const auto boxA = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape());
    const auto boxB = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(gp_Pnt(100.0, 0.0, 0.0), 40.0, 30.0, 20.0)
            .Shape());
    const solidar::BodyId bodyA = 41;
    const solidar::BodyId bodyB = 42;
    const solidar::FeatureId featA = 73;
    const solidar::FeatureId featB = 74;
    view.setBodyShapes({{bodyA, featA, boxA}, {bodyB, featB, boxB}});
    view.setSolidVisible(true);
    int emissions = 0;
    std::vector<solidar::BodyId> emittedIds;
    QObject::connect(
        &view, &solidar::Viewport::bodiesSelected, &view,
        [&](const std::vector<solidar::BodyId>& ids) {
          ++emissions;
          emittedIds = ids;
        });
    sendStandardKey(view);
    const std::vector<solidar::BodyId> expected{bodyA, bodyB};
    CHECK(view.selectedBodies() == expected);
    CHECK(emissions == 1);
    CHECK(emittedIds == expected);
    CHECK(view.selectedBodyFaces().empty());
    CHECK(view.selectedBodyEdges().empty());
  }

  // Single visible body: Ctrl+A selects exactly that body.
  {
    solidar::Viewport view;
    view.resize(800, 600);
    view.setBodyShape(boxShape, bodyId, sourceFeatureId);
    view.setSolidVisible(true);
    sendStandardKey(view);
    const std::vector<solidar::BodyId> expected{bodyId};
    CHECK(view.selectedBodies() == expected);
    CHECK(view.selectedBodyFaces().empty());
    CHECK(view.selectedBodyEdges().empty());
  }

  // Solid not visible: Ctrl+A selects nothing (no body ids are published).
  {
    solidar::Viewport view;
    view.resize(800, 600);
    view.setBodyShape(boxShape, bodyId, sourceFeatureId);
    view.setSolidVisible(false);
    int emissions = 0;
    QObject::connect(&view, &solidar::Viewport::bodiesSelected, &view,
                     [&](const std::vector<solidar::BodyId>&) { ++emissions; });
    sendStandardKey(view);
    CHECK(view.selectedBodies().empty());
    CHECK(view.selectedBodyFaces().empty());
    CHECK(view.selectedBodyEdges().empty());
    CHECK(emissions == 1);
  }

  // Single-select contract: with the relevant multi-select mode OFF, Ctrl+A
  // selects at most one (frontmost) entity instead of everything.
  {
    solidar::Viewport faceView;
    faceView.resize(800, 600);
    faceView.setBodyShape(boxShape, bodyId, sourceFeatureId);
    faceView.setSolidVisible(true);
    faceView.setSelectionFilter(solidar::SelectionFilter::Face);
    sendStandardKey(faceView);
    CHECK(faceView.selectedBodyFaces().size() == 1);
    CHECK(faceView.selectedBodyEdges().empty());

    solidar::Viewport edgeView;
    edgeView.resize(800, 600);
    edgeView.setBodyShape(boxShape, bodyId, sourceFeatureId);
    edgeView.setSolidVisible(true);
    edgeView.setSelectionFilter(solidar::SelectionFilter::Edge);
    sendStandardKey(edgeView);
    CHECK(edgeView.selectedBodyEdges().size() == 1);
    CHECK(edgeView.selectedBodyFaces().empty());
  }

  // Plain marquee in an edge multi-select tool REPLACES the prior selection;
  // Ctrl-drag ADDS to it. The additive flag must reflect only the Ctrl
  // modifier, never the multi-select mode.
  {
    solidar::Viewport view;
    view.resize(800, 600);
    view.setBodyShape(boxShape, bodyId, sourceFeatureId);
    view.setSolidVisible(true);
    view.setSelectionFilter(solidar::SelectionFilter::Edge);
    view.setEdgeMultiSelectionMode(true);
    marqueeDrag(view, {1.0, 1.0}, {799.0, 599.0});
    CHECK(view.selectedBodyEdges().size() == 18);
    // Plain drag over empty area replaces (clears) the selection.
    marqueeDrag(view, {5.0, 5.0}, {60.0, 60.0});
    CHECK(view.selectedBodyEdges().empty());
    // Re-select, then a Ctrl-drag over empty area keeps it (adds).
    marqueeDrag(view, {1.0, 1.0}, {799.0, 599.0});
    CHECK(view.selectedBodyEdges().size() == 18);
    marqueeDrag(view, {5.0, 5.0}, {60.0, 60.0}, Qt::ControlModifier);
    CHECK(view.selectedBodyEdges().size() == 18);
  }

  // Plane filter: a marquee selects no body geometry (Plane means base-plane
  // selection, never body faces/edges).
  {
    solidar::Viewport view;
    view.resize(800, 600);
    view.setBodyShape(boxShape, bodyId, sourceFeatureId);
    view.setSolidVisible(true);
    view.setSelectionFilter(solidar::SelectionFilter::Plane);
    marqueeDrag(view, {1.0, 1.0}, {799.0, 599.0});
    CHECK(view.selectedBodyFaces().empty());
    CHECK(view.selectedBodyEdges().empty());
  }

  // Cross-type: selecting edges clears a prior face selection, and selecting
  // faces clears a prior edge selection, so no mixed selection persists.
  {
    solidar::Viewport view;
    view.resize(800, 600);
    view.setBodyShape(boxShape, bodyId, sourceFeatureId);
    view.setSolidVisible(true);

    // Faces first (Any filter selects faces), then edges.
    marqueeDrag(view, {1.0, 1.0}, {799.0, 599.0});
    CHECK(!view.selectedBodyFaces().empty());
    view.setSelectionFilter(solidar::SelectionFilter::Edge);
    marqueeDrag(view, {1.0, 1.0}, {799.0, 599.0});
    CHECK(!view.selectedBodyEdges().empty());
    CHECK(view.selectedBodyFaces().empty());

    // Edges first, then faces.
    view.setSelectionFilter(solidar::SelectionFilter::Edge);
    marqueeDrag(view, {1.0, 1.0}, {799.0, 599.0});
    CHECK(!view.selectedBodyEdges().empty());
    view.setSelectionFilter(solidar::SelectionFilter::Face);
    marqueeDrag(view, {1.0, 1.0}, {799.0, 599.0});
    CHECK(!view.selectedBodyFaces().empty());
    CHECK(view.selectedBodyEdges().empty());
  }

  // Cancel marquee preserves the pre-drag selection. Pressing on empty area no
  // longer clears selection; Escape only cancels the marquee.
  {
    solidar::Viewport view;
    view.resize(800, 600);
    view.setBodyShape(boxShape, bodyId, sourceFeatureId);
    view.setSolidVisible(true);
    marqueeDrag(view, {1.0, 1.0}, {799.0, 599.0});
    CHECK(!view.selectedBodyFaces().empty());
    const auto before = view.selectedBodyFaces();
    // Start a marquee on empty area.
    mouseMod(view, QEvent::MouseButtonPress, {1.0, 1.0}, Qt::LeftButton,
             Qt::LeftButton, Qt::NoModifier);
    CHECK(view.marqueeActive());
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&view, &escape);
    CHECK(!view.marqueeActive());
    CHECK(view.selectedBodyFaces() == before);
  }

  return EXIT_SUCCESS;
}
