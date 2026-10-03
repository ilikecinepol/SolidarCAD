#include "model/CircularPatternFeature.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax1.hxx>
#include <gp_Trsf.hxx>

#include <cmath>
#include <numbers>
#include "model/Body.h"
#include "model/Document.h"

namespace solidar {
namespace {
bool validSource(const ShapeFeature* self, const RebuildContext& context, FeatureId source) {
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
  if (count < 2) {
    if (error) *error = "Circular Pattern count must be at least 2";
    return {};
  }
  if (!std::isfinite(angleDeg) || angleDeg <= 0.0 || angleDeg > 360.0) {
    if (error) *error = "Circular Pattern angle must be in (0, 360]";
    return {};
  }
  if (source.IsNull()) {
    if (error) *error = "Circular Pattern base shape is missing";
    return {};
  }
  const bool full = std::abs(angleDeg - 360.0) < 1e-9;
  const double step = angleDeg / static_cast<double>(full ? count : count - 1);
  BRep_Builder builder;
  TopoDS_Compound compound;
  builder.MakeCompound(compound);
  if (includeSource) builder.Add(compound, source);
  for (int index = 1; index < count; ++index) {
    gp_Trsf transform;
    transform.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), axisDirection(axis)),
                          step * static_cast<double>(index) *
                              std::numbers::pi / 180.0);
    BRepBuilderAPI_Transform copy(source, transform, true);
    if (!copy.IsDone() || copy.Shape().IsNull()) {
      if (error) *error = "Circular Pattern transformation failed";
      return {};
    }
    builder.Add(compound, copy.Shape());
  }
  if (error) error->clear();
  return std::make_shared<TopoDS_Shape>(compound);
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
bool CircularPatternFeature::dependsOnFeature(FeatureId featureId) const noexcept {
  return operation_ == PatternOperation::NewBody &&
         sourceFeatureId_ == featureId;
}
std::string CircularPatternFeature::typeName() const { return "CircularPattern"; }
bool CircularPatternFeature::rebuild(const RebuildContext& context) {
  clearShape();
  const TopoDS_Shape* source = resolvedSource(
      this, context, sourceBodyId_, sourceFeatureId_, operation_);
  if (!source || source->IsNull()) {
    markError("Circular Pattern source Feature could not be resolved");
    return false;
  }
  std::string error;
  const auto result = buildCircularPatternShape(
      *source, axis_, count_, angleDeg_, &error,
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
