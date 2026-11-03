#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "model/AssemblyGraphAnalysis.h"
#include "model/ModelIds.h"
#include "model/ProductDocument.h"
#include "model/RigidTransform3d.h"

class TopoDS_Shape;

namespace solidar {

class TopologyLineage;

// A presentation epoch is supplied by the owner of a ProductDocument.  The
// pure builder never invents or advances it.  Keeping the epoch in every cache
// and occurrence identity prevents local model IDs from aliasing across
// independently owned products.
class ProductPresentationEpoch final {
 public:
  constexpr ProductPresentationEpoch() noexcept = default;
  explicit constexpr ProductPresentationEpoch(std::uint64_t value) noexcept
      : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept {
    return value_;
  }
  [[nodiscard]] constexpr bool isValid() const noexcept { return value_ != 0; }

  friend constexpr bool operator==(ProductPresentationEpoch,
                                   ProductPresentationEpoch) noexcept =
      default;

 private:
  std::uint64_t value_{};
};

inline constexpr ProductPresentationEpoch kInvalidProductPresentationEpoch{};

enum class AssemblyMeshQuality : std::uint8_t {
  Invalid = 0,
  Normal = 1,
  High = 2,
};

class AssemblyMeshProfile final {
 public:
  constexpr AssemblyMeshProfile() noexcept = default;
  explicit constexpr AssemblyMeshProfile(AssemblyMeshQuality quality) noexcept
      : quality_(quality) {}

  [[nodiscard]] constexpr AssemblyMeshQuality quality() const noexcept {
    return quality_;
  }
  [[nodiscard]] constexpr bool isValid() const noexcept {
    return quality_ == AssemblyMeshQuality::Normal ||
           quality_ == AssemblyMeshQuality::High;
  }

  friend constexpr bool operator==(AssemblyMeshProfile,
                                   AssemblyMeshProfile) noexcept = default;

 private:
  AssemblyMeshQuality quality_{AssemblyMeshQuality::Invalid};
};

// Address-free projection used only for semantic comparison.  The exact mesh
// source key below deliberately also includes shapeIdentity so renderer caches
// cannot reuse a different B-Rep that happens to have the same local IDs.
struct AssemblyMeshSourceSemanticKey {
  ProductPresentationEpoch epoch;
  DefinitionId definitionId;
  BodyId bodyId{kInvalidBodyId};
  FeatureId featureId{kInvalidFeatureId};
  ShapeRevision shapeRevision{kInvalidShapeRevision};
  AssemblyMeshProfile meshProfile;

  friend bool operator==(const AssemblyMeshSourceSemanticKey&,
                         const AssemblyMeshSourceSemanticKey&) = default;
};

struct AssemblyMeshSourceKey {
  ProductPresentationEpoch epoch;
  DefinitionId definitionId;
  BodyId bodyId{kInvalidBodyId};
  FeatureId featureId{kInvalidFeatureId};
  ShapeRevision shapeRevision{kInvalidShapeRevision};
  const TopoDS_Shape* shapeIdentity{};
  AssemblyMeshProfile meshProfile;

  [[nodiscard]] AssemblyMeshSourceSemanticKey semanticKey() const noexcept {
    return {epoch, definitionId, bodyId, featureId, shapeRevision,
            meshProfile};
  }

  friend bool operator==(const AssemblyMeshSourceKey&,
                         const AssemblyMeshSourceKey&) = default;
};

struct AssemblyOccurrenceIdentity {
  ProductPresentationEpoch epoch;
  DefinitionId rootAssemblyDefinitionId;
  OccurrencePath path;

  friend bool operator==(const AssemblyOccurrenceIdentity&,
                         const AssemblyOccurrenceIdentity&) = default;
};

class AssemblyPresentationSource final {
 public:
  [[nodiscard]] const AssemblyMeshSourceKey& key() const noexcept {
    return key_;
  }
  [[nodiscard]] const std::shared_ptr<const TopoDS_Shape>& shape()
      const noexcept {
    return shape_;
  }
  // Immutable last-valid provenance captured together with the retained
  // shape.  Reading this value never asks ShapeFeature to lazily build a
  // TopologyIndex.
  [[nodiscard]] const std::shared_ptr<const TopologyLineage>& topologyLineage()
      const noexcept {
    return topologyLineage_;
  }

 private:
  friend class AssemblyPresentationBuilder;

  AssemblyPresentationSource(AssemblyMeshSourceKey key,
                             std::shared_ptr<const TopoDS_Shape> shape,
                             std::shared_ptr<const TopologyLineage>
                                 topologyLineage) noexcept
      : key_(std::move(key)),
        shape_(std::move(shape)),
        topologyLineage_(std::move(topologyLineage)) {}

