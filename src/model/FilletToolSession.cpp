#include "model/FilletToolSession.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>
#include <Standard_Failure.hxx>

#include <algorithm>
#include <cmath>

#include "model/FilletBuilder.h"
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

FilletToolSession::FilletToolSession(BuildShape buildShape)
    : buildShape_(std::move(buildShape)) {
  if (!buildShape_) buildShape_ = buildFilletShape;
}

void FilletToolSession::begin(BodyId bodyId, FeatureId sourceFeatureId,
                              ShapeFeature::ShapePtr baseShape,
                              std::vector<EdgeReference> edges, double radiusMm,
                              std::optional<FeatureId> editingFeatureId) {
  bodyId_ = bodyId;
  sourceFeatureId_ = sourceFeatureId;
  baseShape_ = std::move(baseShape);
  edges_ = std::move(edges);
  const double initialRadius = editingFeatureId ? radiusMm : 0.0;
  const double cap = baseShape_ && !baseShape_->IsNull()
                         ? conservativeCap(*baseShape_)
                         : 0.0;
  radius_.reset(initialRadius, 0.0, std::max(cap, initialRadius));
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

void FilletToolSession::setEdges(std::vector<EdgeReference> edges) {
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

bool FilletToolSession::setRadiusFromPanel(double radiusMm) {
  return trySetRadius(radiusMm);
}

bool FilletToolSession::setRadiusFromManipulator(double radiusMm) {
  return trySetRadius(radiusMm);
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
  return {{"radius", "Radius", ToolParameterType::Distance, radius_.value(),
            radius_.minimum(), radius_.maximum(), 0.1, "mm", true,
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
  previewShape_ = buildShape_(*baseShape_, indices, radius_.value(), &error_);
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
                                  radius_.minimum(), radius_.maximum()};
  } catch (const Standard_Failure&) {
    return std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}

bool FilletToolSession::trySetRadius(double radiusMm) {
  const auto candidate = radius_.candidate(radiusMm);
  if (!candidate || !baseShape_ || edges_.empty()) return false;
  if (*candidate == radius_.value()) return true;
  const double previous = radius_.value();
  const auto previousPreview = previewShape_;
  const auto previousLifecycle = lifecycle_;
  const auto previousError = error_;
  radius_.accept(*candidate);
  if (updatePreview()) return true;
  radius_.accept(previous);
  previewShape_ = previousPreview;
  lifecycle_ = previousLifecycle;
  error_ = previousError;
  // A rejected growing candidate establishes a conservative operational
  // boundary at the last builder-validated value. Values inside a range are
  // still individually validated; OCCT does not promise a monotonic domain.
  radius_.setRange(radius_.minimum(), previous);
  return false;
}

void FilletToolSession::updateValidatedMaximum(
    const std::vector<std::size_t>& edgeIndices) {
  if (!baseShape_ || baseShape_->IsNull()) return;
  const double current = radius_.value();
  double probe = std::max(conservativeCap(*baseShape_), current);
  constexpr int kMaximumValidationProbes = 8;
  for (int attempt = 0;
       attempt < kMaximumValidationProbes && probe > current + 1e-9;
       ++attempt) {
    std::string ignoredError;
    if (buildShape_(*baseShape_, edgeIndices, probe, &ignoredError)) {
      radius_.setRange(0.0, probe);
      return;
    }
    probe *= 0.5;
  }
  // The current preview was already accepted; do not advertise an unvalidated
  // value above it when the conservative probe was rejected.
  radius_.setRange(0.0, current);
}

void FilletToolSession::cancel() noexcept {
  previewShape_.reset();
  edges_.clear();
  error_.clear();
  lifecycle_ = ToolLifecycle::Inactive;
}

}  // namespace solidar
