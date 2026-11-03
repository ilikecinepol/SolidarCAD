#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "model/AssemblyMate.h"

namespace solidar {

struct MateSolveResult;
struct PlanarMateAuthoringPrepareResult;

enum class MateSolveStatus : std::uint8_t {
  Solved,
  Underconstrained,
  Redundant,
  Overconstrained,
  Ambiguous,
  Missing,
  Broken,
  InvalidInput,
  NonConvergent,
  ResourceFailure,
  Stale,
  Busy,
};

enum class MateEndpointFailure : std::uint8_t {
  None,
  V1Unsupported,
  InvalidOwner,
  PathMissing,
  TargetIsNotPart,
  Suppressed,
  BodyMissing,
  FeatureMissing,
  TopologyUnavailable,
  TopologyEvaluationFailed,
  NonAuthoritativeTopology,
  Missing,
  Ambiguous,
  NonPlanar,
  NonCylindrical,
  NonCircular,
  InvalidDatumAxis,
  InvalidDatumPoint,
  Degenerate,
  DegenerateDirection,
  Unexpected,
  InvalidFaceOrientation,
};

struct MateSolveRequest final {
  std::optional<MateId> seedMateId;
  std::optional<OccurrenceId> seedImmediateOccurrenceId;

  [[nodiscard]] bool isValid() const noexcept {
    const bool validMate = seedMateId && seedMateId->isValid();
    const bool validOccurrence =
        seedImmediateOccurrenceId && seedImmediateOccurrenceId->isValid();
    return validMate != validOccurrence;
  }
};

struct MateResidual final {
  MateId mateId;
  double linearMm{};
  double angularRad{};
};

struct MateEndpointDiagnostic final {
  MateId mateId;
  bool secondEndpoint{};
  MateEndpointFailure failure{MateEndpointFailure::None};
};

struct PlanarMateEndpointClassification final {
  MateSolveStatus status{MateSolveStatus::InvalidInput};
  MateEndpointFailure failure{MateEndpointFailure::Unexpected};
  DefinitionId terminalDefinitionId;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == MateSolveStatus::Solved &&
           failure == MateEndpointFailure::None;
  }
};

struct MateRelationDiagnostic final {
  MateId mateId;
  MateEndpointFailure failure{MateEndpointFailure::None};
};

struct MateSolveDiagnostics final {
  std::size_t rank{};
  std::size_t totalDegreesOfFreedom{};
  std::size_t remainingDegreesOfFreedom{};
  double maximumLinearResidualMm{};
  double maximumAngularResidualRad{};
  std::vector<MateResidual> perMateResiduals;
  std::vector<MateId> redundantMateIds;
  std::vector<MateId> conflictMateIds;
  std::optional<MateEndpointDiagnostic> endpointFailure;
  std::optional<MateRelationDiagnostic> relationFailure;
  std::uint64_t requiredWorkUnits{};
};

struct MateOccurrenceSnapshot final {
  OccurrenceId occurrenceId;
  DefinitionId targetDefinitionId;
  RigidTransform3d placement{RigidTransform3d::identity()};
  OccurrenceFlags flags;

  friend bool operator==(const MateOccurrenceSnapshot&,
                         const MateOccurrenceSnapshot&) = default;
};

struct MatePlacementChange final {
  MateOccurrenceSnapshot before;
  RigidTransform3d afterPlacement{RigidTransform3d::identity()};

  friend bool operator==(const MatePlacementChange&,
                         const MatePlacementChange&) = default;
};

struct MatePlacementCandidateAddress final {
  ProductIncarnationId productIncarnation;
  DefinitionId rootDefinitionId;
  DefinitionId ownerAssemblyId;
  std::uint64_t mateRegistryRevision{};
  std::uint64_t mateRegistryFingerprint{};
};

struct MateOccurrenceDependencySeal final {
  DefinitionId ownerAssemblyId;
  OccurrenceId occurrenceId;
  DefinitionId targetDefinitionId;
  RigidTransform3d placement{RigidTransform3d::identity()};
  OccurrenceFlags flags;
  std::string displayNameUtf8;

  friend bool operator==(const MateOccurrenceDependencySeal&,
                         const MateOccurrenceDependencySeal&) = default;
};

