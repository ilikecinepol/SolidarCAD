#pragma once

#include <string>

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

namespace solidar {
struct DocumentSketch;

bool buildPlanarFaceFromSketch(const DocumentSketch& profile,
                               TopoDS_Face* face, std::string* error);
bool buildExtrusionPrismFromSketch(const DocumentSketch& profile,
                                   double distanceMm, bool reversed,
                                   TopoDS_Shape* prism, std::string* error);

}  // namespace solidar
