#pragma once

#include <QAbstractItemModel>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "model/Document.h"
#include "presentation/ComponentScenePublicationCoordinator.h"

namespace solidar {

class ProductDocument;
class ComponentTreeModel;

enum class ComponentTreeModelCreateStatus : std::uint8_t {
  Created,
  Rejected,
  NodeLimitExceeded,
  CountOverflow,
  ResourceFailure,
};

enum class ComponentTreeModelFailurePoint : std::uint8_t {
  None,
  ResourceFailure,
};

struct ComponentTreeBuildLimits final {
  static constexpr std::size_t kDefaultMaximumNodes = 1'000'000;
  std::size_t maximumNodes{kDefaultMaximumNodes};
};

enum class ComponentTreeGroupKind : std::uint8_t {
  Origin,
  Bodies,
  Sketches,
  Images,
  Drawings,
};

enum class ComponentTreeOriginPlane : std::uint8_t { XY, XZ, YZ };

struct ComponentTreeRootAddress final {
  ComponentInstanceAddress instance;
  friend bool operator==(const ComponentTreeRootAddress&,
                         const ComponentTreeRootAddress&) = default;
};

struct ComponentTreeOccurrenceAddress final {
  ComponentInstanceAddress instance;
  friend bool operator==(const ComponentTreeOccurrenceAddress&,
                         const ComponentTreeOccurrenceAddress&) = default;
};

struct ComponentTreeGroupAddress final {
  ComponentInstanceAddress instance;
  ComponentTreeGroupKind group{ComponentTreeGroupKind::Origin};
  friend bool operator==(const ComponentTreeGroupAddress&,
                         const ComponentTreeGroupAddress&) = default;
};

struct ComponentTreeOriginPlaneAddress final {
  ComponentInstanceAddress instance;
  ComponentTreeOriginPlane plane{ComponentTreeOriginPlane::XY};
  friend bool operator==(const ComponentTreeOriginPlaneAddress&,
                         const ComponentTreeOriginPlaneAddress&) = default;
};

struct ComponentTreeBodyAddress final {
  ComponentBodyIdentity body;
  friend bool operator==(const ComponentTreeBodyAddress&,
                         const ComponentTreeBodyAddress&) = default;
};

struct ComponentTreeSketchAddress final {
  ComponentInstanceAddress instance;
  SketchId sketchId{kInvalidSketchId};
  friend bool operator==(const ComponentTreeSketchAddress&,
                         const ComponentTreeSketchAddress&) = default;
};

struct ComponentTreeReferenceImageAddress final {
  ComponentInstanceAddress instance;
  ReferenceImageId imageId{kInvalidReferenceImageId};
  friend bool operator==(const ComponentTreeReferenceImageAddress&,
                         const ComponentTreeReferenceImageAddress&) = default;
};

struct ComponentTreeDrawingAddress final {
  ComponentInstanceAddress instance;
  DrawingId drawingId{kInvalidDrawingId};
  friend bool operator==(const ComponentTreeDrawingAddress&,
                         const ComponentTreeDrawingAddress&) = default;
};

struct ComponentTreeUnusedSectionAddress final {
  ProductPresentationEpoch epoch;
  DefinitionId rootDefinitionId;
  friend bool operator==(const ComponentTreeUnusedSectionAddress&,
                         const ComponentTreeUnusedSectionAddress&) = default;
};

struct ComponentTreeUnusedDefinitionAddress final {
  ProductPresentationEpoch epoch;
  DefinitionId rootDefinitionId;
  DefinitionId definitionId;
  friend bool operator==(const ComponentTreeUnusedDefinitionAddress&,
                         const ComponentTreeUnusedDefinitionAddress&) = default;
};

using ComponentTreeAddressValue = std::variant<
    ComponentTreeRootAddress, ComponentTreeOccurrenceAddress,
    ComponentTreeGroupAddress, ComponentTreeOriginPlaneAddress,
    ComponentTreeBodyAddress, ComponentTreeSketchAddress,
    ComponentTreeReferenceImageAddress, ComponentTreeDrawingAddress,
    ComponentTreeUnusedSectionAddress, ComponentTreeUnusedDefinitionAddress>;

class ComponentTreeNodeAddress final {
 public:
  [[nodiscard]] const ComponentTreeAddressValue& value() const noexcept {
    return value_;
  }
  [[nodiscard]] const ComponentScenePublicationHandle& publication()
      const noexcept {
    return publication_;
  }

 private:
  friend class ComponentTreeModel;

  ComponentTreeNodeAddress(ComponentScenePublicationHandle publication,
                           ComponentTreeAddressValue value) noexcept
      : publication_(std::move(publication)), value_(std::move(value)) {}

  ComponentScenePublicationHandle publication_;
  ComponentTreeAddressValue value_;
};

struct ComponentTreeModelCreateResult final {
  ComponentTreeModelCreateStatus status{
      ComponentTreeModelCreateStatus::Rejected};
  std::unique_ptr<ComponentTreeModel> model;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == ComponentTreeModelCreateStatus::Created && model != nullptr;
  }
};

