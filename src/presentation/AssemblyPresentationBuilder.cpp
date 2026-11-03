#include "presentation/AssemblyPresentationBuilder.h"

#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "model/Body.h"
#include "model/AssemblyGraphAnalysis.h"
#include "model/Document.h"
#include "model/ShapeFeature.h"

namespace solidar {
namespace {

AssemblyPresentationBuildResult failure(
    AssemblyPresentationBuildStatus status) noexcept {
  return {status, {}};
}

auto stableSourceTuple(const AssemblyMeshSourceKey& key) noexcept {
  return std::tuple{key.definitionId.value(), key.bodyId, key.featureId,
                    key.shapeRevision, key.epoch.value(),
                    static_cast<std::uint8_t>(key.meshProfile.quality())};
}

bool stableSourceLess(const AssemblyMeshSourceKey& left,
                      const AssemblyMeshSourceKey& right) noexcept {
  return stableSourceTuple(left) < stableSourceTuple(right);
}

bool sourceLess(const AssemblyPresentationSource& left,
                const AssemblyPresentationSource& right) noexcept {
  return stableSourceLess(left.key(), right.key());
}

bool pathLess(const OccurrencePath& left, const OccurrencePath& right) noexcept {
  return std::lexicographical_compare(
      left.entries().begin(), left.entries().end(), right.entries().begin(),
      right.entries().end(), [](OccurrenceId a, OccurrenceId b) {
        return a.value() < b.value();
      });
}

bool instanceLess(const AssemblyPresentationInstance& left,
                  const AssemblyPresentationInstance& right) noexcept {
  if (pathLess(left.occurrencePath(), right.occurrencePath())) return true;
  if (pathLess(right.occurrencePath(), left.occurrencePath())) return false;
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

struct DefinitionSource {
  AssemblyMeshSourceKey key;
  bool bodyVisible{};
};

AssemblyPresentationBuildStatus graphFailureStatus(
    AssemblyGraphStatus status) noexcept {
  switch (status) {
    case AssemblyGraphStatus::InvalidRoot:
      return AssemblyPresentationBuildStatus::MissingRootDefinition;
    case AssemblyGraphStatus::RootIsNotAssembly:
      return AssemblyPresentationBuildStatus::RootIsNotAssembly;
    case AssemblyGraphStatus::UnsupportedDefinitionKind:
      return AssemblyPresentationBuildStatus::InvalidAssemblyDefinition;
    case AssemblyGraphStatus::InvalidOccurrenceId:
      return AssemblyPresentationBuildStatus::InvalidOccurrenceId;
    case AssemblyGraphStatus::UnsupportedDepth:
      return AssemblyPresentationBuildStatus::InvalidOccurrencePath;
    case AssemblyGraphStatus::DanglingTarget:
      return AssemblyPresentationBuildStatus::DanglingTargetDefinition;
    case AssemblyGraphStatus::InvalidDefinitionId:
      return AssemblyPresentationBuildStatus::InvalidTargetDefinitionId;
    case AssemblyGraphStatus::InvalidTransform:
      return AssemblyPresentationBuildStatus::InvalidOccurrencePath;
    case AssemblyGraphStatus::DefinitionBudgetExceeded:
    case AssemblyGraphStatus::OccurrenceBudgetExceeded:
    case AssemblyGraphStatus::ExpandedWorkBudgetExceeded:
    case AssemblyGraphStatus::LeafBudgetExceeded:
    case AssemblyGraphStatus::UniqueSourceBudgetExceeded:
    case AssemblyGraphStatus::ResourceFailure:
      return AssemblyPresentationBuildStatus::ResourceLimitExceeded;
    case AssemblyGraphStatus::DuplicateOccurrenceId:
    case AssemblyGraphStatus::Cycle:
      return AssemblyPresentationBuildStatus::InvalidAssemblyDefinition;
    case AssemblyGraphStatus::Valid:
      return AssemblyPresentationBuildStatus::Success;
  }
  return AssemblyPresentationBuildStatus::InvalidAssemblyDefinition;
}

}  // namespace

AssemblyPresentationBuildResult AssemblyPresentationBuilder::build(
    const ProductDocument& product, ProductPresentationEpoch epoch,
    AssemblyMeshProfile meshProfile) noexcept {
  using enum AssemblyPresentationBuildStatus;
  try {
    if (!epoch.isValid()) return failure(InvalidEpoch);
    if (!meshProfile.isValid()) return failure(InvalidMeshProfile);

    const DefinitionId rootId = product.rootDefinitionId();
    if (!rootId.isValid()) return failure(InvalidRootDefinitionId);
    const auto rootKind = product.definitions().kind(rootId);
    if (!rootKind) return failure(MissingRootDefinition);
    if (*rootKind != DefinitionKind::Assembly)
      return failure(RootIsNotAssembly);
    const AssemblyDefinition* root = product.rootAssemblyDefinition();
    if (root == nullptr || root->id() != rootId)
      return failure(InvalidAssemblyDefinition);
    AssemblyGraphAnalysis graph = analyzeAssemblyGraph(
        product.definitions(), product.rootDefinitionId());
    if (!graph) return failure(graphFailureStatus(graph.status));

    std::vector<DefinitionId> reachablePartIds;
    reachablePartIds.reserve(graph.reachableSources);
    for (const auto& leaf : graph.leaves)
      reachablePartIds.push_back(leaf.terminalPartDefinitionId);

    std::sort(reachablePartIds.begin(), reachablePartIds.end());
    reachablePartIds.erase(
        std::unique(reachablePartIds.begin(), reachablePartIds.end()),
        reachablePartIds.end());
    if (reachablePartIds.size() >
        AssemblyPresentationLimits::kMaximumReachablePartDefinitions)
      return failure(ResourceLimitExceeded);

    std::vector<AssemblyPresentationSource> sources;
    std::map<DefinitionId, std::vector<DefinitionSource>> definitionSources;
    std::set<std::tuple<std::uint64_t, BodyId, FeatureId, ShapeRevision,
                        std::uint8_t>>
        stableSourceIdentities;

    for (const DefinitionId definitionId : reachablePartIds) {
      const PartDefinition* part =
          product.definitions().findPart(definitionId);
      if (part == nullptr || part->id() != definitionId)
        return failure(InvalidPartDefinition);

      std::set<BodyId> bodyIds;
      std::set<FeatureId> selectedFeatureIds;
      auto& partSources = definitionSources[definitionId];
      for (const Body& body : part->document().bodies()) {
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

        AssemblyMeshSourceKey key{
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
        if (sources.size() >= AssemblyPresentationLimits::kMaximumSources)
          return failure(ResourceLimitExceeded);

        partSources.push_back({key, body.visible()});
        sources.push_back(AssemblyPresentationSource(
            key, std::move(selectedShape),
            selectedFeature->lastValidTopologyLineage()));
      }
      std::sort(partSources.begin(), partSources.end(),
                [](const DefinitionSource& left,
                   const DefinitionSource& right) {
                  return stableSourceLess(left.key, right.key);
                });
    }
    std::sort(sources.begin(), sources.end(), sourceLess);

    std::size_t instanceCount = 0;
    for (const auto& leaf : graph.leaves) {
      const auto found =
          definitionSources.find(leaf.terminalPartDefinitionId);
      if (found == definitionSources.end())
        return failure(DanglingTargetDefinition);
      std::size_t nextCount = 0;
      if (!checkedAddWithin(instanceCount, found->second.size(),
                            AssemblyPresentationLimits::kMaximumInstances,
                            &nextCount)) {
        if (found->second.size() >
            std::numeric_limits<std::size_t>::max() - instanceCount)
          return failure(CountOverflow);
        return failure(ResourceLimitExceeded);
      }
      instanceCount = nextCount;
    }

    std::vector<AssemblyPresentationInstance> instances;
    instances.reserve(instanceCount);
    for (const auto& leaf : graph.leaves) {
      const auto& partSources =
          definitionSources.at(leaf.terminalPartDefinitionId);
      for (const DefinitionSource& source : partSources) {
        instances.push_back(AssemblyPresentationInstance(
            AssemblyOccurrenceIdentity{epoch, rootId, leaf.path},
            source.key, leaf.worldTransform, leaf.effectiveVisible,
            leaf.effectiveSuppressed, leaf.authoredLeafFlags.grounded,
            source.bodyVisible));
      }
    }
    std::sort(instances.begin(), instances.end(), instanceLess);
    for (std::size_t index = 1; index < instances.size(); ++index) {
      if (instances[index - 1].occurrenceIdentity() ==
              instances[index].occurrenceIdentity() &&
          instances[index - 1].sourceKey().semanticKey() ==
              instances[index].sourceKey().semanticKey())
        return failure(DuplicateStableIdentity);
    }

    std::shared_ptr<const AssemblyPresentationSnapshot> snapshot(
        new AssemblyPresentationSnapshot(epoch, meshProfile,
                                         std::move(sources),
                                         std::move(instances),
                                         std::move(graph)));
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
