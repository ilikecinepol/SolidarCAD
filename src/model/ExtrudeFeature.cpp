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
namespace {

constexpr double kSelectionMatchTolerance = 1e-7;

bool sameValue(double first, double second) noexcept {
  return std::abs(first - second) <= kSelectionMatchTolerance;
}

bool samePoint(sketch::Point first, sketch::Point second) noexcept {
  return sameValue(first.xMm, second.xMm) &&
         sameValue(first.yMm, second.yMm);
}

bool sameLine(const sketch::Line& first, const sketch::Line& second) noexcept {
  return first.dashed == second.dashed &&
         ((samePoint(first.start, second.start) &&
           samePoint(first.end, second.end)) ||
          (samePoint(first.start, second.end) &&
           samePoint(first.end, second.start)));
}

bool sameCircle(const sketch::Circle& first,
                const sketch::Circle& second) noexcept {
  return first.dashed == second.dashed &&
         samePoint(first.center, second.center) &&
         sameValue(first.radiusMm, second.radiusMm);
}

bool sameCircleIdentity(const sketch::Circle& first,
                        const sketch::Circle& second) noexcept {
  return first.dashed == second.dashed &&
         samePoint(first.center, second.center);
}

bool sameArc(const sketch::Arc& first, const sketch::Arc& second) noexcept {
  return first.dashed == second.dashed &&
         samePoint(first.center, second.center) &&
         sameValue(first.radiusMm, second.radiusMm) &&
         sameValue(first.startAngleRad, second.startAngleRad) &&
         sameValue(first.sweepAngleRad, second.sweepAngleRad);
}

bool sameArcIdentity(const sketch::Arc& first,
                     const sketch::Arc& second) noexcept {
  return first.dashed == second.dashed &&
         samePoint(first.center, second.center) &&
         sameValue(first.startAngleRad, second.startAngleRad) &&
         sameValue(first.sweepAngleRad, second.sweepAngleRad);
}

template <typename Primitive, typename Equal, typename SameIdentity,
          typename IdAt>
bool matchPrimitiveIds(const std::vector<Primitive>& source,
                       const std::vector<Primitive>& selection,
                       Equal equal, SameIdentity sameIdentity, IdAt idAt,
                       std::vector<sketch::GeometryId>* ids) {
  std::vector<bool> used(source.size(), false);
  ids->clear();
  ids->reserve(selection.size());
  for (const auto& selected : selection) {
    std::optional<std::size_t> matched;
    for (std::size_t index = 0; index < source.size(); ++index) {
      if (used[index] || !equal(source[index], selected)) continue;
      matched = index;
      break;
    }
    if (!matched) {
      // Recover a profile snapshot saved by an older build after its radius
      // was edited. Accept only one unambiguous topological identity match.
      for (std::size_t index = 0; index < source.size(); ++index) {
        if (used[index] || !sameIdentity(source[index], selected)) continue;
        if (matched) return false;
        matched = index;
      }
    }
    if (!matched) return false;
    used[*matched] = true;
    ids->push_back(idAt(*matched));
  }
  return true;
}

}  // namespace

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
  profileOverride_.reset();
  profileSelectionIds_.reset();
  setDirty();
}

const std::optional<sketch::Sketch>&
ExtrudeFeature::profileOverride() const noexcept {
  return profileOverride_;
}

void ExtrudeFeature::setProfileOverride(
    std::optional<sketch::Sketch> profile) {
  profileOverride_ = std::move(profile);
  profileSelectionIds_.reset();
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
  const auto* sourceProfile = context.document.findSketch(profileId);
  if (!sourceProfile) {
    markError("Extrude profile sketch was not found");
    return false;
  }

  // The document sketch may contain several closed contours.  A feature made
  // from one picked region must rebuild from that exact region, not from the
  // entire sketch.  Placement/support still come from the source DocumentSketch.
  DocumentSketch selectedProfile = *sourceProfile;
  if (profileOverride_) {
    std::string selectionError;
    if (!resolveProfileOverride(sourceProfile->geometry,
                                &selectedProfile.geometry,
                                &selectionError)) {
      markError("Extrude " + selectionError);
      return false;
    }
  }

  try {
    std::string profileError;
    TopoDS_Shape result;
    if (!buildExtrusionFromSketch(selectedProfile, context.previousShape,
                                  lengthMm_, operation_, reversed_, &result,
                                  nullptr, &profileError)) {
      markError("Extrude " + profileError);
      return false;
    }
    // Persist the refreshed fallback only after OCCT accepted the updated
    // profile. A loaded project can then infer the same stable IDs again.
    if (profileOverride_ && profileSelectionIds_)
      profileOverride_ = selectedProfile.geometry;
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

bool ExtrudeFeature::resolveProfileOverride(const sketch::Sketch& source,
                                            sketch::Sketch* selected,
                                            std::string* error) {
  if (!selected || !profileOverride_) return false;

  if (!profileSelectionIds_) {
    ProfileSelectionIds matched;
    const auto& snapshot = *profileOverride_;
    const bool allMatched =
        matchPrimitiveIds(
            source.lines(), snapshot.lines(), sameLine, sameLine,
            [&source](std::size_t index) { return source.lineId(index); },
            &matched.lineIds) &&
        matchPrimitiveIds(
            source.circles(), snapshot.circles(), sameCircle,
            sameCircleIdentity,
            [&source](std::size_t index) { return source.circleId(index); },
            &matched.circleIds) &&
        matchPrimitiveIds(
            source.arcs(), snapshot.arcs(), sameArc, sameArcIdentity,
            [&source](std::size_t index) { return source.arcId(index); },
            &matched.arcIds);

    if (!allMatched) {
      // Compatibility for older files and graph-split regions whose selected
      // boundary is not a one-to-one subset of source primitives.
      *selected = snapshot;
      return true;
    }
    profileSelectionIds_ = std::move(matched);
  }

  sketch::Sketch refreshed;
  refreshed.clear();
  for (const auto id : profileSelectionIds_->lineIds) {
    const auto index = source.lineIndex(id);
    if (!index) {
      if (error) *error = "selected profile line no longer exists";
      return false;
    }
    const auto& line = source.lines()[*index];
    refreshed.addLine(line.start, line.end, line.elementId);
  }
  for (const auto id : profileSelectionIds_->circleIds) {
    const auto index = source.circleIndex(id);
    if (!index) {
      if (error) *error = "selected profile circle no longer exists";
      return false;
    }
    const auto& circle = source.circles()[*index];
    refreshed.addCircle(circle.center, circle.radiusMm);
  }
  for (const auto id : profileSelectionIds_->arcIds) {
    const auto index = source.arcIndex(id);
    if (!index) {
      if (error) *error = "selected profile arc no longer exists";
      return false;
    }
    const auto& arc = source.arcs()[*index];
    refreshed.addArc(arc.center, arc.radiusMm, arc.startAngleRad,
                     arc.sweepAngleRad, arc.dashed);
  }

  *selected = std::move(refreshed);
  return true;
}

std::unique_ptr<Feature> ExtrudeFeature::clone() const {
  return std::make_unique<ExtrudeFeature>(*this);
}

}  // namespace solidar
