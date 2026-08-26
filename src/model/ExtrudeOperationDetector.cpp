#include "model/ExtrudeOperationDetector.h"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>

#include <cmath>

#include "model/Document.h"
#include "model/SketchProfileBuilder.h"

namespace solidar {

NormalizedExtrusionInput normalizeExtrusionInput(double distanceMm,
                                                  bool reversed) noexcept {
  if (distanceMm < 0.0) return {-distanceMm, !reversed};
  return {distanceMm, reversed};
}

ExtrudeOperation detectExtrudeOperation(const DocumentSketch& profile,
                                        double distanceMm, bool reversed,
                                        const TopoDS_Shape* targetBody,
                                        bool faceSupported) {
  if (!targetBody || targetBody->IsNull()) return ExtrudeOperation::NewBody;
  const auto input = normalizeExtrusionInput(distanceMm, reversed);
  if (!std::isfinite(input.distanceMm) || input.distanceMm <= 0.0)
    return ExtrudeOperation::NewBody;

  try {
    // Include a tiny strip behind the support plane.  An outward prism shares
    // its support face with the Body but otherwise has zero common volume;
    // the tolerance strip turns that valid attachment into a robust OCCT
    // volume intersection without relying on bounding boxes.
    constexpr double kContactToleranceMm = 1.0e-5;
    DocumentSketch detectionProfile = profile;
    const auto normal = profile.placement.normal();
    const double direction = input.reversed ? -1.0 : 1.0;
    detectionProfile.placement.origin.x -=
        direction * normal.x * kContactToleranceMm;
    detectionProfile.placement.origin.y -=
        direction * normal.y * kContactToleranceMm;
    detectionProfile.placement.origin.z -=
        direction * normal.z * kContactToleranceMm;
    TopoDS_Shape prism;
    std::string error;
    if (!buildExtrusionPrismFromSketch(
            detectionProfile, input.distanceMm + kContactToleranceMm,
            input.reversed, &prism, &error))
      return ExtrudeOperation::NewBody;
    BRepAlgoAPI_Common common(*targetBody, prism);
    common.Build();
    if (!common.IsDone() || common.Shape().IsNull())
      return ExtrudeOperation::NewBody;

    const TopoDS_Shape intersection = common.Shape();
    GProp_GProps properties;
    BRepGProp::VolumeProperties(intersection, properties);
    bool meaningful = properties.Mass() > 1.0e-9;
    if (!meaningful) {
      const TopExp_Explorer faces(intersection, TopAbs_FACE);
      meaningful = faces.More();
    }
    if (!meaningful) return ExtrudeOperation::NewBody;
    if (!faceSupported) return ExtrudeOperation::Join;
    return input.reversed ? ExtrudeOperation::Cut : ExtrudeOperation::Join;
  } catch (const Standard_Failure&) {
    return ExtrudeOperation::NewBody;
  } catch (...) {
    return ExtrudeOperation::NewBody;
  }
}

}  // namespace solidar
