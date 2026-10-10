#pragma once

#include <string>

#include <TopoDS_Face.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include "model/SketchPlacement.h"
#include "model/OperationFailure.h"

class TopoDS_Shape;
class gp_Pln;

namespace solidar {

enum class ExtrudeOperation;
class TopologyIndex;

// Resolved geometry of the planar face an extrusion is built from. The normal
// is oriented outward from the solid (matching the TopAbs_REVERSED convention
// already used by TopologyReferenceResolver and SketchPlacement).
struct FaceExtrudeGeometry {
  TopoDS_Face face;
  gp_Pnt centroid;
  gp_Dir normal;
};

// Verifies that `face` is planar and computes its plane plus the outward
// oriented normal. Returns false with a stable technical message in `error`
// when the face is not planar (the UI layer maps this to a user-facing string).
[[nodiscard]] bool resolveFacePlaneAndNormal(const TopoDS_Face& face,
                                             gp_Pln* plane, gp_Dir* normal,
                                             std::string* error,
                                             OperationFailureCode* code = nullptr);

// Builds a native face extrusion.
//
// `baseShape` is the owning shape the `source` FaceReference points at; it is
// used both to resolve the reference (via resolveFaceReference) and as the
// boolean base. The caller must provide the current upstream shape whose
// bodyId/featureId match `source`. Join fuses the prism into the body, Cut
// removes it; NewBody is not supported for a face source and returns an error.
[[nodiscard]] bool buildExtrusionFromFace(
    const TopoDS_Shape& baseShape, const FaceReference& source,
    double lengthMm, ExtrudeOperation operation, bool reversed,
    TopoDS_Shape* result, FaceExtrudeGeometry* geometry, std::string* error,
    TopoDS_Shape* sweptTool = nullptr,
    OperationFailureCode* code = nullptr);

// Indexed overload for document/tool-session callers that already own the
// immutable topology snapshot for `baseShape`.
[[nodiscard]] bool buildExtrusionFromFace(
    const TopoDS_Shape& baseShape, const TopologyIndex& topology,
    const FaceReference& source, double lengthMm,
    ExtrudeOperation operation, bool reversed, TopoDS_Shape* result,
    FaceExtrudeGeometry* geometry, std::string* error,
    TopoDS_Shape* sweptTool = nullptr,
    OperationFailureCode* code = nullptr);

}  // namespace solidar
