#include "model/PocketFeature.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>
#include <TopExp_Explorer.hxx>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "model/SketchProfileBuilder.h"

namespace solidar {
namespace {

std::size_t solidCount(const TopoDS_Shape& shape) {
  std::size_t count = 0;
  for (TopExp_Explorer solids(shape, TopAbs_SOLID); solids.More(); solids.Next())
    ++count;
  return count;
}

}  // namespace

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

bool PocketFeature::dependsOnSketch(SketchId sketchId) const noexcept {
  return profileSketchId_ == sketchId;
}

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
  std::string profileError;
  TopoDS_Shape prismShape;
  if (!buildExtrusionPrismFromSketch(*profile, depthMm_, true, &prismShape,
                                     &profileError)) {
    markError("Pocket " + profileError);
    return false;
  }

  try {
    BRepAlgoAPI_Cut cut(*context.previousShape, prismShape);
    cut.Build();
    if (!cut.IsDone() || cut.Shape().IsNull()) {
      markError("Pocket boolean cut failed");
      return false;
    }
    GProp_GProps beforeProperties;
    GProp_GProps afterProperties;
    BRepGProp::VolumeProperties(*context.previousShape, beforeProperties);
    BRepGProp::VolumeProperties(cut.Shape(), afterProperties);
    const double before = beforeProperties.Mass();
    const double tolerance = std::max(1e-7, std::abs(before) * 1e-10);
    if (afterProperties.Mass() >= before - tolerance) {
      markError("Pocket profile does not intersect the body");
      return false;
    }
    TopoDS_Shape result = cut.Shape();
    const std::size_t beforeSolidCount = solidCount(*context.previousShape);
    const std::size_t resultSolidCount = solidCount(result);
    if (resultSolidCount == 0) {
      markError("Pocket result does not contain a solid");
      return false;
    }
    if (resultSolidCount > beforeSolidCount) {
      markError("Pocket would split the body");
      return false;
    }
    BRepCheck_Analyzer analyzer(result);
    if (!analyzer.IsValid()) {
      markError("Pocket result is invalid");
      return false;
    }
    // Preserve multi-solid containers.  Keep the historical single-solid
    // normalization so existing callers still receive a TopAbs_SOLID root.
    if (resultSolidCount == 1 && result.ShapeType() != TopAbs_SOLID) {
      TopExp_Explorer solids(result, TopAbs_SOLID);
      result = solids.Current();
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