struct MateTopologyDependencySeal final {
  BodyId bodyId;
  FeatureId featureId;
  ShapeRevision shapeRevision;
  bool topologyAvailable{};
  bool v2Authoritative{};

  friend bool operator==(const MateTopologyDependencySeal&,
                         const MateTopologyDependencySeal&) = default;
};

struct MateDefinitionDependencySeal final {
  DefinitionId definitionId;
  DefinitionStateId stateId;
  std::optional<MateTopologyDependencySeal> topology;

  friend bool operator==(const MateDefinitionDependencySeal&,
                         const MateDefinitionDependencySeal&) = default;
};

class MatePlacementCandidate final {
 public:
  [[nodiscard]] const MatePlacementCandidateAddress& address() const noexcept {
    return address_;
  }
  [[nodiscard]] const MateSolveRequest& seed() const noexcept { return seed_; }
  [[nodiscard]] const std::vector<MateId>& componentMateIds() const noexcept {
    return componentMateIds_;
  }
  [[nodiscard]] const std::vector<MatePlacementChange>& changes() const noexcept {
    return changes_;
  }
  [[nodiscard]] const std::vector<MateOccurrenceDependencySeal>&
  occurrenceDependencies() const noexcept {
    return occurrenceDependencies_;
  }
  [[nodiscard]] const std::vector<MateDefinitionDependencySeal>&
  definitionDependencies() const noexcept {
    return definitionDependencies_;
  }

 private:
  friend class ProductDocument;
  friend class PlanarMateAuthoringTransaction;
  friend MateSolveResult solveAssemblyMates(
      const ProductDocument&, DefinitionId, const MateSolveRequest&,
      const MateSolverOptions&) noexcept;
  friend MateSolveResult solvePlanarMates(const ProductDocument&, DefinitionId,
                                          const MateSolveRequest&,
                                          const MateSolverOptions&) noexcept;
  friend PlanarMateAuthoringPrepareResult preparePlanarMateAuthoring(
      const ProductDocument&, DefinitionId, PlanarCoincidentMate) noexcept;

  MatePlacementCandidate(MatePlacementCandidateAddress address,
                         MateSolveRequest seed,
                         std::vector<MateId> componentMateIds,
                         std::vector<MatePlacementChange> changes,
                         std::vector<MateOccurrenceDependencySeal>
                             occurrenceDependencies,
                         std::vector<MateDefinitionDependencySeal>
                             definitionDependencies) noexcept;
  [[nodiscard]] std::uint64_t computeIntegrityFingerprint() const noexcept;
  [[nodiscard]] bool integrityValid() const noexcept;

  MatePlacementCandidateAddress address_;
  MateSolveRequest seed_;
  std::vector<MateId> componentMateIds_;
  std::vector<MatePlacementChange> changes_;
  std::vector<MateOccurrenceDependencySeal> occurrenceDependencies_;
  std::vector<MateDefinitionDependencySeal> definitionDependencies_;
  std::uint64_t integrityFingerprint_{};
};

// Fully cold, model-owned authoring transaction prepared without mutating the
// live ProductDocument. Registry snapshots contain authored values only; the
// sealed candidate contains stable dependency values and no OCCT subshapes.
class PlanarMateAuthoringTransaction final {
 public:
  PlanarMateAuthoringTransaction(const PlanarMateAuthoringTransaction&) =
      default;
  PlanarMateAuthoringTransaction(PlanarMateAuthoringTransaction&&) noexcept =
      default;
  PlanarMateAuthoringTransaction& operator=(
      const PlanarMateAuthoringTransaction&) = default;
  PlanarMateAuthoringTransaction& operator=(
      PlanarMateAuthoringTransaction&&) noexcept = default;
  [[nodiscard]] DefinitionId ownerAssemblyId() const noexcept {
    return ownerAssemblyId_;
  }
  [[nodiscard]] const PlanarCoincidentMate& relation() const noexcept {
    return relation_;
  }
  [[nodiscard]] const MateRegistry& beforeRegistry() const noexcept {
    return beforeRegistry_;
  }
  [[nodiscard]] const MateRegistry& afterRegistry() const noexcept {
    return afterRegistry_;
  }
  [[nodiscard]] const MatePlacementCandidate& candidate() const noexcept {
    return candidate_;
  }
  [[nodiscard]] bool integrityValid() const noexcept;

