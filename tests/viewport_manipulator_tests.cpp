#include <QApplication>
#include <QDoubleSpinBox>
#include <cstdlib>
#include <iostream>

#include "ui/Viewport.h"

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

  // HUD edits are proposals. A direct observer sees the previously accepted
  // viewport state until the owning session reconciles a replacement.
  viewport.setToolManipulator(
      {{0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 1.0, 0.0, 5.0});
  CHECK(viewport.toolManipulator());
  double requested = -1.0;
  double observedAccepted = -1.0;
  QObject::connect(&viewport, &solidar::Viewport::toolManipulatorValueRequested,
                   [&viewport, &requested, &observedAccepted](double value) {
                     requested = value;
                     observedAccepted = viewport.toolManipulator()->valueMm;
                   });
  auto* distanceEditor = viewport.findChild<QDoubleSpinBox*>("distance");
  CHECK(distanceEditor);
  distanceEditor->setValue(3.0);
  CHECK(std::abs(requested - 3.0) < 1e-9);
  CHECK(std::abs(observedAccepted - 1.0) < 1e-9);
  CHECK(std::abs(viewport.toolManipulator()->valueMm - 1.0) < 1e-9);
  viewport.restoreToolManipulatorValue();
  CHECK(std::abs(distanceEditor->value() - 1.0) < 1e-9);
  return EXIT_SUCCESS;
}
