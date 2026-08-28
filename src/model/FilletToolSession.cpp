#include "model/FilletToolSession.h"

#include <BRepAdaptor_Curve.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>

#include "model/FilletBuilder.h"
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
  radiusMm_ = radiusMm;
  editingFeatureId_ = editingFeatureId;
  lifecycle_ = ToolLifecycle::Editing;
  updatePreview();
}

void FilletToolSession::setEdges(std::vector<EdgeReference> edges) {
  edges_ = std::move(edges);
  updatePreview();
}

void FilletToolSession::setRadiusFromPanel(double radiusMm) {
  radiusMm_ = radiusMm;
  updatePreview();
}

void FilletToolSession::setRadiusFromManipulator(double radiusMm) {
  radiusMm_ = std::max(0.01, radiusMm);
  updatePreview();
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
double FilletToolSession::radiusMm() const noexcept { return radiusMm_; }
ToolLifecycle FilletToolSession::lifecycle() const noexcept { return lifecycle_; }
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
  std::vector<std::size_t> indices;
  indices.reserve(edges_.size());
  for (const auto& edge : edges_) {
    if (edge.bodyId != bodyId_ || edge.featureId != sourceFeatureId_) {
      error_ = "Fillet edges no longer match the active Body";
      lifecycle_ = ToolLifecycle::PreviewInvalid;
      return false;
    }
    indices.push_back(edge.edgeIndex);
  }
  previewShape_ = buildFilletShape(*baseShape_, indices, radiusMm_, &error_);
  lifecycle_ = previewShape_ ? ToolLifecycle::PreviewValid
                             : ToolLifecycle::PreviewInvalid;
  return static_cast<bool>(previewShape_);
}

std::optional<LinearToolManipulator> FilletToolSession::manipulator() const {
  if (!baseShape_ || edges_.empty()) return std::nullopt;
  const auto edge = resolveEdge(*baseShape_, edges_.front().edgeIndex);
  if (!edge) return std::nullopt;
  BRepAdaptor_Curve curve(*edge);
  const double parameter = (curve.FirstParameter() + curve.LastParameter()) * 0.5;
  const gp_Pnt point = curve.Value(parameter);
  // A stable radial-looking direction is sufficient for a distance handle.
  Vector3d direction{point.X(), point.Y(), 0.0};
  double length = std::hypot(direction.x, direction.y);
  if (length < 1e-8) {
    direction = {0.0, 0.0, 1.0};
  } else {
    direction.x /= length;
    direction.y /= length;
  }
  return LinearToolManipulator{{point.X(), point.Y(), point.Z()}, direction,
                               radiusMm_};
}

void FilletToolSession::cancel() noexcept {
  previewShape_.reset();
  edges_.clear();
  error_.clear();
  lifecycle_ = ToolLifecycle::Inactive;
}

}  // namespace solidar
