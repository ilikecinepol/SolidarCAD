#include "ui/ComponentWorkspace.h"

#include <algorithm>
#include <new>
#include <utility>

namespace solidar {

PreparedComponentProjection::PreparedComponentProjection(
    std::unique_ptr<ComponentViewportSession> session,
    std::unique_ptr<ComponentTreeModel> treeModel,
    ComponentWorkspaceSemanticState semanticState,
    ProductPresentationEpoch epoch,
    AssemblyMeshProfile meshProfile,
    std::optional<ComponentIdentityTransition> identityTransition) noexcept
    : session_(std::move(session)),
      treeModel_(std::move(treeModel)),
      semanticState_(std::move(semanticState)),
      epoch_(epoch),
      meshProfile_(meshProfile),
      identityTransition_(std::move(identityTransition)) {}

namespace {

std::optional<ComponentInstanceAddress> componentAddress(
    const ProductDocument& product, ProductPresentationEpoch epoch,
    const ComponentWorkspaceInstanceKey& key) {
  if (!key.definitionId.isValid()) return std::nullopt;
  return ComponentInstanceAddress{epoch, product.rootDefinitionId(),
                                  key.definitionId, key.path};
}

bool hasComponentAddress(const ComponentViewportSession& session,
                         const ComponentInstanceAddress& address) noexcept {
  return std::any_of(session.components().begin(), session.components().end(),
                     [&address](const auto& component) {
                       return component.address == address;
                     });
}

bool validComponentSemanticState(
    const ProductDocument& product, ProductPresentationEpoch epoch,
    const ComponentViewportSession& session,
    const ComponentWorkspaceSemanticState& state) {
  constexpr std::size_t kMaximumDiagnosticBytes = 1024U * 1024U;
  if (state.diagnostic.size() > kMaximumDiagnosticBytes) return false;
  const auto active = componentAddress(product, epoch, state.activeInstance);
  if (!active || !hasComponentAddress(session, *active)) return false;
  if (state.selectedInstance) {
    const auto selected =
        componentAddress(product, epoch, *state.selectedInstance);
    if (!selected || !hasComponentAddress(session, *selected)) return false;
  }
  if (state.selectedBody) {
    const auto owner = componentAddress(product, epoch,
                                        state.selectedBody->instance);
    if (!owner || !hasComponentAddress(session, *owner)) return false;
    const auto* document =
        product.componentDocument(state.selectedBody->instance.definitionId);
    if (!document || !document->findBody(state.selectedBody->bodyId))
      return false;
  }
  return true;
}

}  // namespace

PreparedComponentProjectionResult ComponentProjectionPreparer::prepareImpl(
    const ProductDocument& product, ProductPresentationEpoch epoch,
    AssemblyMeshProfile meshProfile,
    const ComponentWorkspaceSemanticState& semanticState,
    const std::optional<ComponentWorkspaceInstanceKey>& hoveredInstance,
    std::optional<ComponentIdentityTransition> transition,
    ComponentViewportSessionFailurePoint sessionFailurePoint,
    ComponentTreeModelFailurePoint treeFailurePoint) noexcept {
  if (product.rootDefinitionKind() != DefinitionKind::Component ||
      !epoch.isValid() || !meshProfile.isValid())
    return {ComponentWorkspacePrepareStatus::InvalidCarrier, {},
            ComponentViewportSessionStatus::InvalidProduct,
            ComponentTreeModelCreateStatus::Rejected};
  try {
    if (transition && !transition->isCurrentSource(product))
      return {ComponentWorkspacePrepareStatus::InvalidCarrier, {},
              ComponentViewportSessionStatus::InvalidProduct,
              ComponentTreeModelCreateStatus::Rejected};
    auto session = transition
        ? ComponentViewportSession::prepareProjectedIdentity(
              product, epoch, meshProfile, *transition,
              sessionFailurePoint)
        : ComponentViewportSession::prepare(product, epoch, meshProfile,
                                            sessionFailurePoint);
    if (!session)
      return {session.status == ComponentViewportSessionStatus::ResourceFailure
                  ? ComponentWorkspacePrepareStatus::ResourceFailed
                  : ComponentWorkspacePrepareStatus::SessionFailed,
              {}, session.status, ComponentTreeModelCreateStatus::Rejected};
    if (!validComponentSemanticState(product, epoch, *session.session,
                                     semanticState))
      return {ComponentWorkspacePrepareStatus::InvalidCarrier, {},
              session.status, ComponentTreeModelCreateStatus::Rejected};
    if (semanticState.selectedInstance) {
      const auto address = componentAddress(product, epoch,
                                            *semanticState.selectedInstance);
      if (!address)
        return {ComponentWorkspacePrepareStatus::InvalidCarrier, {},
                session.status, ComponentTreeModelCreateStatus::Rejected};
      const auto selection = ComponentInstanceSelection::create(
          session.session->publicationHandle(), *address);
      if (!selection || !session.session->setSelectedComponent(selection))
        return {ComponentWorkspacePrepareStatus::SessionFailed, {},
                ComponentViewportSessionStatus::PreparationFailed,
                ComponentTreeModelCreateStatus::Rejected};
    }
    if (hoveredInstance) {
      const auto address = componentAddress(product, epoch, *hoveredInstance);
      if (!address || !hasComponentAddress(*session.session, *address))
        return {ComponentWorkspacePrepareStatus::InvalidCarrier, {},
                session.status, ComponentTreeModelCreateStatus::Rejected};
      const auto hover = ComponentInstanceSelection::create(
          session.session->publicationHandle(), *address);
      if (!hover || !session.session->setHoveredComponent(hover))
        return {ComponentWorkspacePrepareStatus::SessionFailed, {},
                ComponentViewportSessionStatus::PreparationFailed,
                ComponentTreeModelCreateStatus::Rejected};
    }
    auto tree = transition
        ? ComponentTreeModel::createTypedProjectedIdentity(
              product, session.session->publicationHandle(),
              *transition, nullptr, treeFailurePoint)
        : ComponentTreeModel::createTyped(
              product, session.session->publicationHandle(), nullptr,
              treeFailurePoint);
    if (!tree)
      return {tree.status == ComponentTreeModelCreateStatus::ResourceFailure
                  ? ComponentWorkspacePrepareStatus::ResourceFailed
                  : ComponentWorkspacePrepareStatus::TreeFailed,
              {}, session.status, tree.status};
    if (tree.model->QObject::parent() != nullptr)
      return {ComponentWorkspacePrepareStatus::TreeFailed, {}, session.status,
              ComponentTreeModelCreateStatus::Rejected};
    auto projection = std::unique_ptr<PreparedComponentProjection>(
        new PreparedComponentProjection(
            std::move(session.session), std::move(tree.model), semanticState,
            epoch, meshProfile, std::move(transition)));
    return {ComponentWorkspacePrepareStatus::Prepared,
            std::move(projection), ComponentViewportSessionStatus::Prepared,
            ComponentTreeModelCreateStatus::Created};
  } catch (const std::bad_alloc&) {
    return {ComponentWorkspacePrepareStatus::ResourceFailed, {},
            ComponentViewportSessionStatus::ResourceFailure,
            ComponentTreeModelCreateStatus::ResourceFailure};
  } catch (...) {
    return {ComponentWorkspacePrepareStatus::PreparationFailed, {},
            ComponentViewportSessionStatus::PreparationFailed,
            ComponentTreeModelCreateStatus::Rejected};
  }
}

PreparedComponentProjectionResult ComponentProjectionPreparer::prepare(
    const ProductDocument& product, ProductPresentationEpoch epoch,
    AssemblyMeshProfile meshProfile,
    const ComponentWorkspaceSemanticState& semanticState,
    const std::optional<ComponentWorkspaceInstanceKey>& hoveredInstance,
    ComponentViewportSessionFailurePoint sessionFailurePoint,
    ComponentTreeModelFailurePoint treeFailurePoint) noexcept {
  return prepareImpl(product, epoch, meshProfile, semanticState,
                     hoveredInstance,
                     std::nullopt, sessionFailurePoint, treeFailurePoint);
}

PreparedComponentProjectionResult ComponentProjectionPreparer::prepare(
    const ProductDocument& product, ProductPresentationEpoch epoch,
    AssemblyMeshProfile meshProfile,
    const ComponentWorkspaceSemanticState& semanticState,
    ComponentViewportSessionFailurePoint sessionFailurePoint,
    ComponentTreeModelFailurePoint treeFailurePoint) noexcept {
  return prepare(product, epoch, meshProfile, semanticState, std::nullopt,
                 sessionFailurePoint, treeFailurePoint);
}

PreparedComponentProjectionResult
ComponentProjectionPreparer::prepareProjectedIdentity(
    const ProductDocument& product, ProductPresentationEpoch epoch,
    AssemblyMeshProfile meshProfile,
    const ComponentWorkspaceSemanticState& semanticState,
    const std::optional<ComponentWorkspaceInstanceKey>& hoveredInstance,
    ComponentIdentityTransition transition,
    ComponentViewportSessionFailurePoint sessionFailurePoint,
    ComponentTreeModelFailurePoint treeFailurePoint) noexcept {
  return prepareImpl(product, epoch, meshProfile, semanticState,
                     hoveredInstance,
                     std::optional<ComponentIdentityTransition>{
                         std::move(transition)},
                     sessionFailurePoint, treeFailurePoint);
}

PreparedComponentWorkspace::PreparedComponentWorkspace(
    ProductDocument&& product,
    std::unique_ptr<ComponentViewportSession> session,
    std::unique_ptr<ComponentTreeModel> treeModel,
    ProductPresentationEpoch epoch) noexcept
    : product_(std::move(product)),
      session_(std::move(session)),
      treeModel_(std::move(treeModel)),
      epoch_(epoch) {}

PreparedComponentWorkspaceResult ComponentWorkspacePreparer::prepare(
    StagedApplicationDocument&& staged, ProductPresentationEpoch epoch,
    AssemblyMeshProfile meshProfile,
    ComponentViewportSessionFailurePoint sessionFailurePoint,
    ComponentTreeModelFailurePoint treeFailurePoint) noexcept {
  if (staged.rootDefinitionKind() != DefinitionKind::Component ||
      !epoch.isValid())
    return {ComponentWorkspacePrepareStatus::InvalidCarrier, {},
            ComponentViewportSessionStatus::InvalidProduct,
            ComponentTreeModelCreateStatus::Rejected};
  try {
    const auto persistenceState = staged.persistenceState;
    ProductDocument product = std::move(staged).releaseProductDocument();
    ComponentWorkspaceSemanticState semantic{
        {product.rootDefinitionId(), std::nullopt}, std::nullopt,
        std::nullopt, {}};
    auto projection = ComponentProjectionPreparer::prepare(
        product, epoch, meshProfile, semantic, sessionFailurePoint,
        treeFailurePoint);
    if (!projection)
      return {projection.status, {}, projection.sessionStatus,
              projection.treeStatus};
    auto workspace = std::unique_ptr<PreparedComponentWorkspace>(
        new PreparedComponentWorkspace(std::move(product),
                                       projection.projection->releaseSession(),
                                       projection.projection->releaseTreeModel(),
                                       epoch));
    workspace->persistenceState = persistenceState;
    return {ComponentWorkspacePrepareStatus::Prepared, std::move(workspace),
            ComponentViewportSessionStatus::Prepared,
            ComponentTreeModelCreateStatus::Created};
  } catch (const std::bad_alloc&) {
    return {ComponentWorkspacePrepareStatus::ResourceFailed, {},
            ComponentViewportSessionStatus::ResourceFailure,
            ComponentTreeModelCreateStatus::ResourceFailure};
  } catch (...) {
    return {ComponentWorkspacePrepareStatus::PreparationFailed, {},
            ComponentViewportSessionStatus::PreparationFailed,
            ComponentTreeModelCreateStatus::Rejected};
  }
}

}  // namespace solidar
