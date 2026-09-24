#include "model/FilletToolSession.h"

#include <BRepAdaptor_Curve.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>
#include <Standard_Failure.hxx>

#include <algorithm>
#include <cmath>

#include "model/FilletBuilder.h"
#include "model/EdgeManipulatorGeometry.h"
#include "model/TopologyReferenceResolver.h"

namespace solidar {

void FilletToolSession::begin(BodyId bodyId, FeatureId sourceFeatureId,
                              ShapeFeature::ShapePtr baseShape,
                              std::vector<EdgeReference> edges, double radiusMm,
                              std::optional<FeatureId> editingFeatureId) {
  bodyId_ = bodyId;
  sourceFeatureId_ = sourceFeatureId;
  baseShape_ = std::move(baseShape);
  edges_ = std::move(edges);
  radius_.reset(radiusMm, 0.0, 100000.0);
  editingFeatureId_ = editingFeatureId;
  maximumValidRadiusMm_.reset();
  limitReached_ = false;
  lifecycle_ = ToolLifecycle::Editing;
  updatePreview();
}

void FilletToolSession::setEdges(std::vector<EdgeReference> edges) {
  edges_ = std::move(edges);
  maximumValidRadiusMm_.reset();
  limitReached_ = false;
  updatePreview();
}

void FilletToolSession::setRadiusFromPanel(double radiusMm) {
  trySetRadius(radiusMm, false);
}

void FilletToolSession::setRadiusFromManipulator(double radiusMm) {
  trySetRadius(radiusMm, true);
}

BodyId FilletToolSession::bodyId() const noexcept { return bodyId_; }
FeatureId FilletToolSession::sourceFeatureId() const noexcept {
  return sourceFeatureId_;
}
std::optional<FeatureId> FilletToolSession::editingFeatureId() const noexcept {
  return editingFeatureId_;
}
const std::vector<EdgeReference>& FilletToolSession::edges() const noexcept {
  return edges_;
}
double FilletToolSession::radiusMm() const noexcept { return radius_.value(); }
std::optional<double> FilletToolSession::maximumValidRadiusMm() const noexcept {
  return maximumValidRadiusMm_;
}
bool FilletToolSession::limitReached() const noexcept { return limitReached_; }
ToolLifecycle FilletToolSession::lifecycle() const noexcept {
  return lifecycle_;
}
ToolSelectionStage FilletToolSession::selectionStage() const noexcept {
  if (lifecycle_ == ToolLifecycle::Inactive) return ToolSelectionStage::None;
  return edges_.empty() ? ToolSelectionStage::SelectingInput
                        : ToolSelectionStage::EditingParameters;
}
std::optional<SelectionRequirement> FilletToolSession::selectionRequirement() const {
  if (!edges_.empty()) return std::nullopt;
  return SelectionRequirement{SelectionType::Edge, "Select edges", 1,
                              static_cast<std::size_t>(-1), true};
}
std::vector<ToolParameterDescriptor> FilletToolSession::parameters() const {
  return {{"radius", "Radius", ToolParameterType::Distance, radius_.value(), 0.0,
           maximumValidRadiusMm_.value_or(100000.0), 0.1, "mm", true,
           ToolManipulatorType::Linear}};
}
std::shared_ptr<const TopoDS_Shape> FilletToolSession::previewShape() const {
  return previewShape_;
}
const std::string& FilletToolSession::error() const noexcept { return error_; }

bool FilletToolSession::updatePreview() {
  previewShape_.reset();
  error_.clear();
  if (!baseShape_ || baseShape_->IsNull()) {
    error_ = "Fillet base shape is missing";
    lifecycle_ = ToolLifecycle::PreviewInvalid;
    return false;
  }
  if (edges_.empty()) {
    lifecycle_ = ToolLifecycle::SelectingInput;
    return false;
  }
  std::vector<std::size_t> indices;
  indices.reserve(edges_.size());
  for (const auto& edge : edges_) {
    if (edge.bodyId != bodyId_ || edge.featureId != sourceFeatureId_) {
      error_ = "Fillet edges no longer match the active Body";
      lifecycle_ = ToolLifecycle::PreviewInvalid;
      return false;
    }
    const auto resolved = resolveEdgeReference(*baseShape_, edge.topology());
    if (!resolved) {
      error_ = "Fillet edge could not be resolved";
      lifecycle_ = ToolLifecycle::PreviewInvalid;
      return false;
    }
    indices.push_back(resolved.index);
  }
  if (radius_.value() <= 0.0) {
    previewShape_ = baseShape_;
    lifecycle_ = ToolLifecycle::EditingParameters;
    return true;
  }
  previewShape_ = buildFilletShape(*baseShape_, indices, radius_.value(), &error_);
  lifecycle_ = previewShape_ ? ToolLifecycle::PreviewValid
                             : ToolLifecycle::PreviewInvalid;
  return static_cast<bool>(previewShape_);
}

std::optional<LinearToolManipulator> FilletToolSession::manipulator() const {
  if (!baseShape_ || edges_.empty()) return std::nullopt;
  try {
    const auto edge =
        resolveEdgeReference(*baseShape_, edges_.front().topology());
    if (!edge) return std::nullopt;
    const auto geometry = localEdgeManipulatorGeometry(*baseShape_, *edge.subshape);
    if (!geometry) return std::nullopt;
    return LinearToolManipulator{geometry->midpoint,
                                 geometry->outwardDirection, radius_.value(),
                                 0.0,
                                 maximumValidRadiusMm_.value_or(100000.0)};
  } catch (const Standard_Failure&) {
    return std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}

bool FilletToolSession::trySetRadius(double radiusMm, bool clampToBoundary) {
  const auto candidate = radius_.candidate(radiusMm);
  if (!candidate || !baseShape_ || edges_.empty()) return false;
  const double previous = radius_.value();
  const auto previousPreview = previewShape_;
  radius_.accept(*candidate);
  if (updatePreview()) {
    limitReached_ = false;
    return true;
  }
  const std::string failure = error_.empty()
                                  ? "Fillet preview could not be built"
                                  : error_;

  if (*candidate > previous && previousPreview) {
    std::vector<std::size_t> indices;
    indices.reserve(edges_.size());
    for (const auto& edge : edges_) {
      const auto resolved = resolveEdgeReference(*baseShape_, edge.topology());
      if (!resolved) {
        indices.clear();
        break;
      }
      indices.push_back(resolved.index);
    }
    if (!indices.empty()) {
      double lower = previous;
      double upper = *candidate;
      auto boundaryPreview = previousPreview;
      for (int iteration = 0;
           iteration < 24 && upper - lower > 1e-4; ++iteration) {
        const double midpoint = lower + (upper - lower) * 0.5;
        auto preview = buildFilletShape(*baseShape_, indices, midpoint);
        if (preview) {
          lower = midpoint;
          boundaryPreview = std::move(preview);
        } else {
          upper = midpoint;
        }
      }
      const double panelSafe = std::floor((lower + 1e-9) * 100.0) / 100.0;
      if (panelSafe >= previous && panelSafe < lower) {
        if (auto preview = buildFilletShape(*baseShape_, indices, panelSafe)) {
          lower = panelSafe;
          boundaryPreview = std::move(preview);
        }
      }
      maximumValidRadiusMm_ = lower;
      limitReached_ = true;
      if (clampToBoundary) {
        radius_.accept(lower);
        previewShape_ = std::move(boundaryPreview);
        lifecycle_ = ToolLifecycle::PreviewValid;
        error_.clear();
        return true;
      }
    }
  }

  radius_.accept(previous);
  previewShape_ = previousPreview;
  lifecycle_ = ToolLifecycle::PreviewInvalid;
  error_ = failure;
  return false;
}

void FilletToolSession::cancel() noexcept {
  previewShape_.reset();
  edges_.clear();
  error_.clear();
  maximumValidRadiusMm_.reset();
  limitReached_ = false;
  lifecycle_ = ToolLifecycle::Inactive;
}

}  // namespace solidar
