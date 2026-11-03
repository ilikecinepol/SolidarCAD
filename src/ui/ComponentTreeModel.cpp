#include "ui/ComponentTreeModel.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <string_view>
#include <utility>

#include <QFont>

#include "model/ProductDocument.h"
#include "presentation/ComponentPresentationBuilder.h"

namespace solidar {
namespace {

bool checkedAdd(std::size_t& value, std::size_t increment) noexcept {
  if (increment > std::numeric_limits<std::size_t>::max() - value)
    return false;
  value += increment;
  return true;
}

QString utf8(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString occurrenceLabel(const ComponentGraphVisit& visit,
                        std::string_view authoredName) {
  const OccurrenceId localId = visit.path->entries().back();
  const QString name = authoredName.empty()
                           ? QString::fromUtf8("Компонент %1")
                                 .arg(visit.definitionId.value())
                           : utf8(authoredName);
  QStringList states;
  states.push_back(visit.authoredFlags.visible
                       ? QString::fromUtf8("видимый")
                       : QString::fromUtf8("скрытый"));
  if (visit.authoredFlags.suppressed)
    states.push_back(QString::fromUtf8("подавлен"));
  if (visit.authoredFlags.grounded)
    states.push_back(QString::fromUtf8("зафиксирован"));
  if (!visit.effectiveVisible)
    states.push_back(QString::fromUtf8("эффективно скрыт"));
  return QString::fromUtf8("%1 (экземпляр %2, %3)")
      .arg(name)
      .arg(localId.value())
      .arg(states.join(QStringLiteral(", ")));
}

QString bodyLabel(const ComponentTreeBodyProjection& body) {
  return body.nameUtf8.empty()
             ? QString::fromUtf8("Тело %1").arg(body.id)
             : utf8(body.nameUtf8);
}

QString sketchLabel(const ComponentTreeSketchProjection& sketch) {
  return sketch.nameUtf8.empty()
             ? QString::fromUtf8("Эскиз %1").arg(sketch.id)
             : utf8(sketch.nameUtf8);
}

QString imageLabel(const ComponentTreeImageProjection& image) {
  return image.nameUtf8.empty()
             ? QString::fromUtf8("Изображение %1").arg(image.id)
             : utf8(image.nameUtf8);
}

QString drawingLabel(const ComponentTreeDrawingProjection& drawing) {
  return drawing.nameUtf8.empty()
             ? QString::fromUtf8("Чертеж %1").arg(drawing.id.value())
             : utf8(drawing.nameUtf8);
}

QString drawingToolTip(const ComponentTreeDrawingProjection& drawing) {
  const QString orientation =
      drawing.orientation == ComponentDrawingOrientation::Portrait
          ? QString::fromUtf8("книжная")
          : QString::fromUtf8("альбомная");
  const QString source = drawing.sourceBodyIds.empty()
                             ? QString::fromUtf8("все тела")
                             : QString::fromUtf8("тел: %1")
                                   .arg(drawing.sourceBodyIds.size());
  return QString::fromUtf8("A4, %1, масштаб %2, %3")
      .arg(orientation,
           QString::number(drawing.scale, 'g', 15), source);
}

std::vector<OccurrenceId> parentPath(const OccurrencePath& path) {
  std::vector<OccurrenceId> result = path.entries();
  if (!result.empty()) result.pop_back();
  return result;
}

}  // namespace

ComponentTreeModel::ComponentTreeModel(QObject* parent)
    : QAbstractItemModel(parent) {}

ComponentTreeModelCreateResult ComponentTreeModel::createTyped(
    const ProductDocument& product,
    ComponentScenePublicationHandle publication,
    QObject* parent, ComponentTreeModelFailurePoint failurePoint,
    ComponentTreeBuildLimits limits) noexcept {
  return createTypedImpl(product, std::move(publication), nullptr, parent,
                         failurePoint, limits);
}

ComponentTreeModelCreateResult
ComponentTreeModel::createTypedProjectedIdentity(
    const ProductDocument& product,
    ComponentScenePublicationHandle publication,
    const ComponentIdentityTransition& transition,
    QObject* parent, ComponentTreeModelFailurePoint failurePoint,
    ComponentTreeBuildLimits limits) noexcept {
  return createTypedImpl(product, std::move(publication), &transition,
                         parent, failurePoint, limits);
}

ComponentTreeModelCreateResult ComponentTreeModel::createTypedImpl(
    const ProductDocument& product,
    ComponentScenePublicationHandle publication,
    const ComponentIdentityTransition* transition,
    QObject* parent, ComponentTreeModelFailurePoint failurePoint,
    ComponentTreeBuildLimits limits) noexcept {
  const auto snapshot = publication.snapshotIfCurrent();
  if (!snapshot || !publication.isCurrent() ||
      !snapshot->epoch().isValid() || !product.rootComponentDefinition())
    return {ComponentTreeModelCreateStatus::Rejected, {}};
  const auto& traversal = snapshot->graphTraversal();
  if (!traversal || traversal.visits.empty() ||
      !traversal.visits.front().rootInstance() ||
      product.rootDefinitionId() != traversal.visits.front().definitionId)
    return {ComponentTreeModelCreateStatus::Rejected, {}};
  const auto currentProjection =
      transition
          ? ComponentPresentationBuilder::buildTreeProjection(product,
                                                               *transition)
          : ComponentPresentationBuilder::buildTreeProjection(product);
  if (!currentProjection)
    return {currentProjection.status ==
                    ComponentTreeProjectionStatus::ResourceFailure
                ? ComponentTreeModelCreateStatus::ResourceFailure
                : ComponentTreeModelCreateStatus::Rejected,
            {}};
  if (*currentProjection.projection != snapshot->treeProjection())
    return {ComponentTreeModelCreateStatus::Rejected, {}};
  const auto& treeProjection = snapshot->treeProjection();

  try {
    std::size_t expectedNodes = 0;
    std::set<DefinitionId> reachable;
    for (const auto& visit : traversal.visits) {
      if (!visit.definitionId.isValid())
        return {ComponentTreeModelCreateStatus::Rejected, {}};
      const auto* definition =
          treeProjection.findDefinition(visit.definitionId);
      if (!definition)
        return {ComponentTreeModelCreateStatus::Rejected, {}};
      // Instance + five fixed sections + three origin planes.
      if (!checkedAdd(expectedNodes, 9) ||
          !checkedAdd(expectedNodes, definition->bodies.size()) ||
          !checkedAdd(expectedNodes, definition->sketches.size()) ||
          !checkedAdd(expectedNodes, definition->images.size()) ||
          !checkedAdd(expectedNodes, definition->drawings.size()))
        return {ComponentTreeModelCreateStatus::CountOverflow, {}};
      reachable.insert(visit.definitionId);
    }
    std::size_t unusedCount = 0;
    for (const auto& definition : treeProjection.definitions) {
      if (!reachable.contains(definition.id)) ++unusedCount;
    }
    if (unusedCount > 0 &&
        (!checkedAdd(expectedNodes, 1) ||
         !checkedAdd(expectedNodes, unusedCount)))
      return {ComponentTreeModelCreateStatus::CountOverflow, {}};
    if (expectedNodes > limits.maximumNodes)
      return {ComponentTreeModelCreateStatus::NodeLimitExceeded, {}};
    if (failurePoint == ComponentTreeModelFailurePoint::ResourceFailure)
      throw std::bad_alloc();

    auto model = std::unique_ptr<ComponentTreeModel>(
        new ComponentTreeModel(parent));
    model->publication_ = std::move(publication);
    model->nodes_.reserve(expectedNodes);
    model->roots_.reserve(unusedCount > 0 ? 2 : 1);

    std::map<std::vector<OccurrenceId>, Node*> instances;
    const auto addInstanceContents = [&](Node* instanceNode,
                                         const ComponentGraphVisit& visit,
                                         const ComponentTreeDefinitionProjection&
                                             definition) {
      const ComponentInstanceAddress instanceAddress{
          snapshot->epoch(), product.rootDefinitionId(), visit.definitionId,
          visit.path};
      const auto addInstanceNode = [&](ComponentTreeAddressValue address,
                                       QString label, Node* parentNode) {
        Node* node = model->addNode(std::move(address), std::move(label),
                                   parentNode);
        node->effectiveVisible = visit.effectiveVisible;
        node->effectiveSuppressed = visit.effectiveSuppressed;
        return node;
      };

      Node* origin = addInstanceNode(
          ComponentTreeGroupAddress{instanceAddress,
                                    ComponentTreeGroupKind::Origin},
          QString::fromUtf8("Начало координат"), instanceNode);
      origin->localVisible = true;
      addInstanceNode(ComponentTreeOriginPlaneAddress{
                          instanceAddress, ComponentTreeOriginPlane::XY},
                      QString::fromUtf8("Плоскость XY"), origin)
          ->localVisible = true;
      addInstanceNode(ComponentTreeOriginPlaneAddress{
                          instanceAddress, ComponentTreeOriginPlane::XZ},
                      QString::fromUtf8("Плоскость XZ"), origin)
          ->localVisible = true;
      addInstanceNode(ComponentTreeOriginPlaneAddress{
                          instanceAddress, ComponentTreeOriginPlane::YZ},
                      QString::fromUtf8("Плоскость YZ"), origin)
          ->localVisible = true;

      Node* bodies = addInstanceNode(
          ComponentTreeGroupAddress{instanceAddress,
                                    ComponentTreeGroupKind::Bodies},
          QString::fromUtf8("Тела"), instanceNode);
      bodies->localVisible = true;
      for (const auto& body : definition.bodies) {
        Node* node = addInstanceNode(
            ComponentTreeBodyAddress{{instanceAddress, body.id}},
            bodyLabel(body), bodies);
        node->editLabel = bodyLabel(body);
        node->renameable = true;
        node->bodyVisible = body.visible;
      }

      Node* sketches = addInstanceNode(
          ComponentTreeGroupAddress{instanceAddress,
                                    ComponentTreeGroupKind::Sketches},
          QString::fromUtf8("Эскизы"), instanceNode);
      sketches->localVisible = true;
      for (const auto& sketch : definition.sketches) {
        Node* node = addInstanceNode(
            ComponentTreeSketchAddress{instanceAddress, sketch.id},
            sketchLabel(sketch), sketches);
        node->editLabel = sketchLabel(sketch);
        node->renameable = true;
        node->localVisible = true;
      }

      Node* images = addInstanceNode(
          ComponentTreeGroupAddress{instanceAddress,
                                    ComponentTreeGroupKind::Images},
          QString::fromUtf8("Изображения"), instanceNode);
      images->localVisible = true;
      for (const auto& image : definition.images) {
        Node* node = addInstanceNode(
            ComponentTreeReferenceImageAddress{instanceAddress, image.id},
            imageLabel(image), images);
        node->editLabel = imageLabel(image);
        node->renameable = true;
        node->imageVisible = image.visible;
      }

      Node* drawings = addInstanceNode(
          ComponentTreeGroupAddress{instanceAddress,
                                    ComponentTreeGroupKind::Drawings},
          QString::fromUtf8("Чертежи"), instanceNode);
      drawings->localVisible = true;
      for (const auto& drawing : definition.drawings) {
        Node* node = addInstanceNode(
            ComponentTreeDrawingAddress{instanceAddress, drawing.id},
            drawingLabel(drawing), drawings);
        node->toolTip = drawingToolTip(drawing);
        node->localVisible = true;
      }

    };

    for (const auto& visit : traversal.visits) {
      Node* parentNode = nullptr;
      std::string_view authoredName;
      if (visit.rootInstance()) {
        if (!instances.empty())
          return {ComponentTreeModelCreateStatus::Rejected, {}};
      } else {
        const std::vector<OccurrenceId> ownerPath = parentPath(*visit.path);
        const auto parentInstance = instances.find(ownerPath);
        if (parentInstance == instances.end())
          return {ComponentTreeModelCreateStatus::Rejected, {}};
        parentNode = parentInstance->second;
        const auto* ownerAddress =
            std::get_if<ComponentTreeRootAddress>(
                &parentInstance->second->address.value());
        DefinitionId ownerDefinitionId;
        if (ownerAddress) {
          ownerDefinitionId = ownerAddress->instance.definitionId;
        } else {
          const auto* occurrenceAddress =
              std::get_if<ComponentTreeOccurrenceAddress>(
                  &parentInstance->second->address.value());
          if (!occurrenceAddress)
            return {ComponentTreeModelCreateStatus::Rejected, {}};
          ownerDefinitionId = occurrenceAddress->instance.definitionId;
        }
        const auto* owner =
            treeProjection.findDefinition(ownerDefinitionId);
        if (!owner)
          return {ComponentTreeModelCreateStatus::Rejected, {}};
        const auto occurrence = std::find_if(
            owner->occurrences.begin(), owner->occurrences.end(),
            [&visit](const ComponentTreeOccurrenceProjection& value) {
              return value.id == visit.path->entries().back();
            });
        if (occurrence == owner->occurrences.end() ||
            occurrence->targetDefinitionId != visit.definitionId ||
            occurrence->flags != visit.authoredFlags)
          return {ComponentTreeModelCreateStatus::Rejected, {}};
        authoredName = occurrence->displayNameUtf8;
      }

      const ComponentInstanceAddress instanceAddress{
          snapshot->epoch(), product.rootDefinitionId(), visit.definitionId,
          visit.path};
      Node* instanceNode = model->addNode(
          visit.rootInstance()
              ? ComponentTreeAddressValue{
                    ComponentTreeRootAddress{instanceAddress}}
              : ComponentTreeAddressValue{
                    ComponentTreeOccurrenceAddress{instanceAddress}},
          visit.rootInstance()
              ? QString::fromUtf8("Новый проект")
              : occurrenceLabel(visit, authoredName),
          parentNode);
      if (!visit.rootInstance()) {
        instanceNode->editLabel =
            authoredName.empty()
                ? QString::fromUtf8("Компонент %1")
                      .arg(visit.definitionId.value())
                : utf8(authoredName);
        instanceNode->renameable = true;
      }
      instanceNode->effectiveVisible = visit.effectiveVisible;
      instanceNode->effectiveSuppressed = visit.effectiveSuppressed;
      instanceNode->componentVisible = visit.rootInstance()
                                             ? true
                                             : visit.authoredFlags.visible;
      if (!visit.rootInstance())
        instanceNode->authoredFlags = visit.authoredFlags;
      const auto* definition =
          treeProjection.findDefinition(visit.definitionId);
      if (!definition)
        return {ComponentTreeModelCreateStatus::Rejected, {}};
      addInstanceContents(instanceNode, visit, *definition);
      const std::vector<OccurrenceId> key =
          visit.path ? visit.path->entries() : std::vector<OccurrenceId>{};
      if (!instances.emplace(key, instanceNode).second)
        return {ComponentTreeModelCreateStatus::Rejected, {}};
    }

    if (unusedCount > 0) {
      Node* unused = model->addNode(
          ComponentTreeUnusedSectionAddress{
              snapshot->epoch(), product.rootDefinitionId()},
          QString::fromUtf8("Неиспользуемые компоненты"), nullptr);
      unused->localVisible = true;
      for (const auto& definition : treeProjection.definitions) {
        const DefinitionId id = definition.id;
        if (reachable.contains(id)) continue;
        Node* node = model->addNode(
            ComponentTreeUnusedDefinitionAddress{
                snapshot->epoch(), product.rootDefinitionId(), id},
            QString::fromUtf8("Компонент %1").arg(id.value()), unused);
        node->unusedDefinition = true;
        node->localVisible = true;
      }
    }

    if (!model->publication_.isCurrent() ||
        model->nodes_.size() != expectedNodes)
      return {ComponentTreeModelCreateStatus::Rejected, {}};
    return {ComponentTreeModelCreateStatus::Created, std::move(model)};
  } catch (const std::bad_alloc&) {
    return {ComponentTreeModelCreateStatus::ResourceFailure, {}};
  } catch (...) {
    return {ComponentTreeModelCreateStatus::Rejected, {}};
  }
}

std::unique_ptr<ComponentTreeModel> ComponentTreeModel::create(
    const ProductDocument& product,
    ComponentScenePublicationHandle publication,
    QObject* parent, ComponentTreeBuildLimits limits) noexcept {
  return createTyped(product, std::move(publication), parent,
                     ComponentTreeModelFailurePoint::None, limits)
      .model;
}

ComponentTreeModel::Node* ComponentTreeModel::addNode(
    ComponentTreeAddressValue address, QString label, Node* parentNode) {
  auto node = std::make_unique<Node>(Node{
      ComponentTreeNodeAddress{publication_, std::move(address)},
      std::move(label)});
  node->parent = parentNode;
  Node* result = node.get();
  nodes_.push_back(std::move(node));
  if (parentNode)
    parentNode->children.push_back(result);
  else
    roots_.push_back(result);
  return result;
}

QModelIndex ComponentTreeModel::index(int row, int column,
                                      const QModelIndex& parentIndex) const {
  if (column != 0 || row < 0) return {};
  const std::vector<Node*>* children = &roots_;
  if (parentIndex.isValid()) {
    if (parentIndex.column() != 0 || parentIndex.model() != this) return {};
    const auto* parentNode =
        static_cast<const Node*>(parentIndex.internalPointer());
    if (!parentNode) return {};
    children = &parentNode->children;
  }
  if (static_cast<std::size_t>(row) >= children->size()) return {};
  return createIndex(row, column, (*children)[static_cast<std::size_t>(row)]);
}

QModelIndex ComponentTreeModel::parent(const QModelIndex& child) const {
  if (!child.isValid() || child.model() != this) return {};
  const auto* node = static_cast<const Node*>(child.internalPointer());
  if (!node || !node->parent) return {};
  return createIndex(rowOf(node->parent), 0, node->parent);
}

int ComponentTreeModel::rowCount(const QModelIndex& parentIndex) const {
  if (parentIndex.isValid() && parentIndex.column() != 0) return 0;
  if (!parentIndex.isValid()) return static_cast<int>(roots_.size());
  if (parentIndex.model() != this) return 0;
  const auto* node = static_cast<const Node*>(parentIndex.internalPointer());
  return node ? static_cast<int>(node->children.size()) : 0;
}

int ComponentTreeModel::columnCount(const QModelIndex&) const { return 1; }

QVariant ComponentTreeModel::data(const QModelIndex& modelIndex,
                                  int role) const {
  if (!modelIndex.isValid() || modelIndex.model() != this ||
      modelIndex.column() != 0)
    return {};
  const auto* node = static_cast<const Node*>(modelIndex.internalPointer());
  if (!node) return {};
  if (role == Qt::DisplayRole)
    return node->activeComponent
               ? QString::fromUtf8("%1 (активный)").arg(node->label)
               : node->label;
  if (role == Qt::EditRole && node->renameable) return node->editLabel;
  if (role == Qt::FontRole && node->activeComponent) {
    QFont font;
    font.setBold(true);
    return font;
  }
  if (role == Qt::CheckStateRole)
    return checkState(node);
  if (role == Qt::ToolTipRole && !node->toolTip.isEmpty())
    return node->toolTip;
  if (role == kComponentTreeUnusedDefinitionRole)
    return node->unusedDefinition;
  if (role == kComponentTreeActiveRole) return node->activeComponent;
  if (role == kComponentTreeAuthoredVisibleRole)
    return node->authoredFlags
               ? QVariant{node->authoredFlags->visible}
               : QVariant{};
  if (role == kComponentTreeAuthoredSuppressedRole)
    return node->authoredFlags
               ? QVariant{node->authoredFlags->suppressed}
               : QVariant{};
  if (role == kComponentTreeAuthoredGroundedRole)
    return node->authoredFlags
               ? QVariant{node->authoredFlags->grounded}
               : QVariant{};
  if (role == kComponentTreeEffectiveVisibleRole)
    return node->effectiveVisible ? QVariant{*node->effectiveVisible}
                                  : QVariant{};
  if (role == kComponentTreeEffectiveSuppressedRole)
    return node->effectiveSuppressed ? QVariant{*node->effectiveSuppressed}
                                     : QVariant{};
  if (role == kComponentTreeBodyVisibleRole)
    return node->bodyVisible ? QVariant{*node->bodyVisible} : QVariant{};
  if (role == kComponentTreeImageVisibleRole)
    return node->imageVisible ? QVariant{*node->imageVisible} : QVariant{};
  return {};
}

Qt::ItemFlags ComponentTreeModel::flags(const QModelIndex& modelIndex) const {
  if (!modelIndex.isValid() || modelIndex.model() != this)
    return Qt::NoItemFlags;
  const auto* node = static_cast<const Node*>(modelIndex.internalPointer());
  if (!node) return Qt::NoItemFlags;
  Qt::ItemFlags result =
      Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable;
  if (node->renameable) result |= Qt::ItemIsEditable;
  return result;
}

bool ComponentTreeModel::setData(const QModelIndex& modelIndex,
                                 const QVariant& value, int role) {
  if (!modelIndex.isValid() || modelIndex.model() != this ||
      modelIndex.column() != 0)
    return false;
  auto* node = static_cast<Node*>(modelIndex.internalPointer());
  if (!node) return false;
  if (role == Qt::EditRole) {
    if (!node->renameable) return false;
    const QString requested = value.toString().trimmed();
    if (requested.isEmpty()) return false;
    if (requested == node->editLabel) return true;
    emit nodeRenameRequested(node->address.value(), requested);
    return true;
  }
  if (role != Qt::CheckStateRole) return false;
  const bool visible = value.toInt() == Qt::Checked;
  const auto* root =
      std::get_if<ComponentTreeRootAddress>(&node->address.value());
  const auto* occurrence =
      std::get_if<ComponentTreeOccurrenceAddress>(&node->address.value());
  if (root || occurrence) {
    if (node->componentVisible && *node->componentVisible == visible)
      return true;
    emit componentVisibilityChangeRequested(
        root ? root->instance : occurrence->instance, visible);
    return true;
  }
  if (checkState(node) == (visible ? Qt::Checked : Qt::Unchecked)) return true;
  setLocalVisibility(node, visible);
  emit nodeVisibilityChangeRequested(node->address.value(), visible);
  return true;
}

Qt::CheckState ComponentTreeModel::checkState(const Node* node) const noexcept {
  if (!node) return Qt::Unchecked;
  if (node->componentVisible)
    return *node->componentVisible ? Qt::Checked : Qt::Unchecked;
  if (node->bodyVisible)
    return *node->bodyVisible ? Qt::Checked : Qt::Unchecked;
  if (node->imageVisible)
    return *node->imageVisible ? Qt::Checked : Qt::Unchecked;
  if (!node->children.empty()) {
    bool anyChecked = false;
    bool anyUnchecked = false;
    for (const Node* child : node->children) {
      const Qt::CheckState childState = checkState(child);
      anyChecked = anyChecked || childState != Qt::Unchecked;
      anyUnchecked = anyUnchecked || childState != Qt::Checked;
    }
    if (anyChecked && anyUnchecked) return Qt::PartiallyChecked;
    return anyChecked ? Qt::Checked : Qt::Unchecked;
  }
  return node->localVisible.value_or(true) ? Qt::Checked : Qt::Unchecked;
}

void ComponentTreeModel::setLocalVisibility(Node* node, bool visible) {
  if (!node) return;
  if (node->bodyVisible) node->bodyVisible = visible;
  if (node->imageVisible) node->imageVisible = visible;
  if (node->localVisible) node->localVisible = visible;
  for (Node* child : node->children) setLocalVisibility(child, visible);
  const QModelIndex changed = createIndex(rowOf(node), 0, node);
  emit dataChanged(changed, changed,
                   {Qt::CheckStateRole, kComponentTreeBodyVisibleRole,
                    kComponentTreeImageVisibleRole});
  notifyCheckStateAncestors(node->parent);
}

void ComponentTreeModel::notifyCheckStateAncestors(Node* node) {
  for (Node* current = node; current; current = current->parent) {
    const QModelIndex changed = createIndex(rowOf(current), 0, current);
    emit dataChanged(changed, changed, {Qt::CheckStateRole});
  }
}

bool ComponentTreeModel::insertRows(int, int, const QModelIndex&) {
  return false;
}

bool ComponentTreeModel::removeRows(int, int, const QModelIndex&) {
  return false;
}

Qt::DropActions ComponentTreeModel::supportedDropActions() const {
  return Qt::IgnoreAction;
}

Qt::DropActions ComponentTreeModel::supportedDragActions() const {
  return Qt::IgnoreAction;
}

ComponentTreeModel::AddressView ComponentTreeModel::addressForIndex(
    const QModelIndex& modelIndex) const noexcept {
  if (!modelIndex.isValid() || modelIndex.model() != this ||
      modelIndex.column() != 0)
    return std::nullopt;
  const auto* node = static_cast<const Node*>(modelIndex.internalPointer());
  if (!node) return std::nullopt;
  return std::cref(node->address);
}

QModelIndex ComponentTreeModel::indexForAddress(
    const ComponentTreeNodeAddress& address) const noexcept {
  if (!address.publication().isSameCurrentPublication(publication_)) return {};
  const auto found = std::find_if(
      nodes_.begin(), nodes_.end(), [&address](const auto& node) {
        return node && node->address.value() == address.value();
      });
  if (found == nodes_.end()) return {};
  const Node* node = found->get();
  return createIndex(rowOf(node), 0, const_cast<Node*>(node));
}

int ComponentTreeModel::rowOf(const Node* node) const noexcept {
  if (!node) return -1;
  const auto& siblings = node->parent ? node->parent->children : roots_;
  const auto found = std::find(siblings.begin(), siblings.end(), node);
  return found == siblings.end() ? -1
                                 : static_cast<int>(found - siblings.begin());
}

void ComponentTreeModel::setRootDisplayName(const QString& name) {
  if (roots_.empty() || name.trimmed().isEmpty()) return;
  Node* root = roots_.front();
  if (!root || !std::holds_alternative<ComponentTreeRootAddress>(
                   root->address.value()) ||
      root->label == name)
    return;
  root->label = name;
  const QModelIndex rootIndex = index(0, 0);
  emit dataChanged(rootIndex, rootIndex, {Qt::DisplayRole});
}

void ComponentTreeModel::setRootVisible(bool visible) {
  if (roots_.empty()) return;
  Node* root = roots_.front();
  if (!root || !std::holds_alternative<ComponentTreeRootAddress>(
                   root->address.value()))
    return;
  if (root->componentVisible && *root->componentVisible == visible) return;
  root->componentVisible = visible;
  const QModelIndex rootIndex = index(0, 0);
  emit dataChanged(rootIndex, rootIndex, {Qt::CheckStateRole});
}

void ComponentTreeModel::setActiveComponent(
    DefinitionId definitionId, const std::optional<OccurrencePath>& path) {
  std::vector<Node*> changed;
  for (const auto& owned : nodes_) {
    Node* node = owned.get();
    if (!node) continue;
    const auto* root =
        std::get_if<ComponentTreeRootAddress>(&node->address.value());
    const auto* occurrence =
        std::get_if<ComponentTreeOccurrenceAddress>(&node->address.value());
    const ComponentInstanceAddress* instance =
        root ? &root->instance : occurrence ? &occurrence->instance : nullptr;
    const bool active = instance && instance->definitionId == definitionId &&
                        instance->occurrencePath == path;
    if (node->activeComponent == active) continue;
    node->activeComponent = active;
    changed.push_back(node);
  }
  for (Node* node : changed) {
    const QModelIndex index = createIndex(rowOf(node), 0, node);
    emit dataChanged(index, index,
                     {Qt::DisplayRole, Qt::FontRole,
                      kComponentTreeActiveRole});
  }
}

void ComponentTreeModel::clearActiveComponent() {
  std::vector<Node*> changed;
  for (const auto& owned : nodes_) {
    Node* node = owned.get();
    if (!node || !node->activeComponent) continue;
    node->activeComponent = false;
    changed.push_back(node);
  }
  for (Node* node : changed) {
    const QModelIndex index = createIndex(rowOf(node), 0, node);
    emit dataChanged(index, index,
                     {Qt::DisplayRole, Qt::FontRole,
                      kComponentTreeActiveRole});
  }
}

}  // namespace solidar
