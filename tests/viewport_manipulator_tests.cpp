#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <QApplication>
#include <QLineF>
#include <QMouseEvent>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <numbers>
#include <utility>

#include "ui/ManipulatorLayout.h"
#include "ui/ViewCube.h"
#include "ui/Viewport.h"
#include "ui/ViewportCamera.h"

namespace {
void mouse(solidar::Viewport& view, QEvent::Type type, QPointF position,
           Qt::MouseButton button, Qt::MouseButtons buttons) {
  QMouseEvent event(type, position, position, button, buttons, Qt::NoModifier);
  QApplication::sendEvent(&view, &event);
}

// Replicates Viewport::toolManipulatorLayout() using the same camera parameters
// (the manipulator origin is the projection center, so the screen position is
// independent of it). Mirrors the value-independent unit probe: the semantic
// direction is derived from origin + direction (1.0 unit), never scaled by
// valueMm, exactly like the fixed presentation path. The handle itself does not
// depend on the HUD size, so a fixed placeholder is sufficient.
solidar::ManipulatorLayoutResult linearLayout(
    const solidar::Viewport& view,
    const solidar::LinearToolManipulator& manipulator) {
  solidar::ViewportCameraState camera{
      view.cameraYawDegrees(), view.cameraPitchDegrees(), 1.0F, {}, view.size(),
      1.0F, manipulator.origin, 1.0};
  const QPointF anchor = camera.worldToScreen(manipulator.origin);
  const solidar::Point3d end{manipulator.origin.x + manipulator.direction.x,
                              manipulator.origin.y + manipulator.direction.y,
                              manipulator.origin.z + manipulator.direction.z};
  const QPointF semanticEnd = camera.worldToScreen(end);
  return solidar::computeManipulatorLayout(
      {anchor, semanticEnd - anchor, QLineF(anchor, semanticEnd).length(),
       QRectF(), QRectF(QPointF(0, 0), view.size()), QSizeF(132, 40),
       {QRectF(view.width() - 126.0, 8.0, 116.0, 116.0)}});
}

QPointF linearHandle(const solidar::Viewport& view,
                     const solidar::LinearToolManipulator& manipulator) {
  return linearLayout(view, manipulator).handle;
}

QPointF translationHandle(
    const solidar::Viewport& view,
    const solidar::TranslationToolManipulator& manipulator, int axisIndex) {
  const std::array<solidar::Vector3d, 3> axes{{{1.0, 0.0, 0.0},
                                               {0.0, 1.0, 0.0},
                                               {0.0, 0.0, 1.0}}};
  const std::array<QPointF, 3> fallbacks{{{1.0, 0.0},
                                          {-0.7, 0.7},
                                          {0.0, -1.0}}};
  solidar::ViewportCameraState camera{
      view.cameraYawDegrees(), view.cameraPitchDegrees(), 1.0F, {}, view.size(),
      1.0F, manipulator.origin, 1.0};
  const QPointF anchor = camera.worldToScreen(manipulator.origin);
  const auto axis = axes[axisIndex];
  const solidar::Point3d endpoint{manipulator.origin.x + axis.x,
                                   manipulator.origin.y + axis.y,
                                   manipulator.origin.z + axis.z};
  const auto stable = solidar::stableProjectedDirection(
      camera.worldToScreen(endpoint) - anchor, fallbacks[axisIndex], 2.0);
  return anchor + stable.normalizedDirection * 64.0;
}

solidar::Vector3d normalizedVector(solidar::Vector3d value) {
  const double length =
      std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
  if (length < 1e-12) return {0.0, 0.0, 1.0};
  return {value.x / length, value.y / length, value.z / length};
}

solidar::Vector3d crossVector(solidar::Vector3d a, solidar::Vector3d b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}

// Replicates Viewport::angularBasis (local static in Viewport.cpp).
std::pair<solidar::Vector3d, solidar::Vector3d> angularBasis(
    solidar::Vector3d axis) {
  axis = normalizedVector(axis);
  const solidar::Vector3d reference =
      std::abs(axis.z) < 0.85 ? solidar::Vector3d{0.0, 0.0, 1.0}
                              : solidar::Vector3d{0.0, 1.0, 0.0};
  const solidar::Vector3d u = normalizedVector(crossVector(axis, reference));
  return {u, normalizedVector(crossVector(axis, u))};
}

// Replicates Viewport::angularVisual()'s handle screen position (origin, basis
// and resolved visual radius) so the press can target the actual drawn handle.
QPointF angularHandle(const solidar::Viewport& view,
                      const solidar::AngularToolManipulator& manipulator) {
  const auto [u, v] = angularBasis(manipulator.axis);
  solidar::ViewportCameraState camera{
      view.cameraYawDegrees(), view.cameraPitchDegrees(), 1.0F, {}, view.size(),
      1.0F, manipulator.origin, 1.0};
  const QPointF origin = camera.worldToScreen(manipulator.origin);
  const solidar::Point3d uWorld{manipulator.origin.x + u.x * manipulator.radiusMm,
                                 manipulator.origin.y + u.y * manipulator.radiusMm,
                                 manipulator.origin.z + u.z * manipulator.radiusMm};
  const solidar::Point3d vWorld{manipulator.origin.x + v.x * manipulator.radiusMm,
                                 manipulator.origin.y + v.y * manipulator.radiusMm,
                                 manipulator.origin.z + v.z * manipulator.radiusMm};
  const auto visual = solidar::computeAngularVisualRadius(
      origin, camera.worldToScreen(uWorld), camera.worldToScreen(vWorld),
      manipulator.radiusMm, QRectF());
  const double angle = manipulator.angleDeg * std::numbers::pi / 180.0;
  const solidar::Point3d handleWorld{
      manipulator.origin.x + u.x * visual.visualRadiusMm * std::cos(angle) +
          v.x * visual.visualRadiusMm * std::sin(angle),
      manipulator.origin.y + u.y * visual.visualRadiusMm * std::cos(angle) +
          v.y * visual.visualRadiusMm * std::sin(angle),
      manipulator.origin.z + u.z * visual.visualRadiusMm * std::cos(angle) +
          v.z * visual.visualRadiusMm * std::sin(angle)};
  return camera.worldToScreen(handleWorld);
}
}  // namespace

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);

  // No solid, Body or preview is installed. The active tool presentation is
  // nevertheless a first-class viewport overlay.
  solidar::Viewport viewport;
  viewport.resize(640, 480);
  viewport.setAngularToolManipulator(
      {{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}, 35.0, 225.0});
  CHECK(viewport.angularToolManipulator().has_value());
  CHECK(std::abs(viewport.angularToolManipulator()->radiusMm - 35.0) < 1e-9);
  CHECK(std::abs(viewport.angularToolManipulator()->angleDeg - 225.0) < 1e-9);

  // Camera changes retain the authoritative world-space manipulator. Pixel
  // rendering is covered by the manual GPU gate because QOpenGLWidget has no
  // framebuffer on Qt's headless offscreen platform.
  viewport.viewTop();
  CHECK(viewport.angularToolManipulator().has_value());
  viewport.viewIsometric();
  CHECK(viewport.angularToolManipulator().has_value());
  viewport.clearToolManipulator();
  CHECK(!viewport.angularToolManipulator().has_value());

  // End-to-end linear drag. This camera arrangement projects the semantic axis
  // such that computeManipulatorLayout selects visualSign < 0 (the flipped
  // side, equivalent to a blocked preferred side): dragging along the drawn
  // arrow must still increase the value and stay within [min, max].
  const solidar::LinearToolManipulator manip{
      {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 10.0, 0.0, 60.0};
  solidar::Viewport dragView;
  dragView.resize(800, 600);
  dragView.setToolManipulator(manip);
  const QPointF handle = linearHandle(dragView, manip);
  const QPointF anchor = solidar::ViewportCameraState{
                             dragView.cameraYawDegrees(),
                             dragView.cameraPitchDegrees(), 1.0F, {},
                             dragView.size(), 1.0F, manip.origin, 1.0}
                             .worldToScreen(manip.origin);
  const double handleLength = QLineF(anchor, handle).length();
  CHECK(handleLength > 0.0);
  const QPointF arrowDirection = (handle - anchor) / handleLength;

  double lastValue = manip.valueMm;
  int emissions = 0;
  QObject::connect(&dragView, &solidar::Viewport::toolManipulatorValueChanged,
                   &dragView, [&](double value) {
                     lastValue = value;
                     ++emissions;
                   });
  mouse(dragView, QEvent::MouseButtonPress, handle, Qt::LeftButton,
        Qt::LeftButton);
  mouse(dragView, QEvent::MouseMove, handle + arrowDirection * 20.0,
        Qt::NoButton, Qt::LeftButton);
  CHECK(emissions >= 1);
  CHECK(lastValue > manip.valueMm);
  CHECK(lastValue <= manip.maximumMm);
  CHECK(lastValue >= manip.minimumMm);
  // The drag value is clamped to the manipulator's declared range: a large
  // outward drag saturates at maximumMm, an inward drag at minimumMm.
  mouse(dragView, QEvent::MouseMove, handle + arrowDirection * 10000.0,
        Qt::NoButton, Qt::LeftButton);
  CHECK(std::abs(lastValue - manip.maximumMm) < 1e-9);
  mouse(dragView, QEvent::MouseMove, handle - arrowDirection * 10000.0,
        Qt::NoButton, Qt::LeftButton);
  CHECK(std::abs(lastValue - manip.minimumMm) < 1e-9);
  mouse(dragView, QEvent::MouseButtonRelease, handle, Qt::LeftButton,
        Qt::NoButton);

  // Move's triad reports the dragged axis independently and supports signed
  // coordinates. Dragging X must not mutate the Y/Z values owned by the tool
  // session.
  {
    const solidar::TranslationToolManipulator translation{
        {0.0, 0.0, 0.0}, {5.0, -7.0, 9.0}, -100.0, 100.0};
    solidar::Viewport moveView;
    moveView.resize(800, 600);
    moveView.setTranslationToolManipulator(translation);
    CHECK(moveView.translationToolManipulator().has_value());
    const QPointF moveHandle = translationHandle(moveView, translation, 0);
    const QPointF moveAnchor = solidar::ViewportCameraState{
                                   moveView.cameraYawDegrees(),
                                   moveView.cameraPitchDegrees(), 1.0F, {},
                                   moveView.size(), 1.0F, translation.origin,
                                   1.0}
                                   .worldToScreen(translation.origin);
    const QPointF direction =
        (moveHandle - moveAnchor) / QLineF(moveAnchor, moveHandle).length();
    int emittedAxis = -1;
    double emittedValue = translation.offsetMm.x;
    QObject::connect(
        &moveView,
        &solidar::Viewport::translationToolManipulatorValueChanged,
        &moveView, [&](int axis, double value) {
          emittedAxis = axis;
          emittedValue = value;
        });
    mouse(moveView, QEvent::MouseButtonPress, moveHandle, Qt::LeftButton,
          Qt::LeftButton);
    mouse(moveView, QEvent::MouseMove, moveHandle + direction * 20.0,
          Qt::NoButton, Qt::LeftButton);
    CHECK(emittedAxis == 0);
    CHECK(emittedValue > translation.offsetMm.x);
    CHECK(moveView.translationToolManipulator()->offsetMm.y ==
          translation.offsetMm.y);
    CHECK(moveView.translationToolManipulator()->offsetMm.z ==
          translation.offsetMm.z);
    mouse(moveView, QEvent::MouseButtonRelease, moveHandle, Qt::LeftButton,
          Qt::NoButton);
    moveView.clearToolManipulator();
    CHECK(!moveView.translationToolManipulator().has_value());
  }

  // Zero-value (Fillet/Chamfer-like) manipulator: the presentation direction is
  // value-independent, so dragging along the ACTUAL drawn arrow must increase
  // the value from zero. Pre-fix, the value-scaled (zero) semantic probe made
  // the layout fall back to screen-up while the drag used the real unit axis,
  // pinning the value at 0 (sign mismatch).
  {
    const solidar::LinearToolManipulator zero{
        {0.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, 0.0, 0.0, 60.0};
    solidar::Viewport zeroView;
    zeroView.resize(800, 600);
    zeroView.setToolManipulator(zero);
    const QPointF zeroHandle = linearHandle(zeroView, zero);
    const QPointF zeroAnchor =
        solidar::ViewportCameraState{zeroView.cameraYawDegrees(),
                                     zeroView.cameraPitchDegrees(), 1.0F, {},
                                     zeroView.size(), 1.0F, zero.origin, 1.0}
            .worldToScreen(zero.origin);
    const double zeroLength = QLineF(zeroAnchor, zeroHandle).length();
    CHECK(zeroLength > 0.0);
    const QPointF zeroArrow = (zeroHandle - zeroAnchor) / zeroLength;
    double zeroLast = zero.valueMm;
    int zeroEmissions = 0;
    QObject::connect(&zeroView, &solidar::Viewport::toolManipulatorValueChanged,
                     &zeroView, [&](double v) {
                       zeroLast = v;
                       ++zeroEmissions;
                     });
    mouse(zeroView, QEvent::MouseButtonPress, zeroHandle, Qt::LeftButton,
          Qt::LeftButton);
    mouse(zeroView, QEvent::MouseMove, zeroHandle + zeroArrow * 20.0,
          Qt::NoButton, Qt::LeftButton);
    CHECK(zeroEmissions >= 1);
    CHECK(zeroLast > 0.0);
    CHECK(zeroLast <= zero.maximumMm);
    CHECK(zeroLast >= zero.minimumMm);
    // Inward drag returns to the zero minimum.
    mouse(zeroView, QEvent::MouseMove, zeroHandle - zeroArrow * 10000.0,
          Qt::NoButton, Qt::LeftButton);
    CHECK(std::abs(zeroLast - zero.minimumMm) < 1e-9);
    mouse(zeroView, QEvent::MouseButtonRelease, zeroHandle, Qt::LeftButton,
          Qt::NoButton);
  }

  // Presentation/drag share the same axis classification for value 0 vs 100:
  // the resolved direction and visual sign must not flip with the parameter
  // value, because both now derive from the same value-independent unit probe.
  {
    const solidar::LinearToolManipulator zero{
        {0.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, 0.0, 0.0, 60.0};
    const solidar::LinearToolManipulator hundred{
        {0.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, 100.0, 0.0, 100000.0};
    solidar::Viewport v0;
    v0.resize(800, 600);
    v0.setToolManipulator(zero);
    solidar::Viewport v100;
    v100.resize(800, 600);
    v100.setToolManipulator(hundred);
    const auto layout0 = linearLayout(v0, zero);
    const auto layout100 = linearLayout(v100, hundred);
    CHECK(std::abs(layout0.direction.x() - layout100.direction.x()) < 1e-9);
    CHECK(std::abs(layout0.direction.y() - layout100.direction.y()) < 1e-9);
    CHECK(layout0.visualSign == layout100.visualSign);
    CHECK(!layout0.usedFallback);
    CHECK(!layout100.usedFallback);
  }

  // Degenerate (edge-on) angular basis: an indeterminate inverse-projection
  // must NOT overwrite the accepted angle or emit a changed value. viewBack
  // looks along +Y, collapsing the angular manipulator's u basis (axis {1,0,0}
  // has u in -Y) to a point while v stays visible.
  {
    solidar::Viewport angularView;
    angularView.resize(800, 600);
    const solidar::AngularToolManipulator angular{
        {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, 20.0, 90.0};
    angularView.setAngularToolManipulator(angular);
    angularView.viewBack();
    const QPointF handlePos = angularHandle(angularView, angular);
    int angularEmissions = 0;
    double lastAngle = angular.angleDeg;
    QObject::connect(
        &angularView, &solidar::Viewport::angularToolManipulatorValueChanged,
        &angularView, [&](double a) {
          lastAngle = a;
          ++angularEmissions;
        });
    mouse(angularView, QEvent::MouseButtonPress, handlePos, Qt::LeftButton,
          Qt::LeftButton);
    mouse(angularView, QEvent::MouseMove, handlePos + QPointF(40.0, 0.0),
          Qt::NoButton, Qt::LeftButton);
    CHECK(angularEmissions == 0);
    CHECK(std::abs(lastAngle - angular.angleDeg) < 1e-9);
    CHECK(std::abs(angularView.angularToolManipulator()->angleDeg - 90.0) <
          1e-9);
    mouse(angularView, QEvent::MouseButtonRelease, handlePos, Qt::LeftButton,
          Qt::NoButton);
  }

  // A signed angular manipulator can be dragged through zero into the opposite
  // draft direction. The target is computed from the same angular basis as the
  // viewport, covering the complete mouse interaction path.
  {
    solidar::Viewport signedView;
    signedView.resize(800, 600);
    signedView.viewTop();
    const solidar::AngularToolManipulator initial{
        {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 20.0, 30.0, -89.99, 89.99};
    signedView.setAngularToolManipulator(initial);
    const QPointF start = angularHandle(signedView, initial);
    auto targetManipulator = initial;
    targetManipulator.angleDeg = -45.0;
    const QPointF target = angularHandle(signedView, targetManipulator);
    double lastAngle = initial.angleDeg;
    int signedEmissions = 0;
    QObject::connect(
        &signedView, &solidar::Viewport::angularToolManipulatorValueChanged,
        &signedView, [&](double value) {
          lastAngle = value;
          ++signedEmissions;
        });
    mouse(signedView, QEvent::MouseButtonPress, start, Qt::LeftButton,
          Qt::LeftButton);
    mouse(signedView, QEvent::MouseMove, target, Qt::NoButton,
          Qt::LeftButton);
    mouse(signedView, QEvent::MouseButtonRelease, target, Qt::LeftButton,
          Qt::NoButton);
    CHECK(signedEmissions >= 1);
    CHECK(lastAngle < -44.0 && lastAngle > -46.0);
    CHECK(signedView.angularToolManipulator().has_value());
    CHECK(signedView.angularToolManipulator()->angleDeg < -44.0);
    CHECK(signedView.angularToolManipulator()->angleDeg > -46.0);
  }

  // clearToolManipulator mid-drag stops any further value emission.
  solidar::Viewport clearView;
  clearView.resize(800, 600);
  clearView.setToolManipulator(manip);
  const QPointF clearHandle = linearHandle(clearView, manip);
  const QPointF clearAnchor =
      solidar::ViewportCameraState{clearView.cameraYawDegrees(),
                                   clearView.cameraPitchDegrees(), 1.0F, {},
                                   clearView.size(), 1.0F, manip.origin, 1.0}
          .worldToScreen(manip.origin);
  const QPointF clearArrow =
      (clearHandle - clearAnchor) / QLineF(clearAnchor, clearHandle).length();
  int clearEmissions = 0;
  QObject::connect(&clearView, &solidar::Viewport::toolManipulatorValueChanged,
                   &clearView, [&](double) { ++clearEmissions; });
  mouse(clearView, QEvent::MouseButtonPress, clearHandle, Qt::LeftButton,
        Qt::LeftButton);
  mouse(clearView, QEvent::MouseMove, clearHandle + clearArrow * 20.0,
        Qt::NoButton, Qt::LeftButton);
  CHECK(clearEmissions >= 1);
  clearView.clearToolManipulator();
  const int afterClear = clearEmissions;
  mouse(clearView, QEvent::MouseMove, clearHandle + clearArrow * 40.0,
        Qt::NoButton, Qt::LeftButton);
  CHECK(clearEmissions == afterClear);

  // resetScene mid-drag stops any further value emission too.
  solidar::Viewport resetView;
  resetView.resize(800, 600);
  resetView.setToolManipulator(manip);
  const QPointF resetHandle = linearHandle(resetView, manip);
  const QPointF resetAnchor =
      solidar::ViewportCameraState{resetView.cameraYawDegrees(),
                                   resetView.cameraPitchDegrees(), 1.0F, {},
                                   resetView.size(), 1.0F, manip.origin, 1.0}
          .worldToScreen(manip.origin);
  const QPointF resetArrow =
      (resetHandle - resetAnchor) / QLineF(resetAnchor, resetHandle).length();
  int resetEmissions = 0;
  QObject::connect(&resetView, &solidar::Viewport::toolManipulatorValueChanged,
                   &resetView, [&](double) { ++resetEmissions; });
  mouse(resetView, QEvent::MouseButtonPress, resetHandle, Qt::LeftButton,
        Qt::LeftButton);
  mouse(resetView, QEvent::MouseMove, resetHandle + resetArrow * 20.0,
        Qt::NoButton, Qt::LeftButton);
  CHECK(resetEmissions >= 1);
  resetView.resetScene();
  const int afterReset = resetEmissions;
  mouse(resetView, QEvent::MouseMove, resetHandle + resetArrow * 40.0,
        Qt::NoButton, Qt::LeftButton);
  CHECK(resetEmissions == afterReset);

  // Higher-priority interactions must never start a marquee.
  {
    // Linear manipulator handle press begins the drag, not a marquee.
    solidar::Viewport manipView;
    manipView.resize(800, 600);
    manipView.setToolManipulator(manip);
    const QPointF handlePress = linearHandle(manipView, manip);
    mouse(manipView, QEvent::MouseButtonPress, handlePress, Qt::LeftButton,
          Qt::LeftButton);
    CHECK(!manipView.marqueeActive());
    mouse(manipView, QEvent::MouseButtonRelease, handlePress, Qt::LeftButton,
          Qt::NoButton);

    // ViewCube press is consumed by the cube, not a marquee.
    solidar::Viewport cubeView;
    cubeView.resize(800, 600);
    const auto cubeGeometry = solidar::viewCubeGeometry(
        cubeView.size(),
        {cubeView.cameraYawDegrees(), cubeView.cameraPitchDegrees()});
    const auto face = cubeGeometry.patches[4];
    QPointF cubePoint;
    for (const auto& point : face.polygon) cubePoint += point;
    cubePoint /= face.polygon.size();
    mouse(cubeView, QEvent::MouseButtonPress, cubePoint, Qt::LeftButton,
          Qt::LeftButton);
    CHECK(!cubeView.marqueeActive());
    mouse(cubeView, QEvent::MouseButtonRelease, cubePoint, Qt::LeftButton,
          Qt::NoButton);

    // A body face click selects the face (higher priority), not a marquee.
    solidar::Viewport bodyView;
    bodyView.resize(800, 600);
    const auto bodyShape = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(40.0, 30.0, 20.0).Shape());
    bodyView.setBodyShape(bodyShape, 41, 73);
    bodyView.setSolidVisible(true);
    const solidar::ViewportCameraState camera{
        bodyView.cameraYawDegrees(), bodyView.cameraPitchDegrees(), 1.0F, {},
        bodyView.size(), 1.0F, {20.0, 15.0, 10.0}, 1.0};
    const QPointF topFace = camera.worldToScreen({20.0, 15.0, 20.0});
    mouse(bodyView, QEvent::MouseButtonPress, topFace, Qt::LeftButton,
          Qt::LeftButton);
    CHECK(!bodyView.marqueeActive());
    CHECK(!bodyView.selectedBodyFaces().empty());
    mouse(bodyView, QEvent::MouseButtonRelease, topFace, Qt::LeftButton,
          Qt::NoButton);
  }

  return EXIT_SUCCESS;
}
