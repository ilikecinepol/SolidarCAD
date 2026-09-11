#include "model/ChamferToolSession.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>

#include "model/ChamferBuilder.h"
#include "model/EdgeManipulatorGeometry.h"
#include "model/TopologyReferenceResolver.h"

namespace solidar {
namespace {

double conservativeCap(const TopoDS_Shape& shape) {
  try {
    Bnd_Box bounds;
    BRepBndLib::Add(shape, bounds);
    if (bounds.IsVoid()) return 0.0;
    double xmin, ymin, zmin, xmax, ymax, zmax;
    bounds.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    const double smallestExtent = std::min(
        {std::abs(xmax - xmin), std::abs(ymax - ymin), std::abs(zmax - zmin)});
    return std::isfinite(smallestExtent) && smallestExtent > 0.0
               ? smallestExtent * 0.25
               : 0.0;
  } catch (const Standard_Failure&) {
    return 0.0;
  } catch (...) {
    return 0.0;
  }
}

}  // namespace

ChamferToolSession::ChamferToolSession(BuildShape buildShape)
    : buildShape_(std::move(buildShape)) {
  if (!buildShape_) buildShape_ = buildChamferShape;
}

void ChamferToolSession::begin(BodyId bodyId, FeatureId sourceFeatureId,
                               ShapeFeature::ShapePtr baseShape,
                               std::vector<EdgeReference> edges,
                               double distanceMm,
                               std::optional<FeatureId> editingFeatureId) {
  bodyId_ = bodyId;
  sourceFeatureId_ = sourceFeatureId;
  baseShape_ = std::move(baseShape);
  edges_ = std::move(edges);
  const double initialDistance = editingFeatureId ? distanceMm : 0.0;
  const double cap = baseShape_ && !baseShape_->IsNull()
                         ? conservativeCap(*baseShape_)
                         : 0.0;
  distance_.reset(initialDistance, 0.0, std::max(cap, initialDistance));
  editingFeatureId_ = editingFeatureId;
  lifecycle_ = ToolLifecycle::Editing;
  if (updatePreview() && !edges_.empty()) {
    std::vector<std::size_t> indices;
    for (const auto& edge : edges_) {
      const auto resolved = resolveEdgeReference(*baseShape_, edge.topology());
      if (!resolved) return;
      indices.push_back(resolved.index);
    }
    updateValidatedMaximum(indices);
  }
}

void ChamferToolSession::setEdges(std::vector<EdgeReference> edges) {
  edges_ = std::move(edges);
  if (!updatePreview() || !baseShape_ || edges_.empty()) return;
  std::vector<std::size_t> indices;
  for (const auto& edge : edges_) {
    const auto resolved = resolveEdgeReference(*baseShape_, edge.topology());
    if (!resolved) return;
    indices.push_back(resolved.index);
  }
  updateValidatedMaximum(indices);
}
bool ChamferToolSession::setDistanceFromPanel(double distanceMm) {
  return trySetDistance(distanceMm);
}
bool ChamferToolSession::setDistanceFromManipulator(double distanceMm) {
  return trySetDistance(distanceMm);
}

BodyId ChamferToolSession::bodyId() const noexcept { return bodyId_; }
FeatureId ChamferToolSession::sourceFeatureId() const noexcept {
  return sourceFeatureId_;
}
std::optional<FeatureId> ChamferToolSession::editingFeatureId() const noexcept {
  return editingFeatureId_;
}
const std::vector<EdgeReference>& ChamferToolSession::edges() const noexcept {
  return edges_;
}
double ChamferToolSession::distanceMm() const noexcept { return distance_.value(); }
ToolLifecycle ChamferToolSession::lifecycle() const noexcept {
  return lifecycle_;
}
ToolSelectionStage ChamferToolSession::selectionStage() const noexcept {
  if (lifecycle_ == ToolLifecycle::Inactive) return ToolSelectionStage::None;
  return edges_.empty() ? ToolSelectionStage::SelectingInput
                        : ToolSelectionStage::EditingParameters;
}
std::optional<SelectionRequirement> ChamferToolSession::selectionRequirement() const {
  if (!edges_.empty()) return std::nullopt;
  return SelectionRequirement{SelectionType::Edge, "Select edges", 1,
                              static_cast<std::size_t>(-1), true};
}
std::vector<ToolParameterDescriptor> ChamferToolSession::parameters() const {
  return {{"distance", "Distance", ToolParameterType::Distance,
            distance_.value(), distance_.minimum(), distance_.maximum(), 0.1,
            "mm", true, ToolManipulatorType::Linear}};
}
std::shared_ptr<const TopoDS_Shape> ChamferToolSession::previewShape() const {
  return previewShape_;
}
const std::string& ChamferToolSession::error() const noexcept { return error_; }

bool ChamferToolSession::updatePreview() {
  previewShape_.reset();
  error_.clear();
  if (!baseShape_ || baseShape_->IsNull()) {
    error_ = "Chamfer base shape is missing";
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
      error_ = "Chamfer edges no longer match the active Body";
      lifecycle_ = ToolLifecycle::PreviewInvalid;
      return false;
    }
    const auto resolved = resolveEdgeReference(*baseShape_, edge.topology());
    if (!resolved) {
      error_ = "Chamfer edge could not be resolved: " + resolved.error;
      lifecycle_ = ToolLifecycle::PreviewInvalid;
      return false;
    }
    indices.push_back(resolved.index);
  }
  if (distance_.value() <= 0.0) {
    previewShape_ = baseShape_;
    lifecycle_ = ToolLifecycle::EditingParameters;
    return true;
  }
  previewShape_ = buildShape_(*baseShape_, indices, distance_.value(), &error_);
  lifecycle_ = previewShape_ ? ToolLifecycle::PreviewValid
                             : ToolLifecycle::PreviewInvalid;
  return static_cast<bool>(previewShape_);
}

