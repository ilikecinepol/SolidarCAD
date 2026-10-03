#include "model/LinearPatternFeature.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Trsf.hxx>

#include <cmath>
#include "model/Body.h"

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
gp_Vec directionVector(PrincipalAxis axis, double distance) {
  if (axis == PrincipalAxis::X) return {distance, 0, 0};
  if (axis == PrincipalAxis::Y) return {0, distance, 0};
  return {0, 0, distance};
}
}  // namespace

ShapeFeature::ShapePtr buildLinearPatternShape(const TopoDS_Shape& source,
                                               PrincipalAxis direction,
                                               int count, double spacingMm,
                                               std::string* error) {
  if (count < 2) {
    if (error) *error = "Linear Pattern count must be at least 2";
    return {};
  }
  if (!std::isfinite(spacingMm) || spacingMm <= 0.0) {
    if (error)
      *error = "Linear Pattern spacing must be finite and positive";
    return {};
  }
  if (source.IsNull()) {
    if (error) *error = "Linear Pattern base shape is missing";
    return {};
  }
  BRep_Builder builder;
  TopoDS_Compound compound;
  builder.MakeCompound(compound);
  builder.Add(compound, source);
  for (int index = 1; index < count; ++index) {
    gp_Trsf transform;
    transform.SetTranslation(
        directionVector(direction, spacingMm * static_cast<double>(index)));
    BRepBuilderAPI_Transform copy(source, transform, true);
    if (!copy.IsDone() || copy.Shape().IsNull()) {
      if (error) *error = "Linear Pattern transformation failed";
      return {};
    }
    builder.Add(compound, copy.Shape());
  }
  if (error) error->clear();
  return std::make_shared<TopoDS_Shape>(compound);
}

LinearPatternFeature::LinearPatternFeature(FeatureId source, PrincipalAxis axis,
                                           int count, double spacing,
                                           std::string name)
    : ShapeFeature(name.empty() ? "Linear Pattern" : std::move(name)),
      sourceFeatureId_(source), direction_(axis), count_(count), spacingMm_(spacing) {}
LinearPatternFeature::LinearPatternFeature(FeatureId id, FeatureId source,
                                           PrincipalAxis axis, int count,
                                           double spacing, std::string name)
    : ShapeFeature(id, std::move(name)), sourceFeatureId_(source), direction_(axis),
      count_(count), spacingMm_(spacing) {}
FeatureId LinearPatternFeature::sourceFeatureId() const noexcept { return sourceFeatureId_; }
PrincipalAxis LinearPatternFeature::direction() const noexcept { return direction_; }
int LinearPatternFeature::count() const noexcept { return count_; }
double LinearPatternFeature::spacingMm() const noexcept { return spacingMm_; }
void LinearPatternFeature::setDirection(PrincipalAxis value) noexcept { if (direction_ != value) { direction_ = value; setDirty(); } }
void LinearPatternFeature::setCount(int value) noexcept { if (count_ != value) { count_ = value; setDirty(); } }
void LinearPatternFeature::setSpacingMm(double value) noexcept { if (spacingMm_ != value) { spacingMm_ = value; setDirty(); } }
std::string LinearPatternFeature::typeName() const { return "LinearPattern"; }
bool LinearPatternFeature::rebuild(const RebuildContext& context) {
  clearShape();
  if (!context.previousShape || context.previousShape->IsNull()) {
    markError("Linear Pattern base shape is missing");
    return false;
  }
  if (!validSource(this, context, sourceFeatureId_)) { markError("Linear Pattern source Feature could not be resolved"); return false; }
  std::string error;
  const auto result = buildLinearPatternShape(
      *context.previousShape, direction_, count_, spacingMm_, &error);
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
