#pragma once

#include <QAbstractItemModel>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "ui/AssemblyOccurrenceSelection.h"
#include "ui/application/ProjectApplicationService.h"

namespace solidar {

class AssemblyTreeModel;

enum class AssemblyTreeModelCreateStatus {
  Created,
  Rejected,
  ResourceFailure,
};

enum class AssemblyTreeModelFailurePoint {
  None,
  ResourceFailure,
};

struct AssemblyTreeModelCreateResult final {
  AssemblyTreeModelCreateStatus status{AssemblyTreeModelCreateStatus::Rejected};
  std::unique_ptr<AssemblyTreeModel> model;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == AssemblyTreeModelCreateStatus::Created && model != nullptr;
  }
};

enum class AssemblyTreeNodeKind : std::uint8_t {
  RootAssemblyDefinition,
  PartDefinition,
  AssemblyDefinition,
  Occurrence,
};

enum class AssemblyTreeIsolationState : std::uint8_t {
  None,
  Isolated,
  TemporarilyHidden,
};

inline constexpr int kAssemblyTreeAuthoredVisibleRole = Qt::UserRole + 1;
inline constexpr int kAssemblyTreeAuthoredSuppressedRole = Qt::UserRole + 2;
inline constexpr int kAssemblyTreeIsolationStateRole = Qt::UserRole + 3;
inline constexpr int kAssemblyTreeAuthoredGroundedRole = Qt::UserRole + 4;
inline constexpr int kAssemblyTreeOrphanDefinitionRole = Qt::UserRole + 5;

struct AssemblyTreeNodeAddress final {
  AssemblyTreeNodeKind kind{AssemblyTreeNodeKind::PartDefinition};
  DefinitionId definitionId;
  std::optional<AssemblyOccurrenceSelection> occurrence;
};

// Immutable projection of ProductDocument. QModelIndex values and item text
// are presentation-only: every interaction must resolve through a typed
// address copied into stable node storage.
class AssemblyTreeModel final : public QAbstractItemModel {
  Q_OBJECT

 public:
  [[nodiscard]] static AssemblyTreeModelCreateResult createTyped(
      const ProductDocument& product, ProductPresentationEpoch epoch,
      AssemblyScenePublicationHandle publication,
      QObject* parent = nullptr,
      AssemblyTreeModelFailurePoint failurePoint =
          AssemblyTreeModelFailurePoint::None,
      std::optional<OccurrencePath> isolatedOccurrence = std::nullopt,
      const LinkedPartRuntimeStates* linkedStates = nullptr) noexcept;
  [[nodiscard]] static std::unique_ptr<AssemblyTreeModel> create(
      const ProductDocument& product, ProductPresentationEpoch epoch,
      AssemblyScenePublicationHandle publication,
      QObject* parent = nullptr,
      std::optional<OccurrencePath> isolatedOccurrence = std::nullopt,
      const LinkedPartRuntimeStates* linkedStates = nullptr) noexcept;

  ~AssemblyTreeModel() override = default;
  AssemblyTreeModel(const AssemblyTreeModel&) = delete;
  AssemblyTreeModel& operator=(const AssemblyTreeModel&) = delete;

  [[nodiscard]] QModelIndex index(int row, int column,
                                  const QModelIndex& parent = {}) const override;
  [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
  [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] int columnCount(const QModelIndex& = {}) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index,
                              int role = Qt::DisplayRole) const override;
  [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
  [[nodiscard]] bool setData(const QModelIndex&, const QVariant&,
                             int = Qt::EditRole) override;
  [[nodiscard]] bool insertRows(int, int, const QModelIndex& = {}) override;
  [[nodiscard]] bool removeRows(int, int, const QModelIndex& = {}) override;
  [[nodiscard]] Qt::DropActions supportedDropActions() const override;

  // Non-owning immutable view. It remains valid only while this immutable
  // model lives; unlike the old value return it never copies OccurrencePath
  // storage from a noexcept selection callback.
  using AddressView =
      std::optional<std::reference_wrapper<const AssemblyTreeNodeAddress>>;
  [[nodiscard]] AddressView addressForIndex(
      const QModelIndex& index) const noexcept;
  [[nodiscard]] QModelIndex indexForOccurrence(
      const AssemblyOccurrenceSelection& occurrence) const noexcept;
  [[nodiscard]] QModelIndex indexForDefinition(
      DefinitionId definitionId) const noexcept;

 private:
  struct Node final {
    AssemblyTreeNodeAddress address;
    QString label;
    std::optional<OccurrenceFlags> occurrenceFlags;
    bool orphanDefinition{};
    AssemblyTreeIsolationState isolationState{
        AssemblyTreeIsolationState::None};
    Node* parent{};
    std::vector<Node*> children;
  };

  explicit AssemblyTreeModel(QObject* parent = nullptr);
  [[nodiscard]] Node* addNode(AssemblyTreeNodeAddress address, QString label,
                              Node* parent);
  [[nodiscard]] int rowOf(const Node* node) const noexcept;

  std::vector<std::unique_ptr<Node>> nodes_;
  std::vector<Node*> roots_;
  AssemblyScenePublicationHandle publication_;
};

}  // namespace solidar
