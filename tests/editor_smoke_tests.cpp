#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariantAnimation>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>

#include "TestGeometryUtils.h"
#include "app/AppSettings.h"
#include "home/HomeWindow.h"
#include "model/ExtrudeFeature.h"
#include "model/TopologyReferenceResolver.h"
#include "ui/MainWindow.h"
#include "ui/SketchCanvas.h"
#include "ui/ViewCube.h"

namespace {

class TestFailure final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition))                                                        \
      throw TestFailure(std::string(__FILE__) + ":" +                        \
                        std::to_string(__LINE__) + ": " #condition);         \
  } while (false)

solidar::SketchEditContext faceContext(const TopoDS_Shape& shape,
                                       double topZ, bool autoProject) {
  const auto face = solidar::test::topPlanarFace(shape, topZ);
  CHECK(face);
  const auto placement = solidar::resolveFacePlacement(shape, *face).placement;
  return {solidar::kInvalidSketchId, placement,
          std::make_shared<TopoDS_Shape>(shape),
          solidar::makeFaceReference(shape, 1, 1, *face), autoProject};
}

std::set<std::size_t> projectedElements(const solidar::SketchCanvas& canvas) {
  std::set<std::size_t> result;
  const auto& sketch = canvas.sketch();
  for (std::size_t index = 0; index < sketch.lines().size(); ++index) {
    const auto& line = sketch.lines()[index];
    if (!line.dashed) continue;
    result.insert(line.elementId);
    CHECK(sketch.isGeometryLocked(sketch.lineId(index)));
  }
  return result;
}

void autoProjectionRegressionTests() {
  // A/C: only a new face-supported sketch receives the rectangular boundary.
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape();
  solidar::SketchCanvas rectangular;
  rectangular.resize(900, 650);
  rectangular.setSketchEditContext(faceContext(box, 20.0, true));
  CHECK(rectangular.hasRealReferenceBody());
  CHECK(rectangular.referenceFaceEdgeCount() == 4);
  CHECK(rectangular.sketch().lines().size() == 4);
  CHECK(projectedElements(rectangular).size() == 4);
  CHECK(rectangular.sketch().constraints().size() == 4);
  CHECK(!rectangular.canUndo());
  const QPixmap renderedCanvas = rectangular.grab();
  CHECK(!renderedCanvas.isNull());

  // The complete scene is visible in Sketcher, but only the support face was
  // auto-projected. Edges from another body remain available for an explicit
  // Projection command.
  const auto secondBody = std::make_shared<TopoDS_Shape>(
      BRepPrimAPI_MakeBox(gp_Pnt(60.0, 0.0, 0.0), 12.0, 10.0, 10.0).Shape());
  solidar::sketch::Sketch existingSketch;
  existingSketch.addLine({-10.0, -8.0}, {-2.0, -8.0});
  solidar::SketchCanvas completeScene;
  completeScene.resize(900, 650);
  completeScene.setSketchEditContext(faceContext(box, 20.0, true));
  const auto supportEdgeCount = completeScene.referenceBodyEdgeCount();
  const auto autoProjectedBeforeScene = projectedElements(completeScene);
  completeScene.setSceneReferences(
      solidar::SketchPlacement::xy(), {secondBody},
      {{existingSketch, solidar::SketchPlacement::xy()}});
  CHECK(completeScene.sceneBodyCount() == 2);
  CHECK(completeScene.sceneSketchCount() == 1);
  CHECK(projectedElements(completeScene) == autoProjectedBeforeScene);
  CHECK(completeScene.referenceBodyEdgeCount() > supportEdgeCount);
  bool projectedSecondBody = false;
  for (std::size_t edge = supportEdgeCount;
       edge < completeScene.referenceBodyEdgeCount(); ++edge) {
    if (!completeScene.projectReferenceEdge(edge)) continue;
    projectedSecondBody = true;
    break;
  }
  CHECK(projectedSecondBody);
  CHECK(projectedElements(completeScene).size() ==
        autoProjectedBeforeScene.size() + 1);
  completeScene.undo();
  CHECK(projectedElements(completeScene) == autoProjectedBeforeScene);
  CHECK(!completeScene.grab().isNull());

  const auto automaticLineCount = rectangular.sketch().lines().size();
  for (std::size_t edge = 0; edge < rectangular.referenceBodyEdgeCount();
       ++edge)
    CHECK(!rectangular.projectReferenceEdge(edge));
  CHECK(rectangular.sketch().lines().size() == automaticLineCount);
  CHECK(projectedElements(rectangular).size() == 4);
  CHECK(!rectangular.canUndo());

  solidar::SketchCanvas datum;
  datum.setSketchEditContext({solidar::kInvalidSketchId,
                              solidar::SketchPlacement::xy(),
                              std::make_shared<TopoDS_Shape>(box),
                              std::nullopt, true});
  CHECK(datum.sketch().lines().empty());
  CHECK(datum.sketch().constraints().empty());

  // B: every edge of both the outer wire and the circular inner wire is used.
  const TopoDS_Shape cylinder =
      BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(20.0, 15.0, 0.0),
                                      gp_Dir(0.0, 0.0, 1.0)),
                               5.0, 20.0)
          .Shape();
  BRepAlgoAPI_Cut cut(box, cylinder);
  cut.Build();
  CHECK(cut.IsDone());
  solidar::SketchCanvas withHole;
  withHole.setSketchEditContext(faceContext(cut.Shape(), 20.0, true));
  CHECK(withHole.referenceFaceEdgeCount() == 5);
  // The four straight support edges project as lines; the circular hole edge
  // projects as a single native circle, never a low-poly segment chain.
  CHECK(withHole.sketch().lines().size() == 4);
  CHECK(withHole.sketch().circles().size() == 1);
  CHECK(withHole.sketch().circles().front().dashed);
  CHECK(std::abs(withHole.sketch().circles().front().radiusMm - 5.0) <= 1e-6);
  CHECK(projectedElements(withHole).size() == 4);
  CHECK(withHole.sketch().constraints().size() == 5);
  CHECK(!withHole.canUndo());

  // D: configuring an existing face sketch does not add another boundary.
  solidar::sketch::Sketch saved = rectangular.sketch();
  saved.addLine({5.0, 5.0}, {10.0, 5.0});
  const auto savedLineCount = saved.lines().size();
  const auto savedConstraintCount = saved.constraints().size();
  rectangular.setSketchEditContext(faceContext(box, 20.0, false));
  rectangular.loadSketch(saved);
  CHECK(rectangular.sketch().lines().size() == savedLineCount);
  CHECK(rectangular.sketch().constraints().size() == savedConstraintCount);
  CHECK(projectedElements(rectangular).size() == 4);

  // E/F: support edges remain duplicate-guarded, while another body edge is
  // still manually projectable and creates exactly one undo entry.
  const TopoDS_Shape lowerExtension =
      BRepPrimAPI_MakeBox(gp_Pnt(45.0, 0.0, 0.0), 10.0, 10.0, 10.0).Shape();
  BRepAlgoAPI_Fuse fuse(box, lowerExtension);
  fuse.Build();
  CHECK(fuse.IsDone());
  solidar::SketchCanvas manual;
  manual.setSketchEditContext(faceContext(fuse.Shape(), 20.0, true));
  const auto automaticElements = projectedElements(manual);
  const auto initialManualLineCount = manual.sketch().lines().size();
  bool externalProjected = false;
  for (std::size_t edge = 0; edge < manual.referenceBodyEdgeCount(); ++edge) {
    if (!manual.projectReferenceEdge(edge)) continue;
    externalProjected = true;
    break;
  }
  CHECK(externalProjected);
  CHECK(projectedElements(manual).size() == automaticElements.size() + 1);
  CHECK(manual.sketch().lines().size() > initialManualLineCount);
  CHECK(manual.canUndo());
  manual.undo();
  CHECK(projectedElements(manual) == automaticElements);
  CHECK(manual.sketch().lines().size() == initialManualLineCount);
  CHECK(!manual.canUndo());
}

