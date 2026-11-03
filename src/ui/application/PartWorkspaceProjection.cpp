#include "ui/application/PartWorkspaceProjection.h"

#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <new>
#include <utility>

namespace solidar {
namespace {

const ShapeFeature* presentationOwner(
    const Body& body, const ShapeFeature::ShapePtr& shape) noexcept {
  if (!shape) return nullptr;
  for (auto feature = body.features().rbegin();
       feature != body.features().rend(); ++feature)
    if (*feature && (*feature)->lastValidShape().get() == shape.get())
      return feature->get();
  return nullptr;
}

bool validPartSemanticState(
    const ProductDocument& product,
    const ComponentWorkspaceSemanticState& semantic) noexcept {
  constexpr std::size_t kMaximumDiagnosticBytes = 1024U * 1024U;
  const auto root = product.rootDefinitionId();
  const auto validRoot = [root](const ComponentWorkspaceInstanceKey& key) {
    return key.definitionId == root && !key.path;
  };
  if (semantic.diagnostic.size() > kMaximumDiagnosticBytes ||
      !validRoot(semantic.activeInstance) ||
      (semantic.selectedInstance &&
       !validRoot(*semantic.selectedInstance)))
    return false;
  if (semantic.selectedBody) {
    if (!validRoot(semantic.selectedBody->instance)) return false;
    const auto* part = product.rootPartDefinition();
    if (!part || !part->document().findBody(semantic.selectedBody->bodyId))
      return false;
  }
  return true;
}

ViewportMeshQuality viewportQuality(AssemblyMeshProfile profile) noexcept {
  return profile.quality() == AssemblyMeshQuality::High
             ? ViewportMeshQuality::High
             : ViewportMeshQuality::Normal;
}

BodyDisplayPrepareFailurePoint bodyFailurePoint(
    PartWorkspaceProjectionFailurePoint point) noexcept {
  switch (point) {
    case PartWorkspaceProjectionFailurePoint::Topology:
      return BodyDisplayPrepareFailurePoint::Topology;
    case PartWorkspaceProjectionFailurePoint::Meshing:
      return BodyDisplayPrepareFailurePoint::Meshing;
    case PartWorkspaceProjectionFailurePoint::Bounds:
      return BodyDisplayPrepareFailurePoint::Bounds;
    case PartWorkspaceProjectionFailurePoint::Resource:
      return BodyDisplayPrepareFailurePoint::Resource;
    case PartWorkspaceProjectionFailurePoint::Finalization:
      return BodyDisplayPrepareFailurePoint::Finalization;
    default:
      return BodyDisplayPrepareFailurePoint::None;
  }
}

}  // namespace

PartWorkspaceProjectionResult PartWorkspaceProjectionPreparer::prepare(
    const ProductDocument& product, ProductPresentationEpoch epoch,
    AssemblyMeshProfile meshProfile,
    const ComponentWorkspaceSemanticState& semanticState,
    PartWorkspaceProjectionFailurePoint failurePoint) noexcept {
  if (product.rootDefinitionKind() != DefinitionKind::Part ||
      !product.hasSingleValidRootDefinition() || !epoch.isValid() ||
      !meshProfile.isValid())
    return {PartWorkspaceProjectionStatus::InvalidProduct, {},
            BodyDisplayPrepareStatus::InvalidInput,
            {GeometryFailureKind::InvalidInput,
             "Part workspace requires one local Part root"}};
  if (failurePoint == PartWorkspaceProjectionFailurePoint::Selection ||
      !validPartSemanticState(product, semanticState))
    return {PartWorkspaceProjectionStatus::InvalidSelection, {},
            BodyDisplayPrepareStatus::InvalidInput,
            {GeometryFailureKind::InvalidInput,
             "Part workspace semantic selection is invalid"}};
  try {
    const auto* part = product.rootPartDefinition();
    if (!part || part->isLinked())
      return {PartWorkspaceProjectionStatus::InvalidProduct, {},
              BodyDisplayPrepareStatus::InvalidInput,
              {GeometryFailureKind::InvalidInput,
               "Linked Part cannot be an authoring workspace"}};
    std::vector<BodyViewShape> shapes;
    shapes.reserve(part->document().bodies().size());
    for (const Body& body : part->document().bodies()) {
      if (!body.visible()) continue;
      auto shape = body.lastValidResultShape();
      if (!shape || shape->IsNull()) {
        if (!body.features().empty())
          return {PartWorkspaceProjectionStatus::BodyDisplayFailed, {},
                  BodyDisplayPrepareStatus::InvalidInput,
                  {GeometryFailureKind::IncompleteDocument,
                   "Visible Part body has no retained valid result"}};
        continue;
      }
      const ShapeFeature* owner = presentationOwner(body, shape);
      if (!owner || owner->shapeRevision() == kInvalidShapeRevision)
        return {PartWorkspaceProjectionStatus::BodyDisplayFailed, {},
                BodyDisplayPrepareStatus::InvalidInput,
                {GeometryFailureKind::InvalidBRep,
                 "Retained Part shape has no stable owner"}};
      std::string topologyError;
      auto topology = owner->lastValidTopologyIndex(&topologyError);
      if (!topology &&
          failurePoint != PartWorkspaceProjectionFailurePoint::Topology)
        return {PartWorkspaceProjectionStatus::BodyDisplayFailed, {},
                BodyDisplayPrepareStatus::TopologyFailed,
                {GeometryFailureKind::InvalidBRep,
                 topologyError.empty()
                     ? "Retained Part topology is unavailable"
                     : std::move(topologyError)}};
      shapes.push_back({body.id(), owner->id(), std::move(shape),
                        std::move(topology), owner->shapeRevision()});
    }
    auto display = Viewport::prepareBodyDisplay(
        std::move(shapes), viewportQuality(meshProfile), epoch,
        product.rootDefinitionId(), bodyFailurePoint(failurePoint));
    if (!display)
      return {display.status == BodyDisplayPrepareStatus::ResourceFailure ||
                      display.status == BodyDisplayPrepareStatus::ResourceLimit
                  ? PartWorkspaceProjectionStatus::ResourceFailure
                  : PartWorkspaceProjectionStatus::BodyDisplayFailed,
              {}, display.status, std::move(display.failure)};
    auto projection = std::unique_ptr<PartWorkspaceProjection>(
        new PartWorkspaceProjection(std::move(display.display), semanticState,
                                    epoch, meshProfile));
    return {PartWorkspaceProjectionStatus::Prepared, std::move(projection),
            BodyDisplayPrepareStatus::Prepared, {}};
  } catch (const std::bad_alloc&) {
    return {PartWorkspaceProjectionStatus::ResourceFailure, {},
            BodyDisplayPrepareStatus::ResourceFailure,
            {GeometryFailureKind::ResourceLimit,
             "Part workspace projection allocation failed"}};
  } catch (...) {
    return {PartWorkspaceProjectionStatus::PreparationFailed, {},
            BodyDisplayPrepareStatus::ResourceFailure,
            {GeometryFailureKind::UnknownException,
             "Part workspace projection failed"}};
  }
}

}  // namespace solidar
