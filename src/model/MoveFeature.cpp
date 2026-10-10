#include "model/MoveFeature.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <utility>

#include "model/Body.h"
#include "model/GeometryOperation.h"

namespace solidar {
namespace {

bool validSource(const ShapeFeature* self, const RebuildContext& context,
                 FeatureId source) {
  if (!context.body) return false;
  const auto& features = context.body->features();
  for (std::size_t index = 1; index < features.size(); ++index)
    if (features[index].get() == self)
      return features[index - 1]->id() == source;
  return false;
}

bool finite(Vector3d value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

}  // namespace

ShapeFeature::ShapePtr buildMovedShape(const TopoDS_Shape& source,
                                       Vector3d offsetMm,
                                       std::string* error) {
  if (source.IsNull()) {
    if (error) *error = "Move base shape is missing";
    return {};
  }
  if (!finite(offsetMm)) {
    if (error) *error = "Move offset must be finite";
    return {};
  }

  ShapeFeature::ShapePtr result;
  GeometryFailure failure;
  const bool completed = runGeometryOperation(
      [&]() -> bool {
        gp_Trsf transform;
        transform.SetTranslation(gp_Vec(offsetMm.x, offsetMm.y, offsetMm.z));
        BRepBuilderAPI_Transform moved(source, transform, true);
        if (!moved.IsDone() || moved.Shape().IsNull()) {
          if (error) *error = "Move transformation failed";
          return false;
        }
        const TopoDS_Shape candidate = moved.Shape();
        if (!BRepCheck_Analyzer(candidate).IsValid()) {
          if (error) *error = "Move produced an invalid B-Rep shape";
          return false;
        }
        result = std::make_shared<TopoDS_Shape>(candidate);
        return true;
      },
      &failure);
  if (!completed) {
    if (failure.kind != GeometryFailureKind::None && error)
      *error = failure.kind == GeometryFailureKind::OcctException
                   ? "Move OpenCASCADE operation failed"
                   : "Move geometry operation failed";
    return {};
  }
  if (error) error->clear();
  return result;
}

MoveFeature::MoveFeature(FeatureId sourceFeatureId, Vector3d offsetMm,
                         std::string name)
    : ShapeFeature(name.empty() ? "Move" : std::move(name)),
      sourceFeatureId_(sourceFeatureId),
      offsetMm_(offsetMm) {}

MoveFeature::MoveFeature(FeatureId id, FeatureId sourceFeatureId,
                         Vector3d offsetMm, std::string name)
    : ShapeFeature(id, std::move(name)),
      sourceFeatureId_(sourceFeatureId),
      offsetMm_(offsetMm) {}

FeatureId MoveFeature::sourceFeatureId() const noexcept {
  return sourceFeatureId_;
}

Vector3d MoveFeature::offsetMm() const noexcept { return offsetMm_; }

void MoveFeature::setOffsetMm(Vector3d offsetMm) noexcept {
  if (offsetMm_.x == offsetMm.x && offsetMm_.y == offsetMm.y &&
      offsetMm_.z == offsetMm.z)
    return;
  offsetMm_ = offsetMm;
  setDirty();
}


bool MoveFeature::rebuildImpl(const RebuildContext& context) {
  clearShape();
  if (!context.previousShape || context.previousShape->IsNull()) {
    markError("Move base shape is missing");
    return false;
  }
  if (!validSource(this, context, sourceFeatureId_)) {
    markError("Move source Feature could not be resolved");
    return false;
  }
  std::string error;
  const auto result = buildMovedShape(*context.previousShape, offsetMm_, &error);
  if (!result) {
    markError(error);
    return false;
  }
  setShape(result);
  markValid();
  return true;
}

std::unique_ptr<Feature> MoveFeature::clone() const {
  return std::make_unique<MoveFeature>(*this);
}

}  // namespace solidar
