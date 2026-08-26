#include "model/SketchProfileBuilder.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <Standard_Failure.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <utility>

#include "model/Document.h"

namespace solidar {

bool buildPlanarFaceFromSketch(const DocumentSketch& profile,
                               TopoDS_Face* face, std::string* error) {
  const auto fail = [&](std::string message) {
    if (error) *error = std::move(message);
    return false;
  };
  if (!face) return fail("Profile output face is missing");
  if (!profile.supportResolved) return fail("Profile support is unresolved");
  const auto& geometry = profile.geometry;
  std::size_t solidLines = 0;
  for (const auto& line : geometry.lines())
    if (!line.dashed) ++solidLines;
  std::size_t solidCircles = 0;
  const sketch::Circle* circle = nullptr;
  for (const auto& candidate : geometry.circles())
    if (!candidate.dashed) {
      ++solidCircles;
      circle = &candidate;
    }
  if ((solidLines > 0 && solidCircles > 0) || solidCircles > 1)
    return fail("Extrude 2.0 supports one profile at a time");
  if (solidCircles == 0 && solidLines < 3)
    return fail("Profile has insufficient geometry");
  if (solidCircles == 0 && !geometry.isClosed())
    return fail("Profile is not closed");

  try {
    if (circle) {
      if (!std::isfinite(circle->radiusMm) || circle->radiusMm <= 0.0 ||
          !std::isfinite(circle->center.xMm) ||
          !std::isfinite(circle->center.yMm))
        return fail("Circle radius and center must be finite and positive");
      const auto center = profile.placement.toWorld(circle->center.xMm,
                                                    circle->center.yMm);
      const auto normal = profile.placement.normal();
      const auto x = profile.placement.xDirection;
      gp_Ax2 axes(gp_Pnt(center.x, center.y, center.z),
                  gp_Dir(normal.x, normal.y, normal.z),
                  gp_Dir(x.x, x.y, x.z));
      BRepBuilderAPI_MakeEdge edgeBuilder(gp_Circ(axes, circle->radiusMm));
      if (!edgeBuilder.IsDone()) return fail("Could not build a circle edge");
      BRepBuilderAPI_MakeWire circleWire(edgeBuilder.Edge());
      if (!circleWire.IsDone()) return fail("Could not build a circle wire");
      BRepBuilderAPI_MakeFace circleFace(circleWire.Wire(), true);
      if (!circleFace.IsDone()) return fail("Could not build a circle face");
      *face = circleFace.Face();
      return true;
    }

    BRepBuilderAPI_MakeWire wireBuilder;
    std::size_t edgeCount = 0;
    for (const auto& line : geometry.lines()) {
      if (line.dashed) continue;
      const auto start = profile.placement.toWorld(line.start.xMm,
                                                   line.start.yMm);
      const auto end = profile.placement.toWorld(line.end.xMm,
                                                 line.end.yMm);
      BRepBuilderAPI_MakeEdge edgeBuilder(gp_Pnt(start.x, start.y, start.z),
                                          gp_Pnt(end.x, end.y, end.z));
      if (!edgeBuilder.IsDone()) return fail("Could not build a profile edge");
      wireBuilder.Add(edgeBuilder.Edge());
      ++edgeCount;
    }
    if (edgeCount < 3 || !wireBuilder.IsDone())
      return fail("Could not build a profile wire");
    BRepBuilderAPI_MakeFace faceBuilder(wireBuilder.Wire(), true);
    if (!faceBuilder.IsDone()) return fail("Could not build a profile face");
    *face = faceBuilder.Face();
    return true;
  } catch (const Standard_Failure& failure) {
    const char* message = failure.what();
    return fail(message && *message ? std::string("OCCT profile error: ") + message
                                    : "OCCT profile operation failed");
  } catch (...) {
    return fail("Unexpected profile geometry error");
  }
}

bool buildExtrusionPrismFromSketch(const DocumentSketch& profile,
                                   double distanceMm, bool reversed,
                                   TopoDS_Shape* prism, std::string* error) {
  const auto fail = [&](std::string message) {
    if (error) *error = std::move(message);
    return false;
  };
  if (!prism) return fail("Prism output shape is missing");
  if (!std::isfinite(distanceMm) || distanceMm <= 0.0)
    return fail("distance must be a finite positive value");
  TopoDS_Face profileFace;
  if (!buildPlanarFaceFromSketch(profile, &profileFace, error)) return false;
  try {
    const auto normal = profile.placement.normal();
    const double sign = reversed ? -1.0 : 1.0;
    BRepPrimAPI_MakePrism builder(
        profileFace, gp_Vec(sign * normal.x * distanceMm,
                            sign * normal.y * distanceMm,
                            sign * normal.z * distanceMm));
    builder.Build();
    if (!builder.IsDone() || builder.Shape().IsNull())
      return fail("could not build a solid prism");
    *prism = builder.Shape();
    return true;
  } catch (const Standard_Failure& failure) {
    const char* message = failure.what();
    return fail(message && *message ? std::string("OCCT prism error: ") + message
                                    : "OCCT prism operation failed");
  } catch (...) {
    return fail("Unexpected prism geometry error");
  }
}

}  // namespace solidar
