#include "model/ExtrudeFeature.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <memory>
#include <utility>

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
  if (!profile->supportResolved) {
    markError("Extrude profile support is unresolved");
    return false;
  }

  const auto& geometry = profile->geometry;
  if (geometry.lines().size() < 3) {
    markError("Extrude profile has insufficient line geometry");
    return false;
  }
  if (!geometry.circles().empty()) {
    markError("Extrude 1.0 supports line profiles only");
    return false;
  }
  if (!geometry.isClosed()) {
    markError("Extrude profile is not closed");
    return false;
  }

  try {
    BRepBuilderAPI_MakeWire wireBuilder;
    std::size_t edgeCount = 0;
    for (const auto& line : geometry.lines()) {
      if (line.dashed) continue;
      const auto start = profile->placement.toWorld(line.start.xMm,
                                                     line.start.yMm);
      const auto end = profile->placement.toWorld(line.end.xMm,
                                                   line.end.yMm);
      BRepBuilderAPI_MakeEdge edgeBuilder(gp_Pnt(start.x, start.y, start.z),
                                          gp_Pnt(end.x, end.y, end.z));
      if (!edgeBuilder.IsDone()) {
        markError("Extrude could not build a profile edge");
        return false;
      }
      wireBuilder.Add(edgeBuilder.Edge());
      ++edgeCount;
    }
    if (edgeCount < 3 || !wireBuilder.IsDone()) {
      markError("Extrude could not build a profile wire");
      return false;
    }

    BRepBuilderAPI_MakeFace faceBuilder(wireBuilder.Wire(), true);
    if (!faceBuilder.IsDone()) {
      markError("Extrude could not build a profile face");
      return false;
    }

    const auto normal = profile->placement.normal();
    BRepPrimAPI_MakePrism prismBuilder(
        faceBuilder.Face(),
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
