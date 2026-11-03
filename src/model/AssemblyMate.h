#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <variant>

#include "model/FaceReference.h"
#include "model/EdgeReference.h"
#include "model/ProductDocument.h"

namespace solidar {

class MateId final {
 public:
  constexpr MateId() noexcept = default;
  explicit constexpr MateId(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept {
    return value_;
  }
  [[nodiscard]] constexpr bool isValid() const noexcept { return value_ != 0; }

  friend constexpr bool operator==(MateId, MateId) noexcept = default;
  friend constexpr auto operator<=>(MateId, MateId) noexcept = default;

 private:
  std::uint64_t value_{};
};

inline constexpr MateId kInvalidMateId{};
inline constexpr std::size_t kMaximumAssemblyMates = 128;
// RigidTransform3d canonicalizes quaternion components on a 2^-48 grid.  A
// 64-quantum angular floor covers the rotation-matrix/residual amplification
// and the canonicalizer's fixed-point margin while remaining far below the
// authored default (1e-9 rad).
inline constexpr double kMinimumMateAngularToleranceRad = 0x1p-42;

// Authored assembly semantics.  These limits are intentionally part of the
// model layer: persistence and every solver entry point share one validation
// boundary and a caller cannot substitute transient solve policy.
struct MateSolverOptions final {
  double linearToleranceMm{1.0e-7};
  double angularToleranceRad{1.0e-9};
  double rankTolerance{1.0e-8};
  double maximumAbsoluteOffsetMm{1.0e9};
  double maximumTranslationStepMm{1.0e6};
  double maximumAngularStepRad{3.14159265358979323846};
  std::size_t maximumIterations{64};
  std::size_t maximumNodes{256};
  std::size_t maximumMates{kMaximumAssemblyMates};
  std::uint64_t maximumWorkUnits{1'000'000'000};

  [[nodiscard]] bool isValid() const noexcept;
  friend bool operator==(const MateSolverOptions&,
                         const MateSolverOptions&) = default;
};

enum class PlanarMateOrientation : std::uint8_t { Aligned, Opposed };

[[nodiscard]] constexpr bool isValidPlanarMateOrientation(
    PlanarMateOrientation orientation) noexcept {
  return orientation == PlanarMateOrientation::Aligned ||
         orientation == PlanarMateOrientation::Opposed;
}

struct MateEndpoint final {
  OccurrencePath occurrencePath;
  FaceReference face;

  friend bool operator==(const MateEndpoint&, const MateEndpoint&) = default;
};

enum class LocalDatumAxis : std::uint8_t { X, Y, Z };

[[nodiscard]] constexpr bool isValidLocalDatumAxis(
    LocalDatumAxis axis) noexcept {
  return axis == LocalDatumAxis::X || axis == LocalDatumAxis::Y ||
         axis == LocalDatumAxis::Z;
}

struct CylindricalFaceAxis final {
  FaceReference face;

  friend bool operator==(const CylindricalFaceAxis&,
                         const CylindricalFaceAxis&) = default;
};

using MateAxisReference = std::variant<LocalDatumAxis, CylindricalFaceAxis>;

struct MateAxisEndpoint final {
  OccurrencePath occurrencePath;
  MateAxisReference reference;

  friend bool operator==(const MateAxisEndpoint&,
                         const MateAxisEndpoint&) = default;
};

enum class LocalDatumPoint : std::uint8_t { Origin };

[[nodiscard]] constexpr bool isValidLocalDatumPoint(
    LocalDatumPoint point) noexcept {
  return point == LocalDatumPoint::Origin;
}

struct CircularEdgeCenter final {
  EdgeReference edge;

  friend bool operator==(const CircularEdgeCenter&,
                         const CircularEdgeCenter&) = default;
};

using MatePointReference =
    std::variant<LocalDatumPoint, CircularEdgeCenter>;

struct MatePointEndpoint final {
  OccurrencePath occurrencePath;
  MatePointReference reference;

  friend bool operator==(const MatePointEndpoint&,
                         const MatePointEndpoint&) = default;
};

class PlanarCoincidentMate final {
 public:
  PlanarCoincidentMate(MateId id, MateEndpoint first, MateEndpoint second,
                       PlanarMateOrientation orientation,
                       double offsetMm);

  [[nodiscard]] MateId id() const noexcept { return id_; }
  [[nodiscard]] const MateEndpoint& first() const noexcept { return first_; }
  [[nodiscard]] const MateEndpoint& second() const noexcept { return second_; }
  [[nodiscard]] PlanarMateOrientation orientation() const noexcept {
    return orientation_;
  }
  [[nodiscard]] double offsetMm() const noexcept { return offsetMm_; }
  [[nodiscard]] bool isValid(double maximumAbsoluteOffsetMm) const noexcept;
  friend bool operator==(const PlanarCoincidentMate&,
                         const PlanarCoincidentMate&) = default;

