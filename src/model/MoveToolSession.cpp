#include "model/MoveToolSession.h"

#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>
#include <utility>

namespace solidar {
namespace {

constexpr double kMinimumMoveMm = -100000.0;
constexpr double kMaximumMoveMm = 100000.0;

double clampedFinite(double value) {
  if (!std::isfinite(value)) return 0.0;
  return std::clamp(value, kMinimumMoveMm, kMaximumMoveMm);
}

}  // namespace

void MoveToolSession::begin(Vector3d offsetMm,
                            std::optional<FeatureId> editingFeatureId) {
  bodyId_ = kInvalidBodyId;
  sourceFeatureId_ = kInvalidFeatureId;
  sourceShape_.reset();
  offsetMm_ = {clampedFinite(offsetMm.x), clampedFinite(offsetMm.y),
               clampedFinite(offsetMm.z)};
  editingFeatureId_ = editingFeatureId;
  previewShape_.reset();
  error_.clear();
  lifecycle_ = ToolLifecycle::SelectingInput;
}

void MoveToolSession::setBody(BodyId bodyId, FeatureId sourceFeatureId,
                              ShapeFeature::ShapePtr sourceShape) {
  bodyId_ = bodyId;
  sourceFeatureId_ = sourceFeatureId;
  sourceShape_ = std::move(sourceShape);
  updatePreview();
}

void MoveToolSession::clearBody() {
  bodyId_ = kInvalidBodyId;
  sourceFeatureId_ = kInvalidFeatureId;
  sourceShape_.reset();
  updatePreview();
}

void MoveToolSession::setOffsetMm(Vector3d offsetMm) {
  offsetMm_ = {clampedFinite(offsetMm.x), clampedFinite(offsetMm.y),
               clampedFinite(offsetMm.z)};
  updatePreview();
}

void MoveToolSession::setOffsetComponent(int axisIndex, double valueMm) {
  const double value = clampedFinite(valueMm);
  if (axisIndex == 0)
    offsetMm_.x = value;
  else if (axisIndex == 1)
    offsetMm_.y = value;
  else if (axisIndex == 2)
    offsetMm_.z = value;
  else
    return;
  updatePreview();
}

BodyId MoveToolSession::bodyId() const noexcept { return bodyId_; }

FeatureId MoveToolSession::sourceFeatureId() const noexcept {
  return sourceFeatureId_;
}

Vector3d MoveToolSession::offsetMm() const noexcept { return offsetMm_; }

std::optional<FeatureId> MoveToolSession::editingFeatureId() const noexcept {
  return editingFeatureId_;
}

ToolLifecycle MoveToolSession::lifecycle() const noexcept { return lifecycle_; }

ToolSelectionStage MoveToolSession::selectionStage() const noexcept {
  if (lifecycle_ == ToolLifecycle::Inactive) return ToolSelectionStage::None;
  if (bodyId_ == kInvalidBodyId) return ToolSelectionStage::SelectingInput;
  return ToolSelectionStage::EditingParameters;
}

std::optional<SelectionRequirement> MoveToolSession::selectionRequirement()
    const {
  if (selectionStage() == ToolSelectionStage::SelectingInput)
    return SelectionRequirement{SelectionType::Body, "Select body", 1, 1,
                                false};
  return std::nullopt;
}

std::vector<ToolParameterDescriptor> MoveToolSession::parameters() const {
  const auto distance = [](const char* id, const char* label, double value) {
    return ToolParameterDescriptor{id, label, ToolParameterType::Distance,
                                   value, kMinimumMoveMm, kMaximumMoveMm, 0.1,
                                   "mm", true, ToolManipulatorType::Linear};
  };
  return {distance("offset_x", "X", offsetMm_.x),
          distance("offset_y", "Y", offsetMm_.y),
          distance("offset_z", "Z", offsetMm_.z)};
}

std::shared_ptr<const TopoDS_Shape> MoveToolSession::previewShape() const {
  return previewShape_;
}

const std::string& MoveToolSession::error() const noexcept { return error_; }

std::optional<TranslationToolManipulator> MoveToolSession::manipulator() const {
  if (!sourceShape_ || sourceShape_->IsNull()) return std::nullopt;
  Bnd_Box bounds;
  BRepBndLib::Add(*sourceShape_, bounds);
  if (bounds.IsVoid()) return std::nullopt;
  double minX = 0.0;
  double minY = 0.0;
  double minZ = 0.0;
  double maxX = 0.0;
  double maxY = 0.0;
  double maxZ = 0.0;
  bounds.Get(minX, minY, minZ, maxX, maxY, maxZ);
  return TranslationToolManipulator{
      {(minX + maxX) * 0.5 + offsetMm_.x,
       (minY + maxY) * 0.5 + offsetMm_.y,
       (minZ + maxZ) * 0.5 + offsetMm_.z},
      offsetMm_, kMinimumMoveMm, kMaximumMoveMm};
}

bool MoveToolSession::updatePreview() {
  previewShape_.reset();
  error_.clear();
  if (bodyId_ == kInvalidBodyId || sourceFeatureId_ == kInvalidFeatureId ||
      !sourceShape_) {
    lifecycle_ = ToolLifecycle::SelectingInput;
    return false;
  }
  previewShape_ = buildMovedShape(*sourceShape_, offsetMm_, &error_);
  lifecycle_ = previewShape_ ? ToolLifecycle::PreviewValid
                             : ToolLifecycle::PreviewInvalid;
  return static_cast<bool>(previewShape_);
}

void MoveToolSession::cancel() noexcept {
  bodyId_ = kInvalidBodyId;
  sourceFeatureId_ = kInvalidFeatureId;
  sourceShape_.reset();
  offsetMm_ = {};
  editingFeatureId_.reset();
  previewShape_.reset();
  error_.clear();
  lifecycle_ = ToolLifecycle::Inactive;
}

}  // namespace solidar
