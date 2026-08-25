#include "model/ExtrudeFeature.h"

#include <BRepPrimAPI_MakePrism.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <memory>
#include <utility>

#include "model/SketchProfileBuilder.h"

namespace solidar {

ExtrudeFeature::ExtrudeFeature(SketchId profileSketchId, double lengthMm,
                               std::string name)
    : ShapeFeature(name.empty() ? "Extrude" : std::move(name)),
      profileSketchId_(profileSketchId),
      lengthMm_(lengthMm) {}

ExtrudeFeature::ExtrudeFeature(FeatureId id, SketchId profileSketchId,
                               double lengthMm, std::string name)
    : ShapeFeature(id, std::move(name)),
      profileSketchId_(profileSketchId),
      lengthMm_(lengthMm) {}

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

std::string ExtrudeFeature::typeName() const { return "Extrude"; }

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
    TopoDS_Face profileFace;
    std::string profileError;
    if (!buildPlanarFaceFromSketch(*profile, &profileFace, &profileError)) {
      markError("Extrude " + profileError);
      return false;
    }

    const auto normal = profile->placement.normal();
    BRepPrimAPI_MakePrism prismBuilder(
        profileFace,
        gp_Vec(normal.x * lengthMm_, normal.y * lengthMm_,
               normal.z * lengthMm_));
    prismBuilder.Build();
    if (!prismBuilder.IsDone() || prismBuilder.Shape().IsNull()) {
      markError("Extrude could not build a solid prism");
      return false;
    }

    setShape(std::make_shared<TopoDS_Shape>(prismBuilder.Shape()));
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