std::optional<LinearToolManipulator> ChamferToolSession::manipulator() const {
  if (!baseShape_ || edges_.empty()) return std::nullopt;
  try {
    const auto edge =
        resolveEdgeReference(*baseShape_, edges_.front().topology());
    if (!edge) return std::nullopt;
    const auto geometry = localEdgeManipulatorGeometry(*baseShape_, *edge.subshape);
    if (!geometry) return std::nullopt;
    return LinearToolManipulator{geometry->midpoint,
                                 geometry->outwardDirection, distance_.value(),
                                  distance_.minimum(), distance_.maximum()};
  } catch (const Standard_Failure&) {
    return std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}

bool ChamferToolSession::trySetDistance(double distanceMm) {
  const auto candidate = distance_.candidate(distanceMm);
  if (!candidate || !baseShape_ || edges_.empty()) return false;
  if (*candidate == distance_.value()) return true;
  const double previous = distance_.value();
  const auto previousPreview = previewShape_;
  const auto previousLifecycle = lifecycle_;
  const auto previousError = error_;
  distance_.accept(*candidate);
  if (updatePreview()) return true;
  distance_.accept(previous);
  previewShape_ = previousPreview;
  lifecycle_ = previousLifecycle;
  error_ = previousError;
  // This is a conservative interaction boundary, not a claim that OCCT
  // validity is mathematically monotonic within the remaining range.
  distance_.setRange(distance_.minimum(), previous);
  return false;
}

void ChamferToolSession::updateValidatedMaximum(
    const std::vector<std::size_t>& edgeIndices) {
  if (!baseShape_ || baseShape_->IsNull()) return;
  const double current = distance_.value();
  double probe = std::max(conservativeCap(*baseShape_), current);
  constexpr int kMaximumValidationProbes = 8;
  for (int attempt = 0;
       attempt < kMaximumValidationProbes && probe > current + 1e-9;
       ++attempt) {
    std::string ignoredError;
    if (buildShape_(*baseShape_, edgeIndices, probe, &ignoredError)) {
      distance_.setRange(0.0, probe);
      return;
    }
    probe *= 0.5;
  }
  distance_.setRange(0.0, current);
}

void ChamferToolSession::cancel() noexcept {
  previewShape_.reset();
  edges_.clear();
  error_.clear();
  lifecycle_ = ToolLifecycle::Inactive;
}

}  // namespace solidar
