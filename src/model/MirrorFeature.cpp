#include "model/MirrorFeature.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax2.hxx>
#include <gp_Trsf.hxx>

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
gp_Dir normal(MirrorPlane plane) {
  if (plane == MirrorPlane::XY) return {0.0, 0.0, 1.0};
  if (plane == MirrorPlane::XZ) return {0.0, 1.0, 0.0};
  return {1.0, 0.0, 0.0};
}
}  // namespace

ShapeFeature::ShapePtr buildMirrorShape(const TopoDS_Shape& source,
                                        MirrorPlane plane,
                                        std::string* error) {
  if (source.IsNull()) {
    if (error) *error = "Mirror base shape is missing";
    return {};
  }
  gp_Trsf transform;
  transform.SetMirror(gp_Ax2(gp_Pnt(0, 0, 0), normal(plane)));
  BRepBuilderAPI_Transform reflected(source, transform, true);
  if (!reflected.IsDone() || reflected.Shape().IsNull()) {
    if (error) *error = "Mirror transformation failed";
    return {};
  }
  BRep_Builder builder;
  TopoDS_Compound compound;
  builder.MakeCompound(compound);
  builder.Add(compound, source);
  builder.Add(compound, reflected.Shape());
  if (error) error->clear();
  return std::make_shared<TopoDS_Shape>(compound);
}

MirrorFeature::MirrorFeature(FeatureId source, MirrorPlane plane,
                             std::string name)
    : ShapeFeature(name.empty() ? "Mirror" : std::move(name)),
      sourceFeatureId_(source), plane_(plane) {}
MirrorFeature::MirrorFeature(FeatureId id, FeatureId source, MirrorPlane plane,
                             std::string name)
    : ShapeFeature(id, std::move(name)), sourceFeatureId_(source), plane_(plane) {}
FeatureId MirrorFeature::sourceFeatureId() const noexcept { return sourceFeatureId_; }
MirrorPlane MirrorFeature::plane() const noexcept { return plane_; }
void MirrorFeature::setPlane(MirrorPlane value) noexcept {
  if (plane_ == value) return;
  plane_ = value;
  setDirty();
}
std::string MirrorFeature::typeName() const { return "Mirror"; }
bool MirrorFeature::rebuild(const RebuildContext& context) {
  clearShape();
  if (!context.previousShape || context.previousShape->IsNull()) {
    markError("Mirror base shape is missing");
    return false;
  }
  if (!validSource(this, context, sourceFeatureId_)) {
    markError("Mirror source Feature could not be resolved");
    return false;
  }
  std::string error;
  const auto result = buildMirrorShape(*context.previousShape, plane_, &error);
  if (!result) {
    markError(error);
    return false;
  }
  setShape(result);
  markValid();
  return true;
}
std::unique_ptr<Feature> MirrorFeature::clone() const {
  return std::make_unique<MirrorFeature>(*this);
}
}  // namespace solidar
