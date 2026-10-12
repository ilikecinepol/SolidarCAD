#include "model/MirrorFeature.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRep_Builder.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax2.hxx>
#include <gp_Trsf.hxx>

#include "model/Body.h"
#include "model/GeometryOperation.h"
#include "model/TopologyReferenceResolver.h"

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

ShapeFeature::ShapePtr buildMirrorShape(
    const TopoDS_Shape& source, const gp_Ax2& mirrorAxis,
    std::string* error) {
  ShapeFeature::ShapePtr result;
  GeometryFailure failure;
  const bool completed = runGeometryOperation(
      [&]() -> bool {
        gp_Trsf transform;
        transform.SetMirror(mirrorAxis);
        BRepBuilderAPI_Transform reflected(source, transform, true);
        if (!reflected.IsDone() || reflected.Shape().IsNull()) {
          if (error) *error = "Mirror transformation failed";
          return false;
        }
        BRep_Builder builder;
        TopoDS_Compound compound;
        builder.MakeCompound(compound);
        builder.Add(compound, source);
        builder.Add(compound, reflected.Shape());
        if (!BRepCheck_Analyzer(compound).IsValid()) {
          if (error) *error = "Mirror produced an invalid B-Rep shape";
          return false;
        }
        result = std::make_shared<TopoDS_Shape>(compound);
        return true;
      },
      &failure);
  if (!completed) {
    if (failure.kind != GeometryFailureKind::None && error)
      *error = failure.kind == GeometryFailureKind::OcctException
                   ? "Mirror OpenCASCADE operation failed"
                   : "Mirror geometry operation failed";
    return {};
  }
  if (error) error->clear();
  return result;
}
}  // namespace

ShapeFeature::ShapePtr buildMirrorShape(const TopoDS_Shape& source,
                                        MirrorPlane plane,
                                        std::string* error) {
  if (source.IsNull()) {
    if (error) *error = "Mirror base shape is missing";
    return {};
  }
  if (!validMirrorPlane(plane)) {
    if (error) *error = "Mirror plane is invalid";
    return {};
  }
  return buildMirrorShape(source,
                          gp_Ax2(gp_Pnt(0, 0, 0), normal(plane)), error);
}

ShapeFeature::ShapePtr buildMirrorShape(const TopoDS_Shape& source,
                                        const FaceReference& planeFace,
                                        std::string* error) {
  if (source.IsNull()) {
    if (error) *error = "Mirror base shape is missing";
    return {};
  }
  const auto resolved = resolveFaceReference(source, planeFace.topology());
  if (!resolved) {
    if (error) *error = "Mirror plane face could not be resolved";
    return {};
  }
  BRepAdaptor_Surface surface(*resolved.subshape);
  if (surface.GetType() != GeomAbs_Plane) {
    if (error) *error = "Mirror reference face must be planar";
    return {};
  }
  const gp_Pln plane = surface.Plane();
  return buildMirrorShape(
      source, gp_Ax2(plane.Location(), plane.Axis().Direction()), error);
}

MirrorFeature::MirrorFeature(FeatureId source, MirrorPlane plane,
                             std::string name)
    : ShapeFeature(name.empty() ? "Mirror" : std::move(name)),
      sourceFeatureId_(source), plane_(plane) {}
MirrorFeature::MirrorFeature(FeatureId id, FeatureId source, MirrorPlane plane,
                             std::string name)
    : ShapeFeature(id, std::move(name)), sourceFeatureId_(source), plane_(plane) {}
MirrorFeature::MirrorFeature(FeatureId source, FaceReference planeFace,
                             std::string name)
    : ShapeFeature(name.empty() ? "Mirror" : std::move(name)),
      sourceFeatureId_(source), planeFace_(std::move(planeFace)) {}
MirrorFeature::MirrorFeature(FeatureId id, FeatureId source,
                             FaceReference planeFace, std::string name)
    : ShapeFeature(id, std::move(name)), sourceFeatureId_(source),
      planeFace_(std::move(planeFace)) {}
FeatureId MirrorFeature::sourceFeatureId() const noexcept { return sourceFeatureId_; }
MirrorPlane MirrorFeature::plane() const noexcept { return plane_; }
const std::optional<FaceReference>& MirrorFeature::planeFace() const noexcept {
  return planeFace_;
}
void MirrorFeature::setPlane(MirrorPlane value) noexcept {
  if (plane_ == value && !planeFace_) return;
  plane_ = value;
  planeFace_.reset();
  setDirty();
}
void MirrorFeature::setPlaneFace(FaceReference value) {
  if (planeFace_ == value) return;
  planeFace_ = std::move(value);
  setDirty();
}
bool MirrorFeature::rebuildImpl(const RebuildContext& context) {
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
  ShapePtr result;
  if (planeFace_) {
    if (!context.body || planeFace_->bodyId != context.body->id() ||
        planeFace_->featureId != sourceFeatureId_) {
      markError("Mirror plane face must belong to the source Feature");
      return false;
    }
    result = buildMirrorShape(*context.previousShape, *planeFace_, &error);
  } else {
    result = buildMirrorShape(*context.previousShape, plane_, &error);
  }
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
