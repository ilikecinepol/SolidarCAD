#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "project/ProjectPersistenceState.h"
#include "ui/ComponentTreeModel.h"
#include "ui/ComponentViewportSession.h"
#include "ui/application/ModelCommandHistory.h"
#include "ui/application/ProjectApplicationService.h"

namespace solidar {

enum class ComponentWorkspacePrepareStatus : std::uint8_t {
  Prepared,
  InvalidCarrier,
  SessionFailed,
  TreeFailed,
  ResourceFailed,
  PreparationFailed,
};

class PreparedComponentWorkspace final {
 public:
  ~PreparedComponentWorkspace() = default;
  PreparedComponentWorkspace(const PreparedComponentWorkspace&) = delete;
  PreparedComponentWorkspace& operator=(const PreparedComponentWorkspace&) =
      delete;
  PreparedComponentWorkspace(PreparedComponentWorkspace&&) noexcept = default;
  PreparedComponentWorkspace& operator=(PreparedComponentWorkspace&&) noexcept =
      default;

  [[nodiscard]] ProductDocument releaseProduct() noexcept {
    return std::move(product_);
  }
  [[nodiscard]] std::unique_ptr<ComponentViewportSession> releaseSession()
      noexcept {
    return std::move(session_);
  }
  [[nodiscard]] std::unique_ptr<ComponentTreeModel> releaseTreeModel() noexcept {
    return std::move(treeModel_);
  }
  [[nodiscard]] ProductPresentationEpoch epoch() const noexcept {
    return epoch_;
  }

  std::optional<project::ProjectPersistenceState> persistenceState;

 private:
  friend class ComponentWorkspacePreparer;
  PreparedComponentWorkspace(ProductDocument&& product,
                             std::unique_ptr<ComponentViewportSession> session,
                             std::unique_ptr<ComponentTreeModel> treeModel,
                             ProductPresentationEpoch epoch) noexcept;

  ProductDocument product_;
  std::unique_ptr<ComponentViewportSession> session_;
  std::unique_ptr<ComponentTreeModel> treeModel_;
  ProductPresentationEpoch epoch_;
};

// Off-side projection of a live candidate ProductDocument. It deliberately
// owns no ProductDocument and gives both QObjects no parent until a later
// atomic workspace install adopts them.
class PreparedComponentProjection final {
 public:
  ~PreparedComponentProjection() = default;
  PreparedComponentProjection(const PreparedComponentProjection&) = delete;
  PreparedComponentProjection& operator=(const PreparedComponentProjection&) =
      delete;
  PreparedComponentProjection(PreparedComponentProjection&&) noexcept =
      default;
  PreparedComponentProjection& operator=(PreparedComponentProjection&&)
      noexcept = default;

  [[nodiscard]] std::unique_ptr<ComponentViewportSession> releaseSession()
      noexcept {
    return std::move(session_);
  }
  [[nodiscard]] std::unique_ptr<ComponentTreeModel> releaseTreeModel() noexcept {
    return std::move(treeModel_);
  }
  [[nodiscard]] const ComponentWorkspaceSemanticState& semanticState()
      const noexcept {
    return semanticState_;
  }
  [[nodiscard]] ProductPresentationEpoch epoch() const noexcept {
    return epoch_;
  }
  [[nodiscard]] AssemblyMeshProfile meshProfile() const noexcept {
    return meshProfile_;
  }
  [[nodiscard]] const ComponentViewportSession* session() const noexcept {
    return session_.get();
  }
  [[nodiscard]] const ComponentTreeModel* treeModel() const noexcept {
    return treeModel_.get();
  }
  [[nodiscard]] const ComponentIdentityTransition* identityTransition()
      const noexcept {
    return identityTransition_ ? &*identityTransition_ : nullptr;
  }

 private:
  friend class ComponentProjectionPreparer;
  PreparedComponentProjection(
      std::unique_ptr<ComponentViewportSession> session,
      std::unique_ptr<ComponentTreeModel> treeModel,
      ComponentWorkspaceSemanticState semanticState,
      ProductPresentationEpoch epoch,
      AssemblyMeshProfile meshProfile,
      std::optional<ComponentIdentityTransition> identityTransition) noexcept;

