#include "model/DraftBuilder.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRepOffsetAPI_DraftAngle.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <numbers>
#include <utility>

#include "model/ShapeContainerUtils.h"

namespace solidar {

std::shared_ptr<TopoDS_Shape> buildDraftShape(
    const TopoDS_Shape& baseShape, const std::vector<std::size_t>& faceIndices,
    const gp_Pln& neutralPlane, const gp_Dir& pullDirection,
    double angleDeg, bool reversed, std::string* error) {
  const auto fail = [&](std::string message) {
    if (error) *error = std::move(message);
    return std::shared_ptr<TopoDS_Shape>{};
  };
  if (baseShape.IsNull()) return fail("Draft base shape is missing");
  if (!std::isfinite(angleDeg) || angleDeg <= 0.0 || angleDeg >= 89.0)
    return fail("Draft angle must be finite and in the range (0, 89)");
  if (faceIndices.empty()) return fail("Draft requires at least one face");
  try {
    std::string mappingError;
    auto selection = mapFacesToOwningSolids(baseShape, faceIndices, &mappingError);
    if (!selection) return fail("Draft " + mappingError);
    const double radians = angleDeg * std::numbers::pi / 180.0 *
                           (reversed ? -1.0 : 1.0);
    for (std::size_t solidIndex = 0; solidIndex < selection->solids.size();
         ++solidIndex) {
      const auto& indices = selection->localFaceIndices[solidIndex];
      if (indices.empty()) continue;
      BRepOffsetAPI_DraftAngle maker(selection->solids[solidIndex]);
      for (const auto wanted : indices) {
        std::size_t current = 0;
        bool found = false;
        for (TopExp_Explorer faces(selection->solids[solidIndex], TopAbs_FACE);
             faces.More(); faces.Next(), ++current) {
          if (current != wanted) continue;
          maker.Add(TopoDS::Face(faces.Current()), pullDirection, radians,
                    neutralPlane);
          if (!maker.AddDone())
            return fail("Draft rejected a selected face for this plane and direction");
          found = true;
          break;
        }
        if (!found) return fail("Draft face could not be resolved in owning solid");
      }
      maker.Build();
      if (!maker.IsDone() || maker.Shape().IsNull())
        return fail("Draft could not be built with the requested angle");
      if (!BRepCheck_Analyzer(maker.Shape()).IsValid())
        return fail("Draft result is invalid or collapsed");
      selection->solids[solidIndex] = maker.Shape();
    }
    return rebuildSolidContainer(selection->solids);
  } catch (const Standard_Failure& failure) {
    const char* message = failure.what();
    return fail(message && *message ? std::string("OCCT Draft error: ") + message
                                    : "OCCT Draft operation failed");
  } catch (const std::exception& failure) {
    return fail(std::string("Draft error: ") + failure.what());
  } catch (...) {
    return fail("Unexpected Draft geometry error");
  }
}

}  // namespace solidar
