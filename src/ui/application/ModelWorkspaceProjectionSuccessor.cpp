#include "ui/application/ModelWorkspaceProjectionSuccessor.h"

#include <new>
#include <utility>

namespace solidar {
namespace {

PartWorkspaceProjectionFailurePoint partFailurePoint(
    ModelWorkspaceProjectionFailurePoint point) noexcept {
  switch (point) {
    case ModelWorkspaceProjectionFailurePoint::PartSelection:
      return PartWorkspaceProjectionFailurePoint::Selection;
    case ModelWorkspaceProjectionFailurePoint::PartTopology:
      return PartWorkspaceProjectionFailurePoint::Topology;
    case ModelWorkspaceProjectionFailurePoint::PartMeshing:
      return PartWorkspaceProjectionFailurePoint::Meshing;
    case ModelWorkspaceProjectionFailurePoint::PartBounds:
      return PartWorkspaceProjectionFailurePoint::Bounds;
    case ModelWorkspaceProjectionFailurePoint::Resource:
      return PartWorkspaceProjectionFailurePoint::Resource;
    case ModelWorkspaceProjectionFailurePoint::Finalization:
      return PartWorkspaceProjectionFailurePoint::Finalization;
    default:
      return PartWorkspaceProjectionFailurePoint::None;
  }
}

}  // namespace

ProductDocumentSuccessorStatus ModelWorkspaceProjectionSuccessor::prepare(
    const ProductDocument& candidate,
    ProductDocumentSuccessorImpact impact) noexcept {
  if (attempted_) {
    status_ = ModelWorkspaceProjectionStatus::AlreadyPrepared;
    return ProductDocumentSuccessorStatus::Rejected;
  }
  attempted_ = true;
  if (!epoch_.isValid() || !meshProfile_.isValid()) {
    status_ = ModelWorkspaceProjectionStatus::InvalidCandidate;
    return ProductDocumentSuccessorStatus::Rejected;
  }
  if (failurePoint_ == ModelWorkspaceProjectionFailurePoint::Resource) {
    status_ = ModelWorkspaceProjectionStatus::ResourceFailure;
    return ProductDocumentSuccessorStatus::ResourceFailure;
  }
  if (impact == ProductDocumentSuccessorImpact::TreeOnly &&
      candidate.rootDefinitionKind() == DefinitionKind::Component) {
    status_ = ModelWorkspaceProjectionStatus::PreparedComponent;
    return ProductDocumentSuccessorStatus::Prepared;
  }
  if (impact != ProductDocumentSuccessorImpact::FullProjection) {
    status_ = ModelWorkspaceProjectionStatus::InvalidCandidate;
    return ProductDocumentSuccessorStatus::Rejected;
  }
  try {
    if (candidate.rootDefinitionKind() == DefinitionKind::Part) {
      auto part = PartWorkspaceProjectionPreparer::prepare(
          candidate, epoch_, meshProfile_, semanticState_,
          partFailurePoint(failurePoint_));
      if (!part) {
        status_ = part.status == PartWorkspaceProjectionStatus::ResourceFailure
                      ? ModelWorkspaceProjectionStatus::ResourceFailure
                      : ModelWorkspaceProjectionStatus::ProjectionRejected;
        return status_ == ModelWorkspaceProjectionStatus::ResourceFailure
                   ? ProductDocumentSuccessorStatus::ResourceFailure
                   : ProductDocumentSuccessorStatus::Rejected;
      }
      if (failurePoint_ == ModelWorkspaceProjectionFailurePoint::Finalization) {
        status_ = ModelWorkspaceProjectionStatus::ResourceFailure;
        return ProductDocumentSuccessorStatus::ResourceFailure;
      }
      prepared_.template emplace<1>(std::move(part.projection));
      status_ = ModelWorkspaceProjectionStatus::PreparedPart;
      return ProductDocumentSuccessorStatus::Prepared;
    }
    if (candidate.rootDefinitionKind() == DefinitionKind::Component) {
      const auto sessionFailure =
          failurePoint_ == ModelWorkspaceProjectionFailurePoint::ComponentSession
              ? ComponentViewportSessionFailurePoint::Resource
              : ComponentViewportSessionFailurePoint::None;
      const auto treeFailure =
          failurePoint_ == ModelWorkspaceProjectionFailurePoint::ComponentTree
              ? ComponentTreeModelFailurePoint::ResourceFailure
              : ComponentTreeModelFailurePoint::None;
      auto component = ComponentProjectionPreparer::prepare(
          candidate, epoch_, meshProfile_, semanticState_, hoveredInstance_,
          sessionFailure, treeFailure);
      if (!component) {
        status_ = component.status == ComponentWorkspacePrepareStatus::ResourceFailed
                      ? ModelWorkspaceProjectionStatus::ResourceFailure
                      : ModelWorkspaceProjectionStatus::ProjectionRejected;
        return status_ == ModelWorkspaceProjectionStatus::ResourceFailure
                   ? ProductDocumentSuccessorStatus::ResourceFailure
                   : ProductDocumentSuccessorStatus::Rejected;
      }
      if (failurePoint_ == ModelWorkspaceProjectionFailurePoint::Finalization) {
        status_ = ModelWorkspaceProjectionStatus::ResourceFailure;
        return ProductDocumentSuccessorStatus::ResourceFailure;
      }
      prepared_.template emplace<2>(std::move(component.projection));
      status_ = ModelWorkspaceProjectionStatus::PreparedComponent;
      return ProductDocumentSuccessorStatus::Prepared;
    }
    status_ = ModelWorkspaceProjectionStatus::InvalidCandidate;
    return ProductDocumentSuccessorStatus::Rejected;
  } catch (const std::bad_alloc&) {
    status_ = ModelWorkspaceProjectionStatus::ResourceFailure;
    return ProductDocumentSuccessorStatus::ResourceFailure;
  } catch (...) {
    status_ = ModelWorkspaceProjectionStatus::ProjectionRejected;
    return ProductDocumentSuccessorStatus::Rejected;
  }
}

const PartWorkspaceProjection* ModelWorkspaceProjectionSuccessor::part() const
    noexcept {
  const auto* value = std::get_if<1>(&prepared_);
  return value ? value->get() : nullptr;
}

const PreparedComponentProjection*
ModelWorkspaceProjectionSuccessor::component() const noexcept {
  const auto* value = std::get_if<2>(&prepared_);
  return value ? value->get() : nullptr;
}

std::unique_ptr<PartWorkspaceProjection>
ModelWorkspaceProjectionSuccessor::takePart() noexcept {
  auto* value = std::get_if<1>(&prepared_);
  if (!value) return {};
  auto result = std::move(*value);
  prepared_.template emplace<0>();
  status_ = ModelWorkspaceProjectionStatus::Consumed;
  return result;
}

std::unique_ptr<PreparedComponentProjection>
ModelWorkspaceProjectionSuccessor::takeComponent() noexcept {
  auto* value = std::get_if<2>(&prepared_);
  if (!value) return {};
  auto result = std::move(*value);
  prepared_.template emplace<0>();
  status_ = ModelWorkspaceProjectionStatus::Consumed;
  return result;
}

}  // namespace solidar
