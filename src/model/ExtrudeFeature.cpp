#include "model/ExtrudeFeature.h"

#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <variant>

#include "model/FaceExtrudeBuilder.h"
#include "model/SketchExtrudeBuilder.h"

namespace solidar {

ExtrudeFeature::ExtrudeFeature(SketchId profileSketchId, double lengthMm,
                               std::string name)
    : ExtrudeFeature(profileSketchId, lengthMm, std::move(name),
                     ExtrudeOperation::NewBody, false) {}

ExtrudeFeature::ExtrudeFeature(SketchId profileSketchId, double lengthMm,
                               std::string name, ExtrudeOperation operation,
                               bool reversed)
    : ShapeFeature(name.empty() ? "Extrude" : std::move(name)),
      source_(SketchExtrudeSource{profileSketchId}),
      lengthMm_(lengthMm), operation_(operation), reversed_(reversed) {}

ExtrudeFeature::ExtrudeFeature(FeatureId id, SketchId profileSketchId,
                               double lengthMm, std::string name)
    : ExtrudeFeature(id, profileSketchId, lengthMm, std::move(name),
                     ExtrudeOperation::NewBody, false) {}

ExtrudeFeature::ExtrudeFeature(FeatureId id, SketchId profileSketchId,
                               double lengthMm, std::string name,
                               ExtrudeOperation operation, bool reversed)
    : ShapeFeature(id, std::move(name)),
      source_(SketchExtrudeSource{profileSketchId}),
      lengthMm_(lengthMm), operation_(operation), reversed_(reversed) {}

ExtrudeFeature::ExtrudeFeature(FaceReference face, double lengthMm,
                               std::string name, ExtrudeOperation operation,
                               bool reversed)
    : ShapeFeature(name.empty() ? "Extrude" : std::move(name)),
      source_(FaceExtrudeSource{std::move(face)}),
      lengthMm_(lengthMm), operation_(operation), reversed_(reversed) {}

ExtrudeFeature::ExtrudeFeature(FeatureId id, FaceReference face,
                               double lengthMm, std::string name,
                               ExtrudeOperation operation, bool reversed)
    : ShapeFeature(id, std::move(name)),
      source_(FaceExtrudeSource{std::move(face)}),
      lengthMm_(lengthMm), operation_(operation), reversed_(reversed) {}

const ExtrudeSource& ExtrudeFeature::source() const noexcept { return source_; }

bool ExtrudeFeature::isFaceSource() const noexcept {
  return std::holds_alternative<FaceExtrudeSource>(source_);
}

SketchId ExtrudeFeature::profileSketchId() const noexcept {
  if (const auto* sketch = std::get_if<SketchExtrudeSource>(&source_))
    return sketch->sketchId;
  return kInvalidSketchId;
}

void ExtrudeFeature::setProfileSketchId(SketchId id) noexcept {
  if (!isFaceSource() && profileSketchId() == id) return;
  source_ = SketchExtrudeSource{id};
  setDirty();
}

std::optional<FaceReference> ExtrudeFeature::faceReference() const noexcept {
  if (const auto* face = std::get_if<FaceExtrudeSource>(&source_))
    return face->face;
  return std::nullopt;
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
  if (const auto* sketch = std::get_if<SketchExtrudeSource>(&source_))
    return sketch->sketchId == sketchId;
  return false;
}

bool ExtrudeFeature::rebuild(const RebuildContext& context) {
  clearShape();
  if (!std::isfinite(lengthMm_) || lengthMm_ <= 0.0) {
    markError("Extrude length must be a finite positive value");
    return false;
  }

  if (const auto* faceSource = std::get_if<FaceExtrudeSource>(&source_)) {
    return rebuildFaceSource(context, *faceSource);
  }

  return rebuildSketchSource(context);
}

bool ExtrudeFeature::rebuildFaceSource(const RebuildContext& context,
                                       const FaceExtrudeSource& source) {
  if (operation_ == ExtrudeOperation::NewBody) {
    markError("Extrude New Body is not supported for a face source");
    return false;
  }
  if (!context.previousShape || context.previousShape->IsNull()) {
    markError(operation_ == ExtrudeOperation::Join
                  ? "Extrude Join base shape is missing"
                  : "Extrude Cut base shape is missing");
    return false;
  }
  if (!context.body || source.face.bodyId != context.body->id()) {
    markError("Extrude face belongs to a different Body");
    return false;
  }
  try {
    std::string faceError;
    TopoDS_Shape result;
    FaceExtrudeGeometry geometry;
    if (!buildExtrusionFromFace(*context.previousShape, source.face, lengthMm_,
                                operation_, reversed_, &result, &geometry,
                                &faceError)) {
      markError("Extrude " + faceError);
      return false;
    }
    setShape(std::make_shared<TopoDS_Shape>(result));
    markValid();
    return true;
  } catch (const Standard_Failure& failure) {
    const char* message = failure.what();
    markError(message && *message ? std::string("OCCT Extrude error: ") + message
                                  : "OCCT Extrude operation failed");
    return false;
  } catch (const std::exception& failure) {
    markError(std::string("Extrude error: ") + failure.what());
    return false;
  } catch (...) {
    markError("Unexpected Extrude geometry error");
    return false;
  }
}

bool ExtrudeFeature::rebuildSketchSource(const RebuildContext& context) {
  const SketchId profileId = profileSketchId();
  const auto* profile = context.document.findSketch(profileId);
  if (!profile) {
    markError("Extrude profile sketch was not found");
    return false;
  }
  try {
    std::string profileError;
    TopoDS_Shape result;
    if (!buildExtrusionFromSketch(*profile, context.previousShape, lengthMm_,
                                  operation_, reversed_, &result, nullptr,
                                  &profileError)) {
      markError("Extrude " + profileError);
      return false;
    }
    setShape(std::make_shared<TopoDS_Shape>(result));
    markValid();
    return true;
  } catch (const Standard_Failure& failure) {
    const char* message = failure.what();
    markError(message && *message ? std::string("OCCT Extrude error: ") + message
                                  : "OCCT Extrude operation failed");
    return false;
  } catch (const std::exception& failure) {
    markError(std::string("Extrude error: ") + failure.what());
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
