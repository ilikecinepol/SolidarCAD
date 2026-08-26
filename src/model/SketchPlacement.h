#pragma once

#include <cmath>
#include <cstddef>

#include "model/Feature.h"
#include "model/Body.h"

class TopoDS_Shape;

namespace solidar {

struct Point3d {
  double x{};
  double y{};
  double z{};
};

struct Vector3d {
  double x{};
  double y{};
  double z{};
};

struct SketchPlacement {
  Point3d origin{};
  Vector3d xDirection{1.0, 0.0, 0.0};
  Vector3d yDirection{0.0, 1.0, 0.0};

  [[nodiscard]] static SketchPlacement xy() noexcept;
  [[nodiscard]] static SketchPlacement xz() noexcept;
  [[nodiscard]] static SketchPlacement yz() noexcept;
  [[nodiscard]] Point3d toWorld(double xMm, double yMm) const noexcept;
  [[nodiscard]] Vector3d normal() const noexcept;
};

struct FaceReference {
  BodyId bodyId{kInvalidBodyId};
  FeatureId featureId{kInvalidFeatureId};
  // Temporary until persistent topological naming is introduced.
  std::size_t faceIndex{};
};

struct EdgeReference {
  BodyId bodyId{kInvalidBodyId};
  FeatureId featureId{kInvalidFeatureId};
  // Temporary until persistent topological naming is introduced.
  std::size_t edgeIndex{};
};

enum class SketchSupportType { BasePlane, Face };

struct SketchSupport {
  SketchSupportType type{SketchSupportType::BasePlane};
  FaceReference face{};
};

struct ResolvedFacePlacement {
  SketchPlacement placement;
  bool planar{false};
};

[[nodiscard]] ResolvedFacePlacement resolveFacePlacement(
    const TopoDS_Shape& shape, std::size_t faceIndex);

}  // namespace solidar
