#include "model/LinearPatternFeature.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Trsf.hxx>

#include <cmath>
#include "model/Body.h"
#include "model/Document.h"
#include "model/GeometryOperation.h"

namespace solidar {
namespace {
bool validSource(const ShapeFeature* self, const RebuildContext& context,
                 FeatureId source) {
  if (!context.body) return false;
  const auto& features = context.body->features();
  for (std::size_t i = 1; i < features.size(); ++i)
    if (features[i].get() == self) return features[i - 1]->id() == source;
  return false;
}
const TopoDS_Shape* resolvedSource(const ShapeFeature* self,
                                   const RebuildContext& context,
                                   BodyId sourceBodyId,
                                   FeatureId sourceFeatureId,
                                   PatternOperation operation) {
  if (operation == PatternOperation::Join)
    return validSource(self, context, sourceFeatureId)
               ? context.previousShape
               : nullptr;
  if (context.previousShape || sourceBodyId == kInvalidBodyId ||
      !context.body || context.body->id() == sourceBodyId)
    return nullptr;
  const Body* sourceBody = context.document.findBody(sourceBodyId);
  if (!sourceBody) return nullptr;
  for (const auto& feature : sourceBody->features())
    if (feature->id() == sourceFeatureId && feature->isValid() &&
        feature->shape() && !feature->shape()->IsNull())
      return feature->shape().get();
  return nullptr;
}
gp_Vec directionVector(PrincipalAxis axis, double distance) {
  if (axis == PrincipalAxis::X) return {distance, 0, 0};
  if (axis == PrincipalAxis::Y) return {0, distance, 0};
  return {0, 0, distance};
}
}  // namespace

ShapeFeature::ShapePtr buildLinearPatternShape(const TopoDS_Shape& source,
                                               PrincipalAxis direction,
                                               int count, double spacingMm,
                                               std::string* error,
                                               bool includeSource) {
  if (!validPatternCount(count)) {
    if (error) *error = "Linear Pattern count must be in [2, 100]";
    return {};
  }
  if (!validPatternSpacing(spacingMm)) {
    if (error)
      *error = "Linear Pattern spacing must be finite and in [0.01, 100000]";
    return {};
  }
  if (!validPrincipalAxis(direction)) {
    if (error) *error = "Linear Pattern direction is invalid";
    return {};
  }
  if (source.IsNull()) {
    if (error) *error = "Linear Pattern base shape is missing";
    return {};
  }
  ShapeFeature::ShapePtr result;
  GeometryFailure failure;
  const bool completed = runGeometryOperation(
      [&]() -> bool {
        BRep_Builder builder;
        TopoDS_Compound compound;
        builder.MakeCompound(compound);
        if (includeSource) builder.Add(compound, source);
        for (int index = 1; index < count; ++index) {
          gp_Trsf transform;
          transform.SetTranslation(directionVector(
              direction, spacingMm * static_cast<double>(index)));
          BRepBuilderAPI_Transform copy(source, transform, true);
          if (!copy.IsDone() || copy.Shape().IsNull()) {
            if (error) *error = "Linear Pattern transformation failed";
            return false;
          }
          builder.Add(compound, copy.Shape());
        }
        if (!BRepCheck_Analyzer(compound).IsValid()) {
          if (error) *error = "Linear Pattern produced an invalid B-Rep shape";
          return false;
        }
        result = std::make_shared<TopoDS_Shape>(compound);
        return true;
      },
      &failure);
  if (!completed) {
    if (failure.kind != GeometryFailureKind::None && error)
      *error = failure.kind == GeometryFailureKind::OcctException
                   ? "Linear Pattern OpenCASCADE operation failed"
                   : "Linear Pattern geometry operation failed";
    return {};
  }
  if (error) error->clear();
  return result;
}

LinearPatternFeature::LinearPatternFeature(FeatureId source, PrincipalAxis axis,
                                           int count, double spacing,
                                           std::string name)
    : LinearPatternFeature(kInvalidBodyId, source, axis, count, spacing,
                           PatternOperation::Join, std::move(name)) {}
LinearPatternFeature::LinearPatternFeature(FeatureId id, FeatureId source,
                                           PrincipalAxis axis, int count,
                                           double spacing, std::string name)
    : LinearPatternFeature(id, kInvalidBodyId, source, axis, count, spacing,
                           PatternOperation::Join, std::move(name)) {}
LinearPatternFeature::LinearPatternFeature(
    BodyId sourceBodyId, FeatureId source, PrincipalAxis axis, int count,
    double spacing, PatternOperation operation, std::string name)
    : ShapeFeature(name.empty() ? "Linear Pattern" : std::move(name)),
      sourceBodyId_(sourceBodyId), sourceFeatureId_(source), direction_(axis),
      count_(count), spacingMm_(spacing), operation_(operation) {}
LinearPatternFeature::LinearPatternFeature(
    FeatureId id, BodyId sourceBodyId, FeatureId source, PrincipalAxis axis,
    int count, double spacing, PatternOperation operation, std::string name)
    : ShapeFeature(id, std::move(name)), sourceBodyId_(sourceBodyId),
      sourceFeatureId_(source), direction_(axis), count_(count),
      spacingMm_(spacing), operation_(operation) {}
BodyId LinearPatternFeature::sourceBodyId() const noexcept {
  return sourceBodyId_;
}
FeatureId LinearPatternFeature::sourceFeatureId() const noexcept { return sourceFeatureId_; }
PrincipalAxis LinearPatternFeature::direction() const noexcept { return direction_; }
int LinearPatternFeature::count() const noexcept { return count_; }
double LinearPatternFeature::spacingMm() const noexcept { return spacingMm_; }
PatternOperation LinearPatternFeature::operation() const noexcept {
  return operation_;
}
void LinearPatternFeature::setDirection(PrincipalAxis value) noexcept { if (direction_ != value) { direction_ = value; setDirty(); } }
void LinearPatternFeature::setCount(int value) noexcept { if (count_ != value) { count_ = value; setDirty(); } }
void LinearPatternFeature::setSpacingMm(double value) noexcept { if (spacingMm_ != value) { spacingMm_ = value; setDirty(); } }
FeatureDependencies LinearPatternFeature::dependencies() const {
  FeatureDependencies result;
  if (operation_ == PatternOperation::NewBody &&
      sourceFeatureId_ != kInvalidFeatureId)
    result.featureIds.push_back(sourceFeatureId_);
  return result;
}
bool LinearPatternFeature::rebuildImpl(const RebuildContext& context) {
  clearShape();
  if (!validPatternOperation(operation_)) {
    markError("Linear Pattern operation is invalid");
    return false;
  }
  const TopoDS_Shape* source = resolvedSource(
      this, context, sourceBodyId_, sourceFeatureId_, operation_);
  if (!source || source->IsNull()) {
    markError("Linear Pattern source Feature could not be resolved");
    return false;
  }
  std::string error;
  const auto result = buildLinearPatternShape(
      *source, direction_, count_, spacingMm_, &error,
      operation_ == PatternOperation::Join);
  if (!result) {
    markError(error);
    return false;
  }
  setShape(result);
  markValid();
  return true;
}
std::unique_ptr<Feature> LinearPatternFeature::clone() const { return std::make_unique<LinearPatternFeature>(*this); }
}  // namespace solidar
