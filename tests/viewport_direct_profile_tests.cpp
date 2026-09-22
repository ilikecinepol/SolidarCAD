#include <QApplication>
#include <QMouseEvent>

#include <cmath>
#include <cstdlib>
#include <iostream>

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
  qputenv("QT_OPENGL", "software");
  QApplication application(argc, argv);

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

  const solidar::SketchPlacement placement{{35, 20, 65}, {1, 0, 0}, {0, 1, 0}};
  const QString support = QStringLiteral("\u0413\u0440\u0430\u043d\u044c \u0442\u0435\u043b\u0430 #7");

  solidar::Viewport view;
  view.resize(800, 600);
  solidar::sketch::Sketch profile;
  profile.addRectangle({-6, -6}, {6, 6});
  view.addSketch(profile, support, placement);

  const solidar::ViewportCameraState camera{
      view.cameraYawDegrees(), view.cameraPitchDegrees(), 1.0F, {}, view.size()};
  const auto normal = placement.normal();
  const QPointF pressPoint = camera.worldToScreen(placement.toWorld(0, 0));
  const QPointF projectedAxis =
      camera.worldToScreen({normal.x, normal.y, normal.z}) -
      camera.worldToScreen({0, 0, 0});

  int picks = 0;
  std::size_t pickedIndex = static_cast<std::size_t>(-1);
  double lastValue = 0.0;
  int valueChanges = 0;
  QObject::connect(&view, &solidar::Viewport::directProfilePicked, &view,
                   [&](std::size_t index) {
                     ++picks;
                     pickedIndex = index;
                     const auto centroid = placement.toWorld(0, 0);
                     view.setToolManipulator(
                         {{centroid.x, centroid.y, centroid.z},
                          {normal.x, normal.y, normal.z},
                          10.0, -100000.0, 100000.0, true});
                     view.seedToolManipulatorDrag(
                         pressPoint, {normal.x, normal.y, normal.z});
                   });
  QObject::connect(&view, &solidar::Viewport::toolManipulatorValueChanged, &view,
                   [&](double value) {
                     lastValue = value;
                     ++valueChanges;
                   });

  // Normal-mode press on the profile centre: no tool was selected.
  mouse(view, QEvent::MouseButtonPress, pressPoint, Qt::LeftButton,
        Qt::LeftButton);
  CHECK(picks == 1);
  CHECK(pickedIndex == 0);
  CHECK(view.extrusionCandidateSketchIndex() == 0);
  CHECK(!view.extrusionCandidateSketch().lines().empty() ||
        !view.extrusionCandidateSketch().circles().empty());

  // The same held press drags outward along the profile normal (no jump).
  mouse(view, QEvent::MouseMove, pressPoint + projectedAxis * 18,
        Qt::NoButton, Qt::LeftButton);
  CHECK(valueChanges >= 1);
  CHECK(lastValue > 10.0);

  // Release stops the drag; further moves emit nothing.
  mouse(view, QEvent::MouseButtonRelease, pressPoint + projectedAxis * 18,
        Qt::LeftButton, Qt::NoButton);
  const int changesAtRelease = valueChanges;
  mouse(view, QEvent::MouseMove, pressPoint + projectedAxis * 24,
        Qt::NoButton, Qt::NoButton);
  CHECK(valueChanges == changesAtRelease);

  // Empty-area press does not emit another direct pick.
  mouse(view, QEvent::MouseButtonPress, {10, 10}, Qt::LeftButton,
        Qt::LeftButton);
  CHECK(picks == 1);

  // An Arc spanning a complete rectangle side and the rectangle itself are
  // two adjacent selectable regions. Picking one must not highlight both.
  {
    solidar::Viewport arcView;
    arcView.resize(800, 600);
    solidar::sketch::Sketch arcProfile;
    arcProfile.addRectangle({-20.0, -10.0}, {20.0, 10.0});
    arcProfile.addArc({0.0, 10.0}, 20.0, 0.0,
                      3.14159265358979323846);
    const auto arcPlacement = solidar::SketchPlacement::xy();
    arcView.addSketch(arcProfile, QStringLiteral("XY"), arcPlacement);

    const solidar::ViewportCameraState arcCamera{
        arcView.cameraYawDegrees(), arcView.cameraPitchDegrees(), 1.0F, {},
        arcView.size()};
    const QPointF insideRectangle =
        arcCamera.worldToScreen(arcPlacement.toWorld(0.0, 0.0));
    const QPointF insideArcSegment =
        arcCamera.worldToScreen(arcPlacement.toWorld(0.0, 20.0));
    int arcPicks = 0;
    QObject::connect(&arcView, &solidar::Viewport::directProfilePicked,
                     &arcView, [&](std::size_t) { ++arcPicks; });

    mouse(arcView, QEvent::MouseButtonPress, insideRectangle,
          Qt::LeftButton, Qt::LeftButton);

    CHECK(arcPicks == 1);
    const auto& selectedRectangle = arcView.extrusionCandidateSketch();
    CHECK(selectedRectangle.lines().size() == 4);
    CHECK(selectedRectangle.arcs().empty());
    CHECK(selectedRectangle.circles().empty());
    CHECK(selectedRectangle.isClosed());

    mouse(arcView, QEvent::MouseButtonPress, insideArcSegment,
          Qt::LeftButton, Qt::LeftButton);
    CHECK(arcPicks == 2);
    const auto& selectedArcSegment = arcView.extrusionCandidateSketch();
    CHECK(selectedArcSegment.lines().size() == 1);
    CHECK(selectedArcSegment.arcs().size() == 1);
    CHECK(selectedArcSegment.circles().empty());
    CHECK(selectedArcSegment.isClosed());

    // Ctrl-click is additive: selecting the rectangle next to the already
    // selected Arc region produces their exact union and keeps the Arc curved.
    mouseMod(arcView, QEvent::MouseButtonPress, insideRectangle,
             Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    const auto& combined = arcView.extrusionCandidateSketch();
    CHECK(combined.lines().size() == 3);
    CHECK(combined.arcs().size() == 1);
    CHECK(combined.circles().empty());
    CHECK(combined.isClosed());

    // Repeating Ctrl-click on the rectangle toggles only that region off.
    mouseMod(arcView, QEvent::MouseButtonPress, insideRectangle,
             Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    const auto& remainingArcSegment = arcView.extrusionCandidateSketch();
    CHECK(remainingArcSegment.lines().size() == 1);
    CHECK(remainingArcSegment.arcs().size() == 1);
    CHECK(remainingArcSegment.circles().empty());
    CHECK(remainingArcSegment.isClosed());

    // A normal click replaces the complete additive selection.
    mouse(arcView, QEvent::MouseButtonPress, insideRectangle,
          Qt::LeftButton, Qt::LeftButton);
    const auto& replacedWithRectangle = arcView.extrusionCandidateSketch();
    CHECK(replacedWithRectangle.lines().size() == 4);
    CHECK(replacedWithRectangle.arcs().empty());
    CHECK(replacedWithRectangle.circles().empty());
    CHECK(replacedWithRectangle.isClosed());
  }

  // Solver-created coincidences can retain a few microns of numerical drift.
  // Region picking accepts that geometry, so the selected union must also
  // canonicalize it before the strict model closed-wire validation.
  {
    solidar::Viewport driftedArcView;
    driftedArcView.resize(800, 600);
    solidar::sketch::Sketch driftedProfile;
    driftedProfile.addRectangle({-20.0, -10.0}, {20.0, 10.0});
    driftedProfile.addArc({0.0, 10.0 + 5e-6}, 20.0, 0.0,
                          3.14159265358979323846);
    const auto driftedPlacement = solidar::SketchPlacement::xy();
    driftedArcView.addSketch(driftedProfile, QStringLiteral("XY"),
                             driftedPlacement);
    const solidar::ViewportCameraState driftedCamera{
        driftedArcView.cameraYawDegrees(),
        driftedArcView.cameraPitchDegrees(), 1.0F, {},
        driftedArcView.size()};
    const QPointF insideRectangle =
        driftedCamera.worldToScreen(driftedPlacement.toWorld(0.0, 0.0));
    const QPointF insideArcSegment =
        driftedCamera.worldToScreen(driftedPlacement.toWorld(0.0, 20.0));

    driftedArcView.beginExtrusionSurfaceSelection();
    mouseMod(driftedArcView, QEvent::MouseButtonPress, insideRectangle,
             Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    mouseMod(driftedArcView, QEvent::MouseButtonPress, insideArcSegment,
             Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    const auto& combined = driftedArcView.extrusionCandidateSketch();
    CHECK(combined.lines().size() == 3);
    CHECK(combined.arcs().size() == 1);
    CHECK(combined.isClosed());
  }

  // The same region separation applies when the Arc covers only part of a
  // rectangle side. Its segment keeps the exact Arc for preview/extrusion.
  {
    solidar::Viewport partialArcView;
    partialArcView.resize(800, 600);
    solidar::sketch::Sketch partialArcProfile;
    partialArcProfile.addRectangle({-20.0, -20.0}, {20.0, 20.0});
    partialArcProfile.addArc({-20.0, -10.0}, 10.0,
                             3.14159265358979323846 * 0.5,
                             3.14159265358979323846);
    const auto placement = solidar::SketchPlacement::xy();
    partialArcView.addSketch(partialArcProfile, QStringLiteral("XY"),
                             placement);
    const solidar::ViewportCameraState camera{
        partialArcView.cameraYawDegrees(),
        partialArcView.cameraPitchDegrees(), 1.0F, {},
        partialArcView.size()};
    const QPointF insideRectangle =
        camera.worldToScreen(placement.toWorld(0.0, 0.0));
    const QPointF insideArcSegment =
        camera.worldToScreen(placement.toWorld(-25.0, -10.0));

    int picks = 0;
    QObject::connect(&partialArcView,
                     &solidar::Viewport::directProfilePicked,
                     &partialArcView, [&](std::size_t) { ++picks; });
    mouse(partialArcView, QEvent::MouseButtonPress, insideRectangle,
          Qt::LeftButton, Qt::LeftButton);

    CHECK(picks == 1);
    const auto& selectedRectangle =
        partialArcView.extrusionCandidateSketch();
    CHECK(selectedRectangle.lines().size() == 4);
    CHECK(selectedRectangle.arcs().empty());
    CHECK(selectedRectangle.isClosed());

    mouse(partialArcView, QEvent::MouseButtonPress, insideArcSegment,
          Qt::LeftButton, Qt::LeftButton);
    CHECK(picks == 2);
    const auto& selectedArcSegment =
        partialArcView.extrusionCandidateSketch();
    CHECK(selectedArcSegment.lines().size() == 1);
    CHECK(selectedArcSegment.arcs().size() == 1);
    CHECK(selectedArcSegment.isClosed());

    // paintGL reprojects the model-space profile for every interactive frame.
    // The rebuilt preview boundary must still reach the Arc's outermost point
    // instead of collapsing back to the original rectangle.
    partialArcView.beginExtrusionSurfaceSelection();
    mouse(partialArcView, QEvent::MouseButtonPress, insideArcSegment,
          Qt::LeftButton, Qt::LeftButton);
    partialArcView.showExtrusionManipulator(12.0);
    partialArcView.show();
    QApplication::processEvents();
    const QPointF arcExtreme =
        camera.worldToScreen(placement.toWorld(-30.0, -10.0));
    CHECK(partialArcView.extrusionPreviewBaseBounds()
              .adjusted(-0.5, -0.5, 0.5, 0.5)
              .contains(arcExtreme));
  }


  // partitioned-region regression: an internal chain whose endpoints land on
  // the outer rectangle must split the sketch into selectable bounded faces.
  {
    solidar::Viewport partitionView;
    partitionView.resize(800, 600);

    solidar::sketch::Sketch partition;
    partition.addRectangle({-20.0, -20.0}, {20.0, 20.0});
    partition.addLine({0.0, 20.0}, {0.0, 5.0});
    partition.addLine({0.0, 5.0}, {10.0, 5.0});
    partition.addLine({10.0, 5.0}, {20.0, -5.0});

    const solidar::SketchPlacement partitionPlacement =
        solidar::SketchPlacement::xy();
    partitionView.addSketch(partition, QStringLiteral("XY"),
                            partitionPlacement);

    const solidar::ViewportCameraState partitionCamera{
        partitionView.cameraYawDegrees(),
        partitionView.cameraPitchDegrees(),
        1.0F, {}, partitionView.size()};
    const QPointF innerPoint =
        partitionCamera.worldToScreen(
            partitionPlacement.toWorld(10.0, 12.0));

    int partitionPicks = 0;
    QObject::connect(
        &partitionView, &solidar::Viewport::directProfilePicked,
        &partitionView, [&](std::size_t) { ++partitionPicks; });

    mouse(partitionView, QEvent::MouseButtonPress, innerPoint,
          Qt::LeftButton, Qt::LeftButton);

    CHECK(partitionPicks == 1);
    const auto& selected =
        partitionView.extrusionCandidateSketch();
    CHECK(!selected.lines().empty());

    double minimumX = 1e9;
    for (const auto& line : selected.lines()) {
      minimumX = std::min(minimumX,
                          std::min(line.start.xMm, line.end.xMm));
    }
    // Whole outer rectangle would reach x=-20. The top-right partition starts
    // at x=0, proving the picked region is the inner bounded face.
    CHECK(minimumX > -1.0);
  }

  return EXIT_SUCCESS;
}
