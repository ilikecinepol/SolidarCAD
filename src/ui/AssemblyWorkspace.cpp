#include "ui/AssemblyWorkspace.h"

#include <exception>
#include <new>
#include <utility>

#include "ui/AssemblyTreeModel.h"

namespace solidar {

PreparedAssemblyWorkspace::PreparedAssemblyWorkspace(
    ProductDocument&& product,
    std::unique_ptr<AssemblyViewportSession> session,
    std::unique_ptr<AssemblyTreeModel> treeModel,
    ProductPresentationEpoch epoch) noexcept
    : product_(std::move(product)),
      session_(std::move(session)),
      treeModel_(std::move(treeModel)),
      epoch_(epoch) {}

PreparedAssemblyWorkspaceResult AssemblyWorkspacePreparer::prepare(
    StagedApplicationDocument&& staged, ProductPresentationEpoch epoch,
    AssemblyMeshProfile meshProfile,
    AssemblyViewportSessionFailurePoint failurePoint) noexcept {
  if (staged.rootDefinitionKind() != DefinitionKind::Assembly ||
      !epoch.isValid())
    return {AssemblyWorkspacePrepareStatus::InvalidCarrier, {},
            AssemblyViewportSessionStatus::InvalidProduct};
  try {
    const auto persistenceState = staged.persistenceState;
    auto linkedPartRuntimeStates =
        std::move(staged.linkedPartRuntimeStates);
    auto mateRuntimeDiagnostics =
        std::move(staged.mateRuntimeDiagnostics);
    ProductDocument product = std::move(staged).releaseProductDocument();
    auto preparedSession = AssemblyViewportSession::prepare(
        product, epoch, meshProfile, failurePoint);
    if (!preparedSession)
      return {AssemblyWorkspacePrepareStatus::SessionFailed, {},
              preparedSession.status};
    auto tree = AssemblyTreeModel::create(
        product, epoch, preparedSession.session->publicationHandle(), nullptr,
        std::nullopt, &linkedPartRuntimeStates);
    if (!tree)
      return {AssemblyWorkspacePrepareStatus::TreeFailed, {},
              preparedSession.status};
    auto result = std::unique_ptr<PreparedAssemblyWorkspace>(
        new PreparedAssemblyWorkspace(std::move(product),
                                      std::move(preparedSession.session),
                                      std::move(tree), epoch));
    result->persistenceState = persistenceState;
    result->linkedPartRuntimeStates = std::move(linkedPartRuntimeStates);
    result->mateRuntimeDiagnostics = std::move(mateRuntimeDiagnostics);
    return {AssemblyWorkspacePrepareStatus::Prepared, std::move(result),
            AssemblyViewportSessionStatus::Prepared};
  } catch (const std::exception&) {
    return {AssemblyWorkspacePrepareStatus::PreparationFailed, {},
            AssemblyViewportSessionStatus::PreparationFailed};
  } catch (...) {
    return {AssemblyWorkspacePrepareStatus::PreparationFailed, {},
            AssemblyViewportSessionStatus::PreparationFailed};
  }
}

PreparedAssemblyProjectionResult AssemblyProjectionPreparer::prepare(
    const ProductDocument& product, ProductPresentationEpoch epoch,
    AssemblyMeshProfile meshProfile,
    AssemblyViewportSessionFailurePoint failurePoint,
    const LinkedPartRuntimeStates* linkedStates) noexcept {
  if (product.rootDefinitionKind() != DefinitionKind::Assembly ||
      !epoch.isValid())
    return {AssemblyWorkspacePrepareStatus::InvalidCarrier, {}};
  try {
    auto session = AssemblyViewportSession::prepare(product, epoch, meshProfile,
                                                    failurePoint);
    if (!session)
      return {session.status == AssemblyViewportSessionStatus::ResourceFailure
                  ? AssemblyWorkspacePrepareStatus::ResourceFailed
                  : AssemblyWorkspacePrepareStatus::SessionFailed,
              {}};
    auto tree = AssemblyTreeModel::createTyped(
        product, epoch, session.session->publicationHandle(), nullptr,
        AssemblyTreeModelFailurePoint::None, std::nullopt, linkedStates);
    if (!tree)
      return {tree.status == AssemblyTreeModelCreateStatus::ResourceFailure
                  ? AssemblyWorkspacePrepareStatus::ResourceFailed
                  : AssemblyWorkspacePrepareStatus::TreeFailed,
              {}};
    auto projection = std::make_unique<PreparedAssemblyProjection>();
    projection->session = std::move(session.session);
    projection->treeModel = std::move(tree.model);
    projection->epoch = epoch;
    return {AssemblyWorkspacePrepareStatus::Prepared, std::move(projection)};
  } catch (const std::bad_alloc&) {
    return {AssemblyWorkspacePrepareStatus::ResourceFailed, {}};
  } catch (...) {
    return {AssemblyWorkspacePrepareStatus::PreparationFailed, {}};
  }
}

}  // namespace solidar