 private:
  MateId id_;
  MateEndpoint first_;
  MateEndpoint second_;
  PlanarMateOrientation orientation_{PlanarMateOrientation::Aligned};
  double offsetMm_{};
};

class PlanarParallelMate final {
 public:
  PlanarParallelMate(MateId id, MateEndpoint first, MateEndpoint second,
                     PlanarMateOrientation orientation);

  [[nodiscard]] MateId id() const noexcept { return id_; }
  [[nodiscard]] const MateEndpoint& first() const noexcept { return first_; }
  [[nodiscard]] const MateEndpoint& second() const noexcept { return second_; }
  [[nodiscard]] PlanarMateOrientation orientation() const noexcept {
    return orientation_;
  }
  [[nodiscard]] bool isValid() const noexcept;
  friend bool operator==(const PlanarParallelMate&,
                         const PlanarParallelMate&) = default;

 private:
  MateId id_;
  MateEndpoint first_;
  MateEndpoint second_;
  PlanarMateOrientation orientation_{PlanarMateOrientation::Aligned};
};

class PlanarPerpendicularMate final {
 public:
  PlanarPerpendicularMate(MateId id, MateEndpoint first, MateEndpoint second);

  [[nodiscard]] MateId id() const noexcept { return id_; }
  [[nodiscard]] const MateEndpoint& first() const noexcept { return first_; }
  [[nodiscard]] const MateEndpoint& second() const noexcept { return second_; }
  [[nodiscard]] bool isValid() const noexcept;
  friend bool operator==(const PlanarPerpendicularMate&,
                         const PlanarPerpendicularMate&) = default;

 private:
  MateId id_;
  MateEndpoint first_;
  MateEndpoint second_;
};

class AxisConcentricMate final {
 public:
  AxisConcentricMate(MateId id, MateAxisEndpoint first,
                     MateAxisEndpoint second);

  [[nodiscard]] MateId id() const noexcept { return id_; }
  [[nodiscard]] const MateAxisEndpoint& first() const noexcept {
    return first_;
  }
  [[nodiscard]] const MateAxisEndpoint& second() const noexcept {
    return second_;
  }
  [[nodiscard]] bool isValid() const noexcept;
  friend bool operator==(const AxisConcentricMate&,
                         const AxisConcentricMate&) = default;

 private:
  MateId id_;
  MateAxisEndpoint first_;
  MateAxisEndpoint second_;
};

class PointDistanceMate final {
 public:
  PointDistanceMate(MateId id, MatePointEndpoint first,
                    MatePointEndpoint second, double distanceMm);

  [[nodiscard]] MateId id() const noexcept { return id_; }
  [[nodiscard]] const MatePointEndpoint& first() const noexcept {
    return first_;
  }
  [[nodiscard]] const MatePointEndpoint& second() const noexcept {
    return second_;
  }
  [[nodiscard]] double distanceMm() const noexcept { return distanceMm_; }
  [[nodiscard]] bool isValid(double maximumAbsoluteOffsetMm) const noexcept;
  friend bool operator==(const PointDistanceMate&,
                         const PointDistanceMate&) = default;

 private:
  MateId id_;
  MatePointEndpoint first_;
  MatePointEndpoint second_;
  double distanceMm_{};
};

class PlanarAngleMate final {
 public:
  PlanarAngleMate(MateId id, MateEndpoint first, MateEndpoint second,
                  PlanarMateOrientation orientation, double angleRad);

  [[nodiscard]] MateId id() const noexcept { return id_; }
  [[nodiscard]] const MateEndpoint& first() const noexcept { return first_; }
  [[nodiscard]] const MateEndpoint& second() const noexcept { return second_; }
  [[nodiscard]] PlanarMateOrientation orientation() const noexcept {
    return orientation_;
  }
  [[nodiscard]] double angleRad() const noexcept { return angleRad_; }
  [[nodiscard]] bool isStructurallyValid() const noexcept;
  [[nodiscard]] bool isValid(double angularToleranceRad) const noexcept;
  friend bool operator==(const PlanarAngleMate&,
                         const PlanarAngleMate&) = default;

