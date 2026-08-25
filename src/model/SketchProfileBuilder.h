#pragma once

#include <string>

#include <TopoDS_Face.hxx>

namespace solidar {
struct DocumentSketch;

bool buildPlanarFaceFromSketch(const DocumentSketch& profile,
                               TopoDS_Face* face, std::string* error);

}  // namespace solidar
