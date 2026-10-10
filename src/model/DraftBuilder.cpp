#include "model/DraftBuilder.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRepOffsetAPI_DraftAngle.hxx>
#include <Standard_Failure.hxx>

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
  if (!std::isfinite(angleDeg) || angleDeg < 0.0 || angleDeg >= 90.0)
    return fail("Draft angle must be finite and in the range [0, 90)");
  if (faceIndices.empty()) return fail("Draft requires at least one face");
  try {
    if (baseShape.IsNull()) return fail("Draft base shape is missing");
    std::string mappingError;
    auto selection = mapFacesToOwningSolids(baseShape, faceIndices, &mappingError);
    if (!selection) return fail("Draft " + mappingError);
    // Zero is a stable intermediate while the signed angular manipulator
    // crosses between the two draft directions. Resolve the selection first,
    // so a stale face reference is not hidden by this no-op result.
    if (angleDeg < 1e-9)
      return std::make_shared<TopoDS_Shape>(baseShape);
    const double radians = angleDeg * std::numbers::pi / 180.0 *
                           (reversed ? -1.0 : 1.0);
    for (std::size_t solidIndex = 0; solidIndex < selection->solids.size();
         ++solidIndex) {
      const auto& selectedFaces = selection->localFaces[solidIndex];
      if (selectedFaces.empty()) continue;
      BRepOffsetAPI_DraftAngle maker(selection->solids[solidIndex]);
      for (const auto& face : selectedFaces) {
        maker.Add(face, pullDirection, radians, neutralPlane);
        if (!maker.AddDone())
          return fail("Draft rejected a selected face for this plane and direction");
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
