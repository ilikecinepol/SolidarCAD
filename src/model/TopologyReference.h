#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "model/Body.h"
#include "model/Feature.h"

namespace solidar {

enum class TopologyKind { Face, Edge };
enum class SurfaceKind { Unknown, Plane, Cylinder };
enum class CurveKind { Unknown, Line, Circle, Ellipse, Other };

struct TopologyPoint3d {
  double x{}, y{}, z{};
  friend bool operator==(const TopologyPoint3d&,
                         const TopologyPoint3d&) = default;
};

struct TopologyBounds {
  TopologyPoint3d minimum;
  TopologyPoint3d maximum;
  friend bool operator==(const TopologyBounds&, const TopologyBounds&) = default;
};

struct FaceSignature {
  SurfaceKind surface{SurfaceKind::Unknown};
  double area{};
  TopologyPoint3d centroid;
  TopologyPoint3d normal;
  TopologyBounds bounds;
  double radius{};
  TopologyPoint3d axis;
  friend bool operator==(const FaceSignature&, const FaceSignature&) = default;
};

struct EdgeSignature {
  CurveKind curve{CurveKind::Unknown};
  double length{};
  TopologyPoint3d midpoint;
  TopologyPoint3d tangent;
  TopologyBounds bounds;
  double radius{};
  TopologyPoint3d center;
  friend bool operator==(const EdgeSignature&, const EdgeSignature&) = default;
};

// Stable owner identity, persistent geometric data, and an explicitly marked
// legacy index fallback for projects created before topology naming v1.
struct TopologyReference {
  BodyId bodyId{kInvalidBodyId};
  FeatureId featureId{kInvalidFeatureId};
  TopologyKind kind{TopologyKind::Face};
  std::size_t legacyIndex{};
  std::string persistentTag;
  std::optional<FaceSignature> faceSignature;
  std::optional<EdgeSignature> edgeSignature;

  [[nodiscard]] bool hasValidOwner() const noexcept {
    return bodyId != kInvalidBodyId && featureId != kInvalidFeatureId;
  }

  friend bool operator==(const TopologyReference&,
                         const TopologyReference&) = default;
};

}  // namespace solidar
