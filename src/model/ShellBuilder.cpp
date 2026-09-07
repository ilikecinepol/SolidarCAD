#include "model/ShellBuilder.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <GeomAbs_JoinType.hxx>
#include <NCollection_List.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "model/ShapeContainerUtils.h"

namespace solidar {

std::shared_ptr<TopoDS_Shape> buildShellShape(
    const TopoDS_Shape& baseShape, const std::vector<std::size_t>& faceIndices,
    double thicknessMm, bool outside, std::string* error) {
  const auto fail = [&](std::string message) {
    if (error) *error = std::move(message);
    return std::shared_ptr<TopoDS_Shape>{};
  };
  if (baseShape.IsNull()) return fail("Shell base shape is missing");
  if (!std::isfinite(thicknessMm) || thicknessMm <= 0.0)
    return fail("Shell thickness must be a finite positive value");
  if (faceIndices.empty()) return fail("Shell requires at least one face");
  Bnd_Box bounds;
  BRepBndLib::Add(baseShape, bounds);
  double diagonal = 0.0;
  double smallestDimension = 0.0;
  if (!bounds.IsVoid()) {
    double xMin, yMin, zMin, xMax, yMax, zMax;
    bounds.Get(xMin, yMin, zMin, xMax, yMax, zMax);
    const double dx = xMax - xMin;
    const double dy = yMax - yMin;
    const double dz = zMax - zMin;
    diagonal = std::hypot(std::hypot(dx, dy), dz);
    smallestDimension = std::min({dx, dy, dz});
    // An inward offset cannot retain a cavity once it reaches half of the
    // narrowest global dimension. Keep this inexpensive rejection ahead of
    // OCCT's substantially heavier intersection work.
    if (!outside && smallestDimension > Precision::Confusion() &&
        thicknessMm >= smallestDimension * 0.5)
      return fail("Shell thickness is too large for the narrowest body dimension");
    if (diagonal > Precision::Confusion() && thicknessMm > diagonal * 4.0)
      return fail("Shell thickness is too large for the source body");
  }
  double shortestEdge = std::numeric_limits<double>::max();
  double smallestFaceScale = std::numeric_limits<double>::max();
  for (TopExp_Explorer edges(baseShape, TopAbs_EDGE); edges.More(); edges.Next()) {
    GProp_GProps properties;
    BRepGProp::LinearProperties(edges.Current(), properties);
    if (properties.Mass() > Precision::Confusion())
      shortestEdge = std::min(shortestEdge, properties.Mass());
  }
  for (TopExp_Explorer faces(baseShape, TopAbs_FACE); faces.More(); faces.Next()) {
    GProp_GProps properties;
    BRepGProp::SurfaceProperties(faces.Current(), properties);
    if (properties.Mass() > Precision::SquareConfusion())
      smallestFaceScale = std::min(smallestFaceScale, std::sqrt(properties.Mass()));
  }
  // This deliberately catches only orders-of-magnitude mistakes. Chamfered
  // models legitimately contain edges and faces smaller than their wall
  // thickness, so local feature size must not become an aggressive hard cap.
  const double localScale = std::min(shortestEdge, smallestFaceScale);
  if (std::isfinite(localScale) && thicknessMm > localScale * 8.0)
    return fail("Shell thickness is too large for local edges or faces");
  try {
    std::string mappingError;
    auto selection =
        mapFacesToOwningSolids(baseShape, faceIndices, &mappingError);
    if (!selection) return fail("Shell " + mappingError);
    std::size_t owningSolidCount = 0;
    for (const auto& indices : selection->localFaceIndices)
      owningSolidCount += !indices.empty();
    if (owningSolidCount != 1)
      return fail("Shell v1 requires all removed faces to belong to one solid");

    for (std::size_t solidIndex = 0; solidIndex < selection->solids.size();
         ++solidIndex) {
      const auto& indices = selection->localFaceIndices[solidIndex];
      if (indices.empty()) continue;
      NCollection_List<TopoDS_Shape> closingFaces;
      for (const auto wanted : indices) {
        std::size_t current = 0;
        bool found = false;
        for (TopExp_Explorer faces(selection->solids[solidIndex], TopAbs_FACE);
             faces.More(); faces.Next(), ++current) {
          if (current == wanted) {
            closingFaces.Append(faces.Current());
            found = true;
            break;
          }
        }
        if (!found) return fail("Shell face could not be resolved in owning solid");
      }
      // OCCT 8.0 documents global intersection and self-intersection cleanup
      // as incomplete. Skin/Arc with those switches disabled is its supported
      // robust path. INTERNAL-edge removal is also left disabled: on OCCT 8.0
      // it can turn a recoverable outward preview into a very long cleanup
      // pass. Tolerance remains scale-aware and many orders of magnitude below
      // normal Part Design dimensions.
      const double tolerance = std::clamp(diagonal * 1e-9,
                                          Precision::Confusion(), 1e-5);
      BRepOffsetAPI_MakeThickSolid maker;
      maker.MakeThickSolidByJoin(selection->solids[solidIndex], closingFaces,
                                 outside ? thicknessMm : -thicknessMm,
                                 tolerance, BRepOffset_Skin, false, false,
                                 GeomAbs_Arc, false);
      if (!maker.IsDone() || maker.Shape().IsNull())
        return fail("Shell could not be built with the requested thickness");
      if (!BRepCheck_Analyzer(maker.Shape()).IsValid())
        return fail("Shell result is invalid or self-intersecting");
      selection->solids[solidIndex] = maker.Shape();
    }
    return rebuildSolidContainer(selection->solids);
  } catch (const Standard_Failure& failure) {
    const char* message = failure.what();
    return fail(message && *message ? std::string("OCCT Shell error: ") + message
                                    : "OCCT Shell operation failed");
  } catch (const std::exception& failure) {
    return fail(std::string("Shell error: ") + failure.what());
  } catch (...) {
    return fail("Unexpected Shell geometry error");
  }
}

}  // namespace solidar
