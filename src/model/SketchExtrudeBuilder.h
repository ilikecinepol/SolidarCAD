#pragma once

#include <string>
#include <vector>

#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include "model/ExtrudeFeature.h"

namespace solidar {
struct DocumentSketch;

struct SketchExtrudeGeometry {
  gp_Pnt centroid;
  gp_Dir normal;
};

// V1 direct-interaction profile contract: one solid closed line wire or one
// solid circle. Construction geometry is ignored; multiple loops and holes are
// rejected because ExtrudeFeature persists only the owning SketchId.
[[nodiscard]] bool isSupportedSingleSketchProfile(
    const DocumentSketch& profile, std::string* error = nullptr);

// Multi-region profile contract used after Ctrl-selection. Each simple,
// non-degenerate closed contour must be strictly disjoint from every other
// contour; nesting, overlap, touching and self-intersection are rejected.
// Construction geometry is ignored. A single-region profile remains valid.
[[nodiscard]] bool isSupportedSketchProfile(
    const DocumentSketch& profile, std::string* error = nullptr);

// Resolves the same validated multi-region profile used by Extrude into one
// planar face per outer region (with nested holes already applied). Revolve
// consumes this seam so both tools accept exactly the same Ctrl-selected
// profile geometry instead of maintaining subtly different contour rules.
[[nodiscard]] bool buildSketchProfileFaces(
    const DocumentSketch& profile, std::vector<TopoDS_Face>* regions,
    std::string* error = nullptr);

// Authoritative sketch extrusion builder shared by interactive previews and
// ExtrudeFeature recompute. baseShape must be null for NewBody and non-null for
// Join/Cut. NewBody may return a checked multi-solid compound for disjoint
// selected regions.
bool buildExtrusionFromSketch(const DocumentSketch& profile,
                              const TopoDS_Shape* baseShape,
                              double lengthMm, ExtrudeOperation operation,
                              bool reversed, TopoDS_Shape* result,
                              SketchExtrudeGeometry* geometry,
                              std::string* error);

}  // namespace solidar