void sketchFreeCameraTests() {
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape();
  const auto secondBody = std::make_shared<TopoDS_Shape>(
      BRepPrimAPI_MakeBox(gp_Pnt(55.0, 0.0, 0.0), 10.0, 12.0, 15.0)
          .Shape());
  solidar::SketchCanvas canvas;
  canvas.resize(900, 650);
  canvas.setSketchEditContext(faceContext(box, 20.0, false));
  canvas.setSceneReferences(solidar::SketchPlacement::xy(), {secondBody}, {});
  canvas.show();
  QApplication::processEvents();

  CHECK(canvas.viewAlignedToSketchPlane());
  CHECK(canvas.sceneBodyCount() == 2);

  const QPointF start(420.0, 310.0);
  const QPointF finish(480.0, 350.0);
  QMouseEvent press(QEvent::MouseButtonPress, start, Qt::MiddleButton,
                    Qt::MiddleButton, Qt::ShiftModifier);
  QApplication::sendEvent(&canvas, &press);
  QMouseEvent move(QEvent::MouseMove, finish, Qt::NoButton,
                   Qt::MiddleButton, Qt::ShiftModifier);
  QApplication::sendEvent(&canvas, &move);
  QMouseEvent release(QEvent::MouseButtonRelease, finish, Qt::MiddleButton,
                      Qt::NoButton, Qt::ShiftModifier);
  QApplication::sendEvent(&canvas, &release);

  CHECK(!canvas.viewAlignedToSketchPlane());
  CHECK(std::abs(canvas.viewYawDegrees()) > 1.0);
  CHECK(std::abs(canvas.viewPitchDegrees()) > 1.0);
  CHECK(!canvas.grab().isNull());

  canvas.rotateViewClockwise();
  CHECK(canvas.viewQuarterTurns() == 1);
  canvas.resetViewRotation();
  CHECK(canvas.viewAlignedToSketchPlane());
  CHECK(canvas.viewQuarterTurns() == 0);
  CHECK(std::abs(canvas.viewYawDegrees()) <= 1e-9);
  CHECK(std::abs(canvas.viewPitchDegrees()) <= 1e-9);

  // The cube arrows roll the sketch plane in 45-degree increments. This must
  // remain an editable sketch-plane view rather than entering inspection mode.
  const auto rotationCube = solidar::viewCubeGeometry(
      canvas.size(), solidar::orientationForDirection({0.0, 0.0, 1.0}));
  QMouseEvent rotatePress(QEvent::MouseButtonPress,
                          rotationCube.rotateClockwise.center(),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&canvas, &rotatePress);
  QMouseEvent rotateRelease(QEvent::MouseButtonRelease,
                            rotationCube.rotateClockwise.center(),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&canvas, &rotateRelease);
  auto* rotationAnimation = canvas.findChild<QVariantAnimation*>(
      QStringLiteral("sketchViewOrientationTransition"));
  CHECK(rotationAnimation != nullptr);
  CHECK(rotationAnimation->state() == QAbstractAnimation::Running);
  rotationAnimation->setCurrentTime(200);
  CHECK(std::abs(canvas.viewRotationDegrees() - 45.0) <= 1e-9);
  CHECK(canvas.viewAlignedToSketchPlane());
  canvas.resetViewRotation();

  // The Sketcher consumes the same cube geometry as the 3D viewport. Clicking
  // one of its bevels animates to that edge's 45-degree view.
  auto* cubeAnimation = canvas.findChild<QVariantAnimation*>(
      QStringLiteral("sketchViewOrientationTransition"));
  CHECK(cubeAnimation != nullptr);
  CHECK(cubeAnimation->duration() == 200);
  const auto alignedCube = solidar::viewCubeGeometry(
      canvas.size(), solidar::orientationForDirection({0.0, 0.0, 1.0}));
  const auto edgeIt = std::find_if(
      alignedCube.patches.begin(), alignedCube.patches.end(),
      [](const solidar::ViewCubePatch& patch) {
        return patch.hit.zone == solidar::ViewCubeZone::Edge;
      });
  CHECK(edgeIt != alignedCube.patches.end());
  QPointF edgePoint;
  for (const QPointF point : edgeIt->polygon) edgePoint += point;
  edgePoint /= edgeIt->polygon.size();
  QMouseEvent cubePress(QEvent::MouseButtonPress, edgePoint, Qt::LeftButton,
                         Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&canvas, &cubePress);
  QMouseEvent cubeRelease(QEvent::MouseButtonRelease, edgePoint,
                           Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&canvas, &cubeRelease);
  CHECK(cubeAnimation->state() == QAbstractAnimation::Running);
  cubeAnimation->setCurrentTime(200);
  const auto direction = edgeIt->hit.direction;
  const double directionLength = std::sqrt(direction.x * direction.x +
                                           direction.y * direction.y +
                                           direction.z * direction.z);
  const double expectedYaw =
      std::atan2(-direction.x, direction.z) * 180.0 / std::numbers::pi;
  const double expectedPitch =
      -std::asin(direction.y / directionLength) * 180.0 / std::numbers::pi;
  CHECK(std::abs(canvas.viewYawDegrees() - expectedYaw) <= 1e-4);
  CHECK(std::abs(canvas.viewPitchDegrees() - expectedPitch) <= 1e-4);
  CHECK(!canvas.viewAlignedToSketchPlane());
  canvas.resetViewRotation();

  // Match the ordinary 3D viewport gesture: right-button dragging orbits the
  // Sketcher camera without requiring a keyboard modifier.
  QMouseEvent rightPress(QEvent::MouseButtonPress, start, Qt::RightButton,
                         Qt::RightButton, Qt::NoModifier);
  QApplication::sendEvent(&canvas, &rightPress);
  QMouseEvent rightMove(QEvent::MouseMove, finish, Qt::NoButton,
                        Qt::RightButton, Qt::NoModifier);
  QApplication::sendEvent(&canvas, &rightMove);
  QMouseEvent rightRelease(QEvent::MouseButtonRelease, finish,
                           Qt::RightButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&canvas, &rightRelease);

  CHECK(!canvas.viewAlignedToSketchPlane());
  CHECK(std::abs(canvas.viewYawDegrees()) > 1.0);
  CHECK(std::abs(canvas.viewPitchDegrees()) > 1.0);
  canvas.resetViewRotation();

  // A short right click remains a cancellation gesture and must not move the
  // camera by itself.
  QMouseEvent clickPress(QEvent::MouseButtonPress, start, Qt::RightButton,
                         Qt::RightButton, Qt::NoModifier);
  QApplication::sendEvent(&canvas, &clickPress);
  QMouseEvent clickRelease(QEvent::MouseButtonRelease, start,
                           Qt::RightButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&canvas, &clickRelease);
  CHECK(canvas.viewAlignedToSketchPlane());

  // Cube labels and tilt are expressed in world coordinates.  An XZ sketch
  // is viewed from the global Front, not from Top.  Vertical orbit is mapped
  // with the opposite camera sign so the cube follows the visible model
  // instead of rotating in the opposite direction.
  const auto viewingDirection = [](solidar::CameraOrientation camera) {
    const double yaw = camera.yaw * std::numbers::pi / 180.0;
    const double pitch = camera.pitch * std::numbers::pi / 180.0;
    return solidar::Point3d{std::sin(yaw) * std::sin(pitch),
                            std::cos(yaw) * std::sin(pitch),
                            std::cos(pitch)};
  };
  canvas.setSceneReferences(solidar::SketchPlacement::xz(), {}, {});
  canvas.resetViewRotation();
  const auto frontDirection = viewingDirection(canvas.viewCubeCamera());
  CHECK(std::abs(frontDirection.x) <= 1e-6);
  CHECK(std::abs(frontDirection.y + 1.0) <= 1e-6);
  CHECK(std::abs(frontDirection.z) <= 1e-6);

  canvas.orbitView(0.0, 20.0);
  const auto sketchTilt = viewingDirection(canvas.viewCubeCamera());
  CHECK(std::abs(sketchTilt.x) <= 1e-6);
  CHECK(sketchTilt.y < -0.9);
  CHECK(sketchTilt.z < -0.3);

  canvas.orbitView(0.0, -40.0);
  const auto oppositeTilt = viewingDirection(canvas.viewCubeCamera());
  CHECK(std::abs(oppositeTilt.x) <= 1e-6);
  CHECK(oppositeTilt.y < -0.9);
  CHECK(oppositeTilt.z > 0.3);
}

