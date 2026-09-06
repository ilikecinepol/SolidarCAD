#include "model/ShellBuilder.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRepBndLib.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <Bnd_Box.hxx>
#include <NCollection_List.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <cmath>
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
  if (!bounds.IsVoid()) {
    Standard_Real xMin, yMin, zMin, xMax, yMax, zMax;
    bounds.Get(xMin, yMin, zMin, xMax, yMax, zMax);
    const double diagonal = std::hypot(std::hypot(xMax - xMin, yMax - yMin),
                                      zMax - zMin);
    if (diagonal > Precision::Confusion() && thicknessMm > diagonal * 10.0)
      return fail("Shell thickness is too large for the source body");
  }
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
      BRepOffsetAPI_MakeThickSolid maker;
      maker.MakeThickSolidByJoin(selection->solids[solidIndex], closingFaces,
                                 outside ? thicknessMm : -thicknessMm,
                                 Precision::Confusion());
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
