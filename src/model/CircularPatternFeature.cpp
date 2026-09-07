#include "model/CircularPatternFeature.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax1.hxx>
#include <gp_Trsf.hxx>

#include <cmath>
#include <numbers>
#include "model/Body.h"

namespace solidar {
namespace {
bool validSource(const ShapeFeature* self, const RebuildContext& context, FeatureId source) {
  if (!context.body) return false;
  const auto& features = context.body->features();
  for (std::size_t i = 1; i < features.size(); ++i)
    if (features[i].get() == self) return features[i - 1]->id() == source;
  return false;
}
gp_Dir axisDirection(PrincipalAxis axis) {
  if (axis == PrincipalAxis::X) return {1, 0, 0};
  if (axis == PrincipalAxis::Y) return {0, 1, 0};
  return {0, 0, 1};
}
}  // namespace
CircularPatternFeature::CircularPatternFeature(FeatureId source, PrincipalAxis axis,
                                               int count, double angle, std::string name)
    : ShapeFeature(name.empty() ? "Circular Pattern" : std::move(name)), sourceFeatureId_(source), axis_(axis), count_(count), angleDeg_(angle) {}
CircularPatternFeature::CircularPatternFeature(FeatureId id, FeatureId source,
                                               PrincipalAxis axis, int count,
                                               double angle, std::string name)
    : ShapeFeature(id, std::move(name)), sourceFeatureId_(source), axis_(axis), count_(count), angleDeg_(angle) {}
FeatureId CircularPatternFeature::sourceFeatureId() const noexcept { return sourceFeatureId_; }
PrincipalAxis CircularPatternFeature::axis() const noexcept { return axis_; }
int CircularPatternFeature::count() const noexcept { return count_; }
double CircularPatternFeature::angleDeg() const noexcept { return angleDeg_; }
void CircularPatternFeature::setAxis(PrincipalAxis value) noexcept { if (axis_ != value) { axis_ = value; setDirty(); } }
void CircularPatternFeature::setCount(int value) noexcept { if (count_ != value) { count_ = value; setDirty(); } }
void CircularPatternFeature::setAngleDeg(double value) noexcept { if (angleDeg_ != value) { angleDeg_ = value; setDirty(); } }
std::string CircularPatternFeature::typeName() const { return "CircularPattern"; }
bool CircularPatternFeature::rebuild(const RebuildContext& context) {
  clearShape();
  if (count_ < 2) { markError("Circular Pattern count must be at least 2"); return false; }
  if (!std::isfinite(angleDeg_) || angleDeg_ <= 0 || angleDeg_ > 360) { markError("Circular Pattern angle must be in (0, 360]"); return false; }
  if (!context.previousShape || context.previousShape->IsNull()) { markError("Circular Pattern base shape is missing"); return false; }
  if (!validSource(this, context, sourceFeatureId_)) { markError("Circular Pattern source Feature could not be resolved"); return false; }
  const bool full = std::abs(angleDeg_ - 360.0) < 1e-9;
  const double step = angleDeg_ / static_cast<double>(full ? count_ : count_ - 1);
  BRep_Builder builder; TopoDS_Compound compound; builder.MakeCompound(compound); builder.Add(compound, *context.previousShape);
  for (int i = 1; i < count_; ++i) {
    gp_Trsf transform; transform.SetRotation(gp_Ax1(gp_Pnt(0,0,0), axisDirection(axis_)), step * i * std::numbers::pi / 180.0);
    BRepBuilderAPI_Transform copy(*context.previousShape, transform, true);
    if (!copy.IsDone() || copy.Shape().IsNull()) { markError("Circular Pattern transformation failed"); return false; }
    builder.Add(compound, copy.Shape());
  }
  setShape(std::make_shared<TopoDS_Shape>(compound)); markValid(); return true;
}
std::unique_ptr<Feature> CircularPatternFeature::clone() const { return std::make_unique<CircularPatternFeature>(*this); }
}  // namespace solidar