void sketchConstraintToolTests() {
  constexpr double centerX = 44.0 + (900.0 - 44.0) * 0.5;
  constexpr double centerY = 30.0 + (650.0 - 30.0) * 0.5;
  const auto screenPoint = [](double xMm, double yMm) {
    return QPointF(centerX + xMm * 5.0, centerY - yMm * 5.0);
  };
  const auto move = [](solidar::SketchCanvas& canvas, QPointF at) {
    QMouseEvent event(QEvent::MouseMove, at, Qt::NoButton,
                      Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
  };
  const auto click = [](solidar::SketchCanvas& canvas, QPointF at) {
    QMouseEvent press(QEvent::MouseButtonPress, at, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &release);
  };
  const auto hasConstraint = [](const solidar::sketch::Sketch& sketch,
                                solidar::sketch::ConstraintType type) {
    return std::any_of(sketch.constraints().begin(),
                       sketch.constraints().end(),
                       [type](const auto& constraint) {
                         return constraint.type == type;
                       });
  };

  // Exercise the actual two-click UI path reported by the user. Both tools
  // must create a persistent relation, not merely show a success message.
  for (const auto [tool, type] : {
           std::pair{solidar::SketchCanvas::Tool::ParallelConstraint,
                     solidar::sketch::ConstraintType::Parallel},
           std::pair{solidar::SketchCanvas::Tool::PerpendicularConstraint,
                     solidar::sketch::ConstraintType::Perpendicular}}) {
    solidar::sketch::Sketch sketch;
    sketch.addLine({-30.0, -10.0}, {-10.0, -4.0});
    sketch.addLine({10.0, 5.0}, {25.0, 22.0});
    solidar::SketchCanvas canvas;
    canvas.resize(900, 650);
    canvas.loadSketch(sketch);
    canvas.setTool(tool);
    canvas.show();
    QApplication::processEvents();

    const QPointF first = screenPoint(-20.0, -7.0);
    const QPointF second = screenPoint(17.5, 13.5);
    move(canvas, first);
    CHECK(canvas.property("constraintHoverKind").toInt() == 1);
    click(canvas, first);
    move(canvas, second);
    CHECK(canvas.property("constraintHoverKind").toInt() == 1);
    click(canvas, second);
    CHECK(hasConstraint(canvas.sketch(), type));
  }

  // Tangency accepts either click order and highlights only a compatible
  // second operand. This also covers the curved-object hover rendering path.
  {
    solidar::sketch::Sketch sketch;
    sketch.addLine({-30.0, 0.0}, {30.0, 0.0});
    sketch.addCircle({0.0, 16.0}, 5.0);
    solidar::SketchCanvas canvas;
    canvas.resize(900, 650);
    canvas.loadSketch(sketch);
    canvas.setTool(solidar::SketchCanvas::Tool::TangentConstraint);
    canvas.show();
    QApplication::processEvents();

    move(canvas, screenPoint(0.0, 0.0));
    CHECK(canvas.property("constraintHoverKind").toInt() == 1);
    click(canvas, screenPoint(0.0, 0.0));
    move(canvas, screenPoint(5.0, 16.0));
    CHECK(canvas.property("constraintHoverKind").toInt() == 2);
    click(canvas, screenPoint(5.0, 16.0));
    CHECK(hasConstraint(canvas.sketch(),
                        solidar::sketch::ConstraintType::Tangent));
  }

  // Every remaining constraint tool advertises a valid target before the
  // click. Coincident prioritises a vertex; Lock also supports finite arcs.
  {
    solidar::sketch::Sketch sketch;
    sketch.addLine({-30.0, 0.0}, {-10.0, 7.0});
    sketch.addCircle({10.0, 12.0}, 5.0);
    sketch.addArc({30.0, 0.0}, 8.0, 0.0, std::numbers::pi);
    solidar::SketchCanvas canvas;
    canvas.resize(900, 650);
    canvas.loadSketch(sketch);
    canvas.show();
    QApplication::processEvents();

    for (const auto tool : {
             solidar::SketchCanvas::Tool::OrthogonalConstraint,
             solidar::SketchCanvas::Tool::EqualConstraint,
             solidar::SketchCanvas::Tool::ParallelConstraint,
             solidar::SketchCanvas::Tool::PerpendicularConstraint}) {
      canvas.setTool(tool);
      move(canvas, screenPoint(-20.0, 3.5));
      CHECK(canvas.property("constraintHoverKind").toInt() == 1);
    }
    canvas.setTool(solidar::SketchCanvas::Tool::CoincidentConstraint);
    move(canvas, screenPoint(-30.0, 0.0));
    CHECK(canvas.property("constraintHoverKind").toInt() == 4);

    canvas.setTool(solidar::SketchCanvas::Tool::AutoDimension);
    move(canvas, screenPoint(-20.0, 3.5));
    CHECK(canvas.property("constraintHoverKind").toInt() == 1);
    move(canvas, screenPoint(0.0, -20.0));
    CHECK(canvas.property("constraintHoverKind").toInt() == 6);

    canvas.setTool(solidar::SketchCanvas::Tool::LockConstraint);
    move(canvas, screenPoint(30.0, 8.0));
    CHECK(canvas.property("constraintHoverKind").toInt() == 3);
    click(canvas, screenPoint(30.0, 8.0));
    CHECK(canvas.sketch().isGeometryLocked(canvas.sketch().arcId(0)));
  }

  // User regression: a new line spans between a projected circle and the
  // body of a projected lower edge. Its numeric length must commit, and the
  // same lower edge must remain selectable for Perpendicular at the shared
  // screen position.
  {
    solidar::sketch::Sketch sketch;
    sketch.addLine({-40.0, -20.0}, {40.0, -20.0});
    sketch.addCircle({0.0, 0.0}, 10.0);
    sketch.addLine({-10.0, 0.0}, {-8.0, -20.0});
    const auto projectionLine = sketch.lineId(0);
    const auto projectionCircle = sketch.circleId(0);
    const auto activeLine = sketch.lineId(1);

    solidar::sketch::Constraint lockLine;
    lockLine.type = solidar::sketch::ConstraintType::Lock;
    lockLine.firstGeometry = projectionLine;
    CHECK(sketch.addConstraint(lockLine) !=
          solidar::sketch::kInvalidConstraintId);
    solidar::sketch::Constraint lockCircle = lockLine;
    lockCircle.id = solidar::sketch::kInvalidConstraintId;
    lockCircle.firstGeometry = projectionCircle;
    CHECK(sketch.addConstraint(lockCircle) !=
          solidar::sketch::kInvalidConstraintId);
    solidar::sketch::Constraint onCircle;
    onCircle.type = solidar::sketch::ConstraintType::PointOnCircle;
    onCircle.firstGeometry = projectionCircle;
    onCircle.secondPoint = {activeLine, true};
    CHECK(sketch.addConstraint(onCircle) !=
          solidar::sketch::kInvalidConstraintId);
    solidar::sketch::Constraint onLine;
    onLine.type = solidar::sketch::ConstraintType::PointOnLine;
    onLine.firstGeometry = projectionLine;
    onLine.secondPoint = {activeLine, false};
    CHECK(sketch.addConstraint(onLine) !=
          solidar::sketch::kInvalidConstraintId);

    solidar::SketchCanvas canvas;
    canvas.resize(900, 650);
    canvas.loadSketch(sketch);
    canvas.setTool(solidar::SketchCanvas::Tool::AutoDimension);
    canvas.show();
    QApplication::processEvents();
    const auto before = canvas.sketch().lines()[
        *canvas.sketch().lineIndex(activeLine)];
    click(canvas, screenPoint((before.start.xMm + before.end.xMm) * 0.5,
                              (before.start.yMm + before.end.yMm) * 0.5));
    auto* input = canvas.findChild<QDoubleSpinBox*>(
        QStringLiteral("primaryDimension"));
    CHECK(input != nullptr);
    CHECK(input->isVisible());
    CHECK(canvas.property("autoDimensionTarget").toString() ==
          QStringLiteral("points"));
    input->setValue(25.0);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(input, &enter);
    CHECK(hasConstraint(canvas.sketch(),
                        solidar::sketch::ConstraintType::Distance));

    canvas.setTool(
        solidar::SketchCanvas::Tool::PerpendicularConstraint);
    const auto sized = canvas.sketch().lines()[
        *canvas.sketch().lineIndex(activeLine)];
    click(canvas, screenPoint((sized.start.xMm + sized.end.xMm) * 0.5,
                              (sized.start.yMm + sized.end.yMm) * 0.5));
    move(canvas, screenPoint(sized.end.xMm, sized.end.yMm));
    CHECK(canvas.property("constraintHoverGeometry").toULongLong() ==
          projectionLine);
    click(canvas, screenPoint(sized.end.xMm, sized.end.yMm));
    CHECK(hasConstraint(canvas.sketch(),
                        solidar::sketch::ConstraintType::Perpendicular));
  }

  // Exact UI order from the reported projected-geometry case:
  // Perpendicular first, then a direct-line AutoDimension that requires the
  // line to slide along the lower projection. Tangency is checked separately
  // from the pre-dimension state because the chosen 25 mm branch intersects
  // the circle but is not itself tangent.
  {
    solidar::sketch::Sketch sketch;
    sketch.addLine({-40.0, -35.0}, {40.0, -35.0});
    sketch.addCircle({0.0, 0.0}, 10.0);
    sketch.addLine({-8.0, -35.0}, {-8.0, -6.0});
    const auto projectionLine = sketch.lineId(0);
    const auto projectionCircle = sketch.circleId(0);
    const auto activeLine = sketch.lineId(1);

    solidar::sketch::Constraint constraint;
    constraint.type = solidar::sketch::ConstraintType::Lock;
    constraint.firstGeometry = projectionLine;
    CHECK(sketch.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    constraint.id = solidar::sketch::kInvalidConstraintId;
    constraint.firstGeometry = projectionCircle;
    CHECK(sketch.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    constraint = {};
    constraint.type = solidar::sketch::ConstraintType::PointOnLine;
    constraint.firstGeometry = projectionLine;
    constraint.secondPoint = {activeLine, true};
    CHECK(sketch.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    constraint = {};
    constraint.type = solidar::sketch::ConstraintType::PointOnCircle;
    constraint.firstGeometry = projectionCircle;
    constraint.secondPoint = {activeLine, false};
    CHECK(sketch.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    constraint = {};
    constraint.type = solidar::sketch::ConstraintType::Perpendicular;
    constraint.firstGeometry = projectionLine;
    constraint.secondGeometry = activeLine;
    CHECK(sketch.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);

    const auto beforeDimension = sketch;
    solidar::SketchCanvas dimensionCanvas;
    dimensionCanvas.resize(900, 650);
    dimensionCanvas.loadSketch(sketch);
    dimensionCanvas.setTool(solidar::SketchCanvas::Tool::AutoDimension);
    dimensionCanvas.show();
    QApplication::processEvents();
    click(dimensionCanvas, screenPoint(-8.0, -20.5));
    auto* input = dimensionCanvas.findChild<QDoubleSpinBox*>(
        QStringLiteral("primaryDimension"));
    CHECK(input != nullptr);
    CHECK(input->isVisible());
    input->setValue(25.0);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(input, &enter);
    CHECK(!input->isVisible());
    CHECK(hasConstraint(dimensionCanvas.sketch(),
                        solidar::sketch::ConstraintType::Distance));
    const auto& sized = dimensionCanvas.sketch().lines()[
        *dimensionCanvas.sketch().lineIndex(activeLine)];
    CHECK(std::abs(std::hypot(sized.end.xMm - sized.start.xMm,
                              sized.end.yMm - sized.start.yMm) -
                   25.0) <= 1e-4);
    CHECK(std::abs(sized.end.xMm) <= 1e-4);
    CHECK(std::abs(sized.end.yMm + 10.0) <= 1e-4);

    solidar::SketchCanvas tangentCanvas;
    tangentCanvas.resize(900, 650);
    tangentCanvas.loadSketch(beforeDimension);
    tangentCanvas.setTool(solidar::SketchCanvas::Tool::TangentConstraint);
    tangentCanvas.show();
    QApplication::processEvents();
    click(tangentCanvas, screenPoint(-8.0, -20.5));
    move(tangentCanvas, screenPoint(10.0, 0.0));
    CHECK(tangentCanvas.property("constraintHoverGeometry").toULongLong() ==
          projectionCircle);
    click(tangentCanvas, screenPoint(10.0, 0.0));
    CHECK(hasConstraint(tangentCanvas.sketch(),
                        solidar::sketch::ConstraintType::Tangent));
    const auto& tangent = tangentCanvas.sketch().lines()[
        *tangentCanvas.sketch().lineIndex(activeLine)];
    CHECK(std::abs(std::abs(tangent.start.xMm) - 10.0) <= 1e-4);
    CHECK(std::abs(tangent.end.yMm) <= 1e-4);
  }

  // The exact combined UI workflow must keep every constraint: make the
  // line tangent first, then set its visible angle to 90 degrees through
  // AutoDimension, then apply the direct 25 mm line size.
  {
    solidar::sketch::Sketch sketch;
    sketch.addLine({-40.0, -25.0}, {40.0, -25.0});
    sketch.addCircle({0.0, 0.0}, 10.0);
    sketch.addLine({-8.0, -25.0}, {-8.0, -6.0});
    const auto projectionLine = sketch.lineId(0);
    const auto projectionCircle = sketch.circleId(0);
    const auto activeLine = sketch.lineId(1);

    solidar::sketch::Constraint constraint;
    constraint.type = solidar::sketch::ConstraintType::Lock;
    constraint.firstGeometry = projectionLine;
    CHECK(sketch.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    constraint.id = solidar::sketch::kInvalidConstraintId;
    constraint.firstGeometry = projectionCircle;
    CHECK(sketch.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    constraint = {};
    constraint.type = solidar::sketch::ConstraintType::PointOnLine;
    constraint.firstGeometry = projectionLine;
    constraint.secondPoint = {activeLine, true};
    CHECK(sketch.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);
    constraint = {};
    constraint.type = solidar::sketch::ConstraintType::PointOnCircle;
    constraint.firstGeometry = projectionCircle;
    constraint.secondPoint = {activeLine, false};
    CHECK(sketch.addConstraint(constraint) !=
          solidar::sketch::kInvalidConstraintId);

    solidar::SketchCanvas canvas;
    canvas.resize(900, 650);
    canvas.loadSketch(sketch);
    canvas.show();
    QApplication::processEvents();

    canvas.setTool(solidar::SketchCanvas::Tool::TangentConstraint);
    click(canvas, screenPoint(-8.0, -15.5));
    click(canvas, screenPoint(10.0, 0.0));
    CHECK(hasConstraint(canvas.sketch(),
                        solidar::sketch::ConstraintType::Tangent));

    canvas.setTool(solidar::SketchCanvas::Tool::AutoDimension);
    const auto tangent = canvas.sketch().lines()[
        *canvas.sketch().lineIndex(activeLine)];
    click(canvas,
          screenPoint((tangent.start.xMm + tangent.end.xMm) * 0.5,
                      (tangent.start.yMm + tangent.end.yMm) * 0.5));
    click(canvas, screenPoint(20.0, -25.0));
    CHECK(canvas.property("autoDimensionTarget").toString() ==
          QStringLiteral("angle"));
    auto* input = canvas.findChild<QDoubleSpinBox*>(
        QStringLiteral("primaryDimension"));
    CHECK(input != nullptr);
    CHECK(input->isVisible());
    input->setValue(90.0);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(input, &enter);
    CHECK(!input->isVisible());
    CHECK(hasConstraint(canvas.sketch(),
                        solidar::sketch::ConstraintType::Angle));
    CHECK(hasConstraint(canvas.sketch(),
                        solidar::sketch::ConstraintType::Tangent));

    canvas.setTool(solidar::SketchCanvas::Tool::AutoDimension);
    const auto perpendicular = canvas.sketch().lines()[
        *canvas.sketch().lineIndex(activeLine)];
    click(canvas,
          screenPoint((perpendicular.start.xMm + perpendicular.end.xMm) * 0.5,
                      (perpendicular.start.yMm + perpendicular.end.yMm) *
                          0.5));
    CHECK(input->isVisible());
    input->setValue(25.0);
    QApplication::sendEvent(input, &enter);
    CHECK(!input->isVisible());
    CHECK(hasConstraint(canvas.sketch(),
                        solidar::sketch::ConstraintType::Distance));
    CHECK(hasConstraint(canvas.sketch(),
                        solidar::sketch::ConstraintType::Angle));
    CHECK(hasConstraint(canvas.sketch(),
                        solidar::sketch::ConstraintType::Tangent));
    const auto& result = canvas.sketch().lines()[
        *canvas.sketch().lineIndex(activeLine)];
    CHECK(std::abs(std::hypot(result.end.xMm - result.start.xMm,
                              result.end.yMm - result.start.yMm) -
                   25.0) <= 1e-4);
    CHECK(std::abs(std::abs(result.start.xMm) - 10.0) <= 1e-4);
    CHECK(std::abs(result.end.yMm) <= 1e-4);
  }
}

void circularEdgeProjectionTests() {
  // A full circular edge (a planar disk) projects as exactly one native
  // Sketch Circle, with its radius preserved, no segment approximations, and
  // a Lock constraint.
  const gp_Circ circle(gp_Ax2(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0)),
                       5.0);
  const TopoDS_Shape disk =
      BRepBuilderAPI_MakeFace(
          BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(circle).Edge())
              .Wire())
          .Shape();
  solidar::SketchCanvas canvas;
  canvas.resize(900, 650);
  canvas.setSketchEditContext(faceContext(disk, 0.0, true));
  CHECK(canvas.hasRealReferenceBody());
  CHECK(canvas.referenceFaceEdgeCount() == 1);
  CHECK(canvas.sketch().lines().empty());
  CHECK(canvas.sketch().circles().size() == 1);
  CHECK(canvas.sketch().arcs().empty());
  const auto& projected = canvas.sketch().circles().front();
  CHECK(projected.dashed);
  CHECK(std::abs(projected.radiusMm - 5.0) <= 1e-6);
  CHECK(std::abs(projected.center.xMm) <= 1e-6);
  CHECK(std::abs(projected.center.yMm) <= 1e-6);
  CHECK(canvas.sketch().constraints().size() == 1);
  CHECK(canvas.sketch().isGeometryLocked(canvas.sketch().circleId(0)));

  // Manual projection is duplicate-guarded: the already-projected circle is
  // not added a second time.
  const auto circleCountBefore = canvas.sketch().circles().size();
  for (std::size_t edge = 0; edge < canvas.referenceBodyEdgeCount(); ++edge)
    (void)canvas.projectReferenceEdge(edge);
  CHECK(canvas.sketch().circles().size() == circleCountBefore);

  // Exercise the actual Projection tool path. Curved reference edges must be
  // hit-testable with the mouse, not only projectable through the index API.
  solidar::SketchCanvas manual;
  manual.resize(900, 650);
  manual.setSketchEditContext(faceContext(disk, 0.0, false));
  manual.setTool(solidar::SketchCanvas::Tool::Projection);
  manual.show();
  QApplication::processEvents();

  constexpr double centerX = 44.0 + (900.0 - 44.0) * 0.5;
  constexpr double centerY = 30.0 + (650.0 - 30.0) * 0.5;
  constexpr double pixelsPerMm = 0.82 * (650.0 - 30.0 - 50.0) / 10.0;
  const QPointF circlePoint(centerX + 5.0 * pixelsPerMm, centerY);
  QMouseEvent move(QEvent::MouseMove, circlePoint, Qt::NoButton,
                   Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&manual, &move);
  QMouseEvent press(QEvent::MouseButtonPress, circlePoint, Qt::LeftButton,
                    Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&manual, &press);

  CHECK(manual.sketch().circles().size() == 1);
  CHECK(manual.sketch().circles().front().dashed);
  CHECK(manual.sketch().isGeometryLocked(manual.sketch().circleId(0)));

  // The same interaction must work for a trimmed circular edge (Arc).
  const TopoDS_Edge upperArc =
      BRepBuilderAPI_MakeEdge(circle, 0.0, std::numbers::pi).Edge();
  const TopoDS_Edge lowerArc =
      BRepBuilderAPI_MakeEdge(circle, std::numbers::pi,
                              2.0 * std::numbers::pi)
          .Edge();
  const TopoDS_Shape splitDisk =
      BRepBuilderAPI_MakeFace(
          BRepBuilderAPI_MakeWire(upperArc, lowerArc).Wire())
          .Shape();
  solidar::SketchCanvas arcCanvas;
  arcCanvas.resize(900, 650);
  arcCanvas.setSketchEditContext(faceContext(splitDisk, 0.0, false));
  arcCanvas.setTool(solidar::SketchCanvas::Tool::Projection);
  arcCanvas.show();
  QApplication::processEvents();

  const QPointF arcPoint(centerX, centerY - 5.0 * pixelsPerMm);
  QMouseEvent arcPress(QEvent::MouseButtonPress, arcPoint, Qt::LeftButton,
                       Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&arcCanvas, &arcPress);
  CHECK(arcCanvas.sketch().arcs().size() == 1);
  CHECK(arcCanvas.sketch().arcs().front().dashed);
  CHECK(arcCanvas.sketch().isGeometryLocked(arcCanvas.sketch().arcId(0)));
}

void arcBodySelectionAndDragTests() {
  solidar::sketch::Sketch sketch;
  constexpr double almostFullSweep = 2.0 * std::numbers::pi - 1e-6;
  // A large-radius arc exposes hit tests that approximate the curve with a
  // fixed number of straight segments. Keep the visible part near the sketch
  // origin while its center remains far below the viewport.
  sketch.addArc({0.0, -1300.0}, 1300.0, 0.0, almostFullSweep);

  solidar::SketchCanvas canvas;
  canvas.resize(900, 650);
  canvas.loadSketch(sketch);
  canvas.setTool(solidar::SketchCanvas::Tool::Select);
  canvas.setSnapEnabled(false);
  canvas.show();
  QApplication::processEvents();

  // Click halfway between two vertices of the former 48-segment hit-test.
  // The point is exactly on the analytic arc but more than nine pixels from
  // that coarse polyline. SketchCanvas starts at 5 px/mm and centers the
  // sketch inside the ruler margins (44 px left, 30 px top).
  constexpr double centerX = 44.0 + (900.0 - 44.0) * 0.5;
  constexpr double centerY = 30.0 + (650.0 - 30.0) * 0.5;
  constexpr double hitAngle = almostFullSweep * 12.5 / 48.0;
  const double hitX = 1300.0 * std::cos(hitAngle);
  const double hitY = -1300.0 + 1300.0 * std::sin(hitAngle);
  const QPointF arcBody(centerX + hitX * 5.0,
                        centerY - hitY * 5.0);
  const QPointF draggedTo = arcBody + QPointF(30.0, 20.0);

  QString selectionDescription;
  QObject::connect(&canvas, &solidar::SketchCanvas::selectionChanged,
                   [&selectionDescription](const QString& description) {
                     selectionDescription = description;
                   });

  QMouseEvent press(QEvent::MouseButtonPress, arcBody, Qt::LeftButton,
                    Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&canvas, &press);
  CHECK(selectionDescription.contains(QString::fromUtf8("дуга"),
                                      Qt::CaseInsensitive));

  QMouseEvent move(QEvent::MouseMove, draggedTo, Qt::NoButton,
                   Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&canvas, &move);
  QMouseEvent release(QEvent::MouseButtonRelease, draggedTo, Qt::LeftButton,
                      Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&canvas, &release);

  CHECK(canvas.sketch().arcs().size() == 1);
  const auto& moved = canvas.sketch().arcs().front();
  CHECK(std::abs(moved.center.xMm - 6.0) <= 1e-6);
  CHECK(std::abs(moved.center.yMm + 1304.0) <= 1e-6);
}

void draggedPointSnappingTests() {
  constexpr double kPi = std::numbers::pi;
  constexpr double centerX = 44.0 + (900.0 - 44.0) * 0.5;
  constexpr double centerY = 30.0 + (650.0 - 30.0) * 0.5;
  const auto screenPoint = [](double xMm, double yMm) {
    return QPointF(centerX + xMm * 5.0, centerY - yMm * 5.0);
  };
  const auto drag = [](solidar::SketchCanvas& canvas, QPointF from,
                       QPointF to) {
    QMouseEvent press(QEvent::MouseButtonPress, from, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &press);
    QMouseEvent move(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton,
                     Qt::NoModifier);
    QApplication::sendEvent(&canvas, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, to, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &release);
  };
  const auto click = [](solidar::SketchCanvas& canvas, QPointF at) {
    QMouseEvent press(QEvent::MouseButtonPress, at, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &release);
  };

  // Arc construction prioritises an existing CAD vertex over the grid and
  // persists both chord endpoints as Coincident constraints. The rectangle
  // coordinates are intentionally off-grid so equality cannot be accidental.
  {
    solidar::sketch::Sketch sketch;
    sketch.addRectangle({1.3, -1.7}, {101.3, 48.7});

    solidar::SketchCanvas canvas;
    canvas.resize(900, 650);
    canvas.loadSketch(sketch);
    canvas.setTool(solidar::SketchCanvas::Tool::Arc);
    canvas.show();
    QApplication::processEvents();

    click(canvas, screenPoint(2.5, 47.5));
    click(canvas, screenPoint(2.0, -1.0));
    click(canvas, screenPoint(-23.0, 23.5));

    const auto& result = canvas.sketch();
    CHECK(result.arcs().size() == 1);
    const auto arcId = result.arcId(0);
    std::size_t attachedArcEndpoints = 0;
    for (const auto& constraint : result.constraints()) {
      if (constraint.type !=
          solidar::sketch::ConstraintType::Coincident)
        continue;
      if (constraint.firstPoint.arcId == arcId ||
          constraint.secondPoint.arcId == arcId)
        ++attachedArcEndpoints;
    }
    CHECK(attachedArcEndpoints == 2);

    const auto& arc = result.arcs().front();
    const auto start = solidar::sketch::arcStartPoint(arc);
    const auto end = solidar::sketch::arcEndPoint(arc);
    CHECK(std::abs(start.xMm - 1.3) <= 1e-6);
    CHECK(std::abs(start.yMm - 48.7) <= 1e-6);
    CHECK(std::abs(end.xMm - 1.3) <= 1e-6);
    CHECK(std::abs(end.yMm + 1.7) <= 1e-6);
  }

  // A manually dragged line endpoint snaps to an existing Arc endpoint and
  // persists the relationship as Coincident.
  {
    solidar::sketch::Sketch sketch;
    sketch.addLine({-30.0, 0.0}, {-20.0, 0.0});
    sketch.addArc({0.0, 0.0}, 10.0, 0.0, kPi);
    const auto lineId = sketch.lineId(0);
    const auto arcId = sketch.arcId(0);

    solidar::SketchCanvas canvas;
    canvas.resize(900, 650);
    canvas.loadSketch(sketch);
    canvas.setTool(solidar::SketchCanvas::Tool::Select);
    canvas.show();
    QApplication::processEvents();

    drag(canvas, screenPoint(-20.0, 0.0), screenPoint(10.0, 0.0));

    const auto& constraints = canvas.sketch().constraints();
    CHECK(std::any_of(constraints.begin(), constraints.end(),
                      [lineId, arcId](const auto& constraint) {
                        return constraint.type ==
                                   solidar::sketch::ConstraintType::Coincident &&
                               constraint.firstPoint.arcId == arcId &&
                               constraint.firstPoint.start &&
                               constraint.secondPoint.lineId == lineId &&
                               !constraint.secondPoint.start;
                      }));
    canvas.undo();
    CHECK(canvas.sketch().constraints().empty());
    CHECK(std::abs(canvas.sketch().lines().front().end.xMm + 20.0) <= 1e-9);
    CHECK(std::abs(canvas.sketch().lines().front().end.yMm) <= 1e-9);
  }

  // The release coordinate wins over a stale line-body hover. This models a
  // final cursor step delivered together with MouseButtonRelease on Windows.
  {
    solidar::sketch::Sketch sketch;
    sketch.addLine({-20.0, 20.0}, {20.0, 20.0});
    sketch.addArc({0.0, 0.0}, 10.0, 0.0, kPi);
    const auto lineId = sketch.lineId(0);
    const auto arcId = sketch.arcId(0);

    solidar::SketchCanvas canvas;
    canvas.resize(900, 650);
    canvas.loadSketch(sketch);
    canvas.setTool(solidar::SketchCanvas::Tool::Select);
    canvas.show();
    QApplication::processEvents();

    QMouseEvent press(QEvent::MouseButtonPress, screenPoint(10.0, 0.0),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &press);
    QMouseEvent move(QEvent::MouseMove, screenPoint(5.0, 20.0), Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, screenPoint(20.0, 20.0),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &release);

    CHECK(std::any_of(canvas.sketch().constraints().begin(),
                      canvas.sketch().constraints().end(),
                      [lineId, arcId](const auto& constraint) {
                        return constraint.type ==
                                   solidar::sketch::ConstraintType::Coincident &&
                               constraint.firstPoint.lineId == lineId &&
                               !constraint.firstPoint.start &&
                               constraint.secondPoint.arcId == arcId &&
                               constraint.secondPoint.start;
                      }));
  }

  // Arc endpoints use the same drag-snap path. Dropping one on a line body
  // creates PointOnLine instead of leaving two merely overlapping shapes.
  {
    solidar::sketch::Sketch sketch;
    sketch.addLine({-20.0, 20.0}, {20.0, 20.0});
    sketch.addArc({0.0, 0.0}, 10.0, 0.0, kPi);
    const auto lineId = sketch.lineId(0);
    const auto arcId = sketch.arcId(0);

    solidar::SketchCanvas canvas;
    canvas.resize(900, 650);
    canvas.loadSketch(sketch);
    canvas.setTool(solidar::SketchCanvas::Tool::Select);
    canvas.show();
    QApplication::processEvents();

    drag(canvas, screenPoint(10.0, 0.0), screenPoint(5.0, 20.0));

    const auto& constraints = canvas.sketch().constraints();
    CHECK(std::any_of(constraints.begin(), constraints.end(),
                      [lineId, arcId](const auto& constraint) {
                        return constraint.type ==
                                   solidar::sketch::ConstraintType::PointOnLine &&
                               constraint.firstGeometry == lineId &&
                               constraint.secondPoint.arcId == arcId &&
                               constraint.secondPoint.start;
                      }));
  }

  // The merged "Coincident / Point-on" tool accepts an Arc endpoint as the
  // point selected after a line carrier.
  {
    solidar::sketch::Sketch sketch;
    sketch.addLine({-20.0, 20.0}, {20.0, 20.0});
    sketch.addArc({40.0, 20.0}, 10.0, kPi, kPi);
    const auto lineId = sketch.lineId(0);
    const auto arcId = sketch.arcId(0);

    solidar::SketchCanvas canvas;
    canvas.resize(900, 650);
    canvas.loadSketch(sketch);
    canvas.setTool(solidar::SketchCanvas::Tool::CoincidentConstraint);
    canvas.show();
    QApplication::processEvents();

    click(canvas, screenPoint(0.0, 20.0));
    click(canvas, screenPoint(30.0, 20.0));
    CHECK(std::any_of(canvas.sketch().constraints().begin(),
                      canvas.sketch().constraints().end(),
                      [lineId, arcId](const auto& constraint) {
                        return constraint.type ==
                                   solidar::sketch::ConstraintType::PointOnLine &&
                               constraint.firstGeometry == lineId &&
                               constraint.secondPoint.arcId == arcId &&
                               constraint.secondPoint.start;
                      }));
  }

  // Regression: when one Arc endpoint is already attached to a rectangle,
  // Coincident must still be able to attach the other endpoint without the
  // solver translating the whole Arc back and forth.
  {
    solidar::sketch::Sketch sketch;
    sketch.addRectangle({0.0, 0.0}, {100.0, 50.0});
    sketch.addArc({0.0, 27.5}, 27.5, -kPi * 0.5, kPi);
    const auto arcId = sketch.arcId(0);

    solidar::sketch::PointReference arcStart;
    arcStart.arcId = arcId;
    arcStart.start = true;
    solidar::sketch::Constraint first;
    first.type = solidar::sketch::ConstraintType::Coincident;
    first.firstPoint = {sketch.lineId(0), true};
    first.secondPoint = arcStart;
    CHECK(sketch.addConstraint(first) !=
          solidar::sketch::kInvalidConstraintId);

    solidar::SketchCanvas canvas;
    canvas.resize(900, 650);
    canvas.loadSketch(sketch);
    canvas.setTool(solidar::SketchCanvas::Tool::CoincidentConstraint);
    canvas.show();
    QApplication::processEvents();

    click(canvas, screenPoint(0.0, 50.0));
    click(canvas, screenPoint(0.0, 55.0));

    const auto& result = canvas.sketch();
    CHECK(std::any_of(
        result.constraints().begin(), result.constraints().end(),
        [arcId](const auto& constraint) {
          return constraint.type ==
                     solidar::sketch::ConstraintType::Coincident &&
                 ((constraint.firstPoint.arcId == arcId &&
                   !constraint.firstPoint.start) ||
                  (constraint.secondPoint.arcId == arcId &&
                   !constraint.secondPoint.start));
        }));
    solidar::sketch::PointReference arcEnd;
    arcEnd.arcId = arcId;
    arcEnd.start = false;
    const auto endpoint = result.referencedPoint(arcEnd);
    CHECK(endpoint.has_value());
    CHECK(std::abs(endpoint->xMm) <= 1e-6);
    CHECK(std::abs(endpoint->yMm - 50.0) <= 1e-6);
  }

  // Point first, finite Arc body second creates PointOnArc.
  {
    solidar::sketch::Sketch sketch;
    sketch.addLine({-30.0, 0.0}, {-20.0, 0.0});
    sketch.addArc({0.0, 0.0}, 10.0, 0.0, kPi);
    const auto lineId = sketch.lineId(0);
    const auto arcId = sketch.arcId(0);

    solidar::SketchCanvas canvas;
    canvas.resize(900, 650);
    canvas.loadSketch(sketch);
    canvas.setTool(solidar::SketchCanvas::Tool::CoincidentConstraint);
    canvas.show();
    QApplication::processEvents();

    click(canvas, screenPoint(-20.0, 0.0));
    click(canvas, screenPoint(0.0, 10.0));
    CHECK(std::any_of(canvas.sketch().constraints().begin(),
                      canvas.sketch().constraints().end(),
                      [lineId, arcId](const auto& constraint) {
                        return constraint.type ==
                                   solidar::sketch::ConstraintType::PointOnArc &&
                               constraint.firstGeometry == arcId &&
                               constraint.secondPoint.lineId == lineId &&
                               !constraint.secondPoint.start;
                      }));
  }

  // An Arc endpoint located on a line body remains a point hit. The selected
  // line vertex must become Coincident with it instead of becoming PointOnLine
  // with the carrier underneath.
  {
    solidar::sketch::Sketch sketch;
    sketch.addLine({0.0, 20.0}, {0.0, -20.0});
    sketch.addArc({0.0, 0.0}, 10.0, kPi * 0.5, kPi);
    const auto lineId = sketch.lineId(0);
    const auto arcId = sketch.arcId(0);

    solidar::SketchCanvas canvas;
    canvas.resize(900, 650);
    canvas.loadSketch(sketch);
    canvas.setTool(solidar::SketchCanvas::Tool::CoincidentConstraint);
    canvas.show();
    QApplication::processEvents();

    click(canvas, screenPoint(0.0, 20.0));
    click(canvas, screenPoint(0.0, 10.0));
    CHECK(std::any_of(canvas.sketch().constraints().begin(),
                      canvas.sketch().constraints().end(),
                      [lineId, arcId](const auto& constraint) {
                        return constraint.type ==
                                   solidar::sketch::ConstraintType::Coincident &&
                               constraint.firstPoint.lineId == lineId &&
                               constraint.firstPoint.start &&
                               constraint.secondPoint.arcId == arcId &&
                               constraint.secondPoint.start;
                      }));
    CHECK(std::none_of(canvas.sketch().constraints().begin(),
                       canvas.sketch().constraints().end(),
                       [](const auto& constraint) {
                         return constraint.type ==
                                solidar::sketch::ConstraintType::PointOnLine;
                       }));
  }

  // Curved carrier bodies produce persistent semantic constraints as well.
  {
    solidar::sketch::Sketch sketch;
    sketch.addLine({-30.0, 0.0}, {-20.0, 0.0});
    sketch.addCircle({0.0, 0.0}, 10.0);
    const auto lineId = sketch.lineId(0);
    const auto circleId = sketch.circleId(0);

    solidar::SketchCanvas canvas;
    canvas.resize(900, 650);
    canvas.loadSketch(sketch);
    canvas.setTool(solidar::SketchCanvas::Tool::Select);
    canvas.show();
    QApplication::processEvents();

    drag(canvas, screenPoint(-20.0, 0.0), screenPoint(0.0, 10.0));
    const auto& constraints = canvas.sketch().constraints();
    CHECK(std::any_of(constraints.begin(), constraints.end(),
                      [lineId, circleId](const auto& constraint) {
                        return constraint.type ==
                                   solidar::sketch::ConstraintType::PointOnCircle &&
                               constraint.firstGeometry == circleId &&
                               constraint.secondPoint.lineId == lineId &&
                               !constraint.secondPoint.start;
                      }));
  }

  {
    solidar::sketch::Sketch sketch;
    sketch.addLine({-30.0, 0.0}, {-20.0, 0.0});
    sketch.addArc({0.0, 0.0}, 10.0, 0.0, kPi);
    const auto lineId = sketch.lineId(0);
    const auto arcId = sketch.arcId(0);

    solidar::SketchCanvas canvas;
    canvas.resize(900, 650);
    canvas.loadSketch(sketch);
    canvas.setTool(solidar::SketchCanvas::Tool::Select);
    canvas.show();
    QApplication::processEvents();

    drag(canvas, screenPoint(-20.0, 0.0), screenPoint(0.0, 10.0));
    const auto& constraints = canvas.sketch().constraints();
    CHECK(std::any_of(constraints.begin(), constraints.end(),
                      [lineId, arcId](const auto& constraint) {
                        return constraint.type ==
                                   solidar::sketch::ConstraintType::PointOnArc &&
                               constraint.firstGeometry == arcId &&
                               constraint.secondPoint.lineId == lineId &&
                               !constraint.secondPoint.start;
                      }));
  }

  // A circle centre is an editable support point too, not just the
  // circumference of the circle.
  {
    solidar::sketch::Sketch sketch;
    sketch.addLine({-10.0, 20.0}, {20.0, 20.0});
    sketch.addCircle({-20.0, 0.0}, 5.0);
    const auto lineId = sketch.lineId(0);
    const auto circleId = sketch.circleId(0);

    solidar::SketchCanvas canvas;
    canvas.resize(900, 650);
    canvas.loadSketch(sketch);
    canvas.setTool(solidar::SketchCanvas::Tool::Select);
    canvas.show();
    QApplication::processEvents();

    drag(canvas, screenPoint(-20.0, 0.0), screenPoint(10.0, 20.0));
    const auto& constraints = canvas.sketch().constraints();
    CHECK(std::any_of(constraints.begin(), constraints.end(),
                      [lineId, circleId](const auto& constraint) {
                        return constraint.type ==
                                   solidar::sketch::ConstraintType::PointOnLine &&
                               constraint.firstGeometry == lineId &&
                               constraint.secondPoint.circleId == circleId;
                      }));
  }
}

void attachedRectangleArcDragTests() {
  constexpr double kPi = std::numbers::pi;
  solidar::sketch::Sketch sketch;
  sketch.addRectangle({0.0, 0.0}, {100.0, 20.0});
  sketch.addArc({50.0, 20.0}, 50.0, 0.0, kPi);
  const auto topLineId = sketch.lineId(2);
  const auto arcId = sketch.arcId(0);

  solidar::sketch::PointReference arcStart;
  arcStart.arcId = arcId;
  arcStart.start = true;
  solidar::sketch::PointReference arcEnd = arcStart;
  arcEnd.start = false;
  solidar::sketch::Constraint first;
  first.type = solidar::sketch::ConstraintType::Coincident;
  first.firstPoint = {topLineId, true};
  first.secondPoint = arcStart;
  CHECK(sketch.addConstraint(first) != solidar::sketch::kInvalidConstraintId);
  solidar::sketch::Constraint second;
  second.type = solidar::sketch::ConstraintType::Coincident;
  second.firstPoint = {topLineId, false};
  second.secondPoint = arcEnd;
  CHECK(sketch.addConstraint(second) != solidar::sketch::kInvalidConstraintId);

  solidar::SketchCanvas canvas;
  canvas.resize(900, 650);
  canvas.loadSketch(sketch);
  canvas.setTool(solidar::SketchCanvas::Tool::Select);
  canvas.show();
  QApplication::processEvents();

  constexpr double centerX = 44.0 + (900.0 - 44.0) * 0.5;
  constexpr double centerY = 30.0 + (650.0 - 30.0) * 0.5;
  const auto point = [](double xMm, double yMm) {
    return QPointF(centerX + xMm * 5.0, centerY - yMm * 5.0);
  };
  const auto drag = [&canvas](QPointF from, QPointF to) {
    QMouseEvent press(QEvent::MouseButtonPress, from, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &press);
    QMouseEvent move(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton,
                     Qt::NoModifier);
    QApplication::sendEvent(&canvas, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, to, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &release);
  };

  drag(point(50.0, 70.0), point(55.0, 75.0));
  drag(point(50.0, 0.0), point(55.0, 5.0));
  CHECK(canvas.sketch().arcs().size() == 1);
  CHECK(canvas.sketch().lines().size() == 4);
  CHECK(canvas.sketch().constraints().size() >= 2);
}

}  // namespace

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  QTemporaryDir tempDir;
  CHECK(tempDir.isValid());
  solidar::AppSettings settings(tempDir.filePath("settings.ini"));

  autoProjectionRegressionTests();
  sketchFreeCameraTests();
  try {
    sketchConstraintToolTests();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
  circularEdgeProjectionTests();
  arcBodySelectionAndDragTests();
  draggedPointSnappingTests();
  attachedRectangleArcDragTests();

  solidar::home::HomeWindow home(settings);
  solidar::MainWindow* editor = nullptr;
  QObject::connect(&home, &solidar::home::HomeWindow::projectRequested,
                   &application, [&](const QString& path) {
    editor = new solidar::MainWindow(settings);
    editor->setProjectPath(path);
    auto* orientSketchPlane = editor->findChild<QPushButton*>(
        QStringLiteral("sketchOrientToPlaneButton"));
    CHECK(orientSketchPlane != nullptr);
    CHECK(orientSketchPlane->text() ==
          QString::fromUtf8("Вернуть исходную ориентацию"));
    auto* sketchCanvas = editor->findChild<solidar::SketchCanvas*>();
    CHECK(sketchCanvas != nullptr);
    auto* sketchSettingsDock = editor->findChild<QDockWidget*>(
        QStringLiteral("sketchSettingsDock"));
    auto* modelTreeDock = editor->findChild<QDockWidget*>(
        QStringLiteral("modelTreeDock"));
    CHECK(sketchSettingsDock != nullptr);
    CHECK(modelTreeDock != nullptr);
    QStackedWidget* workspace = nullptr;
    for (auto* candidate : editor->findChildren<QStackedWidget*>()) {
      if (candidate->indexOf(sketchCanvas) >= 0) {
        workspace = candidate;
        break;
      }
    }
    CHECK(workspace != nullptr);
    QWidget* modelWorkspace = workspace->currentWidget();
    CHECK(modelWorkspace != sketchCanvas);
    CHECK(sketchSettingsDock->isHidden());
    CHECK(!modelTreeDock->isHidden());
    workspace->setCurrentWidget(sketchCanvas);
    CHECK(!sketchSettingsDock->isHidden());
    CHECK(modelTreeDock->isHidden());
    workspace->setCurrentWidget(modelWorkspace);
    CHECK(sketchSettingsDock->isHidden());
    CHECK(!modelTreeDock->isHidden());
    sketchCanvas->setViewOrientation(37.0, -24.0);
    CHECK(!sketchCanvas->viewAlignedToSketchPlane());
    orientSketchPlane->click();
    CHECK(sketchCanvas->viewAlignedToSketchPlane());
    auto* mirrorDock = editor->findChild<QDockWidget*>(
        QStringLiteral("mirrorParametersDock"));
    CHECK(mirrorDock != nullptr);
    CHECK(mirrorDock->findChildren<QComboBox*>().empty());
    CHECK(mirrorDock->findChild<QPushButton*>(
              QStringLiteral("mirrorBodySelectButton")) != nullptr);
    CHECK(mirrorDock->findChild<QPushButton*>(
              QStringLiteral("mirrorPlaneSelectButton")) != nullptr);
    auto* moveDock = editor->findChild<QDockWidget*>(
        QStringLiteral("moveParametersDock"));
    CHECK(moveDock != nullptr);
    CHECK(moveDock->findChildren<QComboBox*>().empty());
    CHECK(moveDock->findChild<QPushButton*>(
              QStringLiteral("moveBodySelectButton")) != nullptr);
    CHECK(moveDock->findChild<QDoubleSpinBox*>(
              QStringLiteral("moveXSpin")) != nullptr);
    CHECK(moveDock->findChild<QDoubleSpinBox*>(
              QStringLiteral("moveYSpin")) != nullptr);
    CHECK(moveDock->findChild<QDoubleSpinBox*>(
              QStringLiteral("moveZSpin")) != nullptr);
    auto* linearPatternDock = editor->findChild<QDockWidget*>(
        QStringLiteral("linearPatternParametersDock"));
    CHECK(linearPatternDock != nullptr);
    CHECK(linearPatternDock->findChild<QPushButton*>(
              QStringLiteral("linearPatternBodySelectButton")) != nullptr);
    CHECK(linearPatternDock->findChild<QPushButton*>(
              QStringLiteral("linearPatternAxisSelectButton")) != nullptr);
    CHECK(linearPatternDock->findChild<QDoubleSpinBox*>(
              QStringLiteral("linearPatternSpacingSpin")) != nullptr);
    CHECK(linearPatternDock->findChild<QComboBox*>(
              QStringLiteral("linearPatternOperationCombo")) != nullptr);
    auto* circularPatternDock = editor->findChild<QDockWidget*>(
        QStringLiteral("circularPatternParametersDock"));
    CHECK(circularPatternDock != nullptr);
    CHECK(circularPatternDock->findChild<QPushButton*>(
              QStringLiteral("circularPatternBodySelectButton")) != nullptr);
    CHECK(circularPatternDock->findChild<QPushButton*>(
              QStringLiteral("circularPatternAxisSelectButton")) != nullptr);
    CHECK(circularPatternDock->findChild<QDoubleSpinBox*>(
              QStringLiteral("circularPatternAngleSpin")) != nullptr);
    CHECK(circularPatternDock->findChild<QSpinBox*>(
              QStringLiteral("circularPatternCountSpin")) != nullptr);
    CHECK(circularPatternDock->findChild<QComboBox*>(
              QStringLiteral("circularPatternOperationCombo")) != nullptr);
    home.hide();
    // Construction/destruction is tested without exposing a native OpenGL
    // surface; rendering is covered by the application-level smoke launch.
  });
  home.show();
  emit home.projectRequested(QStringLiteral("C:/Temp/test.solidar"));
  QTimer::singleShot(700, &application, &QApplication::quit);
  const int result = application.exec();
  delete editor;
  return result;
}
