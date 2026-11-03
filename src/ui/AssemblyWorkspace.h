#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "project/ProjectPersistenceState.h"
#include "ui/AssemblyTreeModel.h"
#include "ui/AssemblyViewportSession.h"
#include "ui/application/ProjectApplicationService.h"

namespace solidar {

enum class AssemblyWorkspacePrepareStatus : std::uint8_t {
  Prepared,
  InvalidCarrier,
  SessionFailed,
  TreeFailed,
  ResourceFailed,
  PreparationFailed,
};

class PreparedAssemblyWorkspace final {
 public:
  ~PreparedAssemblyWorkspace() = default;
  PreparedAssemblyWorkspace(const PreparedAssemblyWorkspace&) = delete;
  PreparedAssemblyWorkspace& operator=(const PreparedAssemblyWorkspace&) =
      delete;
  PreparedAssemblyWorkspace(PreparedAssemblyWorkspace&&) noexcept = default;
  PreparedAssemblyWorkspace& operator=(PreparedAssemblyWorkspace&&) noexcept =
      default;

  [[nodiscard]] ProductDocument releaseProduct() noexcept {
    return std::move(product_);
  }
  [[nodiscard]] std::unique_ptr<AssemblyViewportSession> releaseSession()
      noexcept {
    return std::move(session_);
  }
  [[nodiscard]] std::unique_ptr<AssemblyTreeModel> releaseTreeModel()
      noexcept {
    return std::move(treeModel_);
  }
  [[nodiscard]] ProductPresentationEpoch epoch() const noexcept {
    return epoch_;
  }

  std::optional<project::ProjectPersistenceState> persistenceState;
  LinkedPartRuntimeStates linkedPartRuntimeStates;
  std::vector<project::MateRuntimeDiagnostics> mateRuntimeDiagnostics;

 private:
  friend struct PreparedAssemblyWorkspaceResult;
  friend class AssemblyWorkspacePreparer;
  PreparedAssemblyWorkspace(ProductDocument&& product,
                             std::unique_ptr<AssemblyViewportSession> session,
                             std::unique_ptr<AssemblyTreeModel> treeModel,
                             ProductPresentationEpoch epoch) noexcept;

  ProductDocument product_;
  std::unique_ptr<AssemblyViewportSession> session_;
  std::unique_ptr<AssemblyTreeModel> treeModel_;
  ProductPresentationEpoch epoch_;
};

struct PreparedAssemblyWorkspaceResult final {
  AssemblyWorkspacePrepareStatus status{
      AssemblyWorkspacePrepareStatus::PreparationFailed};
  std::unique_ptr<PreparedAssemblyWorkspace> workspace;
  AssemblyViewportSessionStatus sessionStatus{
      AssemblyViewportSessionStatus::PreparationFailed};

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == AssemblyWorkspacePrepareStatus::Prepared &&
           workspace != nullptr;
  }
};

class AssemblyWorkspacePreparer final {
 public:
  [[nodiscard]] static PreparedAssemblyWorkspaceResult prepare(
      StagedApplicationDocument&& staged, ProductPresentationEpoch epoch,
      AssemblyMeshProfile meshProfile,
      AssemblyViewportSessionFailurePoint failurePoint =
          AssemblyViewportSessionFailurePoint::None) noexcept;
};

struct PreparedAssemblyProjection final {
  std::unique_ptr<AssemblyViewportSession> session;
  std::unique_ptr<AssemblyTreeModel> treeModel;
  ProductPresentationEpoch epoch;
  // Resolved against treeModel while the successor is still preparing.
  // The candidate session already contains the corresponding occurrence
  // selection, so installation after the model commit is allocation-free.
  QModelIndex selectionIndex;
};

struct PreparedAssemblyProjectionResult final {
  AssemblyWorkspacePrepareStatus status{
      AssemblyWorkspacePrepareStatus::PreparationFailed};
  std::unique_ptr<PreparedAssemblyProjection> projection;
  [[nodiscard]] explicit operator bool() const noexcept {
    return status == AssemblyWorkspacePrepareStatus::Prepared &&
           projection != nullptr;
  }
};

class AssemblyProjectionPreparer final {
 public:
  [[nodiscard]] static PreparedAssemblyProjectionResult prepare(
      const ProductDocument& product, ProductPresentationEpoch epoch,
      AssemblyMeshProfile meshProfile,
      AssemblyViewportSessionFailurePoint failurePoint =
          AssemblyViewportSessionFailurePoint::None,
      const LinkedPartRuntimeStates* linkedStates = nullptr) noexcept;
};

}  // namespace solidar
