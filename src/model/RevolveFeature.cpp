#include "model/RevolveFeature.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <cmath>
#include <exception>
#include <numbers>
#include <utility>

#include "model/SketchProfileBuilder.h"

namespace solidar {
namespace {
bool resolveAxis(const Document& document, const AxisReference& reference,
                 gp_Ax1* result, std::string* error) {
  if (reference.type == AxisReferenceType::GlobalX ||
      reference.type == AxisReferenceType::GlobalY ||
      reference.type == AxisReferenceType::GlobalZ) {
    const Vector3d direction = reference.type == AxisReferenceType::GlobalX
                                   ? Vector3d{1.0, 0.0, 0.0}
                                   : reference.type == AxisReferenceType::GlobalY
                                         ? Vector3d{0.0, 1.0, 0.0}
                                         : Vector3d{0.0, 0.0, 1.0};
    *result = gp_Ax1(gp_Pnt(0.0, 0.0, 0.0),
                     gp_Dir(direction.x, direction.y, direction.z));
    return true;
  }
  const auto* sketch = document.findSketch(reference.sketchId);
  if (!sketch) { *error = "Revolve axis sketch was not found"; return false; }
  const auto& placement = sketch->placement;
  Point3d origin = placement.origin;
  Vector3d direction{};
  if (reference.type == AxisReferenceType::SketchHorizontalAxis) {
    direction = placement.xDirection;
  } else if (reference.type == AxisReferenceType::SketchVerticalAxis) {
    direction = placement.yDirection;
  } else {
    const auto index = sketch->geometry.lineIndex(reference.lineId);
    if (!index) { *error = "Revolve axis line was not found"; return false; }
    const auto& line = sketch->geometry.lines()[*index];
    origin = placement.toWorld(line.start.xMm, line.start.yMm);
    const auto end = placement.toWorld(line.end.xMm, line.end.yMm);
    direction = {end.x - origin.x, end.y - origin.y, end.z - origin.z};
  }
  const double length = std::sqrt(direction.x * direction.x +
                                  direction.y * direction.y +
                                  direction.z * direction.z);
  if (!std::isfinite(length) || length <= 1e-12) {
    *error = "Revolve axis is invalid"; return false;
  }
  *result = gp_Ax1(gp_Pnt(origin.x, origin.y, origin.z),
                   gp_Dir(direction.x, direction.y, direction.z));
  return true;
}
}

RevolveFeature::RevolveFeature(SketchId profile, AxisReference axis,
                               double angle, std::string name,
                               ExtrudeOperation operation, bool reversed)
    : ShapeFeature(name.empty() ? "Revolve" : std::move(name)),
      profileSketchId_(profile), axis_(axis), angleDeg_(angle),
      operation_(operation), reversed_(reversed) {}
RevolveFeature::RevolveFeature(FeatureId id, SketchId profile,
                               AxisReference axis, double angle,
                               std::string name, ExtrudeOperation operation,
                               bool reversed)
    : ShapeFeature(id, std::move(name)), profileSketchId_(profile), axis_(axis),
      angleDeg_(angle), operation_(operation), reversed_(reversed) {}
SketchId RevolveFeature::profileSketchId() const noexcept { return profileSketchId_; }
const AxisReference& RevolveFeature::axis() const noexcept { return axis_; }
double RevolveFeature::angleDeg() const noexcept { return angleDeg_; }
ExtrudeOperation RevolveFeature::operation() const noexcept { return operation_; }
bool RevolveFeature::reversed() const noexcept { return reversed_; }
void RevolveFeature::setProfileSketchId(SketchId value) noexcept { if (profileSketchId_ != value) { profileSketchId_ = value; setDirty(); } }
void RevolveFeature::setAxis(AxisReference value) noexcept { if (axis_ != value) { axis_ = value; setDirty(); } }
void RevolveFeature::setAngleDeg(double value) noexcept { if (angleDeg_ != value) { angleDeg_ = value; setDirty(); } }
void RevolveFeature::setOperation(ExtrudeOperation value) noexcept { if (operation_ != value) { operation_ = value; setDirty(); } }
void RevolveFeature::setReversed(bool value) noexcept { if (reversed_ != value) { reversed_ = value; setDirty(); } }
std::string RevolveFeature::typeName() const { return "Revolve"; }
bool RevolveFeature::dependsOnSketch(SketchId id) const noexcept {
  return profileSketchId_ == id || axis_.sketchId == id;
}

bool RevolveFeature::rebuild(const RebuildContext& context) {
  clearShape();
  if (!std::isfinite(angleDeg_) || angleDeg_ <= 0.0 || angleDeg_ > 360.0) {
    markError("Revolve angle must be finite and in the range (0, 360]");
    return false;
  }
  const auto* profile = context.document.findSketch(profileSketchId_);
  if (!profile) { markError("Revolve profile sketch was not found"); return false; }
  try {
    TopoDS_Face face;
    std::string error;
    if (!buildPlanarFaceFromSketch(*profile, &face, &error)) {
      markError("Revolve " + error); return false;
    }
    gp_Ax1 axis;
    if (!resolveAxis(context.document, axis_, &axis, &error)) {
      markError(error); return false;
    }
    const double radians = angleDeg_ * std::numbers::pi / 180.0 *
                           (reversed_ ? -1.0 : 1.0);
    BRepPrimAPI_MakeRevol builder(face, axis, radians, true);
    builder.Build();
    if (!builder.IsDone() || builder.Shape().IsNull()) {
      markError("Revolve could not build a valid shape"); return false;
    }
    TopoDS_Shape result = builder.Shape();
    if (operation_ == ExtrudeOperation::NewBody) {
      if (context.previousShape && !context.previousShape->IsNull()) {
        markError("Revolve New Body must be the first feature of a Body"); return false;
      }
    } else {
      if (!context.previousShape || context.previousShape->IsNull()) {
        markError(operation_ == ExtrudeOperation::Join
                      ? "Revolve Join base shape is missing"
                      : "Revolve Cut base shape is missing");
        return false;
      }
      if (operation_ == ExtrudeOperation::Join) {
        BRepAlgoAPI_Fuse boolean(*context.previousShape, result); boolean.Build();
        if (!boolean.IsDone() || boolean.Shape().IsNull()) { markError("Revolve Join boolean fuse failed"); return false; }
        result = boolean.Shape();
      } else {
        BRepAlgoAPI_Cut boolean(*context.previousShape, result); boolean.Build();
        if (!boolean.IsDone() || boolean.Shape().IsNull()) { markError("Revolve Cut boolean cut failed"); return false; }
        result = boolean.Shape();
      }
    }
    TopExp_Explorer solids(result, TopAbs_SOLID);
    if (!solids.More()) { markError("Revolve result does not contain a solid"); return false; }
    setShape(std::make_shared<TopoDS_Shape>(result)); markValid(); return true;
  } catch (const Standard_Failure& failure) {
    const char* message = failure.what();
    markError(message && *message ? std::string("OCCT Revolve error: ") + message
                                  : "OCCT Revolve operation failed");
  } catch (const std::exception& failure) {
    markError(std::string("Revolve error: ") + failure.what());
  } catch (...) { markError("Unexpected Revolve geometry error"); }
  return false;
}
std::unique_ptr<Feature> RevolveFeature::clone() const { return std::make_unique<RevolveFeature>(*this); }
}  // namespace solidar
