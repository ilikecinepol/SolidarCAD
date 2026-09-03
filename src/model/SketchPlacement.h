#pragma once

#include <cmath>
#include <cstddef>

#include "model/TopologyReference.h"

class TopoDS_Shape;

namespace solidar {

struct Point3d {
  double x{};
  double y{};
  double z{};
};

struct Point2d {
  double x{};
  double y{};
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
  [[nodiscard]] Point2d toLocal(Point3d world) const noexcept;
  [[nodiscard]] Vector3d normal() const noexcept;
};

struct FaceReference {
  BodyId bodyId{kInvalidBodyId};
  FeatureId featureId{kInvalidFeatureId};
  // Temporary until persistent topological naming is introduced.
  std::size_t faceIndex{};
  std::string persistentTag;
  std::optional<FaceSignature> signature;

  [[nodiscard]] TopologyReference topology() const {
    TopologyReference result;
    result.bodyId = bodyId;
    result.featureId = featureId;
    result.kind = TopologyKind::Face;
    result.legacyIndex = faceIndex;
    result.persistentTag = persistentTag;
    result.faceSignature = signature;
    return result;
  }
};

struct EdgeReference {
  BodyId bodyId{kInvalidBodyId};
  FeatureId featureId{kInvalidFeatureId};
  // Temporary until persistent topological naming is introduced.
  std::size_t edgeIndex{};
  std::string persistentTag;
  std::optional<EdgeSignature> signature;
  [[nodiscard]] TopologyReference topology() const {
    TopologyReference result;
    result.bodyId = bodyId;
    result.featureId = featureId;
    result.kind = TopologyKind::Edge;
    result.legacyIndex = edgeIndex;
    result.persistentTag = persistentTag;
    result.edgeSignature = signature;
    return result;
  }
  friend bool operator==(const EdgeReference&, const EdgeReference&) = default;
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
[[nodiscard]] ResolvedFacePlacement resolveFacePlacement(
    const TopoDS_Shape& shape, const TopologyReference& reference);

}  // namespace solidar
