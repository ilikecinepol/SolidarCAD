#include "model/DraftFeature.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <GeomAbs_CurveType.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Vec.hxx>

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

bool resolveDraftEdgeAxis(const TopoDS_Shape& baseShape,
                          const FaceReference& draftedFace,
                          const EdgeReference& rotationEdge,
                          gp_Pln* resolvedPlane,
                          gp_Dir* resolvedDirection,
                          std::string* error) {
  const auto face = resolveFaceReference(baseShape, draftedFace.topology());
  if (!face) {
    *error = "Draft face could not be resolved: " + face.error;
    return false;
  }
  const auto edge = resolveEdgeReference(baseShape, rotationEdge.topology());
  if (!edge) {
    *error = "Draft rotation edge could not be resolved: " + edge.error;
    return false;
  }

  bool adjacent = false;
  for (TopExp_Explorer edges(*face.subshape, TopAbs_EDGE); edges.More();
       edges.Next()) {
    if (TopoDS::Edge(edges.Current()).IsSame(*edge.subshape)) {
      adjacent = true;
      break;
    }
  }
  if (!adjacent) {
    *error = "Draft rotation edge must belong to the selected face";
    return false;
  }

  BRepAdaptor_Surface surface(*face.subshape);
  if (surface.GetType() != GeomAbs_Plane) {
    *error = "Draft edge axis requires a planar selected face";
    return false;
  }
  BRepAdaptor_Curve curve(*edge.subshape);
  if (curve.GetType() != GeomAbs_Line) {
    *error = "Draft rotation edge must be straight";
    return false;
  }

  const gp_Dir faceNormal = surface.Plane().Axis().Direction();
  const gp_Dir edgeDirection = curve.Line().Direction();
  gp_Vec neutralNormal(faceNormal);
  neutralNormal.Cross(gp_Vec(edgeDirection));
  if (neutralNormal.SquareMagnitude() < 1e-18) {
    *error = "Draft rotation edge direction is invalid";
    return false;
  }
  const gp_Dir direction(neutralNormal);
  const double first = curve.FirstParameter();
  const double last = curve.LastParameter();
  const gp_Pnt point = curve.Value((first + last) * 0.5);
  *resolvedPlane = gp_Pln(point, direction);
  *resolvedDirection = direction;
  return true;
}

DraftFeature::DraftFeature(FeatureId source, std::vector<FaceReference> faces,
                           PlaneReference plane, AxisReference direction,
                           double angle, bool reversed, std::string name,
                           std::optional<EdgeReference> rotationEdge)
    : ShapeFeature(name.empty() ? "Draft" : std::move(name)), sourceFeatureId_(source),
      draftedFaces_(std::move(faces)), neutralPlane_(std::move(plane)),
      pullDirection_(direction), rotationEdge_(std::move(rotationEdge)),
      angleDeg_(angle), reversed_(reversed) {}
DraftFeature::DraftFeature(FeatureId id, FeatureId source, std::vector<FaceReference> faces,
                           PlaneReference plane, AxisReference direction,
                           double angle, bool reversed, std::string name,
                           std::optional<EdgeReference> rotationEdge)
    : ShapeFeature(id, std::move(name)), sourceFeatureId_(source), draftedFaces_(std::move(faces)),
      neutralPlane_(std::move(plane)), pullDirection_(direction),
      rotationEdge_(std::move(rotationEdge)), angleDeg_(angle),
      reversed_(reversed) {}
FeatureId DraftFeature::sourceFeatureId() const noexcept { return sourceFeatureId_; }
const std::vector<FaceReference>& DraftFeature::draftedFaces() const noexcept { return draftedFaces_; }
const PlaneReference& DraftFeature::neutralPlane() const noexcept { return neutralPlane_; }
const AxisReference& DraftFeature::pullDirection() const noexcept { return pullDirection_; }
const std::optional<EdgeReference>& DraftFeature::rotationEdge() const noexcept { return rotationEdge_; }
double DraftFeature::angleDeg() const noexcept { return angleDeg_; }
bool DraftFeature::reversed() const noexcept { return reversed_; }
void DraftFeature::setDraftedFaces(std::vector<FaceReference> value) { if (draftedFaces_ != value) { draftedFaces_ = std::move(value); setDirty(); } }
void DraftFeature::setNeutralPlane(PlaneReference value) { if (neutralPlane_ != value) { neutralPlane_ = std::move(value); setDirty(); } }
void DraftFeature::setPullDirection(AxisReference value) { if (pullDirection_ != value) { pullDirection_ = value; setDirty(); } }
void DraftFeature::setRotationEdge(std::optional<EdgeReference> value) { if (rotationEdge_ != value) { rotationEdge_ = std::move(value); setDirty(); } }
void DraftFeature::setAngleDeg(double value) noexcept { if (angleDeg_ != value) { angleDeg_ = value; setDirty(); } }
void DraftFeature::setReversed(bool value) noexcept { if (reversed_ != value) { reversed_ = value; setDirty(); } }
std::string DraftFeature::typeName() const { return "Draft"; }
bool DraftFeature::dependsOnSketch(SketchId id) const noexcept {
  return !rotationEdge_ && pullDirection_.sketchId == id;
}
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
  if (rotationEdge_) {
    if (draftedFaces_.size() != 1) {
      markError("Draft rotation edge requires exactly one selected face");
      return false;
    }
    if (rotationEdge_->bodyId != context.body->id() ||
        rotationEdge_->featureId != sourceFeatureId_) {
      markError("Draft rotation edge belongs to a different Body");
      return false;
    }
    if (!resolveDraftEdgeAxis(*context.previousShape, draftedFaces_.front(),
                              *rotationEdge_, &plane, &direction, &error)) {
      markError(std::move(error));
      return false;
    }
  } else if (!resolveDraftReferences(context.document, *context.previousShape, neutralPlane_, pullDirection_, &plane, &direction, &error)) { markError(std::move(error)); return false; }
  auto result = buildDraftShape(*context.previousShape, indices, plane, direction, angleDeg_, reversed_, &error);
  if (!result) { markError(std::move(error)); return false; }
  setShape(std::move(result)); markValid(); return true;
}
std::unique_ptr<Feature> DraftFeature::clone() const { return std::make_unique<DraftFeature>(*this); }

}  // namespace solidar
