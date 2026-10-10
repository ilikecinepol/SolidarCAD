#include "TestAssertions.h"

#include "ui/SketchInteractionController.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <type_traits>
#include <utility>

using namespace solidar;

static_assert(std::is_same_v<
              decltype(std::declval<SketchInteractionController&>().snapshot()),
              const SketchToolState&>);
static_assert(std::is_same_v<
              decltype(std::declval<SketchDimensionInteractionState&>()
                           .selected),
              std::optional<SketchDimensionReference>>);
static_assert(std::is_same_v<
              decltype(SketchCreationGestureState{}.circleGuideIds),
              std::vector<sketch::GeometryId>>);

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

constexpr std::array kTools{
    SketchInteractionTool::Select,
    SketchInteractionTool::Line,
    SketchInteractionTool::Rectangle,
    SketchInteractionTool::Circle,
    SketchInteractionTool::Arc,
    SketchInteractionTool::Projection,
    SketchInteractionTool::AutoDimension,
    SketchInteractionTool::LockConstraint,
    SketchInteractionTool::OrthogonalConstraint,
    SketchInteractionTool::CoincidentConstraint,
    SketchInteractionTool::PerpendicularConstraint,
    SketchInteractionTool::ParallelConstraint,
    SketchInteractionTool::EqualConstraint,
    SketchInteractionTool::TangentConstraint,
    SketchInteractionTool::Mirror,
    SketchInteractionTool::Trim};

void populatePartialGesture(SketchInteractionController& controller,
                            SketchInteractionTool tool) {
  controller.beginCreation({sketch::Point{1.0, 2.0}});
  switch (tool) {
    case SketchInteractionTool::Rectangle:
      controller.appendRectanglePoint({2.0, 3.0});
      break;
    case SketchInteractionTool::Circle:
      controller.appendCirclePoint({2.0, 3.0});
      controller.setTwoTangentPreview(true);
      break;
    case SketchInteractionTool::Arc:
      controller.appendArcPoint({2.0, 3.0});
      controller.updateArcCreation({0.5, 1.0, false});
      break;
    case SketchInteractionTool::AutoDimension: {
      SketchAutoDimensionState automatic;
      automatic.target = SketchAutoDimensionTarget::Points;
      automatic.firstPoint = sketch::PointReference{42, true};
      controller.beginAutoDimension(std::move(automatic));
      break;
    }
    case SketchInteractionTool::PerpendicularConstraint:
      controller.setPerpendicularFirstLine(42);
      break;
    case SketchInteractionTool::ParallelConstraint:
      controller.setParallelFirstLine(42);
      break;
    case SketchInteractionTool::EqualConstraint:
      controller.setEqualFirst(SketchGeometryOperand{
          SketchGeometryOperandKind::Line, 42, 7});
      break;
    case SketchInteractionTool::TangentConstraint:
      controller.setTangentFirst(SketchGeometryOperand{
          SketchGeometryOperandKind::Circle, 42, 0});
      break;
    case SketchInteractionTool::CoincidentConstraint:
      controller.setCoincidentFirstPoint(sketch::PointReference{42, true});
      controller.setPointOnLineCarrier(42);
      break;
    case SketchInteractionTool::Mirror:
      controller.updateMirror(
          {{{SketchMirrorGeometryKind::Line, 42}}});
      break;
    case SketchInteractionTool::Trim:
      controller.updateTrim({SketchTrimPreview{
          SketchTrimGeometryKind::Line, 42, 0.2, 0.8, false}});
      break;
    case SketchInteractionTool::Select:
      controller.beginPointDrag(SketchLineEndpointDrag{42, true},
                                {1.0, 2.0});
      break;
    default: {
      SketchLineCreationState line;
      line.startLineCarrier = 42;
      controller.updateLineCreation(std::move(line));
      break;
    }
  }
  CHECK(controller.hasActiveGesture());
}

void everyToolSwitchCancelsPartialGesture() {
  SketchInteractionController controller;
  for (std::size_t index = 0; index < kTools.size(); ++index) {
    const auto tool = kTools[index];
    controller.switchTool(tool);
    populatePartialGesture(controller, tool);

    const auto next = kTools[(index + 1) % kTools.size()];
    controller.switchTool(next);
    CHECK(controller.tool() == next);
    CHECK(!controller.hasActiveGesture());

    controller.beginPointDrag(SketchCircleCenterDrag{77});
    CHECK(controller.hasActiveGesture());
    controller.cancelGesture();
    CHECK(!controller.hasActiveGesture());
  }
}

void cancelResetsDragDimensionConstraintAndCamera() {
  SketchInteractionController controller;
  controller.beginPointDrag(SketchArcEndpointDrag{11, false});
  const SketchDimensionReference dimension{3};
  controller.beginDimensionEdit(dimension);
  controller.beginDimensionLabelDrag(dimension);
  controller.setTangentFirst(SketchGeometryOperand{
      SketchGeometryOperandKind::Arc, 12, 0});
  controller.beginSelectionBox({4.0, 5.0}, true);
  controller.beginCameraGesture(SketchCameraGestureState::Kind::Orbit,
                                SketchCameraGestureState::Button::Right,
                                10.0, 20.0);
  CHECK(controller.hasActiveGesture());
  controller.cancelGesture();
  CHECK(!controller.hasActiveGesture());
}

void projectResetAndDestructionLeaveNoStaleState() {
  {
    SketchInteractionController controller;
    controller.switchTool(SketchInteractionTool::Circle);
    populatePartialGesture(controller, SketchInteractionTool::Circle);
    controller.setCameraPan(35.0, 0.0);
    controller.appendDimensionLabel(2.0, 3.0);
    controller.resetForProject();
    CHECK(controller.tool() == SketchInteractionTool::Select);
    CHECK(!controller.hasActiveGesture());
    CHECK(controller.snapshot().camera.panX == 0.0);
    CHECK(controller.snapshot().dimension.labelAlongMm.empty());
  }

  SketchInteractionController nextProject;
  CHECK(!nextProject.hasActiveGesture());
  nextProject.switchTool(SketchInteractionTool::Line);
  nextProject.beginCreation({sketch::Point{0.0, 0.0}});
  CHECK(nextProject.hasActiveGesture());
}

}  // namespace

int main() {
  everyToolSwitchCancelsPartialGesture();
  cancelResetsDragDimensionConstraintAndCamera();
  projectResetAndDestructionLeaveNoStaleState();
  std::cout << "Sketch interaction controller tests passed\n";
  return EXIT_SUCCESS;
}