  AssemblyMeshSourceKey key_;
  std::shared_ptr<const TopoDS_Shape> shape_;
  std::shared_ptr<const TopologyLineage> topologyLineage_;
};

class AssemblyPresentationInstance final {
 public:
  [[nodiscard]] const AssemblyOccurrenceIdentity& occurrenceIdentity()
      const noexcept {
    return occurrenceIdentity_;
  }
  [[nodiscard]] const OccurrencePath& occurrencePath() const noexcept {
    return occurrenceIdentity_.path;
  }
  [[nodiscard]] const AssemblyMeshSourceKey& sourceKey() const noexcept {
    return sourceKey_;
  }
  [[nodiscard]] const RigidTransform3d& worldTransform() const noexcept {
    return worldTransform_;
  }
  [[nodiscard]] bool occurrenceVisible() const noexcept {
    return occurrenceVisible_;
  }
  [[nodiscard]] bool suppressed() const noexcept { return suppressed_; }
  [[nodiscard]] bool grounded() const noexcept { return grounded_; }
  [[nodiscard]] bool bodyVisible() const noexcept { return bodyVisible_; }
  [[nodiscard]] bool effectivelyVisible() const noexcept {
    return effectivelyVisible_;
  }

 private:
  friend class AssemblyPresentationBuilder;

  AssemblyPresentationInstance(AssemblyOccurrenceIdentity occurrenceIdentity,
                               AssemblyMeshSourceKey sourceKey,
                               RigidTransform3d worldTransform,
                               bool occurrenceVisible, bool suppressed,
                               bool grounded, bool bodyVisible) noexcept
      : occurrenceIdentity_(std::move(occurrenceIdentity)),
        sourceKey_(std::move(sourceKey)),
        worldTransform_(std::move(worldTransform)),
        occurrenceVisible_(occurrenceVisible),
        suppressed_(suppressed),
        grounded_(grounded),
        bodyVisible_(bodyVisible),
        effectivelyVisible_(occurrenceVisible && !suppressed && bodyVisible) {}

  AssemblyOccurrenceIdentity occurrenceIdentity_;
  AssemblyMeshSourceKey sourceKey_;
  RigidTransform3d worldTransform_;
  bool occurrenceVisible_{};
  bool suppressed_{};
  bool grounded_{};
  bool bodyVisible_{};
  bool effectivelyVisible_{};
};

class AssemblyPresentationSnapshot final {
 public:
  [[nodiscard]] ProductPresentationEpoch epoch() const noexcept {
    return epoch_;
  }
  [[nodiscard]] AssemblyMeshProfile meshProfile() const noexcept {
    return meshProfile_;
  }
  [[nodiscard]] const std::vector<AssemblyPresentationSource>& sources()
      const noexcept {
    return sources_;
  }
  [[nodiscard]] const std::vector<AssemblyPresentationInstance>& instances()
      const noexcept {
    return instances_;
  }
  [[nodiscard]] const AssemblyGraphAnalysis& graphAnalysis() const noexcept {
    return graphAnalysis_;
  }

  // Semantic equality deliberately excludes only the raw B-Rep address token
  // (and the owning shared_ptr which carries that same token).  Epoch, stable
  // model IDs, shape revision, profile, transforms and presentation flags all
  // remain authoritative comparison fields.
  [[nodiscard]] bool semanticallyEquivalent(
      const AssemblyPresentationSnapshot& other) const noexcept;

 private:
  friend class AssemblyPresentationBuilder;

  AssemblyPresentationSnapshot(
      ProductPresentationEpoch epoch, AssemblyMeshProfile meshProfile,
      std::vector<AssemblyPresentationSource> sources,
      std::vector<AssemblyPresentationInstance> instances,
      AssemblyGraphAnalysis graphAnalysis) noexcept
      : epoch_(epoch),
        meshProfile_(meshProfile),
        sources_(std::move(sources)),
        instances_(std::move(instances)),
        graphAnalysis_(std::move(graphAnalysis)) {}

  ProductPresentationEpoch epoch_;
  AssemblyMeshProfile meshProfile_;
  std::vector<AssemblyPresentationSource> sources_;
  std::vector<AssemblyPresentationInstance> instances_;
  AssemblyGraphAnalysis graphAnalysis_;
};

enum class AssemblyPresentationBuildStatus : std::uint8_t {
  Success,
  InvalidEpoch,
  InvalidMeshProfile,
  InvalidRootDefinitionId,
  MissingRootDefinition,
  RootIsNotAssembly,
  InvalidAssemblyDefinition,
  InvalidOccurrenceId,
  InvalidOccurrencePath,
  InvalidTargetDefinitionId,
  DanglingTargetDefinition,
  UnsupportedTargetDefinitionType,
  InvalidPartDefinition,
  InvalidBodyId,
  InvalidFeatureId,
  InvalidShapeRevision,
  MissingRetainedShape,
  NullRetainedShape,
  DuplicateStableIdentity,
  ResourceLimitExceeded,
  CountOverflow,
  OpenCascadeException,
  StandardException,
  UnknownException,
};

struct AssemblyPresentationBuildResult {
  AssemblyPresentationBuildStatus status{
      AssemblyPresentationBuildStatus::UnknownException};
  std::shared_ptr<const AssemblyPresentationSnapshot> snapshot;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == AssemblyPresentationBuildStatus::Success &&
           snapshot != nullptr;
  }
  [[nodiscard]] std::string_view diagnostic() const noexcept;
};

struct AssemblyPresentationLimits final {
  static constexpr std::size_t kMaximumOccurrences = 4096;
  static constexpr std::size_t kMaximumReachablePartDefinitions = 256;
  static constexpr std::size_t kMaximumSources = 16384;
  static constexpr std::size_t kMaximumInstances = 1048576;
};

}  // namespace solidar
