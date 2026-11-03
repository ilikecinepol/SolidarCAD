#include "ui/AssemblyTreeModel.h"

#include <algorithm>
#include <exception>
#include <new>
#include <map>
#include <set>

#include "model/AssemblyGraphAnalysis.h"
#include "model/ProductDocument.h"

namespace solidar {
namespace {

QString occurrenceLabel(const AssemblyGraphVisit& occurrence,
                        AssemblyTreeIsolationState isolationState) {
  const OccurrenceFlags flags = occurrence.authoredFlags;
  QStringList states;
  states.push_back(flags.visible ? QString::fromUtf8("видимый")
                                 : QString::fromUtf8("скрытый"));
  if (flags.suppressed) states.push_back(QString::fromUtf8("подавлен"));
  if (flags.grounded) states.push_back(QString::fromUtf8("зафиксирован"));
  if (isolationState == AssemblyTreeIsolationState::Isolated)
    states.push_back(QString::fromUtf8("изолирован"));
  else if (isolationState == AssemblyTreeIsolationState::TemporarilyHidden)
    states.push_back(QString::fromUtf8("временно скрыт (изоляция)"));
  const QString authored = QString::fromUtf8(
      occurrence.displayNameUtf8.data(),
      static_cast<qsizetype>(occurrence.displayNameUtf8.size()));
  const OccurrenceId localId = occurrence.path.entries().back();
  const QString name = authored.isEmpty()
                           ? QString::fromUtf8("Экземпляр %1")
                                 .arg(localId.value())
                           : authored;
  const QString targetKind = occurrence.targetKind == DefinitionKind::Part
                                 ? QString::fromUtf8("деталь")
                                 : QString::fromUtf8("подсборка");
  return QString::fromUtf8("%1 → %2 %3 (%4)")
      .arg(name)
      .arg(targetKind)
      .arg(occurrence.targetDefinitionId.value())
      .arg(states.join(QStringLiteral(", ")));
}

QString linkedStatusLabel(LinkedPartRuntimeStatus status) {
  switch (status) {
    case LinkedPartRuntimeStatus::Unchecked:
      return QString::fromUtf8("связь не проверена");
    case LinkedPartRuntimeStatus::Exact:
      return QString::fromUtf8("связь актуальна");
    case LinkedPartRuntimeStatus::Missing:
      return QString::fromUtf8("источник не найден");
    case LinkedPartRuntimeStatus::Outdated:
      return QString::fromUtf8("источник изменён");
    case LinkedPartRuntimeStatus::IdentityMismatch:
      return QString::fromUtf8("неверный источник");
    case LinkedPartRuntimeStatus::SecurityRejected:
      return QString::fromUtf8("небезопасная ссылка");
    case LinkedPartRuntimeStatus::ResourceFailure:
      return QString::fromUtf8("проверка не завершена");
    case LinkedPartRuntimeStatus::InvalidSource:
      return QString::fromUtf8("источник повреждён");
  }
  return QString::fromUtf8("связь не проверена");
}

}  // namespace

AssemblyTreeModel::AssemblyTreeModel(QObject* parent)
    : QAbstractItemModel(parent) {}

AssemblyTreeModelCreateResult AssemblyTreeModel::createTyped(
    const ProductDocument& product, ProductPresentationEpoch epoch,
    AssemblyScenePublicationHandle publication,
    QObject* parent, AssemblyTreeModelFailurePoint failurePoint,
    std::optional<OccurrencePath> isolatedOccurrence,
    const LinkedPartRuntimeStates* linkedStates) noexcept {
  const auto snapshot = publication.snapshotIfCurrent();
  if (!epoch.isValid() || !snapshot || snapshot->epoch() != epoch)
    return {AssemblyTreeModelCreateStatus::Rejected, {}};
  const AssemblyDefinition* rootAssembly = product.rootAssemblyDefinition();
  if (!rootAssembly)
    return {AssemblyTreeModelCreateStatus::Rejected, {}};
  try {
    if (failurePoint == AssemblyTreeModelFailurePoint::ResourceFailure)
      throw std::bad_alloc();
    auto model = std::unique_ptr<AssemblyTreeModel>(
        new AssemblyTreeModel(parent));
    model->publication_ = std::move(publication);
    Node* root = model->addNode(
        {AssemblyTreeNodeKind::RootAssemblyDefinition,
         product.rootDefinitionId(), std::nullopt},
        QString::fromUtf8("Сборка %1")
            .arg(product.rootDefinitionId().value()),
        nullptr);
    if (!root) return {AssemblyTreeModelCreateStatus::Rejected, {}};

    const auto& graph = snapshot->graphAnalysis();
    if (!graph) return {AssemblyTreeModelCreateStatus::Rejected, {}};
    std::map<std::vector<OccurrenceId>, Node*> occurrenceNodes;
    std::set<DefinitionId> reachableDefinitions;
    reachableDefinitions.insert(product.rootDefinitionId());
    for (const auto& visit : graph.visits) {
      const auto resolved = product.resolveOccurrence(visit.path);
      if (!resolved ||
          resolved.occurrence->targetDefinitionId() !=
              visit.targetDefinitionId)
        return {AssemblyTreeModelCreateStatus::Rejected, {}};
      AssemblyGraphVisit currentVisit = visit;
      currentVisit.authoredFlags = resolved.occurrence->flags();
      currentVisit.displayNameUtf8 = resolved.occurrence->displayNameUtf8();
      reachableDefinitions.insert(visit.targetDefinitionId);
      AssemblyOccurrenceIdentity identity{
          epoch, product.rootDefinitionId(), visit.path};
      const auto isolationState =
          !isolatedOccurrence
              ? AssemblyTreeIsolationState::None
              : (*isolatedOccurrence == visit.path
                     ? AssemblyTreeIsolationState::Isolated
                     : AssemblyTreeIsolationState::TemporarilyHidden);
      Node* parentNode = root;
      if (visit.path.depth() > 1) {
        std::vector<OccurrenceId> parentPath = visit.path.entries();
        parentPath.pop_back();
        const auto found = occurrenceNodes.find(parentPath);
        if (found == occurrenceNodes.end())
          return {AssemblyTreeModelCreateStatus::Rejected, {}};
        parentNode = found->second;
      }
      Node* occurrenceNode = model->addNode(
              {AssemblyTreeNodeKind::Occurrence,
               visit.targetDefinitionId,
               AssemblyOccurrenceSelection{model->publication_,
                                           std::move(identity)}},
              occurrenceLabel(currentVisit, isolationState), parentNode);
      if (!occurrenceNode)
        return {AssemblyTreeModelCreateStatus::Rejected, {}};
      occurrenceNode->occurrenceFlags = currentVisit.authoredFlags;
      occurrenceNode->isolationState = isolationState;
      occurrenceNodes.emplace(visit.path.entries(), occurrenceNode);
    }
    for (const DefinitionId id : product.definitions().definitionIds()) {
      if (id == product.rootDefinitionId()) continue;
      const auto kind = product.definitions().kind(id);
      if (!kind) return {AssemblyTreeModelCreateStatus::Rejected, {}};
      const bool orphan = !reachableDefinitions.contains(id);
      const bool part = *kind == DefinitionKind::Part;
      QString definitionLabel = part
          ? (orphan
                 ? QString::fromUtf8("Деталь %1 (не используется)").arg(id.value())
                 : QString::fromUtf8("Деталь %1").arg(id.value()))
          : (orphan
                 ? QString::fromUtf8("Подсборка %1 (не используется)").arg(id.value())
                 : QString::fromUtf8("Подсборка %1").arg(id.value()));
      if (part && product.partLinkedBinding(id)) {
        LinkedPartRuntimeStatus status = LinkedPartRuntimeStatus::Unchecked;
        if (linkedStates) {
          const auto found = linkedStates->find(id);
          if (found != linkedStates->end()) status = found->second.status;
        }
        definitionLabel += QString::fromUtf8(" — %1").arg(
            linkedStatusLabel(status));
      }
      Node* definitionNode = model->addNode(
          {part ? AssemblyTreeNodeKind::PartDefinition
                : AssemblyTreeNodeKind::AssemblyDefinition,
           id, std::nullopt},
          std::move(definitionLabel),
          nullptr);
      if (!definitionNode)
        return {AssemblyTreeModelCreateStatus::Rejected, {}};
      definitionNode->orphanDefinition = orphan;
    }
    return {AssemblyTreeModelCreateStatus::Created, std::move(model)};
  } catch (const std::bad_alloc&) {
    return {AssemblyTreeModelCreateStatus::ResourceFailure, {}};
  } catch (...) {
    return {AssemblyTreeModelCreateStatus::Rejected, {}};
  }
}

std::unique_ptr<AssemblyTreeModel> AssemblyTreeModel::create(
    const ProductDocument& product, ProductPresentationEpoch epoch,
    AssemblyScenePublicationHandle publication, QObject* parent,
    std::optional<OccurrencePath> isolatedOccurrence,
    const LinkedPartRuntimeStates* linkedStates) noexcept {
  return createTyped(product, epoch, std::move(publication), parent,
                     AssemblyTreeModelFailurePoint::None,
                     isolatedOccurrence, linkedStates).model;
}

AssemblyTreeModel::Node* AssemblyTreeModel::addNode(
    AssemblyTreeNodeAddress address, QString label, Node* parentNode) {
  auto node = std::make_unique<Node>();
  node->address = std::move(address);
  node->label = std::move(label);
  node->parent = parentNode;
  Node* result = node.get();
  nodes_.push_back(std::move(node));
  if (parentNode)
    parentNode->children.push_back(result);
  else
    roots_.push_back(result);
  return result;
}

QModelIndex AssemblyTreeModel::index(int row, int column,
                                     const QModelIndex& parentIndex) const {
  if (column != 0 || row < 0) return {};
  const std::vector<Node*>* children = &roots_;
  if (parentIndex.isValid()) {
    if (parentIndex.column() != 0 || parentIndex.model() != this) return {};
    const auto* parentNode = static_cast<const Node*>(parentIndex.internalPointer());
    if (!parentNode) return {};
    children = &parentNode->children;
  }
  if (static_cast<std::size_t>(row) >= children->size()) return {};
  return createIndex(row, column, (*children)[static_cast<std::size_t>(row)]);
}

QModelIndex AssemblyTreeModel::parent(const QModelIndex& child) const {
  if (!child.isValid() || child.model() != this) return {};
  const auto* node = static_cast<const Node*>(child.internalPointer());
  if (!node || !node->parent) return {};
  return createIndex(rowOf(node->parent), 0, node->parent);
}

int AssemblyTreeModel::rowCount(const QModelIndex& parentIndex) const {
  if (parentIndex.isValid() && parentIndex.column() != 0) return 0;
  if (!parentIndex.isValid()) return static_cast<int>(roots_.size());
  if (parentIndex.model() != this) return 0;
  const auto* node = static_cast<const Node*>(parentIndex.internalPointer());
  return node ? static_cast<int>(node->children.size()) : 0;
}

int AssemblyTreeModel::columnCount(const QModelIndex&) const { return 1; }

QVariant AssemblyTreeModel::data(const QModelIndex& modelIndex, int role) const {
  if (!modelIndex.isValid() || modelIndex.model() != this ||
      modelIndex.column() != 0)
    return {};
  const auto* node = static_cast<const Node*>(modelIndex.internalPointer());
  if (!node) return {};
  if (role == Qt::DisplayRole) return node->label;
  if (role == kAssemblyTreeOrphanDefinitionRole)
    return node->orphanDefinition;
  if (!node->occurrenceFlags) return {};
  if (role == kAssemblyTreeAuthoredVisibleRole)
    return node->occurrenceFlags->visible;
  if (role == kAssemblyTreeAuthoredSuppressedRole)
    return node->occurrenceFlags->suppressed;
  if (role == kAssemblyTreeIsolationStateRole)
    return static_cast<int>(node->isolationState);
  if (role == kAssemblyTreeAuthoredGroundedRole)
    return node->occurrenceFlags->grounded;
  return {};
}

Qt::ItemFlags AssemblyTreeModel::flags(const QModelIndex& modelIndex) const {
  if (!modelIndex.isValid() || modelIndex.model() != this)
    return Qt::NoItemFlags;
  return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

bool AssemblyTreeModel::setData(const QModelIndex&, const QVariant&, int) {
  return false;
}

bool AssemblyTreeModel::insertRows(int, int, const QModelIndex&) { return false; }
bool AssemblyTreeModel::removeRows(int, int, const QModelIndex&) { return false; }
Qt::DropActions AssemblyTreeModel::supportedDropActions() const {
  return Qt::IgnoreAction;
}

AssemblyTreeModel::AddressView AssemblyTreeModel::addressForIndex(
    const QModelIndex& modelIndex) const noexcept {
  if (!modelIndex.isValid() || modelIndex.model() != this ||
      modelIndex.column() != 0)
    return std::nullopt;
  const auto* node = static_cast<const Node*>(modelIndex.internalPointer());
  if (!node) return std::nullopt;
  return std::cref(node->address);
}

QModelIndex AssemblyTreeModel::indexForOccurrence(
    const AssemblyOccurrenceSelection& occurrence) const noexcept {
  if (!occurrence.isCurrentFor(publication_)) return {};
  const auto found = std::find_if(
      nodes_.begin(), nodes_.end(), [&occurrence](const auto& node) {
        return node->address.kind == AssemblyTreeNodeKind::Occurrence &&
               node->address.occurrence &&
               node->address.occurrence->occurrence == occurrence.occurrence &&
               node->address.occurrence->isCurrentFor(
                   occurrence.publication);
      });
  if (found == nodes_.end()) return {};
  const Node* node = found->get();
  return createIndex(rowOf(node), 0, const_cast<Node*>(node));
}

QModelIndex AssemblyTreeModel::indexForDefinition(
    DefinitionId definitionId) const noexcept {
  if (!definitionId.isValid()) return {};
  for (const auto& node : nodes_) {
    if (node &&
        (node->address.kind == AssemblyTreeNodeKind::RootAssemblyDefinition ||
         node->address.kind == AssemblyTreeNodeKind::PartDefinition ||
         node->address.kind == AssemblyTreeNodeKind::AssemblyDefinition) &&
        node->address.definitionId == definitionId)
      return createIndex(rowOf(node.get()), 0, node.get());
  }
  return {};
}

int AssemblyTreeModel::rowOf(const Node* node) const noexcept {
  if (!node) return -1;
  const auto& siblings = node->parent ? node->parent->children : roots_;
  const auto found = std::find(siblings.begin(), siblings.end(), node);
  return found == siblings.end() ? -1
                                 : static_cast<int>(found - siblings.begin());
}

}  // namespace solidar
