#include "model/CircularPatternFeature.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax1.hxx>
#include <gp_Trsf.hxx>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>
#include "model/Body.h"
#include "model/Document.h"
#include "model/GeometryOperation.h"
#include "model/TransformFeatureHistoryBuilder.h"

namespace solidar {
namespace {
bool validSource(const ShapeFeature* self, const RebuildContext& context, FeatureId source) {
  if (!context.body) return false;
  const auto& features = context.body->features();
  for (std::size_t i = 1; i < features.size(); ++i)
    if (features[i].get() == self) return features[i - 1]->id() == source;
  return false;
}
struct PatternSource {
  const ShapeFeature* feature{};
  ShapeFeature::ShapePtr shape;
};

PatternSource resolvedSource(const ShapeFeature* self,
                             const RebuildContext& context,
                             BodyId sourceBodyId,
                             FeatureId sourceFeatureId,
                             PatternOperation operation) {
  if (operation == PatternOperation::Join) {
    if (!validSource(self, context, sourceFeatureId) ||
        !context.previousFeature || !context.previousShape ||
        !context.previousFeature->shape() ||
        !context.previousFeature->shape()->IsSame(*context.previousShape))
      return {};
    return {context.previousFeature, context.previousFeature->shape()};
  }
  if (context.previousShape || sourceBodyId == kInvalidBodyId ||
      !context.body || context.body->id() == sourceBodyId)
    return {};
  const Body* sourceBody = context.document.findBody(sourceBodyId);
  if (!sourceBody) return {};
  for (const auto& feature : sourceBody->features())
    if (feature->id() == sourceFeatureId && feature->isValid() &&
        feature->shape() && !feature->shape()->IsNull())
      return {feature.get(), feature->shape()};
  return {};
}
gp_Dir axisDirection(PrincipalAxis axis) {
  if (axis == PrincipalAxis::X) return {1, 0, 0};
  if (axis == PrincipalAxis::Y) return {0, 1, 0};
  return {0, 0, 1};
}
}  // namespace

ShapeFeature::ShapePtr buildCircularPatternShape(const TopoDS_Shape& source,
                                                 PrincipalAxis axis,
                                                 int count, double angleDeg,
                                                 std::string* error,
                                                 bool includeSource) {
  if (!validPatternCount(count)) {
    if (error) *error = "Circular Pattern count must be in [2, 100]";
    return {};
  }
  if (!validPatternAngle(angleDeg)) {
    if (error) *error = "Circular Pattern angle must be in [0.01, 360]";
    return {};
  }
  if (!validPrincipalAxis(axis)) {
    if (error) *error = "Circular Pattern axis is invalid";
    return {};
  }
  if (source.IsNull()) {
    if (error) *error = "Circular Pattern base shape is missing";
    return {};
  }
  ShapeFeature::ShapePtr result;
  GeometryFailure failure;
  const bool completed = runGeometryOperation(
      [&]() -> bool {
        const bool full =
            std::abs(angleDeg - kMaximumPatternAngleDeg) < 1e-9;
        const double step =
            angleDeg / static_cast<double>(full ? count : count - 1);
        BRep_Builder builder;
        TopoDS_Compound compound;
        builder.MakeCompound(compound);
        if (includeSource) builder.Add(compound, source);
        for (int index = 1; index < count; ++index) {
          gp_Trsf transform;
          transform.SetRotation(
              gp_Ax1(gp_Pnt(0, 0, 0), axisDirection(axis)),
              step * static_cast<double>(index) * std::numbers::pi / 180.0);
          BRepBuilderAPI_Transform copy(source, transform, true);
          if (!copy.IsDone() || copy.Shape().IsNull()) {
            if (error) *error = "Circular Pattern transformation failed";
            return false;
          }
          builder.Add(compound, copy.Shape());
        }
        if (!BRepCheck_Analyzer(compound).IsValid()) {
          if (error) *error = "Circular Pattern produced an invalid B-Rep shape";
          return false;
        }
        result = std::make_shared<TopoDS_Shape>(compound);
        return true;
      },
      &failure);
  if (!completed) {
    if (failure.kind != GeometryFailureKind::None && error)
      *error = failure.kind == GeometryFailureKind::OcctException
                   ? "Circular Pattern OpenCASCADE operation failed"
                   : "Circular Pattern geometry operation failed";
    return {};
  }
  if (error) error->clear();
  return result;
}

CircularPatternFeature::CircularPatternFeature(FeatureId source, PrincipalAxis axis,
                                               int count, double angle, std::string name)
    : CircularPatternFeature(kInvalidBodyId, source, axis, count, angle,
                             PatternOperation::Join, std::move(name)) {}
CircularPatternFeature::CircularPatternFeature(FeatureId id, FeatureId source,
                                               PrincipalAxis axis, int count,
                                               double angle, std::string name)
    : CircularPatternFeature(id, kInvalidBodyId, source, axis, count, angle,
                             PatternOperation::Join, std::move(name)) {}
CircularPatternFeature::CircularPatternFeature(
    BodyId sourceBodyId, FeatureId source, PrincipalAxis axis, int count,
    double angle, PatternOperation operation, std::string name)
    : ShapeFeature(name.empty() ? "Circular Pattern" : std::move(name)),
      sourceBodyId_(sourceBodyId), sourceFeatureId_(source), axis_(axis),
      count_(count), angleDeg_(angle), operation_(operation) {}
CircularPatternFeature::CircularPatternFeature(
    FeatureId id, BodyId sourceBodyId, FeatureId source, PrincipalAxis axis,
    int count, double angle, PatternOperation operation, std::string name)
    : ShapeFeature(id, std::move(name)), sourceBodyId_(sourceBodyId),
      sourceFeatureId_(source), axis_(axis), count_(count), angleDeg_(angle),
      operation_(operation) {}
BodyId CircularPatternFeature::sourceBodyId() const noexcept {
  return sourceBodyId_;
}
FeatureId CircularPatternFeature::sourceFeatureId() const noexcept { return sourceFeatureId_; }
PrincipalAxis CircularPatternFeature::axis() const noexcept { return axis_; }
int CircularPatternFeature::count() const noexcept { return count_; }
double CircularPatternFeature::angleDeg() const noexcept { return angleDeg_; }
PatternOperation CircularPatternFeature::operation() const noexcept {
  return operation_;
}
void CircularPatternFeature::setAxis(PrincipalAxis value) noexcept { if (axis_ != value) { axis_ = value; setDirty(); } }
void CircularPatternFeature::setCount(int value) noexcept { if (count_ != value) { count_ = value; setDirty(); } }
void CircularPatternFeature::setAngleDeg(double value) noexcept { if (angleDeg_ != value) { angleDeg_ = value; setDirty(); } }
FeatureDependencies CircularPatternFeature::dependencies() const {
  FeatureDependencies result;
  if (operation_ == PatternOperation::NewBody &&
      sourceFeatureId_ != kInvalidFeatureId)
    result.featureIds.push_back(sourceFeatureId_);
  return result;
}
bool CircularPatternFeature::rebuildImpl(const RebuildContext& context) {
  clearShape();
  if (!validPatternOperation(operation_)) {
    markError("Circular Pattern operation is invalid");
    return false;
  }
  const auto source = resolvedSource(
      this, context, sourceBodyId_, sourceFeatureId_, operation_);
  if (!source.feature || !source.shape || source.shape->IsNull()) {
    markError("Circular Pattern source Feature could not be resolved");
    return false;
  }
  std::string error;
  const auto sourceLineage = source.feature->topologyLineage();
  if (sourceLineage) {
    if (sourceLineage->coverage() !=
        TopologyLineageCoverage::CompleteFaces) {
      markError("Circular Pattern source face lineage is incomplete");
      return false;
    }
    if (!validPatternCount(count_)) {
      markError("Circular Pattern count must be in [2, 100]");
      return false;
    }
    if (!validPatternAngle(angleDeg_)) {
      markError("Circular Pattern angle must be in [0.01, 360]");
      return false;
    }
    if (!validPrincipalAxis(axis_)) {
      markError("Circular Pattern axis is invalid");
      return false;
    }
    const bool full =
        std::abs(angleDeg_ - kMaximumPatternAngleDeg) < 1e-9;
    const double step =
        angleDeg_ / static_cast<double>(full ? count_ : count_ - 1);
    std::vector<TransformFeatureInstanceRequest> instances;
    instances.reserve(static_cast<std::size_t>(count_));
    if (operation_ == PatternOperation::Join) {
      instances.push_back(
          {TransformFeatureInstanceRelation::RetainedSource, gp_Trsf{},
           TopologySemanticRole::PatternInstance,
           TopologyOccurrenceKey{id(), 0}});
    }
    for (int index = 1; index < count_; ++index) {
      gp_Trsf transform;
      transform.SetRotation(
          gp_Ax1(gp_Pnt(0, 0, 0), axisDirection(axis_)),
          step * static_cast<double>(index) * std::numbers::pi / 180.0);
      instances.push_back(
          {TransformFeatureInstanceRelation::ModifiedCopy, transform,
           TopologySemanticRole::PatternInstance,
           TopologyOccurrenceKey{id(), static_cast<std::uint64_t>(index)}});
    }
    TransformFeatureHistoryBuild built;
    if (!buildTransformFeatureHistory(
            {source.feature->id(), source.shape, sourceLineage}, id(),
            instances, false, &built, &error)) {
      markError(error);
      return false;
    }
    setShapeAndTopologyLineage(std::move(built.shape),
                               std::move(built.lineage));
    markValid();
    return true;
  }
  const auto result = buildCircularPatternShape(
      *source.shape, axis_, count_, angleDeg_, &error,
      operation_ == PatternOperation::Join);
  if (!result) {
    markError(error);
    return false;
  }
  setShape(result);
  markValid();
  return true;
}
std::unique_ptr<Feature> CircularPatternFeature::clone() const { return std::make_unique<CircularPatternFeature>(*this); }
}  // namespace solidar
