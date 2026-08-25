#include "model/PocketFeature.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>
#include <TopExp_Explorer.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <memory>
#include <utility>

#include "model/SketchProfileBuilder.h"

namespace solidar {

PocketFeature::PocketFeature(SketchId profileSketchId, double depthMm,
                             std::string name)
    : ShapeFeature(name.empty() ? "Pocket" : std::move(name)),
      profileSketchId_(profileSketchId), depthMm_(depthMm) {}

PocketFeature::PocketFeature(FeatureId id, SketchId profileSketchId,
                             double depthMm, std::string name)
    : ShapeFeature(id, std::move(name)), profileSketchId_(profileSketchId),
      depthMm_(depthMm) {}

SketchId PocketFeature::profileSketchId() const noexcept {
  return profileSketchId_;
}
double PocketFeature::depthMm() const noexcept { return depthMm_; }
void PocketFeature::setProfileSketchId(SketchId id) noexcept {
  if (profileSketchId_ == id) return;
  profileSketchId_ = id;
  setDirty();
}
void PocketFeature::setDepthMm(double value) noexcept {
  if (depthMm_ == value) return;
  depthMm_ = value;
  setDirty();
}
std::string PocketFeature::typeName() const { return "Pocket"; }

bool PocketFeature::rebuild(const RebuildContext& context) {
  clearShape();
  if (!std::isfinite(depthMm_) || depthMm_ <= 0.0) {
    markError("Pocket depth must be a finite positive value");
    return false;
  }
  if (!context.previousShape || context.previousShape->IsNull()) {
    markError("Pocket base shape is missing");
    return false;
  }
  const auto* profile = context.document.findSketch(profileSketchId_);
  if (!profile) {
    markError("Pocket profile sketch was not found");
    return false;
  }
  TopoDS_Face profileFace;
  std::string profileError;
  if (!buildPlanarFaceFromSketch(*profile, &profileFace, &profileError)) {
    markError("Pocket " + profileError);
    return false;
  }

  try {
    const auto normal = profile->placement.normal();
    BRepPrimAPI_MakePrism prism(
        profileFace, gp_Vec(-normal.x * depthMm_, -normal.y * depthMm_,
                            -normal.z * depthMm_));
    prism.Build();
    if (!prism.IsDone() || prism.Shape().IsNull()) {
      markError("Pocket could not build the cutting prism");
      return false;
    }
    BRepAlgoAPI_Cut cut(*context.previousShape, prism.Shape());
    cut.Build();
    if (!cut.IsDone() || cut.Shape().IsNull()) {
      markError("Pocket boolean cut failed");
      return false;
    }
    GProp_GProps beforeProperties;
    GProp_GProps afterProperties;
    BRepGProp::VolumeProperties(*context.previousShape, beforeProperties);
    BRepGProp::VolumeProperties(cut.Shape(), afterProperties);
    if (afterProperties.Mass() >= beforeProperties.Mass() - 1e-7) {
      markError("Pocket profile does not intersect the body");
      return false;
    }
    TopoDS_Shape result = cut.Shape();
    if (result.ShapeType() != TopAbs_SOLID) {
      TopExp_Explorer solids(result, TopAbs_SOLID);
      if (!solids.More()) {
        markError("Pocket result does not contain a solid");
        return false;
      }
      result = solids.Current();
      solids.Next();
      if (solids.More()) {
        markError("Pocket result contains multiple solids");
        return false;
      }
    }
    setShape(std::make_shared<TopoDS_Shape>(result));
    markValid();
    return true;
  } catch (const Standard_Failure& failure) {
    const char* message = failure.what();
    markError(message && *message ? std::string("OCCT Pocket error: ") + message
                                  : "OCCT Pocket operation failed");
    return false;
  } catch (...) {
    markError("Unexpected Pocket geometry error");
    return false;
  }
}

std::unique_ptr<Feature> PocketFeature::clone() const {
  return std::make_unique<PocketFeature>(*this);
}

}  // namespace solidar
