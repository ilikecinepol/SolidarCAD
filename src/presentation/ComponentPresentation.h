#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "model/ComponentGraphAnalysis.h"
#include "presentation/AssemblyPresentation.h"

class TopoDS_Shape;

namespace solidar {

class TopologyLineage;

// Complete immutable projection of every scalar that can affect the component
// tree.  It includes unreachable definitions and product incarnation, so a
// tree can verify the exact ProductDocument state used by its publication
// without retaining pointers into that mutable product.
struct ComponentTreeBodyProjection final {
  BodyId id{kInvalidBodyId};
  std::string nameUtf8;
  bool visible{true};
  friend bool operator==(const ComponentTreeBodyProjection&,
                         const ComponentTreeBodyProjection&) = default;
};

struct ComponentTreeSketchProjection final {
  SketchId id{kInvalidSketchId};
  std::string nameUtf8;
  friend bool operator==(const ComponentTreeSketchProjection&,
                         const ComponentTreeSketchProjection&) = default;
};

struct ComponentTreeImageProjection final {
  ReferenceImageId id{kInvalidReferenceImageId};
  std::string nameUtf8;
  std::string sourcePathUtf8;
  std::string supportNameUtf8;
  double offsetXMm{};
  double offsetYMm{};
  double offsetZMm{};
  double scale{1.0};
  int pixelWidth{};
  int pixelHeight{};
  bool visible{true};
  friend bool operator==(const ComponentTreeImageProjection&,
                         const ComponentTreeImageProjection&) = default;
};

struct ComponentTreeDrawingProjection final {
  DrawingId id{kInvalidDrawingId};
  std::string nameUtf8;
  ComponentDrawingPaper paper{ComponentDrawingPaper::A4};
  ComponentDrawingOrientation orientation{
      ComponentDrawingOrientation::Portrait};
  double scale{1.0};
  std::vector<BodyId> sourceBodyIds;
  friend bool operator==(const ComponentTreeDrawingProjection&,
                         const ComponentTreeDrawingProjection&) = default;
};

struct ComponentTreeOccurrenceProjection final {
  OccurrenceId id;
  DefinitionId targetDefinitionId;
  RigidTransform3d placement{RigidTransform3d::identity()};
  OccurrenceFlags flags;
  std::string displayNameUtf8;
  friend bool operator==(const ComponentTreeOccurrenceProjection&,
                         const ComponentTreeOccurrenceProjection&) = default;
};

struct ComponentTreeDefinitionProjection final {
  DefinitionId id;
  DefinitionStateId stateId;
  std::vector<ComponentTreeBodyProjection> bodies;
  std::vector<ComponentTreeSketchProjection> sketches;
  std::vector<ComponentTreeImageProjection> images;
  std::vector<ComponentTreeDrawingProjection> drawings;
  std::vector<ComponentTreeOccurrenceProjection> occurrences;
  friend bool operator==(const ComponentTreeDefinitionProjection&,
                         const ComponentTreeDefinitionProjection&) = default;
};

struct ComponentTreeProjection final {
  ProductIncarnationId productIncarnation;
  std::optional<ProductDocumentIdentity> productIdentity;
  DefinitionId rootDefinitionId;
  std::vector<ComponentTreeDefinitionProjection> definitions;

  [[nodiscard]] const ComponentTreeDefinitionProjection* findDefinition(
      DefinitionId id) const noexcept;
  friend bool operator==(const ComponentTreeProjection&,
                         const ComponentTreeProjection&) = default;
};

enum class ComponentTreeProjectionStatus : std::uint8_t {
  Projected,
  InvalidProduct,
  DuplicateStableIdentity,
  CountOverflow,
  ResourceLimitExceeded,
  ResourceFailure,
};

struct ComponentTreeProjectionResult final {
  ComponentTreeProjectionStatus status{
      ComponentTreeProjectionStatus::ResourceFailure};
  std::optional<ComponentTreeProjection> projection;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == ComponentTreeProjectionStatus::Projected &&
           projection.has_value();
  }
};

// Stable address of one expanded component instance.  The root component is
// deliberately represented by a missing path rather than a synthetic
// occurrence ID.
struct ComponentInstanceAddress final {
  ProductPresentationEpoch epoch;
  DefinitionId rootDefinitionId;
  DefinitionId definitionId;
  std::optional<OccurrencePath> occurrencePath;

