#include "model/ExtrudeFeature.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "model/SketchProfileBuilder.h"

namespace solidar {

ExtrudeFeature::ExtrudeFeature(SketchId profileSketchId, double lengthMm,
                               std::string name)
    : ExtrudeFeature(profileSketchId, lengthMm, std::move(name),
                     ExtrudeOperation::NewBody, false) {}

ExtrudeFeature::ExtrudeFeature(SketchId profileSketchId, double lengthMm,
                               std::string name, ExtrudeOperation operation,
                               bool reversed)
    : ShapeFeature(name.empty() ? "Extrude" : std::move(name)),
      profileSketchId_(profileSketchId),
      lengthMm_(lengthMm), operation_(operation), reversed_(reversed) {}

ExtrudeFeature::ExtrudeFeature(FeatureId id, SketchId profileSketchId,
                               double lengthMm, std::string name)
    : ExtrudeFeature(id, profileSketchId, lengthMm, std::move(name),
                     ExtrudeOperation::NewBody, false) {}

ExtrudeFeature::ExtrudeFeature(FeatureId id, SketchId profileSketchId,
                               double lengthMm, std::string name,
                               ExtrudeOperation operation, bool reversed)
    : ShapeFeature(id, std::move(name)),
      profileSketchId_(profileSketchId),
      lengthMm_(lengthMm), operation_(operation), reversed_(reversed) {}

SketchId ExtrudeFeature::profileSketchId() const noexcept {
  return profileSketchId_;
}

void ExtrudeFeature::setProfileSketchId(SketchId id) noexcept {
  if (profileSketchId_ == id) return;
  profileSketchId_ = id;
  setDirty();
}

double ExtrudeFeature::lengthMm() const noexcept { return lengthMm_; }

void ExtrudeFeature::setLengthMm(double value) noexcept {
  if (lengthMm_ == value) return;
  lengthMm_ = value;
  setDirty();
}

ExtrudeOperation ExtrudeFeature::operation() const noexcept { return operation_; }
void ExtrudeFeature::setOperation(ExtrudeOperation value) noexcept {
  if (operation_ == value) return;
  operation_ = value;
  setDirty();
}
bool ExtrudeFeature::reversed() const noexcept { return reversed_; }
void ExtrudeFeature::setReversed(bool value) noexcept {
  if (reversed_ == value) return;
  reversed_ = value;
  setDirty();
}

std::string ExtrudeFeature::typeName() const { return "Extrude"; }

bool ExtrudeFeature::dependsOnSketch(SketchId sketchId) const noexcept {
  return profileSketchId_ == sketchId;
}

bool ExtrudeFeature::rebuild(const RebuildContext& context) {
  clearShape();
  if (!std::isfinite(lengthMm_) || lengthMm_ <= 0.0) {
    markError("Extrude length must be a finite positive value");
    return false;
  }

  const auto* profile = context.document.findSketch(profileSketchId_);
  if (!profile) {
    markError("Extrude profile sketch was not found");
    return false;
  }
  try {
    std::string profileError;
    TopoDS_Shape prism;
    if (!buildExtrusionPrismFromSketch(*profile, lengthMm_, reversed_, &prism,
                                       &profileError)) {
      markError("Extrude " + profileError);
      return false;
    }
    if (operation_ == ExtrudeOperation::NewBody) {
      if (context.previousShape && !context.previousShape->IsNull()) {
        markError("Extrude New Body must be the first feature of a Body");
        return false;
      }
      setShape(std::make_shared<TopoDS_Shape>(prism));
      markValid();
      return true;
    }
    if (!context.previousShape || context.previousShape->IsNull()) {
      markError(operation_ == ExtrudeOperation::Join
                    ? "Extrude Join base shape is missing"
                    : "Extrude Cut base shape is missing");
      return false;
    }
    GProp_GProps beforeProperties;
    BRepGProp::VolumeProperties(*context.previousShape, beforeProperties);
    TopoDS_Shape result;
    if (operation_ == ExtrudeOperation::Join) {
      BRepAlgoAPI_Fuse fuse(*context.previousShape, prism);
      fuse.Build();
      if (!fuse.IsDone() || fuse.Shape().IsNull()) {
        markError("Extrude Join boolean fuse failed");
        return false;
      }
      result = fuse.Shape();
    } else {
      BRepAlgoAPI_Cut cut(*context.previousShape, prism);
      cut.Build();
      if (!cut.IsDone() || cut.Shape().IsNull()) {
        markError("Extrude Cut boolean cut failed");
        return false;
      }
      result = cut.Shape();
    }
    TopExp_Explorer solids(result, TopAbs_SOLID);
    if (!solids.More()) {
      markError("Extrude result does not contain a solid");
      return false;
    }
    TopoDS_Shape singleSolid = solids.Current();
    solids.Next();
    if (solids.More()) {
      markError(operation_ == ExtrudeOperation::Join
                    ? "Extrude Join does not intersect the body"
                    : "Extrude result contains multiple solids");
      return false;
    }
    GProp_GProps afterProperties;
    BRepGProp::VolumeProperties(singleSolid, afterProperties);
    const double before = beforeProperties.Mass();
    const double after = afterProperties.Mass();
    const double tolerance = std::max(1e-7, std::abs(before) * 1e-10);
    if (operation_ == ExtrudeOperation::Join && after <= before + tolerance) {
      markError("Extrude Join does not intersect the body");
      return false;
    }
    if (operation_ == ExtrudeOperation::Cut && after >= before - tolerance) {
      markError("Extrude Cut does not intersect the body");
      return false;
    }
    setShape(std::make_shared<TopoDS_Shape>(singleSolid));
    markValid();
    return true;
  } catch (const Standard_Failure& failure) {
    const char* message = failure.what();
    markError(message && *message ? std::string("OCCT Extrude error: ") + message
                                  : "OCCT Extrude operation failed");
    return false;
  } catch (...) {
    markError("Unexpected Extrude geometry error");
    return false;
  }
}

std::unique_ptr<Feature> ExtrudeFeature::clone() const {
  return std::make_unique<ExtrudeFeature>(*this);
}

}  // namespace solidar