 private:
  friend class ProductDocument;
  friend PlanarMateAuthoringPrepareResult preparePlanarMateAuthoring(
      const ProductDocument&, DefinitionId, PlanarCoincidentMate) noexcept;

  PlanarMateAuthoringTransaction(
      DefinitionId ownerAssemblyId, PlanarCoincidentMate relation,
      MateRegistry beforeRegistry, MateRegistry afterRegistry,
      MatePlacementCandidate candidate) noexcept;
  [[nodiscard]] std::uint64_t computeIntegrityFingerprint() const noexcept;

  DefinitionId ownerAssemblyId_;
  PlanarCoincidentMate relation_;
  MateRegistry beforeRegistry_;
  MateRegistry afterRegistry_;
  MatePlacementCandidate candidate_;
  std::uint64_t integrityFingerprint_{};
};

struct PlanarMateAuthoringPrepareResult final {
  MateSolveStatus status{MateSolveStatus::InvalidInput};
  MateSolveDiagnostics diagnostics;
  std::optional<PlanarMateAuthoringTransaction> transaction;

  [[nodiscard]] explicit operator bool() const noexcept {
    return transaction.has_value();
  }
};

struct MateSolveResult final {
  MateSolveStatus status{MateSolveStatus::InvalidInput};
  std::vector<MateId> componentMateIds;
  MateSolveDiagnostics diagnostics;
  std::optional<MatePlacementCandidate> candidate;
};

[[nodiscard]] MateSolveResult solveAssemblyMates(
    const ProductDocument& product, DefinitionId ownerAssemblyId,
    const MateSolveRequest& request,
    const MateSolverOptions& options) noexcept;

[[nodiscard]] MateSolveResult solveAssemblyMates(
    const ProductDocument& product, DefinitionId ownerAssemblyId,
    const MateSolveRequest& request) noexcept;

[[nodiscard]] MateSolveResult solveAssemblyMates(
    const ProductDocument& product, DefinitionId ownerAssemblyId,
    const MateSolverOptions& options) noexcept;

[[nodiscard]] MateSolveResult solveAssemblyMates(
    const ProductDocument& product, DefinitionId ownerAssemblyId) noexcept;

// Prospective root/tool authoring solve. The proposed relation is evaluated
// against a staged registry overlay; the live registry, placements, revision
// and fingerprint remain unobservable and byte-exact.
[[nodiscard]] PlanarMateAuthoringPrepareResult preparePlanarMateAuthoring(
    const ProductDocument& product, DefinitionId ownerAssemblyId,
    PlanarCoincidentMate relation) noexcept;

// Shared UI/model boundary for typed mate picks. It exercises the same V2
// path, topology-authority and planar resolver as the solver and returns cold
// IDs only; no OCCT subshape escapes the call.
[[nodiscard]] PlanarMateEndpointClassification classifyPlanarMateEndpoint(
    const ProductDocument& product, DefinitionId ownerAssemblyId,
    const MateEndpoint& endpoint) noexcept;

[[nodiscard]] MateSolveResult solvePlanarMates(
    const ProductDocument& product, DefinitionId ownerAssemblyId,
    const MateSolveRequest& request,
    const MateSolverOptions& options) noexcept;

[[nodiscard]] MateSolveResult solvePlanarMates(
    const ProductDocument& product, DefinitionId ownerAssemblyId,
    const MateSolveRequest& request) noexcept;

// Compatibility entry point: deterministically seeds the lowest MateId.
[[nodiscard]] MateSolveResult solvePlanarMates(
    const ProductDocument& product, DefinitionId ownerAssemblyId,
    const MateSolverOptions& options) noexcept;

[[nodiscard]] MateSolveResult solvePlanarMates(
    const ProductDocument& product, DefinitionId ownerAssemblyId) noexcept;

enum class MatePlacementApplyStatus : std::uint8_t {
  Applied,
  Busy,
  InvalidCandidate,
  Stale,
  SuccessorRejected,
  ResourceFailure,
  Failed,
};

}  // namespace solidar