  [[nodiscard]] bool rootInstance() const noexcept {
    return !occurrencePath.has_value();
  }

  friend bool operator==(const ComponentInstanceAddress&,
                         const ComponentInstanceAddress&) = default;
};

struct ComponentBodyIdentity final {
  ComponentInstanceAddress component;
  BodyId bodyId{kInvalidBodyId};

  friend bool operator==(const ComponentBodyIdentity&,
                         const ComponentBodyIdentity&) = default;
};

struct ComponentMeshSourceSemanticKey final {
  ProductPresentationEpoch epoch;
  DefinitionId definitionId;
  BodyId bodyId{kInvalidBodyId};
  FeatureId featureId{kInvalidFeatureId};
  ShapeRevision shapeRevision{kInvalidShapeRevision};
  AssemblyMeshProfile meshProfile;

  friend bool operator==(const ComponentMeshSourceSemanticKey&,
                         const ComponentMeshSourceSemanticKey&) = default;
};

// Exact renderer-cache key.  shapeIdentity distinguishes a replacement B-Rep
// even when every persistent model ID and the retained revision are equal.
struct ComponentMeshSourceKey final {
  ProductPresentationEpoch epoch;
  DefinitionId definitionId;
  BodyId bodyId{kInvalidBodyId};
  FeatureId featureId{kInvalidFeatureId};
  ShapeRevision shapeRevision{kInvalidShapeRevision};
  const TopoDS_Shape* shapeIdentity{};
  AssemblyMeshProfile meshProfile;

  [[nodiscard]] ComponentMeshSourceSemanticKey semanticKey() const noexcept {
    return {epoch, definitionId, bodyId, featureId, shapeRevision,
            meshProfile};
  }

  friend bool operator==(const ComponentMeshSourceKey&,
                         const ComponentMeshSourceKey&) = default;
};

class ComponentPresentationSource final {
 public:
  [[nodiscard]] const ComponentMeshSourceKey& key() const noexcept {
    return key_;
  }
  [[nodiscard]] const std::shared_ptr<const TopoDS_Shape>& shape()
      const noexcept {
    return shape_;
  }
  [[nodiscard]] const std::shared_ptr<const TopologyLineage>& topologyLineage()
      const noexcept {
    return topologyLineage_;
  }

 private:
  friend class ComponentPresentationBuilder;

  ComponentPresentationSource(
      ComponentMeshSourceKey key, std::shared_ptr<const TopoDS_Shape> shape,
      std::shared_ptr<const TopologyLineage> topologyLineage) noexcept
      : key_(std::move(key)),
        shape_(std::move(shape)),
        topologyLineage_(std::move(topologyLineage)) {}

  ComponentMeshSourceKey key_;
  std::shared_ptr<const TopoDS_Shape> shape_;
  std::shared_ptr<const TopologyLineage> topologyLineage_;
};

class ComponentPresentationInstance final {
 public:
  [[nodiscard]] const ComponentBodyIdentity& bodyIdentity() const noexcept {
    return bodyIdentity_;
  }
  [[nodiscard]] const ComponentInstanceAddress& componentAddress()
      const noexcept {
    return bodyIdentity_.component;
  }
  [[nodiscard]] const ComponentMeshSourceKey& sourceKey() const noexcept {
    return sourceKey_;
  }
  [[nodiscard]] const RigidTransform3d& worldTransform() const noexcept {
    return worldTransform_;
  }
  [[nodiscard]] bool componentVisible() const noexcept {
    return componentVisible_;
  }
  [[nodiscard]] bool suppressed() const noexcept { return suppressed_; }
  [[nodiscard]] bool grounded() const noexcept { return grounded_; }
  [[nodiscard]] bool bodyVisible() const noexcept { return bodyVisible_; }
  [[nodiscard]] bool effectivelyVisible() const noexcept {
    return effectivelyVisible_;
  }

 private:
  friend class ComponentPresentationBuilder;

