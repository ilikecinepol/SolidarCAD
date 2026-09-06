#include <BRepAdaptor_Surface.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopExp.hxx>
#include <TopoDS_Vertex.hxx>

#include <cstdlib>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "model/ChamferBuilder.h"
#include "model/FilletBuilder.h"
#include "model/ShellBuilder.h"
#include "model/TopologyReferenceResolver.h"

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; return EXIT_FAILURE; } } while(false)

namespace {

double volume(const TopoDS_Shape& shape) {
  GProp_GProps properties;
  BRepGProp::VolumeProperties(shape, properties);
  return properties.Mass();
}

std::size_t solidCount(const TopoDS_Shape& shape) {
  std::size_t count = 0;
  for (TopExp_Explorer solids(shape, TopAbs_SOLID); solids.More(); solids.Next())
    ++count;
  return count;
}

std::size_t topOpening(const TopoDS_Shape& shape) {
  std::size_t index = 0;
  double bestZ = -1e100;
  std::size_t best = static_cast<std::size_t>(-1);
  for (TopExp_Explorer faces(shape, TopAbs_FACE); faces.More();
       faces.Next(), ++index) {
    BRepAdaptor_Surface surface(TopoDS::Face(faces.Current()));
    if (surface.GetType() != GeomAbs_Plane ||
        std::abs(surface.Plane().Axis().Direction().Z()) < 0.99)
      continue;
    const double z = surface.Plane().Location().Z();
    if (z > bestZ) { bestZ = z; best = index; }
  }
  return best;
}

bool validShell(const TopoDS_Shape& source, double thickness, bool outside) {
  const auto opening = topOpening(source);
  if (opening == static_cast<std::size_t>(-1)) return false;
  std::string error;
  const auto result = solidar::buildShellShape(source, {opening}, thickness,
                                                outside, &error);
  if (!result || !BRepCheck_Analyzer(*result).IsValid() ||
      solidCount(*result) != 1)
    return false;
  const double sourceVolume = volume(source);
  const double shellVolume = volume(*result);
  // A real opening/cavity must remove a material-sized volume while leaving a
  // non-collapsed wall solid. This is stable across planar and blended edges.
  return shellVolume > sourceVolume * 0.01 && shellVolume < sourceVolume * 0.95;
}

std::shared_ptr<TopoDS_Shape> firstChamfer(const TopoDS_Shape& source,
                                           double distance,
                                           std::size_t start = 0) {
  for (std::size_t edge = start; edge < 96; ++edge) {
    std::string error;
    auto result = solidar::buildChamferShape(source, {edge}, distance, &error);
    if (result) return result;
    if (error.find("resolved") != std::string::npos) break;
  }
  return {};
}

std::shared_ptr<TopoDS_Shape> firstFillet(const TopoDS_Shape& source,
                                          double radius) {
  for (std::size_t edge = 0; edge < 96; ++edge) {
    std::string error;
    auto result = solidar::buildFilletShape(source, {edge}, radius, &error);
    if (result) return result;
    if (error.find("resolved") != std::string::npos) break;
  }
  return {};
}

std::shared_ptr<TopoDS_Shape> chamferThatShells(const TopoDS_Shape& source,
                                                double distance,
                                                double shellThickness) {
  for (std::size_t edge = 0; edge < 96; ++edge) {
    std::string error;
    auto result = solidar::buildChamferShape(source, {edge}, distance, &error);
    if (result && validShell(*result, shellThickness, false)) return result;
    if (!result && error.find("resolved") != std::string::npos) break;
  }
  return {};
}

bool shareVertex(const TopoDS_Shape& shape, std::size_t first,
                 std::size_t second) {
  const auto a = solidar::resolveEdge(shape, first);
  const auto b = solidar::resolveEdge(shape, second);
  if (!a || !b) return false;
  TopoDS_Vertex a1, a2, b1, b2;
  TopExp::Vertices(*a, a1, a2);
  TopExp::Vertices(*b, b1, b2);
  return a1.IsSame(b1) || a1.IsSame(b2) || a2.IsSame(b1) || a2.IsSame(b2);
}

}  // namespace

int main() {
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(60.0, 40.0, 30.0).Shape();

  // Cases 1-2: two separate edge features with different distances followed
  // by both requested wall sizes.
  const auto chamfer2 = firstChamfer(box, 2.0);
  CHECK(chamfer2);
  const auto chamfer5 = chamferThatShells(*chamfer2, 5.0, 2.0);
  CHECK(chamfer5);
  CHECK(validShell(*chamfer5, 1.0, false));
  CHECK(validShell(*chamfer5, 2.0, false));

  // Cases 3-5: adjacent, opposite and three-edge selections. Trying all
  // topology pairs/triples makes the matrix independent of OCCT enumeration.
  bool adjacentPair = false;
  bool oppositePair = false;
  bool threeDifferent = false;
  for (std::size_t a = 0; a < 12; ++a) {
    for (std::size_t b = a + 1; b < 12; ++b) {
      std::string error;
      auto pair = solidar::buildChamferShape(box, {a, b}, 2.0, &error);
      if (!pair || !validShell(*pair, 1.0, false)) continue;
      if (shareVertex(box, a, b)) adjacentPair = true;
      else oppositePair = true;
      for (std::size_t c = b + 1; c < 12; ++c) {
        auto triple = solidar::buildChamferShape(box, {a, b, c}, 1.0, &error);
        if (!triple) continue;
        auto second = firstChamfer(*triple, 2.0);
        auto third = second ? chamferThatShells(*second, 3.0, 1.0) : nullptr;
        if (third) { threeDifferent = true; break; }
      }
      if (adjacentPair && oppositePair && threeDifferent) break;
    }
    if (adjacentPair && oppositePair && threeDifferent) break;
  }
  CHECK(adjacentPair && oppositePair && threeDifferent);

  // Case 6: blended edge followed by a chamfer.
  const auto fillet = firstFillet(box, 3.0);
  CHECK(fillet);
  const auto afterFillet = chamferThatShells(*fillet, 2.0, 1.0);
  CHECK(afterFillet);
  CHECK(validShell(*afterFillet, 1.0, false));

  // Cases 7-8: the exact same upstream B-Rep is used for inward and outward.
  CHECK(validShell(*chamfer5, 1.0, false));
  CHECK(validShell(*chamfer5, 1.0, true));

  // Impossible input fails early and remains recoverable with unchanged input.
  std::string error;
  CHECK(!solidar::buildShellShape(*chamfer5, {topOpening(*chamfer5)}, 1000.0,
                                  false, &error));
  CHECK(error.find("too large") != std::string::npos);
  CHECK(validShell(*chamfer5, 1.0, false));
  return EXIT_SUCCESS;
}
