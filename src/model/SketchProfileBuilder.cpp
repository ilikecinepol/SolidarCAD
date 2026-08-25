#include "model/SketchProfileBuilder.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <Standard_Failure.hxx>
#include <gp_Pnt.hxx>

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
  if (geometry.lines().size() < 3)
    return fail("Profile has insufficient line geometry");
  if (!geometry.circles().empty())
    return fail("Feature 1.0 supports line profiles only");
  if (!geometry.isClosed()) return fail("Profile is not closed");

  try {
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

}  // namespace solidar
