#include "ui/SketchInteractionController.h"

namespace solidar {

const SketchToolState& SketchInteractionController::snapshot() const noexcept {
  return state_;
}

SketchInteractionTool SketchInteractionController::tool() const noexcept {
  return state_.tool;
}

bool SketchInteractionController::hasActiveGesture() const noexcept {
  const auto& state = state_;
  const auto& dimension = state.dimension;
  const auto& automatic = state.autoDimension;
  const auto& constraint = state.constraint;
  const auto& line = state.lineCreation;
  const auto& creation = state.creation;
  return !std::holds_alternative<std::monostate>(state.pointDrag) ||
         dimension.selected || dimension.draggingLine ||
         dimension.draggingLabel || dimension.editing ||
         automatic.target != SketchAutoDimensionTarget::None ||
         automatic.geometryId || automatic.firstPoint ||
         automatic.secondPoint || automatic.firstDatum ||
         automatic.offsetMm || automatic.angleRad ||
         automatic.directLineId || automatic.angleFirstLine ||
         automatic.angleSecondLine || automatic.distanceFirstLine ||
         automatic.distanceSecondLine ||
         constraint.pointOnLineCarrier || constraint.pointOnCircleCarrier ||
         constraint.perpendicularFirstLine || constraint.parallelFirstLine ||
         constraint.equalFirst || constraint.tangentFirst ||
         constraint.hoverOperand || constraint.hoverPoint ||
         constraint.coincidentFirstPoint ||
         line.startLineCarrier || line.endLineCarrier ||
         line.startMidpointCarrier || line.endMidpointCarrier ||
         line.startArcCarrier || line.endArcCarrier || creation.anchor ||
         !creation.rectanglePoints.empty() || !creation.circlePoints.empty() ||
         !creation.arcPoints.empty() || !creation.circleGuideIds.empty() ||
         !state.mirror.source.empty() || state.trim.preview ||
         state.arc.chordAngleRad || state.arc.sagittaSign ||
         state.arc.dimensionKeyboardEdit ||
         state.camera.kind != SketchCameraGestureState::Kind::None ||
         state.selectionBox.active || state.drag.active ||
         state.twoTangentRadiusPreviewActive;
}

void SketchInteractionController::resetTransient(
    SketchToolState& state) noexcept {
  const auto tool = state.tool;
  const auto camera = state.camera;
  const auto labelAlong = std::move(state.dimension.labelAlongMm);
  const auto labelOffset = std::move(state.dimension.labelOffsetMm);
  state = {};
  state.tool = tool;
  // View pan is persistent presentation state, not a tool gesture. Preserve
  // it while cancelling/switching tools; project reset clears it explicitly.
  state.camera.panX = camera.panX;
  state.camera.panY = camera.panY;
  state.dimension.labelAlongMm = std::move(labelAlong);
  state.dimension.labelOffsetMm = std::move(labelOffset);
}

void SketchInteractionController::switchTool(
    SketchInteractionTool tool) noexcept {
  resetTransient(state_);
  state_.tool = tool;
}

void SketchInteractionController::resetForProject() noexcept {
  state_ = {};
}

void SketchInteractionController::cancelGesture() noexcept {
  resetTransient(state_);
}

void SketchInteractionController::beginCreation(
    SketchCreationGestureState creation) noexcept {
  state_.creation = std::move(creation);
}

void SketchInteractionController::updateCreation(
    SketchCreationGestureState creation) noexcept {
  state_.creation = std::move(creation);
}

void SketchInteractionController::setCreationAnchor(
    std::optional<sketch::Point> anchor) noexcept {
  state_.creation.anchor = std::move(anchor);
}

void SketchInteractionController::appendRectanglePoint(sketch::Point point) {
  state_.creation.rectanglePoints.push_back(point);
}

void SketchInteractionController::clearRectanglePoints() noexcept {
  state_.creation.rectanglePoints.clear();
}

void SketchInteractionController::appendCirclePoint(sketch::Point point) {
  state_.creation.circlePoints.push_back(point);
}

void SketchInteractionController::clearCirclePoints() noexcept {
  state_.creation.circlePoints.clear();
}

void SketchInteractionController::appendArcPoint(sketch::Point point) {
  state_.creation.arcPoints.push_back(point);
}

void SketchInteractionController::clearArcPoints() noexcept {
  state_.creation.arcPoints.clear();
}

void SketchInteractionController::appendCircleGuide(sketch::GeometryId lineId) {
  if (lineId != sketch::kInvalidGeometryId)
    state_.creation.circleGuideIds.push_back(lineId);
}

void SketchInteractionController::clearCircleGuides() noexcept {
  state_.creation.circleGuideIds.clear();
}

void SketchInteractionController::updateLineCreation(
    SketchLineCreationState creation) noexcept {
  state_.lineCreation = std::move(creation);
}

void SketchInteractionController::updateArcCreation(
    SketchArcInteractionState arc) noexcept {
  state_.arc = std::move(arc);
}

void SketchInteractionController::setTwoTangentPreview(bool active) noexcept {
  state_.twoTangentRadiusPreviewActive = active;
}

void SketchInteractionController::completeCreation() noexcept {
  state_.creation = {};
  state_.lineCreation = {};
  state_.arc = {};
  state_.twoTangentRadiusPreviewActive = false;
}

void SketchInteractionController::cancelCreation() noexcept {
  completeCreation();
}

void SketchInteractionController::beginAutoDimension(
    SketchAutoDimensionState automatic) noexcept {
  state_.autoDimension = std::move(automatic);
}

void SketchInteractionController::updateAutoDimension(
    SketchAutoDimensionState automatic) noexcept {
  state_.autoDimension = std::move(automatic);
}

void SketchInteractionController::completeAutoDimension() noexcept {
  state_.autoDimension = {};
  state_.dimension.editing.reset();
}

void SketchInteractionController::cancelAutoDimension() noexcept {
  completeAutoDimension();
}

void SketchInteractionController::updateDimension(
    SketchDimensionInteractionState dimension) noexcept {
  state_.dimension = std::move(dimension);
}

void SketchInteractionController::selectDimension(
    std::optional<SketchDimensionReference> dimension) noexcept {
  state_.dimension.selected = std::move(dimension);
}

void SketchInteractionController::beginDimensionLabelDrag(
    SketchDimensionReference dimension) noexcept {
  state_.dimension.selected = dimension;
  state_.dimension.draggingLabel = std::move(dimension);
}

void SketchInteractionController::beginDimensionLineDrag(
    SketchDimensionReference dimension) noexcept {
  state_.dimension.selected = dimension;
  state_.dimension.draggingLine = std::move(dimension);
}

void SketchInteractionController::endDimensionLabelDrag() noexcept {
  state_.dimension.draggingLabel.reset();
}

void SketchInteractionController::endDimensionLineDrag() noexcept {
  state_.dimension.draggingLine.reset();
}

void SketchInteractionController::clearDimensionLabels() noexcept {
  state_.dimension.labelAlongMm.clear();
  state_.dimension.labelOffsetMm.clear();
}

void SketchInteractionController::setDimensionLabelPosition(
    std::size_t index, double alongMm, double offsetMm) {
  while (state_.dimension.labelAlongMm.size() <= index)
    state_.dimension.labelAlongMm.push_back(0.0);
  while (state_.dimension.labelOffsetMm.size() <= index)
    state_.dimension.labelOffsetMm.push_back(0.0);
  state_.dimension.labelAlongMm[index] = alongMm;
  state_.dimension.labelOffsetMm[index] = offsetMm;
}

void SketchInteractionController::eraseDimensionLabel(
    std::size_t index) noexcept {
  if (index < state_.dimension.labelAlongMm.size())
    state_.dimension.labelAlongMm.erase(
        state_.dimension.labelAlongMm.begin() + index);
  if (index < state_.dimension.labelOffsetMm.size())
    state_.dimension.labelOffsetMm.erase(
        state_.dimension.labelOffsetMm.begin() + index);
}

void SketchInteractionController::appendDimensionLabel(
    double alongMm, double offsetMm) {
  state_.dimension.labelAlongMm.push_back(alongMm);
  state_.dimension.labelOffsetMm.push_back(offsetMm);
}

void SketchInteractionController::completeDimensionInteraction() noexcept {
  state_.dimension.selected.reset();
  state_.dimension.draggingLine.reset();
  state_.dimension.draggingLabel.reset();
  state_.dimension.editing.reset();
}

void SketchInteractionController::cancelDimensionInteraction() noexcept {
  completeDimensionInteraction();
  state_.autoDimension = {};
}

void SketchInteractionController::beginConstraintOperands(
    SketchConstraintInteractionState constraint) noexcept {
  state_.constraint = std::move(constraint);
}

void SketchInteractionController::updateConstraintOperands(
    SketchConstraintInteractionState constraint) noexcept {
  state_.constraint = std::move(constraint);
}

void SketchInteractionController::setPointOnLineCarrier(
    std::optional<sketch::GeometryId> carrier) noexcept {
  state_.constraint.pointOnLineCarrier = carrier;
}

void SketchInteractionController::setPointOnCircleCarrier(
    std::optional<sketch::GeometryId> carrier) noexcept {
  state_.constraint.pointOnCircleCarrier = carrier;
}

void SketchInteractionController::setPerpendicularFirstLine(
    std::optional<sketch::GeometryId> line) noexcept {
  state_.constraint.perpendicularFirstLine = line;
}

void SketchInteractionController::setParallelFirstLine(
    std::optional<sketch::GeometryId> line) noexcept {
  state_.constraint.parallelFirstLine = line;
}

void SketchInteractionController::setEqualFirst(
    std::optional<SketchGeometryOperand> operand) noexcept {
  state_.constraint.equalFirst = std::move(operand);
}

void SketchInteractionController::setTangentFirst(
    std::optional<SketchGeometryOperand> operand) noexcept {
  state_.constraint.tangentFirst = std::move(operand);
}

void SketchInteractionController::setCoincidentFirstPoint(
    std::optional<sketch::PointReference> point) noexcept {
  state_.constraint.coincidentFirstPoint = point;
}

void SketchInteractionController::completeConstraintOperands() noexcept {
  state_.constraint = {};
}

void SketchInteractionController::cancelConstraintOperands() noexcept {
  completeConstraintOperands();
}

void SketchInteractionController::updateMirror(
    SketchMirrorInteractionState mirror) noexcept {
  state_.mirror = std::move(mirror);
}

void SketchInteractionController::updateTrim(
    SketchTrimInteractionState trim) noexcept {
  state_.trim = std::move(trim);
}

void SketchInteractionController::beginPointDrag(
    SketchPointDragTarget target, sketch::Point current) noexcept {
  state_.pointDrag = std::move(target);
  state_.drag.active = true;
  state_.drag.current = current;
}

void SketchInteractionController::updatePointDrag(
    sketch::Point current) noexcept {
  state_.drag.current = current;
}

void SketchInteractionController::completePointDrag() noexcept {
  state_.pointDrag = std::monostate{};
  state_.drag = {};
}

void SketchInteractionController::cancelPointDrag() noexcept {
  completePointDrag();
}

void SketchInteractionController::beginSelectionBox(
    SketchScreenPoint start, bool additive) noexcept {
  state_.selectionBox = {true, start, start, additive};
}

void SketchInteractionController::updateSelectionBox(
    SketchScreenPoint current) noexcept {
  if (state_.selectionBox.active)
    state_.selectionBox.current = current;
}

void SketchInteractionController::completeSelectionBox() noexcept {
  state_.selectionBox = {};
}

void SketchInteractionController::cancelSelectionBox() noexcept {
  completeSelectionBox();
}

void SketchInteractionController::beginDimensionEdit(
    SketchDimensionReference dimension) noexcept {
  state_.dimension.editing = dimension;
  state_.dimension.selected = std::move(dimension);
}

void SketchInteractionController::beginCameraGesture(
    SketchCameraGestureState::Kind kind,
    SketchCameraGestureState::Button button, double x, double y) noexcept {
  state_.camera.kind = kind;
  state_.camera.button = button;
  state_.camera.lastX = x;
  state_.camera.lastY = y;
  state_.camera.moved = false;
}

void SketchInteractionController::updateCameraGesture(
    double x, double y, bool moved) noexcept {
  state_.camera.lastX = x;
  state_.camera.lastY = y;
  state_.camera.moved = state_.camera.moved || moved;
}

void SketchInteractionController::panCameraBy(
    double dx, double dy, double x, double y) noexcept {
  state_.camera.panX += dx;
  state_.camera.panY += dy;
  state_.camera.lastX = x;
  state_.camera.lastY = y;
}

void SketchInteractionController::setCameraPan(double x, double y) noexcept {
  state_.camera.panX = x;
  state_.camera.panY = y;
}

void SketchInteractionController::endCameraGesture() noexcept {
  state_.camera.kind = SketchCameraGestureState::Kind::None;
  state_.camera.button = SketchCameraGestureState::Button::None;
  state_.camera.moved = false;
}

}  // namespace solidar
