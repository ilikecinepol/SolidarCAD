#include "ui/internal/ViewportPickingTopology.h"

#include <TopoDS_Shape.hxx>

#include <exception>
#include <utility>

#include "model/TopologyReferenceResolver.h"

namespace solidar::detail {
namespace {

void increment(std::uint64_t* counter) noexcept {
  if (counter) ++*counter;
}

PreparedViewportPickingTopology failure(
    ViewportPickingTopologyStatus status) noexcept {
  PreparedViewportPickingTopology result;
  result.status = status;
  return result;
}

}  // namespace

PreparedViewportPickingTopology prepareViewportPickingTopology(
    const std::vector<ViewportPickingTopologyInput>& inputs,
    const ViewportPickingTopologyCache& incumbent,
    const std::function<bool()>& current,
    std::size_t& failBuildCountdown, std::size_t& failMappingCountdown,
    ViewportPickingTopologyCounters counters) noexcept {
  try {
    PreparedViewportPickingTopology result;
    result.cache = incumbent;
    result.activeSources.reserve(inputs.size());
    for (const auto& input : inputs) {
      if (!current())
        return failure(ViewportPickingTopologyStatus::StalePublication);
      if (!input.shape || input.shape->IsNull() || !input.mesh ||
          input.key.shapeIdentity != input.shape.get() ||
          input.key.productEpoch == 0 || input.key.definitionId == 0 ||
          input.key.bodyId == kInvalidBodyId ||
          input.key.featureId == kInvalidFeatureId ||
          input.key.shapeRevision == kInvalidShapeRevision)
        return failure(ViewportPickingTopologyStatus::InvalidSource);

      std::shared_ptr<const ViewportPickingTopologySource> prepared;
      const auto cached = result.cache.find(input.key);
      if (cached != result.cache.end()) {
        prepared = cached->second;
        if (!prepared || prepared->shape != input.shape ||
            prepared->mesh != input.mesh)
          return failure(ViewportPickingTopologyStatus::InvalidSource);
        increment(counters.reuses);
      } else {
        increment(counters.buildAttempts);
        if (failBuildCountdown > 0 && --failBuildCountdown == 0) {
          increment(counters.buildFailures);
          return failure(ViewportPickingTopologyStatus::TopologyBuildFailed);
        }
        std::string topologyError;
        auto topology = TopologyIndex::build(
            input.shape, input.key.shapeRevision, input.topologyLineage,
            &topologyError);
        if (!topology) {
          increment(counters.buildFailures);
          return failure(ViewportPickingTopologyStatus::TopologyBuildFailed);
        }
        if (topology->rawFaceCount() != input.mesh->faceCount()) {
          increment(counters.buildFailures);
          return failure(
              ViewportPickingTopologyStatus::InvalidTopologyMapping);
        }
        for (const RenderVertex& vertex : input.mesh->vertices()) {
          if (vertex.faceIndex >= topology->rawFaceCount()) {
            increment(counters.buildFailures);
            return failure(
                ViewportPickingTopologyStatus::InvalidTopologyMapping);
          }
        }
        for (const RenderEdge& edge : input.mesh->edges()) {
          if (edge.edgeIndex >= topology->rawEdgeCount()) {
            increment(counters.buildFailures);
            return failure(
                ViewportPickingTopologyStatus::InvalidTopologyMapping);
          }
        }
        if (failMappingCountdown > 0 && --failMappingCountdown == 0) {
          increment(counters.buildFailures);
          return failure(
              ViewportPickingTopologyStatus::InvalidTopologyMapping);
        }

        auto mutableSource = std::make_shared<ViewportPickingTopologySource>();
        mutableSource->key = input.key;
        mutableSource->shape = input.shape;
        mutableSource->mesh = input.mesh;
        mutableSource->topology = std::move(topology);
        mutableSource->faces.reserve(mutableSource->topology->rawFaceCount());
        for (std::size_t raw = 0;
             raw < mutableSource->topology->rawFaceCount(); ++raw) {
          const auto v2 = mutableSource->topology->createFaceReferenceV2(
              input.key.bodyId, input.key.featureId, raw);
          if (v2.status == FaceReferenceV2CreationStatus::Created) {
            if (v2.reference.bodyId() != input.key.bodyId ||
                v2.reference.featureId() != input.key.featureId)
              return failure(
                  ViewportPickingTopologyStatus::InvalidTopologyMapping);
            mutableSource->faces.push_back(v2.reference);
            increment(counters.v2Faces);
          } else {
            if (v2.status != FaceReferenceV2CreationStatus::Missing)
              return failure(
                  ViewportPickingTopologyStatus::InvalidTopologyMapping);
            const auto v1 = mutableSource->topology->createFaceReference(
                input.key.bodyId, input.key.featureId, raw);
            if (!v1 || v1.reference.bodyId() != input.key.bodyId ||
                v1.reference.featureId() != input.key.featureId)
              return failure(
                  ViewportPickingTopologyStatus::InvalidTopologyMapping);
            mutableSource->faces.push_back(v1.reference);
            increment(counters.v1Faces);
          }
        }
        mutableSource->edges.reserve(mutableSource->topology->rawEdgeCount());
        for (std::size_t raw = 0;
             raw < mutableSource->topology->rawEdgeCount(); ++raw) {
          const auto v2 = mutableSource->topology->createEdgeReferenceV2(
              input.key.bodyId, input.key.featureId, raw);
          if (v2.status == EdgeReferenceV2CreationStatus::Created) {
            if (v2.reference.bodyId() != input.key.bodyId ||
                v2.reference.featureId() != input.key.featureId)
              return failure(
                  ViewportPickingTopologyStatus::InvalidTopologyMapping);
            mutableSource->edges.push_back(v2.reference);
            increment(counters.v2Edges);
          } else {
            if (v2.status != EdgeReferenceV2CreationStatus::Missing)
              return failure(
                  ViewportPickingTopologyStatus::InvalidTopologyMapping);
            const auto v1 = mutableSource->topology->createEdgeReference(
                input.key.bodyId, input.key.featureId, raw);
            if (!v1 || v1.reference.bodyId() != input.key.bodyId ||
                v1.reference.featureId() != input.key.featureId)
              return failure(
                  ViewportPickingTopologyStatus::InvalidTopologyMapping);
            mutableSource->edges.push_back(v1.reference);
            increment(counters.v1Edges);
          }
        }
        prepared = std::move(mutableSource);
        result.cache.emplace(input.key, prepared);
        increment(counters.buildSuccesses);
      }
      if (!result.activeSources.emplace(input.key, std::move(prepared)).second)
        return failure(ViewportPickingTopologyStatus::DuplicateSource);
    }
    if (!current())
      return failure(ViewportPickingTopologyStatus::StalePublication);
    ViewportPickingTopologyCache retained;
    retained.reserve(result.activeSources.size());
    for (const auto& [key, source] : result.activeSources)
      retained.emplace(key, source);
    result.cache.swap(retained);
    result.status = ViewportPickingTopologyStatus::Prepared;
    return result;
  } catch (const std::exception&) {
    return failure(ViewportPickingTopologyStatus::ResourceFailure);
  } catch (...) {
    return failure(ViewportPickingTopologyStatus::ResourceFailure);
  }
}

}  // namespace solidar::detail
