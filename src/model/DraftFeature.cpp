#include "model/DraftFeature.h"

#include <BRepAdaptor_Surface.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopoDS_Shape.hxx>

#include <cmath>
#include <utility>

#include "model/Body.h"
#include "model/DraftBuilder.h"
#include "model/TopologyReferenceResolver.h"

namespace solidar {
namespace {
bool resolveDirection(const Document& document, const AxisReference& reference,
                      gp_Dir* result, std::string* error) {
  Vector3d direction{};
  if (reference.type == AxisReferenceType::GlobalX) direction = {1, 0, 0};
  else if (reference.type == AxisReferenceType::GlobalY) direction = {0, 1, 0};
  else if (reference.type == AxisReferenceType::GlobalZ) direction = {0, 0, 1};
  else {
    const auto* sketch = document.findSketch(reference.sketchId);
    if (!sketch) { *error = "Draft direction sketch was not found"; return false; }
    if (reference.type == AxisReferenceType::SketchHorizontalAxis)
      direction = sketch->placement.xDirection;
    else if (reference.type == AxisReferenceType::SketchVerticalAxis)
      direction = sketch->placement.yDirection;
    else {
      const auto index = sketch->geometry.lineIndex(reference.lineId);
      if (!index) { *error = "Draft direction line was not found"; return false; }
      const auto& line = sketch->geometry.lines()[*index];
      const auto a = sketch->placement.toWorld(line.start.xMm, line.start.yMm);
      const auto b = sketch->placement.toWorld(line.end.xMm, line.end.yMm);
      direction = {b.x - a.x, b.y - a.y, b.z - a.z};
    }
  }
  const double length = std::sqrt(direction.x * direction.x +
                                  direction.y * direction.y +
                                  direction.z * direction.z);
  if (!std::isfinite(length) || length < 1e-12) { *error = "Draft pull direction is invalid"; return false; }
  *result = gp_Dir(direction.x, direction.y, direction.z); return true;
}
}

bool resolveDraftReferences(const Document& document,
                            const TopoDS_Shape& baseShape,
                            const PlaneReference& plane,
                            const AxisReference& direction,
                            gp_Pln* resolvedPlane,
                            gp_Dir* resolvedDirection, std::string* error) {
  if (!resolveDirection(document, direction, resolvedDirection, error)) return false;
  if (plane.type == NeutralPlaneType::GlobalXY) *resolvedPlane = gp_Pln(gp_Pnt(0,0,0), gp_Dir(0,0,1));
  else if (plane.type == NeutralPlaneType::GlobalXZ) *resolvedPlane = gp_Pln(gp_Pnt(0,0,0), gp_Dir(0,1,0));
  else if (plane.type == NeutralPlaneType::GlobalYZ) *resolvedPlane = gp_Pln(gp_Pnt(0,0,0), gp_Dir(1,0,0));
  else {
    if (!plane.face) { *error = "Draft neutral face is missing"; return false; }
    const auto face = resolveFaceReference(baseShape, plane.face->topology());
    if (!face) { *error = "Draft neutral face could not be resolved: " + face.error; return false; }
    BRepAdaptor_Surface surface(*face.subshape);
    if (surface.GetType() != GeomAbs_Plane) { *error = "Draft neutral face must be planar"; return false; }
    *resolvedPlane = surface.Plane();
  }
  return true;
}

DraftFeature::DraftFeature(FeatureId source, std::vector<FaceReference> faces,
                           PlaneReference plane, AxisReference direction,
                           double angle, bool reversed, std::string name)
    : ShapeFeature(name.empty() ? "Draft" : std::move(name)), sourceFeatureId_(source),
      draftedFaces_(std::move(faces)), neutralPlane_(std::move(plane)),
      pullDirection_(direction), angleDeg_(angle), reversed_(reversed) {}
DraftFeature::DraftFeature(FeatureId id, FeatureId source, std::vector<FaceReference> faces,
                           PlaneReference plane, AxisReference direction,
                           double angle, bool reversed, std::string name)
    : ShapeFeature(id, std::move(name)), sourceFeatureId_(source), draftedFaces_(std::move(faces)),
      neutralPlane_(std::move(plane)), pullDirection_(direction), angleDeg_(angle), reversed_(reversed) {}
FeatureId DraftFeature::sourceFeatureId() const noexcept { return sourceFeatureId_; }
const std::vector<FaceReference>& DraftFeature::draftedFaces() const noexcept { return draftedFaces_; }
const PlaneReference& DraftFeature::neutralPlane() const noexcept { return neutralPlane_; }
const AxisReference& DraftFeature::pullDirection() const noexcept { return pullDirection_; }
double DraftFeature::angleDeg() const noexcept { return angleDeg_; }
bool DraftFeature::reversed() const noexcept { return reversed_; }
void DraftFeature::setDraftedFaces(std::vector<FaceReference> value) { if (draftedFaces_ != value) { draftedFaces_ = std::move(value); setDirty(); } }
void DraftFeature::setNeutralPlane(PlaneReference value) { if (neutralPlane_ != value) { neutralPlane_ = std::move(value); setDirty(); } }
void DraftFeature::setPullDirection(AxisReference value) { if (pullDirection_ != value) { pullDirection_ = value; setDirty(); } }
void DraftFeature::setAngleDeg(double value) noexcept { if (angleDeg_ != value) { angleDeg_ = value; setDirty(); } }
void DraftFeature::setReversed(bool value) noexcept { if (reversed_ != value) { reversed_ = value; setDirty(); } }
std::string DraftFeature::typeName() const { return "Draft"; }
bool DraftFeature::dependsOnSketch(SketchId id) const noexcept { return pullDirection_.sketchId == id; }
bool DraftFeature::rebuild(const RebuildContext& context) {
  clearShape();
  if (!context.previousShape || context.previousShape->IsNull()) { markError("Draft base shape is missing"); return false; }
  if (!context.body || draftedFaces_.empty()) { markError(draftedFaces_.empty() ? "Draft requires at least one face" : "Draft face belongs to a different Body"); return false; }
  const auto& features = context.body->features(); std::size_t ownIndex = features.size();
  for (std::size_t i = 0; i < features.size(); ++i) if (features[i].get() == this) { ownIndex = i; break; }
  if (ownIndex == 0 || ownIndex == features.size() || features[ownIndex - 1]->id() != sourceFeatureId_) { markError("Draft source Feature could not be resolved"); return false; }
  std::vector<std::size_t> indices;
  for (const auto& face : draftedFaces_) {
    if (face.bodyId != context.body->id() || face.featureId != sourceFeatureId_) { markError("Draft faces must belong to the active source Feature"); return false; }
    const auto resolved = resolveFaceReference(*context.previousShape, face.topology());
    if (!resolved) { markError("Draft face could not be resolved: " + resolved.error); return false; }
    indices.push_back(resolved.index);
  }
  gp_Pln plane; gp_Dir direction; std::string error;
  if (!resolveDraftReferences(context.document, *context.previousShape, neutralPlane_, pullDirection_, &plane, &direction, &error)) { markError(std::move(error)); return false; }
  auto result = buildDraftShape(*context.previousShape, indices, plane, direction, angleDeg_, reversed_, &error);
  if (!result) { markError(std::move(error)); return false; }
  setShape(std::move(result)); markValid(); return true;
}
std::unique_ptr<Feature> DraftFeature::clone() const { return std::make_unique<DraftFeature>(*this); }

}  // namespace solidar
