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
  QApplication application(argc, argv);

  const auto mouse = [](solidar::Viewport& view, QEvent::Type type,
                        QPointF position, Qt::MouseButton button,
                        Qt::MouseButtons buttons) {
    QMouseEvent event(type, position, position, button, buttons, Qt::NoModifier);
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
