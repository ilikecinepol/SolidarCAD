#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>

#include "model/ChamferBuilder.h"
#include "model/ChamferFeature.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletBuilder.h"
#include "model/FilletFeature.h"
#include "model/TopologyReferenceResolver.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return false;                                                        \
    }                                                                      \
  } while (false)

namespace {

std::optional<std::size_t> usableEdge(const TopoDS_Shape& shape, bool fillet) {
  std::size_t index = 0;
  for (TopExp_Explorer it(shape, TopAbs_EDGE); it.More(); it.Next(), ++index) {
    std::string error;
    const auto result = fillet
        ? solidar::buildFilletShape(shape, {index}, 2.0, &error)
        : solidar::buildChamferShape(shape, {index}, 2.0, &error);
    if (result) return index;
  }
  return std::nullopt;
}

std::optional<std::size_t> planarFace(const TopoDS_Shape& shape, bool side) {
  std::size_t index = 0;
  for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next(), ++index) {
    const auto face = TopoDS::Face(it.Current());
    BRepAdaptor_Surface surface(face, true);
    if (surface.GetType() != GeomAbs_Plane) continue;
    const auto normal = surface.Plane().Axis().Direction();
    if ((side && std::abs(normal.Z()) < 0.1) ||
        (!side && std::abs(normal.Z()) > 0.9))
      return index;
  }
  return std::nullopt;
}

bool scenario(bool fillet, bool side) {
  solidar::Document document;
  auto& profile = document.addSketch("Base");
  profile.geometry.addRectangle({0.0, 0.0}, {40.0, 30.0});
  auto& body = document.addBody("Body");
  auto& extrusion = body.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      profile.id, 20.0, "Extrude"));
  CHECK(document.recompute());
  const auto edgeIndex = usableEdge(*extrusion.shape(), fillet);
  CHECK(edgeIndex);
  const auto edge = solidar::makeEdgeReference(
      *extrusion.shape(), body.id(), extrusion.id(), *edgeIndex);
  solidar::ShapeFeature* edgeFeature = nullptr;
  if (fillet)
    edgeFeature = &body.addFeature(std::make_unique<solidar::FilletFeature>(
        edge, 2.0, "Fillet"));
  else
    edgeFeature = &body.addFeature(std::make_unique<solidar::ChamferFeature>(
        edge, 2.0, "Chamfer"));
  CHECK(document.recompute());
  const auto faceIndex = planarFace(*edgeFeature->shape(), side);
  CHECK(faceIndex);
  const auto support = solidar::makeFaceReference(
      *edgeFeature->shape(), body.id(), edgeFeature->id(), *faceIndex);
  auto& attached = document.addSketch("Attached");
  CHECK(document.attachSketchToFace(attached.id, support));
  CHECK(attached.supportResolved);
  CHECK(attached.support.face.featureId == edgeFeature->id());
  CHECK(solidar::resolveFaceReference(*edgeFeature->shape(),
                                     attached.support.face.topology()));
  return true;
}

}  // namespace

int main() {
  return scenario(true, false) && scenario(true, true) &&
                 scenario(false, false)
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
