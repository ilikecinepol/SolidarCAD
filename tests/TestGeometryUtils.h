#pragma once

#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>

#include <cmath>
#include <cstddef>
#include <optional>

#include "model/SketchPlacement.h"

namespace solidar::test {

struct Bounds {
  double minX{}, minY{}, minZ{}, maxX{}, maxY{}, maxZ{};
  [[nodiscard]] double x() const noexcept { return maxX - minX; }
  [[nodiscard]] double y() const noexcept { return maxY - minY; }
  [[nodiscard]] double z() const noexcept { return maxZ - minZ; }
};

inline bool near(double actual, double expected, double tolerance = 1e-6) {
  return std::abs(actual - expected) <= tolerance;
}

inline Bounds boundsOf(const TopoDS_Shape& shape) {
  Bnd_Box box;
  BRepBndLib::Add(shape, box);
  Bounds result;
  box.Get(result.minX, result.minY, result.minZ, result.maxX, result.maxY,
          result.maxZ);
  return result;
}

inline double volumeOf(const TopoDS_Shape& shape) {
  GProp_GProps properties;
  BRepGProp::VolumeProperties(shape, properties);
  return properties.Mass();
}

inline std::size_t solidCount(const TopoDS_Shape& shape) {
  std::size_t count = 0;
  for (TopExp_Explorer explorer(shape, TopAbs_SOLID); explorer.More();
       explorer.Next())
    ++count;
  return count;
}

inline std::optional<std::size_t> topPlanarFace(const TopoDS_Shape& shape,
                                                double expectedZ) {
  std::size_t index = 0;
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next(), ++index) {
    const auto resolved = resolveFacePlacement(shape, index);
    if (resolved.planar && resolved.placement.normal().z > 0.9 &&
        near(resolved.placement.origin.z, expectedZ))
      return index;
  }
  return std::nullopt;
}

}  // namespace solidar::test
