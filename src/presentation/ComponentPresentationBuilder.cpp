#include "presentation/ComponentPresentationBuilder.h"

#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "model/Body.h"
#include "model/Document.h"
#include "model/ShapeFeature.h"

namespace solidar {
namespace {

ComponentPresentationBuildResult failure(
    ComponentPresentationBuildStatus status) noexcept {
  return {status, {}};
}

auto stableSourceTuple(const ComponentMeshSourceKey& key) noexcept {
  return std::tuple{key.definitionId.value(), key.bodyId, key.featureId,
                    key.shapeRevision, key.epoch.value(),
                    static_cast<std::uint8_t>(key.meshProfile.quality())};
}

bool stableSourceLess(const ComponentMeshSourceKey& left,
                      const ComponentMeshSourceKey& right) noexcept {
  return stableSourceTuple(left) < stableSourceTuple(right);
}

bool sourceLess(const ComponentPresentationSource& left,
                const ComponentPresentationSource& right) noexcept {
  return stableSourceLess(left.key(), right.key());
}

bool optionalPathLess(const std::optional<OccurrencePath>& left,
                      const std::optional<OccurrencePath>& right) noexcept {
  if (!left) return right.has_value();
  if (!right) return false;
  return std::lexicographical_compare(
      left->entries().begin(), left->entries().end(), right->entries().begin(),
      right->entries().end(), [](OccurrenceId a, OccurrenceId b) {
        return a.value() < b.value();
      });
}

bool instanceLess(const ComponentPresentationInstance& left,
                  const ComponentPresentationInstance& right) noexcept {
  const auto& leftAddress = left.componentAddress();
  const auto& rightAddress = right.componentAddress();
  if (optionalPathLess(leftAddress.occurrencePath,
                       rightAddress.occurrencePath))
    return true;
  if (optionalPathLess(rightAddress.occurrencePath,
                       leftAddress.occurrencePath))
    return false;
  if (leftAddress.definitionId != rightAddress.definitionId)
    return leftAddress.definitionId < rightAddress.definitionId;
  return stableSourceLess(left.sourceKey(), right.sourceKey());
}

bool checkedAddWithin(std::size_t current, std::size_t addition,
                      std::size_t maximum, std::size_t* sum) noexcept {
  if (addition > std::numeric_limits<std::size_t>::max() - current)
    return false;
  const std::size_t candidate = current + addition;
  if (candidate > maximum) return false;
  *sum = candidate;
  return true;
}

struct DefinitionSource final {
  ComponentMeshSourceKey key;
  bool bodyVisible{};
};

ComponentPresentationBuildStatus graphFailureStatus(
    const ComponentGraphTraversal& traversal) noexcept {
  using enum ComponentPresentationBuildStatus;
  switch (traversal.status) {
    case ComponentGraphTraversalStatus::Valid:
      return Success;
    case ComponentGraphTraversalStatus::InvalidTransform:
      return InvalidTransform;
    case ComponentGraphTraversalStatus::InvalidOccurrencePath:
      return InvalidOccurrencePath;
    case ComponentGraphTraversalStatus::ResourceFailure:
      return ResourceLimitExceeded;
    case ComponentGraphTraversalStatus::GraphRejected:
      break;
  }
  switch (traversal.graphStatus) {
    case ComponentGraphStatus::MissingRootDefinition:
      return MissingRootDefinition;
    case ComponentGraphStatus::DanglingTarget:
      return DanglingTargetDefinition;
    case ComponentGraphStatus::UnsupportedDepth:
      return InvalidOccurrencePath;
    case ComponentGraphStatus::DefinitionBudgetExceeded:
    case ComponentGraphStatus::OccurrenceBudgetExceeded:
    case ComponentGraphStatus::ExpandedWorkBudgetExceeded:
    case ComponentGraphStatus::DrawingBudgetExceeded:
    case ComponentGraphStatus::ResourceFailure:
      return ResourceLimitExceeded;
    case ComponentGraphStatus::DuplicateDefinitionId:
    case ComponentGraphStatus::InvalidDrawing:
    case ComponentGraphStatus::DuplicateDrawingId:
    case ComponentGraphStatus::DanglingDrawingBody:
    case ComponentGraphStatus::UnsupportedComponentDefinition:
    case ComponentGraphStatus::Cycle:
      return InvalidComponentGraph;
    case ComponentGraphStatus::Valid:
      return InvalidComponentGraph;
  }
  return InvalidComponentGraph;
}

}  // namespace

ComponentTreeProjectionResult
ComponentPresentationBuilder::buildTreeProjection(
    const ProductDocument& product) noexcept {
  return buildTreeProjectionImpl(product, nullptr);
}

ComponentTreeProjectionResult
ComponentPresentationBuilder::buildTreeProjection(
    const ProductDocument& product,
    const ComponentIdentityTransition& transition) noexcept {
  return buildTreeProjectionImpl(product, &transition);
}

ComponentTreeProjectionResult
ComponentPresentationBuilder::buildTreeProjectionImpl(
    const ProductDocument& product,
    const ComponentIdentityTransition* transition) noexcept {
  try {
    if (transition && !transition->isCurrentSource(product))
      return {ComponentTreeProjectionStatus::InvalidProduct, std::nullopt};
    ComponentTreeProjection projection;
    projection.productIncarnation = product.incarnation();
    projection.productIdentity = transition
                                     ? std::optional<ProductDocumentIdentity>{
                                           transition->targetIdentity()}
                                     : product.identity();
    projection.rootDefinitionId = product.rootDefinitionId();
    if (!projection.productIncarnation.isValid() ||
        !projection.rootDefinitionId.isValid() ||
        !product.rootComponentDefinition())
      return {ComponentTreeProjectionStatus::InvalidProduct, std::nullopt};

    const auto definitionIds = product.definitions().definitionIds();
    if (definitionIds.size() >
        ComponentPresentationLimits::kMaximumReachableDefinitions)
      return {ComponentTreeProjectionStatus::ResourceLimitExceeded,
              std::nullopt};
    projection.definitions.reserve(definitionIds.size());
    std::size_t entryCount = 0;
    std::size_t drawingNameBytes = 0;
    std::size_t drawingSourceReferenceCount = 0;
    for (const DefinitionId definitionId : definitionIds) {
      const auto* component =
          product.definitions().findComponent(definitionId);
      if (!component || component->id() != definitionId ||
          !component->stateId().isValid())
        return {ComponentTreeProjectionStatus::InvalidProduct, std::nullopt};
      ComponentTreeDefinitionProjection definition;
      definition.id = definitionId;
      definition.stateId = component->stateId();
      const Document& document = component->document();
      std::size_t nextCount = 0;
      const std::size_t additions[] = {
          1, document.bodies().size(), document.sketches().size(),
          document.referenceImages().size(), component->drawings().size(),
          component->occurrences().size()};
      for (const std::size_t addition : additions) {
        if (!checkedAddWithin(
                entryCount, addition,
                ComponentPresentationLimits::kMaximumTreeProjectionEntries,
                &nextCount)) {
          if (addition >
              std::numeric_limits<std::size_t>::max() - entryCount)
            return {ComponentTreeProjectionStatus::CountOverflow,
                    std::nullopt};
          return {ComponentTreeProjectionStatus::ResourceLimitExceeded,
                  std::nullopt};
        }
        entryCount = nextCount;
      }

      std::set<BodyId> bodyIds;
      definition.bodies.reserve(document.bodies().size());
      for (const Body& body : document.bodies()) {
        if (body.id() == kInvalidBodyId || !bodyIds.insert(body.id()).second)
          return {ComponentTreeProjectionStatus::DuplicateStableIdentity,
                  std::nullopt};
        definition.bodies.push_back(
            {body.id(), body.name(), body.visible()});
      }
      std::set<SketchId> sketchIds;
      definition.sketches.reserve(document.sketches().size());
      for (const auto& sketch : document.sketches()) {
        if (sketch.id == kInvalidSketchId ||
            !sketchIds.insert(sketch.id).second)
          return {ComponentTreeProjectionStatus::DuplicateStableIdentity,
                  std::nullopt};
        definition.sketches.push_back({sketch.id, sketch.name});
      }
      std::set<ReferenceImageId> imageIds;
      definition.images.reserve(document.referenceImages().size());
      for (const auto& image : document.referenceImages()) {
        if (image.id == kInvalidReferenceImageId ||
            !imageIds.insert(image.id).second)
          return {ComponentTreeProjectionStatus::DuplicateStableIdentity,
                  std::nullopt};
        definition.images.push_back(
            {image.id, image.name, image.sourcePath, image.supportName,
             image.offsetXMm, image.offsetYMm, image.offsetZMm, image.scale,
             image.pixelWidth, image.pixelHeight, image.visible});
      }
      std::set<DrawingId> drawingIds;
      definition.drawings.reserve(component->drawings().size());
      for (const auto& drawing : component->drawings()) {
        if (!drawing.id().isValid() ||
            !drawingIds.insert(drawing.id()).second ||
            drawing.nameUtf8().empty() ||
            drawing.nameUtf8().size() >
                kComponentDrawingMaximumNameUtf8Bytes ||
            !std::isfinite(drawing.scale()) ||
            drawing.scale() < kComponentDrawingMinimumScale ||
            drawing.scale() > kComponentDrawingMaximumScale ||
            !std::is_sorted(drawing.sourceBodyIds().begin(),
                            drawing.sourceBodyIds().end()) ||
            std::adjacent_find(drawing.sourceBodyIds().begin(),
                               drawing.sourceBodyIds().end()) !=
                drawing.sourceBodyIds().end())
          return {ComponentTreeProjectionStatus::InvalidProduct,
                  std::nullopt};
        switch (drawing.paper()) {
          case ComponentDrawingPaper::A4:
            break;
          default:
            return {ComponentTreeProjectionStatus::InvalidProduct,
                    std::nullopt};
        }
        switch (drawing.orientation()) {
          case ComponentDrawingOrientation::Portrait:
          case ComponentDrawingOrientation::Landscape:
            break;
          default:
            return {ComponentTreeProjectionStatus::InvalidProduct,
                    std::nullopt};
        }
        std::size_t nextDrawingNameBytes = 0;
        if (!checkedAddWithin(
                drawingNameBytes, drawing.nameUtf8().size(),
                ComponentPresentationLimits::kMaximumTreeDrawingNameUtf8Bytes,
                &nextDrawingNameBytes)) {
          if (drawing.nameUtf8().size() >
              std::numeric_limits<std::size_t>::max() - drawingNameBytes)
            return {ComponentTreeProjectionStatus::CountOverflow,
                    std::nullopt};
          return {ComponentTreeProjectionStatus::ResourceLimitExceeded,
                  std::nullopt};
        }
        drawingNameBytes = nextDrawingNameBytes;
        std::size_t nextSourceReferenceCount = 0;
        if (!checkedAddWithin(
                drawingSourceReferenceCount,
                drawing.sourceBodyIds().size(),
                ComponentPresentationLimits::
                    kMaximumTreeDrawingSourceReferences,
                &nextSourceReferenceCount)) {
          if (drawing.sourceBodyIds().size() >
              std::numeric_limits<std::size_t>::max() -
                  drawingSourceReferenceCount)
            return {ComponentTreeProjectionStatus::CountOverflow,
                    std::nullopt};
          return {ComponentTreeProjectionStatus::ResourceLimitExceeded,
                  std::nullopt};
        }
        drawingSourceReferenceCount = nextSourceReferenceCount;
        for (const BodyId sourceBodyId : drawing.sourceBodyIds()) {
          if (sourceBodyId == kInvalidBodyId ||
              !bodyIds.contains(sourceBodyId))
            return {ComponentTreeProjectionStatus::InvalidProduct,
                    std::nullopt};
        }
        definition.drawings.push_back(
            {drawing.id(), drawing.nameUtf8(), drawing.paper(),
             drawing.orientation(), drawing.scale(),
             drawing.sourceBodyIds()});
      }
      definition.occurrences.reserve(component->occurrences().size());
      for (const auto& [occurrenceId, occurrence] :
           component->occurrences()) {
        if (!occurrenceId.isValid() || occurrence.id() != occurrenceId ||
            !occurrence.targetDefinitionId().isValid())
          return {ComponentTreeProjectionStatus::InvalidProduct,
                  std::nullopt};
        definition.occurrences.push_back(
            {occurrenceId, occurrence.targetDefinitionId(),
             occurrence.placement(), occurrence.flags(),
             occurrence.displayNameUtf8()});
      }
      projection.definitions.push_back(std::move(definition));
    }
    if (!projection.findDefinition(projection.rootDefinitionId))
      return {ComponentTreeProjectionStatus::InvalidProduct, std::nullopt};
    return {ComponentTreeProjectionStatus::Projected,
            std::move(projection)};
  } catch (...) {
    return {ComponentTreeProjectionStatus::ResourceFailure, std::nullopt};
  }
}

ComponentPresentationBuildResult ComponentPresentationBuilder::build(
    const ProductDocument& product, ProductPresentationEpoch epoch,
    AssemblyMeshProfile meshProfile) noexcept {
  return buildImpl(product, epoch, meshProfile, nullptr);
}

ComponentPresentationBuildResult ComponentPresentationBuilder::build(
    const ProductDocument& product, ProductPresentationEpoch epoch,
    AssemblyMeshProfile meshProfile,
    const ComponentIdentityTransition& transition) noexcept {
  return buildImpl(product, epoch, meshProfile, &transition);
}

ComponentPresentationBuildResult ComponentPresentationBuilder::buildImpl(
    const ProductDocument& product, ProductPresentationEpoch epoch,
    AssemblyMeshProfile meshProfile,
    const ComponentIdentityTransition* transition) noexcept {
  using enum ComponentPresentationBuildStatus;
  try {
    if (!epoch.isValid()) return failure(InvalidEpoch);
    if (!meshProfile.isValid()) return failure(InvalidMeshProfile);

    const DefinitionId rootId = product.rootDefinitionId();
    if (!rootId.isValid()) return failure(InvalidRootDefinitionId);
    const auto rootKind = product.definitions().kind(rootId);
    if (!rootKind) return failure(MissingRootDefinition);
    if (*rootKind != DefinitionKind::Component)
      return failure(RootIsNotComponent);
    const ComponentDefinition* root = product.rootComponentDefinition();
    if (root == nullptr || root->id() != rootId)
      return failure(InvalidComponentDefinition);

    auto treeProjection = transition ? buildTreeProjection(product, *transition)
                                     : buildTreeProjection(product);
    if (!treeProjection) {
      switch (treeProjection.status) {
        case ComponentTreeProjectionStatus::DuplicateStableIdentity:
          return failure(DuplicateStableIdentity);
        case ComponentTreeProjectionStatus::CountOverflow:
          return failure(CountOverflow);
        case ComponentTreeProjectionStatus::ResourceLimitExceeded:
          return failure(ResourceLimitExceeded);
        case ComponentTreeProjectionStatus::InvalidProduct:
          return failure(InvalidComponentDefinition);
        case ComponentTreeProjectionStatus::ResourceFailure:
          return failure(StandardException);
        case ComponentTreeProjectionStatus::Projected:
          break;
      }
    }

    ComponentGraphTraversal traversal =
        expandComponentGraph(product.definitions(), rootId);
    if (!traversal) return failure(graphFailureStatus(traversal));
    if (traversal.visits.empty() || !traversal.visits.front().rootInstance() ||
        traversal.visits.front().definitionId != rootId)
      return failure(InvalidComponentGraph);
    if (traversal.visits.size() >
        ComponentPresentationLimits::kMaximumComponentInstances)
      return failure(ResourceLimitExceeded);

    std::vector<DefinitionId> reachableDefinitionIds;
    reachableDefinitionIds.reserve(traversal.visits.size());
    for (const auto& visit : traversal.visits)
      reachableDefinitionIds.push_back(visit.definitionId);
    std::sort(reachableDefinitionIds.begin(), reachableDefinitionIds.end());
    reachableDefinitionIds.erase(
        std::unique(reachableDefinitionIds.begin(),
                    reachableDefinitionIds.end()),
        reachableDefinitionIds.end());
    if (reachableDefinitionIds.size() >
        ComponentPresentationLimits::kMaximumReachableDefinitions)
      return failure(ResourceLimitExceeded);

    std::vector<ComponentPresentationSource> sources;
    std::map<DefinitionId, std::vector<DefinitionSource>> definitionSources;
    std::set<std::tuple<std::uint64_t, BodyId, FeatureId, ShapeRevision,
                        std::uint8_t>>
        stableSourceIdentities;

    for (const DefinitionId definitionId : reachableDefinitionIds) {
      const ComponentDefinition* component =
          product.definitions().findComponent(definitionId);
      if (component == nullptr || component->id() != definitionId)
        return failure(InvalidComponentDefinition);

      std::set<BodyId> bodyIds;
      std::set<FeatureId> selectedFeatureIds;
      auto& componentSources = definitionSources[definitionId];
      for (const Body& body : component->document().bodies()) {
        if (body.id() == kInvalidBodyId) return failure(InvalidBodyId);
        if (!bodyIds.insert(body.id()).second)
          return failure(DuplicateStableIdentity);

        const auto& features = body.features();
        if (features.empty()) continue;

        const ShapeFeature* selectedFeature = nullptr;
        ShapeFeature::ShapePtr selectedShape;
        for (auto candidate = features.rbegin(); candidate != features.rend();
             ++candidate) {
          if (!*candidate) return failure(InvalidFeatureId);
          const auto& retained = (*candidate)->lastValidShape();
          if (!retained) continue;
          selectedFeature = candidate->get();
          selectedShape = retained;
          break;
        }
        if (selectedFeature == nullptr || !selectedShape)
          return failure(MissingRetainedShape);
        if (selectedFeature->id() == kInvalidFeatureId)
          return failure(InvalidFeatureId);
        if (!selectedFeatureIds.insert(selectedFeature->id()).second)
          return failure(DuplicateStableIdentity);
        if (selectedFeature->shapeRevision() == kInvalidShapeRevision)
          return failure(InvalidShapeRevision);
        if (selectedShape->IsNull()) return failure(NullRetainedShape);

        ComponentMeshSourceKey key{
            epoch,
            definitionId,
            body.id(),
            selectedFeature->id(),
            selectedFeature->shapeRevision(),
            selectedShape.get(),
            meshProfile,
        };
        const auto stableIdentity = std::tuple{
            definitionId.value(), body.id(), selectedFeature->id(),
            selectedFeature->shapeRevision(),
            static_cast<std::uint8_t>(meshProfile.quality())};
        if (!stableSourceIdentities.insert(stableIdentity).second)
          return failure(DuplicateStableIdentity);
        if (sources.size() >= ComponentPresentationLimits::kMaximumSources)
          return failure(ResourceLimitExceeded);

        componentSources.push_back({key, body.visible()});
        sources.push_back(ComponentPresentationSource(
            key, std::move(selectedShape),
            selectedFeature->lastValidTopologyLineage()));
      }
      std::sort(componentSources.begin(), componentSources.end(),
                [](const DefinitionSource& left,
                   const DefinitionSource& right) {
                  return stableSourceLess(left.key, right.key);
                });
    }
    std::sort(sources.begin(), sources.end(), sourceLess);

    std::size_t instanceCount = 0;
    for (const auto& visit : traversal.visits) {
      const auto found = definitionSources.find(visit.definitionId);
      if (found == definitionSources.end())
        return failure(DanglingTargetDefinition);
      std::size_t nextCount = 0;
      if (!checkedAddWithin(
              instanceCount, found->second.size(),
              ComponentPresentationLimits::kMaximumBodyInstances,
              &nextCount)) {
        if (found->second.size() >
            std::numeric_limits<std::size_t>::max() - instanceCount)
          return failure(CountOverflow);
        return failure(ResourceLimitExceeded);
      }
      instanceCount = nextCount;
    }

    std::vector<ComponentPresentationInstance> instances;
    instances.reserve(instanceCount);
    for (const auto& visit : traversal.visits) {
      const auto& componentSources = definitionSources.at(visit.definitionId);
      for (const DefinitionSource& source : componentSources) {
        ComponentInstanceAddress address{epoch, rootId, visit.definitionId,
                                         visit.path};
        instances.push_back(ComponentPresentationInstance(
            ComponentBodyIdentity{std::move(address), source.key.bodyId},
            source.key, visit.worldTransform, visit.effectiveVisible,
            visit.effectiveSuppressed,
            visit.rootInstance() ? false : visit.authoredFlags.grounded,
            source.bodyVisible));
      }
    }
    std::sort(instances.begin(), instances.end(), instanceLess);
    for (std::size_t index = 1; index < instances.size(); ++index) {
      if (instances[index - 1].bodyIdentity() ==
              instances[index].bodyIdentity() &&
          instances[index - 1].sourceKey().semanticKey() ==
              instances[index].sourceKey().semanticKey())
        return failure(DuplicateStableIdentity);
    }

    std::shared_ptr<const ComponentPresentationSnapshot> snapshot(
        new ComponentPresentationSnapshot(
            epoch, meshProfile, std::move(sources), std::move(instances),
            std::move(traversal), std::move(*treeProjection.projection)));
    return {Success, std::move(snapshot)};
  } catch (const Standard_Failure&) {
    return failure(OpenCascadeException);
  } catch (const std::exception&) {
    return failure(StandardException);
  } catch (...) {
    return failure(UnknownException);
  }
}

}  // namespace solidar