  std::unique_ptr<ComponentViewportSession> session_;
  std::unique_ptr<ComponentTreeModel> treeModel_;
  ComponentWorkspaceSemanticState semanticState_;
  ProductPresentationEpoch epoch_;
  AssemblyMeshProfile meshProfile_;
  std::optional<ComponentIdentityTransition> identityTransition_;
};

struct PreparedComponentProjectionResult final {
  ComponentWorkspacePrepareStatus status{
      ComponentWorkspacePrepareStatus::PreparationFailed};
  std::unique_ptr<PreparedComponentProjection> projection;
  ComponentViewportSessionStatus sessionStatus{
      ComponentViewportSessionStatus::PreparationFailed};
  ComponentTreeModelCreateStatus treeStatus{
      ComponentTreeModelCreateStatus::Rejected};

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == ComponentWorkspacePrepareStatus::Prepared && projection;
  }
};

class ComponentProjectionPreparer final {
 public:
  [[nodiscard]] static PreparedComponentProjectionResult prepare(
      const ProductDocument& product, ProductPresentationEpoch epoch,
      AssemblyMeshProfile meshProfile,
      const ComponentWorkspaceSemanticState& semanticState,
      const std::optional<ComponentWorkspaceInstanceKey>& hoveredInstance,
      ComponentViewportSessionFailurePoint sessionFailurePoint =
          ComponentViewportSessionFailurePoint::None,
      ComponentTreeModelFailurePoint treeFailurePoint =
          ComponentTreeModelFailurePoint::None) noexcept;
  [[nodiscard]] static PreparedComponentProjectionResult prepare(
      const ProductDocument& product, ProductPresentationEpoch epoch,
      AssemblyMeshProfile meshProfile,
      const ComponentWorkspaceSemanticState& semanticState,
      ComponentViewportSessionFailurePoint sessionFailurePoint =
          ComponentViewportSessionFailurePoint::None,
      ComponentTreeModelFailurePoint treeFailurePoint =
          ComponentTreeModelFailurePoint::None) noexcept;
  [[nodiscard]] static PreparedComponentProjectionResult
  prepareProjectedIdentity(
      const ProductDocument& product, ProductPresentationEpoch epoch,
      AssemblyMeshProfile meshProfile,
      const ComponentWorkspaceSemanticState& semanticState,
      const std::optional<ComponentWorkspaceInstanceKey>& hoveredInstance,
      ComponentIdentityTransition transition,
      ComponentViewportSessionFailurePoint sessionFailurePoint =
          ComponentViewportSessionFailurePoint::None,
      ComponentTreeModelFailurePoint treeFailurePoint =
          ComponentTreeModelFailurePoint::None) noexcept;

 private:
  [[nodiscard]] static PreparedComponentProjectionResult prepareImpl(
      const ProductDocument& product, ProductPresentationEpoch epoch,
      AssemblyMeshProfile meshProfile,
      const ComponentWorkspaceSemanticState& semanticState,
      const std::optional<ComponentWorkspaceInstanceKey>& hoveredInstance,
      std::optional<ComponentIdentityTransition> transition,
      ComponentViewportSessionFailurePoint sessionFailurePoint,
      ComponentTreeModelFailurePoint treeFailurePoint) noexcept;
};

struct PreparedComponentWorkspaceResult final {
  ComponentWorkspacePrepareStatus status{
      ComponentWorkspacePrepareStatus::PreparationFailed};
  std::unique_ptr<PreparedComponentWorkspace> workspace;
  ComponentViewportSessionStatus sessionStatus{
      ComponentViewportSessionStatus::PreparationFailed};
  ComponentTreeModelCreateStatus treeStatus{
      ComponentTreeModelCreateStatus::Rejected};

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == ComponentWorkspacePrepareStatus::Prepared && workspace;
  }
};

class ComponentWorkspacePreparer final {
 public:
  [[nodiscard]] static PreparedComponentWorkspaceResult prepare(
      StagedApplicationDocument&& staged, ProductPresentationEpoch epoch,
      AssemblyMeshProfile meshProfile,
      ComponentViewportSessionFailurePoint sessionFailurePoint =
          ComponentViewportSessionFailurePoint::None,
      ComponentTreeModelFailurePoint treeFailurePoint =
          ComponentTreeModelFailurePoint::None) noexcept;
};

}  // namespace solidar