  ComponentPresentationInstance(ComponentBodyIdentity bodyIdentity,
                                ComponentMeshSourceKey sourceKey,
                                RigidTransform3d worldTransform,
                                bool componentVisible, bool suppressed,
                                bool grounded, bool bodyVisible) noexcept
      : bodyIdentity_(std::move(bodyIdentity)),
        sourceKey_(std::move(sourceKey)),
        worldTransform_(std::move(worldTransform)),
        componentVisible_(componentVisible),
        suppressed_(suppressed),
        grounded_(grounded),
        bodyVisible_(bodyVisible),
        effectivelyVisible_(componentVisible && !suppressed && bodyVisible) {}

  ComponentBodyIdentity bodyIdentity_;
  ComponentMeshSourceKey sourceKey_;
  RigidTransform3d worldTransform_;
  bool componentVisible_{};
  bool suppressed_{};
  bool grounded_{};
  bool bodyVisible_{};
  bool effectivelyVisible_{};
};

class ComponentPresentationSnapshot final {
 public:
  [[nodiscard]] ProductPresentationEpoch epoch() const noexcept {
    return epoch_;
  }
  [[nodiscard]] AssemblyMeshProfile meshProfile() const noexcept {
    return meshProfile_;
  }
  [[nodiscard]] const std::vector<ComponentPresentationSource>& sources()
      const noexcept {
    return sources_;
  }
  [[nodiscard]] const std::vector<ComponentPresentationInstance>& instances()
      const noexcept {
    return instances_;
  }
  [[nodiscard]] const ComponentGraphTraversal& graphTraversal()
      const noexcept {
    return graphTraversal_;
  }
  [[nodiscard]] const ComponentTreeProjection& treeProjection()
      const noexcept {
    return treeProjection_;
  }

  [[nodiscard]] bool semanticallyEquivalent(
      const ComponentPresentationSnapshot& other) const noexcept;

 private:
  friend class ComponentPresentationBuilder;

  ComponentPresentationSnapshot(
      ProductPresentationEpoch epoch, AssemblyMeshProfile meshProfile,
      std::vector<ComponentPresentationSource> sources,
      std::vector<ComponentPresentationInstance> instances,
      ComponentGraphTraversal graphTraversal,
      ComponentTreeProjection treeProjection) noexcept
      : epoch_(epoch),
        meshProfile_(meshProfile),
        sources_(std::move(sources)),
        instances_(std::move(instances)),
        graphTraversal_(std::move(graphTraversal)),
        treeProjection_(std::move(treeProjection)) {}

  ProductPresentationEpoch epoch_;
  AssemblyMeshProfile meshProfile_;
  std::vector<ComponentPresentationSource> sources_;
  std::vector<ComponentPresentationInstance> instances_;
  ComponentGraphTraversal graphTraversal_;
  ComponentTreeProjection treeProjection_;
};

enum class ComponentPresentationBuildStatus : std::uint8_t {
  Success,
  InvalidEpoch,
  InvalidMeshProfile,
  InvalidRootDefinitionId,
  MissingRootDefinition,
  RootIsNotComponent,
  InvalidComponentGraph,
  DanglingTargetDefinition,
  InvalidOccurrencePath,
  InvalidTransform,
  InvalidComponentDefinition,
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

struct ComponentPresentationBuildResult final {
  ComponentPresentationBuildStatus status{
      ComponentPresentationBuildStatus::UnknownException};
  std::shared_ptr<const ComponentPresentationSnapshot> snapshot;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == ComponentPresentationBuildStatus::Success &&
           snapshot != nullptr;
  }
  [[nodiscard]] std::string_view diagnostic() const noexcept;
};

struct ComponentPresentationLimits final {
  static constexpr std::size_t kMaximumReachableDefinitions = 256;
  static constexpr std::size_t kMaximumComponentInstances = 65537;
  static constexpr std::size_t kMaximumSources = 16384;
  static constexpr std::size_t kMaximumBodyInstances = 1048576;
  static constexpr std::size_t kMaximumTreeProjectionEntries = 1048576;
  static constexpr std::size_t kMaximumTreeDrawingNameUtf8Bytes =
      kComponentGraphMaximumDrawings *
      kComponentDrawingMaximumNameUtf8Bytes;
  static constexpr std::size_t kMaximumTreeDrawingSourceReferences =
      kComponentGraphMaximumDrawingBodyReferences;
};

}  // namespace solidar