 private:
  MateId id_;
  MateEndpoint first_;
  MateEndpoint second_;
  PlanarMateOrientation orientation_{PlanarMateOrientation::Aligned};
  double angleRad_{};
};

enum class AssemblyMateKind : std::uint8_t {
  PlanarCoincident,
  PlanarParallel,
  PlanarPerpendicular,
  AxisConcentric,
  PointDistance,
  PlanarAngle,
};

using AssemblyMate = std::variant<PlanarCoincidentMate, PlanarParallelMate,
                                  PlanarPerpendicularMate, AxisConcentricMate,
                                  PointDistanceMate, PlanarAngleMate>;

using AssemblyMateEndpointView =
    std::variant<std::reference_wrapper<const MateEndpoint>,
                 std::reference_wrapper<const MateAxisEndpoint>,
                 std::reference_wrapper<const MatePointEndpoint>>;

[[nodiscard]] MateId assemblyMateId(const AssemblyMate& mate) noexcept;
[[nodiscard]] AssemblyMateEndpointView assemblyMateFirstEndpoint(
    const AssemblyMate& mate) noexcept;
[[nodiscard]] AssemblyMateEndpointView assemblyMateSecondEndpoint(
    const AssemblyMate& mate) noexcept;
[[nodiscard]] const MateEndpoint* assemblyPlanarMateFirst(
    const AssemblyMate& mate) noexcept;
[[nodiscard]] const MateEndpoint* assemblyPlanarMateSecond(
    const AssemblyMate& mate) noexcept;
[[nodiscard]] const OccurrencePath& assemblyMateFirstOccurrencePath(
    const AssemblyMate& mate) noexcept;
[[nodiscard]] const OccurrencePath& assemblyMateSecondOccurrencePath(
    const AssemblyMate& mate) noexcept;
[[nodiscard]] AssemblyMateKind assemblyMateKind(
    const AssemblyMate& mate) noexcept;
[[nodiscard]] bool isValidAssemblyMate(
    const AssemblyMate& mate, double maximumAbsoluteOffsetMm,
    double angularToleranceRad = 1.0e-9) noexcept;

enum class MateRegistryMutationStatus : std::uint8_t {
  Inserted,
  Erased,
  Invalid,
  Duplicate,
  Missing,
  BudgetExceeded,
  RevisionExhausted,
  ResourceFailure,
  Unchanged,
};

class MateRegistry final {
 public:
  [[nodiscard]] MateRegistryMutationStatus insert(
      PlanarCoincidentMate relation) noexcept;
  [[nodiscard]] MateRegistryMutationStatus insert(
      PlanarParallelMate relation) noexcept;
  [[nodiscard]] MateRegistryMutationStatus insert(
      PlanarPerpendicularMate relation) noexcept;
  [[nodiscard]] MateRegistryMutationStatus insert(
      AxisConcentricMate relation) noexcept;
  [[nodiscard]] MateRegistryMutationStatus insert(
      PointDistanceMate relation) noexcept;
  [[nodiscard]] MateRegistryMutationStatus insert(
      PlanarAngleMate relation) noexcept;
  [[nodiscard]] MateRegistryMutationStatus insert(AssemblyMate relation) noexcept;
  [[nodiscard]] MateRegistryMutationStatus erase(MateId id) noexcept;
  [[nodiscard]] MateRegistryMutationStatus setOptions(
      const MateSolverOptions& options) noexcept;
  [[nodiscard]] const PlanarCoincidentMate* find(MateId id) const noexcept;
  [[nodiscard]] const AssemblyMate* findMate(MateId id) const noexcept;
  [[nodiscard]] const PlanarParallelMate* findParallel(MateId id) const noexcept;
  [[nodiscard]] const PlanarPerpendicularMate* findPerpendicular(
      MateId id) const noexcept;
  [[nodiscard]] const AxisConcentricMate* findConcentric(
      MateId id) const noexcept;
  [[nodiscard]] const PointDistanceMate* findDistance(
      MateId id) const noexcept;
  [[nodiscard]] const PlanarAngleMate* findAngle(MateId id) const noexcept;
  [[nodiscard]] const MateSolverOptions& options() const noexcept {
    return options_;
  }
  [[nodiscard]] std::optional<MateId> lowestFreeId() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return relations_.size(); }
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
  [[nodiscard]] std::uint64_t fingerprint() const noexcept;
  [[nodiscard]] const std::map<MateId, AssemblyMate>& relations()
      const noexcept {
    return relations_;
  }
  friend bool operator==(const MateRegistry&, const MateRegistry&) = default;

 private:
  [[nodiscard]] bool advanceRevision() noexcept;

  std::map<MateId, AssemblyMate> relations_;
  MateSolverOptions options_;
  std::uint64_t revision_{1};
};

}  // namespace solidar