inline constexpr int kComponentTreeAuthoredVisibleRole = Qt::UserRole + 1;
inline constexpr int kComponentTreeAuthoredSuppressedRole = Qt::UserRole + 2;
inline constexpr int kComponentTreeAuthoredGroundedRole = Qt::UserRole + 3;
inline constexpr int kComponentTreeEffectiveVisibleRole = Qt::UserRole + 4;
inline constexpr int kComponentTreeEffectiveSuppressedRole = Qt::UserRole + 5;
inline constexpr int kComponentTreeBodyVisibleRole = Qt::UserRole + 6;
inline constexpr int kComponentTreeImageVisibleRole = Qt::UserRole + 7;
inline constexpr int kComponentTreeUnusedDefinitionRole = Qt::UserRole + 8;
inline constexpr int kComponentTreeActiveRole = Qt::UserRole + 9;

// Immutable projection of one exact component-scene publication. Nodes own
// only copied typed addresses, labels and scalar roles; no ProductDocument or
// model entity pointer survives construction.
class ComponentTreeModel final : public QAbstractItemModel {
  Q_OBJECT

 public:
  [[nodiscard]] static ComponentTreeModelCreateResult createTyped(
      const ProductDocument& product,
      ComponentScenePublicationHandle publication,
      QObject* parent = nullptr,
      ComponentTreeModelFailurePoint failurePoint =
          ComponentTreeModelFailurePoint::None,
      ComponentTreeBuildLimits limits = {}) noexcept;
  [[nodiscard]] static ComponentTreeModelCreateResult
  createTypedProjectedIdentity(
      const ProductDocument& product,
      ComponentScenePublicationHandle publication,
      const ComponentIdentityTransition& transition,
      QObject* parent = nullptr,
      ComponentTreeModelFailurePoint failurePoint =
          ComponentTreeModelFailurePoint::None,
      ComponentTreeBuildLimits limits = {}) noexcept;
  [[nodiscard]] static std::unique_ptr<ComponentTreeModel> create(
      const ProductDocument& product,
      ComponentScenePublicationHandle publication,
      QObject* parent = nullptr,
      ComponentTreeBuildLimits limits = {}) noexcept;

  ~ComponentTreeModel() override = default;
  ComponentTreeModel(const ComponentTreeModel&) = delete;
  ComponentTreeModel& operator=(const ComponentTreeModel&) = delete;

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
  [[nodiscard]] Qt::DropActions supportedDragActions() const override;

  using AddressView =
      std::optional<std::reference_wrapper<const ComponentTreeNodeAddress>>;
  [[nodiscard]] AddressView addressForIndex(
      const QModelIndex& index) const noexcept;
  [[nodiscard]] QModelIndex indexForAddress(
      const ComponentTreeNodeAddress& address) const noexcept;
  void setRootDisplayName(const QString& name);
  void setRootVisible(bool visible);
  void setActiveComponent(DefinitionId definitionId,
                          const std::optional<OccurrencePath>& path);
  void clearActiveComponent();

 signals:
  void componentVisibilityChangeRequested(
      solidar::ComponentInstanceAddress instance, bool visible);
  void nodeVisibilityChangeRequested(
      solidar::ComponentTreeAddressValue address, bool visible);
  void nodeRenameRequested(solidar::ComponentTreeAddressValue address,
                           QString name);

 private:
  [[nodiscard]] static ComponentTreeModelCreateResult createTypedImpl(
      const ProductDocument& product,
      ComponentScenePublicationHandle publication,
      const ComponentIdentityTransition* transition,
      QObject* parent, ComponentTreeModelFailurePoint failurePoint,
      ComponentTreeBuildLimits limits) noexcept;
  struct Node final {
    ComponentTreeNodeAddress address;
    QString label;
    QString editLabel;
    QString toolTip;
    std::optional<OccurrenceFlags> authoredFlags;
    std::optional<bool> effectiveVisible;
    std::optional<bool> effectiveSuppressed;
    std::optional<bool> bodyVisible;
    std::optional<bool> imageVisible;
    std::optional<bool> componentVisible;
    std::optional<bool> localVisible;
    bool unusedDefinition{};
    bool activeComponent{};
    bool renameable{};
    Node* parent{};
    std::vector<Node*> children;
  };

  explicit ComponentTreeModel(QObject* parent = nullptr);
  [[nodiscard]] Node* addNode(ComponentTreeAddressValue address,
                              QString label, Node* parent);
  [[nodiscard]] int rowOf(const Node* node) const noexcept;
  [[nodiscard]] Qt::CheckState checkState(const Node* node) const noexcept;
  void setLocalVisibility(Node* node, bool visible);
  void notifyCheckStateAncestors(Node* node);

  std::vector<std::unique_ptr<Node>> nodes_;
  std::vector<Node*> roots_;
  ComponentScenePublicationHandle publication_;
};

}  // namespace solidar
